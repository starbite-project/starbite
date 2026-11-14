#!/usr/bin/env python3
# netcheck (Track E, 8i's stalled App Store downloads): Linux's network through Starbite's Wi-Fi / NAT under a long
# download, as apt does it (plain HTTP from deb.debian.org). Every 5 s one "STARBITE-NET:" line to the kernel log
# (Starbite keeps the guest's lines with STARBITE in pmsg): bytes, rate, sbnet's counters (host drops, tx full, link,
# uplink, config seq), TCP retransmits. A stall (no byte for 30 s) is logged, then a NEW connection resumes the file
# (Range): it tells a dead flow (new connections work) from a dead uplink (they do not).
#   python3 netcheck.py [MiB=32] [URL]          stops after MiB or at the file's end; exit 0 = no stall
import os, socket, sys, time, urllib.request

MIB = int(sys.argv[1]) if len(sys.argv) > 1 else 32
URL = sys.argv[2] if len(sys.argv) > 2 else "http://deb.debian.org/debian/dists/trixie/main/Contents-all.gz"
S = "/sys/class/net/sbnet0/sbnet/"


def rd(n):
    try:
        return open(S + n).read().strip()
    except OSError:
        return "-"


def retrans():
    try:
        l = [x.split() for x in open("/proc/net/snmp") if x.startswith("Tcp:")]
        return l[1][l[0].index("RetransSegs")]
    except (OSError, ValueError, IndexError):
        return "-"


def say(m):
    line = "STARBITE-NET: " + m
    print(line, flush=True)
    try:
        with open("/dev/kmsg", "w") as k:
            k.write(line + "\n")
    except OSError:
        pass


def counters():
    return "host_drops %s tx_full %s link %s uplink %s seq %s retrans %s" % (
        rd("host_drops"), rd("tx_full"), rd("link"), rd("uplink"), rd("seq"), retrans())


got, t0, stalls, conns = 0, time.time(), 0, 0
say("start %d MiB from %s; %s" % (MIB, URL.split("/")[2], counters()))
while got < MIB << 20:
    try:
        req = urllib.request.Request(URL, headers={"Range": "bytes=%d-" % got} if got else {})
        r = urllib.request.urlopen(req, timeout=30)
        conns += 1
        if conns > 1:
            say("new connection %d at %d KiB: answered (%s)" % (conns, got >> 10, r.status))
        last = time.time()
        while got < MIB << 20:
            b = r.read(65536)
            if not b:
                say("the file ended at %d KiB" % (got >> 10))
                MIB = 0
                break
            got += len(b)
            if time.time() - last >= 5:
                last = time.time()
                say("%d KiB in %d s (%d KiB/s); %s" % (got >> 10, last - t0, (got >> 10) / max(1, last - t0), counters()))
    except (OSError, socket.timeout) as e:
        stalls += 1
        say("stalled at %d KiB after %d s (%s): %s" % (got >> 10, time.time() - t0, type(e).__name__, counters()))
        if stalls >= 4:
            break
        time.sleep(2)
say("done: %d KiB in %d s, %d stall(s), %d connection(s); %s" % (got >> 10, time.time() - t0, stalls, conns, counters()))
sys.exit(1 if stalls else 0)
