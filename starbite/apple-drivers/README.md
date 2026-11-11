# Starbite S9 platform drivers for Apple's OSes (macOS Sequoia first, then iOS 26/27)

Design: `docs/APPLE-S9-DRIVERS.md` (Part 2 architecture, Part 3 the driver list, Part 5 milestones). One folder per
driver. Each driver is a normal IOKit kext source using public KPIs only, so it can be built into our kernel
(milestones M1-M8) or put in a Boot Kernel Collection later.

## Folder layout (per driver)
```
starbite/apple-drivers/<Name>/
  <Name>.cpp, <Name>.h     IOKit class(es); addresses come ONLY from the provider nub (ADT "reg", "interrupts")
  Info.plist               personality: IONameMatch on the ADT "compatible" from tools/board/starlte.board
  hw/                      plain C/C++ register logic (no IOKit), shared by the kext and the host tests
  test/                    host tests: clang on the Mac, a mock MMIO backend, run by test/run.sh
  PHONE-TEST.md            the first phone test: milestone, boot-args, dead-man, expected log lines, pass line
  NOTES.md                 register facts with their sources, open questions
```

## Rules
- **Hardware facts:** `tools/board/starlte.board` (addresses, interrupts); today's in-kernel drivers in
  `build/xnu-12377` (`bsd/dev/starlte_*.c`, `bsd/dev/herolte_ufs.c`, `bsd/dev/a5xelte_keys.c`,
  `iokit/Kernel/arm/Sweet*.cpp`, `pexpert/arm/pe_fiq.c`); `device/dtb/starlte.dts`; `docs/`.
- **Licence:** new code is APSL-compatible (`SPDX-License-Identifier: APSL-2.0`).
  - `starbite/libsb` and the bcmw Wi-Fi core are GPL-2.0-only. Read them as register documentation only; never copy
    code from them.
  - an earlier port's public kernel (`build/refport27/refport-kernel`): use only files its `LICENSES.md` marks APSL/MIT, and credit
    them. Never use anything under `build/refport27/private-in/`.
- **Compile-check** every kext source against our kernel, without writing to the build tree:
  `python3 tools/apple/kcheck.py starbite/apple-drivers/<Name>/<Name>.cpp`. IOStorageFamily headers are not in
  XNU: add `-I /Applications/Xcode_16.4.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX15.5.sdk/System/Library/Frameworks/Kernel.framework/Headers`
  only for the storage headers.
- **Host tests** must pass: `sh starbite/apple-drivers/<Name>/test/run.sh`.
- **Stay in your folder:**
  - never edit `build/xnu-12377`, `build/dxb-12377`, `shim/`, `tools/store/` or another driver's folder;
  - no git commit/stash/reset/checkout (the integrating session commits);
  - never touch the phone (no adb, fastboot, heimdall).
- **Integration** into the kernel (KernelConfigTables, `conf/files`) is done by the integrating session, from
  `<Name>/INTEGRATION.md` (which files, which personality, any boot-arg).
