# A full Windows GPU driver for the S9 (Mali-G72): the gap list, ordered by what real apps need first

2026-11-15 (Track K). The user's goal: "100% full features that e.g. an nvidia driver would have ... genuinely a
full-on PC, not some software-rendered unstable driver ... NOT JUST BEAMNG, JUST GENERALLY A FULL DRIVER." Geometry
Dash and BeamNG.drive are test apps, not the scope.

Marks: **[src]** read in our tree tonight (Mesa 26.2.6 `~/sbmesa/src-int`, `starbite/win/sbwddm`), **[shown]** seen on
the phone, **[est]** my estimate. Sizes: S = a night, M = a few nights, L = one to two weeks of nights, XL = a month
or more. Earlier analysis: `WIN11-GPU-CAPS.md` (11-11), `WIN11-GPU.md` 6.86 (BeamNG).

## Where we are

| Piece | Today |
|---|---|
| Kernel driver `sbwddm.sys` | WDDM **1.3** full driver: one aperture segment (system memory mapped into the Mali MMU), one GPU node, preemption-aware scheduling, reset-from-timeout and reset-engine entry points, DECON scan-out with real flips, four rotations, pointer DDIs **[src]** |
| D3D user-mode driver `sbumd.dll` | Mesa d3d10umd on Panfrost, ARM64X (native ARM64 and x64 apps). **D3D10.0 DDI only**: `SUPPORT_D3D10_1 0`, `SUPPORT_D3D11 0`, `SUPPORT_MSAA 0` (no anti-aliasing at all); so apps get D3D11 at **feature level 10_0** **[src]** |
| Shader stages | vertex, pixel, compute in Panfrost. **No geometry or tessellation stage**: the Mali has none and Panfrost does not emulate them. sbumd has the geometry-shader entry points, with nothing behind them **[src]** |
| Presentation | Full-screen/DWM primaries flip. Windowed blt-model swap chains compose with sbumd 0159 (not on the user's install); **flip-model and DirectComposition swap chains crash DWM** (T70 is the diagnosis) **[shown]** |
| 32-bit x86 apps | **No GPU**: the INF has `UserModeDriverName` only, no `UserModeDriverNameWow` **[src]** |
| OpenGL | Not registered system-wide (no `OpenGLDriverName`). Geometry Dash uses our Panfrost WGL driver copied into its folder (GL 3.1, 3.3 by flag) **[src]** |
| D3D9, D3D12, Vulkan, OpenCL, video | None. PanVK in our tree builds for this GPU generation but is Vulkan **1.0**, not conformant, no geometry/tessellation **[src]** |
| Proof of correctness | Our own handful of test programs. No conformance run has ever been done **[shown]** |

## The list

### 1. Windowed presentation that every modern app uses: flip model and DirectComposition (in flight; M)
- **Who needs it:** Edge/WebView2/Electron/Chromium, WinUI and Store apps, the Start menu's own surfaces, and
  nearly every game since Windows 8. Without it windows stay black or DWM dies.
- **Missing:** DWM loses its device when it opens an app's flip buffers (6.91, 6.95). After that: flip-model
  present pacing, `Present1` dirty rectangles, DComp swap chains and surfaces (6.74's clear), several swap chains at
  once, mode changes for full-screen games, later DirectFlip so a full-screen game bypasses DWM.
- **Tonight:** T70 (diagnosis + three candidate fixes), then T71 (Geometry Dash on it).

### 2. Stability you can measure: a conformance baseline, then fix what fails (start tonight; M, then ongoing)
- **Who needs it:** everyone. "Not unstable" is not a feature we can add; it is the result of running thousands of
  API tests and fixing each failure, which is how vendor drivers get there.
- **Missing:** any such run. Plan: Wine's `d3d10core`, `d3d11` and `dxgi` test suites, built from source for ARM64
  Windows, **each test function in its own process** with a timeout (a crash or hang costs one test, not the run),
  results as a pass/fail/crash table per test. Then the same for crashes in real apps (WER dumps per program, as
  for sihost), GPU hang -> reset -> apps keep running (TDR), out-of-memory behaviour.
- **Tonight:** build the runner and stage T74 (baseline on the user's drivers).

### 3. The D3D10-level surface complete and honestly reported (start tonight; S to M each)
- **Who needs it:** Unity's D3D11 player, Chromium's ANGLE, XAML, older D3D10/11 titles. Many probe caps first and
  refuse or fall back to software on zeros.
- **Missing, smaller:**
  - **MSAA.** Off everywhere upstream: zero quality levels, the resolve call unimplemented, multisample
    rasterization forced off, and a multisampled texture load reads sample 0 only **[src]**. Panfrost has 4x (more by
    format). `sb-patches/0165` turns on quality levels, the resolve (a blit) and rasterization; per-sample loads come
    with the new shader front end.
  - **Feature level 10_1** (`SUPPORT_D3D10_1`): needs MSAA plus the Shader Model 4.1 instructions, which the
    translator logs as unsupported (`lod`, `gather4`, `samplepos`, `sampleinfo`) **[src]**. Not a switch.
  - **Capability answers:** at the D3D10 level the runtime only asks one question (`GetCaps` type 130, which
    pipeline levels beyond the DDI's own), and "none" is the correct answer today **[src][shown]**. Real answers
    matter once the D3D11 DDI exists (item 6). The per-format answers (`CheckFormatSupport`) are real already; the
    format table itself has not been checked against the required list (BC1-BC5 at 10_0).
  - **The shader translator** is the common weak point: SM4 bytecode -> TGSI, with a list of logged-unsupported
    cases. Items 3 and 6 both want it replaced (see item 6), rather than extended.
- **Missing, hard:** **geometry shaders and stream output**, which feature level 10_0 formally requires. Mesa 26.2
  has `src/poly`, the compute-based geometry/tessellation lowering made for Apple GPUs (used by asahi and
  kosmickrisp, **not** by Panfrost) **[src]**. Wiring it into Panfrost on Bifrost is the route (L), and the same
  work gives tessellation for item 6.
- **Tonight:** MSAA (0165), built into the next sbumd; T73 = item 2's suite on it, against T74's baseline.

### 4. 32-bit x86 apps (M)
- **Who needs it:** the older half of the PC catalogue, most launchers, many tools. Today they get nothing from us.
- **Missing:** an x86 build of sbumd (`UserModeDriverNameWow`), with every structure we pass to the kernel checked
  for 32/64-bit layout; the same for the OpenGL driver (item 5).

### 5. OpenGL for every app, not one game (S to M)
- **Who needs it:** Minecraft Java, emulators, SDL/LWJGL games, older CAD/tools.
- **Missing:** registration (`OpenGLDriverName`, `...Wow`), x64 + ARM64 + x86 builds in the driver package, GL 3.3
  on by default once geometry shaders exist (item 3), the present path of item 1 instead of GDI.
- **Scoped (11-15):** the GL driver exists as ARM64EC and x64 builds only (`build-tc-ec`, `build-tc-x64`) and is
  copied into one game's folder (`gd-kit-push.sh`) **[src]**. System-wide needs: a native ARM64 build of the same
  tree; one ARM64X forwarder DLL in System32 that loads the ARM64 or the ARM64EC build by process type (the
  documented pattern for ARM64 drivers); `OpenGLDriverName` / `OpenGLVersion` / `OpenGLFlags` in the adapter's class
  key, set offline by a hive tool (our drivers are installed from TWRP, not by PnP); then a test with a GL program
  that is not Geometry Dash.

### 6. Direct3D 11 at feature level 11_0 (XL: larger than everything above together)
- **Who needs it:** most D3D11 games after ~2013 (Unreal 4, BeamNG), and it is the floor for D3D12.
- **Missing:**
  - the **D3D11 DDI** itself in sbumd (device/context function tables for 11.0/11.1/WDDM 1.3; deferred contexts
    can be left to the runtime);
  - **Shader Model 5** translation. d3d10umd translates SM4 bytecode to TGSI only **[src]**. The sound route is a
    new front end, DXBC -> SPIR-V (vkd3d-shader) -> NIR (Mesa's SPIR-V reader), for all stages;
  - **compute shaders, UAVs, indirect draws** (Panfrost has compute, images and storage buffers);
  - **tessellation** (hull/domain) through `src/poly`, as geometry shaders in item 3;
  - BC6H/BC7, 4x/8x MSAA on all required formats, the 11_0 format and limit tables (texture size is fine: 65536
    **[src]**).
- **Order inside:** item 3's geometry shaders first (same lowering), then the DDI + SM5 for the stages we have
  (gives feature level 11_0 minus tessellation for testing), then tessellation.

### 7. Direct3D 9 (L to XL)
- **Who needs it:** the 2000s catalogue and a surprising number of current launchers and tools.
- **Missing:** a D3D9 DDI in sbumd (none exists in Mesa for Windows). The pragmatic step first: D3D9 through a
  translation DLL on our GL or D3D11 per app; a native D3D9 UMD only after item 6.

### 8. The kernel side of "a real PC" (M each)
- **Memory:** one aperture segment of system memory **[src]**. To check and fix: behaviour at the 4 GB limit
  (eviction under pressure, what a game sees as its video-memory budget), no leaks across device loss.
- **TDR:** the entry points exist; a forced GPU hang with apps running has not been tested end to end.
- **Power and heat:** GPU clock/voltage scaling and idle power-down, display off/on, sleep; today the GPU and the
  big cores run fixed and the 80 C guard throttles.
- **Display:** a real vsync interrupt for frame pacing, lower modes with scaling for full-screen games, DirectFlip
  / overlays, night-light gamma, brightness, hardware cursor checked.

### 9. Speed for games once things are correct (M)
- Presents off the game thread (6.96), a shader disk cache (our Mesa builds have it off), threaded submission,
  AFBC render targets, the translation cache for x64 code on the big cores (6.87).

### 10. Vulkan (L, low value alone)
- PanVK on this GPU generation is Vulkan 1.0 without geometry/tessellation and not conformant **[src]**. It needs a
  Windows winsys on our kernel calls and a WSI. DXVK and vkd3d need Vulkan 1.3, so it unlocks few apps by itself.

### 11. Direct3D 12 (not reachable on this path; said plainly)
- Needs a WDDM 2.x kernel driver (GPU virtual addressing, a different memory manager contract), a D3D12 UMD (Mesa
  has none; its d3d12 code goes the other way) and feature level 11_0 underneath. D3D12-only apps stay out of
  reach until items 6 and 10 are far along, if ever on this GPU.

### 12. Video and the rest (M)
- D3D11 video processing for players and browsers (NV12/P010 textures, colour conversion on the GPU); hardware
  decode is the Exynos MFC, a separate driver. OpenCL: rarely needed.

## Progress log
- **11-15 23:15, shader front end, phase 1 (offline):** a direct DXBC -> NIR translator for vertex, pixel and geometry
  shaders, Shader Model 4.0/4.1 (`ShaderNIR.c`, ~3000 lines, branch `sb-nir` in `~/sbmesa/src-nir`, commits
  fc9d48d + d9185f0), with a Linux harness over the 362 shaders in Wine's d3d10core/d3d11 test sources: all 267
  in-scope shaders translate and pass NIR validation; 227 of the 244 vertex/pixel ones compile with the Bifrost
  compiler for this GPU; the other 17 hit one driver gap (reading a texture's mip count is not lowered on this GPU
  generation, which the current translator's path has too). 71 hand-written shaders check the arithmetic and
  control-flow results by constant folding. Not yet in the driver; nothing run on the phone. Details and the bugs it
  found in the old translator: `docs/WIN11-GPU-NIR-FRONTEND.md`.
- **11-15 23:40, phase 2 (offline):** wired into `Shader.cpp` behind the flag file
  `C:\ProgramData\Starbite\nirshaders` (default off; a refused shader falls back to the old path, logged); Panfrost
  on this GPU generation now lowers mip-count and sample-count queries and sample positions by index, so all 244
  corpus vertex/pixel shaders compile (was 227). Running the OLD path through the same harness showed what the
  shipping driver does today: **a shader with a `switch` statement aborts the process** (`tgsi_to_nir` has no
  SWITCH; 7 corpus shaders), a dynamically indexed immediate constant buffer reads the wrong element (2 shaders,
  silently), mip-count queries miscompile (9), and Shader Model 4.1 instructions emitted an invalid opcode (now
  refused). sbumd 98e9e6a6 (ARM64X verify FAILS: 0) from branch `sb-nir` cf67267, `build/win/umd-nir` in the
  worktree. Not run on Windows.
- **T76 staged:** the d3d10core suite on that driver, pass A flag off, pass B flag on (`sb-wt-ab.bat`), to compare
  with T74 (the user's driver).

- **11-16 00:29, geometry shaders (offline):** Panfrost on this GPU generation gets a geometry stage through
  `src/poly` behind the flag file `gsemu` (off by default; needs `nirshaders`): the vertex shader runs as a capture
  job, the geometry shader's rasterization part as an ordinary vertex shader, poly's index kernel as a compute job
  only for dynamic output topologies. All 23 corpus geometry shaders lower, validate and compile in every piece on
  Bifrost v6. **Compiled only: no draw has run.** Not done: stream output (with or without a geometry shader),
  pipeline statistics, layered rendering. Every indexed D3D10 draw with a geometry shader scans its index buffer
  on the CPU (the frontend always enables primitive restart). Branch `sb-nir` 4c517c3, sbumd e9e9d067
  (`build/win/umd-nir-gs`, ARM64X verify FAILS: 0). Design: `docs/WIN11-GPU-NIR-FRONTEND.md`, "Phase 3".
  Also noted: with no geometry stage (today's driver), `CreateGeometryShader` / `GsSetShader` call a null driver
  hook on Panfrost, and DrawAuto without a geometry shader passes a NULL draw list.

- **11-16 01:00, item 1:** T70 found the flip-model crash (the shim listed an app's flip buffers as written in
  DWM's submissions; the system stops such a device). Fixed in 0167; T71 staged (`WIN11-GPU.md` 6.97).
- **11-16 01:06, Shader Model 5 in the translator (offline):** vertex, pixel, geometry, compute, hull and domain
  shaders of SM 5.0 parse and translate (branch `sb-nir` dbf9ef4). All 92 SM5 corpus shaders validate; the 83
  vertex/pixel/geometry/compute ones compile on Bifrost v6 (compute: 22), the 9 hull/domain ones are validated
  only (tessellation's Panfrost side is not started). Refused by name: SM 5.1, 64-bit floats, interfaces, UAV slots
  above 8. Several D3D rules were implemented from memory and are listed as unsure in the write-up (Phase 4).
  Nothing calls it yet: the D3D11 DDI is the missing half.

- **11-16 01:12, item 2:** the first conformance run (T74) on the user's driver: d3d10core 74 pass, 17 fail, 20
  crash (`WIN11-GPU.md` 6.99). The runner works; the fail/crash list is the stability work plan.
- **11-16 01:28, the Direct3D 11 DDI (offline, compiled only):** d3d10umd offers the D3D11.0 DDI behind the flag
  file `d3d11ddi` (needs `nirshaders`; level 10_0, 10_1 with `msaa`, 11_0 with `fl11`): all 150 members of the
  device function table filled by name (89 shared with D3D10, 47 new), compute shaders and `Dispatch`, UAVs on the
  compute and pixel stages (slots 0-7), indirect draws (arguments read on the CPU), raw/structured buffers, the 11
  query types. Refused cleanly: hull and domain shaders (no tessellation yet), deferred contexts and command lists
  (the runtime emulates them), multi-stream stream output. The 10_1 and 11_0 claims are not fully gated: `fl11` is
  for trying things, not a conformance claim. Branch `sb-nir` 1c0a515 (with 0166 and 0167 merged), sbumd 1572d243
  (`build/win/umd-nir-d3d11`). **T77 staged:** five conformance passes on it (new translator, geometry shaders,
  the D3D11 DDI at 10_1 and at 11_0, dxgi).
- **11-16 01:38, first conformance fixes (offline, compiled only; `sb-nir` 91faa99, sbumd 90fe47b0 in
  `build/win/umd-nir-fix1`):** causes found by reading the failing tests against the code:
  - only render target 0 was ever blended/written (`max_rt` never set in blend states): multiple render targets,
    colour masks and independent blend;
  - Panfrost divided the base instance of per-instance vertex data by the step rate: instanced draws;
  - depth bias values were set but never switched on;
  - a copy or update with an empty or inverted box copied the whole region or crashed;
  - the old translator compared an integer front-face value as a float (front/back swapped) and aborted the
    process on `switch` (now refused cleanly there; the new translator handles it).
  Still open: fragment coordinates, depth-stencil sampling, format-compatibility copies, two crashes without a
  cause, stream output, pipeline statistics.
- **11-16 02:18, more conformance fixes (offline, compiled only; `sb-nir` 1e2a6c6, sbumd 35bae4d5 in
  `build/win/umd-nir-fix3`):** nine more causes, several found by resolving T74's crash addresses in the installed
  DLL: the blitter saved 32 sampler views where Panfrost passes up to 128 (crash with a view at slot 127); the
  empty input layout was never bound when an app sets none (crash); SV_Position was half a pixel low in every
  pixel shader (Bifrost gives integer pixel coordinates); clears of array and 3D views cleared one layer; stencil
  views sampled depth; same-format copies of 32-bit-per-channel typeless textures wrote nothing; the old
  translator dropped the index of an indirectly addressed vertex input. The texture/`resinfo` crashes are confirmed
  as the size and mip-count queries already fixed. Stream output: design only.
- **T76b did not count** (the suite ran on the Basic Render Driver; `WIN11-GPU.md` 6.101). T77 carries all of the
  above in seven passes.

- **11-16 03:05, stream output (offline, compiled only; `sb-nir` cd7d8a9, sbumd 24ec17d9 in
  `build/win/umd-nir-so`):** Direct3D stream-output bookkeeping (append, whole primitives only, overflow, the
  statistics queries, DrawAuto); from the vertex shader on Panfrost's transform feedback for non-indexed lists with
  no flag; from geometry shaders, with several streams, and for indexed/strip/instanced vertex-shader draws through
  the geometry path (`gsemu` + `nirshaders`). The only readback is after a draw whose output count depends on data
  (when a later draw or query needs the count). Found on the way and fixed: every shader destroy read an
  uninitialised field (in the DLL first staged for T77, now replaced).
- **Tessellation: design only.** Hull shaders as one-thread-per-patch kernels and domain shaders as vertex shaders
  compile for Bifrost (5 of 5, 4 of 4); the isoline and triangle tessellators compile, the quad tessellator fails
  NIR validation in the form tried (to be written as libpan kernels). Recommended first version: worst-case
  allocation from the hull shader's maximum factor, no readback (`docs/WIN11-GPU-NIR-FRONTEND.md`, Phase 8).

- **11-16 03:41, independent review of the branch before its first run (`sb-nir` 1dff796, sbumd 6cc75683 in
  `build/win/umd-nir-rev1`, now the DLL staged for T77):** seven defects fixed, found by reading, a sanitizer build
  and ~21,700 mutated shaders per translator: a double free at every compute shader destroy; a use-after-free read
  in the geometry-shader set-up; a stack overwrite when a shader has more than 80 output variables; an unchecked
  index range (crash on malformed bytecode); the token parser trusting opcode numbers and lengths in the release
  build; a leak per refused geometry shader; a static array shared between devices. The new translator then refused
  or translated every mutant without a sanitizer report; the old translator crashes on malformed bytecode throughout
  (upstream code). Being fixed next: undefined behaviour for cull distances on the old path, an instanced
  stream-output draw that can write past its checked extent, a stride field that wraps above 1020 bytes, the
  write set going to the wrong device when one thread has two.

## What this means for the two test apps
- **Geometry Dash:** items 1 and 9. T71 measures the first step.
- **BeamNG.drive:** item 6 (feature level 11_0) is the gate; before that the game cannot create its device. Items
  1-3 come first because item 6 is built on them, and CPU, memory (4 GB) and storage limits remain (6.86).

## Tonight (in order; one phone test at a time)
1. T70: flip-model diagnosis (staged). 2. T72: desktop smoothness numbers (staged). 3. T71: Geometry Dash on the
flip model (after T70's read). 4. T74: the conformance baseline (Wine d3d10core) on the user's drivers. 5. T73: the
same suite on the next sbumd (flip fix + MSAA).
