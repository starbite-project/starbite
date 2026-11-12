# GL ICD zero-copy present (sb-patches/0150-0159), 2026-11-12

Problem: Geometry Dash on our GL ICD runs at ~5-10 fps. The GPU needs <10 ms a frame; the present maps the GL back
buffer (write-combined, CPU reads ~100-180 MB/s) and pushes it through GDI `StretchDIBits`: ~50-110 ms per 2220x1080
frame. GDI's redirected-window output then costs the KMD another CPU copy (CDD's big non-primary blts, WIN11-GPU.md
§6.33-6.36). Goal: present without the CPU touching pixels.

## What the present paths look like today [src]

- **sbwddm's DxgkDdiPresent never touches the GPU.** `MwPresent` records the op in the DMA buffer's private data;
  `MwSubmitCommand` -> `DoPresent` carries it out **on the CPU**: a blt or fill is `CopyBlt`/`CopyRects` from the
  source's CPU view (write-combined, §6.13/6.35: 87 MB/s, split over 4 threads ~79 MPx/s), at DISPATCH_LEVEL inside
  SubmitCommand, holding the in-order queue. A flip is recorded and shown (`MwShow` / ScanFlip). There is no
  KMD-built GPU blit.
- **What already runs on the GPU end to end:** D3D11 flip-model swap chains (sbfps: flip 56-65 fps full screen, T18/T25),
  shared surfaces (sbshare, DWM opening every app's surfaces: sb-patches/0103), x64 D3D11 processes through the
  ARM64X sbumd (T45: `sbd3d x64 exit 0`). DWM composes those surfaces with the Mali; nobody copies pixels on the CPU.
- **Windowed flip-model presents never reach our KMD as a flip or a CPU blt** [shown]: the T48 hive (its gputest
  ran sbfps's `FLIP_SEQUENTIAL` window) has `Live_EmuFlipsNotPrimary` 0 and `Live_PresentOtherFlag` 0; every flip
  the KMD saw was DWM's own primary. dxgkrnl hands a windowed flip-model buffer to DWM (redirected flip) without a
  `DxgkDdiPresent` the KMD acts on, and sbfps's flip rate (56-65 fps full screen, 1440x2960) rules out a KMD CPU
  copy per frame.
- **All processes share one GPU VA space**: an allocation's GPU address is fixed at creation (maliwddm_abi.h), and
  `MW_ESCAPE_ALLOC_INFO` answers it for any device that has the allocation open (`DxgkCbGetHandleData`). So an
  allocation opened by a second device in the same process is the same memory at the same GPU address.

## Options

### (A) A DXGI swap chain in the GL process, fed by a GPU copy  <- chosen
The ICD loads d3d11.dll/dxgi.dll at run time, makes a D3D11 device (FL 10_0) on our adapter (the DXGI adapter whose
LUID equals the shim's D3DKMT adapter; sbumd loads into ARM64 and, as the ARM64X file's EC view, into x64 processes),
and a flip-model swap chain (`FLIP_SEQUENTIAL`, 2 buffers) for the window. Two D3D11 textures with
`D3D11_RESOURCE_MISC_SHARED` are the GL framebuffer's back/front buffers:
- sbumd backs a shared texture with a runtime resource allocation carrying our private data (size, pitch,
  D3DDDIFORMAT: 0103);
- the GL side opens its KMT handle on **its own D3DKMT device** (`D3DKMTQueryResourceInfo` + `D3DKMTOpenResource`,
  the same import sbumd's `OpenResource` does through the runtime), asks the GPU address (`MW_ESCAPE_ALLOC_INFO`) and
  imports it into Panfrost as a linear BO (PRIME fake fd, the pitch from the private data);
- GL renders straight into it (stw's `stw_winsys_framebuffer`, as Mesa's d3d12 WGL path does with its swap chain
  buffers, `d3d12_wgl_framebuffer.cpp`).
Each SwapBuffers:
1. GL flushes (stw's end-of-frame flush) and the ICD waits until its D3DKMT context has finished the frame
   (a CPU event the scheduler signals, `D3DKMTSignalSynchronizationObject2` + `EnqueueCpuEvent`; fallback
   `D3DKMTWaitForIdle`). This is the same wait the GDI path made in `pipe_texture_map`, minus the read.
2. D3D11 `CopyResource(swap chain buffer 0, shared texture)`: sbumd turns it into a Panfrost blit (sb-patches/0105),
   ~10 MB read + 10 MB write on the GPU, a few ms.
3. A 1-pixel `CopySubresourceRegion` of the same texture into a staging texture (the write-after-read fence, below).
4. `IDXGISwapChain::Present(interval)`: the path sbfps and every native D3D app already use; DWM composes the buffer.
5. Swap to the other shared texture. Before GL draws into it again, `Map(READ)` its staging pixel from the frame
   before: sbumd's map waits for the D3D submission that read that texture (an in-order context), so GL never
   overwrites a texture the D3D copy has not read yet. It is one frame old, so it normally does not block.
   (A D3D11 event query would not do: Panfrost has no PIPE_QUERY_GPU_FINISHED, d3d10umd answers it at once.)

- **KMD needs:** nothing new. D3DKMTOpenResource from a second device calls `DxgkDdiOpenAllocation`, which
  maliwddm already implements; the escape resolves the opened handle; flip-model presents are the existing path.
- **UMD needs:** the shim: open a shared resource on the D3DKMT device, close it, the adapter LUID, a frame-done
  wait. The GL target: a `stw_winsys_framebuffer` for Panfrost (new file `wgl_sb_zc.cpp`). sbumd: unchanged.
- **Risk to DWM:** low. DWM sees an ordinary flip-model swap chain from one more process, exactly like Edge or sbfps;
  no KMD code changes, no new present type reaches `DxgkDdiPresent`. Costs: one more D3D device in the GL process
  (sbumd's 32 MB tiler heap + 2 swap chain buffers + 2 shared textures: ~70 MB of GPU memory at 2220x1080; system
  commit was exhausted in GD run 1, so this matters), and one more process whose surfaces DWM opens.
- **Work:** ~600 lines (shim ~150, framebuffer ~450), no KMD build. Unknowns only the phone can answer: whether the
  D3DKMT open of a runtime-created shared resource succeeds from a non-runtime device in the same process, and
  sbumd's flip-model behaviour at GD's resolution.

### (B) `D3DKMTPresent` from the ICD's own D3DKMT device (WDDM 1.x GL ICD style)
`D3DKMT_PRESENT` with `hWindow`, `hSource` = our back buffer allocation, `Flags.Blt`.
- **What reaches our KMD:** `DxgkDdiPresent` with Blt -> `DoPresent` -> **a CPU copy** from the write-combined back
  buffer, in kernel mode at DISPATCH_LEVEL, holding the GPU queue that DWM also uses. At 79 MPx/s a 2.4 MPx frame
  is ~30 ms of kernel CPU per frame, and DWM's own submissions wait behind it (§6.33: DWM UMD waits up to 740 ms
  when the KMD copied in SubmitCommand). It moves the copy, it does not remove it.
- **Under DWM** (always on since Windows 8) a windowed `hWindow` present is a redirected present: the destination is
  the window's redirection surface and the protocol is opengl32's `PresentBuffers` callback with a
  `PresentHistoryToken` (Mesa has the hooks: `stw_winsys::shared_surface_open/compose`, unused by every current
  winsys). The token/present semantics are not publicly documented, and what `DxgkDdiPresent` would receive for it
  (a token-only present? a blt with `RedirectedFlip`?) is unknown on our KMD.
- **To be zero-copy, the KMD needs a GPU blit:** a Mali job built in the KMD (fragment job + blit shader binary +
  descriptors, or a compute copy), per present, ordered in the queue. That is the "real GPU blit" §6.33 deferred: weeks,
  and it would also change every GDI/CDD blt DWM depends on.
- **Risk to DWM: high** (shared present code, kernel CPU time on DWM's queue). Work: large.

### (C) Simpler alternatives considered
- **C1. opengl32's `PresentBuffers` callback + `compose` into DWM's redirection surface** (the native WDDM GL ICD
  model). GPU-side it is the same import as (A) (open the redirection surface, blit into it), with no D3D runtime in
  the process. Undocumented token handshake, and the final notification is again a `D3DKMTPresent` whose KMD
  semantics are unknown (above). Good long-term candidate once (A) proves the import; not a prototype.
- **C2. A cached CPU view of the back buffer.** §6.34/6.35: Windows maps it with the pages' existing attribute
  (write-combined), measured no faster. Still a CPU copy. Rejected.
- **C3. DirectComposition / composition swap chain (`IPresentationManager`) on the shared texture**, no copy at all.
  Needs NT-handle sharing and presentation-manager support we have never exercised on a WDDM 1.3 / D3D10.0-DDI
  driver. Possible follow-up to (A) to drop its one GPU copy.
- **C4. GL renders into the swap chain's buffers directly.** Flip-model buffers cannot be opened by handle
  (`GetSharedHandle` fails on them). Rejected; hence the shared texture + one GPU copy.
- **C5. The parallel-copy quick fix (0146, separate work).** Still CPU; complementary as the fallback's speed-up.

## Decision
(A). It is the only option that is zero-CPU-copy with **no KMD change**, it reuses three paths that already work on
the phone (shared surfaces, x64 D3D11 through the ARM64X sbumd, flip-model presents), and its failure modes are
local to the GL process (it falls back to GDI at framebuffer creation).

## Implementation (prototype)
Mesa worktree `~/sbmesa/src-zc`, branch `sb-zc` (from `sb-glp`). Patches in the project worktree:
- `0150-shim-kmt-open-shared.patch` (`include/win32-compat/malikm_drm.c`):
  - `malikm_kmt_adapter_luid(LUID *)`: the D3DKMT adapter's LUID (kept from `D3DKMTEnumAdapters2`);
  - `malikm_kmt_open_shared(global_handle, &info)`: `D3DKMTQueryResourceInfo` + `D3DKMTOpenResource` on the
    process's D3DKMT device; the allocation's `MW_ALLOCATION_INFO` (size, width, height, pitch, D3DDDIFORMAT);
    GPU VA via `MW_ESCAPE_ALLOC_INFO`; registered as an opened BO (kind 3: never destroyed by GEM_CLOSE);
    returns the PRIME fake fd and the opened resource handle;
  - `malikm_kmt_close_shared(hResource)`: device idle, then `D3DKMTDestroyAllocation(hResource)`;
  - `malikm_kmt_frame_wait(ms)`: everything the D3DKMT context was given is done (CPU event through
    `D3DKMTSignalSynchronizationObject2`/`EnqueueCpuEvent`, else `D3DKMTWaitForIdle`).
  - Optional entry points (`QueryResourceInfo`, `OpenResource`, `SignalSynchronizationObject2`) do not change what
    `wddm_init` requires.
- `0151-wgl-panfrost-zero-copy-present.patch` (`src/gallium/targets/wgl/`):
  - `wgl_sb_zc.cpp`: the `stw_winsys_framebuffer` (create, resize, get_resource, present, destroy) described in (A);
  - `wgl_sb_pipe.c`: the calling thread's GL pipe context for the GDI fallbacks (its own file: `st_context.h` must
    precede `<windows.h>`);
  - `wgl.c`: `wgl_create_framebuffer` returns it for Panfrost unless `SB_GL_PRESENT=gdi` (or `copy`, 0145's A/B);
    `NULL` (= the GDI path, unchanged) when any step fails at creation: d3d11/dxgi missing, no adapter with our LUID,
    device, swap chain, shared textures, `GetSharedHandle`, the D3DKMT open, the Panfrost import; or for pixel
    formats with `PFD_SUPPORT_GDI`, single-buffered ones, colour formats other than B8G8R8A8/X8, R8G8B8A8/X8, and
    pbuffers' hidden windows (class `wglpbuffer`). d3d11.dll/dxgi.dll are loaded at run time: the ICD's import
    table is unchanged, so it still loads where they are missing.
  - **After creation**, a resize whose shared textures cannot be made switches that window to its own Panfrost
    buffers presented by the GDI code (0145's `wgl_panfrost_present`), so a failure never leaves stw without a back
    buffer. A failed swap-chain `Present` shows that frame through GDI; three in a row drop the swap chain (it would
    hide GDI's output) and the window presents through GDI from then on.
  - `SB_GL_ZC_STATE` (process environment, set by the ICD for tests): `zc`, `off`, `fail:<step>`,
    `gdi-after-resize`, `gdi-after-present-failures`.
  - Environment: `SB_GL_PRESENT=gdi|copy|zc` (default zc), `SB_GL_ZC_SWAP=flip|flipdiscard|discard` (default flip =
    FLIP_SEQUENTIAL), `SB_GL_ZC_LOG=1` (one line per setup step and a 300-frame timing summary to stderr/umdlog).
  - `MakeWindowAssociation(NO_WINDOW_CHANGES|NO_ALT_ENTER)`: DXGI must not take Alt+Enter from the game.
- `tools/win/mesa/tests/sbglzc.cpp`: renders a known frame at 1280x720 and presents 300 times; reports fps, ms per
  swap (mean, max), the KMD's present CPU time and GPU busy per frame (`MW_ESCAPE_SB_RATES`), which present path the
  ICD took (`SB_GL_ZC_LOG` / modules: d3d11.dll and sbumd.dll loaded?), and reads back the frame's colour bars
  (glReadPixels of the back buffer = the shared texture, before and after swap). Run it twice: `SB_GL_PRESENT=gdi`
  and default, and compare.

## What needs the phone
1. `sbglzc64.exe` (x64, through the ARM64X sbumd) and the ARM64 ICD build, once with `SB_GL_PRESENT=gdi`, once
   default: fps, `kmd present cpu ms/frame` (should be ~0 with zc), visible frames (camera: colour bars moving).
2. umdlog of the test process: the `sb gl zc:` lines (which step, if any, failed) and sbumd's `OpenResource`-less
   import path (`malikm-wddm: opened shared ...`).
3. GD with the x64 ICD (gl-kit): fps; Alt+Enter / resizing / minimize-restore (resize path, ResizeBuffers).
4. DWM health over the run: no new hangs/faults in the KMD's ledger, DWM's present rate unchanged.

## Open risks / known limits
- **D3DKMTOpenResource from a non-runtime device on a runtime-created resource:** documented for WDDM 1.x, never tried
  on our stack. If it fails, the fallback is the GDI path (logged), and the alternative is the reverse direction:
  the D3D device opens a GL allocation (needs the runtime private data D3D11's OpenSharedResource expects: not
  possible for a raw D3DKMT allocation) - so the real alternative would be C1.
- **CPU sync point per frame** (step 1): the GL thread still waits for the GPU to finish each frame, as the GDI path
  did. Pipelining (present frame N-1 while N renders, three shared textures) is the next step once this works.
- **Memory:** +~70 MB GPU memory per GL window (above), in a system that ran out of commit in GD run 1.
- **sbumd in the GL process:** a second user-mode driver instance (its own umdlog, its own 32 MB tiler heap); its
  DXGI flip-model path at non-full-screen sizes has only been exercised by native apps (Edge, sbfps full screen).
- **MSAA visuals:** stw resolves into our shared texture (as for d3d12); untested.
- **ARM64EC/ARM64X ICD:** not built for this prototype (x64 and ARM64 were); the code has nothing arch-specific.
- **A swap chain made and dropped at creation** (a late step failed): the window should go back to DWM's redirection
  bitmap for the GDI path; not verified.

## Built (2026-11-12)
Mesa `~/sbmesa/src-zc`, branch `sb-zc` = `sb-glp` (079d45f) + 0150 (9794673) + 0151 (c258bd3); the two patch files
applied to 079d45f reproduce that tree exactly (checked in a scratch worktree).
```
cd ~/sbmesa && export PATH=~/sbmesa/native:~/sbmesa/venv/bin:$PATH
meson setup build-zc-x64 src-zc <the options of build-wgl-x64's meson-log.txt> --cross-file=~/sbmesa/tc/cross-x64.ini
meson setup build-zc-a64 src-zc <the same options> --cross-file=~/sbmesa/tc/cross.ini
ninja -C build-zc-<arch> src/gallium/targets/wgl/libgallium_wgl.dll src/gallium/targets/libgl-gdi/opengl32.dll
# options: -Dbuildtype=release -Db_ndebug=true -Db_vscrt=mt -Dplatforms=windows -Dgallium-d3d10umd=false
#  -Dvulkan-drivers= -Dllvm=disabled -Dopengl=true -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled
#  -Dgbm=disabled -Dvideo-codecs= -Dmicrosoft-clc=disabled -Dspirv-to-dxil=false -Dzstd=disabled
#  -Dshader-cache=disabled -Dbuild-tests=false -Dexpat=disabled -Dzlib=disabled -Dgallium-drivers=softpipe,panfrost
#  -Dmesa-clc=system -Dprecomp-compiler=system
# test (~/sbmesa/zc-test, source = tools/win/mesa/tests/sbglzc.cpp)
~/sbmesa/tc/x64/clang-cl /nologo /O2 /EHsc /MT -D_CRT_SECURE_NO_WARNINGS sbglzc.cpp /Fesbglzc64.exe /link /machine:x64 \
  /libpath:/Users/dev/sbwin/msvc/lib/x64 /libpath:/Users/dev/sbwin/kit/Lib/10.0.26100.0/ucrt/x64 \
  /libpath:/Users/dev/sbwin/kit/Lib/10.0.26100.0/um/x64
~/sbmesa/tc/clang-cl ... /Fesbglzc.exe /link /machine:arm64 (the arm64 lib dirs)
```
Files here: `x64/` and `arm64/` (`libgallium_wgl.dll` + `opengl32.dll`, app-local like gl-kit), `sbglzc64.exe` (x64),
`sbglzc.exe` (ARM64), `SHA256SUMS`. No KMD change, so no sbwddm build.

## Verified offline
- Both ICDs and both tests compile and link with no new warnings (the two `u_inlines.h` ones are the tree's own).
- The ICDs import the same DLLs and export the same names as 0145's builds (`build/win/gl-present/{x64,arm64}`):
  diffed `llvm-objdump -p`. d3d11/dxgi and the new D3DKMT entry points are resolved at run time.
- `MW_ALLOCATION_INFO`'s layout is checked at compile time (`static_assert` in `wgl_sb_zc.cpp`).
- `sbglzc64.exe` imports `D3DKMTEnumAdapters2`/`D3DKMTEscape` from GDI32 (available since Windows 8).
- The stw contract (traced in `stw_st.c`/`stw_framebuffer.c`): stw asks the winsys framebuffer only for FRONT/BACK
  colour (depth is st's own), swaps its two textures after each `present` the way our `cur` index flips, drops its
  texture references before `resize`, and calls `destroy` before releasing them (hence the deferred
  `malikm_kmt_close_shared`).
- Nothing ran: there is no Windows (or Wine) in the VM, and the path needs sbwddm + sbumd.

## Run on the phone
```
copy x64\libgallium_wgl.dll, x64\opengl32.dll next to sbglzc64.exe    (arm64\ ones next to sbglzc.exe)
set SB_GL_ZC_LOG=1
sbglzc64.exe C:\starbite\sbglzc64.log      -> A/B: SB_GL_PRESENT=gdi, then zero-copy, two RESULT lines
sbglzc.exe   C:\starbite\sbglzc.log        -> the same, native ARM64
```
Expect in the zero-copy RESULT: `icd zc`, `d3d11 yes sbumd yes`, `kmd present cpu` ~0 ms/frame, both readback
CHECKs PASS, fps well above the gdi line. With the umdlog folder present, stderr (`sb gl zc:` and
`malikm-wddm: opened shared ...` lines) lands in the process's umdlog.
