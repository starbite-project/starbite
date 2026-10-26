#!/usr/bin/env python3
"""Tiny forwarding HTTP proxy on the Mac's USB address, so the phone's Linux
(no Wi-Fi yet) can reach package mirrors: plain HTTP with absolute URIs and
CONNECT tunnels for HTTPS. Nothing is cached or stored on the Mac.

    tools/linux/http-proxy.py [--bind 172.16.42.2] [--port 3128]
    phone: export http_proxy=http://172.16.42.2:3128 https_proxy=$http_proxy
"""
import argparse
import select
import socket
import socketserver
import sys
from urllib.parse import urlsplit


def pump(a, b):
    socks = [a, b]
    while True:
        r, _, x = select.select(socks, [], socks, 120)
        if x or not r:
            return
        for s in r:
            data = s.recv(262144)
            if not data:
                return
            (b if s is a else a).sendall(data)


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        c = self.request
        head = b''
        while b'\r\n\r\n' not in head:
            chunk = c.recv(65536)
            if not chunk:
                return
            head += chunk
            if len(head) > 1 << 20:
                return
        line, _, rest = head.partition(b'\r\n')
        try:
            method, target, version = line.decode('latin-1').split(' ', 2)
        except ValueError:
            return
        try:
            if method == 'CONNECT':
                host, _, port = target.rpartition(':')
                up = socket.create_connection((host, int(port)), timeout=30)
                c.sendall(b'HTTP/1.1 200 Connection established\r\n\r\n')
            else:
                u = urlsplit(target)
                up = socket.create_connection((u.hostname, u.port or 80), timeout=30)
                path = (u.path or '/') + (f'?{u.query}' if u.query else '')
                hdrs = [h for h in rest.split(b'\r\n')
                        if h and not h.lower().startswith((b'proxy-', b'connection:'))]
                req = f'{method} {path} {version}\r\n'.encode() + b'\r\n'.join(hdrs) + \
                    b'\r\nConnection: close\r\n\r\n'
                body = head.split(b'\r\n\r\n', 1)[1]
                up.sendall(req + body)
            sys.stderr.write(f'{method} {target}\n')
            pump(c, up)
        except OSError as e:
            sys.stderr.write(f'{method} {target}: {e}\n')
            try:
                c.sendall(b'HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n')
            except OSError:
                pass


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bind', default='172.16.42.2')
    ap.add_argument('--port', type=int, default=3128)
    a = ap.parse_args()
    with Server((a.bind, a.port), Handler) as s:
        print(f'proxy on {a.bind}:{a.port}', flush=True)
        s.serve_forever()


if __name__ == '__main__':
    main()
