/* SPDX-License-Identifier: GPL-2.0-only
 * Engineer OS pages that are new (not in the loader's EngineerMode): Starbite management (catalog,
 * settings record, GPT viewer, write fence), logs (this session, the loader's handoff page), the
 * "coming" pages and About.
 */
#include "eng.h"

/* ---- logs ------------------------------------------------------------------------------------- */

void logs_engineer(void)
{
    tv_reset();
    for (unsigned i = 0; i < sb_log_count(); ++i)
        tv_line(sb_log_line(i));
    text_screen("Engineer OS log");
}

static void fill_loader(void)
{
    volatile uint64_t *l = (volatile uint64_t *)SB_LOADER_LOG;
    struct ub u;
    kv_hdr("LOADER LOG PAGE 0x91FFF000");
    kv_addc("Tag", l[0] == 0x31304554524c5453ULL ? "STRLTE01" : "not found",
            l[0] == 0x31304554524c5453ULL ? C_GREEN : C_RED);
    kv_hex("CurrentEL at S-Boot handoff", l[1], 2);
    kv_hex("SCTLR_EL1 from S-Boot", l[2], 8);
    kv_hex("S-Boot FDT", l[3], 9);
    kv_hex("Loader at", l[4], 9);
    ub_init(&u);
    ub_hex(&u, l[10], 4);
    ub_s(&u, l[10] == 0x600 ? "  (bare image handed off)" : "");
    kv_add("Last step (slot 10)", u.b);
    kv_hex("Entry (slot 15)", l[15], 9);
    kv_hex("x0 for the OS (slot 17)", l[17], 9);
    kv_hex("DECON trigger before (slot 77)", l[77], 8);
    kv_hex("S-Boot CPACR (slot 119)", l[119], 8);
    kv_hdr("THIS IMAGE");
    kv_hex("Running at", sb_boot.base, 9);
    kv_addc("Entry matches the loader's", sb_boot.base == l[15] ? "yes" : "no",
            sb_boot.base == l[15] ? C_GREEN : C_ORANGE);
    kv_hex("x0 (FDT) received", sb_boot.fdt, 9);
    kv_hex("CurrentEL", sb_boot.el, 2);
    kv_hex("SCTLR_EL1 at entry", sb_boot.sctlr_in, 8);
    kv_add("MMU at entry", (sb_boot.sctlr_in & 1) ? "ON (unexpected)" : "off, as the loader leaves it");
    kv_hex("CPACR_EL1 at entry", sb_boot.cpacr_in, 8);
    kv_hex("VBAR_EL1 at entry", sb_boot.vbar_in, 9);
    kv_hex("DAIF at entry", sb_boot.daif_in, 3);
    kv_hdr("RAW SLOTS 0-31, 112-127");
    for (unsigned s = 0; s < 128; s += 2) {
        if (s == 32)
            s = 112;
        ub_init(&u);
        if (s < 100)
            ub_c(&u, '0');
        if (s < 10)
            ub_c(&u, '0');
        ub_dec(&u, s);
        ub_s(&u, ": ");
        for (unsigned k = 0; k < 2; ++k) {
            ub_hexn(&u, l[s + k], 16);
            ub_c(&u, ' ');
        }
        kv_mono(u.b);
    }
}

void logs_loader(void)
{
    info_screen("Loader handoff", fill_loader, 0);
}

/* ---- catalog, settings ------------------------------------------------------------------------- */

static const char *cat_name(unsigned c)
{
    switch (c) {
    case CAT_APPLE: return "apple";
    case CAT_MICROSOFT: return "microsoft";
    case CAT_ANDROID: return "android";
    case CAT_LINUX: return "linux";
    case CAT_STARBITE: return "starbite";
    case CAT_LAB: return "lab";
    default: return "other";
    }
}

static const char *method_s(unsigned m)
{
    return m == M_XNU ? "xnu" : m == M_LINUX ? "linux" : m == M_ANDROID ? "android" : m == M_UEFI ? "uefi" :
           m == M_BARE ? "bare" : "?";
}

static const char *tag_s(unsigned t)
{
    return t == TAG_DAILY ? "daily" : t == TAG_BETA ? "beta" : t == TAG_EXPERIMENTAL ? "experimental" :
           t == TAG_LAB ? "lab" : t == TAG_STABLE ? "stable" : "-";
}

static void fill_catalog(void)
{
    struct ub u;
    kv_hdr("SOURCE");
    kv_add("Catalog", cat_builtin() ? "built in (no catalog in the store)" : "OS store entry \"catalog\"");
    kv_dec("Entries", cat_count(), 0);
    for (unsigned i = 0; i < cat_count(); ++i) {
        const struct cat_ent *c = cat_get(i);
        const char *why;
        int ok = star_entry_ready(i, &why);
        kv_hdr(c->name);
        ub_init(&u);
        ub_s(&u, c->key);
        ub_s(&u, "  " U_DOT "  ");
        ub_s(&u, cat_name(c->category));
        ub_s(&u, ", ");
        ub_s(&u, method_s(c->method));
        ub_s(&u, ", ");
        ub_s(&u, tag_s(c->tag));
        if (c->flags & CF_HIDDEN)
            ub_s(&u, ", hidden");
        kv_add("Key", u.b);
        kv_addc("State", ok ? "ready" : why, ok ? C_GREEN : C_RED);
        for (unsigned f = 0; f < 4; ++f)
            if (c->file[f][0])
                kv_add(f == 0 ? "Files" : "", c->file[f]);
        if (c->cmdline[0])
            kv_add("Command line", c->cmdline);
    }
    kv_note("Editing the catalog (order, hidden, default, command lines) is a store write: EN4.");
}

void sb_pages_catalog(void)
{
    info_screen("Catalog", fill_catalog, 0);
}

static void fill_settings(void)
{
    struct star_settings st;
    char key[24];
    int ok = settings_load(&st);
    kv_hdr("SETTINGS RECORD (UFS STORE BLOCK 2)");
    kv_addc("Record", ok ? "valid (SBSETTNG, CRC ok)" : "none or invalid: the loader uses defaults",
            ok ? C_GREEN : C_ORANGE);
    kv_add("Default entry", ok && st.default_key[0] ? st.default_key : "-");
    kv_dec("Timeout", ok ? st.timeout_s : 0, " s (0 = wait in the menu)");
    kv_add("Last choice", ok && st.last_key[0] ? st.last_key : "-");
    kv_add("Next (one shot)", ok && st.next_key[0] ? st.next_key : "-");
    kv_add("Card store at boot", ok && (st.flags & SET_SD_STORE) ? "on" : "off");
    kv_dec("Writes (seq)", ok ? st.seq : 0, 0);
    kv_hdr("BOOT-NEXT MAILBOX 0xFED17FE0 (RAM)");
    kv_add("Now", sb_boot_next_peek(key) ? key : "empty");
    kv_note("Engineer OS returns to the loader through this mailbox (Start an OS, Engineer OS again); "
            "the loader clears it when it reads it.");
}

void sb_pages_settings(void)
{
    info_screen("Settings record", fill_settings, 0);
}

/* ---- GPT viewer --------------------------------------------------------------------------------- */

static void kv_gpt_head(const char *name, const struct gpt_head *h)
{
    struct ub u;
    kv_hdr(name);
    kv_dec("This header at", h->my_lba, 0);
    kv_dec("Other header at", h->alt_lba, 0);
    ub_init(&u);
    ub_dec(&u, h->first_usable);
    ub_s(&u, " .. ");
    ub_dec(&u, h->last_usable);
    kv_add("Usable blocks", u.b);
    kv_dec("Entries at", h->entries_lba, 0);
    ub_init(&u);
    ub_dec(&u, h->num);
    ub_s(&u, " x ");
    ub_dec(&u, h->esz);
    ub_s(&u, " bytes");
    kv_add("Entry table", u.b);
    ub_init(&u);
    ub_hexn(&u, h->hdr_crc, 8);
    ub_s(&u, h->hdr_crc == h->hdr_crc_calc ? "  matches" : "  MISMATCH (computed ");
    if (h->hdr_crc != h->hdr_crc_calc) {
        ub_hexn(&u, h->hdr_crc_calc, 8);
        ub_c(&u, ')');
    }
    kv_addc("Header CRC32", u.b, h->hdr_crc == h->hdr_crc_calc ? C_GREEN : C_RED);
    ub_init(&u);
    ub_hexn(&u, h->ent_crc, 8);
    ub_s(&u, h->ent_crc == h->ent_crc_calc ? "  matches" : "  MISMATCH (computed ");
    if (h->ent_crc != h->ent_crc_calc) {
        ub_hexn(&u, h->ent_crc_calc, 8);
        ub_c(&u, ')');
    }
    kv_addc("Entries CRC32", u.b, h->ent_crc == h->ent_crc_calc ? C_GREEN : C_RED);
    ub_init(&u);
    for (unsigned i = 0; i < 16; ++i) {
        ub_hexn(&u, h->guid[i], 2);
        if (i == 3 || i == 5 || i == 7 || i == 9)
            ub_c(&u, '-');
    }
    kv_add("Disk GUID (raw)", u.b);
}

static void fill_gpt(void)
{
    struct gpt_head p, b;
    if (!ufs_up()) {
        kv_addc("UFS", "not ready", C_RED);
        return;
    }
    if (!gpt_header(1, &p)) {
        kv_addc("Primary GPT", "not found at block 1", C_RED);
        return;
    }
    kv_gpt_head("PRIMARY HEADER (LU0 BLOCK 1)", &p);
    if (p.alt_lba && gpt_header(p.alt_lba, &b))
        kv_gpt_head("BACKUP HEADER", &b);
    else
        kv_addc("Backup header", "not readable at the primary's alternate LBA", C_ORANGE);
    kv_hdr("PARTITIONS (NAME, FIRST .. LAST, SIZE, FENCE)");
    if (gpt_load() > 0)
        for (int i = 0; i < gpt_n; ++i) {
            struct ub k, v;
            struct sb_wdecide d;
            int r = sb_write_decide(SB_MED_UFS, 0, gpt_first[i], 1, &d);
            ub_init(&k);
            ub_dec(&k, (uint64_t)i + 1U);
            ub_s(&k, "  ");
            ub_s(&k, gpt_name[i]);
            ub_init(&v);
            ub_dec(&v, gpt_first[i]);
            ub_s(&v, "..");
            ub_dec(&v, gpt_last[i]);
            ub_s(&v, "  ");
            ub_size(&v, (gpt_last[i] - gpt_first[i] + 1) * 4096U);
            ub_s(&v, r ? "  refused" : d.tier == 3 ? "  tier 3" : d.tier == 2 ? "  tier 2" : "  tier 1");
            kv_addc(k.b, v.b, r || d.tier == 3 ? C_RED : d.tier == 2 ? C_ORANGE : C_L1);
        }
    kv_note("Writes: Storage > Partitions and LUs (the GPT head and tail are tier 3, with a backup first). "
            "The PIT cross-check (device/pit.txt) and a GPT editor come later.");
}

void sb_pages_gpt(void)
{
    info_screen("GPT viewer", fill_gpt, 0);
}

/* ---- write fence: what each kind of target needs ------------------------------------------- */

static void fence_row(const char *label, unsigned medium, unsigned lu, uint64_t lba, uint64_t n)
{
    struct sb_wdecide d;
    struct ub v;
    int r = sb_write_decide(medium, lu, lba, n, &d);
    ub_init(&v);
    ub_s(&v, d.part[0] ? d.part : "?");
    ub_s(&v, ": ");
    ub_s(&v, r ? "refused" : tier_name(d.tier));
    ub_s(&v, " (");
    ub_s(&v, d.why);
    ub_c(&v, ')');
    kv_addc(label, v.b, r ? C_L3 : d.tier >= 3 ? C_RED : d.tier == 2 ? C_ORANGE : C_GREEN);
}

static void fill_fence(void)
{
    uint64_t f, l;
    kv_hdr("EVERY BLOCK WRITE GOES THROUGH SB_WRITE");
    kv_add("Checks", "GPT name and range, the tier, the open confirmed authorisation (range, tier, steps)");
    kv_add("Tier 1", "warning screen + slide");
    kv_add("Tier 2", "+ the target's name typed");
    kv_add("Tier 3", "+ hold Power 3 s, and a verified backup into the OS store first");
    kv_add("Tier 4", "red warning, \"<target> I ACCEPT\" typed, slide, hold Power 5 s; backup unless waived");
    kv_addc("Dry run", sb_fence_is_dry_run() ? "ON: writes are logged, not made" : "off (Tools > Armed mode)",
            sb_fence_is_dry_run() ? C_ORANGE : C_L2);
    kv_hdr("SAMPLE TARGETS (LIVE GPT)");
    fence_row("Block 0 (GPT/PIT head)", SB_MED_UFS, 0, 0, 1);
    if (gpt_find("EFS", &f, &l))
        fence_row("EFS, first block", SB_MED_UFS, 0, f, 1);
    if (gpt_find("PARAM", &f, &l))
        fence_row("PARAM, first block", SB_MED_UFS, 0, f, 1);
    if (gpt_find("BOOT", &f, &l)) {
        fence_row("BOOT, first block", SB_MED_UFS, 0, f, 1);
        fence_row("BOOT, across its end", SB_MED_UFS, 0, l, 2);
    }
    if (gpt_find("SYSTEM", &f, &l))
        fence_row("SYSTEM, first block", SB_MED_UFS, 0, f, 1);
    if (gpt_find("USERDATA", &f, &l)) {
        fence_row("Starbite store header", SB_MED_UFS, 0, f + 4194304ULL + 1U, 1);
        fence_row("USERDATA, first block", SB_MED_UFS, 0, f, 1);
    }
    fence_row("Boot LU 1, block 0", SB_MED_UFS, 1, 0, 1);
    fence_row("microSD, block 0", SB_MED_SD, 0, 0, 1);
    kv_note("Without an open authorisation every write is refused before the storage driver is called; the "
            "UFS and SD drivers refuse a write that does not come from sb_write.");
}

static void build_fence(void)
{
    fill_fence();
    kv_hdr("TRY");
    row_add("Write the settings block with no confirmation", "refused by the gate, logged", 0);
    row_add("Write EFS with no confirmation", "refused by the gate, logged", 0);
}

static int act_fence(int i)
{
    uint64_t f, l;
    static uint8_t zero[4096];
    sb_gate_close();
    if (i == 0 && gpt_find("USERDATA", &f, &l))
        (void)sb_write(SB_MED_UFS, 0, f + 4194304ULL + 2U, 1, zero);
    if (i == 1 && gpt_find("EFS", &f, &l))
        (void)sb_write(SB_MED_UFS, 0, f, 1, zero);
    ui_toast("Refused and logged (Logs > Engineer OS log)", C_ORANGE, 1000);
    return 0;
}

void sb_pages_fence(void)
{
    list_screen("Write fence", build_fence, act_fence);
}

/* ---- coming, about ------------------------------------------------------------------------- */

static const char *coming_what;

static void fill_coming(void)
{
    kv_bigc("not in Engineer OS yet", "Coming", C_L2);
    kv_hdr("PLANNED");
    if (sb_streq(coming_what, "Radio")) {
        kv_add("What", "modem bench: boot stages, SIM, signal, cells");
        kv_add("Never", "calls, emergency numbers, IMEI, NV/RF data");
        kv_add("Milestone", "later (shared with the Emergency OS)");
    } else {
        kv_add("What", "make an installer drive, flash another S9");
        kv_add("How", "boot-next to the hidden installer entry");
        kv_add("Milestone", "EN8");
    }
    kv_note("docs/STARBITE-MASTER-PLAN.md section 4.5 has the plan.");
}

void sb_pages_coming(const char *what)
{
    coming_what = what;
    info_screen(what, fill_coming, 0);
}

static void fill_about(void)
{
    struct ub u;
    kv_big("Starbite Engineer OS", ENG_VERSION);
    kv_hdr("ABOUT");
    kv_add("Build", ENG_BUILD);
    kv_add("Platform", "Galaxy S9 SM-G960F, Exynos 9810");
    kv_add("Runs as", "a bare image on libsb (EL1, MMU and caches on)");
    kv_add("Started by", "the loader's catalog entry \"engineer\"");
    kv_add("Engineer Lite", "the loader's EngineerMode: hold Bixby in the menu");
    kv_add("Style", "Starbite menu v7 palette and type, flat cards");
    kv_hdr("FRAMES");
    kv_dec("Presented", ui_stats.frames, 0);
    ub_init(&u);
    ub_size(&u, ui_stats.frames ? ui_stats.px_total * 4U / ui_stats.frames : 0);
    ub_s(&u, " per frame (dirty rectangles)");
    kv_add("Average copy", u.b);
    ub_init(&u);
    ub_dec(&u, ui_stats.frames ? ui_stats.us_total / ui_stats.frames : 0);
    ub_s(&u, " " U_MICRO "s average, ");
    ub_dec(&u, ui_stats.us_worst);
    ub_s(&u, " worst");
    kv_add("Copy time", u.b);
    kv_hdr("KEYS");
    kv_add("Volume", "move (hold: repeat)");
    kv_add("Power", "select (on release)");
    kv_add("Hold Power", "back (0.6 s); at a tab's root: the Dashboard");
    kv_add("Bixby", "back; hold: the tab's root");
    kv_add("Touch", "tap, drag, fling; swipe right: back");
    kv_add("Other tabs", "Dashboard > TABS (keys), or the tab bar (touch)");
    kv_add("Leave", "Dashboard or Tools > Exit to the boot menu");
    kv_hdr("CREDITS");
    kv_add("Fonts", "Inter, Inter Tight, JetBrains Mono (OFL)");
    kv_add("Icons", "menu v7 line icons");
    kv_add("Sponsor", "none");
}

void sb_pages_about(void)
{
    info_screen("About Engineer OS", fill_about, 0);
}
