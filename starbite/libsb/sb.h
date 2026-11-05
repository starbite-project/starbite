/* SPDX-License-Identifier: GPL-2.0-only
 * libsb, the Starbite core (docs/STARBITE-MASTER-PLAN.md section 4.2, docs/ENGINEER-OS.md).
 *
 * A bare-metal arm64 image entered by the loader's boot_bare() (shim/starlte/xnu_boot.c): EL1,
 * MMU and caches off, interrupts masked, x0 = S-Boot's FDT, the panel scanning out 0xcc000000
 * (DECON_F free-running). crt0.S relocates the image where it was loaded (it is linked at 0 as a
 * PIE: only R_AARCH64_RELATIVE relocations), turns the MMU and caches on (mmu.c), installs the
 * crash vectors and calls sb_main(). Ordinary C with function-pointer tables works here; the
 * loader's "no absolute relocations" rule does not apply to images built on libsb.
 *
 * Everything in libsb reads hardware. The writes it makes are the ones the loader itself makes on
 * every boot (ACPM IPC, the RTC read latch, I2C controller setup, the touch rails with the
 * kernel's checks) and the reboot / power-off / boot-next mailbox paths. It never writes a CMU
 * block, the MIF/INT clocks, DREX/DMC, PMU DVFS registers, fuses or the ACPM/APM SRAM outside the
 * IPC queue protocol, and it has no storage write path at all: sb_write() (fence.c) only logs
 * what it would write and refuses (v1 is dry-run).
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* ---- memory map (physical = virtual, mmu.c) ------------------------------------------------ */

#define SB_LOAD_MIN   0x92000000UL      /* boot_bare loads at 0x92000000 + text_offset */
#define SB_ARENA_END  0xa0000000UL      /* bump allocator: image end .. here (write-back) */
#define SB_TEST_BASE  0xa0100000UL      /* RAM test window, non-cacheable (DRAM, not caches); starts above
                                          * S-Boot's ECT at 0xa0000000 (+0x32000), which Android's kernel reads */
#define SB_TEST_SIZE  (126UL << 20)      /* to 0xa7f00000: inside the non-cacheable window */
/* The RAM test and bandwidth windows write here. Never below 0xa0100000 (S-Boot's ECT at
 * 0xa0000000 must stay intact for the next OS) and never outside the non-cacheable window
 * 0xa0000000..0xa8000000 that mmu.c maps for it (POST's STAGE 0x8c000000 is far away). */
_Static_assert(SB_TEST_BASE >= 0xa0100000UL, "RAM test window overlaps S-Boot's ECT at 0xa0000000");
_Static_assert(SB_TEST_BASE + SB_TEST_SIZE <= 0xa8000000UL, "RAM test window leaves the non-cacheable map");
_Static_assert(SB_ARENA_END <= 0xa0000000UL, "the arena must end below S-Boot's ECT");
#define SB_DMA_BASE   0xaf000000UL      /* UFS DMA + read scratch, non-cacheable (ufs.h) */
#define SB_DMA_END    0xaf800000UL
#define SB_FB         0xcc000000UL      /* S-Boot's scanout buffer, write-combined */
#define SB_FB_W       1440U
#define SB_FB_H       2960U
#define SB_LOADER_LOG 0x91fff000UL      /* the loader's log page (xnu_boot.S) */
#define SB_PMSG       0xFED14000UL      /* ramoops pmsg ring ("DBGC" header) */
#define SB_NEXT_MBOX  0xFED17FE0UL      /* boot-next mailbox: u64 "SBNEXT2!" + char key[24] */

/* ---- entry ---------------------------------------------------------------------------------- */

struct sb_boot {
    uint64_t fdt;                       /* x0 from the loader */
    uint64_t base;                      /* where the image runs */
    uint64_t image_size;                /* header image_size */
    int64_t relocs;                     /* R_AARCH64_RELATIVE entries applied, < 0 bad entry */
    uint64_t el, sctlr_in, cpacr_in, vbar_in, daif_in;
    uint64_t loader_tag, loader_entry;  /* loader log page: "STRLTE01", entry it recorded */
};
extern struct sb_boot sb_boot;

/* The application's entry, called once libsb is up (never returns). */
void sb_main(void);

/* ---- time --------------------------------------------------------------------------------- */

uint64_t sb_ticks(void);
uint64_t sb_freq(void);                 /* CNTFRQ, 26 MHz when S-Boot left it 0 */
uint64_t sb_us(void);                   /* since power-on (counter) */
uint64_t sb_ms(void);
void sb_udelay(uint64_t us);
void sb_mdelay(unsigned ms);

/* ---- strings and memory ------------------------------------------------------------------- */

void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
unsigned sb_strlen(const char *s);
int sb_streq(const char *a, const char *b);
int sb_prefix(const char *s, const char *pre);
void sb_strcpy(char *d, unsigned cap, const char *s);

/* Small string builder (no libc). */
struct ub {
    char b[256];
    unsigned n;
};
void ub_init(struct ub *u);
void ub_s(struct ub *u, const char *s);
void ub_c(struct ub *u, char c);
void ub_dec(struct ub *u, uint64_t v);
void ub_sdec(struct ub *u, int64_t v);
void ub_hex(struct ub *u, uint64_t v, unsigned digits);   /* "0x" + digits */
void ub_hexn(struct ub *u, uint64_t v, unsigned digits);  /* digits only */
void ub_dec2(struct ub *u, unsigned v);                   /* two digits, zero padded */
void ub_fix(struct ub *u, uint64_t milli, unsigned decimals);  /* 1234 -> "1.234" */
void ub_size(struct ub *u, uint64_t bytes);               /* "16 MiB", "2 GiB" */

/* Bump allocator over [image end, SB_ARENA_END): write-back RAM, 64-byte aligned, never freed. */
void *sb_alloc(uint64_t size);
uint64_t sb_arena_used(void), sb_arena_free(void);

uint32_t sb_crc32(const void *p, uint64_t n);

/* ---- caches (mmu.c) ------------------------------------------------------------------------- */

void sb_mmu_enable(void);
int sb_mmu_on(void);
void sb_dcache_clean_inval(const volatile void *addr, uint64_t len);
void sb_dcache_inval(const volatile void *addr, uint64_t len);
/* What the identity map says about a physical address: 0 unmapped, 1 device, 2 write-back,
 * 3 non-cacheable. */
int sb_map_kind(uint64_t pa);

/* ---- faults (crt0.S vectors) ----------------------------------------------------------------- */

/* While sb_probe_depth > 0, a synchronous abort or trapped instruction is recorded in sb_fault
 * and skipped (ELR + 4); otherwise it is a crash (crash.c). */
struct sb_fault {
    uint64_t esr, elr, far, count, serror_esr, serror_count;
};
extern volatile struct sb_fault sb_fault;
extern volatile uint64_t sb_probe_depth;
/* 32-bit load that survives a synchronous abort: 1 read, 0 faulted. */
int sb_probe32(uint64_t addr, uint32_t *v);
/* System register reads that may trap (armed by the caller with sb_probe_depth). */
#define SB_SAFE_MRS(reg, out)                                                         \
    ({                                                                                \
        uint64_t _c = sb_fault.count, _v;                                             \
        sb_probe_depth++;                                                             \
        __asm__ volatile("mov %0, xzr\n\tmrs %0, " #reg : "=&r"(_v) : : "memory");    \
        sb_probe_depth--;                                                             \
        *(out) = _v;                                                                  \
        sb_fault.count == _c;                                                         \
    })
void sb_crash_show(const char *why, const uint64_t *regs);   /* never returns */

/* ---- log ring (log.c) -------------------------------------------------------------------------
 * In RAM for this session, every line also appended to the pmsg ring (TWRP shows it as
 * /sys/fs/pstore/pmsg-ramoops-0 after a warm reset, and Engineer OS reads its own lines back). */
void sb_log(const char *s);
void sb_logf(const char *a, const char *b);                 /* a + b */
void sb_logx(const char *a, uint64_t v);                    /* a + hex */
unsigned sb_log_count(void);
const char *sb_log_line(unsigned i);                         /* oldest first */
void sb_pmsg_put(const char *s);                             /* raw, no prefix, no newline */

/* ---- keys (ALIVE GPIOs, active low) ------------------------------------------------------------ */

#define SB_K_UP 1U
#define SB_K_DOWN 2U
#define SB_K_BIXBY 4U
#define SB_K_POWER 8U
unsigned sb_keys(void);
void sb_keys_release_wait(void);

/* ---- reset, power, boot-next ------------------------------------------------------------------- */

#define SB_INFORM_NORMAL   0x12345670U
#define SB_INFORM_DOWNLOAD 0x12345671U
#define SB_INFORM_RECOVERY 0x12345674U
#define SB_PMU 0x14060000UL
void sb_reboot(uint32_t inform3) __attribute__((noreturn));
/* PS_HOLD low. With a cable in the PMIC keeps the phone on: returns after 3 s. */
void sb_power_off(void);
/* Write the boot-next mailbox (the loader starts key without the menu, once) and reset. */
void sb_boot_next(const char *key) __attribute__((noreturn));
/* The mailbox as it is now (another OS may have left it): 1 and the key if valid. */
int sb_boot_next_peek(char key[24]);
/* Invalidate a pending boot-next (the magic), so the next reset shows the menu. */
void sb_boot_next_clear(void);

/* ---- PSCI (SMC, PSCI 0.1 function ids of the starlte DTB) ------------------------------------- */

int64_t sb_psci(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3);
#define SB_PSCI_CPU_ON 0xc4000003U
#define SB_PSCI_CPU_OFF 0x84000002U
extern char sb_core_entry[];            /* crt0.S: MMU-off stub, records MPIDR, CPU_OFF */
extern volatile uint64_t sb_core_box[32];   /* 8 slots: state, mpidr, el, cpu_off result */

/* ---- FDT (fdt.c) ---------------------------------------------------------------------------- */

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);
int fdt_chosen_raw(uintptr_t fdt, const char *prop, uintptr_t *val, uint32_t *len);
int fdt_memory_banks(uintptr_t fdt, uint64_t *base, uint64_t *size, int max);
/* /reserved-memory children with a reg: name (cut to 31), base, size. */
int fdt_reserved(uintptr_t fdt, char (*name)[32], uint64_t *base, uint64_t *size, int max);
/* S-Boot's bootargs: copy the value of "key=" (0 if absent). */
int sb_bootarg(const char *key, char *out, unsigned cap);

/* ---- PMIC: S2MPS18 through ACPM (acpm.c) -------------------------------------------------------
 * type 0 TOP, 1 PMIC, 2 RTC. Reads go through the ACPM IPC queue exactly as the loader and the
 * kernel send them. Writes are limited to what libsb itself needs (the RTC read latch and the
 * touch rails with the kernel's checks): there is no general PMIC write in libsb. */
int sb_pmic_read(uint32_t type, uint32_t reg, uint8_t *val);
int sb_acpm_build(char out[25]);
/* RTC (S2MPS18 RTC block): latched read. */
struct sb_rtc {
    unsigned y, mo, d, h, mi, s, ctrl;
    int ok;
};
void sb_rtc_read(struct sb_rtc *r);
int sb_rtc_epoch(uint64_t *epoch);
/* Touch rails LDO35 1.8 V / LDO43 3.0 V, read only: 1 on, 0 off or unexpected, -1 ACPM. */
int sb_touch_rails(uint8_t *l35, uint8_t *l43);
/* microSD rails LDO31 / LDO2, read only: 1 on in the card's range, 0 off, -1 ACPM. */
int sb_sd_rails(uint8_t *l31, uint8_t *l2);

/* ---- HSI2C (hsi2c.c): auto-mode messages on an Exynos USI HSI2C ---------------------------- */

struct sb_i2c {
    uintptr_t base;
    uint32_t fs1, fs2, fs3;             /* timing saved from S-Boot */
    int state;                          /* 0 untouched, 1 up, -1 no answer */
};
void sb_i2c_bus(struct sb_i2c *b);
int sb_i2c_msg(struct sb_i2c *b, unsigned addr, int receive, uint8_t *bytes, unsigned count, int stop);
int sb_i2c_read(struct sb_i2c *b, unsigned addr, uint8_t reg, uint8_t *data, unsigned len);

/* ---- touch: S6SY761 (touch.c) ------------------------------------------------------------------ */

struct sb_touch_info {
    int ok;
    const char *why;
    uint8_t l35, l43, status, id[3];
    unsigned max_x, max_y, tx, rx;
};
extern struct sb_touch_info sb_tp;
int sb_touch_start(void);
/* One finger-0 event: *st 1 down, 2 move, 3 up, panel coordinates. 0 when none. */
int sb_touch_event(int *st, int *x, int *y);
/* Every finger: up to 10 (id, state, x, y). */
int sb_touch_events(int *id, int *st, int *x, int *y, int max);
int sb_touch_read(uint8_t reg, uint8_t *data, unsigned len);
void sb_touch_clear(void);

/* ---- MAX77705 fuel gauge and charger (max77705.c), read only ------------------------------------ */

struct sb_bat {
    int ok;                             /* bus answered */
    int soc_pct;                        /* RepSOC, % */
    int soc_x10;                        /* RepSOC, 0.1 % */
    int mv, avg_mv, ma, avg_ma;         /* VCELL, AvgVCELL, Current, AvgCurrent (+ = charging) */
    int input;                          /* 0 none, 1 USB, 2 wireless */
    int chg_dtls;                       /* CHG_DTLS */
};
int sb_max_init(void);
int sb_max_id(uint8_t *id, uint8_t *rev);
int sb_fg_word(uint8_t reg, uint16_t *v);
int sb_chg_read(uint8_t reg, uint8_t *v);
int sb_usbc_read(uint8_t reg, uint8_t *v);
int sb_bat_read(struct sb_bat *b);
extern struct sb_i2c sb_max_bus;
extern const char *sb_max_why;

/* ---- TMU and clocks, read only (tmu.c) --------------------------------------------------------- */

#define SB_TMU_TOP 0x10070000UL
#define SB_TMU_SUB 0x10074000UL
int sb_tmu_state(uintptr_t base);           /* 0 off, 1 running, 2 emulated */
int sb_tmu_temp(uintptr_t base, unsigned probe, unsigned *flags);
struct sb_temps {
    int top, sub;                           /* tmu states */
    int big[5], big_max, little, gpu, isp, hottest;
    unsigned flags;
};
void sb_temps_read(struct sb_temps *t);
uint64_t sb_pll_hz(uintptr_t con0, int *unlocked);
uint64_t sb_a55_hz(void);
uint64_t sb_m3_hz(int *off);                /* *off = 1: cluster powered off */
uint64_t sb_gpu_hz(int *off);
int sb_m3_on(void);
int sb_g3d_on(void);

/* ---- write fence (fence.c), dry-run in v1 ------------------------------------------------------- */

#define SB_MED_UFS 0U
#define SB_MED_SD 1U
#define SB_W_DRYRUN  -100                   /* v1: logged, refused */
#define SB_W_NEVER   -101                   /* never-touch target */
#define SB_W_TIER    -102                   /* needs a higher tier */
#define SB_W_RANGE   -103                   /* not inside one partition / unknown */
struct sb_wdecide {
    int tier;                               /* 0 read only, 1, 2, 3 never */
    char part[24];                          /* GPT partition (UFS) or "card" */
    uint64_t first, last;                   /* the partition's blocks */
    const char *why;
};
/* What a write of n 4 KiB blocks at lba would need (no write happens). */
int sb_write_decide(unsigned medium, uint64_t lba, uint64_t n, struct sb_wdecide *d);
/* The only write entry point of libsb: checks, logs "would write", returns SB_W_*. */
int sb_write(unsigned medium, uint64_t lba, uint64_t n, const void *src, int armed_tier);
/* Never-touch partition names (EFS, CPEFS, PARAM, ...): 1 if name is one. */
int sb_never_part(const char *name);
