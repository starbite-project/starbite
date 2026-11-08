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

int main(int argc, char **argv)
{
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
