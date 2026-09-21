#!/usr/bin/env python3
import os
import sys
import html
from urllib.parse import parse_qs

length = int(os.environ.get("CONTENT_LENGTH", "0") or "0")
body = sys.stdin.read(length) if length > 0 else ""
fields = parse_qs(body)

name = html.escape(fields.get("name", [""])[0].strip())
email = html.escape(fields.get("email", [""])[0].strip())
message = html.escape(fields.get("message", [""])[0].strip())

print("Content-Type: text/html")
print()

if not name or not message:
    print("<strong>Missing name or message.</strong> Nothing was sent.")
else:
    print("<strong>Thanks, %s!</strong>" % name)
    if email:
        print("<p>We'll get back to you at %s.</p>" % email)
    else:
        print("<p>We'll get back to you soon.</p>")
    print("<p style=\"opacity:.75\">&ldquo;%s&rdquo;</p>" % message)
