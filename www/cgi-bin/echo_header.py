#!/usr/bin/env python3
# Echoes back what the CGI environment actually received for HTTP_X_FOO --
# used to show that two distinct client headers whose names collide after
# RFC 3875's '-' -> '_' env-var mapping (X-Foo and X_Foo both becoming
# HTTP_X_FOO) are now rejected at parse time (400) instead of silently
# picking a winner.
import os
import sys

sys.stdout.write("Content-Type: text/plain\r\n\r\n")
sys.stdout.write("HTTP_X_FOO=" + os.environ.get("HTTP_X_FOO", "<missing>") + "\n")
