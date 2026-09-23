#!/usr/bin/env python3
# Deliberately invalid Python syntax, for demonstrating how webserv
# handles a CGI script that fails before printing anything: python3
# exits 1 (not the execve-failure 127 that anything.broken exercises)
# and writes its traceback to stderr, so the CGI pipe's stdout is empty.
print("Content-Type: text/html"
print("this line is missing a closing paren above, and this one is bad too" +)
