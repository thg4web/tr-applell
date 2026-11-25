#!/usr/bin/env python3
"""Post a PRG to the TR+ SD card, then (optionally) launch it. STATE-CHANGING: it writes to the SD card.
Safe-by-design: reads exactly 2-byte replies, sends the next stage ONLY after a clean Ack (cc 64),
and stops (closing the socket) at the first anything else. Never retry blindly: leftover bytes
get parsed as commands (one of which resets the saved settings).
Usage: launch.py post|launch FILE_OR_NONE /sd/path/NAME.prg [host]"""
import os, socket, sys, time
ACK = b"\xcc\x64"
mode, f, dest = sys.argv[1], sys.argv[2], sys.argv[3]
host = sys.argv[4] if len(sys.argv) > 4 else (os.environ.get("TR_HOST") or sys.exit("give the TeensyROM's address as the last argument or in TR_HOST"))
s = socket.create_connection((host, 2112), timeout=5)
def drain():
    s.settimeout(0.5)
    try:
        d = s.recv(4096)
    except socket.timeout:
        d = b""
    return d
def rd(n, to=10):
    s.settimeout(to); d = b""
    while len(d) < n:
        c = s.recv(n - len(d))
        if not c: break
        d += c
    return d
def stop(msg, got):
    try: extra = drain()
    except Exception: extra = b""
    print("STOP:", msg, "got", got.hex(), "extra", extra[:200]); s.close(); sys.exit(1)
d = drain()
if d: stop("unexpected data on connect", d)
if mode == "post":
    data = open(f, "rb").read()
    s.sendall((0x64BB).to_bytes(2, "little" if False else "big")); a = rd(2)
    if a != ACK: stop("no token ack", a)
    s.sendall(len(data).to_bytes(4, "big") + (sum(data) & 0xFFFF).to_bytes(2, "big") + b"\x01" + dest.encode() + b"\0")
    a = rd(2)
    if a != ACK: stop("header rejected", a)
    s.sendall(data); a = rd(2, max(20, len(data) // 8000))   # big files: about 39 KB/s, so wait in proportion
    if a != ACK: stop("file rejected", a)
    print("posted", len(data), "bytes to SD", dest)
else:
    s.sendall((0x6444).to_bytes(2, "big")); a = rd(2)
    if a != ACK: stop("no launch token ack", a)
    s.sendall(b"\x01" + dest.encode() + b"\0"); a = rd(2, 20)
    if a != ACK: stop("launch failed", a)
    print("launch ack")
s.close()
