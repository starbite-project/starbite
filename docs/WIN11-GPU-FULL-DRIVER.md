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

## What this means for the two test apps
- **Geometry Dash:** items 1 and 9. T71 measures the first step.
- **BeamNG.drive:** item 6 (feature level 11_0) is the gate; before that the game cannot create its device. Items
  1-3 come first because item 6 is built on them, and CPU, memory (4 GB) and storage limits remain (6.86).

## Tonight (in order; one phone test at a time)
1. T70: flip-model diagnosis (staged). 2. T72: desktop smoothness numbers (staged). 3. T71: Geometry Dash on the
flip model (after T70's read). 4. T74: the conformance baseline (Wine d3d10core) on the user's drivers. 5. T73: the
same suite on the next sbumd (flip fix + MSAA).
