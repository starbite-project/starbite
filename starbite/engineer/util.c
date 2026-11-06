/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Lab utilities (Engineer OS v2): register peek / poke, memory dump / fill, partitions
 * (hexdump, edit, backup, restore, erase; UFS LU0, the boot LUs, the microSD card), the OS store
 * (inspect, verify, copy, rename, remove), the settings editor, PMIC / MAX77705 writes, reboot to
 * any target. Every write goes through lab_confirm (armed.c) at the tier policy.c or the write
 * fence gives it; the free-form ones need armed mode. Keys only reach everything: Volume moves or
 * changes a digit, Power selects, hold Power goes back.
 */
#include "eng.h"

/* ---- hex entry ------------------------------------------------------------------------------- */

int ask_hex(const char *title, const char *hint, uint64_t *v, unsigned digits)
{
    uint8_t d[16];
    if (digits < 1)
        digits = 1;
    if (digits > 16)
        digits = 16;
    for (unsigned i = 0; i < digits; ++i)
        d[i] = (uint8_t)((*v >> (4U * (digits - 1U - i))) & 0xfU);
    int cur = 0, res = -1;
    const int dw = digits > 10 ? 80 : 110, x0 = (UI_W - (int)digits * dw) / 2, y0 = UI_VIEW_Y0 + 520, dh = 220;
    const int by = y0 + dh + 160;
    ui_save_screen();
    ui_input_reset();
    while (res < 0) {
        ui_noclip();
        ui_fill(0, UI_VIEW_Y0, UI_W, UI_VIEW_Y1 - UI_VIEW_Y0, C_BG);
        ui_text(F_TITLE_S, 64, UI_VIEW_Y0 + 140, title, C_L1);
        ui_text_cut(F_ROW, 64, UI_VIEW_Y0 + 240, hint ? hint : "", UI_W - 128, C_L2);
        ui_text_cut(F_SMALL, 64, UI_VIEW_Y0 + 330, "Volume: change the digit. Power: next digit, then OK. Hold Power: cancel.",
                    UI_W - 128, C_L3);
        for (unsigned i = 0; i < digits; ++i) {
            int x = x0 + (int)i * dw;
            ui_round(x + 6, y0, dw - 12, dh, 24, (int)i == cur ? C_STAR : C_CARD2);
            char s[2] = { "0123456789ABCDEF"[d[i]], 0 };
            ui_text_c(F_BIG, x + dw / 2, y0 + dh / 2 + 36, s, (int)i == cur ? C_BG : C_L1);
            if ((int)i == cur) {
                ui_text_c(F_SEG, x + dw / 2, y0 - 24, U_TRI_UP, C_STAR);
                ui_text_c(F_SEG, x + dw / 2, y0 + dh + 60, U_TRI_DOWN, C_STAR);
            }
        }
        ui_round(UI_W / 2 - 520, by, 480, 170, 40, cur == (int)digits ? C_STAR : C_CARD2);
        ui_text_c(F_BTN, UI_W / 2 - 280, by + 106, "OK", cur == (int)digits ? C_BG : C_STAR);
        ui_round(UI_W / 2 + 40, by, 480, 170, 40, C_CARD2);
        ui_text_c(F_BTN, UI_W / 2 + 280, by + 106, "Cancel", C_L2);
        ui_present();
        struct ui_ev e;
        ui_poll(&e, 100);
        if (e.type == EV_UP && cur < (int)digits)
            d[cur] = (uint8_t)((d[cur] + 1U) & 0xfU);
        else if (e.type == EV_DOWN && cur < (int)digits)
            d[cur] = (uint8_t)((d[cur] + 15U) & 0xfU);
        else if (e.type == EV_SELECT)
            cur < (int)digits ? (void)++cur : (void)(res = 1);
        else if (e.type == EV_BACK || e.type == EV_HOME)
            res = 0;
        else if (e.type == EV_TAP) {
            if (e.y >= y0 && e.y < y0 + dh && e.x >= x0 && e.x < x0 + (int)digits * dw) {
                cur = (e.x - x0) / dw;
                d[cur] = (uint8_t)((d[cur] + (e.y < y0 + dh / 2 ? 1U : 15U)) & 0xfU);
            } else if (e.y >= by && e.y < by + 170) {
                res = e.x < UI_W / 2 ? 1 : 0;
            }
        }
    }
    ui_restore_screen();
    ui_present();
    ui_input_reset();
    if (res) {
        uint64_t x = 0;
        for (unsigned i = 0; i < digits; ++i)
            x = x << 4 | d[i];
        *v = x;
    }
    return res;
}

static const char *const *pick_l;
static int pick_n, pick_cur, pick_res;

static void build_pick(void)
{
    for (int i = 0; i < pick_n; ++i)
        row_add(pick_l[i], i == pick_cur ? "current" : "", i == pick_cur ? R_CHECK | R_ON : 0);
}

static int act_pick(int i)
{
    pick_res = i;
    return 1;
}

int ask_pick(const char *title, const char *const *labels, int n, int cur)
{
    pick_l = labels;
    pick_n = n;
    pick_cur = cur;
    pick_res = -1;
    list_screen(title, build_pick, act_pick);
    return pick_res;
}

/* ---- register peek / poke and memory dump ------------------------------------------------------ */

static uint64_t pk_addr = 0x14060000ULL;
static uint64_t pk_ok_block = ~0ULL;        /* the 64 KiB block the user allowed to be read */
static int pk_mem;                           /* 0 registers (words), 1 memory (bytes + ASCII) */

static int pk_can_read(uint64_t a)
{
    int t = read_tier(a, 0);
    return t == TIER0 || (t == TIER1 && (a & ~0xffffULL) == pk_ok_block);
}

static void build_peek(void)
{
    struct ub u;
    const char *why = "", *rwhy = "";
    int t = poke_tier(pk_addr, &why), rt = read_tier(pk_addr, &rwhy);
    kv_hdr(pk_mem ? "MEMORY" : "REGISTERS");
    ub_init(&u);
    ub_hex(&u, pk_addr, 9);
    row_value("Address", u.b, 0);
    row_add("Next 256 bytes", "", 0);
    row_add("Previous 256 bytes", "", 0);
    if (rt == TIER1 && !pk_can_read(pk_addr))
        row_add("Read this block anyway", rwhy, R_WARN);
    else
        row_add(pk_mem ? "Fill memory" : "Write a word", t >= TIER_NO ? why : tier_name(t),
                t >= TIER_NO ? R_DIS : t >= TIER3 ? R_RED : R_WARN);
    char what[64];
    int ua = lab_undo_available(what, sizeof(what));
    row_add("Undo the last write", ua ? what : "nothing to undo", ua ? R_WARN : R_DIS);
    ub_init(&u);
    ub_s(&u, tier_name(t));
    ub_s(&u, ": ");
    ub_s(&u, why);
    kv_addc("A write here", u.b, t >= TIER_NO ? C_L3 : t >= TIER3 ? C_RED : t == TIER2 ? C_ORANGE : C_L2);
    kv_hdr("CONTENTS (READ WHEN THE PAGE IS DRAWN)");
    uint64_t base = pk_addr & ~0xfULL;
    for (unsigned r = 0; r < 16; ++r) {
        uint64_t a = base + 16U * r;
        ub_init(&u);
        ub_hexn(&u, a, 9);
        ub_s(&u, ": ");
        uint32_t w[4];
        int ok[4];
        for (unsigned k = 0; k < 4; ++k) {
            ok[k] = pk_can_read(a + 4U * k) && sb_probe32(a + 4U * k, &w[k]);
            if (pk_mem) {
                for (unsigned b = 0; b < 4; ++b) {
                    if (ok[k])
                        ub_hexn(&u, (w[k] >> (8 * b)) & 0xffU, 2);
                    else
                        ub_s(&u, "--");
                }
                ub_c(&u, ' ');
            } else {
                if (ok[k])
                    ub_hexn(&u, w[k], 8);
                else
                    ub_s(&u, pk_can_read(a + 4U * k) ? "*FAULT**" : "--------");
                ub_c(&u, ' ');
            }
        }
        if (pk_mem)
            for (unsigned k = 0; k < 4; ++k)
                for (unsigned b = 0; b < 4; ++b) {
                    char c = ok[k] ? (char)((w[k] >> (8 * b)) & 0xffU) : '.';
                    ub_c(&u, c >= 32 && c < 127 ? c : '.');
                }
        kv_monoc(u.b, a == pk_addr ? C_STAR : C_L1);
    }
    if (rt == TIER_NO)
        kv_note(rwhy);
}

static int peek_write(void)
{
    struct sb_auth au;
    struct action a;
    const char *why;
    char tgt[24], oldv[24], newv[24], hap[96];
    struct ub u;
    uint64_t addr = pk_addr & ~3ULL, v = 0;
    uint32_t cur = 0;
    if (!ask_hex("Write a word", "Address (aligned to 4)", &addr, 9))
        return 0;
    addr &= ~3ULL;
    int readable = pk_can_read(addr) && sb_probe32(addr, &cur);
    v = cur;
    if (!ask_hex("Write a word", "New 32-bit value", &v, 8))
        return 0;
    ub_init(&u);
    ub_hexn(&u, addr, 8);
    sb_strcpy(tgt, sizeof(tgt), u.b);
    ub_init(&u);
    if (readable)
        ub_hex(&u, cur, 8);
    else
        ub_s(&u, "not read");
    sb_strcpy(oldv, sizeof(oldv), u.b);
    ub_init(&u);
    ub_hex(&u, v & 0xffffffffU, 8);
    sb_strcpy(newv, sizeof(newv), u.b);
    a.tier = poke_tier(addr, &why);
    ub_init(&u);
    ub_s(&u, "A 32-bit store to ");
    ub_hex(&u, addr, 9);
    ub_s(&u, ": ");
    ub_s(&u, why);
    sb_strcpy(hap, sizeof(hap), u.b);
    a.title = "Register write";
    a.target = tgt;
    a.happens[0] = hap;
    a.happens[1] = "Read back afterwards; a fault or a pending SError is reported";
    a.happens[2] = 0;
    a.undo = poke_undo(addr);
    a.old_v = oldv;
    a.new_v = newv;
    a.backup = "the old value (Undo the last write)";
    a.space = SB_SP_MEM;
    a.lu = 0;
    a.kind = SB_KIND_FREEFORM;
    a.first = addr;
    a.count = 4;
    if (!lab_confirm(&a, &au))
        return 0;
    int ok = lab_poke32(addr, (uint32_t)v, &au);
    ui_toast(ok ? "Written (see the audit log)" : "Write failed or faulted", ok ? C_GREEN : C_RED, 1100);
    pk_addr = addr;
    return ok;
}

static void mem_fill(void)
{
    struct sb_auth au;
    struct action a;
    const char *why;
    char tgt[24], rng[48], pat[24];
    struct ub u;
    uint64_t addr = pk_addr & ~7ULL, len = 0x1000, p = 0;
    if (!ask_hex("Fill memory", "Start address (8-byte aligned)", &addr, 9) ||
        !ask_hex("Fill memory", "Length in bytes (a multiple of 8)", &len, 8) ||
        !ask_hex("Fill memory", "64-bit pattern", &p, 16))
        return;
    addr &= ~7ULL;
    len &= ~7ULL;
    a.tier = range_tier(addr, len, &why);
    ub_init(&u);
    ub_hexn(&u, addr, 8);
    sb_strcpy(tgt, sizeof(tgt), u.b);
    ub_init(&u);
    ub_hex(&u, addr, 9);
    ub_s(&u, " + ");
    ub_size(&u, len);
    sb_strcpy(rng, sizeof(rng), u.b);
    ub_init(&u);
    ub_hex(&u, p, 16);
    sb_strcpy(pat, sizeof(pat), u.b);
    a.title = "Memory fill";
    a.target = tgt;
    a.happens[0] = rng;
    a.happens[1] = why;
    a.happens[2] = 0;
    a.undo = a.tier >= TIER_NO ? "-" : "a reboot (nothing in that memory survives one anyway)";
    a.old_v = "";
    a.new_v = pat;
    a.backup = "";
    a.space = SB_SP_MEM;
    a.lu = 0;
    a.kind = SB_KIND_FREEFORM;
    a.first = addr;
    a.count = len;
    if (!lab_confirm(&a, &au) || !sb_gate_open(&au))
        return;
    const char *w2;
    int r = sb_gate_check(SB_SP_MEM, 0, addr, len, a.tier, &w2);
    uint64_t done = 0;
    if (!r) {
        lab_stop_reset();
        for (; done < len; done += 8) {
            *(volatile uint64_t *)(addr + done) = p;
            if (!(done & 0xfffffU) && lab_stop_check())
                break;
        }
        __asm__ volatile("dsb sy" ::: "memory");
    }
    sb_gate_close();
    ub_init(&u);
    ub_s(&u, r ? "fill refused: " : "filled ");
    if (r)
        ub_s(&u, w2);
    else {
        ub_size(&u, done);
        ub_s(&u, " at ");
        ub_hex(&u, addr, 9);
    }
    audit_line(u.b);
    ui_toast(u.b, r ? C_RED : C_GREEN, 1200);
}

static int act_peek(int i)
{
    uint64_t v = pk_addr;
    switch (i) {
    case 0:
        if (ask_hex(pk_mem ? "Memory" : "Register peek", "Address", &v, 9))
            pk_addr = v & ~3ULL;
        break;
    case 1: pk_addr += 0x100U; break;
    case 2: pk_addr -= 0x100U; break;
    case 3: {
        const char *rwhy;
        if (read_tier(pk_addr, &rwhy) == TIER1 && !pk_can_read(pk_addr)) {
            if (dialog("Read anyway?", rwhy, "Reading may clear a status bit or stall the bus.", "READ"))
                pk_ok_block = pk_addr & ~0xffffULL;
        } else if (pk_mem) {
            mem_fill();
        } else {
            (void)peek_write();
        }
        break;
    }
    case 4: lab_undo(); break;
    default: break;
    }
    return 0;
}

void util_peek(void)
{
    pk_mem = 0;
    if (pk_addr >= 0x80000000ULL)
        pk_addr = 0x14060000ULL;
    list_screen("Register peek / poke", build_peek, act_peek);
}

void util_memory(void)
{
    pk_mem = 1;
    if (pk_addr < 0x80000000ULL)
        pk_addr = SB_TEST_BASE;
    list_screen("Memory dump / fill", build_peek, act_peek);
}

/* ---- the sector viewer and editor (any medium, any LU) ---------------------------------------- */

static unsigned sv_med, sv_lu;
static uint64_t sv_lba;
static int sv_err;
static uint8_t sv_blk[4096] __attribute__((aligned(64)));

static void sv_load(void)
{
    sv_err = sb_blk_read(sv_med, sv_lu, sv_lba, 1, sv_blk);
}

static const char *sv_where(void)
{
    static char w[48];
    struct sb_wdecide d;
    int r = sb_write_decide(sv_med, sv_lu, sv_lba, 1, &d);
    struct ub u;
    ub_init(&u);
    ub_s(&u, sv_med == SB_MED_SD ? "card" : sv_lu ? "boot LU" : "LU0");
    ub_s(&u, " " U_DOT " ");
    ub_s(&u, d.part[0] ? d.part : "-");
    ub_s(&u, " " U_DOT " write: ");
    ub_s(&u, r ? "refused" : tier_name(d.tier));
    sb_strcpy(w, sizeof(w), u.b);
    return w;
}

static void fill_sv(void)
{
    struct ub u;
    ub_init(&u);
    ub_dec(&u, sv_lba);
    kv_bigc(sv_where(), u.b, sv_err ? C_RED : C_L1);
    if (sv_err) {
        kv_addc("Read", "failed (protected range, no card, or past the end)", C_RED);
        return;
    }
    for (unsigned o = 0; o < 4096; o += 16) {
        ub_init(&u);
        ub_hexn(&u, o, 3);
        ub_c(&u, ' ');
        for (unsigned i = 0; i < 16; ++i) {
            ub_hexn(&u, sv_blk[o + i], 2);
            if ((i & 3) == 3)
                ub_c(&u, ' ');
        }
        for (unsigned i = 0; i < 16; ++i) {
            uint8_t c = sv_blk[o + i];
            ub_c(&u, c >= 32 && c < 127 ? (char)c : '.');
        }
        kv_mono(u.b);
    }
    kv_note("Volume scrolls. Power: the actions (next, previous, jump, edit). Hold Power: back.");
}

static void sv_edit(void)
{
    struct sb_wdecide d;
    struct sb_auth au;
    struct action a;
    char tgt[24], hap[96], oldv[40], newv[40], where[64];
    struct ub u;
    uint64_t off = 0, val = 0;
    int r = sb_write_decide(sv_med, sv_lu, sv_lba, 1, &d);
    if (r) {
        (void)dialog_danger("Not writable", d.why, 0, "OK");
        return;
    }
    if (sv_err || !ask_hex("Edit the block", "Byte offset in the block (0..FFC, 4 bytes are written)", &off, 3))
        return;
    off &= 0xffcU;
    uint32_t cur = (uint32_t)sv_blk[off] | (uint32_t)sv_blk[off + 1] << 8 | (uint32_t)sv_blk[off + 2] << 16 |
                   (uint32_t)sv_blk[off + 3] << 24;
    val = cur;
    if (!ask_hex("Edit the block", "New 32-bit little-endian value at that offset", &val, 8))
        return;
    sb_strcpy(tgt, sizeof(tgt), d.part[0] ? d.part : "BLOCK");
    for (char *p = tgt; *p; ++p)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_'))
            *p = '_';
    ub_init(&u);
    ub_s(&u, "Rewrites block ");
    ub_dec(&u, sv_lba);
    ub_s(&u, " of ");
    ub_s(&u, d.part);
    ub_s(&u, " with 4 bytes changed at ");
    ub_hex(&u, off, 3);
    sb_strcpy(hap, sizeof(hap), u.b);
    ub_init(&u);
    ub_hex(&u, cur, 8);
    sb_strcpy(oldv, sizeof(oldv), u.b);
    ub_init(&u);
    ub_hex(&u, val & 0xffffffffU, 8);
    sb_strcpy(newv, sizeof(newv), u.b);
    a.title = "Edit a block";
    a.target = tgt;
    a.happens[0] = hap;
    a.happens[1] = d.why;
    a.happens[2] = "Read back and compared afterwards";
    a.happens[3] = 0;
    a.undo = d.undo;
    a.old_v = oldv;
    a.new_v = newv;
    a.backup = d.tier >= TIER3 ? "this block, into the OS store first (verified)" : "";
    a.tier = d.tier;
    a.space = sv_med;
    a.lu = sv_lu;
    a.kind = SB_KIND_STORAGE | SB_KIND_FREEFORM;
    a.first = sv_lba;
    a.count = 1;
    if (!lab_confirm(&a, &au))
        return;
    if (d.tier >= TIER3 && lab_backup(sv_med, sv_lu, sv_lba, 1, tgt, &au, where))
        return;
    static uint8_t nb[4096] __attribute__((aligned(64)));
    memcpy(nb, sv_blk, sizeof(nb));
    nb[off] = (uint8_t)val;
    nb[off + 1] = (uint8_t)(val >> 8);
    nb[off + 2] = (uint8_t)(val >> 16);
    nb[off + 3] = (uint8_t)(val >> 24);
    if (!sb_gate_open(&au))
        return;
    r = sb_write(sv_med, sv_lu, sv_lba, 1, nb);
    sb_gate_close();
    sv_load();
    int same = !sv_err && !memcmp(sv_blk, nb, sizeof(nb));
    ui_toast(r == SB_W_DRYRUN ? "Dry run: logged, not written" : !r && same ? "Written and verified" : "Write FAILED",
             !r && same ? C_GREEN : r == SB_W_DRYRUN ? C_ORANGE : C_RED, 1200);
}

static const char *const sv_acts[] = { "Next block", "Previous block", "Forward 256 blocks", "Back 256 blocks",
                                       "Jump to a block number", "Edit bytes in this block", "Back to the hexdump" };

void util_sector(unsigned medium, unsigned lu, uint64_t lba)
{
    struct page p;
    sv_med = medium;
    sv_lu = lu;
    sv_lba = lba;
    sv_load();
    page_init(&p, UI_VIEW_Y0, UI_VIEW_Y1);
    screen_push("Sector viewer");
    chrome("Sector viewer", "", 0);
    eng_build_page(&p, fill_sv);
    ui_present();
    for (;;) {
        struct ui_ev e;
        ui_poll(&e, p.fling_v ? 8 : 40);
        if (E.redraw) {
            chrome("Sector viewer", "", 0);
            page_redraw(&p);
        }
        if (nav_event(&e))
            break;
        int moved = 0;
        if (page_scroll_event(&p, &e)) {
            ;
        } else if (e.type == EV_UP) {
            page_set_scroll(&p, p.scroll - 560);
        } else if (e.type == EV_DOWN) {
            page_set_scroll(&p, p.scroll + 560);
        } else if (e.type == EV_SELECT || e.type == EV_TAP) {
            int k = ask_pick("Sector actions", sv_acts, 7, -1);
            if (E.home || E.tab_req >= 0)
                break;
            uint64_t v = sv_lba;
            switch (k) {
            case 0: ++sv_lba; break;
            case 1: sv_lba = sv_lba ? sv_lba - 1 : 0; break;
            case 2: sv_lba += 256U; break;
            case 3: sv_lba = sv_lba > 256U ? sv_lba - 256U : 0; break;
            case 4:
                if (ask_hex("Jump", "Block number (hex)", &v, 9))
                    sv_lba = v;
                break;
            case 5: sv_edit(); break;
            default: break;
            }
            moved = 1;
        }
        page_fling_step(&p);
        if (moved) {
            sv_load();
            chrome("Sector viewer", "", 0);
            p.drawn = 0;
            p.scroll = 0;
            eng_build_page(&p, fill_sv);
        }
        ui_present();
    }
    screen_pop();
    page_free(&p);
}

/* ---- partitions --------------------------------------------------------------------------------- */

static int pt_idx;                          /* 0..gpt_n-1 the GPT; gpt_n.. boot LU 1..4; then the card */
static unsigned pt_med, pt_lu;
static uint64_t pt_first, pt_count;
static char pt_name[24];

static uint64_t lu_blocks(unsigned lu)
{
    uint8_t d[64];
    if (ufs_desc(0x02, (uint8_t)lu, d, sizeof(d)) < 0x13)
        return 0;
    uint64_t n = 0;
    for (int i = 0; i < 8; ++i)
        n = n << 8 | d[0x0b + i];
    return n;
}

static void pt_select(int i)
{
    pt_idx = i;
    if (i < gpt_n) {
        pt_med = SB_MED_UFS;
        pt_lu = 0;
        pt_first = gpt_first[i];
        pt_count = gpt_last[i] - gpt_first[i] + 1;
        sb_strcpy(pt_name, sizeof(pt_name), gpt_name[i]);
    } else if (i < gpt_n + 4) {
        pt_med = SB_MED_UFS;
        pt_lu = (unsigned)(i - gpt_n + 1);
        pt_first = 0;
        pt_count = lu_blocks(pt_lu);
        struct ub u;
        ub_init(&u);
        ub_s(&u, "BOOTLU");
        ub_dec(&u, pt_lu);
        sb_strcpy(pt_name, sizeof(pt_name), u.b);
    } else {
        pt_med = SB_MED_SD;
        pt_lu = 0;
        pt_first = 0;
        sd_allow();
        pt_count = sd_sectors_hint() / 8U;
        sb_strcpy(pt_name, sizeof(pt_name), "CARD");
    }
}

static void build_part(void)
{
    struct sb_wdecide d;
    struct ub u;
    int r = sb_write_decide(pt_med, pt_lu, pt_first, 1, &d);
    kv_hdr(pt_name);
    ub_init(&u);
    ub_dec(&u, pt_first);
    ub_s(&u, " .. ");
    ub_dec(&u, pt_first + pt_count - (pt_count ? 1 : 0));
    ub_s(&u, "  (");
    ub_size(&u, pt_count * 4096U);
    ub_c(&u, ')');
    kv_add("Blocks", pt_count ? u.b : "size unknown");
    kv_addc("Writes", r ? d.why : tier_name(d.tier), r ? C_L3 : d.tier >= TIER3 ? C_RED : d.tier == TIER2 ? C_ORANGE : C_L1);
    if (!r)
        kv_add("Why", d.why);
    kv_hdr("READ");
    row_add("Hexdump (and edit)", "the sector viewer at the first block", 0);
    row_add("Back up to the OS store", "bak-<name>-<n>, verified; UFS store, else the card's", pt_count ? R_WARN : R_DIS);
    row_add("Compare with a backup", "every block against a bak- entry", 0);
    kv_hdr("WRITE");
    row_add("Restore from a backup", r ? "not writable" : "a bak- entry of the same size", r ? R_DIS : R_RED);
    row_add("Erase (write zeros)", r ? "not writable" : "the whole range, after the confirmation", r ? R_DIS : R_RED);
}

static int pick_backup(struct store_ent *out, int *medium)
{
    static char labels[40][40];
    static const char *lp[40];
    static struct store_ent ents[40];
    int n = 0;
    struct ub pre;
    ub_init(&pre);
    ub_s(&pre, "bak-");
    ub_s(&pre, pt_name);
    ub_c(&pre, '-');
    for (unsigned m = 0; m < STORE_MEDIA; ++m) {
        struct store_ent e;
        for (unsigned i = 0; store_get(m, i, &e) && n < 40; ++i) {
            if (!sb_prefix(e.name, pre.b))
                continue;
            ents[n] = e;
            struct ub u;
            ub_init(&u);
            ub_s(&u, m ? "sd: " : "ufs: ");
            ub_s(&u, e.name);
            sb_strcpy(labels[n], sizeof(labels[0]), u.b);
            lp[n] = labels[n];
            ++n;
        }
    }
    if (!n) {
        (void)dialog("No backup", "No bak- entry for this partition in either store.", "Back it up first.", "OK");
        return 0;
    }
    int k = ask_pick("Pick a backup", lp, n, -1);
    if (k < 0)
        return 0;
    *out = ents[k];
    *medium = (int)ents[k].medium;
    return 1;
}

static void pt_compare(void)
{
    struct store_ent e;
    int m;
    if (!pick_backup(&e, &m))
        return;
    uint64_t n = (e.size + 4095U) / 4096U, diff = 0, first_diff = ~0ULL;
    if (n > pt_count)
        n = pt_count;
    lab_stop_reset();
    for (uint64_t i = 0; i < n;) {
        uint64_t k = n - i < 256 ? n - i : 256;
        if (sb_blk_read(pt_med, pt_lu, pt_first + i, k, (void *)SB_IO_BUF) ||
            sb_blk_read(e.medium, 0, e.lba + i, k, (void *)(SB_IO_BUF + (1U << 20)))) {
            diff = ~0ULL;
            break;
        }
        for (uint64_t b = 0; b < k; ++b)
            if (memcmp((const void *)(SB_IO_BUF + b * 4096U), (const void *)(SB_IO_BUF + (1U << 20) + b * 4096U), 4096)) {
                if (first_diff == ~0ULL)
                    first_diff = i + b;
                ++diff;
            }
        i += k;
        if (lab_stop_check())
            break;
    }
    struct ub u;
    ub_init(&u);
    if (diff == ~0ULL)
        ub_s(&u, "read error");
    else if (!diff)
        ub_s(&u, "identical");
    else {
        ub_dec(&u, diff);
        ub_s(&u, " blocks differ, first at +");
        ub_dec(&u, first_diff);
    }
    (void)dialog("Compare", e.name, u.b, "OK");
}

static void pt_restore(void)
{
    struct store_ent e;
    struct sb_wdecide d;
    struct sb_auth au;
    struct action a;
    char hap[96], where[64], sz[32];
    int m;
    if (sb_write_decide(pt_med, pt_lu, pt_first, pt_count, &d) || !pick_backup(&e, &m))
        return;
    uint64_t n = (e.size + 4095U) / 4096U;
    if (n > pt_count) {
        (void)dialog_danger("Too large", "The backup is larger than the partition.", 0, "OK");
        return;
    }
    struct ub u;
    ub_init(&u);
    ub_s(&u, "Writes ");
    ub_s(&u, e.name);
    ub_s(&u, " over ");
    ub_s(&u, pt_name);
    ub_s(&u, ", then reads it back");
    sb_strcpy(hap, sizeof(hap), u.b);
    ub_init(&u);
    ub_size(&u, n * 4096U);
    sb_strcpy(sz, sizeof(sz), u.b);
    a.title = "Restore a backup";
    a.target = pt_name;
    a.happens[0] = hap;
    a.happens[1] = d.why;
    a.happens[2] = 0;
    a.undo = d.tier >= TIER3 ? "restore the backup of today's contents taken first" : d.undo;
    a.old_v = "today's contents";
    a.new_v = sz;
    a.backup = d.tier >= TIER3 ? "today's contents, into the OS store first (verified)" : "";
    a.tier = d.tier;
    a.space = pt_med;
    a.lu = pt_lu;
    a.kind = SB_KIND_STORAGE;
    a.first = pt_first;
    a.count = n;
    if (!lab_confirm(&a, &au))
        return;
    if (d.tier >= TIER3 && lab_backup(pt_med, pt_lu, pt_first, n, pt_name, &au, where))
        return;
    int r = lab_restore(&e, pt_med, pt_lu, pt_first, &au);
    ui_toast(r == SB_W_DRYRUN ? "Dry run: logged, nothing written" : r ? "Restore FAILED (audit log)" : "Restored and verified",
             r ? C_RED : C_GREEN, 1400);
}

static void pt_erase(void)
{
    struct sb_wdecide d;
    struct sb_auth au;
    struct action a;
    char where[64], sz[32];
    if (sb_write_decide(pt_med, pt_lu, pt_first, pt_count, &d) || !pt_count)
        return;
    struct ub u;
    ub_init(&u);
    ub_size(&u, pt_count * 4096U);
    sb_strcpy(sz, sizeof(sz), u.b);
    a.title = "Erase a partition";
    a.target = pt_name;
    a.happens[0] = "Writes zeros over every block of the partition";
    a.happens[1] = d.why;
    a.happens[2] = 0;
    a.undo = d.undo;
    a.old_v = sz;
    a.new_v = "zeros";
    a.backup = d.tier >= TIER3 ? "the whole partition, into the OS store first (verified)" : "";
    a.tier = d.tier < TIER2 ? TIER2 : d.tier;           /* erasing a whole partition: at least tier 2 */
    a.space = pt_med;
    a.lu = pt_lu;
    a.kind = SB_KIND_STORAGE;
    a.first = pt_first;
    a.count = pt_count;
    if (!lab_confirm(&a, &au))
        return;
    if (a.tier >= TIER3 && lab_backup(pt_med, pt_lu, pt_first, pt_count, pt_name, &au, where))
        return;
    if (!sb_gate_open(&au))
        return;
    volatile uint64_t *z = (volatile uint64_t *)SB_IO_BUF;
    for (unsigned i = 0; i < (1U << 20) / 8U; ++i)
        z[i] = 0;
    int r = 0;
    lab_stop_reset();
    for (uint64_t i = 0; i < pt_count && !r;) {
        uint64_t k = pt_count - i < 256 ? pt_count - i : 256;
        r = sb_write(pt_med, pt_lu, pt_first + i, k, (const void *)SB_IO_BUF);
        i += k;
        if (!r && i < pt_count && lab_stop_check())
            r = -20;
    }
    sb_gate_close();
    ub_init(&u);
    ub_s(&u, r ? "erase stopped / failed: " : "erased: ");
    ub_s(&u, pt_name);
    audit_line(u.b);
    ui_toast(u.b, r ? C_RED : C_GREEN, 1200);
}

static int act_part(int i)
{
    char where[64];
    switch (i) {
    case 0: util_sector(pt_med, pt_lu, pt_first); break;
    case 1: {
        struct sb_auth au;
        char hap[80];
        struct ub u;
        ub_init(&u);
        ub_s(&u, "Reads ");
        ub_s(&u, pt_name);
        ub_s(&u, " and appends it to the OS store as bak-");
        ub_s(&u, pt_name);
        ub_s(&u, "-<n>");
        sb_strcpy(hap, sizeof(hap), u.b);
        struct action a = { "Back up a partition", pt_name, { hap, "The source is only read; the store gains one entry",
                            "Verified: read back, CRC, and compared with the source again", 0 },
                            "Starbite > OS store > the entry > Remove (tier 2)", "", "", "", TIER1,
                            store_free(STORE_UFS) >= pt_count ? SB_MED_UFS : SB_MED_SD, 0, SB_KIND_STORE, 0, 0 };
        if (lab_confirm(&a, &au) && !lab_backup(pt_med, pt_lu, pt_first, pt_count, pt_name, 0, where))
            (void)dialog("Backup done", where, "Verified.", "OK");
        break;
    }
    case 2: pt_compare(); break;
    case 3: pt_restore(); break;
    case 4: pt_erase(); break;
    default: break;
    }
    return 0;
}

static void build_parts(void)
{
    struct ub u;
    (void)gpt_load();
    kv_hdr("LU0 (GPT)");
    for (int i = 0; i < gpt_n; ++i) {
        struct sb_wdecide d;
        int r = sb_write_decide(SB_MED_UFS, 0, gpt_first[i], 1, &d);
        ub_init(&u);
        ub_size(&u, (gpt_last[i] - gpt_first[i] + 1) * 4096U);
        ub_s(&u, "  " U_DOT "  ");
        ub_s(&u, r ? "?" : tier_name(d.tier));
        row_add(gpt_name[i], u.b, !r && d.tier >= TIER3 ? R_RED : !r && d.tier == TIER2 ? R_WARN : 0);
    }
    kv_hdr("BOOT LUs (S-BOOT, CM, ECT)");
    for (unsigned lu = 1; lu <= 4; ++lu) {
        ub_init(&u);
        ub_s(&u, "Boot LU ");
        ub_dec(&u, lu);
        row_add(u.b, "read; writes tier 3 with a backup first", R_RED);
    }
    kv_hdr("MICROSD");
    row_add("The whole card", "4 KiB blocks from 0; writes tier 1", 0);
}

static int act_parts(int i)
{
    pt_select(i);
    struct ub u;
    ub_init(&u);
    ub_s(&u, pt_name);
    list_screen("Partition", build_part, act_part);
    return 0;
}

void util_partitions(void)
{
    list_screen("Partitions and LUs", build_parts, act_parts);
}

/* ---- the OS store ------------------------------------------------------------------------------- */

static unsigned st_m;
static int st_i;
static struct store_ent st_e;
static char st_verify[64];

static const char *type_name(unsigned t)
{
    static const char *const n[] = { "data", "image", "dtb", "initrd", "text", "xnu", "uefi", "catalog" };
    return t < 8 ? n[t] : "?";
}

static void build_entry(void)
{
    struct ub u;
    kv_hdr(st_e.name);
    kv_add("Medium", st_m ? "microSD store" : "UFS store");
    kv_add("Type", type_name(st_e.type));
    ub_init(&u);
    ub_size(&u, st_e.size);
    ub_s(&u, " (");
    ub_dec(&u, st_e.size);
    ub_s(&u, " bytes)");
    kv_add("Size", u.b);
    kv_dec("First block", st_e.lba, 0);
    kv_hex("CRC32 (directory)", st_e.crc, 8);
    kv_add("Verify", st_verify[0] ? st_verify : "not checked");
    kv_hdr("ACTIONS");
    row_add("Verify the CRC", "read the whole entry", 0);
    row_add("Hexdump", "the sector viewer at its first block", 0);
    row_add(st_m ? "Copy to the UFS store" : "Copy to the card's store", "append there, verified", R_WARN);
    row_add("Rename", "rewrites the directory: tier 2", R_WARN);
    row_add("Remove", "rewrites the directory: tier 2 (the data stays until a repack)", R_RED);
}

static int store_dir_action(int rename)
{
    struct sb_auth au;
    uint64_t first = 0, count = 0;
    char hap[96], newname[32];
    if (!store_dir_range(st_m, &first, &count))
        return -1;
    newname[0] = 0;
    if (rename) {
        /* a new name: the old one with the next free number ("-r1".."-r9") */
        for (unsigned k = 1; k < 10; ++k) {
            struct ub u;
            struct store_ent x;
            ub_init(&u);
            ub_s(&u, st_e.name);
            u.n = u.n > 27 ? 27 : u.n;
            u.b[u.n] = 0;
            ub_s(&u, "-r");
            ub_dec(&u, k);
            sb_strcpy(newname, sizeof(newname), u.b);
            ub_init(&u);
            ub_s(&u, st_m ? "sd:" : "ufs:");
            ub_s(&u, newname);
            if (!store_find(u.b, &x))
                break;
        }
    }
    struct ub u;
    ub_init(&u);
    ub_s(&u, rename ? "Renames " : "Removes ");
    ub_s(&u, st_e.name);
    if (rename) {
        ub_s(&u, " to ");
        ub_s(&u, newname);
    }
    ub_s(&u, ": the directory, then the header (2 writes)");
    sb_strcpy(hap, sizeof(hap), u.b);
    char tgt[24];
    sb_strcpy(tgt, sizeof(tgt), rename ? "RENAME" : "REMOVE");
    struct action a = { rename ? "Rename a store entry" : "Remove a store entry", tgt,
                        { hap, "Power lost between the two writes: the loader falls back to the old v1 view",
                          rename ? 0 : "The data blocks stay; the space returns only with a repack on the Mac", 0 },
                        rename ? "rename it back" : "re-add the entry (sbstore2.py) from a copy", st_e.name,
                        rename ? newname : "(removed)", "", TIER2, st_m, 0, SB_KIND_STORAGE, first, count };
    if (!lab_confirm(&a, &au) || !sb_gate_open(&au))
        return -1;
    int r = rename ? store_rename(st_m, (unsigned)st_i, newname) : store_remove(st_m, (unsigned)st_i);
    sb_gate_close();
    ub_init(&u);
    ub_s(&u, r ? "store edit FAILED: " : rename ? "renamed: " : "removed: ");
    ub_s(&u, st_e.name);
    audit_line(u.b);
    ui_toast(u.b, r ? C_RED : C_GREEN, 1200);
    return r;
}

static int act_entry(int i)
{
    struct ub u;
    switch (i) {
    case 0: {
        uint32_t got = 0;
        int r = store_verify(st_m, (unsigned)st_i, &got);
        ub_init(&u);
        ub_s(&u, r == 0 ? "OK, CRC " : r == 1 ? "MISMATCH, read " : "read error");
        if (r >= 0)
            ub_hexn(&u, got, 8);
        sb_strcpy(st_verify, sizeof(st_verify), u.b);
        break;
    }
    case 1: util_sector(st_m == STORE_SD ? SB_MED_SD : SB_MED_UFS, 0, st_e.lba); break;
    case 2: {
        unsigned to = st_m ? STORE_UFS : STORE_SD;
        uint64_t blocks = (st_e.size + 4095U) / 4096U;
        struct sb_auth au;
        struct action a = { "Copy a store entry", st_e.name, { "Appends a copy of the entry to the other medium's store",
                            "Verified: read back and CRC", 0, 0 }, "remove the copy (tier 2)", "", "", "", TIER1, to, 0,
                            SB_KIND_STORE, 0, 0 };
        if (blocks > store_free(to)) {
            (void)dialog_danger("No room", "The other store has no room for it.", 0, "OK");
            break;
        }
        if (!lab_confirm(&a, &au) || !sb_gate_open(&au))
            break;
        struct store_writer w;
        int r = store_wr_begin(&w, to, st_e.name, st_e.type, st_e.size);
        lab_stop_reset();
        for (uint64_t b = 0; !r && b < blocks;) {
            if (b && lab_stop_check()) {
                r = -20;                    /* stopped: nothing committed, the store unchanged */
                break;
            }
            uint64_t n = blocks - b < 256 ? blocks - b : 256;
            uint64_t bytes = (b + n) * 4096U > st_e.size ? st_e.size - b * 4096U : n * 4096U;
            if (sb_blk_read(st_m == STORE_SD ? SB_MED_SD : SB_MED_UFS, 0, st_e.lba + b, n, (void *)SB_IO_BUF))
                r = SB_W_IO;
            else
                r = store_wr_chunk(&w, (const void *)SB_IO_BUF, bytes);
            b += n;
        }
        if (!r)
            r = store_wr_commit(&w, 0);
        sb_gate_close();
        ub_init(&u);
        ub_s(&u, r ? "copy FAILED: " : "copied: ");
        ub_s(&u, st_e.name);
        audit_line(u.b);
        ui_toast(u.b, r ? C_RED : C_GREEN, 1200);
        break;
    }
    case 3: if (!store_dir_action(1)) return 1; break;
    case 4: if (!store_dir_action(0)) return 1; break;
    default: break;
    }
    return 0;
}

static void build_store(void)
{
    struct ub u;
    for (unsigned m = 0; m < STORE_MEDIA; ++m) {
        int n = store_open(m);
        kv_hdr(m ? "MICROSD STORE" : "UFS STORE (USERDATA + 16 GiB)");
        if (n <= 0 && !store_version(m)) {
            kv_addc("Store", m ? "none open (Storage > microSD first)" : "not found", C_L3);
            continue;
        }
        ub_init(&u);
        ub_s(&u, "v");
        ub_dec(&u, (uint64_t)store_version(m));
        ub_s(&u, ", ");
        ub_dec(&u, (uint64_t)n);
        ub_s(&u, " entries, ");
        ub_size(&u, store_used(m) * 4096U);
        ub_s(&u, " used, ");
        ub_size(&u, store_free(m) * 4096U);
        ub_s(&u, " free for new entries");
        kv_add("Store", u.b);
        struct store_ent e;
        for (unsigned i = 0; store_get(m, i, &e); ++i) {
            ub_init(&u);
            ub_s(&u, type_name(e.type));
            ub_s(&u, "  " U_DOT "  ");
            ub_size(&u, e.size);
            row_add(e.name, u.b, sb_prefix(e.name, "bak-") ? R_WARN : 0);
        }
    }
    kv_note("Entries named bak-... are Engineer OS backups; lab-... are saved reports.");
}

static int act_store(int i)
{
    int k = 0;
    for (unsigned m = 0; m < STORE_MEDIA; ++m) {
        struct store_ent e;
        for (unsigned j = 0; store_get(m, j, &e); ++j, ++k)
            if (k == i) {
                st_m = m;
                st_i = (int)j;
                st_e = e;
                st_verify[0] = 0;
                list_screen("Store entry", build_entry, act_entry);
                return 0;
            }
    }
    return 0;
}

void util_store(void)
{
    list_screen("OS store", build_store, act_store);
}

/* ---- settings editor ---------------------------------------------------------------------------- */

static struct star_settings se_old, se_new;
static int se_valid, se_loaded;
static const unsigned se_timeouts[] = { 0, 3, 5, 10, 15, 30, 60 };
static const char *const se_post[] = { "full (default)", "quick", "off", "full (default)" };
static const char *const se_log[] = { "quiet (default)", "normal", "verbose", "debug" };

static void se_load(void)
{
    se_valid = settings_load(&se_old);
    se_new = se_old;
    se_loaded = 1;
}

static int cat_index(const char *key)
{
    return key[0] ? cat_find(key) : -1;
}

static void next_key(char *key)
{
    int i = cat_index(key), n = (int)cat_count();
    i = i + 1 >= n ? -1 : i + 1;
    if (i < 0)
        key[0] = 0;
    else
        sb_strcpy(key, 24, cat_get((unsigned)i)->key);
}

static void build_settings(void)
{
    struct ub u;
    if (!se_loaded)
        se_load();
    kv_hdr("SETTINGS RECORD (UFS STORE BLOCK 2)");
    kv_addc("Record", se_valid ? "valid" : "none: the loader uses defaults; saving creates one",
            se_valid ? C_GREEN : C_ORANGE);
    kv_hdr("EDIT (POWER CHANGES)");
    row_value("Default OS", se_new.default_key[0] ? se_new.default_key : "none", 0);
    ub_init(&u);
    ub_dec(&u, se_new.timeout_s);
    ub_s(&u, se_new.timeout_s ? " s, then the default" : " (wait in the menu)");
    row_value("Timeout", u.b, 0);
    row_value("POST level", se_post[(se_new.flags & SET_POST_MASK) >> SET_POST_SHIFT], 0);
    row_value("Log level", se_log[(se_new.flags & SET_LOG_MASK) >> SET_LOG_SHIFT], 0);
    row_value("Card store at boot", (se_new.flags & SET_SD_STORE) ? "on" : "off", 0);
    row_value("Next boot (once)", se_new.next_key[0] ? se_new.next_key : "none", 0);
    row_value("Last choice", se_new.last_key[0] ? se_new.last_key : "none", 0);
    kv_hdr("SAVE");
    int changed = memcmp(&se_old, &se_new, sizeof(se_new)) != 0;
    row_add("Save", changed ? "tier 1: one block, read back" : "nothing changed", changed ? R_WARN : R_DIS);
    row_add("Revert the changes", "", changed ? 0 : R_DIS);
    kv_note("POST level and log level are read by the loader at every start (loader with the health "
            "update; older loaders ignore them). Last choice: the loader writes it when you pick an OS.");
}

int util_settings_write(struct star_settings *n, const char *what);
int util_settings_write(struct star_settings *n, const char *what)
{
    struct sb_auth au;
    struct ub o, w;
    struct star_settings old;
    int valid = settings_load(&old);
    char ov[64], nv[64];
    ub_init(&o);
    ub_s(&o, valid ? "" : "(no record) ");
    ub_s(&o, old.default_key[0] ? old.default_key : "-");
    ub_c(&o, ' ');
    ub_dec(&o, old.timeout_s);
    ub_s(&o, " s flags ");
    ub_hex(&o, old.flags, 3);
    sb_strcpy(ov, sizeof(ov), o.b);
    ub_init(&w);
    ub_s(&w, n->default_key[0] ? n->default_key : "-");
    ub_c(&w, ' ');
    ub_dec(&w, n->timeout_s);
    ub_s(&w, " s flags ");
    ub_hex(&w, n->flags, 3);
    sb_strcpy(nv, sizeof(nv), w.b);
    struct action a = { what, "SETTINGS", { "Writes the settings record (UFS store block 2), then reads it back",
                        "The boot menu reads it at the next boot", 0, 0 },
                        "edit it again here, or the boot menu's settings", ov, nv, "", TIER1, SB_MED_UFS, 0,
                        SB_KIND_STORE, 0, 0 };
    if (!lab_confirm(&a, &au) || !sb_gate_open(&au))
        return -1;
    int r = settings_save(n);
    sb_gate_close();
    audit_line(r ? "settings NOT saved" : "settings saved and read back");
    ui_toast(r == SB_W_DRYRUN ? "Dry run: not written" : r ? "Settings NOT saved" : "Saved and verified",
             r ? C_RED : C_GREEN, 1200);
    return r;
}

static int act_settings(int i)
{
    unsigned f = se_new.flags;
    switch (i) {
    case 0: next_key(se_new.default_key); break;
    case 1: {
        unsigned k = 0;
        while (k < 7 && se_timeouts[k] != se_new.timeout_s)
            ++k;
        se_new.timeout_s = se_timeouts[(k + 1) % 7];
        break;
    }
    case 2: se_new.flags = (f & ~SET_POST_MASK) | ((((f & SET_POST_MASK) >> SET_POST_SHIFT) + 1U) % 3U) << SET_POST_SHIFT; break;
    case 3: se_new.flags = (f & ~SET_LOG_MASK) | ((((f & SET_LOG_MASK) >> SET_LOG_SHIFT) + 1U) % 4U) << SET_LOG_SHIFT; break;
    case 4: se_new.flags ^= SET_SD_STORE; break;
    case 5: next_key(se_new.next_key); break;
    case 6: se_new.last_key[0] = 0; break;
    case 7:
        if (!util_settings_write(&se_new, "Save the settings"))
            se_load();
        break;
    case 8: se_new = se_old; break;
    default: break;
    }
    return 0;
}

void util_settings(void)
{
    se_load();
    list_screen("Settings editor", build_settings, act_settings);
}

/* ---- PMIC and MAX77705 writes ------------------------------------------------------------------- */

static unsigned pm_type = 1, pm_reg = 0x6c, pm_val = 0xec, mx_addr = 0x69, mx_reg = 0xbd, mx_val = 0x01;

static void build_pmic_w(void)
{
    struct ub u;
    const char *why;
    uint8_t cur = 0;
    kv_hdr("S2MPS18 (ACPM)");
    ub_init(&u);
    ub_s(&u, pm_type == 0 ? "TOP" : pm_type == 1 ? "PMIC" : "RTC");
    row_value("Block", u.b, 0);
    ub_init(&u);
    ub_hex(&u, pm_reg, 2);
    if (sb_pmic_read(pm_type, pm_reg, &cur)) {
        ub_s(&u, "  now ");
        ub_hex(&u, cur, 2);
    }
    row_value("Register", u.b, 0);
    ub_init(&u);
    ub_hex(&u, pm_val, 2);
    row_value("Value", u.b, 0);
    int t = pmic_tier(pm_type, pm_reg, pm_val, &why);
    row_add("Write it", why, t >= TIER3 ? R_RED : R_WARN);
    kv_hdr("MAX77705 (I2C)");
    ub_init(&u);
    ub_hex(&u, mx_addr, 2);
    ub_s(&u, mx_addr == 0x69 ? " charger" : mx_addr == 0x36 ? " fuel gauge" : mx_addr == 0x25 ? " USB-C" : " PMIC");
    row_value("Device", u.b, 0);
    ub_init(&u);
    ub_hex(&u, mx_reg, 2);
    row_value("Register", u.b, 0);
    ub_init(&u);
    ub_hex(&u, mx_val, 2);
    row_value("Value", u.b, 0);
    t = max_tier(mx_addr, mx_reg, mx_val, &why);
    row_add("Write it", why, t >= TIER_NO ? R_DIS : t >= TIER3 ? R_RED : R_WARN);
    char what[64];
    int ua = lab_undo_available(what, sizeof(what));
    row_add("Undo the last write", ua ? what : "nothing to undo", ua ? R_WARN : R_DIS);
    kv_note("Tier 1: the touch and card rails at the kernel's values, the RTC, the charger watchdog feed. "
            "Every other PMIC or MAX77705 register: tier 3 (a wrong voltage can damage the phone at once).");
}

static int pm_write(int max)
{
    struct sb_auth au;
    const char *why;
    char tgt[24], hap[96], nv[16], ov[16];
    struct ub u;
    uint8_t cur = 0;
    int have = max ? (mx_addr == 0x69 ? sb_chg_read((uint8_t)mx_reg, &cur) : 0) : sb_pmic_read(pm_type, pm_reg, &cur);
    ub_init(&u);
    ub_s(&u, max ? "MAX" : "PMIC");
    ub_hexn(&u, max ? mx_addr : pm_type, 2);
    ub_c(&u, '_');
    ub_hexn(&u, max ? mx_reg : pm_reg, 2);
    sb_strcpy(tgt, sizeof(tgt), u.b);
    int t = max ? max_tier(mx_addr, mx_reg, mx_val, &why) : pmic_tier(pm_type, pm_reg, pm_val, &why);
    ub_init(&u);
    ub_s(&u, "Writes one byte: ");
    ub_s(&u, why);
    sb_strcpy(hap, sizeof(hap), u.b);
    ub_init(&u);
    ub_hex(&u, max ? mx_val : pm_val, 2);
    sb_strcpy(nv, sizeof(nv), u.b);
    ub_init(&u);
    if (have)
        ub_hex(&u, cur, 2);
    else
        ub_s(&u, "not read");
    sb_strcpy(ov, sizeof(ov), u.b);
    struct action a = { max ? "MAX77705 write" : "PMIC write", tgt, { hap, "Read back afterwards", 0, 0 },
                        max ? "a power cycle (battery out) resets the MAX77705; Undo writes the old value"
                            : "a reboot resets most PMIC registers; Undo writes the old value",
                        ov, nv, "the old value (Undo the last write)", t, max ? SB_SP_MAX : SB_SP_PMIC, 0,
                        SB_KIND_FREEFORM, max ? ((uint64_t)mx_addr << 8 | mx_reg) : ((uint64_t)pm_type << 8 | pm_reg), 1 };
    if (!lab_confirm(&a, &au))
        return 0;
    int ok = max ? lab_max(mx_addr, mx_reg, mx_val, &au) : lab_pmic(pm_type, pm_reg, pm_val, &au);
    ui_toast(ok ? "Written (see the audit log)" : "Write failed", ok ? C_GREEN : C_RED, 1100);
    return ok;
}

static int act_pmic_w(int i)
{
    uint64_t v;
    switch (i) {
    case 0: pm_type = (pm_type + 1U) % 3U; break;
    case 1: v = pm_reg; if (ask_hex("PMIC register", "Register (00..FF)", &v, 2)) pm_reg = (unsigned)v; break;
    case 2: v = pm_val; if (ask_hex("PMIC value", "Value (00..FF)", &v, 2)) pm_val = (unsigned)v; break;
    case 3: (void)pm_write(0); break;
    case 4: mx_addr = mx_addr == 0x69 ? 0x36 : mx_addr == 0x36 ? 0x25 : mx_addr == 0x25 ? 0x66 : 0x69; break;
    case 5: v = mx_reg; if (ask_hex("MAX77705 register", "Register (00..FF)", &v, 2)) mx_reg = (unsigned)v; break;
    case 6: v = mx_val; if (ask_hex("MAX77705 value", "Value (00..FF)", &v, 2)) mx_val = (unsigned)v; break;
    case 7: (void)pm_write(1); break;
    case 8: lab_undo(); break;
    default: break;
    }
    return 0;
}

void util_pmic(void)
{
    list_screen("PMIC and charger writes", build_pmic_w, act_pmic_w);
}

/* ---- reboot to any target ------------------------------------------------------------------------ */

static void reboot_msg(const char *t, const char *s)
{
    ui_guard(0);
    ui_noclip();
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    ui_text_c(F_TITLE_S, UI_W / 2, 1440, t, C_STAR);
    if (s)
        ui_text_c(F_ROW, UI_W / 2, 1540, s, C_L2);
    ui_present();
}

static void build_reboot_any(void)
{
    char key[24];
    kv_hdr("STARBITE");
    row_add("Boot menu", "reboot: S-Boot, then the Starbite menu", 0);
    row_add("Engineer OS again", "boot-next engineer", 0);
    row_add("Start an OS", "any catalog entry once (boot-next)", 0);
    row_add("Starbite Recovery", "reboot; hold Bixby while the logo shows", 0);
    row_add("Starbite Rescue system", "boot-next rescue (needs a rescue system)", 0);
    kv_hdr("SAMSUNG");
    row_add("TWRP recovery", "INFORM3 recovery", 0);
    row_add("Download mode (Odin)", "INFORM3 download", R_WARN);
    kv_hdr("POWER");
    row_add("Power off", "PS_HOLD low (a cable reboots instead)", 0);
    if (sb_boot_next_peek(key))
        kv_addc("Boot-next mailbox now", key, C_ORANGE);
}

static int act_reboot_any(int i)
{
    switch (i) {
    case 0: eng_exit_to_menu(); break;
    case 1:
        if (dialog("Reboot", "Restart Engineer OS?", 0, "REBOOT")) {
            reboot_msg("Rebooting" U_ELL, "back to Engineer OS");
            sb_boot_next("engineer");
        }
        break;
    case 2: start_page(); break;
    case 3:
        if (dialog("Starbite Recovery", "The phone reboots now.", "Hold Bixby while the Samsung logo shows.", "REBOOT")) {
            reboot_msg("Hold Bixby now", "until the Starbite Recovery menu appears");
            sb_mdelay(1500);
            sb_boot_next_clear();
            sb_reboot(SB_INFORM_NORMAL);
        }
        break;
    case 4:
        if (dialog("Rescue system", "Reboot into the Starbite rescue system?",
                   "Without one the loader opens the recovery menu.", "REBOOT")) {
            reboot_msg("Rebooting" U_ELL, "to the rescue system");
            sb_boot_next("rescue");
        }
        break;
    case 5:
    case 6:
        if (dialog("Reboot", i == 5 ? "Reboot to TWRP?" : "Reboot to Odin download mode?", 0, "REBOOT")) {
            reboot_msg("Rebooting" U_ELL, 0);
            sb_reboot(i == 5 ? SB_INFORM_RECOVERY : SB_INFORM_DOWNLOAD);
        }
        break;
    case 7: reboot_page(); break;
    default: break;
    }
    return 0;
}

void util_reboot(void)
{
    list_screen("Reboot to", build_reboot_any, act_reboot_any);
}
