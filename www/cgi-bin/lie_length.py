#!/usr/bin/env python3
# Sets its own (wrong) Content-Length header alongside a short real body.
# Before CgiHandler.cpp's finishFromCgiOutput() dropped script-supplied
# Content-Length/Date/Server/Connection headers, this landed on the wire as
# TWO Content-Length headers -- a real framing violation, not just a
# cosmetic one, since a proxy or browser isn't guaranteed to resolve the
# conflict the same way curl silently did.
import sys

sys.stdout.write("Content-Type: text/plain\r\nContent-Length: 999999\r\n\r\nhi")
