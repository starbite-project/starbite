#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# tap-test's TCP peer: reads 8 bytes (N up, M down, big-endian), then N bytes; answers their SHA-256 and M
# pattern bytes (byte i = i*17 + (i>>11)), then closes.   bulk-serve.py ADDRESS PORT
import hashlib, socket, struct, sys, threading

def serve(c):
    try:
        h = b""
        while len(h) < 8:
            d = c.recv(8 - len(h))
            if not d:
                return
            h += d
        n, m = struct.unpack(">II", h)
        s, got = hashlib.sha256(), 0
        while got < n:
            d = c.recv(min(65536, n - got))
            if not d:
                return
            s.update(d)
            got += len(d)
        c.sendall(s.digest() + bytes((i * 17 + (i >> 11)) & 255 for i in range(m)))
    finally:
        c.close()

s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((sys.argv[1], int(sys.argv[2])))
s.listen(4)
while True:
    c, _ = s.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()
