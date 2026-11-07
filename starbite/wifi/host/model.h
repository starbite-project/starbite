/* SPDX-License-Identifier: GPL-2.0-only
 * The hardware model of host/model.c, for host/test.c. */
#ifndef BCMW_MODEL_H
#define BCMW_MODEL_H
#include <stdint.h>

struct model {
    uint64_t now;                       /* us */
    uint64_t reads, writes;
    unsigned errors;                    /* the model saw something the phone would not take */
    /* the power sequence */
    int reg_on, perst_high, linked;
    uint64_t t_reg_on, reg_on_seen_low, t_perst, t_pmu, t_cmu, t_ltssm;
    /* the endpoint's configuration space */
    uint32_t cfg[0x400];
    int sizing[4];
    /* the chip */
    uint32_t cr4_ioctl, cr4_reset, mailboxint;
    int start_fw, fw_running, offer_dma_idx, up, scan_v2, hold_answer, release_answer, bad_console, phy_powered_down;
    /* the access point (model_ap.c) */
    int fwsup, handshake_done, pmk_ok;
    unsigned tx_seen, tx_ip, ap_errors, keys_ok, flow_created, disassoc, rekey_answered;
    unsigned watchdog_resets, boots, doorbells, hostready, ioctls, badoption;
    unsigned fw_errors;                 /* bits: what the model's firmware found wrong */
    uint32_t fw_rstvec, nvram_seen, clm_done;
    const uint8_t *fw_image;
    uint32_t fw_image_len;
    uint8_t mac_set[6], event_mask[16];
    char last_iovar[32];
    uint32_t dma_bytes;
};
extern struct model M;

void model_reset(void);
uint32_t model_rd32(void *ctx, uint64_t pa);
void model_wr32(void *ctx, uint64_t pa, uint32_t v);
uint16_t model_rd16(void *ctx, uint64_t pa);
void model_wr16(void *ctx, uint64_t pa, uint16_t v);
void model_udelay(void *ctx, uint32_t us);
uint64_t model_now(void *ctx);
void *model_dma_alloc(void *ctx, uint32_t size, uint32_t align, uint64_t *bus);
void model_rx(const uint8_t *f, uint32_t len);
void model_event(uint32_t type, uint32_t status, uint16_t flags, uint32_t reason, const uint8_t *data, uint32_t dlen);
void model_ap_reset(void);
int model_ap_selftest(void);
void model_ap_tx(const uint8_t *f, unsigned len);
unsigned model_ap_scan_result(uint8_t *r, unsigned cap, uint16_t sync);
void model_ap_rekey(void);
int model_ap_ioctl(uint32_t cmd, const char *name, const uint8_t *data, uint32_t dlen, uint8_t *out, uint16_t *rlen,
                   uint16_t *status);
void model_ap_after(uint32_t cmd);

#endif
