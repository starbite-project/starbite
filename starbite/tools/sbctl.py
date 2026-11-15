#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""sbctl: USB Control for Starbite OS (kern/usbctl.h), the Mac's side. Standard library + the Mac's openssl only.

  sbctl.py [--port /dev/cu.usbmodemX] [--key ~/.starbite/sbctl-key.pem] COMMAND [ARGS]

  pair              authenticate; a new key asks on the (unlocked) phone first, waits up to 90 s for the hold
  status            version, row, uptime, locked, battery, charging
  rows              the catalog's rows (key, name, ready, default)
  boot KEY          restart into a row once (works on a locked phone; night rules are boot-safe.sh's, not this tool's)
  reboot TARGET     menu | recovery | recovery-repair | maint | maint-rescue | fastboot | download
  off               power off
  log [N]           the kernel log (its last N lines)
  pmsg [N]          the newest crash reports
  follow [SECONDS]  stream the kernel log (default until ^C)
  raw LINE          any console line (ver, bat, usb, ...)

The key is made once (P-256, `openssl ecparam`), kept in ~/.starbite/sbctl-key.pem (0600). The phone keeps only its
public half. Exit codes: 0 ok, 1 refused / error, 2 no port, 3 pairing not approved in time.
"""
import glob
import os
import re
import subprocess
import sys
import termios
import time
import tty

KEY_DEFAULT = os.path.expanduser("~/.starbite/sbctl-key.pem")


def openssl():
    for p in ("/opt/homebrew/bin/openssl", "/usr/local/bin/openssl", "/usr/bin/openssl"):
        if os.path.exists(p):
            return p
    return "openssl"


def ensure_key(path):
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), mode=0o700, exist_ok=True)
        subprocess.run([openssl(), "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", path],
                       check=True, stderr=subprocess.DEVNULL)
        os.chmod(path, 0o600)
        print(f"sbctl: a new key: {path}", file=sys.stderr)
    der = subprocess.run([openssl(), "ec", "-in", path, "-pubout", "-outform", "DER"], check=True,
                         capture_output=True).stdout
    point = der[-65:]
    if point[0] != 4:
        raise SystemExit("sbctl: the public key is not an uncompressed P-256 point")
    return point.hex()


def sign(path, text):
    return subprocess.run([openssl(), "dgst", "-sha256", "-sign", path], input=text.encode(), check=True,
                          capture_output=True).stdout.hex()


class Port:
    def __init__(self, dev):
        self.fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(self.fd)
        a = termios.tcgetattr(self.fd)
        a[2] |= termios.CLOCAL | termios.HUPCL
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        self.buf = b""

    def write(self, s):
        data = s.encode()
        while data:
            try:
                n = os.write(self.fd, data)
                data = data[n:]
            except BlockingIOError:
                time.sleep(0.01)

    def read_some(self, wait):
        end = time.time() + wait
        got = b""
        while time.time() < end:
            try:
                d = os.read(self.fd, 4096)
                if d:
                    got += d
                    continue
            except BlockingIOError:
                pass
            time.sleep(0.02)
        return got

    def lines_until(self, pred, timeout, idle=None, echo=None):
        """the phone's lines (the prompt and our echo stripped) until pred(line) or the timeout / an idle gap"""
        out = []
        end = time.time() + timeout
        last = time.time()
        while time.time() < end:
            d = self.read_some(0.1)
            if d:
                last = time.time()
                self.buf += d
                while b"\n" in self.buf:
                    raw, self.buf = self.buf.split(b"\n", 1)
                    line = raw.decode(errors="replace").replace("\r", "")
                    line = re.sub(r"^(> ?)+", "", line)
                    if echo and line.strip() == echo.strip():
                        continue
                    out.append(line)
                    if echo is not None and pred and pred(line):
                        return out
                    if pred and echo is None and pred(line):
                        return out
            elif idle and time.time() - last > idle:
                break
        return out


def find_port(arg):
    if arg:
        return arg
    c = sorted(glob.glob("/dev/cu.usbmodem*"))
    return c[0] if c else None


def handshake(p, key_path, pair_wait=90):
    pub = ensure_key(key_path)
    p.write("hello\r")
    ls = p.lines_until(lambda l: l.startswith("SBCTL1 "), 5)
    ch = [l for l in ls if l.startswith("SBCTL1 ")]
    if not ch:
        raise SystemExit("sbctl: no challenge from the phone (is USB Control on in Settings?)")
    _, nonce, pid = ch[-1].split()[:3]
    sig = sign(key_path, f"sbctl1|{nonce}|{pid}|{pub}")
    line = f"AUTH {pub} {sig}"
    p.write(line + "\r")
    verdict = lambda l: l.startswith(("OK ", "REFUSED", "WAIT", "PENDING"))
    ls = p.lines_until(verdict, 10, echo=line)
    v = [l for l in ls if verdict(l)]
    if v and v[-1].startswith("PENDING"):
        print("sbctl: " + v[-1] + " -- hold to allow on the phone", file=sys.stderr)
        ls = p.lines_until(lambda l: l.startswith(("OK ", "REFUSED")), pair_wait)
        v = [l for l in ls if l.startswith(("OK ", "REFUSED"))]
        if not v:
            print("sbctl: not approved in time", file=sys.stderr)
            sys.exit(3)
    if not v or not v[-1].startswith("OK "):
        print("sbctl: " + (v[-1] if v else "no answer"), file=sys.stderr)
        sys.exit(1)
    return v[-1].split()[1]


def run(p, line, idle=0.8, timeout=30, until=None):
    p.write(line + "\r")
    for l in p.lines_until(until, timeout, idle=idle, echo=line):
        if l.strip():
            print(l)


def main(argv):
    port = key = None
    a = list(argv)
    while a and a[0].startswith("--"):
        o = a.pop(0)
        if o == "--port":
            port = a.pop(0)
        elif o == "--key":
            key = os.path.expanduser(a.pop(0))
        else:
            print(__doc__)
            return 1
    if not a:
        print(__doc__)
        return 1
    dev = find_port(port)
    if not dev:
        print("sbctl: no /dev/cu.usbmodem* (the phone's cable, USB Control on, the Mac's Allow)", file=sys.stderr)
        return 2
    p = Port(dev)
    fp = handshake(p, key or KEY_DEFAULT)
    print(f"sbctl: session {fp} on {dev}", file=sys.stderr)
    cmd, args = a[0], a[1:]
    if cmd == "pair":
        pass
    elif cmd in ("status", "rows", "off") or cmd in ("boot", "reboot") and len(args) == 1:
        run(p, " ".join([cmd] + args), until=(lambda l: l.startswith("rows ")) if cmd == "rows" else None)
        if cmd in ("boot", "reboot", "off"):
            return 0
    elif cmd in ("log", "pmsg"):
        run(p, " ".join([cmd] + args), idle=1.5, timeout=120)
    elif cmd == "follow":
        secs = float(args[0]) if args else 1e9
        p.write("follow on\r")
        end = time.time() + secs
        try:
            while time.time() < end:
                for l in p.lines_until(None, 1.0, idle=None):
                    if l.strip() and l.strip() != "follow on":
                        print(l, flush=True)
        except KeyboardInterrupt:
            pass
    elif cmd == "raw" and args:
        run(p, " ".join(args))
    else:
        print(__doc__)
        return 1
    p.write("bye\r")
    p.read_some(0.2)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
