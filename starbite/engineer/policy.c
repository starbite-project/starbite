/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Lab policy (docs/ENGINEER-OS.md section 8): which tier of confirmation each write needs.
 * Pure (no hardware access, no UI): starbite/tools/unit-test.c checks it on the host.
 *
 * Nothing is refused because it is risky; a riskier target needs more confirmation:
 *   tier 1  warning screen + held confirmation (slide, or hold Volume Up 1.5 s)
 *   tier 2  + the target's name typed on the keypad
 *   tier 3  + a second confirmation (hold Power 3 s); storage also needs a verified backup first
 *   tier 4  (extreme, v2.1) a full-screen red warning naming the exact consequence and how to
 *           recover, "<target> I ACCEPT" typed, the slide, Power held 5 s; storage: a verified
 *           backup first unless "no backup" is picked on a second red screen
 * Tier 4 is everything v2.0 refused as policy: S-Boot's ECT at 0xA0000000 (Android's kernel reads
 * it), the running Engineer OS (0x92000000..0x9fffffff: Lab crashes at once), the secure world
 * (0xAF800000..0xBFFFFFFF) and every address Engineer OS leaves unmapped (the access is attempted
 * through a temporary mapping and its fault is caught and reported), PMIC blocks Starbite does not
 * know, ACPM DVFS outside the GPU's vendor table, raw LU writes across partitions (fence.c).
 * TIER_NO is left for what cannot be expressed: an address above the 36-bit space Engineer OS's
 * MMU covers, a PMIC register the ACPM command cannot carry (4-bit type, 8-bit register).
 *
 * Tier 3 (the user's "most dangerous" list): the PMU (power, reset, DVFS), CMU_TOP / CORE / BUSC
 * (MIF and INT clocks), the MIF block (CMU_MIF, DREX / DMC, DDR PHY), the CPU cluster blocks
 * (CMU_CPUCL0/1, DVFS), CMU_G3D (GPU clock), the ACPM / APM mailbox and APM SRAM, CHIPID / fuses,
 * the TMU (S-Boot's 115 C hardware trip), every PMIC regulator / BUCK / TOP register except the
 * touch and card rails at the kernel's values and vdd_g3d up to 950 mV, every MAX77705 register
 * except the charger watchdog feed, DRAM carve-outs that may be TZASC-protected (secure camera,
 * modem), the GPU's DVFS levels from the vendor table.
 * Tier 2: the other CMU blocks (peripheral clock gates: a closed gate can stall the bus), the UFS
 * host and microSD controller (the disks), the watchdog, the GIC, the always-on GPIOs (the keys),
 * SYSREGs, the Mali GPU's registers (its jobs write memory by DMA), any device address not listed,
 * the UFS DMA window.
 * Tier 1: known peripherals (pin controllers, I2C / USI buses, DECON / DSIM), free DRAM, the
 * framebuffer, pmsg and the boot-next mailbox, vdd_g3d between S-Boot's value and 950 mV.
 */
#include "eng.h"

struct range {
    uint64_t a, n;
    int tier;
    const char *why;
};

#define KB(x) ((uint64_t)(x) << 10)
#define MB(x) ((uint64_t)(x) << 20)

/* First match wins: specific entries before the blocks that contain them. */
static const struct range regs[] = {
    /* tier 3 */
    { 0x02000000ULL, MB(1), TIER3, "APM SRAM (ACPM firmware, DVFS tables)" },
    { 0x10000000ULL, KB(64), TIER3, "CHIPID / OTP fuses" },
    { 0x10070000ULL, KB(64), TIER3, "TMU (S-Boot's 115 C hardware thermal trip)" },
    { 0x14060000ULL, KB(64), TIER3, "PMU (power domains, reset, DVFS, CPU power)" },
    { 0x14100000ULL, KB(64), TIER3, "ACPM/APM mailbox" },
    { 0x17400000ULL, KB(64), TIER3, "CMU_G3D (GPU clock, DVFS)" },
    { 0x1a242070ULL, 4, TIER2, "CMU_TOP gate CLKCMU_FSYS1_MMC_CARD (the card clock only)" },
    { 0x1a000000ULL, MB(4), TIER3, "CMU_TOP / CORE / BUSC (MIF and INT clocks)" },
    { 0x1b800000ULL, MB(8), TIER3, "MIF: CMU_MIF, DREX / DMC, DDR PHY" },
    { 0x1d000000ULL, MB(2), TIER3, "CPU clusters: CMU_CPUCL0/1, DVFS" },
    /* tier 2 */
    { 0x10020000ULL, KB(64), TIER2, "CMU_PERIS (peripheral clock gates)" },
    { 0x10400000ULL, KB(64), TIER2, "CMU_PERIC0 (peripheral clock gates)" },
    { 0x10800000ULL, KB(64), TIER2, "CMU_PERIC1 (peripheral clock gates)" },
    { 0x11000000ULL, KB(64), TIER2, "CMU_FSYS0 (UFS clocks)" },
    { 0x11400000ULL, KB(64), TIER2, "CMU_FSYS1 (card clocks)" },
    { 0x14000000ULL, KB(64), TIER2, "CMU_APM" },
    { 0x14200000ULL, KB(64), TIER2, "CMU_CMGP (battery bus, ADC clocks)" },
    { 0x16000000ULL, KB(64), TIER2, "CMU_DPU (display clocks)" },
    { 0x17c00000ULL, KB(64), TIER2, "CMU_AUD (audio clocks)" },
    { 0x10050000ULL, KB(64), TIER2, "watchdog (a wrong value resets the phone)" },
    { 0x10100000ULL, MB(1), TIER2, "GIC (interrupt controller)" },
    { 0x11100000ULL, MB(1), TIER2, "FSYS0: UFS host and PHY (the boot disk)" },
    { 0x11500000ULL, KB(64), TIER2, "DWMMC2 (microSD controller)" },
    { 0x14050000ULL, KB(64), TIER2, "GPIO_ALIVE (the keys: a wrong value can stop them answering)" },
    { 0x14210000ULL, KB(64), TIER2, "SYSREG_CMGP" },
    { 0x17500000ULL, KB(64), TIER2, "Mali-G72 GPU (its jobs write memory by DMA)" },
    /* tier 1 */
    { 0x10430000ULL, KB(4), TIER1, "GPIO_PERIC0 (pins)" },
    { 0x104b0000ULL, KB(4), TIER1, "HSI2C USI03 (touch bus)" },
    { 0x11430000ULL, KB(4), TIER1, "GPIO_FSYS1 (card pins)" },
    { 0x14220000ULL, KB(4), TIER1, "GPIO_CMGP (pins)" },
    { 0x14230000ULL, KB(4), TIER1, "ADC_CMGP (thermistor ADC)" },
    { 0x14360000ULL, KB(4), TIER1, "HSI2C USI_CMGP03 (MAX77705 bus)" },
    { 0x16030000ULL, KB(64), TIER1, "DECON_F (display: a reboot restores it)" },
    { 0x16080000ULL, KB(64), TIER1, "DSIM (display link: a reboot restores it)" },
};

static const struct range dram[] = {
    { 0x8a000000ULL, MB(1), TIER2, "S-Boot's device tree (Engineer OS and every OS read it)" },
    { 0x92000000ULL, 0xa0000000ULL - 0x92000000ULL, TIER4,
      "Engineer OS itself (code, stack, screen buffers): Lab crashes at once" },
    { 0xa0000000ULL, 0xa0100000ULL - 0xa0000000ULL, TIER4,
      "S-Boot's ECT: Android's kernel reads it at boot; a bad ECT can make Android panic" },
    { 0xa0100000ULL, SB_TEST_SIZE, TIER1, "the RAM test window (free)" },
    { 0xaf000000ULL, MB(8), TIER2, "UFS DMA buffers (a write during a transfer corrupts it)" },
    { 0xaf800000ULL, 0xc0000000ULL - 0xaf800000ULL, TIER4,
      "secure world (RKP, TIMA): mapped for this one access; TrustZone faults it" },
    { 0xe1900000ULL, 0xe3700000ULL - 0xe1900000ULL, TIER3, "secure camera carve-out (TZASC: may raise an SError)" },
    { 0xf0000000ULL, 0xf9200000ULL - 0xf0000000ULL, TIER3, "modem carve-out (TZASC: may raise an SError)" },
    { 0xcc000000ULL, MB(17), TIER1, "the framebuffer (the screen)" },
    { 0xfed14000ULL, KB(16), TIER1, "pmsg ring and the boot-next mailbox" },
    { SB_GPU_MEM, SB_GPU_MEM_SIZE, TIER1, "the GPU tests' memory (page tables, jobs, pixels)" },
};

static int lookup(const struct range *t, unsigned n, uint64_t a, const char **why)
{
    for (unsigned i = 0; i < n; ++i)
        if (a >= t[i].a && a - t[i].a < t[i].n) {
            *why = t[i].why;
            return t[i].tier;
        }
    return -1;
}

#define ADDR_LIMIT (64ULL << 30)    /* the 36-bit space of Engineer OS's MMU (TCR T0SZ = 28) */

int poke_tier(uint64_t a, const char **why)
{
    const char *w = "?";
    int t;
    if (a >= ADDR_LIMIT) {
        t = TIER_NO;
        w = "above 64 GiB: outside the 36-bit address space Engineer OS's MMU covers";
    } else if (!sb_map_kind(a)) {
        t = TIER4;
        w = (a >= 0xaf800000ULL && a < 0xc0000000ULL)
                ? "secure world (RKP, TIMA): mapped for this one access; TrustZone faults it"
                : "unmapped: no memory or device Starbite knows; mapped for this one access, expect a bus fault";
    } else if (a < 0x80000000ULL) {
        t = lookup(regs, sizeof(regs) / sizeof(regs[0]), a, &w);
        if (t < 0) {
            t = TIER2;
            w = "a device address Starbite does not know (may stall the bus)";
        }
    } else {
        t = lookup(dram, sizeof(dram) / sizeof(dram[0]), a, &w);
        if (t < 0) {
            t = TIER1;
            w = a >= 0x880000000ULL ? "DRAM bank 2 (free)" : "DRAM (free: nothing of Engineer OS)";
        }
    }
    if (why)
        *why = w;
    return t;
}

/* What happens and how to recover, for the warning screen. */
const char *poke_undo(uint64_t a)
{
    const char *w;
    int t = poke_tier(a, &w);
    if (t == TIER_NO)
        return "-";
    if (a >= 0x92000000ULL && a < 0xa0000000ULL)
        return "none: Lab crashes or misbehaves at once; if the screen freezes, hold Power + Volume Down "
               "10 s (the PMIC resets the phone) and the loader starts again";
    if (a >= 0xa0000000ULL && a < 0xa0100000ULL)
        return "back it up first (offered next). S-Boot copies the ECT from flash when it starts, so a "
               "full restart most likely brings it back (not verified); do not start Android before that";
    if (t == TIER4)
        return "nothing is written if it faults (Lab reports the fault); if the phone resets instead, it "
               "comes back to the loader with nothing changed on storage";
    if (a >= 0x80000000ULL)
        return "a reboot reloads what needs this memory";
    if (t == TIER3)
        return "a reboot resets it, if the phone survives the write (it can hang or power off at once)";
    return "a reboot resets every register (Undo writes the old value back now)";
}

/* The highest tier over [a, a + len) in 4 KiB steps (the tables are 4 KiB granular). */
int range_tier(uint64_t a, uint64_t len, const char **why)
{
    int worst = -1;
    const char *w = "empty range";
    if (!len || a + len < a) {
        if (why)
            *why = w;
        return TIER_NO;
    }
    uint64_t end = a + len, p = a & ~0xfffULL;
    for (; p < end; p += 0x1000) {
        const char *x;
        int t = poke_tier(p < a ? a : p, &x);
        if (t > worst) {
            worst = t;
            w = x;
        }
        if (worst == TIER_NO)
            break;
    }
    if (why)
        *why = w;
    return worst;
}

/* Reads: tier 0 inside the blocks known to answer without side effects (em.c peek_safe) and DRAM
 * outside the carve-outs; tier 1 for any other mapped address (some registers clear on read, an
 * unclocked block can stall the bus); tier 4 for unmapped addresses (attempted through a
 * temporary mapping: TrustZone or the bus faults it, Lab reports the fault). */
int read_tier(uint64_t a, const char **why)
{
    const char *w = "free to read";
    int t = TIER0;
    if (a >= ADDR_LIMIT) {
        t = TIER_NO;
        w = "above 64 GiB: outside the 36-bit address space Engineer OS's MMU covers";
    } else if (!sb_map_kind(a)) {
        t = TIER4;
        w = "unmapped (secure world or nothing): the read is attempted through a temporary mapping";
    } else if (a >= 0x80000000ULL && a < 0x100000000ULL) {
        if ((a >= 0xe1900000ULL && a < 0xe3700000ULL) || (a >= 0xf0000000ULL && a < 0xf9200000ULL)) {
            t = TIER1;
            w = "carve-out (TZASC): the read may raise an SError";
        }
    } else if (a >= 0x880000000ULL && a < 0x900000000ULL) {
        t = TIER0;
    } else {
        switch (a & ~0xffffULL) {
        case 0x10000000: case 0x10050000: case 0x10070000:
        case 0x10400000: case 0x10410000: case 0x10430000: case 0x104b0000:
        case 0x11120000: case 0x11430000: case 0x11500000:
        case 0x14050000: case 0x14060000:
        case 0x14200000: case 0x14210000: case 0x14220000: case 0x14360000:
        case 0x16030000: case 0x16080000:
        case 0x1a240000: case 0x1b800000: case 0x1d000000:
            break;
        default:
            t = TIER1;
            w = "some registers clear on read; an unclocked block can stall the bus";
            break;
        }
    }
    if (why)
        *why = w;
    return t;
}

int peek_allowed(uint64_t a)
{
    return read_tier(a, 0) == TIER0;
}

/* ---- PMIC (S2MPS18 via ACPM), MAX77705, ACPM DVFS ------------------------------------------------
 * Tier 1 only for the exact values the kernel (and the loader's menu) uses on the peripheral
 * rails (shim/starlte/menu.c tp_power, hw_sd_rails): touch LDO35 = 0xEC (on, 1.8 V), LDO43 = 0xF0
 * (on, 3.0 V); card LDO31 = 0x28 / 0xE8 (off / on, 2.8 V) and LDO2 = off / 0xFC (on, 3.3 V); the
 * RTC block (time, UPDATE latch); vdd_g3d (BUCK6, reg 0x2B, 300 mV + n x 6.25 mV) from 0x30
 * (600 mV, around S-Boot's 619-644 mV) to 0x68 (950 mV: what Linux's s9p-g3d and Starbite's iOS
 * run the Mali at; jobs faulted below it). Any other value or register is tier 3: a regulator,
 * BUCK or TOP register can brown out the SoC or damage a part before anything can be undone.
 * ACPM types 3..15 are blocks Starbite does not know: tier 4. */
int pmic_tier(unsigned type, unsigned reg, unsigned val, const char **why)
{
    const char *w;
    int t = TIER3;
    if (type > 15 || reg > 0xff || val > 0xff) {
        t = TIER_NO;
        w = "not addressable: the ACPM PMIC command has a 4-bit type, 8-bit register and value";
    } else if (type > 2) {
        t = TIER4;
        w = "a PMIC block Starbite does not know: the APM firmware decides where it goes";
    } else if (type == 2) {
        t = TIER1;
        w = "RTC block (the clock; the next OS reads it)";
    } else if (type == 1 && ((reg == 0x6c && val == 0xec) || (reg == 0x74 && val == 0xf0))) {
        t = TIER1;
        w = "touch rail at the kernel's value";
    } else if (type == 1 && ((reg == 0x68 && (val == 0xe8 || val == 0x28)) ||
                             (reg == 0x4a && (val == 0xfc || !(val & 0xc0U))))) {
        t = TIER1;
        w = "microSD rail at the kernel's value";
    } else if (type == 1 && reg == 0x2b && val >= 0x30 && val <= 0x68) {
        t = TIER1;
        w = "vdd_g3d (BUCK6) between S-Boot's value and 950 mV, the GPU's known-good voltage";
    } else if (type == 1 && reg == 0x2b) {
        w = "vdd_g3d (BUCK6) outside 600..950 mV: never run on this phone";
    } else if (type == 0) {
        w = "PMIC TOP (power sequencing, interrupts, power-off)";
    } else {
        w = "a regulator, BUCK or other PMIC register (voltages)";
    }
    if (why)
        *why = w;
    return t;
}

int max_tier(unsigned addr7, unsigned reg, unsigned val, const char **why)
{
    const char *w;
    int t = TIER3;
    if (addr7 > 0x7f || reg > 0xff || val > 0xff) {
        t = TIER_NO;
        w = "not addressable on I2C (7-bit address, 8-bit register and value)";
    } else if (addr7 == 0x69 && reg == 0xbd && (val & 3U) == 1U && !(val & ~0x0fU)) {
        t = TIER1;
        w = "charger watchdog feed (CNFG_06 WDTCLR = 01, as Linux does)";
    } else if (addr7 == 0x69 && reg == 0xb0) {
        w = "CHG_INT is clear-on-read status: the chip should ignore a write (not read back)";
    } else if (addr7 == 0x69) {
        w = "charger configuration (mode, currents, voltages)";
    } else if (addr7 == 0x36) {
        w = "fuel-gauge model (persists while the battery is connected)";
    } else if (addr7 == 0x25) {
        w = "USB-C controller (the CCIC firmware's command mailbox)";
    } else if (addr7 == 0x66) {
        w = "MAX77705 PMIC block (reset, main control)";
    } else {
        w = "MAX77705 debug or unknown address";
    }
    if (why)
        *why = w;
    return t;
}

/* The GPU's DVFS levels (vendor exynos9810.dtsi gpu_dvfs_table, kHz). 260000 is S-Boot's clock;
 * 572000 ran for days under Starbite's iOS (xnu 0303, vdd_g3d 950 mV); the others never ran here. */
const unsigned gpu_levels_khz[GPU_NLEVELS] = { 260000, 299000, 338000, 455000, 546000, 572000 };

int dvfs_tier(unsigned domain, unsigned khz, const char **why)
{
    const char *w = "a DVFS level outside the GPU's vendor table (or another domain: MIF froze the phone on iOS)";
    int t = TIER4;
    if ((domain == 2 || domain == 3) && sb_dvfs_level_listed(domain, khz)) {
        t = TIER3;
        w = "a CPU cluster's clock through ACPM DVFS, a level of the vendor table (2.2: the CPU stress's full clocks)";
    }
    if (domain == 4)
        for (unsigned i = 0; i < GPU_NLEVELS; ++i)
            if (gpu_levels_khz[i] == khz) {
                t = TIER3;
                w = "the GPU clock through ACPM DVFS, a level of the vendor table";
            }
    if (why)
        *why = w;
    return t;
}

const char *tier_name(int t)
{
    return t == TIER0 ? "tier 0" : t == TIER1 ? "tier 1" : t == TIER2 ? "tier 2" : t == TIER3 ? "tier 3" :
           t == TIER4 ? "tier 4 (extreme)" : "impossible";
}
