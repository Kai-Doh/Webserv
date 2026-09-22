#ifndef CONNECTION_HPP
#define CONNECTION_HPP

#include <string>
#include <map>
#include <ctime>        // time_t
#include <sys/types.h>  // pid_t

#include "config/Config.hpp"

// This header follows guide_jour1_HTTP_CGI.md part 3 for the fields owned
// by each side of the project. Fields added beyond what the guide lists
// are flagged individually below.

// ADDED: CGI_RUNNING. The guide's original 4-state enum assumed
// handle_request() always finishes synchronously. That can't hold for CGI
// under the subject's "exactly one poll() (or equivalent) for every
// socket AND pipe" rule (p.8) -- the guide itself flags this exact
// integration as an open question for later ("Point a discuter ensemble
// avant le jour 4-5", part 5). CGI_RUNNING resolves it: handle_request()
// may now leave a connection in this state instead of WRITING_RESPONSE
// when it started a CGI process, and the core server loop drives that
// CGI's two pipes through the *same* poll_fds array as every socket
// (see harness_main.cpp) until it's done, at which point cgi_handler
// moves the connection to WRITING_RESPONSE like any other request.
enum ConnState {
    READING_REQUEST,
    PROCESSING,
    CGI_RUNNING,
    WRITING_RESPONSE,
    DONE
};

struct Connection {
    // --- Belongs to your teammate (Core Server) ---
    int fd;
    ConnState state;
    std::string read_buffer;
    std::string write_buffer;
    size_t bytes_written;
    bool keep_alive;

    // ADDED (Core Server side): wall-clock timestamp of the last read/write
    // on this connection, refreshed by the Core Server. Needed so the
    // server's poll() loop can sweep and close idle connections instead of
    // holding a client fd open forever.
    time_t last_activity;

    // ADDED (Core Server side, not in the original guide table): the
    // config block matched to whichever listening socket accepted this
    // connection. The core server sets this once, right after accept(),
    // exactly like it already sets `fd`. handle_request() needs it to
    // find the right Location (root dir, allowed methods, CGI mapping...).
    const ServerConfig* server_conf;

    // --- Belongs to you (HTTP + CGI) ---
    std::string method;
    std::string path;
    std::string http_version;
    std::map<std::string, std::string> headers;  // keys stored lower-case
    std::string body;
    int cgi_stdin_fd;
    int cgi_stdout_fd;
    pid_t cgi_pid;

    // ADDED (HTTP side): raw query string split off `path` during parsing
    // (e.g. path=/search, query_string=q=cats). Needed for CGI's
    // QUERY_STRING variable and for GET-style form handling.
    std::string query_string;

    // ADDED (HTTP side): CGI/1.1 PATH_INFO -- the extra path segments
    // trailing the resolved script within the URL (e.g. for a request to
    // /cgi-bin/script.py/extra/thing, this holds "/extra/thing"). Empty
    // when the request names the script exactly. Set by handle_request()
    // right before starting a CGI; SCRIPT_NAME is never stored separately,
    // it's always conn.path with this suffix removed (see CgiHandler.cpp).
    std::string cgi_path_info;

    // ADDED (HTTP side): set by try_parse_request()/handle_request() as
    // soon as an error is known (bad request, body too large, ...) so the
    // rest of the pipeline can short-circuit straight to an error
    // response instead of pretending parsing succeeded.
    int status_code;

    // ADDED (HTTP side, CGI_RUNNING bookkeeping): accumulates the CGI's
    // stdout as it arrives across possibly many POLLIN-driven reads;
    // cgi_in_offset tracks how much of `body` has been written to the
    // CGI's stdin so far (also possibly split across many POLLOUT-driven
    // writes); cgi_deadline is a wall-clock cutoff (see CgiHandler.cpp)
    // so a runaway script can never hang the connection -- or the whole
    // server -- indefinitely.
    std::string cgi_out;
    size_t cgi_in_offset;
    time_t cgi_deadline;

    // ADDED (HTTP side): true once the request line + headers for the
    // request currently being read have already been parsed into
    // method/path/headers/etc below. try_parse_request() is called again
    // from scratch on every partial read() while a large body is still
    // arriving (there's no other hook for "more bytes arrived") -- without
    // this, it would re-parse the same fixed header section from zero on
    // every single one of those calls. Harmless for one request in
    // isolation, but multiplied by ~4KB-chunk reads across a large body
    // *and* many concurrent connections, that redundant work becomes the
    // dominant cost: a live stress-test run (20 concurrent 100MB CGI
    // POSTs) pegged one CPU core for 9+ minutes with zero forward
    // progress until this flag was added to skip it.
    bool headers_ready;

    // ADDED (HTTP side): resume point, in bytes past the start of the
    // request body, for incremental Transfer-Encoding: chunked decoding.
    // try_parse_request() is called again from scratch on every partial
    // read() (there is no other hook for "more bytes arrived"), and a
    // chunked body can arrive across thousands of small reads -- without
    // remembering how far decoding already got, each call would re-scan
    // and re-copy everything received so far, making one request's total
    // parsing cost O(body size squared) instead of O(body size). Decoded
    // payload accumulates directly in `body` as each complete chunk is
    // confirmed, so resuming here never redoes completed work.
    size_t chunked_scan_pos;

    Connection()
        : fd(-1), state(READING_REQUEST), bytes_written(0), keep_alive(true),
          last_activity(0), server_conf(0), cgi_stdin_fd(-1), cgi_stdout_fd(-1),
          cgi_pid(-1), status_code(0), cgi_in_offset(0), cgi_deadline(0),
          headers_ready(false), chunked_scan_pos(0) {}
};

// These two functions are the entire HTTP + CGI contract with the core
// server loop. See guide_jour1_HTTP_CGI.md part 3 for the full write-up of
// what each one receives and must do. Note that handle_request() may now
// leave conn.state == CGI_RUNNING instead of always finishing with a
// ready-to-send response -- see the enum comment above.
bool try_parse_request(Connection& conn);
void handle_request(Connection& conn);

#endif
