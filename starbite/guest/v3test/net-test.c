// SPDX-License-Identifier: GPL-2.0-only
/* net-test: the guest-side helper of sbnet's QEMU test (v3test/net-sbinit). One line of result on stdout, exit 0 = ok.
 *   net-test echo IP PORT COUNT SIZE        UDP round trips one at a time: RTT min / avg / max
 *   net-test dns NAME SERVER [WANT]         one A query by hand (no libc resolver): the address, the time
 *   net-test flood IP PORT MB [WINDOW]      UDP echo flood of MB MiB in 1372-byte datagrams (MTU 1400), at most
 *                                           WINDOW (128) unanswered: MB/s out and back, lost and corrupt datagrams
 *   net-test send IP PORT TEXT              one datagram (sim_net's control port: "flap MS")
 * Built static in the VM: gcc -O2 -static -Wall -Wextra v3test/net-test.c -o net-test */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PAYLOAD 1372

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static int udp_to(const char *ip, int port)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };

	if (s < 0 || inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
		printf("FAIL bad address %s or no socket (%s)\n", ip, strerror(errno));
		exit(1);
	}
	if (connect(s, (struct sockaddr *)&sa, sizeof(sa))) {
		printf("FAIL connect %s:%d: %s\n", ip, port, strerror(errno));
		exit(1);
	}
	return s;
}

static int wait_in(int s, int ms)
{
	struct pollfd p = { .fd = s, .events = POLLIN };

	return poll(&p, 1, ms) > 0;
}

static int cmd_echo(char **a)
{
	int s = udp_to(a[0], atoi(a[1])), n = atoi(a[2]), size = atoi(a[3]), ok = 0, bad = 0;
	double mn = 1e9, mx = 0, sum = 0;
	char out[PAYLOAD], in[PAYLOAD + 64];

	if (size < 8 || size > PAYLOAD)
		size = 512;
	for (int i = 0; i < n; i++) {
		for (int k = 0; k < size; k++)
			out[k] = (char)(i * 31 + k);
		double t0 = now();

		if (send(s, out, size, 0) != size)
			continue;
		while (wait_in(s, 2000)) {
			ssize_t r = recv(s, in, sizeof(in), 0);

			if (r == size && !memcmp(in, out, size)) {
				double d = (now() - t0) * 1e6;

				ok++;
				sum += d;
				mn = d < mn ? d : mn;
				mx = d > mx ? d : mx;
				break;
			}
			bad++;
		}
	}
	if (!ok) {
		printf("FAIL udp echo %s:%s: 0/%d answered\n", a[0], a[1], n);
		return 1;
	}
	printf("%s %d/%d round trips of %d bytes, rtt min/avg/max %.0f/%.0f/%.0f us%s\n", ok == n ? "ok" : "FAIL", ok, n,
	       size, mn, sum / ok, mx, bad ? " (stray or corrupt answers seen)" : "");
	return ok == n && !bad ? 0 : 1;
}

static int cmd_dns(char **a, int argc)
{
	int s = udp_to(a[1], 53);
	unsigned char q[300], r[1500];
	const char *name = a[0], *want = argc > 2 ? a[2] : NULL;
	int n = 12, id = (getpid() ^ (int)(now() * 1000)) & 0xffff;

	memset(q, 0, 12);
	q[0] = id >> 8;
	q[1] = id;
	q[2] = 1;		/* RD */
	q[5] = 1;		/* QDCOUNT */
	for (const char *p = name; *p;) {
		const char *e = strchr(p, '.');
		int l = e ? e - p : (int)strlen(p);

		if (l < 1 || l > 63 || n + l + 6 > (int)sizeof(q)) {
			printf("FAIL bad name %s\n", name);
			return 1;
		}
		q[n++] = l;
		memcpy(q + n, p, l);
		n += l;
		p += l + (e ? 1 : 0);
	}
	q[n++] = 0;
	q[n++] = 0;
	q[n++] = 1;		/* A */
	q[n++] = 0;
	q[n++] = 1;		/* IN */
	for (int tries = 0; tries < 3; tries++) {
		double t0 = now();

		if (send(s, q, n, 0) != n)
			continue;
		while (wait_in(s, 2000)) {
			int m = recv(s, r, sizeof(r), 0), o = n, an;

			if (m < n || r[0] != q[0] || r[1] != q[1] || !(r[2] & 0x80))
				continue;
			an = r[6] << 8 | r[7];
			for (int i = 0; i < an && o + 12 <= m; i++) {
				int type, len;

				if ((r[o] & 0xc0) == 0xc0)
					o += 2;
				else {
					while (o < m && r[o])
						o += r[o] + 1;
					o++;
				}
				if (o + 10 > m)
					break;
				type = r[o] << 8 | r[o + 1];
				len = r[o + 8] << 8 | r[o + 9];
				o += 10;
				if (type == 1 && len == 4 && o + 4 <= m) {
					char ip[20];

					snprintf(ip, sizeof(ip), "%u.%u.%u.%u", r[o], r[o + 1], r[o + 2], r[o + 3]);
					printf("%s %s -> %s from %s in %.1f ms (rcode %d)\n",
					       want && strcmp(ip, want) ? "FAIL" : "ok", name, ip, a[1], (now() - t0) * 1e3,
					       r[3] & 15);
					return want && strcmp(ip, want) ? 1 : 0;
				}
				o += len;
			}
			printf("FAIL %s: an answer without an A record (rcode %d, %d answers)\n", name, r[3] & 15, an);
			return 1;
		}
	}
	printf("FAIL %s: no answer from %s in 3 tries\n", name, a[1]);
	return 1;
}

/* datagram i: its number, then bytes derived from it */
static void fill(unsigned char *p, uint32_t i)
{
	memcpy(p, &i, 4);
	for (int k = 4; k < PAYLOAD; k++)
		p[k] = (unsigned char)(i * 131 + k);
}

static int cmd_flood(char **a, int argc)
{
	int s = udp_to(a[0], atoi(a[1])), win = argc > 3 ? atoi(a[3]) : 128, big = 4 << 20;
	uint32_t total = (uint32_t)((atof(a[2]) * 1048576.0) / PAYLOAD), sent = 0, got = 0, corrupt = 0, lost = 0;
	unsigned char out[PAYLOAD], in[PAYLOAD + 64];
	double t0, t_prog, t_end;

	setsockopt(s, SOL_SOCKET, SO_RCVBUFFORCE, &big, sizeof(big));
	setsockopt(s, SOL_SOCKET, SO_SNDBUFFORCE, &big, sizeof(big));
	if (win < 1)
		win = 128;
	t0 = t_prog = now();
	while (got + lost < total) {
		while (sent < total && sent - got - lost < (uint32_t)win) {
			fill(out, sent);
			if (send(s, out, PAYLOAD, MSG_DONTWAIT) != PAYLOAD) {
				if (errno == EAGAIN || errno == ENOBUFS)
					break;
				printf("FAIL send: %s\n", strerror(errno));
				return 1;
			}
			sent++;
		}
		if (wait_in(s, 20)) {
			ssize_t r;

			while ((r = recv(s, in, sizeof(in), MSG_DONTWAIT)) > 0) {
				uint32_t i;

				memcpy(&i, in, 4);
				fill(out, i);
				if (r != PAYLOAD || i >= sent || memcmp(in, out, PAYLOAD))
					corrupt++;
				else
					got++;
				t_prog = now();
			}
		}
		if (now() - t_prog > 0.5) {	/* nothing came back for 500 ms: what is out is lost */
			lost = sent - got;
			t_prog = now();
		}
		if (now() - t0 > 300)
			break;
	}
	t_end = now();
	double secs = t_end - t0, mb_out = sent * (double)PAYLOAD / 1048576.0, mb_in = got * (double)PAYLOAD / 1048576.0;
	int ok = got + lost >= total && lost <= total / 100 && !corrupt;

	printf("%s %.1f MiB out in %.2f s = %.2f MiB/s, %.1f MiB echoed back = %.2f MiB/s (%u datagrams of %d bytes, "
	       "window %d, lost %u, corrupt %u)\n", ok ? "ok" : "FAIL", mb_out, secs, mb_out / secs, mb_in, mb_in / secs,
	       sent, PAYLOAD, win, lost, corrupt);
	return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc >= 6 && !strcmp(argv[1], "echo"))
		return cmd_echo(argv + 2);
	if (argc >= 4 && !strcmp(argv[1], "dns"))
		return cmd_dns(argv + 2, argc - 2);
	if (argc >= 5 && !strcmp(argv[1], "flood"))
		return cmd_flood(argv + 2, argc - 2);
	if (argc >= 5 && !strcmp(argv[1], "send")) {
		int s = udp_to(argv[2], atoi(argv[3]));
		ssize_t n = (ssize_t)strlen(argv[4]);

		if (send(s, argv[4], n, 0) != n) {
			printf("FAIL send: %s\n", strerror(errno));
			return 1;
		}
		printf("ok sent %zd bytes\n", n);
		return 0;
	}
	fprintf(stderr, "usage: net-test echo IP PORT COUNT SIZE | dns NAME SERVER [WANT] | flood IP PORT MB [WINDOW] | "
		"send IP PORT TEXT\n");
	return 2;
}
