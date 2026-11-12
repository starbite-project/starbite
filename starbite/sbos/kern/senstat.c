/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS drivers track: small read-only status parts (docs/drivers/SENSORS.md section 3; shub_more.h).
 *
 * Wiring (the phone's own DT device/dtb/starlte.dts = Samsung's exynos9810-starlte_eur_open_26.dts; bank addresses
 * from pinctrl-exynos.c's exynos9810 tables; Utility OS research 2026-11-07 "utility-hw-research.md" 1, 4a):
 *   hall          flip cover: gpa3-6 (ALIVE 0x14050080, DAT +4 bit 6, EINT, pull-down; DT "hall-irq");
 *                 "certify" hall: gpa0-2 (ALIVE 0x14050020, DAT bit 2, EINT, no pull). The pin reads HIGH with no
 *                 magnet (seen on the phone by Utility OS 2, 2026-11-07); low = a magnet = the cover closed
 *                 (CONFIG_HALL_EVENT_REVERSE). DAT is read as is: no pin is reconfigured.
 *   P9320         idt,mfc-charger @ 0x3B on hsi2c@14370000 (I2C_CMGP03, "hsi2c41", standard mode 100 kHz in the
 *                 DT), pins gpm16-0 / gpm17-0 function 3 no pull (CMGP 0x142201C0 / 0x142201E0); wpc_det gpa2-0
 *                 (ALIVE 0x14050060 bit 0, pull-down; high = a pad's field [GUESS from Samsung's mfc driver's
 *                 attach logic]), wpc_int gpa3-1 (bit 1). Gates CMU_CMGP 0x14200000 +0x2044 IPCLK, +0x2048 PCLK,
 *                 +0x2060 RSTnSYNC (cmucal-sfr.c). Registers (Samsung GPL mfc_charger.h): CHIP_ID 0x00/0x01,
 *                 REV 0x02, STATUS 0x20/0x21, ADC_VOUT 0x3C/0x3D, ADC_VRECT 0x40/0x41, DIE_TEMP 0x46/0x47,
 *                 RX_OP_FREQ 0x48/0x49 (kHz). The chip is powered by the pad's coil: off a pad it does not answer.
 *                 Only reads, ever. Nobody else in Starbite OS uses this bus: sens_wpc_read brings the controller
 *                 and its two pins up for one look and puts them back as found.
 *   MAX86915      hrmsensor @ 0x57 on hsi2c_20 0x108E0000 (USI08 of PERIC1, gpp5-0/1 function 2). VDD 1.8 V =
 *                 S2MPB02 LDO8, I2C pull-ups = S2MPS18 LDO38 (L38CTRL 0x6F, on = bits 7:6 = 11, shared with NFC);
 *                 neither is boot-on. PART_ID 0xFF (0x2B expected, [GUESS from the MAX86916 family map]). This
 *                 driver writes no regulator: it reads the part ID only if LDO38 is on and the bus is already up.
 *   NFC / FP      recorded only (not driven): sec-nfc @ 0x27 on the same hsi2c_20 (VEN expander P1_3, FIRM P1_1,
 *                 PVDD LDO38, clock PMU CLKOUT0); the Egis ET510 fingerprint sensor on spi_1 0x10450000 is owned by
 *                 the TEE (secure-mode): never touched.
 * Seen on a phone: the hall pin's polarity (Utility OS 2). Not seen: everything else here. */
#include "sb.h"
#include "shub_more.h"

#define GPA0 0x14050020UL
#define GPA1 0x14050040UL
#define GPA2 0x14050060UL
#define GPA3 0x14050080UL
#define GPA4 0x140500C0UL
#define GPG0 0x10430080UL
#define GPG1 0x104300A0UL
#define GPP3 0x10430060UL
#define GPP5 0x10830020UL
#define GPH0 0x13A80000UL
#define PMU_CHUB 0x14064084UL
#define GPM16 0x142201C0UL
#define GPM17 0x142201E0UL
#define WPC_BUS 0x14370000UL
#define WPC_ADDR 0x3BU
#define HRM_BUS 0x108E0000UL
#define HRM_ADDR 0x57U

const char *senstat_why = "not checked";

#ifdef SB_HOST
uint32_t senstat_sim_rd(uintptr_t a, int *ok);
void senstat_sim_wr(uintptr_t a, uint32_t v);
static int rd(uintptr_t a, uint32_t *v)
{
    int ok = 1;
    *v = senstat_sim_rd(a, &ok);
    return ok;
}
static void wr(uintptr_t a, uint32_t v) { senstat_sim_wr(a, v); }
#else
static int rd(uintptr_t a, uint32_t *v)
{
    return sb_probe32(a, v);
}
static void wr(uintptr_t a, uint32_t v)
{
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)a = v;
    __asm__ volatile("dsb sy" ::: "memory");
}
#endif

static int pin(uintptr_t bank, unsigned n)
{
    uint32_t v = 0;
    if (!rd(bank + 4, &v))
        return -1;
    return (int)((v >> n) & 1U);
}

/* ---- hall ----------------------------------------------------------------------------------------------------- */
int sens_hall_closed(void)
{
    int p = pin(GPA3, 6);
    return p < 0 ? -1 : !p;
}

int sens_hall_certify(void)
{
    int p = pin(GPA0, 2);
    return p < 0 ? -1 : !p;
}

/* ---- the wireless charger ------------------------------------------------------------------------------------- */
int sens_wpc_pad(void)
{
    return pin(GPA2, 0);
}

/* a CMU gate is held closed: MANUAL (bit 20) set and CG_VAL (bit 21) clear */
static int gates_open(const uintptr_t *g, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        uint32_t v = 0;
        if (!rd(g[i], &v) || (v & 0x00300000U) == 0x00100000U)
            return 0;
    }
    return 1;
}

static const uintptr_t wpc_gates[3] = { 0x14202044UL, 0x14202048UL, 0x14202060UL };
static const unsigned ctl_regs[] = { 0x00, 0x04, 0x08, 0x20, 0x40, 0x44, 0x48, 0x60, 0x64, 0x68, 0x70, 0xc4 };
#define NCTL (sizeof(ctl_regs) / sizeof(ctl_regs[0]))

static int rd16(struct sb_i2c *b, unsigned addr, uint8_t reg, uint16_t *v)
{
    uint8_t d[2] = { 0, 0 };
    if (!sb_i2c_read(b, addr, reg, d, 2))
        return 0;
    *v = (uint16_t)(d[0] | d[1] << 8);
    return 1;
}

int sens_wpc_read(struct sens_wpc *w)
{
    memset(w, 0, sizeof(*w));
    uint32_t c16 = 0, c17 = 0, p16 = 0, p17 = 0, ctl[NCTL];
    if (!gates_open(wpc_gates, 3)) {
        senstat_why = "the wireless charger's I2C clock gate is closed (not opened by this driver)";
        sb_log("senstat: wpc: I2C_CMGP03's clock gate is closed: not used");
        return -1;
    }
    int ok = rd(GPM16, &c16) && rd(GPM17, &c17) && rd(GPM16 + 8, &p16) && rd(GPM17 + 8, &p17);
    for (unsigned i = 0; ok && i < NCTL; ++i)
        ok = rd(WPC_BUS + ctl_regs[i], &ctl[i]);
    if (!ok) {
        senstat_why = "the wireless charger's I2C controller does not read";
        sb_log("senstat: wpc: the controller or its pins do not read: not used");
        return -1;
    }
    /* the controller: S-Boot's timing when it left one, else Android's 200 MHz / 400 kHz one (fled.c's); the pins:
     * function 3, no pull (the DT's hsi2c41-bus) */
    struct sb_i2c bus = { WPC_BUS, ctl[7], ctl[8], ctl[9], 0 };
    if (!((bus.fs3 >> 16) & 0xffU)) {
        bus.fs1 = 0x01F0FF00U;
        bus.fs2 = 0x030003E0U;
        bus.fs3 = 0x00210000U;
    }
    wr(GPM16, (c16 & ~0xfU) | 3U);
    wr(GPM17, (c17 & ~0xfU) | 3U);
    wr(GPM16 + 8, p16 & ~0xfU);
    wr(GPM17 + 8, p17 & ~0xfU);
    sb_i2c_bus(&bus);
    uint8_t id[3] = { 0, 0, 0 };
    int r = 0;
    if (sb_i2c_read(&bus, WPC_ADDR, 0x00, id, 3)) {
        uint16_t t = 0;
        w->answered = 1;
        w->chip_id = (uint16_t)(id[0] | id[1] << 8);
        w->rev = id[2];
        r = 1;
        if (w->chip_id == 0x9320U) {
            (void)rd16(&bus, WPC_ADDR, 0x20, &w->status);
            if (rd16(&bus, WPC_ADDR, 0x3C, &t))
                w->vout_raw = t & 0xfffU;
            if (rd16(&bus, WPC_ADDR, 0x40, &t))
                w->vrect_raw = t & 0xfffU;
            if (rd16(&bus, WPC_ADDR, 0x46, &t))
                w->die_c = (int)(t & 0xfffU);
            (void)rd16(&bus, WPC_ADDR, 0x48, &w->freq_khz);
            w->vout_mv = (int)((uint32_t)w->vout_raw * 12600U / 4095U);
        }
    }
    /* back as found: the controller reset and its registers written back, then the pins */
    wr(WPC_BUS + 0x00, ctl[0] | 0x80000000U);
    wr(WPC_BUS + 0x00, ctl[0] & ~0x80000000U);
    for (unsigned i = 1; i < NCTL; ++i)
        if (ctl_regs[i] != 0x24U)
            wr(WPC_BUS + ctl_regs[i], ctl[i]);
    wr(WPC_BUS + 0x00, ctl[0]);
    wr(GPM16 + 8, p16);
    wr(GPM17 + 8, p17);
    wr(GPM16, c16);
    wr(GPM17, c17);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "senstat: wpc: ");
    if (!r) {
        ub_s(&u, "no answer at 0x3B (off a pad the P9320 is unpowered: normal); pad pin ");
        ub_sdec(&u, sens_wpc_pad());
        senstat_why = "the wireless charger does not answer (no pad)";
    } else {
        ub_s(&u, "chip ");
        ub_hex(&u, w->chip_id, 4);
        ub_s(&u, " rev ");
        ub_hex(&u, w->rev, 2);
        if (w->chip_id == 0x9320U) {
            ub_s(&u, ", status ");
            ub_hex(&u, w->status, 4);
            ub_s(&u, ", Vout raw ");
            ub_dec(&u, w->vout_raw);
            ub_s(&u, " (about ");
            ub_dec(&u, (uint64_t)w->vout_mv);
            ub_s(&u, " mV), Vrect raw ");
            ub_dec(&u, w->vrect_raw);
            ub_s(&u, ", ");
            ub_dec(&u, w->freq_khz);
            ub_s(&u, " kHz");
        } else {
            ub_s(&u, " (not the P9320's 0x9320: nothing more read)");
        }
        senstat_why = 0;
    }
    ub_s(&u, "; pins and controller back as found");
    sb_log(u.b);
    return r;
}

/* ---- the heart-rate sensor ------------------------------------------------------------------------------------ */
int sens_hrm_rails(uint8_t *ldo38)
{
    uint8_t v = 0;
    if (!sb_pmic_read(1, 0x6F, &v) && !sb_pmic_read(1, 0x6F, &v))
        return -1;
    if (ldo38)
        *ldo38 = v;
    return (v & 0xC0U) == 0xC0U;
}

int sens_hrm_part_id(uint8_t *id)
{
    static const uintptr_t g[2] = { 0x108020F4UL, 0x108020F8UL };    /* CMU_PERIC1 USI08 IPCLK, PCLK */
    uint32_t ctl = 0, fs1 = 0, fs2 = 0, fs3 = 0, con = 0;
    if (sens_hrm_rails(0) != 1) {
        senstat_why = "the heart-rate sensor's rails are off (LDO38; this driver writes no regulator)";
        return 0;
    }
    if (!gates_open(g, 2) || !rd(HRM_BUS, &ctl) || !rd(HRM_BUS + 0x60, &fs1) || !rd(HRM_BUS + 0x64, &fs2) ||
        !rd(HRM_BUS + 0x68, &fs3) || !rd(GPP5, &con)) {
        senstat_why = "the heart-rate sensor's I2C bus is not readable (gate closed)";
        return 0;
    }
    if (!(ctl & 0x8U) || (con & 0xffU) != 0x22U || !((fs3 >> 16) & 0xffU)) {
        senstat_why = "the heart-rate sensor's I2C bus is not set up (not brought up by this driver)";
        return 0;
    }
    struct sb_i2c bus = { HRM_BUS, fs1, fs2, fs3, 1 };
    if (!sb_i2c_read(&bus, HRM_ADDR, 0xFF, id, 1)) {
        senstat_why = "the heart-rate sensor does not answer at 0x57";
        return 0;
    }
    return 1;
}

/* ---- the probe's level 0 (shub_more.c drv_sens_probe): everything above and the hub's pins, read only ------------ */
static void bank_line(struct ub *u, const char *name, uintptr_t b)
{
    uint32_t c = 0, d = 0, p = 0;
    ub_s(u, name);
    if (!rd(b, &c) || !rd(b + 4, &d) || !rd(b + 8, &p)) {
        ub_s(u, " unreadable; ");
        return;
    }
    ub_s(u, " CON ");
    ub_hexn(u, c, 8);
    ub_s(u, " DAT ");
    ub_hexn(u, d, 2);
    ub_s(u, " PUD ");
    ub_hexn(u, p, 8);
    ub_s(u, "; ");
}

/* logs the read-only state; 0 something did not read (why in senstat_why), 1 all read; *sum a short summary */
int senstat_look(const char **sum)
{
    static char s[120];
    struct ub u;
    int ok = 1;
    uint32_t pd = 0;
    uint8_t l37 = 0, l38 = 0;
    const char *bad = "?";
    ub_init(&u);
    ub_s(&u, "drvprobe: sens: hub pins: PMU CHUB ");
    if (!rd(PMU_CHUB, &pd)) {
        ub_s(&u, "unreadable");
        ok = 0;
        bad = "the PMU's CHUB status";
    } else {
        ub_hexn(&u, pd, 8);
        ub_s(&u, (pd & 0xfU) == 0xfU ? " (on); " : " (OFF: gph0 not read); ");
        if ((pd & 0xfU) == 0xfU)
            bank_line(&u, "gph0", GPH0);
    }
    ub_s(&u, "HOST_REQ gpa1-7 ");
    ub_sdec(&u, pin(GPA1, 7));
    ub_s(&u, ", AP_SLEEP gpa0-5 ");
    ub_sdec(&u, pin(GPA0, 5));
    ub_s(&u, ", NSTDBY gpg0-2 ");
    ub_sdec(&u, pin(GPG0, 2));
    ub_s(&u, ", SHUB_INT gpa4-0 ");
    ub_sdec(&u, pin(GPA4, 0));
    ub_s(&u, ", LDO37 ");
    if (sb_pmic_read(1, 0x6E, &l37))
        ub_hex(&u, l37, 2);
    else
        ub_s(&u, "unreadable");
    sb_log(u.b);

    int hc = sens_hall_closed(), hk = sens_hall_certify(), pad = sens_wpc_pad(), wint = pin(GPA3, 1);
    ub_init(&u);
    ub_s(&u, "drvprobe: sens: hall (gpa3-6) ");
    ub_s(&u, hc < 0 ? "unreadable" : hc ? "a magnet: cover closed" : "no magnet");
    ub_s(&u, ", certify hall (gpa0-2) ");
    ub_s(&u, hk < 0 ? "unreadable" : hk ? "a magnet" : "no magnet");
    ub_s(&u, "; wireless pad pin wpc_det gpa2-0 ");
    ub_sdec(&u, pad);
    ub_s(&u, ", wpc_int gpa3-1 ");
    ub_sdec(&u, wint);
    ub_s(&u, "; ");
    bank_line(&u, "gpa3", GPA3);
    sb_log(u.b);
    if (hc < 0 || hk < 0 || pad < 0) {
        ok = 0;
        bad = "a hall or wireless-pad pin";
    }

    uint32_t wctl = 0, wfs3 = 0, c16 = 0, c17 = 0;
    ub_init(&u);
    ub_s(&u, "drvprobe: sens: P9320 bus 0x14370000: ");
    if (!gates_open(wpc_gates, 3)) {
        ub_s(&u, "clock gate closed; ID not read");
    } else if (!rd(WPC_BUS, &wctl) || !rd(WPC_BUS + 0x68, &wfs3) || !rd(GPM16, &c16) || !rd(GPM17, &c17)) {
        ub_s(&u, "unreadable");
        ok = 0;
        bad = "the wireless charger's I2C controller";
    } else {
        ub_s(&u, "CTL ");
        ub_hexn(&u, wctl, 8);
        ub_s(&u, " FS3 ");
        ub_hexn(&u, wfs3, 8);
        ub_s(&u, " gpm16 CON ");
        ub_hexn(&u, c16, 8);
        ub_s(&u, " gpm17 CON ");
        ub_hexn(&u, c17, 8);
        int up = (wctl & 0x8U) && ((wfs3 >> 16) & 0xffU) && (c16 & 0xfU) == 3U && (c17 & 0xfU) == 3U;
        if (!up) {
            ub_s(&u, ": not set up by anyone, ID not read at level 0 (level 1 reads it)");
        } else {
            struct sb_i2c bus = { WPC_BUS, 0, 0, wfs3, 1 };
            uint8_t id[2] = { 0, 0 };
            (void)rd(WPC_BUS + 0x60, &bus.fs1);
            (void)rd(WPC_BUS + 0x64, &bus.fs2);
            if (sb_i2c_read(&bus, WPC_ADDR, 0x00, id, 2)) {
                ub_s(&u, ": set up; chip ID ");
                ub_hex(&u, (unsigned)(id[0] | id[1] << 8), 4);
            } else {
                ub_s(&u, ": set up; no answer (no pad: normal)");
            }
        }
    }
    sb_log(u.b);

    int rails = sens_hrm_rails(&l38);
    uint8_t hid = 0;
    ub_init(&u);
    ub_s(&u, "drvprobe: sens: MAX86915: LDO38 ");
    if (rails < 0)
        ub_s(&u, "unreadable");
    else
        ub_hex(&u, l38, 2);
    ub_s(&u, rails == 1 ? " (on)" : " (off)");
    if (sens_hrm_part_id(&hid)) {
        ub_s(&u, "; part ID ");
        ub_hex(&u, hid, 2);
        ub_s(&u, hid == 0x2BU ? " (MAX86915)" : " (NOT 0x2B)");
    } else {
        ub_s(&u, "; part ID not read: ");
        ub_s(&u, senstat_why);
    }
    ub_s(&u, "; int gpp3-0 ");
    ub_sdec(&u, pin(GPP3, 0));
    ub_s(&u, ", LED boost gpg1-6 ");
    ub_sdec(&u, pin(GPG1, 6));
    sb_log(u.b);
    sb_log("drvprobe: sens: NFC (sec-nfc 0x27 on hsi2c_20, LDO38 + expander VEN/FIRM) and the ET510 fingerprint sensor "
           "(TEE-owned spi_1): recorded only, not touched");

    ub_init(&u);
    ub_s(&u, hc < 0 ? "hall ?" : hc ? "cover closed" : "cover open");
    ub_s(&u, pad == 1 ? ", on a wireless pad" : pad == 0 ? ", no wireless pad" : ", pad ?");
    ub_s(&u, (pd & 0xfU) == 0xfU ? ", CHUB on" : ", CHUB off");
    ub_s(&u, rails == 1 ? ", HRM rails on" : ", HRM rails off");
    unsigned i = 0;
    for (; u.b[i] && i < sizeof(s) - 1; ++i)
        s[i] = u.b[i];
    s[i] = 0;
    if (sum)
        *sum = s;
    if (!ok)
        senstat_why = bad;
    return ok;
}
