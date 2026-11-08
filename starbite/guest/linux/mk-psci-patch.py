#!/usr/bin/env python3
"""Apply Starbite's CPU_ON trampoline change to a copy of arch/arm64/kernel/psci.c (Linux 7.1 + the S9 port).

    mk-psci-patch.py PSCI.C       edits the file in place; fails loudly if the source drifted
The patch (0002-arm64-psci-starbite-cpu-on-trampoline.patch) is `diff -u` of before/after, made by
build-patched.sh against the port's psci.c (it carries S9P-VENDOR-DEFAULTS). Why: docs/STARBITE-OS-LINUX-M3.md 2.
P2 on the phone (2026-11-08 18:32): the guest's own CPU_ON of 0x101..0x103 at secondary_entry (in the
0x8_8000_0000 bank) returned 0 and the cores never ran, like P1's high entry.
"""
import sys
from pathlib import Path

p = Path(sys.argv[1])
s = p.read_text()


def sub(old, new):
    global s
    if s.count(old) != 1:
        raise SystemExit(f'mk-psci-patch: expected exactly one match for: {old[:60]!r}')
    s = s.replace(old, new)


sub('''static int cpu_psci_cpu_boot(unsigned int cpu)
{
	phys_addr_t pa_secondary_entry = __pa_symbol(secondary_entry);''', '''/*
 * STARBITE-TRAMPOLINE (starbite/guest, docs/STARBITE-OS-LINUX-M3.md 2): on the Galaxy S9 (Exynos 9810) PSCI
 * CPU_ON accepts an entry point above 4 GiB, powers the core and the core never runs. A kernel living in the
 * 0x8_8000_0000 bank (the Starbite OS guest on the M3 cluster) names a 64-byte slot below 4 GiB in
 * /psci "starbite,cpu-on-trampoline"; CPU_ON then enters there and the trampoline branches to the real entry
 * (x0 = 0: secondary_entry clears it anyway). "starbite,cpu-on-trampoline-always" uses it for any entry (QEMU
 * test). CPU bring-up and hotplug are serialised by the hotplug lock, so one slot is enough.
 */
static const u32 starbite_tramp_code[8] = {
	0x10000185,	/* adr x5, #48     the breadcrumb (word 12) */
	0xf90000a5,	/* str x5, [x5] */
	0x58000100,	/* ldr x0, #32     word 10 (0) */
	0xaa1f03e1,	/* mov x1, xzr */
	0xaa1f03e2,	/* mov x2, xzr */
	0xaa1f03e3,	/* mov x3, xzr */
	0x58000044,	/* ldr x4, #8      word 8: the entry */
	0xd61f0080,	/* br  x4 */
};

static phys_addr_t starbite_cpu_on_entry(phys_addr_t entry)
{
	static void __iomem *tramp;
	static phys_addr_t tramp_pa;
	static bool probed, always;
	struct device_node *np;
	u64 pa = 0;
	int i;

	if (!probed) {
		probed = true;
		np = of_find_node_by_path("/psci");
		if (np && !of_property_read_u64(np, "starbite,cpu-on-trampoline", &pa) && pa &&
		    pa + 64 <= SZ_4G) {
			tramp = ioremap(pa, 64);
			if (tramp)
				tramp_pa = pa;
			always = of_property_read_bool(np, "starbite,cpu-on-trampoline-always");
			pr_info("psci: CPU_ON through the trampoline at %pa (below 4 GiB)%s\\n", &tramp_pa,
				always ? ", always" : "");
		}
		of_node_put(np);
	}
	if (!tramp || (entry < SZ_4G && !always))
		return entry;
	for (i = 0; i < 8; i++)
		writel_relaxed(starbite_tramp_code[i], tramp + 4 * i);
	writel_relaxed(lower_32_bits(entry), tramp + 32);
	writel_relaxed(upper_32_bits(entry), tramp + 36);
	for (i = 40; i < 56; i += 4)
		writel_relaxed(0, tramp + i);
	dsb(sy);
	return tramp_pa;
}

static int cpu_psci_cpu_boot(unsigned int cpu)
{
	phys_addr_t pa_secondary_entry = __pa_symbol(secondary_entry);''')
sub('''	err = psci_ops.cpu_on(cpu_logical_map(cpu), pa_secondary_entry);''',
    '''	err = psci_ops.cpu_on(cpu_logical_map(cpu), starbite_cpu_on_entry(pa_secondary_entry));''')
if '#include <linux/sizes.h>' not in s:
    sub('#include <linux/psci.h>\n', '#include <linux/psci.h>\n#include <linux/sizes.h>\n')
p.write_text(s)
print(f'mk-psci-patch: {p} patched')
