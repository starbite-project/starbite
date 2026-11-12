/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of the Bluetooth keyboard stack: kern/bt_pair.c (HCI pairing / links), bt_l2cap.c (ACL + L2CAP), bt_hid.c
 * (boot reports), against a scripted controller and a hand-written fake keyboard (its own L2CAP / HIDP side, not
 * bt_l2cap.c, so a bug cannot hide on both ends). The Mac's cc; -v traces the log. bthid-test.sh runs it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "bt.h"
#include "bt_pair.h"
#include "bt_l2cap.h"
#include "bt_hid.h"

static int verbose, failures, checks;
#define CHECK(c, what) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

/* ---- the host's world ---- */
static uint64_t T;                          /* ms */
static char logbuf[1 << 18];
static size_t loglen;
static void f_log(const char *s)
{
    size_t n = strlen(s);
    if (verbose)
        printf("  [%6llu] %s\n", (unsigned long long)T, s);
    if (loglen + n + 2 < sizeof(logbuf)) {
        memcpy(logbuf + loglen, s, n);
        loglen += n;
        logbuf[loglen++] = '\n';
        logbuf[loglen] = 0;
    }
}
static int logged(const char *s) { return strstr(logbuf, s) != 0; }
static uint64_t f_now(void) { return T; }
static int chip_on = 1, chip_ready = 1, others_busy;
static int f_others(void) { return others_busy; }
static int f_ready(void) { return chip_on && chip_ready; }
static int f_on(void) { return chip_on; }
static uint32_t f_random(void) { return 271828; }

/* the OS's key store: one slot */
static uint8_t st_addr[6], st_key[16];
static int st_have, st_puts, st_forgets;
static int k_get(const uint8_t a[6], uint8_t k[16])
{
    if (!st_have || memcmp(a, st_addr, 6))
        return 0;
    memcpy(k, st_key, 16);
    return 1;
}
static void k_put(const uint8_t a[6], const uint8_t *k)
{
    if (k) {
        memcpy(st_addr, a, 6);
        memcpy(st_key, k, 16);
        st_have = 1;
        st_puts++;
    } else {
        if (st_have && !memcmp(a, st_addr, 6))
            st_have = 0;
        st_forgets++;
    }
}

/* the keys typed */
static char typed[256];
static unsigned ntyped, ups, downs;
static void key_cb(int usage, int down, uint32_t uc)
{
    if (verbose)
        printf("  key usage %02x %s unicode %u\n", usage, down ? "down" : "up", uc);
    if (!down) {
        ups++;
        return;
    }
    downs++;
    if (uc && ntyped + 1 < sizeof(typed)) {
        typed[ntyped++] = (char)uc;
        typed[ntyped] = 0;
    }
}
static void typed_clear(void) { ntyped = 0; typed[0] = 0; ups = downs = 0; }

/* ---- the scripted controller ---- */
static const uint8_t KB_ADDR[6] = { 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6 };
static const uint8_t KB_KEY[16] = { 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad,
                                    0xae, 0xaf };
#define HANDLE 0x0042
static struct item { uint64_t t; int acl; unsigned n; uint8_t b[300]; } items[512];
static unsigned nitems;
static void sched(uint64_t dt, int acl, const uint8_t *b, unsigned n)
{
    if (nitems < 512) {
        items[nitems].t = T + dt;
        items[nitems].acl = acl;
        items[nitems].n = n;
        memcpy(items[nitems].b, b, n);
        nitems++;
    }
}
static void ev(uint64_t dt, uint8_t code, unsigned n, ...)
{
    uint8_t b[300];
    va_list ap;
    va_start(ap, n);
    b[0] = code;
    b[1] = (uint8_t)n;
    for (unsigned i = 0; i < n; i++)
        b[2 + i] = (uint8_t)va_arg(ap, int);
    va_end(ap);
    sched(dt, 0, b, 2 + n);
}
static void ev_addr(uint64_t dt, uint8_t code, const uint8_t *more, unsigned m)
{
    uint8_t b[300];
    b[0] = code;
    b[1] = (uint8_t)(6 + m);
    memcpy(b + 2, KB_ADDR, 6);
    if (m)
        memcpy(b + 8, more, m);
    sched(dt, 0, b, 8 + m);
}
static void cc(uint16_t op, uint8_t status) { ev(1, 0x0E, 4, 1, op & 0xff, op >> 8, status); }
static void cs(uint16_t op, uint8_t status) { ev(1, 0x0F, 4, status, 1, op & 0xff, op >> 8); }

/* the controller's configuration and what it saw */
static struct {
    int present, ssp, io, refuse_enc, answer_proto, frag;   /* frag: the keyboard's ACL pieces (0: whole) */
    uint16_t acl_len, acl_total;
    uint8_t key[16];                        /* the keyboard's stored key (0s: none) */
    int has_key;
    /* seen */
    int linked, encrypted, scan_enable, rejected, accepted, accept_role, cancels, disconnects, enc_requests;
    int io_cap, io_oob, io_auth, confirmed, key_replies, key_negs, pin_len;
    uint8_t cc_params[13], key_sent[16];
    char pin[17];
    unsigned outstanding, max_outstanding, acl_from_host, bad_pb;
    int boot, unplugged, leds, conf_mtu_seen, proto_reqs;
    uint8_t last_cmd_reject;
    int got_pend, kb_calls, intr_before_ctrl;                 /* kb_calls: the keyboard called (it opens its channels itself) */
} C;

/* ---- the fake keyboard: L2CAP + HIDP ---- */
struct kch { uint16_t kcid, hcid; int conn, conf_in, conf_out, pending; };
static struct kch kc[2];                    /* 0 control (0x11), 1 interrupt (0x13) */
static uint8_t kb_rx[2048];
static unsigned kb_have, kb_need;
static uint8_t kb_ident = 0x80;

static void kb_acl(const uint8_t *frame, unsigned n)
{
    unsigned piece = C.frag ? (unsigned)C.frag : n;
    for (unsigned at = 0; at < n; at += piece) {
        unsigned k = n - at < piece ? n - at : piece;
        uint8_t b[300];
        uint16_t hf = (uint16_t)(HANDLE | (at ? 0x1000 : 0x2000));
        b[0] = (uint8_t)hf;
        b[1] = (uint8_t)(hf >> 8);
        b[2] = (uint8_t)k;
        b[3] = 0;
        memcpy(b + 4, frame + at, k);
        sched(2, 1, b, 4 + k);
    }
}
static void kb_frame(uint16_t cid, const uint8_t *p, unsigned n)
{
    uint8_t f[600];
    f[0] = (uint8_t)n;
    f[1] = (uint8_t)(n >> 8);
    f[2] = (uint8_t)cid;
    f[3] = (uint8_t)(cid >> 8);
    memcpy(f + 4, p, n);
    kb_acl(f, 4 + n);
}
static void kb_sig(uint8_t code, uint8_t id, const uint8_t *d, unsigned n)
{
    uint8_t c[64] = { code, id, (uint8_t)n, 0 };
    memcpy(c + 4, d, n);
    kb_frame(1, c, 4 + n);
}
static void kb_conn_req(int w)
{
    uint16_t psm = w ? 0x13 : 0x11;
    kc[w].kcid = (uint16_t)(0x0070 + w);
    uint8_t d[4] = { (uint8_t)psm, 0, (uint8_t)kc[w].kcid, 0 };
    kb_sig(0x02, kb_ident++, d, 4);
}
static void kb_conf_req(int w)
{
    /* MTU 48 (the smallest), a flush timeout, and an unknown hint option (0x80 | 0x20): all to be accepted */
    uint8_t d[4 + 4 + 4 + 3] = { (uint8_t)kc[w].hcid, (uint8_t)(kc[w].hcid >> 8), 0, 0, 0x01, 2, 48, 0,
                                 0x02, 2, 0xff, 0xff, 0xA0, 1, 7 };
    kb_sig(0x04, kb_ident++, d, sizeof(d));
}
static void kb_maybe_intr(void)
{
    /* a reconnecting keyboard opens the interrupt channel once control is configured both ways */
    if (C.kb_calls && kc[0].conf_in && kc[0].conf_out && !kc[1].kcid)
        kb_conn_req(1);
}
static int kb_which(uint16_t kcid)
{
    for (int w = 0; w < 2; w++)
        if (kc[w].kcid && kc[w].kcid == kcid)
            return w;
    return -1;
}
static void kb_report(uint8_t mods, uint8_t k1, uint8_t k2)
{
    uint8_t r[10] = { 0xA1, 0x01, mods, 0, k1, k2, 0, 0, 0, 0 };
    kb_frame(kc[1].hcid, r, 10);
}
static uint8_t sig_seen[2][32];             /* the host's first two signalling frames, byte for byte */
static unsigned sig_n[2], nsig;
static void kb_on_frame(const uint8_t *f, unsigned n)
{
    if (n >= 4 && f[2] == 1 && f[3] == 0 && nsig < 2 && n <= 32) {
        memcpy(sig_seen[nsig], f, n);
        sig_n[nsig++] = n;
    }
    uint16_t len = (uint16_t)(f[0] | f[1] << 8), cid = (uint16_t)(f[2] | f[3] << 8);
    const uint8_t *p = f + 4;
    if (len + 4U != n)
        return;
    if (cid == 1) {
        for (unsigned at = 0; at + 4 <= len;) {
            uint8_t code = p[at], id = p[at + 1];
            unsigned cl = p[at + 2] | p[at + 3] << 8;
            const uint8_t *d = p + at + 4;
            at += 4 + cl;
            if (verbose)
                printf("  [%6llu] kb <- signalling %02x id %02x len %u\n", (unsigned long long)T, code, id, cl);
            switch (code) {
            case 0x02: {                    /* the host opens a channel */
                int w = (d[0] | d[1] << 8) == 0x13;
                if (w && !kc[0].conf_in)
                    C.intr_before_ctrl = 1;
                kc[w].hcid = (uint16_t)(d[2] | d[3] << 8);
                kc[w].kcid = (uint16_t)(0x0070 + w);
                kc[w].conn = 1;
                uint8_t r[8] = { (uint8_t)kc[w].kcid, 0, d[2], d[3], 0, 0, 0, 0 };
                kb_sig(0x03, id, r, 8);
                kb_conf_req(w);
                break;
            }
            case 0x03: {                    /* the answer to ours */
                int w = kb_which((uint16_t)(d[2] | d[3] << 8));
                uint16_t res = (uint16_t)(d[4] | d[5] << 8);
                if (w < 0)
                    break;
                if (res == 1) {
                    kc[w].pending = 1;
                    C.got_pend++;
                } else if (res == 0) {
                    kc[w].hcid = (uint16_t)(d[0] | d[1] << 8);
                    kc[w].conn = 1;
                    kb_conf_req(w);
                }
                break;
            }
            case 0x04: {                    /* the host's configuration */
                int w = kb_which((uint16_t)(d[0] | d[1] << 8));
                if (w < 0)
                    break;
                if (cl >= 8 && d[4] == 0x01 && d[5] == 2 && (d[6] | d[7] << 8) == 672)
                    C.conf_mtu_seen++;
                uint8_t r[6] = { (uint8_t)kc[w].hcid, (uint8_t)(kc[w].hcid >> 8), 0, 0, 0, 0 };
                kb_sig(0x05, id, r, 6);
                kc[w].conf_in = 1;
                kb_maybe_intr();
                break;
            }
            case 0x05: {
                int w = kb_which((uint16_t)(d[0] | d[1] << 8));
                if (w >= 0 && !(d[4] | d[5] << 8))
                    kc[w].conf_out = 1;
                kb_maybe_intr();
                break;
            }
            case 0x06: {                    /* the host closes a channel */
                int w = kb_which((uint16_t)(d[0] | d[1] << 8));
                kb_sig(0x07, id, d, 4);
                if (w >= 0)
                    memset(&kc[w], 0, sizeof(kc[w]));
                break;
            }
            case 0x01:
                C.last_cmd_reject = d[0];
                break;
            default:
                break;
            }
        }
        return;
    }
    int w = kb_which(cid);
    if (w == 0 && len >= 1) {
        if (p[0] == 0x70) {
            C.boot = 1;
            C.proto_reqs++;
            if (C.answer_proto) {
                uint8_t h = 0x00;
                kb_frame(kc[0].hcid, &h, 1);
            }
        }
        if (p[0] == 0x15)
            C.unplugged = 1;
    }
    if (w == 1 && len == 3 && p[0] == 0xA2 && p[1] == 0x01)
        C.leds = p[2];
}
static void kb_host_acl(const uint8_t *a, unsigned n)
{
    uint16_t hf = (uint16_t)(a[0] | a[1] << 8), dl = (uint16_t)(a[2] | a[3] << 8);
    unsigned pb = (hf >> 12) & 3;
    C.acl_from_host++;
    C.outstanding++;
    if (C.outstanding > C.max_outstanding)
        C.max_outstanding = C.outstanding;
    ev(5, 0x13, 5, 1, HANDLE & 0xff, HANDLE >> 8, 1, 0);     /* Number Of Completed Packets */
    if ((hf & 0xfff) != HANDLE || dl + 4U != n || dl > C.acl_len) {
        C.bad_pb++;
        return;
    }
    if (pb == 2) {
        if (kb_have)
            C.bad_pb++;
        kb_have = 0;
    } else if (pb != 1 || !kb_have) {
        C.bad_pb++;
        return;
    }
    memcpy(kb_rx + kb_have, a + 4, dl);
    kb_have += dl;
    if (kb_have >= 4) {
        kb_need = 4U + (kb_rx[0] | kb_rx[1] << 8);
        if (kb_have >= kb_need) {
            kb_on_frame(kb_rx, kb_need);
            kb_have = 0;
        }
    }
}

static void link_down(uint8_t reason)
{
    if (!C.linked)
        return;
    C.linked = C.encrypted = 0;
    memset(kc, 0, sizeof(kc));
    kb_have = 0;
    ev(10, 0x05, 4, 0, HANDLE & 0xff, HANDLE >> 8, reason);
}

static void conn_complete(uint64_t dt, uint8_t status)
{
    uint8_t m[5] = { HANDLE & 0xff, HANDLE >> 8, 0, 0, 0 };
    uint8_t b[13] = { 0x03, 11, status };
    memcpy(b + 3, m, 2);
    memcpy(b + 5, KB_ADDR, 6);
    b[11] = 1;
    b[12] = 0;
    sched(dt, 0, b, 13);
    if (!status)
        C.linked = 1;
}

static void c_cmd(uint16_t op, const uint8_t *p, unsigned n)
{
    (void)n;
    switch (op) {
    case 0x1005: {
        uint8_t b[13] = { 0x0E, 11, 1, 0x05, 0x10, 0, (uint8_t)C.acl_len, (uint8_t)(C.acl_len >> 8), 64,
                          (uint8_t)C.acl_total, 0, 8, 0 };
        sched(1, 0, b, 13);
        break;
    }
    case 0x0C1A:
        C.scan_enable = p[0];
        cc(op, 0);
        break;
    case 0x0405:
        C.kb_calls = 0;
        memcpy(C.cc_params, p, 13);
        cs(op, 0);
        if (C.present)
            conn_complete(30, 0);
        else
            conn_complete(5120, 0x04);
        break;
    case 0x0408:
        C.cancels++;
        cc(op, 0);
        break;
    case 0x0409:
        C.accepted++;
        C.accept_role = p[6];
        C.kb_calls = 1;
        cs(op, 0);
        conn_complete(20, 0);
        /* the keyboard opens its control channel at once (before the link is encrypted) */
        memset(kc, 0, sizeof(kc));
        {
            uint64_t save = T;
            T += 30;
            kb_conn_req(0);
            T = save;
        }
        break;
    case 0x040A:
        C.rejected = p[6];
        cs(op, 0);
        break;
    case 0x0406:
        C.disconnects++;
        cs(op, 0);
        link_down(0x16);
        break;
    case 0x0411:
        cs(op, 0);
        ev_addr(5, 0x17, 0, 0);             /* Link Key Request */
        break;
    case 0x040B: {
        C.key_replies++;
        memcpy(C.key_sent, p + 6, 16);
        cc(op, 0);
        int ok = C.has_key && !memcmp(p + 6, C.key, 16);
        ev(10, 0x06, 3, ok ? 0 : 0x06, HANDLE & 0xff, HANDLE >> 8);
        break;
    }
    case 0x040C:
        C.key_negs++;
        cc(op, 0);
        if (C.ssp)
            ev_addr(5, 0x31, 0, 0);         /* IO Capability Request */
        else
            ev_addr(5, 0x16, 0, 0);         /* PIN Code Request */
        break;
    case 0x042B: {
        C.io_cap = p[6], C.io_oob = p[7], C.io_auth = p[8];
        cc(op, 0);
        uint8_t r[3] = { (uint8_t)C.io, 0, 0x00 };
        ev_addr(5, 0x32, r, 3);
        if (C.io == 2) {
            uint8_t pk[4] = { 42195 & 0xff, (42195 >> 8) & 0xff, 0, 0 };
            ev_addr(8, 0x3B, pk, 4);
        } else {
            uint8_t v[4] = { 0x40, 0xe2, 0x01, 0 };
            ev_addr(8, 0x33, v, 4);
        }
        break;
    }
    case 0x042C: {
        C.confirmed++;
        cc(op, 0);
        uint8_t kn[17];
        {
            uint8_t b[9] = { 0x36, 7, 0 };
            memcpy(b + 3, KB_ADDR, 6);
            sched(5, 0, b, 9);
        }
        memcpy(kn, KB_KEY, 16);
        kn[16] = 4;
        ev_addr(6, 0x18, kn, 17);
        memcpy(C.key, KB_KEY, 16);
        C.has_key = 1;
        ev(7, 0x06, 3, 0, HANDLE & 0xff, HANDLE >> 8);
        break;
    }
    case 0x040D:
        C.pin_len = p[6];
        memcpy(C.pin, p + 7, 16);
        C.pin[16] = 0;
        cc(op, 0);
        break;
    case 0x0413:
        C.enc_requests++;
        cs(op, 0);
        if (C.refuse_enc) {
            ev(5, 0x08, 4, 0x05, HANDLE & 0xff, HANDLE >> 8, 0);
        } else {
            C.encrypted = 1;
            ev(5, 0x08, 4, 0, HANDLE & 0xff, HANDLE >> 8, 1);
        }
        break;
    default:
        cc(op, 0);
        break;
    }
}

static unsigned bad_h4;                     /* sent while off, a wrong length, ACL with no link, an unknown type */
static void f_h4(const uint8_t *p, unsigned n)
{
    if (!chip_on) {
        bad_h4++;
        return;
    }
    if (p[0] == 1 && n == 4U + p[3]) {
        c_cmd((uint16_t)(p[1] | p[2] << 8), p + 4, p[3]);
    } else if (p[0] == 2 && C.linked) {
        kb_host_acl(p + 1, n - 1);
    } else {
        bad_h4++;
    }
}

/* the person types the code on the keyboard */
static void kb_enter_passkey(int right)
{
    for (int i = 0; i < 6; i++) {
        uint8_t t = 1;
        ev_addr(10 + (uint64_t)i * 10, 0x3C, &t, 1);
    }
    uint8_t b[9] = { 0x36, 7, (uint8_t)(right ? 0 : 0x05) };
    memcpy(b + 3, KB_ADDR, 6);
    sched(80, 0, b, 9);
    if (right) {
        uint8_t kn[17];
        memcpy(kn, KB_KEY, 16);
        kn[16] = 5;
        ev_addr(81, 0x18, kn, 17);
        memcpy(C.key, KB_KEY, 16);
        C.has_key = 1;
    }
    ev(82, 0x06, 3, right ? 0 : 0x05, HANDLE & 0xff, HANDLE >> 8);
}
static void kb_enter_pin(const char *pin)
{
    int ok = (unsigned)C.pin_len == strlen(pin) && !memcmp(C.pin, pin, strlen(pin));
    if (ok) {
        uint8_t kn[17];
        memcpy(kn, KB_KEY, 16);
        kn[16] = 0;
        ev_addr(5, 0x18, kn, 17);
        memcpy(C.key, KB_KEY, 16);
        C.has_key = 1;
    }
    ev(6, 0x06, 3, ok ? 0 : 0x05, HANDLE & 0xff, HANDLE >> 8);
}

/* ---- the loop ---- */
static unsigned untaken;                    /* events bt_pair.c did not take (every scripted one is its) */
static void run(uint64_t ms)
{
    for (uint64_t e = T + ms; T < e; T++) {
        for (unsigned i = 0; i < nitems;) {
            if (items[i].t <= T) {
                struct item it = items[i];
                memmove(items + i, items + i + 1, (nitems - i - 1) * sizeof(items[0]));
                nitems--;
                if (!chip_on)
                    continue;
                if (!it.acl && it.b[0] == 0x13 && C.outstanding)
                    C.outstanding--;
                if (it.acl) {
                    btp_acl(it.b, it.n);
                } else {
                    int k = btp_event(it.b);
                    if (k != 1)
                        untaken++;
                    if (k != 1 && verbose)
                        printf("  event %02x not taken\n", it.b[0]);
                }
            } else {
                i++;
            }
        }
        btp_step();
    }
}
static int run_until(int state, uint64_t max)
{
    for (uint64_t i = 0; i < max; i += 5) {
        run(5);
        if (bt_pair_state() == state)
            return 1;
    }
    return 0;
}

static const struct btp_io io = { f_h4, f_now, f_log, f_others, f_ready, f_on, f_random };

static struct bt_dev kb_dev(void)
{
    struct bt_dev d;
    memset(&d, 0, sizeof(d));
    memcpy(d.addr, KB_ADDR, 6);
    d.cod = 0x002540;                       /* peripheral, keyboard */
    d.psrm = 1;
    d.clkoff = 0x8000 | 0x1234;
    strcpy(d.name, "Test Keyboard");
    return d;
}

static void controller_reset(void)
{
    memset(&C, 0, sizeof(C));
    memset(kc, 0, sizeof(kc));
    kb_have = 0;
    nitems = 0;
    C.present = 1;
    C.ssp = 1;
    C.io = 2;                               /* KeyboardOnly */
    C.answer_proto = 1;
    C.acl_len = 1021;
    C.acl_total = 8;
}

static void type_hi(void)
{
    /* "Hi!\n": Shift+h, i, Shift+1, Enter, with every release */
    kb_report(0x02, 0, 0);
    run(10);
    kb_report(0x02, 0x0B, 0);
    run(10);
    kb_report(0x02, 0, 0);
    run(10);
    kb_report(0x00, 0, 0);
    run(10);
    kb_report(0x00, 0x0C, 0);
    run(10);
    kb_report(0x00, 0, 0);
    run(10);
    kb_report(0x20, 0x1E, 0);               /* the right Shift this time */
    run(10);
    kb_report(0x00, 0, 0);
    run(10);
    kb_report(0x00, 0x28, 0);
    run(10);
    kb_report(0x00, 0, 0);
    run(30);
}

static void hexs(char *o, const uint8_t *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        snprintf(o + 2 * i, 3, "%02x", b[i]);
}
static void no_secrets(const char *when)
{
    char a[13], k[33], ar[13];
    uint8_t rev[6];
    for (int i = 0; i < 6; i++)
        rev[i] = KB_ADDR[5 - i];
    hexs(a, KB_ADDR, 6);
    hexs(ar, rev, 6);
    hexs(k, KB_KEY, 16);
    char m[96];
    snprintf(m, sizeof m, "no address or key in the log (%s)", when);
    CHECK(!strstr(logbuf, a) && !strstr(logbuf, ar) && !strstr(logbuf, k) && !strstr(logbuf, "c1:c2") &&
              !strstr(logbuf, "C1:C2"), m);
}

/* ---- the scenarios ---- */
static void test_pure(void)
{
    CHECK(bthid_char(0x04, 0, 0) == 'a' && bthid_char(0x04, 0x02, 0) == 'A' && bthid_char(0x04, 0, 1) == 'A' &&
              bthid_char(0x04, 0x20, 1) == 'a', "letters with Shift and Caps Lock");
    CHECK(bthid_char(0x1E, 0, 0) == '1' && bthid_char(0x1E, 2, 0) == '!' && bthid_char(0x27, 0, 0) == '0' &&
              bthid_char(0x26, 2, 0) == '(', "digits and their shifted signs");
    CHECK(bthid_char(0x28, 0, 0) == '\n' && bthid_char(0x2A, 0, 0) == 8 && bthid_char(0x2B, 0, 0) == '\t' &&
              bthid_char(0x29, 0, 0) == 0x1b && bthid_char(0x2C, 2, 0) == ' ' && bthid_char(0x4C, 0, 0) == 0x7f,
          "Enter, Backspace, Tab, Esc, Space, Delete");
    CHECK(bthid_char(0x38, 0, 0) == '/' && bthid_char(0x38, 2, 0) == '?' && bthid_char(0x34, 2, 0) == '"' &&
              bthid_char(0x31, 0, 0) == '\\' && bthid_char(0x35, 2, 0) == '~', "punctuation");
    CHECK(bthid_char(0x4F, 0, 0) == 0 && bthid_char(0x52, 0, 0) == 0 && bthid_char(0x3A, 0, 0) == 0 &&
              bthid_char(0x04, 0x01, 0) == 0 && bthid_char(0x04, 0x80, 0) == 0, "arrows, F1, Ctrl+a, GUI+a: no char");
    CHECK(bthid_char(0x59, 0, 0) == '1' && bthid_char(0x62, 0, 0) == '0' && bthid_char(0x58, 0, 0) == '\n',
          "the keypad");

    struct bthid_kbd k;
    memset(&k, 0, sizeof(k));
    k.cb = key_cb;
    bthid_init(&k);
    typed_clear();
    const uint8_t r1[] = { 0xA1, 0x01, 0, 0, 0x04, 0x05, 0, 0, 0, 0 };
    const uint8_t r2[] = { 0xA1, 0x01, 0, 0, 0x05, 0, 0, 0, 0, 0 };
    const uint8_t roll[] = { 0xA1, 0x01, 0, 0, 1, 1, 1, 1, 1, 1 };
    const uint8_t noid[] = { 0xA1, 0, 0, 0x06, 0, 0, 0, 0, 0 };
    const uint8_t cons[] = { 0xA1, 0x03, 0xe9, 0x00 };
    const uint8_t shortr[] = { 0xA1, 0x01, 0 };
    CHECK(bthid_frame(&k, r1, sizeof r1) == 1 && !strcmp(typed, "ab") && downs == 2, "two keys down in one report");
    CHECK(bthid_frame(&k, r2, sizeof r2) == 1 && ups == 1 && downs == 2, "one up, the other still held");
    CHECK(bthid_frame(&k, roll, sizeof roll) == 1 && k.rollover == 1 && ups == 1, "ErrorRollOver ignored");
    CHECK(bthid_frame(&k, noid, sizeof noid) == 1 && !strcmp(typed, "abc") && ups == 2, "a report without its ID");
    CHECK(bthid_frame(&k, cons, sizeof cons) == 0 && bthid_frame(&k, shortr, sizeof shortr) == -1 &&
              bthid_frame(&k, cons, 0) == -1, "a consumer report ignored, a short one refused");
    bthid_release(&k);
    CHECK(ups == 3 && !k.keys[0], "release: every held key up");

    struct bt_dev d = kb_dev();
    CHECK(bt_is_keyboard(&d), "class 0x002540 is a keyboard");
    d.cod = 0x240404;
    CHECK(!bt_is_keyboard(&d) || strstr(d.name, "eyboard"), "a headset's class is not");
    strcpy(d.name, "Speaker");
    CHECK(!bt_is_keyboard(&d), "a speaker is not a keyboard");
    strcpy(d.name, "MX KEYBOARD mini");
    CHECK(bt_is_keyboard(&d), "a keyboard by its name");
    d.le = 1;
    CHECK(!bt_is_keyboard(&d), "LE devices are not taken");
}

static void test_pairing(void)
{
    btp_init(&io);
    bt_set_link_key_store(k_get, k_put);
    bt_hid_set_key_cb(key_cb);
    controller_reset();
    C.acl_len = 10;                         /* our frames go out in pieces */
    C.acl_total = 2;                        /* and wait for the controller's buffers */
    C.frag = 3;                             /* the keyboard's come in pieces of 3 */
    CHECK(bt_pair_state() == BT_PAIR_IDLE && !strcmp(bt_pair_why(), "No keyboard paired"), "idle at first");
    CHECK(btp_poll_ms() == 0, "nothing to poll while nothing is paired");

    struct bt_dev d = kb_dev();
    chip_ready = 0;                         /* a scan runs: the pairing waits for its end */
    CHECK(btp_pair_dev(&d, 1) == 0 && bt_pair_state() == BT_PAIR_CONNECTING, "pairing asked");
    CHECK(strstr(bt_pair_why(), "Connecting to Test Keyboard"), "its words name the keyboard");
    CHECK(!btp_scan_ok(), "no new scan while pairing");
    run(50);
    CHECK(C.acl_from_host == 0 && !C.cc_params[0] && !C.linked, "nothing paged while the scan runs");
    chip_ready = 1;
    CHECK(run_until(BT_PAIR_CODE, 2000), "the passkey shown");
    CHECK(C.cc_params[6] == 0x18 && C.cc_params[7] == 0xCC && C.cc_params[8] == 1 && C.cc_params[9] == 0 &&
              C.cc_params[10] == 0x34 && C.cc_params[11] == 0x92 && C.cc_params[12] == 1 &&
              !memcmp(C.cc_params, KB_ADDR, 6),
          "Create_Connection: DM/DH packets, the inquiry's page scan mode and clock offset, role switch allowed");
    CHECK(C.key_negs == 1 && C.key_replies == 0, "a fresh pairing: no stored key offered");
    CHECK(C.io_cap == 0x00 && C.io_oob == 0 && C.io_auth == 0x03, "IO capability: DisplayOnly, MITM, dedicated bond");
    CHECK(bt_pair_passkey() == 42195 && !strcmp(bt_pair_code(), "042195"), "the passkey, 6 digits");
    CHECK(!strcmp(bt_pair_why(), "Type 042195 on the keyboard, then press Enter"), "the words to show");
    CHECK(bt_pair_digits() == 0, "no digits typed yet");
    kb_enter_passkey(1);
    run(45);
    CHECK(bt_pair_digits() >= 3 && bt_pair_state() == BT_PAIR_CODE, "the digits counted as they are typed");
    CHECK(run_until(BT_PAIR_CONNECTED, 3000), "connected after the passkey");
    CHECK(st_have && !memcmp(st_key, KB_KEY, 16) && !memcmp(st_addr, KB_ADDR, 6) && st_puts == 1,
          "the link key went to the OS's store");
    uint8_t pa[6];
    CHECK(bt_paired(pa) && !memcmp(pa, KB_ADDR, 6), "paired");
    CHECK(C.enc_requests == 1 && C.encrypted, "encryption asked and on");
    CHECK(!C.intr_before_ctrl, "the interrupt channel opened only after control");
    {
        /* Core vol 3 part A 4.2 / 4.4, encoded by hand: Connection Request (PSM 0x0011, source CID 0x0040), then
         * Configuration Request (destination CID = the keyboard's 0x0070, flags 0, MTU option 672) */
        static const uint8_t creq[] = { 0x08, 0x00, 0x01, 0x00, 0x02, 0x01, 0x04, 0x00, 0x11, 0x00, 0x40, 0x00 };
        static const uint8_t cfg[] = { 0x0c, 0x00, 0x01, 0x00, 0x04, 0x02, 0x08, 0x00, 0x70, 0x00, 0x00, 0x00,
                                       0x01, 0x02, 0xa0, 0x02 };
        CHECK(sig_n[0] == sizeof creq && !memcmp(sig_seen[0], creq, sizeof creq), "Connection Request, byte for byte");
        CHECK(sig_n[1] == sizeof cfg && !memcmp(sig_seen[1], cfg, sizeof cfg), "Configuration Request, byte for byte");
    }
    CHECK(C.boot == 1 && C.conf_mtu_seen == 2, "SET_PROTOCOL(boot) sent; both channels configured with MTU 672");
    CHECK(C.scan_enable == 2, "page scan on while paired");
    CHECK(bt_hid_connected() && !strcmp(bt_pair_why(), "Keyboard connected"), "the words: connected");
    CHECK(C.bad_pb == 0, "every ACL packet from the host well formed (fragments marked start / continuation)");
    struct btp_stats s;
    btp_stats(&s);
    if (verbose)
        printf("boot %d mtu_seen %d acl_mtu %u qmax %u maxout %u\n", C.boot, C.conf_mtu_seen, s.acl_mtu, s.acl_queued_max,
               C.max_outstanding);
    CHECK(s.acl_mtu == 10 && s.acl_queued_max >= 1 && C.max_outstanding <= 2,
          "fragments of 10 bytes; queued while the controller's 2 buffers were busy");
    CHECK(btp_scan_ok(), "scans allowed again once connected");
    CHECK(btp_poll_ms() > 0 && btp_poll_ms() <= 10, "polled every 10 ms while connected");

    typed_clear();
    type_hi();
    CHECK(!strcmp(typed, "Hi!\n"), "typed \"Hi!\\n\" arrives as characters");
    CHECK(ups == downs, "every key down has its up");
    CHECK(bt_hid_mods() == 0, "no modifier held");

    /* Caps Lock: the LED report goes back */
    typed_clear();
    kb_report(0, HID_KEY_CAPS, 0);
    run(10);
    kb_report(0, 0, 0);
    run(10);
    kb_report(0, 0x04, 0);
    run(10);
    kb_report(0, 0, 0);
    run(30);
    CHECK(C.leds == 0x02 && !strcmp(typed, "A"), "Caps Lock: its LED report sent; 'a' gives 'A'");
    kb_report(0, HID_KEY_CAPS, 0);
    run(10);
    kb_report(0, 0, 0);
    run(30);
    CHECK(C.leds == 0x00, "Caps Lock off: the LED off");
    no_secrets("pairing");
}

static void test_malformed(void)
{
    /* every kind of broken frame while connected: dropped, nothing breaks */
    struct btp_stats s0, s1;
    btp_stats(&s0);
    C.frag = 0;
    uint8_t b[64];
    /* 1: an L2CAP length that disagrees with the packet */
    uint8_t f1[] = { 0x42, 0x20, 6, 0, 1, 0, 0x40, 0, 1, 2 };
    sched(1, 1, f1, sizeof f1);
    /* 2: a signalling command longer than its frame */
    uint8_t f2[] = { 0x42, 0x20, 12, 0, 8, 0, 1, 0, 0x02, 9, 40, 0, 0x11, 0, 0x50, 0 };
    sched(1, 1, f2, sizeof f2);
    /* 3: a continuation with nothing started */
    uint8_t f3[] = { 0x42, 0x10, 3, 0, 1, 2, 3 };
    sched(1, 1, f3, sizeof f3);
    /* 4: a frame for a channel that does not exist */
    uint8_t f4[] = { 0x42, 0x20, 6, 0, 2, 0, 0x77, 0x77, 0xA1, 0x01 };
    sched(1, 1, f4, sizeof f4);
    /* 5: a frame announcing 4000 bytes, in pieces: dropped whole */
    uint8_t f5[] = { 0x42, 0x20, 6, 0, 0xa0, 0x0f, 0x41, 0, 0xA1, 0x01 };
    sched(1, 1, f5, sizeof f5);
    for (int i = 0; i < 5; i++) {
        memset(b, 0, sizeof b);
        b[0] = 0x42, b[1] = 0x10, b[2] = 40;
        sched(1, 1, b, 44);
    }
    /* 6: an unknown signalling code: Command Reject (not understood) */
    uint8_t f6[] = { 0x42, 0x20, 8, 0, 4, 0, 1, 0, 0x7e, 0x33, 0, 0 };
    sched(1, 1, f6, sizeof f6);
    /* 7: a short HID report on the interrupt channel */
    uint8_t f7[] = { 0x42, 0x20, 7, 0, 3, 0, (uint8_t)kc[1].hcid, 0, 0xA1, 0x01, 0 };
    sched(1, 1, f7, sizeof f7);
    /* 8: an ACL packet for another handle; a wrong ACL length; a zero identifier */
    uint8_t f8[] = { 0x43, 0x20, 4, 0, 0, 0, 1, 0 };
    sched(1, 1, f8, sizeof f8);
    uint8_t f9[] = { 0x42, 0x20, 9, 0, 0, 0 };
    sched(1, 1, f9, sizeof f9);
    uint8_t f10[] = { 0x42, 0x20, 8, 0, 4, 0, 1, 0, 0x08, 0x00, 0, 0 };
    sched(1, 1, f10, sizeof f10);
    /* 9: an echo request: answered */
    uint8_t f11[] = { 0x42, 0x20, 10, 0, 6, 0, 1, 0, 0x08, 0x21, 2, 0, 'h', 'i' };
    sched(1, 1, f11, sizeof f11);
    run(60);
    btp_stats(&s1);
    if (verbose)
        printf("malformed %u -> %u, dropped %u -> %u\n", s0.l2_malformed, s1.l2_malformed, s0.l2_dropped, s1.l2_dropped);
    CHECK(s1.l2_malformed >= s0.l2_malformed + 5, "the malformed frames counted");
    CHECK(s1.l2_dropped >= s0.l2_dropped + 3, "the stray / unknown / oversized frames dropped");
    CHECK(C.last_cmd_reject == 0x00, "an unknown command: Command Reject, not understood");
    CHECK(bt_hid_connected(), "the keyboard still connected");
    typed_clear();
    kb_report(0, 0x1B, 0);                  /* x */
    run(10);
    kb_report(0, 0, 0);
    run(30);
    CHECK(!strcmp(typed, "x"), "and still typing");
}

static void test_disconnect_mid_typing(void)
{
    typed_clear();
    kb_report(0x02, 0x04, 0);               /* Shift+a held */
    run(20);
    CHECK(!strcmp(typed, "A") && downs == 2, "a key held");
    link_down(0x13);                        /* the keyboard goes away (switched off) */
    run(50);
    CHECK(ups == 2, "the held key and Shift released when the link went");
    CHECK(bt_pair_state() == BT_PAIR_WAITING && !bt_hid_connected(), "waiting for the keyboard to come back");
    CHECK(strstr(bt_pair_why(), "The keyboard disconnected"), "the words say so");
    CHECK(C.scan_enable == 2 && btp_poll_ms() == 50, "page scan still on; polled every 50 ms");
}

static void test_reconnect(void)
{
    /* an unknown device calls: refused */
    uint8_t other[10] = { 1, 2, 3, 4, 5, 6, 0x40, 0x25, 0x00, 1 };
    uint8_t b[12] = { 0x04, 10 };
    memcpy(b + 2, other, 10);
    sched(1, 0, b, 12);
    run(20);
    CHECK(C.rejected == 0x0F && !C.accepted, "an unknown device's connection refused (unacceptable address)");

    /* the keyboard calls (a key pressed): accepted with the stored key */
    C.got_pend = 0;
    uint8_t m[4] = { 0x40, 0x25, 0x00, 1 };
    ev_addr(1, 0x04, m, 4);
    CHECK(run_until(BT_PAIR_CONNECTED, 3000), "the keyboard connected again by itself");
    CHECK(C.accepted == 1 && C.accept_role == 0x01, "Accept_Connection_Request, staying peripheral");
    CHECK(C.key_replies == 1 && !memcmp(C.key_sent, KB_KEY, 16), "the stored key given for its Link_Key_Request");
    CHECK(C.got_pend >= 1, "its control channel held 'pending' until the link was encrypted");
    CHECK(C.proto_reqs >= 2, "boot protocol set again");
    typed_clear();
    kb_report(0, 0x12, 0);                  /* o */
    run(10);
    kb_report(0, 0x0E, 0);                  /* o released, k down (one report) */
    run(10);
    kb_report(0, 0, 0);
    run(30);
    CHECK(!strcmp(typed, "ok") && ups == 2, "typing after the reconnect");
    no_secrets("reconnect");
}

static void test_reconnect_key_lost(void)
{
    /* host-initiated reconnect, but the keyboard was reset and lost its key */
    link_down(0x13);
    run(50);
    C.has_key = 0;
    CHECK(bt_hid_reconnect() == 0, "a reconnect asked");
    CHECK(run_until(BT_PAIR_FAILED, 3000), "it fails");
    CHECK(strstr(bt_pair_why(), "no longer knows this phone"), "the words: pair it again");
    CHECK(!st_have && !bt_paired(0), "the dead key forgotten");
    run(50);
    CHECK(C.scan_enable == 0, "page scan off once nothing is paired");
    CHECK(!C.linked, "the link closed");
}

static void test_wrong_passkey(void)
{
    controller_reset();
    struct bt_dev d = kb_dev();
    CHECK(btp_pair_dev(&d, 1) == 0, "pairing asked again");
    CHECK(run_until(BT_PAIR_CODE, 2000), "the passkey shown");
    kb_enter_passkey(0);
    CHECK(run_until(BT_PAIR_FAILED, 2000), "a wrong passkey: failed");
    CHECK(!strcmp(bt_pair_why(), "The code was not typed right on the keyboard: pairing failed. Try again"),
          "the words for a wrong code");
    run(50);
    CHECK(C.disconnects == 1 && !C.linked, "the link disconnected");
    CHECK(!st_have && !bt_paired(0), "no key stored");
    CHECK(btp_scan_ok() && btp_poll_ms() == 0, "nothing left running");
}

static void test_just_works_and_unpair(void)
{
    controller_reset();
    C.io = 3;                               /* NoInputNoOutput */
    struct bt_dev d = kb_dev();
    int saw_code = 0;
    CHECK(btp_pair_dev(&d, 1) == 0, "pairing a Just Works keyboard");
    for (int i = 0; i < 400 && bt_pair_state() != BT_PAIR_CONNECTED; i++) {
        run(5);
        saw_code |= bt_pair_state() == BT_PAIR_CODE;
    }
    CHECK(bt_pair_state() == BT_PAIR_CONNECTED && !saw_code && C.confirmed == 1, "Just Works: no code, confirmed");
    CHECK(st_have, "its key stored");
    bt_unpair();
    run(100);
    CHECK(C.unplugged && C.disconnects == 1 && !C.linked, "unpair: virtual cable unplug sent, then Disconnect");
    CHECK(!st_have && bt_pair_state() == BT_PAIR_IDLE && !strcmp(bt_pair_why(), "No keyboard paired"),
          "unpair: key forgotten, idle");
    CHECK(C.scan_enable == 0, "page scan off");
}

static void test_legacy_pin(void)
{
    controller_reset();
    C.ssp = 0;
    struct bt_dev d = kb_dev();
    CHECK(btp_pair_dev(&d, 1) == 0, "pairing a legacy (PIN) keyboard");
    CHECK(run_until(BT_PAIR_CODE, 2000), "a PIN shown");
    const char *code = bt_pair_code();
    CHECK(strlen(code) == 6 && C.pin_len == 6 && !strcmp(C.pin, code), "the PIN given to the controller is the one shown");
    char m[64];
    snprintf(m, sizeof m, "Type %s on the keyboard, then press Enter", code);
    CHECK(!strcmp(bt_pair_why(), m), "the words to show");
    kb_enter_pin(code);
    CHECK(run_until(BT_PAIR_CONNECTED, 3000), "connected after the PIN");
    CHECK(st_have && !memcmp(st_key, KB_KEY, 16), "its key stored");
    bt_unpair();
    run(100);
    CHECK(bt_pair_state() == BT_PAIR_IDLE, "unpaired");
}

static void test_failures(void)
{
    /* the keyboard is not there: page timeout */
    controller_reset();
    C.present = 0;
    struct bt_dev d = kb_dev();
    CHECK(btp_pair_dev(&d, 1) == 0, "pairing a keyboard that is not there");
    CHECK(run_until(BT_PAIR_FAILED, 8000), "page timeout: failed");
    CHECK(strstr(bt_pair_why(), "is it on and in pairing mode"), "the words for a page timeout");

    /* encryption refused */
    controller_reset();
    C.refuse_enc = 1;
    CHECK(btp_pair_dev(&d, 1) == 0, "pairing, encryption to be refused");
    CHECK(run_until(BT_PAIR_CODE, 2000), "code shown");
    kb_enter_passkey(1);
    CHECK(run_until(BT_PAIR_FAILED, 3000), "encryption refused: failed");
    CHECK(!strcmp(bt_pair_why(), "The keyboard did not accept encryption"), "the words for it");
    run(50);
    CHECK(!C.linked, "disconnected");
    bt_unpair();

    /* SET_PROTOCOL never answered: connected anyway after 3 s */
    controller_reset();
    C.answer_proto = 0;
    CHECK(btp_pair_dev(&d, 1) == 0, "pairing, SET_PROTOCOL unanswered");
    CHECK(run_until(BT_PAIR_CODE, 2000), "code shown");
    kb_enter_passkey(1);
    CHECK(run_until(BT_PAIR_CONNECTED, 6000), "connected without the handshake");
    CHECK(logged("no answer to SET_PROTOCOL"), "logged");

    /* a second pairing while one is connected: refused */
    CHECK(btp_pair_dev(&d, 1) == 0, "pairing the connected keyboard again: nothing to do");
    struct bt_dev o = d;
    o.addr[0] ^= 1;
    CHECK(btp_pair_dev(&o, 1) < 0 && strstr(bt_pair_why(), "remove it first"), "another keyboard: refused");
    CHECK(bt_pair_state() == BT_PAIR_CONNECTED, "the connected one untouched");

    /* the chip goes off under a connected keyboard: forgotten, nothing sent */
    typed_clear();
    kb_report(0, 0x04, 0);
    run(20);
    chip_on = 0;
    btp_chip_off();
    run(50);
    CHECK(ups == 1 && !bt_hid_connected() && bt_pair_state() == BT_PAIR_WAITING, "chip off: keys up, link gone");
    chip_on = 1;
    C.linked = 0;
    nitems = 0;
    run(100);
    CHECK(C.scan_enable == 2, "chip on again: setup again, page scan on (paired)");
    bt_unpair();
    run(50);

    /* the passkey never typed: the pairing gives up (90 s) */
    controller_reset();
    CHECK(btp_pair_dev(&d, 1) == 0, "pairing, the code never typed");
    CHECK(run_until(BT_PAIR_CODE, 2000), "code shown");
    CHECK(run_until(BT_PAIR_FAILED, 95000), "given up");
    CHECK(strstr(bt_pair_why(), "not typed in time"), "the words for it");
    run(100);
    CHECK(!C.linked, "disconnected");

    /* a classic-only check and a busy command slot */
    struct bt_dev le = d;
    le.le = 1;
    CHECK(btp_pair_dev(&le, 1) < 0 && strstr(bt_pair_why(), "classic"), "an LE device refused");
    controller_reset();
    others_busy = 1;
    CHECK(btp_pair_dev(&d, 1) == 0, "pairing while bt.c's command is in flight");
    run(100);
    CHECK(!C.cc_params[0] && !C.linked, "our commands wait for bt.c's");
    others_busy = 0;
    CHECK(run_until(BT_PAIR_CODE, 2000), "then go");
    bt_unpair();
    run(100);
    CHECK(bt_pair_state() == BT_PAIR_IDLE && !C.linked, "unpaired mid-pairing: closed, idle");
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    test_pure();
    test_pairing();
    test_malformed();
    test_disconnect_mid_typing();
    test_reconnect();
    test_reconnect_key_lost();
    test_wrong_passkey();
    test_just_works_and_unpair();
    test_legacy_pin();
    test_failures();
    no_secrets("the end");
    CHECK(untaken == 0, "every scripted event and command answer taken by bt_pair.c");
    CHECK(bad_h4 == 0, "every H4 packet well formed, none while the chip is off, ACL only on a link");
    printf("bthid-test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
