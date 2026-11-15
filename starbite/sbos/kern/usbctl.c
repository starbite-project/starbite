/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: USB Control (usbctl.h; Track A 9f). The handshake, the approved keys, the session and its commands.
 * usb.c's console calls it: before a session only usbctl_line (the handshake) runs; in a session usbctl_exec first,
 * then the console's own read-only commands (ver, bat, log, follow, usb).
 *
 * Commands in a session (all logged "usbctl: <fingerprint>: <command>"):
 *   rows                  the catalog: key, name, ready, default (sh_os_get, as the lock cards)
 *   boot <key>            restart into a row once (power_boot_os, after the reply is out); any row, locked or not
 *   reboot <target>       menu | recovery | recovery-repair | maint | maint-rescue | fastboot | download
 *   off                   power off
 *   status                version, the start's row, uptime, the screen locked, battery and charging
 *   pmsg [N]              the crash reports (the home store's crashlog: kind, OS, the first line), newest first
 *   bye                   the session ends
 * Night rules (no One UI at night) stay on the Mac (boot-safe.sh): the phone takes the command as given. */
#include "sb.h"
#include "sbfw_api.h"
#include "kern.h"
#include "usbctl.h"
#include "usb.h"
#include "bearssl_hash.h"
#include "bearssl_ec.h"

#define K_KEYS "usbctl.keys"
#define K_ON "dev.usb"                      /* the same switch as Settings > Developer's (8b) */
#define K_ID "usbctl.id"
#define K_TRIED "usbctl.tried"

void sbos_random(void *out, uint32_t n);    /* wifi.c's entropy pool */
/* the Next shell's (kern_next.c); weak: the classic UI has none (then: locked, no indicator) */
int sh_usbctl_locked(void) __attribute__((weak));
void sh_usbctl_state(int on, const char *fp) __attribute__((weak));
int sbfw_os_restart_to(const char *target);  /* sbfw_os.c: Startup's Restart Into targets, without the sheet */
int sh_os_get(int i, char *key, char *name, int *category, int *method, int *ready, int *is_default);   /* sbfw_os.c */

static struct {
    int inited, authed, keys_n, fails, pending, pend_answer, pend_ok;
    uint8_t keys[USBCTL_KEYS][USBCTL_KEY_LEN];
    uint8_t pend_key[USBCTL_KEY_LEN];
    char nonce[65], id[17], fp[17];
    uint64_t wait_until, last_cmd, act_at;
    int act;                                /* a deferred restart: 1 boot, 2 reboot, 3 off */
    char act_arg[24];
} U;

/* ---- pure helpers -------------------------------------------------------------------------------------------- */
static void cpy(void *d, const void *s, unsigned n)
{
    uint8_t *o = d;
    const uint8_t *i = s;
    while (n--)
        *o++ = *i++;
}
static int hexv(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
static int unhex(const char *s, unsigned n, uint8_t *out, unsigned cap)
{
    if (n & 1U || n / 2U > cap)
        return -1;
    for (unsigned i = 0; i < n; i += 2) {
        int a = hexv(s[i]), b = hexv(s[i + 1]);
        if (a < 0 || b < 0)
            return -1;
        out[i / 2] = (uint8_t)(a << 4 | b);
    }
    return (int)(n / 2U);
}
static void tohex(char *out, const uint8_t *p, unsigned n)
{
    static const char d[] = "0123456789abcdef";
    for (unsigned i = 0; i < n; ++i) {
        out[2 * i] = d[p[i] >> 4];
        out[2 * i + 1] = d[p[i] & 15];
    }
    out[2 * n] = 0;
}
/* the next word of s: its start and length (0 none) */
static const char *word(const char *s, unsigned *n)
{
    while (*s == ' ' || *s == '\t')
        ++s;
    unsigned k = 0;
    while (s[k] && s[k] != ' ' && s[k] != '\t')
        ++k;
    *n = k;
    return s;
}
static int weq(const char *w, unsigned n, const char *lit)
{
    unsigned i = 0;
    for (; i < n && lit[i] && w[i] == lit[i]; ++i)
        ;
    return i == n && !lit[i];
}

int usbctl_signed_text(char *out, unsigned cap, const char *nonce_hex, const char *id_hex, const char *key_hex)
{
    const char *parts[6] = { "sbctl1|", nonce_hex, "|", id_hex, "|", key_hex };
    unsigned o = 0;
    for (unsigned p = 0; p < 6; ++p)
        for (const char *s = parts[p]; *s; ++s) {
            if (o + 1 >= cap)
                return -1;
            out[o++] = *s;
        }
    out[o] = 0;
    return (int)o;
}

static void fingerprint(const uint8_t key[USBCTL_KEY_LEN], char out[17])
{
    uint8_t h[32];
    br_sha256_context c;
    br_sha256_init(&c);
    br_sha256_update(&c, key, USBCTL_KEY_LEN);
    br_sha256_out(&c, h);
    tohex(out, h, 8);
}

int usbctl_check_auth(const char *line, const char *nonce_hex, const char *id_hex, uint8_t key[USBCTL_KEY_LEN])
{
    unsigned n;
    const char *w = word(line, &n);
    if (!weq(w, n, "AUTH"))
        return 0;
    unsigned kn, sn;
    const char *kh = word(w + n, &kn);
    const char *sh = word(kh + kn, &sn);
    unsigned xn;
    (void)word(sh + sn, &xn);
    if (kn != 2U * USBCTL_KEY_LEN || !sn || xn)
        return 0;
    uint8_t sig[80];
    if (unhex(kh, kn, key, USBCTL_KEY_LEN) != USBCTL_KEY_LEN || key[0] != 4)
        return 0;
    int sl = unhex(sh, sn, sig, sizeof(sig));
    if (sl <= 8)
        return 0;
    char khex[2 * USBCTL_KEY_LEN + 1];
    for (unsigned i = 0; i < kn; ++i)
        khex[i] = (char)(kh[i] >= 'A' && kh[i] <= 'F' ? kh[i] + 32 : kh[i]);
    khex[kn] = 0;
    char text[300];
    int tl = usbctl_signed_text(text, sizeof(text), nonce_hex, id_hex, khex);
    if (tl < 0)
        return 0;
    uint8_t h[32];
    br_sha256_context c;
    br_sha256_init(&c);
    br_sha256_update(&c, text, (size_t)tl);
    br_sha256_out(&c, h);
    br_ec_public_key pk = { BR_EC_secp256r1, key, USBCTL_KEY_LEN };
    return br_ecdsa_i31_vrfy_asn1(&br_ec_p256_m64, h, sizeof(h), &pk, sig, (size_t)sl) == 1;
}

/* ---- the keys, the switch, the phone's id ------------------------------------------------------------------- */
static void load(void)
{
    if (U.inited)
        return;
    U.inited = 1;
    uint8_t b[USBCTL_KEYS * USBCTL_KEY_LEN];
    int n = prefs_blob_get(K_KEYS, b, sizeof(b));
    U.keys_n = n > 0 ? n / USBCTL_KEY_LEN : 0;
    for (int i = 0; i < U.keys_n; ++i)
        cpy(U.keys[i], b + i * USBCTL_KEY_LEN, USBCTL_KEY_LEN);
    if (prefs_get(K_ID, U.id, sizeof(U.id)) != 16) {
        uint8_t r[8];
        sbos_random(r, sizeof(r));
        tohex(U.id, r, sizeof(r));
        (void)prefs_set(K_ID, U.id);        /* kept with the settings (not secret: it only names the phone) */
        settings_changed();
    }
}

static void keys_save(void)
{
    uint8_t b[USBCTL_KEYS * USBCTL_KEY_LEN];
    for (int i = 0; i < U.keys_n; ++i)
        cpy(b + i * USBCTL_KEY_LEN, U.keys[i], USBCTL_KEY_LEN);
    (void)prefs_blob_put(K_KEYS, b, (uint32_t)(U.keys_n * USBCTL_KEY_LEN));
    settings_changed();
}

static int key_find(const uint8_t key[USBCTL_KEY_LEN])
{
    for (int i = 0; i < U.keys_n; ++i) {
        int same = 1;
        for (int k = 0; k < USBCTL_KEY_LEN; ++k)
            same &= U.keys[i][k] == key[k];
        if (same)
            return i;
    }
    return -1;
}

int usbctl_enabled(void)
{
    char v[4];
    return prefs_get(K_ON, v, sizeof(v)) > 0 && v[0] == '1';
}

void usbctl_set_enabled(int on)
{
    (void)prefs_set(K_ON, on ? "1" : "0");
    settings_changed();
    sb_log(on ? "usbctl: USB Control on (Settings): the USB link starts for a computer's cable"
              : "usbctl: USB Control off (Settings): the USB link is stopped");
    if (!on)
        usbctl_close("USB Control was turned off");
#ifndef SB_HOST
    usb_enable(on);
#endif
}

int usbctl_key_count(void) { load(); return U.keys_n; }
void usbctl_key_fp(int i, char out[17])
{
    load();
    if (i < 0 || i >= U.keys_n) {
        out[0] = 0;
        return;
    }
    fingerprint(U.keys[i], out);
}

int usbctl_revoke(int i)
{
    load();
    if (i < 0 || i >= U.keys_n)
        return -1;
    char fp[17];
    fingerprint(U.keys[i], fp);
    for (int k = i; k + 1 < U.keys_n; ++k)
        cpy(U.keys[k], U.keys[k + 1], USBCTL_KEY_LEN);
    --U.keys_n;
    keys_save();
    sb_logf("usbctl: a computer's key revoked (Settings): ", fp);
    if (U.authed && sb_streq(fp, U.fp))
        usbctl_close("its key was revoked");
    return 0;
}

int usbctl_session(char fp[17])
{
    if (!U.authed)
        return 0;
    sb_strcpy(fp, 17, U.fp);
    return 1;
}

void usbctl_init(void)
{
    load();
#ifndef SB_HOST
    if (usbctl_enabled()) {
        sb_log("usbctl: USB Control is on: the USB link watches the cable");
        usb_enable(1);
    }
#endif
    char t[4];
    if (prefs_get(K_TRIED, t, sizeof(t)) > 0) {
        /* a locked phone refused a new computer before this start: told now (the notification waits for the unlock) */
        notif_post("usb", "A computer tried USB Control", "It was not paired. Pair a computer only while the phone is unlocked.");
        (void)prefs_set(K_TRIED, 0);
        settings_changed();
    }
}

/* ---- the session ---------------------------------------------------------------------------------------------- */
static void state(int on)
{
    if (sh_usbctl_state)
        sh_usbctl_state(on, on ? U.fp : "");
}

static void challenge(usbctl_put_fn put)
{
    uint8_t r[32];
    sbos_random(r, sizeof(r));
    tohex(U.nonce, r, sizeof(r));
    put("Starbite OS USB Control: authenticate (tools/sbctl.py)\r\nSBCTL1 ");
    put(U.nonce);
    put(" ");
    put(U.id);
    put("\r\n");
}

void usbctl_open(usbctl_put_fn put)
{
    load();
    U.authed = 0;
    U.pending = 0;
    challenge(put);
}

void usbctl_close(const char *why)
{
    if (U.authed) {
        sb_logf("usbctl: the session ended: ", why);
        U.authed = 0;
        state(0);
    }
    U.pending = 0;
    U.nonce[0] = 0;
}

int usbctl_authed(void) { return U.authed; }

static void pair_done(int ok)
{
    U.pend_answer = 1;
    U.pend_ok = ok;
}

static void begin(const uint8_t key[USBCTL_KEY_LEN], usbctl_put_fn put)
{
    U.authed = 1;
    U.fails = 0;
    U.last_cmd = sb_ms();
    fingerprint(key, U.fp);
    sb_logf("usbctl: a session opened for the computer ", U.fp);
    put("OK ");
    put(U.fp);
    put("\r\n");
    state(1);
}

void usbctl_line(const char *line, usbctl_put_fn put)
{
    load();
    unsigned n;
    const char *w = word(line, &n);
    if (!n)
        return;
    if (weq(w, n, "hello") || weq(w, n, "HELLO")) {
        challenge(put);
        return;
    }
    if (!weq(w, n, "AUTH")) {
        put("REFUSED not authenticated: answer the SBCTL1 challenge (tools/sbctl.py)\r\n");
        return;
    }
    uint64_t now = sb_ms();
    if (now < U.wait_until) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "WAIT ");
        ub_dec(&u, (U.wait_until - now + 999U) / 1000U);
        ub_s(&u, "\r\n");
        put(u.b);
        return;
    }
    if (U.pending) {
        put("PENDING the question is on the phone\r\n");
        return;
    }
    if (!U.nonce[0]) {
        put("REFUSED no challenge: send hello\r\n");
        return;
    }
    uint8_t key[USBCTL_KEY_LEN];
    int ok = usbctl_check_auth(line, U.nonce, U.id, key);
    U.nonce[0] = 0;                         /* used once, whatever the verdict */
    if (!ok) {
        if (++U.fails >= 3) {
            unsigned s = 60U << (U.fails - 3 > 4 ? 4 : U.fails - 3);
            if (s > 900U)
                s = 900U;
            U.wait_until = now + (uint64_t)s * 1000U;
        }
        sb_log("usbctl: a bad signature (or a malformed AUTH): refused");
        put("REFUSED bad signature\r\n");
        challenge(put);
        return;
    }
    if (key_find(key) >= 0) {
        begin(key, put);
        return;
    }
    char fp[17];
    fingerprint(key, fp);
    int locked = sh_usbctl_locked ? sh_usbctl_locked() : 1;
    if (locked) {
        sb_logf("usbctl: a computer that is not paired, with the phone locked: refused ", fp);
        (void)prefs_set(K_TRIED, "1");
        settings_changed();
        notif_post("usb", "A computer tried USB Control", "It was not paired. Pair a computer only while the phone is unlocked.");
        put("REFUSED this computer is not paired: unlock the phone and connect again to pair it\r\n");
        challenge(put);
        return;
    }
    if (U.keys_n >= USBCTL_KEYS) {
        put("REFUSED four computers are paired already: revoke one in Settings > Privacy > USB Control\r\n");
        challenge(put);
        return;
    }
    cpy(U.pend_key, key, USBCTL_KEY_LEN);
    U.pending = 1;
    U.pend_answer = 0;
    static char l1[128];
    struct ub u;
    ub_init(&u);
    ub_s(&u, "Key ");
    ub_s(&u, fp);
    ub_s(&u, ". It can restart the phone into any system and read its logs, locked too.");
    sb_strcpy(l1, sizeof(l1), u.b);
    sb_logf("usbctl: a new computer asks to pair: the question is on the phone ", fp);
    kui_confirm("Allow USB Control from this computer?", l1, "Never your notes, photos, messages or passcode.",
                "Settings > Privacy > USB Control can revoke it.", "Hold to allow", pair_done);
    put("PENDING approve this computer on the phone (");
    put(fp);
    put(")\r\n");
}

static void put_dec(usbctl_put_fn put, uint64_t v)
{
    struct ub u;
    ub_init(&u);
    ub_dec(&u, v);
    put(u.b);
}

static int known_row(const char *k, unsigned n)
{
    char key[24], name[32];
    for (int i = 0; sh_os_get(i, key, name, 0, 0, 0, 0) == 0; ++i)
        if (weq(k, n, key))
            return 1;
    return 0;
}

static void cmd_rows(usbctl_put_fn put)
{
    char key[24], name[32];
    int ready = 0, def = 0, n = 0;
    for (int i = 0; sh_os_get(i, key, name, 0, 0, &ready, &def) == 0; ++i, ++n) {
        put(key);
        put("\t");
        put(name);
        put(ready ? "\tready" : "\tnot-ready");
        put(def ? "\tdefault\r\n" : "\r\n");
    }
    put("rows ");
    put_dec(put, (uint64_t)n);
    put("\r\n");
}

static void cmd_status(usbctl_put_fn put)
{
    struct sbos_battery b;
    struct kcharge kc;
    battery_get(&b);
    charge_get(&kc);
    put("version Starbite OS " SBOS_VERSION "\r\nrow ");
    put(sb_fw.valid ? sb_fw.key : "?");
    put("\r\nuptime_s ");
    put_dec(put, sb_ms() / 1000U);
    put("\r\nlocked ");
    put(sh_usbctl_locked && !sh_usbctl_locked() ? "no" : "yes");
    put("\r\nbattery_pct ");
    put_dec(put, b.ok ? (uint64_t)b.pct : 0);
    put("\r\nbattery_ma ");
    {
        struct ub u;
        ub_init(&u);
        ub_sdec(&u, b.ok ? b.avg_ma : 0);
        put(u.b);
    }
    put("\r\nbattery_c ");
    put_dec(put, b.ok && b.temp_c10 > 0 ? (uint64_t)(b.temp_c10 / 10) : 0);
    put("\r\ncharger ");
    put(kc.in ? "in" : "none");
    put("\r\ncharge_limits_ma ");
    put_dec(put, (uint64_t)kc.il_ma);
    put("/");
    put_dec(put, (uint64_t)kc.cc_ma);
    put("\r\n");
}

static void cmd_pmsg(unsigned want, usbctl_put_fn put)
{
    int n = sbfw_crash_count();
    if (n < 0) {
        put("no crash log on this store\r\n");
        return;
    }
    put("crash reports ");
    put_dec(put, (uint64_t)n);
    put("\r\n");
    for (int i = 0; i < n && (unsigned)i < want; ++i) {
        struct sbfw_crash c;
        if (sbfw_crash_get((unsigned)i, &c))
            continue;
        put_dec(put, c.seq);
        put("\t");
        put(c.kind);
        put("\t");
        put(c.key);
        put("\t");
        put(c.line);
        put("\r\n");
    }
}

static void defer(int act, const char *arg, unsigned n)
{
    U.act = act;
    unsigned i = 0;
    for (; arg && i < n && i + 1 < sizeof(U.act_arg); ++i)
        U.act_arg[i] = arg[i];
    U.act_arg[i] = 0;
    U.act_at = sb_ms() + 300U;              /* the reply goes out first */
}

int usbctl_exec(const char *line, usbctl_put_fn put)
{
    unsigned n, an, xn;
    const char *w = word(line, &n);
    const char *a = word(w + n, &an);
    (void)word(a + an, &xn);
    if (!n)
        return 0;
    U.last_cmd = sb_ms();
    {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "usbctl: ");
        ub_s(&u, U.fp);
        ub_s(&u, ": ");
        for (unsigned i = 0; line[i] && i < 60; ++i)
            ub_c(&u, line[i]);
        sb_log(u.b);
    }
    if (weq(w, n, "rows") && !an) {
        cmd_rows(put);
        return 1;
    }
    if (weq(w, n, "status") && !an) {
        cmd_status(put);
        return 1;
    }
    if (weq(w, n, "pmsg") && !xn) {
        unsigned v = 0;
        for (unsigned i = 0; i < an && i < 3; ++i)
            v = a[i] >= '0' && a[i] <= '9' ? v * 10U + (unsigned)(a[i] - '0') : 0;
        cmd_pmsg(an ? (v ? v : 1) : 4, put);
        return 1;
    }
    if (weq(w, n, "boot") && an && !xn) {
        if (!known_row(a, an)) {
            put("ERR no such row (rows lists them)\r\n");
            return 1;
        }
        put("OK restarting into ");
        for (unsigned i = 0; i < an; ++i) {
            char c[2] = { a[i], 0 };
            put(c);
        }
        put("\r\n");
        defer(1, a, an);
        return 1;
    }
    if (weq(w, n, "reboot") && an && !xn) {
        static const char *const t[] = { "menu", "recovery", "recovery-repair", "maint", "maint-rescue", "fastboot",
                                         "download" };
        for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); ++i)
            if (weq(a, an, t[i])) {
                put("OK restarting to ");
                put(t[i]);
                put("\r\n");
                defer(2, a, an);
                return 1;
            }
        put("ERR reboot menu|recovery|recovery-repair|maint|maint-rescue|fastboot|download\r\n");
        return 1;
    }
    if (weq(w, n, "off") && !an) {
        put("OK powering off\r\n");
        defer(3, 0, 0);
        return 1;
    }
    if (weq(w, n, "bye") && !an) {
        put("OK bye\r\n");
        usbctl_close("bye");
        return 1;
    }
    if (weq(w, n, "help") || weq(w, n, "?")) {
        put("USB Control (this session):\r\n"
            "  rows               the catalog's rows (key, name, ready, default)\r\n"
            "  boot <key>         restart into a row once\r\n"
            "  reboot <target>    menu | recovery | recovery-repair | maint | maint-rescue | fastboot | download\r\n"
            "  off                power off\r\n"
            "  status             version, row, uptime, locked, battery, charging\r\n"
            "  pmsg [N]           the newest crash reports\r\n"
            "  bye                end the session\r\n");
        return 0;                           /* the console's own list follows */
    }
    return 0;
}

void usbctl_tick(uint64_t now, usbctl_put_fn put)
{
    if (U.pending && U.pend_answer) {
        U.pending = 0;
        U.pend_answer = 0;
        int locked = sh_usbctl_locked ? sh_usbctl_locked() : 1;
        if (U.pend_ok && !locked && U.keys_n < USBCTL_KEYS) {
            cpy(U.keys[U.keys_n++], U.pend_key, USBCTL_KEY_LEN);
            keys_save();
            sb_log("usbctl: the computer was paired (the user's hold)");
            if (put)
                begin(U.pend_key, put);
        } else {
            sb_log("usbctl: the computer was not paired (the user declined)");
            if (put) {
                put("REFUSED not allowed on the phone\r\n");
                challenge(put);
            }
        }
    }
    if (U.authed && now - U.last_cmd > USBCTL_IDLE_MS) {
        if (put)
            put("bye (30 minutes without a command)\r\n");
        usbctl_close("30 minutes without a command");
        if (put)
            challenge(put);
    }
    if (U.act && now >= U.act_at) {
        int act = U.act;
        U.act = 0;
        usbctl_close("a restart");
        if (act == 1) {
            sb_logf("usbctl: restarting into the row ", U.act_arg);
            power_boot_os(U.act_arg);
        } else if (act == 2) {
            sb_logf("usbctl: restarting to ", U.act_arg);
            (void)sbfw_os_restart_to(U.act_arg);
        } else {
            sb_log("usbctl: powering off");
            power_off_now();
        }
    }
}
