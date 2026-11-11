# sbumd for emulated x64 processes: ARM64EC build and ARM64X file (2026-11-11)

Built in the Lima VM `starbite`, **verified statically only. Nothing here has run on a phone or on Windows.**

## Files
| File | What | PE machine |
|---|---|---|
| `sbumd-arm64x.dll` | **ARM64X** sbumd: ARM64 view (native processes: DWM, Explorer) + ARM64EC view (x64 processes) | 0xAA64 + hybrid metadata |
| `sbumd-ec.dll` | ARM64EC-only sbumd (loads in x64 processes only; not usable as `UserModeDriverName`) | 0x8664 + CHPE v2 |
| `sbumd-arm64x.map`, `sbumd-ec.map` | lld link maps (the EC map is from a relink of the same command with `/MAP`; image identical but for the timestamp) | |
| `link-arm64x.sh`, `link-x.rsp` | the ARM64X link, start to finish, and the response file it wrote | |
| `altnames_ec.S`, `exports_a64.c`, `tlsindex_ec.c` | the three workaround sources of the ARM64X link (below) | |
| `verify_arm64x.py`, `verify.txt` | independent PE/CHPE/ARM64X parser and its output (35 checks, 0 failures) | |
| `readobj-*.txt` | `llvm-readobj-20 --file-headers --coff-load-config --coff-exports --coff-imports --coff-tls-directory` | |
| `SHA256SUMS` | hashes | |

Source: Mesa 26.2.6 tree `~/sbmesa/src-umdx` = worktree of `~/sbmesa/src`, branch `sb-umdx` = `sb-with-0133` (d727a2e, patches up to
0134 with 0133) + cherry-pick of 0140 (beac6fa). **No other Mesa source change was needed**; there is no patch 0141-0149.

## Toolchain
As `build/win/gl-ec/BUILD.md`: clang/lld/llvm 20.1.2 for ARM64EC and for the ARM64X link (`~/sbmesa/tc/ec/` wrappers,
`cross-ec.ini`), clang 18 + `cross.ini` for the ARM64 objects (same as the shipped sbumd), meson 1.6.1, `/Users/dev/sbwin` libraries.

## Commands
```
cd ~/sbmesa/src && git worktree add -b sb-umdx ~/sbmesa/src-umdx sb-with-0133
cd ~/sbmesa/src-umdx && git cherry-pick 37be64d          # 0140
cd ~/sbmesa && export PATH=~/sbmesa/native:~/sbmesa/venv/bin:$PATH
OPTS="-Dbuildtype=release -Db_ndebug=true -Db_vscrt=mt -Dplatforms=windows -Dgallium-d3d10umd=true -Dvulkan-drivers= \
 -Dllvm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dgbm=disabled -Dvideo-codecs= \
 -Dmicrosoft-clc=disabled -Dspirv-to-dxil=false -Dzstd=disabled -Dshader-cache=disabled -Dbuild-tests=false -Dexpat=disabled \
 -Dzlib=disabled -Dgallium-drivers=softpipe,panfrost -Dmesa-clc=system -Dprecomp-compiler=system"
meson setup build-umd-ec  src-umdx $OPTS --cross-file=$HOME/sbmesa/tc/cross-ec.ini
meson setup build-umd-a64 src-umdx $OPTS --cross-file=$HOME/sbmesa/tc/cross.ini
ninja -C build-umd-ec  src/gallium/targets/d3d10umd/libgallium_d3d10.dll      # -> sbumd-ec.dll
ninja -C build-umd-a64 src/gallium/targets/d3d10umd/libgallium_d3d10.dll      # plain ARM64 reference, 90489b78...
cd build-umd-x && ./link-arm64x.sh                                             # -> sbumd-arm64x.dll, verify.txt
```
Logs in the VM: `~/sbmesa/build-umd-{ec,a64}.setup.log`, `build-umd-{ec,a64}.log`. Both built 975/975 with no errors.
`build-panfrost`, `~/sbmesa/src` and the other sessions' build dirs were not touched.

## 1. ARM64EC build (`sbumd-ec.dll`)
- Compiles and links unchanged (0140 is enough). Warnings are the classes of the ARM64 build.
- PE machine 0x8664, CHPE v2; code map `0x1004-0x453A60 ARM64EC`, `0x454000-0x455320 X64` (50 fast-forward thunks: 49 exports + entry).
- 49 exports, the same names as the T44 sbumd. `OpenAdapter10` = 0x455010 and `OpenAdapter10_2` = 0x455020 are x64 thunks,
  redirected (CHPE RedirectionMetadata) to the ARM64EC functions 0x131E4 / 0x132C8; the word before each function holds the
  entry-thunk offset (`0x43EDF5` -> thunk in the ARM64EC range). `Adapter.cpp.obj` has `$ientry_thunk$cdecl$i8$i8` etc.
- Calls from our code into the runtime (`pfnAllocateCb`, `pfnRenderCb`, `pfnEscapeCb`, `pfnPresentCb`, ...): in
  `malikm_drm.c.obj` every indirect call loads `__os_arm64x_check_icall` and passes an `$iexit_thunk$cdecl$...` (182 disassembly lines naming it,
  11 `$iexit_thunk` signatures, 20 import exit thunks). Same for the device-function tables the runtime calls back through:
  those functions have `$ientry_thunk`s.
- Imports: KERNEL32, USER32, GDI32, api-ms-win-core-synch-l1-2-0, api-ms-win-crt-* (187 symbols).

## 2. ARM64X link (`sbumd-arm64x.dll`): lld-link 20.1.2 `/machine:arm64x`, four workarounds
1. `altnames_ec.S` (from gl-ec): lld 20 does not apply the EC objects' `/alternatename` directives in arm64x mode
   (libcmt startup stubs, `__volatile_metadata`). Without it: 9 undefined symbols.
2. `exports_a64.c` (from gl-ec, regenerated from `d3d10.def`): the `.def` exports only reach the EC view; this ARM64 object
   gives the native view its 49 exports. Without it the native export table is empty and `/opt:ref` drops the native code.
3. **New: `tlssup_a64.obj` first.** lld 20 writes ONE TLS directory, the native `_tls_used`, for both views. When libcmt's
   ARM64EC `tlssup.obj` was pulled first, the directory's template started at `.tls`+1 (the second `_tls_start`), so the
   TLS block was one byte off for all code. The script extracts the ARM64 `tlssup.obj` from libcmt.lib and lists it before
   every ARM64EC input: template = `.tls` section start, as in the plain build.
4. **New: `tlsindex_ec.c`.** That one directory's `AddressOfIndex` is the native `_tls_index`; the loader never writes the
   ARM64EC `_tls_index`, so EC code would use TLS index 0 (another module's block). The object defines the EC `_pRawDllMain`
   (the CRT calls it first in `_DllMainCRTStartup`) and copies the index from the directory into the EC `_tls_index` on
   process attach. It replaces the `_pRawDllMain` alias of `altnames_ec.S`. It is not in `.CRT$XI*` because lld 20 interleaves
   the native and EC initializer tables (native `_initterm` would call it).
   Checked in the image: EC `_pRawDllMain` (0x8227C0) = `#sb_raw_dllmain`, native `_pRawDllMain` = 0; disassembly reads
   DataDirectory[TLS].AddressOfIndex and stores to the EC `_tls_index`.
- The ARM64 objects are listed in the plain ARM64 link's order, so the native code layout equals the plain build's.
- **The earlier `build/win/gl-ec/arm64x-experimental/libgallium_wgl.dll` has defects 3 and 4** and a native TLS callback
  (`0x76CB58`, ARM64 code) in the shared directory, which an x64 process would call too. Do not use it as it is.

## 3. Static verification (`verify.txt`, `readobj-*.txt`)
Both views:
- File header machine 0xAA64; `llvm-readobj-20` says `COFF-ARM64X` and prints a `HybridObject` (EC view). Our own script
  applies the 19 ARM64X dynamic relocations (load config `DynamicValueRelocTableOffset 0x1151C`, section 8 `.reloc`)
  and its result matches llvm-readobj's hybrid view (all export names/RVAs and import symbols).
  Header fixups: machine -> 0x8664, entry point -> 0x816000, export dir -> 0x197CE09, exception dir -> 0x1A21108+0x18,
  load config -> 0x85D230; the rest patch the import tables / CHPE.
- CHPE metadata v2 (pointer 0x18085D360 in both load configs). Code map: `0x1000-0x3C1178 ARM64`,
  `0x3C2004-0x814B00 ARM64EC`, `0x815000-0x816320 X64`. `__os_arm64x_dispatch_*` slots, AuxiliaryIAT + copy, ExtraRFETable present.
- Exports: 49 in each view, names equal to T44's `sbumd.dll`. Native: all in the ARM64 range. EC: all x64 thunks redirected
  to ARM64EC code; `OpenAdapter10` 0x816010 -> 0x3D3BB4, `OpenAdapter10_2` 0x816020 -> 0x3D3C98, each with an entry thunk.
- Imports: native view = the plain ARM64 build = T44's sbumd (177 symbols, set-equal). EC view = `sbumd-ec.dll` (187).
- TLS directory (shared): template 0x1A23000-0x1A330C0 = `.tls` start; callbacks array empty; template all zero bytes.

Native view against the plain ARM64 build of the same objects (`build-umd-a64`, 90489b78; T44's sbumd has the same
`.text` size, entry point and export RVAs):
- **all 49 export RVAs and the entry point (0x3BC790) are identical** to the plain build and to T44's sbumd;
- **`.text` 0x1000-0x3C1178: 983,134 instruction words; 926,517 identical; 28,212 differ only in an ADRP page; 28,405
  only in an imm12 (ADD/LDR offset); none otherwise.** I.e. the same code, with data addresses moved;
- native `.pdata`: 5016 entries with the same begin RVAs as the plain build; the directory (0x15108 bytes) is sorted and
  also carries the ARM64EC functions (ARM64X layout);
- all 8571 native code symbols of the plain map are in the ARM64X map;
- sections: the plain build's six with the same characteristics, plus `.hexpthk` and `.a64xrm`;
- load config: same size (0x140); differences: `GuardFlags` 0x100 (CF_INSTRUMENTED, no function table; plain has 0),
  CHPE pointer and dynamic-reloc fields set; security cookie and guard pointers at new addresses, the check-function
  slot points into ARM64 code.
- differences that remain: IAT in its own page (0x817000, 0x1000); `.data` and `.tls` hold both views' data
  (TLS block per thread 64 KB instead of 32 KB; image 27.5 MB instead of 13.6 MB); linked by lld 20, not lld 18.

## Cannot be verified offline
- That the Windows loader accepts lld 20's ARM64X image at all (hybrid metadata, dynamic relocations, shared import
  directory) in a native process and in an x64 process. Microsoft's linker is the only one Windows is tested with.
- That the D3D runtime of an x64 process actually loads `UserModeDriverName` and calls the EC view (the INF rule in
  WIN11-GPU-CAPS.md §4.2 is inferred).
- The DDI across the EC boundary at run time: struct layouts are the x64 ones in the EC view (same LLP64 sizes, not
  inspected field by field), and nothing called from x64-only code was exercised.
- `malikm_drm.c`'s paths in an x64 process (D3DKMT via `GetProcAddress` on win32u/gdi32, `GetModuleHandleEx` lookups).
- The `_pRawDllMain` TLS-index copy at run time.
