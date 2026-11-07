/* SPDX-License-Identifier: GPL-2.0-only
 * bcmw: the bring-up, step by step (bcmw.h BCMW_STEP_*), and what the shims call. The steps follow
 * an earlier port's numbering (wifi_pcie=1..6, brcmfmac=1..7) so that a phone log of either can be read
 * against the other; each says what it does before it does it.
 *
 * The order of the firmware steps is brcmfmac's (pcie.c brcmf_pcie_setup and
 * brcmf_pcie_download_fw_nvram; common.c brcmf_c_preinit_dcmds), with the BCM4361's module chosen
 * from OTP as bcmdhd does it.
 */
#include "bcmw_priv.h"

/* ---- small things without a C library ------------------------------------------------------------ */

uint32_t bcmw_strlen(const char *s)
{
    uint32_t n = 0;
    while (s && s[n])
        n++;
    return n;
}

int bcmw_strneq(const char *a, const char *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return 0;
        if (!a[i])
            return 1;
    }
    return 1;
}

void bcmw_memcpy(void *d, const void *s, uint32_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    while (n--)
        *dp++ = *sp++;
}

int bcmw_memcmp(const void *a, const void *b, uint32_t n)
{
    const uint8_t *x = a, *y = b;
    for (uint32_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    return 0;
}

void bcmw_memset(void *d, int c, uint32_t n)
{
    uint8_t *dp = d;
    while (n--)
        *dp++ = (uint8_t)c;
}

void bcmw_strlcpy(char *d, const char *s, uint32_t cap)
{
    uint32_t i = 0;
    if (!cap)
        return;
    for (; s && s[i] && i + 1 < cap; i++)
        d[i] = s[i];
    d[i] = 0;
}

void lb_init(struct bcmw_lb *l) { l->n = 0; l->b[0] = 0; }

void lb_s(struct bcmw_lb *l, const char *s)
{
    while (s && *s && l->n + 1 < sizeof(l->b))
        l->b[l->n++] = *s++;
    l->b[l->n] = 0;
}

void lb_hex(struct bcmw_lb *l, uint64_t v, unsigned digits)
{
    char t[17];
    if (digits == 0 || digits > 16)
        digits = 16;
    for (unsigned i = 0; i < digits; i++)
        t[digits - 1 - i] = "0123456789abcdef"[(v >> (4 * i)) & 15];
    t[digits] = 0;
    lb_s(l, t);
}

void lb_dec(struct bcmw_lb *l, uint64_t v)
{
    char t[21];
    int i = 20;
    t[i] = 0;
    do {
        t[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v && i > 0);
    lb_s(l, t + i);
}

void lb_sdec(struct bcmw_lb *l, int64_t v)
{
    if (v < 0) {
        lb_s(l, "-");
        lb_dec(l, (uint64_t)(-(v + 1)) + 1);
    } else {
        lb_dec(l, (uint64_t)v);
    }
}

void bcmw_say(struct bcmw *w, const char *s)
{
    if (w->os.log)
        w->os.log(w->os.ctx, s);
}

void bcmw_say_lb(struct bcmw *w, struct bcmw_lb *l) { bcmw_say(w, l->b); }

void bcmw_sayx(struct bcmw *w, const char *a, uint64_t v, const char *b)
{
    struct bcmw_lb l;
    lb_init(&l);
    lb_s(&l, a);
    lb_hex(&l, v, v >> 32 ? 16 : 8);
    if (b) {
        if (b[0] != ' ' && b[0] != ')' && b[0] != ';' && b[0] != ',')
            lb_s(&l, " ");
        lb_s(&l, b);
    }
    bcmw_say_lb(w, &l);
}

/* ---- the OS, with the allow-list ------------------------------------------------------------------ */

uint32_t rd32(struct bcmw *w, uint64_t pa) { return w->os.rd32(w->os.ctx, pa); }

void wr32(struct bcmw *w, uint64_t pa, uint32_t v)
{
    if (!bcmw_write_allowed(pa)) {
        if (!w->dead)
            bcmw_sayx(w, "REFUSED: a write outside the allow-list, at ", pa, "; nothing more is attempted");
        w->dead = 1;
        return;
    }
    w->os.wr32(w->os.ctx, pa, v);
}

void set32(struct bcmw *w, uint64_t pa, uint32_t mask, uint32_t v)
{
    wr32(w, pa, (rd32(w, pa) & ~mask) | (v & mask));
}

void bcmw_udelay(struct bcmw *w, uint32_t us) { w->os.udelay(w->os.ctx, us); }

void bcmw_mdelay(struct bcmw *w, uint32_t ms)
{
    while (ms--)
        w->os.udelay(w->os.ctx, 1000);
}

uint64_t bcmw_now(struct bcmw *w) { return w->os.now_us(w->os.ctx); }

void *bcmw_dma(struct bcmw *w, uint32_t size, uint32_t align, uint64_t *bus)
{
    void *p = w->os.dma_alloc(w->os.ctx, size, align, bus);
    if (p && (*bus >> 32)) {
        bcmw_sayx(w, "DMA memory above 4 GiB refused: ", *bus, NULL);
        return 0;
    }
    return p;
}

void bcmw_sync(struct bcmw *w, const void *p, uint32_t len, int to_device)
{
    if (w->os.dma_sync)
        w->os.dma_sync(w->os.ctx, p, len, to_device);
}

void bcmw_wmb(struct bcmw *w)
{
    if (w->os.wmb)
        w->os.wmb(w->os.ctx);
}

void bcmw_mb(struct bcmw *w)
{
    if (w->os.mb)
        w->os.mb(w->os.ctx);
    else if (w->os.wmb)
        w->os.wmb(w->os.ctx);
}

void bcmw_rmb(struct bcmw *w)
{
    if (w->os.rmb)
        w->os.rmb(w->os.ctx);
    else
        bcmw_mb(w);
}

/* ---- the device ----------------------------------------------------------------------------------- */

uint32_t bcmw_size(void) { return sizeof(struct bcmw); }

int bcmw_init(struct bcmw *w, const struct bcmw_os *os, const struct bcmw_cfg *cfg)
{
    bcmw_memset(w, 0, sizeof(*w));
    w->os = *os;
    if (cfg)
        w->cfg = *cfg;
    if (!w->cfg.stop_after)
        w->cfg.stop_after = BCMW_STEP_RINGS;
    if (!w->cfg.link_speed)
        w->cfg.link_speed = 2;
    if (!w->cfg.rx_buffers)
        w->cfg.rx_buffers = 64;
    if (w->cfg.rx_buffers > RX_BUFS_MAX)
        w->cfg.rx_buffers = RX_BUFS_MAX;
    w->console_quota = 48;
    if (!os->rd32 || !os->wr32 || !os->udelay || !os->now_us || !os->dma_alloc || !os->file)
        return BCMW_E_STATE;
    return BCMW_OK;
}

unsigned bcmw_step(const struct bcmw *w) { return w->step; }
const struct bcmw_info *bcmw_info(const struct bcmw *w) { return &w->info; }

/* ---- step 7: the module ----------------------------------------------------------------------------- */

static void choose_files(struct bcmw *w)
{
    uint8_t cis[88];
    struct bcm4361_module m;
    const char *tag = fw_rev_tag(w->info.chiprev);
    int n = chip_read_cis(w, cis, sizeof(cis));
    struct bcmw_lb l;

    if (fw_cis_parse(cis, (uint32_t)n, &m) && m.vid_length >= 2) {
        w->info.module_vid[0] = m.vid[0];
        w->info.module_vid[1] = m.vid[1];
    }
    fw_module_files(&m, tag);
    bcmw_strlcpy(w->info.nvram_name, w->cfg.nvram ? w->cfg.nvram : m.nvram, sizeof(w->info.nvram_name));
    bcmw_strlcpy(w->info.fw_name, w->cfg.firmware ? w->cfg.firmware :
                 tag && tag[1] == '2' ? "bcmdhd_sta.bin_b2" : "bcmdhd_sta.bin_b0", sizeof(w->info.fw_name));
    lb_init(&l);
    lb_s(&l, "chip: 7: the module's ID ");
    if (m.vid_length >= 2) {
        lb_hex(&l, m.vid[0], 2);
        lb_s(&l, " ");
        lb_hex(&l, m.vid[1], 2);
        lb_s(&l, " (");
        lb_s(&l, m.maker[0] ? m.maker : "unknown maker");
        lb_s(&l, ")");
    } else {
        lb_s(&l, "NOT IN THE CIS (first words ");
        lb_hex(&l, le32(cis), 8);
        lb_s(&l, ")");
    }
    lb_s(&l, ": NVRAM ");
    lb_s(&l, w->info.nvram_name[0] ? w->info.nvram_name : "(none chosen)");
    lb_s(&l, w->cfg.nvram ? " (given)" : m.known ? " (as bcmdhd chooses)" : " (the maker's only one)");
    lb_s(&l, ", firmware ");
    lb_s(&l, w->info.fw_name);
    bcmw_say_lb(w, &l);
}

/* ---- step 8: firmware and NVRAM ------------------------------------------------------------------- */

/* the file's SHA-256 against the known ones: 1 to go on */
static int file_known(struct bcmw *w, const char *name, const uint8_t *data, uint32_t len)
{
    uint8_t sum[32];
    const char *what;
    struct bcmw_lb l;

    fw_sha256(data, len, sum);
    what = fw_known(sum);
    lb_init(&l);
    lb_s(&l, "fw: 8: ");
    lb_s(&l, name);
    lb_s(&l, " sha256 ");
    for (int i = 0; i < 8; i++)
        lb_hex(&l, sum[i], 2);
    lb_s(&l, "...: ");
    lb_s(&l, what ? what : w->cfg.any_files ? "NOT a known file (any_files=1: used)" : "NOT a known file: REFUSED");
    bcmw_say_lb(w, &l);
    return what || w->cfg.any_files;
}

static int load_nvram(struct bcmw *w, uint32_t *nvlen)
{
    static const char *const guesses[] = { "nvram.txt_r02a_b2", "nvram.txt_r02b_b2", "nvram.txt_murata_r041_b2" };
    const uint8_t *text = 0;
    uint32_t len = 0;
    int have_mac = 0, n;

    for (int i = 0; i < 6; i++)
        have_mac |= w->cfg.mac[i];
    if (w->info.nvram_name[0])
        w->os.file(w->os.ctx, w->info.nvram_name, &text, &len);
    for (unsigned i = 0; !text && i < 3; i++)
        if (w->os.file(w->os.ctx, guesses[i], &text, &len) == 0) {
            bcmw_strlcpy(w->info.nvram_name, guesses[i], sizeof(w->info.nvram_name));
            bcmw_say(w, "fw: 8: the module's NVRAM is not there: taking the first of the vendor's B2 NVRAMs (A GUESS)");
        }
    if (!text) {
        bcmw_say(w, "fw: 8: no NVRAM file");
        return BCMW_E_BADFILE;
    }
    if (!file_known(w, w->info.nvram_name, text, len))
        return BCMW_E_BADFILE;
    n = fw_nvram(text, len, w->nvram_bin, sizeof(w->nvram_bin), have_mac ? w->cfg.mac : 0);
    if (n <= 0) {
        bcmw_say(w, "fw: 8: the NVRAM does not parse");
        return BCMW_E_BADFILE;
    }
    *nvlen = (uint32_t)n;
    {
        struct bcmw_lb l;
        const char *br = fw_nvram_get(w->nvram_bin, (uint32_t)n, "boardrev");
        lb_init(&l);
        lb_s(&l, "fw: 8: ");
        lb_s(&l, w->info.nvram_name);
        lb_s(&l, ": ");
        lb_dec(&l, len);
        lb_s(&l, " bytes of text, ");
        lb_dec(&l, (uint32_t)n);
        lb_s(&l, " for the chip; boardrev ");
        lb_s(&l, br ? br : "?");
        lb_s(&l, have_mac ? "; macaddr replaced by the phone's own" : "");
        bcmw_say_lb(w, &l);
    }
    return BCMW_OK;
}

static int step_firmware(struct bcmw *w)
{
    const uint8_t *fw;
    uint32_t fwlen, nvlen, written, a, ramsize;
    struct fw_image_info fi;
    uint8_t back[64];
    uint64_t t0;
    int e;

    if (w->os.file(w->os.ctx, w->info.fw_name, &fw, &fwlen) || fw_image_check(fw, fwlen, &fi)) {
        bcmw_say(w, "fw: 8: the firmware file is not there or not a firmware");
        return BCMW_E_BADFILE;
    }
    {
        struct bcmw_lb l;
        lb_init(&l);
        lb_s(&l, "fw: 8: ");
        lb_s(&l, w->info.fw_name);
        lb_s(&l, ": ");
        lb_dec(&l, fwlen);
        lb_s(&l, " bytes, ");
        lb_s(&l, fi.version[0] ? fi.version : "no version banner");
        bcmw_say_lb(w, &l);
    }
    if (!file_known(w, w->info.fw_name, fw, fwlen))
        return BCMW_E_BADFILE;
    if ((e = load_nvram(w, &nvlen)) != BCMW_OK)
        return e;
    if (w->os.file(w->os.ctx, "bcmdhd_clm.blob", &w->clm, &w->clm_len) || !fw_clm_check(w->clm, w->clm_len)) {
        bcmw_say(w, "fw: 8: no CLM blob (bcmdhd_clm.blob): the radio would have no regulatory data; going on");
        w->clm = 0;
    } else if (!file_known(w, "bcmdhd_clm.blob", w->clm, w->clm_len)) {
        w->clm = 0;
    }
    ramsize = fi.ramsize_override ? fi.ramsize_override : w->info.ramsize;
    if (fi.ramsize_override)
        bcmw_sayx(w, "fw: 8: the firmware sets the RAM size: ", ramsize, NULL);
    w->info.ramsize = ramsize;
    if (fwlen + nvlen + 0x1000 > ramsize) {
        bcmw_say(w, "fw: 8: the firmware and NVRAM do not fit the chip's RAM");
        return BCMW_E_BADFILE;
    }
    bcmw_say(w, "fw: 8: the ARM held; the firmware into the chip's RAM through BAR1");
    chip_set_passive(w);
    tcm_write(w, w->info.rambase, fw, fwlen);
    tcm_read(w, w->info.rambase, back, sizeof(back));
    for (unsigned i = 0; i < sizeof(back); i++)
        if (back[i] != fw[i]) {
            bcmw_sayx(w, "fw: 8: THE RAM DOES NOT READ BACK what was written, at byte ", i, NULL);
            return BCMW_E_PROTO;
        }
    tcm_wr32(w, w->info.rambase + ramsize - 4, 0);
    a = w->info.rambase + ramsize - nvlen;
    tcm_write(w, a, w->nvram_bin, nvlen);
    written = tcm_rd32(w, w->info.rambase + ramsize - 4);
    bcmw_sayx(w, "fw: 8: the NVRAM at ", a, "; the ARM let go from its reset vector");
    chip_set_active(w, fi.rstvec);
    t0 = bcmw_now(w);
    for (;;) {
        uint32_t s;
        bcmw_mdelay(w, 50);
        s = tcm_rd32(w, w->info.rambase + ramsize - 4);
        if (s != written) {
            struct bcmw_lb l;
            lb_init(&l);
            lb_s(&l, "fw: 8: the firmware is up after ");
            lb_dec(&l, (bcmw_now(w) - t0) / 1000);
            lb_s(&l, " ms: shared area at ");
            lb_hex(&l, s, 8);
            bcmw_say_lb(w, &l);
            if (s < w->info.rambase || s >= w->info.rambase + ramsize) {
                bcmw_say(w, "fw: 8: an invalid shared RAM address");
                return BCMW_E_PROTO;
            }
            return mb_shared(w, s);
        }
        if (bcmw_now(w) - t0 > 5000000) {
            bcmw_say(w, "fw: 8: the firmware did not start in 5 s (no shared area)");
            return BCMW_E_TIMEOUT;
        }
    }
}

/* ---- step 9: rings, buffers, the first commands ------------------------------------------------- */

static int load_clm(struct bcmw *w)
{
    uint8_t chunk[12 + 1400];
    uint32_t off = 0, status = 0;
    int e = BCMW_OK, fwerr = 0;

    if (!w->clm)
        return BCMW_OK;
    bcmw_sayx(w, "fw: 9: the CLM blob in chunks of 1400 (clmload), bytes ", w->clm_len, NULL);
    while (off < w->clm_len && e == BCMW_OK) {
        uint32_t n = fw_clm_chunk(chunk, w->clm, off, w->clm_len, 1400);
        e = bcmw_iovar_set(w, "clmload", chunk, n, &fwerr);
        off += n - 12;
    }
    bcmw_iovar_get_u32(w, "clmload_status", &status);
    {
        struct bcmw_lb l;
        lb_init(&l);
        lb_s(&l, "fw: 9: CLM ");
        lb_s(&l, e == BCMW_OK ? "loaded" : "NOT LOADED");
        lb_s(&l, ", clmload_status ");
        lb_dec(&l, status);
        if (e != BCMW_OK) {
            lb_s(&l, ", firmware error ");
            lb_sdec(&l, fwerr);
        }
        bcmw_say_lb(w, &l);
    }
    return e;
}

static int step_rings(struct bcmw *w)
{
    int e, fwerr;
    uint8_t mac[6];
    int have_mac = 0;

    /* DMA memory once (a later bring-up after bcmw_quiesce reuses it); nothing is posted yet */
    if (!w->ioctl_req)
        w->ioctl_req = bcmw_dma(w, 2048, 64, &w->ioctl_req_bus);
    if (!w->ctl)
        w->ctl = bcmw_dma(w, CTL_BUFS * CTL_BUF_SIZE, 4096, &w->ctl_bus);
    if (!w->rxb)
        w->rxb = bcmw_dma(w, w->cfg.rx_buffers * RX_BUF_SIZE, 4096, &w->rxb_bus);
    w->nrx = w->cfg.rx_buffers < w->info.max_rxbufpost ? w->cfg.rx_buffers : w->info.max_rxbufpost;
    bcmw_memset(w->ctl_posted, 0, sizeof(w->ctl_posted));
    bcmw_memset(w->rx_posted, 0, sizeof(w->rx_posted));
    w->ioctl_done = 0;
    if (!w->ioctl_req || !w->ctl || !w->rxb)
        return BCMW_E_NOMEM;
    if ((e = mb_rings(w)) != BCMW_OK)
        return e;
    chip_select(w, CORE_PCIE2);
    chip_reg_wr(w, w->mb_mask, w->int_d2h_db | w->int_fn0);
    if (w->flags & SHARED_HOSTRDY_DB1) {
        bcmw_say(w, "msgbuf: 9: host ready (doorbell 1)");
        chip_reg_wr(w, w->h2d_mb1, 1);
    }
    if ((e = mb_post_all(w)) != BCMW_OK)
        return e;
    bcmw_say(w, "msgbuf: 9: the first ioctl: cur_etheraddr (the chip reads and writes our memory for it)");
    bcmw_memset(mac, 0, sizeof(mac));
    e = bcmw_iovar_get(w, "cur_etheraddr", mac, 6, &fwerr);
    if (e != BCMW_OK) {
        bcmw_sayx(w, "msgbuf: 9: cur_etheraddr failed; the firmware's error ", (uint32_t)fwerr, NULL);
        return e;
    }
    for (int i = 0; i < 6; i++)
        have_mac |= w->cfg.mac[i];
    if (have_mac) {
        e = bcmw_iovar_set(w, "cur_etheraddr", w->cfg.mac, 6, &fwerr);
        bcmw_say(w, e == BCMW_OK ? "msgbuf: 9: the phone's own address set" : "msgbuf: 9: setting the phone's address FAILED");
        bcmw_memcpy(mac, w->cfg.mac, 6);
    }
    bcmw_memcpy(w->info.mac, mac, 6);
    /* the address is the phone's identity: it is never logged, not even in part */
    bcmw_say(w, have_mac ? "msgbuf: 9: the radio uses the phone's own address (not shown)"
                         : "msgbuf: 9: the radio keeps the firmware's address (not shown)");
    bcmw_memset(w->info.fw_version, 0, sizeof(w->info.fw_version));
    if (bcmw_iovar_get(w, "ver", w->info.fw_version, sizeof(w->info.fw_version) - 1, 0) == BCMW_OK) {
        struct bcmw_lb l;
        for (unsigned i = 0; i < sizeof(w->info.fw_version); i++)
            if (w->info.fw_version[i] == '\n' || w->info.fw_version[i] == '\r')
                w->info.fw_version[i] = 0;
        lb_init(&l);
        lb_s(&l, "msgbuf: 9: Firmware: ");
        lb_s(&l, w->info.fw_version);
        bcmw_say_lb(w, &l);
    }
    load_clm(w);
    return BCMW_OK;
}

/* ---- step 10: up and a scan ------------------------------------------------------------------------- */

/* escan parameters into p (80 bytes): version (v2 when the firmware answered scan_ver), start, sync id; a name
 * (or any), bssid broadcast, any BSS type, active, the firmware's times, every channel. Returns the length. */
uint32_t bcmw_escan_params(struct bcmw *w, uint8_t *p, const uint8_t *ssid, unsigned ssid_len, uint16_t sync)
{
    unsigned o = w->scan_v2 ? 4 : 0;        /* v2: version and length before the name */
    bcmw_memset(p, 0, 80);
    put32(p, w->scan_v2 ? 2 : 1);
    put16(p + 4, 1);
    put16(p + 6, sync);
    if (w->scan_v2) {
        put16(p + 8, 2);                    /* BRCMF_SCAN_PARAMS_VERSION_V2 */
        put16(p + 10, 72);                  /* BRCMF_SCAN_PARAMS_V2_FIXED_SIZE */
    }
    put32(p + 8 + o, ssid_len);
    if (ssid_len)
        bcmw_memcpy(p + 12 + o, ssid, ssid_len);
    bcmw_memset(p + 8 + o + 36, 0xff, 6);
    p[8 + o + 42] = 2;                      /* any BSS type */
    /* v1: scan_type (u8) at 43; v2: a pad byte, then scan_type (u32, 0 active) at 48: zero either way */
    for (unsigned i = 0; i < 4; i++)
        put32(p + 8 + (w->scan_v2 ? 52 : 44) + 4 * i, 0xffffffffu);
    return w->scan_v2 ? 80 : 72;
}

static int step_up(struct bcmw *w)
{
    uint8_t mask[16], p[80];
    uint32_t v = 0, n;
    int fwerr = 0, e, v2;

    bcmw_memset(mask, 0, sizeof(mask));
    if (bcmw_iovar_get(w, "event_msgs", mask, sizeof(mask), &fwerr) == BCMW_OK) {
        static const uint8_t ev[] = { BCMW_E_SET_SSID, BCMW_E_JOIN, BCMW_E_AUTH, BCMW_E_DEAUTH, BCMW_E_DEAUTH_IND,
                                      BCMW_E_ASSOC, BCMW_E_DISASSOC_IND, BCMW_E_LINK, BCMW_E_IF, BCMW_E_ESCAN_RESULT,
                                      46 /* PSK_SUP */ };
        for (unsigned i = 0; i < sizeof(ev); i++)
            mask[ev[i] / 8] |= (uint8_t)(1u << (ev[i] % 8));
        bcmw_iovar_set(w, "event_msgs", mask, sizeof(mask), &fwerr);
    }
    bcmw_iovar_set_u32(w, "mpc", 0);
    v = 0;
    e = bcmw_ioctl(w, 0, BCMW_C_UP, &v, 4, 1, &fwerr);
    bcmw_sayx(w, "fw: 10: up: ", (uint32_t)e, e == BCMW_OK ? "(fine)" : "(FAILED)");
    if (e != BCMW_OK)
        return e;
    /* brcmfmac's rule (feature.c, cfg80211.c): the v2 parameters when the firmware answers "scan_ver" at all */
    v2 = bcmw_iovar_get_u32(w, "scan_ver", &v) == BCMW_OK;
    bcmw_sayx(w, "fw: 10: scan_ver ", v2 ? v : 0xffffffffu, v2 ? "(the v2 escan parameters)" : "(unsupported: v1 parameters)");
    w->scan_v2 = v2;
    n = bcmw_escan_params(w, p, 0, 0, 0x1234);
    e = bcmw_iovar_set(w, "escan", p, n, &fwerr);
    bcmw_sayx(w, "fw: 10: a scan asked for (escan): ", (uint32_t)e, e == BCMW_OK ? "(the results come as events)" : "(REFUSED)");
    return e;
}

/* ---- the bring-up ----------------------------------------------------------------------------------- */

int bcmw_bringup(struct bcmw *w, unsigned upto)
{
    unsigned stop = upto && upto < w->cfg.stop_after ? upto : w->cfg.stop_after;
    int e = BCMW_OK;

    while (w->step < stop && !w->dead && e >= 0) {
        unsigned s = w->step + 1;
        switch (s) {
        case BCMW_STEP_LOOK:
            ex_look(w);
            break;
        case BCMW_STEP_POWER:
            if (!ex_power(w))
                e = BCMW_E_NODEV;
            break;
        case BCMW_STEP_PHY:
            if (!ex_phy(w))
                e = BCMW_E_REFUSED;
            break;
        case BCMW_STEP_LINK:
            if (!ex_link(w))
                e = BCMW_E_NODEV;
            break;
        case BCMW_STEP_ENUM:
            if (!ex_enumerate(w))
                e = BCMW_E_NODEV;
            break;
        case BCMW_STEP_DECODE:
            ex_decode(w);
            break;
        case BCMW_STEP_CHIP:
            if ((e = chip_attach(w)) == BCMW_OK)
                choose_files(w);
            break;
        case BCMW_STEP_FIRMWARE:
            e = step_firmware(w);
            break;
        case BCMW_STEP_RINGS:
            e = step_rings(w);
            break;
        case BCMW_STEP_UP:
            e = step_up(w);
            break;
        }
        if (w->dead && e >= 0)
            e = BCMW_E_REFUSED;
        if (e < 0) {
            bcmw_sayx(w, "bcmw: STOPPED at step ", s, NULL);
            if (s >= BCMW_STEP_FIRMWARE) {
                w->console_quota += 32;
                mb_console(w);
            }
            break;
        }
        w->step = s;
    }
    if (e < 0)
        return e;
    if (w->step < w->cfg.stop_after && !upto)
        return BCMW_E_STATE;
    bcmw_sayx(w, "bcmw: done up to step ", w->step, NULL);
    return (int)w->step;
}

int bcmw_poll(struct bcmw *w)
{
    if (w->step < BCMW_STEP_RINGS || w->dead)
        return 0;
    {
        int n = mb_process(w);
        chip_reg_wr(w, w->mb_mask, w->int_d2h_db | w->int_fn0);
        return n;
    }
}

int bcmw_isr(struct bcmw *w)
{
    uint32_t st;
    if (w->step < BCMW_STEP_RINGS || w->dead)
        return 0;
    ex_intx_ack(w);
    chip_select(w, CORE_PCIE2);
    st = chip_reg_rd(w, w->mb_int);
    if (!st || st == 0xffffffffu)
        return 0;
    chip_reg_wr(w, w->mb_mask, 0);
    w->info.interrupts++;
    return 1;
}

void bcmw_quiesce(struct bcmw *w)
{
    if (w->step >= BCMW_STEP_RINGS && !w->dead && w->linked) {
        chip_select(w, CORE_PCIE2);
        chip_reg_wr(w, w->mb_mask, 0);
    }
    ex_quiesce(w);
    w->step = 0;
    w->flow_state = 0;
    w->join_phase = 0;
    w->link_up = w->keyed = w->keys_due = 0;
    bcmw_memset(w->tx_busy, 0, sizeof(w->tx_busy));
    bcmw_memset(&w->wpa, 0, sizeof(w->wpa));
}

/* ---- commands --------------------------------------------------------------------------------------- */

int bcmw_ioctl(struct bcmw *w, unsigned ifidx, uint32_t cmd, void *buf, uint32_t len, int set, int *fwerr)
{
    (void)set;      /* msgbuf has one request for both (brcmf_msgbuf_set_dcmd) */
    if (w->step < BCMW_STEP_FIRMWARE || !w->ioctl_req)
        return BCMW_E_STATE;
    return mb_ioctl(w, ifidx, cmd, buf, len, fwerr);
}

int bcmw_iovar_get(struct bcmw *w, const char *name, void *buf, uint32_t len, int *fwerr)
{
    uint8_t t[IOCTL_REQ_SIZE];
    uint32_t n = fw_iovar(t, sizeof(t), name, 0, 0);
    uint32_t want = len > n ? len : n;
    int e;
    if (!n || want > sizeof(t))
        return BCMW_E_NOMEM;
    bcmw_memset(t + n, 0, want - n);
    e = bcmw_ioctl(w, 0, BCMW_C_GET_VAR, t, want, 0, fwerr);
    if (e == BCMW_OK)
        bcmw_memcpy(buf, t, len);
    return e;
}

int bcmw_iovar_set(struct bcmw *w, const char *name, const void *buf, uint32_t len, int *fwerr)
{
    uint8_t t[IOCTL_REQ_SIZE];
    uint32_t n = fw_iovar(t, sizeof(t), name, buf, len);
    if (!n)
        return BCMW_E_NOMEM;
    return bcmw_ioctl(w, 0, BCMW_C_SET_VAR, t, n, 1, fwerr);
}

int bcmw_iovar_get_u32(struct bcmw *w, const char *name, uint32_t *v)
{
    uint8_t b[4] = { 0 };
    int e = bcmw_iovar_get(w, name, b, 4, 0);
    *v = le32(b);
    return e;
}

int bcmw_iovar_set_u32(struct bcmw *w, const char *name, uint32_t v)
{
    uint8_t b[4];
    put32(b, v);
    return bcmw_iovar_set(w, name, b, 4, 0);
}
