#ifndef CGI_HANDLER_HPP
#define CGI_HANDLER_HPP

#include <string>
#include <sys/types.h>  // pid_t

struct Connection;
struct Location;

// CGI execution, driven by the *single* shared poll() loop in
// harness_main.cpp -- no function in here ever blocks or runs its own
// poll()/select(). start() forks the CGI and returns immediately; the
// harness then calls onStdinWritable()/onStdoutReadable() only when
// poll() has actually reported that pipe as ready, exactly like it does
// for client sockets, and finish() once both sides are done.
namespace cgi_handler {

// Forks + execve's the CGI (interpreter + scriptPath), sets up its two
// pipes as non-blocking, and stores them in conn.cgi_stdin_fd /
// cgi_stdout_fd / cgi_pid (-1 for cgi_stdin_fd if conn.body is empty --
// stdin is closed immediately in that case, there's nothing to write).
// Also sets conn.cgi_deadline. Returns true on success, in which case the
// caller (RequestHandler) is expected to set conn.state = CGI_RUNNING and
// the harness to register whichever of the two fds are non -1 with
// poll(). On failure, writes a complete error response into
// conn.write_buffer itself (conn.state is left untouched: the normal
// PROCESSING -> WRITING_RESPONSE transition applies, nothing to poll).
bool start(Connection& conn, const std::string& scriptPath, const std::string& interpreter,
           const Location& loc);

// Call exactly once when poll() reports conn.cgi_stdin_fd as writable
// (POLLOUT/POLLERR/POLLHUP). Performs a single write() of the next chunk
// of conn.body. Closes and resets conn.cgi_stdin_fd to -1 once the body
// is fully sent, or on any write error.
void onStdinWritable(Connection& conn);

// Call exactly once when poll() reports conn.cgi_stdout_fd as readable
// (POLLIN/POLLHUP/POLLERR). Performs a single read() into conn.cgi_out.
// Closes and resets conn.cgi_stdout_fd to -1 on EOF or a read error.
void onStdoutReadable(Connection& conn);

// True once both pipes have been closed (both fds are -1).
bool isDone(const Connection& conn);

// Called once both CGI pipes have closed (isDone(conn) is true). Tries a
// single non-blocking waitpid() to learn the child's real exit status --
// needed to tell an execve() failure (exit 127, no output -> 502) apart
// from a script that legitimately produced no output (-> 200). A child's
// pipes closing and it actually becoming reapable are two separate kernel
// events (do_exit() tears down file descriptors before the zombie
// transition), so immediately after EOF the child can briefly still be
// un-reapable. In that case this leaves conn.state untouched (still
// CGI_RUNNING) and returns false so the caller retries on a later poll()
// iteration, instead of guessing 200 by default. Returns true once the
// response has actually been assembled (conn.state is then
// WRITING_RESPONSE), with pendingPid set to a pid that still needs a
// later reapIfExited() call, or 0 if none.
bool finish(Connection& conn, pid_t& pendingPid);

// Called instead of finish() when conn.cgi_deadline has passed. Closes
// whichever pipe(s) are still open, SIGKILLs the CGI, writes a 504 into
// conn.write_buffer and sets conn.state = WRITING_RESPONSE. Like
// finish(), returns a pid still needing a later reap, or 0.
pid_t abortTimeout(Connection& conn);

// Non-blocking opportunistic reap for a pid returned earlier by finish()
// or abortTimeout(). Returns true once the child has actually been
// reaped (safe to stop tracking it).
bool reapIfExited(pid_t pid);

}  // namespace cgi_handler

#endif
