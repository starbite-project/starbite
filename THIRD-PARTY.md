# Third-party code and credits

Starbite builds on the work of many people. Each part keeps its own licence. If we missed a credit, please open an issue.

## Kernels, firmware and boot

| Part | Where | Licence / owner |
|---|---|---|
| XNU, IOHIDFamily (1915.140.3, 2238.120.5, 870.60.4), IONetworkingFamily-188, libpthread, hfs | `xnu/**`, `starbite/apple-families/**`, `starbite/apple-drivers/**` | APSL-2.0, Apple open source. Our patches to them stay APSL-2.0. |
| ios-a5xelte and its herolte branch (wardevsa, orhunnx) | XNU patches 0001–0238 (`sweet-*`, `a5xelte-*`, `herolte-*`), `tools/mkadt_starlte.py`, loader parts in `shim/starlte` (`fbtext`, `probe`, `xnu_boot`) | The upstream project states no licence. XNU changes are APSL-2.0 Modifications. Credited to the original authors. |
| blacktop/darwin-xnu-build | build tooling | MIT |
| exynos9810-woa and its notice chain (Mu-Silicium, edk2-msm, edk2-exynos, WOA-Project, rainbow) | `tools/woa/**`, `starbite/win/sbhsi2c`, `release-pptt.bin` | BSD-2-Clause-Patent / BSD / MIT |
| EDK2 | `tools/woa/patches/*` (RTC code with ARM, Apple, Pete Batard and Microsoft headers) | BSD-2-Clause-Patent |
| Linux kernel (Samsung and upstream drivers, brcmfmac) | `tools/linux/**`, facts and adapted parts in `starbite/sbos/kern`, `starbite/wifi/{chip,msgbuf,ring,fw}.c` | GPL-2.0 / ISC (brcmfmac, Broadcom) |
| galaxy-s9plus-mainline-linux (SheroAbi) | `tools/linux/port` (vendored) | GPL-2.0 |

## GPU and graphics

| Part | Where | Licence / owner |
|---|---|---|
| Mesa (Panfrost, PanVK, Bifrost compiler) | `tools/mesa-ios/*.patch`, `tools/win/mesa/**` | MIT |
| wddm-mali-g52 (gahingwoo) | `starbite/win/sbwddm/maliwddm/*`, `starbite/win/sbwddm/common/maligpu.*`, `tools/win/mesa/g52/**` | GPL-2.0 |
| IOS-26-Hot (dipa-studio) | `tools/metal2/HotMetal2.mm`, `HotPrimeShaders.h` | MIT (`LICENSE.IOS-26-Hot`) |
| NativeMetal bridge (NativeMetal contributors; parts from UniXNU-zeta) | `tools/metal2/bridge/**`, `NativeBridgeMetalFrontend.*` | MIT / BSD-2-Clause-Patent (`bridge/LICENSE`) |
| Linux/Mesa DRM uapi headers | `tools/gpu/panfrost_drm.h` | MIT (Broadcom, Collabora) |
| stb_truetype (Sean Barrett) | `starbite/sbos/ui/third_party` | MIT / public domain |

## Windows drivers

| Part | Where | Licence / owner |
|---|---|---|
| S6SY761Touch (TheMorc), from the Linux s6sy761 driver (Samsung, Andi Shyti) | `starbite/win/s6touchdiag/*` | GPL-2.0 |
| Microsoft Windows-driver-samples skeletons (SPB, HID) | some driver skeletons in `starbite/win` | MIT / MS-PL (Microsoft) |

## Networking, data and crypto

| Part | Where | Licence / owner |
|---|---|---|
| BearSSL | `starbite/wifi/tls/bearssl`, XNU series 0020/0027/0032/0033 | MIT |
| Mozilla CA roots | generated `starbite/wifi/tls/ta.c` | MPL-2.0 |
| LZ4 (Yann Collet) | `starbite/link/proto/lz4` | BSD-2-Clause |
| SplitMix64 | PRNG | public domain |
| GeoNames | generated `starbite/sbos/kern/tzcities.c` | CC BY 4.0 |
| IANA tz database | `starbite/sbos/kern/tzdb.c` | public domain |
| YuNet / SFace face models (OpenCV Zoo) | downloaded by `face-pack.py`, not included | MIT / Apache-2.0 |

## Fonts, icons and artwork

| Part | Licence |
|---|---|
| Inter, Inter Tight, JetBrains Mono, Outfit | SIL OFL 1.1 |
| Roboto | Apache-2.0 |
| Lucide icons | ISC |
| Simple Icons (`si-*.svg`) | CC0. The logos are trademarks of their owners. |
| Tux | © Larry Ewing, made with The GIMP |
| Debian Open Use logo, postmarketOS logo | CC BY-SA 3.0 |
| App Store listing icons (`starbite/appstore/icons`) | Each belongs to its own project and licence. Firefox, Thunderbird, VLC, LibreOffice and others are trademarks of their owners. |

## Facts, not code

Some drivers record register values and sequences that were observed on the phone, or read from public drivers as documentation. Comments say where. Binary-patch recipes for Apple and Microsoft binaries are offsets and a few instruction words only. No vendor code is included. You need your own copies of every OS.
