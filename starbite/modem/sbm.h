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
#define SBM_G_RFS   0x42
#define SBM_G_GEN   0x80   /* seen */
#define SBM_CMD(g, i) ((uint16_t)(((g) << 8) | (i)))
#define SBM_PWR_PHONE_PWR_UP     SBM_CMD(SBM_G_PWR, 0x01)
#define SBM_PWR_PHONE_STATE      SBM_CMD(SBM_G_PWR, 0x07)
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
#define SBM_ACT_LTE         0x07       /* (newer firmwares; confirm on a capture with a SIM) */

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
const char *sbm_sim_name(uint8_t status);
const char *sbm_reg_name(uint8_t state);
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
};
/* Checks the sections in RAM: BOOT/MAIN/VSS against the TOC's CRC-32, NV's size. 0 or SBM_E*. Logs only
 * sizes and CRCs, never content. */
int sbm_image_check(const struct sbm_plat *p, struct sbm_image *im);

enum sbm_boot_stop { SBM_BOOT_DRY = 1, SBM_BOOT_SEC = 2, SBM_BOOT_RUN = 3, SBM_BOOT_LINK = 4 };

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
    uint32_t sec2, sec0;                 /* EL3's answers */
    uint32_t ctrl_s, ctrl_ns;            /* after cp_init */
    uint32_t reset_seq_before, reset_seq_after;
    uint32_t cp_status_ms;               /* how long until ISSR3 cp_status != 0 (RUN) */
    uint32_t issr[4];                    /* after power on */
    /* stage 4 */
    int udl_done, init_start, pif_sent, phone_start;
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
