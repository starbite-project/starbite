// SPDX-License-Identifier: GPL-2.0-only
// Host check (Mac/Linux clang++) of src/sbmodes.h: decodes the EDID the driver hands to IddCx and checks it
// against the mode table. Writes the raw EDID to argv[1] if given (for edid-decode).
//   clang++ -std=c++17 -I../src modes-test.cpp -o modes-test && ./modes-test edid.bin
#include <cstdio>
#include <cstring>

#include "sbmodes.h"

using namespace sbidd;

static int fails;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            fails++;                                                \
        }                                                           \
    } while (0)

struct Dtd {
    unsigned clk10k, ha, hb, va, vb, hso, hsw, vso, vsw, wmm, hmm, flags;
};

static Dtd DecodeDtd(const uint8_t *p)
{
    Dtd d;
    d.clk10k = p[0] | p[1] << 8;
    d.ha = p[2] | (p[4] >> 4) << 8;
    d.hb = p[3] | (p[4] & 15) << 8;
    d.va = p[5] | (p[7] >> 4) << 8;
    d.vb = p[6] | (p[7] & 15) << 8;
    d.hso = p[8] | (p[11] >> 6) << 8;
    d.hsw = p[9] | ((p[11] >> 4) & 3) << 8;
    d.vso = (p[10] >> 4) | ((p[11] >> 2) & 3) << 4;
    d.vsw = (p[10] & 15) | (p[11] & 3) << 4;
    d.wmm = p[12] | (p[14] >> 4) << 8;
    d.hmm = p[13] | (p[14] & 15) << 8;
    d.flags = p[17];
    return d;
}

static void CheckDtdMatches(const uint8_t *p, const Mode &m)
{
    Dtd d = DecodeDtd(p);
    printf("  DTD %ux%u total %ux%u %u.%02u MHz -> %.3f Hz, sync H %u/%u V %u/%u, %ux%u mm, flags 0x%02x\n", d.ha,
           d.va, d.ha + d.hb, d.va + d.vb, d.clk10k / 100, d.clk10k % 100,
           d.clk10k * 10000.0 / ((d.ha + d.hb) * (d.va + d.vb)), d.hso, d.hsw, d.vso, d.vsw, d.wmm, d.hmm, d.flags);
    CHECK(d.ha == m.w && d.va == m.h);
    CHECK(d.ha + d.hb == m.tw && d.va + d.vb == m.th);
    CHECK(uint64_t(d.clk10k) * 10000 == PixelRate(m));
    CHECK(d.hso + d.hsw <= d.hb && d.vso + d.vsw <= d.vb);
}

int main(int argc, char **argv)
{
    const uint8_t *e = kEdid.b;
    unsigned sum = 0;
    for (int i = 0; i < 128; i++)
        sum += e[i];
    printf("EDID checksum byte 0x%02x, sum mod 256 = %u\n", e[127], sum & 255);
    CHECK((sum & 255) == 0);
    static const uint8_t hdr[8] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    CHECK(!memcmp(e, hdr, 8));
    unsigned id = e[8] << 8 | e[9];
    char mfg[4] = {char('@' + ((id >> 10) & 31)), char('@' + ((id >> 5) & 31)), char('@' + (id & 31)), 0};
    printf("manufacturer %s product 0x%04x serial %u week %u year %u, EDID %u.%u, %ux%u cm\n", mfg, e[10] | e[11] << 8,
           e[12] | e[13] << 8 | e[14] << 16 | unsigned(e[15]) << 24, e[16], 1990 + e[17], e[18], e[19], e[21], e[22]);
    CHECK(!strcmp(mfg, "SBT"));
    CHECK(e[18] == 1 && e[19] == 4);

    // standard timings must be exactly the 60 Hz modes other than the DTDs, 16:9
    for (int i = 0; i < 8; i++) {
        const uint8_t *s = e + 38 + 2 * i;
        if (s[0] == 1 && s[1] == 1)
            continue;
        unsigned w = (s[0] + 31) * 8, hz = (s[1] & 63) + 60, h = w * 9 / 16;
        printf("  standard timing %ux%u@%u (aspect bits %u)\n", w, h, hz, s[1] >> 6);
        CHECK(s[1] >> 6 == 3);
        bool found = false;
        for (const Mode &m : kModes)
            found |= m.w == w && m.h == h && m.hz == hz;
        CHECK(found);
    }

    // descriptors
    bool name = false, range = false;
    for (int i = 0; i < 4; i++) {
        const uint8_t *p = e + 54 + 18 * i;
        if (p[0] || p[1]) {
            CHECK(i < 2);
            CheckDtdMatches(p, kModes[i]);  // DTD 1 = preferred (index 0), DTD 2 = 1080p60 (index 1)
            continue;
        }
        if (p[3] == 0xFC) {
            char n[14] = {};
            for (int k = 0; k < 13 && p[5 + k] != 0x0A; k++)
                n[k] = char(p[5 + k]);
            printf("  name \"%s\"\n", n);
            CHECK(!strcmp(n, "Starbite"));
            name = true;
        } else if (p[3] == 0xFD) {
            printf("  range V %u-%u Hz, H %u-%u kHz, max %u MHz\n", p[5], p[6], p[7], p[8], p[9] * 10);
            for (const Mode &m : kModes) {
                CHECK(m.hz >= p[5] && m.hz <= p[6]);
                CHECK(HSyncHz(m) >= p[7] * 1000u && HSyncHz(m) <= p[8] * 1000u);
                CHECK(PixelRate(m) <= p[9] * 10000000ull);
            }
            range = true;
        }
    }
    CHECK(name && range);
    CHECK(kPreferredMode == 0 && kModes[0].w == 1920 && kModes[0].h == 1080 && kModes[0].hz == 30);

    printf("modes (%u, preferred %u):\n", kModeCount, kPreferredMode);
    for (const Mode &m : kModes)
        printf("  %4ux%-4u @%u Hz  total %ux%u  pixel rate %llu  hsync %u Hz\n", m.w, m.h, m.hz, m.tw, m.th,
               (unsigned long long)PixelRate(m), HSyncHz(m));

    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        CHECK(f && fwrite(e, 1, 128, f) == 128);
        if (f)
            fclose(f);
    }
    printf("%s (%d failures)\n", fails ? "FAILED" : "ok", fails);
    return fails != 0;
}
