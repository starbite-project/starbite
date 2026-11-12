/* Host test of the SBFW formats (shim/starlte/sbfmt.c, docs/STARBITE-FIRMWARE-LAYER.md section 10):
 * the settings record's SBFW part (bytes 112..255), the request table, and the 4 KiB table the
 * loader hands to every OS. Writes DIR/sbfw-table.bin and DIR/sbfw-settings.bin for the Python /
 * libsb side to read back (test_sbfw.py).
 *   sbfw_test DIR */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "store.h"
#include "sbfmt.h"

static int fails, checks;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: sbfw_test DIR\n");
        return 2;
    }
    char path[512];
    static uint8_t blk[4096];
    struct star_settings s, s2;
    struct sbfw_rec r, r2;

    /* an older record (no "FW"): every SBFW field reads zero */
    sbset_defaults(&s);
    memcpy(s.default_key, "linux-pmos", 11);
    sbset_encode(blk, &s);
    sbfw_rec_decode(blk, &r);
    CHECK(r.fw_flags == 0 && r.req_action == 0 && r.req_state == 0 && !r.shell_key[0], "old record: SBFW part zero");
    CHECK(sbset_check(blk) == 1, "old record valid");

    /* the SBFW part round trip, the record's CRC fixed, the known fields untouched */
    memset(&r, 0, sizeof(r));
    r.fw_flags = SBFW_FLAG_SHELL;
    r.req_action = SBFW_REQ_SINGLE;
    r.req_state = SBFW_ST_ARMED;
    r.req_seq = 513;
    memcpy(r.req_key, "linux-pmos", 11);
    memcpy(r.shell_key, "starbite-next", 14);
    for (unsigned i = 0; i < 64; ++i)
        r.undo[i] = (uint8_t)(i * 7);
    sbfw_rec_encode(blk, &r);
    CHECK(sbset_check(blk) == -1, "the CRC is stale before sbset_crc_fix");
    sbset_crc_fix(blk);
    CHECK(sbset_check(blk) == 1, "valid after sbset_crc_fix");
    sbset_decode(blk, &s2);
    CHECK(!strcmp(s2.default_key, "linux-pmos"), "known fields kept");
    sbfw_rec_decode(blk, &r2);
    CHECK(!memcmp(&r, &r2, sizeof(r)), "SBFW part round trip");
    CHECK(blk[112] == 'F' && blk[113] == 'W' && blk[114] == 1 && blk[115] == 1 && blk[116] == 3 && blk[117] == 1 &&
          blk[118] == 1 && blk[119] == 2 && !memcmp(blk + 120, "linux-pmos", 10) && !memcmp(blk + 144, "starbite-next", 13) &&
          blk[168 + 63] == (uint8_t)(63 * 7) && blk[232] == 0 && blk[255] == 0,
          "the byte layout of section 10.1");
    snprintf(path, sizeof(path), "%s/sbfw-settings.bin", argv[1]);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(blk, 1, sizeof(blk), f);
        fclose(f);
    }
    /* sbset_encode (the loader's settings_save of the known fields) must leave 112.. to the caller:
     * it zeroes them, so store.c copies its tail back (store_test checks that path) */
    sbset_encode(blk, &s2);
    sbfw_rec_decode(blk, &r2);
    CHECK(r2.fw_flags == 0, "sbset_encode alone zeroes the SBFW part (store.c keeps the tail)");

    /* request names and fits */
    CHECK(!strcmp(sbfw_req_name(SBFW_REQ_PREV), "prev") && sbfw_req_name(99) == 0, "request names");
    CHECK(sbfw_req_fits(SBFW_REQ_SINGLE, 2) && !sbfw_req_fits(SBFW_REQ_SINGLE, 1) && sbfw_req_fits(SBFW_REQ_SAFE, 5) &&
          !sbfw_req_fits(SBFW_REQ_WINRE, 4) && sbfw_req_fits(SBFW_REQ_XNU_SINGLE, 1) && sbfw_req_fits(SBFW_REQ_VERBOSE, 4) &&
          !sbfw_req_fits(SBFW_REQ_NO_SHELL, 5) && !sbfw_req_fits(SBFW_REQ_PREV, 5),
          "which request fits which method");

    /* the table */
    static uint8_t page[SBFW_TABLE_SIZE];
    struct sbfw_table_in in;
    memset(&in, 0, sizeof(in));
    in.debug_level = 2;
    in.req_action = SBFW_REQ_SAFE;
    in.key = "starbite-os";
    in.console_y0 = 2368;
    in.console_y1 = 2960;
    in.fb_base = 0xcc000000ULL;
    in.stride = 5760;
    in.loader_version = 10;
    in.boot_reason = SBFW_WHY_SHELL;
    in.store_medium = 1;
    in.store_flags = 1;
    in.store_base = 22860800;
    in.store_capacity = 8389120;
    in.crash_kind = 3;
    in.crash_sev = 2;
    in.crash_key = "linux-pmos";
    in.post_fails = 1;
    in.post_warns = 4;
    in.settings_status = 1;
    in.flags = SBFW_TF_CARD_ONLY | SBFW_TF_SHELL;
    char lines[8192];
    unsigned n = 0;
    for (int i = 0; i < 40; ++i)
        n += (unsigned)snprintf(lines + n, sizeof(lines) - n, "STARBITE: line %d\x01 of the ring%s\n", i,
                                i == 39 ? " that is long enough to be cut at one hundred and nineteen characters of text, which this sentence certainly is, yes" : "");
    sbfw_table_build(page, &in, lines, n);
    CHECK(sbfw_table_check(page), "table valid");
    CHECK(!memcmp(page, "SBFWTBL1", 8) && le32(page + 8) == 1 && le32(page + 16) == 2 && le32(page + 20) == SBFW_REQ_SAFE &&
          !strcmp((char *)page + 24, "starbite-os") && le32(page + 48) == 2368 && le32(page + 52) == 2960 &&
          le32(page + 56) == 0xcc000000U && le32(page + 64) == 5760 && le32(page + 72) == 10 && le32(page + 76) == 3 &&
          le32(page + 80) == 1 && le32(page + 84) == 1 && le32(page + 88) == 22860800U && le32(page + 96) == 8389120U &&
          le32(page + 104) == 3 && le32(page + 108) == 2 && !strcmp((char *)page + 112, "linux-pmos") &&
          le32(page + 136) == 1 && le32(page + 140) == 4 && le32(page + 144) == 1 && le32(page + 148) == 3,
          "table fields at the offsets of section 10.2");
    CHECK(!strcmp((char *)page + 1024, "STARBITE: line 16. of the ring"), "oldest kept line (16 of 40), control byte as '.'");
    CHECK(!strncmp((char *)page + 1024 + 120 * 23, "STARBITE: line 39. of the ring that is long", 40) &&
          strlen((char *)page + 1024 + 120 * 23) == 119, "newest line last, cut to 119");
    page[200] ^= 1;
    CHECK(!sbfw_table_check(page), "a flipped byte fails the CRC");
    page[200] ^= 1;
    snprintf(path, sizeof(path), "%s/sbfw-table.bin", argv[1]);
    f = fopen(path, "wb");
    if (f) {
        fwrite(page, 1, sizeof(page), f);
        fclose(f);
    }
    /* empty and short rings */
    sbfw_table_build(page, &in, "", 0);
    CHECK(sbfw_table_check(page) && page[1024] == 0, "no lines");
    sbfw_table_build(page, &in, "one\ntwo\n\n", 9);
    CHECK(!strcmp((char *)page + 1024, "one") && !strcmp((char *)page + 1144, "two") && page[1264] == 0, "two lines");

    /* the request's life over loader starts: armed by the OS -> seen (this boot may start it) ->
     * expired if unused; applied -> done */
    CHECK(sbfw_age(SBFW_ST_ARMED) == SBFW_ST_SEEN && sbfw_age(SBFW_ST_SEEN) == SBFW_ST_REFUSED &&
          sbfw_age(SBFW_ST_APPLIED) == SBFW_ST_DONE && sbfw_age(SBFW_ST_DONE) == SBFW_ST_DONE &&
          sbfw_age(SBFW_ST_NONE) == SBFW_ST_NONE && sbfw_age(SBFW_ST_REFUSED) == SBFW_ST_REFUSED, "request ageing");

    /* shell mode's decision, in its order of precedence */
    const unsigned S = SBFW_FLAG_SHELL;
    CHECK(sbfw_shell_decide(0, 0, 0, 0, 0, 0, 3, 1) == SBFW_SH_OFF, "off");
    CHECK(sbfw_shell_decide(S, 0, 0, 0, 0, 0, 3, 1) == SBFW_SH_START, "start");
    CHECK(sbfw_shell_decide(S, 1, 0, 0, 0, 0, 3, 1) == SBFW_SH_SKIP, "Volume Down / menu key");
    CHECK(sbfw_shell_decide(S, 0, SBFW_ST_SEEN, SBFW_REQ_NO_SHELL, 0, 0, 3, 1) == SBFW_SH_NO_SHELL, "no-shell one-shot");
    CHECK(sbfw_shell_decide(S, 0, SBFW_ST_DONE, SBFW_REQ_NO_SHELL, 0, 0, 3, 1) == SBFW_SH_START, "a done no-shell is gone");
    CHECK(sbfw_shell_decide(S, 0, SBFW_ST_SEEN, SBFW_REQ_SAFE, 0, 0, 3, 1) == SBFW_SH_START, "other requests do not stop it");
    CHECK(sbfw_shell_decide(S, 0, 0, 0, 1, 0, 3, 1) == SBFW_SH_CRASH, "a crash to show");
    CHECK(sbfw_shell_decide(S, 0, 0, 0, 0, 1, 3, 1) == SBFW_SH_NEXT, "boot-next pending");
    CHECK(sbfw_shell_decide(S, 0, 0, 0, 0, 0, -1, 0) == SBFW_SH_MISSING, "no shell row");
    CHECK(sbfw_shell_decide(S, 0, 0, 0, 0, 0, 3, 0) == SBFW_SH_NOT_READY, "shell not ready");
    CHECK(sbfw_shell_decide(S, 1, SBFW_ST_SEEN, SBFW_REQ_NO_SHELL, 1, 1, -1, 0) == SBFW_SH_SKIP, "skip comes first");
    CHECK(!strcmp(sbfw_shell_why(SBFW_SH_CRASH), "the last start crashed: the menu shows it"), "reason text");

    /* the update slots (section 10.5): record fields, the start-of-boot ageing with rollback, the header */
    {
        struct sbfw_rec u, v;
        memset(&u, 0, sizeof(u));
        u.slot_active = 1;
        u.slot_try = 2;
        u.slot_tries = 3;
        u.req_action = SBFW_REQ_UPDATE;
        u.req_state = SBFW_ST_ARMED;
        memcpy(u.req_key, "starbite-next-gpu", 18);
        sbfw_rec_encode(blk, &u);
        sbfw_rec_decode(blk, &v);
        CHECK(!memcmp(&u, &v, sizeof(u)) && blk[232] == 1 && blk[233] == 2 && blk[234] == 3, "slot fields at 232..234");
        CHECK(sbfw_boot_age(&u) == SBFW_AGE_SEEN && u.req_state == SBFW_ST_SEEN && u.slot_try == 2,
              "update armed -> seen, slot_try kept for this start");
        struct sbfw_rec w = u;
        w.req_state = SBFW_ST_APPLIED;     /* the trial started, the OS never committed */
        CHECK(sbfw_boot_age(&w) == SBFW_AGE_ROLLBACK && w.slot_try == 0 && w.slot_active == 1 &&
              w.req_state == SBFW_ST_REFUSED, "uncommitted trial -> rollback to slot_active");
        CHECK(sbfw_boot_age(&u) == SBFW_AGE_UPD_EXPIRED && u.slot_try == 0 && u.req_state == SBFW_ST_REFUSED,
              "seen but never started -> expired, slot_try cleared");
        struct sbfw_rec c;
        memset(&c, 0, sizeof(c));
        c.req_action = SBFW_REQ_UPDATE;
        c.req_state = SBFW_ST_DONE;        /* the OS committed: slot_active = 2, slot_try = 0 */
        c.slot_active = 2;
        CHECK(sbfw_boot_age(&c) == 0 && c.slot_active == 2 && c.req_state == SBFW_ST_DONE, "a committed update stays");
        c.req_action = SBFW_REQ_SINGLE;
        c.req_state = SBFW_ST_APPLIED;
        CHECK(sbfw_boot_age(&c) == SBFW_AGE_DONE && c.req_state == SBFW_ST_DONE, "other requests age as before");
        CHECK(!strcmp(sbfw_req_name(SBFW_REQ_UPDATE), "update") && sbfw_req_fits(SBFW_REQ_UPDATE, 5) &&
              !sbfw_req_fits(SBFW_REQ_UPDATE, 2), "update: name, bare only");
        static uint8_t h[4096];
        struct sbslot_info si, so;
        memset(&si, 0, sizeof(si));
        si.size = 22552576;
        si.crc = 0xdeadbeef;
        memcpy(si.vstr, "1.0 P3x", 8);
        memcpy(si.key, "starbite-next-gpu", 18);
        sbslot_build(h, &si);
        CHECK(sbslot_check(h, 64U << 20, "starbite-next-gpu", &so) && so.size == si.size && so.crc == si.crc &&
              !strcmp(so.vstr, "1.0 P3x"), "slot header round trip");
        CHECK(!sbslot_check(h, 64U << 20, "starbite-os", &so), "another row's key refused");
        CHECK(!sbslot_check(h, 20U << 20, "starbite-next-gpu", &so), "an image bigger than the slot refused");
        h[100] ^= 1;
        CHECK(!sbslot_check(h, 64U << 20, "starbite-next-gpu", &so), "a damaged header refused");
        h[100] ^= 1;
        snprintf(path, sizeof(path), "%s/sbfw-slot-header.bin", argv[1]);
        f = fopen(path, "wb");
        if (f) {
            fwrite(h, 1, sizeof(h), f);
            fclose(f);
        }
    }

    /* a store head as Starbite Recovery's repair checks it (argv[2]: a compact home card store image) */
    if (argc > 2) {
        static uint8_t head[7 * 4096];
        FILE *h = fopen(argv[2], "rb");
        size_t got = h ? fread(head, 1, sizeof(head), h) : 0;
        if (h)
            fclose(h);
        struct sbhead_info hi;
        CHECK(got == sizeof(head) && sbstore_head_check(head, 4096, &hi) && hi.home && hi.data_block == 2048 &&
              hi.count >= 2 && hi.capacity == 4096, "card head valid: home, data at 2048, %u entries", hi.count);
        CHECK(!sbstore_head_check(head, 4095, &hi), "wrong capacity refused");
        head[3 * 4096 + 5] ^= 0x40;
        CHECK(!sbstore_head_check(head, 4096, &hi), "a damaged directory refused");
        head[3 * 4096 + 5] ^= 0x40;
        head[4096 + 80] ^= 1;
        CHECK(!sbstore_head_check(head, 4096, &hi), "a damaged home extension refused");
        head[4096 + 80] ^= 1;
        head[4096 + 20] ^= 1;
        CHECK(!sbstore_head_check(head, 0, &hi), "a damaged header refused");
    }

    printf("sbfw_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
