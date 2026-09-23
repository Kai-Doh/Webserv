#!/usr/bin/env bash
# Interactive siege-based stress test menu for the Webserv HTTP/CGI half.
#
# Exercises an ALREADY-RUNNING `./webserv conf/test.conf` -- like
# eval_demo.sh, it never starts, stops, or edits anything itself, and it
# deliberately does NOT run under valgrind (siege wants the server's real,
# non-instrumented performance; use valgrind_test.sh separately for leak
# verification). Run this in a second terminal:
#
#   ./webserv conf/test.conf &
#   ./scripts/siege_test.sh
#
# Covers what both the subject ("stress test your server to ensure it
# remains available") and the eval sheet's "Siege & stress test" section
# ask for: >99.5% availability on a plain GET, no indefinite memory growth,
# no hanging connections, and the server surviving repeated siege runs
# without a restart.

BASE1="http://127.0.0.1:8080"
URLS_FILE="scripts/siege_urls.txt"

GREEN='\033[1;32m'
BLUE='\033[1;34m'
YELLOW='\033[1;33m'
CYAN='\033[1;36m'
RED='\033[1;31m'
RESET='\033[0m'

cd "$(dirname "$0")/.." || exit 1

section() {
    printf "\n${BLUE}=== %s ===${RESET}\n" "$1"
}

ours() {
    printf "${CYAN}-- ours --${RESET}\n"
}

expected() {
    printf "${YELLOW}-- expected --${RESET}\n%s\n" "$1"
}

initial_check() {
    if ! command -v siege >/dev/null 2>&1; then
        echo "siege isn't installed."
        exit 1
    fi
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

server_pid() {
    pgrep -x webserv | head -1
}

rss_kb() {
    local pid="$1"
    ps -o rss= -p "$pid" 2>/dev/null | tr -d ' '
}

fd_count() {
    local pid="$1"
    ls "/proc/$pid/fd" 2>/dev/null | wc -l | tr -d ' '
}

# ---- siege scenarios --------------------------------------------------------

siege_availability() {
    section "Availability check (eval sheet's literal ask: >99.5% on a plain GET)"
    ours
    echo "\$ siege -b -t15S -c10 $BASE1/listing/a.txt"
    siege -b -t15S -c10 "$BASE1/listing/a.txt" 2>&1 | grep -E "Transactions|Availability|Elapsed|Response time|Transaction rate|Concurrency|Successful|Failed|Longest|Shortest"
    expected "Availability should read 100.00% (or at least >99.5% -- anything less on a plain static GET under a 15s/10-concurrent benchmark means requests are being dropped or timing out, which shouldn't happen on a single non-blocking poll() loop)."
}

siege_memory_growth() {
    section "Memory-growth watch (no leak should show up as indefinite RSS climb under sustained load)"
    ours
    local pid
    pid=$(server_pid)
    if [ -z "$pid" ]; then
        printf "${YELLOW}couldn't find the webserv process (needed to sample RSS) -- skipping.${RESET}\n"
        return
    fi
    echo "webserv pid: $pid"
    echo "RSS before: $(rss_kb "$pid") KB"
    echo "\$ siege -b -t30S -c15 $BASE1/listing/a.txt"
    siege -b -t30S -c15 "$BASE1/listing/a.txt" 2>&1 | grep -E "Transactions|Availability|Failed"
    echo "RSS after:  $(rss_kb "$pid") KB"
    expected "RSS after 30s of sustained concurrent load should be close to RSS before -- a few dozen KB of allocator overhead is normal, hundreds of MB or a value that keeps climbing on repeated runs is not. This is a proxy; scripts/valgrind_test.sh gives the actual tool-verified answer."
}

siege_mixed_urls() {
    section "Mixed realistic load (static files, CGI, a 404, a redirect, all in one run)"
    ours
    if [ ! -f "$URLS_FILE" ]; then
        printf "${YELLOW}%s not found -- skipping.${RESET}\n" "$URLS_FILE"
        return
    fi
    echo "\$ siege -b -t20S -c10 -f $URLS_FILE"
    siege -b -t20S -c10 -f "$URLS_FILE" 2>&1 | grep -E "Transactions|Availability|Elapsed|Failed|Longest|Shortest"
    expected "Same non-blocking poll() loop handling a realistic mix -- static files, a CGI script (real fork+exec per hit), a 404, and a 301 redirect -- all interleaved under load, not just one cheap static file repeated. Availability should still be at or near 100%."
}

siege_concurrency_burst() {
    section "High-concurrency burst + hanging-connection check"
    ours
    local pid fds_before fds_after
    pid=$(server_pid)
    if [ -z "$pid" ]; then
        printf "${YELLOW}couldn't find the webserv process (needed to count fds) -- skipping.${RESET}\n"
        return
    fi
    fds_before=$(fd_count "$pid")
    echo "open fds before: $fds_before"
    echo "\$ siege -b -c50 -r20 $BASE1/listing/a.txt"
    siege -b -c50 -r20 "$BASE1/listing/a.txt" 2>&1 | grep -E "Transactions|Availability|Failed|Concurrency"
    sleep 1
    fds_after=$(fd_count "$pid")
    echo "open fds after:  $fds_after"
    expected "50 concurrent clients x 20 reps each = 1000 requests fired at once. Once siege finishes, open fds should settle back down near the pre-burst count -- a persistently high or climbing fd count means connections are being accepted but never fully closed, i.e. a hang."
}

siege_cgi_focus() {
    section "CGI-focused load (fork+exec is the expensive path, worth isolating from static files)"
    ours
    local pid
    pid=$(server_pid)
    echo "\$ siege -b -t20S -c10 $BASE1/cgi-bin/hello.py"
    siege -b -t20S -c10 "$BASE1/cgi-bin/hello.py" 2>&1 | grep -E "Transactions|Availability|Failed|Longest|Shortest"
    if [ -n "$pid" ]; then
        echo "child/zombie processes still attached to webserv: $(pgrep -P "$pid" | wc -l | tr -d ' ')"
    fi
    expected "Every hit here is a real fork()+execve() of python3, not a cheap static read -- availability should still be high, and the child-process count afterward should be 0 (every CGI child reaped, no zombies left over from a burst of concurrent forks)."
}

run_all() {
    siege_availability
    siege_memory_growth
    siege_mixed_urls
    siege_concurrency_burst
    siege_cgi_focus
    section "Process health check"
    ours
    ps aux | grep '[w]ebserv'
    expected "Exactly one webserv process, still alive after every scenario above -- this is the eval sheet's 'you should be able to use siege indefinitely without restarting the server' check."
}

# ---- menu -------------------------------------------------------------------

print_menu() {
    printf "\n${BLUE}Webserv siege stress-test menu -- pick one:${RESET}\n"
    cat <<'EOF'
1) Availability check (>99.5% on a plain GET)
2) Memory-growth watch (RSS before/after sustained load)
3) Mixed realistic load (static + CGI + 404 + redirect)
4) High-concurrency burst + hanging-connection check
5) CGI-focused load (fork/exec path, zombie check)
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
        1) check_running && siege_availability ;;
        2) check_running && siege_memory_growth ;;
        3) check_running && siege_mixed_urls ;;
        4) check_running && siege_concurrency_burst ;;
        5) check_running && siege_cgi_focus ;;
        "") ;;
        *) echo "Unknown choice: $choice" ;;
    esac
done
