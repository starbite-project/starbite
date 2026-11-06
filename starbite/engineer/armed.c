/* SPDX-License-Identifier: GPL-2.0-only
 * Armed mode and the confirmation of every write (Starbite Lab, Engineer OS v2; docs/ENGINEER-OS.md
 * section 8).
 *
 * Every write in Engineer OS goes through lab_confirm, whatever the target:
 *   1. the warning screen: what happens, the target, old and new value, the backup, how to undo,
 *      and which confirmations follow; Continue or Cancel (hold Power = cancel);
 *   2. tier 2 and 3: the target's name typed on the keypad;
 *   3. the held confirmation: slide the knob, or hold Volume Up 1.5 s;
 *   4. tier 3: a second confirmation, hold Power 3 s (a red ring);
 *   5. tier 3 storage: lab_backup copies the target into the OS store (UFS, else the microSD's) and
 *      verifies it before the gate opens.
 * Each passed step is recorded in an authorisation (libsb fence.c struct sb_auth). The write itself
 * runs with the gate open on that authorisation: libsb refuses any write the authorisation does
 * not cover (range, tier, steps). Disarming or a reboot voids every authorisation.
 *
 * Armed mode (hold Power 2 s, then slide) is the session switch for the free-form tools (register
 * poke, memory fill, sector edit, PMIC writes): a red border on every frame, "ARMED" in the status
 * bar; it ends after 5 minutes without input, when the page it was armed on is left, and on reboot.
 * Writes are refused below 20 % battery without a charger (a write must not lose power).
 */
#include "eng.h"

static int armed;
static int arm_depth;

#define IDLE_MS (5U * 60U * 1000U)

/* ---- audit log ---------------------------------------------------------------------------- */

#define AUDIT_N 64
static char audit[AUDIT_N][120];
static unsigned audit_head, audit_count;

void audit_line(const char *s)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "audit: ");
    ub_s(&u, s);
    sb_log(u.b);
    ub_init(&u);
    uint64_t t = sb_ms() / 1000U;
    ub_dec(&u, t / 60U);
    ub_c(&u, ':');
    ub_dec2(&u, (unsigned)(t % 60U));
    ub_c(&u, ' ');
    ub_s(&u, s);
    sb_strcpy(audit[audit_head], sizeof(audit[0]), u.b);
    audit_head = (audit_head + 1) % AUDIT_N;
    if (audit_count < AUDIT_N)
        ++audit_count;
}

static void fill_audit(void)
{
    kv_hdr("THIS SESSION (NEWEST LAST)");
    if (!audit_count)
        kv_note("Nothing yet: every confirmation, write, backup and refusal is listed here.");
    for (unsigned i = 0; i < audit_count; ++i)
        kv_mono(audit[(audit_head + AUDIT_N - audit_count + i) % AUDIT_N]);
    kv_note("Each line is also in the Engineer OS log and the pmsg ring (TWRP: /sys/fs/pstore).");
}

void audit_page(void)
{
    info_screen("Audit log", fill_audit, 1000);
}

/* ---- armed mode ------------------------------------------------------------------------------ */

int armed_tier(void)
{
    return armed;
}

uint64_t armed_left_ms(void)
{
    uint64_t idle = ui_idle_ms();
    return armed && idle < IDLE_MS ? IDLE_MS - idle : 0;
}

void armed_disarm(const char *why)
{
    sb_auth_revoke_all();
    if (!armed)
        return;
    armed = 0;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "disarmed: ");
    ub_s(&u, why);
    audit_line(u.b);
    ui_guard(0);
    ui_dirty_all();
    E.redraw = 1;
}

void armed_tick(void)
{
    if (!armed)
        return;
    if (ui_idle_ms() >= IDLE_MS)
        armed_disarm("5 minutes without input");
    else if (E.depth < arm_depth)
        armed_disarm("left the page");
}

int lab_battery_ok(const char *what)
{
    struct sb_bat b;
    int have = sb_bat_read(&b);
    if (have && b.soc_pct < 20 && !b.input) {
        (void)dialog_danger("Battery too low", "Below 20 % and no charger: a write must not lose power.",
                            "Plug in a charger, then try again.", "OK");
        audit_line("refused: battery below 20 % without a charger");
        return 0;
    }
    if (!have) {
        if (!dialog_danger(what, "The fuel gauge does not answer: the battery level is unknown.",
                           "Continue only with a charger plugged in.", "CONTINUE"))
            return 0;
        audit_line("battery unknown: the user continued");
    }
    return 1;
}

int armed_arm(void)
{
    if (armed)
        return 1;
    if (!lab_battery_ok("Arm"))
        return 0;
    if (!ui_hold_power("Arm: hold Power", "Hold Power for 2 seconds (Bixby cancels)", 2000, C_RED)) {
        audit_line("arm cancelled (Power not held 2 s)");
        return 0;
    }
    static const char *const lines[] = {
        "Free-form tools: register poke, memory fill,",
        "sector edit, PMIC and charger writes.",
        "Every write still shows its warning and asks.",
        "Disarms after 5 minutes idle or leaving the page.",
    };
    if (!ui_confirm_slide("Arm Engineer OS", lines, 4, "slide to arm", C_RED)) {
        audit_line("arm cancelled at the slider");
        return 0;
    }
    armed = 1;
    arm_depth = E.depth;
    audit_line("armed");
    E.redraw = 1;
    return 1;
}

/* ---- typing the target's name ------------------------------------------------------------------ */

static const char keyset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_<";

int lab_type_name(const char *title, const char *part)
{
    char typed[24];
    unsigned n = 0;
    typed[0] = 0;
    int sel = 0, res = -1;
    const int kw = UI_W / 6, kh = 170, ky = 1500;
    ui_save_screen();
    ui_input_reset();
    while (res < 0) {
        ui_noclip();
        ui_fill(0, UI_VIEW_Y0, UI_W, UI_VIEW_Y1 - UI_VIEW_Y0, C_BG);
        ui_text(F_TITLE_S, 64, UI_VIEW_Y0 + 120, title, C_RED);
        struct ub u;
        ub_init(&u);
        ub_s(&u, "Type the target's name: ");
        ub_s(&u, part);
        ui_text_cut(F_ROW, 64, UI_VIEW_Y0 + 220, u.b, UI_W - 128, C_L2);
        ui_round(64, UI_VIEW_Y0 + 280, UI_W - 128, 170, 36, C_CARD);
        ui_text(F_TITLE_S, 110, UI_VIEW_Y0 + 395, typed[0] ? typed : "_", C_L1);
        ui_text(F_SMALL, 64, ky - 40, "Volume moves, Power types, hold Power cancels; < deletes", C_L3);
        for (int k = 0; k < 38; ++k) {
            int x = (k % 6) * kw, y = ky + (k / 6) * kh;
            ui_round(x + 10, y + 10, kw - 20, kh - 20, 26, k == sel ? C_STAR : C_CARD2);
            char s[2] = { keyset[k], 0 };
            ui_text_c(F_SEG, x + kw / 2, y + kh / 2 + 16, k == 37 ? "DEL" : s, k == sel ? C_BG : C_L1);
        }
        int dx = 2 * kw, dy = ky + 6 * kh;
        ui_round(dx + 10, dy + 10, 4 * kw - 20, kh - 20, 26, sel == 38 ? C_RED : ui_mix(C_RED, C_CARD, 90));
        ui_text_c(F_SEG, dx + 2 * kw, dy + kh / 2 + 16, "CONFIRM", C_L1);
        ui_present();
        struct ui_ev e;
        ui_poll(&e, 100);
        int key = -1;
        if (e.type == EV_UP)
            sel = (sel + 38) % 39;
        else if (e.type == EV_DOWN)
            sel = (sel + 1) % 39;
        else if (e.type == EV_SELECT)
            key = sel;
        else if (e.type == EV_BACK || e.type == EV_HOME)
            res = 0;
        else if (e.type == EV_TAP && e.y >= ky && e.y < ky + 7 * kh) {
            int r = (e.y - ky) / kh, c = e.x / kw;
            if (r == 6)
                key = c >= 2 ? 38 : -1;
            else
                key = r * 6 + c < 38 ? r * 6 + c : -1;
        }
        if (key == 37 && n) {
            typed[--n] = 0;
        } else if (key >= 0 && key < 37 && n < 23) {
            typed[n++] = keyset[key];
            typed[n] = 0;
        } else if (key == 38) {
            res = sb_streq(typed, part);
            if (!res) {
                ui_toast("That is not the target's name", C_RED, 900);
                res = -1;
            }
        }
    }
    ui_restore_screen();
    ui_present();
    ui_input_reset();
    return res;
}

/* ---- the warning screen -------------------------------------------------------------------------- */

static const struct action *wa;
static int wa_choice;

static const char *steps_text(int tier, unsigned kind)
{
    if (tier >= TIER3)
        return (kind & SB_KIND_STORAGE) ? "type the name, slide, hold Power 3 s; a verified backup first"
                                        : "type the name, slide, then hold Power 3 s";
    if (tier == TIER2)
        return "type the name, then slide";
    return "slide (or hold Volume Up 1.5 s)";
}

static void build_warning(void)
{
    const struct action *a = wa;
    kv_bigc(tier_name(a->tier), a->title, a->tier >= TIER3 ? C_RED : a->tier == TIER2 ? C_ORANGE : C_YELLOW);
    kv_hdr("WHAT HAPPENS");
    for (unsigned i = 0; i < 4 && a->happens[i]; ++i)
        kv_add(i ? "" : "What", a->happens[i]);
    if (a->target && a->target[0])
        kv_add("Target", a->target);
    if (a->old_v && a->old_v[0])
        kv_add("Now", a->old_v);
    if (a->new_v && a->new_v[0])
        kv_addc("After", a->new_v, C_ORANGE);
    kv_hdr("HOW TO UNDO");
    kv_add("Undo", a->undo && a->undo[0] ? a->undo : "-");
    if (a->backup && a->backup[0])
        kv_add("Backup", a->backup);
    kv_hdr("CONFIRMATION");
    kv_add("Next", steps_text(a->tier, a->kind));
    if (sb_fence_is_dry_run())
        kv_addc("Dry run", "ON: storage writes are logged, not made", C_ORANGE);
    kv_hdr("DECIDE");
    row_add("Continue", "to the confirmation", a->tier >= TIER3 ? R_RED : R_WARN);
    row_add("Cancel", "nothing is written", 0);
}

static int act_warning(int i)
{
    wa_choice = i == 0;
    return 1;
}

static int warning_screen(const struct action *a)
{
    wa = a;
    wa_choice = 0;
    list_screen("Before you write", build_warning, act_warning);
    if (E.home || E.tab_req >= 0)
        return 0;
    return wa_choice;
}

int lab_confirm(const struct action *a, struct sb_auth *auth)
{
    struct ub u;
    ub_init(&u);
    if (a->tier >= TIER_NO) {
        ub_s(&u, "refused (impossible): ");
        ub_s(&u, a->title);
        ub_c(&u, ' ');
        ub_s(&u, a->target ? a->target : "");
        audit_line(u.b);
        (void)dialog_danger("Not possible", a->happens[0] ? a->happens[0] : a->title,
                            "Physically impossible, or it would overwrite Engineer OS itself.", "OK");
        return 0;
    }
    sb_auth_init(auth, a->space, a->lu, a->first, a->count, a->tier < TIER1 ? TIER1 : a->tier, a->kind, a->title);
    if ((a->kind & SB_KIND_FREEFORM) && !armed) {
        if (!dialog("Arm first", "This tool writes anything you enter.", "Arm: hold Power 2 s, then slide.", "ARM") ||
            !armed_arm())
            return 0;
        sb_auth_init(auth, a->space, a->lu, a->first, a->count, a->tier < TIER1 ? TIER1 : a->tier, a->kind, a->title);
    }
    if (armed)
        sb_auth_step(auth, SB_STEP_ARMED);
    if (!lab_battery_ok(a->title))
        return 0;
    if (!warning_screen(a)) {
        ub_s(&u, "cancelled at the warning: ");
        ub_s(&u, a->title);
        audit_line(u.b);
        return 0;
    }
    sb_auth_step(auth, SB_STEP_WARN);
    if (a->tier >= TIER2) {
        if (!lab_type_name(a->title, a->target ? a->target : "YES")) {
            ub_s(&u, "cancelled at the typed name: ");
            ub_s(&u, a->title);
            audit_line(u.b);
            return 0;
        }
        sb_auth_step(auth, SB_STEP_TYPED);
    }
    struct ub l[7];
    const char *lines[7];
    const char *k[7] = { "What", "Target", "Now", "After", "Backup", "Undo", "Tier" };
    const char *v[7] = { a->title, a->target ? a->target : "", a->old_v ? a->old_v : "", a->new_v ? a->new_v : "",
                         a->backup && a->backup[0] ? a->backup : "none", a->undo ? a->undo : "", tier_name(a->tier) };
    for (int i = 0; i < 7; ++i) {
        ub_init(&l[i]);
        ub_s(&l[i], k[i]);
        ub_c(&l[i], '\t');
        ub_s(&l[i], v[i]);
        lines[i] = l[i].b;
    }
    if (!ui_confirm_slide(a->tier >= TIER3 ? "Confirm (1 of 2)" : "Confirm", lines, 7, "slide to confirm",
                          a->tier >= TIER3 ? C_RED : C_ORANGE)) {
        ub_s(&u, "cancelled at the slider: ");
        ub_s(&u, a->title);
        audit_line(u.b);
        return 0;
    }
    sb_auth_step(auth, SB_STEP_HELD);
    if (a->tier >= TIER3) {
        if (!ui_hold_power("Second confirmation", "Tier 3: hold Power 3 seconds to go ahead (Bixby cancels)", 3000,
                           C_RED)) {
            ub_s(&u, "cancelled at the second confirmation: ");
            ub_s(&u, a->title);
            audit_line(u.b);
            return 0;
        }
        sb_auth_step(auth, SB_STEP_SECOND);
    }
    ub_s(&u, "confirmed ");
    ub_s(&u, tier_name(a->tier));
    ub_s(&u, ": ");
    ub_s(&u, a->title);
    ub_c(&u, ' ');
    ub_s(&u, a->target ? a->target : "");
    audit_line(u.b);
    E.redraw = 1;
    return 1;
}

/* ---- progress of a long copy ------------------------------------------------------------------- */

static const char *pg_title, *pg_what;
static uint64_t pg_done, pg_total;

static void fill_progress(void)
{
    struct ub u;
    ub_init(&u);
    ub_dec(&u, pg_total ? pg_done * 100U / pg_total : 0);
    ub_s(&u, " %");
    kv_big(pg_what, u.b);
    ub_init(&u);
    ub_size(&u, pg_done * 4096U);
    ub_s(&u, " of ");
    ub_size(&u, pg_total * 4096U);
    kv_add("Copied", u.b);
    kv_note("Every block is read back and compared. Hold Power stops after the current chunk.");
}

static void progress(uint64_t done, uint64_t total)
{
    static uint64_t last;
    pg_done = done;
    pg_total = total;
    if (done == 0 || done >= total || sb_ms() - last > 300) {
        last = sb_ms();
        show_now(pg_title, fill_progress);
    }
}

/* Power held 0.6 s stops a long operation or a test. A key already down when the operation starts
 * (the hold that confirmed it) must be released first; a key held 3 s counts even then (Power
 * held always aborts). */
static uint64_t stop_down_at;
static int stop_wait_release;

void lab_stop_reset(void)
{
    stop_down_at = 0;
    stop_wait_release = (sb_keys() & SB_K_POWER) != 0;
}

int lab_stop_check(void)
{
    uint64_t now = sb_ms();
    if (!(sb_keys() & SB_K_POWER)) {
        stop_down_at = 0;
        stop_wait_release = 0;
        return 0;
    }
    if (!stop_down_at)
        stop_down_at = now;
    return now - stop_down_at >= (stop_wait_release ? 3000U : UI_POWER_LONG_MS);
}

static int stop_requested(void)
{
    return lab_stop_check();
}

/* ---- backups and restores ------------------------------------------------------------------- */

int lab_backup(unsigned medium, unsigned lu, uint64_t first, uint64_t count, const char *label, struct sb_auth *auth,
               char *where)
{
    struct sb_auth st;
    struct store_writer w;
    struct store_ent e;
    struct ub u;
    int r;
    if (where)
        where[0] = 0;
    unsigned dm = STORE_UFS;
    int in_ufs_store = medium == SB_MED_UFS && !lu && store_base(STORE_UFS) && first < store_base(STORE_UFS) +
                       store_capacity(STORE_UFS) && first + count > store_base(STORE_UFS);
    if (in_ufs_store || store_free(STORE_UFS) < count) {
        dm = STORE_SD;
        if (store_free(STORE_SD) < count) {
            (void)dialog_danger("No room for the backup", "Neither the UFS store nor the card's store has room.",
                                "Free space in the store (Starbite > OS store), or insert a card.", "OK");
            audit_line("backup refused: no room in either store");
            return -5;
        }
    }
    char name[32];
    for (unsigned n = 1; n < 1000; ++n) {
        ub_init(&u);
        ub_s(&u, "bak-");
        ub_s(&u, label);
        ub_c(&u, '-');
        ub_dec(&u, n);
        sb_strcpy(name, sizeof(name), u.b);
        ub_init(&u);
        ub_s(&u, dm == STORE_UFS ? "ufs:" : "sd:");
        ub_s(&u, name);
        if (!store_find(u.b, &e))
            break;
    }
    sb_auth_init(&st, dm, 0, 0, 0, TIER1, SB_KIND_STORE, "backup into the OS store");
    sb_auth_step(&st, SB_STEP_WARN);
    sb_auth_step(&st, SB_STEP_HELD);
    if (!sb_gate_open(&st))
        return SB_W_AUTH;
    pg_title = "Backup";
    pg_what = name;
    lab_stop_reset();
    r = store_wr_begin(&w, dm, name, ST_DATA, count * 4096U);
    for (uint64_t i = 0; !r && i < count;) {
        uint64_t n = count - i < 256U ? count - i : 256U;
        progress(i, count);
        if (sb_blk_read(medium, lu, first + i, n, (void *)SB_IO_BUF))
            r = SB_W_IO;
        else
            r = store_wr_chunk(&w, (const void *)SB_IO_BUF, n * 4096U);
        i += n;
        if (!r && i < count && stop_requested())
            r = -20;
    }
    if (!r)
        r = store_wr_commit(&w, &e);
    sb_gate_close();
    if (!r) {                                       /* the source once more: it must still match */
        uint64_t got = 0;
        int same = 1;
        for (uint64_t i = 0; i < count && same;) {
            uint64_t n = count - i < 256U ? count - i : 256U;
            if (sb_blk_read(medium, lu, first + i, n, (void *)SB_IO_BUF) ||
                sb_blk_read(e.medium, 0, e.lba + i, n, (void *)(SB_IO_BUF + (1U << 20))))
                same = 0;
            else
                same = !memcmp((const void *)SB_IO_BUF, (const void *)(SB_IO_BUF + (1U << 20)), n * 4096U);
            got += n;
            i += n;
        }
        if (!same || got != count)
            r = -21;
    }
    ub_init(&u);
    if (r) {
        ub_s(&u, "backup FAILED (");
        ub_sdec(&u, r);
        ub_s(&u, "): ");
        ub_s(&u, name);
        audit_line(u.b);
        (void)dialog_danger("Backup failed", r == -20 ? "Stopped with Power." : "The copy could not be written or verified.",
                            "Nothing else was written.", "OK");
        return r;
    }
    ub_s(&u, dm == STORE_UFS ? "UFS store: " : "card store: ");
    ub_s(&u, name);
    ub_s(&u, " (");
    ub_size(&u, count * 4096U);
    ub_s(&u, ", CRC ");
    ub_hexn(&u, e.crc, 8);
    ub_c(&u, ')');
    if (where)
        sb_strcpy(where, 64, u.b);
    struct ub a;
    ub_init(&a);
    ub_s(&a, "backup verified: ");
    ub_s(&a, u.b);
    audit_line(a.b);
    if (auth)
        sb_auth_step(auth, SB_STEP_BACKUP);
    return 0;
}

int lab_restore(const struct store_ent *e, unsigned medium, unsigned lu, uint64_t first, const struct sb_auth *auth)
{
    uint64_t count = (e->size + 4095U) / 4096U;
    int r = 0;
    if (!sb_gate_open(auth))
        return SB_W_AUTH;
    pg_title = "Restore";
    pg_what = e->name;
    lab_stop_reset();
    for (uint64_t i = 0; !r && i < count;) {
        uint64_t n = count - i < 256U ? count - i : 256U;
        progress(i, count);
        if (sb_blk_read(e->medium, 0, e->lba + i, n, (void *)SB_IO_BUF))
            r = SB_W_IO;
        else
            r = sb_write(medium, lu, first + i, n, (const void *)SB_IO_BUF);
        if (!r && (sb_blk_read(medium, lu, first + i, n, (void *)(SB_IO_BUF + (1U << 20))) ||
                   memcmp((const void *)SB_IO_BUF, (const void *)(SB_IO_BUF + (1U << 20)), n * 4096U)))
            r = SB_W_IO;
        i += n;
        if (!r && i < count && stop_requested())
            r = -20;
    }
    progress(count, count);
    sb_gate_close();
    struct ub u;
    ub_init(&u);
    ub_s(&u, r ? "restore FAILED: " : "restored and verified: ");
    ub_s(&u, e->name);
    audit_line(u.b);
    return r;
}

/* ---- register, PMIC and MAX77705 writes ------------------------------------------------------- */

static struct {
    int kind;                               /* 0 none, 1 register, 2 PMIC, 3 MAX77705 */
    uint64_t addr;
    uint32_t old;
} last;

static int probe_write32(uint64_t a, uint32_t v, int *serror)
{
    uint64_t c = sb_fault.count, isr = 0;
    sb_probe_depth++;
    __asm__ volatile("str %w0, [%1]\n\tdsb sy\n\tisb" : : "r"(v), "r"(a) : "memory");
    sb_probe_depth--;
#ifndef SB_HOST
    __asm__ volatile("mrs %0, isr_el1" : "=r"(isr));
#endif
    *serror = (isr >> 8) & 1U;
    return sb_fault.count == c;
}

int lab_poke32(uint64_t addr, uint32_t val, const struct sb_auth *auth)
{
    const char *why;
    int serr = 0;
    uint32_t old = 0, back = 0;
    int t = poke_tier(addr, &why);
    if ((addr & 3U) || t >= TIER_NO)
        return 0;
    if (!sb_gate_open(auth))
        return 0;
    int r = sb_gate_check(SB_SP_MEM, 0, addr, 4, t, &why);
    if (!r) {
        int had = sb_probe32(addr, &old);
        int ok = probe_write32(addr, val, &serr);
        (void)sb_probe32(addr, &back);
        if (ok) {
            last.kind = had ? 1 : 0;
            last.addr = addr;
            last.old = old;
        }
        struct ub u;
        ub_init(&u);
        ub_s(&u, ok ? "wrote " : "write FAULTED ");
        ub_hex(&u, addr, 9);
        ub_s(&u, " = ");
        ub_hex(&u, val, 8);
        ub_s(&u, " (was ");
        ub_hex(&u, old, 8);
        ub_s(&u, ", reads ");
        ub_hex(&u, back, 8);
        ub_s(&u, serr ? ", SError PENDING)" : ")");
        audit_line(u.b);
        r = ok ? 0 : -1;
    } else {
        audit_line(why);
    }
    sb_gate_close();
    return r == 0;
}

int lab_pmic(unsigned type, unsigned reg, unsigned val, const struct sb_auth *auth)
{
    const char *why;
    uint8_t old = 0;
    int t = pmic_tier(type, reg, val, &why);
    if (t >= TIER_NO || !sb_gate_open(auth))
        return 0;
    int had = sb_pmic_read(type, reg, &old);
    int ok = sb_pmic_write(type, reg, (uint8_t)val, t);
    sb_gate_close();
    if (ok && had) {
        last.kind = 2;
        last.addr = (uint64_t)type << 8 | reg;
        last.old = old;
    }
    return ok;
}

int lab_max(unsigned addr7, unsigned reg, unsigned val, const struct sb_auth *auth)
{
    const char *why;
    uint8_t old = 0;
    int t = max_tier(addr7, reg, val, &why);
    if (t >= TIER_NO || !sb_gate_open(auth))
        return 0;
    int had = addr7 == 0x36 ? 0 : (addr7 == 0x69 ? sb_chg_read((uint8_t)reg, &old) : 0);
    int ok = sb_max_write(addr7, (uint8_t)reg, (uint8_t)val, t);
    sb_gate_close();
    if (ok && had) {
        last.kind = 3;
        last.addr = (uint64_t)addr7 << 8 | reg;
        last.old = old;
    }
    return ok;
}

int lab_undo_available(char *what, unsigned cap)
{
    struct ub u;
    ub_init(&u);
    if (last.kind == 1)
        ub_s(&u, "register ");
    else if (last.kind == 2)
        ub_s(&u, "PMIC ");
    else if (last.kind == 3)
        ub_s(&u, "MAX77705 ");
    else
        return 0;
    ub_hex(&u, last.addr, last.kind == 1 ? 9 : 4);
    ub_s(&u, " back to ");
    ub_hex(&u, last.old, last.kind == 1 ? 8 : 2);
    if (what)
        sb_strcpy(what, cap, u.b);
    return 1;
}

void lab_undo(void)
{
    char what[64], tgt[24], oldv[24];
    struct sb_auth au;
    struct action a;
    const char *why = "";
    if (!lab_undo_available(what, sizeof(what)))
        return;
    struct ub u;
    ub_init(&u);
    ub_hexn(&u, last.addr, last.kind == 1 ? 8 : 4);
    sb_strcpy(tgt, sizeof(tgt), u.b);
    ub_init(&u);
    ub_hex(&u, last.old, last.kind == 1 ? 8 : 2);
    sb_strcpy(oldv, sizeof(oldv), u.b);
    a.title = "Undo the last write";
    a.target = tgt;
    a.happens[0] = what;
    a.happens[1] = 0;
    a.undo = "the write it undoes, again";
    a.old_v = "";
    a.new_v = oldv;
    a.backup = "";
    a.lu = 0;
    a.count = last.kind == 1 ? 4 : 1;
    a.first = last.addr;
    a.kind = SB_KIND_FREEFORM;
    if (last.kind == 1) {
        a.tier = poke_tier(last.addr, &why);
        a.space = SB_SP_MEM;
    } else if (last.kind == 2) {
        a.tier = pmic_tier((unsigned)(last.addr >> 8), (unsigned)(last.addr & 0xff), last.old, &why);
        a.space = SB_SP_PMIC;
    } else {
        a.tier = max_tier((unsigned)(last.addr >> 8), (unsigned)(last.addr & 0xff), last.old, &why);
        a.space = SB_SP_MAX;
    }
    if (!lab_confirm(&a, &au))
        return;
    int k = last.kind;
    uint64_t ad = last.addr;
    uint32_t ov = last.old;
    int ok = k == 1 ? lab_poke32(ad, ov, &au) : k == 2 ? lab_pmic((unsigned)(ad >> 8), (unsigned)(ad & 0xff), ov, &au)
                                                        : lab_max((unsigned)(ad >> 8), (unsigned)(ad & 0xff), ov, &au);
    if (ok)
        last.kind = 0;
    ui_toast(ok ? "Undone: the old value is back" : "Undo failed (see the audit log)", ok ? C_GREEN : C_RED, 1100);
}

/* ---- the Armed mode page ----------------------------------------------------------------------- */

static void build_armed(void)
{
    struct ub u;
    char what[64];
    kv_hdr("STATE");
    kv_addc("Armed mode", armed ? "ARMED: free-form tools write" : "off: free-form tools ask to arm",
            armed ? UI_RED : UI_GREEN);
    if (armed) {
        ub_init(&u);
        ub_dec(&u, armed_left_ms() / 1000U);
        ub_s(&u, " s without input");
        kv_add("Disarms after", u.b);
    }
    kv_addc("Storage writes", sb_fence_is_dry_run() ? "DRY RUN: logged, not made" : "real (after confirmation)",
            sb_fence_is_dry_run() ? UI_ORANGE : UI_TEXT);
    kv_hdr("ACTIONS");
    row_add(armed ? "Disarm" : "Arm", armed ? "back to safe; voids every confirmation" : "hold Power 2 s, then slide",
            armed ? 0 : R_WARN);
    row_add("Dry run for storage", "rehearse: every block write is logged only",
            R_CHECK | (sb_fence_is_dry_run() ? R_ON : 0));
    int u_ok = lab_undo_available(what, sizeof(what));
    row_add("Undo the last write", u_ok ? what : "nothing to undo", u_ok ? R_WARN : R_DIS);
    row_add("Audit log", "every confirmation, write, backup, refusal", 0);
    kv_hdr("TIERS");
    kv_add("Tier 0", "reading: free");
    kv_add("Tier 1", "warning + slide: OS partitions, the card, peripheral registers, free DRAM, touch and card "
                     "rails, the charger watchdog, settings, store backups");
    kv_addc("Tier 2", "+ type the name: BOOT, RECOVERY, UP_PARAM, RADIO, unknown partitions, the live store, CMU "
                      "gates, the UFS / card controllers, the key GPIOs, unknown registers",
            UI_ORANGE);
    kv_addc("Tier 3", "+ hold Power 3 s, backup first: EFS, CPEFS, PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOTA, "
                      "DTB, DTBO, GPT, boot LUs; PMIC voltages; MIF / INT / CPU / GPU clocks and DVFS; PMU; TMU; fuses",
            UI_RED);
    kv_addc("Impossible", "unmapped secure memory, S-Boot's ECT (0xA0000000), Engineer OS's own memory", UI_TEXT3);
}

static int act_armed(int i)
{
    switch (i) {
    case 0:
        if (armed)
            armed_disarm("by hand");
        else
            (void)armed_arm();
        break;
    case 1:
        if (sb_fence_is_dry_run()) {
            sb_fence_dry_run(0);
            audit_line("dry run off: storage writes are real again");
        } else {
            sb_fence_dry_run(1);
            audit_line("dry run on");
        }
        break;
    case 2:
        lab_undo();
        break;
    case 3:
        audit_page();
        break;
    default:
        break;
    }
    return 0;
}

void armed_page(void)
{
    list_screen("Armed mode", build_armed, act_armed);
}
