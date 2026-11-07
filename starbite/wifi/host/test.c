/* SPDX-License-Identifier: GPL-2.0-only
 * bcmw's host test (starbite/wifi/host/run.sh):
 *  1. the pure parts against the phone's own files, when a directory of them is given (the
 *     firmware, NVRAMs and CLM blob read from VENDOR, kept out of git): NVRAM processing (and,
 *     with BCMW_GOLDEN, byte for byte against Linux's brcmfmac parser), the firmware banner, the
 *     CLM blob; without files, synthetic ones;
 *  2. the module table (OTP VID -> NVRAM), events, iovars, clmload chunks;
 *  3. the ring: indices, wrap, full, and DHD's H2D stamping over three passes;
 *  4. the allow-list;
 *  5. the whole bring-up, steps 1..10, on host/model.c: the power sequence's order and delays,
 *     the PMU before CMU_FSYS1, the SysMMU never touched, the chip found, the module's NVRAM
 *     chosen, the firmware and NVRAM where the firmware looks, the shared area, rings, the first
 *     ioctls, the CLM in chunks, events, a scan, a received frame, no BADOPTION; once with the
 *     indices in the chip's RAM, once by DMA; then the way back.
 * Usage: bcmw-test [dir-with-vendor-files]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "../bcmw_priv.h"
#include "model.h"
#include "../wpa.h"

static int fails, checks;

#define CHECK(c, ...)                                                    \
    do {                                                                 \
        checks++;                                                        \
        if (!(c)) {                                                      \
            fails++;                                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                  \
            printf(__VA_ARGS__);                                         \
            printf("\n");                                                \
        }                                                                \
    } while (0)

static const char *fwdir;
static int verbose;

static uint8_t *slurp(const char *dir, const char *name, uint32_t *len)
{
    char p[1024];
    FILE *f;
    long n;
    uint8_t *b;
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    f = fopen(p, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n)
        n = 0;
    fclose(f);
    *len = (uint32_t)n;
    return b;
}

/* ---- 1. files ------------------------------------------------------------------------------------- */

#ifdef BCMW_GOLDEN
int golden_nvram(const uint8_t *data, uint32_t len, uint8_t **out, uint32_t *outlen);
#endif

static void check_nvram(const char *name, const uint8_t *t, uint32_t len)
{
    static uint8_t out[BCMW_NVRAM_MAX + 64];
    static const uint8_t mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    int n = fw_nvram(t, len, out, sizeof(out), 0), m;
    uint32_t tok, words;

    CHECK(n > 0 && n % 4 == 0, "%s: processed length %d", name, n);
    if (n <= 0)
        return;
    tok = le32(out + n - 4);
    words = (uint32_t)(n - 4) / 4;
    CHECK((tok & 0xffff) == words && (tok >> 16) == (~words & 0xffff), "%s: length token %08x for %u words", name, tok, words);
    CHECK(fw_nvram_get(out, (uint32_t)n, "boardrev") != 0, "%s: no boardrev", name);
    CHECK(!memchr(out, '#', (size_t)n - 4), "%s: a comment survived", name);
    m = fw_nvram(t, len, out, sizeof(out), mac);
    {
        const char *ma = fw_nvram_get(out, (uint32_t)m, "macaddr");
        CHECK(ma && !strcmp(ma, "02:11:22:33:44:55"), "%s: macaddr not replaced (%s)", name, ma ? ma : "none");
    }
#ifdef BCMW_GOLDEN
    {
        uint8_t *g = 0;
        uint32_t gl = 0;
        n = fw_nvram(t, len, out, sizeof(out), 0);
        CHECK(golden_nvram(t, len, &g, &gl) == 0, "%s: Linux's parser failed", name);
        CHECK(gl == (uint32_t)n && !memcmp(g, out, gl), "%s: differs from Linux's brcmfmac (%u vs %d bytes)", name, gl, n);
        free(g);
    }
#endif
}

static void test_files(void)
{
    static const char syn[] = "# a comment\nNVRAMRev=$Rev$\nsromrev=11\nboardrev=0x1101\r\n"
                              "macaddr=00:90:4c:12:d0:01\nRAW1=ignored\n bad key=x\nccode=0 #trailing\n"
                              "last=1";
    uint8_t out[512];
    int n = fw_nvram((const uint8_t *)syn, sizeof(syn) - 1, out, sizeof(out), 0);
    CHECK(n > 0, "synthetic NVRAM");
    CHECK(fw_nvram_get(out, (uint32_t)n, "sromrev") && !strcmp(fw_nvram_get(out, (uint32_t)n, "sromrev"), "11"), "sromrev");
    CHECK(!fw_nvram_get(out, (uint32_t)n, "RAW1"), "RAW1 kept");
    CHECK(fw_nvram_get(out, (uint32_t)n, "ccode") && !strcmp(fw_nvram_get(out, (uint32_t)n, "ccode"), "0 "), "ccode value");
    CHECK(fw_nvram_get(out, (uint32_t)n, "last") != 0, "a last entry without a newline is kept");
    CHECK(!fw_nvram_get(out, (uint32_t)n, "bad key"), "an invalid key kept");
    n = fw_nvram((const uint8_t *)"a=1\n", 4, out, sizeof(out), 0);
    CHECK(fw_nvram_get(out, (uint32_t)n, "boardrev") && !strcmp(fw_nvram_get(out, (uint32_t)n, "boardrev"), "0xff"),
          "the default boardrev");

    if (!fwdir) {
        printf("files: no directory of the phone's files given: synthetic only\n");
        return;
    }
    {
        DIR *d = opendir(fwdir);
        struct dirent *e;
        unsigned nv = 0;
        while (d && (e = readdir(d))) {
            uint32_t len;
            uint8_t *t;
            if (strncmp(e->d_name, "nvram.txt", 9))
                continue;
            t = slurp(fwdir, e->d_name, &len);
            if (t) {
                uint8_t sum[32];
                fw_sha256(t, len, sum);
                CHECK(fw_known(sum) != 0, "%s: its SHA-256 is not in the known list", e->d_name);
                check_nvram(e->d_name, t, len);
                nv++;
                free(t);
            }
        }
        if (d)
            closedir(d);
        printf("files: %u NVRAM files checked%s\n", nv,
#ifdef BCMW_GOLDEN
               " (and against Linux's brcmfmac parser)"
#else
               ""
#endif
        );
        CHECK(nv > 0, "no nvram.txt_* in %s", fwdir);
    }
    {
        const char *names[] = { "bcmdhd_sta.bin_b2", "bcmdhd_sta.bin_b0" };
        for (int i = 0; i < 2; i++) {
            uint32_t len;
            uint8_t *f = slurp(fwdir, names[i], &len);
            struct fw_image_info fi;
            if (!f)
                continue;
            CHECK(fw_image_check(f, len, &fi) == BCMW_OK, "%s: not a firmware", names[i]);
            CHECK(strstr(fi.version, "Version: 13.") != 0, "%s: banner '%s'", names[i], fi.version);
            CHECK(strstr(fi.version, i == 0 ? "4361b2" : "4361b0") != 0, "%s: chip in banner '%s'", names[i], fi.version);
            CHECK(fi.rstvec != 0 && fi.ramsize_override == 0, "%s: reset vector %08x", names[i], fi.rstvec);
            {
                uint8_t sum[32];
                fw_sha256(f, len, sum);
                CHECK(fw_known(sum) != 0, "%s: not a known file", names[i]);
            }
            printf("files: %s: %u bytes, %s\n", names[i], len, fi.version);
            free(f);
        }
    }
    {
        uint32_t len;
        uint8_t *c = slurp(fwdir, "bcmdhd_clm.blob", &len), sum[32];
        CHECK(c && fw_clm_check(c, len), "bcmdhd_clm.blob missing or not a CLM blob");
        if (c) {
            fw_sha256(c, len, sum);
            CHECK(fw_known(sum) != 0, "bcmdhd_clm.blob: its SHA-256 is not in the known list");
        }
        free(c);
    }
}

/* ---- 2. tables and formats ------------------------------------------------------------------------ */

static const char *module_nvram(uint8_t rev, uint8_t maker, const char *tag)
{
    static struct bcm4361_module m;
    uint8_t cis[] = { 0x80, 0x02, 0x00, 0x00, 0x80, 0x03, 0x81, rev, maker };
    CHECK(fw_cis_parse(cis, sizeof(cis), &m) == 1, "CIS parse");
    fw_module_files(&m, tag);
    return m.nvram;
}

static void test_tables(void)
{
    struct bcm4361_module m;
    uint8_t cis[88];
    uint8_t buf[64], frame[200];
    struct bcmw_event ev;

    CHECK(!strcmp(module_nvram(0x91, 0x33, "b2"), "nvram.txt_r02a_b2"), "91 33");
    CHECK(!strcmp(module_nvram(0xa1, 0x33, "b2"), "nvram.txt_r02b_b2"), "a1 33");
    CHECK(!strcmp(module_nvram(0x61, 0x22, "b2"), "nvram.txt_murata_r031_b2"), "61 22");
    CHECK(!strcmp(module_nvram(0x70, 0x22, "b2"), "nvram.txt_murata_r041_b2"), "an unknown Murata B2");
    CHECK(!strcmp(module_nvram(0x77, 0x33, "b2"), "nvram.txt_r02b_b2"), "an unknown SEMCO B2");
    CHECK(!strcmp(module_nvram(0x82, 0x33, "b0"), "nvram.txt_r02j_b0"), "82 33 b0");
    CHECK(!strcmp(module_nvram(0x31, 0x22, "b0"), "nvram.txt_murata_r030_b0"), "r31 -> r30");
    CHECK(!strcmp(module_nvram(0x91, 0x44, "b2"), ""), "an unknown maker");
    memset(cis, 0xff, sizeof(cis));
    CHECK(fw_cis_parse(cis, sizeof(cis), &m) == 0, "an empty OTP");
    CHECK(!strcmp(fw_rev_tag(5), "b2") && !strcmp(fw_rev_tag(4), "b0") && !fw_rev_tag(2), "revision tags");

    {
        uint8_t sum[32];
        static const uint8_t abc[8] = { 0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea };
        static const uint8_t empty[8] = { 0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14 };
        static const uint8_t two[8] = { 0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8 };
        static const char m56[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        fw_sha256((const uint8_t *)"abc", 3, sum);
        CHECK(!memcmp(sum, abc, 8), "sha256 abc");
        fw_sha256((const uint8_t *)"", 0, sum);
        CHECK(!memcmp(sum, empty, 8), "sha256 empty");
        fw_sha256((const uint8_t *)m56, 56, sum);
        CHECK(!memcmp(sum, two, 8), "sha256 56 bytes (two blocks)");
        CHECK(!fw_known(sum), "a random sum is not known");
    }
    {
        /* PBKDF2 (IEEE 802.11-2020 J.4.2 vectors), the PRF against Python's hmac for fixed inputs,
         * the model's AES key wrap against RFC 3394 4.1 */
        static const uint8_t v1[8] = { 0xf4, 0x2c, 0x6f, 0xc5, 0x2d, 0xf0, 0xeb, 0xef };
        static const uint8_t v2[8] = { 0x0d, 0xc0, 0xd6, 0xeb, 0x90, 0x55, 0x5e, 0xd6 };
        static const uint8_t pmk_test[8] = { 0xc5, 0xb6, 0x41, 0x2d, 0x5c, 0x37, 0xa9, 0x30 };
        static const uint8_t ptk_test[8] = { 0xdc, 0x2f, 0x55, 0x4b, 0x4b, 0xb4, 0x6c, 0x21 };
        uint8_t pmk[32], ptk[48], data[76], aa[6] = { 2, 0x11, 0x22, 0x33, 0x44, 0x5a }, sa[6] = { 2, 0x11, 0x22, 0x33, 0x44, 0x55 };
        bcmw_wpa_pmk((const unsigned char *)"password", 8, (const unsigned char *)"IEEE", 4, pmk);
        CHECK(!memcmp(pmk, v1, 8), "PBKDF2 vector 1");
        bcmw_wpa_pmk((const unsigned char *)"ThisIsAPassword", 15, (const unsigned char *)"ThisIsASSID", 11, pmk);
        CHECK(!memcmp(pmk, v2, 8), "PBKDF2 vector 2");
        bcmw_wpa_pmk((const unsigned char *)"correct horse", 13, (const unsigned char *)"test-ap", 7, pmk);
        CHECK(!memcmp(pmk, pmk_test, 8), "the model network's PMK");
        memcpy(data, sa, 6), memcpy(data + 6, aa, 6);
        for (int i = 0; i < 64; i++)
            data[12 + i] = (uint8_t)i;
        bcmw_prf(pmk, 32, "Pairwise key expansion", data, 76, ptk, 48);
        CHECK(!memcmp(ptk, ptk_test, 8), "the PRF against Python's hmac");
        CHECK(model_ap_selftest(), "the model's AES key wrap against RFC 3394");
    }
    {
        uint8_t ours[BCMW_RSN_OURS];
        unsigned g = 0;
        bcmw_rsn_ours(ours, BCMW_CIPHER_CCMP);
        CHECK(bcmw_rsn_check(ours, sizeof(ours), &g) == 0 && g == BCMW_CIPHER_CCMP, "our RSN element is one we join");
        ours[19] = 8;                       /* SAE only (WPA3) */
        CHECK(bcmw_rsn_check(ours, sizeof(ours), &g) != 0, "a WPA3-only network is refused");
    }
    CHECK(fw_iovar(buf, sizeof(buf), "ver", 0, 4) == 8 && !memcmp(buf, "ver\0\0\0\0\0", 8), "iovar get");
    CHECK(fw_iovar(buf, 6, "toolong", 0, 0) == 0, "iovar overflow");
    {
        static uint8_t data[3000], out[1500];
        for (unsigned i = 0; i < sizeof(data); i++)
            data[i] = (uint8_t)i;
        uint32_t n = fw_clm_chunk(out, data, 0, 3000, 1400);
        CHECK(n == 1412 && le16(out) == 0x1002 && le16(out + 2) == 2 && le32(out + 4) == 1400 && le32(out + 8) == 0,
              "first clm chunk %u %04x", n, le16(out));
        n = fw_clm_chunk(out, data, 1400, 3000, 1400);
        CHECK(n == 1412 && le16(out) == 0x1000 && out[12] == (uint8_t)1400, "middle clm chunk");
        n = fw_clm_chunk(out, data, 2800, 3000, 1400);
        CHECK(n == 212 && le16(out) == 0x1004, "last clm chunk");
        n = fw_clm_chunk(out, data, 0, 100, 1400);
        CHECK(le16(out) == 0x1006, "single clm chunk");
    }
    memset(frame, 0, sizeof(frame));
    frame[12] = 0x88, frame[13] = 0x6c, frame[14] = 0x80, frame[15] = 0x01, frame[20] = 0x10, frame[21] = 0x18, frame[23] = 1;
    frame[24 + 7] = 69;
    frame[24 + 11] = 8;
    frame[24 + 23] = 10;
    memcpy(frame + 24 + 30, "wlan0", 5);
    CHECK(fw_event_parse(frame, 72 + 10, &ev) == 0 && ev.type == 69 && ev.status == 8 && ev.datalen == 10 &&
          !strcmp(ev.ifname, "wlan0"), "event parse");
    frame[21] = 0x19;
    CHECK(fw_event_parse(frame, 82, &ev) != 0, "a frame with another OUI is not an event");
}

/* ---- 3. the ring ---------------------------------------------------------------------------------- */

static uint16_t fake_idx[4];
static unsigned bells;
static uint16_t f_rd(struct bcmw_ring *r, uint32_t where) { (void)r; return fake_idx[where]; }
static void f_wr(struct bcmw_ring *r, uint32_t where, uint16_t v) { (void)r; fake_idx[where] = v; }
static void f_bell(struct bcmw_ring *r) { (void)r; bells++; }

static void test_ring(void)
{
    static const struct bcmw_ring_ops ops = { f_rd, f_wr, f_bell, 0 };
    static uint8_t buf[8 * 16];
    struct bcmw_ring r;
    uint16_t got;
    uint32_t seq = 254;
    uint8_t phase = 0;
    unsigned total = 0;

    memset(&r, 0, sizeof(r));
    r.ops = &ops;
    r.w_where = 0, r.r_where = 1;
    ring_config(&r, 8, 16, buf, 0x1000, 1);
    CHECK(ring_space(&r) == 7, "an empty ring of 8 has 7 free (%u)", ring_space(&r));
    /* three passes of 8 items, a few at a time, the "chip" reading behind */
    for (int round = 0; round < 12; round++) {
        uint8_t *p = ring_reserve(&r, 2, &got);
        CHECK(p && got == 2, "reserve 2 at round %d (got %u)", round, got);
        for (unsigned i = 0; i < got; i++)
            p[16 * i] = (uint8_t)(total + i);
        ring_commit(&r);
        for (unsigned i = 0; i < got; i++) {
            unsigned slot = (total + i) % 8;
            if (slot == 0)
                phase ^= 0x80;
            CHECK(buf[16 * slot + 2] == phase && buf[16 * slot + 3] == (uint8_t)(seq % 253),
                  "item %u: phase %02x epoch %u, want %02x %u", total + i, buf[16 * slot + 2], buf[16 * slot + 3],
                  phase, (unsigned)(seq % 253));
            seq++;
        }
        total += got;
        fake_idx[1] = fake_idx[0];      /* the chip reads everything */
    }
    CHECK(bells == 12, "a doorbell per commit (%u)", bells);
    /* full: the chip reads nothing */
    {
        unsigned n = 0;
        while (ring_reserve(&r, 1, &got))
            n++;
        CHECK(n == 7, "7 fit before full (%u)", n);
        ring_cancel(&r, 3);
        CHECK(ring_reserve(&r, 5, &got) && got <= 3, "after cancelling 3, at most 3 (%u)", got);
    }
    /* a completion ring: the chip writes */
    memset(&r, 0, sizeof(r));
    memset(fake_idx, 0, sizeof(fake_idx));
    r.ops = &ops;
    r.w_where = 2, r.r_where = 3;
    ring_config(&r, 8, 16, buf, 0x1000, 0);
    fake_idx[2] = 5;
    CHECK(ring_read(&r, &got) == buf && got == 5, "5 to read");
    ring_consumed(&r, 5);
    CHECK(fake_idx[3] == 5, "read index written back");
    fake_idx[2] = 2;                    /* wrapped */
    CHECK(ring_read(&r, &got) == buf + 5 * 16 && got == 3, "to the end first (%u)", got);
    ring_consumed(&r, 3);
    CHECK(ring_read(&r, &got) == buf && got == 2, "then from the start (%u)", got);
    fake_idx[2] = 9;                    /* nonsense */
    ring_consumed(&r, 2);
    CHECK(ring_read(&r, &got) == 0 && got == 0, "an index past the end is ignored");
}

/* ---- 4. the allow-list -------------------------------------------------------------------------- */

static void test_allow(void)
{
    static const uint64_t no[] = {
        0x11420000, 0x11420000 + 0x000,     /* the PCIe SysMMU */
        0x1A240000, 0x1A242078,             /* CMU_TOP, other gates */
        0x1A440000, 0x1A0C0000,             /* other CMUs (MIF/INT live elsewhere: nothing outside the list) */
        0x14060000, 0x14060718, 0x14060720, /* the PMU, its other words */
        0x10430084 + 4, 0x10430000,         /* other GPIO */
        0x11430030, 0x14050020,             /* gpf3, ALIVE (read only) */
        0x116A1000, 0x11701000, 0x127FF000, /* just past the blocks */
        0x12700000 + 0xF0000,               /* past the memory window */
        0x80000000, 0x0,                    /* DRAM, nothing */
        0x116A0002,                         /* unaligned */
    };
    static const uint64_t yes[] = { 0x116A002C, 0x116C00EC, 0x116D00BC, 0x1170080C, 0x127FE080, 0x11800000,
                                    0x1406071C, 0x11411044, 0x11411050, 0x11410700, 0x11402098, 0x10430084,
                                    0x11430024, 0x11430028 };
    for (unsigned i = 0; i < sizeof(no) / sizeof(no[0]); i++)
        CHECK(!bcmw_write_allowed(no[i]), "allowed: %llx", (unsigned long long)no[i]);
    for (unsigned i = 0; i < sizeof(yes) / sizeof(yes[0]); i++)
        CHECK(bcmw_write_allowed(yes[i]), "refused: %llx", (unsigned long long)yes[i]);
}

/* ---- 5. the whole bring-up on the model --------------------------------------------------------- */

struct files {
    uint8_t *fw, *nv, *clm;
    uint32_t fwlen, nvlen, clmlen;
};
static struct files F;
static int synthetic;
static unsigned n_escan, n_escan_done, n_rx, n_lines;
static uint8_t log_ver[160];
static char log_all[1 << 16];

static int os_file(void *ctx, const char *name, const uint8_t **data, uint32_t *len)
{
    (void)ctx;
    if (!strcmp(name, "bcmdhd_sta.bin_b2") && F.fw) {
        *data = F.fw, *len = F.fwlen;
        return 0;
    }
    if (!strcmp(name, "nvram.txt_r02a_b2") && F.nv) {
        *data = F.nv, *len = F.nvlen;
        return 0;
    }
    if (!strcmp(name, "bcmdhd_clm.blob") && F.clm) {
        *data = F.clm, *len = F.clmlen;
        return 0;
    }
    return -1;
}

static void os_log(void *ctx, const char *line)
{
    (void)ctx;
    n_lines++;
    if (verbose)
        printf("  | %s\n", line);
    if (strlen(log_all) + strlen(line) + 2 < sizeof(log_all)) {
        strcat(log_all, line);
        strcat(log_all, "\n");
    }
}

static void os_event(void *ctx, const struct bcmw_event *ev)
{
    (void)ctx;
    if (ev->type == BCMW_E_ESCAN_RESULT) {
        if (ev->status == 8) {
            n_escan++;
            CHECK(ev->datalen >= 12 + 128 && le16(ev->data + 10) == 1 && !memcmp(ev->data + 12 + 19, "test-ap", 7),
                  "the scan result's BSS");
        } else if (ev->status == 0) {
            n_escan_done++;
        }
    }
}

static void os_rx(void *ctx, unsigned ifidx, const uint8_t *frame, uint32_t len)
{
    (void)ctx;
    (void)ifidx;
    n_rx++;
    CHECK(len == 60 && frame[12] == 0x08 && frame[13] == 0x06, "the received frame");
}

static void synth_files(void)
{
    static uint8_t fw[64 * 1024];
    static const char banner[] = "4361b2-roml/config_pcie_release_ipa Version: 13.50.99 (model) FWID: 01-0";
    static const char nv[] = "boardrev=0x1101\nmacaddr=00:90:4c:12:d0:01\nccode=0\n";
    static uint8_t clm[3000];
    for (unsigned i = 0; i < sizeof(fw); i++)
        fw[i] = (uint8_t)(i * 7);
    fw[0x6c] = 0;
    memcpy(fw + sizeof(fw) - 200, banner, sizeof(banner));
    memcpy(clm, "BLOB", 4);
    F.fw = fw, F.fwlen = sizeof(fw);
    F.nv = (uint8_t *)nv, F.nvlen = sizeof(nv) - 1;
    F.clm = clm, F.clmlen = sizeof(clm);
    synthetic = 1;
}

static void run_bringup(int dma_idx)
{
    static uint8_t mem[1 << 18] __attribute__((aligned(16)));
    struct bcmw *w = (struct bcmw *)mem;
    struct bcmw_os os = { 0 };
    struct bcmw_cfg cfg = { 0 };
    const struct bcmw_info *in;
    int r;

    CHECK(bcmw_size() <= sizeof(mem), "struct bcmw is %u bytes", bcmw_size());
    model_reset();
    M.offer_dma_idx = 1;
    M.scan_v2 = dma_idx;            /* one run each way: the v1 and the v2 escan parameters */
    M.fw_rstvec = le32(F.fw);
    M.fw_image = F.fw;
    M.fw_image_len = F.fwlen;
    n_escan = n_escan_done = n_rx = n_lines = 0;
    log_all[0] = 0;
    os.rd32 = model_rd32, os.wr32 = model_wr32, os.rd16 = model_rd16, os.wr16 = model_wr16;
    os.udelay = model_udelay, os.now_us = model_now, os.dma_alloc = model_dma_alloc;
    os.file = os_file, os.log = os_log, os.event = os_event, os.rx = os_rx;
    cfg.stop_after = BCMW_STEP_ALL;
    cfg.dma_indices = dma_idx;
    cfg.any_files = synthetic;
    cfg.fw_supplicant = dma_idx;    /* the DMA-indices run tries the firmware's supplicant */
    cfg.diag = dma_idx;             /* and the step 8 diagnostics (W1b) */
    memcpy(cfg.mac, "\x02\x11\x22\x33\x44\x55", 6);
    CHECK(bcmw_init(w, &os, &cfg) == BCMW_OK, "init");
    r = bcmw_bringup(w, 0);
    in = bcmw_info(w);
    printf("bring-up (%s indices): reached step %d; %u log lines, %llu reads, %llu writes, %u ioctls, %u KiB DMA, %llu ms\n",
           dma_idx ? "DMA" : "TCM", r, n_lines, (unsigned long long)M.reads, (unsigned long long)M.writes, M.ioctls,
           M.dma_bytes >> 10, (unsigned long long)(M.now / 1000));
    CHECK(r == BCMW_STEP_ALL, "the bring-up stopped at %d (step %u)", r, bcmw_step(w));
    CHECK(M.errors == 0, "the model saw %u errors", M.errors);
    CHECK(M.fw_errors == 0, "the model's firmware found faults %x", M.fw_errors);
    CHECK(M.badoption == 0, "BADOPTION %u times", M.badoption);
    /* the power sequence */
    CHECK(M.reg_on_seen_low && M.t_reg_on >= M.reg_on_seen_low - 1 + 50000, "WLAN_REG_ON low 50 ms before high");
    CHECK(M.t_perst >= M.t_reg_on + 200000, "PERST# high %llu us after WLAN_REG_ON (>= 200 ms)",
          (unsigned long long)(M.t_perst - M.t_reg_on));
    CHECK(M.t_pmu && M.t_cmu && M.t_pmu <= M.t_cmu, "the PMU's PHY isolation off before CMU_FSYS1 is touched");
    CHECK(M.t_ltssm > M.t_perst, "the LTSSM after PERST#");
    /* the chip */
    CHECK(in->vendor == 0x14e4 && in->device == 0x441f, "PCI ID %04x:%04x", in->vendor, in->device);
    CHECK(in->chip == 0x4361 && in->chiprev == 5, "chip %x rev %u", in->chip, in->chiprev);
    CHECK(in->ramsize == 0xE0000 && in->rambase == 0x170000, "RAM %x at %x", in->ramsize, in->rambase);
    CHECK(in->module_vid[0] == 0x91 && in->module_vid[1] == 0x33, "module VID");
    CHECK(!strcmp(in->nvram_name, "nvram.txt_r02a_b2"), "NVRAM name %s", in->nvram_name);
    CHECK(M.watchdog_resets == 1, "watchdog resets %u", M.watchdog_resets);
    CHECK(M.boots == 1 && M.nvram_seen > 0, "firmware boots %u, NVRAM %u", M.boots, M.nvram_seen);
    CHECK(in->shared_version == 7, "shared version %u", in->shared_version);
    CHECK(M.hostready == 1, "host ready %u", M.hostready);
    CHECK(strstr(in->fw_version, "13.50.37") != 0, "ver '%s'", in->fw_version);
    CHECK(!memcmp(M.mac_set, "\x02\x11\x22\x33\x44\x55", 6), "cur_etheraddr set");
    CHECK(M.clm_done == F.clmlen, "CLM %u of %u bytes", M.clm_done, F.clmlen);
    CHECK(M.up == 1, "up");
    CHECK(M.event_mask[69 / 8] & (1u << (69 % 8)), "the escan event enabled");
    CHECK(strstr(log_all, "fw: fake fw: hello from the model") != 0, "the firmware's console read");
    CHECK(!strstr(log_all, "02:11:22") && !strstr(log_all, "33:44:55"), "the phone's address was logged");
    if (dma_idx) {
        CHECK(strstr(log_all, "diag: 8: the firmware read back: ") && strstr(log_all, "diag: 8: the NVRAM read back: "),
              "diag: the read-backs");
        CHECK(!strstr(log_all, "WRONG") && !strstr(log_all, " differs at RAM "), "diag: a download word reads back wrong");
        CHECK(strstr(log_all, "diag: 8: +") && strstr(log_all, "CR4 ioctl "), "diag: the ARM's state while waiting");
    } else {
        CHECK(!strstr(log_all, "diag: "), "diag lines without cfg.diag");
    }
    /* the scan's events and a frame arrive by polling */
    for (int i = 0; i < 20 && !n_escan_done; i++) {
        bcmw_poll(w);
        M.now += 5000;
    }
    CHECK(n_escan == 1 && n_escan_done == 1, "escan events %u/%u", n_escan, n_escan_done);
    CHECK(n_rx == 1, "received frames %u", n_rx);
    {
        /* joining the model's network: the host's handshake in one run, the firmware's in the other */
        int jr;
        uint8_t ip[60];
        M.fwsup = dma_idx;
        jr = bcmw_join(w, (const uint8_t *)"test-ap", 7, "wrong passphrase!", 2000);
        if (dma_idx)
            CHECK(jr != BCMW_OK && !M.pmk_ok, "the firmware's supplicant: a wrong passphrase gives a wrong PMK");
        else
            CHECK(jr != BCMW_OK && (M.ap_errors & 1) && !M.handshake_done, "the host's handshake: a wrong passphrase fails at message 2");
        if (!dma_idx) {
            const char *a = strstr(log_all, "message 1 from the access point"), *b = a ? strstr(a + 1, "message 1 from the access point") : 0;
            CHECK(a && (!b || strstr(b, "joining a network")), "message 1 answered once");
        }
        M.ap_errors = 0;
        jr = bcmw_join(w, (const uint8_t *)"test-ap", 7, "correct horse", 0);
        CHECK(jr == BCMW_OK && bcmw_link_up(w), "joining the model's network (%s): %d", dma_idx ? "the firmware's supplicant" : "the host's handshake", jr);
        CHECK(M.ap_errors == 0, "the access point found faults %x", M.ap_errors);
        if (dma_idx)
            CHECK(M.pmk_ok, "the PMK given to the firmware is the network's");
        else
            CHECK(M.handshake_done && M.keys_ok == 3, "the handshake finished (%d) and both keys are the AP's (%x)", M.handshake_done, M.keys_ok);
        CHECK(M.flow_created == (dma_idx ? 0u : 1u), "flow rings made: %u", M.flow_created);
        CHECK(M.disassoc == 1, "the failed join was left (DISASSOC %u)", M.disassoc);
        if (!dma_idx) {
            /* a group-key renewal after the join, answered from bcmw_poll; a stranger's copy ignored */
            model_ap_rekey();
            for (int i = 0; i < 10; i++) {
                bcmw_poll(w);
                M.now += 1000;
            }
            CHECK(M.rekey_answered == 1 && (M.keys_ok & 4), "the group-key renewal answered once (%u) and installed (%x)",
                  M.rekey_answered, M.keys_ok);
        }
        CHECK(!strstr(log_all, "correct horse") && !strstr(log_all, "test-ap"), "the passphrase or the name in the log");
        /* a data frame out */
        memset(ip, 0, sizeof(ip));
        memcpy(ip, "\x02\x11\x22\x33\x44\x5a", 6);
        memcpy(ip + 6, "\x02\x11\x22\x33\x44\x55", 6);
        ip[12] = 0x08;
        for (int i = 0; i < 40; i++)
            CHECK(bcmw_tx(w, ip, sizeof(ip)) == BCMW_OK, "tx %d", i);
        bcmw_poll(w);
        CHECK(M.tx_ip == 40 && bcmw_info(w)->tx_done == bcmw_info(w)->tx_frames, "40 frames out (%u), all done (%u/%u)", M.tx_ip,
              bcmw_info(w)->tx_done, bcmw_info(w)->tx_frames);
        CHECK(M.badoption == 0 && M.fw_errors == 0, "after the data: BADOPTION %u, faults %x", M.badoption, M.fw_errors);
    }
    /* buffers are reposted: 20 more ioctls without running out */
    for (int i = 0; i < 20; i++) {
        uint32_t v;
        CHECK(bcmw_iovar_get_u32(w, "clmload_status", &v) == BCMW_OK, "ioctl %d after the bring-up", i);
    }
    {
        int fwerr = 0;
        uint32_t v;
        CHECK(bcmw_iovar_get(w, "nonsense", &v, 4, &fwerr) == BCMW_E_FW && fwerr == -23, "an unsupported iovar (%d)", fwerr);
    }
    CHECK(M.fw_errors == 0 && M.errors == 0, "after the ioctls: model faults %x / %u", M.fw_errors, M.errors);
    {
        /* the model holds one answer back: the ioctl times out; the late answer then arrives during
         * the next one and must not be taken for it */
        uint32_t v = 0;
        M.hold_answer = 1;
        CHECK(bcmw_iovar_get_u32(w, "clmload_status", &v) == BCMW_E_TIMEOUT, "a held answer times out");
        M.release_answer = 1;
        CHECK(bcmw_iovar_get(w, "ver", log_ver, sizeof(log_ver) - 1, 0) == BCMW_OK && strstr((char *)log_ver, "13.50.37"),
              "the next ioctl gets its own answer, not the late one");
        CHECK(strstr(log_all, "late ioctl answer") != 0, "the late answer was dropped");
    }
    bcmw_quiesce(w);
    CHECK(!M.reg_on && !M.perst_high, "the way back: WLAN_REG_ON %d PERST# %d", M.reg_on, M.perst_high);
    CHECK(M.phy_powered_down, "the PHY powered down on the way back");
    bcmw_quiesce(w);
    {
        /* a second bring-up with the same state: no DMA memory leaks, it reaches step 10 again */
        uint32_t used = M.dma_bytes;
        int r2;
        M.fw_running = 0;
        r2 = bcmw_bringup(w, 0);
        CHECK(r2 == BCMW_STEP_ALL, "a second bring-up after quiesce: %d", r2);
        CHECK(M.dma_bytes == used, "the second bring-up allocated more DMA memory (%u -> %u)", used, M.dma_bytes);
        bcmw_quiesce(w);
    }
    if (verbose == 0 && fails)
        printf("%s", log_all);
}

static void run_stops(void)
{
    static uint8_t mem[1 << 18] __attribute__((aligned(16)));
    struct bcmw *w = (struct bcmw *)mem;
    struct bcmw_os os = { 0 };
    struct bcmw_cfg cfg = { 0 };

    /* each step alone, then the rest: a phone run can go one step at a time */
    model_reset();
    M.fw_rstvec = le32(F.fw);
    os.rd32 = model_rd32, os.wr32 = model_wr32, os.rd16 = model_rd16, os.wr16 = model_wr16;
    os.udelay = model_udelay, os.now_us = model_now, os.dma_alloc = model_dma_alloc;
    os.file = os_file, os.log = os_log;
    cfg.stop_after = BCMW_STEP_RINGS;
    cfg.any_files = synthetic;
    bcmw_init(w, &os, &cfg);
    for (unsigned s = 1; s <= BCMW_STEP_RINGS; s++)
        CHECK(bcmw_bringup(w, s) == (int)s, "step %u alone", s);
    CHECK(M.errors == 0 && M.fw_errors == 0, "stepwise: model faults %u %x", M.errors, M.fw_errors);
    bcmw_quiesce(w);
    CHECK(!M.reg_on && !M.perst_high, "stepwise: the way back");

    /* a firmware whose shared area points outside its RAM: the core stops, reads nothing wild */
    model_reset();
    M.fw_rstvec = le32(F.fw);
    M.bad_console = 1;
    cfg.stop_after = BCMW_STEP_RINGS;
    cfg.any_files = synthetic;
    bcmw_init(w, &os, &cfg);
    CHECK(bcmw_bringup(w, 0) == BCMW_E_PROTO, "a bad shared-area address is refused");
    CHECK(M.errors == 0, "a bad shared-area address reached the bus (%u)", M.errors);
    bcmw_quiesce(w);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "-v"))
        verbose = 1, argc--, argv++;
    fwdir = argc > 1 ? argv[1] : 0;
    test_files();
    test_tables();
    test_ring();
    test_allow();
    if (fwdir) {
        F.fw = slurp(fwdir, "bcmdhd_sta.bin_b2", &F.fwlen);
        F.nv = slurp(fwdir, "nvram.txt_r02a_b2", &F.nvlen);
        F.clm = slurp(fwdir, "bcmdhd_clm.blob", &F.clmlen);
    }
    if (!F.fw || !F.nv || !F.clm)
        synth_files();
    run_bringup(0);
    run_bringup(1);
    run_stops();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", checks, fails);
    return fails != 0;
}
