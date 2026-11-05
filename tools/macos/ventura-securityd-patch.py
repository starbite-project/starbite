#!/usr/bin/env python3
"""macOS 13.5 (22G74) securityd that keeps running without CSSM (2026-11-05 boots 6-9).

securityd's MDS install fails on our kernel (createSystemDatabase -> DbCreate fails right after a
security.mac.sandbox.sentinel lookup: no Sandbox policy), and both the MDS_Install result and the
CSP attach then abort securityd (CSSMERR_DL_MDS_ERROR, "load cssm failed"), so loginwindow never
logged in. Two instructions become nop:
  0x184e8  cbnz w0, <throw>      after MDS_Install
  0x1858c  blraa x8, x17         the CSP module attach after "CSSM initializing"
With both, securityd serves, autologin completes and the user session starts.
   ventura-securityd-patch.py ORIG OUT      (then: codesign -f -s - --entitlements <orig's> --identifier com.apple.securityd OUT)"""
import struct, sys
src, dst = sys.argv[1], sys.argv[2]
b = bytearray(open(src, 'rb').read())
assert struct.unpack_from('<I', b, 0)[0] == 0xfeedfacf, 'thin arm64e Mach-O expected'
for off, want in ((0x184e8, 0x35002c00), (0x1858c, 0xd73f0911)):
    cur = struct.unpack_from('<I', b, off)[0]
    assert cur in (want, 0xd503201f), f'{off:#x}: {cur:#x} is not 13.5 (22G74) securityd'
    struct.pack_into('<I', b, off, 0xd503201f)
open(dst, 'wb').write(b)
print('patched', dst)
