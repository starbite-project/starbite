// SPDX-License-Identifier: ISC
/*
 * bcmw: the BCM4361 as a set of cores behind its BAR0 window: the chip's ID, the enumeration ROM,
 * the AI cores' reset and disable, the ARM CR4 halted and released, the RAM's size, the watchdog
 * reset, the module's CIS from OTP.
 *
 * Adapted from Linux's brcmfmac (drivers/net/wireless/broadcom/brcm80211/brcmfmac/chip.c and
 * pcie.c, Linux 7.1):
 *   Copyright (c) 2014 Broadcom Corporation
 *
 *   Permission to use, copy, modify, and/or distribute this software for any purpose with or
 *   without fee is hereby granted, provided that the above copyright notice and this permission
 *   notice appear in all copies.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
 *   SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 *   AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *   WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 *   NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 *   PERFORMANCE OF THIS SOFTWARE.
 *
 * BCM4361 facts: RAM base 0x170000 (bcmdhd CR4_4347_RAM_BASE; the pmOS kernel's and an earlier port's
 * brcmfmac patches); the chip reports 0x4361 or 0x4347 (bcmdhd dhd_validate_chipid); the OTP
 * CIS words and the two writes before them (bcmdhd dhd_custom_cis.c, via an earlier port's
 * bcm4361_cis.h).
 */
#include "bcmw_priv.h"

#define SI_ENUM_BASE        0x18000000u
#define CC_CHIPID           0x00
#define CC_CAPABILITIES     0x04
#define CC_WATCHDOG         0x80
#define CC_EROMPTR          0xfc
#define CID_ID_MASK         0x0000ffffu
#define CID_REV_MASK        0x000f0000u
#define CID_REV_SHIFT       16
#define CID_TYPE_SHIFT      28
#define SOCI_AI             1

#define BCMA_IOCTL          0x0408
#define BCMA_IOCTL_CLK      0x0001
#define BCMA_IOCTL_FGC      0x0002
#define BCMA_RESET_CTL      0x0800
#define BCMA_RESET_CTL_RESET 0x0001
#define ARMCR4_IOCTL_CPUHALT 0x0020
#define D11_IOCTL_PHYCLOCKEN 0x0004
#define D11_IOCTL_PHYRESET  0x0008

#define ARMCR4_CAP          0x04
#define ARMCR4_BANKIDX      0x40
#define ARMCR4_BANKINFO     0x44
#define ARMCR4_TCBBNB_MASK  0xf0
#define ARMCR4_TCBBNB_SHIFT 4
#define ARMCR4_TCBANB_MASK  0xf
#define ARMCR4_BSZ_MASK     0x7f
#define ARMCR4_BSZ_MULT     8192
#define ARMCR4_BLK_1K_MASK  0x200

#define DMP_DESC_TYPE_MSK   0x0000000F
#define DMP_DESC_EMPTY      0x00000000
#define DMP_DESC_VALID      0x00000001
#define DMP_DESC_COMPONENT  0x00000001
#define DMP_DESC_MASTER_PORT 0x00000003
#define DMP_DESC_ADDRESS    0x00000005
#define DMP_DESC_ADDRSIZE_GT32 0x00000008
#define DMP_DESC_EOT        0x0000000F
#define DMP_COMP_PARTNUM    0x000FFF00
#define DMP_COMP_PARTNUM_S  8
#define DMP_COMP_REVISION   0xFF000000
#define DMP_COMP_REVISION_S 24
#define DMP_COMP_NUM_SWRAP  0x00F80000
#define DMP_COMP_NUM_SWRAP_S 19
#define DMP_COMP_NUM_MWRAP  0x0007C000
#define DMP_COMP_NUM_MWRAP_S 14
#define DMP_SLAVE_ADDR_BASE 0xFFFFF000
#define DMP_SLAVE_TYPE      0x000000C0
#define DMP_SLAVE_TYPE_S    6
#define DMP_SLAVE_TYPE_SLAVE 0
#define DMP_SLAVE_TYPE_SWRAP 2
#define DMP_SLAVE_TYPE_MWRAP 3
#define DMP_SLAVE_SIZE_TYPE 0x00000030
#define DMP_SLAVE_SIZE_TYPE_S 4
#define DMP_SLAVE_SIZE_4K   0
#define DMP_SLAVE_SIZE_8K   1
#define DMP_SLAVE_SIZE_DESC 3

#define PCIE_BAR0_WINDOW    0x80        /* the endpoint's configuration register for BAR0's window */
#define PCIE_LINK_STATUS_CTRL 0xBC
#define PCIE2_CONFIGADDR    0x120
#define PCIE2_CONFIGDATA    0x124
#define PCIE2_MAILBOXINT    0x48
#define PCIE2_MAILBOXMASK   0x4C
#define PCIE2_H2D_MAILBOX_0 0x140
#define PCIE2_H2D_MAILBOX_1 0x144
#define PCIE64_MAILBOXINT   0xC30
#define PCIE64_MAILBOXMASK  0xC34
#define PCIE64_H2D_MAILBOX_0 0xA20
#define PCIE64_H2D_MAILBOX_1 0xA24

#define BCM4361_RAMBASE     0x170000u
#define BCM4361_OTP_CONTROL1 0x18010324u
#define BCM4361_PMU_MIN_RES 0x18012618u
#define BCM4361_CIS_START   0x18011110u
#define BCM4361_CIS_WORDS   22u

/* ---- the windows ------------------------------------------------------------------------------- */

static void set_window(struct bcmw *w, uint32_t base)
{
    if (w->bar0_window == base)
        return;
    ex_cfg_wr(w, PCIE_BAR0_WINDOW, base);
    if (ex_cfg_rd(w, PCIE_BAR0_WINDOW) != base)
        ex_cfg_wr(w, PCIE_BAR0_WINDOW, base);       /* brcmfmac writes it twice when it does not take */
    w->bar0_window = base;
}

uint32_t chip_bp_rd(struct bcmw *w, uint32_t addr)
{
    set_window(w, addr & ~0xfffu);
    return rd32(w, w->bar0 + (addr & 0xfff));
}

void chip_bp_wr(struct bcmw *w, uint32_t addr, uint32_t v)
{
    set_window(w, addr & ~0xfffu);
    wr32(w, w->bar0 + (addr & 0xfff), v);
}

struct bcmw_core *chip_core(struct bcmw *w, uint16_t id)
{
    for (unsigned i = 0; i < w->ncores; i++)
        if (w->cores[i].id == id)
            return &w->cores[i];
    return 0;
}

void chip_select(struct bcmw *w, uint16_t id)
{
    struct bcmw_core *c = chip_core(w, id);
    if (c)
        set_window(w, c->base);
}

uint32_t chip_reg_rd(struct bcmw *w, uint32_t off)
{
    return off < 0x1000 ? rd32(w, w->bar0 + off) : 0xffffffffu;
}

void chip_reg_wr(struct bcmw *w, uint32_t off, uint32_t v)
{
    if (off < 0x1000)
        wr32(w, w->bar0 + off, v);
}

/* ---- TCM: the chip's RAM through BAR1 (BAR2 by configuration slot) ------------------------------ */

/* An address the firmware gave that lies outside the RAM window stops the core once: a read of it
 * could reach any physical address (an unclocked block aborts the bus). */
static int tcm_ok(struct bcmw *w, uint32_t a, uint32_t n)
{
    if (!w->dead && (uint64_t)a + n <= w->bar1_size && w->bar1)
        return 1;
    if (!w->dead)
        bcmw_sayx(w, "chip: an access outside the chip's RAM window, at TCM ", a, "; nothing more is attempted");
    w->dead = 1;
    return 0;
}

uint32_t tcm_rd32(struct bcmw *w, uint32_t a) { return tcm_ok(w, a, 4) ? rd32(w, w->bar1 + a) : 0xffffffffu; }
void tcm_wr32(struct bcmw *w, uint32_t a, uint32_t v)
{
    if (tcm_ok(w, a, 4))
        wr32(w, w->bar1 + a, v);
}

uint16_t tcm_rd16(struct bcmw *w, uint32_t a)
{
    if (!tcm_ok(w, a, 2))
        return 0xffff;
    if (w->os.rd16)
        return w->os.rd16(w->os.ctx, w->bar1 + a);
    return (uint16_t)(rd32(w, w->bar1 + (a & ~3u)) >> (8 * (a & 2)));
}

void tcm_wr16(struct bcmw *w, uint32_t a, uint16_t v)
{
    if (!tcm_ok(w, a, 2))
        return;
    if (w->os.wr16 && bcmw_write_allowed(w->bar1 + (a & ~3u))) {
        w->os.wr16(w->os.ctx, w->bar1 + a, v);
        return;
    }
    set32(w, w->bar1 + (a & ~3u), 0xffffu << (8 * (a & 2)), (uint32_t)v << (8 * (a & 2)));
}

void tcm_write(struct bcmw *w, uint32_t a, const uint8_t *src, uint32_t len)
{
    if (!tcm_ok(w, a, len))
        return;
    while (len && (a & 3)) {        /* a byte at a time to the first word boundary */
        uint32_t sh = 8 * (a & 3);
        set32(w, w->bar1 + (a & ~3u), 0xffu << sh, (uint32_t)*src << sh);
        a++, src++, len--;
    }
    for (; len >= 4; a += 4, src += 4, len -= 4)
        wr32(w, w->bar1 + a, le32(src));
    for (uint32_t i = 0; i < len; i++)
        set32(w, w->bar1 + a, 0xffu << (8 * i), (uint32_t)src[i] << (8 * i));
}

void tcm_read(struct bcmw *w, uint32_t a, uint8_t *dst, uint32_t len)
{
    if (!tcm_ok(w, a, len)) {
        bcmw_memset(dst, 0xff, len);
        return;
    }
    while (len) {
        uint32_t v = rd32(w, w->bar1 + (a & ~3u));
        for (uint32_t i = a & 3; i < 4 && len; i++, len--, a++)
            *dst++ = (uint8_t)(v >> (8 * i));
    }
}

/* ---- AI cores ----------------------------------------------------------------------------------- */

static void core_disable(struct bcmw *w, struct bcmw_core *c, uint32_t prereset, uint32_t reset)
{
    if (!(chip_bp_rd(w, c->wrap + BCMA_RESET_CTL) & BCMA_RESET_CTL_RESET)) {
        chip_bp_wr(w, c->wrap + BCMA_IOCTL, prereset | BCMA_IOCTL_FGC | BCMA_IOCTL_CLK);
        (void)chip_bp_rd(w, c->wrap + BCMA_IOCTL);
        chip_bp_wr(w, c->wrap + BCMA_RESET_CTL, BCMA_RESET_CTL_RESET);
        bcmw_udelay(w, 20);
        for (unsigned i = 0; i < 30 && chip_bp_rd(w, c->wrap + BCMA_RESET_CTL) != BCMA_RESET_CTL_RESET; i++)
            bcmw_udelay(w, 10);
    }
    chip_bp_wr(w, c->wrap + BCMA_IOCTL, reset | BCMA_IOCTL_FGC | BCMA_IOCTL_CLK);
    (void)chip_bp_rd(w, c->wrap + BCMA_IOCTL);
}

static void core_reset(struct bcmw *w, struct bcmw_core *c, uint32_t prereset, uint32_t reset, uint32_t postreset)
{
    core_disable(w, c, prereset, reset);
    for (unsigned i = 0; i <= 50 && (chip_bp_rd(w, c->wrap + BCMA_RESET_CTL) & BCMA_RESET_CTL_RESET); i++) {
        chip_bp_wr(w, c->wrap + BCMA_RESET_CTL, 0);
        bcmw_udelay(w, 50);
    }
    chip_bp_wr(w, c->wrap + BCMA_IOCTL, postreset | BCMA_IOCTL_CLK);
    (void)chip_bp_rd(w, c->wrap + BCMA_IOCTL);
}

void chip_set_passive(struct bcmw *w)
{
    struct bcmw_core *arm = chip_core(w, CORE_ARM_CR4);
    uint32_t v;

    if (!arm)
        return;
    v = chip_bp_rd(w, arm->wrap + BCMA_IOCTL) & ARMCR4_IOCTL_CPUHALT;
    core_reset(w, arm, v, ARMCR4_IOCTL_CPUHALT, ARMCR4_IOCTL_CPUHALT);
    /* the D11 cores disabled only; the firmware enables them (brcmfmac: releasing them ourselves
     * breaks BCM4387) */
    for (unsigned i = 0; i < w->ncores; i++)
        if (w->cores[i].id == CORE_80211)
            core_disable(w, &w->cores[i], D11_IOCTL_PHYRESET | D11_IOCTL_PHYCLOCKEN, D11_IOCTL_PHYCLOCKEN);
}

/* the ARM CR4's wrapper: ioctl (bit 0x20 = CPUHALT), reset control, I/O status (diagnostics) */
void chip_cr4_state(struct bcmw *w, uint32_t *ioctl, uint32_t *resetctl, uint32_t *iost)
{
    struct bcmw_core *arm = chip_core(w, CORE_ARM_CR4);
    *ioctl = *resetctl = *iost = 0xffffffffu;
    if (!arm)
        return;
    *ioctl = chip_bp_rd(w, arm->wrap + BCMA_IOCTL);
    *resetctl = chip_bp_rd(w, arm->wrap + BCMA_RESET_CTL);
    *iost = chip_bp_rd(w, arm->wrap + 0x0500);
}

int chip_set_active(struct bcmw *w, uint32_t rstvec)
{
    struct bcmw_core *arm = chip_core(w, CORE_ARM_CR4);
    if (!arm)
        return 0;
    tcm_wr32(w, 0, rstvec);         /* the reset vector at TCM 0 (brcmf_pcie_buscore_activate) */
    core_reset(w, arm, ARMCR4_IOCTL_CPUHALT, 0, 0);
    return 1;
}

/* ---- the enumeration ROM ------------------------------------------------------------------------ */

static uint32_t erom_desc(struct bcmw *w, uint32_t *a, uint8_t *type)
{
    uint32_t v = chip_bp_rd(w, *a);
    *a += 4;
    if (type) {
        *type = v & DMP_DESC_TYPE_MSK;
        if ((*type & ~DMP_DESC_ADDRSIZE_GT32) == DMP_DESC_ADDRESS)
            *type = DMP_DESC_ADDRESS;
    }
    return v;
}

static int erom_regaddr(struct bcmw *w, uint32_t *a, uint32_t *regbase, uint32_t *wrapbase)
{
    uint8_t desc, wraptype;
    uint32_t v;
    unsigned guard = 0;

    *regbase = *wrapbase = 0;
    v = erom_desc(w, a, &desc);
    if (desc == DMP_DESC_MASTER_PORT) {
        wraptype = DMP_SLAVE_TYPE_MWRAP;
    } else if (desc == DMP_DESC_ADDRESS) {
        *a -= 4;
        wraptype = DMP_SLAVE_TYPE_SWRAP;
    } else {
        *a -= 4;
        return -1;
    }
    do {
        uint8_t sztype, stype;
        do {
            v = erom_desc(w, a, &desc);
            if (desc == DMP_DESC_EOT) {
                *a -= 4;
                return -1;
            }
            if (++guard > 512)
                return -1;
        } while (desc != DMP_DESC_ADDRESS && desc != DMP_DESC_COMPONENT);
        if (desc == DMP_DESC_COMPONENT) {
            *a -= 4;
            return 0;
        }
        if (v & DMP_DESC_ADDRSIZE_GT32)
            erom_desc(w, a, 0);
        sztype = (uint8_t)((v & DMP_SLAVE_SIZE_TYPE) >> DMP_SLAVE_SIZE_TYPE_S);
        if (sztype == DMP_SLAVE_SIZE_DESC) {
            uint32_t sz = erom_desc(w, a, 0);
            if (sz & DMP_DESC_ADDRSIZE_GT32)
                erom_desc(w, a, 0);
        }
        if (sztype != DMP_SLAVE_SIZE_4K && sztype != DMP_SLAVE_SIZE_8K)
            continue;
        stype = (uint8_t)((v & DMP_SLAVE_TYPE) >> DMP_SLAVE_TYPE_S);
        if (*regbase == 0 && stype == DMP_SLAVE_TYPE_SLAVE)
            *regbase = v & DMP_SLAVE_ADDR_BASE;
        if (*wrapbase == 0 && stype == wraptype)
            *wrapbase = v & DMP_SLAVE_ADDR_BASE;
    } while (*regbase == 0 || *wrapbase == 0);
    return 0;
}

static int erom_scan(struct bcmw *w)
{
    uint32_t a = chip_bp_rd(w, SI_ENUM_BASE + CC_EROMPTR), v;
    uint8_t type = 0;
    unsigned guard = 0;

    w->ncores = 0;
    while (type != DMP_DESC_EOT && ++guard < 1024) {
        uint16_t id;
        uint8_t nmw, nsw, rev;
        uint32_t base, wrap;

        v = erom_desc(w, &a, &type);
        if (!(v & DMP_DESC_VALID) || type == DMP_DESC_EMPTY || type != DMP_DESC_COMPONENT)
            continue;
        id = (uint16_t)((v & DMP_COMP_PARTNUM) >> DMP_COMP_PARTNUM_S);
        v = erom_desc(w, &a, &type);
        if ((v & DMP_DESC_TYPE_MSK) != DMP_DESC_COMPONENT)
            return -1;
        nmw = (uint8_t)((v & DMP_COMP_NUM_MWRAP) >> DMP_COMP_NUM_MWRAP_S);
        nsw = (uint8_t)((v & DMP_COMP_NUM_SWRAP) >> DMP_COMP_NUM_SWRAP_S);
        rev = (uint8_t)((v & DMP_COMP_REVISION) >> DMP_COMP_REVISION_S);
        if (nmw + nsw == 0 && id != CORE_PMU && id != CORE_GCI)
            continue;
        if (erom_regaddr(w, &a, &base, &wrap))
            continue;
        if (w->ncores < BCMW_MAX_CORES) {
            w->cores[w->ncores].id = id;
            w->cores[w->ncores].rev = rev;
            w->cores[w->ncores].base = base;
            w->cores[w->ncores].wrap = wrap;
            w->ncores++;
        }
    }
    return 0;
}

/* ---- RAM ---------------------------------------------------------------------------------------- */

static uint32_t tcm_ramsize(struct bcmw *w, struct bcmw_core *cr4)
{
    uint32_t cap = chip_bp_rd(w, cr4->base + ARMCR4_CAP), size = 0;
    uint32_t banks = (cap & ARMCR4_TCBANB_MASK) + ((cap & ARMCR4_TCBBNB_MASK) >> ARMCR4_TCBBNB_SHIFT);

    for (uint32_t i = 0; i < banks && i < 64; i++) {
        uint32_t info, blk = ARMCR4_BSZ_MULT;
        chip_bp_wr(w, cr4->base + ARMCR4_BANKIDX, i);
        info = chip_bp_rd(w, cr4->base + ARMCR4_BANKINFO);
        if (info & ARMCR4_BLK_1K_MASK)
            blk >>= 3;
        size += ((info & ARMCR4_BSZ_MASK) + 1) * blk;
    }
    return size;
}

/* ---- the watchdog reset (brcmf_pcie_reset_device) ----------------------------------------------- */

void chip_watchdog_reset(struct bcmw *w)
{
    static const uint16_t cfg_offset[] = { 0x4, 0x4C, 0x58, 0x5C, 0x60, 0x64, 0xDC, 0x228, 0x248, 0x4E0, 0x4F4 };
    struct bcmw_core *pcie = chip_core(w, CORE_PCIE2);
    uint32_t lsc;

    chip_select(w, CORE_PCIE2);
    lsc = ex_cfg_rd(w, PCIE_LINK_STATUS_CTRL);
    ex_cfg_wr(w, PCIE_LINK_STATUS_CTRL, lsc & ~3u);        /* ASPM off */
    chip_select(w, CORE_CHIPCOMMON);
    chip_reg_wr(w, CC_WATCHDOG, 4);
    bcmw_mdelay(w, 100);
    w->bar0_window = 0xffffffffu;                          /* the window may have been reset */
    chip_select(w, CORE_PCIE2);
    ex_cfg_wr(w, PCIE_LINK_STATUS_CTRL, lsc);
    if (pcie && pcie->rev <= 13) {
        for (unsigned i = 0; i < sizeof(cfg_offset) / sizeof(cfg_offset[0]); i++) {
            chip_reg_wr(w, PCIE2_CONFIGADDR, cfg_offset[i]);
            chip_reg_wr(w, PCIE2_CONFIGDATA, chip_reg_rd(w, PCIE2_CONFIGDATA));
        }
    }
}

/* ---- attach ------------------------------------------------------------------------------------- */

static const char *core_name(uint16_t id)
{
    switch (id) {
    case CORE_CHIPCOMMON: return "chipcommon";
    case CORE_80211: return "802.11";
    case CORE_PMU: return "PMU";
    case CORE_PCIE2: return "PCIe2";
    case CORE_ARM_CR4: return "ARM CR4";
    case CORE_GCI: return "GCI";
    case CORE_SYS_MEM: return "sysmem";
    default: return "core";
    }
}

int chip_attach(struct bcmw *w)
{
    uint32_t id;
    struct bcmw_core *pcie, *cr4;
    struct bcmw_lb l;

    w->bar0_window = 0xffffffffu;
    id = chip_bp_rd(w, SI_ENUM_BASE + CC_CHIPID);
    if (id == 0xffffffffu) {
        bcmw_say(w, "chip: 7: chipcommon reads all ones: BAR0's window does not take, or the chip is in reset");
        return BCMW_E_NODEV;
    }
    w->info.chip = id & CID_ID_MASK;
    w->info.chiprev = (id & CID_REV_MASK) >> CID_REV_SHIFT;
    lb_init(&l);
    lb_s(&l, "chip: 7: chip ");
    lb_hex(&l, w->info.chip, 4);
    lb_s(&l, " revision ");
    lb_dec(&l, w->info.chiprev);
    lb_s(&l, (id >> CID_TYPE_SHIFT) == SOCI_AI ? ", AXI backplane" : ", NOT an AXI backplane");
    bcmw_say_lb(w, &l);
    if ((id >> CID_TYPE_SHIFT) != SOCI_AI)
        return BCMW_E_NODEV;
    if (w->info.chip != 0x4361 && w->info.chip != 0x4347)
        bcmw_say(w, "chip: 7: not a BCM4361/4347: going on, but nothing here was made for it");
    if (erom_scan(w))
        return BCMW_E_PROTO;
    for (unsigned i = 0; i < w->ncores; i++) {
        lb_init(&l);
        lb_s(&l, "chip: 7: ");
        lb_s(&l, core_name(w->cores[i].id));
        lb_s(&l, " ");
        lb_hex(&l, w->cores[i].id, 3);
        lb_s(&l, " rev ");
        lb_dec(&l, w->cores[i].rev);
        lb_s(&l, " at ");
        lb_hex(&l, w->cores[i].base, 8);
        lb_s(&l, " wrapper ");
        lb_hex(&l, w->cores[i].wrap, 8);
        bcmw_say_lb(w, &l);
    }
    pcie = chip_core(w, CORE_PCIE2);
    cr4 = chip_core(w, CORE_ARM_CR4);
    if (!chip_core(w, CORE_CHIPCOMMON) || !pcie || !cr4) {
        bcmw_say(w, "chip: 7: chipcommon, PCIe2 or the ARM CR4 is missing");
        return BCMW_E_NODEV;
    }
    if (pcie->rev >= 64) {
        w->mb_int = PCIE64_MAILBOXINT, w->mb_mask = PCIE64_MAILBOXMASK;
        w->h2d_mb0 = PCIE64_H2D_MAILBOX_0, w->h2d_mb1 = PCIE64_H2D_MAILBOX_1;
        w->int_d2h_db = 0xffff, w->int_fn0 = 0;
    } else {
        w->mb_int = PCIE2_MAILBOXINT, w->mb_mask = PCIE2_MAILBOXMASK;
        w->h2d_mb0 = PCIE2_H2D_MAILBOX_0, w->h2d_mb1 = PCIE2_H2D_MAILBOX_1;
        w->int_d2h_db = 0xff0000, w->int_fn0 = 0x0300;
    }
    bcmw_say(w, "chip: 7: the ARM halted, the 802.11 cores disabled; the watchdog reset (100 ms)");
    chip_set_passive(w);
    chip_watchdog_reset(w);
    chip_select(w, CORE_PCIE2);
    {
        uint32_t v = chip_reg_rd(w, w->mb_int);
        if (v != 0xffffffffu)
            chip_reg_wr(w, w->mb_int, v);
    }
    chip_set_passive(w);
    w->info.ramsize = tcm_ramsize(w, cr4);
    w->info.rambase = BCM4361_RAMBASE;
    lb_init(&l);
    lb_s(&l, "chip: 7: RAM ");
    lb_dec(&l, w->info.ramsize >> 10);
    lb_s(&l, " KiB at ");
    lb_hex(&l, w->info.rambase, 6);
    bcmw_say_lb(w, &l);
    if (!w->info.ramsize || w->info.ramsize > 4u << 20 || w->bar1_size < w->info.rambase + w->info.ramsize) {
        bcmw_say(w, "chip: 7: the RAM size is undetermined or larger than the RAM window");
        return BCMW_E_PROTO;
    }
    /* brcmf_pcie_attach: BAR1's window may not be sized properly: config 0x4E0 read and written back */
    chip_select(w, CORE_PCIE2);
    chip_reg_wr(w, PCIE2_CONFIGADDR, 0x4e0);
    chip_reg_wr(w, PCIE2_CONFIGDATA, chip_reg_rd(w, PCIE2_CONFIGDATA));
    return BCMW_OK;
}

/* The module's CIS from OTP, as bcmdhd reads it before it picks the NVRAM: OTP Control 1 and the
 * PMU's minimum resources written (each read back), then 22 words. */
int chip_read_cis(struct bcmw *w, uint8_t *cis, uint32_t len)
{
    uint32_t n = len / 4 < BCM4361_CIS_WORDS ? len / 4 : BCM4361_CIS_WORDS;

    chip_bp_wr(w, BCM4361_OTP_CONTROL1, 0x00fa0000u);
    bcmw_sayx(w, "chip: 7: OTP Control 1 written 00fa0000, reads ", chip_bp_rd(w, BCM4361_OTP_CONTROL1), NULL);
    chip_bp_wr(w, BCM4361_PMU_MIN_RES, 0x0e4fffffu);
    bcmw_sayx(w, "chip: 7: the PMU's minimum resources written 0e4fffff, reads ", chip_bp_rd(w, BCM4361_PMU_MIN_RES), NULL);
    for (uint32_t i = 0; i < n; i++)
        put32(cis + 4 * i, chip_bp_rd(w, BCM4361_CIS_START + 4 * i));
    chip_select(w, CORE_PCIE2);
    return (int)(4 * n);
}
