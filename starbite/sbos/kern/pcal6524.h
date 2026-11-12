/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the PCAL6524 I2C GPIO expander (pcal6524.c): 24 pins in three ports at 0x22 on HSI2C 0x10930000
 * (USI10_I2C, PERIC1). Its pins switch the Bluetooth power (P0_7 = BT_REG_ON), NFC, wireless charging and the
 * DisplayPort mux, so a user names the pins it may write (allow_mask) and every other pin is never written.
 *
 * Use (single-threaded):
 *   pcal_look(io, &l)                 level 0: SoC-side reads only (bus pins, mode word, clocks, the reset pin)
 *   pcal_open(io, 1u << 7, 0)         the bus up and the expander out of reset if it was held; the ports as found
 *   pcal_pin_out(7, 1) ...            only allowed pins: the output bit first, then the direction; read back
 *   pcal_pin_restore(7)               that pin's output and direction bits as found
 *   pcal_close()                      the reset pin, the bus pins, the controller and the mode word as found
 */
#ifndef SBOS_PCAL6524_H
#define SBOS_PCAL6524_H
#include <stdint.h>

/* the hardware access a user gives (the kernel: libsb; the host test: fakes). Physical addresses. */
struct pcal_io {
    uint32_t (*rd32)(uint64_t pa);
    int (*probe32)(uint64_t pa, uint32_t *v);       /* 1 and the value, 0 if the read faulted (its clock is off) */
    void (*wr32)(uint64_t pa, uint32_t v);          /* completes before returning */
    void (*i2c_init)(uint32_t fs1, uint32_t fs2, uint32_t fs3);   /* HSI2C 0x10930000: reset, master, auto mode */
    int (*i2c_rd)(uint8_t addr, uint8_t reg, uint8_t *v);         /* 1 ok */
    int (*i2c_wr)(uint8_t addr, uint8_t reg, uint8_t v);
    void (*delay_us)(uint32_t us);
    void (*log)(const char *line);
};

#define PCAL_ADDR 0x22U
#define PCAL_BUS 0x10930000ULL
#define PCAL_IN0 0x00U                      /* INPUT 0x00-0x02 */
#define PCAL_OUT0 0x04U                     /* OUTPUT 0x04-0x06 */
#define PCAL_CFG0 0x0CU                     /* CONFIG 0x0C-0x0E, 1 = input */

struct pcal_look {
    uint32_t mode;                          /* SYSREG_PERIC1 USI10 mode word (4 = I2C) */
    uint32_t gpp6_con, gpp6_pud;            /* the bus pins gpp6-2 SCL, gpp6-3 SDA (function 3) */
    uint32_t gpg3_con, gpg3_dat;            /* gpg3-6: the expander's reset, output high = running */
    uint32_t gate[4], qch, div;             /* CMU_PERIC1 USI10_I2C */
    uint32_t ctl, conf, usi_con;            /* the controller */
    int gate_closed, in_reset, bus_ready, ctl_read;
};
int pcal_look(const struct pcal_io *io, struct pcal_look *l);   /* 0 if a SoC register read faulted */
void pcal_look_log(const struct pcal_look *l, const char *prefix);

#define PCAL_EGATE   -1                     /* a USI10_I2C clock gate is closed and allow_cmu is 0 */
#define PCAL_EBUS    -2                     /* no answer from 0x22 */
#define PCAL_EPIN    -3                     /* a pin outside allow_mask */
#define PCAL_EREAD   -4                     /* a register did not read back as written */
#define PCAL_ESTATE  -5
int pcal_open(const struct pcal_io *io, uint32_t allow_mask, int allow_cmu);
int pcal_ports(uint8_t in[3], uint8_t out[3], uint8_t cfg[3]);
/* *is_out: CONFIG bit 0; *drive: OUTPUT bit; *level: INPUT bit */
int pcal_pin(unsigned pin, int *is_out, int *drive, int *level);
int pcal_pin_out(unsigned pin, int level);
int pcal_pin_restore(unsigned pin);
/* 1 if every register this file changed reads back as found; 0 otherwise (logged) */
int pcal_close(void);
int pcal_is_open(void);
const char *pcal_why(void);
#endif
