/* SPDX-License-Identifier: GPL-2.0-only
 * bcmw: PCIe channel 0 of the Exynos 9810 (a DesignWare root complex with Samsung's PHY) brought up
 * by hand as far as the BCM4361's BARs, the chip powered and held by its pins, and taken down again.
 * Steps 1 to 6 of bcmw.h, each said before it is done.
 *
 * Where the facts come from (read, not copied; register values are the hardware's):
 *  - Samsung's GPL kernel for the S9 (a public exynos9810 kernel tree;
 *    our ref/starlte-kernel): drivers/pci/host/pci-exynos.c (exynos_pcie_poweron, _establish_link,
 *    _setup_rc, own/other conf, viewports, the interrupts), pci-exynos.h, pci-exynos9810_cal.c
 *    (exynos_pcie_phy_config and _all_pwrdn_clear: channel 0's PHY tables, used unchanged here),
 *    bcmdhd's dhd_custom_exynos.c (WLAN_REG_ON, WIFI_TURNON_DELAY 200 ms).
 *  - an earlier port drivers/wifi/exynos_pcie.c (APSL 2.0, 4 Nov 2026), the same sequence on
 *    this phone, run to a joined WPA2 network: its step numbering, the gates it reads, the data
 *    path's gates and the SysMMU lesson (its registers must not be touched; its clock must be on).
 *  - The mainline port's pcie-exynos9810.c (GPL-2.0, SheroAbi/galaxy-s9plus-mainline-linux
 *    bcee6014fc2e, Linux 7.1), the same on Linux: WLAN_REG_ON low before high after a warm restart,
 *    the PMU's PHY isolation before any CMU_FSYS1 gate is touched, the INTx enables.
 */
#include "bcmw_priv.h"

/* PMU */
#define PMU_PCIE_PHY_CONTROL    0x71C   /* bit 0: the PHY out of isolation */

/* ELBI: Samsung's glue */
#define ELBI_IRQ_PULSE          0x000   /* latch, INTA..INTD at bits 0, 2, 4, 6; write back to clear */
#define ELBI_IRQ_LEVEL          0x004
#define ELBI_IRQ_SPECIAL        0x008
#define ELBI_IRQ_EN_PULSE       0x00C
#define ELBI_IRQ_EN_LEVEL       0x010
#define ELBI_IRQ_EN_SPECIAL     0x014
#define ELBI_SW_WAKE            0x018   /* bit 1 clear: the bus number enabled */
#define ELBI_APP_LTSSM_ENABLE   0x02C
#define ELBI_IRQ_TOE_EN_PULSE2  0x034
#define ELBI_IRQ_TOE_EN_LEVEL   0x038
#define ELBI_RDLH_LINKUP        0x074   /* [4:0] LTSSM state: 0x0D..0x14 a link, 0x11 L0 */
#define ELBI_PM_DSTATE          0x088
#define ELBI_APP_REQ_EXIT_L1    0x0F4
#define ELBI_HISTORY(i)         (0x138 + 4 * (i))
#define ELBI_LINKDOWN_RST_SEL   0x1B8
#define ELBI_SOFT_CORE_RESET    0x1D0
#define ELBI_HISTORY_CHECK      0x274
#define ELBI_PCS_G_RST          0x288
#define ELBI_PHY_RST            0x28C
#define ELBI_MAC_RST            0x290
#define ELBI_STATE_POWER_S      0x2BC
#define ELBI_STATE_POWER_M      0x2C0
#define ELBI_QCH_SEL            0x2C8
#define ELBI_L12ERR_CTRL        0x2F0   /* bit 2: NCLK off, the guard against access while unlinked */
#define INTX_ASSERTS            0x55u

/* DBI: the root port's own configuration space and port logic */
#define DBI_ID                  0x000
#define DBI_COMMAND             0x004
#define DBI_CLASS               0x008
#define DBI_BAR0                0x010
#define DBI_BAR1                0x014
#define DBI_BUS                 0x018
#define DBI_MEM_WINDOW          0x020
#define DBI_INTERRUPT           0x03C
#define DBI_PM_CTRL             0x044
#define DBI_LNKCAP              0x07C
#define DBI_LNKCTL              0x080
#define DBI_LNKCTL2             0x0A0
#define DBI_PORT_LINK_CONTROL   0x710
#define DBI_LINK_WIDTH_SPEED    0x80C
#define DBI_MISC_CONTROL        0x8BC
#define DBI_ATU_VIEWPORT        0x900
#define DBI_ATU_CR1             0x904
#define DBI_ATU_CR2             0x908
#define DBI_ATU_LOWER_BASE      0x90C
#define DBI_ATU_UPPER_BASE      0x910
#define DBI_ATU_LIMIT           0x914
#define DBI_ATU_LOWER_TARGET    0x918
#define DBI_ATU_UPPER_TARGET    0x91C
#define DBI_AUX_CLK_FREQ        0xB40
#define DBI_L1_SUBSTATES        0xB44
#define ATU_ENABLE              (1u << 31)
#define ATU_TYPE_MEM            0
#define ATU_TYPE_CFG0           4

/* CMU: a gate is open while hardware runs it (MANUAL clear) or forced on (MANUAL and CG_VAL) */
#define GATE_MANUAL             (1u << 20)
#define GATE_CG_VAL             (1u << 21)
static const struct { uint16_t off; const char *name; } gates[] = {
    { 0x2000, "PHY_REFCLK_IN" }, { 0x2038, "DBI_ACLK" }, { 0x203C, "PHY_APB" }, { 0x2040, "MSTR_ACLK" },
    { 0x2044, "SUB_CTRL_APB" }, { 0x2048, "PCS_APB" }, { 0x204C, "SLV_ACLK" }, { 0x20B0, "XIU_DBI" },
    { 0x20B4, "XIU_SLV" },
};
#define CMU_TOP_GATE_FSYS1_PCIE 0x2074  /* CMU_TOP +0x2074: GATE_CLKCMU_FSYS1_PCIE */
/* the radio's way to memory: master port, XIU, the PCIe SysMMU (in bypass, but clocked), PPMU,
 * BTM, the ACE-lite bridge, SYSREG (an earlier port step 6, wifi-a4..a6) */
static const struct { uint16_t off; const char *name; } dma_path[] = {
    { 0x2040, "PCIE_GEN2 MSTR_ACLK" }, { 0x20ac, "XIU_D_FSYS1" }, { 0x2098, "SYSMMU_FSYS1" },
    { 0x2078, "PPMU_FSYS1" }, { 0x2018, "BTM_FSYS1" }, { 0x202c, "LHS_ACEL_D_FSYS1" }, { 0x209c, "SYSREG_FSYS1" },
};
#define GATE_SYSMMU 0x2098

/* GPIO banks: CON 4 bits a pin at +0, DAT 1 bit at +4, PUD 4 bits at +8 */
#define BANK_GPG0   (EX_GPIO_PERIC0 + 0x080)
#define BANK_GPF2   (EX_GPIO_FSYS1 + 0x020)
#define BANK_GPA0   (EX_GPIO_ALIVE + 0x020)
#define BANK_GPA3   (EX_GPIO_ALIVE + 0x080)
#define PIN_WLAN_EN 5   /* gpg0-5: WLAN_REG_ON, the radio's power, high = on */
#define PIN_PERST   2   /* gpf2-2: PERST#, high = out of reset */
#define PIN_CLKREQ  0   /* gpf2-0: CLKREQ#, function 2, pulled up */
#define PIN_HOST_WAKE 7 /* gpa0-7, read only */
#define PIN_PCIE_WAKE 2 /* gpa3-2, read only */

#define WIFI_TURNON_MS 200
#define LINK_POLLS 2000 /* of 10 us */

/* ---- the allow-list ---------------------------------------------------------------------------- */

static const uint64_t allowed_words[] = {
    EX_PMU + PMU_PCIE_PHY_CONTROL,
    EX_SYSREG + 0x1044, EX_SYSREG + 0x1050, EX_SYSREG + 0x700,
    EX_CMU_TOP + CMU_TOP_GATE_FSYS1_PCIE,
    EX_CMU_FSYS1 + 0x2000, EX_CMU_FSYS1 + 0x2038, EX_CMU_FSYS1 + 0x203C, EX_CMU_FSYS1 + 0x2040,
    EX_CMU_FSYS1 + 0x2044, EX_CMU_FSYS1 + 0x2048, EX_CMU_FSYS1 + 0x204C, EX_CMU_FSYS1 + 0x20B0,
    EX_CMU_FSYS1 + 0x20B4, EX_CMU_FSYS1 + 0x20ac, EX_CMU_FSYS1 + 0x2098, EX_CMU_FSYS1 + 0x2078,
    EX_CMU_FSYS1 + 0x2018, EX_CMU_FSYS1 + 0x202c, EX_CMU_FSYS1 + 0x209c,
    BANK_GPG0, BANK_GPG0 + 4,
    BANK_GPF2, BANK_GPF2 + 4, BANK_GPF2 + 8,
};

int bcmw_write_allowed(uint64_t pa)
{
    if (pa & 3)
        return 0;
    if ((pa >= EX_ELBI && pa < EX_ELBI + 0x1000) || (pa >= EX_PCS && pa < EX_PCS + 0x1000) ||
        (pa >= EX_PHY && pa < EX_PHY + 0x1000) || (pa >= EX_DBI && pa < EX_DBI + 0x1000) ||
        (pa >= EX_CFG && pa < EX_CFG + EX_CFG_SIZE) || (pa >= EX_MEM && pa < EX_MEM + EX_MEM_SIZE))
        return 1;
    for (unsigned i = 0; i < sizeof(allowed_words) / sizeof(allowed_words[0]); i++)
        if (pa == allowed_words[i])
            return 1;
    return 0;
}

/* ---- pins ----------------------------------------------------------------------------------------- */

static unsigned pin_con(struct bcmw *w, uint64_t bank, unsigned pin) { return (rd32(w, bank) >> (4 * pin)) & 0xf; }
static unsigned pin_dat(struct bcmw *w, uint64_t bank, unsigned pin) { return (rd32(w, bank + 4) >> pin) & 1; }
static unsigned pin_pud(struct bcmw *w, uint64_t bank, unsigned pin) { return (rd32(w, bank + 8) >> (4 * pin)) & 0xf; }

/* an output, driven: the level first, then the function (gpio_direction_output's order) */
static void pin_out(struct bcmw *w, uint64_t bank, unsigned pin, unsigned level)
{
    set32(w, bank + 4, 1u << pin, (uint32_t)level << pin);
    set32(w, bank, 0xfu << (4 * pin), 1u << (4 * pin));
}

static void say_pins(struct bcmw *w, const char *when)
{
    struct bcmw_lb l;
    lb_init(&l);
    lb_s(&l, "pcie: ");
    lb_s(&l, when);
    lb_s(&l, ": WLAN_REG_ON con ");
    lb_hex(&l, pin_con(w, BANK_GPG0, PIN_WLAN_EN), 1);
    lb_s(&l, " dat ");
    lb_dec(&l, pin_dat(w, BANK_GPG0, PIN_WLAN_EN));
    lb_s(&l, "; PERST# con ");
    lb_hex(&l, pin_con(w, BANK_GPF2, PIN_PERST), 1);
    lb_s(&l, " dat ");
    lb_dec(&l, pin_dat(w, BANK_GPF2, PIN_PERST));
    lb_s(&l, "; CLKREQ# con ");
    lb_hex(&l, pin_con(w, BANK_GPF2, PIN_CLKREQ), 1);
    lb_s(&l, " pud ");
    lb_hex(&l, pin_pud(w, BANK_GPF2, PIN_CLKREQ), 1);
    lb_s(&l, "; HOST_WAKE ");
    lb_dec(&l, pin_dat(w, BANK_GPA0, PIN_HOST_WAKE));
    lb_s(&l, "; PCIE_WAKE ");
    lb_dec(&l, pin_dat(w, BANK_GPA3, PIN_PCIE_WAKE));
    bcmw_say_lb(w, &l);
}

/* ---- the PHY (pci-exynos9810_cal.c, channel 0, 26 MHz reference, Gen2; Samsung's tables) --------- */

static const uint8_t phy_cmn[48] = {
    0x01, 0xE1, 0x05, 0x00, 0x88, 0x88, 0x88, 0x0C, 0x61, 0x45, 0x65, 0x24, 0x33, 0x18, 0xE3, 0xFC,
    0xD8, 0x05, 0xE6, 0x80, 0x00, 0x00, 0x00, 0x00, 0x60, 0x11, 0x00, 0xA0, 0x05, 0x04, 0x18, 0x88,
    0xC0, 0xFF, 0x9B, 0x52, 0x22, 0x30, 0x4F, 0xDC, 0x40, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x80,
};
static const uint8_t phy_trsv[80] = {
    0x31, 0x40, 0x37, 0x99, 0x85, 0x00, 0xC0, 0xFF, 0xFF, 0x3F, 0x8C, 0xC8, 0x02, 0x01, 0x88, 0x80,
    0x06, 0x90, 0x6C, 0x66, 0x09, 0x61, 0x42, 0x44, 0xC6, 0x50, 0x0A, 0x33, 0x58, 0xE7, 0x20, 0x22,
    0x80, 0x38, 0x05, 0x85, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x55, 0x15, 0xAC, 0xAA, 0x3E, 0x00,
    0x00, 0x00, 0x20, 0x3F, 0x00, 0x03, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x85, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static void rx_elecidle_ignore(struct bcmw *w, int ignore)
{
    set32(w, EX_PCS + 0xEC, 1u << 3, ignore ? 1u << 3 : 0);
}

static void pulse(struct bcmw *w, uint32_t off, uint32_t last_us)
{
    wr32(w, EX_ELBI + off, 1);
    bcmw_udelay(w, 10);
    wr32(w, EX_ELBI + off, 0);
    bcmw_udelay(w, 10);
    wr32(w, EX_ELBI + off, 1);
    bcmw_udelay(w, last_us);
}

static void phy_config(struct bcmw *w)
{
    uint32_t v;

    set32(w, EX_SYSREG + 0x1044, 1u << 1, 1u << 1);                     /* the PCS's high speed */
    set32(w, EX_SYSREG + 0x1050, (0xfu << 4) | (0xfu << 2) | (1u << 1), 0x3u << 2);
    set32(w, EX_SYSREG + 0x1050, 1u << 12, 1u << 12);                   /* lane 0 */
    pulse(w, ELBI_PCS_G_RST, 10);
    for (unsigned i = 0; i < sizeof(phy_cmn); i++)
        wr32(w, EX_PHY + 4 * i, phy_cmn[i]);
    for (unsigned i = 0; i < sizeof(phy_trsv); i++)
        wr32(w, EX_PHY + 4 * (0x30 + i), phy_trsv[i]);
    wr32(w, EX_PCS + 0xF8, 0x70);       /* tx latency */
    wr32(w, EX_PCS + 0x100, 0x87);      /* the PCS's reference clock out */
    wr32(w, EX_PCS + 0x104, 0x50);
    v = rd32(w, EX_PCS + 0x0C);         /* PRGM_TIMEOUT_L1SS_VAL */
    wr32(w, EX_PCS + 0x0C, (v & ~(1u << 1)) | (1u << 4));
    pulse(w, ELBI_MAC_RST, 10);
    pulse(w, ELBI_PHY_RST, 100);
    v = rd32(w, EX_PCS + 0xD0) | (0x3u << 6);     /* the CDR's reset: 3, 20 us, 2, 0 */
    wr32(w, EX_PCS + 0xD0, v);
    bcmw_udelay(w, 20);
    v = (rd32(w, EX_PCS + 0xD0) & ~(0x3u << 6)) | (0x2u << 6);
    wr32(w, EX_PCS + 0xD0, v);
    wr32(w, EX_PCS + 0xD0, v & ~(0x3u << 6));
}

/* ---- DBI: guarded while there is no link (exynos_pcie_rd/_wr_own_conf) --------------------------- */

static void dbi_open(struct bcmw *w)
{
    if (!w->linked) {
        wr32(w, EX_ELBI + ELBI_L12ERR_CTRL, 0);
        rx_elecidle_ignore(w, 1);
    }
}

static void dbi_close(struct bcmw *w)
{
    if (!w->linked) {
        rx_elecidle_ignore(w, 0);
        wr32(w, EX_ELBI + ELBI_L12ERR_CTRL, 1u << 2);
    }
}

static uint32_t dbi_rd(struct bcmw *w, uint32_t off)
{
    uint32_t v;
    dbi_open(w);
    v = rd32(w, EX_DBI + off);
    dbi_close(w);
    return v;
}

static void dbi_wr(struct bcmw *w, uint32_t off, uint32_t v)
{
    dbi_open(w);
    wr32(w, EX_DBI + off, v);
    dbi_close(w);
}

static void dbi_set(struct bcmw *w, uint32_t off, uint32_t mask, uint32_t v)
{
    dbi_wr(w, off, (dbi_rd(w, off) & ~mask) | (v & mask));
}

static void atu_outbound(struct bcmw *w, uint32_t index, uint32_t type, uint64_t cpu, uint64_t size, uint64_t target)
{
    dbi_wr(w, DBI_ATU_VIEWPORT, index);
    dbi_wr(w, DBI_ATU_CR1, type);
    dbi_wr(w, DBI_ATU_LOWER_BASE, (uint32_t)cpu);
    dbi_wr(w, DBI_ATU_UPPER_BASE, (uint32_t)(cpu >> 32));
    dbi_wr(w, DBI_ATU_LIMIT, (uint32_t)(cpu + size - 1));
    dbi_wr(w, DBI_ATU_LOWER_TARGET, (uint32_t)target);
    dbi_wr(w, DBI_ATU_UPPER_TARGET, (uint32_t)(target >> 32));
    dbi_wr(w, DBI_ATU_CR2, ATU_ENABLE);
}

/* ---- step 1: a look ------------------------------------------------------------------------------- */

static int gate_closed(uint32_t v)
{
    return (v & GATE_MANUAL) && !(v & GATE_CG_VAL);
}

/* reads a register whose block may be unclocked: through the OS's fault-catching read when it
 * has one. 0 if it faulted. */
static int careful_rd(struct bcmw *w, uint64_t pa, uint32_t *v)
{
    if (w->os.probe32)
        return w->os.probe32(w->os.ctx, pa, v);
    *v = rd32(w, pa);
    return 1;
}

int ex_look(struct bcmw *w)
{
    uint32_t pmu = rd32(w, EX_PMU + PMU_PCIE_PHY_CONTROL), v;
    int open = 1;

    bcmw_sayx(w, "pcie: 1: PMU PCIE_PHY_CONTROL ", pmu, " (bit 0: the PHY out of isolation)");
    say_pins(w, "1");
    if (!(pmu & 1)) {
        /* the mainline port measured a synchronous external abort on a CMU_FSYS1 PCIe register
         * while the PHY's domain was isolated: the gates are read in step 3, after the PMU */
        bcmw_say(w, "pcie: 1: the PHY is isolated: CMU_FSYS1's PCIe gates are read after step 3 turns it on");
        w->gates_open = -1;
        return 1;
    }
    for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
        struct bcmw_lb l;
        if (!careful_rd(w, EX_CMU_FSYS1 + gates[i].off, &v)) {
            bcmw_say(w, "pcie: 1: a CMU_FSYS1 gate faulted on read: stopping");
            w->dead = 1;
            return 0;
        }
        lb_init(&l);
        lb_s(&l, "pcie: 1: gate ");
        lb_s(&l, gates[i].name);
        lb_s(&l, " ");
        lb_hex(&l, v, 8);
        if (gate_closed(v))
            lb_s(&l, ": CLOSED");
        bcmw_say_lb(w, &l);
        if (gate_closed(v))
            open = 0;
    }
    w->gates_open = open;
    return open;
}

/* ---- step 2: the radio's power ------------------------------------------------------------------- */

int ex_power(struct bcmw *w)
{
    bcmw_say(w, "pcie: 2: PERST# low, CLKREQ# to the controller (function 2, pull-up), WLAN_REG_ON low 50 ms, then high 200 ms");
    pin_out(w, BANK_GPF2, PIN_PERST, 0);
    set32(w, BANK_GPF2 + 8, 0xfu << (4 * PIN_CLKREQ), 0x3u << (4 * PIN_CLKREQ));
    set32(w, BANK_GPF2, 0xfu << (4 * PIN_CLKREQ), 0x2u << (4 * PIN_CLKREQ));
    /* off first: after a warm restart the chip may still run the last OS's firmware (mainline port) */
    pin_out(w, BANK_GPG0, PIN_WLAN_EN, 0);
    bcmw_mdelay(w, 50);
    pin_out(w, BANK_GPG0, PIN_WLAN_EN, 1);
    w->reg_on = 1;
    bcmw_mdelay(w, WIFI_TURNON_MS);
    say_pins(w, "2");
    if (pin_dat(w, BANK_GPG0, PIN_WLAN_EN) != 1) {
        bcmw_say(w, "pcie: 2: WLAN_REG_ON does not read back high: stopping");
        return 0;
    }
    return 1;
}

/* ---- step 3: the PHY ----------------------------------------------------------------------------- */

static int open_gates(struct bcmw *w)
{
    uint32_t v;
    int open = 1;

    for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
        if (!careful_rd(w, EX_CMU_FSYS1 + gates[i].off, &v)) {
            bcmw_say(w, "pcie: 3: a CMU_FSYS1 gate faulted on read after the PMU: stopping");
            w->dead = 1;
            return 0;
        }
        if (gate_closed(v)) {
            if (!w->cfg.ungate) {
                struct bcmw_lb l;
                lb_init(&l);
                lb_s(&l, "pcie: 3: gate ");
                lb_s(&l, gates[i].name);
                lb_s(&l, " CLOSED (");
                lb_hex(&l, v, 8);
                lb_s(&l, "); ungate=1 forces it open");
                bcmw_say_lb(w, &l);
                open = 0;
                continue;
            }
            wr32(w, EX_CMU_FSYS1 + gates[i].off, v | GATE_MANUAL | GATE_CG_VAL);
            bcmw_sayx(w, "pcie: 3: gate forced open, now ", rd32(w, EX_CMU_FSYS1 + gates[i].off), gates[i].name);
        }
    }
    return open;
}

/* SYSREG_FSYS1's gate (step 4 writes its PHY words) and CMU_TOP's FSYS1_PCIE gate, read through the
 * fault-catching read; SYSREG's forced open when closed (the mainline port forces both) */
static int other_gates(struct bcmw *w)
{
    uint32_t v;
    if (!careful_rd(w, EX_CMU_TOP + CMU_TOP_GATE_FSYS1_PCIE, &v)) {
        bcmw_say(w, "pcie: 3: CMU_TOP's FSYS1_PCIE gate faulted on read: stopping");
        w->dead = 1;
        return 0;
    }
    bcmw_sayx(w, "pcie: 3: CMU_TOP GATE_CLKCMU_FSYS1_PCIE ", v, gate_closed(v) ? ": CLOSED (the mainline port forces it)" : NULL);
    if (gate_closed(v) && w->cfg.ungate)
        wr32(w, EX_CMU_TOP + CMU_TOP_GATE_FSYS1_PCIE, v | GATE_MANUAL | GATE_CG_VAL);
    if (!careful_rd(w, EX_CMU_FSYS1 + 0x209c, &v)) {
        bcmw_say(w, "pcie: 3: SYSREG_FSYS1's gate faulted on read: stopping");
        w->dead = 1;
        return 0;
    }
    if (gate_closed(v)) {
        wr32(w, EX_CMU_FSYS1 + 0x209c, v | GATE_MANUAL | GATE_CG_VAL);
        bcmw_sayx(w, "pcie: 3: SYSREG_FSYS1's gate was closed, forced on: ", rd32(w, EX_CMU_FSYS1 + 0x209c), NULL);
    }
    return 1;
}

int ex_phy(struct bcmw *w)
{
    bcmw_say(w, "pcie: 3: the PHY out of isolation (PMU +0x71C bit 0)");
    set32(w, EX_PMU + PMU_PCIE_PHY_CONTROL, 1, 1);
    if (!(rd32(w, EX_PMU + PMU_PCIE_PHY_CONTROL) & 1)) {
        bcmw_say(w, "pcie: 3: PCIE_PHY_CONTROL does not read back 1: stopping");
        return 0;
    }
    if (!open_gates(w) || !other_gates(w))
        return 0;
    w->gates_open = 1;
    w->phy_on = 1;
    bcmw_say(w, "pcie: 3: the PHY out of power-down, the LTSSM's history on");
    set32(w, EX_PCS + 0x100, 1u << 6, 0);
    set32(w, EX_PCS + 0x104, 1u << 7, 0);
    wr32(w, EX_PHY + 0x20 * 4, 0xC0);
    wr32(w, EX_PHY + 0x57 * 4, 0x7E);
    wr32(w, EX_ELBI + ELBI_HISTORY_CHECK, 0);
    wr32(w, EX_ELBI + ELBI_HISTORY_CHECK, 3);
    wr32(w, EX_ELBI + ELBI_STATE_POWER_S, 0x200000);
    wr32(w, EX_ELBI + ELBI_STATE_POWER_M, 0xffffffffu);
    if (w->cfg.coherent) {
        bcmw_say(w, "pcie: 3: coherent=1: SYSREG_FSYS1 +0x700 bits 9:8 (the radio's reads and writes look in the caches)");
        set32(w, EX_SYSREG + 0x700, 3u << 8, 3u << 8);
    }
    return 1;
}

/* the PHY and the core up to the moment the LTSSM is let go: one try */
static void phy_up(struct bcmw *w)
{
    uint32_t pll, id;

    bcmw_say(w, "pcie: 4: the core's soft reset, PERST# high (20 ms), L1 exit in hardware, no clock gating, the PHY's tables");
    rx_elecidle_ignore(w, 1);
    wr32(w, EX_ELBI + ELBI_SOFT_CORE_RESET, 0);
    bcmw_udelay(w, 20);
    wr32(w, EX_ELBI + ELBI_SOFT_CORE_RESET, 1);
    pin_out(w, BANK_GPF2, PIN_PERST, 1);
    bcmw_mdelay(w, 20);
    set32(w, EX_ELBI + ELBI_APP_REQ_EXIT_L1, 0x11, 0x11);
    wr32(w, EX_ELBI + ELBI_LINKDOWN_RST_SEL, 1u << 1);
    set32(w, EX_ELBI + ELBI_QCH_SEL, 0xfff, 0);
    phy_config(w);
    set32(w, EX_ELBI + ELBI_SW_WAKE, 1u << 1, 0);
    pll = rd32(w, EX_PHY + 0xBC) & 3;
    id = dbi_rd(w, DBI_ID);
    {
        struct bcmw_lb l;
        lb_init(&l);
        lb_s(&l, "pcie: 4: PHY PLL ");
        lb_hex(&l, pll, 1);
        lb_s(&l, " (3: locked), the root port's own ID ");
        lb_hex(&l, id, 8);
        bcmw_say_lb(w, &l);
    }
}

/* dw_pcie_setup_rc() then exynos_pcie_setup_rc(), one lane */
static void setup_rc(struct bcmw *w)
{
    unsigned gen = w->cfg.link_speed == 1 ? 1 : 2;

    dbi_set(w, DBI_PORT_LINK_CONTROL, 0x3fu << 16, 1u << 16);
    dbi_set(w, DBI_LINK_WIDTH_SPEED, 0x1fu << 8, 1u << 8);
    dbi_wr(w, DBI_BAR0, 0x4);
    dbi_wr(w, DBI_BAR1, 0);
    dbi_set(w, DBI_INTERRUPT, 0xff00, 0x0100);
    dbi_set(w, DBI_BUS, 0x00ffffff, 0x00ff0100);
    dbi_set(w, DBI_COMMAND, 0xffff, 0x0107);
    dbi_wr(w, DBI_BAR0, 0);
    dbi_set(w, DBI_CLASS, 0xffff0000u, 0x0604u << 16);
    dbi_set(w, DBI_LINK_WIDTH_SPEED, 1u << 17, 1u << 17);
    dbi_wr(w, DBI_MISC_CONTROL, 1);
    dbi_set(w, DBI_LNKCAP, 0x38000 | 0x3f0 | 0xf, (0x7u << 15) | (1u << 4) | gen);
    dbi_wr(w, DBI_AUX_CLK_FREQ, 0x1A);
    dbi_wr(w, DBI_L1_SUBSTATES, 0xEA);
    dbi_wr(w, DBI_PM_CTRL, 0);
    dbi_set(w, DBI_LNKCTL, 1u << 5, 1u << 5);
    dbi_set(w, DBI_LNKCTL2, 0xf, gen);
}

static int train(struct bcmw *w)
{
    uint32_t v;

    bcmw_say(w, "pcie: 4: the root port set up (x1), then the LTSSM enabled");
    setup_rc(w);
    rx_elecidle_ignore(w, 0);
    wr32(w, EX_ELBI + ELBI_APP_LTSSM_ENABLE, 1);
    for (unsigned i = 0; i < LINK_POLLS; i++) {
        v = rd32(w, EX_ELBI + ELBI_RDLH_LINKUP) & 0x1f;
        if (v >= 0x0d && v <= 0x14) {
            struct bcmw_lb l;
            w->linked = 1;
            wr32(w, EX_ELBI + ELBI_L12ERR_CTRL, 0);
            v = rd32(w, EX_DBI + DBI_LNKCTL) >> 16;
            w->info.link_gen = v & 0xf;
            w->info.link_width = (v >> 4) & 0x3f;
            lb_init(&l);
            lb_s(&l, "pcie: 4: LINK UP after ");
            lb_dec(&l, 10 * i);
            lb_s(&l, " us: Gen");
            lb_dec(&l, w->info.link_gen);
            lb_s(&l, " x");
            lb_dec(&l, w->info.link_width);
            bcmw_say_lb(w, &l);
            wr32(w, EX_ELBI + ELBI_IRQ_PULSE, rd32(w, EX_ELBI + ELBI_IRQ_PULSE));
            return 1;
        }
        bcmw_udelay(w, 10);
    }
    bcmw_sayx(w, "pcie: 4: no link in 20 ms, LTSSM state ", rd32(w, EX_ELBI + ELBI_RDLH_LINKUP), "; its history, oldest first:");
    for (int i = 31; i >= 0; i -= 8) {
        struct bcmw_lb l;
        lb_init(&l);
        lb_s(&l, "pcie:   ");
        for (int j = i; j > i - 8; j--) {
            uint32_t h = rd32(w, EX_ELBI + ELBI_HISTORY(j));
            lb_s(&l, " ");
            lb_hex(&l, (h >> 16) & 0x3f, 2);
        }
        bcmw_say_lb(w, &l);
    }
    pin_out(w, BANK_GPF2, PIN_PERST, 0);
    wr32(w, EX_ELBI + ELBI_APP_LTSSM_ENABLE, 0);
    return 0;
}

int ex_link(struct bcmw *w)
{
    unsigned tries = w->cfg.link_tries ? w->cfg.link_tries : 3;
    if (tries > 10)
        tries = 10;
    for (unsigned t = 1; t <= tries; t++) {
        phy_up(w);
        if (train(w))
            return 1;
        if (t < tries)
            bcmw_sayx(w, "pcie: 4: again, try ", t + 1, NULL);
    }
    return 0;
}

/* ---- step 5: the endpoint ------------------------------------------------------------------------ */

uint32_t ex_cfg_rd(struct bcmw *w, uint32_t off)
{
    if (!w->linked || off >= EX_CFG_SIZE)
        return 0xffffffffu;
    return rd32(w, EX_CFG + (off & ~3u));
}

void ex_cfg_wr(struct bcmw *w, uint32_t off, uint32_t v)
{
    if (w->linked && off < EX_CFG_SIZE)
        wr32(w, EX_CFG + (off & ~3u), v);
}

int ex_enumerate(struct bcmw *w)
{
    uint32_t id, class, hdr, next = (uint32_t)EX_MEM;
    unsigned placed = 0;

    bcmw_say(w, "pcie: 5: the iATU: region 1 to type 0 configuration of 1:00.0, region 0 to the memory window");
    atu_outbound(w, 1, ATU_TYPE_CFG0, EX_CFG, EX_CFG_SIZE, 1u << 24);
    atu_outbound(w, 0, ATU_TYPE_MEM, EX_MEM, EX_MEM_SIZE, EX_MEM);
    dbi_wr(w, DBI_MEM_WINDOW, (0x126Fu << 16) | 0x1180u);
    id = ex_cfg_rd(w, 0x00);
    if ((id & 0xffff) == 0xffff || id == 0) {
        bcmw_sayx(w, "pcie: 5: nothing answers at 1:00.0: ", id, NULL);
        return 0;
    }
    class = ex_cfg_rd(w, 0x08);
    hdr = ex_cfg_rd(w, 0x0C);
    w->info.vendor = (uint16_t)id;
    w->info.device = (uint16_t)(id >> 16);
    {
        struct bcmw_lb l;
        lb_init(&l);
        lb_s(&l, "pcie: 5: 1:00.0 is ");
        lb_hex(&l, id & 0xffff, 4);
        lb_s(&l, ":");
        lb_hex(&l, id >> 16, 4);
        lb_s(&l, (id & 0xffff) == 0x14e4 ? " (Broadcom)" : " (NOT Broadcom)");
        lb_s(&l, ", class ");
        lb_hex(&l, class >> 8, 6);
        lb_s(&l, " rev ");
        lb_hex(&l, class & 0xff, 2);
        bcmw_say_lb(w, &l);
    }
    if ((id & 0xffff) != 0x14e4 || ((hdr >> 16) & 0x7f) != 0) {
        bcmw_say(w, "pcie: 5: not a Broadcom endpoint: its BARs are left alone");
        return 0;
    }
    for (unsigned b = 0; b < 6; b++) {
        uint32_t off = 0x10 + 4 * b, was = ex_cfg_rd(w, off), lo, hi = 0xffffffffu;
        int is64 = (was & 0x7) == 0x4;
        uint64_t size;

        ex_cfg_wr(w, off, 0xffffffffu);
        lo = ex_cfg_rd(w, off);
        ex_cfg_wr(w, off, was);
        if (is64 && b < 5) {
            uint32_t was_hi = ex_cfg_rd(w, off + 4);
            ex_cfg_wr(w, off + 4, 0xffffffffu);
            hi = ex_cfg_rd(w, off + 4);
            ex_cfg_wr(w, off + 4, was_hi);
        }
        if (lo == 0 || (was & 1)) {
            if (is64)
                b++;
            continue;
        }
        size = ~(((uint64_t)hi << 32) | (lo & ~0xfu)) + 1;
        if (!is64)
            size &= 0xffffffffu;
        next = (uint32_t)((next + size - 1) & ~(size - 1));
        /* inside the bridge's memory window (0x11800000..0x126FFFFF, set above) */
        if (size == 0 || next + size > 0x12700000ULL) {
            bcmw_sayx(w, "pcie: 5: a BAR does not fit the window, unplaced; its size ", size, NULL);
        } else {
            struct bcmw_lb l;
            ex_cfg_wr(w, off, next | (was & 0xf));
            if (is64)
                ex_cfg_wr(w, off + 4, 0);
            w->bar_pa[b] = next;
            w->bar_size[b] = size;
            lb_init(&l);
            lb_s(&l, "pcie: 5: BAR");
            lb_dec(&l, b);
            lb_s(&l, is64 ? ": 64-bit, " : ": 32-bit, ");
            lb_dec(&l, size >> 10);
            lb_s(&l, " KiB at ");
            lb_hex(&l, next, 8);
            bcmw_say_lb(w, &l);
            placed++;
            next += (uint32_t)size;
        }
        if (is64)
            b++;
    }
    /* brcmfmac's view: BAR0 the register window (32 KiB), BAR2 the RAM window (BAR1 by Linux's
     * count when BAR0 is 64-bit) */
    w->bar0 = w->bar_pa[0];
    w->bar1 = w->bar_pa[2] ? w->bar_pa[2] : w->bar_pa[1];
    w->bar1_size = w->bar_pa[2] ? w->bar_size[2] : w->bar_size[1];
    if (!w->bar0 || !w->bar1 || placed < 2) {
        bcmw_say(w, "pcie: 5: the register and RAM windows are not both placed");
        return 0;
    }
    return 1;
}

/* ---- step 6: decoding and the way to memory ------------------------------------------------------ */

int ex_decode(struct bcmw *w)
{
    uint32_t v;

    bcmw_say(w, "pcie: 6: 1:00.0's memory decoding and bus mastering on");
    ex_cfg_wr(w, 0x04, (ex_cfg_rd(w, 0x04) & 0xffffu) | 0x6u);
    for (unsigned i = 0; i < sizeof(dma_path) / sizeof(dma_path[0]); i++) {
        v = rd32(w, EX_CMU_FSYS1 + dma_path[i].off);
        /* the SysMMU's clock on as Android does (its registers are never touched); the others
         * only when closed */
        if (gate_closed(v) || dma_path[i].off == GATE_SYSMMU) {
            wr32(w, EX_CMU_FSYS1 + dma_path[i].off, v | GATE_MANUAL | GATE_CG_VAL);
            bcmw_sayx(w, "pcie: 6: data path gate forced on: ", rd32(w, EX_CMU_FSYS1 + dma_path[i].off), dma_path[i].name);
        }
    }
    bcmw_sayx(w, "pcie: 6: SYSREG_FSYS1 shareability ", rd32(w, EX_SYSREG + 0x700), " (bits 9:8 = 3: coherent)");
    bcmw_sayx(w, "pcie: 6: command/status now ", ex_cfg_rd(w, 0x04), NULL);
    w->handed_over = 1;
    return 1;
}

/* ---- INTx (pci-exynos.c exynos_pcie_enable_interrupts / _irq_handler) ---------------------------- */

void ex_intx(struct bcmw *w, int on)
{
    if (!w->handed_over)
        return;
    wr32(w, EX_ELBI + ELBI_IRQ_EN_PULSE, 0);
    wr32(w, EX_ELBI + ELBI_IRQ_PULSE, rd32(w, EX_ELBI + ELBI_IRQ_PULSE));
    if (on) {
        wr32(w, EX_ELBI + ELBI_IRQ_TOE_EN_PULSE2, 0);
        wr32(w, EX_ELBI + ELBI_IRQ_TOE_EN_LEVEL, 0);
        wr32(w, EX_ELBI + ELBI_IRQ_EN_LEVEL, 0);
        wr32(w, EX_ELBI + ELBI_IRQ_EN_SPECIAL, 0);
        wr32(w, EX_ELBI + ELBI_IRQ_EN_PULSE, INTX_ASSERTS);
    }
}

uint32_t ex_intx_ack(struct bcmw *w)
{
    uint32_t latched;
    if (!w->handed_over)
        return 0;
    latched = rd32(w, EX_ELBI + ELBI_IRQ_PULSE);
    wr32(w, EX_ELBI + ELBI_IRQ_PULSE, latched);
    return latched;
}

/* ---- the way back -------------------------------------------------------------------------------- */

void ex_quiesce(struct bcmw *w)
{
    if (!w->reg_on)
        return;
    if (w->handed_over) {
        wr32(w, EX_ELBI + ELBI_IRQ_EN_PULSE, 0);
        wr32(w, EX_ELBI + ELBI_IRQ_PULSE, rd32(w, EX_ELBI + ELBI_IRQ_PULSE));
    }
    /* Samsung's exynos_pcie_poweroff(): PERST# low (the chip's reset: it stops its DMA), the LTSSM
     * off, the PHY powered down (exynos_phy_all_pwrdn: TRSV 0xFE, CMN 0xC1, the PCS's two power-down
     * bits), the NCLK guard on; then dhd drops WLAN_REG_ON. The PHY is touched only when this run
     * powered it up (its gates were seen open after the PMU). Configuration space is not touched (the
     * link may be down), so nothing here can hang on a request that never completes. */
    pin_out(w, BANK_GPF2, PIN_PERST, 0);
    if (w->gates_open == 1 && w->phy_on) {
        wr32(w, EX_ELBI + ELBI_APP_LTSSM_ENABLE, 0);
        wr32(w, EX_PHY + 0x57 * 4, 0xFE);
        wr32(w, EX_PHY + 0x20 * 4, 0xC1);
        set32(w, EX_PCS + 0x100, 1u << 6, 1u << 6);
        set32(w, EX_PCS + 0x104, 1u << 7, 1u << 7);
        wr32(w, EX_ELBI + ELBI_L12ERR_CTRL, 1u << 2);
        w->phy_on = 0;
    }
    pin_out(w, BANK_GPG0, PIN_WLAN_EN, 0);
    w->reg_on = 0;
    w->linked = 0;
    w->handed_over = 0;
    bcmw_sayx(w, "pcie: the way back: PERST# low, WLAN_REG_ON low (reads ", pin_dat(w, BANK_GPG0, PIN_WLAN_EN), ")");
}
