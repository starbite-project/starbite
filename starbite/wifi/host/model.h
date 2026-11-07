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
    int start_fw, fw_running, offer_dma_idx, up, scan_v2;
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

#endif
