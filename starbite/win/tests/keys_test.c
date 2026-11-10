/* Host test of sbkeys' core (starbite/win/sbkeys/sbk_core.c). */
#include <stdio.h>
#include "../sbkeys/sbk_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* run keys for ms at 30 ms polls; returns the first non-NONE action (and how many) */
static enum sbk_action run(const struct sbk_cfg *c, struct sbk_state *s, sbk_u32 keys, sbk_u32 ms, int *n)
{
    enum sbk_action first = SBK_NONE, a;
    sbk_u32 t;
    for (t = 0; t < ms; t += 30) {
        a = sbk_step(c, s, keys, 30);
        if (a != SBK_NONE) {
            if (first == SBK_NONE) first = a;
            if (n) (*n)++;
        }
    }
    return first;
}

static void setup(struct sbk_cfg *c, struct sbk_state *s, int sas)
{
    c->hold_ms = 3000; c->arm_ms = 2000; c->sas = (sbk_u32)sas; c->sas_max_ms = 1500;
    sbk_cfg_fix(c);
    sbk_init(s);
}

int main(void)
{
    struct sbk_cfg c;
    struct sbk_state s;
    int n;

    /* register decoding: active low, the loader's bits */
    CHECK(sbk_keys(0xFFFFFFFFu, 0xFFFFFFFFu) == 0);
    CHECK(sbk_keys(~(1u << 3), 0xFFFFFFFFu) == SBK_UP);
    CHECK(sbk_keys(~(1u << 4), 0xFFFFFFFFu) == SBK_DOWN);
    CHECK(sbk_keys(~(1u << 6), 0xFFFFFFFFu) == SBK_BIXBY);
    CHECK(sbk_keys(0xFFFFFFFFu, ~(1u << 4)) == SBK_POWER);
    CHECK(sbk_keys(0, 0) == (SBK_UP | SBK_DOWN | SBK_BIXBY | SBK_POWER));
    /* pins: input (0) or EINT (F) only */
    CHECK(sbk_pins_ok(0x00000000u, 0x00000000u));
    CHECK(sbk_pins_ok(0x0FF00000u | 0x0000F000u, 0x000F0000u));      /* 6, 4, 3 EINT; GPA2 4 EINT */
    CHECK(!sbk_pins_ok(0x01000000u, 0));                               /* Bixby pin an output */
    CHECK(!sbk_pins_ok(0, 0x00010000u));                               /* Power pin an output */
    CHECK(sbk_pins_ok(0x10000000u, 0x1u));                             /* other pins don't matter */
    /* config limits */
    c.hold_ms = 50; c.arm_ms = 0; c.sas = 7; c.sas_max_ms = 99999; sbk_cfg_fix(&c);
    CHECK(c.hold_ms == 3000 && c.arm_ms == 2000 && c.sas == 1 && c.sas_max_ms == 1500);

    /* Bixby held 3 s after arming: one restart, at ~3 s */
    setup(&c, &s, 0);
    CHECK(run(&c, &s, 0, 2100, NULL) == SBK_NONE && s.armed);
    CHECK(run(&c, &s, SBK_BIXBY, 2900, NULL) == SBK_NONE);
    n = 0;
    CHECK(run(&c, &s, SBK_BIXBY, 300, &n) == SBK_RESTART && n == 1);
    n = 0;
    CHECK(run(&c, &s, SBK_BIXBY, 5000, &n) == SBK_NONE && n == 0);    /* held on: still once */
    /* released and held again: fires again (the driver itself acts once per boot) */
    run(&c, &s, 0, 300, NULL);
    CHECK(run(&c, &s, SBK_BIXBY, 3200, NULL) == SBK_RESTART);

    /* a pin stuck "pressed" from the start never arms, never restarts */
    setup(&c, &s, 1);
    CHECK(run(&c, &s, SBK_BIXBY, 60000, NULL) == SBK_NONE && !s.armed);
    CHECK(run(&c, &s, SBK_POWER | SBK_UP, 60000, NULL) == SBK_NONE && !s.armed);
    /* not armed until released 2 s in a row */
    setup(&c, &s, 0);
    run(&c, &s, 0, 1500, NULL);
    run(&c, &s, SBK_UP, 60, NULL);
    run(&c, &s, 0, 1500, NULL);
    CHECK(!s.armed);
    run(&c, &s, 0, 600, NULL);
    CHECK(s.armed);

    /* Bixby with another key: never a restart (Download-mode style chords, Vol Down + Power) */
    setup(&c, &s, 1);
    run(&c, &s, 0, 2100, NULL);
    CHECK(run(&c, &s, SBK_BIXBY | SBK_DOWN, 10000, NULL) == SBK_NONE);
    run(&c, &s, 0, 100, NULL);
    CHECK(run(&c, &s, SBK_DOWN | SBK_POWER, 10000, NULL) == SBK_NONE);  /* the hardware force restart chord */
    CHECK(run(&c, &s, 0, 100, NULL) == SBK_NONE);
    /* Bixby 2.9 s, other key joins, Bixby alone again: the hold restarts from zero */
    CHECK(run(&c, &s, SBK_BIXBY, 2900, NULL) == SBK_NONE);
    CHECK(run(&c, &s, SBK_BIXBY | SBK_UP, 60, NULL) == SBK_NONE);
    CHECK(run(&c, &s, SBK_BIXBY, 2900, NULL) == SBK_NONE);
    CHECK(run(&c, &s, SBK_BIXBY, 200, NULL) == SBK_RESTART);
    /* a long gap between polls (sleep) drops the hold */
    setup(&c, &s, 0);
    run(&c, &s, 0, 2100, NULL);
    run(&c, &s, SBK_BIXBY, 2800, NULL);
    CHECK(sbk_step(&c, &s, SBK_BIXBY, 5000) == SBK_NONE);
    CHECK(run(&c, &s, SBK_BIXBY, 2800, NULL) == SBK_NONE);
    CHECK(run(&c, &s, SBK_BIXBY, 300, NULL) == SBK_RESTART);

    /* SAS: Volume Up + Power brief press, only with sas on */
    setup(&c, &s, 1);
    run(&c, &s, 0, 2100, NULL);
    run(&c, &s, SBK_UP, 90, NULL);
    run(&c, &s, SBK_UP | SBK_POWER, 300, NULL);
    run(&c, &s, SBK_POWER, 60, NULL);
    CHECK(sbk_step(&c, &s, 0, 30) == SBK_SAS);
    CHECK(run(&c, &s, 0, 1000, NULL) == SBK_NONE);
    /* off by default */
    setup(&c, &s, 0);
    run(&c, &s, 0, 2100, NULL);
    run(&c, &s, SBK_UP | SBK_POWER, 300, NULL);
    CHECK(sbk_step(&c, &s, 0, 30) == SBK_NONE);
    /* too long a press: none */
    setup(&c, &s, 1);
    run(&c, &s, 0, 2100, NULL);
    run(&c, &s, SBK_UP | SBK_POWER, 2000, NULL);
    CHECK(sbk_step(&c, &s, 0, 30) == SBK_NONE);
    /* Up then Power without overlap: none */
    run(&c, &s, SBK_UP, 200, NULL);
    run(&c, &s, SBK_POWER, 200, NULL);
    CHECK(sbk_step(&c, &s, 0, 30) == SBK_NONE);
    /* Up + Power with Volume Down or Bixby involved: none */
    run(&c, &s, SBK_UP | SBK_POWER, 200, NULL);
    run(&c, &s, SBK_UP | SBK_POWER | SBK_DOWN, 60, NULL);
    CHECK(sbk_step(&c, &s, 0, 30) == SBK_NONE);
    run(&c, &s, SBK_UP | SBK_POWER | SBK_BIXBY, 200, NULL);
    CHECK(sbk_step(&c, &s, 0, 30) == SBK_NONE);
    /* Power alone, Volume Up alone: none */
    run(&c, &s, SBK_POWER, 200, NULL);
    CHECK(sbk_step(&c, &s, 0, 30) == SBK_NONE);
    run(&c, &s, SBK_UP, 200, NULL);
    CHECK(sbk_step(&c, &s, 0, 30) == SBK_NONE);
    /* and the chord still works after all that */
    run(&c, &s, SBK_UP | SBK_POWER, 200, NULL);
    CHECK(sbk_step(&c, &s, 0, 30) == SBK_SAS);

    printf("sbkeys core: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
