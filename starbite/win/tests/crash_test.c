/* Host test of the crash ledger's portable part (sbwdog/sbcl_core.c). */
#include <stdio.h>
#include <string.h>
#include "../sbwdog/sbcl_core.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    struct sbcl_page p;
    struct sbcl_mod m[3];
    char out[512];

    CHECK(sizeof(struct sbcl_page) <= SBCL_SIZE, "page fits (%zu)", sizeof(struct sbcl_page));
    memset(&p, 0, sizeof p);
    CHECK(!sbcl_valid(&p), "zero page invalid");
    sbcl_describe(&p, out, sizeof out);
    CHECK(!strcmp(out, "no record"), "zero page: %s", out);

    memset(m, 0, sizeof m);
    m[0].base = 0xfffff80001000000ULL; m[0].size = 0x200000; strcpy(m[0].name, "ntoskrnl.exe");
    m[1].base = 0xfffff80005000000ULL; m[1].size = 0x40000; strcpy(m[1].name, "sbwddm.sys");
    m[2].base = 0xfffff80006000000ULL; m[2].size = 0x1000; memcpy(m[2].name, "a_very_long_driver_name_123.sys", 28);
    sbcl_resolve(m, 3, 0xfffff8000501a2b4ULL, out);
    CHECK(!strcmp(out, "sbwddm.sys+0x1a2b4"), "resolve: %s", out);
    sbcl_resolve(m, 3, 0xfffff80005040000ULL, out);
    CHECK(!strcmp(out, ""), "one past the end: '%s'", out);
    sbcl_resolve(m, 3, 0xc0000005ULL, out);
    CHECK(!strcmp(out, ""), "status code is no address: '%s'", out);
    sbcl_resolve(m, 3, 0xfffff80006000010ULL, out);
    CHECK(!strcmp(out, "a_very_long_driver_name_123.+0x10"), "28-char name, no terminator: %s", out);

    memset(&p, 0, sizeof p);
    p.magic = SBCL_MAGIC; p.version = SBCL_VERSION; p.boot_seq = 17; p.state = SBCL_BUGCHECK;
    p.uptime_s = 815; p.bc_uptime_s = 812; p.bc_code = 0x3B; p.mods = 120;
    p.p[0] = 0xc0000005ULL; p.p[1] = 0xfffff8000501a2b4ULL; p.p[2] = 0xffffd00012345678ULL; p.p[3] = 0;
    sbcl_resolve(m, 3, p.p[1], p.where[1]);
    sbcl_seal(&p);
    CHECK(sbcl_valid(&p), "sealed record valid");
    sbcl_describe(&p, out, sizeof out);
    CHECK(!strcmp(out, "boot 17: BUGCHECK 0x0000003b SYSTEM_SERVICE_EXCEPTION (0xc0000005, 0xfffff8000501a2b4, "
                       "0xffffd00012345678, 0x0) at 812 s; p2 = sbwddm.sys+0x1a2b4"), "bugcheck text: %s", out);
    p.uptime_s++;
    CHECK(!sbcl_valid(&p), "a changed word breaks the check");
    sbcl_seal(&p);
    p.bc_code = 0x1234; p.mods = 0; memset(p.where, 0, sizeof p.where); sbcl_seal(&p);
    sbcl_describe(&p, out, sizeof out);
    CHECK(strstr(out, "BUGCHECK 0x00001234 (") && strstr(out, "no module snapshot yet"), "unknown code: %s", out);

    p.state = SBCL_RUNNING; p.uptime_s = 1440; sbcl_seal(&p);
    sbcl_describe(&p, out, sizeof out);
    CHECK(!strncmp(out, "boot 17: ended without a shutdown or bugcheck after ~1440 s", 59), "running: %s", out);
    p.state = SBCL_SHUTDOWN; p.shutdown_uptime_s = 300; sbcl_seal(&p);
    sbcl_describe(&p, out, sizeof out);
    CHECK(!strcmp(out, "boot 17: clean shutdown or restart after 300 s"), "shutdown: %s", out);
    p.state = 9; sbcl_seal(&p);
    CHECK(!sbcl_valid(&p), "unknown state invalid");

    sbcl_describe(&p, out, 8);
    CHECK(strlen(out) == 7, "truncated to the buffer: '%s'", out);
    CHECK(!strcmp(sbcl_bc_name(0x116), "VIDEO_TDR_FAILURE") && !strcmp(sbcl_bc_name(0x51), "REGISTRY_ERROR"), "names");

    printf("sbcl: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
