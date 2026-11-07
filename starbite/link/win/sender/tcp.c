/* SPDX-License-Identifier: GPL-2.0-only
 * sblinkd: the TCP transport (PROTOCOL.md section 1): one listener, one receive thread per connection.
 * A second viewer gets BYE(BUSY) (sbl_link_begin) and is closed. Winsock on Windows, BSD sockets on the
 * Mac host build.
 */
#include "sender.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <ws2tcpip.h>
typedef SOCKET sock_t;
#  define SOCK_BAD INVALID_SOCKET
#  define sock_close closesocket
#  define SHUT_RDWR SD_BOTH
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
#  include <signal.h>
typedef int sock_t;
#  define SOCK_BAD (-1)
#  define sock_close close
#endif

typedef struct tcp_conn {
    sbl_link link;             /* first: sbl_link * == tcp_conn * */
    sbl_sender *s;
    sock_t fd;
} tcp_conn;

typedef struct tcp_listener {
    sbl_sender *s;
    sock_t fd;
} tcp_listener;

static int tcp_send(sbl_link *l, const uint8_t *p, size_t n)
{
    tcp_conn *c = (tcp_conn *)l;
    while (n) {
        int chunk = n > (1u << 30) ? (1 << 30) : (int)n;
#ifdef _WIN32
        int k = send(c->fd, (const char *)p, chunk, 0);
#else
        ssize_t k = send(c->fd, p, (size_t)chunk, 0);
#endif
        if (k <= 0) return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

static void tcp_close(sbl_link *l)
{
    tcp_conn *c = (tcp_conn *)l;
    shutdown(c->fd, SHUT_RDWR);
}

static void conn_thread(void *arg)
{
    tcp_conn *c = arg;
    sbl_sender *s = c->s;
    uint8_t *buf = malloc(64 * 1024);
    if (buf && sbl_link_begin(s, &c->link) == 0) {
        for (;;) {
#ifdef _WIN32
            int k = recv(c->fd, (char *)buf, 64 * 1024, 0);
#else
            ssize_t k = recv(c->fd, buf, 64 * 1024, 0);
#endif
            if (k <= 0) break;
            if (sbl_rx(s, &c->link, buf, (size_t)k, 0) < 0) break;
        }
        sbl_link_end(s, &c->link, "connection closed");
    }
    shutdown(c->fd, SHUT_RDWR);
    sock_close(c->fd);
    free(buf);
    free(c);
}

static void accept_thread(void *arg)
{
    tcp_listener *t = arg;
    for (;;) {
        struct sockaddr_in a;
#ifdef _WIN32
        int al = sizeof a;
#else
        socklen_t al = sizeof a;
#endif
        sock_t fd = accept(t->fd, (struct sockaddr *)&a, &al);
        if (fd == SOCK_BAD) {
            if (t->s->stop) break;
            plat_sleep_ms(100);
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        int sb = 4 << 20;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char *)&sb, sizeof sb);
#ifdef _WIN32
        DWORD to = 5000;   /* a viewer that stops reading must not hold the send lock forever */
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
#else
        struct timeval to = { 5, 0 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);
#  ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#  endif
#endif
        tcp_conn *c = calloc(1, sizeof *c);
        if (!c) { sock_close(fd); continue; }
        char ip[64];
        inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
        char name[64];
        snprintf(name, sizeof name, "tcp %s:%u", ip, (unsigned)ntohs(a.sin_port));
        sbl_link_init(&c->link, name, 0);
        c->link.send = tcp_send;
        c->link.close = tcp_close;
        c->link.ctx = c;
        c->s = t->s;
        c->fd = fd;
        if (plat_thread(conn_thread, c) < 0) { sock_close(fd); free(c); }
    }
}

int sbl_tcp_start(sbl_sender *s, const char *bind_addr, int port, int *bound_port)
{
#ifdef _WIN32
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd)) { sbl_log("tcp: WSAStartup failed"); return -1; }
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    tcp_listener *t = calloc(1, sizeof *t);
    if (!t) return -1;
    t->s = s;
    t->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (t->fd == SOCK_BAD) { free(t); sbl_log("tcp: socket failed"); return -1; }
    int one = 1;
#ifdef _WIN32
    setsockopt(t->fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
#else
    setsockopt(t->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_addr && *bind_addr ? bind_addr : "127.0.0.1", &a.sin_addr) != 1) {
        sbl_log("tcp: bad bind address %s", bind_addr);
        sock_close(t->fd); free(t); return -1;
    }
    if (bind(t->fd, (struct sockaddr *)&a, sizeof a) || listen(t->fd, 4)) {
        sbl_log("tcp: cannot listen on %s:%d", bind_addr, port);
        sock_close(t->fd); free(t); return -1;
    }
#ifdef _WIN32
    int al = sizeof a;
#else
    socklen_t al = sizeof a;
#endif
    getsockname(t->fd, (struct sockaddr *)&a, &al);
    if (bound_port) *bound_port = ntohs(a.sin_port);
    sbl_log("tcp: listening on %s:%u%s", bind_addr, (unsigned)ntohs(a.sin_port),
            strcmp(bind_addr, "127.0.0.1") ? " (reachable from the network: anyone who connects gets keyboard and mouse)" : "");
    if (plat_thread(accept_thread, t) < 0) { sock_close(t->fd); free(t); return -1; }
    return 0;
}
