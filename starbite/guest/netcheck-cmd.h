/* SPDX-License-Identifier: GPL-2.0-only */
/* Linux's network check (Track E, for the linuxtest; 8i's App Store downloads): a command line for the guest's
 * terminal (foot, bash, as "user"), typed by the host's self test like its STARBITE-TERM-OK line. Works with the
 * bases on the card today (base-e8c: curl, coreutils, iproute2). Nothing here is written anywhere but the kernel log.
 *
 * It waits up to 30 s for sbnet's link (the host's uplink: Wi-Fi or mobile data) and sbnet-up's default route, then
 * one plain-HTTP request to deb.debian.org over IPv4 (the NAT's path, as apt uses it: DNS through 10.77.0.1, one TCP
 * connection, a 2xx/3xx answer), and writes ONE kernel log line (kept in pmsg by guest.c: it contains STARBITE):
 *   STARBITE-NET: curl RC http CODE dns S connect S total S; link L uplink U waited N s
 * RC is curl's exit code: 0 the answer came (then CODE is the HTTP status: 200 expected), 6 no DNS answer, 7 no TCP
 * connection, 28 timed out (20 s), other: curl's own. L 0/1 and U none / wifi / cell are what sbnet saw.
 * The host's verdict: PASS when the line has "STARBITE-NET: curl 0 http 2" or "... http 3"; FAIL on any other
 * STARBITE-NET line; no line within 60 s of the typing's end: FAIL ("no answer from the guest's shell").
 * Then guestnet's own counters ("forwarded N out, M in") next to it.
 * The command ends with the usual STARBITE-TERM-OK marker, so a test that waits for it keeps working. */
#ifndef SBG_NETCHECK_CMD_H
#define SBG_NETCHECK_CMD_H

#define SBG_NETCHECK_CMD                                                                                        \
    "for i in $(seq 30); do [ \"$(cat /sys/class/net/sbnet0/sbnet/link 2>/dev/null)\" = 1 ] && "              \
    "ip -4 route | grep -q ^default && break; sleep 1; done; "                                               \
    "r=$(curl -4 -s -o /dev/null -m 20 -w '%{http_code} dns %{time_namelookup} connect %{time_connect} "      \
    "total %{time_total}' http://deb.debian.org/debian/); "                                                  \
    "echo \"STARBITE-NET: curl $? http $r; link $(cat /sys/class/net/sbnet0/sbnet/link) uplink "             \
    "$(cat /sys/class/net/sbnet0/sbnet/uplink) waited $i s\" > /dev/kmsg; echo STARBITE-TERM-OK > /dev/kmsg\n"

#define SBG_NETCHECK_LINE "STARBITE-NET: "          /* the result line's start (guest_log / the test's matcher) */
#define SBG_NETCHECK_PASS1 "STARBITE-NET: curl 0 http 2"
#define SBG_NETCHECK_PASS2 "STARBITE-NET: curl 0 http 3"

/* sbgpu (Track E, 2026-11-14): GL on Starbite's Mali through sbguest.ko's render node. sbgl-probe (base-e9a on) makes
 * an EGL context, names GL's renderer, clears a 64 x 64 target and reads a pixel back (right only when the Mali ran
 * the job chains), times 100 frames, and logs "STARBITE-GPU: ..." lines; LIBGL_ALWAYS_SOFTWARE (sbwl's default for the
 * apps) is dropped for it. PASS: a line starting SBG_GPUCHECK_PASS; FAIL: "STARBITE-GPU: FAIL ..."; no line in 30 s
 * of the typing's end: FAIL. Starbite's own side logs "guest: sbgpu: job N on slot S in us T" for the first jobs. */
#define SBG_GPUCHECK_CMD "env -u LIBGL_ALWAYS_SOFTWARE sbgl-probe >/dev/null 2>&1; echo STARBITE-TERM-OK > /dev/kmsg\n"
#define SBG_GPUCHECK_LINE "STARBITE-GPU: "
#define SBG_GPUCHECK_PASS "STARBITE-GPU: PASS"

#endif
