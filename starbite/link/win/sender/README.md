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
| `main.c` | Options, logging, single instance, DPI awareness, `BYE(SHUTDOWN)` on logoff/shutdown/sleep |
| `capture_win.c` | DXGI duplication: output choice, dirty and move rects, system-memory or staging copy, GDI fallback, MODE via ChangeDisplaySettingsEx |
| `inject_win.c` | SendInput |
| `usbfn_win.c` | GenericUsbFn: interface discovery, ACTIVATE_USB_BUS, bus events, bulk transfers with the ZLP rule |
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
  may import only system DLLs (kernel32, user32, gdi32, d3d11, dxgi, ws2_32, cfgmgr32) and the UCRT (`api-ms-win-crt-*`).
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

Copy `sblinkd.exe` to `C:\starbite\link\sblinkd.exe`. It runs as the logged-on user; capture and input need no admin.

```
sblinkd.exe                     USB only (the default)
sblinkd.exe --tcp               also TCP 47474 on 127.0.0.1
sblinkd.exe --tcp --bind 0.0.0.0   TCP from the network: NO AUTHENTICATION, whoever connects gets keyboard and mouse
sblinkd.exe --console -v        log to a console as well, one line per frame and key
sblinkd.exe --output primary    capture the phone's own screen even when the Starbite Display exists
sblinkd.exe --gdi               force the GDI capture path
```

- Log: `C:\starbite\link\sblinkd.log` (rotated to `.old` at 8 MB). If `C:\starbite` cannot be created, it goes to
  `%LOCALAPPDATA%\Starbite\Link\`.
- Every 10 s it logs a stats line: fps, KiB/s, ms of encoding per frame, rects per frame, and how often flow
  control blocked.
- Only one instance runs per session (mutex `Local\StarbiteLinkSender`).

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
- no input into elevated windows or the secure desktop (UIPI; that would need uiAccess)
- TCP has no authentication; it binds to 127.0.0.1 unless `--bind` is given
