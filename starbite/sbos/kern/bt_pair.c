/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: one classic Bluetooth HID keyboard over the BCM4361 (bt.c): link setup, paging, pairing, encryption,
 * HID channels, boot protocol, reconnects. Pure C (bt_pair.h has the glue, bt.h the API). Single-threaded, bounded:
 * every step is a few HCI commands queued one at a time (each answered or given up after 2 s) and timers.
 *
 * Sources: Bluetooth Core spec 5.x vol 4 part E (HCI): Create_Connection 7.1.5, Disconnect 7.1.6,
 * Create_Connection_Cancel 7.1.7, Accept / Reject_Connection_Request 7.1.8-9, Link_Key_Request_(Negative_)Reply
 * 7.1.10-11, PIN_Code_Request_(Negative_)Reply 7.1.12-13, Authentication_Requested 7.1.15, Set_Connection_Encryption
 * 7.1.16, IO_Capability_Request_Reply 7.1.29, User_Confirmation_Request_(Negative_)Reply 7.1.30-31,
 * User_Passkey_Request_Negative_Reply 7.1.33, IO_Capability_Request_Negative_Reply 7.1.36, Write_Default_Link_Policy
 * 7.2.12, Set_Event_Mask 7.3.1, Write_Scan_Enable 7.3.18, Write_Simple_Pairing_Mode 7.3.59, Read_Buffer_Size 7.4.5;
 * the events of 7.7 (Connection Complete 0x03, Connection Request 0x04, Disconnection Complete 0x05, Authentication
 * Complete 0x06, Encryption Change 0x08, Number Of Completed Packets 0x13, PIN Code Request 0x16, Link Key Request
 * 0x17, Link Key Notification 0x18, IO Capability Request / Response 0x31 / 0x32, User Confirmation Request 0x33,
 * User Passkey Request 0x34, Simple Pairing Complete 0x36, User Passkey Notification 0x3B, Keypress Notification
 * 0x3C); vol 3 part C 5.2.2 (SSP association models: a DisplayOnly host and a KeyboardOnly keyboard use Passkey Entry,
 * the controller makes the passkey and the host shows it). Bluetooth HID Profile 1.1: the host opens control (0x11)
 * then interrupt (0x13), SET_PROTOCOL(boot), a reconnecting keyboard opens them itself. No SDP (boot protocol).
 *
 * Our side: IO capability DisplayOnly, MITM wanted, dedicated bonding. Keys go to the OS's store through bt.h's
 * callbacks and are wiped from RAM here; addresses and keys are never logged. Seen on a phone: not yet (host-tested,
 * kern/host/bthid-test.c). */
#include "bt.h"
#include "bt_pair.h"
#include "bt_l2cap.h"
#include "bt_hid.h"

#define CMD_MS 2000U
#define PAIR_MS 90000U                      /* bt_pair to connected (the passkey typed within it) */
#define RECONNECT_MS 30000U
#define HID_MS 10000U                       /* encrypted to HID channels up */
#define PROTO_MS 3000U                      /* SET_PROTOCOL's handshake */
#define INCOMING_WAIT_MS 4000U              /* a reconnecting keyboard opens its channels itself within this */
#define CLOSE_MS 3000U

#define OP_CREATE_CONN 0x0405
#define OP_DISCONNECT 0x0406
#define OP_CREATE_CANCEL 0x0408
#define OP_ACCEPT_CONN 0x0409
#define OP_REJECT_CONN 0x040A
#define OP_LINK_KEY_REPLY 0x040B
#define OP_LINK_KEY_NEG 0x040C
#define OP_PIN_REPLY 0x040D
#define OP_PIN_NEG 0x040E
#define OP_AUTH_REQ 0x0411
#define OP_SET_ENC 0x0413
#define OP_IO_CAP_REPLY 0x042B
#define OP_CONFIRM_REPLY 0x042C
#define OP_CONFIRM_NEG 0x042D
#define OP_PASSKEY_NEG 0x042F
#define OP_IO_CAP_NEG 0x0434
#define OP_LINK_POLICY 0x080F
#define OP_EVENT_MASK 0x0C01
#define OP_SCAN_ENABLE 0x0C1A
#define OP_SSP_MODE 0x0C56
#define OP_BUFFER_SIZE 0x1005

#define EV_CONN_COMPLETE 0x03
#define EV_CONN_REQUEST 0x04
#define EV_DISCONN_COMPLETE 0x05
#define EV_AUTH_COMPLETE 0x06
#define EV_ENC_CHANGE 0x08
#define EV_NUM_COMPLETED 0x13
#define EV_PIN_REQUEST 0x16
#define EV_LINK_KEY_REQUEST 0x17
#define EV_LINK_KEY_NOTIFY 0x18
#define EV_IO_CAP_REQUEST 0x31
#define EV_IO_CAP_RESPONSE 0x32
#define EV_CONFIRM_REQUEST 0x33
#define EV_PASSKEY_REQUEST 0x34
#define EV_SSP_COMPLETE 0x36
#define EV_PASSKEY_NOTIFY 0x3B
#define EV_KEYPRESS_NOTIFY 0x3C

/* ---- state ---- */
static const struct btp_io *io;
static int ps = BT_PAIR_IDLE;
static const char *why = "No keyboard paired";
static char whyb[80];

static bt_key_get_fn kget;
static bt_key_put_fn kput;
static uint8_t paired_addr[6];
static int paired;

/* the link (one) */
enum { LP_NONE = 0, LP_START, LP_PAGING, LP_INCOMING, LP_SECURING, LP_HID, LP_PROTO, LP_UP, LP_CLOSING };
static struct {
    int lp, outgoing, pairing, have_handle, encrypted, authed, enc_sent, failing, unpairing, code_shown, digits;
    int key_valid, ssp_status, remote_io;
    uint16_t handle;
    uint8_t addr[6], psrm;
    uint16_t clkoff;
    char name[33];
    uint64_t deadline, phase_dead, self_open_at;
    uint8_t key[16];
    char code[8];
    uint32_t passkey;
    struct l2ch *ctrl, *intr;
} L;

static struct l2cap l2;
static struct bthid_kbd kbd;

/* commands: one in flight */
#define QN 12
struct qcmd {
    uint16_t op;
    uint8_t n;
    uint8_t p[24];
};
static struct qcmd q[QN];
static unsigned qh, qn;
static int inflight;
static uint16_t inflight_op;
static uint64_t inflight_dead;

/* ACL out: the controller's buffers */
#define AQN 12
#define AQ_MAX (4U + 4U + 132U)
static uint8_t aq[AQN][1 + AQ_MAX];
static uint16_t aqlen[AQN];
static unsigned aqh, aqn;
static uint16_t acl_mtu = 1021, acl_total = 8, credits = 8;

static int setup_done, setup_queued, scan_cur = -1, scan_queued;
static struct btp_stats S;

/* ---- small helpers ---- */
static uint64_t now(void)
{
    return io ? io->now_ms() : 0;
}

static void say(const char *s)
{
    if (io && io->log)
        io->log(s);
}

static void wipe(void *d, unsigned n)
{
    volatile uint8_t *p = d;
    while (n--)
        *p++ = 0;
}

static int same(const uint8_t *a, const uint8_t *b)
{
    for (unsigned i = 0; i < 6; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static void set_why(const char *a, const char *b, const char *c)
{
    struct btl l;
    btl_init(&l);
    btl_s(&l, a);
    btl_s(&l, b);
    btl_s(&l, c);
    unsigned n = l.n < sizeof(whyb) - 1 ? l.n : (unsigned)sizeof(whyb) - 1;
    bthci_copy(whyb, l.b, n);
    whyb[n] = 0;
    why = whyb;
}

static void logs(const char *a, uint64_t v, int hex, const char *b)
{
    struct btl l;
    btl_init(&l);
    btl_s(&l, "bt: ");
    btl_s(&l, a);
    if (hex)
        btl_x(&l, v, hex);
    else if (b)
        btl_u(&l, v);
    if (b)
        btl_s(&l, b);
    say(l.b);
}

/* ---- the command queue ---- */
static void queue(uint16_t op, const uint8_t *p, unsigned n)
{
    if (qn >= QN || n > sizeof(q[0].p)) {
        logs("a command could not be queued (queue full): ", op, 4, "");
        return;
    }
    struct qcmd *c = &q[(qh + qn) % QN];
    c->op = op;
    c->n = (uint8_t)n;
    bthci_copy(c->p, p, n);
    qn++;
}

static void queue_addr(uint16_t op, const uint8_t *addr, const uint8_t *more, unsigned m)
{
    uint8_t p[24];
    bthci_copy(p, addr, 6);
    if (m)
        bthci_copy(p + 6, more, m);
    queue(op, p, 6 + m);
}

static void queue_handle(uint16_t op, uint8_t extra, int with_extra)
{
    uint8_t p[3] = { (uint8_t)L.handle, (uint8_t)(L.handle >> 8), extra };
    queue(op, p, with_extra ? 3U : 2U);
}

static void send_next(void)
{
    if (inflight || !qn || !io || !io->chip_on() || (io->others_cmd && io->others_cmd()))
        return;
    struct qcmd *c = &q[qh];
    uint8_t pkt[4 + 24];
    unsigned k = bthci_cmd(pkt, sizeof(pkt), c->op, c->p, c->n);
    inflight = 1;
    inflight_op = c->op;
    inflight_dead = now() + CMD_MS;
    wipe(c->p, sizeof(c->p));               /* link keys and PINs do not stay in the queue */
    qh = (qh + 1) % QN;
    qn--;
    S.cmds++;
    io->h4(pkt, k);
    wipe(pkt, sizeof(pkt));
}

/* ---- ACL out ---- */
static void acl_send_now(const uint8_t *pkt, unsigned n)
{
    uint8_t b[1 + AQ_MAX];
    b[0] = H4_ACL;
    bthci_copy(b + 1, pkt, n);
    credits--;
    S.acl_out++;
    io->h4(b, n + 1);
}

static void drain_acl(void)
{
    while (aqn && credits && io && io->chip_on()) {
        acl_send_now(aq[aqh] + 1, aqlen[aqh]);
        aqh = (aqh + 1) % AQN;
        aqn--;
    }
}

static void l2_send_acl(void *ctx, const uint8_t *pkt, unsigned n)
{
    (void)ctx;
    if (n > AQ_MAX)
        return;
    if (!aqn && credits) {
        acl_send_now(pkt, n);
        return;
    }
    if (aqn >= AQN) {
        S.acl_dropped++;
        return;
    }
    unsigned i = (aqh + aqn) % AQN;
    bthci_copy(aq[i] + 1, pkt, n);
    aqlen[i] = (uint16_t)n;
    aqn++;
    if (aqn > S.acl_queued_max)
        S.acl_queued_max = aqn;
}

/* ---- the scan (page scan while paired) ---- */
static void sync_scan(void)
{
    int want = paired ? 2 : 0;
    if (!setup_done || scan_queued || want == scan_cur || !io->chip_on())
        return;
    uint8_t v = (uint8_t)want;
    queue(OP_SCAN_ENABLE, &v, 1);
    scan_queued = want + 1;
}

/* ---- the link's end ---- */
static void public_idle(void)
{
    if (paired) {
        ps = BT_PAIR_WAITING;
        why = "Keyboard paired: press a key on it to connect";
    } else {
        ps = BT_PAIR_IDLE;
        why = "No keyboard paired";
    }
}

static void link_reset(void)
{
    L.lp = LP_NONE;                         /* first: the channels' closed() calls see no link to fail */
    l2_link_down(&l2);
    bthid_release(&kbd);
    wipe(L.key, sizeof(L.key));
    wipe(L.code, sizeof(L.code));
    L.lp = LP_NONE;
    L.have_handle = 0;
    L.ctrl = L.intr = 0;
    L.key_valid = 0;
    L.passkey = 0;
    L.code_shown = 0;
    aqn = 0;
    credits = acl_total;
}

static void finish(void)
{
    int failing = L.failing, unpairing = L.unpairing;
    link_reset();
    L.failing = L.unpairing = 0;
    if (unpairing || !failing)
        public_idle();
    else
        ps = BT_PAIR_FAILED;
    sync_scan();
}

static void fail_link(const char *words, uint8_t reason)
{
    if (L.lp == LP_NONE || L.lp == LP_CLOSING)
        return;
    if (!L.unpairing) {
        L.failing = 1;
        why = words;
        struct btl l;
        btl_init(&l);
        btl_s(&l, "bt: keyboard: ");
        btl_s(&l, words);
        say(l.b);
    }
    if (L.have_handle) {
        if (L.ctrl && L.unpairing) {
            static const uint8_t unplug = HIDP_VC_UNPLUG;
            l2_send(&l2, L.ctrl, &unplug, 1);
        }
        queue_handle(OP_DISCONNECT, reason, 1);
        L.lp = LP_CLOSING;
        L.phase_dead = now() + CLOSE_MS;
        return;
    }
    if (L.lp == LP_PAGING)
        queue_addr(OP_CREATE_CANCEL, L.addr, 0, 0);
    finish();
}

/* ---- HID ---- */
static void connected(void)
{
    L.lp = LP_UP;
    L.deadline = 0;
    L.phase_dead = 0;
    ps = BT_PAIR_CONNECTED;
    why = "Keyboard connected";
    say(L.pairing ? "bt: keyboard: paired and connected (HID boot protocol)"
                  : "bt: keyboard: connected again (HID boot protocol, the stored key)");
}

static void start_hid(void)
{
    L.lp = LP_HID;
    L.phase_dead = now() + HID_MS;
    L.self_open_at = L.outgoing ? 0 : now() + INCOMING_WAIT_MS;
    ps = BT_PAIR_SECURING;
    why = "Setting up the keyboard";
    if (L.outgoing && !L.ctrl)
        L.ctrl = l2_connect(&l2, HID_PSM_CTRL);
}

static void both_open(void)
{
    static const uint8_t setp = HIDP_SET_PROTOCOL_BOOT;
    if (!L.ctrl || !L.intr || L.ctrl->state != L2C_OPEN || L.intr->state != L2C_OPEN || L.lp != LP_HID)
        return;
    L.lp = LP_PROTO;
    L.phase_dead = now() + PROTO_MS;
    l2_send(&l2, L.ctrl, &setp, 1);
}

static uint16_t l2_accept(void *ctx, uint16_t psm)
{
    (void)ctx;
    if ((psm != HID_PSM_CTRL && psm != HID_PSM_INTR) || L.lp < LP_SECURING || L.lp == LP_CLOSING)
        return L2_CR_BAD_PSM;
    if (psm == HID_PSM_INTR && !l2_find_psm(&l2, HID_PSM_CTRL))
        return L2_CR_BAD_PSM;               /* the control channel comes first (HID 1.1) */
    return L.encrypted && L.authed ? L2_CR_SUCCESS : L2_CR_PEND;
}

static void l2_opened(void *ctx, struct l2ch *c)
{
    (void)ctx;
    if (c->psm == HID_PSM_CTRL) {
        L.ctrl = c;
        if (L.outgoing && !L.intr)
            L.intr = l2_connect(&l2, HID_PSM_INTR);
    } else if (c->psm == HID_PSM_INTR) {
        L.intr = c;
    }
    both_open();
}

static void l2_closed(void *ctx, struct l2ch *c)
{
    (void)ctx;
    int mine = 0;
    if (L.ctrl && L.ctrl->lcid == c->lcid)
        L.ctrl = 0, mine = 1;
    if (L.intr && L.intr->lcid == c->lcid)
        L.intr = 0, mine = 1;
    if (!mine || L.lp == LP_NONE || L.lp == LP_CLOSING)
        return;
    if (L.lp == LP_UP || L.lp == LP_PROTO)
        fail_link("The keyboard closed its connection", 0x13);
    else if (c->why == 0xFFFF)
        fail_link("The keyboard did not answer while setting up", 0x13);
    else
        fail_link("The keyboard refused its HID connection", 0x13);
}

static void send_leds(void)
{
    uint8_t r[3] = { HIDP_DATA_OUTPUT, 0x01, kbd.leds };
    kbd.leds_changed = 0;
    if (L.intr)
        l2_send(&l2, L.intr, r, 3);
}

static void l2_data(void *ctx, struct l2ch *c, const uint8_t *p, unsigned n)
{
    (void)ctx;
    if (L.intr && c->lcid == L.intr->lcid) {
        if (bthid_frame(&kbd, p, n) > 0)
            S.hid_reports++;
        if (kbd.leds_changed)
            send_leds();
        return;
    }
    if (!L.ctrl || c->lcid != L.ctrl->lcid || !n)
        return;
    if (p[0] == HIDP_VC_UNPLUG) {
        say("bt: keyboard: it removed its pairing (virtual cable unplug): forgotten here too");
        if (kput && paired)
            kput(paired_addr, 0);
        paired = 0;
        fail_link("The keyboard removed its pairing: pair it again", 0x13);
        return;
    }
    if ((p[0] & 0xF0) == HIDP_HANDSHAKE && L.lp == LP_PROTO) {
        if (p[0] & 0x0F)
            logs("keyboard: SET_PROTOCOL(boot) answered with handshake ", p[0] & 0x0F, 0, " (going on)");
        connected();
    }
}

static const struct l2ops l2o = { l2_send_acl, l2_accept, l2_opened, l2_closed, l2_data };

/* ---- the API (bt.h) ---- */
void btp_init(const struct btp_io *i)
{
    io = i;
    l2_init(&l2, &l2o, 0);
}

void bt_set_link_key_store(bt_key_get_fn get, bt_key_put_fn put)
{
    kget = get;
    kput = put;
}

void bt_set_paired(const uint8_t *addr6)
{
    if (addr6) {
        bthci_copy(paired_addr, addr6, 6);
        paired = 1;
    } else {
        paired = 0;
    }
    if (L.lp == LP_NONE)
        public_idle();
    if (io)
        sync_scan();
}

int bt_paired(uint8_t addr[6])
{
    if (paired && addr)
        bthci_copy(addr, paired_addr, 6);
    return paired;
}

int bt_is_keyboard(const struct bt_dev *d)
{
    if (!d || d->le)
        return 0;
    if (((d->cod >> 8) & 0x1f) == 0x05 && (d->cod & 0x40))
        return 1;                           /* major class peripheral, minor: keyboard (or combo) */
    for (const char *s = d->name; *s; s++) {
        const char *k = "eyboard";
        unsigned j = 0;
        if (*s == 'K' || *s == 'k') {
            while (k[j] && (s[1 + j] | 0x20) == k[j])
                j++;
            if (!k[j])
                return 1;
        }
    }
    return 0;
}

int btp_pair_dev(const struct bt_dev *d, int pairing)
{
    if (!io)
        return -1;
    if (!io->chip_on()) {
        why = "Bluetooth is off";
        return -1;
    }
    if (L.lp != LP_NONE) {
        if (L.lp == LP_UP && same(L.addr, d ? d->addr : paired_addr))
            return 0;
        why = "A keyboard is already connecting or connected: remove it first";
        return -1;
    }
    if (pairing && (!d || d->le)) {
        why = "Only classic Bluetooth keyboards can be paired";
        return -1;
    }
    if (!pairing && !paired) {
        why = "No keyboard paired";
        return -1;
    }
    wipe(&L, sizeof(L));
    L.outgoing = 1;
    L.pairing = pairing;
    if (pairing) {
        bthci_copy(L.addr, d->addr, 6);
        L.psrm = d->psrm <= 2 ? d->psrm : 1;
        L.clkoff = d->clkoff;
        bthci_copy(L.name, d->name, sizeof(L.name));
        L.name[sizeof(L.name) - 1] = 0;
        if (kput) {                         /* a fresh pairing: an old key for it (or another keyboard's) goes */
            if (paired)
                kput(paired_addr, 0);
            kput(L.addr, 0);
        }
        paired = 0;
    } else {
        bthci_copy(L.addr, paired_addr, 6);
        L.psrm = 1;
        L.clkoff = 0;
    }
    L.lp = LP_START;
    L.deadline = now() + (pairing ? PAIR_MS : RECONNECT_MS);
    ps = BT_PAIR_CONNECTING;
    set_why("Connecting to ", L.name[0] ? L.name : "the keyboard", "...");
    say(pairing ? "bt: keyboard: pairing asked (classic; paging when the chip is free)"
                : "bt: keyboard: reconnect asked (the stored key)");
    return 0;
}

int bt_hid_reconnect(void)
{
    return btp_pair_dev(0, 0);
}

void bt_unpair(void)
{
    if (kput && paired)
        kput(paired_addr, 0);
    if (kput && L.lp != LP_NONE && L.authed)
        kput(L.addr, 0);
    paired = 0;
    if (L.lp != LP_NONE && L.lp != LP_CLOSING) {
        L.unpairing = 1;
        fail_link("", 0x13);
    } else if (L.lp == LP_CLOSING) {
        L.unpairing = 1;
    }
    if (L.lp == LP_NONE)
        public_idle();
    say("bt: keyboard: unpaired (its key forgotten)");
    if (io)
        sync_scan();
}

int bt_pair_state(void)
{
    return ps;
}

int bt_pair_passkey(void)
{
    if (ps != BT_PAIR_CODE)
        return -1;
    return (int)L.passkey;
}

const char *bt_pair_code(void)
{
    return ps == BT_PAIR_CODE ? L.code : "";
}

int bt_pair_digits(void)
{
    return ps == BT_PAIR_CODE ? L.digits : -1;
}

const char *bt_pair_why(void)
{
    return why;
}

int bt_hid_connected(void)
{
    return L.lp == LP_UP;
}

void bt_hid_set_key_cb(void (*cb)(int usage, int down, uint32_t unicode))
{
    kbd.cb = cb;
}

int bt_hid_mods(void)
{
    return kbd.mods;
}

/* ---- bt.c's glue ---- */
void btp_chip_off(void)
{
    if (L.lp != LP_NONE)
        say("bt: keyboard: the chip went off: the link is gone");
    link_reset();
    wipe(&L, sizeof(L));
    qh = qn = 0;
    inflight = 0;
    aqh = aqn = 0;
    setup_done = setup_queued = 0;
    scan_cur = -1;
    scan_queued = 0;
    public_idle();
}

int btp_cmd_busy(void)
{
    return inflight;
}

int btp_scan_ok(void)
{
    return L.lp == LP_NONE || L.lp == LP_UP;
}

unsigned btp_poll_ms(void)
{
    if (!io || !io->chip_on())
        return 0;
    if (qn || inflight || aqn)
        return 2;
    if (L.lp != LP_NONE)
        return 10;
    return paired ? 50U : 0U;
}

void btp_stats(struct btp_stats *s)
{
    *s = S;
    s->l2_malformed = l2.malformed;
    s->l2_dropped = l2.dropped;
    s->acl_mtu = acl_mtu;
    s->acl_credits = credits;
}

static void setup(void)
{
    /* the extended event mask: bits 0-47, SSP's 48-53 and 55, 56, 58-61 (passkey / keypress notifications, LE meta) */
    static const uint8_t mask[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xbf, 0x3d };
    static const uint8_t one = 1, policy[2] = { 0x05, 0x00 };   /* role switch + sniff allowed */
    queue(OP_BUFFER_SIZE, 0, 0);
    queue(OP_EVENT_MASK, mask, sizeof(mask));
    queue(OP_SSP_MODE, &one, 1);
    queue(OP_LINK_POLICY, policy, 2);
    setup_queued = 1;
}

static void cmd_result(uint16_t op, uint8_t status, const uint8_t *ret, uint8_t rl)
{
    switch (op) {
    case OP_BUFFER_SIZE:
        if (!status && rl >= 7) {
            uint16_t m = (uint16_t)(ret[0] | ret[1] << 8), t = (uint16_t)(ret[3] | ret[4] << 8);
            acl_mtu = m >= 8 ? (m > 132 ? 132 : m) : 8;   /* we never send long frames: 132 is plenty */
            acl_total = t ? (t > 16 ? 16 : t) : 1;
            credits = acl_total;
            l2.acl_mtu = acl_mtu;
        } else {
            logs("keyboard: Read_Buffer_Size refused (", status, 2, "): 8 x 132 assumed");
        }
        return;
    case OP_EVENT_MASK:
    case OP_LINK_POLICY:
        if (status)
            logs("keyboard: a link setup command was refused: status ", status, 2, "");
        return;
    case OP_SSP_MODE:
        setup_done = 1;
        if (status)
            logs("keyboard: Write_Simple_Pairing_Mode refused (", status, 2, "): only legacy PIN pairing");
        else
            say("bt: keyboard: link setup done (Secure Simple Pairing on)");
        return;
    case OP_SCAN_ENABLE:
        if (!status)
            scan_cur = scan_queued - 1;
        else
            logs("keyboard: Write_Scan_Enable refused: status ", status, 2, "");
        if (!status)
            say(scan_cur ? "bt: keyboard: page scan on (the paired keyboard may connect)" : "bt: keyboard: page scan off");
        scan_queued = 0;
        return;
    case OP_CREATE_CONN:
        if (status && L.lp == LP_PAGING) {
            struct btl l;
            btl_init(&l);
            btl_x(&l, status, 2);
            set_why("The keyboard could not be called (refused, code ", l.b, ")");
            fail_link(whyb, 0x13);
        }
        return;
    case OP_ACCEPT_CONN:
        if (status && L.lp == LP_INCOMING)
            fail_link("The keyboard's connection could not be taken", 0x13);
        return;
    case OP_AUTH_REQ:
        if (status && L.lp == LP_SECURING)
            fail_link("Pairing could not start", 0x13);
        return;
    case OP_SET_ENC:
        if (status && L.lp == LP_SECURING)
            fail_link("The connection could not be encrypted", 0x13);
        return;
    case OP_DISCONNECT:
        if (status && L.lp == LP_CLOSING)
            finish();                       /* no link to close (already gone) */
        return;
    default:
        if (status && status != 0xFF)
            logs("keyboard: a command was refused: ", op, 4, "");
        return;
    }
}

static void auth_failed(uint8_t st)
{
    const char *w;
    if (st == 0x06 && !L.pairing) {
        w = "The keyboard no longer knows this phone: pair it again";
        if (kput)
            kput(L.addr, 0);
        paired = 0;
    } else if (st == 0x05 || st == 0x06 || L.ssp_status == 0x05) {
        w = L.code_shown ? "The code was not typed right on the keyboard: pairing failed. Try again"
                         : "The keyboard refused pairing";
    } else if (st == 0x08 || st == 0x22 || st == 0x10) {
        w = L.code_shown ? "The code was not typed in time: pairing failed. Try again" : "Pairing timed out";
    } else if (st == 0x18) {
        w = "The keyboard does not allow pairing now (is it in pairing mode?)";
    } else {
        struct btl l;
        btl_init(&l);
        btl_x(&l, st, 2);
        set_why("Pairing failed (code ", l.b, ")");
        w = whyb;
    }
    fail_link(w, 0x05);
}

int btp_event(const uint8_t *ev)
{
    const uint8_t *p = ev + 2;
    unsigned n = ev[1];
    uint16_t op;
    uint8_t s, rl;
    const uint8_t *r;
    if (!io)
        return 0;
    if (bthci_cmd_complete(ev, &op, &s, &r, &rl)) {
        if (!inflight || op != inflight_op)
            return 0;
        inflight = 0;
        cmd_result(op, s == 0xFF ? 0 : s, r, rl);
        send_next();
        return 1;
    }
    if (bthci_cmd_status(ev, &op, &s)) {
        if (!inflight || op != inflight_op)
            return 0;
        inflight = 0;
        cmd_result(op, s, 0, 0);
        send_next();
        return 1;
    }
    int ours = n >= 6 && L.lp != LP_NONE && same(p, L.addr);       /* events that start with an address */
    switch (ev[0]) {
    case EV_CONN_COMPLETE: {
        if (n < 11)
            return 1;
        uint16_t h = (uint16_t)((p[1] | p[2] << 8) & 0x0fff);
        int mine = (L.lp == LP_PAGING || L.lp == LP_INCOMING) && same(p + 3, L.addr) && p[9] == 0x01;
        if (!mine) {
            if (!p[0]) {                    /* a link nobody asked for (a cancelled page that still made it) */
                uint8_t d[3] = { (uint8_t)h, (uint8_t)(h >> 8), 0x13 };
                queue(OP_DISCONNECT, d, 3);
            }
            return 1;
        }
        if (p[0]) {
            if (p[0] == 0x04)
                fail_link(L.pairing ? "The keyboard did not answer: is it on and in pairing mode?"
                                    : "The keyboard did not answer (press a key on it to wake it)", 0x13);
            else {
                struct btl l;
                btl_init(&l);
                btl_x(&l, p[0], 2);
                set_why("The connection failed (code ", l.b, ")");
                fail_link(whyb, 0x13);
            }
            return 1;
        }
        L.handle = h;
        L.have_handle = 1;
        L.encrypted = p[10] != 0;
        l2_link_up(&l2, h, acl_mtu);
        l2_tick(&l2, now());
        credits = acl_total;
        L.lp = LP_SECURING;
        ps = BT_PAIR_SECURING;
        why = L.pairing ? "Pairing..." : "Securing the keyboard's connection";
        say(L.outgoing ? "bt: keyboard: connected (ACL); authenticating" : "bt: keyboard: it connected (ACL); authenticating");
        queue_handle(OP_AUTH_REQ, 0, 0);
        return 1;
    }
    case EV_CONN_REQUEST: {
        if (n < 10)
            return 1;
        int known = paired && same(p, paired_addr) && p[9] == 0x01 && L.lp == LP_NONE;
        uint8_t k[16];
        if (known && kget && !kget(p, k))
            known = 0;
        wipe(k, sizeof(k));
        if (!known) {
            uint8_t reason = 0x0F;          /* unacceptable address */
            queue_addr(OP_REJECT_CONN, p, &reason, 1);
            say("bt: keyboard: a connection from an unknown device refused");
            return 1;
        }
        wipe(&L, sizeof(L));
        bthci_copy(L.addr, p, 6);
        L.lp = LP_INCOMING;
        L.deadline = now() + RECONNECT_MS;
        ps = BT_PAIR_CONNECTING;
        why = "The keyboard is connecting";
        uint8_t role = 0x01;                /* stay peripheral: no role switch asked */
        queue_addr(OP_ACCEPT_CONN, p, &role, 1);
        say("bt: keyboard: the paired keyboard is calling: accepted");
        return 1;
    }
    case EV_DISCONN_COMPLETE: {
        if (n < 4)
            return 1;
        uint16_t h = (uint16_t)((p[1] | p[2] << 8) & 0x0fff);
        if (!L.have_handle || h != L.handle || p[0])
            return 1;
        L.have_handle = 0;
        if (L.lp == LP_CLOSING) {
            finish();
        } else if (L.lp == LP_UP) {
            logs("keyboard: it disconnected (reason ", p[3], 2, ")");
            L.failing = 0;
            finish();
            if (paired)
                why = "The keyboard disconnected: press a key on it to connect again";
        } else {
            fail_link(L.pairing ? "The keyboard went away during pairing" : "The keyboard went away while connecting",
                      0x13);               /* no handle now: it finishes at once */
        }
        return 1;
    }
    case EV_AUTH_COMPLETE: {
        if (n < 3 || !L.have_handle || (uint16_t)((p[1] | p[2] << 8) & 0x0fff) != L.handle || L.lp != LP_SECURING)
            return 1;
        if (p[0]) {
            auth_failed(p[0]);
            return 1;
        }
        L.authed = 1;
        if (L.key_valid) {
            if (kput)
                kput(L.addr, L.key);
            wipe(L.key, sizeof(L.key));
            L.key_valid = 0;
            bthci_copy(paired_addr, L.addr, 6);
            paired = 1;
            say("bt: keyboard: paired (the link key went to the OS's store; it is not logged)");
            sync_scan();
        }
        wipe(L.code, sizeof(L.code));
        L.code_shown = 0;
        if (L.encrypted) {
            l2_secure(&l2, 1);
            start_hid();
        } else if (!L.enc_sent) {
            L.enc_sent = 1;
            queue_handle(OP_SET_ENC, 1, 1);
            ps = BT_PAIR_SECURING;
            why = "Encrypting the keyboard's connection";
        }
        return 1;
    }
    case EV_ENC_CHANGE: {
        if (n < 4 || !L.have_handle || (uint16_t)((p[1] | p[2] << 8) & 0x0fff) != L.handle)
            return 1;
        if (p[0] || !p[3]) {
            if (L.lp == LP_UP) {
                fail_link("The keyboard's connection lost its encryption", 0x05);
            } else if (L.enc_sent || L.lp >= LP_HID) {
                l2_secure(&l2, 0);
                fail_link("The keyboard did not accept encryption", 0x05);
            }
            return 1;
        }
        L.encrypted = 1;
        if (L.authed && L.lp == LP_SECURING) {
            l2_secure(&l2, 1);
            start_hid();
        }
        return 1;
    }
    case EV_NUM_COMPLETED: {
        unsigned k = n >= 1 ? p[0] : 0;
        for (unsigned i = 0; i < k && 1 + 4 * (i + 1) <= n; i++) {
            const uint8_t *e = p + 1 + 4 * i;
            uint16_t h = (uint16_t)((e[0] | e[1] << 8) & 0x0fff), c = (uint16_t)(e[2] | e[3] << 8);
            if (L.have_handle && h == L.handle) {
                credits = (uint16_t)(credits + c > acl_total ? acl_total : credits + c);
            }
        }
        drain_acl();
        return 1;
    }
    case EV_PIN_REQUEST:
        if (n < 6)
            return 1;
        if (ours && L.pairing && L.lp == LP_SECURING) {
            uint32_t pin = 100000U + (io->random ? io->random() : (uint32_t)now()) % 900000U;
            uint8_t rp[23];
            L.passkey = pin;
            for (int i = 5; i >= 0; i--, pin /= 10)
                L.code[i] = (char)('0' + pin % 10);
            L.code[6] = 0;
            bthci_copy(rp, p, 6);
            rp[6] = 6;
            for (unsigned i = 0; i < 16; i++)
                rp[7 + i] = i < 6 ? (uint8_t)L.code[i] : 0;
            queue(OP_PIN_REPLY, rp, sizeof(rp));
            wipe(rp, sizeof(rp));
            L.code_shown = 1;
            L.digits = -1;
            ps = BT_PAIR_CODE;
            set_why("Type ", L.code, " on the keyboard, then press Enter");
            say("bt: keyboard: legacy pairing: a PIN is shown to type on the keyboard");
        } else {
            queue_addr(OP_PIN_NEG, p, 0, 0);
        }
        return 1;
    case EV_LINK_KEY_REQUEST: {
        if (n < 6)
            return 1;
        uint8_t k[16];
        if (ours && !L.pairing && kget && kget(p, k)) {
            queue_addr(OP_LINK_KEY_REPLY, p, k, 16);
        } else {
            queue_addr(OP_LINK_KEY_NEG, p, 0, 0);
        }
        wipe(k, sizeof(k));
        return 1;
    }
    case EV_LINK_KEY_NOTIFY:
        if (n >= 23 && ours) {
            bthci_copy(L.key, p + 6, 16);
            L.key_valid = 1;
        }
        return 1;
    case EV_IO_CAP_REQUEST:
        if (n < 6)
            return 1;
        if (ours && L.pairing) {
            uint8_t more[3] = { 0x00, 0x00, 0x03 };   /* DisplayOnly, no OOB data, MITM + dedicated bonding */
            queue_addr(OP_IO_CAP_REPLY, p, more, 3);
        } else {
            uint8_t reason = 0x18;          /* pairing not allowed */
            queue_addr(OP_IO_CAP_NEG, p, &reason, 1);
        }
        return 1;
    case EV_IO_CAP_RESPONSE:
        if (ours && n >= 9) {
            static const char *const caps[] = { "DisplayOnly", "DisplayYesNo", "KeyboardOnly", "NoInputNoOutput" };
            L.remote_io = p[6];
            struct btl l;
            btl_init(&l);
            btl_s(&l, "bt: keyboard: its IO capability: ");
            btl_s(&l, p[6] < 4 ? caps[p[6]] : "unknown");
            btl_s(&l, p[6] == 2 ? " (a passkey to type)" : " (Just Works)");
            say(l.b);
        }
        return 1;
    case EV_CONFIRM_REQUEST:
        if (n < 10)
            return 1;
        if (ours && L.pairing)
            queue_addr(OP_CONFIRM_REPLY, p, 0, 0);   /* Just Works (or numeric comparison shown on no side) */
        else
            queue_addr(OP_CONFIRM_NEG, p, 0, 0);
        return 1;
    case EV_PASSKEY_REQUEST:
        if (n >= 6)
            queue_addr(OP_PASSKEY_NEG, p, 0, 0);     /* we have no keyboard of our own to type on */
        return 1;
    case EV_PASSKEY_NOTIFY:
        if (n >= 10 && ours && L.pairing) {
            uint32_t v = (uint32_t)p[6] | (uint32_t)p[7] << 8 | (uint32_t)p[8] << 16 | (uint32_t)p[9] << 24;
            v %= 1000000U;
            L.passkey = v;
            for (int i = 5; i >= 0; i--, v /= 10)
                L.code[i] = (char)('0' + v % 10);
            L.code[6] = 0;
            L.code_shown = 1;
            L.digits = 0;
            ps = BT_PAIR_CODE;
            set_why("Type ", L.code, " on the keyboard, then press Enter");
            say("bt: keyboard: Secure Simple Pairing: a passkey is shown to type on the keyboard");
        }
        return 1;
    case EV_KEYPRESS_NOTIFY:
        if (n >= 7 && ours && L.digits >= 0) {
            if (p[6] == 1)
                L.digits++;
            else if (p[6] == 2 && L.digits > 0)
                L.digits--;
            else if (p[6] == 3)
                L.digits = 0;
        }
        return 1;
    case EV_SSP_COMPLETE:
        if (n >= 7 && ours && p[0])
            L.ssp_status = p[0];
        return 1;
    case 0x12: case 0x14: case 0x1B: case 0x1D: case 0x20: case 0x30: case 0x38: case 0x3D: case 0x0B: case 0x0C:
        return 1;                           /* role / mode / slots / packet type / features: nothing to do */
    case 0x1A:
        say("bt: keyboard: the controller reported a data buffer overflow");
        return 1;
    default:
        return 0;
    }
}

void btp_acl(const uint8_t *pkt, unsigned n)
{
    S.acl_in++;
    if (L.have_handle)
        l2_acl_in(&l2, pkt, n);
    else
        l2.dropped++;
}

void btp_step(void)
{
    if (!io)
        return;
    uint64_t t = now();
    if (!io->chip_on())
        return;
    if (inflight && t >= inflight_dead) {
        uint16_t op = inflight_op;
        inflight = 0;
        logs("keyboard: no answer to command ", op, 4, "");
        cmd_result(op, 0xFF, 0, 0);
    }
    int need = paired || L.lp != LP_NONE;
    if (need && !setup_queued && io->chip_ready())
        setup();
    if (L.lp == LP_START && setup_queued && io->chip_ready()) {
        uint8_t m[7] = { 0x18, 0xCC, L.psrm, 0, (uint8_t)L.clkoff, (uint8_t)(L.clkoff >> 8), 0x01 };
        queue_addr(OP_CREATE_CONN, L.addr, m, 7);
        L.lp = LP_PAGING;
        say("bt: keyboard: paging");
    }
    sync_scan();
    l2_tick(&l2, t);
    if (L.lp != LP_NONE && L.lp != LP_UP && L.lp != LP_CLOSING && L.deadline && t >= L.deadline)
        fail_link(L.code_shown ? "The code was not typed in time: pairing failed. Try again"
                               : L.pairing ? "Pairing took too long" : "The keyboard did not finish connecting", 0x13);
    if (L.phase_dead && t >= L.phase_dead) {
        L.phase_dead = 0;
        if (L.lp == LP_CLOSING) {
            say("bt: keyboard: no answer to Disconnect: the link is taken as gone");
            finish();
        } else if (L.lp == LP_HID) {
            fail_link("The keyboard did not open its HID connection", 0x13);
        } else if (L.lp == LP_PROTO) {
            say("bt: keyboard: no answer to SET_PROTOCOL(boot) (going on)");
            connected();
        }
    }
    if (L.lp == LP_HID && L.self_open_at && t >= L.self_open_at) {
        L.self_open_at = 0;
        if (!l2_find_psm(&l2, HID_PSM_CTRL)) {
            say("bt: keyboard: it opened no HID channel itself: opening them from here");
            L.outgoing = 1;
            L.ctrl = l2_connect(&l2, HID_PSM_CTRL);
        }
    }
    drain_acl();
    send_next();
}
