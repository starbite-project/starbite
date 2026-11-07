/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS app files (.sba, docs/STARBITE-OS.md 6.1): the check and the placement. Pure C: the host
 * test feeds it the real built apps and broken copies of them. A file that fails any check is never
 * placed, so a bad or hostile file cannot make the kernel write outside the app's slot. */
#include "kern.h"

#define R_AARCH64_RELATIVE 1027U

struct rela {
    uint64_t off, info;
    int64_t add;
};

uint32_t app_crc(const void *file, uint64_t n)
{
    /* CRC32 (IEEE, reflected) with the header's crc32 field (offset 0x60) read as zero */
    const uint8_t *p = file;
    uint32_t c = 0xffffffffU;
    for (uint64_t i = 0; i < n; ++i) {
        uint8_t b = (i >= 0x60 && i < 0x64) ? 0 : p[i];
        c ^= b;
        for (unsigned k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1U)));
    }
    return ~c;
}

static int str_ok(const char *s, unsigned cap)
{
    if (!s[0])
        return 0;
    for (unsigned i = 0; i < cap; ++i) {
        if (!s[i])
            return 1;
        if (s[i] < 32 || s[i] > 126)
            return 0;
    }
    return 0;                                   /* not terminated */
}

const char *app_check(const void *file, uint64_t n, uint64_t slot_size)
{
    const struct sbapp_header *h = file;
    if (n < sizeof(*h))
        return "shorter than its header";
    for (unsigned i = 0; i < 8; ++i)
        if (h->magic[i] != SBAPP_MAGIC[i])
            return "not an app file";
    if (h->api == 0 || h->api > SBOS_API)
        return "needs a newer Starbite OS";
    if (h->header_size != sizeof(*h))
        return "header size";
    if (!str_ok(h->id, sizeof(h->id)) || !str_ok(h->name, sizeof(h->name)))
        return "id or name";
    for (unsigned i = 0; h->id[i]; ++i) {
        char c = h->id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return "id characters";
    }
    if (h->file_size != n)
        return "file size";
    uint64_t stack = h->stack_size ? h->stack_size : (64U << 10);
    if (stack > APP_STACK_MAX || (stack & 15U))
        return "stack size";
    if ((uint64_t)h->file_size + h->bss_size + stack + APP_MAILBOX > slot_size)
        return "too large for a slot";
    if (h->entry < sizeof(*h) || h->entry >= n || (h->entry & 3U))
        return "entry";
    if (h->rela_count) {
        if (h->rela_off < sizeof(*h) || (h->rela_off & 7U) || h->rela_count > (n / sizeof(struct rela)) ||
            (uint64_t)h->rela_off + (uint64_t)h->rela_count * sizeof(struct rela) > n)
            return "relocation table";
        const struct rela *r = (const struct rela *)((const uint8_t *)file + h->rela_off);
        uint64_t image = (uint64_t)h->file_size + h->bss_size;
        for (uint32_t i = 0; i < h->rela_count; ++i) {
            if ((uint32_t)r[i].info != R_AARCH64_RELATIVE)
                return "relocation type";
            if (r[i].off < sizeof(*h) || r[i].off + 8 > n || (r[i].off & 7U))
                return "relocation target";
            if (r[i].add < 0 || (uint64_t)r[i].add > image)
                return "relocation value";
        }
    }
    if (app_crc(file, n) != h->crc32)
        return "damaged (CRC)";
    return 0;
}

void app_place(const void *file, uint64_t n, uintptr_t slot)
{
    const struct sbapp_header *h = file;
    uint8_t *d = (uint8_t *)slot;
    const uint8_t *s = file;
    for (uint64_t i = 0; i < n; ++i)
        d[i] = s[i];
    for (uint64_t i = 0; i < h->bss_size; ++i)
        d[n + i] = 0;
    const struct rela *r = (const struct rela *)(s + h->rela_off);
    for (uint32_t i = 0; i < h->rela_count; ++i) {
        uint64_t v = (uint64_t)slot + (uint64_t)r[i].add;
        uint8_t *p = d + r[i].off;
        for (unsigned k = 0; k < 8; ++k)
            p[k] = (uint8_t)(v >> (8 * k));
    }
}
