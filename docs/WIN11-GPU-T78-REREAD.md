# b78: T78's DWM job timeouts and black screen shots

(2026-11-16 17:10 EET. Read of the T78 data and the code; nothing here has run on a phone.)

Every statement is tagged **[M]** measured (from test data), **[R]** read (from source), or **[I]** inferred.

## 1. What the record should say instead of 6.104

1. **The storm did not start when Edge started.** [M] DWM's own log (`pre/umdlog/dwm.exe-1116-39109.log`, first
   line 02:47:51.365, then one stats block per ~10 s) shows DWM healthy for 100 s with Edge up and after it was
   killed, and the first blocked window only at 02:50:46:

   | DWM stats window | submits | blocked | what the script did |
   |---|---|---|---|
   | 02:49:03 - 02:50:03 (6 windows) | 703-1256 each | 1-53 ms | Edge up (started 02:49:05.15) |
   | 02:50:03.4 - 02:50:13.4 | 332 | 74 ms | Edge killed ~02:50:00-04, Settings started 02:50:08.14 |
   | 02:50:13.4 - 02:50:35.5 | 22 | 0.6 ms | Settings up; shot at 02:50:35 is fine |
   | 02:50:35.5 - 02:50:46.1 | 314 | 17 ms | Settings closed ~02:50:38, force-killed ~02:50:41; **calc.exe started 02:50:43.82** |
   | **02:50:46.1 - 02:50:56.6** | **60** | **6992 ms** (one wait 5493 ms) | Calculator starting |
   | every later window | 5-260 | up to 18 s | |

   [M] The KMD's hang times count from the KMD's start (`A->StartTime`, `adapter.c:419`), not from boot. The first
   hung chain started at KMD 190.19 s and 16 hangs followed back to back (190.94 ... 202.34, 0.752 s apart). [I] For
   those 12 s to fall where DWM's log has its first blocked windows, the KMD started 22.8-27.2 s after the tick
   count's zero (02:47:12.256, from the log names), so **the first hung chain started between 02:50:45.3 and
   02:50:49.6: 1.5-6 s after calc.exe was launched, 4-12 s after Settings was killed, 45 s after Edge was killed.**
   "190.9 s into the boot = Edge" in 6.104 read KMD seconds as boot seconds.
2. **T71 had the same hang.** [M] `results-t71/ours/pre/SYSTEM` holds `Live_HangLog` records #1-#10 at KMD
   2284.6-2296.1 s: `dwm.exe`, `indexed-vertex`, slot 1, js 0x08, no MMU fault, 751-752 ms, the same signature
   (and `Live_JobsTimedOut`/`GpuResets` 10 in a freed cell). [M] T71's sign-out `LogonUI.exe` started at tick 2302.0
   (23:58:05.45); [I] with the same offset the hangs start 5-10 s after it, in the session's last seconds, after
   the test script had ended. DWM's log shows no blocked wait there, which is why 6.104 says T71 had none.
3. **Which boots have timeouts** [M] (allocated or freed `Live_JobsTimedOut` cells and `Live_HangLog` text in each
   test's `pre/SYSTEM`): T70 (0163, flags) 0; **T71 (0167) 10**; T72 (T61) 0; T74 (T61) 0 (62 conformance faults);
   **T78 (0167) 404**; T79 (0167, 33 min of Geometry Dash on the flip path) 0. No boot on the T61 sbumd ran a
   XAML app beyond the shell, so "T61 does not do it" is not established.
4. **Calculator was never closed in T78** [M] (the script looked for `CalculatorApp.exe`; `Calculator.exe` pid 2504
   is still in `tasks-apps-end.csv`). The storm lasted from its start to the end of the session. [M] It paused only
   while sbsmooth's full-screen window covered the desktop (61.7 fps, 3.8 jobs per present) and was back for the
   windowed test (0.5 fps).
5. **The panel was not black.** [M] `Live_FbLiveSecs` 538 of `Live_Seconds` 585: in 538 seconds at least 8 of the
   16 sampled framebuffer points were not black. The black shots are a capture result, not what the panel showed.

## 2. The data, in one place

- [M] T78 KMD counters at the end (allocated cells of `pre/SYSTEM`): JobsTimedOut 404, JobsFailed 404, GpuResets
  404, FaultCount 404 with **FaultsInLive 0, FaultsInNone 0, LastMmuFault 0** (not one MMU fault in the session),
  TdrEngineResets 0, Fail1Ms 190940, Fail1Submits 19451 of 27361, JobsDone 42364, Maps 10151, Unmaps 9981,
  ScratchMaps 9981 (ScratchUnmap 1), AllocMB 240 (peak 741 of 1536), AllocFails 0, PresentWaitTimeouts 11,
  BltSrcShadow 259 -> BltDstPrimary 259, BltSrcOther 1429 -> BltDstOther 1429 (dwm.exe -> a 1440x2960 surface
  made by sblinkd), BltDstShadow 0, BltDstStaging 0.
- [M] Every hang record: slot 1, `chain 1/2` (once 1/1), js 0x08 (ACTIVE), `as0 0x00000000@0x0`, `mmuraw 0x0`,
  `gfault 0`, JS_TAIL = the job just before JS_HEAD. The hung job in the 31 recovered T78 records is always the same draw: Invocation 0x3b
  (60 vertices), 144 indices of 16 bits, triangles, implicit restart, IDVS with a secondary (varying) shader
  (Primitive word 0x18178208), at chain position 6-20; base vertex offset negative (-68 ... -2219: a dynamic vertex
  buffer filled upwards); the index pointer walks through a dynamic buffer (+0xed6 per frame, new buffer every few
  frames). T71's: 16-28 vertices, 42-78 indices, position 4-5.
- [M] Memory was not short in T78: RAM free 836-1352 MiB, commit free 1.6-2.1 GB at every shot. (T71: memory load
  83-86 %, C: 232 MiB.)
- [M] DWM's log in the storm: six 10 s windows whose longest wait is 9999.5-10000.9 ms, i.e. waits that gave up
  at the shim's 10 s cap; 100-650 new CPU maps per window (before: 2-5).
- [M] No sbumd log exists for `Calculator.exe`, `ApplicationFrameHost.exe`, `Notepad.exe` or Edge's GPU process;
  DWM logged no `OpenResource` between Settings' two surfaces (before 02:50:13) and Paint's.
- [R] `command.c:1580-1634` (Reap): at the limit the chain is hard-stopped, **the rest of the submission is
  dropped** (`Queue[QHead].Next = Count`), the GPU is reset and the fence is reported completed.
- [R] `command.c:1955`: a present waits at most 200 ms for the queue to drain, then copies anyway.
- [R] `malikm_drm.c` (0167): `wddm_idle` = one wait of at most 10 s; `GEM_CLOSE` of a resource's allocation called
  it, ignored the result and destroyed the allocation.
- [R] The KMD never reads `WriteOperation` (no occurrence in `starbite/win/sbwddm`). What 0167 changes is seen
  only by dxgkrnl / VidMm.
- [R] `pan_jm.c:156-159`, `jm_emit_tiler_desc`: every batch restarts the tiler heap at its bottom; the heap is
  named (written) by every submit that has a tiler job. DWM's heap is one 64 MiB allocation, made in full.

## 3. (1) the timeouts: hypotheses, ranked

**H1. One particular DWM draw never finishes in the vertex/tiler stage, and DWM repeats it every frame while a
certain window is on screen (Calculator in T78, the sign-out screen in T71).** Most likely. [I]
- For: the onset times above [M]; the same geometry in all 31 recovered T78 hang records (of 404), at a stable chain position
  [M]; no hang while one full-screen window is all DWM draws [M]; the storm lasts exactly as long as Calculator
  stays [M]; the earlier hangs of this signature came with XAML/acrylic shell surfaces (6.50: Quick Settings,
  Notification Centre) [M, from the log].
- What it does not say is *why* the job does not finish. Three sub-cases, which one hang record of the proposed
  KMD patch tells apart:
  - **H1a: the job touches an address with no page** (an allocation destroyed while still referenced, or never
    made resident). Precedent: T46-T48, where this GPU's IDVS jobs stalled silently and scratch-mapping evictions
    ended it (6.58) [M then, I now]. Against: not a single MMU fault in the session, although fragment jobs of the
    same frames would fault on a missing page as they did in T46 [M].
  - **H1b: the job writes an allocation that is on the read-only scratch page** (evicted because the submission
    did not name it): the tiler heap, the position/varying buffers or the tiler context. [R] they are named by
    Panfrost, so this needs a listing gap (a full 256-entry list, or a batch whose heap entry was lost). Against:
    a write to a read-only page should raise a permission fault, and none was seen [M]; no memory pressure [M].
  - **H1c: memory is fine and the shader or the tiler does not terminate** for this draw (a vertex shader the old
    TGSI translator compiled wrongly, or tiler input it cannot handle). For: it explains "ACTIVE, no fault"
    without any assumption about faults; 6.99 already lists shader classes the old translator gets wrong.
    Against: nothing measured yet names the shader.
- The data cannot distinguish H1a/b/c: a hang record holds 64 bytes of the job and no page-table state for
  anything the job points at.

**H2. 0167 itself (opened allocations listed read-only) causes it.** Possible, not shown. [I]
- For: both boots with hangs ran 0167 [M].
- Against: T79 ran 0167 for 33 min with 0 [M]; the T61 boots never opened a XAML app, so they are no control
  [M]; [R] the change touches only the write bit of opened allocations, which the KMD ignores, and [I] a
  vertex/tiler job reads no surface of another process. 0159 (which first makes DWM compose app swap chains and
  DirectComposition content at all) is the bigger behavioural change between T61 and the candidate.
- Decides it: T80b (the user's T61 sbumd, same script). Timeouts there = not 0167/0159.

**H3. Once jobs time out, our own recovery keeps the storm going.** Contributes at most. [I]
- For: [R] the dropped rest of a submission leaves DWM's targets undrawn; [M] 10 s waits gave up; [R] a resource's
  allocation was then destroyed with jobs possibly still queued, which is H1a's precondition.
- Against as the origin: the first 16 hangs are consecutive from the first second, before any 10 s wait could
  expire [M]; T71's 10 hangs [M].
- Fixed in sbumd b78 (section 5): no destroy after a wait that gave up.

**H4. Memory pressure / eviction as in T46.** Unlikely in T78 [M: free memory above; scratch page on]. Not
excluded for T71.

**H5. Edge's aborting GPU process leaves something in DWM's scene.** Unlikely: the timing in section 1 [M].

**H6. The 256-entry allocation list overflows** (DWM had BO slots up to 390 [M]; [R] the forced listing of opened
allocations and the every-64th refresh stop silently when the list is full). Unknown; sbumd b78 counts it.

**H7. An MMU command left the address space locked** ([R] `FlushRange` locks a region for every map, unmap and
scratch-map while jobs run; a lost FLUSH_PT would stall every access to that region without a fault). No
evidence; the KMD patch records AS_STATUS and AS_LOCKADDR at the hang.

## 4. (2) the black screen shots

**They are a consequence of (1).** [I] from [R]+[M]:
- [M] The shots taken before the first hang on the same driver are fine (Edge 02:49:36: 0 % near-black; Settings
  02:50:35: 0 %). Every shot after it is black or 91 % black. So the capture path itself works on 0167.
- [M] No present blt went to a GDI shadow or staging surface (BltDstShadow 0, BltDstStaging 0), so the screen read
  is DWM rendering the area and reading it back in user mode, not a KMD copy. [I]
- [R] When DWM's frame contains the hanging draw, the KMD drops the rest of that submission, including its
  fragment chain, and reports it complete. The target DWM reads back was never drawn: new pages, zeros = black.
  Paint and Task Manager (91 %) are frames of which some batches ran. [I]
- [M] DWM's own dumps show the windows because they read DWM's back buffer, which keeps what earlier frames drew.
- Cannot be distinguished from the data: whether a capture also ran into the shim's 10 s wait cap ([M] such
  waits exist) and read before the GPU got there. Same root either way.
- "Some content areas are dark" in DWM's dumps (Explorer's file list) is the same dropping of work. [I]

## 5. What was changed

### sbumd b78 (compiled only, never run)
- VM: `~/sbmesa/src-b78`, branch `sb-b78`, commit `e2037d3` on `b8730f0` (0167). Files:
  `include/win32-compat/malikm_drm.c`, `src/gallium/drivers/panfrost/pan_jm.c`. Patch copy:
  `build/win/patchwork/b78-umd.patch`.
- Built in new directories (`build-b78-a64`, `build-b78-ec`, `build-b78-x`; script `~/sbmesa/b78/build-b78.sh`,
  same meson options, cross files and link response file as the 0167 build). ARM64X verify FAILS: 0;
  `check-init-tables.py`: 0 C++ initializers, INIT-TABLE FAILS: 0.
- `build/win/umd-b78/sbumd.dll` (sha256 57502837 3043e206...) and `sbumd.map`.
- What it does:
  1. **Diagnostics (always on, log only):**
     - each 10 s stats block gets `sbumd: stats at HH:MM:SS.mmm; allocation list max N of 256, ...`;
     - `sbumd: HH:MM:SS.mmm KMD job timeouts N (+d), failed jobs ..., seen at <the 10 s tick | a long wait | a
       wait that gave up>` when the KMD's counter moved, plus the device's submissions, live BOs and what its last
       allocation list held;
     - `opened allocation ... at HH:MM:SS.mmm` and `closed opened allocation 0x... ` with the clock;
     - `allocation list full: 256 of 256 entries ..., N more BOs left out`;
     - `a wait for submission N gave up after 10000 ms`;
     - **draw trace:** after a KMD timeout that this process itself waited behind (a wait of 300 ms or more), the
       next 96 chains log `chain jc 0x...` and, per draw, `draw job 0x<GPU address> idvs index N: vs <ptr> (code
       0x...), V vertices from F, ... I indices at 0x..., target WxH`. At most three times per process. The KMD's
       `Live_HangLog` gives `head 0x...`: the draw line with that address is the hung draw.
  2. **One fix:** `GEM_CLOSE` of a resource's allocation no longer destroys it after an idle wait that gave up;
     it waits on, up to 60 s, logging each round. Flag file `C:\ProgramData\Starbite\b78nowait` = old behaviour.
     Cost: a thread that destroys a resource during a storm can block up to 60 s instead of 10. It does not fix
     the hang's cause.
  3. Flag file `listall` = `SB_LIST_ALL=1` (environment variables never reached DWM).
- Not done on purpose: no change to what is submitted or listed. The cause is not known, and T48 showed that
  list-all makes this signature more frequent, not less.

### Kernel driver: `build/win/patchwork/b78-kmd.diff` (not applied, not compiled)
`git apply --check` passes against the worktree. 13 hunks:
- `maliwddm.h` (3 hunks): `MW_HANG_PTR` and the new `MW_HANG` fields; `HalfHead`, `HalfHeadFor`, `VtTimeoutMs` in
  the adapter.
- `command.c`:
  - `MW_CHAIN_LIMIT_MS(A, Slot)`: slot 1's limit from `Parameters\VtTimeoutMs` (50..749; default 750 = today's
    behaviour), used in `HangViewMake`, `Reap` and `MwQueryEngineStatus`. **Mitigation only:** at 150 ms a hang
    costs a fifth of the time, the frame with the hanging draw is still dropped, and a legitimately slow
    vertex/tiler chain would be cut too. Leave it at 750 for the diagnostic run.
  - `HangPointers` (new) and its call in `HangCapture`: reads the job as far as JS_HEAD's page goes (up to 384
    bytes, same view and checks as today's 64), takes the 20 pointers of an Indexed Vertex Job (next, indices,
    tiler, the fragment draw's 13, the vertex draw's state/ubos/push/tls), and records for each the PTE state
    (none / page / scratch), the PTE's low 12 bits (0x2C1 read-write, 0x241 read-only), the owning live
    allocation (creator's initial, pages, mapped / scratch, last unmap, renders since it was last listed). One
    walk of the live list also counts the creator's allocations (live, mapped, scratch, listed by the hung
    submission) and describes its biggest one of 16 MiB or more: for DWM the tiler heap.
  - `HangCapture`: AS0 status, lock address and TRANSTAB, MMU interrupt mask, raw job interrupt status.
  - `Reap`: JS_HEAD sampled once at half the limit.
- `adapter.c`: `MwHangPtrLog` writes the newest three records as `Live_HangPtrs` and, while there are at most
  three hangs, also as `Live_HangPtrsFirst` (the storm's first ones stay); `VtTimeoutMs` read and logged.
- Cost: at a hang only (one list walk at interrupt level, as `FaultRecord` already does); 8 x ~0.9 KiB more
  non-paged memory.

## 6. What the next run shows

| In the next run | Means |
|---|---|
| `Live_HangPtrs`: a pointer with `[0 000 none]`, or owner present but `-` (not mapped) | H1a: the named field points at no page. Which field = which buffer |
| a pointer with `[S 241 ...]`, or `big ... scratch 1 pte S/S/S listed N ago` with N > 0 | H1b: evicted allocation in use (big = the tiler heap) |
| every pointer `[1 2c1 d +Np M ... 0]`, `big ... mapped 1 pte 1/1/1 listed 0 ago` | H1c: memory is fine; the draw trace names the shader |
| `half-head` = `head` | one job is stuck. Different: the chain is slow, not stuck (then look at clocks/thermal, not this job) |
| `as` bit 0 set, or `lock` covering the job's addresses | H7 |
| `mmumask 0x0` | MMU interrupts are masked: "no MMU fault" then proves nothing |
| dwm log: first `KMD job timeouts` line, with the `opened` / `closed opened allocation` lines before it and the step times in gputest.log | what DWM had just been given or had just let go of; the onset to the millisecond |
| dwm log: `draw job 0x<head>` | the hung draw: its vertex shader, vertex and index counts, target size. Same `vs` every time = H1c's shader |
| dwm log: `allocation list full` | H6 |
| dwm log: `not idle after N s, before a resource's allocation is destroyed` | H3's path was reached (and is now waited out) |
| T80b (T61 sbumd): `job timeouts` > 0 after Calculator | not caused by 0159-0167 (H2 refuted); 0 with Calculator shown and composed = H2 stands |
| timeouts stop within seconds of Calculator's kill (sb-apps2 kills `Calculator.exe`) and no later step adds any | H1 (that window's draw), H3 not self-sustaining |
| a boot with the flag file `noidvs` (present before DWM starts): no timeouts, or failures with an MMU address / another job type | the IDVS path is necessary; a `vertex` vs `tiler` job type says which half hangs |

## 7. Additions to `starbite/win/autotest/sb-apps2.bat`

1. At the start, tie the three clocks together (today this had to be inferred): `echo` the wall time, the tick
   count (`powershell [Environment]::TickCount`) and `reg query "%K%" /v Live_Seconds` in one line; again at the end.
2. Call `:stat` also 5 s and 12 s after each `start`, not only after the kill, so the onset is bracketed inside a
   step (app start against app kill).
3. In `:stat`, when the timeout count grew, append `Live_HangLog`, `Live_HangPtrs`, `Live_HangPtrsFirst` and
   `Live_FaultLog` (`reg query "%K%" /v ...`) to `umdlog\hang-<step>.txt`: the rings hold 8 and 3 records.
4. One more step after Task Manager: start Calculator, `:stat` at 5, 12 and 20 s **without killing it**, start
   Notepad on top, `:stat`, run `sbshow snap` twice 10 s apart, then kill Calculator and `:stat` 5 and 15 s later.
   This is T78's situation on purpose.
5. Before and after that step: one camera frame of the panel (Photo Booth on the Mac), to have the panel and the
   capture side by side once.
6. Keep `tasklist /v` once while Calculator is up (is `ApplicationFrameHost.exe` there, which processes have no
   umdlog).
7. Runs: (a) candidate KMD with b78-kmd.diff + sbumd b78, no flags; (b) the same with `noidvs`; (c) T80b as planned.

## 8. Side findings (not the cause, worth knowing)

- [I] Flag files under `C:\ProgramData\Starbite` and the umdlog are probably invisible to AppContainer and
  sandboxed processes (Calculator, Edge's GPU process: no log [M]). There `noafbc` reads as absent, so AFBC would
  be on in exactly those processes while every test assumes it is off. Not verified. A registry value under the
  adapter key read through the existing escape would reach every process.
- [R] `MK_PTE_XN` is defined as 0 (`common/maligpu.h:139`): the no-execute request of an allocation has no effect.
- [R] A present copies after 200 ms whether or not the queue drained (`PresentWaitTimeouts` 11 [M]): during a hang
  a blt present can copy a source that was not drawn yet.
- [R] The forced listing of opened allocations (0138) and the refresh are cut off without a trace when the list
  has 256 entries. b78 now logs it.
