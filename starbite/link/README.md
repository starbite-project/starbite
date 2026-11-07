# Starbite Link

Starbite Link is the Starbite version of Samsung DeX, running in the other direction. Plug the S9 into the Mac
and the phone's **Windows 11 desktop appears on the Mac**, in the **Starbite Link** app. The app shows a window
that can go full screen in its own Space. Windows treats it as an **extra monitor** called "Starbite Display"
and extends the desktop onto it, so it is a second screen, not a mirror of the phone's panel. Windows also sets
that monitor's size to match the Mac window. Click into the window and the **Mac's keyboard and mouse control
Windows**. Ctrl+Option+Cmd gives them back to the Mac.

```
 phone (Windows 11 ARM64, 22631)                                   Mac
 ┌───────────────────────────────────────────────┐                ┌────────────────────────────────────┐
 │ desktop extends onto "Starbite Display"        │                │ StarbiteLink.app                   │
 │   = IddCx virtual monitor (win/idd)            │   SBLINK v1    │   window / full screen, Retina      │
 │ sblinkd.exe (win/sender)                       │ ─────────────▶ │   LZ4 decode into an IOSurface      │
 │   DXGI duplication, dirty rects only,          │  frames        │   (mac/)                            │
 │   64x64 tile compare, solid / LZ4 / raw        │ ◀───────────── │   keyboard (HID usages), pointer,   │
 │   SendInput for the Mac's keys and pointer     │  input, ACK    │   wheel, clipboard as text          │
 └───────────────────────────────────────────────┘                └────────────────────────────────────┘
   USB: phone = device 1209:0002 "Starbite Link", one vendor interface, bulk IN 0x81 / OUT 0x01 (GenericUsbFn)
   TCP: port 47474, the same byte stream (development now, and the fallback)
```

The protocol, including the USB rules, is specified in [`PROTOCOL.md`](PROTOCOL.md).

## Parts

| Part | Path | What |
|---|---|---|
| Protocol core | `proto/` | `sblink.{h,c}`: framing, messages, stream parser, dirty rects, rect encoding. LZ4 is the BSD-2 reference implementation, kept with its licence in `proto/lz4/`. `test.c` is the test suite: round trips, parser chunking and a fuzzer. |
| Mac app | `mac/` | `StarbiteLink.app` (Swift and AppKit), with USB (IOUSBHost) and TCP transports. The viewer session lives in `sbv.c`. `fakesender.c` is a fake phone. `selftest.sh` runs headless end-to-end tests. |
| Sender (phone) | `win/sender/` | `sblinkd.exe`: DXGI capture (GDI as a fallback), the encoder, flow control, SendInput, GenericUsbFn and TCP. `sblinkd-host` is the same code built for the Mac, with a synthetic desktop. Details are in `win/sender/README.md`. |
| Virtual monitor | `win/idd/` | The IddCx driver "Starbite Display" (UMDF 2): 1080p/900p/720p at 30 or 60 Hz, with 1080p30 preferred. Also here: an INF, a catalog, `sbdevnode.exe`, and an offline hive installer. Details are in `win/idd/INSTALL.md`. |
| End-to-end check | `e2e/` | `check.sh` builds and tests everything. `sblinkd-drag` is the real sender with a realistic desktop (one window being dragged). `encbench` measures encoder CPU per frame. `viewbench` is a headless viewer that measures traffic and checks the final image. |

## Build (Mac, no phone)

```sh
starbite/link/e2e/check.sh                 # builds all of the parts below, then runs every test and measurement
```

To build the parts one at a time:

```sh
starbite/link/proto/build.sh               # core tests (ASan+UBSan, then -O2) + Windows ARM64 compile check
starbite/link/mac/build.sh                 # build/link/StarbiteLink.app, build/link/mac/{fakesender,sbv-test}
starbite/link/win/sender/build.sh          # host tests, build/link/sender/{sblinkd-host,sblinkd.exe} (signed, PE checked)
starbite/link/win/idd/build.sh             # build/link/idd/pkg/StarbiteDisplay.{dll,inf,cat}, sbdevnode.exe, payload/
starbite/link/e2e/build.sh                 # build/link/e2e/{sblinkd-drag,encbench,viewbench}
```

Requirements:

- the Command Line Tools: clang and swiftc
- for the two Windows builds, the shared kit in `~/sbwin` (see `tools/win/README.md`)

Nothing is downloaded. The Mac app build deletes its ~200 MB Swift module cache when it finishes.

## Run it over TCP today

You need two terminals: one for the "phone" side and one for the app.

```sh
# the "phone": the real sender code with a desktop where one window is being dragged (or sblinkd-host)
build/link/e2e/sblinkd-drag --tcp --synth-auto 60 --print-input
# the Mac side
open build/link/StarbiteLink.app --args --tcp 127.0.0.1:47474
```

Alternatively, run the fake phone, which shows the keys and clicks it receives on its own screen:

```sh
build/link/mac/fakesender --port 47474
```

How to use the app:

- **Starting it.** With no arguments the app uses **Automatic** mode: USB when 1209:0002 is plugged in,
  otherwise TCP to the remembered address. The Link menu switches between USB, TCP and the address.
- **Full screen.** View > Toggle Full Screen, or Ctrl+Cmd+F, turns the window into a full screen of its own.
  With "Display Follows Window Size" on, Windows resizes the Starbite Display to fit the window.
- **Keys.** Cmd is the Windows key by default. The Input menu can make Cmd act as Ctrl instead.
- **Clipboard.** Shift+Cmd+V types the Mac clipboard into Windows.

## Measured on this Mac (M4), 2026-11-07

The test desktop is mostly static: a wallpaper, a taskbar with a clock, and two windows of text. One 46 % x 55 %
text window is dragged around continuously at 240 px/s. The compositor draws at 60 Hz and the sender is capped at
30 fps.

Encoder: the same `pipeline.c` that runs in sblinkd.exe, measured in thread CPU time. Every frame was decoded
back to exactly the original image.

| Display | Encoder CPU per frame, P-core avg / p95 | E-core avg | Per frame | At 30 fps | Keyframe |
|---|---|---|---|---|---|
| 1280x720 | 0.46 / 0.64 ms | 0.68 ms | 40.7 KiB, 28 % of pixels | 10.0 Mbit/s | 195 KiB, 1.8 ms |
| 1920x1080 | 1.16 / 1.46 ms | 2.17 ms | 117 KiB, 27 % of pixels | 28.8 Mbit/s | 403 KiB, 4.0 ms |
| idle (clock only) | ~0.01 ms, 10 frames in 10 s | | 1 KiB | 0.01 Mbit/s | |

End to end over TCP: `sblinkd-drag` → loopback → `viewbench`, which uses the app's own session code. The ranges
cover two runs of `check.sh`.

| Display | Measured | Notes |
|---|---|---|
| 1280x720 | 25-27 frames/s, 7.6-8.2 Mbit/s, 37 KiB and 8 rects per frame | Sender: 0.4-0.5 ms of encoding per frame; the whole process uses 0.7-0.8 ms of CPU per frame. Viewer: 0.23-0.27 ms per frame. |
| 1920x1080 | 25-27 frames/s, 22.5-24.6 Mbit/s, 111 KiB per frame | Sender: 1.1-1.7 ms of encoding per frame; the whole process uses 1.6-2.4 ms of CPU per frame. Viewer: 0.55-0.6 ms per frame. |

In every run:

- **Static desktop:** 0 frames and 0 bytes of frames in 3 s. Only PING/PONG (120 bytes) is sent.
- **Final image:** byte-exact with the sender's desktop.
- **Input:** 10 key-and-pointer bursts a second to the fixed sender (see Known issues) arrived without
  dropping the session.

**Estimate for the phone (not measured):** a Cortex-A55 is roughly 5-10x slower per core than an M4 P-core. That
puts encoding at about 2.5-5 ms per frame at 720p and 6-12 ms at 1080p, about 7-35 % of one of the four A55
cores at 30 fps while a window is being dragged, and nothing while the desktop is static.

Capturing frames on the phone costs extra: DWM in software, plus DXGI copies. That cost is not in these numbers
and is unknown. The 30 fps cap and the 2-frames-in-flight limit keep the sender from falling behind. If it is
slow, it simply sends fewer, larger updates.

**USB 2.0** (about 280 Mbit/s usable) has 10x headroom even at 1080p.

## What the phone needs (nothing here has run on the phone yet)

1. **USB device mode.** Another session is working on this. The phone must enumerate as `1209:0002` "Starbite
   Link": one vendor interface, bulk IN 0x81 and bulk OUT 0x01, served by the inbox GenericUsbFn driver. That
   needs the UsbFn controller to work, plus the USBFN configuration in the SYSTEM hive (`tools/win/hive-service.py
   usbfn-default` / `usbfn-config`). Until it works, TCP over a network link is the only transport.
2. **Offline install of the Starbite Display driver**, following `win/idd/INSTALL.md`:
   - **Route A:** hive edits from TWRP plus `StarbiteDisplay.dll` in `System32\drivers\UMDF`.
   - **Route B:** `install.cmd` as a one-shot LocalSystem service.

   Testsigning must stay on. Expected result: Settings > Display shows a second display, "Starbite", 1920x1080,
   extended.
3. **Offline install of sblinkd:**
   - copy `build/link/sender/sblinkd.exe` to `C:\starbite\link\sblinkd.exe`
   - start it at logon with the per-user Run key: `StarbiteLink` = `"C:\starbite\link\sblinkd.exe"` in the user's
     `NTUSER.DAT`, written offline

   It needs no admin rights. Its log goes to `C:\starbite\link\sblinkd.log`, with a stats line every 10 s: fps,
   KiB/s, ms of encoding per frame.
4. **First boot checks** (the phone-only list in `win/sender/README.md` and `win/idd/INSTALL.md`):
   - Does IddCx come up on the Basic Render (WARP) adapter?
   - Does DXGI duplication of the IddCx output work, or does sblinkd fall back to GDI?
   - Does SendInput work?
   - What do the GenericUsbFn IOCTLs actually do?
   - What does it all cost on the A55s?

## Known issues found by this check

- **Fixed in a copy only: sblinkd dropped the session on input.** This was in `win/sender/session.c`, the
  capture loop's dead-link test, `if (owner && now - last_rx > ...)`:
  - **Cause:** the loop reads `now` *before* it waits for `s->lk`. If the receive thread handles input in the
    meantime, `last_rx` is later than `now`, the unsigned difference wraps, and the session ends at once with
    "nothing received for 5000 ms: link dead".
  - **Reproduced:** `viewbench --input-hz 20/30` against both `sblinkd-host` and `sblinkd-drag`. The session
    dropped in 9 of 11 runs (sblinkd-host 3 of 3), at the first or second input burst.
  - **Fix:** `if (owner && now > last_rx && now - last_rx > ...)`. `e2e/build.sh` builds that fix into a copy,
    `sblinkd-drag-fix`, which passed 5 of 5 runs and every run of `check.sh`, with input at 10-30 bursts/s.
  - **Still to do:** apply the fix to `session.c`. `check.sh` uses the unfixed sender only without input.
- **Dragging resends the window.** SBLINK v1 has no copy-rect or move message, so a dragged or scrolled window
  is sent again in full: old plus new area, about 28 % of the screen per frame in the test above. DXGI already
  reports move rects. A MOVE message in a later minor version would cut a drag to a few KiB per frame.
- **Not done yet:**
  - The Windows cursor shape is not sent; the Mac pointer is shown instead.
  - The Caps Lock and Num Lock state is not synced.
  - TCP has no authentication. It listens on 127.0.0.1 unless `--bind` is given.
