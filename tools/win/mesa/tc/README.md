# Cross toolchains for Mesa on Windows (VM `starbite`, copies of `~/sbmesa/tc`)

| Target | Wrapper (install in the VM as …) | Cross file | Used for |
|---|---|---|---|
| ARM64 | `clang-cl-arm64` → `~/sbmesa/tc/clang-cl` | `cross-arm64.ini` (= `~/sbmesa/tc/cross.ini`) | sbumd, the ARM64 GL ICD |
| x64 | `clang-cl-x64` → `~/sbmesa/tc/x64/clang-cl` | `cross-x64.ini` | the x64 GL ICD |
| x86 | `clang-cl-x86` → `~/sbmesa/tc/x86/clang-cl` | `cross-x86.ini` (adds `-mfxsr`) | the x86 GL ICD |
| ARM64EC | `ec/clang-cl`, `ec/lld-link`, `ec/llvm-lib` → `~/sbmesa/tc/ec/` (clang/lld 20) | `cross-ec.ini` | the ARM64EC GL ICD, sbumd's EC half |

- **Each wrapper must be named exactly `clang-cl`:** meson only treats it as clang-cl under that name.
- **Libraries:** `tools/win/setup.sh`; `SBWIN_ARCHES="x64 x86"` adds x64/x86. The arm64 libraries are ARM64X, so
  ARM64EC needs nothing extra.
- **More detail:** BUILD-gl-ec.md (ARM64EC) and `../arm64x/BUILD.md` (the ARM64X sbumd link). See also
  docs/WIN11-GPU-CAPS.md §6.
