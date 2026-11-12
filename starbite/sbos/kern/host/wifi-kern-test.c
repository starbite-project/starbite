/* SPDX-License-Identifier: GPL-2.0-only
 * Host harness of Starbite OS's kernel Wi-Fi service (kern/wifi.c) on the real core (starbite/wifi) and its chip and
 * access point model (starbite/wifi/host/model.c, model_ap.c: the made-up network "test-ap", passphrase "correct
 * horse", 5 GHz). The kernel around it is wifi-kern-stubs.c: a fake clock, a timer table this file runs (the Wi-Fi
 * tick, and the self test's phase every 20 ms as uitest.c), the log in memory.
 *
 *   wifi-kern-test <scenario> [file]     one scenario per process (wifi.c's state is static: a process is a boot)
 *   -v first: the kernel's log as it happens
 *
 * Each check prints "ok: ..." or "FAIL: ..."; the exit status is the number of failures (capped). Every scenario
 * ends with the log checked for names, passphrases and addresses (none may be logged). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "sb.h"
#include "kern.h"
#include "sbapp.h"
#include "bcmw.h"
#include "sae.h"
#include "http.h"
#include "host/model.h"
#include "wifi-kern-stubs.h"

#define AP "test-ap"
#define PASS "correct horse"

static const char *scen = "?";
static unsigned checks, fails;

static void check(int ok, const char *fmt, ...)
{
    va_list a;
    checks++;
    printf("%s: %s: ", ok ? "ok" : "FAIL", scen);
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    printf("\n");
    if (!ok)
        fails++;
}

/* ---- the loop ---- */
static int selftest_on, selftest_done, selftest_ok;
static uint64_t selftest_done_at;
static unsigned spins;

static void dispatch(unsigned owner, unsigned id)
{
    if (owner == KT_WIFI)
        wifi_timer(id);
    else if (owner == KT_SELFTEST && selftest_on && !selftest_done) {
        int done = 0;
        int r = wifi_selftest_phase(&done);
        if (done) {
            selftest_done = 1;
            selftest_ok = r;
            selftest_done_at = sb_ms();
        }
    }
}

/* run the timers until cond() holds or ms pass; 1 if cond held */
static int run_until(int (*cond)(void), uint64_t ms)
{
    uint64_t end = sb_ms() + ms, last = 0;
    unsigned same = 0;
    for (;;) {
        if (cond && cond())
            return 1;
        uint64_t t = kt_next();
        if (t > end) {
            if (sb_ms() < end)
                wk_now_ms = end;
            return cond ? cond() : 1;
        }
        if (t > sb_ms())
            wk_now_ms = t;
        if (sb_ms() == last) {
            if (++same > 20000) {
                spins++;
                wk_now_ms++;            /* a timer re-armed at the same moment forever: counted, time moves */
                same = 0;
            }
        } else {
            same = 0;
            last = sb_ms();
        }
        unsigned o, i;
        while (kt_pop(sb_ms(), &o, &i))
            dispatch(o, i);
    }
}

static void run_ms(uint64_t ms) { run_until(0, ms); }

static int state(void)
{
    struct sbos_wifi s;
    wifi_status(&s);
    return s.state;
}
static int is_up(void) { return state() == SBOS_WIFI_UP; }
static int is_connected(void) { return state() == SBOS_WIFI_CONNECTED; }
static int is_off(void) { return state() == SBOS_WIFI_OFF; }
static int is_failed(void) { return state() == SBOS_WIFI_FAILED; }
static int is_settled(void)      /* connected and the clock check and power save decided */
{
    struct kwifi_details d;
    wifi_details(&d);
    return d.state == SBOS_WIFI_CONNECTED && d.power_save != 0;
}
static int st_done(void) { return selftest_done; }

/* ---- the log ---- */
static unsigned mark;
static void mark_log(void) { mark = wk_log_n(); }
static int logged_since(unsigned from, const char *s)
{
    for (unsigned i = from; i < wk_log_n(); ++i)
        if (strstr(wk_log_line(i), s))
            return 1;
    return 0;
}
static int logged(const char *s) { return logged_since(mark, s); }
static int count_since(unsigned from, const char *s)
{
    int n = 0;
    for (unsigned i = from; i < wk_log_n(); ++i)
        n += strstr(wk_log_line(i), s) != 0;
    return n;
}
static uint64_t until_logged(const char *s, uint64_t ms)  /* runs until the line; ms it took, or UINT64_MAX */
{
    uint64_t t0 = sb_ms();
    unsigned from = wk_log_n();
    while (sb_ms() - t0 < ms) {
        if (logged_since(from, s))
            return sb_ms() - t0;
        run_ms(10);
    }
    return logged_since(from, s) ? sb_ms() - t0 : UINT64_MAX;
}

static void privacy(void)
{
    /* (the core logs an AP's maker's half, "02:11:22:..:..:..": not an address) */
    static const char *const bad[] = { AP, PASS, "192.168.4.", "192.0.2.", "22:33:44", "33:44:5", "11:22:33:44" };
    int found = 0;
    for (unsigned i = 0; i < wk_log_n(); ++i)
        for (unsigned k = 0; k < sizeof(bad) / sizeof(bad[0]); ++k)
            if (strstr(wk_log_line(i), bad[k])) {
                if (!found)
                    printf("  the line: %s\n", wk_log_line(i));
                found = 1;
            }
    check(!found, "no network name, passphrase or address in the log (%u lines)", wk_log_n());
    check(!spins, "no timer re-armed at the same moment forever (%u)", spins);
    check(!(M.fw_errors & ~8u), "the model's firmware saw nothing wrong (%#x)", M.fw_errors);
}

static int find_net(const char *name)
{
    struct sbos_wifi_net n[SBOS_WIFI_NETS];
    int c = wifi_list(n, SBOS_WIFI_NETS);
    for (int i = 0; i < c; ++i)
        if (n[i].ssid_len == strlen(name) && !memcmp(n[i].ssid, name, n[i].ssid_len))
            return i;
    return -1;
}

static int power_on_scan(void)
{
    if (wifi_power(1))
        return 0;
    return run_until(is_up, 15000) && find_net(AP) >= 0;
}

static void wpa2_env(void)
{
    unsetenv("SBOS_HOST_WIFINET");
    setenv("SBOS_HOST_WPA2", "1", 1);
}

static void saved_env(void)
{
    unsetenv("SBOS_HOST_WPA2");
    setenv("SBOS_HOST_WIFINET", AP "\n" PASS, 1);
}

static void join_typed(void)
{
    int i = find_net(AP);
    check(i >= 0 && wifi_join(i, PASS) == 0, "the join with a typed passphrase started");
    check(run_until(is_settled, 30000), "CONNECTED with the clock checked and power save decided (state %d)", state());
}

/* the AP drops the phone (the event, then 100 ms of ticks: the kernel has seen it) */
static void inject_drop(uint32_t type)
{
    model_event(type, 0, 0, 3, 0, 0);
    run_ms(100);
    check(!is_connected(), "the drop seen (state %d)", state());
}

static int known_n(struct kwifi_known *k)
{
    return wifi_known2(k, 9);
}

/* ---- 1, 2, 4, 5, 7, 8, the fetch, the radio dying: the model's WPA2 network, typed ---- */
static void sc_main(void)
{
    struct kwifi_known k[9];
    struct kwifi_details d;
    struct sbos_wifi s;
    int i, n;
    wpa2_env();

    scen = "heater";
    wk_heater_busy = 1;
    check(wifi_power(1) == SBOS_EAGAIN && is_off(), "refused while heating, stays off");
    wk_heater_busy = 0;

    scen = "1 bring-up";
    mark_log();
    check(wifi_power(1) == 0 && state() == SBOS_WIFI_STARTING, "on: STARTING");
    check(run_until(is_up, 15000), "UP after the bring-up and the scan (state %d)", state());
    check(logged("wifi: up in ") && logged("wifi: the scan heard 1 networks"), "the log: up, the scan heard 1");
    i = find_net(AP);
    check(i >= 0, "the scan lists the model's AP");
    {
        struct sbos_wifi_net nl[SBOS_WIFI_NETS];
        n = wifi_list(nl, SBOS_WIFI_NETS);
        check(n == 1 && i == 0 && nl[0].secure && nl[0].band5 && nl[0].channel == 36 && nl[0].rssi == -48 && !nl[0].saved,
              "the listed network: secure, 5 GHz channel %d, %d dBm, not saved", nl[0].channel, nl[0].rssi);
    }
    check(wifi_net_sec(i) == KWIFI_SEC_WPA2 && wifi_net_sec(-1) == -1 && wifi_net_sec(1) == -1,
          "wifi_net_sec: WPA2 (%d), -1 out of range", wifi_net_sec(i));
    check(wifi_caps() == 0, "wifi_caps: none of sae/sae_ext/mfp/idsup in the model's default cap (%#x)", wifi_caps());
    check(logged("wifi: firmware cap: ap sta wme"), "the cap string logged");
    check(wifi_module() == 1, "wifi_module: SEMCO from the model's OTP (%d)", wifi_module());
    check(wifi_bar() == 1 && wifi_on(), "the status bar: on, not joined");
    check(!logged("automatically"), "nothing known: no automatic join");
    check(wifi_join(i, 0) == SBOS_EPERM, "secured, no passphrase known: SBOS_EPERM");
    check(wifi_join(i, "short") == SBOS_EINVAL, "a 5-character passphrase: SBOS_EINVAL");
    check(wifi_join(5, PASS) == SBOS_EAGAIN && wifi_join(-1, PASS) == SBOS_EAGAIN, "a bad index: SBOS_EAGAIN");

    scen = "on/off";
    for (int c = 0; c < 3; ++c) {
        wifi_power(0);
        check(is_off() && !wifi_on() && wifi_bar() == 0, "cycle %d: off", c);
        mark_log();
        check(power_on_scan(), "cycle %d: up and scanned again", c);
        check(logged("wifi: DMA blocks: 0 new,"), "cycle %d: the DMA blocks reused", c);
    }

    scen = "2 join";
    mark_log();
    unsigned sc0 = wk_settings_changed;
    join_typed();
    check(logged("wifi: joined a secured network in") && logged("wifi: an address from DHCP (not logged), lease 3600 s"),
          "the log: joined, DHCP (lease 3600 s)");
    check(logged("wifi: the gateway answers a ping"), "the router answered a ping");
    check(logged("wifi: the clock was set from the network") && wk_net_utc == 1794101400, "NTP set the clock (%lld)",
          (long long)wk_net_utc);
    check(logged("wifi: power save on (PM 2)") || logged("wifi: power save refused (PM 2)"), "power save decided");
    check(M.handshake_done && (M.keys_ok & 3) == 3 && M.ap_errors == 0, "the AP: handshake %d, keys %#x, faults %#x",
          M.handshake_done, M.keys_ok, M.ap_errors);
    wifi_details(&d);
    check(d.ssid_len == 7 && !memcmp(d.ssid, AP, 7) && d.sec == KWIFI_SEC_WPA2 && d.band5 == 1 && d.channel == 36,
          "details: the name, WPA2, 5 GHz channel 36");
    check(!memcmp(d.ip, "\xc0\xa8\x04\x17", 4) && !memcmp(d.mask, "\xff\xff\xff\x00", 4) &&
          !memcmp(d.gateway, "\xc0\xa8\x04\x01", 4) && !memcmp(d.dns, "\xc0\xa8\x04\x01", 4) && d.lease_s == 3600,
          "details: 192.168.4.23/24, router and DNS .1, lease 3600 s");
    check(d.power_save == 1 || d.power_save == -1, "details: power save %d", d.power_save);
    check(d.ping_ms >= 0, "details: the ping %d ms", d.ping_ms);
    wifi_status(&s);
    check(s.clock_checked == 3 && s.ssid_len == 7 && s.ping_ms >= 0, "status: clock set (%d), the name, ping",
          s.clock_checked);
    n = known_n(k);
    check(n == 1 && k[0].ssid_len == 7 && !memcmp(k[0].ssid, AP, 7) && !k[0].saved && k[0].heard && !k[0].hidden &&
          k[0].autojoin && k[0].sec == KWIFI_SEC_WPA2 && k[0].joined,
          "wifi_known2: remembered (n %d, heard, auto, WPA2, joined)", n);
    check(wk_settings_changed > sc0, "the settings are saved (settings_changed)");
    check(wifi_bar() == 2 && wifi_uplink(0, 0) == 1, "the status bar: joined; the uplink is Wi-Fi");
    check(wifi_join(i, PASS) == SBOS_EAGAIN, "a second join while connected: SBOS_EAGAIN");

    scen = "scan while joined";
    mark_log();
    check(wifi_scan() == 0, "a scan while connected starts");
    run_ms(3000);
    check(is_connected() && find_net(AP) >= 0, "still connected, the list kept");

    scen = "11 fetch";
    {
        struct sbos_wifi_fetch f;
        check(wifi_fetch("http://example.invalid/") == 0, "a fetch starts over the model's network");
        check(wifi_fetch("http://example.invalid/") == SBOS_EBUSY, "one at a time: SBOS_EBUSY");
        wifi_fetch_cancel();
        wifi_fetch_status(&f);
        check(f.state == 2 && f.result == SBHTTP_E_ABORTED, "cancelled: aborted (%d)", f.result);
        uint64_t t0 = sb_ms();
        check(wifi_fetch("http://example.invalid/") == 0, "again");
        for (int k2 = 0; k2 < 400; ++k2) {
            wifi_fetch_status(&f);
            if (f.state == 2)
                break;
            run_ms(100);
        }
        wifi_fetch_status(&f);
        check(f.state == 2 && f.result < 0 && sb_ms() - t0 <= 31000,
              "the model has no TCP server: the fetch ends by itself (result %d, %s) in %llu ms", f.result, f.why,
              (unsigned long long)(sb_ms() - t0));
        check(is_connected(), "still connected after the fetch");
        check(wifi_fetch("http://example.invalid/") == 0, "a fetch, then Wi-Fi off");
        run_ms(200);
        wifi_power(0);
        wifi_fetch_status(&f);
        check(f.state == 2 && f.result == SBHTTP_E_ABORTED, "Wi-Fi off ends the fetch (%d)", f.result);
        check(wifi_fetch("http://example.invalid/") == SBOS_EAGAIN, "off: SBOS_EAGAIN");
        mark_log();
        check(power_on_scan(), "on again");
        check(run_until(is_settled, 20000) && logged("joining it automatically"),
              "the remembered network joins by itself after a power-on");
    }

    scen = "4 drop";
    for (int r = 0; r < 3; ++r) {
        static const uint32_t ev[3] = { BCMW_E_DEAUTH_IND, BCMW_E_LINK, BCMW_E_DISASSOC_IND };
        mark_log();
        inject_drop(ev[r]);
        check(is_up() && (logged("left by the access point") || logged("the link went down")), "drop %d: UP, logged", r);
        check(wifi_uplink(0, 0) == 0, "drop %d: no uplink", r);
        uint64_t t = until_logged("trying to get back on a known network, attempt 1", 5000);
        check(t >= 1800 && t <= 2300, "drop %d: the first attempt after 2 s (%llu ms)", r, (unsigned long long)t);
        check(run_until(is_connected, 15000) && logged("joining it automatically"), "drop %d: back by itself", r);
    }
    /* the access point gone for a while: the attempts back off 2, 5, 15, 30, 60 s */
    run_until(is_settled, 20000);
    mark_log();
    model_ap_hidden = 1;                    /* not heard by the scans */
    inject_drop(BCMW_E_DEAUTH_IND);
    unsigned from = wk_log_n();
    run_ms(200000);
    {
        int a = count_since(from, "trying to get back on a known network, attempt");
        check(a >= 5, "the AP not heard: %d attempts in 200 s (2, 5, 15, 30, 60 s backoff: 5 expected)", a);
        check(logged("attempt 2") && logged("attempt 3"), "attempts 2 and 3 logged");
    }
    model_ap_hidden = 0;
    check(run_until(is_connected, 400000), "back once the AP is heard again (a background scan)");
    run_until(is_settled, 20000);

    scen = "5 leave";
    mark_log();
    check(wifi_leave() == 0 && is_up() && wifi_leave() == SBOS_EAGAIN, "left; a second leave: SBOS_EAGAIN");
    check(logged("left the network (asked)"), "logged");
    run_ms(60000);
    check(wifi_scan() == 0, "the user scans meanwhile");
    run_ms(480000);
    check(is_up() && !logged("automatically"), "9 min: no automatic rejoin (state %d)", state());
    check(run_until(is_connected, 75000) && logged("automatically"), "after 10 min: joined by itself again");
    run_until(is_settled, 20000);

    scen = "7 auto off, forget";
    n = known_n(k);
    check(wifi_known_auto(0, 0) == 0 && known_n(k) == 1 && !k[0].autojoin, "automatic join off for it");
    check(wifi_known_auto(5, 0) == SBOS_EINVAL && wifi_forget(5) == SBOS_EINVAL && wifi_forget(-1) == SBOS_EINVAL,
          "bad indexes: SBOS_EINVAL");
    mark_log();
    inject_drop(BCMW_E_DEAUTH_IND);
    run_ms(130000);
    check(is_up() && !logged("automatically"), "dropped: no automatic join for it (state %d)", state());
    check(wifi_known_auto(0, 1) == 0, "automatic join on again");
    check(run_until(is_connected, 310000), "joined by itself again (the next scan)");
    run_until(is_settled, 20000);
    mark_log();
    check(wifi_forget(0) == 0 && known_n(k) == 0, "forgotten");
    check(logged("a remembered network was forgotten"), "logged");
    inject_drop(BCMW_E_DEAUTH_IND);
    run_ms(130000);
    check(is_up() && !logged("automatically"), "forgotten: no automatic join");
    i = find_net(AP);
    check(i >= 0 && wifi_join(i, 0) == SBOS_EPERM, "forgotten: its passphrase is gone (SBOS_EPERM)");

    scen = "8 survival";
    join_typed();
    check(wifi_user_power(1) == 0 && wifi_user_on(), "the user's switch on");
    mark_log();
    wifi_survival(1);
    check(is_off() && logged("Survival mode: off"), "Survival: off");
    wifi_boot_time_check();
    run_ms(400000);
    check(is_off(), "Survival: nothing turns it on (400 s, the boot's check)");
    wifi_survival(1);
    check(is_off(), "Survival twice: still off");
    wifi_survival(0);
    check(state() != SBOS_WIFI_OFF && logged("Survival mode ended: on again"), "Survival ends: on again");
    check(run_until(is_connected, 30000), "and the remembered network joins by itself");
    check(wifi_user_power(0) == 0 && is_off() && !wifi_user_on(), "the user's switch off");
    wifi_survival(1);
    wifi_survival(0);
    run_ms(10000);
    check(is_off(), "Survival over with the switch off: stays off");
    wifi_survival(1);
    check(wifi_user_power(1) == 0 && run_until(is_connected, 40000), "Survival: the user may still turn it on by hand");
    wifi_survival(0);
    check(is_connected(), "Survival ends: stays on");

    scen = "radio dies";
    run_until(is_settled, 20000);
    for (int r = 0; r < 3; ++r) {
        mark_log();
        model_link_gone = 1;
        run_until(is_failed, 2000);
        check(is_failed() && logged("the chip stopped answering: off"), "death %d: FAILED, logged", r);
        model_link_gone = 0;
        run_ms(4000);
        check(is_failed(), "death %d: not before 5 s", r);
        run_ms(1500);
        if (r < 2) {
            check(logged("the radio is started again after it stopped answering, time") && run_until(is_connected, 30000),
                  "death %d: started again, joined", r);
        } else {
            check(is_failed() && !logged("started again"), "death 3: no third restart this boot");
        }
    }
    check(wifi_power(1) == 0 && run_until(is_connected, 30000), "the user turns it on again: joins");
    wifi_power(0);
    privacy();
}

/* ---- 3: the remembered network and the switch across a restart (two processes) ---- */
static void sc_persist_write(const char *file)
{
    static uint8_t half[STATE_HALF];
    struct state_buf b;
    scen = "3 persist (write)";
    wpa2_env();
    check(power_on_scan(), "up");
    join_typed();
    check(wifi_user_power(1) == 0 && wifi_user_on(), "the user's switch on");
    state_begin(&b, half, 7);
    check(wifi_state_put(&b) == 0, "wifi_state_put");
    uint32_t len = state_finish(&b);
    FILE *f = fopen(file, "wb");
    check(f && fwrite(half, 1, len, f) == len, "written (%u bytes)", len);
    if (f)
        fclose(f);
    wifi_power(0);
    privacy();
}

static void sc_persist_read(const char *file)
{
    static uint8_t half[STATE_HALF];
    struct kwifi_known k[9];
    uint64_t seq = 0;
    scen = "3 persist (read)";
    wpa2_env();
    /* a record with unusable entries first: skipped, the rest kept */
    {
        static uint8_t h2[STATE_HALF];
        static uint8_t r[8 + 4 * 100];
        struct state_buf b;
        memset(r, 0, sizeof(r));
        memcpy(r, "WKN2", 4);
        r[4] = 0, r[5] = 4;
        r[8 + 0] = 0;                                   /* no name */
        r[108] = 5, memcpy(r + 109, "abcde", 5), r[108 + 33] = 3;      /* a 3-character passphrase */
        r[208] = 33;                                    /* a 33-byte name */
        r[308] = 4, memcpy(r + 309, "wxyz", 4), r[308 + 98] = 3, r[308 + 99] = KWIFI_SEC_OPEN;   /* open, hidden, no auto */
        state_begin(&b, h2, 1);
        state_add(&b, "wifi.known", r, sizeof(r));
        state_finish(&b);
        wifi_state_get(h2);
        int n = known_n(k);
        check(n == 1 && k[0].ssid_len == 4 && k[0].hidden && !k[0].autojoin && k[0].sec == KWIFI_SEC_OPEN && !wifi_user_on(),
              "unusable entries skipped, the good one kept (n %d)", n);
        r[5] = 9;                                       /* more than 8: the record ignored */
        state_begin(&b, h2, 2);
        state_add(&b, "wifi.known", r, sizeof(r));
        state_finish(&b);
        wifi_state_get(h2);
        check(known_n(k) == 1, "a count over 8: ignored");
    }
    FILE *f = fopen(file, "rb");
    size_t len = f ? fread(half, 1, sizeof(half), f) : 0;
    if (f)
        fclose(f);
    check(len > 0 && state_valid(half, &seq) && seq == 7, "the half read back, valid (%zu bytes)", len);
    mark_log();
    wifi_state_get(half);
    check(logged("wifi: remembered networks restored: 1 (names not logged); Wi-Fi was on"), "restored: 1, was on");
    int n = known_n(k);
    check(n == 1 && k[0].ssid_len == 7 && !memcmp(k[0].ssid, AP, 7) && k[0].sec == KWIFI_SEC_WPA2 && k[0].autojoin &&
          !k[0].hidden && !k[0].heard, "wifi_known2 after the restart (n %d)", n);
    check(wifi_user_on() == 1, "the user's switch: on");
    mark_log();
    wifi_boot_time_check();
    check(logged("wifi: on at start (the user's switch was on)") && state() == SBOS_WIFI_STARTING, "on at start");
    check(run_until(is_connected, 30000) && logged("joining it automatically"), "joined without a passphrase");
    check(M.handshake_done && M.ap_errors == 0, "the handshake with the restored passphrase");
    wifi_power(0);
    privacy();
}

/* ---- 6: a hidden network, joined by its name ---- */
static int remembered_hidden(void)
{
    struct kwifi_known k[9];
    return known_n(k) == 1 && k[0].hidden;
}

static void sc_hidden(void)
{
    scen = "6 hidden";
    wpa2_env();
    model_ap_hidden = 1;
    check(wifi_join_name((const uint8_t *)AP, 7, PASS) == SBOS_EAGAIN, "off: SBOS_EAGAIN");
    check(wifi_power(1) == 0 && run_until(is_up, 15000), "up");
    check(find_net(AP) < 0 && logged_since(0, "the scan heard 0 networks"), "the hidden network is not listed");
    check(wifi_join_name((const uint8_t *)AP, 0, PASS) == SBOS_EAGAIN &&
          wifi_join_name((const uint8_t *)AP, 33, PASS) == SBOS_EAGAIN, "a name of 0 or 33 bytes: SBOS_EAGAIN");
    check(wifi_join_name((const uint8_t *)AP, 7, "short") == SBOS_EINVAL, "a short passphrase: SBOS_EINVAL");
    mark_log();
    check(wifi_join_name((const uint8_t *)AP, 7, PASS) == 0, "joining by its name");
    check(run_until(is_settled, 30000), "CONNECTED (state %d)", state());
    check(logged("joining a network by its name"), "logged");
    check(remembered_hidden(), "remembered as hidden");
    {
        struct kwifi_details d;
        wifi_details(&d);
        check(d.ssid_len == 7 && !memcmp(d.ssid, AP, 7), "details: its name");
    }
    mark_log();
    inject_drop(BCMW_E_DEAUTH_IND);
    check(run_until(is_connected, 60000), "dropped: back by itself (a remembered hidden network; state %d)", state());
    run_until(is_settled, 20000);
    /* a join by name left before it finished: the next ordinary join is not taken for a hidden one */
    model_ap_hidden = 0;
    check(wifi_leave() == 0, "left");
    struct kwifi_known k[9];
    if (known_n(k) == 1)
        wifi_forget(0);
    mark_log();
    wifi_scan();
    run_until(is_up, 10000);
    check(find_net(AP) >= 0, "the AP heard again (not hidden)");
    model_ap_hidden = 1;
    wifi_scan();
    run_until(is_up, 10000);
    check(find_net(AP) < 0 && wifi_join_name((const uint8_t *)AP, 7, PASS) == 0, "a join by name of a network not heard");
    check(wifi_leave() == 0, "... left at once");
    model_ap_hidden = 0;
    wifi_scan();
    run_until(is_up, 10000);
    int i = find_net(AP);
    check(i >= 0 && wifi_join(i, PASS) == 0 && run_until(is_connected, 30000), "an ordinary join from the list");
    check(known_n(k) == 1 && !k[0].hidden, "remembered as not hidden (hidden %d)", k[0].hidden);
    wifi_power(0);
    privacy();
}

/* ---- 9: the self tests (uitest.c's phase every tick) ---- */
static void sc_selftest(int mode, int saved, int band24)
{
    char nm[64];
    snprintf(nm, sizeof(nm), "9 self test %d%s%s", mode, saved ? " saved" : " no saved", band24 ? " 2.4 refused" : "");
    scen = strdup(nm);
    if (saved)
        saved_env();
    else
        wpa2_env();
    setenv("SBOS_HOST_SELFTEST", "1", 1);
    model_ap_listed24 = band24;
    wifi_selftest_scan_only = mode;
    selftest_on = 1;
    uint64_t t0 = sb_ms();
    kt_set(KT_SELFTEST, 0, sb_ms(), 20, 1);
    run_until(st_done, 300000);
    kt_cancel(KT_SELFTEST, 0);
    uint64_t took = selftest_done_at - t0;
    check(selftest_done, "finished (done=1) in %llu ms", (unsigned long long)took);
    check(mode != 2 || took <= 240000 + 1000, "within the 240 s budget");
    check(selftest_ok == 1, "passed (%d)", selftest_ok);
    check(is_off(), "Wi-Fi off at the end");
    check(count_since(0, "FAILED") == count_since(0, "HTTPS FAILED") + count_since(0, "wifi: http: FAILED"),
          "no FAILED in the log (but the HTTPS GET's: the model has no TCP server)");
    if (mode) {
        check(logged_since(0, "selftest: wifi: the TLS self-check passed"), "the TLS self-check passed");
    }
    check(logged_since(0, "selftest: wifi: up and scanned in") && logged_since(0, "selftest: wifi: networks heard: 1"),
          "up and scanned, 1 network");
    if (mode == 1)
        check(logged_since(0, "selftest: wifi: passed: scan only"), "scan only: passed");
    if (mode == 0)
        check(saved ? logged_since(0, "selftest: wifi: JOINED the saved network") : logged_since(0, "no saved network: no join"),
              "the saved network joined / none");
    if (mode == 2) {
        check(logged_since(0, "selftest: wifi: h1: heard by security: WPA2 1"), "h1: the security counts");
        check(logged_since(0, "selftest: wifi: h1: mfp"), "h1: the iovars read");
        if (!saved) {
            check(logged_since(0, "h1: no saved network on the phone: diagnostics only") &&
                  logged_since(0, "h1: summary: 2.4 GHz not tried, 5 GHz not tried"), "h1: diagnostics only");
        } else if (!band24) {
            check(logged_since(0, "h1: the saved network (name not logged): 2.4 GHz not heard, 5 GHz -48 dBm, security WPA2"),
                  "h1: the saved network heard on 5 GHz");
            check(logged_since(0, "h1: 5 GHz: JOINED: an address, the router answers"), "h1: 5 GHz joined");
            check(logged_since(0, "h1: 5 GHz: HTTPS FAILED") || logged_since(0, "h1: 5 GHz: HTTPS WORKS"),
                  "h1: the HTTPS GET ended (the model has no TCP server)");
            check(logged_since(0, "h1: power: radio interrupts in 5 s idle with PM 0") &&
                  logged_since(0, "h1: power: radio interrupts in 5 s idle with PM 2"), "h1: the power windows");
            check(logged_since(0, "h1: power: under PM 2 the router answered 5 of 5 pings"), "h1: 5 of 5 pings");
            check(logged_since(0, "h1: summary: 2.4 GHz not tried, 5 GHz JOINED"), "h1: the summary");
            check(!logged_since(0, "selftest: WARNING"), "no warning");
        } else {
            check(logged_since(0, "h1: the saved network (name not logged): 2.4 GHz -48 dBm, 5 GHz not heard"),
                  "h1: heard on 2.4 GHz only");
            check(logged_since(0, "h1: 2.4 GHz: did NOT join"), "h1: 2.4 GHz did not join");
            check(logged_since(0, "h1: summary: 2.4 GHz did not join") && logged_since(0, "5 GHz not tried"), "h1: the summary");
            check(logged_since(0, "selftest: WARNING: wifi: h1: 2.4 GHz did not join") && selftest_warnings == 1,
                  "h1: one warning");
        }
    }
    unsetenv("SBOS_HOST_SELFTEST");
    privacy();
}

/* ---- 10: WPA3 ---- */
static void wpa3_model(int sec, int ext)
{
    M.ap_sec = sec;
    M.cap_sae_ext = ext, M.cap_mfp = ext;
    model_sae = bcmw_sae_get_ops();
}

static void sc_wpa3(void)
{
    struct kwifi_known k[9];
    scen = "10 WPA3";
    wpa2_env();
    check(wifi_power(1) == 0, "on");
    wpa3_model(1, 1);                       /* after wifi_power's model_reset, before the bring-up's cap */
    check(run_until(is_up, 15000) && find_net(AP) >= 0, "up, listed");
    int i = find_net(AP);
    check(wifi_net_sec(i) == KWIFI_SEC_WPA3, "wifi_net_sec: WPA3 (%d)", wifi_net_sec(i));
    check(wifi_caps() == (KWIFI_CAP_SAE_EXT | KWIFI_CAP_MFP), "wifi_caps: sae_ext, mfp (%#x)", wifi_caps());
    mark_log();
    check(wifi_join(i, PASS) == 0 && run_until(is_settled, 30000), "joined (state %d)", state());
    check(M.sae_ok == 1 && M.akm_joined == 8 && M.ap_errors == 0 && M.handshake_done, "SAE (ok %u, AKM %u, faults %#x)",
          M.sae_ok, M.akm_joined, M.ap_errors);
    check(logged("the host's SAE"), "the log: the host's SAE");
    check(known_n(k) == 1 && k[0].sec == KWIFI_SEC_WPA3, "remembered as WPA3");
    /* the drop and back: SAE again from the remembered passphrase */
    inject_drop(BCMW_E_DEAUTH_IND);
    check(run_until(is_connected, 30000) && M.sae_ok == 2, "dropped: back by itself with SAE (ok %u)", M.sae_ok);
    wifi_power(0);
    /* without sae_ext: refused cleanly */
    check(wifi_power(1) == 0, "on again");
    wpa3_model(1, 0);
    check(run_until(is_up, 15000), "up");
    i = find_net(AP);
    mark_log();
    check(wifi_join(i, PASS) == 0, "a join without sae_ext starts");
    check(run_until(is_up, 20000) && !is_connected(), "refused: UP again (state %d)", state());
    check(logged("a WPA3-only network: this firmware cannot do SAE"), "the log says why");
    {
        struct sbos_wifi s;
        wifi_status(&s);
        check(s.why[0] != 0, "the status has a reason (%s)", s.why);
    }
    check(M.ap_errors == 0, "the AP saw nothing wrong (%#x)", M.ap_errors);
    run_ms(70000);
    check(is_up(), "no join loop: still UP after 70 s (state %d)", state());
    wifi_power(0);
    /* a transition network without sae_ext: WPA2-PSK */
    check(wifi_power(1) == 0, "on again");
    wpa3_model(2, 0);
    check(run_until(is_up, 15000), "up");
    i = find_net(AP);
    check(wifi_net_sec(i) == KWIFI_SEC_WPA23, "wifi_net_sec: WPA2/WPA3 (%d)", wifi_net_sec(i));
    check(run_until(is_connected, 30000) && M.akm_joined == 2, "transition: joined by itself with WPA2-PSK (AKM %u)",
          M.akm_joined);
    wifi_power(0);
    /* hash-to-element only with sae_ext */
    check(wifi_power(1) == 0, "on again");
    wpa3_model(3, 1);
    check(run_until(is_up, 15000), "up");
    check(run_until(is_connected, 30000) && M.sae_h2e_seen >= 1 && M.ap_errors == 0, "H2E only: joined by itself (H2E %u)",
          M.sae_h2e_seen);
    wifi_power(0);
    privacy();
}

/* ---- the open network ---- */
static void sc_open(void)
{
    struct kwifi_known k[9];
    static uint8_t h[STATE_HALF], r[8 + 8 * 100];
    struct state_buf b;
    scen = "open";
    unsetenv("SBOS_HOST_WPA2");
    unsetenv("SBOS_HOST_WIFINET");
    /* eight remembered already (made-up names, never in the air) */
    memcpy(r, "WKN2", 4);
    r[5] = 8;
    for (int e = 0; e < 8; ++e) {
        uint8_t *p = r + 8 + e * 100;
        p[0] = 5, memcpy(p + 1, "net-", 4), p[5] = (uint8_t)('0' + e);
        p[33] = 9, memcpy(p + 34, "not-real-", 9);
        p[99] = KWIFI_SEC_WPA2;
    }
    state_begin(&b, h, 1);
    state_add(&b, "wifi.known", r, sizeof(r));
    state_finish(&b);
    wifi_state_get(h);
    check(known_n(k) == 8, "eight remembered (%d)", known_n(k));
    wk_spi_ok = 1;                          /* the interrupt line "works" (none arrive: polling stays) */
    mark_log();
    check(power_on_scan(), "up, listed");
    check(logged("wifi: interrupts on"), "interrupts on");
    int i = find_net(AP);
    check(wifi_net_sec(i) == KWIFI_SEC_OPEN, "wifi_net_sec: open");
    check(wifi_join(i, 0) == 0 && run_until(is_settled, 30000), "joined");
    int n = known_n(k);
    check(n == 8 && k[0].ssid_len == 7 && !memcmp(k[0].ssid, AP, 7) && k[0].sec == KWIFI_SEC_OPEN && k[1].ssid[4] == '0' &&
          k[7].ssid[4] == '6', "remembered (open) first; the least recent of the eight went (n %d)", n);
    state_begin(&b, h, 2);
    check(wifi_state_put(&b) == 0 && state_finish(&b) == 24 + 28 + 808, "the record: 8 networks, 808 bytes");
    mark_log();
    for (int q = 0; q < 2001; ++q)
        wifi_irq();
    check(logged("an interrupt storm"), "2001 interrupts in a ms: a storm, interrupts off");
    run_ms(3000);
    check(is_connected(), "still connected after the storm (polling)");
    inject_drop(BCMW_E_LINK);
    check(run_until(is_connected, 30000), "dropped: back by itself");
    wifi_power(0);
    privacy();
}

/* ---- the boot's time check (no clock) ---- */
static void sc_time_only(void)
{
    scen = "time check";
    saved_env();
    wk_clock_utc = 0;
    mark_log();
    wifi_boot_time_check();
    check(wifi_time_only() && logged("no clock at boot: on once for the time"), "no clock: on for the time");
    check(run_until(is_off, 60000), "off again (state %d)", state());
    check(logged("the boot's time check ends: the clock is set; Wi-Fi off again") && wk_net_utc == 1794101400,
          "the clock set, then off");
    check(!wifi_user_on(), "the user's switch stays off");
    {
        struct kwifi_known k[9];
        int n = known_n(k);
        /* (joined once, the saved network is also a remembered one: listed once, its automatic join can be turned
         * off then, and its passphrase is in the state slot's record too) */
        check(n == 1 && k[0].saved && k[0].autojoin && k[0].sec == KWIFI_SEC_WPA2, "wifi_known2: the saved entry, listed "
              "once (n %d)", n);
        check(wifi_forget(0) == SBOS_EPERM, "the saved entry is not forgotten (SBOS_EPERM)");
    }
    wifi_power(0);
    privacy();
}

/* Survival during the boot's time check: when Survival ends, the user's switch (off) decides */
static void sc_time_survival(void)
{
    scen = "time check + Survival";
    saved_env();
    wk_clock_utc = 0;
    mark_log();
    wifi_boot_time_check();
    run_ms(200);
    check(wifi_time_only(), "on for the time again");
    wifi_survival(1);
    check(is_off() && !wifi_time_only(), "Survival: off, the time check ended");
    wifi_survival(0);
    run_ms(5000);
    check(is_off(), "Survival ends: off (the time check is not the user's switch; state %d)", state());
    wifi_power(0);
    privacy();
}

int main(int argc, char **argv)
{
    int a = 1;
    if (a < argc && !strcmp(argv[a], "-v"))
        wk_verbose = 1, ++a;
    const char *s = a < argc ? argv[a] : "main";
    const char *file = a + 1 < argc ? argv[a + 1] : "wifi-known.bin";
    K.screen_on = 1;
    if (!strcmp(s, "main"))
        sc_main();
    else if (!strcmp(s, "persist-write"))
        sc_persist_write(file);
    else if (!strcmp(s, "persist-read"))
        sc_persist_read(file);
    else if (!strcmp(s, "hidden"))
        sc_hidden();
    else if (!strcmp(s, "wpa3"))
        sc_wpa3();
    else if (!strcmp(s, "open"))
        sc_open();
    else if (!strcmp(s, "time"))
        sc_time_only();
    else if (!strcmp(s, "time-survival"))
        sc_time_survival();
    else if (!strcmp(s, "selftest0"))
        sc_selftest(0, 1, 0);
    else if (!strcmp(s, "selftest1"))
        sc_selftest(1, 0, 0);
    else if (!strcmp(s, "h1-saved"))
        sc_selftest(2, 1, 0);
    else if (!strcmp(s, "h1-nosaved"))
        sc_selftest(2, 0, 0);
    else if (!strcmp(s, "h1-24"))
        sc_selftest(2, 1, 1);
    else {
        fprintf(stderr, "wifi-kern-test: no scenario %s\n", s);
        return 2;
    }
    printf("wifi-kern-test %s: %u checks, %u failed\n", s, checks, fails);
    return fails > 100 ? 100 : (int)fails;
}
