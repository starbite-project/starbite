/* SPDX-License-Identifier: APSL-2.0 */
#include "test.h"

unsigned t_checks, t_fails;

/* --parse ALLOW DENY LU0_BLOCKS: the C parser on what tools/sbufs-fence.py packed (run.sh) */
static int
parse_files(const char *ap, const char *dp, const char *cap)
{
	static uint8_t a[4096], d[4096];
	static struct sbufs_fence f;
	char err[160];
	FILE *fa = fopen(ap, "rb"), *fd = fopen(dp, "rb");
	if (fa == NULL || fd == NULL) {
		return 2;
	}
	size_t al = fread(a, 1, sizeof(a), fa), dl = fread(d, 1, sizeof(d), fd);
	fclose(fa);
	fclose(fd);
	CHECK_EQ(sbufs_fence_parse(&f, a, al, d, dl, err, sizeof(err)), SBUFS_OK);
	sbufs_fence_set_capacity(&f, 0, strtoull(cap, NULL, 0));
	unsigned usable = sbufs_fence_seal(&f);
	for (unsigned i = 0; i < f.nallow; i++) {
		printf("  packed range %u %s lba %llu +%llu flags %u: %s\n", i, f.allow[i].name,
		    (unsigned long long)f.allow[i].first, (unsigned long long)f.allow[i].count, f.allow[i].flags, f.allow[i].why);
	}
	CHECK_EQ(usable, f.nallow);
	CHECK(f.nallow > 0);
	/* every denied extent, its first and last block, is refused; the allowed range's edges pass */
	for (unsigned i = 0; i < f.ndeny; i++) {
		uint64_t last = f.deny[i].first + f.deny[i].count - 1;
		CHECK(sbufs_fence_check(&f, 0, f.deny[i].first, 1, false) != SBUFS_FENCE_OK);
		CHECK(sbufs_fence_check(&f, 0, last, 1, false) != SBUFS_FENCE_OK);
	}
	for (unsigned i = 0; i < f.nallow; i++) {
		CHECK_EQ(sbufs_fence_check(&f, 0, f.allow[i].first, 1, false), SBUFS_FENCE_OK);
		CHECK_EQ(sbufs_fence_check(&f, 0, f.allow[i].first + f.allow[i].count - 1, 1, false), SBUFS_FENCE_OK);
		CHECK(sbufs_fence_check(&f, 0, f.allow[i].first - 1, 1, false) != SBUFS_FENCE_OK);
		CHECK(sbufs_fence_check(&f, 0, f.allow[i].first + f.allow[i].count, 1, false) != SBUFS_FENCE_OK);
	}
	printf("  packed fence: %u ranges, %u denied extents, %u checks, %u failed\n", f.nallow, f.ndeny, t_checks, t_fails);
	return t_fails != 0;
}

int
main(int argc, char **argv)
{
	if (argc == 5 && strcmp(argv[1], "--parse") == 0) {
		return parse_files(argv[2], argv[3], argv[4]);
	}
	struct { const char *name; void (*fn)(void); } suites[] = {
		{ "fence", test_fence }, { "proto", test_proto }, { "hc", test_hc },
	};
	for (unsigned i = 0; i < sizeof(suites) / sizeof(suites[0]); i++) {
		unsigned f0 = t_fails, c0 = t_checks;
		printf("[%s]\n", suites[i].name);
		suites[i].fn();
		printf("  %u checks, %u failed\n", t_checks - c0, t_fails - f0);
	}
	printf("StarbiteUFS host tests: %u checks, %u failed: %s\n", t_checks, t_fails, t_fails ? "FAIL" : "PASS");
	return t_fails != 0;
}
