/*
 * Just enough flattened-device-tree parsing to find what ABL put in
 * /chosen (initrd location). Big-endian, 4-byte aligned tokens.
 */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	/* byte loads are fine on Device memory as long as they are aligned to 1 */
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

static int chosen_prop(uintptr_t fdt, const char *prop, uint64_t *out, const uint64_t *set)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	int depth = 0, in_chosen = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_chosen = depth == 2 && streq(name, "chosen");
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (in_chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_chosen && streq((const char *)(strings + nameoff), prop)) {
				if (len != 4 && len != 8)
					return 0;
				if (set) {
					volatile uint32_t *w = (volatile uint32_t *)val;
					if (len == 4)
						w[0] = __builtin_bswap32((uint32_t)*set);
					else {
						w[0] = __builtin_bswap32((uint32_t)(*set >> 32));
						w[1] = __builtin_bswap32((uint32_t)*set);
					}
				}
				*out = len == 4 ? be32(val) : (uint64_t)be32(val) << 32 | be32(val + 4);
				return 1;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 0;
		}
	}
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	return chosen_prop(fdt, prop, out, 0);
}

int fdt_chosen_set_u64(uintptr_t fdt, const char *prop, uint64_t value)
{
	uint64_t now;
	return chosen_prop(fdt, prop, &now, &value);
}

/*
 * Raw value of a /chosen property (e.g. "bootargs", S-Boot's command line):
 * *val = address inside the FDT, *len = length in bytes. Returns 0 if absent.
 */
int fdt_chosen_raw(uintptr_t fdt, const char *prop, uintptr_t *val, uint32_t *len)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);
	uintptr_t strings = fdt + be32(fdt + 12);
	int depth = 0, in_chosen = 0;
	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		if (tok == FDT_BEGIN_NODE) {
			depth++;
			in_chosen = depth == 2 && streq((const char *)p, "chosen");
			p = align4(skip_str(p));
		} else if (tok == FDT_END_NODE) {
			if (in_chosen)
				return 0;
			depth--;
		} else if (tok == FDT_PROP) {
			uint32_t l = be32(p), nameoff = be32(p + 4);
			if (in_chosen && streq((const char *)(strings + nameoff), prop)) {
				*val = p + 8;
				*len = l;
				return 1;
			}
			p = align4(p + 8 + l);
		} else if (tok != FDT_NOP) {
			return 0;
		}
	}
}

static int prefix(const char *s, const char *pre)
{
	while (*pre)
		if (*s++ != *pre++)
			return 0;
	return 1;
}

/*
 * DRAM banks from every top-level node named "memory" / "memory@..." (S-Boot
 * adds them at boot), using the root #address-cells / #size-cells. Returns the
 * number of banks stored (at most max).
 */
int fdt_memory_banks(uintptr_t fdt, uint64_t *base, uint64_t *size, int max)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);
	uintptr_t strings = fdt + be32(fdt + 12);
	int depth = 0, in_mem = 0, n = 0;
	uint32_t ac = 2, sc = 1;
	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		if (tok == FDT_BEGIN_NODE) {
			depth++;
			in_mem = depth == 2 && prefix((const char *)p, "memory") &&
			    (((const char *)p)[6] == 0 || ((const char *)p)[6] == '@');
			p = align4(skip_str(p));
		} else if (tok == FDT_END_NODE) {
			if (depth == 2)
				in_mem = 0;
			if (--depth == 0)
				return n;
		} else if (tok == FDT_PROP) {
			uint32_t l = be32(p), nameoff = be32(p + 4);
			const char *name = (const char *)(strings + nameoff);
			uintptr_t v = p + 8;
			if (depth == 1 && l == 4 && streq(name, "#address-cells"))
				ac = be32(v);
			else if (depth == 1 && l == 4 && streq(name, "#size-cells"))
				sc = be32(v);
			else if (in_mem && streq(name, "reg") && ac >= 1 && ac <= 2 && sc >= 1 && sc <= 2) {
				uint32_t step = 4 * (ac + sc);
				for (uint32_t o = 0; o + step <= l && n < max; o += step) {
					uint64_t b = ac == 2 ? (uint64_t)be32(v + o) << 32 | be32(v + o + 4) : be32(v + o);
					uintptr_t s0 = v + o + 4 * ac;
					uint64_t s = sc == 2 ? (uint64_t)be32(s0) << 32 | be32(s0 + 4) : be32(s0);
					base[n] = b;
					size[n] = s;
					n++;
				}
			}
			p = align4(v + l);
		} else if (tok != FDT_NOP) {
			return n;
		}
	}
}
