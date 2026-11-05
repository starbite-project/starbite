#!/usr/bin/env python3
"""sepolicy-unmapped.py OUTDIR : after sepolicy-check.sh, list the vendor's versioned public types
(plat_pub_versioned.cil) that the GSI's mapping files do not map to any type, and how many vendor rules
use each. Those are types that existed in the system the vendor was built against (LineageOS 24: Lineage's
own system_ext policy) but not in the GSI: rules on them compile but match nothing."""
import re, sys, collections, os
d = sys.argv[1]
pub = open(os.path.join(d, 'ven/pub.cil')).read()
ven = open(os.path.join(d, 'ven/vendor.cil')).read()
maps = ''.join(open(os.path.join(d, 'sys', f)).read() for f in os.listdir(os.path.join(d, 'sys')) if 'map' in f)
ver = re.search(r'_(\d{6})\b', pub).group(1)
declared = set(re.findall(r'\(typeattribute (\S+_%s)\)' % ver, pub))
mapped = set(re.findall(r'\(typeattributeset (\S+_%s) ' % ver, maps))
unmapped = sorted(t for t in declared - mapped if not t.startswith("base_typeattr_"))
use = collections.Counter()
for t in unmapped:
    use[t] = len(re.findall(r'\b%s\b' % re.escape(t), ven))
print(f'{len(declared)} versioned public types, {len(unmapped)} not mapped by the GSI:')
for t in unmapped:
    print(f'  {t:60s} {use[t]:4d} vendor uses')
