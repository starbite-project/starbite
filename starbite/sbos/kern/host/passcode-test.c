/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of starbite/sbos/kern/passcode.c (Track A): SHA-256 against the FIPS vectors; set, check, change and
 * remove; wrong tries and the back-off schedule on a fake clock; the record's round trip (a restart with and without
 * the wall clock); the digits never in the record; the hash's time (an estimate for the A55).
 *   sh starbite/sbos/kern/host/passcode-test.sh [-v]        (exit status 0 = all passed) */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "passcode.h"

static int verbose, fails, checks;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        } else if (verbose) {                                                                                          \
            printf("ok   ");                                                                                           \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static void hex(const uint8_t *p, int n, char *out)
{
    for (int i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", p[i]);
}

static int contains(const uint8_t *hay, int n, const void *needle, int m)
{
    for (int i = 0; i + m <= n; i++)
        if (!memcmp(hay + i, needle, (size_t)m)) return 1;
    return 0;
}

static const uint8_t SALT_A[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
static const uint8_t SALT_B[16] = { 0xa5, 0x5a, 0x3c, 0xc3, 0x0f, 0xf0, 0x99, 0x66, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0 };

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    char h[65];
    uint8_t d[32];

    /* ---- SHA-256: FIPS 180-4 examples and a two-block message ---- */
    pass_sha256("abc", 3, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256(abc) %s", h);
    pass_sha256("", 0, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "sha256('') %s", h);
    const char *m2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    pass_sha256(m2, (uint32_t)strlen(m2), d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "sha256(448 bits) %s", h);
    {
        static uint8_t mil[1000000];
        memset(mil, 'a', sizeof mil);
        pass_sha256(mil, sizeof mil, d);
        hex(d, 32, h);
        CHECK(!strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "sha256(10^6 a) %s", h);
    }
    /* 55, 56 and 64 bytes: the padding's edges */
    {
        uint8_t m[64];
        memset(m, 'x', sizeof m);
        static const char *const W[3] = { "d5e285683cd4efc02d021a5c62014694958901005d6f71e89e0989fac77e4072",
                                          "04c26261370ee7541549d16dee320c723e3fd14671e66a099afe0a377c16888e",
                                          "7ce100971f64e7001e8fe5a51973ecdfe1ced42befe7ee8d5fd6219506b5393c" };
        static const int L[3] = { 55, 56, 64 };
        for (int i = 0; i < 3; i++) {
            pass_sha256(m, (uint32_t)L[i], d); hex(d, 32, h);
            CHECK(!strcmp(h, W[i]), "sha256(%d x) %s", L[i], h);
        }
    }

    /* ---- no passcode ---- */
    pass_reset();
    uint64_t now = 1000;
    CHECK(pass_len() == 0, "none: len 0");
    CHECK(pass_check("1234", now, 0) == 1, "none: any check passes");
    uint8_t rec[PASS_REC_LEN];
    CHECK(pass_pack(rec, now, 0) == 0, "none: nothing to keep");

    /* ---- set, check, timing ---- */
    CHECK(pass_set("", "12345", SALT_A, now, 0) == -1, "5 digits refused");
    CHECK(pass_set("", "12a456", SALT_A, now, 0) == -1, "a letter refused");
    CHECK(pass_set("", "1234567", SALT_A, now, 0) == -1, "7 digits refused");
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK(pass_set("", "482915", SALT_A, now, 0) == 0, "set 6 digits");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    CHECK(pass_len() == 6, "len 6");
    CHECK(pass_check("482915", now, 0) == 1, "right code");
    CHECK(pass_check("482914", now, 0) == 0, "wrong code");
    CHECK(pass_fails() == 1, "one wrong try");
    CHECK(pass_check("4829", now, 0) == 0, "a 4-digit try at a 6-digit code is wrong");
    CHECK(pass_check("482915", now, 0) == 1 && pass_fails() == 0, "right again: tries back to 0");

    /* ---- change and remove ---- */
    CHECK(pass_set("000000", "1357", SALT_B, now, 0) == -1 && pass_fails() == 1, "change with a wrong old code: refused, counted");
    CHECK(pass_set("482915", "1357", SALT_B, now, 0) == 0 && pass_len() == 4 && pass_fails() == 0, "changed to 4 digits");
    CHECK(pass_check("482915", now, 0) == 0, "old code no longer works");
    CHECK(pass_check("1357", now, 0) == 1, "new code works");
    CHECK(pass_set("1357", "", SALT_B, now, 0) == 0 && pass_len() == 0, "removed");
    CHECK(pass_check("9999", now, 0) == 1, "removed: anything passes");

    /* ---- back-off: 1 min at the 6th, 5 at the 7th, 15 at the 8th, 1 h at the 9th and 10th ---- */
    CHECK(pass_set("", "2468", SALT_A, now, 0) == 0, "set 4 digits");
    for (int i = 1; i <= 5; i++) {
        CHECK(pass_check("1111", now, 0) == 0, "wrong try %d", i);
        CHECK(pass_wait(now) == 0, "no wait after %d", i);
    }
    static const uint32_t want[5] = { 60, 300, 900, 3600, 3600 };
    for (int k = 0; k < 5; k++) {
        int n = 6 + k;
        CHECK(pass_check("1111", now, 0) == 0, "wrong try %d", n);
        CHECK(pass_wait(now) == want[k], "wait after try %d: %u s (want %u)", n, pass_wait(now), want[k]);
        int r = pass_check("2468", now + 1000, 0);
        CHECK(r == -(int)(want[k] - 1), "a try during the wait is refused (%d), the right code too", r);
        CHECK(pass_fails() == n, "the refused try is not counted (%d)", pass_fails());
        now += (uint64_t)want[k] * 1000;
        CHECK(pass_wait(now) == 0, "the wait over after %u s", want[k]);
    }
    CHECK(pass_backoff_s(25) == 3600, "the 25th: 1 h");

    /* ---- the record: a restart during a wait ---- */
    int64_t utc = 1794500000;
    CHECK(pass_check("1111", now, utc) == 0 && pass_wait(now) == 3600, "11th wrong: 1 h");
    now += 600 * 1000;                                  /* 10 min of it gone */
    int n = pass_pack(rec, now, utc + 600);
    CHECK(n == (int)PASS_REC_LEN, "packed %d bytes", n);
    CHECK(!contains(rec, n, "2468", 4), "the digits are not in the record (ASCII)");
    {
        const uint8_t bcd[2] = { 0x24, 0x68 }, raw[4] = { 2, 4, 6, 8 };
        CHECK(!contains(rec, n, bcd, 2) && !contains(rec, n, raw, 4), "nor as BCD or digit bytes");
    }
    pass_reset();
    CHECK(pass_len() == 0, "after a reset nothing");
    /* the restart: sb_ms from 0 again; the wall clock known: 50 min left, from it */
    CHECK(pass_load(rec, n, 5, utc + 600 + 30) == 1, "loaded");
    CHECK(pass_len() == 4 && pass_fails() == 11, "len and tries survive (%d, %d)", pass_len(), pass_fails());
    CHECK(pass_wait(5) == 3000 - 30, "the wait goes on from the clock: %u s", pass_wait(5));
    /* the clock moved back an hour: never longer than it was */
    CHECK(pass_load(rec, n, 5, utc - 3000) == 1 && pass_wait(5) == 3000, "a clock moved back: not longer (%u)", pass_wait(5));
    /* no clock at the restart: all of it again */
    CHECK(pass_load(rec, n, 5, 0) == 1 && pass_wait(5) == 3000, "no clock: the whole wait again (%u)", pass_wait(5));
    /* the clock is past its end */
    CHECK(pass_load(rec, n, 5, utc + 99999) == 1 && pass_wait(5) == 0, "past the end: no wait");
    CHECK(pass_check("2468", 10, 0) == 1 && pass_fails() == 0, "the right code after the load");
    /* a record packed without a clock: the whole wait again at the load */
    pass_check("0000", 10, 0); pass_check("0000", 10, 0); pass_check("0000", 10, 0);
    pass_check("0000", 10, 0); pass_check("0000", 10, 0); pass_check("0000", 10, 0);
    CHECK(pass_wait(10) == 60, "6 wrong: 1 min");
    n = pass_pack(rec, 30010, 0);
    CHECK(n == (int)PASS_REC_LEN, "packed");
    CHECK(pass_load(rec, n, 0, utc) == 1 && pass_wait(0) == 30, "packed without a clock: the 30 s left, all of them");
    /* tries only (no code): kept */
    pass_reset();
    CHECK(pass_pack(rec, 0, 0) == 0, "nothing to keep");
    /* a bad record changes nothing */
    CHECK(pass_set("", "135790", SALT_B, 0, 0) == 0, "set");
    n = pass_pack(rec, 0, 0);
    rec[0] = 'X';
    CHECK(pass_load(rec, n, 0, 0) == 0 && pass_len() == 6, "a bad magic is refused, the state kept");
    rec[0] = 'S';
    rec[5] = 5;
    CHECK(pass_load(rec, n, 0, 0) == 0, "length 5 refused");
    rec[5] = 6;
    CHECK(pass_load(rec, n - 1, 0, 0) == 0, "short record refused");
    pass_reset();
    CHECK(pass_load(rec, n, 0, 0) == 1 && pass_check("135790", 0, 0) == 1, "round trip: the code still works");
    CHECK(!contains(rec, n, "135790", 6), "the digits are not in the record");
    /* two salts: the same digits, other hashes */
    {
        uint8_t r1[PASS_REC_LEN], r2[PASS_REC_LEN];
        pass_reset(); pass_set("", "135790", SALT_A, 0, 0); pass_pack(r1, 0, 0);
        pass_reset(); pass_set("", "135790", SALT_B, 0, 0); pass_pack(r2, 0, 0);
        CHECK(memcmp(r1 + 44, r2 + 44, 32) != 0, "another salt, another hash");
    }

    /* the time (informational: this build has ASan and UBSan, several times slower than the kernel's -O2). The A55
     * estimate is from the kernel compiler's code (gcc -O2 -mcpu=cortex-a55, objdump of sha_block): ~4100 instructions
     * a block (64 rounds of 43, 48 schedule steps of 27), one block a round; at an IPC of 1 (in-order, the rounds'
     * dependency chain) 20000 rounds are ~82 M cycles: ~46 ms at 1.8 GHz, ~78 ms at the idle 1.05 GHz (budget 150) */
    printf("passcode: %u rounds took %.1f ms here (sanitizers on: not a measure); the A55 estimate: ~46 ms at 1.8 GHz\n", PASS_ROUNDS, ms);
    printf("passcode host test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
