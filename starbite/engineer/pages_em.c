/* SPDX-License-Identifier: GPL-2.0-only
 * Engineer OS: the pages of the loader's EngineerMode (shim/starlte/em.c), ported onto libsb.
 *
 * The fill / build / act functions are em.c's with libsb's drivers underneath (sb_pmic_read,
 * sb_fg_word, ufs_*, store_*, the libsb fault probes). What changed, and why:
 *   - the screens (info_screen, list_screen, text_screen, dialog) are Engineer OS's (main.c);
 *   - custom screens (display, touch, keys, sector viewer, register peek) are drawn with sbui;
 *   - every write the loader's EngineerMode makes by hand (touch rails, card rails, the settings
 *     record) goes through lab_confirm and the write gate (armed.c): a warning screen, the
 *     confirmations its tier needs, then the write; the register peek moved to util.c;
 *   - things only the loader can see (BOOT's iOS payload and its boot-args, the UEFI bundle, the
 *     intro's frame statistics) are shown as such; "Start an OS" writes the boot-next mailbox and
 *     resets, and the loader starts the entry;
 *   - the RAM test and bandwidth run in libsb's windows (mmu.c), not at 0x92000000 where this
 *     image now lives.
 */
#include "eng.h"

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

static void kv_arg(const char *label, const char *key)
{
    char v[96];
    if (sb_bootarg(key, v, sizeof(v)))
        kv_add(label, v);
    else
        kv_addc(label, "-", UI_TEXT3);
}

/* ======================================================================= */
/* DEVICE                                                                   */
/* ======================================================================= */

void fill_device_info(void)
{
    char v[96];
    struct ub u;
    kv_hdr("PRODUCT");
    kv_arg("Model", "androidboot.em.model");
    kv_add("Device", "Samsung Galaxy S9 (starlte)");
    kv_arg("Hardware revision", "androidboot.revision");
    kv_arg("Serial number", "androidboot.serialno");
    kv_arg("AP serial", "androidboot.ap_serial");
    kv_arg("Sales code", "sales_code");
    kv_arg("Carrier ID", "androidboot.carrierid");
    kv_arg("Region", "region1");
    kv_hdr("BOOTLOADER");
    kv_arg("S-Boot version", "androidboot.bootloader");
    kv_arg("Warranty bit (Knox)", "androidboot.warranty_bit");
    kv_arg("Verified boot state", "androidboot.verifiedbootstate");
    kv_arg("FMM lock", "androidboot.fmm_lock");
    kv_arg("Odin download flag", "androidboot.odin_download");
    kv_arg("HMAC mismatch", "androidboot.hmac_mismatch");
    kv_arg("Boot mode", "bootmode");
    kv_arg("Charging mode", "charging_mode");
    kv_hdr("HARDWARE");
    kv_arg("DRAM", "androidboot.dram_info");
    if (sb_bootarg("sec_debug.lpddr4_size", v, sizeof(v))) {
        ub_init(&u);
        ub_s(&u, v);
        ub_s(&u, " GB LPDDR4X");
        kv_add("Memory", u.b);
    }
    kv_arg("LCD type", "lcdtype");
    kv_arg("PMIC info", "pmic_info");
    kv_arg("CCIC info", "ccic_info");
    kv_hdr("STARBITE");
    kv_add("Engineer OS", ENG_VERSION " " U_DOT " " ENG_BUILD);
    kv_hex("Image base", sb_boot.base, 9);
    kv_dec("Relocations applied", (uint64_t)(sb_boot.relocs < 0 ? 0 : sb_boot.relocs), 0);
    kv_add("Loader log page", sb_boot.loader_tag == 0x31304554524c5453ULL ? "STRLTE01 (bare handoff)" : "not found");
    kv_add("Engineer Lite", "in the loader: hold Bixby in the menu");
}

/* ---- trapped reads (libsb fault probes) ------------------------------------------ */

#define SAFE_MRS(reg, out) SB_SAFE_MRS(reg, out)

/* 32-bit load that survives a synchronous abort: 1 = read, 0 = faulted. */
static int probe32(uintptr_t a, uint32_t *v)
{
    return sb_probe32(a, v);
}

static void kv_trap(const char *k)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "trapped, ESR ");
    ub_hex(&u, sb_fault.esr, 8);
    kv_addc(k, u.b, UI_ORANGE);
}

static void kv_sysreg(const char *k, int ok, uint64_t v)
{
    if (ok)
        kv_hex(k, v, 16);
    else
        kv_trap(k);
}

/* CHIPID block 0x10000000: PRO_ID at +0x0, unique ID at +0x14 / +0x18. */
void fill_soc(void)
{
    uint64_t midr, mpidr, cel, isar0 = 0, pfr0 = 0, mmfr0 = 0, ctr = 0, clidr = 0, cntfrq;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(cel));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));
    int ok_isar = SAFE_MRS(id_aa64isar0_el1, &isar0);
    int ok_pfr = SAFE_MRS(id_aa64pfr0_el1, &pfr0);
    int ok_mmfr = SAFE_MRS(id_aa64mmfr0_el1, &mmfr0);
    int ok_ctr = SAFE_MRS(ctr_el0, &ctr);
    int ok_clidr = SAFE_MRS(clidr_el1, &clidr);
    uint32_t pro = 0, u0 = 0, u1 = 0;
    int ok_pro = probe32(0x10000000UL, &pro);
    int ok_uid = probe32(0x10000014UL, &u0) && probe32(0x10000018UL, &u1);
    struct ub u;

    kv_hdr("SYSTEM ON CHIP");
    kv_add("SoC", "Samsung Exynos 9810 (10 nm LPP)");
    if (ok_pro) {
        kv_hex("CHIPID PRO_ID", pro, 8);
        ub_init(&u);
        ub_s(&u, "product ");
        ub_hexn(&u, pro >> 12, 5);
        ub_s(&u, ", rev ");
        ub_dec(&u, (pro >> 4) & 0xfU);
        ub_c(&u, '.');
        ub_dec(&u, pro & 0xfU);
        kv_add("Decoded", u.b);
    } else {
        kv_trap("CHIPID PRO_ID");
    }
    if (ok_uid) {
        ub_init(&u);
        ub_hexn(&u, u1 & 0xffffU, 4);
        ub_hexn(&u, u0, 8);
        kv_add("Unique ID", u.b);
    } else {
        kv_trap("Unique ID");
    }
    kv_add("CPU cores", "4x Mongoose M3 + 4x Cortex-A55");
    kv_add("GPU", "Mali-G72 MP18");

    kv_hdr("THIS CPU (BOOT CORE)");
    ub_init(&u);
    unsigned part = (unsigned)(midr >> 4) & 0xfffU, impl = (unsigned)(midr >> 24) & 0xffU;
    if (impl == 0x41 && part == 0xd05)
        ub_s(&u, "ARM Cortex-A55");
    else if (impl == 0x53)
        ub_s(&u, "Samsung Mongoose");
    else
        ub_s(&u, "unknown");
    ub_s(&u, " r");
    ub_dec(&u, (midr >> 20) & 0xfU);
    ub_c(&u, 'p');
    ub_dec(&u, midr & 0xfU);
    kv_add("Core", u.b);
    kv_hex("MIDR_EL1", midr, 8);
    kv_hex("MPIDR_EL1", mpidr, 8);
    kv_dec("Exception level", (cel >> 2) & 3U, 0);
    kv_dec("CNTFRQ_EL0", cntfrq, cntfrq ? " Hz" : " (S-Boot left 0; 26 MHz)");

    kv_hdr("ID REGISTERS");
    kv_sysreg("ID_AA64ISAR0_EL1", ok_isar, isar0);
    if (ok_isar) {
        ub_init(&u);
        if ((isar0 >> 4) & 0xfU)
            ub_s(&u, "AES ");
        if ((isar0 >> 8) & 0xfU)
            ub_s(&u, "SHA1 ");
        if ((isar0 >> 12) & 0xfU)
            ub_s(&u, "SHA2 ");
        if ((isar0 >> 16) & 0xfU)
            ub_s(&u, "CRC32 ");
        if ((isar0 >> 20) & 0xfU)
            ub_s(&u, "LSE ");
        if ((isar0 >> 28) & 0xfU)
            ub_s(&u, "RDM ");
        if ((isar0 >> 44) & 0xfU)
            ub_s(&u, "DotProd");
        kv_add("ISA extensions", u.b);
    }
    kv_sysreg("ID_AA64PFR0_EL1", ok_pfr, pfr0);
    if (ok_pfr) {
        ub_init(&u);
        ub_s(&u, (((pfr0 >> 8) & 0xfU) ? "EL2 " : "no-EL2 "));
        ub_s(&u, (((pfr0 >> 12) & 0xfU) ? "EL3 " : ""));
        ub_s(&u, (((pfr0 >> 16) & 0xfU) == 0xf) ? "no-FP" : "FP");
        ub_s(&u, (((pfr0 >> 20) & 0xfU) == 0xf) ? " no-SIMD" : " SIMD");
        kv_add("Features", u.b);
    }
    kv_sysreg("ID_AA64MMFR0_EL1", ok_mmfr, mmfr0);
    if (ok_mmfr) {
        static const unsigned char pa[7] = { 32, 36, 40, 42, 44, 48, 52 };
        unsigned par = (unsigned)mmfr0 & 0xfU;
        kv_dec("Physical address bits", par < 7 ? pa[par] : 0, 0);
    }
    kv_sysreg("CTR_EL0", ok_ctr, ctr);
    if (ok_ctr)
        kv_dec("D-cache line", 4U << ((ctr >> 16) & 0xfU), " bytes");
    kv_sysreg("CLIDR_EL1", ok_clidr, clidr);

    kv_hdr("CACHES");
    for (unsigned lvl = 0; ok_clidr && lvl < 3; ++lvl) {
        unsigned ty = (unsigned)(clidr >> (3 * lvl)) & 7U;
        if (!ty)
            break;
        uint64_t sel = (uint64_t)lvl << 1, ccs = 0, c = sb_fault.count;
        sb_probe_depth++;
        __asm__ volatile("mov %0, xzr\n\tmsr csselr_el1, %1\n\tisb\n\tmrs %0, ccsidr_el1"
                         : "=&r"(ccs) : "r"(sel) : "memory");
        sb_probe_depth--;
        const char *name = lvl == 0 ? "L1 D-cache" : lvl == 1 ? "L2 cache" : "L3 cache";
        if (sb_fault.count != c) {
            kv_trap(name);
            continue;
        }
        uint64_t line = 16ULL << (ccs & 7U), ways = ((ccs >> 3) & 0x3ffU) + 1, sets = ((ccs >> 13) & 0x7fffU) + 1;
        ub_init(&u);
        ub_size(&u, line * ways * sets);
        ub_s(&u, ", ");
        ub_dec(&u, ways);
        ub_s(&u, "-way");
        kv_add(name, u.b);
    }
    if (sb_fault.count) {
        kv_hdr("TRAPS (CAUGHT BY LIBSB PROBES)");
        kv_dec("Count", sb_fault.count, 0);
        kv_hex("Last ESR", sb_fault.esr, 8);
        kv_hex("Last ELR", sb_fault.elr, 16);
    }
}

void fill_memory(void)
{
    uint64_t base[8], size[8], total = 0;
    int n = fdt_memory_banks(sb_boot.fdt, base, size, 8);
    struct ub u;
    kv_hdr("DRAM BANKS (S-BOOT DEVICE TREE)");
    for (int i = 0; i < n; ++i) {
        ub_init(&u);
        ub_hex(&u, base[i], 9);
        ub_s(&u, " + ");
        ub_size(&u, size[i]);
        kv_add("Bank", u.b);
        total += size[i];
    }
    if (!n)
        kv_addc("Bank", "none reported", UI_TEXT3);
    ub_init(&u);
    ub_size(&u, total);
    kv_add("Total", u.b);
    kv_hdr("ENGINEER OS (LIBSB IDENTITY MAP)");
    ub_init(&u);
    ub_hex(&u, sb_boot.base, 9);
    ub_s(&u, " + ");
    ub_size(&u, sb_boot.image_size);
    kv_add("Image (write-back)", u.b);
    ub_init(&u);
    ub_size(&u, sb_arena_used());
    ub_s(&u, " used, ");
    ub_size(&u, sb_arena_free());
    ub_s(&u, " free");
    kv_add("Arena to 0x0A0000000", u.b);
    kv_add("RAM test window", "0x0A0100000 + 126 MiB, non-cacheable");
    kv_addc("Never written", "0x0A0000000 + 1 MiB: S-Boot's ECT (the kernel reads it)", UI_ORANGE);
    kv_add("UFS DMA", "0x0AF000000 + 8 MiB, non-cacheable");
    kv_add("Framebuffer", "0x0CC000000, write-combined");
    kv_add("S-Boot FDT", "0x08A000000 (x0 at entry)");
    kv_add("Unmapped", "0x0AF800000 .. 0x0C0000000 (RKP, TIMA, secure)");
    {
        static char rn[24][32];
        uint64_t rb[24], rs[24];
        int nr = fdt_reserved(sb_boot.fdt, rn, rb, rs, 24);
        if (nr > 0) {
            kv_hdr("RESERVED MEMORY (S-BOOT DEVICE TREE, LIVE)");
            for (int i = 0; i < nr; ++i) {
                ub_init(&u);
                ub_hex(&u, rb[i], 9);
                ub_s(&u, " + ");
                ub_size(&u, rs[i]);
                kv_add(rn[i], u.b);
            }
        }
    }
    kv_hdr("CARVE-OUTS (VENDOR DTB)");
    kv_add("RKP", "0x0AF800000 + 32 MiB");
    kv_add("TIMA", "0x0B8000000 + 2 MiB");
    kv_add("Secure world", "0x0BC800000 .. 0x0C0000000");
    kv_add("seclog", "0x0C0000000 + 512 KiB");
    kv_add("Framebuffer", "0x0CC000000 + 16.3 MiB");
    kv_add("Secure camera", "0x0E1900000 + 30 MiB");
    kv_add("Modem (CP)", "0x0F0000000 + 146 MiB");
    kv_add("CP logging", "0x0F9200000 + 32 MiB");
    kv_add("Audio (abox)", "0x0FB200000 + 40 MiB");
    kv_add("exynos-ss + pstore", "0x0FDA00000 + 28 MiB");
    kv_add("Camera firmware", "0x0FF800000 + 8 MiB");
}

static void rst_bits(struct ub *u, uint32_t v)
{
    if (v & 0x20000000U)
        ub_s(u, "SWRESET ");
    if (v & 0x01000000U)
        ub_s(u, "CL0-WDT ");
    if (v & 0x02000000U)
        ub_s(u, "CL1-WDT ");
    if (v & 0x00010000U)
        ub_s(u, "PIN ");
    if (!u->n)
        ub_s(u, "none decoded");
}

void fill_reset(void)
{
    volatile uint32_t *pmu = (volatile uint32_t *)SB_PMU;
    uint32_t rst = pmu[0x404 / 4], i2 = pmu[0x808 / 4], i3 = pmu[0x80c / 4], sys = pmu[0x810 / 4];
    struct ub u;
    kv_hdr("PMU (LIVE)");
    kv_hex("RST_STAT", rst, 8);
    ub_init(&u);
    rst_bits(&u, rst);
    kv_add("Reset source", u.b);
    kv_hex("INFORM2", i2, 8);
    kv_hex("INFORM3", i3, 8);
    kv_add("INFORM3 means", i3 == SB_INFORM_RECOVERY ? "recovery next" : i3 == SB_INFORM_DOWNLOAD ? "download next" :
                           i3 == SB_INFORM_NORMAL ? "normal boot" : "other");
    kv_hex("SYSIP_DAT0", sys, 8);
    kv_hdr("S-BOOT REPORT");
    kv_arg("sec_debug.rst_stat", "sec_debug.rst_stat");
    kv_arg("Reset reason", "sec_debug.reset_reason");
    kv_arg("Power-on source", "sec_debug.pwronsrc");
    kv_arg("Power-off source", "sec_debug.pwroffsrc");
    kv_arg("Boot mode", "bootmode");
    kv_hdr("PMIC");
    uint8_t on = 0, off = 0, s1 = 0, s2 = 0;
    if (sb_pmic_read(1, 0x10, &on))
        kv_hex("PWRONSRC", on, 2);
    if (sb_pmic_read(1, 0x11, &off))
        kv_hex("OFFSRC", off, 2);
    if (sb_pmic_read(1, 0x0e, &s1))
        kv_hex("STATUS1", s1, 2);
    if (sb_pmic_read(1, 0x0f, &s2))
        kv_hex("STATUS2", s2, 2);
}

/* ======================================================================= */
/* HARDWARE TESTING                                                          */
/* ======================================================================= */

void fill_cpu(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    kv_hdr("CLUSTERS");
    kv_add("Cluster 0", "4x Cortex-A55, up to 1.95 GHz");
    kv_add("Cluster 1", "4x Mongoose M3, up to 2.7 GHz");
    kv_hdr("CORE MAP (MPIDR: MT, AFF2 CLUSTER, AFF1 CORE)");
    for (unsigned c = 0; c < 8; ++c) {
        struct ub k, v;
        uint64_t aff = 0x81000000ULL | (c < 4 ? 0 : 0x10000U) | ((uint64_t)(c & 3U) << 8);
        ub_init(&k);
        ub_s(&k, "cpu");
        ub_dec(&k, c);
        ub_s(&k, c < 4 ? "  A55" : "  M3");
        ub_init(&v);
        ub_hex(&v, aff, 8);
        ub_s(&v, c < 4 ? "  PSCI " : "  PSCI ");
        ub_hex(&v, c < 4 ? c : 0x100U + (c - 4U), 3);
        int here = (mpidr & 0xffffffU) == (aff & 0xffffffU);
        ub_s(&v, here ? "  running here" : "");
        kv_addc(k.b, v.b, here ? UI_GREEN : UI_TEXT2);
    }
    kv_hdr("NOTES");
    kv_add("Bootloader", "single core, MMU off");
    kv_add("iOS", "cpu0-3 (A55) via PSCI CPU_ON 1-3");
    kv_add("Windows", "4 cores (numproc 4)");
    kv_add("M3 MPIDR", "expected value; see the bring-up test");
}

/* ---- display test: full-screen fields drawn into the back buffer -------------------------- */

static void field_draw(unsigned f, const char **name)
{
    static const uint32_t col[7] = { 0xffff0000U, 0xff00ff00U, 0xff0000ffU, 0xffffffffU, 0xff000000U,
                                     0xff808080U, 0xff333333U };
    static const char *const names[11] = { "red", "green", "blue", "white", "black", "grey 50 %",
                                           "grey 20 % (uniformity)", "grey ramp (banding)", "checkerboard",
                                           "grid (geometry)", "dead pixels: white dots on black" };
    ui_guard(0);
    ui_noclip();
    if (f < 7) {
        ui_fill(0, 0, UI_W, UI_H, col[f]);
    } else if (f == 7) {
        for (int y = 0; y < UI_H; y += 4)
            ui_fill(0, y, UI_W, 4, 0xff000000U | (uint32_t)(y * 255 / UI_H) * 0x010101U);
    } else if (f == 8) {
        for (int y = 0; y < UI_H; y += 80)
            for (int x = 0; x < UI_W; x += 80)
                ui_fill(x, y, 80, 80, ((x / 80 + y / 80) & 1) ? 0xffffffffU : 0xff000000U);
    } else if (f == 9) {
        ui_fill(0, 0, UI_W, UI_H, C_BG);
        for (int x = 0; x <= UI_W; x += 144)
            ui_fill(x >= UI_W ? UI_W - 2 : x, 0, 2, UI_H, C_STAR);
        for (int y = 0; y <= UI_H; y += 148)
            ui_fill(0, y >= UI_H ? UI_H - 2 : y, UI_W, 2, C_STAR);
        ui_ring(UI_W / 2, UI_H / 2, 600, 3, C_STAR);
    } else {
        ui_fill(0, 0, UI_W, UI_H, C_BG);
        for (int y = 40; y < UI_H; y += 120)
            for (int x = 40; x < UI_W; x += 120)
                ui_fill(x, y, 1, 1, 0xffffffffU);
    }
    *name = names[f];
}

#define FIELDS 11

void test_display(void)
{
    int f = 0;
    for (;;) {
        const char *name;
        struct ub u;
        field_draw((unsigned)f, &name);
        ub_init(&u);
        ub_dec(&u, (uint64_t)f + 1);
        ub_c(&u, '/');
        ub_dec(&u, FIELDS);
        ub_s(&u, "  ");
        ub_s(&u, name);
        uint32_t bg = f == 3 || f == 1 ? 0xc0000000U : 0xc0ffffffU;
        uint32_t fg = f == 3 || f == 1 ? C_L1 : 0xff000000U;
        if (f == 4 || f >= 9) {
            bg = 0x80303034U;
            fg = C_L2;
        }
        int w = ui_text_w(F_DETAIL, u.b) + 80;
        ui_round(UI_W / 2 - w / 2, UI_H - 230, w, 96, 48, bg);
        ui_text_c(F_DETAIL, UI_W / 2, UI_H - 168, u.b, fg);
        ui_present();
        /* bright static fields stay at most 20 s (no burn-in from a forgotten test) */
        uint64_t t0 = sb_ms();
        int next = 0;
        while (!next) {
            struct ui_ev e;
            ui_poll(&e, 50);
            if (e.type == EV_BACK || e.type == EV_HOME) {
                ui_guard(armed_tier() > TIER0 ? 14 : 0);
                return;
            }
            if (e.type == EV_SELECT || e.type == EV_TAP || e.type == EV_DOWN || e.type == EV_SWIPE_L)
                next = 1;
            else if ((e.type == EV_UP || e.type == EV_SWIPE_R) && f > 0) {
                f -= 2;
                next = 1;
            } else if (sb_ms() - t0 > 20000)
                next = 1;
        }
        if (++f >= FIELDS)
            break;
    }
    ui_guard(armed_tier() > TIER0 ? 14 : 0);
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    int ok = dialog("Display test", "Did every field look right?", "No lines, blotches, dead or stuck pixels.", "PASS");
    test_record("Display fields", ok, ok ? "user: all fields correct" : "user: a field looked wrong");
}

/* ---- touch test: ten fingers, one colour each --------------------------------------------- */

static const uint32_t finger_col[10] = { 0xff7cf0b4U, 0xff6fb3ffU, 0xffff9f0aU, 0xffff453aU, 0xffbf5af2U,
                                         0xffffd60aU, 0xff64d2ffU, 0xffff375fU, 0xff30d158U, 0xffe5e5eaU };

static void touch_head(void)
{
    struct ub u;
    ui_fill(0, 0, UI_W, 220, C_BG);
    ub_init(&u);
    if (!sb_tp.ok) {
        ub_s(&u, "Touch did not start: ");
        ub_s(&u, sb_tp.why ? sb_tp.why : "?");
        ui_text(F_DETAIL, 48, 90, u.b, C_RED);
    } else {
        ub_s(&u, "S6SY761 ");
        ub_hexn(&u, sb_tp.id[0], 2);
        ub_hexn(&u, sb_tp.id[1], 2);
        ub_hexn(&u, sb_tp.id[2], 2);
        ub_s(&u, "  panel ");
        ub_dec(&u, sb_tp.max_x);
        ub_c(&u, 'x');
        ub_dec(&u, sb_tp.max_y);
        ub_s(&u, "  ");
        ub_dec(&u, sb_tp.tx);
        ub_c(&u, 'x');
        ub_dec(&u, sb_tp.rx);
        ub_s(&u, " ch");
        ui_text(F_MONO, 48, 90, u.b, C_STAR);
    }
    ui_text(F_SMALL, 48, 170, "Ten fingers " U_DOT " Power: clear " U_DOT " Volume, Bixby or hold Power: back", C_L2);
}

void test_touch(void)
{
    int lx[10], ly[10], seen = 0, maxf = 0;
    for (int i = 0; i < 10; ++i)
        lx[i] = -1;
    if (!sb_tp.ok)
        (void)sb_touch_start();
    uint64_t pw_t0 = 0;
    ui_guard(0);
    ui_noclip();
    sb_keys_release_wait();             /* the Power release that opened this page */
    for (int clear = 1;;) {
        if (clear) {
            ui_fill(0, 0, UI_W, UI_H, C_BG);
            for (int x = 0; x < UI_W; x += 144)
                ui_fill(x, 220, 1, UI_H - 220, 0xff1a1e26U);
            for (int y = 220; y < UI_H; y += 148)
                ui_fill(0, y, UI_W, 1, 0xff1a1e26U);
            touch_head();
            ui_present();
            clear = 0;
        }
        /* Keys only leave or clear (touch is the thing under test): Volume or Bixby leave, a
         * short Power clears, Power held UI_POWER_LONG_MS leaves (as "back" everywhere else). */
        unsigned k = sb_keys();
        if (k & (SB_K_BIXBY | SB_K_UP | SB_K_DOWN))
            break;
        if (k & SB_K_POWER) {
            if (!pw_t0)
                pw_t0 = sb_ms() | 1U;
            else if (sb_ms() - pw_t0 >= UI_POWER_LONG_MS)
                break;
        } else if (pw_t0) {
            pw_t0 = 0;
            clear = 1;
            continue;
        }
        int id[10], st[10], x[10], y[10];
        int n = sb_touch_events(id, st, x, y, 10), down = 0;
        for (int i = 0; i < n; ++i) {
            int f = id[i] < 10 ? id[i] : 9;
            ++seen;
            if (st[i] == 3) {
                lx[f] = -1;
                continue;
            }
            if (lx[f] >= 0) {
                int steps = iabs(x[i] - lx[f]) > iabs(y[i] - ly[f]) ? iabs(x[i] - lx[f]) : iabs(y[i] - ly[f]);
                steps = steps / 6 + 1;
                for (int s = 0; s <= steps; ++s)
                    ui_disc(lx[f] + (x[i] - lx[f]) * s / steps, ly[f] + (y[i] - ly[f]) * s / steps, 9, finger_col[f]);
            } else {
                ui_disc(x[i], y[i], 16, finger_col[f]);
            }
            lx[f] = x[i];
            ly[f] = y[i];
        }
        for (int f = 0; f < 10; ++f)
            down += lx[f] >= 0;
        if (down > maxf)
            maxf = down;
        if (n) {
            struct ub u;
            ub_init(&u);
            ub_dec(&u, (uint64_t)down);
            ub_s(&u, down == 1 ? " finger" : " fingers");
            ub_s(&u, "   most at once: ");
            ub_dec(&u, (uint64_t)maxf);
            ui_fill(0, 186, UI_W, 34, C_BG);
            ui_text(F_MONO, 48, 214, u.b, C_L1);
        }
        ui_present();
        sb_udelay(3000);
    }
    sb_keys_release_wait();
    ui_input_reset();
    ui_guard(armed_tier() > TIER0 ? 14 : 0);
    struct ub d;
    ub_init(&d);
    ub_dec(&d, (uint64_t)seen);
    ub_s(&d, " events, up to ");
    ub_dec(&d, (uint64_t)maxf);
    ub_s(&d, " fingers");
    test_record("Touch paint", sb_tp.ok && seen > 0, sb_tp.ok ? d.b : "controller did not start");
}

/* ---- keys ------------------------------------------------------------------------------------ */

static unsigned keys_seen;

static void fill_keys(void)
{
    unsigned k = sb_keys();
    uint32_t a = *(volatile uint32_t *)0x14050024UL, b = *(volatile uint32_t *)0x14050064UL;
    keys_seen |= k;
    kv_hdr("LIVE " U_DOT " HOLD ANY ONE KEY 1.5 S OR TAP BACK TO LEAVE");
    kv_addc("Volume up   (GPA0-3)", (k & SB_K_UP) ? "PRESSED" : (keys_seen & SB_K_UP) ? "released " U_CHECK : "released",
            (k & SB_K_UP) ? UI_GREEN : UI_TEXT2);
    kv_addc("Volume down (GPA0-4)", (k & SB_K_DOWN) ? "PRESSED" : (keys_seen & SB_K_DOWN) ? "released " U_CHECK : "released",
            (k & SB_K_DOWN) ? UI_GREEN : UI_TEXT2);
    kv_addc("Bixby       (GPA0-6)", (k & SB_K_BIXBY) ? "PRESSED" : (keys_seen & SB_K_BIXBY) ? "released " U_CHECK : "released",
            (k & SB_K_BIXBY) ? UI_GREEN : UI_TEXT2);
    kv_addc("Power       (GPA2-4)", (k & SB_K_POWER) ? "PRESSED" : (keys_seen & SB_K_POWER) ? "released " U_CHECK : "released",
            (k & SB_K_POWER) ? UI_GREEN : UI_TEXT2);
    kv_hdr("RAW");
    kv_hex("GPA0 DAT", a, 8);
    kv_hex("GPA2 DAT", b, 8);
    kv_addc("Touch INT  (GPA1-0)", (*(volatile uint32_t *)0x14050044UL & 1U) ? "high (idle)" : "LOW (event)",
            UI_TEXT2);
}

/* Keys page: its own loop (keys are the thing under test). */
void test_keys(void)
{
    struct page p;
    page_init(&p, UI_VIEW_Y0, UI_VIEW_Y1);
    screen_push("Keys");
    chrome("Keys", "LIVE", C_GREEN);
    keys_seen = 0;
    unsigned held = 0;
    uint64_t t0 = 0;
    sb_keys_release_wait();             /* the Power release that opened this page */
    for (;;) {
        eng_build_page(&p, fill_keys);
        ui_present();
        /* Leave: one key held alone for 1.5 s (Bixby: 1 s), so every key can still be tested. */
        unsigned k = sb_keys();
        if (k == SB_K_UP || k == SB_K_DOWN || k == SB_K_POWER || k == SB_K_BIXBY) {
            if (held != k)
                t0 = sb_ms();
            held = k;
            if (sb_ms() - t0 >= (k == SB_K_BIXBY ? 1000U : 1500U))
                break;
        } else {
            held = 0;
        }
        int st, x, y;
        if (sb_touch_event(&st, &x, &y) && st == 3 && (ui_header_back_hit(x, y) || y >= UI_TAB_Y))
            break;
        sb_mdelay(30);
    }
    sb_keys_release_wait();
    ui_input_reset();
    screen_pop();
    page_free(&p);
    unsigned all = SB_K_UP | SB_K_DOWN | SB_K_BIXBY | SB_K_POWER;
    test_record("Keys", (keys_seen & all) == all,
                (keys_seen & all) == all ? "all four keys seen" : "not every key was pressed");
}

/* ---- memory test: DRAM through the non-cacheable window -------------------------------------- */

#define RAMTEST_BASE SB_TEST_BASE

static uint64_t ram_errors, ram_ms, ram_size = 16UL << 20, ram_first_bad;
static int ram_done;

static void fill_ram(void)
{
    struct ub u;
    kv_hdr("PATTERN TEST");
    ub_init(&u);
    ub_hex(&u, RAMTEST_BASE, 9);
    ub_s(&u, " + ");
    ub_size(&u, ram_size);
    kv_add("Region", u.b);
    kv_add("Mapping", "non-cacheable: every access reaches DRAM");
    kv_add("Passes", "address hash, inverted, walking ones");
    if (!ram_done) {
        kv_add("Status", "not run");
        return;
    }
    kv_addc("Result", ram_errors ? "FAIL" : "PASS: no errors", ram_errors ? UI_RED : UI_GREEN);
    kv_dec("Bad words", ram_errors, 0);
    if (ram_errors)
        kv_hex("First bad address", ram_first_bad, 9);
    kv_dec("Time", ram_ms, " ms");
    ub_init(&u);
    ub_dec(&u, ram_ms ? (ram_size * 5 / 1024 / 1024) * 1000 / ram_ms : 0);
    ub_s(&u, " MiB/s (5 sweeps)");
    kv_add("Throughput", u.b);
}

static int ram_pass;

static void fill_ram_running(void)
{
    kv_hdr("RUNNING");
    kv_add("Pass", ram_pass == 0 ? "1/3 address hash" : ram_pass == 1 ? "2/3 inverted" : "3/3 walking ones");
    kv_addc("Do not", "press keys: the test owns the CPU", UI_TEXT3);
}

static void run_ram(uint64_t size)
{
    volatile uint64_t *m = (volatile uint64_t *)RAMTEST_BASE;
    uint64_t n = size / 8, t0 = sb_ticks();
    ram_errors = 0;
    ram_size = size;
    ram_first_bad = 0;
    for (unsigned pass = 0; pass < 3; ++pass) {
        ram_pass = (int)pass;
        show_now("Memory test", fill_ram_running);
        for (uint64_t i = 0; i < n; ++i)
            m[i] = pass == 2 ? 1ULL << (i & 63) : (i * 0x9e3779b97f4a7c15ULL) ^ (pass ? ~0ULL : 0);
        __asm__ volatile("dsb sy" : : : "memory");
        for (uint64_t i = 0; i < n; ++i) {
            uint64_t want = pass == 2 ? 1ULL << (i & 63) : (i * 0x9e3779b97f4a7c15ULL) ^ (pass ? ~0ULL : 0);
            if (m[i] != want) {
                if (!ram_errors)
                    ram_first_bad = RAMTEST_BASE + i * 8;
                ++ram_errors;
            }
        }
    }
    ram_ms = (sb_ticks() - t0) * 1000U / sb_freq();
    ram_done = 1;
    struct ub u;
    ub_init(&u);
    ub_size(&u, size);
    ub_s(&u, ram_errors ? ", errors: " : ", no errors, ");
    if (ram_errors)
        ub_dec(&u, ram_errors);
    else {
        ub_dec(&u, ram_ms);
        ub_s(&u, " ms");
    }
    test_record("Memory test", !ram_errors, u.b);
    info_screen("Memory test", fill_ram, 0);
}

static void build_ram(void)
{
    kv_hdr("RUN");
    row_add("Quick: 16 MiB", "about a second", 0);
    row_add("Whole window: 126 MiB", "0x0A0100000 .. 0x0A7F00000, several seconds", 0);
    row_add("Last result", ram_done ? (ram_errors ? "FAIL" : "PASS") : "not run", ram_done ? 0 : R_DIS);
    kv_note("Only the free window libsb maps non-cacheable is tested: this image, its arena and every "
            "carve-out are left alone.");
}

static int act_ram(int i)
{
    if (i == 0)
        run_ram(16UL << 20);
    else if (i == 1)
        run_ram(SB_TEST_SIZE);
    else
        info_screen("Memory test", fill_ram, 0);
    return 0;
}

void run_ram_dialog(void)
{
    list_screen("Memory test", build_ram, act_ram);
}


void fill_rtc(void)
{
    struct sb_rtc r;
    struct ub u;
    sb_rtc_read(&r);
    ub_init(&u);
    if (r.ok) {
        ub_dec2(&u, r.h);
        ub_c(&u, ':');
        ub_dec2(&u, r.mi);
        ub_c(&u, ':');
        ub_dec2(&u, r.s);
    } else {
        ub_s(&u, "--:--:--");
    }
    struct ub d;
    ub_init(&d);
    if (r.ok) {
        ub_dec(&d, r.y);
        ub_c(&d, '-');
        ub_dec2(&d, r.mo);
        ub_c(&d, '-');
        ub_dec2(&d, r.d);
        ub_s(&d, "  UTC");
    } else {
        ub_s(&d, "RTC not readable");
    }
    kv_big(d.b, u.b);
    kv_hdr("S2MPS18 RTC (ACPM TYPE 2)");
    kv_hex("CTRL", r.ctrl, 2);
    kv_add("Mode", (r.ctrl & 2U) ? "24 h, binary" : "12 h or BCD");
    kv_add("Used by", "Windows clock (UEFI), TWRP, iOS later");
    kv_add("Set from", "TWRP: date -u -s, hwclock -w -u");
}

static uint64_t hz_measured;

void fill_timer(void)
{
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    kv_hdr("GENERIC TIMER");
    kv_dec("CNTFRQ_EL0", f, f ? " Hz" : " (not set by S-Boot)");
    kv_dec("Assumed", sb_freq(), " Hz");
    kv_hex("CNTPCT_EL0", sb_ticks(), 16);
    struct ub u;
    ub_init(&u);
    ub_dec(&u, sb_ticks() / sb_freq());
    ub_s(&u, " s");
    kv_big("since power-on (counter)", u.b);
    if (hz_measured)
        kv_dec("Counter vs RTC", hz_measured, " ticks per RTC second");
}

/* Count counter ticks across one PMIC RTC second boundary. */
void measure_timer(void)
{
    struct sb_rtc a, b;
    sb_rtc_read(&a);
    if (!a.ok)
        return;
    uint64_t t0 = 0;
    for (unsigned i = 0; i < 400; ++i) {
        sb_rtc_read(&b);
        if (b.ok && b.s != a.s) {
            if (!t0) {
                t0 = sb_ticks();
                a = b;
            } else {
                hz_measured = sb_ticks() - t0;
                return;
            }
        }
        sb_mdelay(5);
    }
}

/* ======================================================================= */
/* POWER                                                                      */
/* ======================================================================= */

static void ldo_name(struct ub *u, unsigned n)
{
    ub_s(u, "LDO");
    ub_dec(u, n);
    switch (n) {
    case 35:
        ub_s(u, "  tsp_io");
        break;
    case 43:
        ub_s(u, "  tsp_avdd");
        break;
    default:
        break;
    }
}

/* S2MPS18 LDOn CTRL: L1 0x49, L2 0x4A (+0x4B CTRL2), L3.. 0x4C + n - 3. */
static unsigned ldo_reg(unsigned n)
{
    return n == 1 ? 0x49U : n == 2 ? 0x4aU : 0x4cU + (n - 3U);
}

void fill_pmic(void)
{
    char build[25];
    struct ub u;
    uint8_t id = 0;
    kv_hdr("S2MPS18 (VIA ACPM, READ ONLY)");
    if (sb_acpm_build(build))
        kv_add("ACPM firmware", build);
    else
        kv_addc("ACPM", "PMIC channel not found", UI_RED);
    if (sb_pmic_read(0, 0x00, &id))
        kv_hex("PMIC ID (TOP 0x00)", id, 2);
    kv_hdr("LDO REGULATORS: CTRL [7:6] ENABLE, [5:0] VSEL");
    for (unsigned n = 1; n <= 45; ++n) {
        uint8_t v = 0;
        struct ub k;
        ub_init(&k);
        ldo_name(&k, n);
        ub_init(&u);
        if (!sb_pmic_read(1, ldo_reg(n), &v)) {
            kv_addc(k.b, "read failed", UI_RED);
            continue;
        }
        ub_hex(&u, v, 2);
        ub_s(&u, (v & 0xc0U) == 0xc0U ? "  ON   " : (v & 0xc0U) ? "  CTRL " : "  off  ");
        ub_s(&u, "vsel ");
        ub_dec(&u, v & 0x3fU);
        if (n == 35 || n == 43) {
            ub_s(&u, "  ");
            ub_fix(&u, (n == 35 ? 700U : 1800U) + (v & 0x3fU) * 25U, 3);
            ub_s(&u, " V");
        }
        kv_addc(k.b, u.b, (v & 0xc0U) == 0xc0U ? UI_TEXT : UI_TEXT2);
    }
}

void dump_pmic(void)
{
    struct ub u;
    tv_reset();
    tv_line("S2MPS18 PMIC block (ACPM type 1), registers 0x00-0xFF:");
    tv_line("");
    for (unsigned r = 0; r < 0x100; r += 16) {
        ub_init(&u);
        ub_hexn(&u, r, 2);
        ub_s(&u, ": ");
        for (unsigned c = 0; c < 16; ++c) {
            uint8_t v = 0;
            if (sb_pmic_read(1, r + c, &v))
                ub_hexn(&u, v, 2);
            else
                ub_s(&u, "??");
            ub_c(&u, c == 7 ? '-' : ' ');
        }
        tv_line(u.b);
    }
    tv_line("");
    tv_line("TOP block (ACPM type 0), registers 0x00-0x3F:");
    tv_line("");
    for (unsigned r = 0; r < 0x40; r += 16) {
        ub_init(&u);
        ub_hexn(&u, r, 2);
        ub_s(&u, ": ");
        for (unsigned c = 0; c < 16; ++c) {
            uint8_t v = 0;
            if (sb_pmic_read(0, r + c, &v))
                ub_hexn(&u, v, 2);
            else
                ub_s(&u, "??");
            ub_c(&u, c == 7 ? '-' : ' ');
        }
        tv_line(u.b);
    }
    text_screen("PMIC registers");
}

/* ---- touch rails: read here, switching them on is a tier 1 change (dry-run in v1) -------- */

static void fill_touch_rails(void)
{
    uint8_t l35 = 0, l43 = 0;
    int r = sb_touch_rails(&l35, &l43);
    struct ub u;
    kv_hdr("S2MPS18 (READ VIA ACPM)");
    if (r < 0) {
        kv_addc("ACPM", "PMIC channel did not answer", UI_RED);
        return;
    }
    ub_init(&u);
    ub_hex(&u, l35, 2);
    ub_s(&u, (l35 & 0xc0U) == 0xc0U ? "  ON  " : "  off  ");
    ub_fix(&u, 700U + (l35 & 0x3fU) * 25U, 3);
    ub_s(&u, " V");
    kv_addc("LDO35 tsp_io (1.8 V)", u.b, (l35 & 0xc0U) == 0xc0U ? UI_GREEN : UI_TEXT2);
    ub_init(&u);
    ub_hex(&u, l43, 2);
    ub_s(&u, (l43 & 0xc0U) == 0xc0U ? "  ON  " : "  off  ");
    ub_fix(&u, 1800U + (l43 & 0x3fU) * 25U, 3);
    ub_s(&u, " V");
    kv_addc("LDO43 tsp_avdd (3.0 V)", u.b, (l43 & 0xc0U) == 0xc0U ? UI_GREEN : UI_TEXT2);
    kv_addc("Kernel checks", r > 0 ? "both on, voltages as expected" : "off or unexpected voltage",
            r > 0 ? UI_GREEN : UI_ORANGE);
    kv_addc("Touch controller", sb_tp.ok ? "running" : sb_tp.why ? sb_tp.why : "not started",
            sb_tp.ok ? UI_GREEN : UI_ORANGE);
}

static int touch_rails_act(int i)
{
    uint8_t l35 = 0, l43 = 0;
    struct sb_auth au;
    const char *why;
    (void)i;
    (void)sb_touch_rails(&l35, &l43);
    struct ub o;
    ub_init(&o);
    ub_s(&o, "LDO35 ");
    ub_hex(&o, l35, 2);
    ub_s(&o, ", LDO43 ");
    ub_hex(&o, l43, 2);
    struct action a = { "Touch rails on", "TOUCH", { "Writes S2MPS18 LDO35 = 0xEC (on, 1.8 V) and LDO43 = 0xF0 (on, 3.0 V)",
                        "the kernel's and the loader's values; read back", 0, 0 },
                        "a reboot (the loader sets them again)", o.b, "LDO35 0xEC, LDO43 0xF0", "", TIER1, SB_SP_PMIC, 0,
                        0, 0x16cU, 9 };
    int t1 = pmic_tier(1, 0x6c, 0xec, &why), t2 = pmic_tier(1, 0x74, 0xf0, &why);
    a.tier = t1 > t2 ? t1 : t2;
    if (!lab_confirm(&a, &au))
        return 0;
    int ok = lab_pmic(1, 0x6c, 0xec, &au) && lab_pmic(1, 0x74, 0xf0, &au);
    if (ok && !sb_tp.ok)
        (void)sb_touch_start();
    ui_toast(ok ? "Rails on (read back)" : "Rail write failed", ok ? C_GREEN : C_RED, 1100);
    return 0;
}

static void build_touch_rails(void)
{
    fill_touch_rails();
    kv_hdr("CHANGE");
    row_add("Switch both rails on", "LDO35 1.8 V, LDO43 3.0 V: tier 1 (warning, slide)", R_WARN);
    kv_note("The loader's menu switches them on before Engineer OS starts; this is for a touch controller "
            "that did not come up.");
}

void touch_rails(void)
{
    list_screen("Touch rails", build_touch_rails, touch_rails_act);
}

/* ---- MAX77705 (libsb max77705.c, read only) ------------------------------------------------ */

#define BI_PMIC 0x66U
#define BI_FG 0x36U
#define BI_CHG 0x69U
#define BI_USBC 0x25U

static uint32_t rd(uintptr_t a)
{
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dmb oshld" : : : "memory");
    return v;
}

static void wr(uintptr_t a, uint32_t v)
{
    __asm__ volatile("dmb oshst" : : : "memory");
    *(volatile uint32_t *)a = v;
}

static int bi_read(unsigned addr, uint8_t reg, uint8_t *data, unsigned len)
{
    if (addr == BI_CHG && len == 1)
        return sb_chg_read(reg, data);
    if (addr == BI_USBC && len == 1)
        return sb_usbc_read(reg, data);
    return sb_max_init() && sb_i2c_read(&sb_max_bus, addr, reg, data, len);
}

static int fg_word(uint8_t reg, uint16_t *v)
{
    return sb_fg_word(reg, v);
}

/* mV for VCELL-type registers: 78.125 uV per bit */
static void ub_mv(struct ub *u, uint16_t raw)
{
    ub_fix(u, (uint64_t)raw * 5U / 64U, 3);
    ub_s(u, " V");
}

static void ub_pct(struct ub *u, uint16_t raw)
{
    ub_dec(u, raw >> 8);
    ub_c(u, '.');
    ub_dec(u, (raw & 0xffU) * 10U / 256U);
    ub_s(u, " %");
}

/* Current 0x0A/0x0B: 0.3125 mA per bit with the S9's 5 mOhm sense resistor */
static void ub_ma(struct ub *u, uint16_t raw)
{
    int32_t ma = (int32_t)(int16_t)raw * 5 / 16;
    if (ma > 0)
        ub_c(u, '+');
    ub_sdec(u, ma);
    ub_s(u, " mA");
}

/* Fuel-gauge temperature: 1/256 degC per bit, shown to 0.1 */
static void ub_t256(struct ub *u, int16_t raw)
{
    int32_t t = (int32_t)raw * 10 / 256;
    if (t < 0) {
        ub_c(u, '-');
        t = -t;
    }
    ub_dec(u, (uint64_t)t / 10U);
    ub_c(u, '.');
    ub_dec(u, (uint64_t)t % 10U);
    ub_s(u, " " UI_DEG "C");
}

static const char *chg_dtls(unsigned d)
{
    switch (d) {
    case 0x0: return "pre-charge";
    case 0x1: return "fast charge (CC)";
    case 0x2: return "fast charge (CV)";
    case 0x3: return "top-off";
    case 0x4: return "done (full)";
    case 0x5: return "timer fault";
    case 0x6: return "thermal suspend";
    case 0x7: return "off (input invalid)";
    case 0x8: return "off";
    case 0xa: return "reverse boost (OTG)";
    case 0xb: return "off (watchdog expired)";
    default: return "?";
    }
}

static const char *bat_dtls(unsigned d)
{
    switch (d) {
    case 0: return "no battery";
    case 1: return "below pre-charge threshold";
    case 2: return "dead (timer)";
    case 3: return "OK";
    case 4: return "low voltage";
    case 5: return "overvoltage";
    default: return "?";
    }
}

void fill_battery(void)
{
    struct ub u;
    uint16_t v = 0;
    uint8_t c = 0;
    uint8_t bi_id = 0, bi_rev = 0;
    if (sb_max_bus.state < 0)
        sb_max_bus.state = 0;                          /* retry each time the page refreshes */
    if (!sb_max_init()) {
        kv_hdr("MAX77705");
        kv_addc("Bus", sb_max_why ? sb_max_why : "no answer on HSI2C 0x14360000", UI_RED);
        kv_hex("TIMING_FS3 (S-Boot)", sb_max_bus.fs3, 8);
        kv_hex("CMU_CMGP gate 0x14202014", rd(0x14202014UL), 8);
        return;
    }
    (void)sb_max_id(&bi_id, &bi_rev);
    if (fg_word(0x06, &v)) {
        ub_init(&u);
        ub_dec(&u, v >> 8);
        ub_s(&u, " %");
        kv_big("battery (RepSOC)", u.b);
    }
    kv_hdr("FUEL GAUGE (0x36)");
#define FG_ROW(reg, label, fmt)                                     \
    do {                                                            \
        ub_init(&u);                                                \
        if (fg_word(reg, &v)) {                                     \
            fmt;                                                    \
            kv_add(label, u.b);                                     \
        } else {                                                    \
            kv_addc(label, "read failed", UI_RED);                  \
        }                                                           \
    } while (0)
    FG_ROW(0x06, "State of charge (RepSOC)", ub_pct(&u, v));
    FG_ROW(0xff, "State of charge (VFSOC)", ub_pct(&u, v));
    FG_ROW(0x09, "Cell voltage", ub_mv(&u, v));
    FG_ROW(0x19, "Average cell voltage", ub_mv(&u, v));
    FG_ROW(0xfb, "Open-circuit voltage (VFOCV)", ub_mv(&u, v));
    FG_ROW(0x0a, "Current (+ = charging)", ub_ma(&u, v));
    FG_ROW(0x0b, "Average current", ub_ma(&u, v));
    FG_ROW(0x05, "Remaining capacity", (ub_dec(&u, v), ub_s(&u, " mAh")));
    FG_ROW(0x35, "Full capacity (learned)", (ub_dec(&u, v), ub_s(&u, " mAh")));
    FG_ROW(0x18, "Design capacity", (ub_dec(&u, v), ub_s(&u, v == 0x0b97 ? " mAh (model loaded)" : " mAh")));
    FG_ROW(0x17, "Cycles (raw)", ub_dec(&u, v));
    FG_ROW(0x08, "Temperature (last host write)", ub_t256(&u, (int16_t)v));
    FG_ROW(0x00, "Status", (ub_hex(&u, v, 4), ub_s(&u, (v & 8U) ? "  battery ABSENT" : "  battery present")));
#undef FG_ROW

    kv_hdr("CHARGER (0x69)");
    if (bi_read(BI_CHG, 0xb2, &c, 1)) {
        kv_addc("Input", (c & 0x20U) ? "wireless" : (c & 0x40U) ? "USB cable" : "none (on battery)",
                (c & 0x60U) ? UI_GREEN : UI_TEXT);
        kv_hex("CHG_INT_OK", c, 2);
    }
    if (bi_read(BI_CHG, 0xb3, &c, 1)) {
        unsigned d = (c >> 5) & 3U;
        kv_add("VBUS", d == 3 ? "valid" : d == 2 ? "over voltage" : d == 1 ? "low (below VBAT + margin)" : "none / UVLO");
    }
    if (bi_read(BI_CHG, 0xb4, &c, 1)) {
        unsigned d = c & 0xfU;
        kv_addc("Charger state", chg_dtls(d), d <= 2 ? UI_GREEN : d <= 4 ? UI_HOLO : d == 0xb ? UI_ORANGE : UI_TEXT);
        kv_add("Battery state", bat_dtls((c >> 4) & 7U));
    }
    if (bi_read(BI_CHG, 0xb9, &c, 1)) {
        ub_init(&u);
        ub_dec(&u, (c & 0x3fU) <= 2 ? 100U : (c & 0x3fU) * 50U);
        ub_s(&u, " mA");
        kv_add("Charge current limit", u.b);
    }
    if (bi_read(BI_CHG, 0xc0, &c, 1)) {
        unsigned r = c & 0x7fU;
        ub_init(&u);
        ub_dec(&u, r <= 3 ? 100U : r >= 0x7f ? 3200U : (r + 1U) * 25U);
        ub_s(&u, " mA");
        kv_add("Input current limit", u.b);
    }
    if (bi_read(BI_CHG, 0xb7, &c, 1)) {
        ub_init(&u);
        ub_hex(&u, c, 2);
        ub_s(&u, (c & 1U) ? "  charge" : "");
        ub_s(&u, (c & 4U) ? "  buck" : "");
        ub_s(&u, (c & 0x10U) ? "  watchdog on" : "");
        kv_add("CNFG_00 mode", u.b);
    }
    kv_hdr("CHIP");
    ub_init(&u);
    ub_s(&u, "MAX77705  id ");
    ub_hex(&u, bi_id, 2);
    ub_s(&u, "  rev ");
    ub_dec(&u, bi_rev & 7U);
    ub_s(&u, " ver ");
    ub_dec(&u, bi_rev >> 3);
    kv_add("PMIC", u.b);
    if (bi_read(BI_USBC, 0x08, &c, 1)) {
        unsigned t = c & 3U;
        kv_add("USB charger type", t == 1 ? "SDP (computer port)" : t == 2 ? "CDP" : t == 3 ? "DCP (wall charger)" : "none");
    }
    kv_addc("Note", "charger watchdog is not fed here: charging may pause", UI_TEXT3);
}

/* ---- TMU (temperatures), read only: build/em-research-thermal-clocks.md -- */

#define TMU_TOP 0x10070000UL
#define TMU_SUB 0x10074000UL

static const unsigned char big_probe[5] = { 1, 2, 3, 4, 6 };   /* DT mask 0x5E */

/* Probe p of a running TMU in degrees C; *flags |= 1 if uncalibrated. */
static int tmu_temp(uintptr_t base, unsigned p, unsigned *flags)
{
    uint32_t tri = rd(base + 4U * p);
    int te1 = (int)(tri & 0x1ffU), te2 = (int)((tri >> 9) & 0x1ffU);
    int two = (int)((rd(base) >> 23) & 1U);
    if (!te1) {
        te1 = 55;
        *flags |= 1U;
    }
    uintptr_t off = p < 2 ? 0x40U : 0x44U + 4U * ((p - 2U) / 3U);
    unsigned sh = p < 2 ? 9U * p : 9U * ((p - 2U) % 3U);
    int code = (int)((rd(base + off) >> sh) & 0x1ffU);
    return (two && te2 > te1) ? (code - te1) * 60 / (te2 - te1) + 25 : code - te1 + 25;
}

static uint32_t temp_col(int t)
{
    return t >= 85 ? UI_RED : t >= 65 ? UI_ORANGE : UI_TEXT;
}

static void kv_temp(const char *k, int t, unsigned flags)
{
    struct ub u;
    ub_init(&u);
    ub_sdec(&u, t);
    ub_s(&u, " " UI_DEG "C");
    if (flags & 1U)
        ub_s(&u, "  (uncalibrated)");
    kv_addc(k, u.b, temp_col(t));
}

/* 0 off, 1 running, 2 emulated */
static int tmu_state(uintptr_t base)
{
    if (!(rd(base + 0x20) & 1U))
        return 0;
    return (rd(base + 0x160) & 1U) ? 2 : 1;
}

void fill_thermal(void)
{
    int top = tmu_state(TMU_TOP), sub = tmu_state(TMU_SUB), hot = -100;
    unsigned fl = 0;
    int big[5], little = 0, gpu = 0, isp = 0;
    if (top) {
        for (unsigned i = 0; i < 5; ++i) {
            big[i] = tmu_temp(TMU_TOP, big_probe[i], &fl);
            if (big[i] > hot)
                hot = big[i];
        }
        little = tmu_temp(TMU_TOP, 5, &fl);
        if (little > hot)
            hot = little;
    }
    if (sub) {
        gpu = tmu_temp(TMU_SUB, 1, &fl);
        isp = tmu_temp(TMU_SUB, 2, &fl);
        if (gpu > hot)
            hot = gpu;
        if (isp > hot)
            hot = isp;
    }
    if (hot > -100) {
        struct ub u;
        ub_init(&u);
        ub_sdec(&u, hot);
        ub_s(&u, UI_DEG "C");
        kv_bigc("hottest sensor", u.b, temp_col(hot));
    }
    kv_hdr("TMU_TOP 0x10070000");
    if (!top) {
        kv_addc("State", "off (not enabled by S-Boot)", UI_TEXT3);
    } else {
        if (top == 2)
            kv_addc("State", "EMULATION ON: values are fake", UI_ORANGE);
        int bmax = big[0];
        for (unsigned i = 1; i < 5; ++i)
            if (big[i] > bmax)
                bmax = big[i];
        kv_temp("Big cluster (Mongoose M3), max", bmax, fl);
        for (unsigned i = 0; i < 5; ++i) {
            struct ub k;
            ub_init(&k);
            ub_s(&k, "  M3 probe ");
            ub_dec(&k, big_probe[i]);
            kv_temp(k.b, big[i], 0);
        }
        kv_temp("Little cluster (Cortex-A55)", little, 0);
        kv_temp("Probe 0 (unassigned)", tmu_temp(TMU_TOP, 0, &fl), 0);
        kv_temp("Probe 7 (S-Boot log sensor)", tmu_temp(TMU_TOP, 7, &fl), 0);
        kv_add("Hardware trip", (rd(TMU_TOP + 0x20) & 0x1000U) ? "armed (115 " UI_DEG "C, S-Boot)" : "not armed");
    }
    kv_hdr("TMU_SUB 0x10074000");
    if (!sub) {
        kv_addc("State", "off (not enabled by S-Boot)", UI_TEXT3);
    } else {
        if (sub == 2)
            kv_addc("State", "EMULATION ON: values are fake", UI_ORANGE);
        kv_temp("GPU (Mali-G72)", gpu, 0);
        kv_temp("ISP (camera)", isp, 0);
        kv_temp("Probe 0 (unassigned)", tmu_temp(TMU_SUB, 0, &fl), 0);
        kv_add("Hardware trip", (rd(TMU_SUB + 0x20) & 0x1000U) ? "armed" : "not armed");
    }
    kv_hdr("CALIBRATION");
    kv_add("Mode", ((rd(TMU_TOP) >> 23) & 1U) ? "two-point (25/85 " UI_DEG "C eFuse)" : "one-point");
    kv_add("Source", "direct TMU read (APM reads the same)");
}

/* ---- clocks, read only ----------------------------------------------------- */

/* PLL_CON0: [31] enable, [29] locked, [25:16] M, [13:8] P, [4] mux (0 = 26 MHz), [2:0] S */
static uint64_t pll_hz(uintptr_t con0, int *unlocked)
{
    uint32_t v = rd(con0);
    if (!(v & 0x10U))
        return 26000000U;
    if (!(v & 0x80000000U))
        return 0;
    if (unlocked && !(v & 0x20000000U))
        *unlocked = 1;
    uint64_t m = (v >> 16) & 0x3ffU, p = (v >> 8) & 0x3fU, s = v & 7U;
    return p ? 26000000ULL * m / (p << s) : 0;
}

/* CPU cluster switch path through CMU_TOP */
static uint64_t top_switch(uintptr_t user, uintptr_t mux, uintptr_t div)
{
    uint64_t src;
    if (!((rd(user) >> 4) & 1U))
        return 26000000U;
    switch (rd(mux) & 3U) {
    case 0: src = pll_hz(0x1a240120UL, 0) / ((rd(0x1a2418d4UL) & 1U) + 1U); break;
    case 1: src = pll_hz(0x1a2401a0UL, 0) / ((rd(0x1a2418e0UL) & 1U) + 1U); break;
    case 2: src = pll_hz(0x1a240220UL, 0); break;
    default: src = pll_hz(0x1a240320UL, 0); break;
    }
    return src / ((rd(div) & 7U) + 1U);
}

static void kv_mhz(const char *k, uint64_t hz, const char *note)
{
    struct ub u;
    ub_init(&u);
    ub_fix(&u, hz / 1000U, 1);
    ub_s(&u, " MHz");
    if (note)
        ub_s(&u, note);
    kv_add(k, u.b);
}

/* ---- Mali-G72 (read only): the probe the iOS GPU driver starts from ---------
 * gpu@17500000 (job/mmu/gpu SPI 282/281/283), G3D domain PMU 0x14064040/44,
 * CMU_G3D 0x17400000, vdd_g3d = S2MPS18 BUCK6 (reg 0x2b, 300 mV + n x 6.25).
 * Registers are read only while the domain reports on: a powered-off block
 * stalls the bus (and EngineerMode's vectors catch a synchronous abort).
 */
#define MALI 0x17500000UL

static void kv_mali_hex(const char *k, uint32_t off)
{
    uint32_t v = 0;
    if (probe32(MALI + off, &v))
        kv_hex(k, v, 8);
    else
        kv_trap(k);
}

void fill_gpu(void)
{
    struct ub u;
    uint32_t st = rd(0x14064044UL);
    uint8_t b6 = 0;
    kv_hdr("POWER (LIVE)");
    kv_hex("G3D domain status", st, 8);
    kv_addc("Domain", (st & 0xfU) == 0xfU ? "on (left on by S-Boot)" : "OFF", (st & 0xfU) == 0xfU ? UI_GREEN : UI_ORANGE);
    if (sb_pmic_read(1, 0x2b, &b6)) {
        ub_init(&u);
        ub_fix(&u, 300000U + b6 * 6250U, 3);
        ub_s(&u, " mV  (vsel ");
        ub_hex(&u, b6, 2);
        ub_s(&u, b6 >= 0x68 ? ")" : ", jobs need 950 mV)");
        kv_addc("BUCK6 vdd_g3d", u.b, b6 >= 0x68 ? UI_TEXT : UI_ORANGE);
    }
    if ((st & 0xfU) != 0xfU) {
        kv_addc("GPU registers", "not read: domain off", UI_TEXT3);
        return;
    }
    uint32_t con0 = rd(0x17400140UL);
    kv_hex("PLL_G3D CON0", con0, 8);
    kv_mhz("PLL_G3D", pll_hz(0x17400140UL, 0), (con0 & 0x20000000U) ? "  locked" : "  not stable");
    kv_hdr("MALI-G72 0x17500000 (LIVE)");
    uint32_t id = 0;
    if (probe32(MALI + 0x000, &id)) {
        ub_init(&u);
        ub_hex(&u, id, 8);
        ub_s(&u, (id >> 16) == 0x6221U ? "  Mali-G72" : "  unexpected");
        ub_s(&u, " r");
        ub_dec(&u, (id >> 12) & 0xfU);
        ub_c(&u, 'p');
        ub_dec(&u, (id >> 4) & 0xfU);
        kv_addc("GPU_ID", u.b, (id >> 16) == 0x6221U ? UI_GREEN : UI_ORANGE);
    } else {
        kv_trap("GPU_ID");
        return;
    }
    kv_mali_hex("L2_FEATURES", 0x004);
    kv_mali_hex("CORE_FEATURES", 0x008);
    kv_mali_hex("TILER_FEATURES", 0x00c);
    kv_mali_hex("MEM_FEATURES", 0x010);
    kv_mali_hex("MMU_FEATURES", 0x014);
    kv_mali_hex("AS_PRESENT", 0x018);
    kv_mali_hex("JS_PRESENT", 0x01c);
    kv_mali_hex("GPU_STATUS", 0x034);
    uint32_t sh = 0, rdy = 0;
    if (probe32(MALI + 0x100, &sh)) {
        unsigned n = 0;
        for (uint32_t x = sh; x; x &= x - 1)
            ++n;
        ub_init(&u);
        ub_hex(&u, sh, 8);
        ub_s(&u, "  ");
        ub_dec(&u, n);
        ub_s(&u, " shader cores");
        kv_add("SHADER_PRESENT", u.b);
    }
    kv_mali_hex("TILER_PRESENT", 0x110);
    kv_mali_hex("L2_PRESENT", 0x120);
    if (probe32(MALI + 0x140, &rdy))
        kv_addc("SHADER_READY", rdy ? "powered" : "off (powered on by a driver)", rdy ? UI_GREEN : UI_TEXT2);
    kv_mali_hex("L2_READY", 0x160);
    kv_hdr("FOR THE IOS DRIVER");
    kv_add("Interrupts", "job SPI 282, mmu 281, gpu 283");
    kv_add("Before jobs", "BUCK6 950 mV, PLL_G3D relock");
    kv_add("After a power cycle", "SMC 0x82000410 (TZPC restore)");
}

void fill_clocks(void)
{
    int ul = 0;
    uint64_t a55src = (rd(0x1d00100cUL) & 1U) ? top_switch(0x1d000120UL, 0x1a241034UL, 0x1a241830UL)
                                              : pll_hz(0x1d000140UL, &ul);
    uint64_t a55 = a55src / ((rd(0x1d001818UL) & 0xfffU) + 1U);
    struct ub u;
    ub_init(&u);
    ub_dec(&u, a55 / 1000000U);
    ub_s(&u, " MHz");
    kv_big("Cortex-A55 (this core)", u.b);

    kv_hdr("CPU CLUSTERS");
    kv_mhz("A55 cluster", a55, (rd(0x1d00100cUL) & 1U) ? "  (switch path)" : ul ? "  (PLL unlocked)" : 0);
    if ((rd(0x14062044UL) & 0xfU) == 0xfU) {
        int ul1 = 0;
        uint64_t src = (rd(0x1d101000UL) & 1U) ? top_switch(0x1d100100UL, 0x1a241038UL, 0x1a241834UL)
                                               : pll_hz(0x1d100120UL, &ul1);
        kv_mhz("M3 cluster", src / ((rd(0x1d10180cUL) & 0xfffU) + 1U), ul1 ? "  (PLL unlocked)" : 0);
    } else {
        kv_addc("M3 cluster", "powered off", UI_TEXT3);
    }
    kv_hdr("GPU AND MEMORY");
    if ((rd(0x14064044UL) & 0xfU) == 0xfU)
        kv_mhz("GPU (PLL_G3D)", pll_hz(0x17400140UL, 0), (rd(0x17401000UL) & 1U) ? "  (bus on switch)" : 0);
    else
        kv_addc("GPU (PLL_G3D)", "powered off", UI_TEXT3);
    if (rd(0x1b801000UL) & 1U)
        kv_add("PLL_MIF (DDR PHY 2x)", "on switch path");
    else
        kv_mhz("PLL_MIF (DDR PHY 2x)", pll_hz(0x1b800120UL, 0), 0);
    kv_hdr("SHARED BUS PLLS (CMU_TOP)");
    kv_mhz("PLL_SHARED0", pll_hz(0x1a240120UL, 0), 0);
    kv_mhz("PLL_SHARED1", pll_hz(0x1a2401a0UL, 0), 0);
    kv_mhz("PLL_SHARED2", pll_hz(0x1a240220UL, 0), 0);
    kv_mhz("PLL_SHARED3", pll_hz(0x1a2402a0UL, 0), 0);
    kv_mhz("PLL_SHARED4", pll_hz(0x1a240320UL, 0), 0);
    kv_hdr("RAW PLL_CON0");
    kv_hex("PLL_CPUCL0", rd(0x1d000140UL), 8);
    kv_hex("PLL_MIF", rd(0x1b800120UL), 8);
    kv_hex("PLL_SHARED0", rd(0x1a240120UL), 8);
    kv_addc("Owner", "APM firmware (DVFS); read only here", UI_TEXT3);
}

/* ======================================================================= */
/* STORAGE                                                                    */
/* ======================================================================= */

void fill_ufs(void)
{
    volatile uint32_t *h = (volatile uint32_t *)0x11120000UL;
    uint32_t cap = h[0x00 / 4], ver = h[0x08 / 4], hcs = h[0x30 / 4], is = h[0x20 / 4];
    struct ub u;
    kv_hdr("UFS HOST CONTROLLER 0x11120000 (LIVE)");
    kv_hex("CAP", cap, 8);
    ub_init(&u);
    ub_dec(&u, (cap & 0x1fU) + 1);
    ub_s(&u, " transfer slots, ");
    ub_dec(&u, ((cap >> 16) & 7U) + 1);
    ub_s(&u, " TM slots");
    kv_add("Slots", u.b);
    kv_add("64-bit DMA", (cap & (1U << 24)) ? "yes" : "no");
    ub_init(&u);
    ub_s(&u, "UFSHCI ");
    ub_dec(&u, (ver >> 8) & 0xfU);
    ub_c(&u, '.');
    ub_dec(&u, (ver >> 4) & 0xfU);
    kv_add("Version", u.b);
    kv_hex("HCS", hcs, 8);
    ub_init(&u);
    ub_s(&u, (hcs & 1U) ? "device present " : "no device ");
    ub_s(&u, (hcs & 2U) ? "UTRL ready" : "UTRL not ready");
    kv_add("Status", u.b);
    kv_hex("IS", is, 8);
    kv_hex("UTRLBA", ((uint64_t)h[0x54 / 4] << 32) | h[0x50 / 4], 9);
    kv_hex("UTRLDBR", h[0x58 / 4], 8);
    kv_hdr("EXYNOS VS_HCI 0x11121100");
    kv_hex("TXPRDT_ENTRY_SIZE", h[0x1100 / 4], 8);
    kv_hex("RXPRDT_ENTRY_SIZE", h[0x1104 / 4], 8);
    kv_hex("UTRL_NEXUS_TYPE", h[0x1140 / 4], 8);
    kv_hex("DATA_REORDER", h[0x1160 / 4], 8);
    kv_hex("AXIDMA_RWDATA_BURST", h[0x116c / 4], 8);
    kv_hdr("BOOT-TIME FACTS");
    kv_add("iOS driver", "multi-block, 16-byte PRDT, 64 KiB");
    kv_add("Windows driver", "S9 write fence (WINESP+WINDOWS)");
    kv_add("Device descriptor", "coming: needs a UFS driver here");
}


/* ======================================================================= */
/* OS                                                                         */
/* ======================================================================= */

static const char *rwd1_state(uint32_t s)
{
    switch (s) {
    case 0x00: return "empty";
    case 0x10: return "boot shim armed";
    case 0x20: return "SEC";
    case 0x30: return "DXE";
    case 0x40: return "BDS";
    case 0x50: return "ExitBootServices";
    case 0x60: return "Windows owned";
    case 0x70: return "P3 post-handoff";
    case 0x80: return "shutdown pending";
    case 0x90: return "controlled stop";
    case 0xa0: return "recovery pending";
    case 0xb0: return "TWRP ack";
    case 0xe0: return "fatal: watchdog not petted";
    default: return "?";
    }
}

void fill_windows(void)
{
    struct ub u;
    struct store_ent se;
    kv_hdr("FIRMWARE (UEFI IMAGE)");
    if (store_find("windows.uefi", &se)) {
        ub_init(&u);
        ub_size(&u, se.size);
        ub_s(&u, se.medium == STORE_SD ? "  OS store (microSD)" : "  OS store (UFS)");
        kv_add("windows.uefi", u.b);
    } else {
        kv_addc("windows.uefi", "not in the OS store", UI_RED);
    }
    kv_addc("BOOT bundle", "in BOOT: only the loader sees it (Engineer Lite)", UI_TEXT3);
    kv_add("Build", "src38: Starbite SEC/PEI + woa DXE");
    kv_add("RAM", "stage 3: 3.4 GB (BS-data banks)");
    kv_add("Clock", "PMIC RTC seed (patch 0023)");
    kv_add("Watchdog handshake", "RWD1 SEC_ACTIVE + P3 clear");
    kv_add("UFS driver", "9.0.0.1 + S9 write fence (re-signed)");
    kv_add("Edition", "Windows 11 Pro 23H2 ARM64");

    volatile uint32_t *r = (volatile uint32_t *)0xFED13D80UL;
    volatile uint32_t *p3 = (volatile uint32_t *)0xFED13E80UL;
    volatile uint32_t *w = (volatile uint32_t *)0xFED17F70UL;
    volatile uint32_t *nrm = (volatile uint32_t *)0xFED17F18UL;
    kv_hdr("LAST RUN (RETAINED RECORDS)");
    if (r[0] == 0x31445752U) {
        ub_init(&u);
        ub_s(&u, rwd1_state(r[4]));
        kv_add("RWD1 state", u.b);
        kv_dec("RWD1 owner", r[5], r[5] == 6 ? "  (Windows UFS)" : 0);
        kv_dec("RWD1 phase", r[6], r[6] == 10 ? "  (Windows running)" : 0);
        kv_dec("RWD1 reason", r[7], 0);
        kv_dec("Watchdog heartbeats", r[10], 0);
    } else {
        kv_addc("RWD1", "no record (cold reset clears it)", UI_TEXT3);
    }
    if (p3[0] == 0x31504d53U)
        kv_add("P3 (SMP1)", (p3[5] & 0xffU) == 0x70U ? "acknowledged by Windows" :
                            (p3[5] & 0xffU) == 0x60U ? "pre-branch (not acked)" : "other");
    else
        kv_addc("P3 (SMP1)", "no record", UI_TEXT3);
    if (w[0] == 0x31525757U) {
        ub_init(&u);
        ub_dec(&u, w[1]);
        ub_s(&u, " CPUs, ");
        ub_dec(&u, ((uint64_t)w[5] << 32 | w[4]) * 4096U >> 20);
        ub_s(&u, " MiB, ");
        ub_dec(&u, w[6]);
        ub_s(&u, " ranges");
        kv_add("Windows saw", u.b);
    } else {
        kv_addc("Windows witness", "no record", UI_TEXT3);
    }
    if (nrm[0] == 0x314d524eU)
        kv_add("UFS shutdown record", ((nrm[1] >> 16) == 2) ? "orderly stop" : "init only (no orderly stop)");
    kv_hex("PMU RST_STAT", *(volatile uint32_t *)(SB_PMU + 0x404), 8);
}

static const char *method_name(unsigned m)
{
    switch (m) {
    case M_XNU: return "xnu";
    case M_LINUX: return "linux";
    case M_ANDROID: return "android";
    case M_UEFI: return "uefi";
    case M_BARE: return "bare";
    default: return "?";
    }
}

static void kv_store(const char *prefix);

/* The Apple entries of the catalog (iOS 16 via its UEFI, iOS 26 and macOS on XNU): what they boot
 * and with which command line. BOOT's own iOS payload and its boot-args live in the loader's RAM
 * (Engineer Lite's "iOS boot options"). */
void fill_ios(void)
{
    struct ub u;
    int any = 0;
    for (unsigned i = 0; i < cat_count(); ++i) {
        const struct cat_ent *c = cat_get(i);
        if (c->category != CAT_APPLE)
            continue;
        const char *why;
        int ok = star_entry_ready(i, &why);
        ub_init(&u);
        ub_s(&u, c->name);
        kv_hdr(u.b);
        kv_add("Key", c->key);
        kv_add("Version", c->version);
        kv_add("Method", method_name(c->method));
        kv_add("Files", c->file[0]);
        if (c->root[0])
            kv_add("Root", c->root);
        if (c->cmdline[0])
            kv_add("Command line (appended)", c->cmdline);
        kv_addc("State", ok ? "ready" : why, ok ? UI_GREEN : UI_RED);
        if (c->flags & CF_HIDDEN)
            kv_addc("Menu", "hidden (Start an OS only)", UI_TEXT3);
        any = 1;
    }
    if (!any)
        kv_addc("Catalog", "no Apple entries", UI_TEXT3);
    kv_hdr("OS STORE: XNU PAYLOADS AND IMAGES (LIVE)");
    kv_store("ios");
    kv_store("mac");
    kv_note("BOOT's own iOS payload and its boot-args are in the loader's RAM: edit them in Engineer Lite "
            "(hold Bixby in the menu).");
}

/* The iOS boot options as the catalog's command lines carry them, decoded (read only: changing
 * one is a catalog write, EN4). Each row starts that entry through the boot-next mailbox. */
static int opt_num(const char *c, const char *key, int dflt)
{
    unsigned kl = sb_strlen(key);
    for (const char *p = c; *p;) {
        while (*p == ' ')
            ++p;
        unsigned n = 0;
        while (p[n] && p[n] != ' ')
            ++n;
        if (n > kl && p[kl] == '=' && !memcmp(p, key, kl)) {
            int v = 0;
            for (unsigned i = kl + 1; i < n && p[i] >= '0' && p[i] <= '9'; ++i)
                v = v * 10 + (p[i] - '0');
            return v;
        }
        if (n == kl && !memcmp(p, key, kl))
            return 1;
        p += n;
    }
    return dflt;
}

static int ios_rows[CAT_MAX];
static int ios_nrows;

static void build_ios_opts(void)
{
    ios_nrows = 0;
    for (unsigned i = 0; i < cat_count(); ++i) {
        const struct cat_ent *c = cat_get(i);
        if (c->method != M_XNU)
            continue;
        struct ub u;
        kv_hdr(c->name);
        ub_init(&u);
        ub_dec(&u, (uint64_t)opt_num(c->cmdline, "cpus", 1));
        kv_add("CPU cores (cpus)", u.b);
        kv_add("Root read-write (ufsrw / sdrw)", opt_num(c->cmdline, "ufsrw", 0) || opt_num(c->cmdline, "sdrw", 0) ? "on" : "off");
        kv_add("Verbose boot (-v)", opt_num(c->cmdline, "-v", 0) ? "on" : "off");
        kv_add("Touchscreen (startouch)", opt_num(c->cmdline, "startouch", 1) ? "on" : "off");
        kv_add("UFS multi-block (ufsmulti)", opt_num(c->cmdline, "ufsmulti", 0) ? "on" : "off");
        kv_add("CPU core probe (starsmpprobe)", opt_num(c->cmdline, "starsmpprobe", 0) ? "ON (parks cores)" : "off");
        if (c->cmdline[0])
            kv_mono(c->cmdline);
        const char *why;
        int ok = star_entry_ready(i, &why);
        ub_init(&u);
        ub_s(&u, "Start ");
        ub_s(&u, c->name);
        row_add(u.b, ok ? "reboots; the loader starts it (boot-next)" : why, ok ? 0 : R_DIS);
        if (ios_nrows < (int)CAT_MAX)
            ios_rows[ios_nrows++] = (int)i;
    }
    kv_note("These are the catalog's command lines (appended to each payload's own). Changing one is a "
            "catalog write: not in v1.");
}

static int act_ios_opts(int i)
{
    if (i < 0 || i >= ios_nrows)
        return 0;
    const struct cat_ent *c = cat_get((unsigned)ios_rows[i]);
    if (c && dialog("Start", c->name, "Reboots; the loader starts it once (boot-next).", "START"))
        sb_boot_next(c->key);
    return 0;
}

void ios_opts_page(void)
{
    list_screen("iOS boot options", build_ios_opts, act_ios_opts);
}

static void fs_kind(uint64_t lba, struct ub *u);   /* STORAGE section */

/* Store entries whose name starts with prefix, as rows "name  size  crc", both media. */
static void kv_store(const char *prefix)
{
    struct store_ent e;
    int shown = 0, any = 0;
    for (unsigned m = 0; m < STORE_MEDIA; ++m) {
        any |= store_open(m) > 0;
        for (unsigned i = 0; store_get(m, i, &e); ++i) {
            unsigned k = 0;
            while (prefix[k] && e.name[k] == prefix[k])
                ++k;
            if (prefix[k])
                continue;
            struct ub v;
            ub_init(&v);
            ub_size(&v, e.size);
            ub_s(&v, "  crc ");
            ub_hexn(&v, e.crc, 8);
            if (m == STORE_SD)
                ub_s(&v, "  SD");
            kv_add(e.name, v.b);
            ++shown;
        }
    }
    if (!shown)
        kv_addc("OS store", any ? "no entries for this OS" : "no store found", UI_RED);
}

/* Partition row: size and the filesystem found on it, live. */
static void kv_part(const char *label, const char *gpt, const char *role)
{
    uint64_t first, last;
    struct ub v;
    ub_init(&v);
    if (!gpt_find(gpt, &first, &last)) {
        kv_addc(label, "no such partition", UI_RED);
        return;
    }
    ub_size(&v, (last - first + 1) * 4096U);
    ub_s(&v, "  ");
    fs_kind(first, &v);
    if (role) {
        ub_s(&v, "  ");
        ub_s(&v, role);
    }
    kv_add(label, v.b);
}

void fill_android(void)
{
    kv_hdr("ANDROID 17 " UI_DOT " LINEAGEOS 24.0");
    kv_add("Build", "lineage-24.0-20261025 unofficial (krazey90)");
    kv_add("Kernel", "Samsung Linux 4.9.337, first-stage init");
    kv_add("Device tree", "LineageOS DTBH in BOOT, S-Boot fixups");
    kv_add("Boot path", "loader: kernel + ramdisk from the OS store");
    kv_hdr("OS STORE (LIVE)");
    kv_store("android.");
    kv_hdr("PARTITIONS (LIVE)");
    kv_part("SYSTEM", "SYSTEM", "/system");
    kv_part("VENDOR", "VENDOR", "/vendor");
    kv_part("ODM", "ODM", "/metadata");
    kv_part("A16DATA", "A16DATA", "/data");
    kv_hdr("PROTECTIONS");
    kv_addc("/data", "A16DATA (vendor + ramdisk fstab, sepolicy)", UI_GREEN);
    kv_addc("TWRP /data", "A16DATA: recovery wipes cannot reach USERDATA", UI_GREEN);
    kv_addc("Updater", "removed: its OTA zips would write BOOT", UI_GREEN);
    kv_add("USERDATA", "never mounted by Android");
}

void fill_linux(void)
{
    kv_hdr("LINUX 7.1 " UI_DOT " POSTMARKETOS (ALPINE EDGE)");
    kv_add("Kernel", "mainline 7.1 + Exynos 9810 port drivers");
    kv_add("Desktop", "Plasma Mobile, OpenRC");
    kv_add("Root", "microSD: /starbite-linux.img (ext4, 8 GiB)");
    kv_add("GPU", "Mali-G72 MC18 via Panfrost (Mesa)");
    kv_add("Display", "s9p-decon KMS, 60 Hz");
    kv_add("Wi-Fi", "BCM4361 via brcmfmac (PCIe)");
    kv_add("CPUs", "8 cores: 4x Cortex-A55 + 4x Mongoose M3");
    kv_add("USB", "network 172.16.42.1: SSH (telnet in rescue)");
    kv_add("Rescue", "hold Vol down while Linux starts");
    kv_add("Internal UFS", "read-only under Linux");
    kv_hdr("OS STORE (LIVE)");
    kv_store("linux.");
}

static void fill_store_medium(unsigned m)
{
    struct store_ent e;
    struct ub v;
    kv_hdr(m == STORE_UFS ? "UFS " UI_DOT " USERDATA +16 GIB (LIVE)" : "MICROSD " UI_DOT " PARTITION 2 (LIVE)");
    int n = store_open(m);
    if (!store_version(m)) {
        kv_addc("Store", "not found", UI_RED);
        return;
    }
    kv_add("Format", store_version(m) == 2 ? "SBSTORE2" : "SBSTORE1 (old)");
    kv_dec("First block", store_base(m), 0);
    kv_dec("Entries", (uint64_t)n, 0);
    ub_init(&v);
    ub_size(&v, store_used(m) * 4096U);
    ub_s(&v, " of ");
    ub_size(&v, store_capacity(m) * 4096U);
    kv_add("Used", v.b);
    for (unsigned i = 0; store_get(m, i, &e); ++i) {
        ub_init(&v);
        ub_size(&v, e.size);
        ub_s(&v, "  crc ");
        ub_hexn(&v, e.crc, 8);
        kv_add(e.name, v.b);
    }
}

void fill_store(void)
{
    struct star_settings st;
    struct ub v;
    fill_store_medium(STORE_UFS);
    fill_store_medium(STORE_SD);
    kv_hdr("CATALOG");
    kv_add("Source", cat_builtin() ? "built in (no catalog in the store)" : "OS store");
    for (unsigned i = 0; i < cat_count(); ++i) {
        const struct cat_ent *c = cat_get(i);
        const char *why;
        ub_init(&v);
        ub_s(&v, c->key);
        ub_s(&v, "  ");
        ub_s(&v, star_entry_ready(i, &why) ? "ready" : why);
        kv_addc(c->name, v.b, star_entry_ready(i, 0) ? UI_TEXT : UI_RED);
    }
    kv_hdr("SETTINGS");
    if (!settings_load(&st)) {
        kv_addc("Record", "none (defaults)", UI_RED);
        return;
    }
    kv_add("Last choice", st.last_key[0] ? st.last_key : "-");
    kv_add("Default", st.default_key[0] ? st.default_key : "-");
    kv_dec("Timeout", st.timeout_s, " s (0 = wait)");
    kv_add("Next (one shot)", st.next_key[0] ? st.next_key : "-");
    kv_dec("Writes", st.seq, 0);
}

/* ---- microSD (sd.c) ---------------------------------------------------------
 * The card driver runs only from here until "Card store at boot" is on: a probe with the fault
 * vectors armed, every step shown. */
static void kv_ok(const char *k, int ok, const char *v)
{
    kv_addc(k, v, ok ? UI_GREEN : UI_RED);
}

static int sd_probed;
static uint64_t sd_probe_faults;

void fill_sd(void)
{
    struct ub v;
    if (!sd_probed) {
        uint64_t f0 = sb_fault.count;
        sd_allow();
        (void)sd_up();
        store_reset(STORE_SD);
        (void)store_open(STORE_SD);
        sd_probed = 1;
        sd_probe_faults = sb_fault.count - f0;
    }
    uint64_t faults = sb_fault.count - sd_probe_faults;
    kv_hdr("CARD");
    kv_ok("Card detect (GPA1-5)", sd_rep.present, sd_rep.present ? "card present" : "no card");
    ub_init(&v);
    ub_s(&v, sd_rep.rails > 0 ? "on" : sd_rep.rails == 0 ? "off (not switched here)" : "no ACPM");
    ub_s(&v, "  LDO31 ");
    ub_hexn(&v, sd_rep.l31, 2);
    ub_s(&v, "  LDO2 ");
    ub_hexn(&v, sd_rep.l2, 2);
    kv_ok("Power", sd_rep.rails > 0, v.b);
    kv_hdr("CONTROLLER (DWMMC2 0x11500000)");
    for (unsigned i = 0; i < 3; ++i) {
        ub_init(&v);
        ub_hexn(&v, sd_rep.gate[i], 8);
        kv_add(i == 0 ? "Gate CMU top 1a242070" : i == 1 ? "Gate ACLK 11402030" : "Gate SDCLKIN 11402034", v.b);
    }
    kv_hex("CMU divider 1a241864", sd_rep.cmu_div, 8);
    kv_hex("VERID", sd_rep.verid, 8);
    kv_hex("HCON", sd_rep.hcon, 8);
    kv_hex("CLKSEL64", sd_rep.clksel, 8);
    ub_init(&v);
    ub_dec(&v, sd_rep.bus_hz / 1000000U);
    ub_s(&v, " MHz bus, card ");
    ub_dec(&v, sd_rep.card_hz / 1000U);
    ub_s(&v, " kHz");
    kv_add("Clock", v.b);
    kv_hdr("SD INIT");
    kv_ok("Result", sd_rep.state > 0, sd_rep.state > 0 ? "ready, 4-bit" : sd_rep.why ? sd_rep.why : "not tried");
    kv_hex("CMD8 response", sd_rep.r7, 8);
    kv_hex("OCR", sd_rep.ocr, 8);
    kv_add("Capacity class", sd_rep.high_capacity ? "SDHC/SDXC (block addressed)" : "SDSC");
    kv_hex("RCA", sd_rep.rca, 4);
    ub_init(&v);
    uint32_t pnm[5] = { sd_rep.cid[3] & 255U, sd_rep.cid[2] >> 24, (sd_rep.cid[2] >> 16) & 255U,
                        (sd_rep.cid[2] >> 8) & 255U, sd_rep.cid[2] & 255U };
    for (unsigned i = 0; i < 5; ++i)
        ub_c(&v, pnm[i] >= 32 && pnm[i] < 127 ? (char)pnm[i] : '.');
    ub_s(&v, "  mfr ");
    ub_hexn(&v, sd_rep.cid[3] >> 24, 2);
    kv_add("CID product", v.b);
    kv_hex("Last RINTSTS", sd_rep.last_rintsts, 8);
    kv_hex("Last STATUS", sd_rep.last_status, 8);
    kv_dec("Faults while probing", sb_fault.count - faults, 0);
    if (sd_rep.state <= 0)
        return;
    kv_hdr("MBR");
    static uint8_t mbr[512] __attribute__((aligned(8)));
    if (sd_read(0, 1, (uintptr_t)mbr) || mbr[510] != 0x55 || mbr[511] != 0xaa) {
        kv_addc("Sector 0", "unreadable or no MBR", UI_RED);
    } else {
        for (unsigned i = 0; i < 4; ++i) {
            const uint8_t *e = mbr + 446 + 16 * i;
            uint32_t lba = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24;
            uint32_t len = e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24;
            if (!e[4])
                continue;
            struct ub k;
            ub_init(&k);
            ub_s(&k, "p");
            ub_dec(&k, i + 1);
            ub_s(&k, " type ");
            ub_hexn(&k, e[4], 2);
            ub_init(&v);
            ub_dec(&v, lba);
            ub_s(&v, "  ");
            ub_size(&v, (uint64_t)len * 512U);
            kv_add(k.b, v.b);
        }
    }
    kv_hdr("OS STORE (PARTITION TYPE DA)");
    struct store_ent e;
    if (!store_version(STORE_SD)) {
        kv_addc("Store", "none on the card", UI_RED);
        return;
    }
    kv_dec("First block", store_base(STORE_SD), 0);
    ub_init(&v);
    ub_size(&v, store_used(STORE_SD) * 4096U);
    ub_s(&v, " of ");
    ub_size(&v, store_capacity(STORE_SD) * 4096U);
    kv_add("Used", v.b);
    for (unsigned i = 0; store_get(STORE_SD, i, &e); ++i) {
        ub_init(&v);
        ub_size(&v, e.size);
        ub_s(&v, "  crc ");
        ub_hexn(&v, e.crc, 8);
        kv_add(e.name, v.b);
    }
}

static void build_sd(void)
{
    fill_sd();
    if (sd_rep.state <= 0 && sd_rep.present) {
        kv_hdr("BRING THE CARD UP");
        row_add("1. Power the card", "LDO31 2.8 V + LDO2 3.3 V, power-cycled: tier 1", R_WARN);
        row_add("2. Open the card clocks", "CMU gates 0x1A242070, 0x11402030/34: tier 2", R_WARN);
        row_add("3. Try the card again", "controller and SD init", 0);
        kv_note("With \"Card store at boot\" on, the loader powers and clocks the card before the menu and "
                "none of this is needed.");
    }
}

static int act_sd(int i)
{
    struct sb_auth au;
    const char *why;
    struct ub o;
    if (i == 0) {
        uint8_t l31 = 0, l2 = 0;
        (void)sb_sd_rails(&l31, &l2);
        ub_init(&o);
        ub_s(&o, "LDO31 ");
        ub_hex(&o, l31, 2);
        ub_s(&o, ", LDO2 ");
        ub_hex(&o, l2, 2);
        struct action a = { "Power the card", "CARD", { "LDO31 = 0x28 and LDO2 off, 20 ms, then LDO31 = 0xE8 (2.8 V) and LDO2 = 0xFC (3.3 V)",
                            "the loader's power cycle (hw_sd_rails); read back", 0, 0 },
                            "a reboot", o.b, "0xE8, 0xFC", "", TIER1, SB_SP_PMIC, 0, 0, 0x14aU, 0x1fU };
        a.tier = pmic_tier(1, 0x68, 0xe8, &why);
        if (!lab_confirm(&a, &au))
            return 0;
        int ok = lab_pmic(1, 0x68, 0x28, &au) && lab_pmic(1, 0x4a, (unsigned)(l2 & 0x3fU), &au);
        sb_mdelay(20);
        ok = ok && lab_pmic(1, 0x68, 0xe8, &au) && lab_pmic(1, 0x4a, 0xfc, &au);
        sb_mdelay(5);
        ui_toast(ok ? "Card rails on" : "Rail write failed", ok ? C_GREEN : C_RED, 1100);
    } else if (i == 1) {
        static const uint64_t g[3] = { 0x1a242070ULL, 0x11402030ULL, 0x11402034ULL };
        struct action a = { "Open the card clocks", "CMU", { "Sets bits 20 (manual) and 21 (on) in three clock gates:",
                            "CMU_TOP 0x1A242070 (MMC_CARD), CMU_FSYS1 0x11402030 (ACLK), 0x11402034 (SDCLKIN)",
                            "as Linux's s9p-clkgate does", 0 }, "a reboot", "", "gates on", "", TIER2, SB_SP_MEM, 0, 0,
                            0x11402030ULL, 0x1a242074ULL - 0x11402030ULL };
        int t = 0;
        for (int k = 0; k < 3; ++k) {
            int x = poke_tier(g[k], &why);
            t = x > t ? x : t;
        }
        a.tier = t;
        if (!lab_confirm(&a, &au))
            return 0;
        int ok = 1;
        for (int k = 0; k < 3; ++k) {
            uint32_t v = 0;
            if (sb_probe32(g[k], &v) && (v & 0x00300000U) != 0x00300000U)
                ok &= lab_poke32(g[k], v | 0x00300000U, &au);
        }
        ui_toast(ok ? "Card clocks open" : "Gate write failed", ok ? C_GREEN : C_RED, 1100);
    } else if (i == 2) {
        sd_rep.state = 0;
        sd_allow();
        store_reset(STORE_SD);
        (void)sd_up();
        (void)store_open(STORE_SD);
    }
    return 0;
}

void sd_page(void)
{
    if (!sd_probed && !dialog("microSD card", "Read the card's controller and store?",
                              "Read only. Hold Vol Down + Power if it hangs.", "READ"))
        return;
    list_screen("microSD card", build_sd, act_sd);
}

static void build_sdboot(void)
{
    struct star_settings st;
    int valid = settings_load(&st);
    int on = valid && (st.flags & SET_SD_STORE);
    kv_hdr("SETTINGS RECORD (UFS STORE BLOCK 2, READ)");
    kv_addc("Record", valid ? "valid" : "none (defaults)", valid ? UI_GREEN : UI_TEXT3);
    kv_add("Card store at boot", on ? "on" : "off");
    kv_hdr("CHANGE");
    row_add("Read the card's OS store at boot", "a settings write: tier 1 (warning, slide)",
            R_CHECK | R_WARN | (on ? R_ON : 0));
    kv_note("All the settings: Tools > Settings editor.");
}

int util_settings_write(struct star_settings *n, const char *what);

static int act_sdboot(int i)
{
    struct star_settings st;
    (void)i;
    (void)settings_load(&st);
    st.flags ^= SET_SD_STORE;
    (void)util_settings_write(&st, (st.flags & SET_SD_STORE) ? "Card store at boot: on" : "Card store at boot: off");
    return 0;
}

void sdboot_page(void)
{
    list_screen("Card store at boot", build_sdboot, act_sdboot);
}

/* Start an OS: every catalog entry (hidden ones too). Engineer OS cannot start one itself: it
 * writes the boot-next mailbox (0xFED17FE0) and resets; the loader starts the entry once. */
static void build_start(void)
{
    char key[24];
    if (sb_boot_next_peek(key))
        kv_addc("Mailbox holds", key, UI_ORANGE);
    for (unsigned i = 0; i < cat_count(); ++i) {
        const struct cat_ent *c = cat_get(i);
        const char *why;
        int ok = star_entry_ready(i, &why);
        row_add(c->name, ok ? c->version : why, ok ? 0 : R_DIS);
    }
    kv_note("Starting an entry reboots the phone; the loader reads the boot-next mailbox and starts it "
            "without the menu, once.");
}

static int act_start(int i)
{
    if (i < 0 || (unsigned)i >= cat_count() || !star_entry_ready((unsigned)i, 0))
        return 0;
    const struct cat_ent *c = cat_get((unsigned)i);
    if (!dialog("Start", c->name, "Reboots; the loader starts it once (boot-next).", "START"))
        return 0;
    sb_boot_next(c->key);
    return 1;
}

void start_page(void)
{
    list_screen("Start an OS", build_start, act_start);
}

/* ======================================================================= */
/* LOG AND DEBUGGING                                                          */
/* ======================================================================= */

void show_pmsg(void)
{
    volatile uint32_t *h = (volatile uint32_t *)0xFED14000UL;
    const volatile char *d = (const volatile char *)0xFED1400CUL;
    const uint32_t cap = 0x3f00;
    tv_reset();
    if (h[0] != 0x43474244U) {
        tv_line("No pmsg ring at 0xFED14000.");
        text_screen("Boot log");
        return;
    }
    uint32_t start = h[1], size = h[2];
    if (size > cap)
        size = cap;
    uint32_t pos = size < cap ? 0 : start % cap;
    char line[200];
    unsigned k = 0;
    for (uint32_t i = 0; i < size; ++i) {
        char c = d[(pos + i) % cap];
        if (c == '\n' || k == sizeof(line) - 1) {
            line[k] = 0;
            tv_line(line);
            k = 0;
            if (c == '\n')
                continue;
        }
        line[k++] = (c >= 32 && c < 127) ? c : '.';
    }
    line[k] = 0;
    if (k)
        tv_line(line);
    text_screen("Boot log (pmsg)");
}

void show_cmdline(void)
{
    uintptr_t args = 0;
    uint32_t args_len = 0;
    tv_reset();
    tv_line("S-Boot /chosen bootargs:");
    tv_line("");
    if (!fdt_chosen_raw(sb_boot.fdt, "bootargs", &args, &args_len)) {
        tv_line("(none)");
    } else {
        char tok[200];
        unsigned k = 0;
        const volatile char *a = (const volatile char *)args;
        for (uint32_t i = 0; i <= args_len; ++i) {
            char c = i < args_len ? a[i] : ' ';
            if (c == ' ' || c == 0) {
                if (k) {
                    tok[k] = 0;
                    tv_line(tok);
                    k = 0;
                }
            } else if (k < sizeof(tok) - 1) {
                tok[k++] = c;
            }
        }
    }
    tv_line("");
    tv_line("Catalog command lines (appended to each OS's own):");
    for (unsigned i = 0; i < cat_count(); ++i) {
        const struct cat_ent *c = cat_get(i);
        if (!c->cmdline[0])
            continue;
        struct ub u;
        tv_line("");
        ub_init(&u);
        ub_s(&u, c->key);
        ub_c(&u, ':');
        tv_line(u.b);
        tv_line(c->cmdline);
    }
    tv_line("");
    tv_line("BOOT's iOS payload boot-args: in the loader (Engineer Lite).");
    text_screen("Kernel command lines");
}

static void hexrow(struct ub *u, uintptr_t a, unsigned words)
{
    ub_init(u);
    ub_hexn(u, a, 9);
    ub_s(u, ": ");
    for (unsigned i = 0; i < words; ++i) {
        ub_hexn(u, *(volatile uint32_t *)(a + 4 * i), 8);
        ub_c(u, ' ');
    }
}

void show_records(void)
{
    struct ub u;
    tv_reset();
    tv_line("RWD1 watchdog record @0xFED13D80 (+cookie):");
    for (unsigned o = 0; o < 0x44; o += 16) {
        hexrow(&u, 0xFED13D80UL + o, o + 16 <= 0x44 ? 4 : 1);
        tv_line(u.b);
    }
    tv_line("");
    tv_line("P3 SMP1 record @0xFED13E80:");
    for (unsigned o = 0; o < 0x80; o += 16) {
        hexrow(&u, 0xFED13E80UL + o, 4);
        tv_line(u.b);
    }
    tv_line("");
    tv_line("Low WDT record @0xFED13800:");
    hexrow(&u, 0xFED13800UL, 3);
    tv_line(u.b);
    tv_line("");
    tv_line("Latch page @0xFED17F00 (NRM1 F18, WATL F40, witness F70, KEP FB0):");
    for (unsigned o = 0; o < 0x100; o += 16) {
        hexrow(&u, 0xFED17F00UL + o, 4);
        tv_line(u.b);
    }
    text_screen("Retained records");
}

void fill_regs(void)
{
    volatile uint32_t *pmu = (volatile uint32_t *)SB_PMU;
    kv_hdr("PMU 0x14060000 (LIVE)");
    kv_hex("SWRESET (+0x400)", pmu[0x400 / 4], 8);
    kv_hex("RST_STAT (+0x404)", pmu[0x404 / 4], 8);
    kv_hex("WDT_DISABLE (+0x408)", pmu[0x408 / 4], 8);
    kv_hex("WDT_MASK_RESET (+0x40C)", pmu[0x40c / 4], 8);
    kv_hex("INFORM2 (+0x808)", pmu[0x808 / 4], 8);
    kv_hex("INFORM3 (+0x80C)", pmu[0x80c / 4], 8);
    kv_hdr("WATCHDOG 0x10050000");
    kv_hex("WTCON", *(volatile uint32_t *)0x10050000UL, 8);
    kv_hex("WTCNT", *(volatile uint32_t *)0x10050008UL, 8);
    kv_hdr("CHIPID 0x10000000");
    kv_hex("PRO_ID", *(volatile uint32_t *)0x10000000UL, 8);
    kv_hdr("ALIVE GPIO 0x14050000");
    kv_hex("GPA0 DAT", *(volatile uint32_t *)0x14050024UL, 8);
    kv_hex("GPA1 DAT", *(volatile uint32_t *)0x14050044UL, 8);
    kv_hex("GPA2 DAT", *(volatile uint32_t *)0x14050064UL, 8);
    kv_hdr("UFS 0x11120000");
    kv_hex("HCS", *(volatile uint32_t *)0x11120030UL, 8);
    kv_hex("IS", *(volatile uint32_t *)0x11120020UL, 8);
}

/* ======================================================================= */
/* OTHERS                                                                     */
/* ======================================================================= */

/* ---- core bring-up test (PSCI 0.1 via SMC, starlte DTB /psci) ------------ */

#define BOX sb_core_box

static struct core_result {
    int64_t ret;
    int slot;
    uint64_t mpidr, el, state;
    uint32_t ms;
} cores[7];
static int cores_done;

/* PSCI targets: A55 cpu1-3 = 1..3 (measured 2026-10-25), M3 = DTB reg 0x100..0x103. */
static uint64_t core_target(unsigned i)
{
    return i < 3 ? i + 1U : 0x100U + (i - 3U);
}

static const char *psci_err(int64_t r)
{
    switch (r) {
    case 0: return "SUCCESS";
    case -1: return "NOT_SUPPORTED";
    case -2: return "INVALID_PARAMETERS";
    case -3: return "DENIED";
    case -4: return "ALREADY_ON";
    case -5: return "ON_PENDING";
    case -6: return "INTERNAL_FAILURE";
    case -9: return "INVALID_ADDRESS";
    default: return "?";
    }
}

void fill_cores(void)
{
    struct ub k, v;
    if (!cores_done) {
        kv_add("Status", "not run");
        return;
    }
    unsigned up = 0;
    for (unsigned i = 0; i < 7; ++i)
        up += cores[i].slot >= 0;
    ub_init(&v);
    ub_dec(&v, up + 1U);
    ub_s(&v, " / 8");
    kv_big("cores answered (cpu0 = this one)", v.b);
    kv_hdr("PER CORE: CPU_ON, CHECK-IN, CPU_OFF");
    kv_addc("cpu0 A55", "boot core, running Engineer OS", UI_GREEN);
    for (unsigned i = 0; i < 7; ++i) {
        const struct core_result *c = &cores[i];
        ub_init(&k);
        ub_s(&k, "cpu");
        ub_dec(&k, i + 1U);
        ub_s(&k, i < 3 ? " A55  PSCI " : " M3  PSCI ");
        ub_hex(&k, core_target(i), 3);
        ub_init(&v);
        if (c->ret) {
            ub_s(&v, "CPU_ON ");
            ub_s(&v, psci_err(c->ret));
            kv_addc(k.b, v.b, UI_RED);
        } else if (c->slot < 0) {
            ub_s(&v, "CPU_ON ok, no check-in in 1 s");
            kv_addc(k.b, v.b, UI_ORANGE);
        } else {
            ub_s(&v, "up in ");
            ub_dec(&v, c->ms);
            ub_s(&v, " ms, MPIDR ");
            ub_hex(&v, c->mpidr, 8);
            ub_s(&v, ", EL");
            ub_dec(&v, (c->el >> 2) & 3U);
            ub_s(&v, c->state == 2 ? ", PARKED" : ", off again");
            kv_addc(k.b, v.b, c->state == 2 ? UI_ORANGE : UI_GREEN);
        }
    }
    kv_hdr("NOTES");
    kv_add("Entry", "libsb crt0.S sb_core_entry (MMU off)");
    kv_add("Parked core", "CPU_OFF returned: reboot before an OS");
}

static unsigned cores_cur;

static void fill_cores_running(void)
{
    struct ub u;
    kv_hdr("RUNNING");
    ub_init(&u);
    ub_s(&u, "CPU_ON ");
    ub_hex(&u, core_target(cores_cur), 3);
    ub_s(&u, cores_cur < 3 ? " (A55)" : " (M3)");
    kv_add("Starting", u.b);
    ub_init(&u);
    ub_dec(&u, cores_cur + 1U);
    ub_s(&u, " of 7");
    kv_add("Core", u.b);
}

static void run_cores(void)
{
    for (unsigned i = 0; i < 32; ++i)
        BOX[i] = 0;
    __asm__ volatile("dsb sy" : : : "memory");
    sb_dcache_inval(sb_core_box, 32 * sizeof(uint64_t));   /* zeros to DRAM now, never later over a check-in */
    for (unsigned i = 0; i < 7; ++i) {
        struct core_result *c = &cores[i];
        uint64_t before[8];
        for (unsigned s = 0; s < 8; ++s)
            before[s] = BOX[s * 4];
        cores_cur = i;
        show_now("Core bring-up test", fill_cores_running);
        c->slot = -1;
        c->state = 0;
        uint64_t t0 = sb_ticks();
        c->ret = sb_psci(SB_PSCI_CPU_ON, core_target(i), (uint64_t)(uintptr_t)sb_core_entry, 0);
        if (c->ret)
            continue;
        for (unsigned ms = 0; ms < 1000 && c->slot < 0; ms += 1) {
            /* the core writes with its MMU off, straight to DRAM; ours caches .data (mmu.c) */
            sb_dcache_inval(sb_core_box, 32 * sizeof(uint64_t));
            for (unsigned s = 0; s < 8; ++s)
                if (BOX[s * 4] != before[s]) {
                    c->slot = (int)s;
                    break;
                }
            if (c->slot < 0)
                sb_mdelay(1);
        }
        c->ms = (uint32_t)((sb_ticks() - t0) * 1000U / sb_freq());
        if (c->slot >= 0) {
            sb_mdelay(50);
            sb_dcache_inval(sb_core_box, 32 * sizeof(uint64_t));
            c->mpidr = BOX[c->slot * 4 + 1];
            c->el = BOX[c->slot * 4 + 2];
            c->state = BOX[c->slot * 4];
        }
    }
    cores_done = 1;
    unsigned up = 0, parked_n = 0;
    for (unsigned i = 0; i < 7; ++i) {
        up += cores[i].slot >= 0;
        parked_n += cores[i].slot >= 0 && cores[i].state == 2;
        struct ub n, d;
        ub_init(&n);
        ub_s(&n, "Core cpu");
        ub_dec(&n, i + 1U);
        ub_s(&n, i < 3 ? " A55" : " M3");
        ub_init(&d);
        if (cores[i].ret) {
            ub_s(&d, "CPU_ON ");
            ub_s(&d, psci_err(cores[i].ret));
        } else if (cores[i].slot < 0) {
            ub_s(&d, "no check-in in 1 s");
        } else {
            ub_s(&d, "up in ");
            ub_dec(&d, cores[i].ms);
            ub_s(&d, cores[i].state == 2 ? " ms, PARKED" : " ms, off again");
        }
        test_record(n.b, !cores[i].ret && cores[i].slot >= 0 && cores[i].state != 2, d.b);
    }
    (void)up;
    info_screen("Core bring-up test", fill_cores, 0);
    if (parked_n && !E.home && dialog("Core parked", "A core did not power off.", "Reboot before starting an OS.", "REBOOT"))
        sb_reboot(SB_INFORM_NORMAL);
}

void run_cores_dialog(void)
{
    if (dialog("Core bring-up test", "Start each core, then turn it off.", "Reboot after it if a core parks.", "RUN"))
        run_cores();
}

static void kv_ufs_fail(void)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, ufs_why == 1 ? "host not ready" : ufs_why == 2 ? "S-Boot request outstanding" :
             ufs_why == 3 ? "host refused our request list" : "device did not answer");
    kv_addc("UFS", u.b, UI_RED);
    ub_init(&u);
    ub_s(&u, "OCS ");
    ub_hex(&u, ufs_ocs, 2);
    ub_s(&u, "  UPIU ");
    ub_hex(&u, ufs_resp, 2);
    ub_s(&u, "  status ");
    ub_hex(&u, ufs_status, 2);
    kv_add("Last command", u.b);
}

/* UTF-16BE string descriptor to ASCII; hex if unprintable. */
static void ufs_string(uint8_t index, struct ub *u)
{
    uint8_t d[256];
    int n = ufs_desc(0x05, index, d, sizeof(d));
    if (n < 2) {
        ub_s(u, "?");
        return;
    }
    unsigned len = d[0] < (unsigned)n ? d[0] : (unsigned)n, printable = 1;
    for (unsigned i = 2; i + 1 < len; i += 2)
        if (d[i] || d[i + 1] < 32 || d[i + 1] > 126)
            printable = 0;
    for (unsigned i = 2; i + 1 < len && u->n < 60; i += 2) {
        if (printable)
            ub_c(u, (char)d[i + 1]);
        else
            ub_hexn(u, (uint64_t)d[i] << 8 | d[i + 1], 4);
    }
    while (printable && u->n && u->b[u->n - 1] == ' ')
        u->b[--u->n] = 0;
}

static uint64_t be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i)
        v = v << 8 | p[i];
    return v;
}

static const char *life_text(unsigned v)
{
    switch (v) {
    case 0x00: return "not reported";
    case 0x01: return "0-10 % used";
    case 0x02: return "10-20 % used";
    case 0x03: return "20-30 % used";
    case 0x04: return "30-40 % used";
    case 0x05: return "40-50 % used";
    case 0x06: return "50-60 % used";
    case 0x07: return "60-70 % used";
    case 0x08: return "70-80 % used";
    case 0x09: return "80-90 % used";
    case 0x0a: return "90-100 % used";
    case 0x0b: return "EXCEEDED rated life";
    default: return "?";
    }
}

void fill_ufs_dev(void)
{
    struct ub u;
    uint8_t d[256];
    if (!ufs_up()) {
        kv_ufs_fail();
        return;
    }
    kv_hdr("SCSI INQUIRY (LU0)");
    static const uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    if (!ufs_scsi(inq, 6, 36)) {
        char s[20];
        for (unsigned i = 0; i < 8; ++i)
            s[i] = (char)u8r(UFS_BUF + 8 + i);
        s[8] = 0;
        kv_add("Vendor", s);
        for (unsigned i = 0; i < 16; ++i)
            s[i] = (char)u8r(UFS_BUF + 16 + i);
        s[16] = 0;
        kv_add("Product", s);
        for (unsigned i = 0; i < 4; ++i)
            s[i] = (char)u8r(UFS_BUF + 32 + i);
        s[4] = 0;
        kv_add("Firmware revision", s);
    } else {
        kv_ufs_fail();
    }
    kv_hdr("DEVICE DESCRIPTOR");
    if (ufs_desc(0x00, 0, d, sizeof(d)) >= 0x2b) {
        ub_init(&u);
        ub_s(&u, "UFS ");
        ub_dec(&u, d[0x10]);
        ub_c(&u, '.');
        ub_dec(&u, d[0x11] >> 4);
        kv_add("Specification", u.b);
        uint16_t mid = (uint16_t)(d[0x18] << 8 | d[0x19]);
        ub_init(&u);
        ub_hex(&u, mid, 4);
        ub_s(&u, mid == 0x01ce ? "  Samsung" : mid == 0x0198 ? "  Toshiba" : mid == 0x01ad ? "  SK hynix" :
                 mid == 0x012c ? "  Micron" : "");
        kv_add("Manufacturer ID", u.b);
        ub_init(&u);
        ufs_string(d[0x15], &u);
        kv_add("Product name", u.b);
        ub_init(&u);
        ufs_string(d[0x2a], &u);
        kv_add("Product revision", u.b);
        ub_init(&u);
        ufs_string(d[0x16], &u);
        kv_add("Serial", u.b);
        kv_hex("Manufacture date", (uint64_t)d[0x12] << 8 | d[0x13], 4);
        kv_dec("Logical units", d[0x06], 0);
        kv_dec("Queue depth", d[0x21], 0);
        kv_hex("Device version", (uint64_t)d[0x22] << 8 | d[0x23], 4);
    } else {
        kv_ufs_fail();
    }
    kv_hdr("GEOMETRY");
    if (ufs_desc(0x07, 0, d, sizeof(d)) >= 12) {
        ub_init(&u);
        ub_size(&u, be64(d + 4) * 512U);
        kv_add("Raw capacity", u.b);
    }
    if (ufs_desc(0x02, 0, d, sizeof(d)) >= 0x13) {
        ub_init(&u);
        ub_size(&u, be64(d + 0x0b) << d[0x0a]);
        ub_s(&u, ", ");
        ub_dec(&u, 1U << d[0x0a]);
        ub_s(&u, "-byte blocks");
        kv_add("LU0", u.b);
    }
    kv_hdr("HEALTH (UFS 2.1 DESCRIPTOR 0x09)");
    if (ufs_desc(0x09, 0, d, sizeof(d)) >= 5) {
        unsigned eol = d[2];
        kv_addc("Reserved blocks (pre-EOL)", eol == 1 ? "normal" : eol == 2 ? "WARNING: 80 % consumed" :
                                             eol == 3 ? "URGENT: 90 % consumed" : "not reported",
                eol == 1 ? UI_GREEN : eol >= 2 ? UI_RED : UI_TEXT2);
        kv_addc("Wear, type A (SLC)", life_text(d[3]), d[3] >= 0x09 ? UI_ORANGE : UI_TEXT);
        kv_addc("Wear, type B (MLC/TLC)", life_text(d[4]), d[4] >= 0x09 ? UI_ORANGE : UI_TEXT);
    } else {
        kv_addc("Health", "descriptor not supported", UI_TEXT3);
    }
}

/* What sits at LU0 block lba (superblock magics), e.g. "ext4 \"vendor\"". */
static void fs_kind(uint64_t lba, struct ub *u)
{
    if (ufs_read(lba, 1)) {
        ub_s(u, "unreadable");
        return;
    }
    const uintptr_t b = UFS_BUF;
    uint32_t f2 = (uint32_t)u8r(b + 1024) | (uint32_t)u8r(b + 1025) << 8 | (uint32_t)u8r(b + 1026) << 16 |
                  (uint32_t)u8r(b + 1027) << 24;
    if (u8r(b + 1080) == 0x53 && u8r(b + 1081) == 0xef) {
        ub_s(u, "ext4");
        if (u8r(b + 1144)) {
            ub_s(u, " \"");
            for (unsigned i = 0; i < 16 && u8r(b + 1144 + i); ++i)
                ub_c(u, (char)u8r(b + 1144 + i));
            ub_c(u, '"');
        }
    } else if (f2 == 0xf2f52010U) {
        ub_s(u, "f2fs");
    } else if (u8r(b + 1024) == 'H' && (u8r(b + 1025) == '+' || u8r(b + 1025) == 'X')) {
        ub_s(u, "HFS+ (iOS)");
    } else if (u8r(b + 3) == 'N' && u8r(b + 4) == 'T' && u8r(b + 5) == 'F' && u8r(b + 6) == 'S') {
        ub_s(u, "NTFS");
    } else if (u8r(b + 82) == 'F' && u8r(b + 83) == 'A' && u8r(b + 84) == 'T') {
        ub_s(u, "FAT32");
    } else {
        int zero = 1;
        for (unsigned i = 0; i < 4096 && zero; i += 8)
            zero = !*(volatile uint64_t *)(b + i);
        ub_s(u, zero ? "empty" : "raw data");
    }
}

void fill_partitions(void)
{
    struct ub k, v;
    if (gpt_load() < 0) {
        if (ufs_state < 0)
            kv_ufs_fail();
        else
            kv_addc("GPT", "not readable", UI_RED);
        return;
    }
    kv_hdr("LU0 GPT, LIVE (4 KIB BLOCKS)");
    uint64_t total = 0;
    for (int i = 0; i < gpt_n; ++i) {
        uint64_t n = gpt_last[i] - gpt_first[i] + 1;
        total += n;
        ub_init(&k);
        ub_s(&k, "sda");
        ub_dec(&k, (uint64_t)i + 1U);
        ub_s(&k, "  ");
        ub_s(&k, gpt_name[i]);
        ub_init(&v);
        ub_size(&v, n * 4096U);
        ub_s(&v, "  ");
        fs_kind(gpt_first[i], &v);
        const char *g = gpt_name[i];
        int mine = (g[0] == 'W' && g[1] == 'I' && g[2] == 'N') || (g[0] == 'A' && g[1] == '1' && g[2] == '6') ||
                   (g[0] == 'U' && g[1] == 'S' && g[2] == 'E' && g[3] == 'R') || (g[0] == 'B' && g[1] == 'O' && g[2] == 'O' && g[3] == 'T') ||
                   (g[0] == 'S' && g[1] == 'Y' && g[2] == 'S') || (g[0] == 'V' && g[1] == 'E' && g[2] == 'N') ||
                   (g[0] == 'O' && g[1] == 'D' && g[2] == 'M') || (g[0] == 'R' && g[1] == 'E' && g[2] == 'C');
        kv_addc(k.b, v.b, mine ? UI_HOLO : UI_TEXT);
    }
    ub_init(&v);
    ub_size(&v, total * 4096U);
    kv_add("Partitioned", v.b);
    kv_hdr("STARBITE");
    kv_add("BOOT", "this loader + iOS payload + Windows UEFI");
    kv_add("RECOVERY", "TWRP 3.7.0 (/data = A16DATA)");
    kv_add("USERDATA", "iOS root, OS store (+16 GiB), Kiosk (+17 GiB)");
    kv_add("SYSTEM / VENDOR", "Android 17 (LineageOS 24.0)");
    kv_add("ODM", "Android /metadata");
    kv_add("A16DATA", "Android /data (f2fs)");
    kv_add("WINESP / WINDOWS", "Windows 11");
    kv_add("microSD", "postmarketOS root (image file)");
}

/* ---- speed test --------------------------------------------------------------- */

static uint64_t sp_single, sp_multi;
static int sp_done, sp_multi_ok;
static uint64_t sp_lba;

void fill_speed(void)
{
    struct ub u;
    if (!sp_done) {
        kv_ufs_fail();
        return;
    }
    ub_init(&u);
    ub_dec(&u, sp_multi_ok ? sp_multi : sp_single);
    ub_s(&u, " MB/s");
    kv_big(sp_multi_ok ? "sequential read, 64 KiB commands" : "sequential read, 4 KiB commands", u.b);
    kv_hdr("16 MIB FROM SYSTEM, READ ONLY");
    ub_init(&u);
    ub_dec(&u, sp_single);
    ub_s(&u, " MB/s");
    kv_add("4 KiB per command (S-Boot host state)", u.b);
    ub_init(&u);
    if (sp_multi_ok) {
        ub_dec(&u, sp_multi);
        ub_s(&u, " MB/s");
    } else {
        ub_s(&u, "self-test failed: skipped");
    }
    kv_addc("64 KiB per command (16 x 16-byte PRDT)", u.b, sp_multi_ok ? UI_TEXT : UI_ORANGE);
    kv_dec("Start block", sp_lba, 0);
    kv_hdr("NOTE");
    kv_add("Polled, one command at a time", "iOS uses 64 KiB commands");
}

static int bufs_equal(uintptr_t a, uintptr_t b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i += 8)
        if (*(volatile uint64_t *)(a + i) != *(volatile uint64_t *)(b + i))
            return 0;
    return 1;
}

static void fill_speed_running(void)
{
    kv_hdr("RUNNING");
    kv_add("Pass", "4 KiB commands, then 64 KiB");
    kv_add("Reads", "16 MiB from SYSTEM, read only");
}

void run_speed(void)
{
    const uint32_t total = 4096;                    /* 16 MiB in 4 KiB blocks */
    sp_done = 0;
    sp_multi_ok = 0;
    if (!ufs_up()) {
        info_screen("Speed test", fill_speed, 0);
        return;
    }
    sp_lba = 68736;                                 /* SYSTEM (ext4), any readable range */
    if (gpt_load() > 0)
        for (int i = 0; i < gpt_n; ++i)
            if (gpt_name[i][0] == 'S' && gpt_name[i][1] == 'Y' && gpt_name[i][2] == 'S')
                sp_lba = gpt_first[i];
    show_now("Speed test", fill_speed_running);
    uint64_t t0 = sb_ticks();
    for (uint32_t i = 0; i < total; ++i)
        if (ufs_read(sp_lba + i, 1)) {
            test_record("UFS speed", 0, "read failed");
            info_screen("Speed test", fill_speed, 0);
            return;
        }
    uint64_t dt = sb_ticks() - t0;
    sp_single = dt ? (uint64_t)total * 4096U * sb_freq() / dt / 1000000U : 0;
    sp_done = 1;

    /* 16-block commands as iOS does (starlte_ufs_pick_stride): verify first. */
    uint32_t tx0 = rd(UFS + 0x1100), rx0 = rd(UFS + 0x1104), ro0 = rd(UFS + 0x1160), ax0 = rd(UFS + 0x116c);
    const uintptr_t ref = UFS_BUF + (512U << 10);
    int ok = !ufs_read(sp_lba, 16);
    for (uint32_t i = 0; ok && i < 16U * 4096U; i += 8)
        *(volatile uint64_t *)(ref + i) = *(volatile uint64_t *)(UFS_BUF + i);
    wr(UFS + 0x1100, 0x8000000cU);
    wr(UFS + 0x1104, 0x8000000cU);
    wr(UFS + 0x1160, 0xaU);
    wr(UFS + 0x116c, 0x98000003U);
    if (ok) {
        u_zero(UFS_BUF, 16U * 4096U);
        ok = !ufs_read(sp_lba, 16) && bufs_equal(UFS_BUF, ref, 16U * 4096U);
    }
    if (ok) {
        t0 = sb_ticks();
        for (uint32_t i = 0; ok && i < total; i += 16)
            ok = !ufs_read(sp_lba + i, 16);
        dt = sb_ticks() - t0;
        sp_multi = dt ? (uint64_t)total * 4096U * sb_freq() / dt / 1000000U : 0;
        sp_multi_ok = ok;
    }
    wr(UFS + 0x1100, tx0);
    wr(UFS + 0x1104, rx0);
    wr(UFS + 0x1160, ro0);
    wr(UFS + 0x116c, ax0);
    {
        struct ub u;
        ub_init(&u);
        ub_dec(&u, sp_single);
        ub_s(&u, " MB/s (4 KiB), ");
        if (sp_multi_ok) {
            ub_dec(&u, sp_multi);
            ub_s(&u, " MB/s (64 KiB)");
        } else {
            ub_s(&u, "64 KiB self-test failed");
        }
        test_record("UFS speed", 1, u.b);
    }
    info_screen("Speed test", fill_speed, 0);
}

/* ---- sector viewer -------------------------------------------------------------- */

static uint64_t sv_lba = 1;
static int sv_err;
static char sv_lines[256][72];
static int sv_n;

static void sv_load(void)
{
    struct ub u;
    sv_n = 0;
    sv_err = !ufs_up() || ufs_read(sv_lba, 1);
    if (sv_err) {
        ub_init(&u);
        ub_s(&u, "Read failed: OCS ");
        ub_hex(&u, ufs_ocs, 2);
        ub_s(&u, " UPIU ");
        ub_hex(&u, ufs_resp, 2);
        ub_s(&u, " status ");
        ub_hex(&u, ufs_status, 2);
        sb_strcpy(sv_lines[sv_n++], 72, u.b);
        sb_strcpy(sv_lines[sv_n++], 72, "(protected range? S-Boot's UFSP regions stay as they are)");
        return;
    }
    for (unsigned o = 0; o < 4096; o += 16) {
        ub_init(&u);
        ub_hexn(&u, o, 3);
        ub_c(&u, ' ');
        for (unsigned i = 0; i < 16; ++i) {             /* 4-byte groups: 56 columns fit the panel */
            ub_hexn(&u, u8r(UFS_BUF + o + i), 2);
            if ((i & 3) == 3)
                ub_c(&u, ' ');
        }
        for (unsigned i = 0; i < 16; ++i) {
            uint8_t c = u8r(UFS_BUF + o + i);
            ub_c(&u, c >= 32 && c < 127 ? (char)c : '.');
        }
        sb_strcpy(sv_lines[sv_n++], 72, u.b);
    }
}

static const char *sv_part(void)
{
    for (int i = 0; i < gpt_n; ++i)
        if (sv_lba >= gpt_first[i] && sv_lba <= gpt_last[i])
            return gpt_name[i];
    return sv_lba < 6 ? "GPT / PIT head" : "LU0";
}

/* the button bar: -1, -256, [block, partition], +256, +1 */
static void sv_buttons(const struct pitem *it, int x, int y, int w, int focused)
{
    (void)it;
    (void)focused;
    static const char *const lab[4] = { U_LEFT " 1", U_LEFT " 256", "256 " U_RIGHT, "1 " U_RIGHT };
    const int bx[4] = { 24, 224, w - 424, w - 224 };
    for (int b = 0; b < 4; ++b) {
        ui_round(x + bx[b], y + 30, 200, 140, 30, C_CARD2);
        ui_text_c(F_SEG, x + bx[b] + 100, y + 116, lab[b], C_STAR);
    }
    struct ub u;
    ub_init(&u);
    ub_dec(&u, sv_lba);
    ui_text_c(F_TILE, x + w / 2, y + 96, u.b, sv_err ? C_RED : C_L1);
    ui_text_c(F_SMALL, x + w / 2, y + 150, sv_part(), C_L2);
}

static void fill_sv(void)
{
    kv_custom(200, sv_buttons, 0, (uint32_t)sv_lba * 2654435761U ^ (uint32_t)sv_err);
    for (int i = 0; i < sv_n; ++i)
        kv_monoc(sv_lines[i], sv_err ? UI_RED : UI_TEXT);
    kv_note("Tap the block number to jump to a partition. Volume scrolls, Power: next block.");
}

static void build_sv_parts(void)
{
    row_add("GPT header", "block 1", 0);
    for (int i = 0; i < gpt_n && i < 60; ++i) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "block ");
        ub_dec(&u, gpt_first[i]);
        row_add(gpt_name[i], u.b, 0);
    }
}

static int act_sv_parts(int i)
{
    sv_lba = i == 0 ? 1 : gpt_first[i - 1];
    return 1;
}

void sector_screen(void)
{
    struct page p;
    (void)gpt_load();
    sv_load();
    page_init(&p, UI_VIEW_Y0, UI_VIEW_Y1);
    screen_push("Sector viewer");
    chrome("Sector viewer", "READ ONLY", C_GREEN);
    eng_build_page(&p, fill_sv);
    ui_present();
    for (;;) {
        struct ui_ev e;
        int64_t move = 0;
        ui_poll(&e, p.fling_v ? 8 : 40);
        if (E.redraw) {
            chrome("Sector viewer", "READ ONLY", C_GREEN);
            page_redraw(&p);
        }
        if (nav_event(&e))
            break;
        if (page_scroll_event(&p, &e)) {
            ;
        } else if (e.type == EV_UP) {
            page_set_scroll(&p, p.scroll - 560);
        } else if (e.type == EV_DOWN) {
            page_set_scroll(&p, p.scroll + 560);
        } else if (e.type == EV_SELECT) {
            move = 1;
        } else if (e.type == EV_TAP) {
            int item = -1;
            (void)page_hit(&p, e.x, e.y, &item);
            if (item == 0) {
                int x = e.x - 40;
                if (x < 224)
                    move = -1;
                else if (x < 424)
                    move = -256;
                else if (x >= 1360 - 224)
                    move = 1;
                else if (x >= 1360 - 424)
                    move = 256;
                else if (gpt_n > 0) {
                    list_screen("Go to partition", build_sv_parts, act_sv_parts);
                    if (E.home || E.tab_req >= 0)
                        break;
                    sv_load();
                    chrome("Sector viewer", "READ ONLY", C_GREEN);
                    p.drawn = 0;
                    p.scroll = 0;
                    eng_build_page(&p, fill_sv);
                }
            }
        }
        page_fling_step(&p);
        if (move) {
            sv_lba = move < 0 && (uint64_t)-move > sv_lba ? 0 : (uint64_t)((int64_t)sv_lba + move);
            sv_load();
            eng_build_page(&p, fill_sv);
        }
        ui_present();
    }
    screen_pop();
    page_free(&p);
}

/* ---- panel brightness (S6E3HA8 AOR, as S-Boot does it) ----------------------
 * build/em-research-brightness.md. S-Boot dims this panel with AOR only:
 * F0 5A 5A (level-2 key), B1 hi lo (AOR), F7 03 (commit), F0 A5 A5, all DCS
 * long writes (0x39) through DSIM 0x16080000: payload FIFO first, then the
 * header, then wait for the header FIFO to drain. Never sends gamma, ELVSS,
 * voltages, POC/MTP writes, sleep or display off. The next OS re-inits the
 * panel, so the setting lasts until then.
 */
#define DSIM 0x16080000UL
#define DSIM_INTSRC (DSIM + 0x50)
#define DSIM_PKTHDR (DSIM + 0x58)
#define DSIM_PAYLOAD (DSIM + 0x5c)
#define DSIM_RXFIFO (DSIM + 0x60)
#define DSIM_FIFOCTRL (DSIM + 0x68)
#define DSIM_PH_EMPTY (1U << 28)
#define DSIM_RX_DONE (1U << 18)

static uint32_t panel_aor = 0x0440;     /* S-Boot's normal level, assumed */

static int dsim_ready(void)
{
    return (rd(DSIM + 0x20) & (1U << 20)) && !(rd(DSIM + 0x4c) & (1U << 18));
}

static void dsim_hdr(uint32_t id, uint32_t d0, uint32_t d1, uint32_t bta)
{
    uint32_t v = rd(DSIM_PKTHDR) & 0xfe000000U;
    wr(DSIM_PKTHDR, v | (bta << 24) | ((d1 & 0xffU) << 16) | ((d0 & 0xffU) << 8) | id);
}

static int dsim_wait(void)
{
    for (unsigned t = 0; t < 50000; t += 10) {
        uint32_t s = rd(DSIM_INTSRC), f = rd(DSIM_FIFOCTRL);
        if ((s & DSIM_PH_EMPTY) || ((f & (1U << 10)) && !((f >> 16) & 0x3fU))) {
            wr(DSIM_INTSRC, DSIM_PH_EMPTY);
            return 0;
        }
        sb_udelay(10);
    }
    return -1;
}

static int dcs_long(const uint8_t *b, uint32_t n)
{
    if (((rd(DSIM_FIFOCTRL) >> 16) & 0x3fU) && dsim_wait())
        return -1;
    wr(DSIM_INTSRC, DSIM_PH_EMPTY);
    for (uint32_t i = 0; i < n; i += 4) {
        uint32_t w = 0;
        for (uint32_t k = 0; k < 4 && i + k < n; ++k)
            w |= (uint32_t)b[i + k] << (8 * k);
        wr(DSIM_PAYLOAD, w);
    }
    dsim_hdr(0x39, n & 0xffU, n >> 8, 0);
    return dsim_wait();
}

static int panel_set_aor(uint32_t aor)
{
    if (!dsim_ready())
        return -1;
    if (aor < 0x000c)
        aor = 0x000c;
    if (aor > 0x0ba3)
        aor = 0x0ba3;
    const uint8_t key_on[3] = { 0xf0, 0x5a, 0x5a };
    const uint8_t commit[2] = { 0xf7, 0x03 };
    const uint8_t key_off[3] = { 0xf0, 0xa5, 0xa5 };
    const uint8_t b1[3] = { 0xb1, (uint8_t)(aor >> 8), (uint8_t)aor };
    int r = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        r = dcs_long(key_on, 3);
        if (!r)
            r = dcs_long(b1, 3);
        if (!r)
            r = dcs_long(commit, 2);
        (void)dcs_long(key_off, 3);             /* always lock again */
        if (!r)
            break;
        sb_mdelay(20);
    }
    if (!r)
        panel_aor = aor;
    return r;
}

/* DCS read 0x04 (panel ID, 3 bytes; S-Boot read 91 04 44): bytes, or < 0. */
static int panel_read_id(uint8_t out[3])
{
    if (!dsim_ready())
        return -1;
    for (int g = 0; g < 64 && !(rd(DSIM_FIFOCTRL) & (1U << 12)); ++g)
        (void)rd(DSIM_RXFIFO);
    wr(DSIM_INTSRC, DSIM_RX_DONE | DSIM_PH_EMPTY);
    dsim_hdr(0x37, 3, 0, 0);
    if (dsim_wait())
        return -1;
    wr(DSIM_INTSRC, DSIM_RX_DONE);
    dsim_hdr(0x06, 0x04, 0, 1);
    for (unsigned t = 0; !(rd(DSIM_INTSRC) & DSIM_RX_DONE); t += 10) {
        if (t > 100000)
            return -1;
        sb_udelay(10);
    }
    wr(DSIM_INTSRC, DSIM_RX_DONE | DSIM_PH_EMPTY);
    for (int g = 0; g < 64 && !(rd(DSIM_FIFOCTRL) & (1U << 12)); ++g) {
        uint32_t w = rd(DSIM_RXFIFO);
        switch (w & 0xffU) {
        case 0x1c: {
            uint32_t wc = (w >> 8) & 0xffffU, d = rd(DSIM_RXFIFO);
            out[0] = (uint8_t)d;
            out[1] = (uint8_t)(d >> 8);
            out[2] = (uint8_t)(d >> 16);
            for (uint32_t i = 4; i < wc; i += 4)
                (void)rd(DSIM_RXFIFO);
            return 3;
        }
        case 0x21:
            out[0] = (uint8_t)(w >> 8);
            return 1;
        case 0x22:
            out[0] = (uint8_t)(w >> 8);
            out[1] = (uint8_t)(w >> 16);
            return 2;
        case 0x02:
            return -2;
        default:
            break;
        }
    }
    return -1;
}

static uint32_t bright_aor(int i)
{
    switch (i) {
    case 0: return 0x000c;
    case 1: return 0x0200;
    case 2: return 0x0440;
    case 3: return 0x0800;
    case 4: return 0x0b7c;
    default: return 0x0ba3;
    }
}

static int bright_msg;          /* 0 none, 1 ok, -1 failed, 2 id read */
static uint8_t bright_id[3];
static int bright_idn;

static void build_bright(void)
{
    struct ub u;
    int ok = dsim_ready();
    for (int i = 0; i < 6; ++i) {
        ub_init(&u);
        ub_s(&u, "AOR ");
        ub_hex(&u, bright_aor(i), 4);
        if (i == 2)
            ub_s(&u, "  (S-Boot normal)");
        row_add(i == 0 ? "Maximum" : i == 1 ? "Bright (~83 %)" : i == 2 ? "Normal (~64 %)" : i == 3 ? "Dim (~31 %)" :
                i == 4 ? "Very dim (~4 %)" : "Darkest (~2 %)",
                u.b, (ok ? 0 : R_DIS) | R_CHECK | (panel_aor == bright_aor(i) ? R_ON : 0));
    }
    ub_init(&u);
    if (bright_idn > 0) {
        ub_s(&u, "read: ");
        for (int i = 0; i < bright_idn; ++i) {
            ub_hexn(&u, bright_id[i], 2);
            ub_c(&u, ' ');
        }
        ub_s(&u, bright_idn == 3 && bright_id[0] == 0x91 ? "(S6E3HA8)" : "");
    } else {
        ub_s(&u, bright_idn < 0 ? "read failed" : "DCS read 0x04 (S-Boot read 91 04 44)");
    }
    row_add("Read panel ID", u.b, ok ? 0 : R_DIS);
    row_add(ok ? (bright_msg < 0 ? "Last write: no answer from DSIM" : "Lasts until an OS starts") : "DSI link not in command mode",
            "AOR only, like S-Boot: no gamma or voltage writes", R_DIS);
}

static int act_bright(int i)
{
    static int asked;
    if (i < 6) {
        /* The one write on the Tests tab (S-Boot's own AOR command, RAM in the panel, gone at the
         * next OS start): asked once per session. */
        if (!asked && !dialog("Brightness", "Send the AOR command to the panel?",
                              "S-Boot's own command; lasts until an OS starts.", "SEND"))
            return 0;
        asked = 1;
        bright_msg = panel_set_aor(bright_aor(i)) ? -1 : 1;
    } else if (i == 6) {
        bright_idn = panel_read_id(bright_id);
        if (bright_idn == 0)
            bright_idn = -1;
    }
    return 0;
}

void bright_page(void)
{
    list_screen("Brightness", build_bright, act_bright);
}

/* ---- reboot and power --------------------------------------------------------------- */

static void big_message(const char *t, const char *s, uint32_t c)
{
    ui_guard(0);
    ui_noclip();
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    ui_text_c(F_TITLE_S, UI_W / 2, 1440, t, c);
    if (s)
        ui_text_c(F_ROW, UI_W / 2, 1540, s, C_L2);
    ui_present();
}

static void power_off(void)
{
    big_message("Powering off" UI_ELLIPSIS, "release all keys", C_STAR);
    sb_power_off();
    /* Still here: a cable keeps the PMIC on (Samsung reboots to LP charging). */
    big_message("Still powered", "charger connected? rebooting to the menu instead", C_ORANGE);
    sb_mdelay(1500);
    sb_reboot(SB_INFORM_NORMAL);
}

static void build_reboot(void)
{
    kv_hdr("LEAVE ENGINEER OS");
    row_add("Back to the boot menu", "reboot: S-Boot, then the Starbite menu", 0);
    row_add("Engineer OS again", "reboot straight back here (boot-next)", 0);
    kv_hdr("OTHER MODES");
    row_add("Reboot to recovery", "TWRP 3.7.0", 0);
    row_add("Reboot to download", "Odin mode", R_WARN);
    row_add("Power off", "PS_HOLD low (with a cable: reboots)", 0);
}

void eng_exit_to_menu(void)
{
    if (!dialog("Exit Engineer OS", "Reboot to the Starbite boot menu?",
                armed_tier() ? "Armed mode ends with the reboot." : "Test history stays in the pmsg ring.",
                "EXIT"))
        return;
    audit_line("exit: reboot to the boot menu");
    big_message("Rebooting" UI_ELLIPSIS, "to the Starbite boot menu", C_STAR);
    sb_boot_next_clear();               /* a stale boot-next would skip the menu */
    sb_reboot(SB_INFORM_NORMAL);
}

static int act_reboot(int i)
{
    if (i == 0) {
        eng_exit_to_menu();
        return 0;
    }
    if (i == 4) {
        if (dialog("Power off", "Turn the phone off now?", 0, "POWER OFF"))
            power_off();
        return 0;
    }
    const char *what = i == 0 ? "Back to the boot menu?" : i == 1 ? "Restart Engineer OS?" : i == 2 ? "Reboot to TWRP?" :
                       "Reboot to Odin mode?";
    if (!dialog("Reboot", what, armed_tier() ? "Armed mode ends with the reboot." : 0, "REBOOT"))
        return 0;
    big_message("Rebooting" UI_ELLIPSIS, 0, C_STAR);
    if (i == 1)
        sb_boot_next("engineer");
    sb_reboot(i == 0 ? SB_INFORM_NORMAL : i == 2 ? SB_INFORM_RECOVERY : SB_INFORM_DOWNLOAD);
}

void reboot_page(void)
{
    list_screen("Reboot and power", build_reboot, act_reboot);
}

/* ---- memory bandwidth: DRAM (non-cacheable window) and cached (arena) -------------------- */

static uint64_t bw_rd, bw_wr, bw_cp, bwc_rd, bwc_wr, bwc_cp;
static int bw_done;

static uint64_t mib_s(uint64_t bytes, uint64_t ticks)
{
    return ticks ? bytes * sb_freq() / ticks / (1U << 20) : 0;
}

static void kv_mibs(const char *k, uint64_t v)
{
    struct ub u;
    ub_init(&u);
    ub_dec(&u, v);
    ub_s(&u, " MiB/s");
    kv_add(k, u.b);
}

void fill_bw(void)
{
    struct ub u;
    if (!bw_done) {
        kv_add("Status", "not run");
        return;
    }
    ub_init(&u);
    ub_dec(&u, bw_rd);
    ub_s(&u, " MiB/s");
    kv_big("DRAM read (non-cacheable, 64-bit)", u.b);
    kv_hdr("DRAM: 16 MIB AT 0x0A0100000, NON-CACHEABLE");
    kv_mibs("Write", bw_wr);
    kv_mibs("Read", bw_rd);
    kv_mibs("Copy (8 MiB)", bw_cp);
    kv_hdr("CACHED: 16 MIB OF THE ARENA, WRITE-BACK");
    kv_mibs("Write", bwc_wr);
    kv_mibs("Read", bwc_rd);
    kv_mibs("Copy (8 MiB)", bwc_cp);
    kv_note("Engineer Lite runs with the MMU off (device memory): its figures are 10-50x lower. One core, "
            "plain 64-bit loads and stores; the A55 at the clock S-Boot left.");
}

static void fill_bw_running(void)
{
    kv_hdr("RUNNING");
    kv_add("Pass", "write, read, copy: DRAM, then cached");
}

static void bw_pass(volatile uint64_t *m, uint64_t n, uint64_t *wr_, uint64_t *rd_, uint64_t *cp_)
{
    uint64_t sum = 0, t = sb_ticks();
    for (uint64_t i = 0; i < n; ++i)
        m[i] = i;
    *wr_ = mib_s(n * 8, sb_ticks() - t);
    t = sb_ticks();
    for (uint64_t i = 0; i < n; ++i)
        sum += m[i];
    *rd_ = mib_s(n * 8, sb_ticks() - t);
    t = sb_ticks();
    for (uint64_t i = 0; i < n / 2; ++i)
        m[n / 2 + i] = m[i];
    *cp_ = mib_s(n * 4, sb_ticks() - t);
    __asm__ volatile("" : : "r"(sum));
}

void run_bw(void)
{
    static volatile uint64_t *cached;
    show_now("Memory bandwidth", fill_bw_running);
    bw_pass((volatile uint64_t *)SB_TEST_BASE, (16UL << 20) / 8, &bw_wr, &bw_rd, &bw_cp);
    if (!cached)
        cached = sb_alloc(16UL << 20);
    if (cached)
        bw_pass(cached, (16UL << 20) / 8, &bwc_wr, &bwc_rd, &bwc_cp);
    bw_done = 1;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "DRAM read ");
    ub_dec(&u, bw_rd);
    ub_s(&u, ", write ");
    ub_dec(&u, bw_wr);
    ub_s(&u, " MiB/s; cached read ");
    ub_dec(&u, bwc_rd);
    test_record("Memory bandwidth", bw_rd > 0, u.b);
    info_screen("Memory bandwidth", fill_bw, 0);
}

/* ---- display panel ----------------------------------------------------------- */

void fill_panel(void)
{
    char v[64];
    struct ub u;
    kv_hdr("PANEL");
    kv_add("Panel", "Samsung S6E3HA8 AMOLED, 5.8\"");
    kv_add("Resolution", "1440 " UI_TIMES " 2960 (18.5:9), 570 ppi");
    if (sb_bootarg("lcdtype", v, sizeof(v))) {
        uint64_t id = 0;
        for (unsigned i = 0; v[i]; ++i)
            id = id * 10U + (uint64_t)(v[i] >= '0' && v[i] <= '9' ? v[i] - '0' : 0);
        ub_init(&u);
        ub_s(&u, v);
        ub_s(&u, "  = ID ");
        ub_hexn(&u, id, 6);
        kv_add("lcdtype (S-Boot)", u.b);
    }
    kv_add("Interface", "MIPI DSI, command mode (DECON_F)");
    kv_hex("DECON_F trigger (0x16030070)", *(volatile uint32_t *)0x16030070UL, 8);
    kv_hdr("FRAMEBUFFER (S-BOOT)");
    kv_hex("Base", SB_FB, 9);
    kv_add("Format", "32 bpp BGRA, 5760 bytes per line");
    ub_init(&u);
    ub_size(&u, (uint64_t)SB_FB_W * SB_FB_H * 4U);
    kv_add("Size", u.b);
    kv_add("Mapping", "write-combined (libsb mmu.c)");
    kv_hdr("ENGINEER OS PRESENTATION");
    kv_add("Back buffer", "write-back RAM, 1440 " UI_TIMES " 2960");
    kv_dec("Frames presented", ui_stats.frames, 0);
    ub_init(&u);
    ub_size(&u, ui_stats.px_last * 4U);
    ub_s(&u, " in ");
    ub_dec(&u, ui_stats.us_last);
    ub_s(&u, " " U_MICRO "s");
    kv_add("Last frame (dirty rects)", u.b);
    ub_init(&u);
    ub_dec(&u, ui_stats.us_worst);
    ub_s(&u, " " U_MICRO "s");
    kv_add("Worst copy", u.b);
    kv_add("Brightness", "Tests > Brightness (AOR)");
}
