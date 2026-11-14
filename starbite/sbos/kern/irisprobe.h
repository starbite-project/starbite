/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the iris scanner's probes (docs/drivers/IRIS.md 6). Levels 0-1 (P0) are READ ONLY towards the iris parts:
 * nothing is powered, no GPIO / PMIC / LED register is written, the sensor is not touched. Level 2 is P1 (attended).
 *   level 0  register reads: the S2MPS18's LDO39/40/41 control words (VDDD 1.05 V, VDDA 2.8 V, VDDIO 1.8 V of the
 *            iris), the iris GPIOs (iris_en gpg0-7, reset gpg2-3, MCLK gpc1-6, the I2C pins gpp4-6/7) and their line
 *            levels, USI07's mode words, the ISPPRE domain's state and, only if it is already on, CSIS3's first
 *            registers through sb_probe32 (a fault there = the secure world holds CSIS3).
 *   level 1  silent: the S2MPB02's IR LED registers (0x13, 0x16-0x1B) read over the torch's I2C bus (fled.c brings the
 *            bus up as for the torch; nothing written to the chip): the LED's boot values, decoded.
 *   level 2  attended only: P1 (kern/iris.c, iris.h): P1a the sensor up in the stock order, its ID and bus (HSI2C or
 *            bit-banged), off; P1b ISPPRE on, CSIS3 read fault-safe (a fault: stop), one dark frame without the LED
 *            (at most 0.5 s of streaming) into 0xD8000000, everything off, the frame's statistics. Never the LED.
 * The decoding below is pure C (host test kern/host/irisprobe-test.c). */
#ifndef SBOS_IRISPROBE_H
#define SBOS_IRISPROBE_H
#include <stdint.h>

/* S2MPS18 LDOn CTRL (s2mps18-private.h: L39CTRL 0x70, L40 0x71, L41 0x72): [7:6] enable, [5:0] the voltage code */
#define IRIS_L39CTRL 0x70U
#define IRIS_L40CTRL 0x71U
#define IRIS_L41CTRL 0x72U
struct iris_ldo { unsigned on; unsigned code; };
void iris_ldo_decode(uint8_t ctrl, struct iris_ldo *o);

/* a Samsung GPIO bank (CON +0: 4 bits a pin; DAT +4: 1 bit; PUD +8: 4 bits) */
struct iris_pin { unsigned func, level, pud; };
void iris_pin_decode(uint32_t con, uint32_t dat, uint32_t pud, unsigned pin, struct iris_pin *o);

/* the S2MPB02's IR LED (leds-s2mpb02.c): CTRL2 0x13, CUR2 0x16 (high nibble: the IR current code n-1), TIME2 0x17
 * (bit 0 the IR max timer), IRON1/2 0x18/0x19 (pulse width [9:2] / [7:6] = [1:0]; IRON2 [4:0] the max on-time - 1),
 * IRD1/2 0x1A/0x1B (delay the same; IRD2 bit 1 active discharge) */
struct iris_irled {
    uint8_t ctrl2, cur2, time2;
    unsigned cur_code;      /* 1..16 (CUR2 [7:4] + 1) */
    unsigned width, delay;  /* 10 bits each (units not documented) */
    unsigned max_timer_on, max_time;   /* TIME2 bit 0; IRON2 [4:0] + 1 */
    unsigned discharge;     /* IRD2 bit 1 */
};
void iris_irled_decode(const uint8_t r[7], struct iris_irled *o);   /* r: 0x13, 0x16..0x1B */

int drv_iris_probe(int level, unsigned *wait_ms);
void drv_iris_abort(void);
#define DRV_IRIS_BUDGET_MS 10000U              /* level 2 (P1) stops itself at 9 s; levels 0-1 take < 1 s */

#endif
