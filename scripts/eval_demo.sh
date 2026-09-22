#!/usr/bin/env bash
# Live-demo script for the Webserv defense.
#
# Exercises an ALREADY-RUNNING `./webserv conf/test.conf` and prints every
# command it runs followed by the real output -- it never starts, stops, or
# edits anything itself. Run this in a second terminal, e.g.:
#
#   ./webserv conf/test.conf &
#   ./scripts/eval_demo.sh
#
# Each section is labelled with the eval-sheet question it answers.

BASE1="http://127.0.0.1:8080"
BASE2="http://127.0.0.1:8081"

GREEN='\033[1;32m'
BLUE='\033[1;34m'
RESET='\033[0m'

section() {
    printf "\n${BLUE}=== %s ===${RESET}\n" "$1"
}

run() {
    printf "${GREEN}\$ %s${RESET}\n" "$*"
    "$@"
    echo
}

section "Precondition: is webserv actually running?"
if ! curl -s -o /dev/null --max-time 2 "$BASE1/"; then
    echo "webserv is not answering on $BASE1 -- launch it first, e.g.:"
    echo "    ./webserv conf/test.conf"
    exit 1
fi
echo "OK -- $BASE1 is up, proceeding."

section 'Q: "setup multiple servers with different port"'
run curl -s "$BASE1/"
run curl -s "$BASE2/"

section 'Q: "setup default error page (try to change the error 404)"'
run curl -si "$BASE1/does-not-exist"

section 'Q: "limit the client body" (:8081 has client_max_body_size 10)'
run curl -s -X POST -H "Content-Type: plain/text" --data "this-is-twenty-bytes" "$BASE2/"
run curl -s -X POST -H "Content-Type: plain/text" --data "hello" "$BASE2/"

section 'Q: "setup routes in a server to different directories"'
run curl -s "$BASE1/"
run curl -s "$BASE1/listing/"

section 'Q: "setup a default file to search for if you ask for a directory"'
run curl -si "$BASE1/"

section 'Q: "setup a list of method accepted for a certain route"'
run curl -si -X DELETE "$BASE1/index.html"

section 'Q: "GET/POST/DELETE requests should work" + "upload some file and get it back"'
run curl -si -X POST --data-binary "evaluation demo content" "$BASE1/upload/eval_demo.txt"
run curl -s "$BASE1/upload/eval_demo.txt"
run curl -si -X DELETE "$BASE1/upload/eval_demo.txt"
run curl -si "$BASE1/upload/eval_demo.txt"

section 'Q: "UNKNOWN requests -> should not produce any crash"'
run curl -si -X FOOBAR "$BASE1/"
run curl -s "$BASE1/"

section 'Q: CGI environment + "the status code must be good"'
run curl -s "$BASE1/cgi-bin/hello.py"
run curl -s -X POST --data "field=value" "$BASE1/cgi-bin/echo.py"
run curl -si "$BASE1/cgi-bin/anything.broken"

section 'Q: "only one read or one write per client per select" -- CGI pipes share it too'
echo "\$ curl -s -o /dev/null -w \"hang.py: %{http_code} in %{time_total}s\\n\" $BASE1/cgi-bin/hang.py &"
curl -s -o /dev/null -w "hang.py: %{http_code} in %{time_total}s\n" "$BASE1/cgi-bin/hang.py" &
sleep 1
run curl -s -o /dev/null -w "concurrent GET /: %{http_code} in %{time_total}s\n" "$BASE1/"
echo "(waiting up to ~10s for hang.py's CGI deadline to fire...)"
wait

section "Health check"
run bash -c "ps aux | grep '[w]ebserv'"

echo
echo "Done."
