# sbumd for 32-bit x86 programs: audit, build, tests, install draft

2026-11-16 16:58 EET. Gap-list item 4 (`docs/WIN11-GPU-FULL-DRIVER.md`): "32-bit x86 apps".

**Status: compiled only.** Nothing here has run on Windows or on a phone. No phone, no adb, no push. The kernel
driver source and the existing tools are unchanged.

## 1. Result in short

- A 32-bit x86 `sbumd.dll` exists: `build/win/umd-x86/sbumd.dll` (PE32, i386, 13.85 MB), built from Mesa branch
  `sb-x86` (0167 `b8730f0` + 3 commits) with the same options as the ARM64 halves. It built on the first try; no
  build error needed fixing.
- **The kernel driver needs no change.** Every structure of ours that reaches sbwddm has the same layout from a
  32-bit and a 64-bit caller, and sbwddm has no code that depends on the caller's bitness. So
  `build/win/patchwork/x86-kmd.diff` was **not created**: there is nothing to put in it.
- Nothing found that would let a 32-bit process corrupt kernel state. What can go wrong is listed in section 7,
  each marked as "process crash", "wrong picture" or "merely fails".
- Tests built for x86: `d3d10core_test32.exe`, `sbwt32.exe`, `sbd3d32.exe`. Install pieces: a hive helper for
  `UserModeDriverNameWow` (round trip tried on a copy of a pulled hive) and a draft test batch.

## 2. Audit A: what crosses from the user-mode driver to sbwddm

User side: `include/win32-compat/maliwddm_abi.h` and `malikm_drm.c` in `~/sbmesa/src-x86`. Kernel side:
`starbite/win/sbwddm/maliwddm/maliwddm_abi.h` and the parsers named below. (`sbx_abi.h` / `sbx_cmd.h` are the
planned ABI; sbumd 0167 does not use them. Only `sbx_head_in()` is used, inside the kernel. They are also
fixed-width with no pointers.)

| Structure | Travels as | User side writes it in | Kernel side reads it in | Size, 32-bit = 64-bit | Clean? |
|---|---|---|---|---|---|
| `MW_ALLOCATION_INFO` | allocation private data (`pfnAllocateCb` / CreateAllocation) | `malikm_drm.c`: CREATE_BO, `malikm_present_d3dddi` | `memory.c` `MwCreateAllocation` / `NewAllocation` (checks size and Version) | 32, align 8 | yes |
| same, read back | OpenResource private data: written by the creating process's driver, read by the opening one (a 32-bit app's window surface opened by 64-bit DWM, and the reverse) | creator as above; reader `Resource.cpp` OpenResource, which reads eight `unsigned` words, not the struct | passed through by the system; also made by `MwGetStandardAllocationDriverData` | 32 | yes |
| `MW_COMMAND` (1 or 2 per Render) | the command buffer | `malikm_drm.c` PANFROST_SUBMIT, `sb_flush_pend` | `command.c` `MwRender`: copied under `__try`, Type / Slot / Jc checked, Jc must start inside an allocation the same Render lists | 16, align 8 | yes |
| `MW_ESCAPE_ALLOC_INFO_DATA` | escape | CREATE_BO, `malikm_import_alloc` | `adapter.c` `MwEscape` -> `MwEscapeAllocInfo` (size checked, handle looked up by the system) | 24 | yes |
| `MW_ESCAPE_GPU_INFO_DATA` | escape | `malikm_bind_d3dddi`, `wddm_init` | `MwEscape` -> `GpuInfo` | 376 | yes |
| `MW_ESCAPE_STATS_DATA` | escape | `wddm_poll_idle`, `wddm_idle` | `MwEscape` -> `Stats` | 232 | yes |
| note `{Code, Id, Value, Reserved}` (the shim's own copy of `MW_ESCAPE_SB_NOTE_DATA`) | escape | `malikm_note` | `MwEscape`, `Id < 64` checked | 16 | yes |
| `MW_ESCAPE_SB_RATES_DATA`, `MW_ESCAPE_SB_CACHE_DATA` | escape; not used by sbumd 0167 (test tools, the GL driver) | - | `MwEscape`, `MwEscapeCache` | 56 / 32 | yes |
| device-state query `{UINT State}` | escape with `DeviceStatusQuery`; answered by the system, never reaches sbwddm | `sb_devstate` | - | 4 | yes |

Why they are clean: only `uint32_t` / `uint64_t` members; no pointer, `size_t`, `HANDLE` or `long`; every 64-bit
member sits at a multiple of 8, so no alignment rule can move it (i686 MSVC aligns 64-bit members to 8 as well;
there is no `#pragma pack`). Allocation handles are `D3DKMT_HANDLE` = 32 bits everywhere.

How it was checked, not just read:

- `build/win/patchwork/x86-abi-layout.sh` compiles `x86-abi-layout.c` (size, alignment and offset of every field)
  for i686, x86_64, aarch64 and arm64ec against Mesa's header and against the kernel driver's header, the latter
  also with `-D_KERNEL_MODE`. All 9 combinations: `same`. A wrong number does fail it (tried).
- The same sizes are now `_Static_assert`s in `malikm_drm.c` (commit `8c048ca`), so a later change that breaks
  this stops the build of every architecture.

### System-defined structures (not ours)

`D3DDDICB_ALLOCATE / RENDER / LOCK / ESCAPE / ...`, `D3DKMT_*`, `DXGIDDICB_PRESENT`, `D3DDDI_ALLOCATIONINFO`
contain pointers and handles. A 32-bit process passes their 32-bit forms to the 32-bit runtime; Windows converts
them on the way into the kernel, and sbwddm only ever sees `DXGKARG_*` structures that the graphics kernel built.
The two arrays the driver fills in place are the same in both worlds: `D3DDDI_ALLOCATIONLIST` 8 bytes,
`D3DDDI_PATCHLOCATIONLIST` 24 bytes (compile-checked for x86, x64, ARM64). CPU addresses of locked allocations and
of the command buffer are chosen by the system and fit the 32-bit process by construction.

### The kernel side

Read: `adapter.c` (Escape, QueryAdapterInfo), `memory.c` (Create/Open/DescribeAllocation, standard allocations),
`command.c` (Render, Present, cache escape). No `IoIs32bitProcess`, no user pointer taken from private data, no
structure whose size depends on the caller. `MwQueryAdapterInfo` answers type 47 with zeros ("not restricted to
64-bit"). Nothing to change.

## 3. Audit B: the user-mode driver itself in a 32-bit address space

| Question | Finding |
|---|---|
| GPU addresses stay 64-bit? | Yes. `struct pan_ptr.gpu`, `wbo.va`, `drm_panfrost_*` fields, `MW_COMMAND.Jc` are all `uint64_t`. Measured: every Panfrost, d3d10umd and shim source was compiled (syntax only) with `-Wshorten-64-to-32` and the pointer/integer cast warnings for x86 and for ARM64; 337 and 353 warnings, **13 only on x86**. All 13 are byte sizes going into `size_t` (BO size, texture data size, stride, the tiler heap size, index-cache ranges). None is a GPU address. Script: `~/sbmesa/build-x86-audit/narrow.py`, list `x86-only.txt`. |
| Pointer casts of GPU addresses? | None. The explicit `uintptr_t` / `intptr_t` casts (about 20 places) all turn a CPU pointer into a 64-bit field (zero-extended) or back. |
| Large address-space reservations? | None of the mmap-reserve kind (that is Panthor/CSF; this GPU uses the job-manager path). The tiler heap (32 MB, DWM 64 MB) is never CPU-mapped. **But** every other BO stays CPU-mapped for its whole life (a Lock held until close), so GPU memory used counts against the process's 2 GB (4 GB if large-address-aware). See risk R2. |
| 64-bit atomics? | Compiled inline (`lock cmpxchg8b`); the map has no `__sync_*` / `__atomic_*` library calls. 64-bit divide helpers come from the static compiler runtime. |
| Calling conventions? | Every DDI entry is assigned through the SDK's typed function tables in C++, with no function-pointer cast, so a `__cdecl`/`__stdcall` mismatch could not have compiled. `OpenAdapter10` ends in `ret 4` (disassembled). The D3DKMT fallbacks are called through a `__stdcall` pointer type. |
| printf arguments (a wrong width crashes on x86)? | `-Wformat` clean for `fprintf`; `DebugPrintf` checked too by marking it printf-like for one syntax pass (4 old debug lines print a handle struct with `%p`: garbage at worst, upstream, not new). |
| Symbol decoration? | Two `/alternatename` fallbacks named undecorated symbols, which do not exist on x86. Harmless here (the real functions are linked; verified in the map) but fixed (commit `fa24465`). |
| x86-only code that would run for the first time | Mesa's vertex-conversion JIT (`translate_sse2`: writes x86 code at run time into RWX memory). Removed from this build (commit `afea139`): the C path is what the ARM64 driver uses, and under the emulator generated code gains nothing. What remains x86-only: CPU detection (`cpuid`, one `fxsave`) and statically compiled SSE2/SSE4.1 helpers. |

## 4. What changed (Mesa, branch `sb-x86` in `~/sbmesa/src-x86`, worktree of src-int)

| Commit | What | Effect on 64-bit builds |
|---|---|---|
| `8c048ca` | compile-time checks of the private ABI in `malikm_drm.c`; in the x86 build only: one umdlog line ("this is the 32-bit x86 build") and two notes to the kernel driver's registry trace at bind, `UmdNote_48` (binds from this process) and `UmdNote_49` (its pid) | none (asserts are true there; the rest is `#if defined(_M_IX86)`) |
| `fa24465` | `/alternatename` with the x86 leading underscore (`os_file.c`, `DxgiFns.cpp`) | none |
| `afea139` | no `translate_sse2` in the 32-bit x86 Windows build (`translate.c`) | none (x86_64 unchanged) |

4 files, +46 -1. The three changed files that the ARM64 halves also build were syntax-checked with the ARM64 and
ARM64EC build directories' own commands (read-only): no error. `src-int`, `src-tc`, `src-nir` and the existing
`build-*` directories were not modified.

## 5. What is built

Recipe: `build/win/patchwork/build-x86.sh` (run inside the VM). `meson setup` with the options of `build-int-a64`
and `~/sbmesa/tc/cross-x86.ini` (it was correct as found: clang-cl 18 for `i686-pc-windows-msvc`, lld-link 18,
`/MT`), `ninja -j4`, then the same link again as `sbumd.dll` with a map, then `verify-x86.py`. Build trees stay in
the VM: `~/sbmesa/build-x86-umd` (146 MB), `build-x86-link`, `build-x86-audit`.

`build/win/umd-x86/` (sha256 in `SHA256SUMS`):

| File | sha256 | What |
|---|---|---|
| `sbumd.dll` | `cc015f8eb7af3c85fc1c031a1fb78365d75481e7cb14a0692fd2de60a1c1c8bb` | the x86 driver, source `afea139`, 13,850,112 bytes |
| `sbumd.map` | `bf7960cbe9aa308c0ca7a479d9a1914f0db16567a61a0e0434a22891f4cb34bd` | its map (umdlog stacks are offsets in the DLL) |
| `d3d10core_test32.exe` | `27004e295ce336dbaec25b4f85b2530022d3797dcd56e5d73a005caa0d78c409` | Wine 10.0 d3d10core, x86 (108 queued tests + 3 direct, as the ARM64 build) |
| `sbwt32.exe` | `92ba472a6a231137457a9bf960bed62c437aa559d3317eaa029e9153faa9e7ca` | the runner, x86 |
| `sbd3d32.exe` | `f26dca752e02935ddc553daf53917c904b78f58fb979c94131795ca280f243c6` | `tools/win/mesa/tests/sbd3d.cpp`, x86 |
| `sb-x86.bat` | `f2698ee993604c5f8d94953fd7440c2f3194e9abea7fded3fcb91a4d18961f5c` | draft test batch (never run) |
| `verify.txt` | - | the verification below |

`verify-x86.py sbumd.dll sbumd.map`: **passed**, 13 checks. PE32 / i386 / DLL; relocatable and NX; base
relocations and a TLS directory; `OpenAdapter10` and `OpenAdapter10_2` exported undecorated and pointing at
`_OpenAdapter10@4` / `_OpenAdapter10_2@4` (meson's .def generator strips the `@4`); no decorated export;
imports from exactly the 14 DLLs the ARM64X sbumd imports from (GDI32, KERNEL32, USER32, the synch api set and
ten `api-ms-win-crt-*`: the compiler runtime is linked in, the UCRT is the system's, as on ARM64; no
`vcruntime140` / `msvcp140`); SafeSEH table (7 handlers); the two weak-default functions are the real ones.

Tests: `build/win/patchwork/winetests-x86.sh [SUITE...]` is `tools/win/winetests/build.sh` copied and adapted
(x86 compiler and libraries, output names with `32`, sources taken from the main checkout's fetched copies so
nothing was downloaded). The test programs import only system DLLs.

## 6. Install and test: draft

Nothing below was done. File names and paths follow `tools/win/sbsel-install.sh`.

**Install (from TWRP, Windows volume mounted read-write):**

1. Push `build/win/umd-x86/sbumd.dll` to `Windows/SysWOW64/sbumd.dll`; read back its sha256 (`cc015f8e...`).
   `Windows/System32/sbumd.dll` (the ARM64X driver) is a different file and is not touched. Same name in both
   folders is the normal arrangement: a 32-bit process resolves a bare name in SysWOW64.
2. Pull `Windows/System32/config/SYSTEM` (clean shutdown only), keep a backup, then
   `build/win/patchwork/hive-umdname-wow.py SYSTEM SYSTEM.new set` and push `SYSTEM.new` back.
   It adds `UserModeDriverNameWow` = `sbumd.dll` x4 (REG_MULTI_SZ) to the adapter's display class key, the key
   `hive-umdname.py` edits, and nothing else. It refuses a dirty hive, a hive with no or several adapter keys, or
   an unknown existing name; it reads its own result back.
   Tried on a copy of `build/win/cleanup-20261116-0627/SYSTEM`: after `set`, a walk of all 24,641 keys finds
   exactly one difference (that value); after `clear`, none. `show` prints both names.
3. **The way back:** `hive-umdname-wow.py SYSTEM SYSTEM.new clear` (32-bit programs are on Windows' software
   renderer again, as today). The file in SysWOW64 may stay; without the value nothing loads it.

For a later driver package (not applied; the INF is not mine to edit): a `SysWOW64` destination
(`SB_UmdWow = 10,SysWOW64`), the x86 file under another source name copied as `sbumd.dll`
(`sbumd.dll,sbumd32.dll`), and `HKR,,UserModeDriverNameWow,%REG_MULTI_SZ%,sbumd.dll,sbumd.dll,sbumd.dll,sbumd.dll`.

**Test boot:** stage `sbd3d32.exe`, `sbwt32.exe`, `d3d10core_test32.exe` into the test folder and
`sb-x86.bat` as `sb-user.bat` (as `sb-wt.bat` is staged), with `C:\ProgramData\Starbite\umdlog` present. First on
the row whose DWM runs 0167 (`win11-ng`), because the x86 driver is 0167; then a standard row. This is a
never-booted driver: a short smoke boot before any long run.

**What the boot should log if it works** (`gputest.log` unless said otherwise):

- `UserModeDriverNameWow    REG_MULTI_SZ    sbumd.dll\0sbumd.dll\0sbumd.dll\0sbumd.dll`
- `X86 file C:\Windows\SysWOW64\sbumd.dll sha256 cc015f8e...`
- `X86 dxgkrnl knows UserModeDriverNameWow` (if this line is missing, or another name appears, see R1)
- `sbd3d start (built x86)`, then `device: feature level 0xa000 on "Starbite ..." ...; sbumd.dll LOADED C:\Windows\SysWOW64\sbumd.dll`,
  `PASS clear`, `PASS triangle`, `sbd3d end: exit 0 (on the Mali)`, `X86 sbd3d x86 exit 0`
- `UmdNote_48    REG_DWORD    0x1` (or more) and `UmdNote_49` = a pid, under the device's `sbwddm` key: the
  kernel driver's own record that a 32-bit process bound to it
- in `umdlog\sbd3d32.exe-<pid>-*.log`: `sbumd: this is the 32-bit x86 build (sb-x86), pointers 32 bits` and
  `malikm: bind to the runtime device ...: yes, through its callbacks`
- 12 `SBWT d3d10core <test>: PASS|FAIL ...` lines **without** `[NOT OUR ADAPTER: ...]`, and no
  `SBWT d3d10core WARNING` line; pass/fail counts should resemble the ARM64 run of the same 12
- the native control, if `sbd3d.exe` is staged: `X86 sbd3d arm64 exit 0` on a standard row. On `win11-ng` it
  prints exit 1 by design of the test: the selector loads `sbumd-ng.dll`, and sbd3d looks for a module named
  `sbumd.dll`
- `dwm.exe` still listed; `Live_JobsTimedOut`, `Live_GpuResets`, `Live_FaultCount`, `Live_HangCount` unchanged

**What it logs if 32-bit programs still do not get us:** `X86 sbd3d x86 exit 1` with
`sbd3d end: exit 1 (drew, but NOT on our UMD)`, every sbwt line marked `[NOT OUR ADAPTER: Microsoft Basic Render
Driver]`, no `UmdNote_48`. That is the state of today and harmless.

## 7. Open risks

Kernel state: **none found.** The private data is identical in layout, sbwddm checks sizes, versions, handles
and that a job chain starts inside a listed allocation, exactly as for 64-bit callers. A 32-bit process can do
to the kernel what a 64-bit one can, no more.

| # | Risk | Kind | Note |
|---|---|---|---|
| R1 | On ARM64 Windows the value for x86 programs might not be `UserModeDriverNameWow`. Microsoft documents that name for 32-bit drivers on 64-bit Windows; I found nothing that confirms it for x86-on-ARM64. | merely fails (software rendering, as today) | `sb-x86.bat` step 2 prints the value names inside `dxgkrnl.sys`, which settles it in the first boot |
| R2 | Address space. Every visible BO stays CPU-mapped until it is closed, so a 32-bit game's GPU memory (Geometry Dash reached 435 MiB) competes with its own heap for 2 GB. When a Lock fails, BO creation fails; d3d10umd reports out-of-memory, but some Panfrost paths write through the null pool pointer or `abort()`. | **process crash** of that app; no effect on the kernel or other processes | same failure exists on 64-bit when GPU memory runs out; here it comes earlier. A fix would be to unmap idle BOs in the x86 build (not done) |
| R3 | The whole driver, shader compiler included, runs under the x86 emulator. Shader compiles and state setup will be several times slower than in the ARM64 driver. | slow; stutter at first use of each shader | not measured |
| R4 | Setting the value moves every 32-bit D3D10/11 program from the software renderer to the Mali at once: launchers, tools, installers. Programs that work today on software may show the 0167 driver's known problems (black classic-app windows in shots, the DWM job-timeout storm seen with Edge, T78). | wrong picture, or a DWM restart as with 64-bit apps on 0167 | `clear` undoes it; test on `win11-ng` first |
| R5 | The x86 driver is 0167 on every catalog row. On standard rows DWM runs the older 64-bit sbumd, so a 32-bit flip-model app and DWM are different driver generations. The shared private data is the same version (1), but the T70 class (DWM's device stopped by how an opened surface is listed) was fixed in DWM's driver only from 0167. | DWM device stop (desktop redraws after DWM restarts) | same exposure as a 64-bit flip-model app on that row today |
| R6 | D3D9 (most of the old x86 catalogue) is still not served: the name sits in the D3D9 slot as it does for 64-bit, and the DLL has no D3D9 entry. | merely fails, as today | gap-list item 7 |
| R7 | Processes that only load Microsoft-signed DLLs or forbid unsigned images will not load an unsigned file from SysWOW64. | merely fails (software) | sign it as the ARM64X file is signed, when it goes into the package |
| R8 | Never loaded by Windows: an x86-only startup fault (CPU detection under the emulator, TLS, the static CRT start-up) would show at the first `OpenAdapter10_2`. | **process crash** at device creation in every 32-bit D3D10/11 program | this is what `sbd3d32.exe` shows in seconds; with umdlog on, the driver logs the exception and stack for `sb*.exe` |

## 8. Files

New, in `build/win/patchwork/`: `x86-report.md` (this), `build-x86.sh`, `verify-x86.py`, `winetests-x86.sh`,
`hive-umdname-wow.py`, `x86-abi-layout.c`, `x86-abi-layout.sh`. New, in `build/win/umd-x86/`: section 5, plus
`wt/` (the test build's working files, 1.8 MB). In the VM: branch `sb-x86` in `~/sbmesa/src-x86`,
`~/sbmesa/build-x86-umd`, `build-x86-link`, `build-x86-audit` (the audit scripts and their lists).
No existing file was edited.

Source for R1: [Adding user-mode display driver names to the registry](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/adding-user-mode-display-driver-names-to-the-registry),
[WDDM 64-bit issues](https://github.com/MicrosoftDocs/windows-driver-docs/blob/staging/windows-driver-docs-pr/display/microsoft-windows-vista-display-driver-64-bit-issues.md).
