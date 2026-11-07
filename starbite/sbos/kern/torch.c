/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS flashlight LED (docs/STARBITE-OS.md 9.1), v0.2: the torch GPIO only.
 *
 * On the S9 the rear LED's torch is switched by torch1-gpio (gpg2 pin 4, PERIC0 pinctrl 0x10430000, bank +0xC0:
 * CON 0x104300C0 bits 19:16 = 1 output, DAT 0x104300C4 bit 4); the S2MPB02 behind it (I2C 0x59) only sets the
 * current, and its torch channel is limited to 300 mA by design (FLED_CUR1 bits 3:0, 20 mA steps; Samsung's
 * leds-s2mpb02.c, LineageOS android_kernel_samsung_exynos9810 lineage-20, GPL-2.0). The chip's I2C bus was not
 * brought up by S-Boot, so v0.2 leaves the current as the chip holds it and switches the pin only.
 *
 * Checks before the first switch: both banks read (a probe), the flash pin (flash1-gpio, gpg3 pin 5, PERIC1
 * 0x108300C0, 1250 mA) is not an output driven high; it is never written. Every first use logs what it read.
 * Off: the pin low. Auto-off after 10 minutes, at 2 % battery and when SOS ends. */
#include "sb.h"
#include "kern.h"

#define TORCH_CON 0x104300C0UL
#define TORCH_DAT 0x104300C4UL
#define TORCH_PIN 4U
#define FLASH_CON 0x108300C0UL
#define FLASH_DAT 0x108300C4UL
#define FLASH_PIN 5U

static int state;                   /* 0 unknown, 1 checked and usable, -1 refused (why) */
static const char *why = "not checked";
static int on;
static uint64_t on_since;

static void wr32(uintptr_t a, uint32_t v)
{
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)a = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

static int check(void)
{
    if (state)
        return state > 0;
    uint32_t tc = 0, td = 0, fc = 0, fd = 0;
    int ok = sb_probe32(TORCH_CON, &tc) && sb_probe32(TORCH_DAT, &td) && sb_probe32(FLASH_CON, &fc) &&
             sb_probe32(FLASH_DAT, &fd);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "torch: gpg2 con ");
    ub_hex(&u, tc, 8);
    ub_s(&u, " dat ");
    ub_hex(&u, td, 2);
    ub_s(&u, ", gpg3 con ");
    ub_hex(&u, fc, 8);
    ub_s(&u, " dat ");
    ub_hex(&u, fd, 2);
    sb_log(u.b);
    if (!ok) {
        state = -1;
        why = "The light's pins do not read";
    } else if (tc == 0xffffffffU || fc == 0xffffffffU) {
        state = -1;
        why = "The light's pins read all ones";
    } else if (((fc >> (4 * FLASH_PIN)) & 0xfU) == 1U && (fd >> FLASH_PIN) & 1U) {
        state = -1;
        why = "The camera flash pin is on: not touching the LED";
    } else {
        state = 1;
        why = 0;
    }
    if (state < 0)
        sb_logf("torch: refused: ", why);
    return state > 0;
}

int torch_ready(void)
{
    return check();
}

const char *torch_why(void)
{
    return why;
}

int torch_on(void)
{
    return on;
}

int torch_set(int lit)
{
    if (!lit) {
        if (on) {
            uint32_t d = *(volatile uint32_t *)TORCH_DAT;
            wr32(TORCH_DAT, d & ~(1U << TORCH_PIN));
            on = 0;
            kt_cancel(KT_SOS, 9);
        }
        return 0;
    }
    if (!check())
        return SBOS_EAGAIN;
    if (on)
        return 0;
    uint32_t d = *(volatile uint32_t *)TORCH_DAT, c = *(volatile uint32_t *)TORCH_CON;
    wr32(TORCH_DAT, d & ~(1U << TORCH_PIN));                         /* low before it becomes an output */
    wr32(TORCH_CON, (c & ~(0xfU << (4 * TORCH_PIN))) | (1U << (4 * TORCH_PIN)));
    d = *(volatile uint32_t *)TORCH_DAT;
    wr32(TORCH_DAT, d | (1U << TORCH_PIN));
    on = 1;
    on_since = sb_ms();
    kt_set(KT_SOS, 9, sb_ms(), 10U * 60U * 1000U, 0);                 /* the 10-minute auto-off */
    return 0;
}

/* SOS's strobe and the auto-off share the SOS task's timers (id 9: the auto-off) */
void torch_timer(unsigned id)
{
    if (id == 9 && on) {
        sb_log("torch: off after 10 minutes");
        torch_set(0);
    }
}
