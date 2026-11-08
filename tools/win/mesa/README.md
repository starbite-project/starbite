# Mesa for Windows ARM64 (stage 4 user-mode driver)

`build-mesa.sh` cross-builds Mesa's D3D10 user-mode driver (`libgallium_d3d10.dll`, the gallium d3d10umd
frontend) for Windows 11 ARM64 in the lima VM `starbite`, with the MSVC ABI. That is the ABI the Direct3D
runtime loads. docs/WIN11-GPU.md sections 6.2-6.7 explain why.

- **Mesa 26.2.6**: the last release line with d3d10umd. It was removed upstream on 2026-09-30 (MR 44717). The
  tarball is checked against the SHA-256 published in Mesa's 26.2.6 release notes.
- **Toolchain:** the VM's clang 18 in `cl` mode with lld-link, llvm-lib and llvm-rc. Headers and libraries
  come from the read-only kit in `~/sbwin`, which the VM sees at the same path. The CRT is hybrid: static
  vcruntime plus the in-box `ucrtbase.dll`, so the phone needs no VC++ redistributable.
  `_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH` lets clang 18 use MSVC 14.44's headers.
- **Work tree:** `~/sbmesa` on the VM's disk, never the Mac's. It holds `src/` (a git repo: pristine,
  then the patches, then win32-compat), `build-<config>/`, `libpan/` and `venv/` (meson 1.6.1;
  Ubuntu's 1.3.2 is too old).
- **Output:** `build/win/mesa/libgallium_d3d10-<config>.dll` in this tree.

```bash
limactl shell starbite -- tools/win/mesa/build-mesa.sh softpipe
```

```bash
limactl shell starbite -- tools/win/mesa/build-mesa.sh libpan
```

```bash
limactl shell starbite -- tools/win/mesa/build-mesa.sh panfrost
```

`softpipe` is d3d10umd plus the CPU rasteriser. `libpan` builds Panfrost's precompiled OpenCL C kernels
as SPIR-V, natively on Linux with LLVM 18; the Windows build has no LLVM. `panfrost` is softpipe plus
Panfrost, and needs `libpan` first. Set `FRESH=1` to reconfigure an existing build directory.

## `g52/`: patches from wddm-mali-g52

`g52/` is a copy of github.com/gahingwoo/wddm-mali-g52 at commit e8bcd36, licensed GPL-2.0 (`g52/LICENSE`).
It contains:
- `mesa-patches/0001-0020`: MSVC-ABI fixes for Panfrost's bitfields, Windows portability, d3d10umd and
  Panfrost glue, and presenting through DXGI. All 20 apply unchanged to 26.2.6.
- `win32-compat/`: the POSIX and libdrm headers Panfrost needs, plus `malikm_drm.c`. That file turns
  Panfrost's DRM calls into `DeviceIoControl` on the Mali kernel driver, so Mesa's Linux kernel backend
  `panfrost_kmod.c` runs unchanged.
- `include/`: that driver's ABI headers.
- `clc-stubs/`: `mesa_clc` and `vtn_bindgen2` stand-ins that hand over the prebuilt libpan.

The DLL built with them is therefore GPL-2.0 overall. Mesa is MIT, and our Windows drivers are GPL-2.0-only.
Our own replacement for the device side (sbwddm / sbx_abi.h) will replace `malikm_drm.c` and patches
0014/0015/0019.
