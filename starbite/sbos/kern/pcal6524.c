/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the PCAL6524 GPIO expander (docs/drivers/BLUETOOTH.md 2).
 *
 * Wiring (device/dtb/starlte.dts, the phone's own DT):
 *   - pcal6524@22 (compatible "pcal6524,gpio-expander", phandle 0x4c) on hsi2c@10930000 (USI10_I2C of PERIC1,
 *     default-clk 200 MHz). Reset: pcal6524,reset-gpio = gpg3-6 (PERIC1 GPIO 0x108300C0), high = running. Linux
 *     programs every pin as an input at probe (pcal6524,config = 0xFFFFFF, data_out 0).
 *   - Registers (Samsung's include/linux/i2c/pcal6524.h, GPL-2.0): INPUT 0x00-0x02, OUTPUT 0x04-0x06, CONFIG
 *     0x0C-0x0E (1 = input); one byte per port. An output: its OUTPUT bit first, then its CONFIG bit cleared
 *     (gpio-pcal6524.c's direction_output order).
 *   - The bus pins gpp6-2 SCL / gpp6-3 SDA, function 3, no pull (PERIC1 GPIO 0x10830040); USI10's mode word
 *     SYSREG_PERIC1 0x1081103C = 4 (I2C) leaves pins 2/3 to USI10_I2C (as Android and kern/fled.c set it).
 *   - Clocks CMU_PERIC1: USI10_I2C gates +0x2050 +0x20A8 +0x210C +0x2110 (bit 20 manual, bit 21 on), Q-channel
 *     +0x304C, divider +0x1830. Read only unless the user passes allow_cmu (then forced on and restored).
 * Pin map (Samsung DT via docs/utility-hw-research): P0_1 DP SBU_SEL, P0_4 DP AUX_OE, P0_6 fingerprint sleep,
 * P0_7 BT_REG_ON, P1_0 WPC_EN, P1_1 NFC FIRM, P1_3 NFC VEN. Only pins in allow_mask are ever written, one bit,
 * read-modify-write, read back.
 * Seen on a phone: an earlier port's XNU driver drives P0_7 this way on this S9 (facts only). This file: not yet run. */
#include "pcal6524.h"
#include "bt_hci.h"

#define USI10_MODE 0x1081103CULL
#define GPP6 0x10830040ULL                  /* CON +0, DAT +4, PUD +8 */
#define GPG3 0x108300C0ULL
#define RST_PIN 6U
#define CMU 0x10800000ULL
static const uint32_t gate_off[4] = { 0x2050, 0x20A8, 0x210C, 0x2110 };
#define QCH_OFF 0x304C
#define DIV_OFF 0x1830
#define GATE_MANUAL (1U << 20)
#define GATE_ON (1U << 21)
/* the controller's registers saved and restored */
static const uint16_t ctl_regs[] = { 0x00, 0x04, 0x08, 0x20, 0x40, 0x48, 0x60, 0x64, 0x68 };
#define NCTL (sizeof(ctl_regs) / sizeof(ctl_regs[0]))

static const struct pcal_io *io;
static int is_open;
static uint32_t allow;
static const char *why = "not opened";
static struct {
    uint32_t mode, gpp6_con, gpp6_pud, gpg3_con, gpg3_dat, ctl[NCTL], usi_con, gate[4], qch;
    uint8_t out[3], cfg[3];
    int mode_set, pins_set, reset_released, ctl_saved, gates_set;
} was;

static void say(const char *s)
{
    if (io && io->log)
        io->log(s);
}

static int allowed(uint64_t pa)
{
    if (pa >= PCAL_BUS && pa < PCAL_BUS + 0x100)
        return 1;
    if (pa == USI10_MODE || pa == GPP6 || pa == GPP6 + 8 || pa == GPG3 || pa == GPG3 + 4)
        return 1;
    for (unsigned i = 0; i < 4; i++)
        if (pa == CMU + gate_off[i])
            return 1;
    return pa == CMU + QCH_OFF;
}

static void wr(uint64_t pa, uint32_t v)
{
    if (!allowed(pa)) {
        struct btl l;
        btl_init(&l);
        btl_s(&l, "pcal: refused a write outside the expander's blocks at 0x");
        btl_x(&l, pa, 8);
        say(l.b);
        return;
    }
    io->wr32(pa, v);
}

static uint32_t pr(uint64_t pa, int *ok)
{
    uint32_t v = 0;
    if (!io->probe32(pa, &v)) {
        *ok = 0;
        return 0xdeadbeefU;
    }
    return v;
}

int pcal_look(const struct pcal_io *i, struct pcal_look *l)
{
    int ok = 1;
    io = i;
    l->mode = pr(USI10_MODE, &ok);
    l->gpp6_con = pr(GPP6, &ok);
    l->gpp6_pud = pr(GPP6 + 8, &ok);
    l->gpg3_con = pr(GPG3, &ok);
    l->gpg3_dat = pr(GPG3 + 4, &ok);
    l->gate_closed = 0;
    for (unsigned k = 0; k < 4; k++) {
        l->gate[k] = pr(CMU + gate_off[k], &ok);
        if ((l->gate[k] & (GATE_MANUAL | GATE_ON)) == GATE_MANUAL)
            l->gate_closed = 1;
    }
    l->qch = pr(CMU + QCH_OFF, &ok);
    l->div = pr(CMU + DIV_OFF, &ok);
    l->in_reset = ((l->gpg3_con >> (4 * RST_PIN)) & 0xfU) != 1U || !((l->gpg3_dat >> RST_PIN) & 1U);
    /* the controller only when its clocks are not forced off (a read could stop the bus) */
    l->ctl_read = 0;
    l->ctl = l->conf = l->usi_con = 0;
    if (!l->gate_closed) {
        int cok = 1;
        l->ctl = pr(PCAL_BUS + 0x00, &cok);
        l->conf = pr(PCAL_BUS + 0x40, &cok);
        l->usi_con = pr(PCAL_BUS + 0xC4, &cok);
        l->ctl_read = cok;
    }
    l->bus_ready = l->ctl_read && l->mode == 4U && ((l->gpp6_con >> 8) & 0xffU) == 0x33U && !(l->usi_con & 1U) &&
                   (l->ctl & 0x8U) && (l->conf & 0x80000000U);
    return ok;
}

void pcal_look_log(const struct pcal_look *l, const char *prefix)
{
    struct btl b;
    btl_init(&b);
    btl_s(&b, prefix);
    btl_s(&b, "expander bus: usi10 mode ");
    btl_x(&b, l->mode, 2);
    btl_s(&b, ", gpp6 con ");
    btl_x(&b, l->gpp6_con, 8);
    btl_s(&b, " pud ");
    btl_x(&b, l->gpp6_pud, 8);
    btl_s(&b, "; reset gpg3-6 function ");
    btl_x(&b, (l->gpg3_con >> (4 * RST_PIN)) & 0xfU, 1);
    btl_s(&b, " level ");
    btl_u(&b, (l->gpg3_dat >> RST_PIN) & 1U);
    btl_s(&b, l->in_reset ? " (held in reset)" : " (running)");
    btl_s(&b, "; CMU USI10_I2C gates ");
    for (unsigned k = 0; k < 4; k++) {
        btl_x(&b, l->gate[k], 8);
        btl_s(&b, k < 3 ? "/" : "");
    }
    btl_s(&b, " qch ");
    btl_x(&b, l->qch, 8);
    btl_s(&b, " div ");
    btl_x(&b, l->div, 8);
    if (l->ctl_read) {
        btl_s(&b, "; hsi2c ctl ");
        btl_x(&b, l->ctl, 8);
        btl_s(&b, " conf ");
        btl_x(&b, l->conf, 8);
        btl_s(&b, " usi_con ");
        btl_x(&b, l->usi_con, 2);
    } else {
        btl_s(&b, "; hsi2c not read (a gate is closed)");
    }
    say(b.b);
}

static int reg_rd(uint8_t reg, uint8_t *v)
{
    return io->i2c_rd(PCAL_ADDR, reg, v);
}

static int bit_set(uint8_t reg, unsigned bit, int on)
{
    uint8_t now = 0, back = 0;
    if (!reg_rd(reg, &now))
        return 0;
    uint8_t want = on ? (uint8_t)(now | 1U << bit) : (uint8_t)(now & ~(1U << bit));
    if (want != now && !io->i2c_wr(PCAL_ADDR, reg, want))
        return 0;
    return reg_rd(reg, &back) && back == want;
}

int pcal_open(const struct pcal_io *i, uint32_t allow_mask, int allow_cmu)
{
    struct pcal_look l;
    if (is_open)
        return PCAL_ESTATE;
    io = i;
    allow = allow_mask & 0xffffffU;
    for (unsigned k = 0; k < sizeof(was); k++)
        ((uint8_t *)&was)[k] = 0;
    if (!pcal_look(i, &l)) {
        why = "the expander's bus registers do not read";
        return PCAL_EGATE;
    }
    pcal_look_log(&l, "pcal: as found: ");
    was.mode = l.mode;
    was.gpp6_con = l.gpp6_con;
    was.gpp6_pud = l.gpp6_pud;
    was.gpg3_con = l.gpg3_con;
    was.gpg3_dat = l.gpg3_dat;
    if (l.gate_closed) {
        if (!allow_cmu) {
            why = "the expander's I2C clock gate is closed (not opened from here)";
            say("pcal: refused: a USI10_I2C gate is forced off; nothing written");
            return PCAL_EGATE;
        }
        for (unsigned k = 0; k < 4; k++) {
            was.gate[k] = l.gate[k];
            wr(CMU + gate_off[k], l.gate[k] | GATE_MANUAL | GATE_ON);
        }
        was.qch = l.qch;
        was.gates_set = 1;
        say("pcal: the USI10_I2C gates forced on (allowed by the caller); restored at close");
    }
    is_open = 1;
    int ok = 1;
    for (unsigned k = 0; k < NCTL; k++)
        was.ctl[k] = pr(PCAL_BUS + ctl_regs[k], &ok);
    was.usi_con = pr(PCAL_BUS + 0xC4, &ok);
    was.ctl_saved = ok;
    if (!ok) {
        why = "the expander's I2C controller does not read";
        pcal_close();
        return PCAL_EGATE;
    }
    if (l.mode != 4U) {
        wr(USI10_MODE, 4U);
        was.mode_set = 1;
    }
    if (((l.gpp6_con >> 8) & 0xffU) != 0x33U || ((l.gpp6_pud >> 8) & 0xffU)) {
        wr(GPP6 + 8, l.gpp6_pud & ~0xff00U);
        wr(GPP6, (l.gpp6_con & ~0xff00U) | 0x3300U);
        was.pins_set = 1;
    }
    uint32_t fs1 = was.ctl[6], fs2 = was.ctl[7], fs3 = was.ctl[8];
    if (!((fs3 >> 16) & 0xffU)) {           /* nothing from S-Boot: Android's 200 MHz / 400 kHz (as fled.c) */
        fs1 = 0x01F0FF00U;
        fs2 = 0x030003E0U;
        fs3 = 0x00210000U;
    }
    io->i2c_init(fs1, fs2, fs3);
    if (l.in_reset) {
        /* in reset every pin is high-impedance; out of it every pin is an input (the chip's default): the same */
        say("pcal: the expander was held in reset (gpg3-6): released (an output, high), 10 ms; put back at close");
        uint32_t d = io->rd32(GPG3 + 4), c = io->rd32(GPG3);
        wr(GPG3 + 4, d | 1U << RST_PIN);
        wr(GPG3, (c & ~(0xfU << (4 * RST_PIN))) | 1U << (4 * RST_PIN));
        was.reset_released = 1;
        io->delay_us(10000);
    }
    for (unsigned p = 0; p < 3; p++)
        if (!reg_rd((uint8_t)(PCAL_OUT0 + p), &was.out[p]) || !reg_rd((uint8_t)(PCAL_CFG0 + p), &was.cfg[p])) {
            why = "the GPIO expander does not answer on I2C (0x22)";
            say("pcal: no answer from 0x22");
            pcal_close();
            return PCAL_EBUS;
        }
    {
        uint8_t in[3];
        struct btl b;
        btl_init(&b);
        btl_s(&b, "pcal: ports: output ");
        for (unsigned p = 0; p < 3; p++)
            btl_x(&b, was.out[p], 2);
        btl_s(&b, ", direction ");
        for (unsigned p = 0; p < 3; p++)
            btl_x(&b, was.cfg[p], 2);
        if (reg_rd(PCAL_IN0, &in[0]) && reg_rd(PCAL_IN0 + 1, &in[1]) && reg_rd(PCAL_IN0 + 2, &in[2])) {
            btl_s(&b, ", input ");
            for (unsigned p = 0; p < 3; p++)
                btl_x(&b, in[p], 2);
        }
        say(b.b);
    }
    why = 0;
    return 0;
}

int pcal_ports(uint8_t in[3], uint8_t out[3], uint8_t cfg[3])
{
    if (!is_open)
        return PCAL_ESTATE;
    for (unsigned p = 0; p < 3; p++)
        if (!reg_rd((uint8_t)(PCAL_IN0 + p), &in[p]) || !reg_rd((uint8_t)(PCAL_OUT0 + p), &out[p]) ||
            !reg_rd((uint8_t)(PCAL_CFG0 + p), &cfg[p]))
            return PCAL_EBUS;
    return 0;
}

int pcal_pin(unsigned pin, int *is_out, int *drive, int *level)
{
    uint8_t c = 0, o = 0, in = 0;
    if (!is_open || pin > 23)
        return PCAL_ESTATE;
    uint8_t port = (uint8_t)(pin / 8), bit = (uint8_t)(pin % 8);
    if (!reg_rd((uint8_t)(PCAL_CFG0 + port), &c) || !reg_rd((uint8_t)(PCAL_OUT0 + port), &o) ||
        !reg_rd((uint8_t)(PCAL_IN0 + port), &in))
        return PCAL_EBUS;
    *is_out = !((c >> bit) & 1U);
    *drive = (o >> bit) & 1U;
    *level = (in >> bit) & 1U;
    return 0;
}

int pcal_pin_out(unsigned pin, int level)
{
    if (!is_open)
        return PCAL_ESTATE;
    if (pin > 23 || !((allow >> pin) & 1U))
        return PCAL_EPIN;
    uint8_t port = (uint8_t)(pin / 8), bit = (uint8_t)(pin % 8);
    if (!bit_set((uint8_t)(PCAL_OUT0 + port), bit, level) || !bit_set((uint8_t)(PCAL_CFG0 + port), bit, 0)) {
        why = "the GPIO expander did not take a pin (read back differs)";
        return PCAL_EREAD;
    }
    return 0;
}

int pcal_pin_restore(unsigned pin)
{
    if (!is_open)
        return PCAL_ESTATE;
    if (pin > 23 || !((allow >> pin) & 1U))
        return PCAL_EPIN;
    uint8_t port = (uint8_t)(pin / 8), bit = (uint8_t)(pin % 8);
    int out_was = (was.out[port] >> bit) & 1U, in_was = (was.cfg[port] >> bit) & 1U;
    /* driven low first; then the direction as found; then the output bit as found */
    int ok = bit_set((uint8_t)(PCAL_OUT0 + port), bit, 0) && bit_set((uint8_t)(PCAL_CFG0 + port), bit, in_was) &&
             bit_set((uint8_t)(PCAL_OUT0 + port), bit, out_was);
    return ok ? 0 : PCAL_EREAD;
}

int pcal_is_open(void)
{
    return is_open;
}

const char *pcal_why(void)
{
    return why ? why : "the expander is open";
}

int pcal_close(void)
{
    int same = 1, ok = 1;
    if (!is_open)
        return 1;
    if (was.reset_released) {
        uint32_t c = io->rd32(GPG3), d = io->rd32(GPG3 + 4);
        wr(GPG3 + 4, (d & ~(1U << RST_PIN)) | (was.gpg3_dat & 1U << RST_PIN));
        wr(GPG3, (c & ~(0xfU << (4 * RST_PIN))) | (was.gpg3_con & 0xfU << (4 * RST_PIN)));
    }
    if (was.ctl_saved) {
        /* the controller as found: its registers, then its USI reset bit */
        wr(PCAL_BUS + 0x20, 0);
        for (unsigned k = 0; k < NCTL; k++)
            wr(PCAL_BUS + ctl_regs[k], was.ctl[k]);
        wr(PCAL_BUS + 0xC4, was.usi_con);
    }
    if (was.pins_set) {
        uint32_t c = io->rd32(GPP6), p = io->rd32(GPP6 + 8);
        wr(GPP6, (c & ~0xff00U) | (was.gpp6_con & 0xff00U));
        wr(GPP6 + 8, (p & ~0xff00U) | (was.gpp6_pud & 0xff00U));
    }
    if (was.mode_set) {
        /* USI10's mode word is shared with the flash LED's bus (USI10 main, gpp6-0/1, kern/fled.c): left at I2C
         * when those pins are I2C's now */
        if ((io->rd32(GPP6) & 0xffU) == 0x22U)
            say("pcal: USI10's mode left at I2C: the flash LED's bus uses it now");
        else
            wr(USI10_MODE, was.mode);
    }
    if (was.gates_set) {
        wr(CMU + QCH_OFF, was.qch);
        for (unsigned k = 0; k < 4; k++)
            wr(CMU + gate_off[k], was.gate[k]);
    }
    /* read back */
    if (((io->rd32(GPG3) >> (4 * RST_PIN)) & 0xfU) != ((was.gpg3_con >> (4 * RST_PIN)) & 0xfU) ||
        ((io->rd32(GPG3 + 4) ^ was.gpg3_dat) & 1U << RST_PIN))
        same = 0, say("pcal: the reset pin gpg3-6 is not as found");
    if (((io->rd32(GPP6) ^ was.gpp6_con) & 0xff00U) || ((io->rd32(GPP6 + 8) ^ was.gpp6_pud) & 0xff00U))
        same = 0, say("pcal: the bus pins gpp6-2/3 are not as found");
    if (was.ctl_saved && (io->rd32(PCAL_BUS + 0xC4) & 1U) != (was.usi_con & 1U))
        same = 0, say("pcal: the controller's USI reset bit is not as found");
    if (was.gates_set)
        for (unsigned k = 0; k < 4; k++)
            if (io->rd32(CMU + gate_off[k]) != was.gate[k])
                same = 0, say("pcal: a USI10_I2C gate is not as found");
    (void)ok;
    is_open = 0;
    allow = 0;
    if (!why)
        why = "closed";
    return same;
}
