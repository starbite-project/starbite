#!/usr/bin/env python3
"""Generate 0003-starbite-debug-to-pstore.patch: every SerialPortWrite also
goes into the ramoops pmsg ring (0xFED14000, "DBGC" sig/start/size, data cap
0x3F00 like the woa breadcrumb writers), wrapping so the newest 16 KiB of
DEBUG output (errors + warnings) survives to TWRP's /sys/fs/pstore/pmsg-ramoops-0;
ASSERTs print
(PcdDebugPropertyMask 0x0F) but still never dead-loop.
  python3 tools/woa/patches/gen-0003.py   (from the repo root)"""
import difflib
from pathlib import Path

REF = Path("ref/exynos9810-woa/firmware")
OUT = Path("tools/woa/patches/0003-starbite-debug-to-pstore.patch")

RING = r'''
//
// Starbite: mirror DEBUG output into the ramoops pmsg ring at 0xFED14000
// (header u32 "DBGC", start, size; data at +12). The woa breadcrumb writers
// append at `size` while size < 0x3F00; this writer keeps start == size until
// the ring is full, then wraps, so the newest output always survives.
//
#define STARBITE_RING_BASE  0xFED14000UL
#define STARBITE_RING_CAP   0x3F00u

STATIC
VOID
StarbiteRingWrite (
  IN UINT8  *Buffer,
  IN UINTN  NumberOfBytes
  )
{
  volatile UINT32  *W = (volatile UINT32 *)STARBITE_RING_BASE;
  volatile UINT8   *B = (volatile UINT8 *)(STARBITE_RING_BASE + 12);
  UINTN            Index;
  UINT32           Pos;

  if ((W[0] != 0x43474244u) || (W[1] >= STARBITE_RING_CAP) || (W[2] > STARBITE_RING_CAP)) {
    W[0] = 0x43474244u;
    W[1] = 0;
    W[2] = 0;
  }

  for (Index = 0; Index < NumberOfBytes; Index++) {
    if (W[2] < STARBITE_RING_CAP) {
      Pos  = W[2];
      W[2] = Pos + 1;
      W[1] = (Pos + 1) % STARBITE_RING_CAP;
    } else {
      Pos  = W[1];
      W[1] = (Pos + 1) % STARBITE_RING_CAP;
    }

    B[Pos] = Buffer[Index];
  }

  __asm__ __volatile__ ("dsb sy" ::: "memory");
}
'''


def edit(rel, edits):
    p = REF / rel
    s = p.read_text(newline="")
    nl = "\r\n" if "\r\n" in s else "\n"
    t = s
    for old, new in edits:
        old, new = old.replace("\n", nl), new.replace("\n", nl)
        assert t.count(old) == 1, (rel, old)
        t = t.replace(old, new)
    return list(difflib.unified_diff(s.splitlines(True), t.splitlines(True), "a/" + rel, "b/" + rel))


diff = []
diff += edit("Silicon/Exynos9810Pkg/Library/Exynos9810SerialPortLib/Exynos9810SerialPortLib.c", [
    ("UINTN\nEFIAPI\nSerialPortWrite (\n", RING.lstrip("\n") + "\nUINTN\nEFIAPI\nSerialPortWrite (\n"),
    ("  if (Buffer == NULL) {\n    return 0;\n  }\n\n  Base = GetUartBase ();\n  if (Base == 0) {\n    return 0;  // not configured yet\n  }\n",
     "  if (Buffer == NULL) {\n    return 0;\n  }\n\n  StarbiteRingWrite (Buffer, NumberOfBytes);\n\n  Base = GetUartBase ();\n  if (Base == 0) {\n    return NumberOfBytes;  // not configured yet; the ring has it\n  }\n"),
])
diff += edit("Platform/Star2LtePkg/Star2LtePkg.dsc", [
    ("  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x8000004F\n",
     "  # Starbite: errors + warnings only, so DEBUG text leaves room in the 16 KiB pstore\n"
     "  # ring for the BDS =FATSCAN/=... breadcrumbs (they stop once the ring is full).\n"
     "  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000002\n"),
    ("  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x0E\n",
     "  # Starbite: + ASSERT_ENABLED (0x01) so asserts print into the pstore ring; still\n"
     "  # no ASSERT_DEADLOOP/BREAKPOINT, so the boot carries on.\n"
     "  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x0F\n"),
])
OUT.write_text("".join(diff), newline="")
print(OUT, len(diff), "lines")
