// THROWAWAY TEST HARNESS -- not the final Core Server implementation.
//
// This file exists only so the HTTP + CGI code (RequestParser.cpp,
// RequestHandler.cpp, CgiHandler.cpp) can be exercised against a real
// browser/curl while the project is split across two people, per
// guide_jour1_HTTP_CGI.md. It intentionally mirrors main_skeleton.cpp
// from that guide as closely as possible so it's easy to compare against
// whatever the Core Server side eventually builds for real.
//
// IMPORTANT: exactly one poll() call drives every socket AND every CGI
// pipe in this program (subject p.8: "It must be non-blocking and use
// only 1 poll() ... for all the I/O operations between the clients and
// the server (listen included)" / "Writing or reading ANY file
// descriptor without going through poll() ... is strictly FORBIDDEN").
// CGI pipes are registered into the same poll_fds vector as client
// sockets (see registerCgiFds()) and driven exactly like them -- one
// read() or one write() per ready fd per iteration, never outside of a
// poll()-reported readiness bit, never branching on errno afterwards.
// Regular disk files (static file serving, CGI script existence checks)
// are exempt from this per the subject's own explicit carve-out (p.8,
// warning box: "Regular disk files are exempt"), so RequestHandler.cpp's
// stat()/ifstream use is intentionally not routed through poll().
#include "connection.hpp"
#include "config/Config.hpp"
#include "cgi/CgiHandler.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <map>
#include <vector>

namespace {

// Graceful shutdown on Ctrl+C / SIGTERM: without this, the process dies
// via the default signal disposition, which skips normal exit() cleanup
// entirely -- including, notably, LeakSanitizer's/valgrind's ability to
// report anything, since neither gets to run its exit-time checks on a
// signal-killed process. sig_atomic_t + a plain flag is what's safe to
// touch from a signal handler.
volatile sig_atomic_t g_shouldStop = 0;

/** @brief SIGINT/SIGTERM handler -- just flips a flag the main loop checks each iteration. */
void handleShutdownSignal(int) {
    g_shouldStop = 1;
}

const size_t READ_CHUNK = 4096;
const int CGI_POLL_TIMEOUT_MS = 1000;   // wake up periodically to sweep CGI deadlines
const int REAP_POLL_TIMEOUT_MS = 100;   // faster catch-up while zombies are pending reap

/** @brief Sets O_NONBLOCK on a socket/pipe fd. */
void setNonBlocking(int fd) {
    fcntl(fd, F_SETFL, O_NONBLOCK);
}

/**
 * @brief Creates, binds and listens on one non-blocking TCP socket.
 * @param host Interface to bind ("0.0.0.0"/empty for all interfaces).
 * @param port Port to listen on.
 * @return The listening fd, or -1 on any failure (already logged via perror).
 */
int openListenSocket(const std::string& host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return -1;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (host.empty() || host == "0.0.0.0")
        addr.sin_addr.s_addr = INADDR_ANY;
    else
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 128) < 0) {
        std::perror("listen");
        close(fd);
        return -1;
    }
    setNonBlocking(fd);
    return fd;
}

/**
 * @brief Resets a Connection for the next request on a keep-alive socket.
 * @param conn Connection to reset. fd/server_conf/keep_alive are left
 *             untouched -- those belong to the connection, not the request.
 */
void resetForNextRequest(Connection& conn) {
    conn.method.clear();
    conn.path.clear();
    conn.query_string.clear();
    conn.http_version.clear();
    conn.headers.clear();
    conn.body.clear();
    conn.write_buffer.clear();
    conn.bytes_written = 0;
    conn.status_code = 0;
    conn.cgi_out.clear();
    conn.cgi_in_offset = 0;
    conn.cgi_path_info.clear();
    conn.cgi_stdin_fd = -1;
    conn.cgi_stdout_fd = -1;
    conn.cgi_pid = -1;
    conn.state = READING_REQUEST;
}

/**
 * @brief Queues a freshly-started CGI's pipe(s) to join poll_fds.
 *
 * Does nothing unless conn.state == CGI_RUNNING. Never touches poll_fds
 * directly -- a push_back() there could reallocate the vector while the
 * caller may still be holding a reference into it for this same iteration.
 *
 * @param conn             Connection that may have just started a CGI.
 * @param clientPollEntry  This connection's own pollfd entry, so its
 *                         events can be cleared while the CGI runs.
 * @param fdsToAdd         Pipe fds to append to poll_fds once the caller's
 *                         current pass over it is done.
 * @param cgiOwner         Updated so the main loop can map a pipe fd back
 *                         to the client fd that owns it.
 */
void registerCgiFdsIfStarted(Connection& conn, struct pollfd& clientPollEntry,
                              std::vector<struct pollfd>& fdsToAdd, std::map<int, int>& cgiOwner) {
    if (conn.state != CGI_RUNNING)
        return;
    clientPollEntry.events = 0;  // nothing to do on the client fd while CGI runs;
                                  // POLLHUP/POLLERR are still reported regardless.
    if (conn.cgi_stdin_fd != -1) {
        struct pollfd p;
        p.fd = conn.cgi_stdin_fd;
        p.events = POLLOUT;
        p.revents = 0;
        fdsToAdd.push_back(p);
        cgiOwner[conn.cgi_stdin_fd] = conn.fd;
    }
    if (conn.cgi_stdout_fd != -1) {
        struct pollfd p;
        p.fd = conn.cgi_stdout_fd;
        p.events = POLLIN;
        p.revents = 0;
        fdsToAdd.push_back(p);
        cgiOwner[conn.cgi_stdout_fd] = conn.fd;
    }
}

/**
 * @brief Parses + handles whatever request is already sitting in the
 *        read buffer, with no socket I/O of its own.
 *
 * try_parse_request()/handle_request() only touch in-memory buffers
 * (handle_request() possibly forking a CGI doesn't count -- fork()/pipe()
 * aren't read()/write() on an existing fd, so that's not a poll()
 * violation). This is what lets a pipelined keep-alive request that
 * arrived in the same read() as the previous one get processed right
 * away, instead of waiting on a POLLIN event that may never come.
 *
 * @param conn      Connection to pump.
 * @param pfd       This connection's pollfd entry, updated for whatever
 *                  comes next (POLLOUT once a response is ready).
 * @param fdsToAdd  Passed through to registerCgiFdsIfStarted().
 * @param cgiOwner  Passed through to registerCgiFdsIfStarted().
 */
void pump(Connection& conn, struct pollfd& pfd, std::vector<struct pollfd>& fdsToAdd,
          std::map<int, int>& cgiOwner) {
    if (conn.state != READING_REQUEST || !try_parse_request(conn))
        return;
    conn.state = PROCESSING;
    handle_request(conn);
    if (conn.state == CGI_RUNNING) {
        registerCgiFdsIfStarted(conn, pfd, fdsToAdd, cgiOwner);
    } else {
        conn.state = WRITING_RESPONSE;
        pfd.events = POLLOUT;
    }
}

/** @brief Finds `fd` in poll_fds and updates its events mask. No-op if not found. */
void findAndSetEvents(std::vector<struct pollfd>& poll_fds, int fd, short events) {
    for (size_t k = 0; k < poll_fds.size(); ++k) {
        if (poll_fds[k].fd == fd) {
            poll_fds[k].events = events;
            return;
        }
    }
}

}  // namespace

/**
 * @brief Entry point: loads the config, opens the listen sockets, then
 *        runs the single-poll() event loop until SIGINT/SIGTERM.
 * @param argc/argv Optional config file path (falls back to
 *                  conf/default.conf if omitted).
 * @return 0 on a clean shutdown, 1 on a startup failure (bad args, socket
 *         setup, or a malformed config).
 */
int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, handleShutdownSignal);
    std::signal(SIGTERM, handleShutdownSignal);

    // Subject p.8: "a configuration file, provided as an argument on the
    // command line, or available in a default path" -- so a missing
    // argument falls back to DEFAULT_CONFIG_PATH instead of a hard error.
    if (argc > 2) {
        std::fprintf(stderr, "usage: %s [config file]\n", argv[0]);
        return 1;
    }
    const char* DEFAULT_CONFIG_PATH = "conf/default.conf";
    std::string configPath = (argc == 2) ? argv[1] : DEFAULT_CONFIG_PATH;

    std::vector<ServerConfig> configs;
    try {
        configs = Config::load(configPath);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "config error: %s\n", e.what());
        return 1;
    }

    std::vector<struct pollfd> poll_fds;
    std::map<int, Connection> connections;          // client fd -> Connection
    std::map<int, const ServerConfig*> listenerConfig;  // listen fd -> config
    std::map<int, int> cgiOwner;                     // cgi pipe fd -> owning client fd
    std::vector<pid_t> pendingReap;                  // CGI pids not yet reaped

    for (size_t i = 0; i < configs.size(); ++i) {
        int fd = openListenSocket(configs[i].host, configs[i].port);
        if (fd < 0)
            return 1;
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        poll_fds.push_back(pfd);
        listenerConfig[fd] = &configs[i];
        std::fprintf(stderr, "listening on %s:%d\n",
                      configs[i].host.empty() ? "0.0.0.0" : configs[i].host.c_str(),
                      configs[i].port);
    }

    while (!g_shouldStop) {
        bool anyCgiRunning = false;
        bool anyCgiAwaitingReap = false;
        for (std::map<int, Connection>::iterator it = connections.begin(); it != connections.end(); ++it) {
            if (it->second.state == CGI_RUNNING) {
                anyCgiRunning = true;
                // Pipes closed but finish() couldn't reap the child yet
                // (see CgiHandler.cpp's finish()): needs the fast retry
                // cadence below, same as a pending zombie does.
                if (cgi_handler::isDone(it->second))
                    anyCgiAwaitingReap = true;
            }
        }
        // Also keep waking up periodically while a killed/finished CGI's
        // pid is still waiting to be reaped -- otherwise, once the last
        // CGI_RUNNING connection is gone, the timeout would revert to -1
        // (block until the next unrelated event) and a zombie could sit
        // around until something unrelated happens to wake the loop. Use
        // a tighter interval than the general CGI-deadline cadence so a
        // burst of finishing/killed CGIs gets swept up quickly rather
        // than lingering as visible (if harmless) zombies for seconds.
        int timeout = -1;
        if (!pendingReap.empty() || anyCgiAwaitingReap)
            timeout = REAP_POLL_TIMEOUT_MS;
        else if (anyCgiRunning)
            timeout = CGI_POLL_TIMEOUT_MS;

        int ready = poll(&poll_fds[0], poll_fds.size(), timeout);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        std::vector<int> fdsToRemove;
        std::vector<struct pollfd> fdsToAdd;

        for (size_t i = 0; i < poll_fds.size(); ++i) {
            int fd = poll_fds[i].fd;
            short revents = poll_fds[i].revents;
            if (revents == 0)
                continue;

            std::map<int, const ServerConfig*>::iterator listenIt = listenerConfig.find(fd);
            if (listenIt != listenerConfig.end()) {
                if (revents & POLLIN) {
                    struct sockaddr_in clientAddr;
                    socklen_t clientLen = sizeof(clientAddr);
                    int clientFd = accept(fd, reinterpret_cast<struct sockaddr*>(&clientAddr), &clientLen);
                    if (clientFd >= 0) {
                        setNonBlocking(clientFd);
                        Connection conn;
                        conn.fd = clientFd;
                        conn.server_conf = listenIt->second;
                        connections[clientFd] = conn;
                        struct pollfd pfd;
                        pfd.fd = clientFd;
                        pfd.events = POLLIN;
                        pfd.revents = 0;
                        fdsToAdd.push_back(pfd);
                    }
                }
                continue;
            }

            std::map<int, int>::iterator cgiIt = cgiOwner.find(fd);
            if (cgiIt != cgiOwner.end()) {
                std::map<int, Connection>::iterator connIt = connections.find(cgiIt->second);
                if (connIt == connections.end()) {
                    fdsToRemove.push_back(fd);  // orphaned pipe: owner already gone
                    continue;
                }
                Connection& conn = connIt->second;
                if (fd == conn.cgi_stdin_fd && (revents & (POLLOUT | POLLERR | POLLHUP))) {
                    cgi_handler::onStdinWritable(conn);
                    if (conn.cgi_stdin_fd == -1)
                        fdsToRemove.push_back(fd);
                } else if (fd == conn.cgi_stdout_fd && (revents & (POLLIN | POLLHUP | POLLERR))) {
                    cgi_handler::onStdoutReadable(conn);
                    if (conn.cgi_stdout_fd == -1)
                        fdsToRemove.push_back(fd);
                }
                continue;
            }

            std::map<int, Connection>::iterator connIt = connections.find(fd);
            if (connIt == connections.end())
                continue;
            Connection& conn = connIt->second;
            bool shouldClose = false;

            if ((revents & (POLLHUP | POLLERR)) && !(revents & POLLIN)) {
                shouldClose = true;
            } else if (conn.state == READING_REQUEST && (revents & POLLIN)) {
                char buf[READ_CHUNK];
                ssize_t n = read(fd, buf, sizeof(buf));
                if (n > 0) {
                    conn.read_buffer.append(buf, static_cast<size_t>(n));
                    pump(conn, poll_fds[i], fdsToAdd, cgiOwner);
                } else {
                    shouldClose = true;
                }
            }

            if (conn.state == WRITING_RESPONSE && (revents & POLLOUT)) {
                size_t remaining = conn.write_buffer.size() - conn.bytes_written;
                ssize_t n = write(fd, conn.write_buffer.c_str() + conn.bytes_written, remaining);
                if (n > 0) {
                    conn.bytes_written += static_cast<size_t>(n);
                    if (conn.bytes_written == conn.write_buffer.size()) {
                        if (conn.keep_alive) {
                            resetForNextRequest(conn);
                            poll_fds[i].events = POLLIN;
                            pump(conn, poll_fds[i], fdsToAdd, cgiOwner);  // leftover pipelined request?
                        } else {
                            conn.state = DONE;
                        }
                    }
                } else {
                    shouldClose = true;
                }
            }

            if (conn.state == DONE || shouldClose) {
                fdsToRemove.push_back(fd);
                if (conn.cgi_stdin_fd != -1)
                    fdsToRemove.push_back(conn.cgi_stdin_fd);
                if (conn.cgi_stdout_fd != -1)
                    fdsToRemove.push_back(conn.cgi_stdout_fd);
                if (conn.cgi_pid != -1) {
                    kill(conn.cgi_pid, SIGKILL);
                    pendingReap.push_back(conn.cgi_pid);
                    conn.cgi_pid = -1;
                }
            }
        }

        // CGI completion + deadline sweep: runs after the full poll_fds
        // pass above so every ready pipe for this round has already been
        // serviced, regardless of where it sat in the array relative to
        // its owning client fd.
        for (std::map<int, Connection>::iterator it = connections.begin(); it != connections.end(); ++it) {
            Connection& conn = it->second;
            if (conn.state != CGI_RUNNING)
                continue;
            // A connection already handled by the shouldClose/DONE cleanup
            // earlier this same iteration (client disconnected right as its
            // CGI's pipes also hit EOF) has cgi_pid == -1 already, with its
            // real pid already queued in pendingReap. Without this guard,
            // isDone() can still read true here and finish() would call
            // waitpid(-1, ...) -- "reap any child" -- potentially stealing
            // an unrelated CGI's exit status and leaving *that* one's real
            // pid permanently unreapable.
            if (conn.cgi_pid == -1)
                continue;
            if (cgi_handler::isDone(conn)) {
                pid_t pending = 0;
                if (cgi_handler::finish(conn, pending)) {
                    if (pending != 0)
                        pendingReap.push_back(pending);
                    findAndSetEvents(poll_fds, conn.fd, POLLOUT);
                }
                // else: pipes closed but the child isn't reapable yet --
                // conn.state is still CGI_RUNNING, retried next iteration
                // (anyCgiAwaitingReap above keeps that iteration coming
                // soon rather than waiting out the full CGI poll timeout).
            } else if (std::time(0) >= conn.cgi_deadline) {
                int inFd = conn.cgi_stdin_fd;
                int outFd = conn.cgi_stdout_fd;
                pid_t pending = cgi_handler::abortTimeout(conn);
                if (pending != 0)
                    pendingReap.push_back(pending);
                if (inFd != -1)
                    fdsToRemove.push_back(inFd);
                if (outFd != -1)
                    fdsToRemove.push_back(outFd);
                findAndSetEvents(poll_fds, conn.fd, POLLOUT);
            }
        }

        // Opportunistic, non-blocking reap sweep -- never waitpid(..., 0).
        for (size_t k = 0; k < pendingReap.size();) {
            if (cgi_handler::reapIfExited(pendingReap[k]))
                pendingReap.erase(pendingReap.begin() + static_cast<long>(k));
            else
                ++k;
        }

        for (size_t k = 0; k < fdsToAdd.size(); ++k)
            poll_fds.push_back(fdsToAdd[k]);

        for (size_t k = 0; k < fdsToRemove.size(); ++k) {
            int fd = fdsToRemove[k];
            close(fd);
            for (size_t j = 0; j < poll_fds.size(); ++j) {
                if (poll_fds[j].fd == fd) {
                    poll_fds.erase(poll_fds.begin() + static_cast<long>(j));
                    break;
                }
            }
            cgiOwner.erase(fd);
            connections.erase(fd);
        }
    }

    for (size_t i = 0; i < poll_fds.size(); ++i)
        close(poll_fds[i].fd);
    return 0;
}
