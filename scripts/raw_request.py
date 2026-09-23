#!/usr/bin/env python3
"""Sends raw bytes (stdin) straight to a TCP socket and prints the raw
response, bypassing curl's own URL/header normalization entirely.

Used by eval_demo.sh's sneaky-test section for requests curl can't
faithfully send at all -- curl collapses literal ".." path segments before
the request ever leaves the client, and there's no curl flag for "send a
second Host header" or "put a space before this header's colon". Talking
straight to the socket is the only way to prove what the server itself
actually does with bytes like that on the wire.

Usage: printf '...' | python3 raw_request.py [port]
"""
import socket
import sys


def main():
    port = 8080
    if len(sys.argv) > 1 and sys.argv[1].isdigit():
        port = int(sys.argv[1])
    payload = sys.stdin.buffer.read()
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    sock.sendall(payload)
    sock.settimeout(2)
    data = b""
    try:
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        pass
    sock.close()
    if data:
        sys.stdout.buffer.write(data)
    else:
        sys.stdout.write("(no response -- connection held open / timed out)\n")


if __name__ == "__main__":
    main()
