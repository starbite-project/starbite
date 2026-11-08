/* SPDX-License-Identifier: GPL-2.0-only
 * bcmw: a portable core for the Galaxy S9's Wi-Fi, the Broadcom BCM4361 on PCIe channel 0 of the
 * Exynos 9810 (docs/WIFI-BCM4361.md). One C core, no OS in it: the root complex brought up by hand
 * (no OS can enumerate this controller), the chip attached through its BAR0 window, firmware and
 * NVRAM written into its RAM, brcmfmac's msgbuf protocol on rings in host memory, ioctls and
 * events. Used by Starbite OS (libsb shim) and by the Windows driver (KMDF shim, NDIS/WiFiCx on
 * top).
 *
 * The OS gives the core a struct bcmw_os: register access by physical address, a delay and a
 * clock, DMA memory, files, a log line. Everything else is here. DMA memory is asked for once, at
 * step 9, and reused by a later bring-up after bcmw_quiesce(); nothing blocks except the delays the
 * hardware asks for and the bounded waits.
 *
 * What it is modelled on (facts and sequences, credited at the top of each file):
 *  - Samsung's GPL kernel for the S9 (drivers/pci/host/pci-exynos*.c, bcmdhd_100_10), through
 *    two drivers that run on this phone: an earlier port's exynos_pcie.c (XNU, phone-tested to WPA2
 *    and ping on 3 Nov 2026) and the mainline port's pcie-exynos9810.c (Linux 7.1, pmOS/ChromeOS);
 *  - Linux's brcmfmac (ISC, Broadcom) for the chip, the firmware download and msgbuf.
 */
#ifndef BCMW_H
#define BCMW_H
#include <stdint.h>
#include <stddef.h>

/* ---- what the OS gives the core --------------------------------------------------------------- */

struct bcmw_event;

struct bcmw_os {
    void *ctx;
    /* 32-bit and 16-bit register access by physical address (the shim maps; Device memory).
     * The core only ever writes inside bcmw_write_allowed(); the shim may check it again. */
    uint32_t (*rd32)(void *ctx, uint64_t pa);
    void (*wr32)(void *ctx, uint64_t pa, uint32_t v);
    uint16_t (*rd16)(void *ctx, uint64_t pa);
    void (*wr16)(void *ctx, uint64_t pa, uint16_t v);
    /* optional: a read that survives a bus fault (libsb's sb_probe32): 1 read, 0 faulted. Used
     * for the first read of each block whose clock or power is not known to be on. */
    int (*probe32)(void *ctx, uint64_t pa, uint32_t *v);
    /* barriers (NULL on the host). wmb: DMA memory writes before the register write that tells the
     * chip (dsb st). mb: everything before, before what follows (dsb sy): before every ring index the
     * host writes, so the items are in memory before the chip sees the index, and the items read are
     * read before the slots are given back. rmb: a ring index read before the items it covers are
     * read (dsb ld). DMA memory is Normal non-cacheable and the chip's RAM is Device memory: their
     * accesses are not ordered with each other without these. */
    void (*wmb)(void *ctx);
    void (*mb)(void *ctx);
    void (*rmb)(void *ctx);
    void (*udelay)(void *ctx, uint32_t us);
    uint64_t (*now_us)(void *ctx);
    /* DMA memory, physically contiguous, below 4 GiB, never freed. *bus is what the chip is
     * given (the physical address: the PCIe SysMMU is in bypass). The memory is uncached unless
     * coherent was asked for and given (bcmw_cfg.coherent). NULL when there is no more. */
    void *(*dma_alloc)(void *ctx, uint32_t size, uint32_t align, uint64_t *bus);
    /* cache maintenance for cached, non-coherent DMA memory; NULL when uncached or coherent.
     * to_device 1: clean before the chip reads; 0: invalidate before the CPU reads. */
    void (*dma_sync)(void *ctx, const void *p, uint32_t len, int to_device);
    /* a whole file by name (bcmdhd's names, below), into memory the core reads once. 0 on success.
     * The firmware, NVRAM and CLM blob are Broadcom's and Samsung's: read from the phone's own
     * VENDOR partition at run time, never shipped with the code. */
    int (*file)(void *ctx, const char *name, const uint8_t **data, uint32_t *len);
    void (*log)(void *ctx, const char *line);
    /* the firmware's events and received frames (may be NULL) */
    void (*event)(void *ctx, const struct bcmw_event *ev);
    void (*rx)(void *ctx, unsigned ifidx, const uint8_t *frame, uint32_t len);
    /* n random bytes (the handshake's nonce); 0 on success. NULL: a weak timing-jitter fallback is used */
    int (*random)(void *ctx, uint8_t *out, uint32_t n);
};

/* ---- configuration ----------------------------------------------------------------------------- */

struct bcmw_cfg {
    unsigned stop_after;    /* bring-up steps, as an earlier port's wifi_pcie=/brcmfmac= (BCMW_STEP_*) */
    unsigned link_speed;    /* the root port's target speed: 2 Gen2 (an earlier port, default), 1 Gen1 (mainline) */
    unsigned link_tries;    /* PHY + training attempts (3; Android does 10) */
    int coherent;           /* 1: SYSREG_FSYS1 shareability set, DMA memory may be cached (Linux's way) */
    int dma_indices;        /* 1: the ring indices in host memory by the chip's DMA if it offers it;
                             * 0 (default, an earlier port's working setting): in the chip's RAM */
    int ungate;             /* 1: force closed CMU_FSYS1 gates of the controller open */
    unsigned rx_buffers;    /* rx data buffers posted (<= the firmware's max_rxbufpost; 64) */
    const char *nvram;      /* NULL: chosen from the module's ID in OTP (bcm4361_module_files) */
    const char *firmware;   /* NULL: bcmdhd_sta.bin_b2 / _b0 by the chip's revision */
    int any_files;          /* 1: files whose SHA-256 is not one of the phone's known ones are used too
                             * (default: refused, so a reader's mistake never reaches the chip) */
    uint8_t mac[6];         /* all zero: the firmware's own (NVRAM/OTP); else set as cur_etheraddr */
    int fw_supplicant;      /* 1: the firmware's own supplicant when "cap" lists idsup (not yet seen on this
                             * phone); 0 (default): the host's four-way handshake, as an earlier port joined */
    int diag;               /* 1: step 8 verifies the whole download, dumps the RAM's tail and logs the ARM's
                             * state while it waits for the firmware (W1b: the firmware did not start under
                             * Windows) */
};

enum {
    BCMW_STEP_LOOK = 1,     /* reads only: PMU isolation, CMU gates, the pins */
    BCMW_STEP_POWER,        /* PERST# low, CLKREQ#, WLAN_REG_ON low 50 ms then high 200 ms */
    BCMW_STEP_PHY,          /* PMU on, gates checked, the PHY out of power-down */
    BCMW_STEP_LINK,         /* soft reset, PERST# high, PHY tables and resets, root port, LTSSM: L0 */
    BCMW_STEP_ENUM,         /* 1:00.0's ID, BARs placed */
    BCMW_STEP_DECODE,       /* memory decoding, bus mastering, the DMA path's gates */
    BCMW_STEP_CHIP,         /* BAR0 window: chip ID, cores, RAM size; the module's ID from OTP */
    BCMW_STEP_FIRMWARE,     /* firmware + NVRAM into RAM, ARM started, the shared area read */
    BCMW_STEP_RINGS,        /* rings, buffers posted, the first ioctl ("ver"), the MAC, CLM */
    BCMW_STEP_UP,           /* event mask, "up", a scan */
    BCMW_STEP_ALL = BCMW_STEP_UP
};

/* ---- events and results ------------------------------------------------------------------------ */

struct bcmw_event {
    uint32_t type, status, reason, auth_type, datalen;
    uint16_t flags;
    uint8_t addr[6], ifidx, bsscfgidx;
    char ifname[17];
    const uint8_t *data;    /* datalen bytes after the event message */
};

/* firmware event types used here (brcmfmac fweh.h) */
#define BCMW_E_SET_SSID         0
#define BCMW_E_JOIN             1
#define BCMW_E_AUTH             3
#define BCMW_E_DEAUTH           5
#define BCMW_E_DEAUTH_IND       6
#define BCMW_E_ASSOC            7
#define BCMW_E_DISASSOC_IND     12
#define BCMW_E_LINK             16
#define BCMW_E_IF               54
#define BCMW_E_ESCAN_RESULT     69

#define BCMW_OK          0
#define BCMW_E_NODEV    -1  /* nothing answers / not there */
#define BCMW_E_TIMEOUT  -2
#define BCMW_E_FW       -3  /* the firmware refused (fwerr has its code) */
#define BCMW_E_NOMEM    -4
#define BCMW_E_BADFILE  -5
#define BCMW_E_STATE    -6  /* not up to that step */
#define BCMW_E_REFUSED  -7  /* a register outside the allow-list, or a guard tripped */
#define BCMW_E_PROTO    -8  /* the shared area or a ring is not what the protocol says */

/* ---- the device -------------------------------------------------------------------------------- */

struct bcmw;    /* opaque; bcmw_size() bytes, from the OS */

uint32_t bcmw_size(void);
int bcmw_init(struct bcmw *w, const struct bcmw_os *os, const struct bcmw_cfg *cfg);
/* Run the bring-up from where it is up to cfg.stop_after (or `step`, if smaller). Each step logs
 * what it does before it does it. Returns the last step reached (>= 0) or a BCMW_E_*. */
int bcmw_bringup(struct bcmw *w, unsigned step);
unsigned bcmw_step(const struct bcmw *w);
/* Service the chip: the mailbox, the completion rings, the firmware console. Call it from the
 * interrupt's deferred part, or every few ms when polling. Returns the items processed. */
int bcmw_poll(struct bcmw *w);
/* For an INTx handler: 1 if the chip has something (its mailbox interrupts are then masked
 * until the next bcmw_poll, which unmasks). Clears the root complex's latch. */
int bcmw_isr(struct bcmw *w);
/* Take the chip down for a way back to the loader / unload: interrupts off, the chip held in
 * reset (PERST#), the PHY powered down, the chip unpowered (WLAN_REG_ON). Safe at any step, twice,
 * and after a fault. The core never calls it by itself: the caller does, also after a failed
 * bring-up. */
void bcmw_quiesce(struct bcmw *w);

/* Firmware commands, synchronous (they poll until the answer or 2 s). fwerr: the firmware's own
 * status (0 = fine), may be NULL. */
int bcmw_ioctl(struct bcmw *w, unsigned ifidx, uint32_t cmd, void *buf, uint32_t len, int set, int *fwerr);
int bcmw_iovar_get(struct bcmw *w, const char *name, void *buf, uint32_t len, int *fwerr);
int bcmw_iovar_set(struct bcmw *w, const char *name, const void *buf, uint32_t len, int *fwerr);
int bcmw_iovar_get_u32(struct bcmw *w, const char *name, uint32_t *v);
/* a get whose request carries input after the name (e.g. "sta_info" + the peer's address) */
int bcmw_iovar_getbuf(struct bcmw *w, const char *name, const void *in, uint32_t inlen, void *out, uint32_t outlen,
                      int *fwerr);
int bcmw_iovar_set_u32(struct bcmw *w, const char *name, uint32_t v);

/* ---- the data path and joining (link.c); after step 10 (up) ------------------------------------- */

/* An Ethernet frame (header included, at most 14 + 2048 bytes) to send; copied, so the caller keeps it.
 * The first one creates the tx flow ring (waits up to 2 s for it). Not from inside the event/rx callbacks
 * before the flow ring is open (BCMW_E_STATE): those run inside bcmw_poll. */
int bcmw_tx(struct bcmw *w, const uint8_t *frame, uint32_t len);
/* Join a network: pass "" for an open one, else a WPA2 passphrase (8..63 characters). Synchronous: scans for
 * the name, joins the strongest suitable access point, does the handshake (the firmware's if it has one, else
 * the host's), installs the keys. timeout_ms 0: 15 s. The passphrase and keys are never logged. */
int bcmw_join(struct bcmw *w, const uint8_t *ssid, unsigned ssid_len, const char *pass, unsigned timeout_ms);
/* The same in two parts, for a caller that must not block (Starbite OS): _start scans for the name, makes the PMK and
 * sends SET_SSID (a few seconds at most); _poll does one round of the handshake: BCMW_OK joined, BCMW_E_PENDING not
 * yet (call again), any other BCMW_E_* failed (the join is left). The passphrase is not kept after _start. */
#define BCMW_E_PENDING  -9
int bcmw_join_start(struct bcmw *w, const uint8_t *ssid, unsigned ssid_len, const char *pass, unsigned timeout_ms);
int bcmw_join_poll(struct bcmw *w);
int bcmw_leave(struct bcmw *w);
int bcmw_link_up(const struct bcmw *w);

/* ---- the pieces, for an OS with its own supplicant and connection manager (Windows' WiFiCx) ------ */

struct bcmw_bss {                   /* one access point from an escan result event */
    uint8_t bssid[6], ssid[32], ssid_len;
    uint16_t chanspec, capability, beacon_period;
    unsigned channel, band5;        /* channel number; 1 for 5 GHz */
    int rssi;
    const uint8_t *ie;              /* its information elements (inside the event: copy them) */
    uint32_t ie_len;
};
/* the next n received data frames are logged (interface, ethertype, length, flags; no addresses or payload) */
void bcmw_rx_log(struct bcmw *w, unsigned n);
/* one line: the rx-post ring (our write index, the firmware's read index: how many of our buffers it took) and
 * the rx-completion ring, the event and frame counts (W2e: after association the firmware dropped packets) */
void bcmw_rx_state(struct bcmw *w, const char *when);
/* n more firmware console lines go to the log (it stops after a quota, to keep the log short) */
void bcmw_console_more(struct bcmw *w, unsigned n);
/* the radio's own address (cur_etheraddr), for an OS that picks one per network; not while associated */
int bcmw_set_mac(struct bcmw *w, const uint8_t mac[6]);
/* the control (primary 20 MHz) channel of a d11ac chanspec (its low byte is the center for 40 MHz and wider) */
unsigned bcmw_chanspec_channel(uint16_t chanspec);
/* 1 if ev is an escan result carrying a BSS (status 8), into *b; *sync = the scan's sync id */
int bcmw_bss_parse(const struct bcmw_event *ev, struct bcmw_bss *b, uint16_t *sync);
/* 1 if ev ends the escan with this sync id (status other than 8) */
int bcmw_scan_ended(const struct bcmw_event *ev, uint16_t sync);
/* start an escan (all channels; a name or none); results and its end come as events */
int bcmw_scan_start(struct bcmw *w, const uint8_t *ssid, unsigned ssid_len, uint16_t sync);
/* Associate without the core's own handshake: open (rsn_ie NULL) or WPA2-PSK with the OS's supplicant
 * (rsn_ie = the RSN element for the association request; group TKIP allowed: wsec gets TKIP too).
 * Non-blocking: SET_SSID, LINK, DEAUTH... come as events; EAPOL frames go to os->rx. */
int bcmw_connect(struct bcmw *w, const uint8_t *ssid, unsigned ssid_len, const uint8_t bssid[6], const uint8_t *rsn_ie,
                 unsigned rsn_len, int group_tkip);
/* a key into the firmware (wsec_key): pairwise with the peer's address (flags 0), group without (primary);
 * algo: 4 AES-CCM, 2 TKIP; rsc: the 6-byte receive sequence counter or NULL */
int bcmw_set_key(struct bcmw *w, unsigned index, const uint8_t *key, unsigned len, unsigned algo, const uint8_t *ea,
                 const uint8_t *rsc);
/* the association's request and response IEs as the firmware sent / got them (assoc_info, assoc_req_ies,
 * assoc_resp_ies); lengths in and out */
int bcmw_assoc_ies(struct bcmw *w, uint8_t *req, uint32_t *req_len, uint8_t *resp, uint32_t *resp_len);

/* what the bring-up learned */
struct bcmw_info {
    uint16_t vendor, device;    /* PCI IDs of 1:00.0 */
    uint32_t chip, chiprev;     /* chipcommon's ID (0x4361 or 0x4347) and revision (5 = B2) */
    uint32_t ramsize, rambase;
    uint32_t link_gen, link_width;
    uint8_t module_vid[2];      /* from OTP; 0 0 if not found */
    char nvram_name[40], fw_name[32];
    uint32_t shared_addr, shared_version, shared_flags;
    uint32_t max_rxbufpost, rx_dataoffset, max_flowrings;
    char fw_version[160];       /* the "ver" iovar */
    uint8_t mac[6];
    uint32_t polls, interrupts, ioctls, events, rx_frames, console_lines;
    uint32_t tx_frames, tx_done, tx_errors;
};
const struct bcmw_info *bcmw_info(const struct bcmw *w);

/* ---- what is safe to write (also for the shims and the host test) ------------------------------ */

/* 1 if a 32-bit write at physical address pa is one the core may make: the PCIe block's own
 * registers, the endpoint's window, and single named words of the PMU, CMU_TOP, CMU_FSYS1,
 * SYSREG_FSYS1 and three GPIO banks. Never the MIF or INT CMUs, never anything else. */
int bcmw_write_allowed(uint64_t pa);

/* the firmware command numbers used (brcmfmac fwil.h) */
#define BCMW_C_GET_VERSION      1
#define BCMW_C_UP               2
#define BCMW_C_DOWN             3
#define BCMW_C_SET_INFRA        20
#define BCMW_C_SET_AUTH         22
#define BCMW_C_SET_SSID         26
#define BCMW_C_GET_PM           85
#define BCMW_C_SET_PM           86
#define BCMW_C_SET_SCAN_CHANNEL_TIME   185
#define BCMW_C_GET_VAR          262
#define BCMW_C_SET_VAR          263
#define BCMW_C_SET_WSEC_PMK     268

#endif
