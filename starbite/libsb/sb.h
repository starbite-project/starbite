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
 * libsb reads hardware freely. The writes it makes on its own are the ones the loader makes on
 * every boot (ACPM IPC, the RTC read latch, I2C controller setup) and the reboot / power-off /
 * boot-next mailbox paths. Every other write (storage, PMIC, MAX77705, and the application's
 * register pokes) needs an authorisation the user confirmed on the phone, at the tier the target
 * needs (fence.c: the gate). Storage writes go through sb_write only; the UFS and SD drivers refuse
 * a write that does not come from it.
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
/* Tier 4 only (engineer/armed.c, after the confirmation): map the block that holds pa (2 MiB in
 * 0x80000000..0xbfffffff, else 1 GiB; below 64 GiB) as Device-nGnRnE for one attempted access, so
 * the access reaches the bus and TrustZone / the interconnect answers it. 1 mapped (call
 * sb_mmu_unmap_temp afterwards), 0 already mapped, -1 not mappable. */
int sb_mmu_map_temp(uint64_t pa);
void sb_mmu_unmap_temp(void);

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
/* Called once at the start of sb_crash_show, interrupts masked: an image puts hardware it drives into a
 * safe state (wifitest: the radio's PERST# and WLAN_REG_ON low). Plain register writes only. */
extern void (*sb_crash_hook)(void);

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
/* A fuel-gauge word read up to 3 times when the bus fails; with lo < hi, a value outside
 * [lo, hi] is read again and, if it stays outside, rejected (0). Counted for the pages: the
 * gauge's I2C reads used to turn a failed read into "0 mV". */
int sb_fg_word_checked(uint8_t reg, uint16_t *v, unsigned lo, unsigned hi);
struct sb_fg_stat {
    uint32_t reads, retries, failed, rejected;
    int last_bad;
};
extern struct sb_fg_stat sb_fg_stat;
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
    int top_all[8], sub_all[3];             /* every probe (TOP 0..7, SUB 0..2), -100 off or no valid sample */
    int code_top[8], code_sub[3];           /* the last accepted CURRENT_TEMP code */
    int trip_c, trip_sub_c;                 /* hardware trip (THD_TEMP0 rise) decoded, -100 not armed */
    int trip_armed;                         /* bit 0 TOP, bit 1 SUB: CONTROL bit 12 (THERM_TRIP_EN) set */
};
/* Every probe is the median of 3 samples; code 0 / 0x1ff and temperatures outside -30..150 C are
 * rejected and counted (sb_tmu_stats: slot = probe for TOP, 8 + probe for SUB). */
void sb_temps_read(struct sb_temps *t);
void sb_tmu_stats(unsigned slot, uint32_t *reads, uint32_t *rejected, int *last_bad_code);
uint64_t sb_pll_hz(uintptr_t con0, int *unlocked);
uint64_t sb_a55_hz(void);
uint64_t sb_m3_hz(int *off);                /* *off = 1: cluster powered off */
uint64_t sb_gpu_hz(int *off);
int sb_m3_on(void);
int sb_g3d_on(void);

/* ---- write fence and authorisation gate (fence.c) ----------------------------------------------
 * Tiers: 0 read, 1 warning + held confirmation, 2 + the target's name typed, 3 + a second
 * confirmation and, for storage, a verified backup first, 4 (extreme) + a full-screen red warning
 * naming the consequence and the recovery, a typed phrase ("<target> I ACCEPT") and Power held
 * 5 s; for storage a verified backup first unless the user picks "no backup" on a second red
 * screen. Tier 4 is what v2.0 refused: S-Boot's ECT, the running Engineer OS, the secure world and
 * unmapped addresses (the access is attempted and its fault caught), raw LU writes across
 * partition boundaries. SB_TIER_NO is left only for what cannot be expressed or does not exist
 * (docs/ENGINEER-OS.md section 8.1). */
#define SB_TIER0 0
#define SB_TIER1 1
#define SB_TIER2 2
#define SB_TIER3 3
#define SB_TIER4 4
#define SB_TIER_NO 5

/* address spaces of an authorisation */
#define SB_MED_UFS 0U                       /* blocks of a UFS LU (lu 0 = the GPT disk) */
#define SB_MED_SD 1U                        /* 4 KiB blocks of the microSD card */
#define SB_SP_MEM 2U                        /* physical addresses: registers and DRAM, bytes */
#define SB_SP_PMIC 3U                       /* S2MPS18 via ACPM: type << 8 | reg */
#define SB_SP_MAX 4U                        /* MAX77705: 7-bit address << 8 | reg */
#define SB_SP_DVFS 5U                       /* ACPM DVFS: the domain (g3d = 4) */

/* confirmation steps (struct sb_auth.steps) */
#define SB_STEP_WARN   1U                   /* the warning screen (what happens, how to undo) */
#define SB_STEP_HELD   2U                   /* the held confirmation (slide / hold Volume Up 1.5 s) */
#define SB_STEP_TYPED  4U                   /* the target's name typed on the keypad */
#define SB_STEP_SECOND 8U                   /* tier 3: the second confirmation (hold Power 3 s) */
#define SB_STEP_BACKUP 16U                  /* tier 3 storage: a verified backup of the target */
#define SB_STEP_ARMED  32U                  /* free-form tools: the session was armed */
#define SB_STEP_PHRASE 64U                  /* tier 4: "<target> I ACCEPT" typed */
#define SB_STEP_HOLD5  128U                 /* tier 4: Power held 5 s */
#define SB_STEP_WAIVE  256U                 /* tier 4 storage: "no backup" picked on the second red screen */
#define SB_STEP_ALL    511U
/* kinds (struct sb_auth.kind) */
#define SB_KIND_STORAGE  1U                 /* block storage: tier 3 needs the backup step */
#define SB_KIND_FREEFORM 2U                 /* poke, fill, sector edit: needs armed mode too */
#define SB_KIND_STORE    4U                 /* the action writes the OS store (backup, settings) */
/* The steps a tier needs. Tier 4 storage needs one of SB_STEP_BACKUP / SB_STEP_WAIVE on top
 * (not in this mask: sb_steps_ok checks it). */
unsigned sb_steps_needed(int tier, unsigned kind);
int sb_steps_ok(int tier, unsigned kind, unsigned steps);

struct sb_auth {
    uint64_t magic;
    uint32_t space, lu;
    uint64_t first, count;                  /* blocks (storage) or bytes / registers */
    int32_t tier;
    uint32_t steps, kind;
    uint64_t epoch, nonce, seal;
    char what[48];                          /* for the log */
};
void sb_auth_init(struct sb_auth *a, unsigned space, unsigned lu, uint64_t first, uint64_t count, int tier,
                  unsigned kind, const char *what);
/* Record a passed step (and reseal). Only engineer/armed.c's confirmation screens call it. */
void sb_auth_step(struct sb_auth *a, unsigned step);
int sb_auth_valid(const struct sb_auth *a);
int sb_auth_complete(const struct sb_auth *a);     /* valid and every step its tier needs */
void sb_auth_revoke_all(void);                     /* disarm: every authorisation so far is void */
/* One open authorisation at a time, for the length of one confirmed action. */
int sb_gate_open(const struct sb_auth *a);         /* 1 if complete */
void sb_gate_close(void);
/* 0 if the open authorisation covers [first, first + count) of space / lu with a tier >= need and
 * the steps need asks for; else SB_W_* and *why. need <= 0 is always allowed. */
int sb_gate_check(unsigned space, unsigned lu, uint64_t first, uint64_t count, int need, const char **why);

#define SB_W_DRYRUN  -100                   /* dry-run switch on: logged, nothing written */
#define SB_W_NO      -101                   /* impossible target */
#define SB_W_TIER    -102                   /* the confirmation was for a lower tier */
#define SB_W_RANGE   -103                   /* not inside one partition / empty / unknown */
#define SB_W_AUTH    -104                   /* no confirmed authorisation covers it */
#define SB_W_IO      -105                   /* the device reported an error */
#define SB_W_BACKUP  -106                   /* tier 3 storage without a verified backup */
struct sb_wdecide {
    int tier;                               /* 1, 2, 3 (SB_TIER_NO when refused) */
    char part[24];                          /* GPT partition, "boot LU n", "microSD" */
    uint64_t first, last;                   /* the partition's blocks */
    const char *why, *undo;
};
/* What a write of n 4 KiB blocks at lba of medium / lu would need (no write happens). */
int sb_write_decide(unsigned medium, unsigned lu, uint64_t lba, uint64_t n, struct sb_wdecide *d);
/* The block write entry point: decides, checks the gate, writes, logs. 0 or SB_W_*. */
int sb_write(unsigned medium, unsigned lu, uint64_t lba, uint64_t n, const void *src);
/* store.c only: inside the OS store region, with an open authorisation of kind SB_KIND_STORE. */
int sb_write_store(unsigned medium, uint64_t lba, uint64_t n, const void *src);
/* store.c reports the live store region of a medium (raw writes into it need tier 2). */
void sb_fence_store(unsigned medium, uint64_t base, uint64_t used, uint64_t capacity);
/* 1 while sb_write runs a write (the storage drivers refuse otherwise). */
int sb_fence_active(void);
void sb_fence_dry_run(int on);
int sb_fence_is_dry_run(void);
/* EFS, CPEFS, PARAM, ...: 1 if name is one (tier 3). */
int sb_never_part(const char *name);

/* ---- block I/O (blkio.c): one entry for UFS LUs and the card ---------------------------------- */

int sb_blk_read(unsigned medium, unsigned lu, uint64_t lba, uint64_t n, void *dst);
/* Only from sb_write (the drivers check sb_fence_active). */
int sb_blk_write(unsigned medium, unsigned lu, uint64_t lba, uint64_t n, const void *src);

/* ---- secondary cores as workers (smp.c, crt0.S sb_worker_entry) -------------------------------
 * Core w (1..3 the other A55s, PSCI 1..3; 4..7 the Mongoose M3s, PSCI 0x100..0x103) runs fn(w, arg)
 * with its MMU and caches on (the boot core's tables), its own stack and vectors (a fault records
 * ESR / ELR / FAR in its slot and powers the core off). fn polls sb_worker_stop() and publishes its
 * counters with sb_worker_publish(); when it returns the core calls PSCI CPU_OFF. Code that runs
 * on a worker must be ARMv8.0 (the M3 has no LSE, RDM, DotProd, FP16, LRCPC): smp.c and the
 * kernels are built with -march=armv8-a and checked for v8.1+ instructions. */
#define SB_NCORES 8U
#define SB_WK_IDLE    0U
#define SB_WK_RUNNING 1U
#define SB_WK_DONE    2U                    /* fn returned, CPU_OFF called */
#define SB_WK_FAULT   3U                    /* exception: esr / elr / far set */
#define SB_WK_PARKED  4U                    /* CPU_OFF returned: the core spins in WFE */
struct sb_wslot {
    volatile uint64_t state, mpidr, beats, iters, errors, esr, elr, far;
    volatile uint64_t v[8];                 /* the work function's own counters */
};                                          /* 128 bytes: two cache lines, one writer */
typedef void (*sb_work_fn)(unsigned w, void *arg);
typedef void (*sb_m3_prep_fn)(void);
/* Called before CPU_ON of an M3 core when set (the vendor PMU defaults: the application sets it
 * once the user confirmed those PMU writes). */
extern sb_m3_prep_fn sb_smp_m3_prep;
/* Start core w on fn (after the previous fn has stopped). 0, a PSCI error (< 0), or -100 when the
 * core did not check in within 1 s. */
int64_t sb_smp_start(unsigned w, sb_work_fn fn, void *arg);
/* PSCI target of core w. */
uint64_t sb_smp_target(unsigned w);
/* The slot of core w as the worker last published it (0 = this core: fill it yourself). */
const struct sb_wslot *sb_smp_slot(unsigned w);
struct sb_wslot *sb_smp_own_slot(unsigned w);
/* Ask every worker to stop; wait up to ms for all to leave fn. Returns how many did not. */
unsigned sb_smp_stop(unsigned ms);
unsigned sb_smp_running(void);
/* Inside fn: 1 when the boot core asked to stop; publish the slot (visible to the boot core). */
int sb_worker_stop(void);
void sb_worker_publish(struct sb_wslot *s);
extern char sb_worker_entry[];
extern volatile uint64_t sb_worker_boot[8];

/* ---- CMGP ADC and thermistors (adc.c) --------------------------------------------------------- */

/* One conversion of channel ch (0..11): 12-bit value, or -1 (clock gated, timeout). Polled. */
int sb_adc_read(unsigned ch, const char **why);
/* Linear interpolation in a thermistor table (adc ascending, temperature in 0.1 C): 0.1 C. */
int sb_therm_c10(int adc, const int *tab_adc, const int *tab_c10, unsigned n);
/* The vendor DT's tables: 0 AP / Wi-Fi, 1 battery / USB / charger, 2 PA. 0.1 C. */
int sb_therm_convert(unsigned table, int adc);
struct sb_therm {
    const char *name;
    unsigned ch;
    int ok, raw, c10;                       /* 0.1 C; raw = the median of 3 accepted conversions */
    unsigned rej;                           /* samples rejected in this reading (raw 0, 4095, timeout) */
    int bad;                                /* the last rejected raw value (-1 none / no conversion) */
    int clamped;                            /* raw outside the table: the temperature is its end */
    const char *why;                        /* no valid sample: why */
};
/* Sensor sample filter: calls get() up to max times, rejects SB_NO_SAMPLE and values outside
 * [lo, hi], stops at want accepted samples and returns their median (even count: the mean of the
 * middle two), or SB_NO_SAMPLE when none was accepted; *rejected and *last_bad say what was
 * dropped. Pure (unit-tested). */
#define SB_NO_SAMPLE (-0x7fffffff)
int sb_sample_filter(int (*get)(void *ctx), void *ctx, int lo, int hi, unsigned want, unsigned max,
                     unsigned *rejected, int *last_bad);
/* Per channel, this session. */
struct sb_adc_stat {
    uint32_t conversions, readings, rejected, timeouts, flag_only;
    int last_bad;
};
const struct sb_adc_stat *sb_adc_stats(unsigned ch);
/* Median of 3 conversions, raw 0 / 4095 rejected: raw or -1. */
int sb_adc_filtered(unsigned ch, const char **why, unsigned *rejected, int *last_bad);
int sb_therm_raw_min(unsigned table);
int sb_therm_raw_max(unsigned table);
/* The vendor DT's thermistors (AP, Wi-Fi, PA, battery, USB, charger): n read, -1 no ADC. */
int sb_therms_read(struct sb_therm *t, unsigned max, const char **why);

/* ---- writes with the gate (acpm.c, max77705.c) -------------------------------------------------
 * Refused (0) unless an open authorisation covers SB_SP_PMIC (type << 8 | reg) / SB_SP_MAX
 * (address << 8 | reg) at the tier the caller's policy gives (engineer/policy.c). */
int sb_pmic_write(uint32_t type, uint32_t reg, uint8_t val, int tier);
int sb_max_write(unsigned addr7, uint8_t reg, uint8_t val, int tier);
/* ACPM DVFS request { domain, kHz, FREQ_REQ, 0 } on the first ACPM channel (as the vendor kernel
 * and Starbite's iOS port send it; g3d = 4), with the gate: SB_SP_DVFS, first = domain, at the
 * caller's tier (at least 3). 1 sent and answered. */
int sb_acpm_dvfs(uint32_t domain, uint32_t khz, int tier);

/* ---- the Mali-G72 (mali.c, mali_desc.c) ------------------------------------------------------- */

#define SB_GPU 0x17500000UL
#define SB_GPU_MEM 0x8f0000000ULL           /* 32 MiB of DRAM bank 2: page tables + data */
#define SB_GPU_MEM_SIZE (32ULL << 20)
#define SB_GPU_DATA (SB_GPU_MEM + 0x10000ULL)
#define SB_GPU_DATA_SIZE (8ULL << 20)
#define SB_GPU_VA 0x02000000ULL             /* GPU VA of SB_GPU_DATA */
#define SB_GPU_PANEL_VA 0x04000000ULL       /* GPU VA of the scanout SB_FB */
#define SB_GPU_HOLE_VA 0x10000000ULL        /* never mapped: the deliberate-fault test */
/* The CPU's view of a GPU VA (data or panel). */
#define SB_GPU_CPU(va) ((va) >= SB_GPU_PANEL_VA ? (SB_FB + ((va) - SB_GPU_PANEL_VA)) : (SB_GPU_DATA + ((va) - SB_GPU_VA)))

struct sb_gpu_result {
    int ok, timeout;
    unsigned js;                            /* the slot reported (the first that failed) */
    uint64_t us, ticks;
    uint32_t jirq, mirq, girq;              /* JOB / MMU / GPU IRQ_RAWSTAT at the end */
    uint32_t js_status, as_status, as_fault, gpu_fault;
    uint64_t js_head, as_addr, gpu_addr;
};
int sb_gpu_domain(void);                    /* PMU G3D status [3:0] == 0xf */
uint32_t sb_gpu_rd(unsigned off);
int sb_gpu_wr(unsigned off, uint32_t v);    /* 0 (nothing written) without an open session */
/* The session: a complete authorisation for the BUCK6 write of the confirmed GPU power-up. */
int sb_gpu_open(const struct sb_auth *a);
int sb_gpu_is_open(void);
int sb_gpu_is_up(void);
void sb_gpu_close(void);
int sb_gpu_reset(unsigned *us);
int sb_gpu_power_on(uint32_t shaders, unsigned us[3]);
void sb_gpu_power_off(void);
void sb_gpu_tables(void);
int sb_gpu_as0(void);
int sb_gpu_up(uint32_t shaders, unsigned us[4]);     /* reset, power, AS0 */
int sb_gpu_submit(unsigned js, uint64_t job_va, uint32_t affinity);
int sb_gpu_wait(unsigned js_mask, uint64_t timeout_us, struct sb_gpu_result *r);
int sb_gpu_run(unsigned js, uint64_t job_va, uint32_t affinity, uint64_t timeout_us, struct sb_gpu_result *r);
const char *sb_gpu_exc_name(uint32_t code);
void sb_gpu_cycles(int on);
uint64_t sb_gpu_cycle_count(void);
uint64_t sb_gpu_timestamp(void);

/* Descriptors (Mesa genxml v6 layouts; pure, unit-tested against Mesa's own packing). */
#define MALI_JOB_NULL 1U
#define MALI_JOB_WRITE_VALUE 2U
#define MALI_JOB_FRAGMENT 9U
#define MALI_WV_CYCLES 1U
#define MALI_WV_TIMESTAMP 2U
#define MALI_WV_ZERO 3U
#define MALI_WV_IMM8 4U
#define MALI_WV_IMM16 5U
#define MALI_WV_IMM32 6U
#define MALI_WV_IMM64 7U
#define MALI_FBD_TAG 1U                     /* MFBD, 1 render target, no ZS/CRC extension */
void mali_job_header(uint32_t *w, unsigned type, unsigned index, uint64_t next);
void mali_write_value(uint32_t w[16], unsigned index, uint64_t next, uint64_t addr, unsigned type, uint64_t imm);
void mali_fragment(uint32_t w[16], unsigned index, uint64_t next, unsigned wpx, unsigned hpx, uint64_t fbd);
void mali_fbd(uint32_t w[48], uint64_t samples, unsigned wpx, unsigned hpx, uint64_t rt_base, uint32_t stride,
              uint32_t clear);
void mali_samples(uint32_t w[33]);
