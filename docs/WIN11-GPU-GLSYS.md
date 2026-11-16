# OpenGL for every program: the Starbite GL driver as the system's ICD

(2026-11-16 17:00 EET. **Compiled only: nothing here has run on a phone.** Gap-list item "OpenGL for every app,
not one game". Work tree: `~/sbmesa/src-glsys`, branch `sb-glsys` = `sb-tc` 3d8ee0f + one commit 5057691.
`src-tc`, `src-int`, `src-nir` and every existing `build-*` directory are untouched.)

## 1. What is ready

| piece | file | state |
|---|---|---|
| The ICD front, ARM64X, C only | `build/win/gl-sys/sbglicd.dll` (60 KB) | built; ARM64X check FAILS 0, initializer tables 0, 20 exports the same in both views |
| The driver for native programs | `build/win/gl-sys/sbgl-arm64.dll` (18.5 MB) | built (plain ARM64) |
| The driver for x64 / ARM64EC programs | `build/win/gl-sys/sbgl-x64.dll` (19.5 MB) | built (ARM64EC, PE machine 0x8664, as the game kit's) |
| Driver patch 0171 | `build/win/gl-sys/0171-wgl-system-icd.patch` | committed on `sb-glsys`; not filed under `tools/win/mesa/sb-patches` (not mine to edit) |
| Registry tool | `build/win/patchwork/hive-opengl-icd.py` | round trip run on a copy of a pulled hive (section 5) |
| Test program | `build/win/patchwork/sbgltest.c` -> `gl-sys/sbgltest-arm64.exe`, `-arm64ec.exe`, `-x64.exe` | built, no warnings |
| Test script | `build/win/patchwork/sb-glsys.bat` | draft, never run |
| Build scripts | `patchwork/sbglicd/build.sh`, `patchwork/sbgltest-build.sh`, VM: `~/sbmesa/glsys-build.sh` | all three re-run from scratch give the files below |

sha256:

```
6a3d73e22e5887c4e8edb6734de56149a4200db84ad4c8762792b83a06b353b9  sbglicd.dll
9c7989ab11d0cc6233536b99b7df78cce374797f159977d9ea4268b641f21063  sbgl-arm64.dll
20b0ff9954bfbf5f64829db3d448433d71f26e624bf3e5951a1d5bc13b55c92b  sbgl-x64.dll
6f5f84d053407ec3d19fa1568060a6ced4d2f9bebd2d95585ab50f3198f2d259  sbgltest-arm64.exe
5880dc099b8d02ed15ac1b1f0cffd9f1ddacc0df7b00e6247b41ac6c5d270350  sbgltest-arm64ec.exe
f296220243afe433185b66acd6d280b3960d010c82d2ec62b3ab054d1f4af730  sbgltest-x64.exe
cad6cf27da226f1536b387a01d8d56d209acfae8e3f372e4196b55386a3e17be  0171-wgl-system-icd.patch
```

Maps: `sbglicd.map`, `sbgl-arm64.map`, `sbgl-x64.map` next to them. The two driver DLLs are meson's own link
command for `libgallium_wgl.dll` run once more with `/MAP /Brepro` (`~/sbmesa/build-glsys-x/link-sbgl-*.cmd`),
so DLL and map belong together; inputs are meson's objects, unchanged.

## 2. Structure: a C-only ARM64X front and two ordinary driver DLLs

**Why one file name must serve two kinds of process.** The system opengl32.dll gets the ICD's name from dxgkrnl
per adapter. There is a second name only for 32-bit programs (`OpenGLDriverNameWow`). x64 programs on ARM64 are
not WOW64 and get the same `OpenGLDriverName` as native ones, from the same System32 (T45, docs/WIN11-GPU.md 6.54:
an emulated x64 program loaded the ARM64X `sbumd.dll` there). So the named file has to be ARM64X.

**Why Mesa's GL driver is not linked as ARM64X.** In our hand-linked ARM64X images both halves' `.CRT$XCU` tables
are merged and both start-ups run both (6.106, T77's crash at DLL load). The real link maps of this driver say:

- `check-init-tables.py sbgl-arm64.map` and `sbgl-x64.map`: **1 C++ initializer each** (it fails the check).
- It is `builtin_functions.cpp`'s `static builtin_builder builtins` in the GLSL compiler: an object with a
  constructor and an `atexit` destructor (`??__Fbuiltins@@YAXXZ` in the map), exactly the T77 kind: in an ARM64X
  image the native start-up would also run the ARM64EC copy, which registers with a CRT that was never started.
- More wait in the archives, not linked today: `libpan_vN.cpp` in each `libpanfrost-vN.a` (the `vtn_bindgen`
  object that caused T77) and `threads_win32_tls_callback.cpp` (`.CRT$XDU`). A link-order change could pull them in.
- The driver reads compiler TLS on every GL call (the dispatch pointer), so it would lean on the ARM64X link's TLS
  workaround objects (`tlssup_a64`, `tlsindex_ec`) far harder than sbumd does. Not examined further.

Patching `builtins` away would give 0 today and make every Mesa update a new risk, in a file that every GL
program loads. So:

```
System32\sbglicd.dll      ARM64X, C only, 60 KB: the name in the registry
System32\sbgl-arm64.dll   Mesa's libgallium_wgl.dll, ARM64    <- loaded by sbglicd's ARM64 half
System32\sbgl-x64.dll     Mesa's libgallium_wgl.dll, ARM64EC  <- loaded by sbglicd's ARM64EC half
```

Each driver DLL is one architecture with one CRT, linked by meson as the game kit's is, so its initializer table
is correct as it stands. The front (`patchwork/sbglicd/sbglicd.c`, the pattern of `starbite/win/sbsel`):

- exports the 19 `Drv*` functions Mesa's DLL exports (checked against `gallium_wgl.def.in` and both built DLLs:
  the same 19 names) plus `SbGlIcdState` for tests;
- on the first `Drv*` call (never in DllMain), once per process under a lock, loads
  `<System32>\sbgl-arm64.dll` or `sbgl-x64.dll` by full path (which one is a compile-time fact of the half that
  runs), resolves all 19, and from then on forwards;
- imports kernel32 and the CRT start-up only; `verify_arm64x.py` FAILS 0 (84 ok lines; the build uses a copy of
  the verifier that checks our 20 export names where the original names sbumd's two), `check-init-tables.py`
  0 C++ and 0 C initializers.
- If it must not or cannot load the driver, every entry point fails (`DrvValidateVersion` FALSE,
  `DrvDescribePixelFormat` 0, no contexts): the program then gets Microsoft's OpenGL, as before registration.
  That is also the switch (section 7).

## 3. The driver for system-wide use: what I read, what 0171 changes

Read: `targets/wgl/wgl.c`, `wgl_sb_zc.cpp`, `frontends/wgl/stw_*.c`, `include/win32-compat/malikm_drm.c`.

| behaviour in the kit | problem for arbitrary programs | 0171 (only when the DLL's file name starts with `sbgl-`; the kit is unchanged) |
|---|---|---|
| Flag files `C:\ProgramData\Starbite\glzc*`, `glsync`, `glcached`, `gltouch` | switches set for the game would apply to every program; `gltouch` wraps the window procedure of every GL window | read from `C:\ProgramData\Starbite\glsys\<name>` instead |
| New-GPU row (0169): zero-copy FLIP_DISCARD + present thread by default | a D3D11 device and a flip-model swap chain on any program's window, on the driver set whose desktop problem (T78) is open | off; every row presents through GDI unless `glsys\glzc*` asks |
| Panfrost fails -> softpipe | seconds per frame at this screen size, looks like a hang | no screen, so no ICD pixel formats: Microsoft's OpenGL takes over. `glsys\softpipe`, `GALLIUM_DRIVER`, `LIBGL_ALWAYS_SOFTWARE` still give softpipe |
| `wgl.c` prints to stderr (4 lines at start, 4 every 10 s) | noise in console programs and launchers that keep a child's stderr | printed only while the `umdlog` folder exists |
| First-chance exception log keyed on the name `libgallium_wgl.dll` | off under the new names | also on for `sbgl-*` (still only with `umdlog`) |

Left as they are, by design or because not small:

- **Default present:** GDI with the async helper (0153/0154), as the user's kit. The frame reaches the window
  shortly after `SwapBuffers` returns. A program that draws with GDI on top right after the swap, or captures the
  window right after it, can see the frame before. `glsys\glsync` makes it synchronous.
- **`SwapBuffers` with no current context on the calling thread shows nothing** (`wgl_panfrost_present` needs the
  thread's pipe context). Legal on Windows, rare, the game never does it.
- **Swap interval:** Mesa's default is 1 and on the GDI path it sleeps to the refresh rate (`wait_swap_interval`,
  with its 1.75 factor). Unchanged from the kit.
- **Present state is one per process** (one async job, one copy buffer, locked). Several GL windows or contexts
  are correct by reading, tested only with the game's one window.
- **Shared switches stay global:** `panthreaded`, `noafbc` (Panfrost, shared with sbumd), `umdlog` (the shim then
  reopens the process's stderr into its log file, as sbumd does).
- **A process that cannot reach sbwddm** (D3DKMT refused, a session without our adapter, `MALIKM_WDDM=0`):
  `malikm_open` fails -> no Panfrost screen -> 0 pixel formats -> Microsoft's OpenGL. No crash by reading; step 3b
  of the test runs it. A process that may not load unsigned DLLs never gets as far as our code.
- **Version reported** (from the source; the test logs the real strings): vendor `Mesa`, renderer
  `Mali-G72 MCn (Panfrost)`, `wglCreateContext` gives compatibility 3.1 (`glsl_feature_level` 140), GLSL 1.40;
  core profile up to 3.1, a 3.2+ request fails (no geometry shaders). `PAN_MESA_DEBUG=gl3` claims 3.3.
- **New code path with the system opengl32:** it hands the ICD its callbacks, so `wglCreateContextAttribsARB`
  goes through opengl32's own `wglCreateContext` (Mesa's standard ICD route, never used by the kit). sbgltest
  uses it.

## 4. Registry values

Key: `ControlSet001\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\0000` (MatchingDeviceId `acpi\strb0072`).

| value | written | meaning | source |
|---|---|---|---|
| `OpenGLDriverName` | REG_MULTI_SZ `sbglicd.dll` | the ICD's file name | Microsoft, "Loading an OpenGL Installable Client Driver" (learn.microsoft.com/windows-hardware/drivers/display/loading-an-opengl-installable-client-driver): the runtime calls `D3DKMTQueryAdapterInfo(KMTQAITYPE_UMOPENGLINFO)`, which reads this key. Mesa's comment in `DrvValidateVersion` (`stw_device.c`) names the same query |
| `OpenGLVersion` | REG_DWORD 1 | "the version number ... the OpenGL runtime uses to validate"; handed to `DrvValidateVersion`, which Mesa accepts whatever it is | same page; `stw_device.c` |
| `OpenGLFlags` | REG_DWORD 3 | bit 0 "is set for compatibility"; bit 1: the runtime does not call the ICD's finish before swap-buffers | same page |
| `OpenGL*Wow` | not written | 32-bit programs; no 32-bit build, they keep Microsoft's OpenGL | same page |

Unsure, to be settled by the first boot (sbgltest logs what dxgkrnl answers):

- **Type.** The page says REG_SZ; installed vendor drivers show REG_MULTI_SZ in registry exports, and our own
  `UserModeDriverName` is REG_MULTI_SZ. Default REG_MULTI_SZ; `--type sz` writes REG_SZ.
- **Flags 3 or 1.** With 1 the runtime calls `glFinish` before every swap, which puts the GPU wait back on the
  program's thread. Mesa flushes the frame itself in `DrvSwapBuffers` and the kit (Mesa's own opengl32) never had
  that finish, so 3 is the kit's behaviour. Whether today's opengl32 honours bit 1 is not verified. `--flags 1`.
- **A bare file name is looked up in System32**, as `UserModeDriverName = sbumd.dll` is. Not verified for OpenGL.
- **What opengl32 does when the ICD refuses** (`DrvValidateVersion` FALSE, or 0 pixel formats): I expect
  Microsoft's implementation; the switches depend on it. Test steps 3a and 3b are that check.

Not used: the older `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\MSOGL` route (Mesa
`docs/drivers/llvmpipe.rst`): system-wide rather than per adapter.

## 5. The registry tool

`build/win/patchwork/hive-opengl-icd.py IN show | IN OUT set [--type multi|sz] [--flags 3|1] [--version N] | IN OUT clear`
(pattern and helpers of `tools/win/hive-umdname.py`: staged copy, hivexsh in the VM, refuses a dirty hive, IN never
modified, own read-back).

- `set` refuses unless exactly one adapter key exists, and refuses to overwrite an ICD name it does not know.
- `clear` removes `OpenGLDriverName`, `OpenGLVersion`, `OpenGLFlags` and nothing else, and only when the name is
  the one it writes.
- Run on a copy of `build/win/hive/SYSTEM.17634.30914bef` (input sha unchanged afterwards), then compared with an
  independent walk of all 24,663 keys: after `set` one key differs (those three values added); after `set` +
  `clear` **no key or value differs** from the original.

## 6. Risks to other programs

- **The desktop compositor:** DWM does not use OpenGL and never loads the ICD. GL windows reach it as they do
  from the game today, GDI drawing into the window's surface. No flip-model swap chain is made by default.
- **Browsers:** Chromium (Edge, Chrome, WebView2, Electron, Steam's pages) and Firefox draw through ANGLE on
  D3D11 and normally never touch opengl32. Forced to desktop GL by a flag they would load our driver in their GPU
  process. Not tested.
- **Every program that asks for pixel formats** (many toolkits do at start, without drawing) now loads 18.5 or
  19.5 MB and makes a D3DKMT device and a Panfrost screen. sbgltest logs that time
  ("first DescribePixelFormat"); memory is not measured.
- **Programs that worked on Microsoft's software GL 1.1** now run on Panfrost: faster, and exposed to its limits.
  Programs that render into a bitmap still get Microsoft's formats (the ICD has window formats only); programs
  asking for GDI-compatible window formats now get Mesa's.
- **Programs needing GL 3.2+** (Minecraft 1.17+, Blender) fail as before, with a different message.
- **The game:** unchanged. Its app-local opengl32.dll + libgallium_wgl.dll are found first and never go through
  the system opengl32.
- **32-bit programs:** unchanged.

## 7. Install and test draft

Files to `C:\Windows\System32`: `sbglicd.dll`, `sbgl-arm64.dll`, `sbgl-x64.dll` (read back by sha256). Nothing
existing is overwritten. Folder `C:\ProgramData\Starbite\glsys` with the files below; the `umdlog` folder for logs.

Switch files in `C:\ProgramData\Starbite\glsys\` (read once per process):

| file | effect |
|---|---|
| `off` | no program gets the driver (Microsoft's OpenGL) |
| `off-<exe name>` | that program does not |
| `only` + `on-<exe name>` | allow list: only the named programs do |
| `log` | each process writes one line to `umdlog\sbglicd-<pid>.log` |
| `glsync`, `glcached`, `glzc*`, `gltouch`, `softpipe` | the kit's switches, for system-ICD processes |

Steps (S9 #1, from TWRP, in the manner of `tools/win/sbsel-install.sh`; none of this is scripted yet):

1. Clean-hive check, pull and back up `SYSTEM`.
2. Push the three DLLs; make `glsys\` with `log`, `only`, `on-sbgltest-arm64.exe`, `on-sbgltest-arm64ec.exe`,
   `on-sbgltest-x64.exe`. **First boot = allow list:** only the test programs get the driver.
3. `hive-opengl-icd.py SYSTEM SYSTEM.new set`, push it, read back.
4. Stage `sbgltest-*.exe` and `sb-glsys.bat` in a folder with **no** `opengl32.dll` / `libgallium_wgl.dll`.
5. Boot. These files have never booted: a short smoke run first, then the script.
6. If it passes: delete `only`, boot the user's row, start the game (must be as before) and one GL program that is
   neither ours nor the game.

What the test boot should log (`sb-glsys.bat` -> `gputest.log`):

- `OpenGLDriverName REG_MULTI_SZ sbglicd.dll`, `OpenGLVersion 0x1`, `OpenGLFlags 0x3`; the three sha256 above.
- per sbgltest (arm64, arm64ec, x64), `hw` mode:
  - `opengl32.dll: C:\Windows\System32\opengl32.dll (the system's)`
  - `registered ICD (...): name "sbglicd.dll", version 1, flags 0x3`
  - `chosen format N: ... (ICD, composition)`
  - `GL_VENDOR Mesa`, `GL_RENDERER Mali-G72 ... (Panfrost)`, `GL_VERSION 3.1 Mesa 26.2.6...`, `GLSL 1.40`
    (expected from the source, not from a run)
  - `core profile: 3.1 asked and given`
  - `checked frames: 30 of 30 read back right`, the timed frames' fps
  - `loaded sbglicd.dll C:\Windows\System32\sbglicd.dll` and `sbgl-arm64.dll` (native) or `sbgl-x64.dll`
  - `sbgltest: PASS`
- `umdlog\sbglicd-<pid>.log`: `sbglicd: sbgltest-arm64.exe pid N (arm64): driver C:\Windows\System32\sbgl-arm64.dll`,
  and `(x64): driver ...\sbgl-x64.dll` for the other two.
- the driver's own log: `sbumd: gl: system ICD (flag files in C:\ProgramData\Starbite\glsys)`.
- step 3a: `OFF for this program`, renderer `GDI Generic`, GL 1.1, PASS in `ms` mode.
- step 3b (`MALIKM_WDDM=0`): `0 from an ICD`, renderer `GDI Generic`, PASS in `ms` mode.
- `GLSYS DWM SURVIVED`, `GLSYS VERDICT: PASS`.

## 8. The way back

1. No reboot: create `C:\ProgramData\Starbite\glsys\off` (new processes get Microsoft's OpenGL), or
   `off-<exe name>` for one program. Depends on the refusal behaviour of section 4.
2. Offline: `hive-opengl-icd.py SYSTEM SYSTEM.back clear` and push it: the key's values are what they were. The
   three DLLs can stay in System32, nothing names them.
3. The driver DLLs and the front are new files; removing them restores System32 exactly.

## 9. Not done

- No device install script and no smoke-stage entry.
- No 32-bit build.
- Patch 0171 is not copied into `tools/win/mesa/sb-patches`, and docs/WIN11-GPU.md has no section for this.
- The kit built from `sb-glsys` (`libgallium_wgl.dll` + `opengl32.dll` in `build-glsys-ec`) was not compared on a
  phone with the 0170 kit. By reading, 0171 changes one thing when the file name is `libgallium_wgl.dll`: log
  wording. There is a new first line `sbumd: gl: game kit (...)`, and the cached / async lines end in
  `(flag file glcached)` / `(flag file glsync)` where they had the full path. A script that greps those paths in
  the game's umdlog would need the new text.
