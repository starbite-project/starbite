/* usbpeek: a read-only sample of the raw frames on the installer environment's USB network (usb0, CDC-ECM: each USB
 * bulk transfer carries one Ethernet frame). For the night dashboard (tools/installer/dash/sbdash.sh).
 *
 *   usbpeek IFACE OUTFILE [PERIOD_S]
 *
 * Every PERIOD_S seconds (default 3) it opens an AF_PACKET socket on IFACE, takes up to 3 frames within 300 ms,
 * closes the socket again (so the kernel copies nothing in between: no cost to the install's own traffic) and
 * rewrites OUTFILE with one line per frame: direction, length, the decoded Ethernet / IPv4 / TCP-UDP header and the
 * first 32 bytes in hex. It sends nothing, binds nothing else, and writes only OUTFILE (+ OUTFILE.tmp).
 * Static build (Lima VM, aarch64): gcc -static -O2 -o usbpeek usbpeek.c */
#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static void line(FILE *f, const unsigned char *b, int n, int out)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    fprintf(f, "%02d:%02d:%02d.%03ld %s %4dB ", tm.tm_hour, tm.tm_min, tm.tm_sec, (long)tv.tv_usec / 1000,
            out ? "TX>" : "<RX", n);
    if (n >= 14) {
        unsigned type = b[12] << 8 | b[13];
        if (type == 0x0800 && n >= 34) {
            const unsigned char *ip = b + 14;
            int ihl = (ip[0] & 15) * 4, proto = ip[9];
            fprintf(f, "%u.%u.%u.%u>%u.%u.%u.%u ", ip[12], ip[13], ip[14], ip[15], ip[16], ip[17], ip[18], ip[19]);
            const unsigned char *t = ip + ihl;
            if (proto == 6 && n >= 14 + ihl + 20) {
                unsigned fl = t[13];
                fprintf(f, "TCP %u>%u seq=%u win=%u [%s%s%s%s] ", t[0] << 8 | t[1], t[2] << 8 | t[3],
                        (unsigned)(t[4] << 24 | t[5] << 16 | t[6] << 8 | t[7]), t[14] << 8 | t[15],
                        fl & 2 ? "S" : "", fl & 16 ? "A" : "", fl & 8 ? "P" : "", fl & 1 ? "F" : "");
            } else if (proto == 17 && n >= 14 + ihl + 8) {
                fprintf(f, "UDP %u>%u ", t[0] << 8 | t[1], t[2] << 8 | t[3]);
            } else {
                fprintf(f, "proto %d ", proto);
            }
        } else if (type == 0x0806) {
            fprintf(f, "ARP ");
        } else if (type == 0x86dd) {
            fprintf(f, "IPv6 ");
        } else {
            fprintf(f, "type %04x ", type);
        }
    }
    fprintf(f, "|");
    for (int i = 0; i < n && i < 32; i++)
        fprintf(f, "%s%02x", i && i % 8 == 0 ? " " : "", b[i]);
    fprintf(f, "\n");
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: usbpeek IFACE OUTFILE [PERIOD_S]\n");
        return 2;
    }
    const char *ifn = argv[1], *outp = argv[2];
    int period = argc > 3 ? atoi(argv[3]) : 3;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", outp);
    unsigned char buf[2048];
    for (;;) {
        int s = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
        unsigned idx = if_nametoindex(ifn);
        FILE *f = fopen(tmp, "w");
        if (!f) {
            sleep(period);
            continue;
        }
        if (s < 0 || !idx) {
            fprintf(f, "(no packet socket on %s: %s)\n", ifn, s < 0 ? strerror(errno) : "no such interface");
        } else {
            struct sockaddr_ll sa = {.sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL), .sll_ifindex = idx};
            if (bind(s, (struct sockaddr *)&sa, sizeof sa) == 0) {
                struct timeval t0, t1;
                gettimeofday(&t0, NULL);
                for (int got = 0; got < 3;) {
                    struct pollfd p = {.fd = s, .events = POLLIN};
                    if (poll(&p, 1, 300) <= 0)
                        break;
                    struct sockaddr_ll from;
                    socklen_t fl = sizeof from;
                    int n = recvfrom(s, buf, sizeof buf, MSG_TRUNC, (struct sockaddr *)&from, &fl);
                    if (n <= 0)
                        break;
                    line(f, buf, n < (int)sizeof buf ? n : (int)sizeof buf, from.sll_pkttype == PACKET_OUTGOING);
                    got++;
                    gettimeofday(&t1, NULL);
                    if ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000 > 300)
                        break;
                }
            }
        }
        if (s >= 0)
            close(s);
        fclose(f);
        rename(tmp, outp);
        sleep(period);
    }
}
