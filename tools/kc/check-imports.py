#!/usr/bin/env python3
"""Check a kext's imports against one of our kernels, the way kmutil would resolve them (plan H5).

  check-imports.py IMPORTS.json OUR_KERNEL APPLE_SYMBOLSETS.plist OUT_PREFIX [--libs kpi,kpi,...]

IMPORTS.json comes from fileset-imports.py (or nm -u of a KDK kext, see --from-nm). A kernel-provided
import resolves only if it is in the __LINKINFO,__symbolsets set of a KPI the kext lists in
OSBundleLibraries (that is what kmutil links against). Writes OUT_PREFIX.json and OUT_PREFIX.md.
Also diffs Apple's symbol sets against ours (what our kernel does not export that Apple's does)."""
import json, plistlib, re, subprocess, sys
from collections import Counter, defaultdict

APFS_KPIS = ['com.apple.kpi.bsd', 'com.apple.kpi.dsep', 'com.apple.kpi.iokit', 'com.apple.kpi.libkern',
             'com.apple.kpi.mach', 'com.apple.kpi.private', 'com.apple.kpi.unsupported']


def linkinfo_symbolsets(kernel):
    out = subprocess.run(['otool', '-l', kernel], capture_output=True, text=True).stdout.splitlines()
    for i, l in enumerate(out):
        if l.strip() == 'sectname __symbolsets':
            size = int(out[i + 3].split()[1], 16)
            off = int(out[i + 4].split()[1])
            data = open(kernel, 'rb').read()[off:off + size]
            return plistlib.loads(data.rstrip(b'\0'))
    raise SystemExit('no __symbolsets in ' + kernel)


def sets(p):
    s, prefixes = {}, {}
    for ss in p['SymbolsSets']:
        names = set()
        pre = []
        for x in ss.get('Symbols', []):
            if 'SymbolName' in x:
                names.add(x['SymbolName'])
            elif 'SymbolPrefix' in x:
                pre.append(x['SymbolPrefix'])
        s[ss['CFBundleIdentifier']] = names
        prefixes[ss['CFBundleIdentifier']] = pre
    return s, prefixes


def kernel_exports(kernel):
    r = subprocess.run(['nm', '-gU', kernel], capture_output=True, text=True).stdout
    return {l.split()[-1] for l in r.splitlines() if l.strip()}


FAMILIES = [
    ('corecrypto', r'^_cc|^_corecrypto|^_ccdigest|^_ccsha|^_ccaes|^_ccmode'),
    ('AppleImage4 / img4', r'img4|Image4|_image4'),
    ('AppleKeyStore / AKS / content protection', r'aks|AKS|KeyStore|_cp_|content_protection|cprotect|_fscp'),
    ('sandbox / MACF', r'^_mac_|^_mpo_|sandbox|Sandbox|^_kauth|policy'),
    ('code signing / CoreTrust', r'_cs_|codesign|CoreTrust|_csblob|_csproc|_csvnode'),
    ('decmpfs / compression', r'decmpfs|compress'),
    ('VFS / vnode / buf / cluster', r'^_vnode|^_vnop|^_VNOP|^_vfs|^_VFS|^_buf_|^_cluster|^_ubc|^_vn_|^_namei|^_nameidone|^_mount|^_fs|^_uio|^_vttoif|^_disk_conditioner|^_kdebug_vfs'),
    ('IOKit C++', r'^__ZN?K?\d*IO|^_IO|^__ZTV\d*IO|^__ZN\d+IO'),
    ('libkern C++ / OS*', r'^__ZN?K?\d*OS|^_OS|^__Z.*OS[A-Z]'),
    ('locks / threads / mach', r'^_lck_|^_thread|^_kernel_thread|^_mach_|^_semaphore|^_clock|^_absolutetime|^_nanotime|^_assert_wait|^_wakeup|^_task_|^_current_'),
    ('memory / zalloc / kalloc', r'^_kalloc|^_kfree|^_zalloc|^_zone|^_kmem|^_IOMalloc|^_IOFree|^_kheap|^_mem'),
    ('BSD / proc / sysctl / misc', r'^_proc|^_sysctl|^_kauth|^_suser|^_microtime|^_microuptime|^_tsleep|^_msleep|^_kern|^_os_|^__os_|^_printf|^_panic'),
]


def family(sym, provider):
    if provider and provider != 'com.apple.kernel':
        return {'com.apple.iokit.IOStorageFamily': 'IOStorageFamily',
                'com.apple.security.AppleImage4': 'AppleImage4 / img4',
                'com.apple.kec.corecrypto': 'corecrypto',
                'com.apple.driver.AppleEffaceableStorage': 'AppleEffaceableStorage',
                'com.apple.iokit.CoreAnalyticsFamily': 'CoreAnalyticsFamily'}.get(provider, provider)
    for name, rx in FAMILIES:
        if re.search(rx, sym):
            return name
    return 'other kernel'


def main():
    imp_path, kernel, apple_sets_path, prefix = sys.argv[1:5]
    imports = json.load(open(imp_path))['imports']
    ours, our_pre = sets(linkinfo_symbolsets(kernel))
    apple, _ = sets(plistlib.load(open(apple_sets_path, 'rb')))
    exp = kernel_exports(kernel)
    our_kpi = set().union(*(ours.get(k, set()) for k in APFS_KPIS))
    apple_kpi_of = {}
    for k, names in apple.items():
        for n in names:
            apple_kpi_of.setdefault(n, k)
    rows = []
    for i in imports:
        s, prov = i['symbol'], i['provider']
        if prov == 'com.apple.kernel':
            if s in our_kpi:
                status = 'ok'
            elif s in exp:
                status = 'in kernel, not in a KPI set'
            else:
                status = 'missing from kernel'
        else:
            status = 'needs ' + prov + (' (kernel has a same-named symbol)' if s in exp else '')
        rows.append({'symbol': s, 'provider': prov, 'apple_kpi': apple_kpi_of.get(s, ''),
                     'status': status, 'family': family(s, prov)})
    by_status = Counter(r['status'].split(' (')[0] for r in rows)
    # Apple 15.6 KPI exports that our kernel's sets lack (for every KPI, not only apfs's).
    kpi_gap = {k: sorted(apple[k] - ours.get(k, set())) for k in apple}
    kpi_gap_in_kernel = {k: [s for s in v if s in exp] for k, v in kpi_gap.items()}
    json.dump({'rows': rows, 'by_status': by_status, 'kpi_gap': kpi_gap,
               'kpi_gap_in_kernel_symtab': kpi_gap_in_kernel}, open(prefix + '.json', 'w'), indent=1)
    fam = defaultdict(lambda: defaultdict(list))
    for r in rows:
        if r['status'] != 'ok':
            fam[r['family']][r['status']].append(r['symbol'])
    with open(prefix + '.md', 'w') as f:
        f.write(f'imports: {len(rows)}; ' + ', '.join(f'{k}: {v}' for k, v in by_status.most_common()) + '\n\n')
        f.write('| family | unresolved | status | symbols |\n|---|---|---|---|\n')
        for fm in sorted(fam, key=lambda x: -sum(len(v) for v in fam[x].values())):
            for st, syms in sorted(fam[fm].items()):
                show = ', '.join(f'`{s}`' for s in sorted(syms)[:40]) + (' ...' if len(syms) > 40 else '')
                f.write(f'| {fm} | {len(syms)} | {st} | {show} |\n')
        f.write('\nApple KPI exports absent from our symbol sets (all KPIs):\n\n| KPI | Apple | ours | missing | of which in our symtab |\n|---|---|---|---|---|\n')
        for k in apple:
            f.write(f'| {k} | {len(apple[k])} | {len(ours.get(k, set()))} | {len(kpi_gap[k])} | {len(kpi_gap_in_kernel[k])} |\n')
    print(by_status)


if __name__ == '__main__':
    main()
