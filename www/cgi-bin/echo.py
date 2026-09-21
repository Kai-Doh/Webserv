#!/usr/bin/env python3
import os
import sys

length = int(os.environ.get("CONTENT_LENGTH", "0") or "0")
body = sys.stdin.read(length) if length > 0 else sys.stdin.read()

print("Content-Type: text/plain")
print()
print("method=%s" % os.environ.get("REQUEST_METHOD", ""))
print("content_length=%s" % os.environ.get("CONTENT_LENGTH", ""))
print("content_type=%s" % os.environ.get("CONTENT_TYPE", ""))
print("query=%s" % os.environ.get("QUERY_STRING", ""))
print("body=%s" % body)
