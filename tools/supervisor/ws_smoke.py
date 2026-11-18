#!/usr/bin/env python3
"""Smoke test for the supervisor's websocket terminal bridge.
Speaks just enough RFC6455 client-side to validate the daemon's server side:
handshake -> send a command -> expect the command's output back -> close.
Usage: python ws_smoke.py <instance-port>   (instance must be running)"""
import base64
import hashlib
import os
import socket
import struct
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7918


def send_frame(s, payload, opcode=0x1):
    mask = os.urandom(4)
    header = bytes([0x80 | opcode])
    n = len(payload)
    if n < 126:
        header += bytes([0x80 | n])
    elif n < 65536:
        header += bytes([0x80 | 126]) + struct.pack(">H", n)
    else:
        header += bytes([0x80 | 127]) + struct.pack(">Q", n)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    s.sendall(header + mask + masked)


def read_exact(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(min(4096, n - len(buf)))
        if not chunk:
            raise ConnectionError("eof")
        buf += chunk
    return buf


def read_frame(s):
    hdr = read_exact(s, 2)
    opcode = hdr[0] & 0x0F
    ln = hdr[1] & 0x7F
    if ln == 126:
        ln = struct.unpack(">H", read_exact(s, 2))[0]
    elif ln == 127:
        ln = struct.unpack(">Q", read_exact(s, 8))[0]
    return opcode, read_exact(s, ln)


s = socket.create_connection(("127.0.0.1", 7911), timeout=15)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall((
    "GET /ws/%d HTTP/1.1\r\n"
    "Host: 127.0.0.1:7911\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: %s\r\n"
    "Sec-WebSocket-Version: 13\r\n\r\n" % (PORT, key)).encode())
resp = b""
while b"\r\n\r\n" not in resp:
    resp += s.recv(4096)
head, _, rest = resp.partition(b"\r\n\r\n")
assert b"101" in head.split(b"\r\n")[0], head
expect = base64.b64encode(hashlib.sha1(
    (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
assert expect.encode() in head, "accept key mismatch"
print("handshake ok")
buf = rest

send_frame(s, b"echo ws-bridge-$((41+1))\n")
seen = b""
import time
deadline = time.time() + 20
while time.time() < deadline and b"ws-bridge-42" not in seen:
    try:
        opcode, payload = read_frame(s) if buf else read_frame(s)
        buf = b""
        if opcode in (0x1, 0x2):
            seen += payload
    except (ConnectionError, socket.timeout):
        break
print("received %d bytes: %r" % (len(seen), seen))
ok = b"ws-bridge-42" in seen
print("WS-BRIDGE-OK" if ok else "WS-BRIDGE-FAIL")
send_frame(s, b"", 0x8)
s.close()
sys.exit(0 if ok else 1)
