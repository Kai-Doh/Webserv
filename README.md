*This project has been created as part of the 42 curriculum by ktiomico, wcapt.*

# Webserv

## Description

Webserv is an HTTP/1.1 server written from scratch in C++98 — no external
HTTP or socket library, just a single-threaded, non-blocking event loop
built on one `poll()` call for every socket and every CGI pipe at once.

It serves fully static websites, handles file uploads and deletions,
executes CGI scripts (PHP, Python, or anything else invoked via an
interpreter), and is driven entirely by an nginx-inspired configuration
file: multiple virtual servers on different `host:port` pairs, per-route
method restrictions, redirects, directory listing, default/error pages,
and per-location body-size limits.

The project is split into two halves that share one contract
(`Connection`, `try_parse_request()`, `handle_request()`, in
`include/connection.hpp`):

- **The Core Server** (`srcs/core/`, `srcs/net/`) — listen sockets,
  `accept()`, the shared `poll()` loop, connection/timeout bookkeeping,
  and the CGI pipe integration that grafts a running script's stdin/stdout
  onto that same loop.
- **HTTP + CGI** (`srcs/http/`, `srcs/cgi/`, `srcs/config/`, `srcs/utils/`)
  — request parsing, routing, static file serving, uploads, and CGI
  execution. Documented in detail in [DOCUMENTATION.md](DOCUMENTATION.md).

## Instructions

```sh
make                              # builds ./webserv
./webserv conf/test.conf          # serves on 127.0.0.1:8080 and :8081
./webserv                         # no argument: falls back to conf/default.conf
curl http://127.0.0.1:8080/
```

`make`/`clean`/`fclean`/`re` are all supported, compiling with
`-Wall -Wextra -Werror -std=c++98`.

`conf/test.conf` and the `www/` directory are a working demo config + site
exercising every mandatory feature: static files, directory autoindex,
redirects, a custom 404 page, file upload (`POST /upload/...`), `DELETE`,
and Python CGI scripts under `www/cgi-bin/`. A second `server{}` block
listens on `:8081` with a 10-byte body limit to demonstrate `413 Payload
Too Large`. `conf/tester.conf` is set up specifically for the official 42
`tester` binary and its `YoupiBanane` fixture under `www/YoupiBanane/`.

## Resources

- RFC 7230 (HTTP/1.1 message syntax) and RFC 3875 (CGI/1.1), for the
  request-parsing/chunked-decoding rules and the CGI environment-variable
  contract.
- nginx's `server`/`location` configuration syntax, as inspiration for
  `conf/*.conf`'s shape.
