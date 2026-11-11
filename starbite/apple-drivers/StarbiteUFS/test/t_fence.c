/* SPDX-License-Identifier: APSL-2.0 */
/* Fence tests: property parsing (fail closed), sealing, edges, off-by-one, overflow, and an exhaustive comparison
 * against a block-by-block reference model over many random layouts. */
#include "test.h"

void le32w(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
void le64w(uint8_t *p, uint64_t v) { le32w(p, (uint32_t)v); le32w(p + 4, (uint32_t)(v >> 32)); }

void
blob_allow_begin(struct blob *x)
{
	memset(x, 0, sizeof(*x));
	le32w(x->b, SBUFS_ALLOW_MAGIC);
	le32w(x->b + 4, SBUFS_FENCE_VERSION);
	le32w(x->b + 8, SBUFS_ALLOW_REC_SIZE);
	x->len = 16;
}

void
blob_allow_add(struct blob *x, uint32_t lun, uint32_t flags, uint64_t first, uint64_t count, const char *name)
{
	uint8_t *r = x->b + x->len;
	le32w(r, lun);
	le32w(r + 4, flags);
	le64w(r + 8, first);
	le64w(r + 16, count);
	for (int i = 0; i < 16; i++) {
		r[24 + i] = (uint8_t)(0xa0 + i + x->len);
	}
	strncpy((char *)r + 40, name, 16);
	x->len += SBUFS_ALLOW_REC_SIZE;
	le32w(x->b + 12, sbufs_le32(x->b + 12) + 1);
}

void
blob_deny_begin(struct blob *x)
{
	memset(x, 0, sizeof(*x));
	le32w(x->b, SBUFS_DENY_MAGIC);
	le32w(x->b + 4, SBUFS_FENCE_VERSION);
	le32w(x->b + 8, SBUFS_DENY_REC_SIZE);
	x->len = 16;
}

void
blob_deny_add(struct blob *x, uint32_t lun, uint64_t first, uint64_t count, const char *name)
{
	uint8_t *r = x->b + x->len;
	le32w(r, lun);
	le64w(r + 8, first);
	le64w(r + 16, count);
	strncpy((char *)r + 24, name, 16);
	x->len += SBUFS_DENY_REC_SIZE;
	le32w(x->b + 12, sbufs_le32(x->b + 12) + 1);
}

static struct sbufs_fence F;

static int
parse(const struct blob *a, const struct blob *d)
{
	char err[160];
	return sbufs_fence_parse(&F, a ? a->b : NULL, a ? a->len : 0, d ? d->b : NULL, d ? d->len : 0, err, sizeof(err));
}

static void
t_parse(void)
{
	struct blob a, d, bad;
	blob_allow_begin(&a);
	blob_allow_add(&a, 0, SBUFS_RANGE_ROOT, 1698432, 13917563, "USERDATA");
	blob_allow_add(&a, 0, SBUFS_RANGE_WRITABLE, 100000, 5000, "SBSTORE");
	blob_deny_begin(&d);
	blob_deny_add(&d, 0, 6, 1000, "EFS");
	CHECK_EQ(parse(&a, &d), SBUFS_OK);
	CHECK_EQ(F.nallow, 2);
	CHECK_EQ(F.ndeny, 1);
	CHECK_EQ(F.allow[0].first, 1698432);
	CHECK_EQ(F.allow[0].count, 13917563);
	CHECK(strcmp(F.allow[0].name, "USERDATA") == 0);
	CHECK(strcmp(F.deny[0].name, "EFS") == 0);
	CHECK(!F.sealed && !F.writes_enabled);

	/* every malformation fails closed: nothing exposed */
	CHECK_EQ(parse(NULL, NULL), SBUFS_E_INVAL);
	CHECK_EQ(F.nallow, 0);
	bad = a; bad.len = 15;                          CHECK_EQ(parse(&bad, NULL), SBUFS_E_INVAL); CHECK_EQ(F.nallow, 0);
	bad = a; bad.len += 1;                          CHECK_EQ(parse(&bad, NULL), SBUFS_E_INVAL);
	bad = a; bad.len -= 1;                          CHECK_EQ(parse(&bad, NULL), SBUFS_E_INVAL);
	bad = a; bad.b[0] ^= 1;                         CHECK_EQ(parse(&bad, NULL), SBUFS_E_INVAL);
	bad = a; le32w(bad.b + 4, 2);                   CHECK_EQ(parse(&bad, NULL), SBUFS_E_INVAL);
	bad = a; le32w(bad.b + 8, 56);                  CHECK_EQ(parse(&bad, NULL), SBUFS_E_INVAL);
	bad = a; le32w(bad.b + 12, 3);                  CHECK_EQ(parse(&bad, NULL), SBUFS_E_INVAL);
	bad = a; le32w(bad.b + 12, 0xffffffffu);        CHECK_EQ(parse(&bad, NULL), SBUFS_E_INVAL);  /* count*size overflow */
	bad = a; le32w(bad.b + 16 + 64, 8);             CHECK_EQ(parse(&bad, NULL), SBUFS_E_INVAL);  /* lun 8 */
	bad = a; le32w(bad.b + 16, 0xd0);               CHECK_EQ(parse(&bad, NULL), SBUFS_E_INVAL);  /* W-LUN */
	bad = a; le32w(bad.b + 16 + 4, 0x4);            CHECK_EQ(parse(&bad, NULL), SBUFS_E_INVAL);  /* unknown flag */
	bad = a; le64w(bad.b + 16 + 16, 0);             CHECK_EQ(parse(&bad, NULL), SBUFS_E_INVAL);  /* empty */
	bad = a; le64w(bad.b + 16 + 56, 1);             CHECK_EQ(parse(&bad, NULL), SBUFS_E_INVAL);  /* reserved */
	bad = a; le64w(bad.b + 16 + 8, UINT64_MAX); le64w(bad.b + 16 + 16, 1);
	CHECK_EQ(parse(&bad, NULL), SBUFS_E_INVAL);     /* first + count = 2^64: wraps */
	bad = a; le64w(bad.b + 16 + 8, UINT64_MAX - 9); le64w(bad.b + 16 + 16, 10);
	CHECK_EQ(parse(&bad, NULL), SBUFS_E_INVAL);
	bad = a; le64w(bad.b + 16 + 8, UINT64_MAX - 10); le64w(bad.b + 16 + 16, 10);
	CHECK_EQ(parse(&bad, NULL), SBUFS_OK);          /* ends exactly at 2^64-1: parses, the floors drop it */
	sbufs_fence_set_capacity(&F, 0, 1u << 24);
	CHECK_EQ(sbufs_fence_seal(&F), 1);
	CHECK(!F.allow[0].usable);
	/* a loader-filled placeholder: capacity records, the unused ones zero */
	bad = a; bad.len = 16 + 8 * 64;
	CHECK_EQ(parse(&bad, NULL), SBUFS_OK);
	CHECK_EQ(F.nallow, 2);
	bad.b[16 + 2 * 64 + 7] = 1;                     /* garbage in an unused record */
	CHECK_EQ(parse(&bad, NULL), SBUFS_E_INVAL);
	CHECK_EQ(F.nallow, 0);
	bad = a; bad.len = 16 + 9 * 64;                 /* more room than SBUFS_MAX_RANGES */
	CHECK_EQ(parse(&bad, NULL), SBUFS_E_INVAL);
	bad = a; bad.len = 16 + 3 * 64 + 8;             /* not whole records */
	CHECK_EQ(parse(&bad, NULL), SBUFS_E_INVAL);
	blob_allow_begin(&bad); bad.len = 16 + 8 * 64;  /* an unfilled placeholder: parses, exposes nothing */
	CHECK_EQ(parse(&bad, NULL), SBUFS_OK);
	CHECK_EQ(F.nallow, 0);
	memset(bad.b, 0, 16);                           /* zero header (no magic): refused */
	CHECK_EQ(parse(&bad, NULL), SBUFS_E_INVAL);
	/* nine ranges is one too many */
	bad = a;
	for (int i = 0; i < 7; i++) {
		blob_allow_add(&bad, 1, 0, 10 + i * 10, 5, "X");
	}
	CHECK_EQ(parse(&bad, NULL), SBUFS_E_INVAL);
	/* malformed deny-list: nothing exposed either */
	bad = d; bad.len += 1;                          CHECK_EQ(parse(&a, &bad), SBUFS_E_INVAL); CHECK_EQ(F.nallow, 0);
	bad = d; le64w(bad.b + 16 + 16, 0);             CHECK_EQ(parse(&a, &bad), SBUFS_E_INVAL); CHECK_EQ(F.nallow, 0);
	bad = d; le32w(bad.b + 16 + 4, 1);              CHECK_EQ(parse(&a, &bad), SBUFS_E_INVAL);
	bad = d; bad.b[0] = 'X';                        CHECK_EQ(parse(&a, &bad), SBUFS_E_INVAL);
	/* a wrapping denied extent is clipped, not refused (denying more is safe) */
	bad = d; le64w(bad.b + 16 + 8, 50); le64w(bad.b + 16 + 16, UINT64_MAX);
	CHECK_EQ(parse(&a, &bad), SBUFS_OK);
	CHECK_EQ(F.deny[0].count, UINT64_MAX - 50);
	/* an empty allow-list parses and exposes nothing */
	blob_allow_begin(&bad);
	CHECK_EQ(parse(&bad, NULL), SBUFS_OK);
	CHECK_EQ(F.nallow, 0);
	CHECK_EQ(sbufs_fence_seal(&F), 0);
	CHECK_EQ(sbufs_fence_check(&F, 0, 100, 1, false), SBUFS_FENCE_FLOOR);   /* no LU probed */
}

static void
t_seal(void)
{
	struct blob a, d;
	const uint64_t cap = 1000;

	blob_allow_begin(&a);
	blob_allow_add(&a, 0, 0, 5, 10, "HEAD-1");      /* 0: starts one block into the GPT head */
	blob_allow_add(&a, 0, 0, 6, 10, "HEAD-OK");     /* 1: first block after the head floor */
	blob_allow_add(&a, 0, 0, 984, 10, "TAIL-OK");   /* 2: ends at cap - 6 */
	blob_allow_add(&a, 0, 0, 985, 10, "TAIL+1");    /* 3: one block into the tail floor */
	blob_allow_add(&a, 0, 0, 90, 10, "BEFORE-DENY");/* 4: [90,100) ends where the deny starts */
	blob_allow_add(&a, 0, 0, 90, 11, "INTO-DENY");  /* 5: [90,101): one block into it */
	blob_allow_add(&a, 0, 0, 110, 20, "AFTER-DENY");/* 6: [110,130) starts where it ends */
	blob_allow_add(&a, 0, 0, 129, 5, "OVERLAP");    /* 7: overlaps 6 by one block */
	blob_deny_begin(&d);
	blob_deny_add(&d, 0, 100, 10, "EFS");
	blob_deny_add(&d, 1, 0, 1000, "OTHER-LU");
	CHECK_EQ(parse(&a, &d), SBUFS_OK);
	/* before the seal nothing is usable */
	CHECK_EQ(sbufs_fence_check(&F, 0, 10, 1, false), SBUFS_FENCE_OUTSIDE);
	CHECK_EQ(sbufs_fence_seal(&F), 0);              /* capacity unknown: nothing */
	CHECK(strstr(F.allow[1].why, "capacity") != NULL);
	F.sealed = false;
	sbufs_fence_set_capacity(&F, 0, cap);
	CHECK_EQ(sbufs_fence_seal(&F), 4);
	CHECK(!F.allow[0].usable);
	CHECK(F.allow[1].usable);
	CHECK(F.allow[2].usable);
	CHECK(!F.allow[3].usable);
	CHECK(F.allow[4].usable);
	CHECK(!F.allow[5].usable);
	CHECK(F.allow[6].usable);
	CHECK(!F.allow[7].usable);
	CHECK(strstr(F.allow[7].why, "overlaps") != NULL);
	/* capacity is frozen after the seal */
	sbufs_fence_set_capacity(&F, 0, 5000);
	CHECK_EQ(F.lu_blocks[0], cap);
	/* a tiny LU has no usable block at all */
	blob_allow_begin(&a);
	blob_allow_add(&a, 2, 0, 6, 1, "TINY");
	CHECK_EQ(parse(&a, NULL), SBUFS_OK);
	sbufs_fence_set_capacity(&F, 2, 12);
	CHECK_EQ(sbufs_fence_seal(&F), 0);
	CHECK_EQ(parse(&a, NULL), SBUFS_OK);
	sbufs_fence_set_capacity(&F, 2, 13);
	CHECK_EQ(sbufs_fence_seal(&F), 1);
}

static void
t_check_and_map(void)
{
	struct blob a, d;
	uint32_t lun;
	uint64_t lba;

	blob_allow_begin(&a);
	blob_allow_add(&a, 0, SBUFS_RANGE_ROOT, 1000, 100, "ROOT");             /* [1000,1100) read-only */
	blob_allow_add(&a, 0, SBUFS_RANGE_WRITABLE, 1100, 50, "DATA");          /* [1100,1150) adjacent, writable */
	blob_allow_add(&a, 3, 0, 4294967290ull, 20, "HIGH");                    /* crosses 2^32 */
	blob_deny_begin(&d);
	blob_deny_add(&d, 0, 2000, 10, "PARAM");
	CHECK_EQ(parse(&a, &d), SBUFS_OK);
	sbufs_fence_set_capacity(&F, 0, 100000);
	sbufs_fence_set_capacity(&F, 3, 1ull << 33);
	CHECK_EQ(sbufs_fence_seal(&F), 3);

	CHECK_EQ(sbufs_fence_check(&F, 0, 1000, 100, false), SBUFS_FENCE_OK);
	CHECK_EQ(sbufs_fence_check(&F, 0, 999, 1, false), SBUFS_FENCE_OUTSIDE);
	CHECK_EQ(sbufs_fence_check(&F, 0, 999, 2, false), SBUFS_FENCE_OUTSIDE);
	CHECK_EQ(sbufs_fence_check(&F, 0, 1099, 1, false), SBUFS_FENCE_OK);
	CHECK_EQ(sbufs_fence_check(&F, 0, 1099, 2, false), SBUFS_FENCE_OUTSIDE);  /* spans two ranges: refused */
	CHECK_EQ(sbufs_fence_check(&F, 0, 1149, 1, false), SBUFS_FENCE_OK);
	CHECK_EQ(sbufs_fence_check(&F, 0, 1149, 2, false), SBUFS_FENCE_OUTSIDE);
	CHECK_EQ(sbufs_fence_check(&F, 0, 1150, 1, false), SBUFS_FENCE_OUTSIDE);
	CHECK_EQ(sbufs_fence_check(&F, 0, 1000, 0, false), SBUFS_FENCE_EMPTY);
	CHECK_EQ(sbufs_fence_check(&F, 0, 2005, 1, false), SBUFS_FENCE_DENIED);
	CHECK_EQ(sbufs_fence_check(&F, 0, 0, 1, false), SBUFS_FENCE_FLOOR);
	CHECK_EQ(sbufs_fence_check(&F, 0, 99999, 1, false), SBUFS_FENCE_FLOOR);
	CHECK_EQ(sbufs_fence_check(&F, 0, 100000, 1, false), SBUFS_FENCE_FLOOR);
	CHECK_EQ(sbufs_fence_check(&F, 0, UINT64_MAX, 1, false), SBUFS_FENCE_WRAP);     /* ends at 2^64 */
	CHECK_EQ(sbufs_fence_check(&F, 0, UINT64_MAX - 1, 1, false), SBUFS_FENCE_FLOOR);
	CHECK_EQ(sbufs_fence_check(&F, 0, UINT64_MAX, 2, false), SBUFS_FENCE_WRAP);
	CHECK_EQ(sbufs_fence_check(&F, 0, 1000, UINT64_MAX, false), SBUFS_FENCE_WRAP);
	CHECK_EQ(sbufs_fence_check(&F, 0, 1, UINT64_MAX, false), SBUFS_FENCE_WRAP);
	CHECK_EQ(sbufs_fence_check(&F, 0, 0, UINT64_MAX, false), SBUFS_FENCE_FLOOR);
	CHECK_EQ(sbufs_fence_check(&F, 1, 1000, 1, false), SBUFS_FENCE_FLOOR);    /* LU 1 never probed */
	CHECK_EQ(sbufs_fence_check(&F, 8, 1000, 1, false), SBUFS_FENCE_BAD_LUN);
	CHECK_EQ(sbufs_fence_check(&F, SBUFS_WLUN_BOOT, 1000, 1, false), SBUFS_FENCE_BAD_LUN);
	CHECK_EQ(sbufs_fence_check(&F, SBUFS_WLUN_RPMB, 0, 1, false), SBUFS_FENCE_BAD_LUN);
	CHECK_EQ(sbufs_fence_check(&F, 0xffffffffu, 1000, 1, false), SBUFS_FENCE_BAD_LUN);
	/* the 2^32 crossing: 32-bit truncation would land at block 0..13 */
	CHECK_EQ(sbufs_fence_check(&F, 3, 4294967290ull, 20, false), SBUFS_FENCE_OK);
	CHECK_EQ(sbufs_fence_check(&F, 3, 4294967290ull, 21, false), SBUFS_FENCE_OUTSIDE);
	CHECK_EQ(sbufs_fence_check(&F, 3, 13, 1, false), SBUFS_FENCE_OUTSIDE);

	/* writes: off globally */
	CHECK_EQ(sbufs_fence_check(&F, 0, 1100, 1, true), SBUFS_FENCE_READONLY);
	CHECK_EQ(sbufs_fence_check(&F, 0, 1000, 1, true), SBUFS_FENCE_READONLY);
	CHECK(!sbufs_fence_range_writable(&F, 1));
	F.writes_enabled = true;
	CHECK_EQ(sbufs_fence_check(&F, 0, 1100, 50, true), SBUFS_FENCE_OK);
	CHECK_EQ(sbufs_fence_check(&F, 0, 1000, 1, true), SBUFS_FENCE_READONLY);  /* range not WRITABLE */
	CHECK_EQ(sbufs_fence_check(&F, 0, 1150, 1, true), SBUFS_FENCE_OUTSIDE);
	CHECK(sbufs_fence_range_writable(&F, 1));
	CHECK(!sbufs_fence_range_writable(&F, 0));
	F.writes_enabled = false;

	/* map: range-relative */
	uint64_t refused0 = F.refused;
	CHECK_EQ(sbufs_fence_map(&F, 0, 0, 1, false, &lun, &lba), SBUFS_FENCE_OK);
	CHECK_EQ(lun, 0);
	CHECK_EQ(lba, 1000);
	CHECK_EQ(sbufs_fence_map(&F, 0, 99, 1, false, &lun, &lba), SBUFS_FENCE_OK);
	CHECK_EQ(lba, 1099);
	lba = 7;
	CHECK_EQ(sbufs_fence_map(&F, 0, 100, 1, false, &lun, &lba), SBUFS_FENCE_OUTSIDE);
	CHECK_EQ(lba, 7);                               /* untouched on refusal */
	CHECK_EQ(sbufs_fence_map(&F, 0, 99, 2, false, &lun, &lba), SBUFS_FENCE_OUTSIDE);
	CHECK_EQ(sbufs_fence_map(&F, 0, 0, 100, false, &lun, &lba), SBUFS_FENCE_OK);
	CHECK_EQ(sbufs_fence_map(&F, 0, 0, 101, false, &lun, &lba), SBUFS_FENCE_OUTSIDE);
	CHECK_EQ(sbufs_fence_map(&F, 0, 0, 0, false, &lun, &lba), SBUFS_FENCE_EMPTY);
	CHECK_EQ(sbufs_fence_map(&F, 0, UINT64_MAX, 1, false, &lun, &lba), SBUFS_FENCE_WRAP);
	CHECK_EQ(sbufs_fence_map(&F, 0, 1, UINT64_MAX, false, &lun, &lba), SBUFS_FENCE_WRAP);
	CHECK_EQ(sbufs_fence_map(&F, 0, UINT64_MAX - 5, 5, false, &lun, &lba), SBUFS_FENCE_OUTSIDE);
	CHECK_EQ(sbufs_fence_map(&F, 0, 50, UINT64_MAX - 50, false, &lun, &lba), SBUFS_FENCE_OUTSIDE);
	CHECK_EQ(sbufs_fence_map(&F, 3, 0, 1, false, &lun, &lba), SBUFS_FENCE_BAD_RANGE);
	CHECK_EQ(sbufs_fence_map(&F, 99, 0, 1, false, &lun, &lba), SBUFS_FENCE_BAD_RANGE);
	CHECK_EQ(sbufs_fence_map(&F, 1, 0, 1, true, &lun, &lba), SBUFS_FENCE_READONLY);
	CHECK_EQ(sbufs_fence_map(&F, 2, 5, 1, false, &lun, &lba), SBUFS_FENCE_OK);
	CHECK_EQ(lba, 4294967295ull);
	CHECK_EQ(sbufs_fence_map(&F, 2, 6, 1, false, &lun, &lba), SBUFS_FENCE_OK);
	CHECK_EQ(lba, 4294967296ull);
	CHECK_EQ(F.refused - refused0, 11);
	F.allow[1].usable = false;                      /* an unusable range cannot be mapped */
	CHECK_EQ(sbufs_fence_map(&F, 1, 0, 1, false, &lun, &lba), SBUFS_FENCE_BAD_RANGE);
	for (int v = 0; v <= SBUFS_FENCE_BAD_RANGE + 1; v++) {
		CHECK(sbufs_fence_verdict_name(v) != NULL);
	}
}

/* ---- the reference model: block by block, written without the fence's arithmetic ---- */

struct ref {
	uint64_t cap[8];
	struct { uint32_t lun, flags; uint64_t first, count; bool usable; } r[8];
	unsigned nr;
	struct { uint32_t lun; uint64_t first, count; } d[8];
	unsigned nd;
	bool wen;
};

static bool
ref_block_ok(const struct ref *R, uint32_t lun, uint64_t b)
{
	if (lun >= 8 || R->cap[lun] <= 12 || b < 6 || b >= R->cap[lun] - 6) {
		return false;
	}
	for (unsigned i = 0; i < R->nd; i++) {
		if (R->d[i].lun == lun && b >= R->d[i].first && b - R->d[i].first < R->d[i].count) {
			return false;
		}
	}
	return true;
}

static void
ref_seal(struct ref *R)
{
	for (unsigned i = 0; i < R->nr; i++) {
		R->r[i].usable = true;
		for (uint64_t b = R->r[i].first; b < R->r[i].first + R->r[i].count; b++) {
			if (!ref_block_ok(R, R->r[i].lun, b)) {
				R->r[i].usable = false;
				break;
			}
			for (unsigned j = 0; j < i; j++) {
				if (R->r[j].usable && R->r[j].lun == R->r[i].lun && b >= R->r[j].first &&
				    b < R->r[j].first + R->r[j].count) {
					R->r[i].usable = false;
				}
			}
			if (!R->r[i].usable) {
				break;
			}
		}
	}
}

static bool
ref_ok(const struct ref *R, uint32_t lun, uint64_t lba, uint64_t n, bool w)
{
	if (n == 0 || n > 200) {                        /* every LU here is < 200 blocks: a longer one cannot fit */
		return false;
	}
	if (lba > UINT64_MAX - n) {
		return false;
	}
	for (uint64_t b = lba; b < lba + n; b++) {
		if (!ref_block_ok(R, lun, b)) {
			return false;
		}
	}
	for (unsigned i = 0; i < R->nr; i++) {
		if (R->r[i].usable && R->r[i].lun == lun && lba >= R->r[i].first &&
		    lba + n <= R->r[i].first + R->r[i].count) {
			return !w || (R->wen && (R->r[i].flags & SBUFS_RANGE_WRITABLE));
		}
	}
	return false;
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(uint64_t n) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return n ? rng % n : rng; }

static void
t_exhaustive(void)
{
	static const uint64_t big[] = { 0, 1, 63, 64, 65, 199, 200, 0xffffffffull, 0x100000000ull, UINT64_MAX / 2,
		                        UINT64_MAX - 70, UINT64_MAX - 1, UINT64_MAX };
	uint64_t mismatches = 0, cases = 0, oks = 0;

	for (int layout = 0; layout < 300; layout++) {
		struct ref R;
		struct blob a, d;
		memset(&R, 0, sizeof(R));
		R.cap[0] = 40 + rnd(150);
		R.cap[1] = 5 + rnd(60);
		R.cap[5] = rnd(2) ? 0 : 64;                     /* sometimes never probed */
		R.nr = 1 + (unsigned)rnd(8);
		R.nd = (unsigned)rnd(5);
		R.wen = rnd(2);
		blob_allow_begin(&a);
		for (unsigned i = 0; i < R.nr; i++) {
			static const uint32_t luns[] = { 0, 0, 0, 1, 5 };
			R.r[i].lun = luns[rnd(5)];
			R.r[i].flags = (uint32_t)rnd(4);
			R.r[i].first = rnd(R.cap[0] + 5);
			R.r[i].count = 1 + rnd(40);
			blob_allow_add(&a, R.r[i].lun, R.r[i].flags, R.r[i].first, R.r[i].count, "R");
		}
		blob_deny_begin(&d);
		for (unsigned i = 0; i < R.nd; i++) {
			R.d[i].lun = (uint32_t)rnd(2);
			R.d[i].first = rnd(R.cap[0]);
			R.d[i].count = 1 + rnd(20);
			blob_deny_add(&d, R.d[i].lun, R.d[i].first, R.d[i].count, "D");
		}
		CHECK_EQ(parse(&a, R.nd ? &d : NULL), SBUFS_OK);
		for (unsigned l = 0; l < 8; l++) {
			if (R.cap[l]) {
				sbufs_fence_set_capacity(&F, l, R.cap[l]);
			}
		}
		F.writes_enabled = R.wen;
		sbufs_fence_seal(&F);
		ref_seal(&R);
		for (unsigned i = 0; i < R.nr; i++) {
			if (F.allow[i].usable != R.r[i].usable) {
				mismatches++;
			}
		}
		static const uint32_t luns[] = { 0, 1, 2, 5, 7, 8, 0xd0, 0xffffffffu };
		for (unsigned li = 0; li < 8; li++) {
			for (uint64_t lba = 0; lba < 13 + 200; lba++) {
				uint64_t L = lba < 200 ? lba : big[lba - 200];
				for (uint64_t ni = 0; ni < 60 + 13; ni++) {
					uint64_t n = ni < 60 ? ni : big[ni - 60];
					for (int w = 0; w < 2; w++) {
						bool want = ref_ok(&R, luns[li], L, n, w);
						bool got = sbufs_fence_check_quiet(&F, luns[li], L, n, w) == SBUFS_FENCE_OK;
						cases++;
						oks += got;
						if (want != got) {
							if (mismatches++ < 5) {
								printf("  exhaustive mismatch: layout %d lun %u lba %llu n %llu w %d: ref %d fence %d\n",
								    layout, luns[li], (unsigned long long)L, (unsigned long long)n, w, want, got);
							}
						}
					}
				}
			}
		}
		/* map agrees with check for every range-relative request */
		for (unsigned i = 0; i < R.nr; i++) {
			for (uint64_t b = 0; b < 45; b++) {
				for (uint64_t n = 0; n < 45; n++) {
					uint32_t lun = 99;
					uint64_t lba = 0;
					bool got = sbufs_fence_map(&F, i, b, n, false, &lun, &lba) == SBUFS_FENCE_OK;
					bool want = R.r[i].usable && n > 0 && b + n <= R.r[i].count &&
					    ref_ok(&R, R.r[i].lun, R.r[i].first + b, n, false);
					if (got != want || (got && (lun != R.r[i].lun || lba != R.r[i].first + b))) {
						mismatches++;
					}
				}
			}
		}
	}
	printf("  fence exhaustive: %llu cases over 300 random layouts, %llu allowed, %llu mismatches\n",
	    (unsigned long long)cases, (unsigned long long)oks, (unsigned long long)mismatches);
	CHECK_EQ(mismatches, 0);
	CHECK(oks > 1000);
}

void
test_fence(void)
{
	t_parse();
	t_seal();
	t_check_and_map();
	t_exhaustive();
}
