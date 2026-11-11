# Mesa OpenGL ICD for emulated x64 processes: ARM64EC build (2026-11-11)

Built in the Lima VM `starbite`, statically verified only. **Nothing here has run on a phone or on Windows.**

## Files
| File | What | PE machine |
|---|---|---|
| `libgallium_wgl.dll` | Mesa 26.2.6 WGL ICD, softpipe + Panfrost + malikm shim, **ARM64EC** | 0x8664 + CHPE v2 |
| `opengl32.dll` | Mesa's app-local opengl32 (forwards to libgallium_wgl.dll), **ARM64EC** | 0x8664 + CHPE v2 |
| `sbgl-ec.exe` | `tools/win/mesa/tests/sbgl.cpp` as **ARM64EC** (runs as an x64-compatible process) | 0x8664 + CHPE v2 |
| `arm64x-experimental/libgallium_wgl.dll` | stretch: one **ARM64X** file (ARM64 view + ARM64EC view) | 0xAA64 + hybrid |
| `tc/` | copies of the new toolchain files (`~/sbmesa/tc/` in the VM) | |
| `SHA256SUMS` | hashes of the four binaries | |

## Toolchain
- clang / clang-cl 20.1.2, lld-link 20.1.2, llvm-lib 20.1.2 (Ubuntu 24.04 `clang-20 lld-20 llvm-20`, 1:20.1.2-0ubuntu1~24.04.3).
  clang-18 stays the default; no alternatives changed.
- meson 1.6.1 (`~/sbmesa/venv`), ninja 1.11.1.
- Windows libraries: `/Users/dev/sbwin` (MSVC libs `msvc/lib/arm64`, SDK 10.0.26100.0 `um/arm64` + `ucrt/arm64`; these are ARM64X libs, no downloads).
- Mesa: `~/sbmesa/src-ec` = git worktree of `~/sbmesa/src`, branch `sb-ec` (master is checked out in `src`, so a new branch on the same commit 53cee4a), plus commit 37be64d (patch 0140).

### New files in `~/sbmesa/tc/` (nothing existing changed)
- `clang-cl-ec`: `clang-20 --driver-mode=cl -fuse-ld=lld --target=arm64ec-pc-windows-msvc -fms-compatibility-version=19.44 -imsvc ...` (same include dirs as `clang-cl`).
- `cross-ec.ini`: like `cross.ini`, but `-D_AMD64_ -D_ARM64EC_` instead of `-D_ARM64_ -DARM64`, `/machine:arm64ec`, and the tools below. `cpu_family = 'aarch64'` (so meson picks the NEON blake3 and no x86 asm).
- `ec/clang-cl`: execs `clang-cl-ec`. Needed because meson recognises clang-cl by its basename.
- `ec/lld-link`: `lld-link-20 "$@" ec/asan_noop_ec.obj`.
- `ec/llvm-lib`: `llvm-lib-20`, rewriting `/MACHINE:arm` to `/machine:arm64ec` (meson 1.6 reads the target `arm64ec-...` as "arm").
- `ec/llvm-rc` -> `llvm-rc-20`.
- `ec/asan_noop_ec.c/.obj`: a no-op `__sanitizer_annotate_contiguous_container`. lld-link 20.1.2 does not apply libcpmt's
  `/alternatename:#X=#X_default` to the weak anti-dependency chain MSVC emits for ARM64EC, and reported the symbol undefined
  (from `libcpmt.lib(thread0.obj)`). The stub equals libcpmt's own default (`asan_noop.obj`).

### Predefines (clang 20, `--target=arm64ec-pc-windows-msvc`)
`_M_ARM64EC`, `__arm64ec__`, `_M_X64`, `_M_AMD64`, `__x86_64__`, `__x86_64`, `__amd64__`, `__ARM_NEON`. Not `__aarch64__`, not `_M_ARM64`, no `__SSE*__`.
`windows.h` wants `_AMD64_` + `_ARM64EC_` for this target (it defines them itself; the cross file also does).

## Commands
```
sudo apt-get install -y clang-20 lld-20 llvm-20
cd ~/sbmesa/src && git worktree add -b sb-ec ~/sbmesa/src-ec master      # then apply sb-patches/0140
cd ~/sbmesa && export PATH=~/sbmesa/native:~/sbmesa/venv/bin:$PATH
meson setup build-wgl-ec src-ec -Dbuildtype=release -Db_ndebug=true -Db_vscrt=mt -Dplatforms=windows \
  -Dgallium-d3d10umd=false -Dvulkan-drivers= -Dllvm=disabled -Dopengl=true -Dgles1=disabled -Dgles2=disabled \
  -Degl=disabled -Dglx=disabled -Dgbm=disabled -Dvideo-codecs= -Dmicrosoft-clc=disabled -Dspirv-to-dxil=false \
  -Dzstd=disabled -Dshader-cache=disabled -Dbuild-tests=false -Dexpat=disabled -Dzlib=disabled \
  -Dgallium-drivers=softpipe,panfrost -Dmesa-clc=system -Dprecomp-compiler=system \
  --cross-file=/home/dev.guest/sbmesa/tc/cross-ec.ini
ninja -C build-wgl-ec src/gallium/targets/wgl/libgallium_wgl.dll src/gallium/targets/libgl-gdi/opengl32.dll

# test exe (source copied to ~/sbmesa/ec-test/sbgl/, sha256 831d5a3a...89d4)
~/sbmesa/tc/clang-cl-ec /nologo /O2 /EHsc /MT -D_CRT_SECURE_NO_WARNINGS sbgl.cpp /Fesbgl-ec.exe /link /machine:arm64ec \
  /libpath:/Users/dev/sbwin/msvc/lib/arm64 /libpath:/Users/dev/sbwin/kit/Lib/10.0.26100.0/ucrt/arm64 \
  /libpath:/Users/dev/sbwin/kit/Lib/10.0.26100.0/um/arm64
```
Logs in the VM: `~/sbmesa/build-wgl-ec.setup.log`, `build-wgl-ec.pass1.log` (compile), `build-wgl-ec.log` (final link).
The meson configure checks are identical to the ARM64 build's (diffed).

## Source changes (one patch)
`tools/win/mesa/sb-patches/0140-arm64ec-clang-arch-detect.patch` (commit 37be64d on `sb-ec` in `~/sbmesa/src-ec`):
- `src/util/detect_arch.h`: `DETECT_ARCH_X86_64` is no longer set when `_M_ARM64EC` / `__arm64ec__` is defined.
  Upstream excluded ARM64EC only for MSVC's `_M_X64`; clang also defines `__x86_64__` and `__x86_64`.
  Without it Mesa included `<xmmintrin.h>` (half_float.h, rounding.h, u_math.c) and took the SSE paths.
  `DETECT_ARCH_AARCH64` and `DETECT_ARCH_ARM64EC` were already set for ARM64EC upstream.
- `src/util/xxhash.h`: the `_mm_prefetch` branch (`_MSC_VER && _M_X64`) excludes `_M_ARM64EC`; clang then uses `__builtin_prefetch`.
- No effect on ARM64 (neither macro family is defined) or x64 (`_M_ARM64EC` is not defined).
Nothing else needed a change: no inline asm, `_Interlocked*`, `__declspec(thread)` and the bitscan intrinsics compile as they are.

## Static verification (llvm-readobj-20 / llvm-objdump-20)
- **libgallium_wgl.dll**: PE machine 0x8664, CHPE metadata v2; code map `0x1004-0x84E904 ARM64EC`, `0x84F000-0x850340 X64`
  (the x64 part is only the 52 export fast-forward thunks, `.hexpthk`); `__os_arm64x_dispatch_*` pointers, AuxiliaryIAT, `.a64xrm` present.
  - Imports: GDI32 (4), KERNEL32 (86), USER32 (17), api-ms-win-core-synch-l1-2-0, api-ms-win-crt-* (UCRT). Nothing else.
  - 51 exports, identical names to the ARM64 build.
  - strings: 67 "panfrost" hits, `malikm-wddm:` / `malikm-fake:` messages, `NtGdiDdDDI%s`, `D3DKMT%s`, the Mali model table.
- **opengl32.dll**: PE machine 0x8664, CHPE v2; `0x1004-0xB34C ARM64EC`, `0xC000-0xE690 X64` (361 export thunks).
  - Imports: libgallium_wgl.dll (25, all present in its export table), GDI32, KERNEL32, UCRT API sets.
  - 381 exports (336 gl*, 24 wgl*, plus Drv* etc.), identical names to the ARM64 build.
- **sbgl-ec.exe**: PE machine 0x8664, CHPE v2, console subsystem; imports GDI32, KERNEL32, OPENGL32, USER32.
- Toolchain smoke test (`~/sbmesa/ec-test/`: DLL with TLS + Interlocked + exported function pointer, C++ EXE importing it): same structure.
- Warnings: same classes as the ARM64 build plus three clang-20 ones (benign).

## ARM64X (stretch): lld-link 20.1.2 can link it, with two workarounds; treat as experimental
`arm64x-experimental/libgallium_wgl.dll` = the EC objects (`build-wgl-ec`) + the existing ARM64 objects (`build-wgl`, clang 18, read only),
`lld-link-20 @link-x.rsp` with `/machine:arm64x` (in `~/sbmesa/build-wgl-x/`).
- Workaround 1: in arm64x mode lld 20 does not apply the EC objects' `/alternatename` directives (libcmt startup stubs, `_pRawDllMain`,
  `__volatile_metadata`, the ASan defaults). `altnames_ec.S` restates them (EC tail jumps for functions, weak aliases for data).
- Workaround 2: the `.def` exports only reach the EC view (native export table was empty and most native code was dropped).
  `exports_a64.c` (ARM64 object with `/export:` directives) gives the native view its 51 exports. Also `user32.lib gdi32.lib` explicitly.
- Result: PE machine 0xAA64; llvm-readobj shows `COFF-ARM64X` with a HybridObject (EC view); code map `ARM64 0x1000-0x7730F4`,
  `ARM64EC 0x774004-0xFC1964`, `X64 0xFC2000-0xFC3340`; 51 exports in each view (native RVAs in the ARM64 range, EC RVAs in the x64 thunk range);
  14 import DLLs in each view; dynamic relocation table (ARM64X fixups) present.
- Not verified: that Windows' loader accepts lld 20's ARM64X fixups. LLVM 21 has more complete ARM64X support. No ARM64X opengl32.dll was made.

## Not known (needs the phone)
- Whether the ARM64EC DLLs load and run at all in an x64-compatible process.
- Whether `malikm_drm.c`'s `kmt_resolve` (GetProcAddress on win32u.dll, then gdi32.dll) gets callable D3DKMT entry points there, and
  whether the D3DKMT device/context path behaves the same as in a native process.
- x64 -> EC calls into GL functions returned by `wglGetProcAddress` rely on clang's entry thunks; not inspected per function.
