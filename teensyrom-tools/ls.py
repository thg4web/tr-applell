#!/usr/bin/env python3
"""List a directory on the TR+ SD card (read-only). Strict: parameters are sent only after a clean Ack.
Usage: ls.py /sd/path [host]"""
import json, socket, sys, time
path = sys.argv[1]; host = sys.argv[2] if len(sys.argv) > 2 else (os.environ.get("TR_HOST") or sys.exit("give the TeensyROM's address as the last argument or in TR_HOST"))
ACK = b"\xcc\x64"
s = socket.create_connection((host, 2112), timeout=5)
def rd(n, to=8):
    s.settimeout(to); d = b""
    while len(d) < n:
        c = s.recv(n - len(d))
        if not c: break
        d += c
    return d
s.sendall((0x64DE).to_bytes(2, "big"))
if rd(2) != ACK: sys.exit("no token ack (is the TeensyROM menu idle? file commands return Busy! while a program runs)")
s.sendall(b"\x01" + (0).to_bytes(2, "big") + (500).to_bytes(2, "big") + path.encode() + b"\0")
if rd(2) != ACK: sys.exit("directory refused")
if rd(2) != b"\x5a\x5a": sys.exit("no start marker")
buf = b""; s.settimeout(5)
while not buf.endswith(b"\xa5\xa5"):
    c = s.recv(4096)
    if not c: break
    buf += c
txt = buf[:-2].decode("latin-1")
for line in txt.splitlines():
    line = line.strip()
    if line:
        try: print(json.dumps(json.loads(line)))
        except Exception: print("raw:", line)
