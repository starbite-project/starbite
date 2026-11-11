#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# sbhttp's test server (host/http-test.c): crafted HTTP answers by path, plain or (--tls cert key) TLS.
#   http-serve.py PORT [--tls CERT KEY]      (SBHTTP_BIND=address: listen there instead of 127.0.0.1)
import os, socket, ssl, sys, threading, time

BIG = bytes((i * 7 + (i >> 8)) & 255 for i in range(3000000))

def answer(path):
    """(bytes to send, close afterwards?, hold seconds before closing)"""
    if path == "/length":
        return b"HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nhello, length", 3.0
    if path == "/chunked":
        return (b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                b"7;ext=1\r\nhello, \r\nD\r\nchunked world\r\n0\r\nX-Trailer: y\r\n\r\n"), 3.0
    if path == "/eof":
        return b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nto the end", 0
    if path == "/continue":
        return (b"HTTP/1.1 100 Continue\r\n\r\n"
                b"HTTP/1.1 200 OK\r\nContent-Length: 14\r\n\r\nafter continue"), 3.0
    if path == "/redirect":
        return b"HTTP/1.1 302 Found\r\nLocation: /length\r\nContent-Length: 0\r\n\r\n", 3.0
    if path == "/redirect-loop":
        return b"HTTP/1.1 302 Found\r\nLocation: /redirect-loop\r\nContent-Length: 0\r\n\r\n", 3.0
    if path == "/no-content":
        return b"HTTP/1.1 204 No Content\r\n\r\n", 3.0
    if path == "/big":
        return b"HTTP/1.1 200 OK\r\nContent-Length: 3000000\r\n\r\n" + BIG, 0
    if path == "/bad-status":
        return b"HTTP/1.1 2x0 OK\r\n\r\n", 0
    if path == "/bad-chunk":
        return b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", 3.0
    if path == "/two-lengths":
        return b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello", 0
    if path == "/short":
        return b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n0123456789", 0
    if path == "/huge-head":
        return b"HTTP/1.1 200 OK\r\n" + b"X-Filler: " + b"a" * 9000 + b"\r\n\r\n", 0
    if path == "/silent":
        return b"", 30.0
    return b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n", 0

def serve(conn):
    try:
        conn.settimeout(10)
        req = b""
        while b"\r\n\r\n" not in req:
            d = conn.recv(4096)
            if not d:
                return
            req += d
        path = req.split(b" ")[1].decode()
        data, hold = answer(path)
        if data:
            conn.sendall(data)
        if hold:
            time.sleep(hold)
    except Exception:
        pass
    finally:
        try:
            if isinstance(conn, ssl.SSLSocket):
                conn.unwrap()
        except Exception:
            pass
        conn.close()

def main():
    port = int(sys.argv[1])
    ctx = None
    if len(sys.argv) >= 5 and sys.argv[2] == "--tls":
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.load_cert_chain(sys.argv[3], sys.argv[4])
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((os.environ.get("SBHTTP_BIND", "127.0.0.1"), port))
    s.listen(16)
    while True:
        c, _ = s.accept()
        if ctx:
            try:
                c.settimeout(10)
                c = ctx.wrap_socket(c, server_side=True)
            except Exception:
                c.close()
                continue
        threading.Thread(target=serve, args=(c,), daemon=True).start()

main()
