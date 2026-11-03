# Pending: the 8796 (iOS 16 / macOS 13.5) series' macOS additions

Ported from xnu/series-12377 on `build/xnu-8796` (a worktree of `build/xnu-7195`'s
`base-8796` branch: xnu-8796.141.3 + the fork's patches + xnu/patches/03*.patch, as
tools/build-xnu-local.sh builds it). The six apply in order on that tree.
**Not compiled yet**, so they live here, outside `xnu/patches/03*.patch`: the iOS 16
kernel build is unchanged until they are moved up one directory. docs/MACOS.md, section 7.

| Patch | From 12377 | Note |
|---|---|---|
| 0307-starlte-microsd-driver | 0345 | unchanged |
| 0308-starlte-microsd-root | 0346 | unchanged (IOFindBSDRoot STARLTE block of 0300) |
| 0309-starlte-microsd-cmu-div | 0347 | unchanged |
| 0310-starlte-microsd-high-speed | 0348 | unchanged |
| 0311-sweet-arm64e-userland | 0319 | grading hunk re-made: 8796's grade_binary() |
| 0312-starlte-pmsg-sweetreboot | 0309 (part) | new bsd/dev/starlte_pmsg.c + the panic_spin_forever hook; no UFS ring |

To use: `mv xnu/patches/pending-mac13/03*.patch xnu/patches/ && FRESH=1 tools/build-xnu-local.sh`
(Xcode 15.4, KDK 13.5 22G74), then fix what the compiler reports.
