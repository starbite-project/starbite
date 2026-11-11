# Windows on the S9: what games and apps need from the GPU, and the order to build it

Track D (Windows GPU), 2026-11-11. Context: docs/WIN11-GPU.md. The user, on the big-core entry with T41:
"smooth, with the occasional graphical bug". Geometry Dash crashes at launch, and their verdict is "the graphics
driver is simply not advanced enough". This plan answers that: what each kind of game asks for, what we have, and
what to build first.

Tags: **[shown]** = seen on our phone; **[src]** = read in our source tree; **[doc]** = vendor documentation (URL
in §8); **[inferred]**; **[unverified]**.

## 1. In one paragraph

Today the Mali serves only **native ARM64 programs** (DWM, Explorer, Edge, the shell, Settings), and only
through **Direct3D 10/11 at feature level 10_0**. Nothing serves **OpenGL** at all: Windows falls back to its
1997 GDI software GL 1.1. Nothing serves **Direct3D 9** or **Direct3D 12**. **x64 and x86 programs** (almost every
Steam game, Geometry Dash included) run under Windows' emulator, and our UMD is not built for them (§4.2).

**Recommended order:**
1. **an OpenGL driver** (Mesa's WGL frontend on our Panfrost and kernel shim), built for ARM64 and for x64
   processes;
2. **sbumd for x64/x86 processes**, so D3D11 games reach the GPU;
3. **D3D 10_1 plus real caps**;
4. **D3D9 through wined3d on our OpenGL**, not a native D3D9 DDI;
5. **no D3D12.**

Step 1 alone fixes Geometry Dash and the other cocos2d/SDL/GL games.

## 2. Geometry Dash: why it crashes [shown]

From the user's WER reports (pull 2026-11-11, `build/win/pull-1111/wer/ReportArchive/AppCrash_GeometryDash.exe_*`,
three reports from 2026-11-08 and 2026-11-10):
- **The same crash every time:** `libcocos2d.dll+0xd9cd0`, c0000005 (access violation).
- **An x64 program:** `xtajit64.dll` is loaded and it uses `steam_api64.dll`. So GD runs under Windows' x64
  emulator (GD 2.2 is 64-bit).
- **The graphics stack it loaded:** `OPENGL32.dll`, `GLU32.dll`, `glew32.dll`, and **no OpenGL driver**. There is
  no ICD DLL in the module list.
- **The registry agrees:**
  - `SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers` is empty, and so is its WOW6432Node twin;
  - our display key has no `OpenGLDriverName`;
  - no Khronos Vulkan/OpenCL keys;
  - the compatibility pack is not installed (pulled SOFTWARE hive, this morning).
- **What follows:**
  - opengl32.dll gives GD Microsoft's GDI generic implementation (GL 1.1, software);
  - cocos2d-x needs GL 2.0 at least (shaders, VBOs, FBOs; Steam lists "OpenGL 2.0");
  - glew leaves the GL 2.0 entry points NULL, and cocos2d dereferences one of them or its result.
  - **[inferred]** until the instruction at +0xd9cd0 is disassembled; see the pull list in the notes.
- **So:** a "graphics driver not advanced enough" in the most literal sense: for OpenGL there is no driver at all.
  This is not a Panfrost bug.

## 3. What games ask for

| API | Who uses it | Minimum that matters |
|---|---|---|
| OpenGL 2.0-2.1 | cocos2d-x (GD), older SDL/LWJGL games, many indies, emulator front-ends | GLSL 1.10/1.20, FBOs |
| OpenGL 3.2-3.3 core | Minecraft Java 1.17+ (3.2), most GL engines since ~2012, wined3d's good path | geometry shaders are formally part of 3.2 |
| OpenGL 4.x | newer emulators (RPCS3, Dolphin's best paths), shader packs | tessellation, compute (4.3) |
| D3D9 | a huge back catalogue of x86 games, plus many launchers and tools | a D3D9 DDI in the UMD, or D3D9On12 (needs D3D12) |
| D3D11 FL 10_0/10_1 | Unity's D3D11 player (SM4 floor) [inferred], older D3D11 titles, WebView2/Chromium, XAML | what we have, minus geometry shaders |
| D3D11 FL 11_0 | Unreal 4, most D3D11 games after ~2013 | SM5, tessellation, CS 5.0 |
| D3D12 | modern AAA, Unreal 5 | FL 11_0+, a WDDM 2.0 GPU VA model |
| Vulkan | a few games; DXVK/vkd3d translation layers | Vulkan 1.3 for current DXVK |

## 4. What we have

### 4.1 The hardware and Panfrost [src]
Mali-G72 MP18 = Bifrost **v6**. Mesa 26.2's Panfrost on v6 (`pan_screen.c`):
- **GLES 3.2** (ESSL 320);
- desktop **GL 3.1** (GLSL 140; `PAN_MESA_DEBUG=gl3` claims 3.3 without geometry shaders);
- compute shaders, int64, cube-map arrays, image load/store, 16x anisotropy, texture buffers;
- **no geometry or tessellation shaders** (not in the hardware; Mesa has no emulation for Panfrost).

So the realistic ceilings are **GL 3.1 (3.3 by the flag)** and **D3D11 FL 10_x without geometry shaders**. GL 4.x,
FL 11_0 and D3D12 need tessellation, which the G72 doesn't have.

### 4.2 Windows' side [src][shown]
- **D3D10/11:** sbumd = Mesa 26.2 d3d10umd + Panfrost.
  - `SUPPORT_D3D10_1 0`, `SUPPORT_D3D11 0`, so only the D3D10.0 DDI.
  - `GetCaps` answers zeros.
  - The INF lists `UserModeDriverName = sbumd.dll ×4` and nothing else.
  - sbumd.dll is **ARM64 only**.
- **x64 / x86 processes:** **x64 / x86 processes get no GPU from us.**
  - **x64 on ARM64 Windows has no WOW64 layer.** An emulated x64 process loads x64 or ARM64EC code only. Windows'
    own DLLs are ARM64X (one file holding both an ARM64 and an ARM64EC image), so they serve both kinds of
    process [doc].
  - **Microsoft documents no separate value for x64 processes**, so the DLL named in `UserModeDriverName` must
    itself be ARM64X [inferred].
  - **Qualcomm's naming fits that:** their native UMDs are `qcdx11arm64xum.dll` / `qcdx12arm64xum.dll` [doc,
    third-party lists]. NVIDIA's ARM64 driver uses an ARM64X forwarder that loads an ARM64EC build into x64
    processes [doc].
  - **x86 (32-bit) processes** use `UserModeDriverNameWow`. Qualcomm ships x86 UMDs (`qcdx11x86um.dll`, plus CHPE
    builds) for them [doc, third-party list].
  - **Ours:** sbumd.dll is ARM64-only and there is no `…Wow` value.
    - An x64 or x86 game's D3D11 device therefore cannot get our GPU: it either fails or falls back to WARP, the
      CPU renderer. Which of the two is [unverified].
    - Snapdragon 850 devices have the same gap (no x64-capable UMD), and x64 apps there run without GPU
      acceleration [doc].
- **OpenGL:** none (above). **D3D9:** none of our own. **D3D12, Vulkan, OpenCL:** none.
- **Our kernel shim already has a standalone path:** the Panfrost WDDM shim (`tools/win/mesa/g52/win32-compat/
  malikm_drm.c`) talks to sbwddm through the D3D runtime's callbacks (`pfnAllocateCb` / `pfnRenderCb`) *or*
  directly through the **D3DKMT** system calls:
  - `D3DKMTCreateDevice`, `CreateContext`, `CreateAllocation`, `Escape`;
  - one device and context per process.
  - An OpenGL ICD is exactly such a D3DKMT client (OpenGL ICDs submit with `D3DKMTRender`), so most of its
    kernel plumbing is written.
  - It was written for the G52 port's WinPE tests and has not yet run against sbwddm [unverified].

## 5. The options

### 5a. An OpenGL ICD from Mesa on our Panfrost (recommended first)

**What it is:** Mesa's gallium WGL frontend (`src/gallium/frontends/wgl`, the `stw_*` code) builds
`libgallium_wgl.dll`, a real OpenGL ICD. Mesa ships it on Windows for d3d12 (GLOn12), zink and llvmpipe.
`targets/wgl/wgl.c` picks the screen by name. We add `panfrost`:
- `malikm_open()` takes the shim's D3DKMT path;
- then the Panfrost screen;
- the present path in wgl.c is the same hook d3d12 uses.

**Present, first version:**
- map the back buffer (linear) and `StretchDIBits` it into the window's DC, the way llvmpipe's GDI winsys does;
- this costs one CPU copy per frame, like the blt presents WIN11-GPU.md §6.31 measured;
- later, a D3DKMT present or a DXGI swap chain on a shared allocation.

**Registration:**
- the display driver's own key: `OpenGLDriverName` (plus `OpenGLVersion`, `OpenGLFlags`, and the `…Wow` variant
  for x86), so opengl32 finds it through `D3DKMTQueryAdapterInfo(KMTQAITYPE_UMOPENGLINFO)`;
- or, for testing without touching the registry, an **app-local** Mesa `opengl32.dll` + `libgallium_wgl.dll` next
  to the game's exe (opengl32 is not a KnownDLL).

**The architecture question** (an ICD loads *into the game's process*, so it must match that process):
- **ARM64 processes:** a plain ARM64 build (our toolchain today).
- **x64 processes (GD, most Steam games):** the ICD must load into an x64 process, so it must be **ARM64EC**, best
    as **ARM64X**: one DLL for both ARM64 and x64 processes, like Windows' own opengl32.dll [doc].
    - No one has published an ARM64EC Mesa build yet [doc: none found].
    - **The toolchain:** clang 18 (ours) has only early ARM64EC support. MSVC's `/arm64EC` is the proven compiler;
      clang ≥ 19 is the alternative. The libraries are the ARM64EC CRT and the SDK's ARM64X import libraries
      (we have the ARM64 ones only) [unverified].
    - **Fallback:** a plain **x64** build of the ICD, run by the emulator. GPU work is unchanged, but the driver's
      CPU side (including the shader compiler) is emulated.
    - **For a first test:** an x64 build, used as an app-local `opengl32.dll` next to GeometryDash.exe, needs no
      registry change at all.
- **x86 processes (32-bit games):** an x86 build, run by the emulator.
  - Our private-data ABI (`starbite/win/sbwddm/maliwddm/maliwddm_abi.h`) uses only fixed-width fields
    (`mw_u32`/`mw_u64`, no pointers), so its layout is the same for 32-bit callers [src].
  - Not yet tried: a WOW64 process driving the KMD through D3DKMT [unverified].

**The Microsoft route, for comparison:** the **OpenCL, OpenGL and Vulkan Compatibility Pack** (Store
`9NQPSL29BFFF`, package `Microsoft.D3DMappingLayers`):
- It is Mesa's own GL-on-D3D12 driver (GLOn12), registered by Windows so that `opengl32.dll` redirects to
  `OpenGLOn12.dll` [doc].
- Mesa marks d3d12 complete up to **GL 4.6**; Microsoft first shipped 3.3 [doc].
- It ships x86, x64 and ARM64 packages [doc, third-party]. Whether the ARM64 one serves emulated x64 games is
  [unverified]; one reader comment on Microsoft's blog says emulated x86 games work.
- **It needs a D3D12 driver for the GPU, and we have none**, so it cannot use the Mali. On WARP (D3D12 on the CPU)
  Mesa's d3d12 driver runs with `GALLIUM_DRIVER=d3d12` and `LIBGL_ALWAYS_SOFTWARE=1` [doc]; whether the Store pack
  falls back to WARP by itself is [unverified].
- **So:** the pack is a cheap *diagnostic* for GD (does it start with any GL ≥ 2.0?), not a solution. The real one
  is our own ICD on the Mali.

**What it gives:**
- GL 3.1 on the GPU, which covers GD, cocos2d, SDL/GL indies and most GL 2.x/3.x engines;
- plus wined3d (5d) for D3D9;
- Minecraft Java needs 3.2 (the gl3 flag may do).

**Effort:**
- the build: Mesa's WGL target with our Panfrost and shim, plus the present hook;
- a small native GL test (`sbgl.exe`: context, `GL_VERSION`, a triangle read back, a swap);
- then the x64 build and the toolchain for it.

**Risk:**
- GL contexts on many threads;
- `SwapBuffers` pacing;
- the shim's "one device per process" assumption under a game that creates several contexts.

### 5b. D3D12 via Mesa's Panfrost: no

- Mesa has **no D3D12 frontend**; its `d3d12` driver goes the other way (gallium on top of D3D12).
- A D3D12 UMD would need FL 11_0 (tessellation, which the G72 lacks), resource binding tier 1, and the WDDM 2.0
  GPU VA model in our KMD.
- The only other path is vkd3d-proton on a Windows PanVK (Vulkan) port: PanVK on v6 is far from the Vulkan
  1.3 + features vkd3d needs [inferred].
- Not worth starting. D3D12-only games stay out of reach on this GPU.

### 5c. The D3D11 path's gaps (do after 5a, in this order)

1. **x64/x86 builds of sbumd:** the same architecture rule as the ICD:
   - `UserModeDriverName` → an **ARM64X** sbumd (ARM64 image + ARM64EC image);
   - `UserModeDriverNameWow` → an **x86** sbumd for 32-bit games;
   - each is a separate Mesa build of the same tree.
   - Before we copy the pattern, confirm Qualcomm's `qcdx11arm64xum.dll` is ARM64X (`dumpbin /headers` on a real
     package) [unverified].
   - **Note:** upstream Mesa deleted d3d10umd on 2026-09-30 (MR 44717). We already maintain it ourselves at
     26.2.6 (WIN11-GPU.md §6.3), so this is no new cost. It also means upstream will never fix it for us.
2. **`SUPPORT_D3D10_1 1`:**
   - the code paths exist in d3d10umd, and the viogpu3d lineage runs with it on;
   - FL 10_1 is what Chromium/ANGLE and many D3D11 games probe first.
3. **Real `GetCaps`:**
   - today every query gets zeros;
   - games read format support, the threading caps and the shader-model caps through it, and some refuse to start
     on zeros.
4. **Not FL 11_0:** that needs SM5 DXBC (d3d10umd translates SM4 only) and tessellation (no hardware).

### 5d. D3D9: through wined3d on our OpenGL, later

- Windows runs D3D9 either through a D3D9 DDI in the UMD (Mesa had one, Gallium Nine, removed in 25.2 and
  Linux-only) or through **D3D9On12**, which needs D3D12.
- Once 5a exists, **wined3d** (Wine's D3D-on-GL, built for Windows as app-local `d3d9.dll`/`d3d8.dll`) gives D3D8/9
  on GL 3.x, per game, without touching Windows.

## 6. Order, and what to start today

| # | Step | Phone? | Unlocks |
|---|---|---|---|
| 1 | Mesa WGL + Panfrost + the shim's D3DKMT path, **ARM64**, plus `sbgl.exe` (native test) | one unattended cycle | proves GL on the Mali |
| 2 | The same ICD for **x64** processes; GD test with an app-local opengl32.dll | one cycle + the user | GD and x64 GL games |
| 3 | Register it in the INF (`OpenGLDriverName`, plus `…Wow` once the x86 build exists) | install | all GL apps, no copying |
| 4 | sbumd for x64 (and x86) processes | cycle | D3D11 games on the GPU |
| 5 | `SUPPORT_D3D10_1`, real `GetCaps` | cycle | games that probe 10_1 |
| 6 | wined3d app-local for D3D9 games | user | D3D9 games |

**Done today (2026-11-11, no phone):**
- **The plan, and the GD diagnosis** (§2).
- **Mesa's GL stack builds for ARM64** in the VM (`~/sbmesa/build-wgl`: `-Dopengl=true`, softpipe + panfrost,
  same cross file as sbumd). Two fixes were needed, both **sb-patches/0135**:
  - UCRT's `wchar.h` pulls in `arm_neon.h` under clang-cl on ARM64, and its global `float16_t` (`__fp16`) clashed
    with `ir.h`'s global alias of `mesa::float16_t`. The alias is gone; its six users say `mesa::float16_t`.
  - The ARB program lexer called `isatty` (flex on a Linux build host). It is now `never-interactive`, like
    Mesa's other two lexers.
- **sb-patches/0136: Panfrost in the WGL target** (`targets/wgl/wgl.c`, meson):
  - `panfrost` is tried first, unless `LIBGL_ALWAYS_SOFTWARE` is set;
  - screen creation is d3d10umd's: `malikm_open()` takes the shim's D3DKMT path when no runtime is bound, plus
    driconf;
  - present maps the back buffer and `StretchDIBits` it into the window DC (one CPU copy).
  - Output: `libgallium_wgl.dll` (18.4 MB) + `opengl32.dll`.
- **`tools/win/mesa/tests/sbgl.cpp`:** the native GL test (clear, GL 1.1, GLSL + VBO, texture, FBO, swap rate;
  `hw` fails on a software renderer).
- **sb-gputest.cmd** runs it app-local from `gl\`, on the Mali and then on softpipe.
- **Stage `sbwddm-t43`** = T42 + that test.
- **Next, once T43 has run:**
  - fix what sbgl finds;
  - then the x64 question: an ARM64EC build needs clang ≥ 19 or MSVC, plus ARM64EC libraries; an x64 build needs
    x64 libraries. Either way, GD is then tested with an app-local opengl32.dll in its folder.

**Later the same day (scale-up):**
- **The x64 ICD builds** with no source changes beyond 0135/0136:
  - `~/sbmesa/build-wgl-x64`, with `~/sbmesa/tc/cross-x64.ini` and the wrapper `~/sbmesa/tc/x64/clang-cl` (meson
    only treats it as clang-cl under that exact name);
  - `libgallium_wgl.dll` (c2b4225c, AMD64) and `opengl32.dll` (8aca347d): 360 gl/wgl exports, system imports only,
    Panfrost and the shim inside.
- **`sbgl64.exe`:** the test as an x64 program.
- **The kit:**
  - `tools/win/setup.sh` gained `SBWIN_ARCHES="x64 x86"`: Microsoft's x64/x86 SDK and CRT packages of the same
    releases, pinned by sha256; the VS ones match Microsoft's manifest.
  - **ARM64EC needs nothing extra:** our arm64 libraries are ARM64X. libcmt/msvcrt/libucrt carry ARM64EC objects
    (machine 0xA641), and the SDK import libraries carry ARM64EC import thunks.
- **The ARM64EC build** (clang/lld 20, `~/sbmesa/build-wgl-ec`): in progress in a separate worktree
  (`~/sbmesa/src-ec`), patches 0140-0149.
- **Stage `sbwddm-t44`:** T42 + the KMD hang forensics (WIN11-GPU.md §6.52) + the GL tests on ARM64 and x64.
- **The attended GD kit `build/win/gl-kit`:**
  - `gd-gl-on.cmd` copies the x64 ICD into the Geometry Dash folder and marks it; `gd-gl-off.cmd` removes exactly
    those files;
  - it refuses to touch an `opengl32.dll` that isn't ours.

## 7. What the user can do meanwhile

- **Geometry Dash:** no fix to install yet. It needs our OpenGL driver (§5a, steps 1-2).
- **Optional, a 5-minute diagnostic:** install "OpenCL™, OpenGL®, and Vulkan® Compatibility Pack" from the
  Microsoft Store (small download), then start GD once.
  - **If GD then starts (slowly, or with a message):** the missing OpenGL driver is confirmed as the cause.
  - **If it still crashes the same way:** the pull list in the notes tells us more.
  - The pack can be uninstalled afterwards. It does not touch our driver.
- **x64 games in general:** today they run on the CPU (WARP) or not at all, whatever their API (§4.2). Native ARM64
  apps (Edge, the shell, Explorer, Office for ARM) are the ones that use the GPU.

## 8. Sources

- Loading an OpenGL ICD (`OpenGLDriverName`, `…Wow`, `KMTQAITYPE_UMOPENGLINFO`):
  https://learn.microsoft.com/en-us/windows-hardware/drivers/display/loading-an-opengl-installable-client-driver
- x86/x64 emulation on ARM64, ARM64X system binaries:
  https://learn.microsoft.com/en-us/windows/arm/apps-on-arm-x86-emulation
- ARM64EC (which code an x64 process can load): https://learn.microsoft.com/windows/arm/arm64ec
- NVIDIA's ARM64X/ARM64EC driver DLLs:
  https://docs.nvidia.com/rtx-spark/rtx-spark-porting-guide/latest/portingandemulation/arm64ecvsarm64.html and
  https://docs.nvidia.com/rtx-spark/rtx-spark-porting-guide/latest/porting/vk-gl.html
- Qualcomm UMD file names (third-party copy list): https://github.com/Justsenger/ExHyperV/blob/main/doc/drivermapping.md ;
  qcdx11arm64xum.dll in crash logs: https://github.com/citizenfx/fivem/issues/4023
- Snapdragon 850, no x64 UMDs: https://chipsandcheese.com/p/state-of-windows-on-arm64-a-high-level-perspective
- The Compatibility Pack: https://devblogs.microsoft.com/directx/announcing-the-opencl-and-opengl-compatibility-pack-for-windows-10-on-arm/ ;
  opengl32 → OpenGLOn12 redirect by design: https://github.com/microsoft/OpenCLOn12/issues/49 ; install layout:
  https://github.com/microsoft/OpenCLOn12/issues/68 ; WARP use: https://github.com/pal1000/mesa-dist-win/blob/master/readme.md
- Mesa features (d3d12 to GL 4.6): https://gitlab.freedesktop.org/mesa/mesa/-/raw/main/docs/features.txt
- d3d10umd removed upstream: https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/44717
- ARM64 Windows Mesa WGL builds exist (MSYS2 clangarm64):
  https://packages.msys2.org/packages/mingw-w64-clang-aarch64-mesa
- Panfrost v6 caps: our tree, `src/gallium/drivers/panfrost/pan_screen.c` (Mesa 26.2.6).
