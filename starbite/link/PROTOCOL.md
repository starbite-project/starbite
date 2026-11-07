# SBLINK v1: the Starbite Link wire protocol

Starbite Link is the DeX-like experience: the phone's Windows 11 desktop shows up on the Mac in a Mac app, as an
**extra monitor** ("Starbite Display") that Windows extends onto, and the Mac's keyboard and mouse drive Windows.
SBLINK is the one protocol between the two ends, the same over USB and over TCP.

| End | Role | Sends | Receives |
|---|---|---|---|
| Phone, Windows 11 ARM64 (IddCx virtual monitor + link service) | **source** | HELLO, MODE (answers), frames, PONG, BYE | HELLO, MODE (requests), ACK, input, PING, BYE |
| Mac app (the viewer window) | **viewer** | HELLO, MODE (requests), ACK, input, PING, BYE | HELLO, MODE, frames, PONG, BYE |

PING/PONG and BYE go both ways. The portable C core is `starbite/link/proto/sblink.{h,c}` (encode/decode, the
stream parser, LZ4, dirty rectangles); its tests are `starbite/link/proto/test.c` (`proto/build.sh`).

## 1. Transports

Both are a reliable, ordered **byte stream** in each direction. Message boundaries are only in the framing
(section 2), never in the transport.

- **TCP** (development now, Wi-Fi later): the source listens on port **47474**; the viewer connects. One viewer
  at a time; a second one gets `BYE(BUSY)` and is closed. `TCP_NODELAY` on both ends (input is latency-bound).
- **USB** (the real thing): the phone is a USB device, VID **0x1209** PID **0x0002**, product "Starbite Link",
  one vendor interface (class 0xFF) with bulk **IN 0x81** (phone to Mac: frames) and bulk **OUT 0x01** (Mac to
  phone: input, ACK). Windows serves it with the inbox GenericUsbFn driver (user mode: `GenericUsbFnIoctl.h`,
  device interface `{d274dae6-37b0-4c76-bf10-3a9681346dd1}`). Section 6 has the USB rules.

## 2. Framing

Every message is a 16-byte header plus a payload. **All integers are little endian.** No padding, no alignment.

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | magic | bytes `'S' 'B' 'L' 'K'` (u32 `0x4B4C4253`) |
| 4 | 1 | version | **1** (the framing version; see section 7) |
| 5 | 1 | type | message type (section 3) |
| 6 | 2 | flags | bit 0 `MUST_UNDERSTAND`; other bits 0 when sent, ignored when received |
| 8 | 4 | length | payload length in bytes, **at most 1 MiB (1048576)** and at most the peer's HELLO `max_payload` |
| 12 | 4 | sequence | per-direction message counter (below) |

Sequence: each end numbers the messages it sends, starting at 0 with its HELLO and adding 1 per message (wrapping
at 2^32). Both transports are lossless, so a gap means a bug; a receiver may log it, but must not act on it.

A PING with id 7 and t_us `0x1122334455667788`, sequence `0x04030201`, is exactly these 32 bytes
(`test.c` checks it):

```
53 42 4C 4B 01 30 00 00 10 00 00 00 01 02 03 04   header: 'SBLK' v1 PING flags 0 len 16 seq
07 00 00 00 00 00 00 00 88 77 66 55 44 33 22 11   id 7, reserved, t_us
```

## 3. Messages

Reserved bytes are 0 when sent and ignored when received. A payload **longer** than the fixed part of a known
type is accepted and the extra bytes ignored (that is how minor versions grow a message); a payload **shorter**
than the fixed part is an error.

| Type | Name | Direction | Payload size |
|---|---|---|---|
| 0x01 | HELLO | both | 56 |
| 0x02 | MODE | both | 12 |
| 0x10 | FRAME_BEGIN | source to viewer | 12 |
| 0x11 | RECT | source to viewer | 12 + data |
| 0x12 | FRAME_END | source to viewer | 16 |
| 0x13 | ACK | viewer to source | 8 |
| 0x20 | INPUT_KEY | viewer to source | 8 |
| 0x21 | INPUT_POINTER | viewer to source | 16 |
| 0x22 | INPUT_TEXT | viewer to source | 1..1024 |
| 0x30 | PING | both | 16 |
| 0x31 | PONG | both | 24 |
| 0x3F | BYE | both | 4 + 0..256 |

### HELLO (0x01), both ways, first message of a session

| Off | Size | Field |
|---|---|---|
| 0 | 1 | role: 1 = source (phone), 2 = viewer (Mac) |
| 1 | 1 | protocol major = 1 |
| 2 | 1 | protocol minor = 0 |
| 3 | 1 | reserved |
| 4 | 4 | capabilities (below) |
| 8 | 2 | width (pixels) |
| 10 | 2 | height |
| 12 | 4 | refresh rate in millihertz (60000 = 60 Hz), 0 = unknown |
| 16 | 4 | pixel format: 1 = BGRA8888 (bytes B, G, R, A; A is ignored, sent as 0xFF) |
| 20 | 4 | max_payload: the largest payload this end accepts (<= 1 MiB) |
| 24 | 32 | name, UTF-8, NUL padded (e.g. "Starbite S9", "Dev's MacBook") |

Source: width/height/refresh = the Starbite Display's current mode. Viewer: the size it would like (its window
or a whole Mac screen, in pixels), 0 = no preference.

Capabilities: bit 0 RAW, bit 1 LZ4, bit 2 SOLID (the RECT encodings this end can decode or produce), bit 3
MODE (viewer sends MODE requests / source can change size), bit 4 INPUT_KEY, bit 5 INPUT_POINTER, bit 6
INPUT_TEXT, bit 7 EXTEND (source only: the frames are an extra monitor Windows extends onto, not a mirror).
A feature is used only when **both** HELLOs have its bit. RAW is mandatory for both ends.

### MODE (0x02)

| Off | Size | Field |
|---|---|---|
| 0 | 2 | width |
| 2 | 2 | height |
| 4 | 4 | refresh, millihertz |
| 8 | 1 | status: 0 = REQUEST (viewer to source), 1 = APPLIED, 2 = REJECTED (source to viewer) |
| 9 | 3 | reserved |

The viewer asks for a size (window resized, moved to another Mac screen). The source changes the Starbite
Display's mode if it can and answers APPLIED with the size **it actually applied** (may differ: the nearest mode
it supports), or REJECTED with the size it kept. The source also sends an unsolicited APPLIED when the size
changes on the Windows side (Display Settings). After APPLIED, the next frame is a keyframe of the new size.

### FRAME_BEGIN (0x10)

| Off | Size | Field |
|---|---|---|
| 0 | 4 | frame id (1, 2, 3, ... per session; wraps from 0xFFFFFFFF to 1, never 0) |
| 4 | 2 | width of the display this frame is drawn on |
| 6 | 2 | height |
| 8 | 2 | number of RECT messages that follow |
| 10 | 2 | frame flags: bit 0 KEYFRAME (the rects cover the whole display) |

A frame is FRAME_BEGIN, exactly `nrects` RECTs, FRAME_END, with nothing else of the source's in between
(PONG, MODE, BYE wait until FRAME_END). A frame with 0 rects is allowed (a pure timing/heartbeat frame). If
width/height differ from the viewer's buffer, the viewer resizes it (contents undefined until a keyframe; the
source always sends a keyframe after a size change, after HELLO, and when the viewer asks by sending ACK with
frame id 0).

### RECT (0x11)

| Off | Size | Field |
|---|---|---|
| 0 | 2 | x |
| 2 | 2 | y |
| 4 | 2 | w (>= 1) |
| 6 | 2 | h (>= 1) |
| 8 | 1 | encoding |
| 9 | 3 | reserved |
| 12 | n | data |

Encodings (`n` = payload length - 12):

| Value | Name | Data |
|---|---|---|
| 0 | RAW | `w*h*4` bytes BGRA, rows top to bottom, no row padding; `n` must be exactly `w*h*4` |
| 1 | LZ4 | one LZ4 **block** (no frame header) that decompresses to exactly `w*h*4` bytes of RAW layout |
| 2 | SOLID | 4 bytes B, G, R, A: every pixel of the rect |

The rect must lie inside the frame's width x height; a viewer drops a rect that does not (and may close).
Rects of one frame do not overlap (the core's dirty-rect helper never makes overlaps), but a viewer applies them
in order, so overlap would still be well defined. Because of the 1 MiB payload limit, a big area is sent as
several RECTs: `sblk_split_rect()` cuts full-width strips that fit as RAW (e.g. 1920 wide: 136 rows per strip).

### FRAME_END (0x12)

| Off | Size | Field |
|---|---|---|
| 0 | 4 | frame id (same as FRAME_BEGIN) |
| 4 | 4 | reserved |
| 8 | 8 | capture time, microseconds, source clock (when the OS handed the frame over) |

The viewer presents the frame (all its rects at once) on FRAME_END, then sends ACK.

### ACK (0x13), flow control

| Off | Size | Field |
|---|---|---|
| 0 | 4 | frame id: acknowledges this frame and every earlier one |
| 4 | 4 | present_us: viewer's decode + present time for it, 0 = unknown (diagnostics, adaptive LZ4 effort) |

**The source keeps at most 2 frames in flight** (FRAME_END sent, ACK not received). With 2 in flight it does not
send frames; it keeps accumulating dirty area instead (the next frame covers everything that changed meanwhile,
so nothing is lost, frames are just merged). That bounds latency on a slow link and stops the phone burning CPU
on frames nobody will see. Frame ids compare as serial numbers: in flight = `last_sent - last_acked` (u32).
`ACK(0)` from the viewer means "send a keyframe" (after a resize or a decode error).

### INPUT_KEY (0x20)

| Off | Size | Field |
|---|---|---|
| 0 | 2 | HID usage page: 0x07 keyboard, 0x0C consumer (media, volume, brightness) |
| 2 | 2 | HID usage (e.g. 0x04 = A, 0x28 = Enter, 0xE0..0xE7 = modifiers) |
| 4 | 1 | 1 = down, 0 = up (other values are an error) |
| 5 | 1 | reserved |
| 6 | 2 | modifier state **after** this event: bits 0-7 = the HID modifier byte (LCtrl, LShift, LAlt, LGUI, RCtrl, RShift, RAlt, RGUI), bit 8 Caps Lock on, bit 9 Num Lock on |

Physical keys by HID position, not characters: the Windows keyboard layout decides the character, as with a real
USB keyboard. The Mac maps Cmd to GUI (the Windows key) by default; the viewer may offer Cmd-as-Ctrl. The
source uses the modifier state to resynchronise (a modifier released while the Mac window was not focused):
before injecting, it releases any modifier it holds down that the state says is up. When the viewer loses focus
it sends up events for every key it reported down.

### INPUT_POINTER (0x21)

| Off | Size | Field |
|---|---|---|
| 0 | 4 | x, signed, pixels of the Starbite Display (0,0 = its top-left) |
| 4 | 4 | y, signed |
| 8 | 4 | buttons held after this event: bit 0 left, 1 right, 2 middle, 3 back, 4 forward |
| 12 | 2 | wheel dx, signed, 1/120 notch units (+ = right) |
| 14 | 2 | wheel dy, signed, 1/120 notch units (+ = up/away from the user, the Windows sign) |

Absolute positions (the Mac cursor is over the viewer window). The viewer scales from window points to display
pixels. Coordinates outside the display are allowed (a drag that leaves the window); the source clamps. Trackpad
smooth scrolling sends small deltas (e.g. 12 = a tenth of a notch); Windows takes high-resolution wheel deltas.
The viewer coalesces moves: at most one INPUT_POINTER per Mac display refresh unless buttons or wheel changed.

### INPUT_TEXT (0x22), optional

1..1024 bytes of UTF-8 text to type (Mac input methods, emoji picker, dictation). The source injects it as
Unicode key events. Only when both HELLOs have the INPUT_TEXT bit.

### PING (0x30) / PONG (0x31)

PING: `u32 id, u32 reserved, u64 t_us` (sender's clock). The receiver answers at once with PONG: `u32 id, u32
reserved, u64 echo_us` (the PING's t_us, unchanged), `u64 t_us` (responder's clock). Round trip = now - echo_us;
the clocks are never compared directly (the offset estimate is `t_us - (echo_us + rtt/2)`, used only to turn
FRAME_END capture times into glass-to-glass latency on the Mac). Either end PINGs every 1 s when idle; **5 s
without receiving any message** = the link is dead (close, section 5).

### BYE (0x3F)

`u32 reason` + optional UTF-8 text (0..256 bytes). Reasons: 0 normal, 1 protocol error, 2 version not supported,
3 shutting down (Windows shutdown/sleep, app quit), 4 busy (another viewer). After sending BYE the sender stops
sending and closes; after receiving BYE the receiver closes.

## 4. Session

```
viewer                                   source (phone)
  HELLO(role 2, caps, preferred size) -->
                                    <--  HELLO(role 1, caps, current size, EXTEND)
  MODE(REQUEST w x h)               -->          (optional, if both have MODE)
                                    <--  MODE(APPLIED w' x h')
                                    <--  FRAME_BEGIN(1, KEYFRAME) RECT... FRAME_END(1)
  ACK(1)                            -->
  INPUT_* ...                       -->  <-- FRAME 2, 3 (at most 2 in flight)
  PING                              -->  <-- PONG
  BYE                               -->
```

- The **viewer** speaks first (on TCP it connected; on USB it is the host). The source answers HELLO with
  HELLO. Nothing else is sent before a HELLO has been received, except BYE.
- A HELLO received in the middle of a session starts a **new session**: the receiver drops all state (frames in
  flight, held keys and buttons), answers HELLO, and the source sends a keyframe. That is how a viewer restarts
  over USB, where there is no "connection".
- Source with no viewer: it stops capturing (the IddCx swap chain is still serviced, but no comparisons and no
  compression), so an unwatched Starbite Display costs almost nothing.

## 5. Errors

There is **no resynchronisation inside a stream**. Any of these is a protocol error:

- bad magic, header version != 1, length > 1 MiB or > own max_payload
- payload shorter than the type's fixed part; a field out of range (role, MODE status, key down > 1, RECT with
  w or h = 0, unknown encoding, RAW length != w\*h\*4, SOLID length != 4, text over its limit)
- an unknown type with `MUST_UNDERSTAND` set (an unknown type without it is skipped: its length is known)
- LZ4 data that does not decompress to exactly w\*h\*4 bytes, a rect outside the frame

On a protocol error the receiver sends `BYE(PROTOCOL)` if it can, then **closes**:

- TCP: close the socket. The viewer reconnects after 1 s (and keeps retrying, 1 s apart).
- USB: see section 6.4. Either way the next session starts from HELLO with all state dropped.

The core's parser makes errors sticky (`sblk_parser_feed()` keeps returning the error) until
`sblk_parser_reset()`, so a caller cannot accidentally continue parsing garbage.

## 6. USB rules

### 6.1 Endpoints

Bulk IN 0x81 carries the source's messages (frames, PONG, MODE answers, BYE); bulk OUT 0x01 carries the viewer's.
Max packet 512 bytes (high speed). The S9's port is USB 2.0 high speed: about 35-40 MB/s usable in practice.

### 6.2 Transfers and the stream

Each direction is a byte stream: the receiver concatenates the data of its completed transfers and feeds it to
the parser (`sblk_parser_feed()` takes any chunk sizes). **Message boundaries need not line up with transfers**:
a transfer may hold several messages or a piece of one.

A USB transfer ends at a **short packet** (less than 512 bytes) or when the receiver's buffer is full. So:

- The receiver always posts reads whose size is a multiple of 512 (the core's suggested size is 64 KiB; on the
  Mac, several reads queued so the pipe never idles), and treats whatever arrives as stream bytes.
- The sender writes a message, or a batch of messages (e.g. a whole frame), as one transfer. **If a transfer's
  length is an exact multiple of 512, the sender ends it with a zero-length packet (ZLP)**, otherwise the
  receiver's read would not complete until more data comes and the message would sit in its buffer. On Windows:
  `IOCTL_GENERICUSBFN_TRANSFER_IN_APPEND_ZERO_PKT` for such transfers (plain `TRANSFER_IN` otherwise). On the
  Mac (IOUSBHost): set the ZLP option on the OUT pipe, or send a zero-length write after a multiple-of-512 write.
- A ZLP or short packet is only a "deliver now" marker; it carries no framing meaning.

### 6.3 Session start on USB

There is no connection, so:

1. The Mac opens the interface and **drains IN** (reads with a short timeout until nothing comes): that throws
   away the tail of an earlier session.
2. The Mac sends HELLO **as a transfer of its own** (so it starts at a transfer boundary).
3. The phone's link service treats a HELLO at the start of an OUT transfer as a new session (section 4).

### 6.4 Errors on USB

- The **Mac** on a protocol error: sends BYE(PROTOCOL) if it can, aborts its pending reads, clears halt on both
  pipes (which also resets the data toggles), drains IN, resets its parser, and starts again at 6.3.
- The **phone** on a protocol error in the OUT stream: sends BYE(PROTOCOL) on IN, resets its parser and then
  **ignores OUT transfers until one begins with a valid HELLO header** (magic, version 1, type HELLO). Because
  the Mac always sends HELLO as its own transfer (6.3), this always recovers without unplugging.
- USB reset, detach, or a new SET_CONFIGURATION (GenericUsbFn bus events) = the session is gone: both sides
  drop state; the Mac starts again at 6.3 when the device comes back. A SET_INTERFACE (alternate setting 0)
  is handled like SET_CONFIGURATION: the phone takes the pipes again and waits for HELLO.
- **The phone does not answer HELLO** (no OUT read posted: its link service restarted on a bus that stayed
  configured, so it saw no bus event). The Mac escalates; each restart without a HELLO answer counts and a
  HELLO answer resets the count: restarts 1-2 as above; restart 3 (or at once after two HELLO OUT transfers
  in a row time out) sends SET_INTERFACE first; from restart 5 the Mac resets the USB device (it
  re-enumerates: the phone sees Reset and Configured), at most every 30 s, doubling to 5 minutes. Retries
  back off to 8 s. No message or wire format changes.
- The **phone**, when its link service starts on a bus that a previous instance had configured and no bus
  event arrives within 1.5 s, adopts the pipes (GET_CLASS_INFO) and posts its OUT reads, so the Mac's next
  HELLO is read without any reset.

## 7. Versioning

- **Header version** (byte 4) changes only for incompatible framing changes. A receiver closes on a version it
  does not know (BYE VERSION, best effort).
- **Protocol major/minor** in HELLO: a different major = incompatible: send BYE(VERSION) and close. A higher
  minor is compatible: it can only add message types (old receivers skip them), add bytes at the end of a
  message (old receivers ignore them), add capability bits (used only when both ends set them), and add RECT
  encodings (used only after a capability bit says the viewer decodes them).
- Anything a newer end needs the older one to understand is sent with `MUST_UNDERSTAND` so the old end closes
  instead of misbehaving.

## 8. Performance rules for the source (the phone)

Windows on the S9 draws in software on 4 slow A55 cores, so the source must cost nothing when nothing changes and
little when something does:

1. **Start from the OS's dirty rectangles.** IddCx hands each new frame with the dirty rects Windows already
   knows. Pass each one as the `roi` of `sblk_dirty_rects()` with `SBLK_DIRTY_UPDATE_PREV`: only those areas are
   compared (64x64 tiles, early exit on the first differing row of a tile) and copied into the shadow frame. An
   OS frame with no dirty rects costs no pixel work at all. A full-frame compare happens only for keyframes.
2. **Solid first, then LZ4, raw last** (`sblk_build_rect()`): solid areas (window backgrounds, fills) are 4
   bytes; LZ4 is used only when it saves at least 1/8 (desktop content: 8 % of raw in the tests); noise
   (video, photos) goes raw rather than wasting CPU on failed compression. The acceleration parameter can be
   raised (2-8) when the viewer's ACKs show the link has spare bandwidth but the CPU does not.
3. **At most 2 frames in flight**; while blocked, keep merging dirty area (no capture work per OS frame beyond
   recording the dirty rects).
4. **No copies in the send path**: `sblk_encode_rect_head()` + the rect data can be handed to the transfer as
   they are; one gather of the rect into a contiguous buffer is the only copy for RAW/LZ4.
5. Cap the frame rate at the display's refresh and stop entirely with no viewer (section 4).

Measured on the Mac (M-series, -O2, `test.c`): a 1280x720 synthetic desktop compresses 3.7 MB -> 0.30 MB (8.1 %)
in about 2 ms, decompresses in about 1 ms; expect roughly 10x slower on an A55, so a full keyframe is about 20 ms
of one core and a typical update (a few tiles) well under 1 ms.

## 9. Core API (starbite/link/proto/sblink.h)

| Function | Use |
|---|---|
| `sblk_encode`, `sblk_encoded_size`, `sblk_encode_rect_head`, `sblk_put_header` | build messages |
| `sblk_decode`, `sblk_decode_header`, `sblk_decode_payload` | decode from a contiguous buffer |
| `sblk_parser_init/feed/reset` | the stream parser; payloads wholly inside a chunk are returned in place |
| `sblk_lz4_compress/decompress/bound` | LZ4 block (embedded reference LZ4 1.10.0, BSD-2-Clause, `proto/lz4/`) |
| `sblk_dirty_rects` | 64x64 tile compare + merge, roi, shadow-frame update |
| `sblk_solid_color`, `sblk_split_rect`, `sblk_build_rect` | source-side rect encoding |
| `sblk_apply_rect` | viewer side: draw a RECT into a frame buffer, bounds-checked |

The core is C99 with no OS calls and no allocation (all buffers from the caller), so the same file builds into the
Windows UMDF/user-mode side (clang-cl, `tools/win/build.sh`), the Mac app and the tests.
