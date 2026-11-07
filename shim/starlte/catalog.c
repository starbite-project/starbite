/* SPDX-License-Identifier: GPL-2.0-only
 * The boot catalog: see catalog.h.
 */
#include <stdint.h>
#include "catalog.h"
#include "store.h"
#include "ui_glyphs.h"

#ifndef CAT_BUF                    /* the host test (tools/loader-test) points it at its own buffer */
#define CAT_BUF  0x8c000000UL      /* the OS staging area (xnu_boot.c), free until a choice is made */
#endif
#define CAT_SIZE (64U << 10)

static struct cat_ent cat[CAT_MAX];
static unsigned cat_n;
static int cat_state;              /* 0 not read, 1 from the store, 2 built in */

static uint64_t le(uintptr_t p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = n; i; --i)
        v = v << 8 | *(volatile uint8_t *)(p + i - 1);
    return v;
}

static void str_set(char *dst, unsigned cap, const char *src)
{
    unsigned k = 0;
    for (; src && k + 1 < cap && src[k]; ++k)
        dst[k] = src[k];
    for (; k < cap; ++k)
        dst[k] = 0;
}

/* String at offset off of a catalog of size total ("" for 0 or out of range). */
static void str_at(char *dst, unsigned cap, uintptr_t base, uint32_t off, uint32_t total)
{
    unsigned k = 0;
    if (off)
        for (; off + k < total && k + 1 < cap; ++k) {
            char c = (char)*(volatile uint8_t *)(base + off + k);
            if (!c)
                break;
            dst[k] = c;
        }
    for (; k < cap; ++k)
        dst[k] = 0;
}

static int parse(uintptr_t b, uint64_t size)
{
    if (size < 32 || le(b, 8) != 0x31474c5441434253ULL || le(b + 8, 4) != 1)   /* "SBCATLG1" */
        return 0;
    uint32_t n = (uint32_t)le(b + 12, 4), esz = (uint32_t)le(b + 16, 4), total = (uint32_t)le(b + 24, 4);
    if (esz != 64 || total > size || 32ULL + (uint64_t)n * 64U > total)
        return 0;
    /* The extension table (catalog.h): bounds checked as a whole, else ignored. */
    uint32_t ext = (uint32_t)le(b + 28, 4);
    if (ext && ((uint64_t)ext + 24U > total || le(b + ext, 8) != 0x3158455441434253ULL ||   /* "SBCATEX1" */
                le(b + ext + 8, 4) != 1 || le(b + ext + 12, 4) != n || le(b + ext + 16, 4) != 32 ||
                (uint64_t)ext + 24U + 32ULL * n > total))
        ext = 0;
    cat_n = 0;
    for (uint32_t i = 0; i < n && cat_n < CAT_MAX; ++i) {
        uintptr_t p = b + 32 + 64U * i;
        struct cat_ent *e = &cat[cat_n];
        for (unsigned k = 0; k < 24; ++k)
            e->key[k] = (char)*(volatile uint8_t *)(p + k);
        e->key[23] = 0;
        if (!e->key[0])
            continue;
        e->category = (uint8_t)le(p + 24, 1);
        e->method = (uint8_t)le(p + 25, 1);
        e->tag = (uint8_t)le(p + 26, 1);
        e->icon = (uint8_t)le(p + 27, 1);
        e->flags = (uint16_t)le(p + 28, 2);
        e->order = (uint16_t)le(p + 30, 2);
        str_at(e->name, sizeof(e->name), b, (uint32_t)le(p + 32, 4), total);
        str_at(e->version, sizeof(e->version), b, (uint32_t)le(p + 36, 4), total);
        for (unsigned f = 0; f < 4; ++f)
            str_at(e->file[f], sizeof(e->file[f]), b, (uint32_t)le(p + 40 + 4 * f, 4), total);
        str_at(e->cmdline, sizeof(e->cmdline), b, (uint32_t)le(p + 56, 4), total);
        str_at(e->root, sizeof(e->root), b, (uint32_t)le(p + 60, 4), total);
        uintptr_t x = ext ? b + ext + 24 + 32U * i : 0;
        for (unsigned k = 0; k < 4; ++k)
            str_at(e->log[k], sizeof(e->log[k]), b, x ? (uint32_t)le(x + 4 * k, 4) : 0, total);
        e->wdt_s = x ? (uint16_t)le(x + 16, 2) : 0;
        e->wdt_feed = x ? (uint8_t)le(x + 18, 1) : 0;
        e->cflags = x ? (uint8_t)le(x + 19, 1) : 0;
        ++cat_n;
    }
    return cat_n > 0;
}

static void def(const char *key, const char *name, const char *version, unsigned category,
    unsigned method, unsigned tag, const char *f0, const char *f1, const char *f2)
{
    struct cat_ent *e = &cat[cat_n];
    str_set(e->key, sizeof(e->key), key);
    str_set(e->name, sizeof(e->name), name);
    str_set(e->version, sizeof(e->version), version);
    str_set(e->file[0], sizeof(e->file[0]), f0);
    str_set(e->file[1], sizeof(e->file[1]), f1);
    str_set(e->file[2], sizeof(e->file[2]), f2);
    str_set(e->file[3], sizeof(e->file[3]), 0);
    str_set(e->cmdline, sizeof(e->cmdline), 0);
    str_set(e->root, sizeof(e->root), 0);
    for (unsigned k = 0; k < 4; ++k)
        str_set(e->log[k], sizeof(e->log[k]), 0);
    e->wdt_s = 0;
    e->wdt_feed = 0;
    e->cflags = 0;
    e->category = (uint8_t)category;
    e->method = (uint8_t)method;
    e->tag = (uint8_t)tag;
    e->icon = 0;
    e->flags = 0;
    e->order = (uint16_t)cat_n;
    ++cat_n;
}

/*
 * What the loader booted before catalogs: the same four entries. Linux 7.1 has
 * no built-in initramfs, so its entry carries the store's initrd and starts the
 * rescue shell (sb.root=none): a store without a readable catalog still gets a
 * USB shell, and a store without linux.initrd fails back to the menu.
 */
static void builtin(void)
{
    cat_n = 0;
    def("ios26", "iOS 26", "26.6.2 " UI_DOT " XNU 12377", CAT_APPLE, M_XNU, TAG_BETA, "@boot", 0, 0);
    def("win11", "Windows 11", "23H2 " UI_DOT " build 22631", CAT_MICROSOFT, M_UEFI, TAG_EXPERIMENTAL, "windows.uefi|@bundle", 0, 0);
    def("android", "One UI 8", "Android 16 " UI_DOT " UN1CA 9810 " UI_DOT " Linux 4.9", CAT_ANDROID, M_ANDROID, TAG_EXPERIMENTAL,
        "android.kernel", "android.ramdisk", "android.cmdline");
    def("linux", "Linux", "rescue shell", CAT_LINUX, M_LINUX, TAG_EXPERIMENTAL, "linux.image", "linux.dtb",
        "linux.initrd");
    str_set(cat[cat_n - 1].cmdline, sizeof(cat[cat_n - 1].cmdline), "sb.root=none");
}

int cat_load(void)
{
    if (cat_state)
        return (int)cat_n;
    struct store_ent e;
    if (store_find("catalog", &e) && store_load(&e, CAT_BUF, CAT_SIZE) == 0 && parse(CAT_BUF, e.size)) {
        cat_state = 1;
    } else {
        builtin();
        cat_state = 2;
    }
    return (int)cat_n;
}

unsigned cat_count(void)
{
    (void)cat_load();
    return cat_n;
}

const struct cat_ent *cat_get(unsigned i)
{
    return i < cat_count() ? &cat[i] : 0;
}

int cat_find(const char *key)
{
    for (unsigned i = 0; i < cat_count(); ++i) {
        unsigned k = 0;
        while (k < 23 && key[k] && cat[i].key[k] == key[k])
            ++k;
        if (key[k] == cat[i].key[k] || k == 23)
            return (int)i;
    }
    return -1;
}

int cat_builtin(void)
{
    (void)cat_load();
    return cat_state == 2;
}

int cat_alt(const char *field, unsigned k, char *out)
{
    unsigned i = 0;
    while (k && field[i]) {
        if (field[i++] == '|')
            --k;
    }
    if (k || !field[i])
        return 0;
    unsigned n = 0;
    while (field[i] && field[i] != '|' && n < 63)
        out[n++] = field[i++];
    out[n] = 0;
    return n > 0;
}
