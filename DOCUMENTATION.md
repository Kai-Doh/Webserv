# Webserv — HTTP + CGI Module: A Beginner's Guide

## What this document is

This documents **Kai's half** of the two-person 42 `Webserv` project — the
part of the server that speaks HTTP: reading a request off the wire,
figuring out what it's asking for, and either answering it directly or
handing it off to a CGI script. William's half (`srcs/core/`, `srcs/net/`)
is the plumbing around it — opening sockets, accepting connections, and
running the one `poll()` loop that makes the whole thing non-blocking. That
part is only described here where it's needed for context, never in depth.

This version of the document is written for someone who has never built a
web server before and wants to actually understand *why* the code looks the
way it does, not just *what* it does. Every mechanism is explained from
first principles, with a real-world comparison next to it where one helps,
and every claim is backed by the actual file and function that implements
it, so you can always go read the real thing right after.

If you already know HTTP servers inside and out, this document will feel
slow. That's on purpose.

---

# Chapter 0 — The big picture: what is a web server, actually?

Strip away every buzzword and a web server is a very boring machine that
does one thing, over and over, forever:

1. Someone connects and sends it a carefully-formatted piece of text.
2. It reads that text, figures out what's being asked for.
3. It sends back another carefully-formatted piece of text as a reply.
4. Repeat, for potentially thousands of people, all at once.

That's it. There's no magic. The "carefully-formatted piece of text" in
step 1 and step 3 is HTTP — a plain-text protocol, which means if you
squint, an HTTP request looks almost like an email:

```
GET /index.html HTTP/1.1
Host: localhost
User-Agent: curl/8.4.0
Accept: */*

```

That's a **real, complete, valid HTTP request** — the kind a browser sends
every time you load a page. Four lines: a "request line" saying what's
wanted, a few headers giving context, and a blank line marking the end.
Nothing here is encrypted, compressed, or binary. You could type this by
hand into a raw TCP connection (with `telnet` or `nc`) and a real server
would answer you.

**Analogy: the post office.** Think of an HTTP request as a letter you drop
in a mailbox. The first line is the address on the envelope ("GET
/index.html" — deliver *this specific thing*, using *this specific
action*). The headers are the extra notes stapled to the envelope ("also,
here's who I am, here's what languages I speak, here's a cookie you gave me
last time"). The blank line is where the letter itself starts, if there is
one. The server is the post office: it reads the envelope, decides which
department handles it, and eventually mails a reply letter back — which
looks exactly the same, just starting with a status line instead of a
request line:

```
HTTP/1.1 200 OK
Content-Type: text/html
Content-Length: 137

<html>...</html>
```

Everything this document covers is really just: **how do we read that
envelope correctly, and how do we decide what to write back?**

---

# Chapter 1 — Why you can't just `read()` a request and be done

If HTTP requests are just text, why is `srcs/http/RequestParser.cpp` nearly
350 lines long? Why not just call `read()` once and parse the string?

Because of two inconvenient facts about real networks:

**Fact 1 — TCP has no idea where one request ends and the next begins.**
A TCP connection is just a stream of bytes, like a garden hose. When a
browser sends a request, those bytes might arrive at the server in one
single `read()`, or in five separate `read()` calls, each with a random
number of bytes, with pauses in between (a slow phone connection, a big
file upload, whatever). The server has to keep calling `read()`, keep
appending whatever comes in, and keep *asking itself* — "do I have a
complete request yet, or should I wait for more?" — until the answer is
yes.

**Analogy: reading a letter through a mail slot, one page at a time.**
Imagine someone is feeding a multi-page letter through your door's mail
slot, one page every few seconds, in no guaranteed rhythm. You can't just
grab the first page and start replying — you have to keep watching the
slot, collecting pages, and checking after each one: "does what I have so
far actually make a complete letter?" Only once you see the letter's own
"the end" marker do you know it's safe to act.

**Fact 2 — the server is doing this for hundreds of people at once, and
can't afford to wait around for any single one of them.** This is where
William's `poll()` loop comes in (briefly, for context — it's not this
module's code): instead of one dedicated employee per visitor who blocks
everything else while waiting for that one visitor's next page, there's
*one* employee who checks a big board of "who has something new to hand me
right now" (`poll()`), handles whoever's ready, and immediately moves on.
Nobody's `read()` call is ever allowed to sit and block waiting for bytes
that haven't arrived yet — that would freeze the one employee for
everyone else.

The consequence for this module's code: **`try_parse_request()` in
`srcs/http/RequestParser.cpp` is not a function that runs once per
request.** It's a function that gets called *again every single time a new
chunk of bytes arrives* for a connection, and its whole design revolves
around answering one question fast: "is a complete request sitting in
`conn.read_buffer` yet, or not?"

```cpp
bool try_parse_request(Connection& conn);
```

Return `false` → "not yet, call me again once more bytes show up."
Return `true` → "done — either I successfully parsed a request, *or* I
found something so wrong I'm reporting an error status instead. Either
way, stop calling me for this data and go handle what I found."

That single design decision — *never block, always be interruptible,
always be resumable* — is the thread that runs through everything in this
module.

---

# Chapter 2 — Reading a request, one piece at a time

Let's walk through `try_parse_request()` exactly as the code does it,
explaining every decision.

## 2.1 — First: do we even have the headers yet?

A request has two halves: the **headers** (request line + header lines) and
the **body** (whatever comes after, like a POST's form data). Headers
always come first and are always separated from the body by a blank line —
`\r\n\r\n` per the HTTP spec, though the parser also accepts a bare `\n\n`
so you can test it by hand with `telnet` without fighting your terminal's
line endings.

```cpp
size_t crlf = buf.find("\r\n\r\n");
size_t lf = buf.find("\n\n");
```

Until that separator shows up somewhere in `conn.read_buffer`, there is
*nothing useful to do yet* — we don't even know the method or the URL. So
the very first check is: "have I found the blank line? If not, and the
buffer isn't absurdly large yet, just wait for more bytes."

**Why "absurdly large" matters**: an attacker (or a badly-behaved client)
could just keep sending header bytes forever and never send the blank
line, trying to make the server hold an ever-growing buffer in memory
until it runs out. So there's a hard cap —`MAX_HEADER_SECTION = 8192`
bytes — and if the buffer blows past that with still no blank line in
sight, the server gives up and answers **`431 Request Header Fields Too
Large`** instead of waiting forever.

## 2.2 — The request line: method, target, version

Once we have the full header block, the very first line is split into
exactly three pieces on spaces:

```
GET /index.html HTTP/1.1
└┬┘ └────┬─────┘ └──┬───┘
method  target    version
```

```cpp
std::string requestLine = su::trim(lines[0]);
std::vector<std::string> parts = su::split(requestLine, ' ');
if (parts.size() != 3) {
    failParse(conn, 400, bodyStart, true);
    return true;
}
std::string method = parts[0];
std::string target = parts[1];
std::string version = parts[2];
```

`su::split(requestLine, ' ')` on anything that isn't *exactly* three
space-separated pieces gives a vector whose size isn't 3 — that one check
covers "too few pieces," "too many pieces," and "empty line" all at once.

- **Not exactly three pieces** (extra spaces, missing pieces) → `400 Bad
  Request`. A request line is a fixed, rigid format; there's no reasonable
  way to guess what was meant if it doesn't match.
- **Target doesn't start with `/`** → `400`. This server only understands
  "origin-form" targets (`/path`), not the full-URL form some proxies use
  (`http://example.com/path`) — the subject explicitly says only a subset
  of the RFC needs implementing, and full proxy-style requests are outside
  that subset.
- **Version isn't `HTTP/1.0` or `HTTP/1.1`** → `400` if it doesn't even
  start with `HTTP/`, or **`505 HTTP Version Not Supported`** if it does
  but names a version we don't speak (`HTTP/2.0`, `HTTP/0.9`, garbage like
  `HTTP/9.9`).
- **The target itself is too long** (over 8000 characters) → **`414 URI
  Too Long`**.

That last one has a subtlety worth calling out: the check for an
over-length *target* has to happen **before** the general "header section
is too large" check, not after — otherwise an absurdly long URL would
always trip the generic 8192-byte cap first and you'd never actually see a
414, only ever 431. Order of checks matters here, and it's a good example
of a bug that's invisible until you specifically go looking for it (which
is exactly how it *was* found, later — see Appendix B).

**One more check on the target, easy to miss but important: no raw NUL
bytes allowed, once decoded.** The target is percent-decoded (`%2E` →
`.`, `%2F` → `/`, and so on — the same decoding a browser does before
submitting a form) into what becomes `conn.path`, and the decoded result
is checked for a literal `\0` byte before it's trusted for anything:

```cpp
std::string decodedPath = collapseSlashes(su::urlDecode(rawPath));
if (decodedPath.find('\0') != std::string::npos) {
    failParse(conn, 400, bodyStart, true);
    return true;
}
```

**Why this matters, concretely:** every filesystem call downstream
(`stat()`, `std::ifstream`, ...) takes a plain C string, which ends at the
*first* `\0` it finds — but a C++ `std::string` doesn't; it just carries
that byte along as ordinary data. Without this check, a request for
`/cgi-bin/script.py%00.txt` would decode to a `std::string` containing
`script.py`, a NUL byte, then `.txt`. The code that decides "is this a CGI
script?" (Chapter 3) looks at the *whole* string and sees the extension
`.txt` — not a configured CGI extension, so: not CGI. The filesystem call
that actually opens the file, right after, only sees up to the NUL and
opens the real `script.py`. Put those two together and the script's raw
*source code* gets served back as plain text instead of being executed —
a real, well-known vulnerability class (the same NUL-byte trick that hit
early PHP installations). Rejecting the NUL outright, before either check
gets a chance to disagree with the other, closes it for good.

## 2.3 — Headers: turning lines into a lookup table

Every line after the request line gets split on its first `:` into a key
and a value, lower-cased on the key (`Content-Length` and `content-length`
must be treated identically — HTTP header names are case-insensitive), and
stored in a `std::map<std::string, std::string>`.

```cpp
size_t colon = line.find(':');
if (colon == std::string::npos) {
    failParse(conn, 400, bodyStart, true);   // "Malformed-Header" with no colon at all
    return true;
}
```

A line with no colon at all isn't a header — it's garbage — so that's a
`400` too.

**Two specific header rules worth understanding, not just memorizing:**

- **A repeated `Content-Length` with two different values is rejected
  (`400`).** Why does this matter so much it gets its own check? Because
  `Content-Length` tells the server exactly how many bytes of body to
  expect. If a request smuggles in *two different* values for it, and two
  different pieces of software along the way (say, a proxy and this
  server) each trust a *different* one of the two, they can end up
  disagreeing about where the request actually ends — which is exactly how
  a class of attack called **request smuggling** works: an attacker hides
  a second, forged request inside what looks like the tail end of a
  legitimate one, betting that the two systems in the chain will each read
  the boundary differently. Rejecting outright the moment we see
  conflicting values closes that door before it opens.
- **`HTTP/1.1` with no `Host` header → `400`.** `HTTP/1.0` never required
  one, but `HTTP/1.1` made it mandatory (RFC 7230 §5.4) specifically
  because a single server can host multiple different sites — `Host` is
  how the request says *which one it means*. A 1.1 request without it is
  simply malformed by the spec, not a matter of leniency.

Two more hardening checks live right here too, both added after running an
independent third-party HTTP conformance test suite against the server and
seeing exactly what it could get away with:

- **More than 100 header lines → `431`.** The byte cap from §2.1 stops a
  client from sending one giant header section, but says nothing about
  sending *thousands of tiny ones* instead — so there's a separate count
  check, right alongside the byte-size one:
  ```cpp
  if (lines.size() > MAX_HEADER_COUNT + 1) {
      failParse(conn, 431, bodyStart, true);
      return true;
  }
  ```
- **`Content-Length` *and* `Transfer-Encoding: chunked`, both present on
  the same request → `400`, unconditionally**, instead of picking one of
  the two and ignoring the other:
  ```cpp
  if (chunkedTE && headers.find("content-length") != headers.end()) {
      // RFC 7230 3.3.3: a message with both headers is a smuggling
      // risk and must be rejected outright, not resolved by picking
      // one of the two framings.
      failParse(conn, 400, bodyStart, true);
      return true;
  }
  ```
  This is the other half of the request-smuggling defense described above
  for a *repeated* `Content-Length` — here it's two *different* framing
  mechanisms disagreeing about where the body ends, which is exactly the
  ambiguity a smuggled request hides inside. The fix follows the same
  principle both times: when two trusted signals disagree, don't guess
  which one to believe — refuse the request outright.

A third batch of hardening checks lives here too, added after deliberately
trying to construct requests that would *look* almost fine and see what
survived:

- **A second `Host` header → `400`, whether or not the two values agree.**
  RFC 7230 §5.4 doesn't just forbid *conflicting* `Host` headers, it
  forbids *more than one*, full stop — a request with two identical `Host`
  headers is still ambiguous about which one a downstream system is
  supposed to trust, so this checks for the second occurrence outright
  rather than waiting to see if the values happen to differ:
  ```cpp
  if (key == "host" && headers.find("host") != headers.end()) {
      failParse(conn, 400, bodyStart, true);
      return true;
  }
  ```
- **Whitespace between a header name and its colon → `400`.** `X-Foo :
  bar` (note the space before `:`) is, per RFC 7230 §3.2.4, something a
  server **must** reject outright — not because it's ambiguous *here*, but
  because different implementations disagree on whether that space is
  part of the field name or not. If this server quietly stripped it while
  something in front of it (a reverse proxy) didn't, the two could
  disagree about whether a header even matches a given name — the same
  smuggling-shaped problem as the two checks above, just one character
  wide:
  ```cpp
  if (colon > 0 && (line[colon - 1] == ' ' || line[colon - 1] == '\t')) {
      failParse(conn, 400, bodyStart, true);
      return true;
  }
  ```
- **Two headers that collide once mapped to a CGI environment variable →
  `400`.** `buildEnv()` (Chapter 6) turns every header into an `HTTP_`
  env var per RFC 3875, converting `-` to `_` along the way — which means
  a client-sent `X-Foo` and a client-sent `X_Foo` both become the exact
  same `HTTP_X_FOO`. Left unchecked, both would sit in `conn.headers`
  under their own distinct keys, both would get pushed into the CGI
  child's `envp`, and *which value a script's `getenv()` actually sees*
  would come down to `std::map`'s iteration order — an implementation
  accident, not something either header's sender could predict or rely
  on. Caught the same way as a duplicate `Content-Length`, just keyed on
  the post-normalization name instead of the literal one:
  ```cpp
  std::string envName = key;
  for (size_t k = 0; k < envName.size(); ++k) {
      if (envName[k] == '-')
          envName[k] = '_';
  }
  std::map<std::string, std::string>::const_iterator envIt = envNames.find(envName);
  if (envIt != envNames.end() && envIt->second != key) {
      failParse(conn, 400, bodyStart, true);
      return true;
  }
  envNames[envName] = key;
  ```

## 2.4 — Caching what we've learned: `headers_ready`

Here's where the "called again and again" nature from Chapter 1 really
bites. Once headers are successfully parsed, the code sets:

```cpp
conn.headers_ready = true;
```

and every *later* call to `try_parse_request()` for this same request
**skips the entire header-parsing block above** and jumps straight to body
handling, reading `method`/`path`/`headers`/`body_start` back out of
`conn` instead of re-deriving them.

This sounds like a micro-optimization. It is not. **This exact caching is
the fix for a real, serious performance bug** that was found by stress
testing this server with the official 42 tester: 20 simultaneous clients
each uploading a 100MB body to a CGI script pegged a CPU core at 100% for
over nine minutes, with the server barely making progress. The root cause:
without the cache, the *separator search* from §2.1
(`buf.find("\r\n\r\n")`) was being run again from scratch on **every single
`read()`**, over the **entire buffer**, including all the megabytes of
body that had already arrived. Each new chunk of body meant re-scanning
everything that came before it too — classic O(n²) behavior, invisible on
a small test request, catastrophic on a 100MB one.

**Analogy:** imagine re-reading an entire novel from page one, every single
time someone hands you one more page of it, just to check whether you've
reached "The End" yet. Fine for a five-page pamphlet. Unusable for a
thousand-page book. The fix — remembering *where you already checked up
to* — is `conn.body_start`: computed exactly once, then just read back out
on every subsequent call.

## 2.5 — Reading the body: two very different philosophies

Once headers are settled, there are two ways a client can tell the server
how much body to expect, and they represent two genuinely different
philosophies:

**Content-Length: "here's the total size, up front."**
```
Content-Length: 13

Hello, world!
```
The client counts its own bytes ahead of time and declares the total. The
server just waits until it has that many bytes past the blank line, then
it's done. Simple, but it means the client has to know the full size
*before* it starts sending — awkward if the body is being generated on the
fly (say, streaming a file that's still being compressed).

**Transfer-Encoding: chunked — "here's a bit, then another bit, then I'll
tell you when I'm out."**
```
5\r\n
Hello\r\n
7\r\n
, world!\r\n
0\r\n
\r\n
```
**Analogy: shipping a package in labeled boxes instead of declaring the
total weight up front.** Each "chunk" starts with its own size (in
hexadecimal!) on its own line, followed by exactly that many bytes of data,
followed by `\r\n`. When a chunk announces size `0`, that's the sender
saying "that was the last box — nothing more is coming." This lets the
client start sending before it knows the final total size at all.

`request_parser::decodeChunked()` walks this format one chunk at a time,
and — just like `body_start` above — it **resumes from where it left off**
via `conn.chunked_scan_pos` instead of re-decoding every previously-seen
chunk on each call. This was the *other* half of that same O(n²) bug: the
chunked decoder was originally re-copying the entire body-so-far into
`conn.body` on every partial read too. Same disease, same cure: cache the
position, never redo already-finished work.

Each chunk-size line is hex text, parsed with `std::strtol()` — and
`strtol()` has a quiet failure mode worth guarding against explicitly: on
a value too large to fit in a `long` (a client sending, say, `ffffffffffffffff`
as a chunk size), it doesn't fail loudly — it clamps to `LONG_MAX` and
sets `errno = ERANGE`, leaving the return value looking like a perfectly
ordinary large-but-valid number. Without checking `errno`, that "clamped"
size would be accepted as genuine, and the parser would then sit waiting
for a chunk body of a preposterous, functionally-unreachable size — not
an infinite hang (the raw-buffer size guard just below and the 60-second
idle timeout both eventually catch it), but a real delay for something
that should just be `400` immediately:
```cpp
char* endptr = 0;
errno = 0;
long chunkSizeLong = std::strtol(sizeLine.c_str(), &endptr, 16);
if (endptr == sizeLine.c_str() || *endptr != '\0' || chunkSizeLong < 0 || errno == ERANGE) {
    malformed = true;
    return false;
}
```
This is the one place in the whole codebase that checks `errno` after
anything — and it's checked after a `strtol()` call on an already-fully-
buffered string, never after a `read()`/`write()`/`recv()`/`send()`, which
the subject explicitly forbids using `errno` to drive behavior around.

Whichever method is used, the result is checked against
`client_max_body_size` (the server's configured limit, or a more specific
one set on the matched location) — too big → **`413 Payload Too Large`**.
No `Content-Length` and no chunked encoding at all just means a body of
zero bytes, which is perfectly normal (most `GET` requests have no body)
and not an error.

## 2.6 — Never throwing: `failParse()`

Every single rejection path above — bad request line, bad version, huge
header, huge body, whatever — funnels through one function:

```cpp
void failParse(Connection& conn, int code, size_t consumedBytes, bool closeConn) {
    conn.status_code = code;
    conn.keep_alive = !closeConn;
    if (consumedBytes >= conn.read_buffer.size())
        conn.read_buffer.clear();
    else
        conn.read_buffer.erase(0, consumedBytes);
}
```

It sets `conn.status_code`, trims the bad bytes out of the buffer, and
optionally marks the connection for closing afterward — and that's it. It
never throws an exception, never calls `abort()`, never lets a malformed
request take the whole process down. **A broken request from one client is
just data to reject, not a crisis.** This one small design rule is a big
part of *why* this server survives being thrown genuinely garbage input —
covered concretely in Chapter 10.

---

# Chapter 3 — Routing: turning a URL into "what do I do now?"

Once `try_parse_request()` says "done," control passes to
`handle_request()` in `srcs/http/RequestHandler.cpp` — the function that
decides what the response actually *is*.

## 3.1 — Server blocks and location blocks

The config file (Chapter 8 covers its full syntax) describes one or more
`server { ... }` blocks, each listening on its own `host:port`, and inside
each one, a set of `location { ... }` blocks — one per URL prefix the
server should know how to handle.

**Analogy: a company directory.** A `server{}` block is like one building
(one address). Each `location{}` inside it is like a department sign in
the lobby: "`/billing` → third floor," "`/support` → second floor," "`/` →
front desk (handles anything nobody else claimed)." When a request comes
in for `/support/tickets/42`, the routing logic looks for the *most
specific* sign that matches — `/support` beats the generic `/` — exactly
like you'd follow the most specific department sign rather than defaulting
to the front desk. This is called **longest-prefix matching**, and it's
implemented in `ServerConfig::matchLocation()`:

```cpp
if (matches && p.size() >= bestLen) {
    bestLen = p.size();
    best = &locations[i];
}
```

Every location whose prefix matches the request path is a candidate;
whichever candidate's prefix is *longest* wins.

## 3.2 — What a location can say about a route

Each `location{}` block can carry any combination of these settings (all
parsed by `Config::load()` in `srcs/config/Config.cpp`):

- **`root <dir>`** — the filesystem directory this location's files live
  under. Requests are mapped onto it *alias-style*: whatever's left of the
  URL after the location's own prefix gets appended straight onto `root`.
  The subject gives the canonical example: if `/kapouet` is rooted at
  `/tmp/www`, then `/kapouet/pouic/toto/pouet` resolves to
  `/tmp/www/pouic/toto/pouet` — the `/kapouet` part is *replaced* by the
  root, not nested inside it. `joinPath()` implements exactly this.
- **`index <file>`** — if the request resolves to a *directory*, serve this
  file from inside it instead of a listing (like nginx's/Apache's
  `index.html` default).
- **`autoindex on|off`** — if there's no matching `index` file (or none was
  configured), should the server generate an HTML directory listing
  instead of just giving up?
- **`methods GET POST ...`** — the whitelist of HTTP methods this route
  will accept. Anything else gets rejected (details in §3.4).
- **`return <code> <target>`** — an unconditional redirect: the server
  never even looks at the filesystem for this location, it just answers
  immediately with the given status code and a `Location:` header pointing
  at `target`.
- **`upload_store <dir>`** — turns this location into one clients can
  `POST` files to, and says *where on disk* uploaded bodies get written.
- **`cgi <ext> <interpreter>`** — maps a file extension (`.py`, `.sh`, ...)
  to the program that should execute matching files (Chapter 6 covers this
  whole mechanism).
- **`client_max_body_size <bytes>`** — a per-location override of the
  server-wide body size limit, for routes that need a tighter (or looser)
  cap than the rest of the site.
- **`error_page <code> <path>`** — a custom error page for this location
  specifically, checked *before* falling back to the server-wide one (see
  §5.2).

## 3.3 — The decision tree, in order

Here's the exact sequence `handle_request()` walks through for every
request, and the reasoning behind each step:

1. **Was there already a parse error?** (`conn.status_code != 0`, set back
   in Chapter 2) — if so, skip everything below and just write that error.
   No point routing a request we already know is broken.
2. **Find the matching location.** No match at all → **`404 Not Found`**
   immediately — there's genuinely nothing configured to answer this URL.
3. **Is this a redirect location?** (`return` was set) — answer with the
   configured code and stop. Nothing filesystem-related happens for a pure
   redirect.
4. **Is the method allowed here?** — covered in detail next (§3.4).
5. **Reject directory traversal.** Before touching the filesystem *at all*,
   the remainder of the path is checked for a literal `..` segment. This
   is the guard against a request like `/files/../../../etc/passwd` trying
   to walk *outside* the directory it was given access to.
   **Analogy: a visitor badge that only opens doors on one floor.** Even if
   someone tries to walk into the stairwell and go up or down, the badge
   just doesn't work there. The check happens *before* any `stat()` or
   `open()` call, so a traversal attempt never even reaches the real
   filesystem.
6. **Branch on the method** — `DELETE`, CGI, `POST`, or fall through to
   `GET`-style static serving. Each is its own chapter/section below.

## 3.4 — Method checking, and a subtlety worth understanding: 405 vs. 501

If the resolved method isn't in the location's allowed list, something has
to be rejected — but *which* status code is correct depends on a
distinction that's easy to miss:

- **`405 Method Not Allowed`** means: "I recognize this method just fine,
  it's simply not permitted *here*." A `DELETE` on a read-only route.
- **`501 Not Implemented`** means: "I don't know what this method even
  *is*, anywhere on this server." A request using `PROPFIND`, or someone
  typing garbage like `BLARGH / HTTP/1.1`.

The code keeps a small whitelist of methods it actually recognizes as real
HTTP methods (`GET`, `POST`, `DELETE`, `HEAD`, `PUT`, `OPTIONS`, `PATCH`) —
`isKnownMethod()` in `RequestHandler.cpp`. Anything **not** on that list
gets `501` immediately, regardless of what any location's `methods`
directive says. Anything that **is** on the list, but just isn't in *this*
location's allowed set, gets the more specific `405`, along with an
`Allow:` header listing what actually *is* permitted — which is itself a
spec requirement (RFC 7231 §6.5.5), not just a nicety.

A second, narrower list — `isUnsupportedMethod()` — covers `PUT`,
`OPTIONS`, and `PATCH` specifically: real HTTP methods (so still `405`,
never `501`), but ones the subject never asks for and that this server has
no actual handling for. Without this guard, a location config that
mistakenly listed one of them as allowed would fall straight through
`handle_request()`'s method branch into the same logic as `GET` — serving
the target file back and silently ignoring the request body, which is not
`PUT`/`PATCH` semantics at all, and `OPTIONS` has no real "describe this
resource" response either. `isUnsupportedMethod()` is checked *before*
`loc->methodAllowed()`, so these three always 405 regardless of what any
config says:

```cpp
bool isKnownMethod(const std::string& method) {
    return method == "GET" || method == "POST" || method == "DELETE" ||
           method == "HEAD" || method == "PUT" || method == "OPTIONS" ||
           method == "PATCH";
}

bool isUnsupportedMethod(const std::string& method) {
    return method == "PUT" || method == "OPTIONS" || method == "PATCH";
}
```

and the actual branch in `handle_request()` that uses them:

```cpp
if (isUnsupportedMethod(conn.method) || !loc->methodAllowed(conn.method)) {
    if (!isKnownMethod(conn.method)) {
        conn.status_code = 501;
        request_handler::writeErrorResponse(conn, 501, loc);
        return;
    }
    std::string allow;
    for (size_t i = 0; i < loc->methods.size(); ++i) {
        if (isUnsupportedMethod(loc->methods[i]))
            continue;
        if (!allow.empty())
            allow += ", ";
        allow += loc->methods[i];
    }
    conn.status_code = 405;
    request_handler::writeResponse(conn, 405, "text/html", http_status::defaultErrorBody(405),
                                    "Allow: " + allow + "\r\n");
    return;
}
```

The `Allow:` header is filtered through the same `isUnsupportedMethod()`
check, so even a config that lists `PUT` never advertises it as available
— the header stays truthful about what the server will actually do.

Notice the order: `isKnownMethod()` is only even consulted *after*
`loc->methodAllowed()` already said no. A method the location genuinely
does allow never has to prove it's "known" first — the whitelist exists
purely to pick the right rejection code, not to gate anything on the
success path.

---

# Chapter 4 — Serving static files, directories, uploads, and deletes

Once a request has passed routing and method checks, `handle_request()`
does one `stat()` on the resolved filesystem path and branches on the
method:

## 4.1 — `DELETE`

Straightforward, but every edge case is checked in order: file doesn't
exist → `404`; it's a directory (deleting a whole directory isn't
supported) → `403`; `std::remove()` fails for some other reason
(permissions, etc.) → `403`; otherwise → **`204 No Content`** — the correct
response for "the thing you asked me to do is done, and there's nothing
useful to send back."

```cpp
if (conn.method == "DELETE") {
    if (!exists) {
        request_handler::writeErrorResponse(conn, 404, loc);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        request_handler::writeErrorResponse(conn, 403, loc);
        return;
    }
    if (std::remove(fsPath.c_str()) != 0) {
        request_handler::writeErrorResponse(conn, 403, loc);
        return;
    }
    conn.status_code = 204;
    request_handler::writeResponse(conn, 204, "", "");
    return;
}
```

Every branch here `return`s immediately — there's no fall-through, so
there's never a chance of accidentally reaching the `GET`-handling code
below with a `DELETE` still in flight.

## 4.2 — `POST` (uploads)

If the matched location has no `upload_store` configured, the server
still accepts the request with a plain `200` — it's legitimate for a route
to accept a `POST` it doesn't need to persist anywhere (a contact form
that just emails itself, for instance, in spirit — this project doesn't
send email, but the *shape* of "accept data, don't necessarily store a
file" is valid). If `upload_store` *is* configured, the body is written to
disk under that directory (inventing a filename from the current time and
the connection's own fd if the URL didn't supply one) and the server
answers **`201 Created`** with a `Location:` header pointing at the new
resource — the standard way to tell a client "here's where what you just
sent now lives."

```cpp
std::string filename = basenameOf(rel);
if (filename.empty()) {
    std::ostringstream gen;
    gen << "upload_" << static_cast<long>(std::time(0)) << "_" << conn.fd;
    filename = gen.str();
}
std::string dest = joinPath(loc->upload_store, filename);
std::ofstream out(dest.c_str(), std::ios::binary | std::ios::trunc);
```

`basenameOf(rel)` is "everything after the last `/`" — so `POST
/upload/photo.jpg` names the file `photo.jpg`, while a bare `POST /upload/`
(nothing after the trailing slash) falls into the `filename.empty()`
branch and gets a generated name instead, combining the current time with
`conn.fd` so two uploads landing in the same second on different
connections still can't collide.

## 4.3 — `GET` (and the fall-through case for everything else)

- **Nothing exists at the resolved path** → `404`.
- **It's a directory, and the URL didn't end in `/`** → **`301 Moved
  Permanently`** redirecting to the same URL *with* a trailing slash.
  **Why this matters, concretely:** a browser resolves *relative* links
  (`<a href="style.css">`) against the current URL. If the server served
  the directory's content directly at `/blog` (no slash), a relative link
  to `style.css` would resolve to `/style.css` — wrong. Redirecting to
  `/blog/` first means every relative link inside the page resolves
  correctly afterward. This is the same behavior nginx and Apache both
  have, for the same reason.
  ```cpp
  if (conn.path.empty() || conn.path[conn.path.size() - 1] != '/') {
      std::string target = conn.path + "/";
      if (!conn.query_string.empty())
          target += "?" + conn.query_string;
      conn.status_code = 301;
      request_handler::writeResponse(conn, 301, "text/html", "",
                                      "Location: " + target + "\r\n");
      return;
  }
  ```
  Note the query string is preserved across the redirect (`/blog?page=2`
  becomes `/blog/?page=2`, not a bare `/blog/`) — losing it would silently
  drop information the client explicitly asked to keep.
- **It's a directory, and the URL *does* end in `/`** — look for the
  location's configured `index` file inside it and serve that if it
  exists; otherwise, if `autoindex` is on, generate a directory listing
  page; otherwise, `404` (deliberately `404`, not `403` — from the
  client's point of view, there's simply nothing to see here, which is a
  more honest answer than "you're not allowed").
- **It's a regular file** → read the whole thing and serve it, with a
  guessed `Content-Type` (`HttpStatus::mimeType()` — a simple
  extension-to-MIME-type lookup table) and a `Last-Modified` header built
  from the file's own `stat()` modification time (this is meaningful for a
  static file with one clear "last changed" moment; CGI output and
  generated directory listings deliberately don't get one, since neither
  has a single well-defined modification time — that's a real editorial
  choice worth naming so it's not mistaken for an oversight).

---

# Chapter 5 — Writing the response, and how errors get their pages

## 5.1 — Building the raw bytes

`writeResponse()` assembles the actual text that goes back over the wire:

```
HTTP/1.1 200 OK
Date: Tue, 22 Sep 2026 15:54:23 GMT
Server: webserv/1.0
Connection: keep-alive
Content-Type: text/html
Content-Length: 5394
Last-Modified: Tue, 22 Sep 2026 11:40:17 GMT

<!DOCTYPE html>...
```

...and here's the function that actually writes those lines, in order:

```cpp
void writeResponse(Connection& conn, int code, const std::string& contentType,
                    const std::string& body, const std::string& extraHeaders) {
    std::ostringstream out;
    std::string version = conn.http_version.empty() ? "HTTP/1.1" : conn.http_version;
    out << version << " " << code << " " << http_status::reasonPhrase(code) << "\r\n";
    out << "Date: " << httpDate() << "\r\n";
    out << "Server: webserv/1.0\r\n";
    out << "Connection: " << (conn.keep_alive ? "keep-alive" : "close") << "\r\n";
    if (!contentType.empty())
        out << "Content-Type: " << contentType << "\r\n";
    out << "Content-Length: " << body.size() << "\r\n";
    out << extraHeaders;
    out << "\r\n";
    conn.write_buffer = out.str();
    if (conn.method != "HEAD")
        conn.write_buffer += body;
    conn.bytes_written = 0;
}
```

`extraHeaders` is how every one-off header gets bolted on without this
function needing to know about every possible case — a redirect's
`Location:`, an upload's `Location:`, a `405`'s `Allow:`, a static file's
`Last-Modified:` are all just pre-formatted `"Name: value\r\n"` strings
passed in from whichever caller needs them (Chapters 3-4).

Two details worth calling out because they're easy to get subtly wrong:

- **`HEAD` requests get every header a `GET` would, but never the body** —
  the whole *point* of `HEAD` (RFC 7231) is "tell me what you'd send,
  without actually sending it," so `Content-Length` still reflects the
  real size the body *would* have been, it's just never appended to
  `conn.write_buffer`.
- **`Connection: keep-alive` vs. `close`** decides whether, once this
  response finishes sending, the socket stays open waiting for another
  request on the same connection (much cheaper than opening a new TCP
  connection per request) or gets closed outright. This is decided back in
  Chapter 2 from the HTTP version and any `Connection` header the client
  sent.

## 5.2 — Error pages: a chain of fallbacks

`writeErrorResponse(conn, code, loc)` is what every rejection path in this
whole module eventually calls. It checks, in order:

1. **Does the *matched location* have a custom `error_page` for this exact
   code?** If so, and the file's readable, serve it.
2. **Does the *server block* have one?** Same check, one level less
   specific.
3. **Otherwise**, fall back to a minimal built-in HTML page
   (`HttpStatus::defaultErrorBody()`) — so a server with *zero* error pages
   configured still never sends back a broken, empty, or confusing
   response. The subject requires this outright: "Your server must have
   default error pages if none are provided."

```cpp
void writeErrorResponse(Connection& conn, int code, const Location* loc) {
    if (loc) {
        std::map<int, std::string>::const_iterator it = loc->error_pages.find(code);
        if (it != loc->error_pages.end()) {
            std::ifstream file(it->second.c_str(), std::ios::binary);
            if (file.is_open()) {
                std::ostringstream buf;
                buf << file.rdbuf();
                writeResponse(conn, code, http_status::mimeType(it->second), buf.str());
                return;
            }
        }
    }
    if (conn.server_conf) {
        std::map<int, std::string>::const_iterator it = conn.server_conf->error_pages.find(code);
        if (it != conn.server_conf->error_pages.end()) {
            // ...same open-and-serve pattern as above, one level less specific
        }
    }
    writeResponse(conn, code, "text/html", http_status::defaultErrorBody(code));
}
```

Each tier only falls through to the next if it *doesn't* return early —
`loc` might be null (some rejections, like a totally unmatched URL, happen
before a location was ever found), a configured page might not exist on
disk, or nothing might be configured at all. Every one of those cases
still ends up at the same last line, which is what guarantees a response
always goes out.

This is a genuinely useful design pattern beyond just this project: **most
specific setting wins, with a sane universal fallback at the bottom**, so
nothing ever falls through to "undefined behavior."

## 5.3 — A quick reference: which status code means what, here

| Code | Meaning here |
|---|---|
| 200 | OK — normal successful response |
| 201 | Created — a POST/upload wrote a new file |
| 204 | No Content — a DELETE succeeded, nothing to send back |
| 301 | Moved Permanently — `return` redirect, or a directory needing a trailing slash |
| 302/303/307 | Other `return`-directive redirect flavors |
| 400 | Bad Request — malformed request line/headers/body framing |
| 403 | Forbidden — directory traversal attempt, or a filesystem permission failure |
| 404 | Not Found — no matching location, or nothing at the resolved path |
| 405 | Method Not Allowed — a real method, just not permitted on this route |
| 413 | Payload Too Large — body over `client_max_body_size` |
| 414 | URI Too Long |
| 431 | Request Header Fields Too Large — too many headers, or the header section itself too big |
| 500 | Internal Server Error — something failed on *our* side (e.g. couldn't open a file to write an upload) |
| 501 | Not Implemented — a method this server doesn't recognize at all |
| 502 | Bad Gateway — a CGI script failed to even start, exited nonzero with no output, or was killed by a signal (output or not) |
| 504 | Gateway Timeout — a CGI script ran past its time limit |
| 505 | HTTP Version Not Supported |

---

# Chapter 6 — CGI: hiring an outside contractor

This is the most involved part of the module, so it gets the most space.
CGI (the **Common Gateway Interface**, RFC 3875) is how a web server hands
a request off to a *completely separate program* — a Python script, a
shell script, a compiled binary — and gets back a response to forward to
the client. Think `.py` files that generate a page dynamically, instead of
serving a fixed `.html` file.

## 6.1 — The core idea: don't talk directly, use mailboxes

**Analogy: hiring an outside contractor who works in a locked room, and
communicating only via two mail slots.** You don't walk into their room and
have a conversation — you slide instructions through one slot (their
"inbox"), and they slide their finished work back through a different slot
(their "outbox"). This is exactly what a **pipe** is: a one-way tube of
bytes between two processes. The server opens two of them —
`conn.cgi_stdin_fd` (our outbox, their inbox) and `conn.cgi_stdout_fd`
(their outbox, our inbox) — before the contractor even starts working.

## 6.2 — `fork()` + `execve()`: clone yourself, then transform

Starting the contractor is a two-step Unix dance, in `cgi_handler::start()`
(`srcs/cgi/CgiHandler.cpp`):

```cpp
pid_t pid = fork();
```

**`fork()` clones the current process into two identical copies** — same
code, same memory, same everything, running from the exact same point,
distinguished only by `fork()`'s own return value (0 in the new child, the
child's process ID in the original parent). It's less "hiring someone new"
and more "an office worker suddenly splits into two identical people, and
one of them immediately walks out the door."

That's why the very next thing the child does, right after `fork()`, is:

```cpp
execve(interpreter.c_str(), &argv[0], &envp[0]);
```

**`execve()` replaces the *entire* running program in the child process**
with a brand new one — the interpreter (`python3`, `/bin/sh`, ...) — while
keeping the same process ID and the same open file descriptors. That's the
"immediately transforms into a completely different specialist" half of
the metaphor: the clone doesn't keep being a copy of the web server, it
*becomes* the CGI interpreter, argv `[interpreter, scriptPath]`, running
the requested script as its actual first argument (the subject specifically
asks for this: "call the CGI with the file requested as the first
argument").

Before that swap happens, the child:
- `dup2()`'s the pipe ends onto its own stdin/stdout, so the interpreter
  reading `stdin`/writing `stdout` is — without knowing it — actually
  talking through our pipes.
- `chdir()`'s into the script's own directory, so if the script opens a
  file using a relative path, it resolves the way the script's author
  expects, not relative to wherever the web server itself happened to be
  launched from.

If `execve()` itself fails (bad interpreter path, no permission, ...), the
child calls `_exit(127)` — `127` being the conventional Unix "command not
found" exit code, which the parent later checks for specifically (§6.5).

## 6.3 — Telling the contractor what the job is: environment variables

The interpreter that gets `execve()`'d has no idea it's running inside a
web server, or what the original HTTP request even looked like — unless we
tell it. That's `buildEnv()`'s whole job: translating the parsed HTTP
request into a list of `KEY=value` strings the CGI process receives as its
environment (the same mechanism a shell uses for `$PATH`, `$HOME`, etc.).

**Analogy: a work order form filled out before the contractor starts.**
Instead of the contractor asking questions, everything they need to know
is already sitting in labeled fields when they arrive:

| Variable | What it tells the script |
|---|---|
| `REQUEST_METHOD` | `GET`, `POST`, `DELETE`, ... |
| `SCRIPT_NAME` | The URL path up to (and including) the script itself |
| `SCRIPT_FILENAME` | The real filesystem path of the script |
| `PATH_INFO` | Anything *after* the script in the URL (see §6.4) |
| `QUERY_STRING` | Everything after the `?` in the URL |
| `CONTENT_LENGTH` / `CONTENT_TYPE` | Size and type of the request body, if any |
| `SERVER_PROTOCOL` | `HTTP/1.1` or `HTTP/1.0` |
| `SERVER_NAME` / `SERVER_PORT` | Which server block, which port |
| `REQUEST_URI` | The full original path + query string |
| `HTTP_<HEADER_NAME>` | **Every** request header, individually, uppercased with dashes turned to underscores (`Accept-Language` becomes `HTTP_ACCEPT_LANGUAGE`) — this is how a script reads cookies, auth tokens, content negotiation headers, anything the client sent |

That last one is the loop at the bottom of `buildEnv()` — it walks every
header the client sent (except `Content-Length`/`Content-Type`, which
already got their own dedicated variables) and re-exposes each one, which
is exactly what the CGI/1.1 spec requires: *"The full request and
arguments provided by the client must be available to the CGI,"* as the
subject itself puts it.

## 6.4 — `SCRIPT_NAME` vs. `PATH_INFO`: splitting a URL in two

A URL like `/cgi-bin/gallery.py/vacation/beach.jpg` is ambiguous on
purpose: is `gallery.py` the script, with `/vacation/beach.jpg` as extra
information for it to interpret (say, a photo gallery script using the
"path" as which album/photo to show)? RFC 3875 says yes — this is exactly
what `PATH_INFO` is for.

`resolveCgiScript()` (`RequestHandler.cpp`) walks the URL one `/`-segment
at a time, checking at each boundary whether *that much* of the path
resolves to a real file with a configured CGI extension. The first one it
finds *is* the script; everything after it becomes `PATH_INFO`. Once
found, `buildEnv()` splits the two apart:

```cpp
std::string scriptName = conn.path;
if (!conn.cgi_path_info.empty() && conn.cgi_path_info.size() <= scriptName.size())
    scriptName.erase(scriptName.size() - conn.cgi_path_info.size());
```

`SCRIPT_NAME` = `/cgi-bin/gallery.py`, `PATH_INFO` = `/vacation/beach.jpg`
— exactly the split the script expects to receive.

## 6.5 — Not waiting around: pipes join the same `poll()` everyone else uses

This is the part that ties CGI back into Chapter 1's central rule: **never
block, ever.** A CGI script could be instant, or it could take ten
seconds, or it could hang forever. The server can't afford to sit and wait
on any one of those possibilities while every other client goes unanswered.

So the two pipe file descriptors get registered into the *exact same*
`poll_fds` array as every listening socket and every client socket (this
is William's registry code, but it's what makes the next part possible).
When `poll()` reports the CGI's stdin pipe is ready to accept more bytes,
`onStdinWritable()` writes one chunk of the request body into it. When it
reports the stdout pipe has data ready to read, `onStdoutReadable()` reads
one chunk of the script's output. Neither function ever blocks waiting —
they only ever act *after* `poll()` has already said "this is ready right
now."

```cpp
void onStdinWritable(Connection& conn) {
    ...
    ssize_t n = write(conn.cgi_stdin_fd, ...);
    ...
}
```

This is a direct, literal implementation of "the contractor's mail slots
get checked on the same rounds as everyone else's" — the CGI pipes aren't
a special case requiring their own separate waiting loop; they're just two
more entries on the one big board everything else already uses.

## 6.6 — Knowing when the contractor is actually finished

A pipe closing (both fds hitting `-1`, `isDone()`) only means the script
has stopped talking — it doesn't yet say whether the script *succeeded*.
That's what `finish()` figures out with one non-blocking `waitpid()` call,
checking the child's real exit status:

- **Exited nonzero** — whether that's status 127 (the "command not found"
  convention from §6.2, meaning the interpreter itself never even started)
  or any other nonzero status (a script that crashed on its own, e.g. a
  Python syntax error) — *and* produced no output → **`502 Bad Gateway`**.
  A script can fail for reasons that have nothing to do with execve() —
  the interpreter starts fine but the script itself errors out before
  printing anything — and that's just as much "nothing usable to send
  back" as an execve() failure is, so both collapse to the same check:
  nonzero exit, empty stdout.
- **Killed by a signal** (a segfault, for instance) → **always `502`**,
  *regardless of whether it had already printed something first*. This
  is stricter than the nonzero-exit case above on purpose: a signal kill
  is never a legitimate way for a script to finish, so partial output
  doesn't get the benefit of the doubt. Trusting it would mean serving a
  silently truncated response — valid-looking headers plus half a body —
  as a normal `200`, with nothing to tell a client the script never
  actually completed. (This was a real, verified bug: a script that wrote
  full headers plus part of a body and then `os.abort()`'d used to come
  back as a clean, if short, `200 OK`.)
- **Otherwise** — even a script that exits with a nonzero status but *did*
  print a valid response — its output is trusted and forwarded as-is via
  `finishFromCgiOutput()`. A CGI script's own **`Status:`** header (if it
  wrote one) becomes the actual HTTP status code of the response; its own
  `Content-Type:` header is honored too. This lets a script override the
  default `200` when it needs to — a script can literally say "actually,
  answer this with a 226" and the server will.

```cpp
bool finish(Connection& conn, pid_t& pendingPid) {
    pendingPid = 0;
    if (conn.cgi_pid == -1) {
        finishFromCgiOutput(conn, conn.cgi_out);
        conn.state = WRITING_RESPONSE;
        return true;
    }
    int status = 0;
    pid_t reaped = waitpid(conn.cgi_pid, &status, WNOHANG);
    if (reaped != conn.cgi_pid)
        return false;                 // <-- the race, handled: try again next poll()

    bool failedExit = WIFEXITED(status) && WEXITSTATUS(status) != 0;
    bool crashed = WIFSIGNALED(status);
    conn.cgi_pid = -1;

    if (crashed || (failedExit && conn.cgi_out.empty()))
        request_handler::writeErrorResponse(conn, 502);
    else
        finishFromCgiOutput(conn, conn.cgi_out);
    conn.state = WRITING_RESPONSE;
    return true;
}
```

Notice `crashed` sits *outside* the `&& conn.cgi_out.empty()` check that
`failedExit` is still gated by — that asymmetry is the whole fix. A
nonzero exit with real output on stdout is still trusted (a script can
legitimately `sys.exit(1)` after printing a complete, valid response); a
signal kill never is, output or not.

`finishFromCgiOutput()` itself is also deliberately selective about which
of the script's own headers it forwards. `Status:` and `Content-Type:`
are read and honored, as above; but `Content-Length`, `Date`, `Server`,
and `Connection` are silently **dropped**, not forwarded:

```cpp
} else if (lowerKey == "content-length" || lowerKey == "date" ||
           lowerKey == "server" || lowerKey == "connection") {
    // writeResponse() always writes its own correct version of each of
    // these -- the real Content-Length computed from the actual body,
    // our own Date/Server, the connection's own keep-alive decision.
    // Forwarding the script's version too would put two of the same
    // header on the wire.
} else {
    extraHeaders += key + ": " + value + "\r\n";
}
```

Without this, a script that set its own (correct or not) `Content-Length`
header would end up on the wire *twice* — `writeResponse()`'s own
correctly-computed one, plus the script's, verbatim, as an "extra"
header. Two `Content-Length` headers in one response is exactly the kind
of framing ambiguity RFC 7230 says a server must never produce, and
exactly the same category of problem the request-parsing side already
guards against for *incoming* requests (§2.3) — just discovered on the
way out instead of the way in.

**A subtlety that took real debugging to get right:** a child process's
pipes closing, and that same child becoming *reapable* via `waitpid()`, are
**two separate events from the kernel's point of view**, and they don't
necessarily happen in the same instant. Calling `waitpid()` immediately
after seeing both pipes close can, some of the time, come back empty —
"not ready yet" — even though the script is about to be reapable a moment
later. Getting this wrong (assuming "pipes closed" always means "ready to
reap") caused a **real bug**: roughly 1 in 3 CGI requests would get
misreported as a plain `200` when they should have been a `502`, because
the code guessed instead of checking. The fix is exactly what you'd expect
once you see the race clearly: `finish()` returns `false` ("not ready,
don't change anything") instead of guessing, and the caller just tries
again on the next `poll()` iteration.

## 6.7 — Firing a contractor who's taking too long

Every CGI gets a 10-second deadline (`conn.cgi_deadline`,
`CGI_TIMEOUT_SECONDS`). If a script is still running when that deadline
passes, `abortTimeout()` sends it `SIGKILL` and answers the original
client with **`504 Gateway Timeout`** — and critically, *this happens
without freezing anything else the server is doing*. A hung CGI script on
one connection has zero effect on any other connection's requests, which
was verified directly: with one client's request stuck behind a
deliberately-hanging CGI script, a second, unrelated client's plain `GET /`
completed in well under a millisecond while the first was still waiting
out its timeout.

```cpp
pid_t abortTimeout(Connection& conn) {
    if (conn.cgi_pid == -1) {
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
```

Notice `SIGKILL`, not `SIGTERM`: a script that's already stuck (in an
infinite loop, blocked on something that will never unblock) can't be
trusted to notice and honor a polite "please stop" — `SIGKILL` can't be
caught, blocked, or ignored, so it's guaranteed to end the process. The
`waitpid()` right after is the same "maybe not reapable yet" situation
from §6.6, handled the same way: if it's not ready, the pid is handed back
to the caller as `pending` instead of blocking to wait for it.

## 6.8 — Cleaning up after: avoiding zombie processes

A child process that has exited but hasn't been "acknowledged" via
`waitpid()` yet lingers in the operating system's process table as a
**zombie** — not doing anything, but still taking up a slot, forever, until
something reaps it. Left unchecked across thousands of requests, that's a
slow leak that will eventually exhaust the system.

```cpp
bool reapIfExited(pid_t pid) {
    int status = 0;
    return waitpid(pid, &status, WNOHANG) == pid;
}
```

The fix here is `reapIfExited()`, called opportunistically every loop
iteration for any pid still owed a reap — always with `WNOHANG`, so it
never blocks waiting for a process that isn't ready yet. Note also what
this code deliberately **never** does: call `waitpid(-1, ...)`, which means
"reap *anything* that's exited, I don't care which pid." With several CGI
children potentially in flight for different clients simultaneously, that
call could reap the *wrong* child and steal another connection's exit
status out from under it. Every reap here names its exact pid.

---

# Chapter 7 — The configuration file: teaching the server without recompiling it

## 7.1 — Turning text into structure

`Config::load()` (`srcs/config/Config.cpp`) reads the whole config file
into memory and processes it in two passes:

1. **Tokenize** — walk the raw text once, character by character, splitting
   it into a flat list of words. `{` and `}` are always their own token
   (even with no space around them); a `#` starts a comment that runs to
   the end of the line; everything else is split on whitespace.
2. **Parse** — walk that token list expecting a specific shape: zero or
   more `server { ... }` blocks at the top level, each containing
   directives and `location { ... }` blocks, each of *those* containing
   its own directives (Chapter 3.2 covers what each directive means).

The tokenizer, in full — it's short enough to read as one piece:

```cpp
std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> tokens;
    std::string cur;
    bool inComment = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (inComment) {
            if (c == '\n')
                inComment = false;
            continue;
        }
        if (c == '#') {
            inComment = true;
            continue;
        }
        if (c == '{' || c == '}') {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
            tokens.push_back(std::string(1, c));
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        tokens.push_back(cur);
    return tokens;
}
```

One pass, one character at a time, accumulating into `cur` until something
ends the current token (whitespace, `{`, `}`, or a comment starting) — the
exact same incremental-accumulation shape as `try_parse_request()` in
Chapter 2, just over a whole file read into memory at once instead of a
socket buffer that arrives piece by piece. `location /cgi-bin{root x}`
(no spaces at all around the braces) tokenizes identically to the
nicely-spaced version in §7.2, because `{`/`}` always force a token
boundary regardless of what's touching them.

**A deliberate design choice worth naming:** an unrecognized directive, or
one with a malformed value, makes `Config::load()` `throw`. It's caught in
`main()` and turned into a clean startup failure with an error message —
never a crash, but also never a silent "I'll just ignore what I don't
understand and keep going." A typo in the config file should be loud and
obvious the moment you try to start the server, not a mystery you discover
during a live evaluation when some route just doesn't behave as expected.

## 7.2 — A real example, annotated

```conf
server {
    listen 127.0.0.1:8080
    server_name localhost
    client_max_body_size 1048576
    error_page 404 www/errors/404.html

    location / {
        root www
        index index.html
        methods GET
        autoindex off
    }

    location /cgi-bin {
        root www/cgi-bin
        methods GET POST
        cgi .py /usr/bin/python3
    }

    location /upload {
        root www/upload
        methods GET POST DELETE
        upload_store www/upload
        autoindex on
    }
}
```

Read top to bottom: this server listens on `127.0.0.1:8080`, allows up to
1MB request bodies by default, and shows `www/errors/404.html` for any
`404`. The root `/` serves static files out of `www/`, GET-only, no
directory listing. `/cgi-bin` allows GET and POST, and treats any `.py`
file under it as a script to run through `/usr/bin/python3`. `/upload`
allows the full GET/POST/DELETE lifecycle and writes uploaded files into
`www/upload`.

---

# Chapter 8 — One request, start to finish

Let's tie every chapter together by narrating one concrete request all the
way through: a browser loading `http://localhost:8080/cgi-bin/hello.py?name=webserv`.

1. **The bytes arrive.** William's `poll()` loop sees the client socket is
   readable, calls `read()`, appends whatever came in to
   `conn.read_buffer`, and calls `try_parse_request(conn)`.
2. **Headers aren't ready yet the first time through.** The parser looks
   for `\r\n\r\n`. If the whole request arrived in one `read()` (likely,
   for something this small), it's found immediately.
3. **The request line is split**: method `GET`, target
   `/cgi-bin/hello.py?name=webserv`, version `HTTP/1.1`. Version is
   recognized, target starts with `/` and isn't over-length — no rejection
   yet.
4. **The target is split on `?`**: path `/cgi-bin/hello.py`, query string
   `name=webserv`.
5. **Headers are parsed** into the lower-cased map — `Host`, `User-Agent`,
   `Accept`, etc. `Host` is present, so the `HTTP/1.1`-requires-`Host`
   check passes.
6. **No `Content-Length`, no `Transfer-Encoding`** — this is a `GET`, no
   body expected. `conn.body` stays empty.
7. **`try_parse_request()` returns `true`.** `handle_request(conn)` is
   called.
8. **Routing**: `matchLocation("/cgi-bin/hello.py")` finds the `/cgi-bin`
   location (longest matching prefix). It has no `return`, and `GET` is in
   its allowed methods, so we continue.
9. **Traversal guard**: the remainder of the path (`/hello.py`) has no
   `..` segment. Safe to proceed.
10. **`stat()`** finds `www/cgi-bin/hello.py` exists and is a regular file.
    Its extension (`.py`) is in the location's `cgi_extensions` map →
    **this is a CGI request.**
11. **`cgi_handler::start()`** opens two pipes, `fork()`s, and in the
    child: `dup2()`s the pipes onto stdin/stdout, `chdir()`s into
    `www/cgi-bin`, and `execve()`s `/usr/bin/python3` with `hello.py` as
    its argument and a full CGI/1.1 environment (`REQUEST_METHOD=GET`,
    `QUERY_STRING=name=webserv`, `SCRIPT_NAME=/cgi-bin/hello.py`, every
    `HTTP_*` header, ...). `conn.state` becomes `CGI_RUNNING`.
12. **The body is empty**, so there's nothing to write to the script's
    stdin — `conn.cgi_stdin_fd` is immediately closed and marked `-1`.
13. **A few milliseconds later**, `poll()` reports the script's stdout pipe
    is readable. `onStdoutReadable()` reads the script's printed HTML into
    `conn.cgi_out`. This might take one read or several, depending on how
    much the script prints and how the kernel buffers it — each one just
    appends to `conn.cgi_out`.
14. **The script exits, its stdout pipe closes.** `isDone()` is now true.
    `finish()` calls `waitpid()`, sees a normal exit (not 127, not
    signaled), and calls `finishFromCgiOutput()`, which finds the script's
    own `Content-Type: text/html` header, and forwards everything after
    the blank line as the body.
15. **`writeResponse()`** assembles the final bytes: status line, `Date`,
    `Server`, `Connection`, `Content-Type`, `Content-Length`, and the
    script's HTML body.
16. **William's loop flips the socket to `POLLOUT`**, and once `poll()`
    says it's writable, sends the response — possibly across several
    `write()` calls for a larger page, each one only happening after
    `poll()` confirms the socket is ready.
17. **If the connection is keep-alive**, this module's per-request state
    (`headers_ready`, `body_start`, `chunked_scan_pos`) is reset, and the
    server immediately checks whether a *second* request is already
    sitting in the buffer (a browser might have pipelined one). If not, it
    goes back to waiting for more bytes on the next `poll()` — ready to
    start this whole story over again.

---

# Appendix A — File-by-file map

### `include/` — the shared contract with William's Core Server

**`connection.hpp`** — the `Connection` struct and `ConnState` enum
(`READING_REQUEST → PROCESSING → (CGI_RUNNING) → WRITING_RESPONSE → DONE`),
shared by both halves. Fields this module owns: `method`, `path`,
`http_version`, `headers`, `body`, `query_string`, `cgi_path_info`,
`status_code`, the whole CGI bookkeeping group (`cgi_stdin_fd`,
`cgi_stdout_fd`, `cgi_pid`, `cgi_out`, `cgi_in_offset`, `cgi_deadline`),
and the incremental-parsing cache (`headers_ready`, `body_start`,
`chunked_scan_pos` — Chapter 2.4/2.5). `try_parse_request()` and
`handle_request()` are declared here since they're the entire contract
between the two halves of the project.

**`Config.hpp`** — a forwarding shim to `srcs/config/Config.hpp`.

### `srcs/http/` — request parsing and routing

**`RequestParser.hpp`/`.cpp`** — `try_parse_request()` (Chapter 2),
`request_parser::decodeChunked()`, and the file-local helpers
`findHeaderEnd()`, `collapseSlashes()`, `failParse()`.

**`RequestHandler.hpp`/`.cpp`** — `handle_request()` (Chapters 3–5),
`writeResponse()`/`writeErrorResponse()` (also called from
`CgiHandler.cpp`, so CGI error pages look identical to every other error
path), and the routing helpers: `joinPath()`, `hasDotDotSegment()`,
`extensionOf()`/`basenameOf()`, `resolveCgiScript()` (Chapter 6.4),
`serveFile()`, `serveAutoindex()`, `isKnownMethod()` (Chapter 3.4),
`formatHttpDate()`/`httpDate()` (Chapter 5.1).

**`HttpStatus.hpp`/`.cpp`** — `reasonPhrase()`, `defaultErrorBody()`,
`mimeType()`. Small, stateless lookup tables.

### `srcs/cgi/` — CGI execution (Chapter 6)

**`CgiHandler.hpp`/`.cpp`** — `start()`, `onStdinWritable()`/
`onStdoutReadable()`, `isDone()`, `finish()`, `abortTimeout()`,
`reapIfExited()`, plus the file-local `splitDirFile()`, `parentPath()`,
`buildEnv()`, `finishFromCgiOutput()`.

### `srcs/config/` — configuration (Chapter 7)

**`Config.hpp`/`.cpp`** — `Location` and `ServerConfig` structs,
`Location::methodAllowed()`, `ServerConfig::matchLocation()`, and the
parser itself: `tokenize()`, `parseLocation()`, `parseServer()`,
`Config::load()`.

### `srcs/utils/` — shared helpers

**`StringUtils.hpp`/`.cpp`** — `trim`, `toLower`/`toUpper`, `split`,
`startsWith`, `toString`, `toLong`, `urlDecode`, `headerKeyToEnv`. The
small building blocks everything above is written in terms of. Nothing
here allocates on the heap beyond ordinary `std::string`/`std::vector`
growth — this module has zero `new`/`malloc`/`calloc` calls anywhere.

### `conf/` — configuration files

**`default.conf`** — used when `webserv` is launched with no argument.

**`test.conf`** — the main demo config: two `server{}` blocks, one real
site on `:8080` (static serving, autoindex demo, redirects, CGI, uploads)
and a genuinely separate second site on `:8081` with a deliberately tiny
10-byte body limit, to demonstrate `413` on demand.

**`tester.conf`** — config shaped to match the official 42 `tester` Go
binary's exact, stated requirements (Chapter 9 of the eval process).

### `www/` — the site itself

Static pages, `cgi-bin/` fixture scripts (GET/POST CGI, `PATH_INFO`, a
deliberately hanging script, a script-that-isn't for exercising `502`),
`errors/404.html`, `listing/` (autoindex demo files), upload targets, a
second distinct site (`site2/`), and the `YoupiBanane/` fixture the
official tester's setup instructions require.

---

# Appendix B — Real bugs found, and how

This project's correctness wasn't established by "it compiled and curl
looks right" — it was established by throwing real, adversarial, and
high-volume traffic at it and watching what broke. Some of what that
process actually found, because seeing *real* bugs and their fixes is
often more instructive than any amount of clean-path explanation:

- **`HEAD` responses were leaking a body** (a real RFC 7231 violation) —
  fixed in `writeResponse()`.
- **No trailing-slash redirect for directories** — fixed in
  `matchLocation()`/`handle_request()` (Chapter 4.3).
- **A genuine race in `finish()`** between a CGI child's pipes closing and
  it becoming reapable, misreporting `502` as `200` roughly 1 in 3
  requests — fixed by reporting "not ready" instead of guessing (Chapter
  6.6).
- **`431` was silently unenforced** for a header section that arrived
  whole in a single `read()` (only the still-growing case was checked) —
  and fixing *that* then made `414` unreachable for an overlong URI, which
  needed its own fix: check the request line's own length before the
  whole-section length (Chapter 2.2).
- **`PATH_INFO` (RFC 3875) wasn't implemented at all**, originally — added
  via `resolveCgiScript()` (Chapter 6.4).
- **A real O(n²) performance blowup**, found via the official tester's own
  20-concurrent-100MB-CGI-POST stress case: a CPU core pegged at 100% for
  over nine minutes with almost no progress. Root cause: both the chunked
  decoder and the header/body separator search were redoing already-
  finished work on every partial read instead of resuming from a cached
  position. Fixed by adding exactly that cache (`chunked_scan_pos`,
  `body_start` — Chapter 2.4/2.5).
- **The official 42 `cgi_tester` binary's non-standard `PATH_INFO`
  expectation**, and **CGI dispatch originally requiring the target file
  to exist** (it shouldn't, for an interpreter that never touches the
  filesystem itself) — both found by running the official tester directly
  and reverse-engineering its exact checks (via `objdump` on the
  unstripped binary), both fixed in `CgiHandler.cpp`/`RequestHandler.cpp`.
- **Missing per-location `client_max_body_size`** — the config format
  originally only supported a server-wide limit, but a real test case
  needed a route with its own tighter one. Added as a `Location`-level
  override.
- **No `Date` header, and no `Last-Modified` header** — both are
  RFC-expected on ordinary HTTP responses and were simply missing; added
  once an independent third-party test suite specifically flagged their
  absence.
- **No distinction between "unrecognized method" and "recognized but not
  allowed here"** — everything not on a location's `methods` list was
  getting a blanket `405`, when an entirely-unknown method should be `501`
  instead (Chapter 3.4).
- **A request with both `Content-Length` and `Transfer-Encoding: chunked`
  was silently resolved by preferring one of the two**, instead of being
  rejected outright — closed as the classic request-smuggling ambiguity it
  is (Chapter 2.3).

---

# Appendix C — What's been verified

Beyond ordinary `curl`, this module's behavior has been checked against:

- **A real browser**, driven end-to-end: page rendering, in-page
  navigation, autoindex listings, CGI over `GET` with a query string, CGI
  over `POST` via `fetch()`, and an unrecognized-method request — all
  confirmed correct, with the connection staying healthy afterward (no
  crash, no hang).
- **The official 42 `tester` Go binary**, against `conf/tester.conf` + the
  `YoupiBanane` fixture, including its heaviest concurrency case (20
  workers × 5 requests each, 100MB CGI POST per request; 128 concurrent
  workers hammering a single route): **exits 0, no failures**.
- **Two independent third-party Python test suites** (not written by
  anyone on this project), each testing HTTP conformance from a different
  angle — used specifically to find gaps this project's own tests
  wouldn't have thought to check, several of which turned into real fixes
  listed in Appendix B.
- **`valgrind`**, functional suite + a clean `SIGINT` shutdown: no leaks,
  no invalid-fd reports — consistent with there being no `new`/`malloc`
  anywhere in this module.

---

# Appendix D — Where things stand

- Code lives on the `kai` branch of `github.com/Kai-Doh/Webserv`, merged
  into `main`; `william` holds the Core Server side.
- `main` is a working, integrated `webserv` binary: `srcs/main.cpp` →
  `Server::run()` → this module, CGI pipes included.
- Every non-trivial function across this module has a `@brief`/`@param`/
  `@return` doc-comment in the source itself — this document explains the
  *why*; the source comments pin down the exact *what*.
- Outstanding: nothing blocking. Virtual hosting (routing by `Host:`
  header to different `server{}` blocks sharing one port) is explicitly
  optional per the subject and isn't implemented; everything the subject
  actually requires is.

---

# Appendix E — A real example: opening the site in a browser

Chapter 8 traced one CGI request end to end. This is the everyday case
instead: what actually happens, request by request, when someone types
`http://localhost:8080/` into a browser's address bar and hits Enter —
using this project's own real `www/index.html` and `conf/test.conf`, with
real file sizes, not made-up numbers.

## E.1 — Request #1: the page itself

The browser opens a TCP connection to `127.0.0.1:8080` (William's side —
out of scope here, but it's what makes the next line possible) and sends:

```
GET / HTTP/1.1
Host: localhost
User-Agent: Mozilla/5.0 ...
Accept: text/html,application/xhtml+xml,...
Connection: keep-alive

```

Here is *every* function call this module makes to answer it, in the exact
order it makes them, with the real argument values at each step — not just
"which function runs," but **what gets put into it and what comes back
out**.

**1. `try_parse_request(conn)`** (Chapter 2) — after finding `\r\n\r\n` and
splitting the request line, this is what ends up sitting in `conn` by the
time it returns:

```cpp
conn.method       = "GET"
conn.path         = "/"
conn.query_string = ""
conn.http_version = "HTTP/1.1"
conn.headers      = { "host": "localhost", "user-agent": "Mozilla/5.0 ...",
                       "accept": "text/html,...", "connection": "keep-alive" }
conn.keep_alive   = true    // HTTP/1.1's default, confirmed by the Connection header
conn.body         = ""      // no Content-Length, no Transfer-Encoding -> zero-byte body
conn.status_code  = 0       // 0 means "no parse error" (Chapter 3 §1 checks this first)
```
`try_parse_request()` returns `true` — a complete, valid request. Nothing
here required a second `read()`; it all arrived in one TCP segment.

**2. `handle_request(conn)`** (Chapter 3) reads those fields back out and
starts calling into the routing helpers, in order:

```cpp
const Location* loc = conn.server_conf->matchLocation("/");
```
`matchLocation()` (§3.1) walks every `location{}` in `conf/test.conf`'s
first `server{}` block and checks each `path` against `"/"`:

| Location's `path` | Does `"/"` match it? |
|---|---|
| `/` | yes — exact match, length 1 |
| `/listing` | no (`"/"` doesn't start with `/listing`) |
| `/cgi-bin`, `/upload`, `/old`, `/moved` | no, same reason |

Only one candidate, so it wins by default: `loc` now points at the
`location /` block — `loc->root == "www"`, `loc->index == "index.html"`,
`loc->methods == ["GET"]`, `loc->autoindex == false`.

```cpp
loc->redirect_target.empty()      // -> true  (no `return` directive on this location) -> skip
loc->methodAllowed("GET")         // -> true  ("GET" is in loc->methods) -> continue
loc->root.empty()                 // -> false ("www") -> continue

std::string rel = conn.path.substr(loc->path.size());
// conn.path == "/", loc->path == "/", both length 1
// -> rel = "/".substr(1) = ""

hasDotDotSegment(rel)             // su::split("", '/') is empty -> no ".." segment -> false

std::string fsPath = joinPath(loc->root, rel);
// rel.empty() is true, so joinPath's first branch fires: return root as-is
// -> fsPath = "www"

struct stat st;
bool exists = (stat(fsPath.c_str(), &st) == 0);
// -> exists = true, S_ISDIR(st.st_mode) = true  ("www" is a directory)
```

Method isn't `DELETE`; the CGI check (`exists && S_ISREG(st.st_mode)`) is
`false` because `www` is a *directory*, not a regular file, so `isCgi`
stays `false`; method isn't `POST`; `exists` is `true` so the `404` branch
is skipped. That leaves the directory branch from Chapter 4.3:

```cpp
// conn.path is "/", and conn.path[conn.path.size()-1] == '/' is true
// -> the "needs a trailing slash" 301 branch is skipped entirely

loc->index.empty()                // -> false, index == "index.html"
std::string idx = joinPath(fsPath, loc->index);
// -> idx = joinPath("www", "index.html") = "www/index.html"

struct stat ist;
stat(idx.c_str(), &ist) == 0 && S_ISREG(ist.st_mode)
// -> true: www/index.html exists and is a regular file

serveFile(conn, "www/index.html", ist, loc);
```

**3. `serveFile(conn, "www/index.html", ist, loc)`** (Chapter 4.3) —
`ist.st_size` is this repository's real, current size for that file:

```cpp
std::ifstream file("www/index.html", std::ios::binary);   // opens fine
body.resize(5394);                                        // ist.st_size == 5394
file.read(&body[0], 5394);                                // whole file, one read
conn.status_code = 200;
writeResponse(conn, 200, http_status::mimeType("www/index.html"), body,
              "Last-Modified: " + formatHttpDate(ist.st_mtime) + "\r\n");
```
`mimeType("www/index.html")` looks at the substring after the last `.` —
`"html"` — and returns `"text/html"`. `formatHttpDate(ist.st_mtime)`
turns this file's real, current modification time into
`"Tue, 22 Sep 2026 14:36:43 GMT"`.

**4. `writeResponse(conn, 200, "text/html", body, "Last-Modified: ...\r\n")`**
(Chapter 5.1) assembles the actual bytes that go on the wire — real
numbers throughout, nothing invented:

```
HTTP/1.1 200 OK
Date: Tue, 22 Sep 2026 15:02:11 GMT
Server: webserv/1.0
Connection: keep-alive
Content-Type: text/html
Content-Length: 5394
Last-Modified: Tue, 22 Sep 2026 14:36:43 GMT

<!DOCTYPE html>
<html lang="en">
...
  <link rel="stylesheet" href="/styles.css">
...
</html>
```

(`Date` is *now* — generated fresh on every response, per §5.1;
`Last-Modified` is the file's own `stat()` mtime, and stays fixed until
`index.html` is actually edited.)

## E.2 — The browser reads the HTML, and asks for more

The browser doesn't stop at one request. Parsing the HTML it just
received, it finds `<link rel="stylesheet" href="/styles.css">` — a
resource it needs before it can even finish rendering the page — and,
separately, every browser also automatically probes `/favicon.ico`
whether or not the page ever mentions one. Two more requests, neither
initiated by a person clicking anything.

**Because the first response said `Connection: keep-alive`, the browser
doesn't open a new TCP connection for these** — it reuses the exact same
socket. On this module's side, that's the keep-alive path from Chapter
8 §17: once `writeResponse()`'s bytes finish going out, the connection
resets its per-request state (`headers_ready`, `body_start`,
`chunked_scan_pos` all go back to their defaults) and waits, on the *same*
`fd`, for whatever the browser sends next. From this module's point of
view, request #2 below is indistinguishable from a request on a
brand-new connection — `try_parse_request()`/`handle_request()` have no
notion of "this is the second request on this socket" at all.

## E.3 — Request #2: `GET /styles.css`

```
GET /styles.css HTTP/1.1
Host: localhost
Connection: keep-alive

```

Same functions, different arguments — this time worth tracing precisely
because the *decision* is subtly different, even though the outcome
(another `200`) looks the same:

```cpp
try_parse_request(conn);
// conn.method = "GET", conn.path = "/styles.css", conn.query_string = ""
// (a fresh Connection state -- resetConnectionForReuse() cleared the
// previous request's fields before this one arrived, per E.2)

const Location* loc = conn.server_conf->matchLocation("/styles.css");
// su::startsWith("/styles.css", "/listing") -> false
// su::startsWith("/styles.css", "/cgi-bin") -> false
// su::startsWith("/styles.css", "/")        -> true, and p == "/" so it counts
// -> still only one match: the same `location /` block as request #1

std::string rel = conn.path.substr(loc->path.size());
// "/styles.css".substr(1) = "styles.css"   <- NOT empty this time

hasDotDotSegment("styles.css");   // su::split gives {"styles.css"}, no ".." -> false

std::string fsPath = joinPath("www", "styles.css");
// rel isn't empty, root doesn't end in '/', rel doesn't start with '/'
// -> joinPath's middle branch: "www" + "/" + "styles.css" = "www/styles.css"

stat("www/styles.css", &st);
// -> exists = true, S_ISREG(st.st_mode) = true   <- a FILE this time, not a directory

extensionOf("www/styles.css");   // -> ".css"
loc->cgi_extensions.find(".css");   // -> not found (this location configures no CGI at all)
// -> isCgi stays false

// method isn't DELETE, isn't POST; exists is true; S_ISDIR(st.st_mode) is
// false (it's a file, not a directory) -> the whole directory branch
// (index/autoindex/301) is skipped entirely this time

serveFile(conn, "www/styles.css", st, loc);
```

The key difference from request #1: `rel` came out *non-empty*
(`"styles.css"`), which sent `fsPath` down `joinPath()`'s other branch,
and `stat()` found a regular file directly — no `index.html` lookup
needed, because there was no directory to resolve an index *inside*.
Same function, same location, genuinely different path through its logic,
purely because of what `rel` turned out to be.

```
HTTP/1.1 200 OK
Date: Tue, 22 Sep 2026 15:02:11 GMT
Server: webserv/1.0
Connection: keep-alive
Content-Type: text/css
Content-Length: 5180
Last-Modified: Tue, 22 Sep 2026 14:36:43 GMT

body { ... }
...
```

`Content-Type: text/css` comes from `HttpStatus::mimeType()`'s
extension table (Chapter 4.3) recognizing `.css` — nothing CSS-specific
had to be written anywhere in the request-handling code itself.

## E.4 — Request #3: `GET /favicon.ico` — and a real 404

```
GET /favicon.ico HTTP/1.1
Host: localhost
Connection: keep-alive

```

Traced the same way:

```cpp
const Location* loc = conn.server_conf->matchLocation("/favicon.ico");
// same reasoning as before -> still only `location /` matches

std::string rel = conn.path.substr(loc->path.size());
// "/favicon.ico".substr(1) = "favicon.ico"

std::string fsPath = joinPath("www", "favicon.ico");
// -> "www/favicon.ico"

struct stat st;
bool exists = (stat("www/favicon.ico", &st) == 0);
// -> exists = FALSE. This file was never added to www/ in this project.

// exists && S_ISREG(...) is false (exists is false) -> the CGI `if` is
// skipped; its `else if (!loc->cgi_extensions.empty())` is also false,
// since `location /` configures no `cgi` directives at all -> isCgi stays
// false either way

// method isn't DELETE, isn't POST
if (!exists) {
    request_handler::writeErrorResponse(conn, 404, loc);
    return;
}
```

**`writeErrorResponse(conn, 404, loc)`** (Chapter 5.2) — this is where the
"most specific setting wins" fallback chain actually runs:

```cpp
loc->error_pages.find(404);
// `location /` itself never sets its own `error_page` directive
// -> not found, first tier falls through

conn.server_conf->error_pages.find(404);
// conf/test.conf's server{} block DOES set: error_page 404 www/errors/404.html
// -> found: "www/errors/404.html"

std::ifstream file("www/errors/404.html", std::ios::binary);   // opens fine
// -> reads all 162 bytes of this project's real custom 404 page

writeResponse(conn, 404, http_status::mimeType("www/errors/404.html"), buf.str());
// mimeType(...) -> "text/html" (same ".html" -> text/html rule as request #1)
// no fourth argument this time -> extraHeaders defaults to "" -> no Last-Modified
```

Two tiers checked, the first came up empty, the second had exactly what
was needed — the fallback chain from §5.2 isn't just a description, this
is literally the two `if` blocks it's made of, run back to back:

```
HTTP/1.1 404 Not Found
Date: Tue, 22 Sep 2026 15:02:11 GMT
Server: webserv/1.0
Connection: keep-alive
Content-Type: text/html
Content-Length: 162

<!DOCTYPE html>
...a small custom "not found" page...
```

Notice there's no `Last-Modified` here — `writeErrorResponse()` calls
`writeResponse()` without that extra header (only `serveFile()`, in the
success path, ever adds it), which is a real, deliberate asymmetry: a
custom error page doesn't have one obvious "last changed" moment tied to
*this specific response* the way a requested file does.

## E.5 — Stepping back

Three requests, one TCP connection, zero new code paths invented for any
of them: every single one went through exactly `try_parse_request()` →
`handle_request()` → `writeResponse()`/`writeErrorResponse()`, the same
four functions Chapters 2 through 5 already fully explain. The only thing
that changed between them was which branch of `handle_request()`'s
decision tree (§3.3) each request's path happened to fall into — a
successful static file, another successful static file with a different
MIME type, and a location match with nothing at the resolved path. A
"page load" isn't a special case this code has to know about; it's just
several ordinary, independent requests that happen to arrive close
together on a connection that stayed open between them.
