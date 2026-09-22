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
    demo_concurrency
    demo_health
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
10) Concurrent clients don't block each other
11) Process health check
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
        10) check_running && demo_concurrency ;;
        11) check_running && demo_health ;;
        "") ;;
        *) echo "Unknown choice: $choice" ;;
    esac
done
