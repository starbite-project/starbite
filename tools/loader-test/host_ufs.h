/* Host stand-in for shim/starlte/ufs.h (tools/loader-test/store_test.c): the loader's store.c is
 * compiled against this instead (run.sh rewrites its include), with its fixed DMA buffer as a host
 * array and the UFS / GPT calls served by store_test.c from an image file. */
#pragma once
#include <stdint.h>

extern uint8_t host_ufs_buf[];
#define UFS_BUF ((uintptr_t)host_ufs_buf)
#define UFS_BUF_SIZE (1U << 20)

int ufs_up(void);
int ufs_read(uint64_t lba, uint32_t count);
int ufs_read_to(uint64_t lba, uint64_t count, uintptr_t dst);
int ufs_write_block(uint64_t lba, uintptr_t src);
int gpt_find(const char *name, uint64_t *first, uint64_t *last);
