/* SPDX-License-Identifier: GPL-2.0-only
 * The golden NVRAM: Linux's brcmfmac parser (firmware.c, ISC, Broadcom), taken from a Linux tree at
 * test time by host/run.sh (BCMW_LINUX=<brcmfmac dir>) and built here on a small shim, so that
 * fw_nvram() can be compared with it byte for byte. Nothing of Linux is kept in this repository.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint32_t __le32;
struct device;
#define GFP_KERNEL 0
#define ETH_ALEN 6
#define kzalloc(n, f) calloc(1, (n))
#define kfree free
#define brcmf_dbg(...) do { } while (0)
#define brcmf_err(...) do { } while (0)
#define WARN_ON(x) (x)
#define cpu_to_le32(x) (x)
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#define scnprintf snprintf
static int eth_platform_get_mac_address(struct device *dev, u8 *mac) { (void)dev; (void)mac; return -1; }
static char *strnchr(const char *s, size_t n, int c)
{
    for (size_t i = 0; i < n && s[i]; i++)
        if (s[i] == c)
            return (char *)s + i;
    return 0;
}

#include "linux-nvram.inc"

int golden_nvram(const uint8_t *data, uint32_t len, uint8_t **out, uint32_t *outlen);
int golden_nvram(const uint8_t *data, uint32_t len, uint8_t **out, uint32_t *outlen)
{
    /* brcmfmac drops a last entry without a newline; bcmw keeps it. Give both a newline. */
    uint8_t *copy = malloc(len + 2);
    memcpy(copy, data, len);
    copy[len] = '\n';
    copy[len + 1] = 0;
    *out = brcmf_fw_nvram_strip(copy, len + 1, outlen, 0, 0, 0);
    free(copy);
    return *out ? 0 : -1;
}
