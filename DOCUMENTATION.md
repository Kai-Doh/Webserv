# Webserv — HTTP + CGI Module: Full Documentation

This documents **Kai's half** of the two-person 42 `Webserv` project: HTTP
request parsing, request routing, and CGI execution — everything reachable
from `try_parse_request()` and `handle_request()`, the two functions that
are the entire contract with William's Core Server (sockets, `accept()`,
the shared `poll()` loop). See `guide_jour1_HTTP_CGI.md` for how that split
was defined on day one.

Two things are *not* part of this documentation on purpose:

- **The Core Server itself.** `srcs/harness_main.cpp` is a stand-in main
  loop written so this half could be tested end-to-end while William
  builds the real one — it's documented below because it's currently the
  only thing that makes the code run, not because it's the deliverable.
- **Tooling/editor setup** (nvim, clangd, etc.) — that's local machine
  configuration, not part of the project.

---

## Part 1 — The request lifecycle, start to finish

This is the actual path a request takes through the code, in order. Every
step names the file responsible.

### 1. Startup (`srcs/harness_main.cpp`, `main()`)

- Installs signal handlers: `SIGPIPE` ignored (so a client closing mid-write
  doesn't kill the process), `SIGINT`/`SIGTERM` set a flag that lets the
  loop exit cleanly (needed so valgrind/leak checkers get to run their
  exit-time reports at all).
- Resolves the config path: the argument if given, else
  `conf/default.conf` (subject p.8: "provided as an argument on the
  command line, or available in a default path").
- `Config::load()` (`srcs/Config.cpp`) parses it into one `ServerConfig`
  per `server{}` block. Any malformed config throws `std::runtime_error`,
  caught here and turned into a clean exit(1) — never a crash.
- One `openListenSocket()` per server block: `socket()` → `SO_REUSEADDR` →
  `bind()` → `listen()` → set non-blocking. Each listening fd is recorded
  in `poll_fds` and mapped to its `ServerConfig` in `listenerConfig`.

### 2. The event loop (`harness_main.cpp`, the `while` loop)

Exactly **one** `poll()` call per iteration, covering every listening
socket, every client socket, and every CGI pipe currently in flight — the
subject's core requirement (p.8). The timeout passed to it is computed
each iteration: `-1` (block forever) normally, `100ms` if any CGI pid is
waiting to be reaped or any CGI's pipes closed but isn't reapable yet, or
`1000ms` if a CGI is merely running (so its deadline gets checked
periodically even with no I/O activity).

After `poll()` returns, every fd with a nonzero `revents` is dispatched:

### 3. Accept

If the ready fd is a listening socket, `accept()` it, mark the new client
fd non-blocking, create a `Connection` for it (`fd` and `server_conf` set,
everything else default), and queue it to join `poll_fds`.

### 4. Read (`harness_main.cpp` → `srcs/RequestParser.cpp`)

If the ready fd is a client in `READING_REQUEST`, `read()` up to 4096
bytes, append them to `conn.read_buffer`, then call `pump()`, which calls
**`try_parse_request(conn)`** — this is where your code starts.

`try_parse_request()` (`RequestParser.cpp`) works incrementally, since a
request can arrive in pieces:

1. Look for the header/body separator (`\r\n\r\n`, or a bare `\n\n` for
   telnet-friendliness). Not found yet → return `false`, wait for more
   bytes (unless the buffer's already past 8192 bytes with no separator in
   sight → `431`).
2. Once found, check the **request line's own length** first (→ `414` if
   too long) *before* checking the whole header section's length (→
   `431`) — a long URI always also blows past the header-section cap, so
   checking section-size first would make `414` unreachable. This ordering
   was one of the real bugs found and fixed this session.
3. Split into method / target / HTTP version. Reject a malformed line, an
   empty or non-`/`-rooted target, a non-`HTTP/` version string (`400`),
   or an unsupported version (`505`).
4. Parse headers into a lower-cased `std::map`. A repeated
   `Content-Length` with a *different* value is rejected (`400` — a
   request-smuggling guard). `HTTP/1.1` with no `Host` header is rejected
   (`400`).
5. Decide keep-alive from the version + any `Connection` header.
6. Read the body: `Transfer-Encoding: chunked` goes through
   `request_parser::decodeChunked()` (a separate function so it can be
   unit-tested / reused); otherwise `Content-Length` is trusted (bounds-
   checked against `client_max_body_size` → `413`); no header at all means
   a zero-length body (RFC 7230 3.3.3 case 6 — not an error).
7. On success, fills `conn.method/path/query_string/headers/body/http_version`,
   runs the decoded path through `collapseSlashes()` (so `//foo` and `/foo`
   route identically), and returns `true`.

Every failure path goes through `failParse()`, which sets
`conn.status_code` and trims the offending bytes from `read_buffer` — it
never throws, so a single bad request never takes the process down.

### 5. Route (`srcs/RequestHandler.cpp`, `handle_request()`)

Once `try_parse_request()` returns `true`, `pump()` calls
**`handle_request(conn)`** — the other half of the contract. In order:

1. If a parse error already set `conn.status_code`, write that error and
   stop.
2. `conn.server_conf->matchLocation(conn.path)` — longest-prefix match
   (`srcs/Config.cpp`), nginx-style, with one extra rule added this
   session: a location declared with a trailing slash (`/directory/`)
   also matches the slash-less form (`/directory`), so step 8 below can
   redirect it properly instead of 404ing through the `/` catch-all.
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
8. **CGI dispatch**: if the resolved path is a regular file whose
   extension is in the location's `cgi_extensions` map, or (fallback) a
   *prefix* of the remaining path is such a file — RFC 3875 `PATH_INFO`,
   e.g. `/cgi-bin/script.py/extra/thing` — hand off to
   `cgi_handler::start()` and leave `conn.state = CGI_RUNNING` instead of
   writing a response directly.
9. `POST` to a location with no `upload_store` configured → `403`;
   otherwise write `conn.body` to disk under `upload_store` (generating a
   filename if the URL didn't name one) and return `201 Created`.
10. Remaining case is `GET`: 404 if nothing's there; if it's a directory
    without a trailing slash, `301` to the slash-terminated form (another
    bug found and fixed — browsers/tools resolve relative links against
    the URL, so serving content at the slash-less path breaks them); else
    serve the configured `index` file, or an autoindex listing, or `403`
    if neither is available; otherwise serve the file directly with a
    guessed `Content-Type`.

### 6a. Non-CGI response → write (`harness_main.cpp`)

`handle_request()` filled `conn.write_buffer` via
`request_handler::writeResponse()`/`writeErrorResponse()`
(`RequestHandler.cpp`) — full status line, `Server`/`Connection`/
`Content-Type`/`Content-Length` headers, plus the body (dropped entirely
for `HEAD`, per RFC 7231 — a real bug found this session: sending it
anyway desyncs a keep-alive connection for any compliant client). The
harness then flips that fd's poll events to `POLLOUT` and, once ready,
`write()`s the buffer — possibly across several `poll()` iterations for a
large response. On completion: keep-alive resets the `Connection` and
immediately tries `pump()` again (in case a pipelined next request is
already sitting in the buffer); otherwise the connection is marked `DONE`
and closed.

### 6b. CGI response → the pipe dance (`srcs/CgiHandler.cpp`)

If `handle_request()` started a CGI, the harness's
`registerCgiFdsIfStarted()` adds `conn.cgi_stdin_fd`/`cgi_stdout_fd` (only
whichever aren't `-1`) into the *same* `poll_fds` array as every socket,
mapped back to the owning connection via `cgiOwner`. From here:

- `cgi_handler::start()` already forked, `dup2()`'d the pipes into the
  child's stdin/stdout, `chdir()`'d into the script's directory (so
  relative file access from the script works), built the full CGI/1.1
  environment via `buildEnv()`, and called `execve()`. The parent side
  never blocks — it returns immediately with both fds non-blocking.
- Every time `poll()` reports the stdin pipe writable,
  `onStdinWritable()` writes one chunk of `conn.body`; every time it
  reports the stdout pipe readable, `onStdoutReadable()` reads one chunk
  into `conn.cgi_out`. Both fds are marked `-1` (not `close()`'d
  immediately — a later close() from the wrong iteration could otherwise
  hand that fd number to something else mid-pass) once done.
- Once both are `-1` (`isDone()`), `finish()` tries a non-blocking
  `waitpid()` to tell an `execve()` failure (exit 127, no output → `502
  Bad Gateway`) apart from a script that legitimately produced nothing
  (→ `200`). **This exact spot had a real, reproducible race**: a child's
  pipes closing and it becoming *reapable* are two separate kernel events,
  so immediately after EOF `waitpid()` can still return 0. The fix:
  `finish()` reports "not ready" instead of guessing, and the harness
  retries on the next iteration (with a tightened 100ms poll timeout so
  that retry is near-instant, not a full second).
- If `conn.cgi_deadline` (10s from start) passes first, `abortTimeout()`
  `SIGKILL`s the child and writes `504 Gateway Timeout` — the rest of the
  server keeps serving other connections throughout.
- Either way, a still-unreaped pid goes into `pendingReap` and gets swept
  opportunistically (`reapIfExited()`, non-blocking) every iteration —
  `waitpid(-1, ...)` ("reap anything") is never called, since that could
  steal an unrelated CGI's exit status.

### 7. Cleanup

Closed/finished fds are removed from `poll_fds` and `cgiOwner`, and
`connections.erase()`'d, all at the end of the iteration — never mid-pass,
so no reference into the vectors being mutated goes stale underneath a
still-running loop.

---

## Part 2 — Every file

### `include/` — headers (the contracts)

**`connection.hpp`** — the shared struct from the guide, plus every field
added beyond it (each flagged individually in its own comment):
`server_conf` (so `handle_request()` has a config to route against),
`query_string`, `cgi_path_info` (RFC 3875), `status_code` (early-exit
signal from a parse failure), and the `CGI_RUNNING` bookkeeping fields
(`cgi_out`, `cgi_in_offset`, `cgi_deadline`). Also declares the `CGI_RUNNING`
`ConnState` — the guide's original 4-state enum assumed `handle_request()`
always finished synchronously, which can't hold once CGI has to share the
one `poll()` loop; this state is what makes that work. `try_parse_request`/
`handle_request` are declared here since they're the shared contract.

**`RequestParser.hpp`** — exposes only `request_parser::decodeChunked()`
beyond the contract function, since `RequestHandler`/tests have no other
reason to reach into the parser.

**`RequestHandler.hpp`** — exposes `writeResponse()`/`writeErrorResponse()`
so `CgiHandler.cpp` can produce error pages that look identical to every
other error path (same headers, same default-body fallback).

**`CgiHandler.hpp`** — the full CGI interface: `start()`,
`onStdinWritable()`/`onStdoutReadable()`, `isDone()`, `finish()`,
`abortTimeout()`, `reapIfExited()`. Every function's contract is spelled
out here in detail (when the harness is allowed to call it, what it
assumes about `poll()` state) since this is the trickiest integration
surface in the whole module.

**`Config.hpp`** — `Location` (one route's config) and `ServerConfig` (one
`server{}` block) structs, plus `Config::load()`. Explicitly noted as
*not* your teammate's-job-turned-yours: config parsing is officially
William's responsibility per the guide, but `RequestHandler`/`CgiHandler`
need something real to route against during development, so this exists
as a working stand-in whose *shape* (the struct fields) is the actual
contract the rest of the code relies on.

**`HttpStatus.hpp`** — `reasonPhrase()`, `defaultErrorBody()`,
`mimeType()`. Tiny, stateless lookup helpers.

**`StringUtils.hpp`** — `trim`, `toLower`/`toUpper`, `split`,
`startsWith`, `toString` (long/size_t → string, since C++98 has no
`std::to_string`), `toLong` (strict, no `strtol` trailing-garbage
tolerance), `urlDecode`, `headerKeyToEnv`. The small building blocks
everything else is written in terms of.

### `srcs/` — implementation

**`RequestParser.cpp`** — implements `try_parse_request()` and
`decodeChunked()`, plus three file-local helpers: `findHeaderEnd()`,
`collapseSlashes()`, `failParse()`. This is the file described step-by-step
in Part 1 §4 above. Every function has a `@brief`/`@param`/`@return`
doc-comment above its definition.

**`RequestHandler.cpp`** — implements `handle_request()`,
`writeResponse()`, `writeErrorResponse()`, and the routing helpers:
`joinPath()` (alias-style root+rel joining, matching the subject's own
`/kapouet` example), `hasDotDotSegment()`, `extensionOf()`/`basenameOf()`,
`resolveCgiScript()` (the `PATH_INFO`-aware CGI lookup), `serveFile()`,
`serveAutoindex()`. This is Part 1 §5 above, function by function.

**`CgiHandler.cpp`** — implements every function in `CgiHandler.hpp`, plus
file-local `splitDirFile()`, `parentPath()`, `buildEnv()` (the full CGI/1.1
environment — `REQUEST_METHOD`, `SCRIPT_NAME`/`PATH_INFO` split,
`SCRIPT_FILENAME`, `QUERY_STRING`, `CONTENT_LENGTH`/`CONTENT_TYPE`,
`SERVER_*`, `REQUEST_URI`, `REMOTE_ADDR`, `PATH`, and every request header
as `HTTP_*`), and `finishFromCgiOutput()` (parses the CGI's own
`Status:`/`Content-Type:` headers out of its stdout). This is Part 1 §6b.

**`Config.cpp`** — implements `Location::methodAllowed()`,
`ServerConfig::matchLocation()`, and the config-file parser itself:
`tokenize()` (whitespace/`{}`/`#`-comment tokenizer), `parseLocation()`,
`parseServer()`, `Config::load()`. Every directive the config format
supports (`listen`, `server_name`, `client_max_body_size`, `error_page`,
`root`, `index`, `autoindex`, `methods`, `return`, `upload_store`, `cgi`)
is a branch in `parseLocation()`/`parseServer()`; an unknown directive or
malformed value throws, never crashes.

**`HttpStatus.cpp`** — the actual switch/if-chains behind the three
lookups in the header. Nothing stateful.

**`StringUtils.cpp`** — the actual implementations behind the header.
Nothing here does heap allocation beyond ordinary `std::string`/
`std::vector` growth — consistent with the whole module having zero
`new`/`malloc`/`calloc` calls (confirmed by grep, and by a completely
clean valgrind run — see Part 3).

**`harness_main.cpp`** — the throwaway-turned-load-bearing main loop
covered in Part 1 §1–2, §6a, §7. Every helper (`handleShutdownSignal`,
`setNonBlocking`, `openListenSocket`, `resetForNextRequest`,
`registerCgiFdsIfStarted`, `pump`, `findAndSetEvents`) is documented in
place. The file's own top-of-file comment explains why it exists and what
it's not (a note that predates the point where it also became this
project's actual `NAME = webserv` binary — see Part 4).

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

**`tester.conf`** — config for the official 42 `tester` Go binary (see
Part 3). Routes match exactly what that tool's interactive prompts
demand: `/` GET-only, `/directory/` aliasing the `YoupiBanane` fixture
with a default-file fallback, `/youpi.bla` as an exact-path CGI location
(root pointed at the file itself, not its directory — the alias-style
joining in `joinPath()` means an exact-match location's `rel` is empty,
so `root` has to already *be* the target), and `/post_body`.

### `www/` — the site itself

**`index.html`** — the real homepage: hero section, feature cards, a
downloads section linking to `/listing/` and `/about.html`, and a live
contact form that POSTs to `/cgi-bin/contact.py` via `fetch()` and renders
the CGI's actual response — a genuine round trip, not a mock.

**`about.html`** — a second static page (GET-only, used to demonstrate
`405` from the diagnostics page), with real prose about the project.

**`dashboard.html`** — the diagnostics/test console, linked from every
page's nav and footer. Eleven numbered sections, each exercising one
mandatory feature or HTTP status code live via buttons that call
`fetch()` and print the real status/headers/body: static serving,
autoindex, redirects, the custom 404, method restrictions, CGI GET/POST,
`PATH_INFO`, the upload→GET→DELETE lifecycle, per-server body limits, and
a full status-code sweep (`302`, two flavors of `403`, `413`, `414`,
`431`, `502`, `504`, plus copy-paste `curl`/`nc` commands for the two
codes no browser can trigger on its own: `400` and `505`).

**`styles.css`** — the shared design system (light/dark aware) behind
`index.html`/`about.html`/`dashboard.html`.

**`cgi-bin/`** — every CGI script used somewhere in this documentation or
the dashboard:
- `hello.py` — trivial GET CGI, echoes `REQUEST_METHOD`/`QUERY_STRING`/
  `SCRIPT_NAME` back.
- `echo.py` — reflects `REQUEST_METHOD`/`CONTENT_LENGTH`/`CONTENT_TYPE`/
  the decoded body, used for the CGI-over-POST and chunked-body tests.
- `contact.py` — parses the homepage contact form's POST body and returns
  a real HTML confirmation fragment.
- `pathinfo.py` — echoes `SCRIPT_NAME`/`PATH_INFO`/`REQUEST_URI`, used to
  verify the `PATH_INFO` split.
- `slow.py` / `hang.py` — sleep 3s / forever, used to prove a slow or
  stuck CGI never blocks the rest of the server (and that `hang.py`
  specifically gets killed with `504` after its deadline).
- `anything.broken` — not a script at all; its content is irrelevant. Its
  `.broken` extension is mapped in the config to a nonexistent
  interpreter path, so `execve()` always fails and the `502` path is
  exercised on demand without breaking a real script.

**`errors/404.html`** — the custom error page configured via
`error_page 404`, proving the config-driven error-page override actually
works (vs. the built-in default body).

**`listing/`** (`a.txt`, `b.txt`) — sample files for the autoindex demo.

**`upload/`, `site2/uploads/`** — empty upload targets (a `.gitkeep`
placeholder each, since git doesn't track empty directories) — populated
and emptied again by the upload/DELETE tests at runtime.

**`site2/index.html`** — the distinct second site served on `:8081`,
explaining in its own text that it's a different `server{}` block, same
binary, same config file.

**`YoupiBanane/`** — fixture directory for the official 42 `tester`
binary (`youpi.bad_extension`, `youpi.bla`, `nop/`, `Yeah/` — exact
layout that tool's setup instructions demand). Not part of the demo
site; exists purely so `./tester http://127.0.0.1:8090/` (against
`conf/tester.conf`) can be re-run at any time.

### Root

**`Makefile`** — builds `webserv` (`NAME = webserv`, matching the
subject's required binary name) with `-Wall -Wextra -Werror -std=c++98`.
Standard `all`/`clean`/`fclean`/`re`, dependency-tracked (`-MMD -MP`) so
`make` after `make` with no changes does nothing — verified directly
(second run prints "Nothing to be done").

**`README.md`** — the project-facing summary: description, build/run
instructions, what's been tested and how, scope/boundaries (what's yours
vs. William's), resources, AI-usage disclosure. Shorter and more
outward-facing than this document.

**`.gitignore`** — excludes `webserv` (the built binary) and `objs/`
(build artifacts), and the *contents* of `www/upload/`/`www/site2/uploads/`
while keeping the directories themselves via `.gitkeep`.

---

## Part 3 — What was actually verified, and the bugs it found

This module wasn't just written — every claim above was checked against a
running server, and several real bugs were caught and fixed in the
process rather than left as theoretical descriptions:

- **HEAD responses leaking a body** — violated RFC 7231, confirmed via a
  real HTTP client (Go's `net/http`) logging "Unsolicited response
  received on idle HTTP channel" when it happened. Fixed in
  `writeResponse()`.
- **No trailing-slash redirect for directories** — `GET /directory`
  404'd instead of redirecting to `/directory/`, breaking the official
  tester's directory tests. Fixed in `ServerConfig::matchLocation()` +
  `handle_request()`.
- **A genuine CGI race** — `finish()` could report a false `200` instead
  of the correct `502` if called in the split-second window between a
  CGI's pipes closing and the process actually becoming reapable.
  Reproduced reliably (roughly 1 in 3 requests to the `.broken` CGI) and
  fixed by having `finish()` report "not ready" instead of guessing.
- **431 silently unenforced** — the header-size limit only applied while
  headers were still arriving in pieces; a full oversized header section
  arriving in one `read()` sailed straight through. Fixed, then
  discovered that fix broke `414` (an oversized URI always also exceeds
  the header-section cap) — fixed again by checking the request line's
  own length first.
- **`PATH_INFO` (RFC 3875)** wasn't implemented at all originally — added
  this session (`resolveCgiScript()` in `RequestHandler.cpp`).

Verification methods used, beyond ordinary `curl`:

- **A real browser**, driven headlessly via Chrome DevTools Protocol —
  actual `fetch()` calls, a real file picked via `DOM.setFileInputFiles`,
  a real form submission — not simulated requests.
- **The official 42 `tester` Go binary**, against `conf/tester.conf` +
  the `YoupiBanane` fixture — this is what caught the HEAD and
  trailing-slash bugs.
- **`siege -b`**, twice back-to-back on the same running process: **100%
  availability across 346,262 then 333,519 transactions**, RSS memory
  flat throughout (no leak trend), zero `CLOSE_WAIT` connections
  afterward (the real "hanging connection" indicator), well past the
  eval sheet's 99.5% bar.
- **`valgrind --leak-check=full --track-fds=yes`**, running the full
  functional suite (every status code, every method, uploads, CGI) then
  a clean `SIGINT` shutdown: **0 bytes in use at exit, 2,218 allocs /
  2,218 frees, "All heap blocks were freed — no leaks are possible", 0
  errors, only the 3 inherited stdio fds open at exit.**

---

## Part 4 — Where things stand

- Code lives on the `kai` branch of `github.com/Kai-Doh/Webserv`; `main`
  is the shared integration point; `william` is the Core Server branch,
  currently identical to `main` until he starts his half.
- Files unrelated to the actual deliverable (subject/eval PDFs, the
  onboarding guide, the official tester binaries) live outside the repo
  in a sibling `Webserv-extras/` directory, not in git history.
- Every non-trivial function across `srcs/` has a `@brief`/`@param`/
  `@return` doc-comment (hover-able in an editor with a working C++ LSP).
- Outstanding, not yet done: the README's mandatory first line (`*This
  project has been created as part of the 42 curriculum by <login>*`) —
  needs your actual 42 login, which isn't something to guess; and the
  eventual merge of this code with William's real Core Server under the
  `webserv` binary this Makefile already builds toward.
