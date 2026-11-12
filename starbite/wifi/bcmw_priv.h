/* SPDX-License-Identifier: GPL-2.0-only
 * bcmw's insides, shared by its files and the host test. Not for the shims.
 */
#ifndef BCMW_PRIV_H
#define BCMW_PRIV_H
#include "bcmw.h"
#include "wpa.h"
#include "sae.h"

/* ---- the log: a line builder without a C library ---------------------------------------------- */

struct bcmw_lb {
    char b[200];
    unsigned n;
};
void lb_init(struct bcmw_lb *l);
void lb_s(struct bcmw_lb *l, const char *s);
void lb_hex(struct bcmw_lb *l, uint64_t v, unsigned digits);    /* digits only, no 0x */
void lb_dec(struct bcmw_lb *l, uint64_t v);
void lb_sdec(struct bcmw_lb *l, int64_t v);
void bcmw_say(struct bcmw *w, const char *s);
void bcmw_say_lb(struct bcmw *w, struct bcmw_lb *l);
/* one call for the common case: text, a hex value, more text (NULL for none) */
void bcmw_sayx(struct bcmw *w, const char *a, uint64_t v, const char *b);

uint32_t bcmw_strlen(const char *s);
int bcmw_strneq(const char *a, const char *b, uint32_t n);
void bcmw_memcpy(void *d, const void *s, uint32_t n);
void bcmw_memset(void *d, int c, uint32_t n);
int bcmw_memcmp(const void *a, const void *b, uint32_t n);
void bcmw_strlcpy(char *d, const char *s, uint32_t cap);

static inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* ---- rings (ring.c): brcmfmac's commonring with Broadcom DHD's H2D stamping -------------------- */

struct bcmw_ring;
struct bcmw_ring_ops {
    uint16_t (*rd_idx)(struct bcmw_ring *r, uint32_t where);
    void (*wr_idx)(struct bcmw_ring *r, uint32_t where, uint16_t v);
    void (*bell)(struct bcmw_ring *r);
    void (*flush)(struct bcmw_ring *r);     /* the items to memory (cache clean), after stamping */
};
struct bcmw_ring {
    uint8_t *buf;               /* depth * item_len bytes of DMA memory */
    uint64_t bus;
    uint16_t depth, item_len;
    uint16_t r, w;              /* our copies of the read and write indices */
    uint16_t stamp;             /* H2D: the next item not yet stamped */
    uint8_t phase;              /* H2D: 0 or 0x80, flipped each time an item lands on the base */
    uint8_t h2d;
    uint32_t seq;               /* H2D: epoch = seq % 253, from 254 (DHD: H2D_EPOCH_INIT_VAL) */
    uint32_t w_where, r_where;  /* where the indices live (TCM address or host index slot) */
    uint8_t id, was_full;
    const struct bcmw_ring_ops *ops;
    void *ctx;
};
#define BCMW_EPOCH_MODULO 253
#define BCMW_EPOCH_INIT   254
#define BCMW_PHASE_BIT    0x80

void ring_config(struct bcmw_ring *r, uint16_t depth, uint16_t item_len, uint8_t *buf, uint64_t bus, int h2d);
uint16_t ring_space(struct bcmw_ring *r);                          /* items free for writing */
uint8_t *ring_reserve(struct bcmw_ring *r, uint16_t n, uint16_t *got);   /* contiguous items */
void ring_cancel(struct bcmw_ring *r, uint16_t n);
void ring_commit(struct bcmw_ring *r);                             /* stamp, write index, bell */
uint8_t *ring_read(struct bcmw_ring *r, uint16_t *n);              /* contiguous readable items */
void ring_consumed(struct bcmw_ring *r, uint16_t n);

/* ---- the root complex (exynos_pcie.c) ---------------------------------------------------------- */

#define EX_ELBI       0x116A0000ULL
#define EX_PCS        0x116C0000ULL
#define EX_PHY        0x116D0000ULL
#define EX_DBI        0x11700000ULL
#define EX_SYSREG     0x11410000ULL     /* SYSREG_FSYS1: +0x1044, +0x1050 the PHY, +0x700 shareability */
#define EX_CMU_FSYS1  0x11400000ULL
#define EX_SYSMMU     0x11420000ULL     /* never read or written here: a read stopped the bus (an earlier port wifi-a4/a5) */
#define EX_CMU_TOP    0x1A240000ULL
#define EX_PMU        0x14060000ULL
#define EX_GPIO_PERIC0 0x10430000ULL
#define EX_GPIO_FSYS1 0x11430000ULL
#define EX_GPIO_ALIVE 0x14050000ULL
#define EX_CFG        0x127FE000ULL     /* the iATU's type 0 configuration window, 4 KiB */
#define EX_CFG_SIZE   0x1000ULL
#define EX_MEM        0x11800000ULL     /* where the endpoint's BARs go (bus address = CPU address) */
#define EX_MEM_SIZE   0x00FF0000ULL
#define EX_SPI        253               /* the RC's interrupt: SPI 253, GIC INTID 285, level high */

int ex_look(struct bcmw *w);            /* 1: every gate of the controller open */
int ex_power(struct bcmw *w);
int ex_phy(struct bcmw *w);
int ex_link(struct bcmw *w);
int ex_enumerate(struct bcmw *w);
int ex_decode(struct bcmw *w);
uint32_t ex_cfg_rd(struct bcmw *w, uint32_t off);
void ex_cfg_wr(struct bcmw *w, uint32_t off, uint32_t v);
void ex_intx(struct bcmw *w, int on);
uint32_t ex_intx_ack(struct bcmw *w);
void ex_quiesce(struct bcmw *w);

/* ---- the chip (chip.c) ------------------------------------------------------------------------- */

#define CORE_CHIPCOMMON 0x800
#define CORE_INTERNAL_MEM 0x80E
#define CORE_80211      0x812
#define CORE_PMU        0x827
#define CORE_ARM_CM3    0x82A
#define CORE_PCIE2      0x83C
#define CORE_ARM_CR4    0x83E
#define CORE_GCI        0x840
#define CORE_ARM_CA7    0x847
#define CORE_SYS_MEM    0x849
#define BCMW_MAX_CORES  16

struct bcmw_core {
    uint16_t id;
    uint8_t rev;
    uint32_t base, wrap;
};

int chip_attach(struct bcmw *w);        /* ID, EROM, cores, passive, reset, RAM */
struct bcmw_core *chip_core(struct bcmw *w, uint16_t id);
uint32_t chip_bp_rd(struct bcmw *w, uint32_t addr);         /* backplane, through BAR0's window */
void chip_bp_wr(struct bcmw *w, uint32_t addr, uint32_t v);
void chip_select(struct bcmw *w, uint16_t core);            /* BAR0's window on a core */
uint32_t chip_reg_rd(struct bcmw *w, uint32_t off);         /* the selected core's register */
void chip_reg_wr(struct bcmw *w, uint32_t off, uint32_t v);
void chip_set_passive(struct bcmw *w);
int chip_set_active(struct bcmw *w, uint32_t rstvec);
void chip_cr4_state(struct bcmw *w, uint32_t *ioctl, uint32_t *resetctl, uint32_t *iost);
int chip_read_cis(struct bcmw *w, uint8_t *cis, uint32_t len);
void chip_watchdog_reset(struct bcmw *w);
uint32_t tcm_rd32(struct bcmw *w, uint32_t a);
void tcm_wr32(struct bcmw *w, uint32_t a, uint32_t v);
uint16_t tcm_rd16(struct bcmw *w, uint32_t a);
void tcm_wr16(struct bcmw *w, uint32_t a, uint16_t v);
void tcm_write(struct bcmw *w, uint32_t a, const uint8_t *src, uint32_t len);
void tcm_read(struct bcmw *w, uint32_t a, uint8_t *dst, uint32_t len);

/* ---- files (fw.c), pure functions: host-tested ------------------------------------------------- */

#define BCMW_NVRAM_MAX 64000
/* brcmfmac's NVRAM text to the chip's form: key=value\0... padded to 4, then the length token.
 * out must hold len + 32 bytes (BCMW_NVRAM_MAX + 32 at most). macaddr != NULL replaces macaddr=.
 * Returns the length written or < 0. */
int fw_nvram(const uint8_t *text, uint32_t len, uint8_t *out, uint32_t cap, const uint8_t *macaddr);
/* NVRAM value lookup in the processed form; NULL if absent */
const char *fw_nvram_get(const uint8_t *nv, uint32_t len, const char *key);

struct bcm4361_module {
    uint8_t vid[8];
    unsigned vid_length;
    int boardtype;
    char maker[8];
    char nvram[40];
    int known;
};
int fw_cis_parse(const uint8_t *cis, uint32_t length, struct bcm4361_module *m);
void fw_module_files(struct bcm4361_module *m, const char *rev_tag);
const char *fw_rev_tag(uint32_t chiprev);           /* "b0" for 3, 4; "b2" from 5; NULL before */
/* the firmware image: reset vector (first word), the RAM size override ("SMAR" at 0x6c), its
 * version string (Broadcom's "Version: ..." / "FWID" text near the end) */
struct fw_image_info {
    uint32_t rstvec, ramsize_override;
    char version[96];
};
int fw_image_check(const uint8_t *fw, uint32_t len, struct fw_image_info *fi);
/* CLM blob: is it one (Broadcom's "BLOB" container at the start) */
int fw_clm_check(const uint8_t *clm, uint32_t len);
/* one clmload chunk: the iovar's payload (flag, type, len, crc, data). Returns its length. */
uint32_t fw_clm_chunk(uint8_t *out, const uint8_t *data, uint32_t off, uint32_t total, uint32_t max_chunk);
/* an iovar buffer: name\0 then data. Returns its length or 0 if it does not fit. */
uint32_t fw_iovar(uint8_t *out, uint32_t cap, const char *name, const void *data, uint32_t len);
/* SHA-256; and which of the phone's known radio files a sum is (NULL: not one of them) */
void fw_sha256(const uint8_t *data, uint32_t len, uint8_t out[32]);
const char *fw_known(const uint8_t sum[32]);
/* a firmware event frame (Ethernet + Broadcom header + event message) parsed; 0 if it is one */
int fw_event_parse(const uint8_t *frame, uint32_t len, struct bcmw_event *ev);

/* ---- msgbuf (msgbuf.c) ------------------------------------------------------------------------- */

#define MSGBUF_TYPE_GEN_STATUS            0x1
#define MSGBUF_TYPE_RING_STATUS           0x2
#define MSGBUF_TYPE_FLOW_RING_CREATE      0x3
#define MSGBUF_TYPE_FLOW_RING_CREATE_CMPLT 0x4
#define MSGBUF_TYPE_FLOW_RING_DELETE      0x5
#define MSGBUF_TYPE_FLOW_RING_DELETE_CMPLT 0x6
#define MSGBUF_TYPE_IOCTLPTR_REQ          0x9
#define MSGBUF_TYPE_IOCTLPTR_REQ_ACK      0xA
#define MSGBUF_TYPE_IOCTLRESP_BUF_POST    0xB
#define MSGBUF_TYPE_IOCTL_CMPLT           0xC
#define MSGBUF_TYPE_EVENT_BUF_POST        0xD
#define MSGBUF_TYPE_WL_EVENT              0xE
#define MSGBUF_TYPE_TX_POST               0xF
#define MSGBUF_TYPE_TX_STATUS             0x10
#define MSGBUF_TYPE_RXBUF_POST            0x11
#define MSGBUF_TYPE_RX_CMPLT              0x12

#define RING_H2D_CONTROL  0
#define RING_H2D_RXPOST   1
#define RING_D2H_CONTROL  2
#define RING_D2H_TXCPL    3
#define RING_D2H_RXCPL    4
#define RING_COMMON       5

#define SHARED_VERSION_MASK   0x00FF
#define SHARED_DMA_INDEX      0x10000
#define SHARED_D2H_SYNC_SEQNUM 0x20000
#define SHARED_DMA_2B_IDX     0x100000
#define SHARED_HOSTRDY_DB1    0x10000000

#define CTL_BUF_SIZE   8192u    /* brcmfmac's BRCMF_MSGBUF_MAX_CTL_PKT_SIZE: 4 KiB was too small (an earlier port wifi-a6) */
#define CTL_BUFS       16u      /* 8 for ioctl responses, 8 for events */
#define RX_BUF_SIZE    2048u
#define RX_BUFS_MAX    256u
#define IOCTL_REQ_SIZE 1518u    /* BRCMF_TX_IOCTL_MAX_MSG_SIZE */
#define PKTID_CTL      0x4000u
#define TX_BUFS        128u     /* in flight to the firmware (W2n: 32 ran out when tx status lagged on a busy channel) */
#define TX_BUF_SIZE    2048u  /* request ids: CTL buffers 0x4000 + i, rx data buffers i */

int mb_shared(struct bcmw *w, uint32_t shared_addr);
uint32_t bcmw_escan_params(struct bcmw *w, uint8_t *p, const uint8_t *ssid, unsigned ssid_len, uint16_t sync);
int mb_rings(struct bcmw *w);
int mb_post_all(struct bcmw *w);
int mb_process(struct bcmw *w);
int mb_ioctl(struct bcmw *w, unsigned ifidx, uint32_t cmd, void *buf, uint32_t len, int *fwerr);
void mb_console(struct bcmw *w);

/* ---- link.c: the completions and events it needs ------------------------------------------------- */
void link_flow_created(struct bcmw *w, const uint8_t *it);
void link_tx_status(struct bcmw *w, const uint8_t *it);
void link_event(struct bcmw *w, const struct bcmw_event *ev);
void link_eapol(struct bcmw *w, const uint8_t *frame, uint32_t len);
void link_service(struct bcmw *w);

/* ---- the state ---------------------------------------------------------------------------------- */

struct bcmw {
    struct bcmw_os os;
    struct bcmw_cfg cfg;
    struct bcmw_info info;
    unsigned step;
    int dead;                   /* a guard tripped: nothing more is attempted */
    /* root complex */
    int reg_on, linked, handed_over, gates_open, phy_on;
    uint64_t bar_pa[6], bar_size[6];
    uint64_t bar0, bar1;        /* the chip's register window and its RAM window */
    uint64_t bar1_size;
    /* chip */
    struct bcmw_core cores[BCMW_MAX_CORES];
    unsigned ncores;
    uint32_t bar0_window;       /* what config 0x80 holds (ours) */
    uint32_t mb_int, mb_mask, h2d_mb0, h2d_mb1, int_d2h_db, int_fn0;
    /* shared area */
    uint32_t shared, flags, htod_mb, dtoh_mb, ring_info;
    uint32_t console_base, console_buf, console_size, console_ridx;
    char console_line[160];
    unsigned console_n, console_quota;
    unsigned rx_log;                    /* received frames still to be logged (bcmw_rx_log) */  /* lines still logged (48, more after a halt) */
    unsigned idx_size;          /* 0: indices in TCM (16-bit); 2 or 4: in host memory */
    uint8_t *idxbuf;
    uint64_t idxbuf_bus;
    uint16_t max_sub, max_cpl;
    struct bcmw_ring ring[RING_COMMON];
    uint8_t *ring_mem[RING_COMMON];     /* DMA memory is allocated once and reused by a later bring-up */
    uint64_t ring_bus[RING_COMMON];
    uint32_t idxbuf_size;
    /* buffers */
    uint8_t *ioctl_req;
    uint64_t ioctl_req_bus;
    uint8_t *ctl;               /* CTL_BUFS * CTL_BUF_SIZE */
    uint64_t ctl_bus;
    uint8_t ctl_posted[CTL_BUFS];
    uint8_t *rxb;
    uint64_t rxb_bus;
    unsigned nrx;
    uint8_t rx_posted[RX_BUFS_MAX];
    unsigned ioctlresp_posted, event_posted, rxdata_posted;
    uint8_t *scratch, *ringupd;
    uint64_t scratch_bus, ringupd_bus;
    /* the outstanding ioctl */
    uint16_t trans_id;
    volatile int ioctl_done;
    int ioctl_status;
    uint32_t ioctl_len, ioctl_pktid;
    /* files kept for the CLM step */
    const uint8_t *clm;
    uint32_t clm_len;
    /* the data path */
    struct bcmw_ring flow;
    uint8_t *flow_mem, *txb;
    uint64_t flow_bus, txb_bus;
    uint32_t flow_w_where, flow_r_where;
    int flow_state;                 /* 0 none, 1 asked for, 2 open, -1 refused */
    uint8_t tx_busy[TX_BUFS];
    /* joining */
    int scan_v2;                    /* the firmware answered scan_ver: v2 escan parameters */
    int scan_running;               /* step 10's escan has not ended yet */
    int join_phase;                 /* 0 idle, 1 scanning, 2 joining, 3 joined, -1 failed */
    uint64_t join_settle_t;             /* the end of the directed scan (the 300 ms settle) */
    uint64_t join_t0, join_timeout_us;  /* bcmw_join_start / bcmw_join_poll: SET_SSID's time, the limit */
    int join_open, join_fwsup, join_found, join_scan_done, link_up, keyed, keys_due;
    unsigned join_unsuitable, join_group;
    int join_rssi;
    uint8_t join_ssid[32], join_ssid_len, join_bssid[6];
    uint16_t join_chanspec;
    uint8_t rsn_ours[BCMW_RSN_OURS];
    struct bcmw_wpa wpa;
    uint8_t eapol_out[14 + BCMW_WPA_REPLY_MOST];    /* a handshake answer waiting to go (sent outside the completions) */
    unsigned eapol_out_len;
    int eapol_out_kind;
    /* WPA3 and management frame protection (link.c; the arithmetic in sae.c) */
    const struct bcmw_sae_ops *sae_ops;     /* bcmw_sae_enable(); NULL: no SAE in this build */
    uint32_t cap_sum;                       /* the "cap" string last logged (a checksum: logged again if it changes) */
    unsigned caps;                          /* BCMW_CAP_* (bcmw_caps) */
    unsigned join_akm;                      /* the AKM chosen: 0 open, 2 PSK, 6 PSK-SHA256, 8 SAE */
    int join_fwsae;                         /* SAE inside the firmware ("sae" + idsup) */
    int join_h2e;                           /* SAE by hash-to-element (the AP requires it) */
    int join_mfp;                           /* 0 none, 1 capable, 2 required: what "mfp" was set to */
    unsigned join_wpa3_refused;             /* WPA3-only access points of the name that this firmware cannot join */
    unsigned join_group_mgmt;               /* the AP's group management cipher (0: default BIP-CMAC-128) */
    uint16_t join_ap_caps;                  /* the AP's RSN capabilities */
    uint32_t join_ap_akms;                  /* the AKMs it offers (bit n: 00-0F-AC:n) */
    int join_no_sae;                        /* SAE failed on a transition network: WPA2-PSK instead */
    int sae_wrong;                          /* SAE failed on the AP's confirm: the password (no PSK fallback) */
    uint8_t join_psk[32];                   /* the PSK's PMK, kept for that fallback until the join ends */
    uint8_t join_ap_rsn[64], join_ap_rsnxe[16];     /* the chosen AP's RSN element and RSNXE (message 3's check) */
    unsigned join_ap_rsn_len, join_ap_rsnxe_len;
    uint8_t join_ie[BCMW_RSN_OURS + 3];     /* our RSN element (and RSNXE with H2E) for "wpaie" and message 2 */
    unsigned join_ie_len;
    char join_pass[68];                     /* the password, kept from _start to the choice of the AP (SAE needs it
                                             * with the AP's address), wiped then */
    unsigned join_pass_len;
    struct bcmw_sae sae;
    int sae_phase;                          /* 0 none, 1 waiting for EXT_AUTH_REQ, 2 exchanging, 3 done */
    int sae_req;                            /* EXT_AUTH_REQ seen: our commit to send (outside the completions) */
    uint8_t sae_rx[4][BCMW_SAE_FRAME_MOST]; /* received Authentication frames' bodies, processed by bcmw_join_poll */
    unsigned sae_rx_len[4], sae_rx_n;
    uint8_t sae_last[BCMW_SAE_FRAME_MOST];  /* the last frame we sent (sent again after 1 s without an answer) */
    unsigned sae_last_len, sae_tries;
    uint64_t sae_t;
    uint32_t sae_packet, sae_acked, sae_noack;
    int in_process;                 /* mb_process is running: it does not run again inside itself */
    uint8_t nvram_bin[BCMW_NVRAM_MAX + 32];
};

/* register access through the OS, with the allow-list */
uint32_t rd32(struct bcmw *w, uint64_t pa);
void wr32(struct bcmw *w, uint64_t pa, uint32_t v);
void set32(struct bcmw *w, uint64_t pa, uint32_t mask, uint32_t v);
void bcmw_udelay(struct bcmw *w, uint32_t us);
void bcmw_mdelay(struct bcmw *w, uint32_t ms);
uint64_t bcmw_now(struct bcmw *w);
void *bcmw_dma(struct bcmw *w, uint32_t size, uint32_t align, uint64_t *bus);
void bcmw_sync(struct bcmw *w, const void *p, uint32_t len, int to_device);
void bcmw_wmb(struct bcmw *w);
void bcmw_mb(struct bcmw *w);
void bcmw_rmb(struct bcmw *w);

#endif
