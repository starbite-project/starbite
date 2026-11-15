#!/usr/bin/env python3
"""mk-override.py OUTDIR [--ppmm X]: the S9 panel's display override files, laid out like the volume root
(/Library/Displays/Contents/Resources/Overrides/DisplayVendorID-<v>/DisplayProductID-<p>).

WindowServer (SkyLight, WS::Displays::SLCADisplay::process_display_connect) looks a display's override file up by
vendor and product ID, first under /Library/Displays. GenerateModeListForDisplay then takes "target-default-ppmm"
from it: target points per inch = ppmm * 25.4 * 0.5, and the mode whose point density is closest becomes the default.
A panel under 6.5 inches wide is taken for 72 dpi (width in pixels / 72 = its assumed width), so on any canvas the
1x mode counts as 72 points per inch and the 2x mode as 36: ppmm = 72 / 25.4 = 2.8346 makes 2x (Retina) the default.

Files for three identities, same content: the one our framebuffer driver publishes (xnu 0455: vendor 0x4c83 "SDC",
product 0x960), and the two a display without an identity may get (0/0, and 'unkn'/0x717)."""
import os, plistlib, sys

out = sys.argv[1]
ppmm = float(sys.argv[sys.argv.index('--ppmm') + 1]) if '--ppmm' in sys.argv else 72 / 25.4
for vendor, product in ((0x4c83, 0x960), (0, 0), (0x756e6b6e, 0x717)):
    d = f'{out}/Library/Displays/Contents/Resources/Overrides/DisplayVendorID-{vendor:x}'
    os.makedirs(d, exist_ok=True)
    with open(f'{d}/DisplayProductID-{product:x}', 'wb') as f:
        plistlib.dump({'DisplayProductName': 'Galaxy S9', 'DisplayVendorID': vendor, 'DisplayProductID': product,
                       'target-default-ppmm': ppmm}, f, fmt=plistlib.FMT_XML)
    print(f'{d}/DisplayProductID-{product:x}')
