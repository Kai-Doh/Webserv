*This project has been created as part of the 42 curriculum by ktiomico, wcapt.*

# Webserv — HTTP + CGI module

This covers **one person's part** of the 42 `Webserv` project (see
`en.subject.pdf`): HTTP request parsing, request handling/routing, and CGI
execution, as scoped in `guide_jour1_HTTP_CGI.md`. The Core Server half
(sockets, the shared `poll()` event loop, accept/read/write) belongs to a
teammate and is **not** implemented here — see "Scope & boundaries" below.

## Description

Implements the two functions that are the entire contract with the Core
Server loop, exactly as specified in `connection.hpp`:

- `bool try_parse_request(Connection&)` — incrementally parses an HTTP/1.0
  or HTTP/1.1 request out of `read_buffer` (request line, headers, and
  body via `Content-Length` or chunked transfer-encoding), tolerating
  partial reads and pipelined requests.
- `void handle_request(Connection&)` — routes the parsed request against
  the config file (longest-prefix location matching, method checks,
  redirects, static files, directory listing, file uploads, `DELETE`, and
  CGI execution), and writes a full HTTP response into `write_buffer`.

A minimal config-file parser (`Config.cpp`) and a throwaway test harness
(`harness_main.cpp`) are included so this code can actually be run and
tested end-to-end — see below.

## Instructions

```sh
make                              # builds ./webserv
./webserv conf/test.conf          # serves on 127.0.0.1:8080 and :8081
./webserv                         # no argument: falls back to conf/default.conf
curl http://127.0.0.1:8080/
```

`conf/test.conf` and the `www/` directory are a working demo config +
site exercising every feature: static files, directory autoindex,
redirects, a custom 404 page, file upload (`POST /upload/...`), `DELETE`,
and Python CGI scripts under `www/cgi-bin/` (`hello.py`, `echo.py`, plus
`slow.py`/`hang.py` used only to test the CGI timeout/concurrency path).
A second `server{}` block listens on `:8081` with a 10-byte body limit to
exercise `413 Payload Too Large`.

### What's been tested and how

Functional (curl / raw-socket scripts): static GET, custom + default
error pages, 301 redirect, 405 with `Allow` header, upload → GET →
DELETE → 404 lifecycle, CGI over GET and POST, a hand-crafted multi-chunk
`Transfer-Encoding: chunked` body reaching the CGI correctly un-chunked,
request pipelining/keep-alive on one TCP connection, `413` on both the
`Content-Length` and chunked paths, a malformed request line, an
unsupported HTTP version, a directory traversal attempt
(`/listing/../../etc/passwd`, rejected with `403` before touching the
filesystem), a malformed config file (rejected at startup without a
crash), two server processes racing for the same port (second one fails
cleanly, exit 1, first keeps serving), and duplicate `listen` lines
inside one config (same result).

Single-`poll()` concurrency (this is the property the whole CGI
redesign below exists to prove): a fast static request completes in
~0ms while a slow CGI (deliberately sleeping 3s) is still in flight on a
separate connection -- the old design would have serialized these. A
CGI that never terminates is killed with `504 Gateway Timeout` after
its deadline, and the *rest of the server stays fully responsive* while
that happens.

Stress / availability (a hand-rolled equivalent of `siege -b`, since
neither `siege` nor `valgrind` are installed in this environment and
installing them needs `sudo` this session doesn't have a password for):
25 concurrent connections hammering a static page for 20s sustained
**100.000% availability across 138,289 requests** with **8KB of RSS
growth total** (4364KB -> 4372KB) -- run twice back-to-back with no
restart and no degradation. A second script drives the CGI path itself
(10 concurrent connections POSTing to a CGI script) at **100.000% over
2,500+ requests** after the fd-reuse fix below.

Memory safety: the whole functional + concurrency + stress suite above,
re-run against a `-fsanitize=address,undefined` build with a clean
`SIGINT` shutdown (added specifically so LeakSanitizer's exit-time check
actually gets to run) -- zero ASan/UBSan reports, zero LeakSanitizer
reports. Consistent with there being no `new`/`malloc` anywhere in this
codebase (`grep -rn "new \|malloc\|calloc" srcs/ include/` is empty) --
every allocation is `std::string`/`std::vector`/`std::map`, RAII-managed.

`make re` builds clean with `-Wall -Wextra -Werror -std=c++98`.

## Scope & boundaries

- **`include/connection.hpp`** is the shared struct from the guide, plus
  two additive fields explicitly called out in comments there
  (`Connection::server_conf`, so `handle_request()` has something to
  route against, and `Connection::query_string`, needed for CGI). Nothing
  the guide specifies was removed or renamed.
- **`Config.{hpp,cpp}`** (config file parsing) is officially the Core
  Server side's job per the guide's own division of labor. It exists here
  only so this module isn't developed against a stub — replace it, or
  merge it, when integrating with the real core server.
- **`harness_main.cpp`** is an explicitly throwaway, single-`poll()`,
  non-blocking event loop good enough to drive real traffic at this code
  for testing (accept/read/parse/handle/write, keep-alive, multiple
  listen sockets). It is **not** meant to become the team's submitted
  core server — a comment at the top of the file says so.
- **CGI/poll integration**: the guide flags folding the CGI pipes into
  the *single shared* `poll()` as an open design question to settle with
  the teammate (part 5, "Point à discuter ensemble avant le jour 4-5").
  This is now resolved: `connection.hpp` adds a `CGI_RUNNING` state, and
  `handle_request()` leaves a connection in it instead of finishing
  synchronously when it starts a CGI. `harness_main.cpp` registers the
  CGI's `cgi_stdin_fd`/`cgi_stdout_fd` into the *same* `poll_fds` vector
  as every client socket and drives them through the one shared `poll()`
  call, calling `CgiHandler`'s `onStdinWritable()`/`onStdoutReadable()`
  only on a reported readiness bit — never a bare `read()`/`write()`, and
  never branching on `errno` after one. The interface (`CgiHandler.hpp`)
  is written so a real Core Server loop can integrate it the same way.
- The subject's allowed-external-functions list (p.6) does not include
  `unlink`/`remove`, which `DELETE` requires. `std::remove()` is used
  regardless since there is no way to implement the mandatory `DELETE`
  method without it; worth a one-line mention if it comes up at defense.

## Resources

- RFC 7230 (HTTP/1.1 message syntax) and RFC 3875 (CGI/1.1), read while
  implementing chunked transfer-encoding decoding and the CGI
  environment-variable contract.
- `nginx`'s `server`/`location` config syntax, as the subject suggests,
  for `conf/test.conf`'s shape.

### AI usage

This module's code (all files under `include/` and `srcs/`, the test
config/site under `conf/` and `www/`, and this README) was generated by
Claude (Anthropic), in an interactive session, from the subject PDF and
`guide_jour1_HTTP_CGI.md`. Every design decision above — the config
schema, the traversal/body-size/chunked-size guards, the keep-alive
pipelining fix found by testing, and the CGI/poll boundary — was reviewed
and is understood by the person submitting this work, who can explain and
modify any part of it at defense.
