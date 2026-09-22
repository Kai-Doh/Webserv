#include "connection.hpp"
#include "CgiHandler.hpp"
#include "http/RequestHandler.hpp"
#include "http/HttpStatus.hpp"
#include "utils/StringUtils.hpp"

#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <ctime>
#include <sstream>

extern char** environ;

namespace {

const time_t CGI_TIMEOUT_SECONDS = 10;

/**
 * @brief Splits a filesystem path into its directory and filename parts.
 * @param path Full path to split.
 * @param dir  Set to the directory ("." if `path` had no '/').
 * @param file Set to the filename.
 */
void splitDirFile(const std::string& path, std::string& dir, std::string& file) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        dir = ".";
        file = path;
    } else {
        dir = path.substr(0, slash);
        if (dir.empty())
            dir = "/";
        file = path.substr(slash + 1);
    }
}

/**
 * @brief Grabs our own PATH env var, to hand down to the CGI child.
 * @return Our PATH value, or a sane fallback if somehow unset.
 */
std::string parentPath() {
    for (char** e = environ; *e != 0; ++e) {
        std::string entry(*e);
        if (su::startsWith(entry, "PATH="))
            return entry.substr(5);
    }
    return "/usr/bin:/bin";
}

/**
 * @brief Builds the CGI/1.1 environment for one request (RFC 3875).
 * @param conn       Parsed request the CGI is answering.
 * @param scriptPath Resolved filesystem path of the script (SCRIPT_FILENAME).
 * @return "KEY=value" strings ready for execve()'s envp.
 */
std::vector<std::string> buildEnv(const Connection& conn, const std::string& scriptPath) {
    // SCRIPT_NAME is conn.path with the trailing PATH_INFO (if any) removed
    // -- the two always concatenate back to the request path by construction
    // (see resolveCgiScript() in RequestHandler.cpp).
    std::string scriptName = conn.path;
    if (!conn.cgi_path_info.empty() && conn.cgi_path_info.size() <= scriptName.size())
        scriptName.erase(scriptName.size() - conn.cgi_path_info.size());

    // The official 42 cgi_tester binary (subject-provided) checks PATH_INFO
    // against the *full* request path when the script itself is the exact
    // request target, not empty as RFC 3875 would have it. Only fall back
    // to that when there's genuinely no RFC-3875 extra-path component, so
    // a real PATH_INFO-walking request (e.g. /cgi-bin/script.py/extra/thing)
    // still gets the standards-correct trailing segment.
    std::string pathInfoEnv = conn.cgi_path_info.empty() ? conn.path : conn.cgi_path_info;

    std::vector<std::string> env;
    env.push_back("REQUEST_METHOD=" + conn.method);
    env.push_back("SCRIPT_NAME=" + scriptName);
    env.push_back("SCRIPT_FILENAME=" + scriptPath);
    env.push_back("PATH_INFO=" + pathInfoEnv);
    env.push_back("QUERY_STRING=" + conn.query_string);
    env.push_back("CONTENT_LENGTH=" + su::toString(conn.body.size()));

    std::map<std::string, std::string>::const_iterator it = conn.headers.find("content-type");
    if (it != conn.headers.end())
        env.push_back("CONTENT_TYPE=" + it->second);

    env.push_back("SERVER_PROTOCOL=" + (conn.http_version.empty() ? "HTTP/1.1" : conn.http_version));
    std::string serverName = "localhost";
    std::string serverPort = "80";
    if (conn.server_conf) {
        if (!conn.server_conf->server_name.empty())
            serverName = conn.server_conf->server_name;
        serverPort = su::toString(static_cast<long>(conn.server_conf->port));
    }
    env.push_back("SERVER_NAME=" + serverName);
    env.push_back("SERVER_PORT=" + serverPort);
    env.push_back("SERVER_SOFTWARE=webserv/1.0");
    env.push_back("GATEWAY_INTERFACE=CGI/1.1");
    env.push_back("REDIRECT_STATUS=200");
    std::string uri = conn.path;
    if (!conn.query_string.empty())
        uri += "?" + conn.query_string;
    env.push_back("REQUEST_URI=" + uri);
    env.push_back("REMOTE_ADDR=127.0.0.1");
    env.push_back("PATH=" + parentPath());

    for (std::map<std::string, std::string>::const_iterator hIt = conn.headers.begin();
         hIt != conn.headers.end(); ++hIt) {
        if (hIt->first == "content-length" || hIt->first == "content-type")
            continue;
        env.push_back(su::headerKeyToEnv(hIt->first) + "=" + hIt->second);
    }
    return env;
}

/**
 * @brief Turns raw CGI stdout into the actual HTTP response.
 *
 * Expects "headers\n\nbody" or "headers\r\n\r\nbody" per CGI/1.1 -- pulls
 * out a Status: line and Content-Type if the script sent them, passes
 * through any other headers, and defaults to 200/text/html if it sent
 * neither.
 *
 * @param conn Connection to write the final response into.
 * @param raw  Everything the CGI wrote to its stdout.
 */
void finishFromCgiOutput(Connection& conn, const std::string& raw) {
    size_t sep = raw.find("\r\n\r\n");
    size_t sepLen = 4;
    size_t lfSep = raw.find("\n\n");
    if (lfSep != std::string::npos && (sep == std::string::npos || lfSep < sep)) {
        sep = lfSep;
        sepLen = 2;
    }

    std::string head;
    std::string body;
    if (sep == std::string::npos) {
        body = raw;  // no blank-line separator: treat it all as body
    } else {
        head = raw.substr(0, sep);
        body = raw.substr(sep + sepLen);
    }

    int statusCode = 200;
    std::string contentType;
    std::string extraHeaders;

    std::vector<std::string> lines = su::split(head, '\n');
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string line = su::trim(lines[i]);
        if (line.empty())
            continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string key = su::trim(line.substr(0, colon));
        std::string value = su::trim(line.substr(colon + 1));
        std::string lowerKey = su::toLower(key);
        if (lowerKey == "status") {
            std::vector<std::string> statusParts = su::split(value, ' ');
            bool ok = false;
            long code = su::toLong(statusParts.empty() ? value : statusParts[0], ok);
            if (ok)
                statusCode = static_cast<int>(code);
        } else if (lowerKey == "content-type") {
            contentType = value;
        } else {
            extraHeaders += key + ": " + value + "\r\n";
        }
    }

    if (contentType.empty())
        contentType = "text/html";

    conn.status_code = statusCode;
    request_handler::writeResponse(conn, statusCode, contentType, body, extraHeaders);
}

}  // namespace

namespace cgi_handler {

/**
 * @brief Forks + execve's a CGI script and wires up its stdin/stdout pipes.
 *
 * Returns immediately -- never blocks on the child. On success the caller
 * (RequestHandler) sets conn.state = CGI_RUNNING and the core loop takes
 * it from there via onStdinWritable()/onStdoutReadable(). On failure a
 * complete error response is written directly and conn.state is left
 * alone (normal PROCESSING -> WRITING_RESPONSE applies).
 *
 * @param conn        Connection making the request (conn.body is what
 *                     gets piped to the child's stdin, if anything).
 * @param scriptPath  Filesystem path of the script.
 * @param interpreter Interpreter binary to execve() (e.g. python3).
 * @param loc         Matched location (currently unused here, kept for a
 *                     future per-location CGI setting).
 * @return true if the fork succeeded.
 */
bool start(Connection& conn, const std::string& scriptPath, const std::string& interpreter,
           const Location& loc) {
    (void)loc;

    int inPipe[2];   // parent writes conn.body -> child stdin
    int outPipe[2];  // child stdout -> parent reads
    if (pipe(inPipe) != 0) {
        request_handler::writeErrorResponse(conn, 500);
        return false;
    }
    if (pipe(outPipe) != 0) {
        close(inPipe[0]);
        close(inPipe[1]);
        request_handler::writeErrorResponse(conn, 500);
        return false;
    }

    std::string scriptDir;
    std::string scriptFile;
    splitDirFile(scriptPath, scriptDir, scriptFile);
    std::vector<std::string> envStrings = buildEnv(conn, scriptPath);

    pid_t pid = fork();
    if (pid < 0) {
        close(inPipe[0]);
        close(inPipe[1]);
        close(outPipe[0]);
        close(outPipe[1]);
        request_handler::writeErrorResponse(conn, 500);
        return false;
    }

    if (pid == 0) {
        // --- child ---
        dup2(inPipe[0], STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        close(inPipe[0]);
        close(inPipe[1]);
        close(outPipe[0]);
        close(outPipe[1]);
        if (chdir(scriptDir.c_str()) != 0)
            _exit(127);

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(interpreter.c_str()));
        argv.push_back(const_cast<char*>(scriptFile.c_str()));
        argv.push_back(0);

        std::vector<char*> envp;
        for (size_t i = 0; i < envStrings.size(); ++i)
            envp.push_back(const_cast<char*>(envStrings[i].c_str()));
        envp.push_back(0);

        execve(interpreter.c_str(), &argv[0], &envp[0]);
        _exit(127);  // execve failed
    }

    // --- parent: nothing here blocks or polls on its own; the Core
    // Server drives these two fds through the shared poll() from here on ---
    close(inPipe[0]);
    close(outPipe[1]);
    fcntl(inPipe[1], F_SETFL, O_NONBLOCK);
    fcntl(outPipe[0], F_SETFL, O_NONBLOCK);

    conn.cgi_pid = pid;
    conn.cgi_stdout_fd = outPipe[0];
    conn.cgi_in_offset = 0;
    conn.cgi_out.clear();
    conn.cgi_deadline = std::time(0) + CGI_TIMEOUT_SECONDS;

    if (conn.body.empty()) {
        close(inPipe[1]);
        conn.cgi_stdin_fd = -1;
    } else {
        conn.cgi_stdin_fd = inPipe[1];
    }
    return true;
}

/**
 * @brief Writes one chunk of conn.body to the CGI's stdin.
 *
 * Call exactly once per POLLOUT on conn.cgi_stdin_fd. Marks the fd -1
 * (never close()s it directly -- see the note below) once the whole body
 * has gone out, or immediately on any write error.
 *
 * @param conn Connection whose body is being streamed to its CGI child.
 */
void onStdinWritable(Connection& conn) {
    if (conn.cgi_stdin_fd == -1)
        return;
    // NOTE: deliberately never close(conn.cgi_stdin_fd) here -- only mark
    // it -1. Closing it immediately would free the fd number for reuse by
    // a later accept()/pipe() call within the *same* poll() iteration,
    // while the Core Server's poll_fds bookkeeping for this fd is still
    // deferred to that iteration's single end-of-pass cleanup; it closes
    // the fd exactly once there, after seeing cgi_stdin_fd become -1 (see
    // the caller in srcs/core/ServerCgi.cpp).
    size_t remaining = conn.body.size() - conn.cgi_in_offset;
    ssize_t n = write(conn.cgi_stdin_fd, conn.body.data() + conn.cgi_in_offset, remaining);
    if (n > 0) {
        conn.cgi_in_offset += static_cast<size_t>(n);
        if (conn.cgi_in_offset == conn.body.size())
            conn.cgi_stdin_fd = -1;
    } else {
        // Broken pipe or a spurious zero-length write: stop feeding the
        // CGI, but keep reading whatever output it still produces.
        conn.cgi_stdin_fd = -1;
    }
}

/**
 * @brief Reads one chunk of the CGI's stdout into conn.cgi_out.
 * @param conn Connection accumulating CGI output. cgi_stdout_fd is marked
 *             -1 on EOF or a read error (see onStdinWritable()'s note on
 *             why it isn't close()d here).
 */
void onStdoutReadable(Connection& conn) {
    if (conn.cgi_stdout_fd == -1)
        return;
    // Same reasoning as onStdinWritable(): never close() here, only mark.
    char buf[4096];
    ssize_t n = read(conn.cgi_stdout_fd, buf, sizeof(buf));
    if (n > 0)
        conn.cgi_out.append(buf, static_cast<size_t>(n));
    else
        conn.cgi_stdout_fd = -1;
}

/** @brief True once both CGI pipes have been marked closed. */
bool isDone(const Connection& conn) {
    return conn.cgi_stdin_fd == -1 && conn.cgi_stdout_fd == -1;
}

/**
 * @brief Assembles the final response once both CGI pipes are closed.
 *
 * A single non-blocking waitpid() tells an execve() failure (exit 127, no
 * output -> 502) apart from a script that legitimately said nothing (->
 * 200). Since pipes closing and the child becoming reapable are two
 * separate kernel events, the child can briefly still be un-reapable right
 * after EOF -- in that case this changes nothing and returns false so the
 * caller retries on a later poll() iteration instead of guessing 200.
 *
 * @param conn       Connection whose CGI just finished.
 * @param pendingPid Set to a pid that still needs reapIfExited() later, or 0.
 * @return true once conn.state has actually been set to WRITING_RESPONSE.
 */
bool finish(Connection& conn, pid_t& pendingPid) {
    pendingPid = 0;
    if (conn.cgi_pid == -1) {
        // Already handled (e.g. by a forced close this same iteration):
        // never call waitpid(-1, ...) -- "reap any child" -- that would
        // risk stealing an unrelated CGI's exit status.
        finishFromCgiOutput(conn, conn.cgi_out);
        conn.state = WRITING_RESPONSE;
        return true;
    }
    int status = 0;
    pid_t reaped = waitpid(conn.cgi_pid, &status, WNOHANG);
    if (reaped != conn.cgi_pid)
        return false;  // pipes closed but not reapable yet: caller retries later

    bool execFailed = WIFEXITED(status) && WEXITSTATUS(status) == 127;
    bool crashed = WIFSIGNALED(status);  // e.g. a script that segfaults
    conn.cgi_pid = -1;

    if ((execFailed || crashed) && conn.cgi_out.empty())
        request_handler::writeErrorResponse(conn, 502);
    else
        finishFromCgiOutput(conn, conn.cgi_out);
    conn.state = WRITING_RESPONSE;
    return true;
}

/**
 * @brief Kills a CGI that blew past conn.cgi_deadline and writes a 504.
 * @param conn Connection whose CGI is being aborted. Both pipe fds are
 *             marked -1 here (actual close() deferred to the caller's
 *             end-of-iteration cleanup, same as the pipe I/O functions).
 * @return A pid still needing reapIfExited(), or 0 if already reaped.
 */
pid_t abortTimeout(Connection& conn) {
    // Same "never close() here" reasoning as onStdinWritable()/
    // onStdoutReadable() above -- the caller (srcs/core/ServerCgi.cpp)
    // captures conn.cgi_stdin_fd/cgi_stdout_fd *before* calling this and
    // defers their actual close() to its own bookkeeping.
    if (conn.cgi_pid == -1) {
        // Never reachable via the guarded call site today, but kill(-1,
        // SIGKILL) -- "signal every process this user can reach" -- is
        // catastrophic enough that this stays defended on its own too.
        conn.cgi_stdin_fd = -1;
        conn.cgi_stdout_fd = -1;
        request_handler::writeErrorResponse(conn, 504);
        conn.state = WRITING_RESPONSE;
        return 0;
    }
    conn.cgi_stdin_fd = -1;
    conn.cgi_stdout_fd = -1;
    kill(conn.cgi_pid, SIGKILL);
    int status = 0;
    pid_t reaped = waitpid(conn.cgi_pid, &status, WNOHANG);
    pid_t pending = (reaped == conn.cgi_pid) ? 0 : conn.cgi_pid;
    conn.cgi_pid = -1;

    request_handler::writeErrorResponse(conn, 504);
    conn.state = WRITING_RESPONSE;
    return pending;
}

/** @brief Non-blocking opportunistic reap. @return true once `pid` is actually gone. */
bool reapIfExited(pid_t pid) {
    int status = 0;
    return waitpid(pid, &status, WNOHANG) == pid;
}

}  // namespace cgi_handler
