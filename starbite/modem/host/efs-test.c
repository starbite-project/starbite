/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* sbm host test: the EFS reader (sb/ext4.c + sb/jbd2.c) on images made by mk-efs-fixture.sh: a file system
 * whose newest nv_data.bin is only complete through its journal must read back as that newest file (the
 * in-memory replay), a cleanly unmounted one as itself. Usage: efs-test expected.bin img... */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../sb/ext4.h"
#include "../sb/jbd2.h"

static FILE *img;
static const struct jbd2 *jr;
static uint32_t bs = 4096;
static uint8_t scratch[4096];
static int raw(void *ctx, uint64_t off, void *dst, uint32_t len)
{
    (void)ctx;
    return fseek(img, (long)off, SEEK_SET) || fread(dst, 1, len, img) != len ? -1 : 0;
}
static int rd(void *ctx, uint64_t off, void *dst, uint32_t len) { return jbd2_read(jr, bs, raw, ctx, off, dst, len, scratch, sizeof(scratch)); }

static int read_nv(uint8_t *out, uint32_t size, int replay, struct jbd2 *j, struct jbd2_map *map, unsigned cap)
{
    static struct ext4 fs;
    uint8_t sb[256], inode[EXT4_INODE_MAX];
    uint32_t ino;
    jr = 0;
    fs.read = rd;
    fs.ctx = 0;
    if (raw(0, 1024, sb, sizeof(sb)) || ext4_open(&fs))
        return -10;
    bs = fs.bs;
    uint32_t incompat = sb[0x60] | sb[0x61] << 8 | sb[0x62] << 16 | (uint32_t)sb[0x63] << 24;
    uint32_t jinum = sb[0xE0] | sb[0xE1] << 8 | sb[0xE2] << 16 | (uint32_t)sb[0xE3] << 24;
    if (replay && (incompat & 4)) {
        int e = jbd2_scan(&fs, jinum, j, map, cap);
        printf("  journal: %d %s start %u tx %u blocks %u revoked %u features 0x%x%s\n", e, j->why ? j->why : "ok",
               j->start, j->transactions, j->n, j->skipped_revoked, j->features,
               j->dropped_incomplete ? " +incomplete" : "");
        if (e)
            return -11;
        jr = j;
        if (ext4_open(&fs))
            return -12;
    }
    if (ext4_lookup(&fs, "/nv_data.bin", &ino) || ext4_inode(&fs, ino, inode))
        return -13;
    if (ext4_size(inode) != size)
        return -14;
    for (uint32_t o = 0; o < size; o += 4096)
        if (ext4_read(&fs, inode, o, out + o, 4096))
            return -15;
    return 0;
}

/* --inspect IMG: what the phone's loader would see, summary only: superblock facts, the journal, the root's
 * names (runs of 7+ digits masked), nv_data.bin's inode and size and the CRC-32 of what the reader returns. No
 * file content is printed and nothing is written anywhere. */
static unsigned nnames;
static void name_cb(void *ctx, const char *name, unsigned len, uint32_t ino)
{
    (void)ctx;
    char b[260];
    unsigned run = 0;
    for (unsigned i = 0; i < len && i < 255; ++i) {
        char c = name[i] >= 0x20 && name[i] < 0x7F ? name[i] : '?';
        b[i] = c;
        run = c >= '0' && c <= '9' ? run + 1 : 0;
        if (run >= 7)
            for (unsigned k = i + 1 - run; k <= i; ++k)
                b[k] = '#';
    }
    b[len < 255 ? len : 255] = 0;
    printf("  / %-28s inode %u\n", b, ino);
    nnames++;
}
static uint32_t crc32b(const uint8_t *p, uint32_t n)
{
    uint32_t c = ~0u;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}
static int inspect(const char *path, const char *file)
{
    static struct ext4 fs;
    static struct jbd2_map map[8192];
    static uint8_t nv[4u << 20];
    struct jbd2 j;
    uint8_t sb[256], inode[EXT4_INODE_MAX];
    uint32_t ino;
    img = fopen(path, "rb");
    if (!img || raw(0, 1024, sb, sizeof(sb)))
        return printf("inspect: cannot read %s\n", path), 1;
#define L32(o) ((uint32_t)sb[o] | (uint32_t)sb[o + 1] << 8 | (uint32_t)sb[o + 2] << 16 | (uint32_t)sb[o + 3] << 24)
    printf("inspect: magic 0x%04x bs %u compat 0x%x incompat 0x%x ro_compat 0x%x state 0x%x journal inode %u%s\n",
           sb[0x38] | sb[0x39] << 8, 1024u << L32(0x18), L32(0x5C), L32(0x60), L32(0x64), sb[0x3A] | sb[0x3B] << 8,
           L32(0xE0), (L32(0x60) & 4) ? " NEEDS_RECOVERY" : "");
    jr = 0;
    fs.read = rd;
    if (ext4_open(&fs))
        return printf("inspect: ext4_open refused\n"), 1;
    bs = fs.bs;
    if (L32(0x60) & 4) {
        int e = jbd2_scan(&fs, L32(0xE0), &j, map, 8192);
        printf("inspect: journal %d %s: start %u seq %u tx %u blocks %u revoked %u features 0x%x%s\n", e,
               j.why ? j.why : "ok", j.start, j.seq0, j.transactions, j.n, j.skipped_revoked, j.features,
               j.dropped_incomplete ? " +incomplete tail" : "");
        if (e)
            return 1;
        jr = &j;
        if (ext4_open(&fs))
            return printf("inspect: ext4_open after the replay refused\n"), 1;
    }
    nnames = 0;
    int e = ext4_list(&fs, 2, name_cb, 0);
    printf("inspect: root: %u names (%d)\n", nnames, e);
    if ((e = ext4_lookup(&fs, file, &ino)) || (e = ext4_inode(&fs, ino, inode)))
        return printf("inspect: %s: lookup %d\n", file, e), 1;
    uint64_t size = ext4_size(inode);
    uint32_t flags = (uint32_t)inode[0x20] | (uint32_t)inode[0x21] << 8 | (uint32_t)inode[0x22] << 16 | (uint32_t)inode[0x23] << 24;
    printf("inspect: %s inode %u mode 0x%x flags 0x%x size %llu\n", file, ino, inode[0] | inode[1] << 8, flags,
           (unsigned long long)size);
    if (size <= sizeof(nv)) {
        for (uint32_t o = 0; o < size; o += 4096)
            if ((e = ext4_read(&fs, inode, o, nv + o, size - o < 4096 ? (uint32_t)(size - o) : 4096)))
                return printf("inspect: read %d at 0x%x\n", e, o), 1;
        printf("inspect: %s read ok, crc32 0x%08x (content not shown)\n", file, crc32b(nv, (uint32_t)size));
    }
    fclose(img);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "--inspect"))
        return inspect(argv[2], argc > 3 ? argv[3] : "/nv_data.bin");
    static uint8_t want[1048576], got[1048576];
    static struct jbd2_map map[8192];
    struct jbd2 j;
    unsigned fails = 0, checks = 0;
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : 0;
    if (!f || fread(want, 1, sizeof(want), f) != sizeof(want)) {
        printf("efs test: no fixture (sh starbite/modem/host/mk-efs-fixture.sh in the VM), skipped\n");
        return 0;
    }
    fclose(f);
    for (int a = 2; a < argc; ++a) {
        img = fopen(argv[a], "rb");
        if (!img)
            continue;
        const char *b = strrchr(argv[a], '/');
        b = b ? b + 1 : argv[a];
        int dirty = strstr(b, "dirty") != 0;
        memset(got, 0, sizeof(got));
        int e = read_nv(got, sizeof(got), 1, &j, map, 8192);
        int ok = e == 0 && !memcmp(got, want, sizeof(want));
        checks++;
        fails += !ok;
        printf("%s %s: with replay: %s (%d)\n", ok ? "ok  " : "FAIL", b, ok ? "the newest nv_data.bin" : "wrong", e);
        if (dirty) {
            checks++;
            fails += !(j.replayed && j.transactions > 0);
            memset(got, 0, sizeof(got));
            e = read_nv(got, sizeof(got), 0, &j, map, 8192);
            printf("     %s without replay: %s (%d)\n", b,
                   e ? "lookup / read fails" : memcmp(got, want, sizeof(want)) ? "STALE data (the replay is needed)" : "the same (already checkpointed)", e);
        }
        fclose(img);
    }
    printf("efs test: %u checks, %u failed\n", checks, fails);
    return fails ? 1 : 0;
}
