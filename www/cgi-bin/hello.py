#!/usr/bin/env python3
import os

print("Content-Type: text/html")
print("Status: 200 OK")
print()
print("<html><body>")
print("<h1>Hello from CGI</h1>")
print("<p>REQUEST_METHOD = %s</p>" % os.environ.get("REQUEST_METHOD", ""))
print("<p>QUERY_STRING = %s</p>" % os.environ.get("QUERY_STRING", ""))
print("<p>SCRIPT_NAME = %s</p>" % os.environ.get("SCRIPT_NAME", ""))
print("<p>SERVER_PROTOCOL = %s</p>" % os.environ.get("SERVER_PROTOCOL", ""))
print("</body></html>")
