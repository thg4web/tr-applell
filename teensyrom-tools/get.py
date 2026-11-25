#!/usr/bin/env python3
"""Fetch a file from the TR+ SD card (GetFile 0x64B0) and save it. Read-only on the device.
Strict: sends the next stage only after a clean Ack (cc 64); stops at anything else.
Usage: get.py /sd/path/NAME OUTFILE [host]"""
import socket, sys, time
ACK = b"\xcc\x64"; path, out = sys.argv[1], sys.argv[2]
host = sys.argv[3] if len(sys.argv) > 3 else (os.environ.get("TR_HOST") or sys.exit("give the TeensyROM's address as the last argument or in TR_HOST"))
s = socket.create_connection((host, 2112), timeout=5)
def rd(n, to=15):
    s.settimeout(to); d = b""
    while len(d) < n:
        c = s.recv(n - len(d))
        if not c: break
        d += c
    return d
def stop(msg, got):
    s.settimeout(0.5)
    try: extra = s.recv(200)
    except Exception: extra = b""
    print("STOP:", msg, got.hex(), extra); s.close(); sys.exit(1)
s.sendall((0x64B0).to_bytes(2, "big")); a = rd(2)
if a != ACK: stop("no token ack", a)
s.sendall(b"\x01" + path.encode() + b"\0"); a = rd(2)
if a != ACK: stop("storage/path rejected", a)
n = int.from_bytes(rd(4), "little"); cs = int.from_bytes(rd(4), "little")   # device sends little-endian (like its 2-byte acks)
if n > 16*1024*1024: stop(f"implausible length {n}", n.to_bytes(4, "little"))
t = time.time(); data = rd(n, 30); a = rd(2)
if len(data) != n or a != ACK: stop(f"short file {len(data)}/{n} or no final ack", a)
if sum(data) & 0xFFFF != cs: stop("checksum mismatch", data[:4])
open(out, "wb").write(data); print(f"got {n} bytes in {time.time()-t:.2f}s, checksum ok -> {out}")
s.close()
