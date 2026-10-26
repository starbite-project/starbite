#!/usr/bin/env python3
"""Run a shell command on the phone's Linux (busybox telnetd on 172.16.42.1,
USB CDC-ECM from tools/linux/init) and print its output.

    tools/linux/phone.py 'dmesg | tail'
    tools/linux/phone.py --push LOCAL REMOTE      (small files, base64 over the shell)
"""
import base64
import socket
import sys
import time

HOST, PORT = '172.16.42.1', 23
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240


class Shell:
    def __init__(self, timeout=30):
        self.s = socket.create_connection((HOST, PORT), timeout=10)
        self.s.settimeout(timeout)
        self.buf = b''
        self.run('stty -echo 2>/dev/null; PS1=; export PS1', quiet=True)

    def _recv(self):
        data = self.s.recv(65536)
        if not data:
            raise EOFError('connection closed')
        out, i = bytearray(), 0
        while i < len(data):
            b = data[i]
            if b == IAC and i + 1 < len(data):
                cmd = data[i + 1]
                if cmd in (DO, DONT, WILL, WONT) and i + 2 < len(data):
                    opt = data[i + 2]
                    reply = WONT if cmd in (DO, DONT) else DONT
                    self.s.sendall(bytes([IAC, reply, opt]))
                    i += 3
                    continue
                if cmd == SB:
                    j = data.find(bytes([IAC, SE]), i)
                    i = j + 2 if j >= 0 else len(data)
                    continue
                i += 2
                continue
            out.append(b)
            i += 1
        return bytes(out)

    def run(self, cmd, quiet=False):
        n = time.time_ns()
        mark = f'__SB{n}__'
        # split in the command text so the tty echo never contains the marker
        self.s.sendall(f'{cmd}; echo "__S""B{n}__$?"\n'.encode())
        while True:
            k = self.buf.find(mark.encode())
            if k >= 0:
                nl = self.buf.find(b'\n', k)
                if nl >= 0:
                    rc = self.buf[k + len(mark):nl].strip()
                    out, self.buf = self.buf[:k], self.buf[nl + 1:]
                    # drop the echoed command line, if the tty echoed it
                    text = out.decode(errors='replace').replace('\r', '')
                    lines = [l for l in text.split('\n') if f'B{n}__' not in l]
                    if not quiet:
                        sys.stdout.write('\n'.join(lines).strip('\n') + '\n')
                    return int(rc or b'0') if rc.isdigit() else 1
            self.buf += self._recv()


def main():
    sh = Shell()
    if sys.argv[1:2] == ['--push']:
        data = open(sys.argv[2], 'rb').read()
        remote = sys.argv[3]
        sh.run(f': > {remote}.b64', quiet=True)
        b = base64.b64encode(data).decode()
        for i in range(0, len(b), 3000):
            sh.run(f"printf '%s' '{b[i:i + 3000]}' >> {remote}.b64", quiet=True)
        sys.exit(sh.run(f'base64 -d {remote}.b64 > {remote} && rm {remote}.b64 && ls -l {remote}'))
    sys.exit(sh.run(' '.join(sys.argv[1:])))


if __name__ == '__main__':
    main()
