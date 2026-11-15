#!/usr/bin/env python3
"""mk-override.py OUTDIR [--ppmm X]: the S9 panel's display override files, laid out like the volume root
(/Library/Displays/Contents/Resources/Overrides/DisplayVendorID-<v>/DisplayProductID-<p>).

WindowServer (SkyLight, WS::Displays::SLCADisplay::process_display_connect) looks a display's override file up by
vendor and product ID, first under /Library/Displays. GenerateModeListForDisplay then takes "target-default-ppmm"
from it: target points per inch = ppmm * 25.4 * 0.5, and the mode whose point density is closest becomes the default.
A panel under 6.5 inches wide is taken for 72 dpi (width in pixels / 72 = its assumed width), so on any canvas the
1x mode counts as 72 points per inch and the 2x mode as 36: ppmm = 72 / 25.4 = 2.8346 makes 2x (Retina) the default.

Identity (S3zy's log, 2026-11-15): for the built-in panel WindowServer does not use the driver's DisplayAttributes
IDs. Vendor = 0x610 (Apple) and product = "Gestalt Model ID": get_gestalt_model() maps the machine's product type to
one of a045, a049, a04c, a04e, a050, a052, a054, a057, else 'unkn' (0x756e6b6e). With no override file SkyLight uses
built-in tables for a045-a051 (a MacBook's density), which made the 1x mode the default. So: one file per candidate
under DisplayVendorID-610, same content (--all-ids also writes S3zy's first guesses: 4c83/960, 0/0, unkn/717)."""
import os, plistlib, sys

out = sys.argv[1]
ppmm = float(sys.argv[sys.argv.index('--ppmm') + 1]) if '--ppmm' in sys.argv else 72 / 25.4
ids = [(0x610, p) for p in (0xa045, 0xa049, 0xa04c, 0xa04e, 0xa050, 0xa052, 0xa054, 0xa057, 0x756e6b6e)]
if '--all-ids' in sys.argv:
    ids += [(0x4c83, 0x960), (0, 0), (0x756e6b6e, 0x717)]
for vendor, product in ids:
    d = f'{out}/Library/Displays/Contents/Resources/Overrides/DisplayVendorID-{vendor:x}'
    os.makedirs(d, exist_ok=True)
    with open(f'{d}/DisplayProductID-{product:x}', 'wb') as f:
        plistlib.dump({'DisplayProductName': 'Galaxy S9', 'DisplayVendorID': vendor, 'DisplayProductID': product,
                       'target-default-ppmm': ppmm}, f, fmt=plistlib.FMT_XML)
    print(f'{d}/DisplayProductID-{product:x}')
