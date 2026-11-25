#!/usr/bin/env python3
"""Read-only TeensyROM+ TCP client (port 2112). Use ONE persistent connection.
Usage: tr.py [host]   -> FWCheck, Ping, VersionInfo, then screen RAM read."""
import socket, sys, time
HOST = sys.argv[1] if len(sys.argv) > 1 else (os.environ.get("TR_HOST") or sys.exit("give the TeensyROM's address as the last argument or in TR_HOST"))
s = socket.create_connection((HOST, 2112), timeout=3); s.settimeout(1)
def rx(n=None, limit=4):
    d = b""; t = time.time()
    while time.time() - t < limit:
        try:
            c = s.recv(4096)
            if not c: break
            d += c
            if n and len(d) >= n: break
        except socket.timeout:
            if d and not n: break
    return d
def cmd(tok, extra=b"", n=None):
    s.sendall(tok.to_bytes(2, "big") + extra); return rx(n)
print("FWCheck", cmd(0x64E0, n=2).hex())
print("Ping", cmd(0x6455))
print("Version", cmd(0x6476).decode("latin-1"))
def read(addr, n):
    d = cmd(0x64FD, addr.to_bytes(2, "big") + n.to_bytes(2, "big"), n=n + 2)
    return d[:2], d[2:]
ack, scr = read(0x0400, 1000)
print("ReadC64Mem ack", ack.hex(), "len", len(scr))
sc2asc = lambda b: chr(b + 64) if b < 32 else chr(b) if b < 64 else "."
for r in range(25): print("".join(sc2asc(b) for b in scr[r*40:(r+1)*40]))
print("$D020", read(0xD020, 1)[1].hex())
