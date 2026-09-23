#!/usr/bin/env bash
# Interactive feature demo for the Webserv HTTP/CGI half.
#
# Exercises an ALREADY-RUNNING `./webserv conf/test.conf` -- it never
# starts, stops, or edits anything itself. Run this in a second terminal:
#
#   ./webserv conf/test.conf &
#   ./scripts/eval_demo.sh
#
# Pick a feature from the menu. Each one prints the exact command it runs,
# then what webserv actually returned ("ours"), then roughly what that
# should look like ("expected"), so the two can be compared side by side.

BASE1="http://127.0.0.1:8080"
BASE2="http://127.0.0.1:8081"

GREEN='\033[1;32m'
BLUE='\033[1;34m'
YELLOW='\033[1;33m'
CYAN='\033[1;36m'
RESET='\033[0m'

section() {
    printf "\n${BLUE}=== %s ===${RESET}\n" "$1"
}

run() {
    printf "${GREEN}\$ %s${RESET}\n" "$*"
    "$@"
    echo
}

ours() {
    printf "${CYAN}-- ours --${RESET}\n"
}

expected() {
    printf "${YELLOW}-- expected --${RESET}\n%s\n" "$1"
}

initial_check() {
    if ! curl -s -o /dev/null --max-time 2 "$BASE1/"; then
        echo "webserv is not answering on $BASE1 -- launch it first, e.g.:"
        echo "    ./webserv conf/test.conf"
        exit 1
    fi
}

check_running() {
    if ! curl -s -o /dev/null --max-time 2 "$BASE1/"; then
        printf "${YELLOW}webserv isn't answering on %s right now -- start it and try again.${RESET}\n" "$BASE1"
        return 1
    fi
    return 0
}

# ---- feature demos ---------------------------------------------------------

demo_multi_port() {
    section "Multiple servers, different ports"
    ours
    run curl -s "$BASE1/"
    run curl -s "$BASE2/"
    expected "Two independent responses: :8080 serves www/index.html, :8081 serves www/site2/index.html -- one poll() loop, two listening sockets, two unrelated configs."
}

demo_error_page() {
    section "Custom error page"
    ours
    run curl -si "$BASE1/does-not-exist"
    expected "404 Not Found, body from www/errors/404.html (configured via error_page 404), not a generic hardcoded page."
}

demo_body_limit() {
    section "Client body size limit"
    ours
    run curl -s -X POST -H "Content-Type: plain/text" --data "this-is-twenty-bytes" "$BASE2/"
    run curl -s -X POST -H "Content-Type: plain/text" --data "hello" "$BASE2/"
    expected ":8081 has client_max_body_size 10. A 21-byte body -> 413 Payload Too Large. A 5-byte body -> accepted."
}

demo_routing() {
    section "Routing to separate directories"
    ours
    run curl -s "$BASE1/"
    run curl -s "$BASE1/listing/"
    expected "/ serves www/index.html; /listing/ serves a directory listing from www/listing (autoindex on) -- two locations, two roots, one server block."
}

demo_index_fallback() {
    section "Directory index file"
    ours
    run curl -si "$BASE1/"
    expected "GET / with no filename resolves to index index.html (configured on location /) -- 200, not a listing or a 404."
}

demo_methods() {
    section "Per-route allowed methods"
    ours
    run curl -si -X DELETE "$BASE1/index.html"
    expected "location / only allows GET -- DELETE there should be 405 Method Not Allowed, not silently accepted and not a crash."
}

demo_upload_cycle() {
    section "Upload, download, and delete a file"
    ours
    run curl -si -X POST --data-binary "evaluation demo content" "$BASE1/upload/eval_demo.txt"
    run curl -s "$BASE1/upload/eval_demo.txt"
    run curl -si -X DELETE "$BASE1/upload/eval_demo.txt"
    run curl -si "$BASE1/upload/eval_demo.txt"
    expected "POST writes the file under www/upload, GET reads back the exact bytes we sent, DELETE removes it, and the final GET 404s -- a full read/write/delete round trip on real files."
}

demo_unknown_method() {
    section "Unknown HTTP methods"
    ours
    run curl -si -X FOOBAR "$BASE1/"
    run curl -s "$BASE1/"
    expected "An unrecognized method gets a clean error response, and the server keeps running right after -- no crash, no hang, still answering normal GETs."
}

demo_cgi() {
    section "CGI execution and environment"
    ours
    run curl -s "$BASE1/cgi-bin/hello.py"
    run curl -s -X POST --data "field=value" "$BASE1/cgi-bin/echo.py"
    run curl -si "$BASE1/cgi-bin/anything.broken"
    expected "hello.py and echo.py run as real child processes through python3 and return normal 200s (echo.py reflects the POSTed body back). anything.broken points at a deliberately bogus interpreter -- execve fails in the child, which maps to 502 Bad Gateway, not a hang or a crash."
}

demo_cgi_syntax_error() {
    section "CGI script with a Python syntax error"
    ours
    run curl -si "$BASE1/cgi-bin/syntax_error.py"
    expected "python3 finds the interpreter fine (unlike anything.broken) but exits 1 with only a traceback on stderr -- stdout is empty. finish() treats any nonzero exit status with empty output as a failed CGI, the same bucket execve failure (exit 127) and signal crashes fall into -- 502 Bad Gateway, not a silent 200."
}

demo_concurrency() {
    section "Concurrent clients don't block each other"
    ours
    echo "\$ curl -s -o /dev/null -w \"hang.py: %{http_code} in %{time_total}s\\n\" $BASE1/cgi-bin/hang.py &"
    curl -s -o /dev/null -w "hang.py: %{http_code} in %{time_total}s\n" "$BASE1/cgi-bin/hang.py" &
    sleep 1
    run curl -s -o /dev/null -w "concurrent GET /: %{http_code} in %{time_total}s\n" "$BASE1/"
    echo "(waiting up to ~10s for hang.py's CGI deadline to fire...)"
    wait
    expected "A slow/stuck CGI for one client (hang.py) never blocks a second, unrelated client's GET / -- that request completes in well under a second even while hang.py is still running, because it's one non-blocking poll() loop, not one thread per client. hang.py itself eventually times out at 504."
}

demo_health() {
    section "Process health check"
    ours
    run bash -c "ps aux | grep '[w]ebserv'"
    expected "Exactly one webserv process, still alive after everything above -- no crashes, no zombie children left over from CGI."
}

run_all() {
    demo_multi_port
    demo_error_page
    demo_body_limit
    demo_routing
    demo_index_fallback
    demo_methods
    demo_upload_cycle
    demo_unknown_method
    demo_cgi
    demo_cgi_syntax_error
    demo_concurrency
    demo_health
    demo_sneaky_403
    demo_sneaky_400
    demo_sneaky_502
    demo_sneaky_405
    demo_sneaky_cgi_response
}

# ---- sneaky / adversarial error-code tests ----------------------------------
#
# Everything below was invented while specifically hunting for requests that
# LOOK like they should be fine but actually hide a bug -- a CGI crash that
# still manages to flush a plausible-looking response, a header collision
# that silently picks a winner, a duplicate Host header nobody rejected.
# Several of these can't be sent with curl at all (it normalizes ".." out of
# URLs before the request ever leaves the client, and there's no curl flag
# for "send two Host headers" or "put a space before this colon"), so they
# go straight to a raw socket via raw_request.py instead.

demo_sneaky_403() {
    section "Sneaky 403s: path traversal & permission-denied"
    ours
    echo "-- plain path traversal (raw socket -- curl would silently normalize this away) --"
    run bash -c "printf 'GET /../../../etc/passwd HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' | python3 scripts/raw_request.py"
    echo "-- traversal starting from inside a real location --"
    run bash -c "printf 'GET /listing/../../../etc/passwd HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' | python3 scripts/raw_request.py"
    echo "-- URL-encoded traversal (%2f = /) --"
    run bash -c "printf 'GET /..%%2f..%%2f..%%2fetc%%2fpasswd HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' | python3 scripts/raw_request.py"
    echo "-- double-encoded (%252f) -- NOT a bypass: single-decode leaves it a literal filename, so this one's an honest 404, not 403 --"
    run bash -c "printf 'GET /..%%252f..%%252fetc%%252fpasswd HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' | python3 scripts/raw_request.py"
    echo "-- self-canceling traversal that would land back in-bounds (still blocked, on purpose) --"
    run bash -c "printf 'GET /listing/../listing/a.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' | python3 scripts/raw_request.py"
    echo "-- traversal via CGI PATH_INFO, past a real script --"
    run bash -c "printf 'GET /cgi-bin/hello.py/../../../../etc/passwd HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' | python3 scripts/raw_request.py"
    echo "-- traversal on DELETE, not just GET --"
    run bash -c "printf 'DELETE /upload/../../../../etc/passwd HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' | python3 scripts/raw_request.py"
    echo "-- permission-denied: a real file, chmod 000 (reverted right after) --"
    chmod 000 www/listing/a.txt
    run curl -si "$BASE1/listing/a.txt"
    chmod 644 www/listing/a.txt
    echo "-- permission-denied: a real CGI script, chmod 000 (reverted right after) --"
    chmod 000 www/cgi-bin/hello.py
    run curl -si "$BASE1/cgi-bin/hello.py"
    chmod 644 www/cgi-bin/hello.py
    expected "Every traversal attempt -- plain, encoded, from a subdirectory, self-canceling, through CGI PATH_INFO, on DELETE -- is 403, verified on a raw socket so curl's own URL normalization can't hide a false pass. The double-encoded one is deliberately 404, not 403: single-decode means %252f never becomes a real '/', so there's no segment to catch -- proving there's no double-decode bypass, not exposing one. Permission-denied files/scripts are 403, not a crash or a silent 200."
}

demo_sneaky_400() {
    section "Sneaky 400s: malformed and ambiguous requests"
    ours
    echo "-- null byte smuggled into the path --"
    run curl -si "$BASE1/index.html%00.jpg"
    echo "-- two DIFFERENT Host headers on one request --"
    run bash -c "printf 'GET / HTTP/1.1\r\nHost: localhost\r\nHost: evil.example.com\r\n\r\n' | python3 scripts/raw_request.py"
    echo "-- two IDENTICAL Host headers (RFC 7230 5.4: any duplicate is invalid, not just a disagreement) --"
    run bash -c "printf 'GET / HTTP/1.1\r\nHost: localhost\r\nHost: localhost\r\n\r\n' | python3 scripts/raw_request.py"
    echo "-- whitespace between a header name and its colon (RFC 7230 3.2.4 smuggling guard) --"
    run bash -c "printf 'POST /upload/x HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding : chunked\r\n\r\n0\r\n\r\n' | python3 scripts/raw_request.py"
    echo "-- two headers that collide into the same CGI env var (X-Foo / X_Foo -> both HTTP_X_FOO) --"
    run bash -c "printf 'GET /cgi-bin/echo_header.py HTTP/1.1\r\nHost: localhost\r\nX-Foo: from-dash\r\nX_Foo: from-underscore\r\n\r\n' | python3 scripts/raw_request.py"
    echo "-- negative chunk size --"
    run bash -c "printf 'POST /upload/x HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n-1\r\n\r\n' | python3 scripts/raw_request.py"
    echo "-- chunk-size hex value that overflows strtol (used to hang silently instead of failing) --"
    run bash -c "printf 'POST /upload/x HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\nffffffffffffffff\r\n' | python3 scripts/raw_request.py"
    expected "Every one of these looks almost fine -- a stray null byte, a duplicate header, one extra space, two header names differing only by '-' vs '_' -- and every one is 400: not silently accepted, not ambiguously resolved by picking a winner, not a multi-second hang waiting for data that will never arrive."
}

demo_sneaky_502() {
    section "Sneaky 502s: CGI failures that don't look like failures"
    ours
    echo "-- CGI dies by signal before writing anything --"
    run curl -si "$BASE1/cgi-bin/crash_silent.py"
    echo "-- CGI writes valid headers + a partial body, THEN dies by signal (the sneaky one) --"
    run curl -si "$BASE1/cgi-bin/crash_partial.py"
    echo "-- request for a CGI script that doesn't exist on disk at all --"
    run curl -si "$BASE1/cgi-bin/does-not-exist.py"
    expected "crash_silent.py and crash_partial.py die the exact same way (SIGABRT) -- the only difference is crash_partial.py manages to flush a plausible-looking response first. A signal kill is always fatal regardless of what it already wrote, so both are 502; before that fix, crash_partial.py looked like a normal, if short, 200. A CGI extension whose script file is simply missing also 502s rather than silently returning an empty 200."
}

demo_sneaky_405() {
    section "Sneaky 405s: PUT/OPTIONS/PATCH never allowed, anywhere"
    ours
    run curl -si -X PUT --data "x" "$BASE1/index.html"
    run curl -si -X OPTIONS "$BASE1/"
    run curl -si -X PATCH --data "x" "$BASE1/upload/x"
    expected "PUT/OPTIONS/PATCH are real HTTP methods the subject never asks for, so they're rejected unconditionally -- not just because no location's config happens to allow them today, but because the server has no real handling for them at all (they'd otherwise silently fall through to GET-like static-file serving, body ignored). Allow: never lists them either, even if a config mistakenly did."
}

demo_sneaky_cgi_response() {
    section "Sneaky CGI response hygiene"
    ours
    echo "-- CGI script sets its OWN (wrong) Content-Length header --"
    run bash -c "printf 'GET /cgi-bin/lie_length.py HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' | python3 scripts/raw_request.py"
    echo "-- sanity check: a single X-Foo header (no collision) still reaches the CGI normally --"
    run curl -s -H "X-Foo: only-one" "$BASE1/cgi-bin/echo_header.py"
    expected "lie_length.py's own bogus Content-Length: 999999 is dropped -- only the server's correctly-computed Content-Length (matching the real 2-byte body) makes it onto the wire, not both. The single-header sanity check confirms the earlier 400 (two colliding headers) isn't refusing ALL custom headers -- one on its own still passes straight through to the CGI environment."
}

# ---- menu -------------------------------------------------------------------

print_menu() {
    printf "\n${BLUE}Webserv feature demo -- pick one:${RESET}\n"
    cat <<'EOF'
 1) Multiple servers, different ports
 2) Custom error page
 3) Client body size limit
 4) Routing to separate directories
 5) Directory index file
 6) Per-route allowed methods
 7) Upload, download, and delete a file
 8) Unknown HTTP methods
 9) CGI execution and environment
10) CGI script with a Python syntax error
11) Concurrent clients don't block each other
12) Process health check
13) Sneaky 403s: path traversal & permission-denied
14) Sneaky 400s: malformed and ambiguous requests
15) Sneaky 502s: CGI failures that don't look like failures
16) Sneaky 405s: PUT/OPTIONS/PATCH never allowed
17) Sneaky CGI response hygiene
 a) Run everything
 q) Quit
EOF
    printf "> "
}

initial_check

while true; do
    print_menu
    read -r choice
    case "$choice" in
        q|Q) echo "Bye."; exit 0 ;;
        a|A) check_running && run_all ;;
        1) check_running && demo_multi_port ;;
        2) check_running && demo_error_page ;;
        3) check_running && demo_body_limit ;;
        4) check_running && demo_routing ;;
        5) check_running && demo_index_fallback ;;
        6) check_running && demo_methods ;;
        7) check_running && demo_upload_cycle ;;
        8) check_running && demo_unknown_method ;;
        9) check_running && demo_cgi ;;
        10) check_running && demo_cgi_syntax_error ;;
        11) check_running && demo_concurrency ;;
        12) check_running && demo_health ;;
        13) check_running && demo_sneaky_403 ;;
        14) check_running && demo_sneaky_400 ;;
        15) check_running && demo_sneaky_502 ;;
        16) check_running && demo_sneaky_405 ;;
        17) check_running && demo_sneaky_cgi_response ;;
        "") ;;
        *) echo "Unknown choice: $choice" ;;
    esac
done
