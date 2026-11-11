/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteUFS fence (see sbufs_fence.h). The policy follows docs/ENGINEER-OS.md section 8 and docs/APPLE-S9-DRIVERS.md
 * 2.6; it is written new here (starbite/libsb/fence.c is GPL-2.0-only and was read for the policy only).
 */
#include "sbufs_fence.h"

static void
say(char *err, size_t errlen, const char *msg)
{
	if (err != NULL && errlen != 0) {
		size_t n = strlen(msg);
		if (n >= errlen) {
			n = errlen - 1;
		}
		memcpy(err, msg, n);
		err[n] = '\0';
	}
}

static void
copy_name(char dst[17], const uint8_t *src)
{
	unsigned i;
	for (i = 0; i < 16 && src[i] != 0; i++) {
		dst[i] = (src[i] >= 0x20 && src[i] < 0x7f) ? (char)src[i] : '?';
	}
	dst[i] = '\0';
}

/* [a, a+an) and [b, b+bn) meet; neither wraps (checked before). */
static bool
meets(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{
	return a < b + bn && b < a + an;
}

static int
header(const uint8_t *p, size_t len, uint32_t magic, uint32_t recsize, uint32_t max, uint32_t *count)
{
	if (p == NULL || len < SBUFS_FENCE_HDR_SIZE) {
		return SBUFS_E_INVAL;
	}
	if (sbufs_le32(p) != magic || sbufs_le32(p + 4) != SBUFS_FENCE_VERSION || sbufs_le32(p + 8) != recsize) {
		return SBUFS_E_INVAL;
	}
	*count = sbufs_le32(p + 12);
	/* exactly count records, or a loader-filled placeholder: whole records up to max, the unused ones zero */
	if (*count > max || len < SBUFS_FENCE_HDR_SIZE + (size_t)*count * recsize ||
	    (len - SBUFS_FENCE_HDR_SIZE) % recsize != 0 || len > SBUFS_FENCE_HDR_SIZE + (size_t)max * recsize) {
		return SBUFS_E_INVAL;
	}
	for (size_t i = SBUFS_FENCE_HDR_SIZE + (size_t)*count * recsize; i < len; i++) {
		if (p[i] != 0) {
			return SBUFS_E_INVAL;
		}
	}
	return SBUFS_OK;
}

int
sbufs_fence_parse(struct sbufs_fence *f, const void *allow, size_t allow_len, const void *deny, size_t deny_len,
    char *err, size_t errlen)
{
	const uint8_t *a = (const uint8_t *)allow, *d = (const uint8_t *)deny;
	uint32_t na = 0, nd = 0;

	memset(f, 0, sizeof(*f));
	if (header(a, allow_len, SBUFS_ALLOW_MAGIC, SBUFS_ALLOW_REC_SIZE, SBUFS_MAX_RANGES, &na) != SBUFS_OK) {
		say(err, errlen, "allowed-ranges: missing or malformed header/length");
		return SBUFS_E_INVAL;
	}
	if (d != NULL && header(d, deny_len, SBUFS_DENY_MAGIC, SBUFS_DENY_REC_SIZE, SBUFS_MAX_DENY, &nd) != SBUFS_OK) {
		say(err, errlen, "denied-ranges: malformed header/length (nothing exposed)");
		return SBUFS_E_INVAL;
	}
	for (uint32_t i = 0; i < na; i++) {
		const uint8_t *r = a + SBUFS_FENCE_HDR_SIZE + (size_t)i * SBUFS_ALLOW_REC_SIZE;
		struct sbufs_range *x = &f->allow[i];
		x->lun = sbufs_le32(r);
		x->flags = sbufs_le32(r + 4);
		x->first = sbufs_le64(r + 8);
		x->count = sbufs_le64(r + 16);
		memcpy(x->uuid, r + 24, 16);
		copy_name(x->name, r + 40);
		if (x->lun >= SBUFS_MAX_GENERAL_LU || (x->flags & ~SBUFS_RANGE_KNOWN_FLAGS) != 0 || x->count == 0 ||
		    x->first > UINT64_MAX - x->count || sbufs_le64(r + 56) != 0) {
			memset(f, 0, sizeof(*f));
			say(err, errlen, "allowed-ranges: a record is malformed (lun, flags, empty, wrapping or reserved): nothing exposed");
			return SBUFS_E_INVAL;
		}
		x->why = "not sealed";
	}
	for (uint32_t i = 0; i < nd; i++) {
		const uint8_t *r = d + SBUFS_FENCE_HDR_SIZE + (size_t)i * SBUFS_DENY_REC_SIZE;
		struct sbufs_extent *x = &f->deny[i];
		x->lun = sbufs_le32(r);
		x->first = sbufs_le64(r + 8);
		x->count = sbufs_le64(r + 16);
		copy_name(x->name, r + 24);
		/* a denied extent that wraps is taken to the end of the LU's address space: denying more is safe */
		if (x->count == 0 || sbufs_le32(r + 4) != 0) {
			memset(f, 0, sizeof(*f));
			say(err, errlen, "denied-ranges: a record is malformed (empty or reserved bits): nothing exposed");
			return SBUFS_E_INVAL;
		}
		if (x->first > UINT64_MAX - x->count) {
			x->count = UINT64_MAX - x->first;
		}
	}
	f->nallow = na;
	f->ndeny = nd;
	say(err, errlen, na ? "ok" : "allowed-ranges: empty, nothing exposed");
	return SBUFS_OK;
}

void
sbufs_fence_set_capacity(struct sbufs_fence *f, uint32_t lun, uint64_t blocks)
{
	if (lun < SBUFS_MAX_GENERAL_LU && !f->sealed) {
		f->lu_blocks[lun] = blocks;
	}
}

static int
floors(const struct sbufs_fence *f, uint32_t lun, uint64_t lba, uint64_t n)
{
	uint64_t cap = f->lu_blocks[lun];
	if (cap <= SBUFS_HEAD_FLOOR + SBUFS_TAIL_FLOOR || lba < SBUFS_HEAD_FLOOR ||
	    lba + n > cap - SBUFS_TAIL_FLOOR) {
		return SBUFS_FENCE_FLOOR;
	}
	return SBUFS_FENCE_OK;
}

static const struct sbufs_extent *
denied(const struct sbufs_fence *f, uint32_t lun, uint64_t lba, uint64_t n)
{
	for (unsigned i = 0; i < f->ndeny; i++) {
		const struct sbufs_extent *e = &f->deny[i];
		if (e->lun == lun && meets(lba, n, e->first, e->count)) {
			return e;
		}
	}
	return NULL;
}

unsigned
sbufs_fence_seal(struct sbufs_fence *f)
{
	unsigned usable = 0;
	for (unsigned i = 0; i < f->nallow; i++) {
		struct sbufs_range *r = &f->allow[i];
		r->usable = false;
		if (f->lu_blocks[r->lun] == 0) {
			r->why = "LU capacity unknown (not probed or not 4 KiB blocks)";
			continue;
		}
		if (floors(f, r->lun, r->first, r->count) != SBUFS_FENCE_OK) {
			r->why = "meets the GPT head/tail floor or runs past the LU";
			continue;
		}
		if (denied(f, r->lun, r->first, r->count) != NULL) {
			r->why = "meets a denied extent";
			continue;
		}
		bool overlap = false;
		for (unsigned j = 0; j < i; j++) {
			const struct sbufs_range *o = &f->allow[j];
			if (o->usable && o->lun == r->lun && meets(r->first, r->count, o->first, o->count)) {
				overlap = true;
			}
		}
		if (overlap) {
			r->why = "overlaps an earlier allowed range";
			continue;
		}
		r->usable = true;
		r->why = "ok";
		usable++;
	}
	f->sealed = true;
	return usable;
}

int
sbufs_fence_check_quiet(const struct sbufs_fence *f, uint32_t lun, uint64_t lba, uint64_t n, bool write)
{
	if (n == 0) {
		return SBUFS_FENCE_EMPTY;
	}
	if (lba > UINT64_MAX - n) {
		return SBUFS_FENCE_WRAP;
	}
	if (lun >= SBUFS_MAX_GENERAL_LU) {
		return SBUFS_FENCE_BAD_LUN;
	}
	if (!f->sealed) {
		return SBUFS_FENCE_OUTSIDE;
	}
	if (floors(f, lun, lba, n) != SBUFS_FENCE_OK) {
		return SBUFS_FENCE_FLOOR;
	}
	if (denied(f, lun, lba, n) != NULL) {
		return SBUFS_FENCE_DENIED;
	}
	for (unsigned i = 0; i < f->nallow; i++) {
		const struct sbufs_range *r = &f->allow[i];
		if (!r->usable || r->lun != lun || lba < r->first || lba - r->first >= r->count ||
		    n > r->count - (lba - r->first)) {
			continue;
		}
		if (write && (!f->writes_enabled || !(r->flags & SBUFS_RANGE_WRITABLE))) {
			return SBUFS_FENCE_READONLY;
		}
		return SBUFS_FENCE_OK;
	}
	return SBUFS_FENCE_OUTSIDE;
}

static int
tally(struct sbufs_fence *f, int v, uint64_t n, bool write)
{
	f->checked++;
	if (v == SBUFS_FENCE_OK) {
		f->blocks_ok += n;
	} else {
		f->refused++;
		if (write) {
			f->refused_write++;
		}
	}
	return v;
}

int
sbufs_fence_check(struct sbufs_fence *f, uint32_t lun, uint64_t lba, uint64_t n, bool write)
{
	return tally(f, sbufs_fence_check_quiet(f, lun, lba, n, write), n, write);
}

int
sbufs_fence_map(struct sbufs_fence *f, unsigned range, uint64_t block, uint64_t nblks, bool write, uint32_t *lun,
    uint64_t *lba)
{
	if (range >= f->nallow || !f->allow[range].usable || !f->sealed) {
		return tally(f, SBUFS_FENCE_BAD_RANGE, nblks, write);
	}
	const struct sbufs_range *r = &f->allow[range];
	if (nblks == 0) {
		return tally(f, SBUFS_FENCE_EMPTY, nblks, write);
	}
	if (block > UINT64_MAX - nblks) {
		return tally(f, SBUFS_FENCE_WRAP, nblks, write);
	}
	if (block >= r->count || nblks > r->count - block) {
		return tally(f, SBUFS_FENCE_OUTSIDE, nblks, write);
	}
	int v = sbufs_fence_check(f, r->lun, r->first + block, nblks, write);
	if (v == SBUFS_FENCE_OK) {
		*lun = r->lun;
		*lba = r->first + block;
	}
	return v;
}

bool
sbufs_fence_range_writable(const struct sbufs_fence *f, unsigned range)
{
	return range < f->nallow && f->allow[range].usable && f->writes_enabled &&
	       (f->allow[range].flags & SBUFS_RANGE_WRITABLE);
}

const char *
sbufs_fence_verdict_name(int v)
{
	switch (v) {
	case SBUFS_FENCE_OK: return "ok";
	case SBUFS_FENCE_EMPTY: return "empty";
	case SBUFS_FENCE_WRAP: return "wraps";
	case SBUFS_FENCE_BAD_LUN: return "not a general LU";
	case SBUFS_FENCE_OUTSIDE: return "outside every allowed range";
	case SBUFS_FENCE_DENIED: return "denied extent";
	case SBUFS_FENCE_FLOOR: return "GPT floor / past the LU";
	case SBUFS_FENCE_READONLY: return "read-only";
	case SBUFS_FENCE_BAD_RANGE: return "no such range";
	default: return "?";
	}
}
