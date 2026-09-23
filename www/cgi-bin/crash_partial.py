#!/usr/bin/env python3
# Writes valid headers plus a partial body, THEN dies by signal (SIGABRT).
# Before CgiHandler.cpp's finish() treated any signal kill as fatal
# regardless of output, this produced a clean 200 OK with a silently
# truncated body -- a crash indistinguishable from a short-but-legitimate
# response. Exercises the WIFSIGNALED branch with non-empty output.
import sys
import os

sys.stdout.write("Content-Type: text/html\r\n\r\n<html><body>partial output before crash")
sys.stdout.flush()
os.abort()
