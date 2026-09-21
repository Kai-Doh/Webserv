#!/usr/bin/env python3
import os

print("Content-Type: text/plain")
print()
print("SCRIPT_NAME=%s" % os.environ.get("SCRIPT_NAME", ""))
print("PATH_INFO=%s" % os.environ.get("PATH_INFO", ""))
print("REQUEST_URI=%s" % os.environ.get("REQUEST_URI", ""))
