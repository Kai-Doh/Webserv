#!/usr/bin/env python3
# Dies by signal (SIGABRT) before writing anything at all -- exercises the
# WIFSIGNALED branch of CgiHandler.cpp's finish() with empty output.
import os
os.abort()
