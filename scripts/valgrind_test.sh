#!/usr/bin/env bash
# Runs the full eval_demo.sh suite against a valgrind-wrapped webserv, then
# reports the leak summary. This is the actual check the subject asks for
# ("you must verify absence of memory leaks... any memory allocated on the
# heap must be properly freed") -- RSS-watching (see siege_test.sh) is a
# useful proxy under real load, but this is the real, tool-verified answer.
#
# Non-interactive by design: it starts webserv itself (under valgrind),
# drives every scenario in eval_demo.sh's "run everything", shuts the
# server down cleanly with SIGINT so its destructors actually run (a
# SIGKILL'd process never reaches the "no leaks possible" report -- the
# leak summary would just be wrong), then prints what valgrind found.
#
# Usage: ./scripts/valgrind_test.sh [config file]
#   defaults to conf/test.conf, matching eval_demo.sh's own assumptions
#   (ports 8080/8081) -- pass another config if you need different ports.

set -u

GREEN='\033[1;32m'
BLUE='\033[1;34m'
YELLOW='\033[1;33m'
RED='\033[1;31m'
RESET='\033[0m'

cd "$(dirname "$0")/.." || exit 1

CONF="${1:-conf/test.conf}"
LOGFILE="$(mktemp /tmp/webserv_valgrind_XXXXXX.log)"
STDOUT_LOG="$(mktemp /tmp/webserv_valgrind_stdout_XXXXXX.log)"
DEMO_LOG="$(mktemp /tmp/eval_demo_under_valgrind_XXXXXX.log)"
VG_PID=""

cleanup() {
    if [ -n "$VG_PID" ] && kill -0 "$VG_PID" 2>/dev/null; then
        kill -TERM "$VG_PID" 2>/dev/null
    fi
}
trap cleanup INT TERM

if ! command -v valgrind >/dev/null 2>&1; then
    printf "${RED}valgrind isn't installed.${RESET}\n"
    exit 1
fi
if [ ! -x ./webserv ]; then
    printf "${RED}./webserv isn't built -- run 'make' first.${RESET}\n"
    exit 1
fi
if [ ! -f "$CONF" ]; then
    printf "${RED}config file not found: %s${RESET}\n" "$CONF"
    exit 1
fi
if pgrep -x webserv >/dev/null 2>&1; then
    printf "${RED}a webserv instance is already running -- stop it first (this script needs its ports free).${RESET}\n"
    exit 1
fi

printf "${BLUE}=== Starting webserv under valgrind (%s) ===${RESET}\n" "$CONF"
printf "valgrind log: %s\n" "$LOGFILE"
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
    --log-file="$LOGFILE" ./webserv "$CONF" > "$STDOUT_LOG" 2>&1 &
VG_PID=$!

# valgrind adds heavy instrumentation overhead -- give it real time to bind
# the listening sockets and start actually polling before hammering it.
tries=0
until curl -s -o /dev/null --max-time 1 http://127.0.0.1:8080/ 2>/dev/null; do
    tries=$((tries + 1))
    if [ "$tries" -ge 30 ]; then
        printf "${RED}webserv never came up under valgrind after 30s -- see %s${RESET}\n" "$STDOUT_LOG"
        kill -TERM "$VG_PID" 2>/dev/null
        exit 1
    fi
    if ! kill -0 "$VG_PID" 2>/dev/null; then
        printf "${RED}valgrind/webserv exited during startup -- see %s${RESET}\n" "$STDOUT_LOG"
        exit 1
    fi
    sleep 1
done
printf "${GREEN}up (took ~%ss to bind under instrumentation) -- exercising the full test suite${RESET}\n\n" "$tries"

printf "a\nq\n" | ./scripts/eval_demo.sh > "$DEMO_LOG" 2>&1
printf "${BLUE}=== eval_demo.sh 'run everything' finished (log: %s) ===${RESET}\n\n" "$DEMO_LOG"

printf "${BLUE}=== Shutting down cleanly (SIGINT, so destructors run and the leak report means something) ===${RESET}\n"
kill -INT "$VG_PID" 2>/dev/null

tries=0
while kill -0 "$VG_PID" 2>/dev/null; do
    tries=$((tries + 1))
    if [ "$tries" -ge 30 ]; then
        printf "${RED}valgrind didn't exit within 30s of SIGINT -- forcing it, the report below may be incomplete.${RESET}\n"
        kill -KILL "$VG_PID" 2>/dev/null
        break
    fi
    sleep 1
done
trap - INT TERM

printf "\n${BLUE}=== Leak summary (every process report found in the log -- the main server, plus any CGI fork-children valgrind glimpsed before their execve()) ===${RESET}\n"
# Two distinct valgrind phrasings show up here: a perfectly clean process
# just says "All heap blocks were freed" (no itemized LEAK SUMMARY block
# at all, since there's nothing to itemize); anything with reachable/lost
# bytes gets the full LEAK SUMMARY block. Catch both, or the perfectly
# clean case -- which is what the main server process should report --
# silently goes missing from this display.
awk '/LEAK SUMMARY/{show=6} show{print; show--} /blocks were freed/{print}' "$LOGFILE"
echo
printf "${BLUE}=== Error summaries ===${RESET}\n"
grep "ERROR SUMMARY" "$LOGFILE"
echo

BAD_LEAKS=$(grep -o "definitely lost: [0-9,]* bytes" "$LOGFILE" | grep -vc "definitely lost: 0 bytes")
BAD_INDIRECT=$(grep -o "indirectly lost: [0-9,]* bytes" "$LOGFILE" | grep -vc "indirectly lost: 0 bytes")
BAD_ERRORS=$(grep -o "ERROR SUMMARY: [0-9]* errors" "$LOGFILE" | grep -vc "ERROR SUMMARY: 0 errors")

if [ "$BAD_LEAKS" -eq 0 ] && [ "$BAD_INDIRECT" -eq 0 ] && [ "$BAD_ERRORS" -eq 0 ]; then
    printf "${GREEN}PASS -- no definitely/indirectly lost bytes, no memcheck errors, anywhere in the log.${RESET}\n"
    printf "(\"still reachable\" bytes, if any, are expected: CGI children valgrind glimpses for a few\n"
    printf " instructions between fork() and execve() before their process image is replaced.)\n"
else
    printf "${RED}FAIL -- see the full report for details.${RESET}\n"
fi
printf "\nFull report: %s\n" "$LOGFILE"
