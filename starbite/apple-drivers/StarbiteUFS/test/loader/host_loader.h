/* SPDX-License-Identifier: APSL-2.0 */
/* Host stand-in for shim/starlte/ufs.h + store.h, for shim/starlte/ufs_fence.c and ufs.c's GPT reader in the
 * StarbiteUFS loader test (t_loader.c): UFS_DMA / UFS_BUF are a host array, the UFS and store calls are fakes. */
#pragma once
#include <stdint.h>

extern uint8_t host_ufs_dma[];
#define UFS_DMA ((uintptr_t)host_ufs_dma)
#define UFS_BUF (UFS_DMA + 0x4000)
#define UFS_BUF_SIZE (1U << 20)

static inline uint8_t u8r(uintptr_t a)
{
    return *(volatile uint8_t *)a;
}

int ufs_up(void);
int ufs_read(uint64_t lba, uint32_t count);

#define GPT_MAX 48
extern char gpt_name[GPT_MAX][24];
extern uint64_t gpt_first[GPT_MAX], gpt_last[GPT_MAX];
extern int gpt_n;
int gpt_load(void);
int gpt_find(const char *name, uint64_t *first, uint64_t *last);

#define STORE_UFS 0U
#define STORE_SD 1U
int store_home(void);
int store_card_only(void);
