/* SPDX-License-Identifier: APSL-2.0 */
/* Controller tests on the mock host: take-over, wake, LU probe, the doorbell/completion state machine, error paths,
 * the multi-block self test, the range -> LU mapping, and that no block outside the fence is ever touched. */
#include "test.h"
#include "mock_hc.h"

#define NSLOTS          4
#define LU0_BLOCKS      4096
#define R_FIRST         1000
#define R_COUNT         200
#define CLIENT_PA       0x40000000ull

static struct mock M;
static struct sbufs_hc H;
static struct sbufs_fence F;
static uint8_t client[256 * 1024];

static struct sbufs_cfg
cfg_default(void)
{
	struct sbufs_cfg c;
	memset(&c, 0, sizeof(c));
	c.nslots = NSLOTS;
	c.clr_one = true;
	c.nexus = true;
	c.timeout_us = 5000000;
	return c;
}

/* the block's own signature, as mock_add_lu wrote it */
static bool
block_is(const uint8_t *p, unsigned lun, uint64_t b)
{
	for (unsigned i = 0; i < SBUFS_BLOCK; i += 16) {
		uint64_t v;
		memcpy(&v, p + i + 1, 8);
		if (p[i] != lun || v != b || p[i + 9] != (uint8_t)(i >> 4)) {
			return false;
		}
	}
	return true;
}

static void
fence_setup(bool writable)
{
	struct blob a, d;
	char err[100];
	blob_allow_begin(&a);
	blob_allow_add(&a, 0, SBUFS_RANGE_ROOT | (writable ? SBUFS_RANGE_WRITABLE : 0), R_FIRST, R_COUNT, "ROOT");
	blob_deny_begin(&d);
	blob_deny_add(&d, 0, 100, 50, "EFS");
	blob_deny_add(&d, 0, 2000, 100, "PARAM");
	CHECK_EQ(sbufs_fence_parse(&F, a.b, a.len, d.b, d.len, err, sizeof(err)), SBUFS_OK);
	F.writes_enabled = writable;
}

/* a fresh mock + core, taken over, woken, LU 0 probed, fence sealed */
static void
up(bool writable, struct sbufs_cfg cfg)
{
	uint64_t blocks;
	uint32_t bs;
	mock_free(&M);
	mock_init(&M, SBUFS_DMA_SIZE(NSLOTS));
	mock_add_lu(&M, 0, LU0_BLOCKS);
	mock_add_lu(&M, 1, 64);
	mock_map_buffer(&M, CLIENT_PA, client, sizeof(client));
	fence_setup(writable);
	CHECK_EQ(sbufs_hc_init(&H, &M.ops, &cfg, M.dma, MOCK_DMA_PA, M.dma_size, &F), SBUFS_OK);
	CHECK_EQ(sbufs_hc_take_over(&H), SBUFS_OK);
	CHECK_EQ(sbufs_hc_wake(&H), SBUFS_OK);
	CHECK_EQ(sbufs_hc_probe_lu(&H, 0, &blocks, &bs), SBUFS_OK);
	CHECK_EQ(blocks, LU0_BLOCKS);
	CHECK_EQ(bs, 4096);
	sbufs_fence_set_capacity(&F, 0, blocks);
	CHECK_EQ(sbufs_fence_seal(&F), 1);
}

static void
t_takeover(void)
{
	struct sbufs_cfg cfg = cfg_default();
	mock_init(&M, SBUFS_DMA_SIZE(NSLOTS));
	fence_setup(false);
	/* init argument checks */
	CHECK_EQ(sbufs_hc_init(&H, &M.ops, &cfg, M.dma, MOCK_DMA_PA + 0x10, M.dma_size, &F), SBUFS_E_DMA);
	CHECK_EQ(sbufs_hc_init(&H, &M.ops, &cfg, M.dma, MOCK_DMA_PA, SBUFS_DMA_SIZE(NSLOTS) - 1, &F), SBUFS_E_DMA);
	CHECK_EQ(sbufs_hc_init(&H, &M.ops, &cfg, M.dma, 0xfffc0000ull, M.dma_size, &F), SBUFS_E_DMA);  /* crosses 4 GiB */
	cfg.nslots = 0;
	CHECK_EQ(sbufs_hc_init(&H, &M.ops, &cfg, M.dma, MOCK_DMA_PA, M.dma_size, &F), SBUFS_E_INVAL);
	cfg.nslots = SBUFS_MAX_SLOTS + 1;
	CHECK_EQ(sbufs_hc_init(&H, &M.ops, &cfg, M.dma, MOCK_DMA_PA, M.dma_size, &F), SBUFS_E_INVAL);
	cfg = cfg_default();
	CHECK_EQ(sbufs_hc_init(&H, &M.ops, &cfg, M.dma, MOCK_DMA_PA, M.dma_size, NULL), SBUFS_E_INVAL);
	/* host states that must not be taken */
	CHECK_EQ(sbufs_hc_init(&H, &M.ops, &cfg, M.dma, MOCK_DMA_PA, M.dma_size, &F), SBUFS_OK);
	M.hci[SBUFS_HCS / 4] = 0x10e;                           /* no device present */
	CHECK_EQ(sbufs_hc_take_over(&H), SBUFS_E_NODEV);
	M.hci[SBUFS_HCS / 4] = 0x10d;                           /* list not ready */
	CHECK_EQ(sbufs_hc_take_over(&H), SBUFS_E_NODEV);
	M.hci[SBUFS_HCS / 4] = 0x10f;
	M.hci[SBUFS_UTRLDBR / 4] = 0x1;                         /* S-Boot's request in flight */
	CHECK_EQ(sbufs_hc_take_over(&H), SBUFS_E_NODEV);
	CHECK_EQ(M.hci[SBUFS_UTRLBA / 4], 0xef002000u);         /* its list untouched */
	M.hci[SBUFS_UTRLDBR / 4] = 0;
	M.refuse_list = true;
	CHECK_EQ(sbufs_hc_take_over(&H), SBUFS_E_NODEV);
	CHECK(!H.ready);
	CHECK_EQ(sbufs_hc_alloc_slot(&H), SBUFS_E_NODEV);       /* nothing rung before the list is ours */
	M.refuse_list = false;
	M.hci[SBUFS_AHIT / 4] = 0x1234;
	M.hci[SBUFS_IS / 4] = SBUFS_IS_UE;
	CHECK_EQ(sbufs_hc_take_over(&H), SBUFS_OK);             /* HCE reads 0, as on the phone: still taken */
	CHECK_EQ(M.hci[SBUFS_UTRLBA / 4], (uint32_t)MOCK_DMA_PA);
	CHECK_EQ(M.hci[SBUFS_UTRLBAU / 4], 0);
	CHECK_EQ(M.hci[SBUFS_UTRLRSR / 4], 1);
	CHECK_EQ(M.hci[SBUFS_AHIT / 4], 0);
	CHECK_EQ(M.hci[SBUFS_IE / 4], 0);
	CHECK_EQ(M.hci[SBUFS_IS / 4], 0);
	CHECK_EQ(H.cfg.nslots, NSLOTS);
	CHECK_EQ(M.ncmds, 0);
	sbufs_hc_enable_irq(&H, true);
	CHECK_EQ(M.hci[SBUFS_IE / 4], SBUFS_IE_WANTED);
	sbufs_hc_enable_irq(&H, false);
	CHECK_EQ(M.hci[SBUFS_IE / 4], 0);
	/* NUTRS caps the slots */
	M.hci[SBUFS_CAP / 4] = (M.hci[SBUFS_CAP / 4] & ~0x1fu) | 1;    /* 2 slots */
	CHECK_EQ(sbufs_hc_init(&H, &M.ops, &cfg, M.dma, MOCK_DMA_PA, M.dma_size, &F), SBUFS_OK);
	CHECK_EQ(sbufs_hc_take_over(&H), SBUFS_OK);
	CHECK_EQ(H.cfg.nslots, 2);
	/* 64-bit wanted but CAP.64AS clear: falls back to 32 */
	M.hci[SBUFS_CAP / 4] = 0x1303ff0f & ~SBUFS_CAP_64AS;
	cfg.dma64 = true;
	CHECK_EQ(sbufs_hc_init(&H, &M.ops, &cfg, M.dma, MOCK_DMA_PA, M.dma_size, &F), SBUFS_OK);
	CHECK_EQ(sbufs_hc_take_over(&H), SBUFS_OK);
	CHECK(!H.cfg.dma64);
	CHECK_EQ(H.L.dma_limit, 1ull << 32);
}

static void
t_startup(void)
{
	char v[9], p[17], r[5];
	uint64_t blocks;
	uint32_t bs;
	up(false, cfg_default());
	CHECK(!M.device_asleep);                                /* SSU ACTIVE reached the device W-LUN */
	CHECK_EQ(sbufs_hc_inquiry(&H, 0, v, p, r), SBUFS_OK);
	CHECK(strcmp(v, "SAMSUNG") == 0);
	CHECK(strcmp(p, "KLUCG4J1ED-B0C1") == 0);
	CHECK(strcmp(r, "0200") == 0);
	CHECK_EQ(sbufs_hc_probe_lu(&H, 3, &blocks, &bs), SBUFS_E_IO);  /* absent LU */
	CHECK_EQ(blocks, 0);
	CHECK_EQ(sbufs_hc_probe_lu(&H, SBUFS_WLUN_BOOT, &blocks, &bs), SBUFS_E_INVAL);
	M.rc10_ffff = true;
	CHECK_EQ(sbufs_hc_probe_lu(&H, 1, &blocks, &bs), SBUFS_OK);    /* through READ CAPACITY(16) */
	CHECK_EQ(blocks, 64);
	CHECK_EQ(M.nexus_missing, 0);
	CHECK_EQ(M.bad_prdt, 0);
	/* nothing so far touched a block */
	CHECK_EQ(mock_count_touched(&M, 0, 0, LU0_BLOCKS, 0), 0);
}

static void
t_sync_io(void)
{
	static uint8_t buf[SBUFS_BOUNCE_SIZE];
	struct sbufs_cmd c;
	struct sbufs_result r;
	up(false, cfg_default());
	sbufs_cdb_read(&c, 0, R_FIRST, 1);
	CHECK_EQ(sbufs_hc_run(&H, &c, buf, 4096, &r), SBUFS_OK);
	CHECK(block_is(buf, 0, R_FIRST));
	sbufs_cdb_read(&c, 0, R_FIRST + R_COUNT - 1, 1);
	CHECK_EQ(sbufs_hc_run(&H, &c, buf, 4096, &r), SBUFS_OK);
	CHECK(block_is(buf, 0, R_FIRST + R_COUNT - 1));
	/* before the self test: one entry per command, so 2 blocks are refused as a DMA shape, not sent */
	sbufs_cdb_read(&c, 0, R_FIRST, 2);
	uint64_t n0 = M.ncmds;
	CHECK_EQ(sbufs_hc_run(&H, &c, buf, 8192, &r), SBUFS_E_DMA);
	CHECK_EQ(M.ncmds, n0);
	CHECK_EQ(sbufs_hc_max_xfer(&H), 4096);
	/* the fence, as the audit enforces it: nothing reaches the mock */
	static const struct { uint8_t lun; uint64_t lba; uint32_t n; bool w; } bad[] = {
		{ 0, 0, 1, false }, { 0, 1, 1, false }, { 0, 5, 1, false },         /* MBR, GPT */
		{ 0, R_FIRST - 1, 1, false }, { 0, R_FIRST + R_COUNT, 1, false },
		{ 0, 120, 1, false }, { 0, 2050, 1, false },                        /* EFS, PARAM */
		{ 0, LU0_BLOCKS - 1, 1, false },                                    /* backup GPT */
		{ 1, R_FIRST, 1, false }, { 1, 10, 1, false },                      /* another LU */
		{ 0, R_FIRST, 1, true }, { 0, R_FIRST + 10, 1, true },               /* writes, read-only */
		{ 0, R_FIRST | (1ull << 32), 1, false },                            /* aliases R_FIRST in 32 bits */
	};
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		if (bad[i].w) {
			sbufs_cdb_write(&c, bad[i].lun, bad[i].lba, bad[i].n, false);
		} else {
			sbufs_cdb_read(&c, bad[i].lun, bad[i].lba, bad[i].n);
		}
		n0 = M.ncmds;
		CHECK_EQ(sbufs_hc_run(&H, &c, buf, 4096, &r), SBUFS_E_GATE);
		CHECK_EQ(M.ncmds, n0);
	}
	CHECK_EQ(H.ngate, sizeof(bad) / sizeof(bad[0]));
	sbufs_cdb_sync(&c, 0, R_FIRST, R_COUNT);
	CHECK_EQ(sbufs_hc_run(&H, &c, NULL, 0, &r), SBUFS_E_GATE);     /* read-only: no flush either */
	/* every slot is free again after the refusals */
	for (unsigned s = 0; s < NSLOTS; s++) {
		CHECK(!H.slot[s].busy);
	}
	CHECK_EQ(mock_count_touched_outside(&M, 0, R_FIRST, R_COUNT), 0);
	CHECK_EQ(mock_count_touched(&M, 1, 0, 64, 0), 0);
	CHECK_EQ(mock_count_touched(&M, 0, 0, LU0_BLOCKS, 2), 0);      /* nothing written */
	CHECK_EQ(H.foreign_touched, 0);
}

static void
t_multi(void)
{
	static uint8_t buf[SBUFS_BOUNCE_SIZE];
	struct sbufs_cmd c;
	struct sbufs_result r;

	up(false, cfg_default());
	M.multi_needs_vs = true;
	uint32_t saved_tx = M.vs[SBUFS_VS_TXPRDT_ENTRY_SIZE / 4];
	uint64_t n0 = M.ncmds;
	CHECK_EQ(sbufs_hc_multi_selftest(&H, 0, R_FIRST - 8), SBUFS_E_FENCE);  /* must stay inside the fence */
	CHECK_EQ(M.ncmds, n0);
	CHECK_EQ(sbufs_hc_multi_selftest(&H, 0, R_FIRST + R_COUNT - 15), SBUFS_E_FENCE);
	CHECK_EQ(sbufs_hc_multi_selftest(&H, 0, R_FIRST), SBUFS_OK);
	CHECK_EQ(H.L.max_entries, 16);
	CHECK_EQ(M.vs[SBUFS_VS_TXPRDT_ENTRY_SIZE / 4], SBUFS_VS_PRDT_PREFETCH_4K);
	CHECK_EQ(M.vs[SBUFS_VS_AXIDMA_RWDATA_BURST / 4], SBUFS_VS_AXIDMA_VAL);
	sbufs_cdb_read(&c, 0, R_FIRST + 100, 16);
	CHECK_EQ(sbufs_hc_run(&H, &c, buf, 65536, &r), SBUFS_OK);
	for (int i = 0; i < 16; i++) {
		CHECK(block_is(buf + i * 4096, 0, R_FIRST + 100 + i));
	}
	CHECK_EQ(sbufs_hc_max_xfer(&H), 65536);

	/* a host whose multi-entry reads come back wrong: registers restored, one block per command */
	up(false, cfg_default());
	M.multi_broken = true;
	M.vs[SBUFS_VS_DATA_REORDER / 4] = 0x77;
	H.vs_saved[2] = 0x77;
	CHECK_EQ(sbufs_hc_multi_selftest(&H, 0, R_FIRST), SBUFS_E_IO);
	CHECK_EQ(H.L.max_entries, 1);
	CHECK_EQ(M.vs[SBUFS_VS_TXPRDT_ENTRY_SIZE / 4], saved_tx);
	CHECK_EQ(M.vs[SBUFS_VS_DATA_REORDER / 4], 0x77);
	sbufs_cdb_read(&c, 0, R_FIRST, 1);
	CHECK_EQ(sbufs_hc_run(&H, &c, buf, 4096, &r), SBUFS_OK);       /* still works single-block */
	CHECK(block_is(buf, 0, R_FIRST));
	/* a single-block read error during the test: also restored */
	up(false, cfg_default());
	M.inject_check = 2;
	CHECK_EQ(sbufs_hc_multi_selftest(&H, 0, R_FIRST), SBUFS_E_IO);
	CHECK_EQ(H.L.max_entries, 1);
	CHECK_EQ(mock_count_touched_outside(&M, 0, R_FIRST, R_COUNT), 0);
}

/* ---- the asynchronous path ---- */

struct done_log {
	unsigned n;
	struct { unsigned slot; uintptr_t cookie; struct sbufs_result r; } e[64];
};

static void
on_done(void *ctx, unsigned slot, void *cookie, const struct sbufs_result *r)
{
	struct done_log *l = ctx;
	if (l->n < 64) {
		l->e[l->n].slot = slot;
		l->e[l->n].cookie = (uintptr_t)cookie;
		l->e[l->n].r = *r;
	}
	l->n++;
}

static int
issue_direct(unsigned slot, uint64_t lba, uint32_t nblks, uint64_t client_off, void *cookie)
{
	struct sbufs_cmd c;
	struct sbufs_seg s[16];
	sbufs_cdb_read(&c, 0, lba, nblks);
	for (uint32_t i = 0; i < nblks; i++) {
		s[i].pa = CLIENT_PA + client_off + i * 4096;
		s[i].len = 4096;
	}
	return sbufs_hc_issue(&H, slot, &c, s, nblks, cookie);
}

static void
t_async(void)
{
	struct done_log L;
	struct sbufs_cfg cfg = cfg_default();
	cfg.irq = true;
	up(false, cfg);
	M.multi_needs_vs = true;
	CHECK_EQ(sbufs_hc_multi_selftest(&H, 0, R_FIRST), SBUFS_OK);
	M.deferred = true;
	memset(&L, 0, sizeof(L));
	memset(client, 0, sizeof(client));
	uint64_t poll0 = H.ncompl_poll;
	/* fill every slot with direct-DMA reads into the client buffer */
	for (unsigned i = 0; i < NSLOTS; i++) {
		int s = sbufs_hc_alloc_slot(&H);
		CHECK_EQ(s, (int)i);
		CHECK_EQ(issue_direct((unsigned)s, R_FIRST + 16 * i, 16, (uint64_t)i * 65536, (void *)(uintptr_t)(100 + i)),
		    SBUFS_OK);
		CHECK_EQ(M.vs[SBUFS_VS_UTRL_NEXUS_TYPE / 4] & (1u << s), 1u << s);
	}
	CHECK_EQ(sbufs_hc_alloc_slot(&H), SBUFS_E_BUSY);
	CHECK_EQ(H.inflight, 0xf);
	CHECK_EQ(sbufs_hc_poll(&H, true, on_done, &L), 0);     /* nothing finished yet */
	/* out of order: 2 and 0 first */
	mock_tick_mask(&M, 0x5);
	CHECK_EQ(sbufs_hc_poll(&H, true, on_done, &L), 2);
	CHECK_EQ(L.n, 2);
	CHECK_EQ(L.e[0].slot, 0);
	CHECK_EQ(L.e[0].cookie, 100);
	CHECK_EQ(L.e[1].slot, 2);
	CHECK_EQ(L.e[1].cookie, 102);
	CHECK_EQ(H.inflight, 0xa);
	CHECK_EQ(sbufs_hc_alloc_slot(&H), 0);                   /* a freed slot is reused at once */
	sbufs_hc_free_slot(&H, 0);
	mock_tick(&M);
	CHECK_EQ(sbufs_hc_poll(&H, false, on_done, &L), 2);
	CHECK_EQ(L.n, 4);
	for (unsigned i = 0; i < 4; i++) {
		CHECK_EQ(L.e[i].r.err, SBUFS_OK);
	}
	for (unsigned i = 0; i < NSLOTS * 16; i++) {
		CHECK(block_is(client + i * 4096, 0, R_FIRST + i));
	}
	CHECK_EQ(H.ncompl_irq, 2);
	CHECK_EQ(H.ncompl_poll - poll0, 2);
	CHECK_EQ(M.hci[SBUFS_IS / 4], 0);                       /* acknowledged */

	/* a segment the host cannot take: refused before the doorbell, slot freed (the caller bounces) */
	int s = sbufs_hc_alloc_slot(&H);
	struct sbufs_cmd c;
	struct sbufs_seg seg = { CLIENT_PA + 2, 4096 };
	sbufs_cdb_read(&c, 0, R_FIRST, 1);
	uint64_t n0 = M.ncmds;
	CHECK_EQ(sbufs_hc_issue(&H, (unsigned)s, &c, &seg, 1, NULL), SBUFS_E_DMA);
	CHECK(!H.slot[s].busy);
	CHECK_EQ(H.inflight, 0);
	/* the bounce path */
	s = sbufs_hc_alloc_slot(&H);
	CHECK_EQ(sbufs_hc_issue(&H, (unsigned)s, &c, NULL, 0, (void *)7), SBUFS_OK);
	mock_tick(&M);
	L.n = 0;
	CHECK_EQ(sbufs_hc_poll(&H, false, on_done, &L), 1);
	CHECK(block_is(sbufs_hc_bounce_va(&H, (unsigned)s), 0, R_FIRST));
	CHECK_EQ(M.ncmds, n0 + 1);
	/* issuing into a slot not allocated, or twice */
	CHECK_EQ(sbufs_hc_issue(&H, 3, &c, NULL, 0, NULL), SBUFS_E_INVAL);
	s = sbufs_hc_alloc_slot(&H);
	CHECK_EQ(sbufs_hc_issue(&H, (unsigned)s, &c, NULL, 0, NULL), SBUFS_OK);
	CHECK_EQ(sbufs_hc_issue(&H, (unsigned)s, &c, NULL, 0, NULL), SBUFS_E_INVAL);
	sbufs_hc_free_slot(&H, (unsigned)s);                    /* in flight: must not free */
	CHECK(H.slot[s].busy);
	mock_tick(&M);
	CHECK_EQ(sbufs_hc_poll(&H, false, on_done, &L), 1);
	CHECK_EQ(sbufs_hc_run(&H, &c, client, 4096, NULL), SBUFS_OK);  /* run works once idle */
	s = sbufs_hc_alloc_slot(&H);
	CHECK_EQ(sbufs_hc_issue(&H, (unsigned)s, &c, NULL, 0, NULL), SBUFS_OK);
	CHECK_EQ(sbufs_hc_run(&H, &c, client, 4096, NULL), SBUFS_E_BUSY);  /* not while something is in flight */
	mock_tick(&M);
	sbufs_hc_poll(&H, false, NULL, NULL);
	CHECK_EQ(mock_count_touched_outside(&M, 0, R_FIRST, R_COUNT), 0);
}

static void
t_errors(void)
{
	struct done_log L;
	struct sbufs_cmd c;
	up(false, cfg_default());
	M.deferred = true;
	sbufs_cdb_read(&c, 0, R_FIRST, 1);
	uint64_t nerr0 = H.nerr;

	/* OCS error */
	memset(&L, 0, sizeof(L));
	M.inject_ocs = 1;
	int s = sbufs_hc_alloc_slot(&H);
	CHECK_EQ(sbufs_hc_issue(&H, (unsigned)s, &c, NULL, 0, NULL), SBUFS_OK);
	mock_tick(&M);
	sbufs_hc_poll(&H, false, on_done, &L);
	CHECK_EQ(L.e[0].r.err, SBUFS_E_IO);
	CHECK_EQ(L.e[0].r.ocs, 7);
	/* CHECK CONDITION, medium error */
	M.inject_check = 1;
	s = sbufs_hc_alloc_slot(&H);
	CHECK_EQ(sbufs_hc_issue(&H, (unsigned)s, &c, NULL, 0, NULL), SBUFS_OK);
	mock_tick(&M);
	sbufs_hc_poll(&H, false, on_done, &L);
	CHECK_EQ(L.e[1].r.err, SBUFS_E_IO);
	CHECK_EQ(L.e[1].r.sense_key, 3);
	CHECK_EQ(L.e[1].r.asc, 0x11);
	/* unit attention on a data command */
	M.lu[0].ua_pending = true;
	s = sbufs_hc_alloc_slot(&H);
	CHECK_EQ(sbufs_hc_issue(&H, (unsigned)s, &c, NULL, 0, NULL), SBUFS_OK);
	mock_tick(&M);
	sbufs_hc_poll(&H, false, on_done, &L);
	CHECK_EQ(L.e[2].r.sense_key, SBUFS_SENSE_UNIT_ATTN);
	CHECK_EQ(H.nerr - nerr0, 3);

	/* timeout: the slot is cleared with a 1 in UTRLCLR (Exynos) and is usable again */
	M.inject_hang = 1;
	s = sbufs_hc_alloc_slot(&H);
	CHECK_EQ(sbufs_hc_issue(&H, (unsigned)s, &c, NULL, 0, (void *)9), SBUFS_OK);
	int s2 = sbufs_hc_alloc_slot(&H);
	CHECK_EQ(sbufs_hc_issue(&H, (unsigned)s2, &c, NULL, 0, (void *)10), SBUFS_OK);
	mock_tick(&M);
	L.n = 0;
	CHECK_EQ(sbufs_hc_poll(&H, false, on_done, &L), 1);    /* the healthy one */
	CHECK_EQ(L.e[0].cookie, 10);
	M.now += 4999000;
	CHECK_EQ(sbufs_hc_poll(&H, false, on_done, &L), 0);    /* not yet */
	M.now += 2000;
	CHECK_EQ(sbufs_hc_poll(&H, false, on_done, &L), 1);
	CHECK_EQ(L.e[1].cookie, 9);
	CHECK_EQ(L.e[1].r.err, SBUFS_E_TIMEOUT);
	CHECK_EQ(M.hung, 0);
	CHECK_EQ(H.ntimeout, 1);
	CHECK(!H.dead);
	CHECK_EQ(sbufs_hc_run(&H, &c, client, 4096, NULL), SBUFS_OK);

	/* a host error with nothing completing: everything in flight fails, the host is given up */
	M.inject_fatal = true;
	s = sbufs_hc_alloc_slot(&H);
	CHECK_EQ(sbufs_hc_issue(&H, (unsigned)s, &c, NULL, 0, (void *)11), SBUFS_OK);
	L.n = 0;
	CHECK_EQ(sbufs_hc_poll(&H, true, on_done, &L), 1);
	CHECK_EQ(L.e[0].r.err, SBUFS_E_NODEV);
	CHECK(H.dead);
	CHECK_EQ(H.nfatal, 1);
	CHECK_EQ(sbufs_hc_alloc_slot(&H), SBUFS_E_NODEV);
	CHECK_EQ(sbufs_hc_run(&H, &c, client, 4096, NULL), SBUFS_E_NODEV);

	/* a slot that will not clear after its timeout: host given up too */
	up(false, cfg_default());
	M.deferred = true;
	M.inject_hang = 1;
	s = sbufs_hc_alloc_slot(&H);
	sbufs_cdb_read(&c, 0, R_FIRST, 1);
	CHECK_EQ(sbufs_hc_issue(&H, (unsigned)s, &c, NULL, 0, NULL), SBUFS_OK);
	H.cfg.clr_one = false;                                  /* the standard convention: the mock ignores it */
	M.now += 6000000;
	L.n = 0;
	CHECK_EQ(sbufs_hc_poll(&H, false, on_done, &L), 1);
	CHECK_EQ(L.e[0].r.err, SBUFS_E_TIMEOUT);
	CHECK(H.dead);

	/* without the vendor nexus bit the host does not run it as SCSI (the mock answers OCS 8) */
	struct sbufs_cfg cfg = cfg_default();
	cfg.nexus = false;
	mock_free(&M);
	mock_init(&M, SBUFS_DMA_SIZE(NSLOTS));
	mock_add_lu(&M, 0, LU0_BLOCKS);
	fence_setup(false);
	CHECK_EQ(sbufs_hc_init(&H, &M.ops, &cfg, M.dma, MOCK_DMA_PA, M.dma_size, &F), SBUFS_OK);
	CHECK_EQ(sbufs_hc_take_over(&H), SBUFS_OK);
	M.vs[SBUFS_VS_UTRL_NEXUS_TYPE / 4] = 0;
	CHECK(sbufs_hc_wake(&H) != SBUFS_OK);
	CHECK(M.nexus_missing > 0);
}

static void
t_writes(void)
{
	static uint8_t buf[4096], back[4096];
	struct sbufs_cmd c;
	up(true, cfg_default());
	memset(buf, 0x3c, sizeof(buf));
	sbufs_cdb_write(&c, 0, R_FIRST + 5, 1, true);
	CHECK_EQ(sbufs_hc_run(&H, &c, buf, 4096, NULL), SBUFS_OK);
	sbufs_cdb_read(&c, 0, R_FIRST + 5, 1);
	CHECK_EQ(sbufs_hc_run(&H, &c, back, 4096, NULL), SBUFS_OK);
	CHECK(memcmp(buf, back, 4096) == 0);
	sbufs_cdb_sync(&c, 0, R_FIRST, R_COUNT);
	CHECK_EQ(sbufs_hc_run(&H, &c, NULL, 0, NULL), SBUFS_OK);
	sbufs_cdb_write(&c, 0, R_FIRST + R_COUNT, 1, false);    /* one past the range */
	CHECK_EQ(sbufs_hc_run(&H, &c, buf, 4096, NULL), SBUFS_E_GATE);
	sbufs_cdb_write(&c, 0, 110, 1, false);                  /* EFS */
	CHECK_EQ(sbufs_hc_run(&H, &c, buf, 4096, NULL), SBUFS_E_GATE);
	CHECK_EQ(mock_count_touched(&M, 0, 0, LU0_BLOCKS, 2), 1);
	CHECK_EQ(mock_count_touched(&M, 0, R_FIRST + 5, 1, 2), 1);
	/* the global switch off again: the same write is refused */
	F.writes_enabled = false;
	sbufs_cdb_write(&c, 0, R_FIRST + 5, 1, false);
	CHECK_EQ(sbufs_hc_run(&H, &c, buf, 4096, NULL), SBUFS_E_GATE);
}

/* ---- soak: the IOBlockStorageDevice view (range-relative requests) under random timing and errors ---- */
static void
t_soak(void)
{
	struct done_log L;
	struct sbufs_cfg cfg = cfg_default();
	uint64_t rng = 777, issued = 0, refused = 0, okreads = 0, bad_data = 0;
	struct { bool used; uint64_t lba; uint32_t n; } track[NSLOTS];

	up(false, cfg);
	M.multi_needs_vs = true;
	CHECK_EQ(sbufs_hc_multi_selftest(&H, 0, R_FIRST), SBUFS_OK);
	M.deferred = true;
	memset(track, 0, sizeof(track));
	for (int iter = 0; iter < 20000; iter++) {
		rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
		/* an IOBlockStorageDriver request against range 0: sometimes nonsense */
		uint64_t block = (rng >> 3) % (R_COUNT + 20);
		uint64_t nblks = 1 + (rng >> 16) % 16;
		if ((rng & 0xff) == 1) {
			block = UINT64_MAX - (rng >> 40) % 4;
		}
		if ((rng & 0xff) == 2) {
			nblks = UINT64_MAX;
		}
		bool write = (rng & 0x700) == 0x700;
		uint32_t lun;
		uint64_t lba;
		if (sbufs_fence_map(&F, 0, block, nblks, write, &lun, &lba) != SBUFS_FENCE_OK) {
			refused++;
		} else {
			int s = sbufs_hc_alloc_slot(&H);
			if (s >= 0) {
				if ((rng >> 50) % 50 == 0) {
					M.inject_check = 1;
				}
				struct sbufs_cmd c;
				sbufs_cdb_read(&c, (uint8_t)lun, lba, (uint32_t)nblks);
				CHECK_EQ(sbufs_hc_issue(&H, (unsigned)s, &c, NULL, 0, (void *)(uintptr_t)s), SBUFS_OK);
				track[s].used = true;
				track[s].lba = lba;
				track[s].n = (uint32_t)nblks;
				issued++;
			}
		}
		if ((rng >> 33) % 3 == 0) {
			mock_tick_mask(&M, (uint32_t)(rng >> 44));
		}
		L.n = 0;
		sbufs_hc_poll(&H, (rng >> 60) & 1, on_done, &L);
		for (unsigned i = 0; i < L.n && i < 64; i++) {
			unsigned s = (unsigned)L.e[i].cookie;
			if (L.e[i].r.err == SBUFS_OK) {
				okreads++;
				for (uint32_t b = 0; b < track[s].n; b++) {
					if (!block_is(sbufs_hc_bounce_va(&H, s) + b * 4096, 0, track[s].lba + b)) {
						bad_data++;
					}
				}
			}
			track[s].used = false;
		}
	}
	mock_tick(&M);
	sbufs_hc_poll(&H, false, NULL, NULL);
	printf("  soak: %llu requests issued, %llu refused by the fence, %llu good reads, %llu bad data, %llu UTP cmds\n",
	    (unsigned long long)issued, (unsigned long long)refused, (unsigned long long)okreads,
	    (unsigned long long)bad_data, (unsigned long long)M.ncmds);
	CHECK(issued > 5000);
	CHECK(refused > 500);
	CHECK_EQ(bad_data, 0);
	CHECK_EQ(H.inflight, 0);
	CHECK_EQ(mock_count_touched_outside(&M, 0, R_FIRST, R_COUNT), 0);
	CHECK_EQ(mock_count_touched(&M, 0, 0, LU0_BLOCKS, 2), 0);
	CHECK_EQ(mock_count_touched(&M, 1, 0, 64, 0), 0);
	CHECK_EQ(H.foreign_touched, 0);
	CHECK_EQ(H.ngate, 0);                   /* the request-level fence caught everything before the audit */
	CHECK_EQ(M.nexus_missing, 0);
	CHECK_EQ(M.bad_prdt, 0);
	mock_free(&M);
	memset(&M, 0, sizeof(M));
}

void
test_hc(void)
{
	memset(&M, 0, sizeof(M));
	t_takeover();
	t_startup();
	t_sync_io();
	t_multi();
	t_async();
	t_errors();
	t_writes();
	t_soak();
}
