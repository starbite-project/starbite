/* SPDX-License-Identifier: MIT
 * sbgw with a real Linux guest (run by host/run-gw-netns.sh, as root, in the VM): the gateway's guest link is a TAP
 * device that the script moves into the network namespace "sbguest" (Linux's own stack there is the guest: its
 * DHCP client, ping, resolver, curl); the uplinks are sbnet on a second TAP (Ethernet, as Wi-Fi: 10.77.0.2/24,
 * Linux 10.77.0.1 routes and masquerades) and on a TUN (raw IP, as mobile data: 10.78.0.2, MTU 1400).
 *   gw-tap <guest tap> <uplink tap> <uplink tun> <dns ip> <ready file>
 * Signals: USR1 mobile data, USR2 Wi-Fi, HUP no uplink, TERM/INT the counters and out. */
#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
#include "../net.h"
#include "../gw.h"

static int gfd, efd, rfd;
static struct sbnet eth, raw;
static struct sbgw gw;
static volatile sig_atomic_t want = -1, quit;

static uint64_t ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}
static uint64_t n_now(void *ctx) { (void)ctx; return ms(); }
static uint32_t n_rnd(void *ctx)
{
    uint32_t r;
    (void)ctx;
    if (getrandom(&r, sizeof(r), 0) != sizeof(r))
        r = (uint32_t)random();
    return r;
}
static void n_log(void *ctx, const char *l)
{
    (void)ctx;
    printf("gw-tap: %s\n", l);
    fflush(stdout);
}
static int e_tx(void *ctx, const uint8_t *f, uint32_t len) { (void)ctx; return write(efd, f, len) == (ssize_t)len ? 0 : -1; }
static int r_tx(void *ctx, const uint8_t *f, uint32_t len) { (void)ctx; return write(rfd, f, len) == (ssize_t)len ? 0 : -1; }
static int g_tx(void *ctx, const uint8_t *f, uint32_t len) { (void)ctx; return write(gfd, f, len) == (ssize_t)len ? 0 : -1; }

static int tap_open(const char *name, int flags)
{
    struct ifreq ifr;
    int fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = (short)(flags | IFF_NO_PI);
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    if (fd < 0 || ioctl(fd, TUNSETIFF, &ifr) < 0) {
        perror(name);
        exit(2);
    }
    return fd;
}

static void on_sig(int s)
{
    if (s == SIGUSR1)
        want = 1;
    else if (s == SIGUSR2)
        want = 0;
    else if (s == SIGHUP)
        want = 2;
    else
        quit = 1;
}

int main(int argc, char **argv)
{
    static const uint8_t mac[6] = { 0x02, 0x5b, 0x00, 0x00, 0x00, 0x02 };
    static const uint8_t e_ip[4] = { 10, 77, 0, 2 }, e_gw[4] = { 10, 77, 0, 1 }, mask[4] = { 255, 255, 255, 0 };
    static const uint8_t r_ip[4] = { 10, 78, 0, 2 }, host32[4] = { 255, 255, 255, 255 }, none[4] = { 0, 0, 0, 0 };
    struct sbnet_os os = { 0 };
    struct sbgw_os go = { 0 };
    uint8_t dns[4], f[2048];
    uint64_t last = 0, t0 = ms();
    FILE *rf;
    if (argc < 6) {
        printf("usage: gw-tap <guest tap> <uplink tap> <uplink tun> <dns ip> <ready file>\n");
        return 2;
    }
    inet_pton(AF_INET, argv[4], dns);
    gfd = tap_open(argv[1], IFF_TAP);
    efd = tap_open(argv[2], IFF_TAP);
    rfd = tap_open(argv[3], IFF_TUN);
    os.now_ms = n_now, os.random = n_rnd, os.log = n_log;
    os.tx = e_tx;
    sbnet_init(&eth, &os, mac);
    sbnet_static(&eth, e_ip, mask, e_gw, dns);
    os.tx = r_tx;
    sbnet_init_raw(&raw, &os);
    sbnet_static(&raw, r_ip, host32, none, dns);
    raw.mtu = 1400;
    go.to_guest = g_tx, go.now_ms = n_now, go.random = n_rnd, go.log = n_log;
    sbgw_init(&gw, &go);
    sbgw_set_uplink(&gw, &eth);
    signal(SIGUSR1, on_sig), signal(SIGUSR2, on_sig), signal(SIGHUP, on_sig);
    signal(SIGTERM, on_sig), signal(SIGINT, on_sig);
    rf = fopen(argv[5], "w");
    if (rf)
        fclose(rf);
    while (!quit) {
        struct pollfd p[3] = { { gfd, POLLIN, 0 }, { efd, POLLIN, 0 }, { rfd, POLLIN, 0 } };
        if (want >= 0) {
            int w = want;
            want = -1;
            sbgw_set_uplink(&gw, w == 1 ? &raw : w == 0 ? &eth : 0);
        }
        if (poll(p, 3, 10) > 0) {
            for (int k = 0; k < 256; k++) {     /* fair: a batch from each side in turn */
                int any = 0;
                ssize_t n;
                if ((n = read(gfd, f, sizeof(f))) > 0)
                    sbgw_guest_rx(&gw, f, (uint32_t)n), any = 1;
                if ((n = read(efd, f, sizeof(f))) > 0)
                    sbnet_rx(&eth, f, (uint32_t)n), any = 1;
                if ((n = read(rfd, f, sizeof(f))) > 0)
                    sbnet_rx_ip(&raw, f, (uint32_t)n), any = 1;
                if (!any)
                    break;
            }
        }
        if (ms() - last >= 100) {
            last = ms();
            sbnet_tick(&eth), sbnet_tick(&raw);
            sbgw_tick(&gw);
        }
    }
    printf("gw-tap: %llu s; guest frames in %u, out %u; NAT out %u, in %u; dropped %u, fragments %u, too big %u, "
           "fragmented %u, evicted %u, flushed %u, no uplink %u; DHCP acks %u, ARP replies %u, echo replies %u, RSTs "
           "%u, ICMP errors in %u; conns now %u\n",
           (unsigned long long)((ms() - t0) / 1000), gw.guest_rx, gw.guest_tx, gw.out, gw.in, gw.dropped, gw.frags,
           gw.too_big, gw.fragmented, gw.evicted, gw.flushed, gw.no_uplink, gw.dhcp_acks, gw.arp_replies,
           gw.echo_replies, gw.rst_sent, gw.icmp_in, sbgw_conns(&gw));
    {
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        printf("gw-tap: CPU %.2f s user, %.2f s system\n", ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6,
               ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6);
    }
    printf("gw-tap: uplinks: Wi-Fi-like %u frames in (%u ignored), %u out (%u errors); mobile-like %u in, %u out\n",
           eth.rx_frames, eth.rx_ignored, eth.tx_frames, eth.tx_errors, raw.rx_frames, raw.tx_frames);
    return 0;
}
