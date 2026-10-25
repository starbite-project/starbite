#!/usr/bin/env python3
"""Generate 0004-starbite-ufs-aligned-diag.patch: Star2LteUfsHcDxe's per-READ
diagnostic read of CDB[6..9] at Ucd+22 is a misaligned MmioRead32, which
ASSERTs on every command and floods the pstore ring (patches/0003). Read the
same 32 bits as two aligned 16-bit halves.
  python3 tools/woa/patches/gen-0004.py   (from the repo root)"""
import difflib
from pathlib import Path

REF = Path("ref/exynos9810-woa/firmware")
REL = "Platform/Star2LtePkg/Drivers/Star2LteUfsHcDxe/Star2LteUfsHcDxe.c"
OUT = Path("tools/woa/patches/0004-starbite-ufs-aligned-diag.patch")

s = (REF / REL).read_text(newline="")
nl = "\r\n" if "\r\n" in s else "\n"
old = "        UfsHcDiagSet (18, MmioRead32 ((UINTN)Ucd + 22u));" + nl
new = ("        UfsHcDiagSet (18, (UINT32)MmioRead16 ((UINTN)Ucd + 22u) |   // Starbite: Ucd+22 is not" + nl +
       "                          ((UINT32)MmioRead16 ((UINTN)Ucd + 24u) << 16));  // 4-byte aligned" + nl)
assert s.count(old) == 1
t = s.replace(old, new)
OUT.write_text("".join(difflib.unified_diff(s.splitlines(True), t.splitlines(True), "a/" + REL, "b/" + REL)), newline="")
print(OUT)
