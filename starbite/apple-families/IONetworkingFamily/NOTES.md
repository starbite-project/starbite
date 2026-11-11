# IONetworkingFamily: notes

## Source

| | |
|---|---|
| Repository | https://github.com/apple-oss-distributions/IONetworkingFamily |
| Tag | `IONetworkingFamily-188` (tag object `41159c9178d626007c1c1ab7a8f9333ce4e9ba14`) |
| Commit | `bb910e2b458939f6b1f53c0fa6907f258b431ea4` (2026-04-17) |
| Why this tag | `distribution-macOS` `macos-265` (macOS 26.5, which ships `xnu-12377.121.6`) lists `IONetworkingFamily-188`; so does `macos-264`. 188 is also the newest tag in the repository |
| Licence | APSL 2.0 (`APPLE_LICENSE`; every source carries the APSL header) |
| Clone | `build/families/IONetworkingFamily` (gitignored, 1.6 MB, `--depth 1`) |

## Result

- 12 of 12 files compile-check clean with `tools/apple/kcheck.py` (macOS STARLTE flags) through `check.sh`, with
  `-include kext-view.h -D__PRIVATE_SPI__`. 0 errors; warnings only, all in Apple's code
  (`-Wzero-as-null-pointer-constant` ~525, `-Wdocumentation` ~127, `-Wmissing-prototypes` 6, `-Wshadow-field` 3,
  `-Wimplicit-int-conversion` 2, `-Wcast-function-type-mismatch` 1; kcheck shows at most 80 lines per file).
- Throw-away object build + `nm`: 0 undefined symbols missing from the kernel, 0 duplicates.
- `starbite/apple-drivers/StarbiteUSB/StarbiteUSBNCM.cpp` compile-checks against `_inc/IONetworkingFamily` (and
  `-I xnu/bsd`) as well, i.e. the driver is ready for the in-tree family.
- No patches.

## Choices worth a second look

1. **Kext view is required here** (`kext-view.h`): with XNU's private view the family meets private `mbuf`
   overloads, `net/dlil.h` internals, Skywalk headers and C++ templates in `extern "C"` blocks. With the kext view it
   sees exactly the KPI it was written for. `IOKITSTATS` must be cleared too (otherwise `IOWorkLoop.h` pulls
   `IOStatisticsPrivate.h` -> `OSKext.h`, which needs the private view); it only swaps a pointer for a reserved
   pointer in IOWorkLoop/IOEventSource/IOUserClient, so the layout is the same.
2. **`net/classq/classq.h` from our tree's `bsd/`**: XNU puts it only in Kernel.framework PrivateHeaders, which
   our kernel build does not have (the KDK path in our flags points at the parked KDK). The include path adds
   `$(SRCROOT)/bsd` last, so nothing else is shadowed.
3. **`__PRIVATE_SPI__`** is defined for the family only, as Apple does. Clients may be built with the public
   header view (vtable slots reserved), which is what StarbiteUSBNCM does today.
4. **IOKDP personality** kept for completeness; it never matches unless a controller offers polled KDP.

## Not done here

- No iOS-target compile check.
- Not run on the phone (M11, StarbiteUSB/PHONE-TEST.md).
- Interface naming for M11 (configd/InterfaceNamer or a kernel-side name request) is outside this folder.
