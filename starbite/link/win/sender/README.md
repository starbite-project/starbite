# sblinkd: the Starbite Link sender (phone side)

This is the phone's half of the DeX-like experience. The phone's Windows desktop appears in the Mac app, ideally
as an extra monitor ("Starbite Display", the IddCx virtual monitor) that Windows extends onto, and the Mac's
keyboard and mouse drive Windows. sblinkd speaks SBLINK v1 (`../../PROTOCOL.md`) as the **source**. It:

- captures the Starbite Display, or the primary screen when that display is missing, using DXGI Desktop
  Duplication, with GDI BitBlt as the fallback
- sends only the areas that changed (solid fill, LZ4 or raw), with at most 2 frames in flight and at most 30 fps
- injects the Mac's input with SendInput
- talks over USB (GenericUsbFn) and/or TCP (port 47474)

## Files

| File | What |
|---|---|
| `sender.h` | All interfaces: region, encoder, flow control, input mapping, capture, links, the session |
| `session.c` | The SBLINK session (HELLO, ACK, MODE, PING, BYE, input dispatch, BUSY, dead link) and the capture/send loop |
| `pipeline.c` | The dirty-region accumulator, the frame encoder (core `sblk_dirty_rects` / `split_rect` / `build_rect`) and flow control |
| `input_map.c` | HID usage to set-1 scan code, consumer keys to VK, absolute pointer mapping, key/button state, UTF-8 text |
| `tcp.c` | TCP listener (Winsock / BSD sockets). One viewer; a second one gets `BYE(BUSY)` |
| `plat.c/.h` | Mutex, event, thread, clock and log (Win32 and POSIX) |
| `main.c` | Options, logging, single instance (`Global\StarbiteLinkSender`), DPI awareness, `BYE(SHUTDOWN)` on logoff/shutdown/sleep, the watchdog thread, the session-helper mode |
| `service_win.c` | `--service`: the StarbiteLink LocalSystem service that runs the sender as SYSTEM in the console session, never-sleep power settings, its own crash recovery |
| `capture_win.c` | DXGI duplication: output choice, dirty and move rects, system-memory or staging copy, GDI fallback, MODE via ChangeDisplaySettingsEx |
| `inject_win.c` | SendInput; every thread that injects or captures follows the input desktop (Default / Winlogon) |
| `usbfn_win.c` | GenericUsbFn: interface discovery, ACTIVATE_USB_BUS, bus events, bulk transfers with the ZLP rule, bounded cancels, re-activation of the USB function after a stuck transfer |
| `capture_synth.c`, `inject_rec.c`, `host.h` | Mac host build: a synthetic desktop and an injector that records |
| `test_host.c` | Host tests: unit tests, plus end-to-end tests over TCP loopback against the real session code |
| `build.sh` | Builds and runs everything (below) |

## Build

```sh
starbite/link/win/sender/build.sh            # host tests (ASan+UBSan, then -O2) + sblinkd-host + sblinkd.exe, runs the tests
starbite/link/win/sender/build.sh --no-run   # build only;  --host-only / --win-only
```

Output goes to `build/link/sender/`:

- `test_host-san` and `test_host`: the host tests
- `sblinkd-host`: the Mac build
- `sblinkd.exe`: Windows 11 ARM64, built with `tools/win/build.sh exe` and the WINDOWS subsystem (no console
  window at logon), signed with `tools/win/sign.sh`. Its PE is checked: it must be ARM64 and WINDOWS_GUI, and it
  may import only system DLLs (kernel32, user32, gdi32, d3d11, dxgi, ws2_32, cfgmgr32, advapi32, powrprof) and the
  UCRT (`api-ms-win-crt-*`).
- `sblinkd.exe.sha256`

## Develop the Mac app against it now (no phone needed)

```sh
build/link/sender/sblinkd-host --synth-auto 30 --print-input     # TCP 127.0.0.1:47474, 1280x720 synthetic "Starbite Display"
```

This is the real session, encoder, flow control and TCP code. The only differences from the phone are:

- capture is a synthetic desktop: a taskbar, a window, a moving box and a counter, with OS-style dirty rects
- input is logged instead of injected

It announces EXTEND and MODE, applies MODE requests (any even size from 64x64 to 4096x2304), and answers PING.
Options: `--synth-size WxH` and `--synth-auto FPS` (0 = static).

## Run on the phone

Copy `sblinkd.exe` to `C:\starbite\link\sblinkd.exe`. There are two ways to run it:

- **As the StarbiteLink service (recommended).** Input then reaches every window, including Control Center
  (quick settings), Start, elevated windows, UAC prompts and the lock and logon screen. See "Service mode".
- **As the logged-on user** from the Startup folder or the Run key (the fallback). No admin rights are needed,
  but Windows' UIPI blocks clicks into elevated and UIAccess windows, and the secure desktop is out of reach.
  If the service is running, this instance notices it and exits, so leaving both installed is fine.

```
sblinkd.exe                     USB only (the default)
sblinkd.exe --tcp               also TCP 47474 on 127.0.0.1
sblinkd.exe --tcp --bind 0.0.0.0   TCP from the network: NO AUTHENTICATION, whoever connects gets keyboard and mouse
sblinkd.exe --console -v        log to a console as well, one line per frame and key
sblinkd.exe --output primary    capture the phone's own screen even when the Starbite Display exists
sblinkd.exe --gdi               force the GDI capture path
sblinkd.exe --diag              log every OS frame: accumulated frames, move/dirty rect counts, their bounding box
sblinkd.exe --stall-ms N        forget frames unacknowledged for N ms and resend a keyframe (default 3000)
sblinkd.exe --no-keep-awake     do not keep Windows awake while the Mac is connected
```

- Log: `C:\starbite\link\sblinkd.log` (rotated to `.old` at 8 MB). If `C:\starbite` cannot be created, it goes to
  `%LOCALAPPDATA%\Starbite\Link\`.
- Every 10 s it logs a stats line: fps, KiB/s, ms of encoding per frame, rects per frame, how often flow
  control blocked, stall timeouts, and what DXGI handed over in the last 10 s (`os` frames acquired, `acc`
  frames DWM presented, `ptr` pointer-only frames, `move`/`dirty` rects, `full` whole-image copies, `no-meta`
  presents without metadata).
- Only one instance runs on the machine (mutex `Global\StarbiteLinkSender`).
- While a Mac is connected it calls `SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED |
  ES_DISPLAY_REQUIRED)` and releases it when the Mac goes (logged as `power: ...`). This phone's Windows has no
  power button or wake driver, so once it sleeps it does not come back.

### Service mode (LocalSystem): input everywhere

**Why a service and not `uiAccess=true`.** UIPI blocks `SendInput` into windows of a higher integrity level, and
the shell's flyouts are UIAccess windows in a higher z-band. A `uiAccess=true` manifest would lift UIPI, but:

- it only works for a binary signed by a certificate in the machine's trusted root store; testsigning does not
  relax this for user-mode Authenticode
- the binary has to live in a secure location such as `C:\Program Files`
- it still runs only after logon
- it cannot open or capture the secure desktop (UAC prompts, the lock and logon screen)

A SYSTEM process in the console session has none of these limits. This is how remote desktop tools work.
DXGI duplicates the secure desktop only for LocalSystem.

**How it works.** `sblinkd.exe --service` is a real service EXE (`StartServiceCtrlDispatcher`), not cmd.exe.
Session 0 has no desktop, so the service only supervises:

- It starts `sblinkd.exe --session-helper --quit-event H [ARGS]` in the active console session, using a copy of
  its own SYSTEM token with `TokenSessionId` set (`WTSGetActiveConsoleSessionId`, `DuplicateTokenEx`,
  `CreateProcessAsUser` on `winsta0\default`).
- If the helper exits, the service restarts it with a backoff of 1 s, doubling to 30 s. Exit code 3 (a
  watchdog restart) gets an immediate restart.
- On a session change (logon, logoff, fast user switching) it moves the helper to the new console session.
- When it stops, or Windows shuts down, it asks the helper to send `BYE(SHUTDOWN)` first.

The helper's capture loop and receive threads call `SetThreadDesktop(OpenInputDesktop())` whenever the input
desktop changes, so the Mac keeps working on the logon screen and on UAC prompts. As SYSTEM the helper ignores
`--tcp`, because any local program could then click "Yes" on UAC; `--unsafe-tcp` overrides this.

At every start the service also:

- sets the active power scheme's sleep, hibernate, unattended sleep, display off, dim, and lock-screen display
  off timeouts to 0 ("never") on AC and DC, and sets `HibernateEnabled` to 0 (`--keep-power-settings` skips
  this)
- registers its own crash recovery: restart after 5 s, twice, then after 30 s

The service logs to `C:\starbite\link\sblinkd-service.log`. The helper logs to `sblinkd.log` as usual; its start
line says `service session helper (SYSTEM), session N`.

**Offline install, from TWRP.** This follows the same preconditions as `tools/win/README.md`: Windows was shut
down cleanly, and the hive copy is clean.

1. Copy `build/link/sender/sblinkd.exe` to `C:\starbite\link\sblinkd.exe`. The Startup-folder copy may stay;
   it exits when the service runs.
2. Add the service to the pulled SYSTEM hive:
   ```sh
   tools/win/hive-service.py build/win/hive/SYSTEM.pulled build/win/hive/SYSTEM.svc add StarbiteLink \
       --type 16 --start 2 --object-name LocalSystem --oneshot 0 --display "Starbite Link" \
       --image '"C:\starbite\link\sblinkd.exe" --service'
   ```
   This writes `Services\StarbiteLink`:

   | Value | Type | Data |
   |---|---|---|
   | `Type` | DWORD | 0x10 (own process) |
   | `Start` | DWORD | 2 (auto) |
   | `ErrorControl` | DWORD | 0 |
   | `ImagePath` | EXPAND_SZ | `"C:\starbite\link\sblinkd.exe" --service` |
   | `DisplayName` | SZ | `Starbite Link` |
   | `ObjectName` | SZ | `LocalSystem` |
   | `Parameters\OneShot` | DWORD | 0 (unused) |

   Helper options go after `--service`, for example `... --service --diag`.
   This was rehearsed on a copy of `build/win/results-link5/SYSTEM`.
3. Optionally, and recommended, apply the offline power recipe below to the same hive. The service applies the
   same settings at its first start anyway.
4. Push the hive back and boot. Expected results:
   - `sblinkd-service.log` shows `service: helper pid N started in session 1`
   - `sblinkd.log` shows the SYSTEM helper starting, followed by `desktop: thread ... now on the "Winlogon"
     desktop` at the logon screen
   - Clicks then work in Control Center and on UAC prompts.
5. Undo: `hive-service.py IN OUT disable StarbiteLink` (Start = 4) or `remove StarbiteLink`. The Startup-folder
   instance takes over at the next logon.

### Never sleep: the offline registry recipe

On this phone, the active power scheme is Balanced (`381b4222-f694-41f0-9685-ff5bb260df2e`). It has no per-user
override for these settings, so the scheme defaults apply. Set them to 0 (never) with `hive-service.py setkey`.
Every key already exists; this was rehearsed on a copy of `results-link5/SYSTEM`. Each `setkey` call writes a
new file, so chain them: IN, then OUT1, then OUT2, and so on.

```sh
B='Control\Power\PowerSettings'; S=381b4222-f694-41f0-9685-ff5bb260df2e
for k in 238c9fa8-0aad-41ed-83f4-97be242c8f20\\29f6c1db-86da-48c5-9fdb-f2b67b1f44da \
         238c9fa8-0aad-41ed-83f4-97be242c8f20\\9d7815a6-7ee4-497e-8888-515a05f02364 \
         238c9fa8-0aad-41ed-83f4-97be242c8f20\\7bc4a2f9-d8fc-4469-b07b-33eb785aaca0 \
         7516b95f-f776-4464-8c53-06167f40cc99\\3c0bc021-c8a8-4e07-a973-6b14cbcb2b7e \
         7516b95f-f776-4464-8c53-06167f40cc99\\17aaa29b-8b43-4b94-aafe-35f64daaf1ee \
         7516b95f-f776-4464-8c53-06167f40cc99\\8ec4b3a5-6868-48c2-be75-4f3044be88a7; do
  tools/win/hive-service.py IN OUT setkey "$B\\$k\\DefaultPowerSchemeValues\\$S" \
      --set ACSettingIndex=0,DCSettingIndex=0,ProvAcSettingIndex=0,ProvDcSettingIndex=0
done
tools/win/hive-service.py IN OUT setkey 'Control\Power' --set HibernateEnabled=0,HibernateEnabledDefault=0
```

| Setting (subgroup \ setting) | What | Before (AC / DC) |
|---|---|---|
| SLEEP `238c9fa8...` \ `29f6c1db...` | sleep after | 1800 / 900 s |
| SLEEP \ `9d7815a6...` | hibernate after | 0 / 0 (Prov DC 0x7fffffff) |
| SLEEP \ `7bc4a2f9...` | unattended sleep timeout | 120 / 120 s |
| VIDEO `7516b95f...` \ `3c0bc021...` | display off after | 600 / 300 s |
| VIDEO \ `17aaa29b...` | dim display after | 585 / 285 s |
| VIDEO \ `8ec4b3a5...` | display off on the lock screen | 60 / 60 s (Prov 30 / 30) |

Each value goes into `Control\Power\PowerSettings\<subgroup>\<setting>\DefaultPowerSchemeValues\381b4222-...` as
`ACSettingIndex`, `DCSettingIndex`, `ProvAcSettingIndex` and `ProvDcSettingIndex`, all DWORD 0. On top of these,
`Control\Power` gets `HibernateEnabled` = 0 and `HibernateEnabledDefault` = 0.

If the user later changes these settings in the Settings app, Windows writes per-user overrides under
`Control\Power\User\PowerSchemes\381b4222...\<subgroup>\<setting>`, and those win over the defaults above. The
service's start-up step writes exactly such overrides with `PowerWriteAC/DCValueIndex`, which covers this case.

### Recovery: what happens when the Mac goes away

Found on the S9 on 2026-11-07, and fixed:

- **Flow control froze the picture.** The Mac ACKed a frame before `l->send()` had returned. The ACK was
  "for a frame not sent" and was ignored, the frame stayed in flight forever, and after two such frames
  nothing was sent again ("9 frames in 118 s"). Frames are now recorded as in flight *before* the send.
- **Frame ids carry on across sessions.** An ACK from an earlier session can no longer match a new frame.
- **Stall timeout.** If frames stay unacknowledged for `--stall-ms` (3 s) while the link is alive, they are
  forgotten and a keyframe follows. This is logged as `flow: N frames (a..b) unacknowledged ...`.
- **False "link dead".** The dead-link test read `now` before `last_rx`, and an ACK in between wrapped
  `now - last_rx` around ("link dead" 16 ms after a HELLO). It is re-read now, and checked again under the locks.
- **HELLO during a hung send.** A send that failed because the Mac had stopped reading set `hunting` *after*
  the new Mac's HELLO had been accepted, so every later message was ignored. `HELLO` now clears `hunting`.
- **The receive thread never waits on a send.** A PONG goes out at once when the transmit lock is free;
  otherwise it is queued for the capture loop. A new HELLO is therefore read even while a frame send hangs.
- **USB: no unbounded waits on the driver.** An IN transfer that times out after 5 s (the Mac app died) or
  fails, or an OUT transfer that fails, leads to a full re-activation of the USB function: cancel everything,
  `DEACTIVATE_USB_BUS` (the Mac sees an unplug), reopen, `ACTIVATE_USB_BUS`. The Mac app then reopens the
  interface. Before this fix, after such a cancel the phone never completed another OUT transfer, and the
  new Mac app got "link dead" forever. A request that does not complete within 3 s of its cancel is leaked,
  never reused, and leads to a process restart.
- **Watchdog.** A thread checks every 2 s for a send that has not returned for 20 s, a USB receive loop that
  has not turned for 20 s, a capture loop that has not turned for 30 s, or a wedged driver request. If it
  finds one, it restarts the process. A helper exits with code 3 and the service starts a new one at once;
  a Startup-folder instance starts a copy of itself.
- **Mac side.** After its HELLO, the app skips IN transfers until one starts with the phone's HELLO (or BYE).
  Leftover frames from the old session are dropped instead of causing "message before HELLO" and yet another
  restart.

`test_host` checks all of this with a USB-style link whose viewer ACKs before `send()` returns:

- ACKs racing the send
- dropped ACKs
- the Mac app killed mid-session and a new app with a cut-off message first
- a HELLO during a hung send
- queued PONGs
- keep-awake
- the watchdog

Against the old ordering, the first scenario delivered 6 frames in 1.5 s instead of 60 or more.

### Start at logon (choose one)

1. **Startup folder** (no admin). Run this once as the user:
   ```
   powershell -c "$s=(New-Object -ComObject WScript.Shell).CreateShortcut(\"$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Startup\Starbite Link.lnk\"); $s.TargetPath='C:\starbite\link\sblinkd.exe'; $s.Save()"
   ```
   To do the same offline, the per-user Run key `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, value
   `StarbiteLink` = `"C:\starbite\link\sblinkd.exe"` in the user's `NTUSER.DAT`, has the same effect.
2. **Scheduled task**, which runs in the user's session with normal rights:
   ```
   schtasks /Create /TN "Starbite Link" /TR "C:\starbite\link\sblinkd.exe" /SC ONLOGON /RL LIMITED /IT /F
   ```
   Creating an ONLOGON task needs an elevated prompt once. The one-shot install service in `tools/win/README.md`
   could run it. The task then starts unelevated at every logon. One advantage over the Startup folder:
   `schtasks /Run /TN "Starbite Link"` restarts it.

Stopping it: Task Manager, or `taskkill /IM sblinkd.exe`. That kill is not graceful; on logoff, shutdown and
sleep, the hidden window sends `BYE(SHUTDOWN)` first.

## How it works (the CPU budget: 4 A55 cores, software rendering)

- **No viewer, no work.** Without a session the capture is never polled. DXGI accumulates dirty rects by itself,
  so an idle sblinkd only waits on events.
- **Flow control.** With 2 frames unacknowledged, the loop does not call AcquireNextFrame at all (the host test
  checks that no captures happen while blocked). The next frame then carries everything that changed meanwhile.
- **Dirty rects only.** For each OS frame, only the dirty and move-destination rects are copied out of the
  desktop image. If the image is in system memory (`DesktopImageInSystemMemory`, expected with the Basic
  Render/WARP adapter) the copy comes through MapDesktopSurface; otherwise through a staging texture. Only those
  rects are compared, in 64x64 tiles, against the shadow of what the viewer has. A frame where nothing really
  changed sends nothing.
- **Encoding.** Each changed area becomes full-width strips under the viewer's max_payload. Each strip is sent
  as solid, then LZ4 if that saves at least 1/8, else raw. A whole frame goes out as one batch: one TCP send, or
  256 KiB USB transfers, with a ZLP when a transfer is a multiple of 512 bytes.
- **Frame rate.** At most `--fps` (30) frames per second, and frames are only produced on OS frames.
- **GDI fallback.** GDI has no dirty rects, so it polls at `--gdi-fps` (10) and compares the whole screen.
  It costs more and is only for when duplication fails.
- **Input.** Input is injected directly on the receive thread, so it never waits for capture or encoding.
  - Keys are sent by scan code from the HID position, so the Windows keyboard layout decides the character.
    The HID Power usage is deliberately not mapped.
  - Pause and the media/volume keys are sent by virtual key.
  - Text is sent as KEYEVENTF_UNICODE (`\n` becomes Enter, `\t` becomes Tab).
  - The pointer is absolute over the virtual desktop and mapped onto the captured output. The mapping lands on
    the right pixel whether Windows truncates or rounds (tested for every pixel).
  - Modifiers are resynchronised from the viewer's modifier state. Everything held down is released on session
    end, BYE, a new HELLO or a dead link.
- **MODE.** Only the Starbite Display is ever resized, never the phone's own panel. The request goes to the
  nearest mode the IddCx monitor lists, applied with ChangeDisplaySettingsEx (dynamic, not saved). APPLIED goes
  out after the capture re-opens at the new size, followed by a keyframe.

## Verified, and not

Verified on the Mac (`build.sh`, 2026-11-07): the host tests pass under ASan+UBSan and at -O2. An independent
Python client also interoperated with `sblinkd-host`. `sblinkd.exe` builds with 0 warnings at /W4, is signed,
and passes the PE check.

**Can only be verified on the phone** (nothing here has run on Windows):

- service mode: the helper starting as SYSTEM in session 1, input into Control Center, Start, elevated windows
  and UAC; capture and input on the Winlogon desktop; the move on logoff/logon; the power settings applied
- the USB re-activation after an IN timeout: does the Mac see the unplug/replug, and does the next session work
- the quick settings / Control Center picture with alpha forced to 0xFF (run once with `--diag`)
- DXGI duplication on the S9's adapter: Basic Display/WARP, and IddCx outputs
- whether the frames arrive in system memory
- the GDI fallback
- the friendly name match for "Starbite Display"
- SendInput behaviour: scan codes, NumLock (0x45) and Pause by VK, the wheel and the absolute mapping
- DPI awareness
- `BYE(SHUTDOWN)` on logoff and sleep
- every GenericUsbFn call: whether `GET_CLASS_INFO` works at Configured, the IOCTL buffer conventions (pipe id
  in, data out), the setup-packet handshake choice, and the maximum transfer size (`--usb-chunk`)
- the Startup and scheduled task instructions above
- the CPU cost on the A55s

Not done:

- the Windows cursor shape is not sent (the protocol has no message for it; the Mac shows its own cursor)
- Caps Lock and Num Lock state are not synchronised
- rotated outputs are sent unrotated (logged)
- in the Startup-folder mode, no input into elevated or UIAccess windows or the secure desktop (UIPI); use the
  service mode
- TCP has no authentication; it binds to 127.0.0.1 unless `--bind` is given
