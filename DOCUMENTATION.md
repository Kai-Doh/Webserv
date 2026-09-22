# Webserv — HTTP + CGI Module: Documentation

This documents **Kai's half** of the two-person 42 `Webserv` project: HTTP
request parsing, request routing, and CGI execution — everything reachable
from `try_parse_request()` and `handle_request()`, the two functions that
are the entire contract with the Core Server (William's half: sockets,
`accept()`, the shared `poll()` loop, in `srcs/core/`).

Not covered here on purpose:

- **The Core Server itself** (`srcs/core/`, `srcs/net/`) — William's part.
  It's referenced below only where it calls into this module, since that's
  the actual integration surface.
- **Tooling/editor setup** — local machine configuration, not part of the
  project.

The two halves are wired together and run as one binary: `srcs/main.cpp`
loads the config and hands off to `Server::run()` (`srcs/core/Server.cpp`),
which drives `try_parse_request()`/`handle_request()` for every connection
exactly the way this document describes.

---

## Part 1 — The request lifecycle, start to finish

This is the actual path a request takes through the code, in order. Every
step names the file responsible.

### 1. Startup and the event loop

`srcs/main.cpp` resolves the config path (the argument if given, else
`conf/default.conf` — subject p.8: "provided as an argument on the command
line, or available in a default path"), calls `Config::load()`
(`srcs/config/Config.cpp`) to parse it into one `ServerConfig` per
`server{}` block, then constructs a `Server` and calls `run()`. A malformed
config throws `std::runtime_error`, caught in `main()` and turned into a
clean `exit(1)` — never a crash.

From there it's the Core Server's loop: exactly **one** `poll()` call per
iteration, covering every listening socket, every client socket, and every
CGI pipe currently in flight — the subject's core requirement (p.8). After
`poll()` returns, every fd with a nonzero `revents` is dispatched: a
listening socket gets `accept()`ed; a CGI pipe is routed to
`cgi_handler::onStdinWritable()`/`onStdoutReadable()`; a client socket in
`READING_REQUEST` reaches this module's code, below.

### 2. Read (`Server::handleRead()` → `srcs/http/RequestParser.cpp`)

A ready client socket gets `read()`, the bytes are appended to
`conn.read_buffer`, and **`try_parse_request(conn)`** is called — this is
where this module's code starts.

`try_parse_request()` works incrementally, since a request can arrive in
pieces, and is called again on every partial read while the body is still
streaming in:

1. If headers aren't parsed yet for this request (`conn.headers_ready ==
   false`): look for the header/body separator (`\r\n\r\n`, or a bare
   `\n\n` for telnet-friendliness). Not found yet → return `false`, wait
   for more bytes (unless the buffer's already past 8192 bytes with no
   separator in sight → `431`).
2. Once found, check the **request line's own length** first (→ `414` if
   too long) *before* checking the whole header section's length (→
   `431`) — a long URI always also blows past the header-section cap, so
   checking section-size first would make `414` unreachable.
3. Split into method / target / HTTP version. Reject a malformed line, an
   empty or non-`/`-rooted target, a non-`HTTP/` version string (`400`),
   or an unsupported version (`505`).
4. Parse headers into a lower-cased `std::map`. A repeated
   `Content-Length` with a *different* value is rejected (`400` — a
   request-smuggling guard). `HTTP/1.1` with no `Host` header is rejected
   (`400`).
5. Decide keep-alive from the version + any `Connection` header.
6. Commit `method`/`path`/`headers`/`query_string`/`keep_alive` and the
   body's start offset into `conn` and set `conn.headers_ready = true` —
   every later call for this same request skips steps 1–6 entirely and
   reads these back out of `conn` instead. This isn't just an
   optimization: re-deriving the body offset via a fresh string search on
   every call, instead of caching it once, is what caused a real O(n²)
   blowup found via the official tester's own stress test (20 concurrent
   100MB CGI POSTs pegged a CPU core for 9+ minutes before the cache was
   added — see `conn.headers_ready`'s and `conn.body_start`'s comments in
   `include/connection.hpp`).
7. Read the body: `Transfer-Encoding: chunked` goes through
   `request_parser::decodeChunked()`, which also resumes from
   `conn.chunked_scan_pos` rather than re-scanning already-decoded chunks
   (the same O(n²) class of bug, found and fixed the same way); otherwise
   `Content-Length` is trusted (bounds-checked against
   `client_max_body_size` — server-wide, or a location's own override if
   it set one — → `413`); no header at all means a zero-length body (RFC
   7230 3.3.3 case 6 — not an error).
8. On success, runs the decoded path through `collapseSlashes()` (so
   `//foo` and `/foo` route identically) and returns `true`.

Every failure path goes through `failParse()`, which sets
`conn.status_code` and trims the offending bytes from `read_buffer` — it
never throws, so a single bad request never takes the process down.

### 3. Route (`srcs/http/RequestHandler.cpp`, `handle_request()`)

Once `try_parse_request()` returns `true`, the Core Server calls
**`handle_request(conn)`** — the other half of the contract. In order:

1. If a parse error already set `conn.status_code`, write that error and
   stop.
2. `conn.server_conf->matchLocation(conn.path)` — longest-prefix match
   (`srcs/config/Config.cpp`), nginx-style. A location declared with a
   trailing slash (`/directory/`) also matches the slash-less form
   (`/directory`), so step 10 below can redirect it properly instead of
   404ing through the `/` catch-all.
3. No match → `404`.
4. A `return` directive on the location → the configured redirect code
   (301/302/...) with a `Location` header.
5. Method not in the location's `methods` list → `405` with an `Allow`
   header listing what *is* allowed.
6. Compute `rel` (path remainder after the location prefix) and reject any
   `..` segment (`403`, directory-traversal guard) before touching the
   filesystem at all.
7. `DELETE`: 404 if the file doesn't exist, 403 if it's a directory,
   `std::remove()` it, 204 on success (or 403 on failure).
8. **CGI dispatch**: if the resolved path's extension is in the location's
   `cgi_extensions` map — whether or not a real file exists there (some
   interpreters, including the official 42 `cgi_tester` binary, answer
   for *any* matching path regardless of the filesystem) — or a *prefix*
   of the remaining path is such a file (RFC 3875 `PATH_INFO`, e.g.
   `/cgi-bin/script.py/extra/thing`), hand off to `cgi_handler::start()`
   and leave `conn.state = CGI_RUNNING` instead of writing a response
   directly.
9. `POST`: a location with no `upload_store` configured acknowledges with
   `200` without persisting anything (a location can legitimately accept
   a POST body it doesn't need to store); otherwise the body is written to
   disk under `upload_store` (generating a filename if the URL didn't name
   one) and the response is `201 Created`.
10. Remaining case is `GET`: 404 if nothing's there; if it's a directory
    without a trailing slash, `301` to the slash-terminated form (browsers
    resolve relative links against the URL, so serving content at the
    slash-less path breaks them); else serve the configured `index` file,
    or an autoindex listing, or `404` if neither is available (not `403`
    — from the client's perspective there's simply nothing at that URL);
    otherwise serve the file directly with a guessed `Content-Type`.

### 4a. Non-CGI response → write

`handle_request()` filled `conn.write_buffer` via
`request_handler::writeResponse()`/`writeErrorResponse()`
(`RequestHandler.cpp`) — full status line, `Server`/`Connection`/
`Content-Type`/`Content-Length` headers, plus the body (dropped entirely
for `HEAD`, per RFC 7231 — sending it anyway desyncs a keep-alive
connection for any compliant client). The Core Server flips that fd's poll
events to `POLLOUT` and, once ready, writes the buffer — possibly across
several `poll()` iterations for a large response. On completion:
keep-alive resets the connection (including this module's per-request
parser state — `headers_ready`, `body_start`, `chunked_scan_pos`) and
immediately tries to parse again in case a pipelined next request is
already sitting in the buffer; otherwise the connection closes.

### 4b. CGI response → the pipe dance (`srcs/cgi/CgiHandler.cpp`)

If `handle_request()` started a CGI, the Core Server registers
`conn.cgi_stdin_fd`/`cgi_stdout_fd` (only whichever aren't `-1`) into the
*same* `poll_fds` array as every socket. From here:

- `cgi_handler::start()` already forked, `dup2()`'d the pipes into the
  child's stdin/stdout, `chdir()`'d into the script's directory (so
  relative file access from the script works), built the full CGI/1.1
  environment via `buildEnv()`, and called `execve()`. The parent side
  never blocks — it returns immediately with both fds non-blocking.
- `PATH_INFO` is normally the RFC-3875 trailing path segment (empty when
  the script itself is the exact request target). The official
  `cgi_tester` binary instead expects it to equal the *full* request path
  in that exact-match case — confirmed by running it directly with
  controlled environment variables. `buildEnv()` only takes that fallback
  when there's genuinely no RFC-3875 trailing segment, so a real
  `PATH_INFO`-walking request is unaffected.
- Every time `poll()` reports the stdin pipe writable, `onStdinWritable()`
  writes one chunk of `conn.body`; every time it reports the stdout pipe
  readable, `onStdoutReadable()` reads one chunk into `conn.cgi_out`.
  Neither fd is `close()`'d by these functions, only marked `-1` — the
  actual `close()` is deferred to the Core Server's own bookkeeping, so a
  reused fd number can never collide with bookkeeping for the old one
  still in flight.
- Once both are `-1` (`isDone()`), `finish()` tries a non-blocking
  `waitpid()` to tell an `execve()` failure (exit 127, no output → `502
  Bad Gateway`) or a signal-killed script (e.g. a segfault) with no output
  apart from a script that legitimately produced nothing (→ `200`). A
  child's pipes closing and it becoming *reapable* are two separate
  kernel events, so immediately after EOF `waitpid()` can still return 0;
  `finish()` reports "not ready" instead of guessing, and the caller
  retries on the next iteration.
- If `conn.cgi_deadline` (10s from start) passes first, `abortTimeout()`
  `SIGKILL`s the child and writes `504 Gateway Timeout` — the rest of the
  server keeps serving other connections throughout (verified live: a
  concurrent plain `GET` completes in under a millisecond while a hung
  CGI is still in flight on another connection).
- Either way, a still-unreaped pid is swept opportunistically
  (`reapIfExited()`, non-blocking) — `waitpid(-1, ...)` ("reap anything")
  is never called, since that could steal an unrelated CGI's exit status.

---

## Part 2 — Every file (this module's)

### `include/` — the shared contract

**`connection.hpp`** — the canonical `Connection` struct and `ConnState`
enum, shared with the Core Server (`srcs/core/`, `srcs/shared/`) via a
forwarding header there — see its own comment for why. Fields owned by
this module: `method`, `path`, `http_version`, `headers`, `body`,
`query_string`, `cgi_path_info`, `status_code`, the `CGI_RUNNING`
bookkeeping (`cgi_out`, `cgi_in_offset`, `cgi_deadline`), and the
incremental-parsing state added this session (`headers_ready`,
`body_start`, `chunked_scan_pos` — see Part 1 §2.6–7 for why each exists).
`try_parse_request`/`handle_request` are declared here since they're the
shared contract; `CGI_RUNNING` is what lets `handle_request()` leave a
connection mid-flight instead of always finishing synchronously, so CGI
can share the one `poll()` loop.

**`Config.hpp`** — a forwarding shim to `srcs/config/Config.hpp` (kept so
any code, on either side, that still includes it bare from `include/`
keeps compiling unchanged).

### `srcs/http/` — request parsing and routing

**`RequestParser.hpp`/`.cpp`** — `try_parse_request()` (Part 1 §2) and
`request_parser::decodeChunked()`, plus file-local `findHeaderEnd()`,
`collapseSlashes()`, `failParse()`.

**`RequestHandler.hpp`/`.cpp`** — `handle_request()` (Part 1 §3),
`writeResponse()`/`writeErrorResponse()` (exposed so `CgiHandler.cpp` can
produce error pages identical to every other error path), and the routing
helpers: `joinPath()` (alias-style root+rel joining, matching the
subject's own `/kapouet` example), `hasDotDotSegment()`,
`extensionOf()`/`basenameOf()`, `resolveCgiScript()` (the
`PATH_INFO`-aware CGI lookup), `serveFile()`, `serveAutoindex()`.

**`HttpStatus.hpp`/`.cpp`** — `reasonPhrase()`, `defaultErrorBody()`,
`mimeType()`. Tiny, stateless lookup helpers.

### `srcs/cgi/` — CGI execution

**`CgiHandler.hpp`/`.cpp`** — the full CGI interface (Part 1 §4b):
`start()`, `onStdinWritable()`/`onStdoutReadable()`, `isDone()`,
`finish()`, `abortTimeout()`, `reapIfExited()`, plus file-local
`splitDirFile()`, `parentPath()`, `buildEnv()` (the full CGI/1.1
environment — `REQUEST_METHOD`, `SCRIPT_NAME`/`PATH_INFO` split,
`SCRIPT_FILENAME`, `QUERY_STRING`, `CONTENT_LENGTH`/`CONTENT_TYPE`,
`SERVER_*`, `REQUEST_URI`, `REMOTE_ADDR`, `PATH`, and every request header
as `HTTP_*`), and `finishFromCgiOutput()` (parses the CGI's own
`Status:`/`Content-Type:` headers out of its stdout).

### `srcs/config/` — configuration

**`Config.hpp`/`.cpp`** — `Location` (one route's config) and
`ServerConfig` (one `server{}` block) structs, `Location::methodAllowed()`,
`ServerConfig::matchLocation()`, and the config-file parser:
`tokenize()` (whitespace/`{}`/`#`-comment tokenizer), `parseLocation()`,
`parseServer()`, `Config::load()`. Every directive the config format
supports (`listen`, `server_name`, `client_max_body_size`, `error_page`,
`root`, `index`, `autoindex`, `methods`, `return`, `upload_store`, `cgi`,
and a location-level `client_max_body_size` override) is a branch in
`parseLocation()`/`parseServer()`; an unknown directive or malformed value
throws, never crashes.

### `srcs/utils/` — shared helpers

**`StringUtils.hpp`/`.cpp`** — `trim`, `toLower`/`toUpper`, `split`,
`startsWith`, `toString` (long/size_t → string, since C++98 has no
`std::to_string`), `toLong` (strict, no `strtol` trailing-garbage
tolerance), `urlDecode`, `headerKeyToEnv`. The small building blocks
everything else is written in terms of. Nothing here does heap allocation
beyond ordinary `std::string`/`std::vector` growth — consistent with the
whole module having zero `new`/`malloc`/`calloc` calls.

### `conf/` — configuration files

**`default.conf`** — used when `webserv` is launched with no argument.
Same site as `test.conf`'s first `server{}` block, listening on
`0.0.0.0:8080`.

**`test.conf`** — the main demo config, two `server{}` blocks:
- `:8080` — the real site (`www/`): static `/`, autoindex on `/listing`,
  a `301` (`/old`) and a `302` (`/moved`) redirect, `/cgi-bin` (Python
  `.py` scripts plus a deliberately-broken `.broken` extension mapped to
  a nonexistent interpreter, for exercising `502` on demand), and
  `/upload` (GET/POST/DELETE, uploads land in `www/upload`).
- `:8081` — a genuinely different site (`www/site2/`), with a 10-byte
  `client_max_body_size` to demonstrate per-server body limits and `413`.

**`tester.conf`** — config for the official 42 `tester` Go binary.
Routes match exactly what that tool's interactive prompts demand: `/`
GET-only, `/directory/` aliasing the `YoupiBanane` fixture (POST + `.bla`
CGI mapping included, since the tester exercises that combination there
too, not just at `/youpi.bla`), `/youpi.bla` as an exact-path CGI location
(root pointed at the file itself, not its directory), and `/post_body`
with a location-level `client_max_body_size 100` (the tester's own stated
requirement). The `.bla` extension maps to `../../cgi_tester` — relative,
not a hardcoded personal path: the CGI child `chdir()`s into
`www/YoupiBanane` before `execve()`, and `execve()` never does a `PATH`
search, so this resolves correctly on any machine once the official
`cgi_tester` binary is dropped at the repo root.

### `www/` — the site itself

Static site (`index.html`, `about.html`, `dashboard.html`, `styles.css`)
plus fixtures: `cgi-bin/` (Python scripts covering GET/POST CGI,
`PATH_INFO`, a deliberately hanging/slow script, and a script-that-isn't
for exercising `502`), `errors/404.html` (custom error page), `listing/`
(autoindex demo files), `upload/` + `site2/uploads/` (runtime upload
targets, empty in git via `.gitkeep`), `site2/` (the distinct second site
on `:8081`), and `YoupiBanane/` (fixture layout the official 42 `tester`
binary's setup instructions demand).

---

## Part 2a — Quick reference: structures & functions

A scannable index to keep open while reading source — Part 1 explains the
*flow*, this is the *map*. One row per struct field / function; file:line so
you can jump straight there.

### `Connection` — one per client fd (`include/connection.hpp`)

There's no separate "connection ID" anywhere in the code: the client's fd
(from `accept()`) is both the key into `Server`'s `ConnMap` and
`conn.fd` itself. Every buffer below is private to that one connection —
nothing here is shared across fds.

| Field | Type | Purpose |
|---|---|---|
| `fd` | `int` | This connection's socket fd — same value as its key in `Server::_connections`. |
| `state` | `ConnState` | `READING_REQUEST` → `PROCESSING` → (`CGI_RUNNING` →) `WRITING_RESPONSE` → `DONE`. Drives which `Server::handle*()` function runs on the next `poll()` event (`ServerLoop.cpp:handleClientEvent`). |
| `read_buffer` | `string` | Raw bytes from `read()`, appended to on every `handleRead()` call. Trimmed from the front once a full request is consumed — leftover bytes here are a pipelined *next* request. |
| `write_buffer` | `string` | The complete response, built once by `writeResponse()`. Sent out over possibly several `write()`s. |
| `bytes_written` | `size_t` | How much of `write_buffer` has gone out so far — `handleWrite()`'s resume point. |
| `keep_alive` | `bool` | Whether to reset for another request after this one finishes, or close the fd. |
| `last_activity` | `time_t` | Updated on every successful read/write; `sweepTimeouts()` closes fds silent for 60s+. |
| `server_conf` | `const ServerConfig*` | Which `server{}` block this connection belongs to — set once at `accept()` time, from the listening socket it arrived on. |
| `method`, `path`, `http_version` | `string` | Parsed request line. `path` is already URL-decoded + slash-collapsed. |
| `headers` | `map<string,string>` | Lower-cased header names → values. |
| `body` | `string` | The fully assembled request body (post Content-Length/chunked decoding). |
| `query_string` | `string` | Everything after `?` in the target — **not** URL-decoded (kept raw for CGI's `QUERY_STRING`). |
| `cgi_stdin_fd`, `cgi_stdout_fd` | `int` | This connection's CGI pipe fds; `-1` once each side is done (see `CgiHandler.cpp`'s note on why they're marked, not `close()`'d, inline). |
| `cgi_pid` | `pid_t` | The CGI child's pid; `-1` when none is running. |
| `cgi_path_info` | `string` | RFC 3875 `PATH_INFO` — the trailing path segment past the script, if any. |
| `status_code` | `int` | Set by a parse failure (`failParse()`) or by `handle_request()`; `0` means "no error yet". |
| `cgi_out` | `string` | Accumulated CGI stdout bytes, fed to `finishFromCgiOutput()` once done. |
| `cgi_in_offset` | `size_t` | How much of `body` has already been written to the CGI's stdin. |
| `cgi_deadline` | `time_t` | `start()` time + 10s; `sweepCgi()` SIGKILLs past this. |
| `headers_ready` | `bool` | **Perf-critical cache**: true once the request line + headers are parsed for the *current* request, so every later `try_parse_request()` call (as more body bytes trickle in) skips re-parsing them from scratch. |
| `body_start` | `size_t` | Cached offset into `read_buffer` where the body begins — the O(n²) fix from Part 1 §2.6. |
| `chunked_scan_pos` | `size_t` | Cached offset into an in-progress chunked body already decoded — same O(n²) fix, for `decodeChunked()`. |

`ConnState` enum: `READING_REQUEST`, `PROCESSING`, `CGI_RUNNING`,
`WRITING_RESPONSE`, `DONE`.

### `Server`'s own fd bookkeeping (`srcs/core/Server.hpp`, core-side — referenced here only because it's what actually holds one `Connection` per fd)

| Member | Type | Purpose |
|---|---|---|
| `_connections` | `map<int, Connection>` | The fd → `Connection` table itself — this *is* "a buffer per connection ID". |
| `_poll_fds` | `vector<pollfd>` | The flat array handed to `poll()` every iteration: every listener, every client fd, every CGI pipe, all in one call. |
| `_listen_fds` | `map<int, Socket*>` | Listening-socket fd → the `Socket` object (not owned here). |
| `_listener_config` | `map<int, const ServerConfig*>` | Listening-socket fd → which `server{}` block it serves, so a new client inherits the right config at `accept()`. |
| `_cgi_owner` | `map<int, int>` | A CGI pipe fd → the *client* fd that owns it, so a pipe event (`handleCgiEvent()`) can find the right `Connection`. |
| `_pending_reap` | `vector<pid_t>` | CGI pids killed/finished but not yet `waitpid()`-reapable; retried non-blockingly every loop (`reapPending()`). |
| `_spare_fd` | `int` | A held-back `/dev/null` fd, freed then immediately reused to `accept()`-and-drop a client when the process is out of fds (`rejectWhenOutOfFds()`). |

### `Location` / `ServerConfig` (`srcs/config/Config.hpp`)

| Struct | Key fields | Purpose |
|---|---|---|
| `Location` | `path`, `root`, `methods`, `autoindex`, `index`, `redirect_target`/`redirect_code`, `upload_enabled`/`upload_store`, `cgi_extensions` (map ext → interpreter), `client_max_body_size` (or `NO_BODY_SIZE_OVERRIDE`) | One `location{}` block. |
| `ServerConfig` | `host`, `port`, `server_name`, `client_max_body_size`, `error_pages` (map code → path), `locations` | One `server{}` block. |

### Function index — HTTP/CGI module

| Function | Where | One-liner |
|---|---|---|
| `try_parse_request()` | `RequestParser.cpp:157` | Entry point: turns `conn.read_buffer` into a parsed request (or an error), incrementally. |
| `request_parser::decodeChunked()` | `RequestParser.cpp:16` | Decodes a `Transfer-Encoding: chunked` body, resumable via `chunked_scan_pos`. |
| `findHeaderEnd()` | `RequestParser.cpp:84` | Locates `\r\n\r\n` (or bare `\n\n`) — the header/body boundary. |
| `collapseSlashes()` | `RequestParser.cpp:111` | Merges `//` runs so `//foo` and `/foo` route identically. |
| `failParse()` | `RequestParser.cpp:132` | Sets `conn.status_code`, trims consumed bytes — every parse-error exit goes through here. |
| `handle_request()` | `RequestHandler.cpp:249` | Entry point: routes a parsed request, writes a response (or starts a CGI). |
| `joinPath()` | `RequestHandler.cpp:30` | Alias-style `root` + `rel` filesystem path join. |
| `hasDotDotSegment()` | `RequestHandler.cpp:45` | Directory-traversal guard — rejects a literal `..` path segment. |
| `extensionOf()` / `basenameOf()` | `RequestHandler.cpp:58` / `67` | Small path-string helpers (`.py`, last `/`-segment). |
| `resolveCgiScript()` | `RequestHandler.cpp:94` | `PATH_INFO`-aware CGI lookup — finds a script hiding behind trailing segments. |
| `serveFile()` / `serveAutoindex()` | `RequestHandler.cpp:127` / `148` | Static-file response / directory-listing response. |
| `request_handler::writeResponse()` | `RequestHandler.cpp:193` | Assembles a full HTTP response into `conn.write_buffer`. |
| `request_handler::writeErrorResponse()` | `RequestHandler.cpp:217` | Same, using a configured `error_page` if one exists for that code. |
| `reasonPhrase()` / `defaultErrorBody()` / `mimeType()` | `HttpStatus.cpp:7` / `36` / `49` | Stateless status-code/MIME lookup tables. |
| `cgi_handler::start()` | `CgiHandler.cpp:191` | Forks, pipes, `chdir()`s, `execve()`s — never blocks. |
| `buildEnv()` | `CgiHandler.cpp:58` | Builds the full CGI/1.1 environment (RFC 3875). |
| `cgi_handler::onStdinWritable()` / `onStdoutReadable()` | `CgiHandler.cpp:276` / `296` | One pipe I/O chunk per `poll()` event. |
| `cgi_handler::isDone()` | `CgiHandler.cpp:308` | True once both CGI pipes are marked closed. |
| `cgi_handler::finish()` | `CgiHandler.cpp:326` | Reaps + classifies the CGI's exit (`200`/`502`), or reports "not ready yet". |
| `finishFromCgiOutput()` | `CgiHandler.cpp:117` | Parses the CGI's own `Status:`/`Content-Type:` out of its stdout, builds the response. |
| `cgi_handler::abortTimeout()` | `CgiHandler.cpp:357` | `SIGKILL`s a CGI past its deadline, writes `504`. |
| `cgi_handler::reapIfExited()` | `CgiHandler.cpp:379` | Non-blocking opportunistic `waitpid()` for `_pending_reap`. |
| `Location::methodAllowed()` | `Config.cpp:9` | Is `method` in this location's allow-list? |
| `ServerConfig::matchLocation()` | `Config.cpp:23` | Longest-prefix `location{}` match for a request path. |
| `Config::load()` / `tokenize()` / `parseServer()` / `parseLocation()` | `Config.cpp:217` / `58` / `162` / `100` | Config-file reading → tokens → `ServerConfig`/`Location` structs. |
| `su::trim` / `toLower` / `toUpper` / `split` / `startsWith` / `toString` / `toLong` / `urlDecode` / `headerKeyToEnv` | `StringUtils.cpp` | Small stateless string helpers everything above is built from. |

---

## Part 3 — What's been verified

Beyond ordinary `curl`, this module's behavior has been checked against:

- **A real browser** exercising the dashboard's live test buttons (`fetch()`
  calls, a real file upload, a real form submission).
- **The official 42 `tester` Go binary**, against `conf/tester.conf` + the
  `YoupiBanane` fixture, including its heaviest concurrency case (20
  workers × 5 requests each, 100MB CGI POST per request; 128 concurrent
  workers hammering a single route): **exits 0, no failures**, against the
  real integrated `Server` + this module, verified on a real Linux
  machine. This run is what caught most of the real bugs below.
- **`valgrind`**, functional suite + a clean `SIGINT` shutdown: no leaks,
  no invalid-fd reports — consistent with there being no `new`/`malloc`
  anywhere in this module.

Real bugs found this way and fixed, roughly in the order found:

- **HEAD responses leaking a body** (RFC 7231 violation) — fixed in
  `writeResponse()`.
- **No trailing-slash redirect for directories** — fixed in
  `ServerConfig::matchLocation()` + `handle_request()`.
- **A genuine CGI race** in `finish()` between a child's pipes closing and
  it becoming reapable, which could misreport `502` as `200` roughly 1 in
  3 requests — fixed by reporting "not ready" instead of guessing.
- **`431` silently unenforced** for a header section that arrived whole in
  a single `read()` (only the still-growing case was checked) — fixed,
  which then made `414` unreachable for an overlong URI, fixed again by
  checking the request line's own length first.
- **`PATH_INFO` (RFC 3875)** wasn't implemented at all originally — added
  via `resolveCgiScript()`.
- **An O(n²) blowup in `try_parse_request()`**, found via the official
  tester's own 20-concurrent-100MB-CGI-POST stress case (a CPU core
  pegged for 9+ minutes with zero progress): the chunked decoder re-scanned
  and re-copied the whole body-so-far on every partial read, and — even
  after fixing that — the header/body separator was still being
  re-derived via a full string search on every call too. Fixed by making
  both resumable/cached instead of redone from scratch (`chunked_scan_pos`,
  `body_start`).
- **`cgi_tester`'s non-standard `PATH_INFO` expectation** and **CGI
  dispatch requiring the target file to exist** (it shouldn't, for an
  interpreter like `cgi_tester` that never touches the filesystem) — both
  found by running the official tester and reverse-engineering its exact
  checks, both fixed in `CgiHandler.cpp`/`RequestHandler.cpp`.
- **Missing per-location `client_max_body_size`** — the config format only
  ever supported a server-wide limit, but the official tester's
  `/post_body` route needs its own tighter one. Added as a `Location`-level
  override.

---

## Part 4 — Where things stand

- Code lives on the `kai` branch of `github.com/Kai-Doh/Webserv`, merged
  into `main`; `william` holds the Core Server side.
- `main` is a working, integrated `webserv` binary: `srcs/main.cpp` →
  `Server::run()` → this module, CGI pipes included — not two halves
  sitting side by side.
- Every non-trivial function across this module has a `@brief`/`@param`/
  `@return` doc-comment.
- Outstanding: nothing blocking on this module's side. Ongoing
  maintenance (new config directives, additional CGI env vars, etc.)
  would extend `srcs/config/Config.cpp` and `srcs/cgi/CgiHandler.cpp`
  respectively.
