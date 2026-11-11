/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sbm: Starbite's OS-independent driver core for the Galaxy S9's integrated Shannon modem (SM-G960F,
 * Exynos 9810 CP, Samsung's "ss360ap"). docs/MODEM.md is the design and the fact sheet.
 *
 * Shape (the same as the BCM4361 Wi-Fi core "bcmw"): a plain, freestanding C99 core with no libc and no
 * allocation; every hardware access goes through struct sbm_plat; a host test (host/run.sh) runs the core
 * against a fake CP (host/fakecp.c); thin per-OS glue (Starbite OS / libsb test images, iOS 16 XNU) supplies
 * sbm_plat. Original code written from the recorded facts (Samsung's GPL kernel drivers/misc/modem_v1 and
 * mcu_ipc as the interface reference, this phone's own boot logs and RADIO image as ground truth); nothing is
 * copied from Samsung's driver, libsamsung-ipc or an earlier port, so it can be built under GPL-2.0 or MIT/APSL glue.
 *
 * Stage 1 (this file): modem.bin TOC + CP memory map (read-only parsing), SIPC5 link framing, the SBD ring
 * layout and frame transfer on a memory buffer, the Samsung IPC header codec, decoders for the SIM and
 * network status messages, redaction. Stage 2: a read-only probe of the CP's state (sbm_probe_*). Nothing
 * here boots the CP, writes a register or calls EL3.
 *
 * Hard rules this core enforces by construction:
 *   - it never dials: sbm_ipc_build has an allow-list of request commands and the CALL group (0x02) is not
 *     on it, nor any SMS send, SS (supplementary services) or SAT command; there is no switch to add them;
 *   - IMEI / IMSI / ICCID / MSISDN / numbers never leave it unredacted: the log helpers mask digit runs, the
 *     frame dumper shows payload bytes only for an allow-list of identifier-free messages, the decoders keep
 *     MCC-MNC at most;
 *   - it has no block-write path at all: RADIO / EFS / CPEFS are only ever parsed from caller buffers.
 */
#ifndef SBM_H
#define SBM_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------------------------------ */
/* Fixed facts of the S9 (docs/MODEM.md section 1; this phone's DT and boot logs)                     */

#define SBM_MODEM_IF_PA     0xF0000000ULL  /* reserved-memory modem_if                              */
#define SBM_MODEM_IF_SIZE   0x09200000U
#define SBM_CPLOG_PA        0xF9200000ULL  /* cp_ram_logging, 32 MB (not ours to touch)             */
#define SBM_CPLOG_SIZE      0x02000000U
/* DT fallbacks for the CP memory map; the MEMn table (sbm_memmap_parse) overrides them              */
#define SBM_DT_CP_SIZE      0x07900000U
#define SBM_DT_VSS_SIZE     0x00700000U
#define SBM_DT_IPC_OFF      0x08000000U
#define SBM_DT_IPC_SIZE     0x00500000U
#define SBM_DT_ZMB_OFF      0x08500000U
#define SBM_DT_ZMB_SIZE     0x00B00000U

#define SBM_MBOX_PA         0x14170000ULL  /* mcu_ipc_cp, 0x180 bytes, GIC SPI 69                   */
#define SBM_MBOX_SIZE       0x180U
#define SBM_MBOX_MCUCTLR    0x00
#define SBM_MBOX_INTGR0     0x08
#define SBM_MBOX_INTCR0     0x0C
#define SBM_MBOX_INTMR0     0x10
#define SBM_MBOX_INTSR0     0x14           /* CP -> AP interrupt n = bit 16 + n                    */
#define SBM_MBOX_INTMSR0    0x18
#define SBM_MBOX_INTGR1     0x1C           /* AP -> CP interrupt n: write 1 << n                   */
#define SBM_MBOX_INTCR1     0x20
#define SBM_MBOX_INTMR1     0x24
#define SBM_MBOX_INTSR1     0x28
#define SBM_MBOX_INTMSR1    0x2C
#define SBM_MBOX_ISSR(n)    (0x80U + 4U * (n))
#define SBM_MBOX_NSSR       16             /* ISSR words the probe reads (of 64)                   */

/* mailbox words (mif_pdata) */
#define SBM_MBX_AP2CP_MSG     0
#define SBM_MBX_CP2AP_MSG     1
#define SBM_MBX_AP2CP_STATUS  2
#define SBM_MBX_CP2AP_STATUS  3
/* interrupt numbers (mif_pdata): AP -> CP through INTGR1 bit n, CP -> AP in INTSR0 bit 16 + n */
#define SBM_INT_AP2CP_MSG     0
#define SBM_INT_AP2CP_WAKEUP  1
#define SBM_IRQ_CP2AP_MSG     0
/* field packing of the status words */
#define SBM_CP_STATUS(v)      (((v) >> 1) & 0xFU)     /* ISSR3 */
#define SBM_CP_LTE_ACTIVE(v)  (((v) >> 5) & 1U)
#define SBM_CP_WAKELOCK(v)    (((v) >> 6) & 1U)
#define SBM_CP_RAT_MODE(v)    (((v) >> 26) & 0x3FU)
#define SBM_AP_WAKELOCK(v)    ((v) & 1U)              /* ISSR2 */
#define SBM_AP_STATUS(v)      (((v) >> 1) & 0xFU)
#define SBM_AP_PDA_ACTIVE(v)  (((v) >> 5) & 1U)
#define SBM_AP_SYS_REV(v)     (((v) >> 6) & 0xFFU)
#define SBM_AP_DS_DET(v)      (((v) >> 14) & 0x3U)

#define SBM_PMU_PA                 0x14060000ULL
#define SBM_PMU_CP_STAT            0x0038
#define SBM_PMU_CP_DEBUG           0x003C
#define SBM_PMU_CENTRAL_SEQ_CP_CFG 0x0280
#define SBM_PMU_CENTRAL_SEQ_CP_ST  0x0284     /* bits 23:16: 0x00 running, 0x80 in reset           */
#define SBM_PMU_RESET_SEQ_STATUS   0x0504     /* bits 2:0: 5 = CP up                               */
/* CP_CTRL_NS 0x30 / CP_CTRL_S 0x34 are secure (EL3 SMC only): the probe never touches them          */

/* link commands (in the msg words, with the message interrupt) */
#define SBM_INT_VALID       0x0080
#define SBM_CMD_VALID       0x0040
#define SBM_CMD_FIELD       0x003F
#define SBM_CMD_INIT_START  0x01
#define SBM_CMD_INIT_END    0x02
#define SBM_CMD_REQ_ACTIVE  0x03
#define SBM_CMD_RES_ACTIVE  0x04
#define SBM_CMD_REQ_TIME_SYNC 0x05
#define SBM_CMD_CRASH_RESET 0x07
#define SBM_CMD_PHONE_START 0x08
#define SBM_CMD_CRASH_EXIT  0x09
#define SBM_CMD_CP_DEPEND   0x0A
#define SBM_CMD_PIF_INIT_DONE 0x0D
#define SBM_CMD2INT(c)      ((uint16_t)((c) | SBM_CMD_VALID | SBM_INT_VALID))

/* first word of the IPC region */
#define SBM_IPC_MAGIC       0x000000AAU
#define SBM_BOOT_MAGIC      0x424F4F54U    /* "BOOT" */
#define SBM_DUMP_MAGIC      0x44554D50U    /* "DUMP" */
#define SBM_CRASH_MAGIC     0xDEADDEADU

/* SIPC channels */
#define SBM_CH_CSD          1
#define SBM_CH_PDP0         10
#define SBM_CH_PDP_HI       11
#define SBM_CH_ROUTER       25
#define SBM_CH_DM           28
#define SBM_CH_CASS         35
#define SBM_CH_BOOT0        215
#define SBM_CH_DUMP0        225
#define SBM_CH_FMT0         235
#define SBM_CH_FMT1         236
#define SBM_CH_RFS0         245
static inline int sbm_ch_is_fmt(unsigned ch) { return ch >= 235 && ch <= 244; }
static inline int sbm_ch_is_rfs(unsigned ch) { return ch >= 245 && ch <= 254; }
static inline int sbm_ch_is_ps(unsigned ch) { return ch >= 10 && ch <= 24; }

/* ------------------------------------------------------------------------------------------------ */
/* Platform (per-OS glue). Stage 1 needs only log; the stage 2 probe needs probe32 too.               */

struct sbm_plat {
    void *ctx;
    /* fault-safe 32-bit read of a physical address: 1 = read *v, 0 = the access faulted (libsb: sb_probe32,
     * plus a pending-SError check for DRAM carve-outs). NULL = the probe reads nothing. */
    int (*probe32)(void *ctx, uint64_t pa, uint32_t *v);
    void (*log)(void *ctx, const char *line);   /* one line, no newline; already redacted */
    void (*barrier)(void *ctx);                 /* dmb ish around ring index updates; NULL = none */
    /* stage 2b, all optional (NULL = that part is skipped) */
    uint64_t (*smc)(void *ctx, uint64_t id, uint64_t a1, uint64_t a2, uint64_t a3);   /* SMC #0 to EL3 */
    /* bytes [off, off + len) of the RADIO partition, read only: 0 ok */
    int (*radio_read)(void *ctx, uint64_t off, void *buf, uint32_t len);
    /* store v at pa and push it to DRAM (clean + invalidate the line): 1 ok, 0 the store faulted. Only
     * sbm_probe_write_test calls it, and only for its two fixed words. */
    int (*poke32)(void *ctx, uint64_t pa, uint32_t v);
    /* stage 3 (sbm_boot.c), all optional (NULL = the step that needs it refuses) */
    /* one MMIO store; the glue keeps its own exact allow-list (PMU CP regs, the mailbox, AP2CP_CFG): 1 ok */
    int (*mmio_wr32)(void *ctx, uint64_t pa, uint32_t v);
    /* copy into / out of modem_if + off, through DRAM (clean / invalidate the lines): 1 ok, 0 refused */
    int (*carve_write)(void *ctx, uint32_t off, const void *src, uint32_t len);
    int (*carve_read)(void *ctx, uint32_t off, void *dst, uint32_t len);
    void (*udelay)(void *ctx, uint32_t us);
    uint64_t (*now_us)(void *ctx);
    void (*feed)(void *ctx);                    /* the watchdog, during long steps */
    /* stage 4: the CPU view of the IPC region (modem_if + ipc_off, ipc_size bytes); NULL = stage 4 refused */
    uint8_t *ipc;
};

/* ------------------------------------------------------------------------------------------------ */
/* modem.bin (the RADIO partition): TOC and the CP memory map                                        */

/* TOC: 0x20-byte entries { char name[12]; u32 offset, loadaddr, size, crc, entryid; } from offset 0;
 * entry 0 is "TOC" itself (size = header size, entryid = number of entries after it). */
#define SBM_TOC_ENT       32
#define SBM_TOC_MAX       16
#define SBM_TOC_BYTES     (SBM_TOC_ENT * SBM_TOC_MAX)   /* 512: what sbm_toc_parse reads */
#define SBM_CP_VIEW_BASE  0x40000000U   /* the CP's own address of modem_if + 0 (BOOT's loadaddr)   */
struct sbm_toc_ent {
    char name[13];         /* NUL-terminated copy of the 12-byte field */
    uint32_t offset;       /* in modem.bin */
    uint32_t loadaddr;     /* CP view */
    uint32_t size;
    uint32_t crc;
    uint32_t entryid;
};
struct sbm_toc {
    unsigned n;            /* entries, including "TOC" */
    struct sbm_toc_ent e[SBM_TOC_MAX];
    uint32_t hdr_size;     /* TOC.size */
    int boot, main, vss, nv, offset_ent;   /* indices, -1 = absent */
};
/* Parses the first SBM_TOC_BYTES of modem.bin. image_size (0 = unknown) bounds every entry that is read from
 * modem.bin (NV comes from EFS's nv_data.bin, OFFSET describes a range inside NV). 0 or SBM_E*. */
int sbm_toc_parse(const uint8_t *first512, uint64_t image_size, struct sbm_toc *t);

/* CP memory map, the "MEMn" table at modem.bin + SBM_MEMMAP_OFF (inside BOOT; S-Boot leaves the same bytes
 * at modem_if + SBM_MEMMAP_OFF, which is where Samsung's shm_probe reads it). */
#define SBM_MEMMAP_OFF   (0x200 + 0xA0)
#define SBM_MEMMAP_BYTES 80
#define SBM_EXTBIN_MAX   5
struct sbm_memmap {
    char id[5];            /* "MEM0" */
    uint32_t dram_size, ext_mem_size, ext_bin_count;
    struct { char tag[4]; uint32_t addr, size; } bin[SBM_EXTBIN_MAX];
    /* resolved (DT defaults when the table is absent or invalid) */
    uint32_t cp_size, ipc_off, ipc_size, vss_off, vss_size, vparam_off, vparam_size, zmb_off, zmb_size,
             log_off, log_size;
    int valid;             /* 1 = a MEMn table that passed Samsung's contiguity check */
};
void sbm_memmap_parse(const uint8_t *p, struct sbm_memmap *m);

/* Load plan: where each section goes in modem_if (what cbd does through umts_boot0), checked against the
 * memory map. Nothing is copied here. */
enum sbm_src { SBM_SRC_RADIO, SBM_SRC_NV };
struct sbm_load {
    char name[13];
    enum sbm_src src;      /* RADIO: file offset in modem.bin; NV: EFS nv_data.bin from its start */
    uint32_t src_off, size;
    uint32_t dst_off;      /* in modem_if */
};
#define SBM_LOAD_MAX 4
struct sbm_plan {
    unsigned n;
    struct sbm_load l[SBM_LOAD_MAX];   /* BOOT, MAIN, VSS, NV (in cbd's order) */
    uint32_t boot_size, main_size;     /* for the EL3 security request, later */
};
int sbm_plan_build(const struct sbm_toc *t, const struct sbm_memmap *m, struct sbm_plan *p);

/* ------------------------------------------------------------------------------------------------ */
/* SIPC5 link frames                                                                                 */

#define SBM_SIPC5_START     0xF8           /* top 5 bits of cfg */
#define SBM_SIPC5_PADDING   0x04
#define SBM_SIPC5_EXT       0x02
#define SBM_SIPC5_CTL       0x01
#define SBM_SIPC5_MULTI     0x03           /* EXT|CTL: a 5-byte header with { id:7, more:1 }        */
#define SBM_SIPC5_EXTLEN    0x02           /* EXT only: a 6-byte header, u32 length at offset 2      */
#define SBM_SIPC5_ALIGN     8              /* padding unit (the AP kernel's sizeof(long))            */
#define SBM_SIPC5_HDR_MAX   6
struct sbm_sipc5 {
    uint8_t cfg, ch;
    uint8_t multi, more, id;   /* multi-frame control, when multi */
    uint32_t hdr_len, frame_len;   /* frame_len includes the header, excludes padding */
    uint32_t total;            /* bytes this frame takes in a stream (with padding) */
    const uint8_t *payload;
    uint32_t payload_len;
};
/* Parses one frame from buf[0..len). 0 or SBM_E*. */
int sbm_sipc5_parse(const uint8_t *buf, uint32_t len, struct sbm_sipc5 *f);
/* Writes one frame: 4-byte header (6 with an extended length when needed), no padding. Returns the frame
 * length or SBM_E*. */
int sbm_sipc5_build(uint8_t *out, uint32_t cap, uint8_t ch, const uint8_t *payload, uint32_t plen);
/* One fragment of a multi-frame message (5-byte header). */
int sbm_sipc5_build_multi(uint8_t *out, uint32_t cap, uint8_t ch, uint8_t id, int more,
                          const uint8_t *payload, uint32_t plen);

/* Reassembly of multi-frame messages, per channel (caller-owned buffer). */
struct sbm_reasm {
    uint8_t *buf;
    uint32_t cap, len;
    uint8_t active, id;
    uint32_t dropped;
};
/* Feeds one parsed frame. Returns 1 when a whole message is ready in *msg and *mlen (single frames point into the
 * frame itself), 0 while a multi-frame message is incomplete, or SBM_E* (and drops the partial message). */
int sbm_reasm_feed(struct sbm_reasm *r, const struct sbm_sipc5 *f, const uint8_t **msg, uint32_t *mlen);

/* ------------------------------------------------------------------------------------------------ */
/* SBD: the shared buffer descriptor rings in the IPC region (on a memory buffer; no doorbells here)  */

#define SBM_SBD_MAX_CH       32
#define SBM_SBD_CTRL_OFF     0x0000     /* magic, access, legacy map (1 KB)              */
#define SBM_SBD_CMD_OFF      0x0400     /* IOSM command area (unused on this phone)       */
#define SBM_SBD_DESC_OFF     0x0800     /* global descriptor, RBP set, SBD vectors        */
#define SBM_SBD_DESC_END     0x10000
#define SBM_SBD_BUFF_OFF     0x10000
#define SBM_SBD_RBCH_SIZE    16
#define SBM_SBD_RBDESC_SIZE  20
#define SBM_SBD_RBCH_OFF     (SBM_SBD_DESC_OFF + 12)
#define SBM_SBD_RBDESC_OFF   (SBM_SBD_RBCH_OFF + SBM_SBD_MAX_CH * SBM_SBD_RBCH_SIZE)
#define SBM_SBD_GDESC_END    (SBM_SBD_RBDESC_OFF + SBM_SBD_MAX_CH * 2 * SBM_SBD_RBDESC_SIZE)
#define SBM_UL 0                        /* AP -> CP */
#define SBM_DL 1                        /* CP -> AP */

struct sbm_chan {          /* one link channel (a ring pair), in Samsung's DT order */
    const char *name;
    uint16_t ch;           /* SIPC channel */
    uint8_t lnk_hdr;       /* frames carry a SIPC5 header */
    uint8_t zerocopy;      /* DL buffers hold zero-copy descriptors (multipdp) */
    uint16_t len[2];       /* ring length [UL], [DL] */
    uint16_t size[2];      /* buffer size */
};
extern const struct sbm_chan sbm_starlte_chans[];
extern const unsigned sbm_starlte_nchans;

struct sbm_ring {
    uint16_t id, ch, dir, len, size;
    uint32_t rbd_off;      /* offsets inside the IPC region */
    uint32_t addr_v_off, size_v_off, buff_off;
};
struct sbm_sbd {
    uint8_t *ipc;          /* CPU view of the IPC region */
    uint32_t ipc_size;
    unsigned n;
    uint32_t rbps_off;     /* u16 rp[UL][n], wp[UL][n], rp[DL][n], wp[DL][n] */
    uint32_t desc_end, buff_end;
    struct sbm_ring r[SBM_SBD_MAX_CH][2];
    uint8_t ch2id[256];    /* 0xFF = none */
    uint32_t errors;       /* rejected ring entries (bad index, size or address) */
    uint32_t last_size_v;  /* the raw size word of the entry sbm_sbd_get looked at last (diagnostics) */
    /* the zero-copy DL ring (multipdp, ch 11) once filled (the attended data test): the AP owns the DL rp as the
     * producer of empty cells and keeps its own read index; cells are SBM_ZC_CELL bytes in ZMB, right after the IPC
     * region, and a slot holds a u64 offset from the IPC base (data = ipc + offset) */
    uint16_t zc_pre_rp;
    uint8_t zc_on;
    uint32_t zc_errors, zc_zmb_rel;
    uint16_t zc_cell[2048];    /* the cell each DL slot holds (the AP's own record; the slot's offset is checked) */
};
/* Lays out the descriptor region and buffer pool for chans[0..n) the way Samsung's AP does on CMD_INIT_START
 * (the CP reads this layout). Clears the descriptor region only. 0 or SBM_E*. */
int sbm_sbd_init(struct sbm_sbd *s, uint8_t *ipc, uint32_t ipc_size, const struct sbm_chan *chans, unsigned n);
/* Attaches to a layout already in the IPC region (the fake CP; checks). Validates every offset. 0 or SBM_E*. */
int sbm_sbd_attach(struct sbm_sbd *s, uint8_t *ipc, uint32_t ipc_size);
/* Puts one frame into the next free buffer of (channel, dir): the AP uses SBM_UL, the fake CP SBM_DL.
 * 0, SBM_ENOSPC when the ring is full, SBM_ERANGE when the frame is larger than a buffer. */
int sbm_sbd_put(struct sbm_sbd *s, uint16_t ch, unsigned dir, const uint8_t *frame, uint32_t len,
                const struct sbm_plat *p);
/* The next frame of (id, dir): its length (0 = empty, SBM_E* = a bad entry, which is skipped) and *f pointing
 * at it in shared memory; sbm_sbd_get_done releases it. */
int sbm_sbd_get(struct sbm_sbd *s, unsigned id, unsigned dir, const uint8_t **f, const struct sbm_plat *p);
void sbm_sbd_get_done(struct sbm_sbd *s, unsigned id, unsigned dir, const struct sbm_plat *p);
#define SBM_ZC_CELL      2560u       /* One UI's mif_buff cell size (dmesg: cell_size 2560, 4505 cells) */
#define SBM_ZC_HEADROOM  64u         /* NET_HEADROOM: where the data starts in a cell */
/* fill every free slot of ch's DL ring with a ZMB cell (zmb_rel: ZMB's offset from the IPC base, mem: ZMB's size) */
int sbm_sbd_zc_fill(struct sbm_sbd *s, uint16_t ch, uint32_t zmb_rel, uint32_t zmb_size, const struct sbm_plat *p);
/* the next filled DL entry: its packet (in ZMB) and length; 0 none; then sbm_sbd_zc_done gives the cell back */
int sbm_sbd_zc_get(struct sbm_sbd *s, uint16_t ch, uint32_t zmb_rel, uint32_t zmb_size, const uint8_t **pkt,
                   uint16_t *iod_ch, const struct sbm_plat *p);
void sbm_sbd_zc_done(struct sbm_sbd *s, uint16_t ch, const struct sbm_plat *p);
uint16_t sbm_sbd_rp(const struct sbm_sbd *s, unsigned id, unsigned dir);
uint16_t sbm_sbd_wp(const struct sbm_sbd *s, unsigned id, unsigned dir);
int sbm_sbd_id(const struct sbm_sbd *s, uint16_t ch);   /* ring id of a channel, -1 = none */

/* ------------------------------------------------------------------------------------------------ */
/* Samsung IPC (the FMT channels 235/236): header and messages                                       */

#define SBM_IPC_HDR 7      /* u16 length (incl. header), u8 mseq, u8 aseq, u8 group, u8 index, u8 type */
/* AP -> CP request types */
#define SBM_IPC_EXEC  0x01
#define SBM_IPC_GET   0x02
#define SBM_IPC_SET   0x03
#define SBM_IPC_CFRM  0x04
#define SBM_IPC_EVENT 0x05
/* CP -> AP types */
#define SBM_IPC_INDI  0x01
#define SBM_IPC_RESP  0x02
#define SBM_IPC_NOTI  0x03
/* groups (confirmed on this phone's captured traffic where marked) */
#define SBM_G_PWR   0x01   /* seen */
#define SBM_G_CALL  0x02   /* seen (call list); never built */
#define SBM_G_SMS   0x04   /* seen (CBS config) */
#define SBM_G_SEC   0x05   /* seen */
#define SBM_G_PB    0x06
#define SBM_G_DISP  0x07   /* seen */
#define SBM_G_NET   0x08   /* seen */
#define SBM_G_SND   0x09
#define SBM_G_MISC  0x0A   /* seen */
#define SBM_G_SVC   0x0B
#define SBM_G_SS    0x0C
#define SBM_G_GPRS  0x0D   /* seen */
#define SBM_G_SAT   0x0E
#define SBM_G_CFG   0x0F   /* seen */
#define SBM_G_IMEI  0x10
#define SBM_G_GPS   0x11   /* seen */
#define SBM_G_RFS   0x42   /* RFS channel: its own header { u32 length; u8 index; u8 id } */
#define SBM_G_GEN   0x80   /* seen */
#define SBM_CMD(g, i) ((uint16_t)(((g) << 8) | (i)))
#define SBM_PWR_PHONE_PWR_UP     SBM_CMD(SBM_G_PWR, 0x01)
#define SBM_PWR_PHONE_STATE      SBM_CMD(SBM_G_PWR, 0x07)
/* GPRS (libsec-ril IPC 4.1 layouts, read 2026-11-11; docs/MODEM.md 7.9) */
#define SBM_GPRS_DEFINE_PDP      SBM_CMD(SBM_G_GPRS, 0x01)   /* SET, 149 bytes: 01, cid, type (02 IPv4), APN[101] */
#define SBM_GPRS_PS              SBM_CMD(SBM_G_GPRS, 0x03)   /* SET 01 00 00 = attach */
#define SBM_GPRS_PDP_CONTEXT     SBM_CMD(SBM_G_GPRS, 0x04)   /* SET 112 (or 248) bytes: act, cid, flags=0, ...  */
#define SBM_GPRS_IP_CONFIG       SBM_CMD(SBM_G_GPRS, 0x09)   /* NOTI: cid, u16 flags, ip, dns1, dns2, gw, mask */
#define SBM_GPRS_CALL_STATUS     SBM_CMD(SBM_G_GPRS, 0x10)   /* NOTI: cid, status (1 connected), cause */
#define SBM_GPRS_LTE_ATTACH      SBM_CMD(SBM_G_GPRS, 0x14)   /* SET 201 (or 337): the LTE initial-attach APN */
#define SBM_GPRS_ATTACH_LEN      194
#define SBM_GPRS_ATTACH_LONG_LEN 330
#define SBM_GPRS_DEFINE_LEN      149
#define SBM_GPRS_ACT_LEN         112
#define SBM_GPRS_ACT_LONG_LEN    248
#define SBM_APN_MAX              100
#define SBM_SEC_PIN_STATUS       SBM_CMD(SBM_G_SEC, 0x01)
#define SBM_SEC_PHONE_LOCK       SBM_CMD(SBM_G_SEC, 0x02)
#define SBM_SEC_RSIM_ACCESS      SBM_CMD(SBM_G_SEC, 0x05)
#define SBM_SEC_SIM_ICC_TYPE     SBM_CMD(SBM_G_SEC, 0x07)
#define SBM_SEC_LOCK_INFO        SBM_CMD(SBM_G_SEC, 0x08)
#define SBM_DISP_ICON_INFO       SBM_CMD(SBM_G_DISP, 0x01)
#define SBM_DISP_RSSI_INFO       SBM_CMD(SBM_G_DISP, 0x06)
#define SBM_NET_PREF_PLMN        SBM_CMD(SBM_G_NET, 0x01)
#define SBM_NET_PLMN_SEL         SBM_CMD(SBM_G_NET, 0x02)
#define SBM_NET_SERVING_NETWORK  SBM_CMD(SBM_G_NET, 0x03)
#define SBM_NET_PLMN_LIST        SBM_CMD(SBM_G_NET, 0x04)
#define SBM_NET_REGIST           SBM_CMD(SBM_G_NET, 0x05)
#define SBM_NET_SUBSCRIBER_NUM   SBM_CMD(SBM_G_NET, 0x06)
#define SBM_NET_MODE_SEL         SBM_CMD(SBM_G_NET, 0x0A)
#define SBM_MISC_ME_VERSION      SBM_CMD(SBM_G_MISC, 0x01)
#define SBM_MISC_ME_IMSI         SBM_CMD(SBM_G_MISC, 0x02)
#define SBM_MISC_ME_SN           SBM_CMD(SBM_G_MISC, 0x03)
#define SBM_MISC_TIME_INFO       SBM_CMD(SBM_G_MISC, 0x05)
#define SBM_GEN_PHONE_RES        SBM_CMD(SBM_G_GEN, 0x01)
#define SBM_RFS_NV_READ_ITEM     SBM_CMD(SBM_G_RFS, 0x01)   /* libsamsung-ipc rfs.h: body { u32 offset; u32 length } */
#define SBM_RFS_NV_WRITE_ITEM    SBM_CMD(SBM_G_RFS, 0x02)   /* { u32 offset; u32 length; data } */
#define SBM_RFS_HDR              6
/* why stage 5a stopped */
#define SBM_STOP_TIME     1
#define SBM_STOP_SIM      2       /* a SIM is present: not tonight's experiment */
#define SBM_STOP_REG      3       /* the CP reported registration */
#define SBM_STOP_CRASH    4
#define SBM_STOP_FRAMES   5       /* the frame budget is used up */

struct sbm_ipc_msg {
    uint16_t len;          /* whole message, header included */
    uint8_t mseq, aseq, type;
    uint16_t cmd;          /* group << 8 | index */
    const uint8_t *data;
    uint16_t data_len;
};
/* Parses one message (the payload of a FMT frame). 0 or SBM_E*. */
int sbm_ipc_parse(const uint8_t *buf, uint32_t len, struct sbm_ipc_msg *m);
/* 1 if Starbite may send (cmd, type) at this stage: GETs of status, nothing that dials, sends or changes the
 * SIM / network setup. */
int sbm_ipc_allowed(uint16_t cmd, uint8_t type);
/* Writes a request; returns its length, SBM_EPERM when not allowed, or SBM_E*. */
int sbm_ipc_build(uint8_t *out, uint32_t cap, uint8_t mseq, uint16_t cmd, uint8_t type,
                  const uint8_t *data, uint16_t dlen);
/* Request + SIPC5 frame in one go (the FMT payload carries one IPC message). */
#ifdef SBM_ATTENDED_TX
/* the attended SIM test's builder (sbm_ipc.c): the status GETs plus the radio on / off EXECs, nothing else */
int sbm_ipc_attended_allowed(uint16_t cmd, uint8_t type, const uint8_t *data, uint16_t dlen);
int sbm_ipc_frame_attended(uint8_t *out, uint32_t cap, uint8_t ch, uint8_t mseq, uint16_t cmd, uint8_t type,
                           const uint8_t *data, uint16_t dlen);
#endif
int sbm_ipc_frame(uint8_t *out, uint32_t cap, uint8_t ch, uint8_t mseq, uint16_t cmd, uint8_t type,
                  const uint8_t *data, uint16_t dlen);
/* Names: caller buffers, so these are reentrant. */
const char *sbm_ipc_name(uint16_t cmd, char buf[12]);
const char *sbm_ipc_type_name(uint8_t type, int from_cp);
/* 1 if a message's payload carries nothing that identifies the phone, SIM or user (only these get a hex
 * dump in a log). */
int sbm_ipc_payload_loggable(uint16_t cmd);

/* GEN_PHONE_RES: the CP's answer to a SET/EXEC/CFRM (also to a refused GET) */
struct sbm_gen_res {
    uint16_t cmd;          /* the request it answers */
    uint8_t type;
    uint16_t code;         /* 0x8000 + n = accepted; anything else = an error code */
};
int sbm_gen_res_decode(const struct sbm_ipc_msg *m, struct sbm_gen_res *r);
static inline int sbm_gen_res_ok(const struct sbm_gen_res *r) { return (r->code & 0xFF00) == 0x8000; }

/* ------------------------------------------------------------------------------------------------ */
/* SIM and network status                                                                            */

/* SEC_PIN_STATUS status byte */
#define SBM_SIM_READY          0x00
#define SBM_SIM_LOCK_SC        0x03    /* PIN / PUK, see the key byte */
#define SBM_SIM_LOCK_FD        0x04
#define SBM_SIM_LOCK_PN        0x05    /* network personalisation */
#define SBM_SIM_LOCK_PU        0x06
#define SBM_SIM_LOCK_PP        0x07
#define SBM_SIM_LOCK_PC        0x08
#define SBM_SIM_ABSENT         0x80    /* seen on this phone (no SIM) */
#define SBM_SIM_ERROR          0x81
#define SBM_SIM_INIT_COMPLETE  0x82
#define SBM_SIM_PB_INIT_COMPLETE 0x83
/* NET_REGIST state */
#define SBM_REG_NONE        0x01
#define SBM_REG_HOME        0x02
#define SBM_REG_SEARCHING   0x03
#define SBM_REG_EMERGENCY   0x04       /* limited service: seen on this phone without a SIM */
#define SBM_REG_UNKNOWN     0x05
#define SBM_REG_ROAMING     0x06
/* NET_REGIST domain */
#define SBM_DOMAIN_CS       0x02
#define SBM_DOMAIN_PS       0x03
/* access technology (act) */
#define SBM_ACT_GSM         0x01
#define SBM_ACT_GPRS        0x02
#define SBM_ACT_EDGE        0x03
#define SBM_ACT_UMTS        0x04
#define SBM_ACT_LTE         0x21       /* libsec-ril's NET_REGIST decode (2026-11-11): 0x21 LTE, 0x24 NR; 7 = unknown */
#define SBM_ACT_NR          0x24

struct sbm_reg {
    uint8_t act, domain, state, edge, reject;
    int have_loc;          /* LAC/CID were present; their values are not kept (location) */
};
struct sbm_cell_state {
    int have_pwr;   uint8_t pwr_up, phone_state;
    int have_sim;   uint8_t sim_status, sim_key;
    int have_icc;   uint8_t icc_type;
    int have_rssi;  uint8_t rssi_raw[2];
    int have_bars;  uint8_t bars;
    int have_cs;    struct sbm_reg cs;
    int have_ps;    struct sbm_reg ps;
    int have_plmn;  char plmn[7];      /* MCC-MNC of the serving network (operator identity only) */
    uint8_t plmn_sel;                  /* NET_PLMN_SEL: 2 automatic, 3 manual */
    uint32_t n_msgs, n_unknown, n_bad, n_gen_err;
};
void sbm_cell_init(struct sbm_cell_state *st);
/* Folds one message from the CP into the state; returns 1 if it changed something the user sees. */
int sbm_cell_update(struct sbm_cell_state *st, const struct sbm_ipc_msg *m);
/* One line, e.g. "SIM absent | CS emergency-only (GSM) | PS emergency-only | PLMN 00101 | bars 2" */
void sbm_cell_line(const struct sbm_cell_state *st, char *out, size_t cap);
/* The same without the PLMN (it identifies the country and operator): "PLMN set" / "PLMN -". For logs and for a
 * status screen that is shared or photographed. Also "rssi 0x.." from DISP_RSSI_INFO when known. */
void sbm_cell_line_masked(const struct sbm_cell_state *st, char *out, size_t cap);
const char *sbm_sim_name(uint8_t status);
const char *sbm_reg_name(uint8_t state);
const char *sbm_operator_name(const char *plmn);   /* SCREEN ONLY: MCC-MNC -> a name, 0 if not listed */
const char *sbm_act_name(uint8_t act);

/* ------------------------------------------------------------------------------------------------ */
/* Redaction                                                                                         */

/* Masks, in place, every run of >= 7 decimal digits (IMEI, IMSI, ICCID, MSISDN, phone numbers) in a text
 * line with '#'. Returns the number of runs masked. */
unsigned sbm_redact_text(char *s, size_t cap);
/* EF_IMSI (9 bytes: length, then BCD with the parity nibble) -> MCC-MNC only; mnc_len 2 or 3 (0 = 2).
 * Returns the IMSI's digit count, or SBM_E*. The rest of the IMSI is never written out. */
int sbm_imsi_mccmnc(const uint8_t ef[9], int mnc_len, char out[8]);
/* Hex dump of a frame for a log line: header bytes always, payload bytes only if sbm_ipc_payload_loggable,
 * else "<n bytes>". */
void sbm_frame_line(const uint8_t *frame, uint32_t len, int from_cp, char *out, size_t cap);

/* ------------------------------------------------------------------------------------------------ */
/* Stage 2: read-only probe of the CP's state                                                        */

enum sbm_probe_step {
    SBM_PROBE_REGS = 1,    /* PMU status + mailbox registers (no DRAM) */
    SBM_PROBE_SHM  = 2,    /* + four small reads in the CP carve-out (may fault: TZASC) */
};
struct sbm_probe {
    unsigned steps;        /* what was asked */
    /* PMU */
    int pmu_ok;
    uint32_t cp_stat, cp_debug, central_cfg, central_st, reset_seq;
    /* mailbox */
    int mbox_ok;
    uint32_t mcuctlr, intmr0, intsr0, intmsr0, intmr1, intsr1, intmsr1;
    uint32_t issr[SBM_MBOX_NSSR];
    uint32_t issr_ok;      /* bit n = ISSR n was read */
    /* carve-out */
    int shm_tried, shm_head_ok, shm_map_ok, shm_ipc_ok;
    int shm_zero;          /* read without a fault but all zero: empty OR TZASC read-as-zero */
    uint8_t head[16];      /* modem_if + 0 */
    uint8_t map_raw[SBM_MEMMAP_BYTES];
    struct sbm_memmap mm;
    uint32_t ipc_magic, ipc_access;
    uint32_t faults;       /* reads that faulted */
};
/* Only reads, through p->probe32, in a fixed order: PMU, mailbox, then (step SHM) the carve-out. Logs a
 * breadcrumb before each block and the PMU values right after they are read. */
void sbm_probe_run(const struct sbm_plat *p, unsigned steps, struct sbm_probe *pr);
/* What the readings mean, one line each, through p->log. */
void sbm_probe_report(const struct sbm_plat *p, const struct sbm_probe *pr);
/* A one-word verdict: "cp-off", "cp-in-reset", "cp-powered", "cp-running", "cp-booting", "cp-online",
 * "cp-crashed", "shm-zero-ambiguous" (carve-out read as all zero: no answer), "unreadable". */
const char *sbm_probe_verdict(const struct sbm_probe *pr);

/* Stage 2b, step 3 (read only): what S-Boot left in the carve-out, against the RADIO partition. Each range is
 * read with probe32 (stopping at the first fault) and from RADIO with radio_read, and CRC32'd. "raw" = at its
 * modem.bin file offset (S-Boot's copy starts with the TOC at modem_if + 0), "cbd" = where cbd would load it. */
#define SBM_SURVEY_MAX   8
#define SBM_SURVEY_CHUNK 0x10000U       /* bytes compared per range at most */
struct sbm_survey_ent {
    char what[16];
    uint32_t carve_off, radio_off, len;
    uint32_t crc_carve, crc_radio;
    int carve_ok, radio_ok;
    uint32_t zero_words;                /* in the carve-out range */
};
struct sbm_survey {
    int toc_ok;                         /* the carve-out's own TOC parsed (else this phone's known values) */
    struct sbm_toc toc;
    unsigned n;
    struct sbm_survey_ent e[SBM_SURVEY_MAX];
};
void sbm_probe_survey(const struct sbm_plat *p, struct sbm_survey *sv);
void sbm_survey_report(const struct sbm_plat *p, const struct sbm_survey *sv);

/* Stage 2b, step 4 (user present): CP_CTRL_S / CP_CTRL_NS through EL3's read call (SMC 0x82000700, x1 = 3
 * READ_CTRL, x2 = half, x3 = register: 0 CP_CTRL_S, 1 CP_CTRL_NS), as Samsung's PMUCAL reads them. Never the
 * write call. 0 ok, SBM_EPERM no smc, SBM_EIO EL3's error (*err = its low 16 bits). */
#define SBM_SMC_CP         0x82000700ULL
#define SBM_SMC_READ_CTRL  3
#define SBM_CP_CTRL_S      0
#define SBM_CP_CTRL_NS     1
int sbm_cp_ctrl_read(const struct sbm_plat *p, unsigned reg, uint32_t *v, uint32_t *err);

/* Step 4: can the AP store into the carve-out before EL3's security call (stage 3's copy-in needs it)? Two fixed
 * words, the IPC region's command area (+0x400, unused on this phone) and the last word of the CP's own memory:
 * read, store (original ^ 0xA5A5A5A5), push to DRAM, read back, then store the original again and check. */
#define SBM_WTEST_N 2
struct sbm_wtest {
    uint64_t pa;
    uint32_t before, pattern, after, restored;
    int read_ok, store_ok, after_ok, restore_ok;
    const char *result;                 /* "writable", "write-ignored", "store-faulted", "unreadable", "other" */
};
void sbm_probe_write_test(const struct sbm_plat *p, struct sbm_wtest w[SBM_WTEST_N]);
void sbm_wtest_report(const struct sbm_plat *p, const struct sbm_wtest w[SBM_WTEST_N]);
uint32_t sbm_crc32(uint32_t crc, const void *buf, size_t n);   /* zlib's CRC-32; start with 0 */

/* ------------------------------------------------------------------------------------------------ */
/* Stage 3 (sbm_boot.c): load the CP image, pass EL3's check, start the CP, park it                  */
/* The order is Android's on this phone (docs/MODEM.md section 6): cp_init (EL3 CP_CTRL writes), security
 * mode 2, copy BOOT/MAIN/VSS/NV, security mode 0, power on (mailbox words, AP2CP_CFG = 1). Parking is
 * Android's ss310ap_off: wakeup interrupt, 5 ms, dump_pc_no_pg, cal_cp_reset_assert. */

#define SBM_SMC_WRITE_CTRL 4
#define SBM_SMC_SSS_CLK    0x82001011ULL     /* x1: 0 enable, 1 disable */
#define SBM_SEC_NORMAL     0                 /* x1 = 0 | modem_if base, x2 = BOOT size, x3 = MAIN size */
#define SBM_SEC_REINIT     2                 /* x1 = 2, 0, 0: Android's first call, answers 0x0b */
#define SBM_SEC_OK         0x00
#define SBM_SEC_NOT_WORKING 0x0b
#define SBM_AP2CP_CFG_PA   0x14820000ULL     /* bit 0: the CP CPU runs */
#define SBM_GICD_CP_PEND   0x10101238ULL     /* GICD ISPENDR, bit 28: PMUCAL waits for it after cp_init */
#define SBM_MBOX_WORDS     64
#define SBM_SYS_REV        174               /* this phone's (Android's init_mailbox_regs) */
#define SBM_DS_DET         1

enum sbm_seq_op { SBM_SEQ_WRITE, SBM_SEQ_WRITE_EL3, SBM_SEQ_DELAY, SBM_SEQ_WAIT };
struct sbm_seq {
    uint8_t op;
    const char *name;
    uint64_t pa;               /* WRITE / WAIT: the register; WRITE_EL3: SBM_CP_CTRL_S or _NS in pa */
    uint32_t mask, value;      /* new = (old & ~mask) | value; DELAY: value = us; WAIT: (reg & mask) == value */
};
extern const struct sbm_seq sbm_seq_cp_init[], sbm_seq_reset_assert[], sbm_seq_dump_pc_no_pg[];
extern const unsigned sbm_seq_cp_init_n, sbm_seq_reset_assert_n, sbm_seq_dump_pc_no_pg_n;
/* PMUCAL's cp_reset_release (pmucal_cp_exynos9810.h): CP_START again, the central sequencer back to 0, the GIC bit */
extern const struct sbm_seq sbm_seq_reset_release[];
extern const unsigned sbm_seq_reset_release_n;
/* Runs a PMUCAL-style table; a WAIT polls up to timeout_us. 0, SBM_ETIME (which wait), SBM_EIO / SBM_EPERM. */
int sbm_seq_run(const struct sbm_plat *p, const struct sbm_seq *s, unsigned n, uint32_t timeout_us);
/* CP_CTRL through EL3, read-modify-write the way PMUCAL does. 0 or SBM_E*. */
int sbm_cp_ctrl_rmw(const struct sbm_plat *p, unsigned reg, uint32_t mask, uint32_t value, uint32_t *now);
/* The security call (SSS clock on around it). *ret = EL3's answer. 0 if the call was made. */
int sbm_sec_req(const struct sbm_plat *p, unsigned mode, uint32_t boot_size, uint32_t main_size, uint32_t *ret);

/* What stage 3 works from: this phone's TOC, map and plan, and the sections already in RAM (the glue reads
 * them read-only from RADIO and EFS). */
struct sbm_image {
    struct sbm_toc toc;
    struct sbm_memmap mm;
    struct sbm_plan plan;
    const uint8_t *data[SBM_LOAD_MAX];   /* in plan order: BOOT, MAIN, VSS, NV */
    uint32_t crc[SBM_LOAD_MAX];          /* filled by sbm_image_check */
    /* Streaming (the small footprint): a RADIO section whose data[k] is NULL is read from the partition through
     * plat->radio_read in chunks of chunk_size bytes, twice: once for the CRC check, once into the carve-out (its CRC
     * must match the check's, then the read-back's). Only NV needs RAM. 0 / NULL = everything in RAM as before. */
    uint8_t *chunk;
    uint32_t chunk_size;                 /* >= 4096, a multiple of 4096 */
    /* NV as EFS holds it (read once, never written): copied over data[NV] before every start, so each start and
     * each restart serves the same data, whatever the CP wrote to the RAM copy last time. NULL = not restored. */
    const uint8_t *nv_pristine;
};
/* Checks the sections in RAM: BOOT/MAIN/VSS against the TOC's CRC-32, NV's size. 0 or SBM_E*. Logs only
 * sizes and CRCs, never content. */
int sbm_image_check(const struct sbm_plat *p, struct sbm_image *im);

enum sbm_boot_stop { SBM_BOOT_DRY = 1, SBM_BOOT_SEC = 2, SBM_BOOT_RUN = 3, SBM_BOOT_LINK = 4, SBM_BOOT_ONLINE = 5 };

/* Stage 4 (sbm_link.c): the legacy 4 MB map at the front of the IPC region (Samsung's shmem_4mb_phys_map without
 * LEGACY_QOS), used for the boot-time UDL exchange before the SBD rings exist */
#define SBM_LEG_MAGIC        0
#define SBM_LEG_ACCESS       4
#define SBM_LEG_RAW_TX_HEAD  24
#define SBM_LEG_RAW_TX_TAIL  28
#define SBM_LEG_RAW_RX_HEAD  32
#define SBM_LEG_RAW_RX_TAIL  36
#define SBM_LEG_HEADS        8          /* fmt tx/rx, raw tx/rx head+tail: 8 words from offset 8 */
#define SBM_LEG_RAW_TX_BUFF  12288
#define SBM_LEG_RAW_TX_SIZE  2084864
#define SBM_LEG_RAW_RX_BUFF  2097152
#define SBM_LEG_RAW_RX_SIZE  2097152
/* cbd's std UDL handshake on umts_boot0 (ch 215, SIPC5, padded to 8): request -> expected answer */
#define SBM_UDL_BOOT_DONE    0x900D
#define SBM_UDL_BOOT_DONE_OK 0xA00D
#define SBM_UDL_FIN          0x9F00
#define SBM_UDL_FIN_OK       0xAF00
/* bytes of the request payload: a LE u32. Confirmed in One UI's /vendor/bin/cbd (2026-11-08, build ID b42a5281):
 * std_udl_req_resp(fd, req, exp) does write(fd, &req, 4), then std_udl_poll, read(fd, &resp, 4), resp == exp;
 * umts_boot0 (ATTR_SIPC5, BOOT_ALIGNED) adds the 0xFC header: fc d7 08 00 0d 90 00 00 on the wire */
#define SBM_UDL_PAYLOAD      4
struct sbm_boot {
    enum sbm_boot_stop stop;
    /* what happened */
    int checked, inited, copied, validated, released, parked;
    int restart;                         /* output: a CP parked earlier in this boot was restarted (reset release) */
    uint32_t streamed;                   /* output: bytes read from RADIO straight into the carve-out */
    uint32_t check_ms, copy_ms, link_ms; /* output: how long the image check, the copy and the link took */
    uint32_t sec2, sec0;                 /* EL3's answers */
    uint32_t ctrl_s, ctrl_ns;            /* after cp_init */
    uint32_t reset_seq_before, reset_seq_after;
    uint32_t cp_status_ms;               /* how long until ISSR3 cp_status != 0 (RUN) */
    uint32_t issr[4];                    /* after power on */
    /* stage 4 */
    uint32_t hold_ms;                    /* input (LINK): stay after PHONE_START this long, watching, sending nothing */
    int udl_done, init_start, pif_sent, phone_start;
    uint32_t phone_start_repeats, phone_start_gap_ms, dl_frames_seen, other_cmds;
    /* stage 5a (ONLINE, receive only): INIT_END sent, then what the CP says for hold_ms */
    int init_end_sent;
    uint32_t fmt_rx, rfs_rx, rfs_reads, rfs_writes, rfs_other, other_rx, bad_rx, sim_status, stopped_by;
    int rfs_serve;                     /* input (T10): answer RFS from the RAM copy of nv_data.bin */
    uint32_t rfs_tx, rfs_tx_frames, rfs_tx_bytes, big_frames, big_dropped, rfs_joined, rfs_parts;
    int fmt_query;                     /* input (T11): send the allow-listed status GETs once, 3 s after INIT_END */
    uint32_t query_period_ms;          /* input (soak): and again every this many ms (0 = once) */
    uint32_t keep_running;             /* input (session API only): SBM_KEEP_RUNNING = return online after INIT_END,
                                        * not parked. Any other value (0, or stack garbage) parks: soak 2ecf7cd */
    uint32_t fmt_tx, fmt_tx_refused, fmt_resp, fmt_gen_res;
    uint32_t fmt_unmatched;            /* RESPs matching no open query of ours, by seq or by command */
    uint32_t fmt_seq_fixed;            /* RESPs with a seq that was not open, matched by command (NET_REGIST CS) */
    /* the attended SIM test (only with SBM_ATTENDED_TX and attended == SBM_ATTENDED_MAGIC): the guards for a SIM and a
     * registration are off, and the radio EXEC's answer is kept */
    uint32_t attended;
    uint8_t radio_seq, radio_res_seen;
    uint16_t radio_res;                /* GEN_PHONE_RES code for the last radio EXEC (0x80xx accepted) */
    /* the attended data test: the PDP context (cid 1) as the CP reports it; addresses never logged */
    struct sbm_pdp {
        uint8_t define_seq, act_seq, define_seen, act_seen;
        uint16_t define_res, act_res;  /* GEN_PHONE_RES codes */
        uint8_t have_status, status, cause, have_ip;
        uint16_t ip_flags, mtu;
        uint8_t ip[4], dns1[4], dns2[4], gw[4];
        uint32_t status_notis, ip_notis;
    } pdp;
    uint32_t data_tx, data_rx, data_rx_zc, data_rx_bad, data_tx_err;
    /* the attended registration helpers (attach APN, network selection automatic, PS attach): the last one's answer */
    uint8_t ctl_seq, ctl_seen;
    uint16_t ctl_cmd, ctl_res;
    uint32_t noti_logged;              /* attended: unsolicited frames logged after the first ONLINE_LOG_MAX */
    uint32_t fmt_stale;                /* a seq came round again (188 queries later) while its query was still open */
    struct sbm_cell_state cell;        /* what the CP reported (decoded, identity-free) */
    uint32_t udl_resp[2], init_start_ms, phone_start_ms, n_msgs, last_msg, crash_cmd;
    uint64_t sec0_us;
    int err;                             /* 0, or the SBM_E* that stopped it */
    const char *stopped_at;
};
/* Runs stage 3 up to b->stop. Whatever happens after cp_init, the CP is parked before it returns (and the
 * caller parks again from its reset hook if it never returns). 0 or SBM_E*. */
int sbm_boot_run(const struct sbm_plat *p, struct sbm_image *im, struct sbm_boot *b);
/* Stage 4 (sbm_link.c), called by sbm_boot_run for SBM_BOOT_LINK after power on: DL_START, the UDL handshake,
 * INIT_START -> SBD layout -> PIF_INIT_DONE, wait for PHONE_START. Never INIT_END. 0 or SBM_E*. */
int sbm_link_boot(const struct sbm_plat *p, struct sbm_image *im, struct sbm_boot *b);
/* Stage 5 prep (sbm_clk.c): the ECT clock table at the legacy map's reserved area, written before INIT_END. Returns
 * its size (248 on this phone) or SBM_E*. */
#define SBM_CLK_TABLE_OFF 40
uint32_t sbm_clk_table_size(void);
int sbm_clk_table_write(uint8_t *ipc, uint32_t ipc_size);
/* Stage 5b (sbm_rfs.c): the RFS server's core, NV against a RAM copy (layouts from One UI's libsec-ril.so). */
#define SBM_RFS_IDX_NV_READ        0x01
#define SBM_RFS_IDX_NV_WRITE       0x02
#define SBM_RFS_IDX_READ_FILE      0x03
#define SBM_RFS_IDX_WRITE_FILE     0x04
#define SBM_RFS_IDX_LSEEK          0x05
#define SBM_RFS_IDX_CLOSE          0x06
#define SBM_RFS_IDX_PUT_FILE       0x07
#define SBM_RFS_IDX_GET_FILE       0x08
#define SBM_RFS_IDX_RENAME         0x09
#define SBM_RFS_IDX_FILE_INFO      0x0a
#define SBM_RFS_IDX_DELETE         0x0b
#define SBM_RFS_IDX_MKDIR          0x0c
#define SBM_RFS_IDX_OPEN           0x11
#define SBM_RFS_IDX_INFO_BY_HANDLE 0x13
#define SBM_RFS_IDX_CREATE         0x14
#define SBM_RFS_IDX_NV_WRITE_ALL   0x15
#define SBM_RFS_IDX_NV_RESTORE     0x17
#define SBM_RFS_REPLY_HDR          15     /* header + u8 confirm + u32 offset + u32 length */
#define SBM_RFS_OPEN_REPLY         14     /* header + u32 handle + u32 errno (ProcessOpenFile) */
#define SBM_RFS_MAX_IO             0x80000
struct sbm_rfs {
    uint8_t *nv;                      /* the RAM copy of nv_data.bin: the CP's writes land here and nowhere else */
    uint32_t nv_size;
    uint32_t reads, writes, opens, read_bytes, write_bytes, refused, bad, unhandled;
    uint32_t other[0x20];             /* requests per index this core does not answer yet */
};
void sbm_rfs_init(struct sbm_rfs *r, uint8_t *nv, uint32_t nv_size);
/* One RFS request (the payload of a ch 245 frame) -> the reply in out. Returns its length, 0 = not answered (an
 * index this core does not handle yet), or SBM_E*. */
int sbm_rfs_handle(struct sbm_rfs *r, const uint8_t *req, uint32_t n, uint8_t *out, uint32_t cap);

/* The radio's power state request, from One UI's libsec-ril (IpcProtocol41Power::IpcTxRadioPower): PWR_PHONE_STATE
 * (0x0107) EXEC with u16 data: 0x0001 = low power mode (RF off), 0x0202 = normal (RF on). The RIL sends NORMAL at
 * boot. T11 showed that the CP scans and camps without it too (receive only with no SIM), so it is not the receiver's
 * on/off switch. Not on the builder's allow-list (nothing sends it yet). */
#define SBM_PWR_STATE_LPM     0x0001
#define SBM_PWR_STATE_NORMAL  0x0202

/* Dial / send policy (sbm_policy.c): never an emergency number (world list + the SIM's and network's own lists + any
 * of them behind an international prefix), never a short code, only the user's own allow-list. No switch. */
#define SBM_DIAL_ALLOW_MAX   4
#define SBM_DIAL_MIN_DIGITS  7
#define SBM_KEEP_RUNNING     0x4B454550u   /* "KEEP": the only value of sbm_boot.keep_running that skips the park */
struct sbm_dial_policy {
    const char *allowed[SBM_DIAL_ALLOW_MAX];    /* the user's own numbers (never in git) */
    unsigned n_allowed;
    const char *const *extra_emergency;          /* SIM ECC / network emergency numbers, if known */
    unsigned n_extra;
    /* Starbite OS Next's Phone / Messages: the user typed the number and held to call / send. Then any number that
     * passes the two locks (not an emergency number, at least SBM_DIAL_MIN_DIGITS digits) is allowed; the locks have
     * no switch. Only SBM_DIAL_USER_CONFIRMED counts. */
    uint32_t user_confirmed;
};
#define SBM_DIAL_USER_CONFIRMED 0x55534552u   /* "USER" */
int sbm_is_emergency(const char *number, const char *const *extra, unsigned n_extra);   /* 1 = refuse */
int sbm_dial_check(const struct sbm_dial_policy *pol, const char *number);              /* 0 or SBM_EPERM */

/* SMS (sbm_sms.c): TP-DELIVER decode, the incoming IPC (0x0402), TP-SUBMIT encode (policy first). */
#define SBM_SMS_TEXT_MAX 640
struct sbm_sms {
    char from[24];                    /* the sender: the user's data, never logged by this core */
    int from_alpha;                   /* an alphanumeric sender name */
    uint8_t pid, dcs, alphabet;       /* alphabet: 0 GSM 7-bit, 1 8-bit, 2 UCS-2 */
    uint8_t scts[7];                  /* service centre time stamp (BCD, swapped) */
    int concat;
    uint8_t ref, total, seq;          /* concatenated SMS */
    uint32_t binary_len;              /* 8-bit data: its length (the data is not kept) */
    char text[SBM_SMS_TEXT_MAX];      /* UTF-8 */
};
int sbm_sms_deliver_decode(const uint8_t *tpdu, uint32_t n, struct sbm_sms *s);
int sbm_sms_incoming(const struct sbm_ipc_msg *m, struct sbm_sms *s);
/* TP-SUBMIT for number (allow-listed, never an emergency number: sbm_dial_check) and an ASCII text that fits one
 * GSM 7-bit message (160 characters, the common subset). Returns the PDU length or SBM_E*. Nothing sends it yet. */
int sbm_sms_submit(const struct sbm_dial_policy *pol, const char *number, const char *text, uint8_t *out, uint32_t cap);
#define SBM_SMS_PDU_MAX   176               /* SMS-SUBMIT: 1 + 1 + 12 (DA) + 1 + 1 + 1 + 140 (UD) and some room */
#define SBM_SMS_MAX_PARTS 4
/* the text (UTF-8) as 1..maxparts SMS-SUBMIT PDUs: GSM 7-bit (default + extension table) when it fits, else UCS-2;
 * several parts carry a concatenation header with ref. Policy first. Returns the number of parts or SBM_E*. */
int sbm_sms_submit_parts(const struct sbm_dial_policy *pol, const char *number, const char *utf8, uint8_t ref,
                         uint8_t pdu[][SBM_SMS_PDU_MAX], uint16_t *len, unsigned maxparts);

/* The session API (Starbite OS): sbm_modem_start loads, checks and starts the CP and brings the link online with the RFS
 * server (about 2 s, blocking); sbm_modem_poll is non-blocking, from the event loop (every few ms): mailbox, rings,
 * RFS, guards; a guard (a SIM that is not absent, a registration) or a crash parks the CP and returns 1 / SBM_E*.
 * sbm_modem_query sends the allow-listed status GETs; sbm_modem_status gives the masked status line; sbm_modem_stop
 * parks. Receive-only by design until the SIM day: the guards stay in. */
enum sbm_modem_state {
    SBM_MS_OFF = 0,        /* never started, or stopped by the caller (parked) */
    SBM_MS_ONLINE = 1,     /* up: polling, queries answered */
    SBM_MS_STALLED = 2,    /* the CP stopped answering: parked; sbm_modem_recover may restart it */
    SBM_MS_GUARD = 3,      /* a guard parked it (a SIM that is not absent, a registration): stays off */
    SBM_MS_CRASHED = 4,    /* the CP sent its crash command: parked; sbm_modem_recover may restart it */
    SBM_MS_FAILED = 5,     /* the start failed (parked); last_err says why */
};
#define SBM_STALL_MS        20000u   /* an open query and nothing from the CP for this long = stalled */
#define SBM_RECOVER_GAP_MS   5000u   /* at least this long parked before a restart */
#define SBM_RECOVER_MAX         3u   /* restarts per SBM_RECOVER_WINDOW_MS; then it stays off */
#define SBM_RECOVER_WINDOW_MS (30u * 60u * 1000u)
/* What a UI shows: one copy, taken with sbm_modem_snapshot whenever it wants (cheap, no I/O). Everything here is
 * safe to show on screen; only `line` (masked) and the numbers may go to a log. plmn and cell are SCREEN ONLY: they
 * identify the country and operator. */
struct sbm_modem_snap {
    uint32_t gen;          /* bumps whenever anything below changes: redraw when it differs from the last one */
    uint8_t state;         /* enum sbm_modem_state */
    uint8_t sim;           /* SBM_SIM_*, 0xFF = not reported yet */
    uint8_t cs, ps;        /* SBM_REG_* (registration), 0xFF = not reported yet */
    uint8_t rat;           /* SBM_ACT_* of the CS (else PS) registration, 0 = unknown */
    uint8_t bars;          /* 0..5 from DISP_ICON_INFO, 0xFF = unknown */
    uint8_t rssi_raw;      /* DISP_RSSI_INFO's byte, 0xFF = unknown; rssi_dbm = -rssi_raw (Samsung's IPC habit,
                            * unconfirmed: 0x3d = -61 dBm with 4 bars fits) */
    int16_t rssi_dbm;      /* 0 = unknown */
    uint8_t emergency_only;/* 1: camped with limited service (no SIM): "Emergency calls only" */
    uint8_t have_plmn;
    char plmn[7];          /* SCREEN ONLY: MCC-MNC of the serving cell, e.g. "00101" */
    uint8_t have_cell;     /* the CP sent LAC / cell id (their values are not kept: location) */
    uint32_t up_ms;        /* since the last start */
    uint32_t last_rx_ms;   /* since anything came from the CP (0xFFFFFFFF = nothing yet) */
    uint32_t queries, answers, open;   /* status GETs sent / answered / still waiting */
    uint32_t starts, restarts;         /* this session: starts by the caller, automatic restarts */
    int32_t last_err;      /* SBM_E* of the last failure, 0 = none */
    char line[112];        /* the masked one-line status (loggable) */
};
/* The session. Keep it static (it holds the boot record); start/stop as often as needed: the same struct, the same
 * image, no allocation in the core. */
struct sbm_modem {
    const struct sbm_plat *p;
    struct sbm_image *im;
    struct sbm_boot b;
    uint32_t ms;           /* ms since the last start (the caller's elapsed_ms added up) */
    int up;
    uint8_t state;         /* enum sbm_modem_state */
    uint32_t gen, starts, restarts, stalls, crashes;
    uint32_t last_rx_ms, last_q_ms, down_ms;  /* last_rx: when the CP last sent anything; down: when it was parked */
    uint32_t total_ms;     /* ms since the first start (for the restart window) */
    uint32_t restart_at[SBM_RECOVER_MAX];     /* total_ms of the recent restarts */
    uint32_t rx_seen;      /* fmt_rx + rfs_rx at the last poll (to see new traffic) */
    int32_t last_err;
    uint32_t attended;     /* SBM_ATTENDED_MAGIC: the attended SIM test (honoured only in an SBM_ATTENDED_TX build) */
    struct sbm_modem_snap last_snap;          /* for gen: what changed since */
};
int sbm_modem_start(struct sbm_modem *m, const struct sbm_plat *p, struct sbm_image *im);
/* Non-blocking. 0 = nothing new; 1 = something the UI shows changed (take a snapshot); SBM_E* = it went down
 * (parked): SBM_ETIME a stall, SBM_EIO a CP crash, SBM_EPERM a guard. */
int sbm_modem_poll(struct sbm_modem *m, uint32_t elapsed_ms);
int sbm_modem_query(struct sbm_modem *m);
void sbm_modem_status(const struct sbm_modem *m, char *out, size_t cap);
void sbm_modem_snapshot(struct sbm_modem *m, struct sbm_modem_snap *s);
/* After a stall or a crash (not a guard, not a stop): restarts the CP (blocking, about 2 s) when it has been parked
 * for SBM_RECOVER_GAP_MS and fewer than SBM_RECOVER_MAX restarts happened in the window. Call it from a timer while
 * the state is STALLED / CRASHED, with elapsed_ms since the last poll/recover call. 0 restarted, 1 not yet (wait),
 * SBM_EPERM not allowed (state, or the window is used up), else the start's SBM_E*. */
int sbm_modem_recover(struct sbm_modem *m, uint32_t elapsed_ms);
int sbm_modem_stop(struct sbm_modem *m);
#define SBM_ATTENDED_MAGIC 0x41545444u   /* "ATTD": SIM, radio, registration */
#define SBM_ATTENDED_DATA  0x4154544Eu   /* "ATTN": the same plus the data test (the zero-copy ring filled, GPRS) */
#define SBM_IS_ATTENDED(x) ((x) == SBM_ATTENDED_MAGIC || (x) == SBM_ATTENDED_DATA)
#ifdef SBM_ATTENDED_TX
/* the attended SIM test: radio normal (on = 1) or low power mode (on = 0; the CP detaches, then RF off). The answer
 * lands in m->b.radio_res / radio_res_seen. SBM_EPERM unless the session is attended. */
int sbm_modem_radio(struct sbm_modem *m, int on);
/* the attended data test: define the PDP context (cid 1, IPv4, apn); activate (on 1) or deactivate it (long: the
 * 248-byte form); answers and NOTIs land in m->b.pdp */
/* registration helpers (SBM_ATTENDED_MAGIC or _DATA); the answer lands in m->b.ctl_res / ctl_seen:
 * the LTE initial-attach APN (pdp_type 2 IPv4 / 6 IPv4v6, profile 1, no credentials), network selection
 * automatic, PS attach */
int sbm_modem_attach_apn(struct sbm_modem *m, const char *apn, uint8_t pdp_type, int long_form);
int sbm_modem_net_auto(struct sbm_modem *m);
int sbm_modem_ps_attach(struct sbm_modem *m);
int sbm_modem_pdp_define(struct sbm_modem *m, const char *apn);
int sbm_modem_pdp_activate(struct sbm_modem *m, int on, int long_form);
/* raw IPv4 out on the PDP channel (rmnet0, ch 10), and the receive callback for packets from either PS ring */
int sbm_modem_ip_tx(struct sbm_modem *m, const uint8_t *pkt, uint32_t len);
void sbm_modem_ip_rx_cb(void (*cb)(void *ctx, const uint8_t *pkt, uint32_t len), void *ctx);
#endif

/* sbm_net.c: the data test's IPv4 client (DNS A query, one HTTP GET over minimal TCP), no allocation, no logging */
enum { SBM_NET_IDLE = 0, SBM_NET_WAITING, SBM_NET_DONE, SBM_NET_FAILED };
#define SBM_NET_WINDOW 8192
struct sbm_net {
    uint8_t ip[4], dns[4], peer[4];
    uint16_t mtu, ip_id;
    int (*tx)(void *ctx, const uint8_t *pkt, uint32_t len);
    void *ctx;
    uint32_t seed;
    /* DNS */
    int dns_state;
    char dns_name[128];
    uint16_t dns_id, dns_port;
    uint8_t dns_answer[4], dns_rcode;
    uint32_t dns_at, dns_tries, dns_sent;
    /* TCP + HTTP */
    int tcp_state, http_state, http_code, tcp_connected, tcp_reset;
    uint16_t sport, dport, req_len;
    uint32_t iss, snd_nxt, snd_una, rcv_nxt, tcp_at, tcp_tries, tcp_sent, tcp_ooo;
    uint8_t req[256];
    char http_head[192];
    uint32_t http_hdr_len, http_bytes;
    /* counters */
    uint32_t tx_packets, tx_errors, rx_packets, rx_other, rx_bad_csum;
    uint8_t pkt[1600];
};
void sbm_net_init(struct sbm_net *n, const uint8_t ip[4], const uint8_t dns[4], uint16_t mtu,
                  int (*tx)(void *ctx, const uint8_t *pkt, uint32_t len), void *ctx, uint32_t seed);
int sbm_net_dns(struct sbm_net *n, const char *name, uint32_t now_ms);
int sbm_net_http_get(struct sbm_net *n, const uint8_t ip[4], const char *host, const char *path, uint32_t now_ms);
void sbm_net_rx(struct sbm_net *n, const uint8_t *pkt, uint32_t len);
void sbm_net_tick(struct sbm_net *n, uint32_t now_ms);

/* Park: Android's power-off. Safe to call more than once and when nothing was started. */
int sbm_cp_park(const struct sbm_plat *p);
void sbm_boot_report(const struct sbm_plat *p, const struct sbm_boot *b);

/* ------------------------------------------------------------------------------------------------ */
/* errors */
#define SBM_EINVAL  (-1)
#define SBM_ERANGE  (-2)
#define SBM_ENOSPC  (-3)
#define SBM_ENOENT  (-4)
#define SBM_EPERM   (-5)
#define SBM_ETIME   (-6)
#define SBM_EIO     (-7)
#define SBM_ESHORT  (-8)
#define SBM_EBADF   (-9)    /* a malformed frame / message */
const char *sbm_strerror(int e);

/* freestanding helpers (sbm_util.c) */
void sbm_memcpy(void *d, const void *s, size_t n);
void sbm_memset(void *d, int c, size_t n);
int sbm_memcmp(const void *a, const void *b, size_t n);
uint32_t sbm_strlen(const char *s);
size_t sbm_strlcpy(char *d, const char *s, size_t cap);
/* tiny formatter: %s %u %d %x %c %% with width / zero pad, l / ll; returns the full length */
int sbm_fmt(char *out, size_t cap, const char *fmt, ...);
/* formats, redacts, then p->log */
void sbm_logf(const struct sbm_plat *p, const char *fmt, ...);
static inline uint16_t sbm_le16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static inline uint32_t sbm_le32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static inline void sbm_put16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static inline void sbm_put32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}

#endif
