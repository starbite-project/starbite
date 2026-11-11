/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteUFS host controller core (sbufs_hc.h). The take-over, the wake-up and the multi-block self test follow
 * what build/xnu-12377/bsd/dev/herolte_ufs.c (Starbite, patches 0204/0300) proved on the S9; the slot state machine,
 * the doorbell audit and the fence are new. Facts and sources: NOTES.md.
 */
#include "sbufs_hc.h"

#define SAY(hc, ...) do { \
	if ((hc)->ops.log != NULL) { \
	        snprintf((hc)->line, sizeof((hc)->line), __VA_ARGS__); \
	        (hc)->ops.log((hc)->ops.ctx, (hc)->line); \
	} \
} while (0)

static inline uint32_t rd(struct sbufs_hc *hc, unsigned bank, uint32_t off) { return hc->ops.rd(hc->ops.ctx, bank, off); }
static inline void wr(struct sbufs_hc *hc, unsigned bank, uint32_t off, uint32_t v) { hc->ops.wr(hc->ops.ctx, bank, off, v); }
static inline void barrier(struct sbufs_hc *hc) { if (hc->ops.barrier) hc->ops.barrier(hc->ops.ctx); }
static inline void clean(struct sbufs_hc *hc, uint32_t off, uint32_t len) { if (hc->ops.dma_clean) hc->ops.dma_clean(hc->ops.ctx, off, len); }
static inline void inval(struct sbufs_hc *hc, uint32_t off, uint32_t len) { if (hc->ops.dma_inval) hc->ops.dma_inval(hc->ops.ctx, off, len); }

static uint32_t utrd_off(unsigned slot) { return SBUFS_DMA_UTRL + slot * 32u; }
static uint32_t ucd_off(unsigned slot) { return SBUFS_DMA_UCD + slot * SBUFS_UCD_SIZE; }
static uint32_t bounce_off(unsigned slot) { return SBUFS_DMA_BOUNCE + slot * SBUFS_BOUNCE_SIZE; }

int
sbufs_hc_init(struct sbufs_hc *hc, const struct sbufs_hw_ops *ops, const struct sbufs_cfg *cfg, uint8_t *dma_va,
    uint64_t dma_pa, size_t dma_size, struct sbufs_fence *fence)
{
	memset(hc, 0, sizeof(*hc));
	hc->ops = *ops;
	hc->cfg = *cfg;
	if (hc->cfg.nslots == 0 || hc->cfg.nslots > SBUFS_MAX_SLOTS || hc->cfg.timeout_us == 0 || dma_va == NULL ||
	    fence == NULL || ops->rd == NULL || ops->wr == NULL || ops->now_us == NULL || ops->delay_us == NULL) {
		return SBUFS_E_INVAL;
	}
	if ((dma_pa & 0xfff) != 0 || dma_size < SBUFS_DMA_SIZE(hc->cfg.nslots) ||
	    (!cfg->dma64 && dma_pa + dma_size > (1ull << 32))) {
		return SBUFS_E_DMA;
	}
	hc->dma = dma_va;
	hc->dma_pa = dma_pa;
	hc->dma_size = dma_size;
	hc->fence = fence;
	sbufs_layout_s9(&hc->L, 1, cfg->dma64);         /* one 4 KiB entry per command until the self test says more */
	return SBUFS_OK;
}

int
sbufs_hc_take_over(struct sbufs_hc *hc)
{
	hc->cap = rd(hc, SBUFS_BANK_HCI, SBUFS_CAP);
	hc->ver = rd(hc, SBUFS_BANK_HCI, SBUFS_VER);
	hc->hcs = rd(hc, SBUFS_BANK_HCI, SBUFS_HCS);
	hc->hce = rd(hc, SBUFS_BANK_HCI, SBUFS_HCE);
	SAY(hc, "host: cap 0x%x ver 0x%x hcs 0x%x hce 0x%x utrlba 0x%x:%08x dbr 0x%x rsr 0x%x nexus 0x%x",
	    hc->cap, hc->ver, hc->hcs, hc->hce, rd(hc, SBUFS_BANK_HCI, SBUFS_UTRLBAU), rd(hc, SBUFS_BANK_HCI, SBUFS_UTRLBA),
	    rd(hc, SBUFS_BANK_HCI, SBUFS_UTRLDBR), rd(hc, SBUFS_BANK_HCI, SBUFS_UTRLRSR),
	    rd(hc, SBUFS_BANK_VS, SBUFS_VS_UTRL_NEXUS_TYPE));
	/* Exynos 9810 after S-Boot: HCE reads 0 while HCS reports the device present and the list ready (phone,
	 * 2026-10-25). Go by HCS. */
	if (!(hc->hcs & SBUFS_HCS_DP) || !(hc->hcs & SBUFS_HCS_UTRLRDY)) {
		SAY(hc, "host not ready (hcs 0x%x): not taken", hc->hcs);
		return SBUFS_E_NODEV;
	}
	if (rd(hc, SBUFS_BANK_HCI, SBUFS_UTRLDBR) != 0) {
		SAY(hc, "requests outstanding from the bootloader: not taken");
		return SBUFS_E_NODEV;
	}
	if (hc->cfg.nslots > SBUFS_CAP_NUTRS(hc->cap)) {
		hc->cfg.nslots = SBUFS_CAP_NUTRS(hc->cap);
	}
	if (hc->cfg.dma64 && !(hc->cap & SBUFS_CAP_64AS)) {
		hc->cfg.dma64 = false;
		hc->L.dma_limit = 1ull << 32;
	}
	if (hc->dma_pa + hc->dma_size > (1ull << 32) && !(hc->cap & SBUFS_CAP_64AS)) {
		SAY(hc, "DMA area at 0x%llx but no 64-bit addressing: not taken", (unsigned long long)hc->dma_pa);
		return SBUFS_E_DMA;
	}
	memset(hc->dma + SBUFS_DMA_UTRL, 0, 32u * 32u);
	clean(hc, SBUFS_DMA_UTRL, 32u * 32u);
	wr(hc, SBUFS_BANK_HCI, SBUFS_IE, 0);
	wr(hc, SBUFS_BANK_HCI, SBUFS_AHIT, 0);          /* auto-hibernate drops completions on the 9810 */
	wr(hc, SBUFS_BANK_HCI, SBUFS_IS, rd(hc, SBUFS_BANK_HCI, SBUFS_IS));
	wr(hc, SBUFS_BANK_HCI, SBUFS_UTRLRSR, 0);
	uint64_t list = hc->dma_pa + SBUFS_DMA_UTRL;
	wr(hc, SBUFS_BANK_HCI, SBUFS_UTRLBA, (uint32_t)list);
	wr(hc, SBUFS_BANK_HCI, SBUFS_UTRLBAU, (uint32_t)(list >> 32));
	wr(hc, SBUFS_BANK_HCI, SBUFS_UTRLRSR, 1);
	barrier(hc);
	/* never ring a doorbell unless the host reads OUR list */
	if (rd(hc, SBUFS_BANK_HCI, SBUFS_UTRLBA) != (uint32_t)list ||
	    rd(hc, SBUFS_BANK_HCI, SBUFS_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(rd(hc, SBUFS_BANK_HCI, SBUFS_UTRLRSR) & 1)) {
		SAY(hc, "host did not take our request list");
		return SBUFS_E_NODEV;
	}
	for (unsigned i = 0; i < 4; i++) {
		static const uint32_t vs[4] = { SBUFS_VS_TXPRDT_ENTRY_SIZE, SBUFS_VS_RXPRDT_ENTRY_SIZE,
			                        SBUFS_VS_DATA_REORDER, SBUFS_VS_AXIDMA_RWDATA_BURST };
		hc->vs_saved[i] = rd(hc, SBUFS_BANK_VS, vs[i]);
	}
	hc->ready = true;
	SAY(hc, "host taken: list at 0x%llx, %u slots, %s DMA, %s", (unsigned long long)list, hc->cfg.nslots,
	    hc->cfg.dma64 ? "64-bit" : "32-bit", hc->cfg.irq ? "interrupts" : "polled");
	return SBUFS_OK;
}

void
sbufs_hc_enable_irq(struct sbufs_hc *hc, bool on)
{
	hc->cfg.irq = on;
	wr(hc, SBUFS_BANK_HCI, SBUFS_IS, rd(hc, SBUFS_BANK_HCI, SBUFS_IS));
	wr(hc, SBUFS_BANK_HCI, SBUFS_IE, on ? SBUFS_IE_WANTED : 0);
}

int
sbufs_hc_alloc_slot(struct sbufs_hc *hc)
{
	if (!hc->ready || hc->dead) {
		return SBUFS_E_NODEV;
	}
	for (unsigned s = 0; s < hc->cfg.nslots; s++) {
		if (!hc->slot[s].busy) {
			hc->slot[s].busy = true;
			hc->slot[s].cookie = NULL;
			return (int)s;
		}
	}
	return SBUFS_E_BUSY;
}

void
sbufs_hc_free_slot(struct sbufs_hc *hc, unsigned slot)
{
	if (slot < SBUFS_MAX_SLOTS && !(hc->inflight & (1u << slot))) {
		hc->slot[slot].busy = false;
	}
}

uint8_t *
sbufs_hc_bounce_va(struct sbufs_hc *hc, unsigned slot)
{
	return hc->dma + bounce_off(slot);
}

uint64_t
sbufs_hc_bounce_pa(struct sbufs_hc *hc, unsigned slot)
{
	return hc->dma_pa + bounce_off(slot);
}

uint32_t
sbufs_hc_max_xfer(const struct sbufs_hc *hc)
{
	return hc->L.max_entries * SBUFS_BLOCK;
}

int
sbufs_hc_issue(struct sbufs_hc *hc, unsigned slot, const struct sbufs_cmd *cmd, const struct sbufs_seg *segs,
    unsigned nsegs, void *cookie)
{
	struct sbufs_seg bseg[SBUFS_BOUNCE_SIZE / SBUFS_PRD_MAX_BYTES];
	struct sbufs_decoded d;
	uint8_t *utrd = hc->dma + utrd_off(slot), *ucd = hc->dma + ucd_off(slot);
	bool bounce = false;
	int err;

	if (slot >= hc->cfg.nslots || !hc->slot[slot].busy || (hc->inflight & (1u << slot))) {
		return SBUFS_E_INVAL;
	}
	if (!hc->ready || hc->dead) {
		hc->slot[slot].busy = false;
		return SBUFS_E_NODEV;
	}
	if (cmd->xfer_len != 0 && segs == NULL) {
		if (cmd->xfer_len > SBUFS_BOUNCE_SIZE) {
			hc->slot[slot].busy = false;
			return SBUFS_E_INVAL;
		}
		nsegs = 0;
		for (uint32_t done = 0; done < cmd->xfer_len; done += SBUFS_PRD_MAX_BYTES) {
			uint32_t left = cmd->xfer_len - done;
			bseg[nsegs].pa = sbufs_hc_bounce_pa(hc, slot) + done;
			bseg[nsegs].len = left < SBUFS_PRD_MAX_BYTES ? left : SBUFS_PRD_MAX_BYTES;
			nsegs++;
		}
		segs = bseg;
		bounce = true;
	}
	err = sbufs_build(&hc->L, utrd, ucd, hc->dma_pa + ucd_off(slot), cmd, (uint8_t)slot, segs, nsegs, hc->cfg.irq);
	if (err == SBUFS_OK) {
		err = sbufs_gate(&hc->L, utrd, ucd, hc->fence, &d);
		if (err != SBUFS_OK) {
			hc->ngate++;
			SAY(hc, "fence: AUDIT refused slot %u lun 0x%x op 0x%02x lba 0x%llx n %llu: %s", slot, d.lun, d.opcode,
			    (unsigned long long)d.lba, (unsigned long long)d.nblks, d.why);
		}
	}
	if (err != SBUFS_OK) {
		memset(utrd, 0, 32);
		hc->slot[slot].busy = false;
		return err;
	}
	clean(hc, utrd_off(slot), 32);
	clean(hc, ucd_off(slot), hc->L.prdt_off + nsegs * hc->L.stride);
	if (bounce) {
		/* a write's data out of the cache; a read's lines cleaned so no dirty line lands on the DMA'd data */
		clean(hc, bounce_off(slot), cmd->xfer_len);
	}
	if (hc->cfg.nexus) {
		wr(hc, SBUFS_BANK_VS, SBUFS_VS_UTRL_NEXUS_TYPE, rd(hc, SBUFS_BANK_VS, SBUFS_VS_UTRL_NEXUS_TYPE) | (1u << slot));
	}
	hc->slot[slot].bounce = bounce;
	hc->slot[slot].cmd = *cmd;
	hc->slot[slot].cookie = cookie;
	hc->slot[slot].deadline = hc->ops.now_us(hc->ops.ctx) + hc->cfg.timeout_us;
	hc->inflight |= 1u << slot;
	barrier(hc);
	wr(hc, SBUFS_BANK_HCI, SBUFS_UTRLDBR, 1u << slot);
	hc->ncmds++;
	/* the fence log's own count, independent of the gate's verdict: what was rung, decoded again */
	if (d.data && sbufs_fence_check_quiet(hc->fence, d.lun, d.lba, d.nblks, d.write) != SBUFS_FENCE_OK) {
		hc->foreign_touched++;
	}
	return SBUFS_OK;
}

static void
finish(struct sbufs_hc *hc, unsigned s, struct sbufs_result *r, sbufs_done_fn done, void *ctx)
{
	void *cookie = hc->slot[s].cookie;
	hc->inflight &= ~(1u << s);
	hc->slot[s].busy = false;
	if (r->err != SBUFS_OK) {
		hc->nerr++;
	}
	if (done != NULL) {
		done(ctx, s, cookie, r);
	}
}

unsigned
sbufs_hc_poll(struct sbufs_hc *hc, bool from_irq, sbufs_done_fn done, void *ctx)
{
	unsigned n = 0;
	struct sbufs_result r;

	if (!hc->ready) {
		return 0;
	}
	uint32_t is = rd(hc, SBUFS_BANK_HCI, SBUFS_IS);
	if (is != 0) {
		wr(hc, SBUFS_BANK_HCI, SBUFS_IS, is);
		hc->last_is = is;
	}
	if (is & SBUFS_IS_UE) {
		hc->nue++;
		SAY(hc, "UIC error (IS 0x%x), #%llu", is, (unsigned long long)hc->nue);
	}
	if ((is & SBUFS_IS_FATAL) != 0 && !hc->dead) {
		hc->nfatal++;
		hc->dead = true;
		SAY(hc, "FATAL host error IS 0x%x: every command fails from now on (no reset: the link is S-Boot's)", is);
	}
	if (hc->dead) {
		for (unsigned s = 0; s < hc->cfg.nslots; s++) {
			if (hc->inflight & (1u << s)) {
				memset(&r, 0, sizeof(r));
				r.err = SBUFS_E_NODEV;
				finish(hc, s, &r, done, ctx);
				n++;
			}
		}
		return n;
	}
	if (hc->inflight == 0) {
		return 0;
	}
	uint32_t dbr = rd(hc, SBUFS_BANK_HCI, SBUFS_UTRLDBR);
	uint32_t ready = hc->inflight & ~dbr;
	for (unsigned s = 0; s < hc->cfg.nslots; s++) {
		if (!(ready & (1u << s))) {
			continue;
		}
		barrier(hc);
		inval(hc, utrd_off(s), 32);
		inval(hc, ucd_off(s) + hc->L.rsp_off, hc->L.rsp_len);
		if (hc->slot[s].bounce && hc->slot[s].cmd.dir == SBUFS_DIR_READ) {
			inval(hc, bounce_off(s), hc->slot[s].cmd.xfer_len);
		}
		sbufs_parse_response(&hc->L, hc->dma + utrd_off(s), hc->dma + ucd_off(s), (uint8_t)s, &r);
		if (r.err != SBUFS_OK) {
			SAY(hc, "lun 0x%x op 0x%02x failed: ocs 0x%x upiu 0x%x resp 0x%x status 0x%x sense %x/%02x/%02x resid %u",
			    hc->slot[s].cmd.lun, hc->slot[s].cmd.cdb[0], r.ocs, r.upiu, r.response, r.status, r.sense_key,
			    r.asc, r.ascq, r.residual);
		}
		if (from_irq) {
			hc->ncompl_irq++;
		} else {
			hc->ncompl_poll++;
		}
		finish(hc, s, &r, done, ctx);
		n++;
	}
	uint64_t now = hc->ops.now_us(hc->ops.ctx);
	for (unsigned s = 0; s < hc->cfg.nslots; s++) {
		if (!(hc->inflight & (1u << s)) || now < hc->slot[s].deadline) {
			continue;
		}
		uint32_t bit = 1u << s;
		wr(hc, SBUFS_BANK_HCI, SBUFS_UTRLCLR, hc->cfg.clr_one ? bit : ~bit);
		bool cleared = false;
		for (unsigned i = 0; i < 1000 && !cleared; i++) {
			cleared = !(rd(hc, SBUFS_BANK_HCI, SBUFS_UTRLDBR) & bit);
			if (!cleared) {
				hc->ops.delay_us(hc->ops.ctx, 10);
			}
		}
		hc->ntimeout++;
		SAY(hc, "slot %u lun 0x%x op 0x%02x timed out%s", s, hc->slot[s].cmd.lun, hc->slot[s].cmd.cdb[0],
		    cleared ? ", slot cleared" : " and would not clear: host given up");
		if (!cleared) {
			hc->dead = true;
		}
		memset(&r, 0, sizeof(r));
		r.err = SBUFS_E_TIMEOUT;
		finish(hc, s, &r, done, ctx);
		n++;
	}
	return n;
}

struct run_ctx {
	bool done;
	struct sbufs_result r;
};

static void
run_done(void *ctx, unsigned slot, void *cookie, const struct sbufs_result *r)
{
	struct run_ctx *c = (struct run_ctx *)ctx;
	(void)slot;
	(void)cookie;
	c->done = true;
	c->r = *r;
}

int
sbufs_hc_run(struct sbufs_hc *hc, const struct sbufs_cmd *cmd, uint8_t *data, uint32_t data_len,
    struct sbufs_result *r)
{
	struct run_ctx c;
	int s, err;

	memset(&c, 0, sizeof(c));
	if (hc->inflight != 0) {
		return SBUFS_E_BUSY;            /* would steal another request's completion */
	}
	if (data_len != cmd->xfer_len || (data_len != 0 && data == NULL)) {
		return SBUFS_E_INVAL;
	}
	s = sbufs_hc_alloc_slot(hc);
	if (s < 0) {
		return s;
	}
	if (cmd->dir == SBUFS_DIR_WRITE) {
		memcpy(sbufs_hc_bounce_va(hc, (unsigned)s), data, data_len);
	}
	err = sbufs_hc_issue(hc, (unsigned)s, cmd, NULL, 0, &c);
	if (err != SBUFS_OK) {
		return err;
	}
	while (!c.done) {
		if (sbufs_hc_poll(hc, false, run_done, &c) == 0) {
			hc->ops.delay_us(hc->ops.ctx, 5);
		}
	}
	if (c.r.err == SBUFS_OK && cmd->dir == SBUFS_DIR_READ) {
		memcpy(data, sbufs_hc_bounce_va(hc, (unsigned)s), data_len);
	}
	if (r != NULL) {
		*r = c.r;
	}
	return c.r.err;
}

static int
run_retry(struct sbufs_hc *hc, const struct sbufs_cmd *cmd, uint8_t *data, uint32_t len, struct sbufs_result *r,
    unsigned tries)
{
	int err = SBUFS_E_IO;
	for (unsigned i = 0; i < tries; i++) {
		err = sbufs_hc_run(hc, cmd, data, len, r);
		if (err == SBUFS_OK || err == SBUFS_E_GATE || err == SBUFS_E_NODEV || hc->dead) {
			break;
		}
	}
	return err;
}

int
sbufs_hc_wake(struct sbufs_hc *hc)
{
	struct sbufs_cmd c;
	struct sbufs_result r;
	int e1, e2;

	/* After S-Boot the device W-LUN reports a unit attention / not ready (OCS 7, sense 2/04/02 on the phone);
	 * START STOP UNIT (ACTIVE) wakes it, as ufshcd_set_dev_pwr_mode() does. */
	sbufs_cdb_tur(&c, SBUFS_WLUN_DEVICE);
	e1 = run_retry(hc, &c, NULL, 0, &r, 3);
	sbufs_cdb_ssu_active(&c);
	e2 = run_retry(hc, &c, NULL, 0, &r, 3);
	SAY(hc, "device W-LUN: TEST UNIT READY %d, START STOP UNIT (ACTIVE) %d", e1, e2);
	return e2;
}

int
sbufs_hc_probe_lu(struct sbufs_hc *hc, uint8_t lun, uint64_t *blocks, uint32_t *blksz)
{
	struct sbufs_cmd c;
	struct sbufs_result r;
	uint8_t buf[32];
	int err;

	*blocks = 0;
	*blksz = 0;
	if (lun >= SBUFS_MAX_GENERAL_LU) {
		return SBUFS_E_INVAL;
	}
	sbufs_cdb_tur(&c, lun);
	err = run_retry(hc, &c, NULL, 0, &r, 3);        /* the first command may report a unit attention */
	if (err != SBUFS_OK) {
		SAY(hc, "lun %u: TEST UNIT READY %d", lun, err);
		return err;
	}
	sbufs_cdb_read_cap10(&c, lun);
	err = run_retry(hc, &c, buf, 8, &r, 2);
	if (err != SBUFS_OK) {
		return err;
	}
	uint64_t last = sbufs_be32(buf);
	*blksz = sbufs_be32(buf + 4);
	if (last == 0xffffffffu) {
		sbufs_cdb_read_cap16(&c, lun);
		err = run_retry(hc, &c, buf, 32, &r, 2);
		if (err != SBUFS_OK) {
			return err;
		}
		last = sbufs_be64(buf);
		*blksz = sbufs_be32(buf + 8);
	}
	*blocks = last + 1;
	SAY(hc, "lun %u: %llu blocks of %u bytes (%llu MiB)", lun, (unsigned long long)*blocks, *blksz,
	    (unsigned long long)((*blocks * *blksz) >> 20));
	return SBUFS_OK;
}

static void
trim(char *dst, const uint8_t *src, unsigned n)
{
	unsigned i;
	for (i = 0; i < n; i++) {
		dst[i] = (src[i] >= 0x20 && src[i] < 0x7f) ? (char)src[i] : ' ';
	}
	while (i > 0 && dst[i - 1] == ' ') {
		i--;
	}
	dst[i] = '\0';
}

int
sbufs_hc_inquiry(struct sbufs_hc *hc, uint8_t lun, char vendor[9], char product[17], char rev[5])
{
	struct sbufs_cmd c;
	struct sbufs_result r;
	uint8_t buf[36];
	sbufs_cdb_inquiry(&c, lun, sizeof(buf));
	int err = run_retry(hc, &c, buf, sizeof(buf), &r, 2);
	if (err != SBUFS_OK) {
		vendor[0] = product[0] = rev[0] = '\0';
		return err;
	}
	trim(vendor, buf + 8, 8);
	trim(product, buf + 16, 16);
	trim(rev, buf + 32, 4);
	return SBUFS_OK;
}

static void
vs_program(struct sbufs_hc *hc, const uint32_t v[4])
{
	wr(hc, SBUFS_BANK_VS, SBUFS_VS_TXPRDT_ENTRY_SIZE, v[0]);
	wr(hc, SBUFS_BANK_VS, SBUFS_VS_RXPRDT_ENTRY_SIZE, v[1]);
	wr(hc, SBUFS_BANK_VS, SBUFS_VS_DATA_REORDER, v[2]);
	wr(hc, SBUFS_BANK_VS, SBUFS_VS_AXIDMA_RWDATA_BURST, v[3]);
	barrier(hc);
}

int
sbufs_hc_multi_selftest(struct sbufs_hc *hc, uint8_t lun, uint64_t lba)
{
	static const uint32_t want[4] = { SBUFS_VS_PRDT_PREFETCH_4K, SBUFS_VS_PRDT_PREFETCH_4K, SBUFS_VS_REORDER_VAL,
		                          SBUFS_VS_AXIDMA_VAL };
	const uint32_t nb = SBUFS_MULTI_BLOCKS, bytes = SBUFS_MULTI_BLOCKS * SBUFS_BLOCK;
	uint8_t *ref = hc->dma + SBUFS_DMA_SCRATCH + bytes, *got = hc->dma + SBUFS_DMA_SCRATCH;
	struct sbufs_cmd c;
	int e0 = SBUFS_OK, e1 = SBUFS_OK, e2;

	/* reads only, and only inside the fence: the gate refuses anything else */
	if (sbufs_fence_check_quiet(hc->fence, lun, lba, nb, false) != SBUFS_FENCE_OK) {
		SAY(hc, "multi-block self test: lun %u lba 0x%llx +%u is not inside the fence: one block per command", lun,
		    (unsigned long long)lba, nb);
		return SBUFS_E_FENCE;
	}
	hc->L.max_entries = 1;
	for (uint32_t i = 0; i < nb && e0 == SBUFS_OK; i++) {
		(void)sbufs_cdb_read(&c, lun, lba + i, 1);
		e0 = run_retry(hc, &c, ref + i * SBUFS_BLOCK, SBUFS_BLOCK, NULL, 2);
	}
	vs_program(hc, want);
	memset(got, 0xa5, bytes);
	for (uint32_t i = 0; i < nb && e1 == SBUFS_OK; i++) {
		(void)sbufs_cdb_read(&c, lun, lba + i, 1);
		e1 = run_retry(hc, &c, got + i * SBUFS_BLOCK, SBUFS_BLOCK, NULL, 2);
	}
	bool single_ok = e0 == SBUFS_OK && e1 == SBUFS_OK && memcmp(got, ref, bytes) == 0;
	SAY(hc, "vs_hci was tx 0x%x rx 0x%x reorder 0x%x axidma 0x%x, now programmed: single-block %d/%d %s",
	    hc->vs_saved[0], hc->vs_saved[1], hc->vs_saved[2], hc->vs_saved[3], e0, e1, single_ok ? "same" : "DIFFERENT");
	if (single_ok) {
		hc->L.max_entries = nb;
		memset(got, 0xa5, bytes);
		(void)sbufs_cdb_read(&c, lun, lba, nb);
		e2 = run_retry(hc, &c, got, bytes, NULL, 1);
		if (e2 == SBUFS_OK && memcmp(got, ref, bytes) == 0) {
			SAY(hc, "multi-block commands on: %u x 4 KiB entries per command (16-byte entries)", nb);
			return SBUFS_OK;
		}
		SAY(hc, "%u-entry read %d %s", nb, e2, e2 == SBUFS_OK ? "differs" : "failed");
	}
	hc->L.max_entries = 1;
	if (!hc->dead) {
		vs_program(hc, hc->vs_saved);
	}
	SAY(hc, "multi-block self test failed: vendor registers restored, one block per command");
	return SBUFS_E_IO;
}
