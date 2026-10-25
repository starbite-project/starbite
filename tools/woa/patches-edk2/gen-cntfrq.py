#!/usr/bin/env python3
"""Re-create the S9+ project's unpublished "workspace-local edit" (see the
Star2LtePkg.dsc CNTFRQ note): S-Boot leaves CNTFRQ_EL0 = 0 (RO at EL1), so
ArmGenericTimerGetTimerFreq() falls back to 26 MHz. Without it TimerDxe
programs a zero period and the first timer interrupt storms forever right
after the Timer arch protocol is installed (seen on starlte, 2026-10-25)."""
for path in ("ArmPkg/Library/ArmGenericTimerPhyCounterLib/ArmGenericTimerPhyCounterLib.c",
             "ArmPkg/Library/ArmGenericTimerVirtCounterLib/ArmGenericTimerVirtCounterLib.c"):
    s = open(path, newline="").read()
    nl = "\r\n" if "\r\n" in s else "\n"
    old = "  return ArmReadCntFrq ();" + nl + "}"
    new = ("  UINTN  Freq;" + nl + nl +
           "  // Starbite/woa: Exynos 9810 S-Boot leaves CNTFRQ_EL0 = 0; the counter runs at 26 MHz." + nl +
           "  Freq = ArmReadCntFrq ();" + nl +
           "  return (Freq != 0) ? Freq : 26000000;" + nl + "}")
    assert s.count(old) == 1, path
    open(path, "w", newline="").write(s.replace(old, new))
print("cntfrq fallback added")
