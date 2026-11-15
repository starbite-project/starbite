/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of kern/usbctl.c (USB Control's handshake, pairing, session and commands) with BearSSL's real ECDSA
 * verify and signatures made by the Mac's openssl (usbctl-test.sh builds and runs it). Everything else is faked:
 * prefs, the clock, the entropy (fixed: the nonces are known in advance), the lock, the confirmation sheet, the
 * catalog, power. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "sb.h"
#include "sbfw_api.h"
#include "kern.h"
#include "usbctl.h"

static int fails, checks;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- fakes ---- */
struct sb_fw sb_fw;
static uint64_t now_ms = 1000;
uint64_t sb_ms(void) { return now_ms; }
static char logbuf[65536];
void sb_log(const char *s) { strncat(logbuf, s, sizeof(logbuf) - strlen(logbuf) - 2); strcat(logbuf, "\n"); if (getenv("V")) printf("log: %s\n", s); }
void sb_logf(const char *a, const char *b) { char t[512]; snprintf(t, sizeof(t), "%s%s", a, b); sb_log(t); }
static struct { char k[32], v[64]; } pv[16];
static uint8_t blob[512];
static int blob_n = -1;
int prefs_get(const char *key, char *val, int cap)
{
    for (int i = 0; i < 16; ++i)
        if (pv[i].k[0] && !strcmp(pv[i].k, key)) { snprintf(val, (size_t)cap, "%s", pv[i].v); return (int)strlen(pv[i].v); }
    return -1;
}
int prefs_set(const char *key, const char *val)
{
    for (int i = 0; i < 16; ++i)
        if (pv[i].k[0] && !strcmp(pv[i].k, key)) { if (!val) pv[i].k[0] = 0; else snprintf(pv[i].v, sizeof(pv[i].v), "%s", val); return 1; }
    if (!val) return 0;
    for (int i = 0; i < 16; ++i)
        if (!pv[i].k[0]) { snprintf(pv[i].k, sizeof(pv[i].k), "%s", key); snprintf(pv[i].v, sizeof(pv[i].v), "%s", val); return 1; }
    return -1;
}
int prefs_blob_put(const char *name, const void *p, uint32_t n) { (void)name; memcpy(blob, p, n); blob_n = (int)n; return 1; }
int prefs_blob_get(const char *name, void *p, uint32_t cap) { (void)name; if (blob_n < 0) return -1; memcpy(p, blob, (size_t)blob_n < cap ? (size_t)blob_n : cap); return blob_n; }
static int saves;
void settings_changed(void) { ++saves; }
static uint8_t rnd_ctr;
void sbos_random(void *out, uint32_t n) { for (uint32_t i = 0; i < n; ++i) ((uint8_t *)out)[i] = (uint8_t)(rnd_ctr * 31 + i); ++rnd_ctr; }
static int notifs;
void notif_post(const char *who, const char *title, const char *text) { (void)who; (void)title; (void)text; ++notifs; }
static void (*cf_done)(int);
static int confirms;
void kui_confirm(const char *t, const char *l1, const char *l2, const char *l3, const char *hold, void (*done)(int ok))
{ (void)t; (void)l1; (void)l2; (void)l3; (void)hold; cf_done = done; ++confirms; }
static int locked = 1;
int sh_usbctl_locked(void) { return locked; }
static int ind_on;
void sh_usbctl_state(int on, const char *fp) { (void)fp; ind_on = on; }
int sh_os_get(int i, char *key, char *name, int *category, int *method, int *ready, int *is_default)
{
    static const char *const k[] = { "starbite-next", "win11-big", "android", "ios27" };
    if (i < 0 || i >= 4) return -1;
    snprintf(key, 24, "%s", k[i]); snprintf(name, 32, "Name %d", i);
    if (category) *category = 0;
    if (method) *method = 0;
    if (ready) *ready = 1;
    if (is_default) *is_default = i == 0;
    return 0;
}
void battery_get(struct sbos_battery *b) { memset(b, 0, sizeof(*b)); b->ok = 1; b->pct = 55; b->avg_ma = 1500; b->temp_c10 = 312; }
void charge_get(struct kcharge *k) { memset(k, 0, sizeof(*k)); k->in = 1; k->il_ma = 1800; k->cc_ma = 2100; }
int sbfw_crash_count(void) { return 2; }
int sbfw_crash_get(unsigned i, struct sbfw_crash *c) { memset(c, 0, sizeof(*c)); c->seq = 10 - i; snprintf(c->kind, 24, "KERNEL PANIC"); snprintf(c->key, 24, "linux"); snprintf(c->line, 96, "oops %u", i); return 0; }
static char booted[32];
static int offs, restarts;
void power_boot_os(const char *key) { snprintf(booted, sizeof(booted), "%s", key); }
void power_off_now(void) { ++offs; }
int sbfw_os_restart_to(const char *t) { (void)t; ++restarts; return 0; }
void usb_enable(int on) { (void)on; }

/* ---- the console ---- */
static char out[65536];
static void put(const char *s) { strncat(out, s, sizeof(out) - strlen(out) - 1); }
static int got(const char *s) { return strstr(out, s) != 0; }
static void clr(void) { out[0] = 0; }
static char nonce[65], pid[17];
static void read_challenge(void)
{
    const char *p = strstr(out, "SBCTL1 ");
    nonce[0] = pid[0] = 0;
    if (p) sscanf(p, "SBCTL1 %64s %16s", nonce, pid);
}

/* an AUTH line from openssl: the key's public point and a signature over the text */
static char pub_hex[2][131];
static void key_pub(int k)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "openssl ec -in \"$T/k%d.pem\" -pubout -outform DER 2>/dev/null | tail -c 65 | xxd -p -c 200", k);
    FILE *f = popen(cmd, "r");
    if (!f || !fgets(pub_hex[k], sizeof(pub_hex[k]), f)) { printf("openssl failed\n"); exit(1); }
    pclose(f);
    pub_hex[k][130] = 0;
}
static void auth_line(int k, const char *nonce_hex, const char *id_hex, char *line, size_t cap)
{
    char text[400], cmd[600], sig[300];
    usbctl_signed_text(text, sizeof(text), nonce_hex, id_hex, pub_hex[k]);
    FILE *f = fopen(getenv("T") ? "/dev/null" : "/dev/null", "r");
    if (f) fclose(f);
    snprintf(cmd, sizeof(cmd), "printf '%%s' '%s' | openssl dgst -sha256 -sign \"$T/k%d.pem\" | xxd -p -c 400", text, k);
    f = popen(cmd, "r");
    if (!f || !fgets(sig, sizeof(sig), f)) { printf("openssl sign failed\n"); exit(1); }
    pclose(f);
    sig[strcspn(sig, "\n")] = 0;
    snprintf(line, cap, "AUTH %s %s", pub_hex[k], sig);
}

int main(void)
{
    char line[600], text[400];
    key_pub(0);
    key_pub(1);
    CHECK(strlen(pub_hex[0]) == 130 && pub_hex[0][0] == '0' && pub_hex[0][1] == '4', "openssl's point: %s", pub_hex[0]);
    CHECK(usbctl_signed_text(text, sizeof(text), "aa", "bb", "cc") == 15 && !strcmp(text, "sbctl1|aa|bb|cc"), "signed text");

    /* the challenge; anything but AUTH refused; nothing authed */
    usbctl_open(put);
    read_challenge();
    CHECK(strlen(nonce) == 64 && strlen(pid) == 16, "SBCTL1 nonce id: [%s]", out);
    clr();
    usbctl_line("rows", put);
    CHECK(got("REFUSED") && !usbctl_authed(), "a command before the session is refused");

    /* the pure check: valid, a wrong nonce, a flipped signature byte */
    auth_line(0, nonce, pid, line, sizeof(line));
    uint8_t key[USBCTL_KEY_LEN];
    CHECK(usbctl_check_auth(line, nonce, pid, key) == 1, "a good signature verifies");
    char other[65];
    snprintf(other, sizeof(other), "%s", nonce);
    other[0] = other[0] == 'a' ? 'b' : 'a';
    CHECK(usbctl_check_auth(line, other, pid, key) == 0, "another nonce does not");
    char bad[600];
    snprintf(bad, sizeof(bad), "%s", line);
    size_t L = strlen(bad);
    bad[L - 3] = bad[L - 3] == '0' ? '1' : '0';
    CHECK(usbctl_check_auth(bad, nonce, pid, key) == 0, "a changed signature does not");
    CHECK(usbctl_check_auth("AUTH 04zz 3045", nonce, pid, key) == 0, "malformed");

    /* locked phone, unknown key: refused, a notification, the tried note; a fresh challenge */
    clr();
    usbctl_line(line, put);
    CHECK(got("REFUSED this computer is not paired") && !usbctl_authed() && notifs == 1, "locked: an unknown key refused");
    { char t[4]; CHECK(prefs_get("usbctl.tried", t, 4) > 0, "the tried note kept for the next start"); }
    CHECK(confirms == 0, "no question on a locked phone");
    read_challenge();
    CHECK(strlen(nonce) == 64, "a new challenge after the refusal");

    /* replay: the old line against the new nonce */
    clr();
    usbctl_line(line, put);
    CHECK(got("REFUSED bad signature") && !usbctl_authed(), "a replayed AUTH is refused");
    read_challenge();

    /* unlocked: the question; declined; then allowed */
    locked = 0;
    auth_line(0, nonce, pid, line, sizeof(line));
    clr();
    usbctl_line(line, put);
    CHECK(got("PENDING") && confirms == 1 && cf_done, "unlocked: the question is on the phone");
    clr();
    usbctl_line(line, put);
    CHECK(got("PENDING the question is on the phone"), "a second AUTH while it waits");
    cf_done(0);
    usbctl_tick(now_ms, put);
    CHECK(got("REFUSED not allowed") && !usbctl_authed() && usbctl_key_count() == 0, "declined: no key, no session");
    read_challenge();
    auth_line(0, nonce, pid, line, sizeof(line));
    clr();
    usbctl_line(line, put);
    cf_done(1);
    clr();
    usbctl_tick(now_ms, put);
    CHECK(got("OK ") && usbctl_authed() && usbctl_key_count() == 1 && ind_on, "allowed: paired, a session, the indicator");
    char fp[17];
    usbctl_key_fp(0, fp);
    CHECK(strlen(fp) == 16 && got(fp), "the fingerprint: %s", fp);

    /* commands */
    clr();
    CHECK(usbctl_exec("rows", put) && got("starbite-next\tName 0\tready\tdefault") && got("rows 4"), "rows: [%s]", out);
    clr();
    CHECK(usbctl_exec("status", put) && got("battery_pct 55") && got("charge_limits_ma 1800/2100") && got("locked no"), "status");
    clr();
    CHECK(usbctl_exec("pmsg 1", put) && got("crash reports 2") && got("KERNEL PANIC\tlinux\toops 0") && !got("oops 1"), "pmsg 1");
    clr();
    CHECK(usbctl_exec("boot nosuch", put) && got("ERR no such row"), "boot of an unknown row");
    clr();
    CHECK(usbctl_exec("ver", put) == 0, "the console's own commands fall through");
    CHECK(usbctl_exec("boot win11-big", put) && got("OK restarting into win11-big") && !booted[0], "boot: the reply first");
    now_ms += 400;
    usbctl_tick(now_ms, put);
    CHECK(!strcmp(booted, "win11-big") && !usbctl_authed(), "then the restart (the session closed)");
    CHECK(strstr(logbuf, "usbctl: ") && strstr(logbuf, ": boot win11-big"), "the command is logged");

    /* a paired key while locked: straight in, no question */
    locked = 1;
    clr();
    usbctl_open(put);
    read_challenge();
    auth_line(0, nonce, pid, line, sizeof(line));
    clr();
    usbctl_line(line, put);
    CHECK(got("OK ") && usbctl_authed() && confirms == 2, "a paired computer on a locked phone: a session, no question");
    clr();
    CHECK(usbctl_exec("reboot download", put) && got("OK restarting to download"), "reboot download");
    CHECK(usbctl_exec("reboot nowhere", put) && got("ERR reboot"), "reboot to an unknown target");
    now_ms += 400;
    usbctl_tick(now_ms, put);
    CHECK(restarts == 1, "the restart ran");

    /* idle: 30 minutes */
    usbctl_open(put);
    read_challenge();
    auth_line(0, nonce, pid, line, sizeof(line));
    usbctl_line(line, put);
    CHECK(usbctl_authed(), "a session again");
    now_ms += USBCTL_IDLE_MS + 1000;
    clr();
    usbctl_tick(now_ms, put);
    CHECK(!usbctl_authed() && got("bye (30 minutes") && !ind_on, "idle 30 min: the session ends, the indicator off");

    /* the back-off: three bad signatures */
    read_challenge();
    for (int i = 0; i < 3; ++i) {
        auth_line(1, nonce, "0000000000000000", line, sizeof(line));   /* signed for another phone id */
        clr();
        usbctl_line(line, put);
        read_challenge();
    }
    auth_line(0, nonce, pid, line, sizeof(line));
    clr();
    usbctl_line(line, put);
    CHECK(got("WAIT 60") && !usbctl_authed(), "after three bad signatures: wait 60 s [%s]", out);
    now_ms += 61000;
    clr();
    usbctl_line(line, put);
    CHECK(got("OK ") && usbctl_authed(), "after the wait: a good key gets in");

    /* revoke ends its session */
    CHECK(usbctl_revoke(0) == 0 && usbctl_key_count() == 0 && !usbctl_authed(), "revoke: the key gone, the session ended");
    usbctl_open(put);
    read_challenge();
    auth_line(0, nonce, pid, line, sizeof(line));
    clr();
    usbctl_line(line, put);
    CHECK(got("REFUSED this computer is not paired"), "a revoked key on a locked phone is refused");

    printf("usbctl-test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
