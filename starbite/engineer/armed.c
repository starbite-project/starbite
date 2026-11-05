/* SPDX-License-Identifier: GPL-2.0-only
 * Armed mode, in dry-run (docs/STARBITE-MASTER-PLAN.md section 4.5 "Armed mode", EN3).
 *
 *   tier 0   the default: read only
 *   tier 1   armed: hold Power 2 s, then slide; battery >= 30 %. Peripheral rails within policy,
 *            register writes in allowlisted blocks, Starbite-owned store areas, OS partitions,
 *            the card
 *   tier 2   a second unlock: type the partition name. BOOT, RECOVERY, UP_PARAM
 *   never    the compiled-in exclusions below, at any tier
 *
 * Arming lasts while the page it was armed on is open, at most 5 minutes without input, and
 * never across a reboot (it is RAM). While armed every frame has a red border and the status bar
 * says ARMED. Every risky action shows a change card (target, old value, new value, backup
 * location), and after the slide it is logged as a dry run: v1 writes nothing, whatever the tier.
 */
#include "eng.h"

static int tier;
static int arm_depth;
static uint64_t armed_at;

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
        kv_note("Nothing yet: arm the phone or open a change card.");
    for (unsigned i = 0; i < audit_count; ++i)
        kv_mono(audit[(audit_head + AUDIT_N - audit_count + i) % AUDIT_N]);
    kv_note("Each line is also in the Engineer OS log and the pmsg ring (TWRP: /sys/fs/pstore).");
}

void audit_page(void)
{
    info_screen("Audit log", fill_audit, 1000);
}

/* ---- state ---------------------------------------------------------------------------------- */

int armed_tier(void)
{
    return tier;
}

uint64_t armed_left_ms(void)
{
    uint64_t idle = ui_idle_ms();
    return tier && idle < IDLE_MS ? IDLE_MS - idle : 0;
}

void armed_disarm(const char *why)
{
    if (!tier)
        return;
    tier = 0;
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
    if (!tier)
        return;
    if (ui_idle_ms() >= IDLE_MS)
        armed_disarm("5 minutes without input");
    else if (E.depth < arm_depth)
        armed_disarm("left the page");
}

int armed_arm(void)
{
    if (tier)
        return 1;
    struct sb_bat b;
    int have = sb_bat_read(&b);
    if (have && b.soc_pct < 30) {
        (void)dialog_danger("Cannot arm", "The battery is below 30 %.", "Charge it first: a write must not lose power.",
                            "OK");
        audit_line("arm refused: battery below 30 %");
        return 0;
    }
    if (!ui_hold_power("Arm: hold Power", "Hold Power for 2 seconds (Bixby cancels)", 2000, C_RED)) {
        audit_line("arm cancelled (Power not held 2 s)");
        return 0;
    }
    static const char *const lines[] = {
        "Tier 1: peripheral rails, allowlisted registers,",
        "OS partitions, Starbite store areas, the card.",
        "v1 is DRY RUN: nothing is ever written.",
        "Disarms after 5 minutes idle or leaving the page.",
    };
    if (!ui_confirm_slide("Arm Engineer OS", lines, 4, "slide to arm", C_RED)) {
        audit_line("arm cancelled at the slider");
        return 0;
    }
    tier = TIER1;
    arm_depth = E.depth;
    armed_at = sb_ms();
    audit_line(have ? "armed: tier 1 (battery ok)" : "armed: tier 1 (battery unreadable: writes would be refused)");
    E.redraw = 1;
    return 1;
}

/* ---- tier 2: type the partition name ------------------------------------------------------ */

static const char keyset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_<";

int armed_tier2(const char *part)
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
        ui_text(F_TITLE_S, 64, UI_VIEW_Y0 + 120, "Tier 2 unlock", C_RED);
        struct ub u;
        ub_init(&u);
        ub_s(&u, "Type the partition name: ");
        ub_s(&u, part);
        ui_text(F_ROW, 64, UI_VIEW_Y0 + 220, u.b, C_L2);
        ui_round(64, UI_VIEW_Y0 + 280, UI_W - 128, 170, 36, C_CARD);
        ui_text(F_TITLE_S, 110, UI_VIEW_Y0 + 395, typed[0] ? typed : "_", C_L1);
        ui_text(F_SMALL, 64, ky - 40, "Volume moves, Power types, Bixby cancels; < deletes", C_L3);
        /* keys 0..36 letters, digits, '_'; 37 delete; 38 UNLOCK (the wide key) */
        for (int k = 0; k < 38; ++k) {
            int x = (k % 6) * kw, y = ky + (k / 6) * kh;
            ui_round(x + 10, y + 10, kw - 20, kh - 20, 26, k == sel ? C_STAR : C_CARD2);
            char s[2] = { keyset[k], 0 };
            ui_text_c(F_SEG, x + kw / 2, y + kh / 2 + 16, k == 37 ? "DEL" : s, k == sel ? C_BG : C_L1);
        }
        int dx = 2 * kw, dy = ky + 6 * kh;
        ui_round(dx + 10, dy + 10, 4 * kw - 20, kh - 20, 26, sel == 38 ? C_RED : ui_mix(C_RED, C_CARD, 90));
        ui_text_c(F_SEG, dx + 2 * kw, dy + kh / 2 + 16, "UNLOCK", C_L1);
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
                ui_toast("That is not the partition name", C_RED, 900);
                res = -1;
            }
        }
    }
    ui_restore_screen();
    ui_present();
    ui_input_reset();
    if (res) {
        tier = TIER2;
        struct ub u;
        ub_init(&u);
        ub_s(&u, "tier 2 unlocked for ");
        ub_s(&u, part);
        audit_line(u.b);
        E.redraw = 1;
    }
    return res;
}

/* ---- change cards -------------------------------------------------------------------------------- */

static const char *tier_name(int t)
{
    return t == TIER0 ? "tier 0" : t == TIER1 ? "tier 1" : t == TIER2 ? "tier 2" : "never";
}

int armed_change(const struct change *c)
{
    struct ub a;
    if (c->tier >= TIER_NEVER) {
        ub_init(&a);
        ub_s(&a, "REFUSED (never at any tier): ");
        ub_s(&a, c->what);
        ub_s(&a, " ");
        ub_s(&a, c->target);
        audit_line(a.b);
        (void)dialog_danger("Never allowed", c->target, "This target is on the compiled-in exclusion list.", "OK");
        return SB_W_NEVER;
    }
    if (tier < c->tier) {
        ub_init(&a);
        ub_s(&a, "This change needs ");
        ub_s(&a, tier_name(c->tier));
        ub_s(&a, ".");
        if (!dialog("Arm first", a.b, "Nothing is written in v1: the change card is a dry run.", "ARM"))
            return SB_W_TIER;
        if (!armed_arm())
            return SB_W_TIER;
        if (c->tier == TIER2 && !armed_tier2(c->target))
            return SB_W_TIER;
    }
    struct ub l[7];
    const char *lines[7];
    const char *k[7] = { "What", "Target", "Old", "New", "Backup", "Then", "Mode" };
    const char *v[7] = { c->what, c->target, c->old_v, c->new_v, c->backup,
                         "backup, verify, write, verify, log", "DRY RUN: nothing is written" };
    for (int i = 0; i < 7; ++i) {
        ub_init(&l[i]);
        ub_s(&l[i], k[i]);
        ub_c(&l[i], '\t');
        ub_s(&l[i], v[i]);
        lines[i] = l[i].b;
    }
    if (!ui_confirm_slide("Change card", lines, 7, "slide to dry-run", C_ORANGE)) {
        audit_line("change card cancelled");
        return 0;
    }
    ub_init(&a);
    ub_s(&a, "DRY RUN ");
    ub_s(&a, tier_name(c->tier));
    ub_s(&a, ": ");
    ub_s(&a, c->what);
    ub_s(&a, " ");
    ub_s(&a, c->target);
    ub_s(&a, " ");
    ub_s(&a, c->old_v);
    ub_s(&a, " -> ");
    ub_s(&a, c->new_v);
    ub_s(&a, "; nothing written");
    audit_line(a.b);
    ui_toast("Dry run logged: nothing was written", C_ORANGE, 1100);
    return SB_W_DRYRUN;
}

/* ---- register policy ------------------------------------------------------------------------------
 * Reads: the blocks Engineer Lite's peek knows to answer (em.c peek_safe) and DRAM outside the
 * secure carve-outs, if libsb maps them. Pokes: never inside the compiled-in exclusions (every CMU
 * block, the ACPM/APM mailbox and SRAM, DREX/DMC and the MIF region, the whole PMU with its DVFS
 * registers, CHIPID / fuses, the TZPC and secure blocks, DRAM), tier 1 only in the allowlisted
 * peripheral blocks. */

struct range {
    uint64_t a, n;
    const char *why;
};

static const struct range never[] = {
    { 0x02000000ULL, 0x00100000ULL, "APM SRAM (ACPM)" },
    { 0x10000000ULL, 0x00010000ULL, "CHIPID / fuses" },
    { 0x10020000ULL, 0x00010000ULL, "CMU_PERIS" },
    { 0x10400000ULL, 0x00010000ULL, "CMU_PERIC0" },
    { 0x10800000ULL, 0x00010000ULL, "CMU_PERIC1" },
    { 0x11000000ULL, 0x00010000ULL, "CMU_FSYS0" },
    { 0x11400000ULL, 0x00010000ULL, "CMU_FSYS1" },
    { 0x14000000ULL, 0x00010000ULL, "CMU_APM" },
    { 0x14060000ULL, 0x00010000ULL, "PMU (DVFS, reset, power)" },
    { 0x14100000ULL, 0x00010000ULL, "ACPM/APM mailbox" },
    { 0x14200000ULL, 0x00010000ULL, "CMU_CMGP" },
    { 0x16000000ULL, 0x00010000ULL, "CMU_DPU" },
    { 0x17400000ULL, 0x00010000ULL, "CMU_G3D" },
    { 0x17c00000ULL, 0x00010000ULL, "CMU_AUD" },
    { 0x1a000000ULL, 0x00400000ULL, "CMU_TOP / CORE / BUSC (MIF and INT muxes)" },
    { 0x1b800000ULL, 0x00800000ULL, "MIF: CMU_MIF0-3, DREX / DMC, PHY" },
    { 0x1d000000ULL, 0x00010000ULL, "CMU_CPUCL0" },
    { 0x1d100000ULL, 0x00010000ULL, "CMU_CPUCL1" },
    { 0x80000000ULL, 0x900000000ULL - 0x80000000ULL, "DRAM (both banks)" },
};

static const struct range poke_ok[] = {
    { 0x10430000ULL, 0x1000ULL, "GPIO_PERIC0 (pins)" },
    { 0x104b0000ULL, 0x1000ULL, "HSI2C USI03 (touch bus)" },
    { 0x11430000ULL, 0x1000ULL, "GPIO_FSYS1 (card pins)" },
    { 0x14220000ULL, 0x1000ULL, "GPIO_CMGP (pins)" },
    { 0x14360000ULL, 0x1000ULL, "HSI2C USI_CMGP03 (MAX77705 bus)" },
};

int poke_tier(uint64_t a, const char **why)
{
    for (unsigned i = 0; i < sizeof(never) / sizeof(never[0]); ++i)
        if (a >= never[i].a && a - never[i].a < never[i].n) {
            *why = never[i].why;
            return TIER_NEVER;
        }
    for (unsigned i = 0; i < sizeof(poke_ok) / sizeof(poke_ok[0]); ++i)
        if (a >= poke_ok[i].a && a - poke_ok[i].a < poke_ok[i].n) {
            *why = poke_ok[i].why;
            return TIER1;
        }
    *why = "not on the poke allowlist";
    return TIER_NEVER;
}

int peek_allowed(uint64_t a)
{
    if (!sb_map_kind(a))
        return 0;
    if (a >= 0x80000000ULL && a < 0x100000000ULL)
        return !(a >= 0xaf800000ULL && a < 0xc0080000ULL) &&      /* RKP, TIMA, secure world, seclog */
               !(a >= 0xe1900000ULL && a < 0xe3700000ULL) &&      /* secure camera */
               !(a >= 0xf0000000ULL && a < 0xf9200000ULL);        /* modem */
    if (a >= 0x880000000ULL && a < 0x900000000ULL)
        return 1;
    switch (a & ~0xffffULL) {
    case 0x10000000: case 0x10050000: case 0x10070000:
    case 0x10400000: case 0x10410000: case 0x10430000: case 0x104b0000:
    case 0x11120000: case 0x11430000: case 0x11500000:
    case 0x14050000: case 0x14060000:
    case 0x14200000: case 0x14210000: case 0x14220000: case 0x14360000:
    case 0x16030000: case 0x16080000:
    case 0x1a240000: case 0x1b800000: case 0x1d000000:
        return 1;
    default:
        return 0;
    }
}

/* ---- the Armed mode page ----------------------------------------------------------------------- */

static void build_armed(void)
{
    struct ub u;
    kv_hdr("STATE");
    kv_addc("Tier", tier == TIER0 ? "0: read only" : tier == TIER1 ? "1: armed" : "2: armed + unlocked",
            tier ? UI_RED : UI_GREEN);
    if (tier) {
        ub_init(&u);
        ub_dec(&u, armed_left_ms() / 1000U);
        ub_s(&u, " s without input");
        kv_add("Disarms after", u.b);
    }
    kv_addc("Writes", "none in v1: every change is a dry run", UI_ORANGE);
    kv_hdr("ACTIONS");
    row_add(tier ? "Disarm" : "Arm (tier 1)", tier ? "back to read only" : "hold Power 2 s, then slide", tier ? 0 : R_WARN);
    row_add("Unlock tier 2", "type a partition name (BOOT)", tier ? R_WARN : R_DIS);
    kv_hdr("DRY-RUN EXAMPLES");
    row_add("Register write in GPIO_CMGP", "allowlisted block: tier 1", 0);
    row_add("Register write in CMU_TOP", "MIF/INT muxes: never", 0);
    row_add("Touch rails on", "peripheral PMIC rail: tier 1", 0);
    row_add("Write BOOT", "boot chain partition: tier 2", 0);
    row_add("Write EFS", "never-touch partition", 0);
    row_add("Audit log", "every arm, disarm and dry run", 0);
    kv_hdr("TIERS");
    kv_add("Tier 0", "read only (default)");
    kv_add("Tier 1", "peripheral rails within policy, allowlisted registers, OS partitions, Starbite store areas, the card, USB");
    kv_add("Tier 2", "BOOT, RECOVERY, UP_PARAM; never-touch partitions only as \"restore this phone's verified backup\"");
    kv_addc("Never", "boot LUNs, MIF/INT, CMU blocks, ACPM/APM mailbox and SRAM, DREX/DMC, PMU DVFS, memory and CPU rails, fuses, secure registers, modem NV, IMEI",
            UI_RED);
}

static int act_armed(int i)
{
    struct change c = { 0, 0, 0, 0, 0, TIER1 };
    const char *why = 0;
    uint64_t first = 0, last = 0;
    struct ub t;
    switch (i) {
    case 0:
        if (tier)
            armed_disarm("by hand");
        else
            (void)armed_arm();
        break;
    case 1:
        if (tier)
            (void)armed_tier2("BOOT");
        break;
    case 2: {
        uint32_t cur = 0;
        ub_init(&t);
        if (sb_probe32(0x14220180ULL, &cur))
            ub_hex(&t, cur, 8);
        else
            ub_s(&t, "not readable");
        c.what = "Register write";
        c.target = "0x14220180 (GPM14 CON)";
        c.old_v = t.b;
        c.new_v = "0x00000002";
        c.backup = "the old value, in the audit log";
        c.tier = poke_tier(0x14220180ULL, &why);
        (void)armed_change(&c);
        break;
    }
    case 3:
        c.what = "Register write";
        c.target = "0x1A2410A8 (CMU_TOP MIF switch mux)";
        c.old_v = "-";
        c.new_v = "-";
        c.backup = "-";
        c.tier = poke_tier(0x1a2410a8ULL, &why);
        (void)armed_change(&c);
        break;
    case 4:
        touch_rails();
        break;
    case 5:
        (void)gpt_find("BOOT", &first, &last);
        ub_init(&t);
        ub_s(&t, "BOOT (UFS ");
        ub_dec(&t, first);
        ub_s(&t, "..");
        ub_dec(&t, last);
        ub_c(&t, ')');
        c.what = "Partition";
        c.target = "BOOT";
        c.old_v = "SHA-256 of today's BOOT";
        c.new_v = "SHA-256 of the new image";
        c.backup = "card: backups/<serial>/BOOT.img + manifest";
        c.tier = TIER2;
        if (armed_change(&c) == SB_W_DRYRUN && first)
            (void)sb_write(SB_MED_UFS, first, 1, 0, tier);
        break;
    case 6: {
        struct sb_wdecide d;
        (void)gpt_find("EFS", &first, &last);
        int r = first ? sb_write_decide(SB_MED_UFS, first, 1, &d) : SB_W_NEVER;
        c.what = "Partition";
        c.target = "EFS";
        c.tier = r == SB_W_NEVER || !first ? TIER_NEVER : d.tier;
        (void)armed_change(&c);
        if (first)
            (void)sb_write(SB_MED_UFS, first, 1, 0, tier);
        break;
    }
    case 7:
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
