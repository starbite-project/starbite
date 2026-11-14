/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of irisprobe.c's decoders (the iris P0 probe's register words). */
#define IRISPROBE_HOST
#include <stdio.h>
#include "../irisprobe.c"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s (line %d)\n", m, __LINE__); ++fails; } } while (0)

int main(void)
{
    struct iris_ldo l;
    iris_ldo_decode(0xC0 | 0x14, &l);
    CHECK(l.on == 3 && l.code == 0x14, "LDO on + code");
    iris_ldo_decode(0x14, &l);
    CHECK(l.on == 0 && l.code == 0x14, "LDO off");

    struct iris_pin p;
    iris_pin_decode(0x10000000U, 0x80U, 0x30000000U, 7, &p);           /* gpg0-7: output, high, pull-up */
    CHECK(p.func == 1 && p.level == 1 && p.pud == 3, "pin 7 fields");
    iris_pin_decode(0x22000000U, 0xC0U, 0, 6, &p);                     /* gpp4-6 function 2, high */
    CHECK(p.func == 2 && p.level == 1 && p.pud == 0, "pin 6 fields");

    /* Samsung's boot values (s2mpb02_led_setup): CTRL2 0x38, CUR2 0xAF, IRON1 0x19, IRON2 0x0B, IRD1 0, IRD2 0x2C
     * (+ active discharge, bit 1); TIME2 as found (0 here) */
    const uint8_t r[7] = { 0x38, 0xAF, 0x00, 0x19, 0x0B, 0x00, 0x2E };
    struct iris_irled d;
    iris_irled_decode(r, &d);
    CHECK(d.cur_code == 11, "current code: CUR2 0xAF high nibble 0xA + 1 = 11 (Samsung's 1050 mA comment)");
    CHECK(d.width == (0x19U << 2 | 0U), "pulse width: IRON1 0x19 << 2 | IRON2 [7:6]");
    CHECK(d.delay == 0, "delay 0");
    CHECK(d.max_time == (0x0BU & 0x1FU) + 1U && !d.max_timer_on, "max time IRON2 [4:0] + 1, timer off");
    CHECK(d.discharge == 1, "active discharge IRD2 bit 1");
    const uint8_t r2[7] = { 0, 0x0F, 0x01, 0xFF, 0xDF, 0x80, 0xC0 };
    iris_irled_decode(r2, &d);
    CHECK(d.cur_code == 1 && d.width == 1023 && d.delay == (0x80U << 2 | 3U) && d.max_timer_on && d.max_time == 32,
          "extremes: code 1, width 1023, delay, timer on, max time 32");
    printf("irisprobe host test: %s (%d failed)\n", fails ? "FAILED" : "passed", fails);
    return fails != 0;
}
