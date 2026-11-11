/* SPDX-License-Identifier: APSL-2.0 */
/* Protocol tests: CDBs, UTRD/UPIU/PRDT encoding, the doorbell audit (incl. a mutation fuzz), response parsing. */
#include "test.h"

static struct sbufs_fence F;
static uint8_t utrd[32], ucd[SBUFS_UCD_SIZE];

static void
fence_one(bool writable)
{
	struct blob a;
	char err[100];
	blob_allow_begin(&a);
	blob_allow_add(&a, 0, writable ? SBUFS_RANGE_WRITABLE : 0, 1000, 100, "R");
	CHECK_EQ(sbufs_fence_parse(&F, a.b, a.len, NULL, 0, err, sizeof(err)), SBUFS_OK);
	sbufs_fence_set_capacity(&F, 0, 1u << 20);
	F.writes_enabled = writable;
	CHECK_EQ(sbufs_fence_seal(&F), 1);
}

static void
t_cdb(void)
{
	struct sbufs_cmd c;
	CHECK_EQ(sbufs_cdb_read(&c, 0, 0x12345678, 16), SBUFS_OK);
	CHECK_EQ(c.cdb[0], SBUFS_SCSI_READ10);
	CHECK_EQ(c.cdb_len, 10);
	CHECK_EQ(sbufs_be32(c.cdb + 2), 0x12345678);
	CHECK_EQ(sbufs_be16(c.cdb + 7), 16);
	CHECK_EQ(c.xfer_len, 65536);
	CHECK_EQ(c.dir, SBUFS_DIR_READ);
	CHECK_EQ(sbufs_cdb_read(&c, 0, 0xffffffffull, 1), SBUFS_OK);    /* last 32-bit block: still READ(10) */
	CHECK_EQ(c.cdb[0], SBUFS_SCSI_READ10);
	CHECK_EQ(sbufs_cdb_read(&c, 0, 0xffffffffull, 2), SBUFS_OK);    /* crosses 2^32: READ(16) */
	CHECK_EQ(c.cdb[0], SBUFS_SCSI_READ16);
	CHECK_EQ(sbufs_be64(c.cdb + 2), 0xffffffffull);
	CHECK_EQ(sbufs_be32(c.cdb + 10), 2);
	CHECK_EQ(sbufs_cdb_read(&c, 0, 0x100000000ull, 1), SBUFS_OK);
	CHECK_EQ(c.cdb[0], SBUFS_SCSI_READ16);
	CHECK_EQ(sbufs_cdb_read(&c, 0, 0, 0xffff), SBUFS_OK);
	CHECK_EQ(c.cdb[0], SBUFS_SCSI_READ10);
	CHECK_EQ(sbufs_cdb_read(&c, 0, 0, 0x10000), SBUFS_OK);          /* count does not fit 16 bits */
	CHECK_EQ(c.cdb[0], SBUFS_SCSI_READ16);
	CHECK_EQ(sbufs_cdb_read(&c, 0, 0, 0), SBUFS_E_INVAL);
	CHECK_EQ(sbufs_cdb_read(&c, 0, 0, UINT32_MAX / SBUFS_BLOCK + 1), SBUFS_E_INVAL);  /* length overflows 32 bits */
	CHECK_EQ(sbufs_cdb_read(&c, 0, UINT64_MAX - 1, 1), SBUFS_OK);
	CHECK_EQ(sbufs_cdb_read(&c, 0, UINT64_MAX, 1), SBUFS_E_INVAL);     /* its end is 2^64 */
	CHECK_EQ(sbufs_cdb_read(&c, 0, UINT64_MAX, 2), SBUFS_E_INVAL);
	CHECK_EQ(sbufs_cdb_write(&c, 2, 77, 3, true), SBUFS_OK);
	CHECK_EQ(c.cdb[0], SBUFS_SCSI_WRITE10);
	CHECK_EQ(c.cdb[1], 0x08);
	CHECK_EQ(c.dir, SBUFS_DIR_WRITE);
	CHECK_EQ(c.lun, 2);
	CHECK_EQ(sbufs_cdb_write(&c, 2, 1ull << 40, 3, false), SBUFS_OK);
	CHECK_EQ(c.cdb[0], SBUFS_SCSI_WRITE16);
	CHECK_EQ(c.cdb[1], 0);
	CHECK_EQ(sbufs_cdb_sync(&c, 0, 1000, 0), SBUFS_E_INVAL);        /* never "the whole LU" */
	CHECK_EQ(sbufs_cdb_sync(&c, 0, 1000, 100), SBUFS_OK);
	CHECK_EQ(c.cdb[0], SBUFS_SCSI_SYNC10);
	CHECK_EQ(c.xfer_len, 0);
	CHECK_EQ(sbufs_cdb_sync(&c, 0, 1000, 70000), SBUFS_OK);
	CHECK_EQ(c.cdb[0], SBUFS_SCSI_SYNC16);
	sbufs_cdb_ssu_active(&c);
	CHECK_EQ(c.lun, SBUFS_WLUN_DEVICE);
	CHECK_EQ(c.cdb[4], 0x10);
	sbufs_cdb_read_cap16(&c, 1);
	CHECK_EQ(c.cdb[1], SBUFS_SAI_READ_CAP16);
	CHECK_EQ(c.xfer_len, 32);
}

static void
t_build(void)
{
	struct sbufs_layout L;
	struct sbufs_cmd c;
	struct sbufs_seg s[17];
	const uint64_t ucd_pa = 0x9a001000ull;

	sbufs_layout_s9(&L, 16, false);
	CHECK_EQ(L.max_entries, 16);
	CHECK(L.byte_gran);
	CHECK_EQ(L.dma_limit, 1ull << 32);
	for (int i = 0; i < 17; i++) {
		s[i].pa = 0x80000000ull + i * 0x10000;
		s[i].len = 4096;
	}
	sbufs_cdb_read(&c, 0, 1000, 2);
	CHECK_EQ(sbufs_build(&L, utrd, ucd, ucd_pa, &c, 5, s, 2, true), SBUFS_OK);
	CHECK_EQ(sbufs_le32(utrd), SBUFS_UTRD_CT_UFS | SBUFS_UTRD_DD_READ | SBUFS_UTRD_I);
	CHECK_EQ(sbufs_le32(utrd + 8), 0x0f);
	CHECK_EQ(sbufs_le32(utrd + 16), 0x9a001000u);
	CHECK_EQ(sbufs_le32(utrd + 20), 0);
	CHECK_EQ(sbufs_le32(utrd + 24), 512u << 16 | 512u);            /* bytes (Exynos) */
	CHECK_EQ(sbufs_le32(utrd + 28), 1024u << 16 | 32u);             /* 2 entries x 16 bytes */
	CHECK_EQ(ucd[0], 0x01);
	CHECK_EQ(ucd[1], 0x40);
	CHECK_EQ(ucd[3], 5);
	CHECK_EQ(sbufs_be32(ucd + 12), 8192);
	CHECK_EQ(ucd[16], SBUFS_SCSI_READ10);
	CHECK_EQ(sbufs_le32(ucd + 1024), 0x80000000u);
	CHECK_EQ(sbufs_le32(ucd + 1024 + 12), 4095);
	CHECK_EQ(sbufs_le32(ucd + 1040), 0x80010000u);
	/* standard (dword) granularity */
	L.byte_gran = false;
	CHECK_EQ(sbufs_build(&L, utrd, ucd, ucd_pa, &c, 5, s, 2, false), SBUFS_OK);
	CHECK_EQ(sbufs_le32(utrd + 24), 128u << 16 | 128u);
	CHECK_EQ(sbufs_le32(utrd + 28), 256u << 16 | 2u);
	CHECK_EQ(sbufs_le32(utrd) & SBUFS_UTRD_I, 0);
	L.byte_gran = true;
	/* segment rules: each refusal is SBUFS_E_DMA (the caller bounces) */
	struct sbufs_seg b[2];
	b[0] = s[0]; b[1] = s[1];
	b[1].pa += 2;                           CHECK_EQ(sbufs_build(&L, utrd, ucd, ucd_pa, &c, 0, b, 2, false), SBUFS_E_DMA);
	b[1] = s[1]; b[0].len = 4094; b[1].len = 4098;
	CHECK_EQ(sbufs_build(&L, utrd, ucd, ucd_pa, &c, 0, b, 2, false), SBUFS_E_DMA);  /* > 4 KiB */
	b[0].len = 4097; b[1].len = 4095;       CHECK_EQ(sbufs_build(&L, utrd, ucd, ucd_pa, &c, 0, b, 2, false), SBUFS_E_DMA);
	b[0].len = 2048; b[1].len = 4096;       CHECK_EQ(sbufs_build(&L, utrd, ucd, ucd_pa, &c, 0, b, 2, false), SBUFS_E_DMA);  /* sum */
	b[0] = s[0]; b[1] = s[1]; b[1].pa = 0xfffff000ull;
	CHECK_EQ(sbufs_build(&L, utrd, ucd, ucd_pa, &c, 0, b, 2, false), SBUFS_OK);   /* ends exactly at 4 GiB */
	b[1].pa = 0xfffff004ull;                CHECK_EQ(sbufs_build(&L, utrd, ucd, ucd_pa, &c, 0, b, 2, false), SBUFS_E_DMA);
	b[1].pa = 0x100000000ull;               CHECK_EQ(sbufs_build(&L, utrd, ucd, ucd_pa, &c, 0, b, 2, false), SBUFS_E_DMA);
	b[1].pa = UINT64_MAX - 3;               CHECK_EQ(sbufs_build(&L, utrd, ucd, ucd_pa, &c, 0, b, 2, false), SBUFS_E_DMA);
	L.dma_limit = 0;
	b[1].pa = 0x880000000ull;               CHECK_EQ(sbufs_build(&L, utrd, ucd, ucd_pa, &c, 0, b, 2, false), SBUFS_OK);
	CHECK_EQ(sbufs_le32(ucd + 1040 + 4), 8);
	b[1].pa = UINT64_MAX - 3;               CHECK_EQ(sbufs_build(&L, utrd, ucd, ucd_pa, &c, 0, b, 2, false), SBUFS_E_DMA);
	L.dma_limit = 1ull << 32;
	b[0].len = 0;                           CHECK_EQ(sbufs_build(&L, utrd, ucd, ucd_pa, &c, 0, b, 2, false), SBUFS_E_DMA);
	sbufs_cdb_read(&c, 0, 1000, 17);
	CHECK_EQ(sbufs_build(&L, utrd, ucd, ucd_pa, &c, 0, s, 17, false), SBUFS_E_DMA);  /* > max entries */
	sbufs_cdb_read(&c, 0, 1000, 16);
	CHECK_EQ(sbufs_build(&L, utrd, ucd, ucd_pa, &c, 0, s, 16, false), SBUFS_OK);
	CHECK_EQ(sbufs_build(&L, utrd, ucd, ucd_pa, &c, 0, NULL, 0, false), SBUFS_E_DMA);
	CHECK_EQ(sbufs_build(&L, utrd, ucd, ucd_pa + 0x40, &c, 0, s, 16, false), SBUFS_E_INVAL);  /* UCD alignment */
	sbufs_cdb_tur(&c, 0);
	CHECK_EQ(sbufs_build(&L, utrd, ucd, ucd_pa, &c, 0, s, 1, false), SBUFS_E_INVAL);  /* segments without data */
	CHECK_EQ(sbufs_build(&L, utrd, ucd, ucd_pa, &c, 0, NULL, 0, false), SBUFS_OK);
	CHECK_EQ(sbufs_le32(utrd + 28), 1024u << 16);
}

static int
gate_of(const struct sbufs_layout *L, const struct sbufs_cmd *c, struct sbufs_decoded *d)
{
	struct sbufs_seg s[16];
	for (int i = 0; i < 16; i++) {
		s[i].pa = 0x80000000ull + i * 4096;
		s[i].len = 4096;
	}
	unsigned n = c->xfer_len ? (c->xfer_len + 4095) / 4096 : 0;
	if (n == 1 && c->xfer_len < 4096) {
		s[0].len = c->xfer_len;
	}
	if (sbufs_build(L, utrd, ucd, 0x9a001000ull, c, 1, n ? s : NULL, n, false) != SBUFS_OK) {
		return -100;
	}
	return sbufs_gate(L, utrd, ucd, &F, d);
}

/* an independent decoder for the fuzz: what would the device do with these bytes? */
static bool
device_view(uint8_t *lun, bool *medium, bool *write, uint64_t *lba, uint64_t *n)
{
	const uint8_t *cdb = ucd + 16;
	*lun = ucd[2];
	*medium = *write = false;
	switch (cdb[0]) {
	case 0x28: case 0x2a: case 0x35:
		*lba = sbufs_be32(cdb + 2); *n = sbufs_be16(cdb + 7); break;
	case 0x88: case 0x8a: case 0x91:
		*lba = sbufs_be64(cdb + 2); *n = sbufs_be32(cdb + 10); break;
	default:
		/* anything else that touches the medium would be a gate hole: list what the whitelist allows */
		return cdb[0] == 0x00 || cdb[0] == 0x12 || cdb[0] == 0x1b || cdb[0] == 0x25 ||
		       (cdb[0] == 0x9e && (cdb[1] & 0x1f) == 0x10);
	}
	*medium = true;
	*write = cdb[0] != 0x28 && cdb[0] != 0x88;
	return true;
}

static void
t_gate(void)
{
	struct sbufs_layout L;
	struct sbufs_cmd c;
	struct sbufs_decoded d;

	fence_one(false);
	sbufs_layout_s9(&L, 16, false);
	sbufs_cdb_read(&c, 0, 1000, 16);        CHECK_EQ(gate_of(&L, &c, &d), SBUFS_OK);
	CHECK(d.data && !d.write && d.lba == 1000 && d.nblks == 16);
	sbufs_cdb_read(&c, 0, 1084, 16);        CHECK_EQ(gate_of(&L, &c, &d), SBUFS_OK);
	sbufs_cdb_read(&c, 0, 1085, 16);        CHECK_EQ(gate_of(&L, &c, &d), SBUFS_E_GATE);   /* one past the end */
	sbufs_cdb_read(&c, 0, 999, 1);          CHECK_EQ(gate_of(&L, &c, &d), SBUFS_E_GATE);
	sbufs_cdb_read(&c, 0, 1, 1);            CHECK_EQ(gate_of(&L, &c, &d), SBUFS_E_GATE);   /* the GPT */
	sbufs_cdb_read(&c, 1, 1000, 1);         CHECK_EQ(gate_of(&L, &c, &d), SBUFS_E_GATE);   /* other LU */
	sbufs_cdb_read(&c, 0, 1000ull | (1ull << 32), 1);
	CHECK_EQ(gate_of(&L, &c, &d), SBUFS_E_GATE);    /* READ(16) whose low word is inside */
	sbufs_cdb_write(&c, 0, 1000, 1, false); CHECK_EQ(gate_of(&L, &c, &d), SBUFS_E_GATE);   /* read-only */
	sbufs_cdb_sync(&c, 0, 1000, 10);        CHECK_EQ(gate_of(&L, &c, &d), SBUFS_E_GATE);   /* sync = write class */
	sbufs_cdb_tur(&c, 0);                   CHECK_EQ(gate_of(&L, &c, &d), SBUFS_OK);
	sbufs_cdb_tur(&c, 7);                   CHECK_EQ(gate_of(&L, &c, &d), SBUFS_OK);
	sbufs_cdb_tur(&c, SBUFS_WLUN_DEVICE);   CHECK_EQ(gate_of(&L, &c, &d), SBUFS_OK);
	sbufs_cdb_tur(&c, SBUFS_WLUN_RPMB);     CHECK_EQ(gate_of(&L, &c, &d), SBUFS_E_GATE);
	sbufs_cdb_tur(&c, SBUFS_WLUN_BOOT);     CHECK_EQ(gate_of(&L, &c, &d), SBUFS_E_GATE);
	sbufs_cdb_tur(&c, 8);                   CHECK_EQ(gate_of(&L, &c, &d), SBUFS_E_GATE);
	sbufs_cdb_ssu_active(&c);               CHECK_EQ(gate_of(&L, &c, &d), SBUFS_OK);
	c.cdb[4] = 0x30;                        CHECK_EQ(gate_of(&L, &c, &d), SBUFS_E_GATE);   /* power down */
	c.cdb[4] = 0x11;                        CHECK_EQ(gate_of(&L, &c, &d), SBUFS_E_GATE);   /* + START */
	sbufs_cdb_ssu_active(&c); c.lun = 0;    CHECK_EQ(gate_of(&L, &c, &d), SBUFS_E_GATE);
	sbufs_cdb_inquiry(&c, 0, 36);           CHECK_EQ(gate_of(&L, &c, &d), SBUFS_OK);
	sbufs_cdb_inquiry(&c, SBUFS_WLUN_DEVICE, 36); CHECK_EQ(gate_of(&L, &c, &d), SBUFS_E_GATE);
	sbufs_cdb_read_cap10(&c, 0);            CHECK_EQ(gate_of(&L, &c, &d), SBUFS_OK);
	sbufs_cdb_read_cap16(&c, 0);            CHECK_EQ(gate_of(&L, &c, &d), SBUFS_OK);
	/* opcodes off the whitelist, built by hand */
	static const uint8_t bad_ops[] = { 0x42 /* UNMAP */, 0x04 /* FORMAT */, 0x3b /* WRITE BUFFER */,
		                           0xb5 /* SECURITY OUT */, 0x55 /* MODE SELECT */, 0xa0 /* REPORT LUNS */,
		                           0x03 /* REQUEST SENSE */, 0x2f /* VERIFY */, 0x41 /* WRITE SAME */ };
	for (unsigned i = 0; i < sizeof(bad_ops); i++) {
		sbufs_cdb_tur(&c, 0);
		c.cdb[0] = bad_ops[i];
		CHECK_EQ(gate_of(&L, &c, &d), SBUFS_E_GATE);
	}
	/* hand-tampered slots of a valid read */
	struct { unsigned at; uint8_t xor; bool in_utrd; } tam[] = {
		{ 0, 0x01, false },             /* UPIU type */
		{ 1, 0x20, false },             /* W flag added */
		{ 1, 0x40, false },             /* R flag removed */
		{ 2, 0x01, false },             /* LUN */
		{ 4, 0x01, false },             /* command set type */
		{ 11, 0x01, false },            /* data segment length */
		{ 15, 0x01, false },            /* expected length */
		{ 16 + 1, 0x08, false },        /* FUA on a read */
		{ 16 + 6, 0x01, false },        /* group number */
		{ 16 + 9, 0x01, false },        /* control */
		{ 16 + 12, 0x01, false },       /* trailing CDB byte */
		{ 1024 + 15, 0x10, false },     /* PRDT: FMP encryption bits */
		{ 1024 + 12, 0x01, false },     /* PRDT: length */
		{ 1024 + 0, 0x02, false },      /* PRDT: misaligned address */
		{ 3, 0x02, true },              /* UTRD: DD read -> write */
		{ 3, 0x20, true },              /* UTRD: command type */
		{ 24, 0x04, true },             /* UTRD: response length */
		{ 28, 0x10, true },             /* UTRD: PRDT length */
		{ 31, 0x01, true },             /* UTRD: PRDT offset */
	};
	for (unsigned i = 0; i < sizeof(tam) / sizeof(tam[0]); i++) {
		sbufs_cdb_read(&c, 0, 1000, 2);
		CHECK_EQ(gate_of(&L, &c, &d), SBUFS_OK);
		(tam[i].in_utrd ? utrd : ucd)[tam[i].at] ^= tam[i].xor;
		int g = sbufs_gate(&L, utrd, ucd, &F, &d);
		if (g != SBUFS_E_GATE) {
			printf("  tamper %u not refused\n", i);
		}
		CHECK_EQ(g, SBUFS_E_GATE);
	}
	/* writes pass only with the global switch and a WRITABLE range */
	fence_one(true);
	sbufs_cdb_write(&c, 0, 1000, 16, true); CHECK_EQ(gate_of(&L, &c, &d), SBUFS_OK);
	CHECK(d.write);
	sbufs_cdb_write(&c, 0, 1099, 2, false); CHECK_EQ(gate_of(&L, &c, &d), SBUFS_E_GATE);
	sbufs_cdb_sync(&c, 0, 1000, 100);       CHECK_EQ(gate_of(&L, &c, &d), SBUFS_OK);
	sbufs_cdb_sync(&c, 0, 1000, 101);       CHECK_EQ(gate_of(&L, &c, &d), SBUFS_E_GATE);
	sbufs_cdb_sync(&c, 0, 1000, 1); c.cdb[7] = c.cdb[8] = 0;
	CHECK_EQ(gate_of(&L, &c, &d), SBUFS_E_GATE);                    /* SYNC 0 = whole LU */
	F.writes_enabled = false;
	sbufs_cdb_write(&c, 0, 1000, 1, false); CHECK_EQ(gate_of(&L, &c, &d), SBUFS_E_GATE);

	/* mutation fuzz: whatever the gate passes, the device must see as harmless */
	uint64_t rng = 12345, passed = 0, holes = 0, tries = 0;
	F.writes_enabled = false;
	for (int iter = 0; iter < 400000; iter++) {
		rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
		switch (rng % 6) {
		case 0: sbufs_cdb_read(&c, 0, 1000 + (rng >> 8) % 100, 1 + (rng >> 20) % 16); break;
		case 1: sbufs_cdb_write(&c, 0, 1000 + (rng >> 8) % 100, 1 + (rng >> 20) % 4, false); break;
		case 2: sbufs_cdb_tur(&c, 0); break;
		case 3: sbufs_cdb_read(&c, 0, 1000ull + (rng >> 8) % 100 + ((rng >> 30) & 1 ? 1ull << 32 : 0), 1); break;
		case 4: sbufs_cdb_inquiry(&c, 0, 36); break;
		default: sbufs_cdb_ssu_active(&c); break;
		}
		if (c.dir == SBUFS_DIR_WRITE) {
			F.writes_enabled = true;                /* build a passing write, then switch writes off */
		}
		gate_of(&L, &c, &d);
		F.writes_enabled = false;
		unsigned nmut = 1 + (rng >> 40) % 3;
		for (unsigned k = 0; k < nmut; k++) {
			rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
			unsigned at = (unsigned)((rng >> 8) % (32 + 1024 + 64));
			uint8_t v = (uint8_t)(rng >> 32);
			if (at < 32) {
				utrd[at] ^= v ? v : 1;
			} else if (at < 32 + 1024) {
				ucd[at - 32] ^= v ? v : 1;
			} else {
				ucd[1024 + (at - 32 - 1024)] ^= v ? v : 1;
			}
		}
		tries++;
		if (sbufs_gate(&L, utrd, ucd, &F, &d) != SBUFS_OK) {
			continue;
		}
		passed++;
		uint8_t lun;
		bool medium, write;
		uint64_t lba = 0, n = 0;
		bool known = device_view(&lun, &medium, &write, &lba, &n);
		if (!known || write || (medium && (lun != 0 || n == 0 || lba < 1000 || lba + n > 1100 || lba + n < lba)) ||
		    (!medium && lun >= 8 && lun != SBUFS_WLUN_DEVICE)) {
			holes++;
		}
	}
	printf("  gate fuzz: %llu mutated slots, %llu passed the audit, %llu reached the medium outside the fence\n",
	    (unsigned long long)tries, (unsigned long long)passed, (unsigned long long)holes);
	CHECK_EQ(holes, 0);
}

static void
t_response(void)
{
	struct sbufs_layout L;
	struct sbufs_result r;
	sbufs_layout_s9(&L, 1, false);
	memset(utrd, 0, sizeof(utrd));
	memset(ucd, 0, sizeof(ucd));
	uint8_t *rsp = ucd + 512;
	rsp[0] = 0x21; rsp[3] = 4;
	sbufs_parse_response(&L, utrd, ucd, 4, &r);
	CHECK_EQ(r.err, SBUFS_OK);
	sbufs_parse_response(&L, utrd, ucd, 5, &r);     /* another slot's tag */
	CHECK_EQ(r.err, SBUFS_E_IO);
	utrd[8] = 0x0f;                                 /* OCS never written */
	sbufs_parse_response(&L, utrd, ucd, 4, &r);
	CHECK_EQ(r.err, SBUFS_E_IO);
	CHECK_EQ(r.ocs, 0x0f);
	utrd[8] = 0;
	rsp[7] = SBUFS_STATUS_CHECK;
	rsp[33] = 18; rsp[34 + 2] = 6; rsp[34 + 12] = 0x29; rsp[34 + 13] = 1;
	sbufs_parse_response(&L, utrd, ucd, 4, &r);
	CHECK_EQ(r.err, SBUFS_E_IO);
	CHECK_EQ(r.sense_key, 6);
	CHECK_EQ(r.asc, 0x29);
	CHECK_EQ(r.ascq, 1);
	rsp[7] = 0;
	rsp[33] = 0xff; rsp[32] = 0xff;                 /* absurd sense length: clipped, no overread */
	sbufs_parse_response(&L, utrd, ucd, 4, &r);
	CHECK_EQ(r.err, SBUFS_OK);
	rsp[32] = rsp[33] = 0;
	rsp[1] = SBUFS_RESP_FLAG_U; rsp[15] = 0x10;     /* underflow, 16 bytes short */
	sbufs_parse_response(&L, utrd, ucd, 4, &r);
	CHECK_EQ(r.err, SBUFS_E_IO);
	CHECK_EQ(r.residual, 16);
	rsp[1] = 0; rsp[15] = 0;
	rsp[6] = 1;                                     /* target failure */
	sbufs_parse_response(&L, utrd, ucd, 4, &r);
	CHECK_EQ(r.err, SBUFS_E_IO);
}

void
test_proto(void)
{
	t_cdb();
	t_build();
	t_gate();
	t_response();
}
