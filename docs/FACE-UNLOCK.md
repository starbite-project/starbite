# Face Unlock (Starbite OS Next)

Face Unlock opens the lock screen when the front camera sees the owner's face. It runs entirely on the phone.
The S9's front camera is a plain colour camera, without Face ID's depth camera or IR dot projector, so this is a
convenience unlock with liveness checks. The passcode stays the root of trust.

## What the user sees

- **Settings > Face Unlock & Passcode > Face Unlock**
  - Without a set-up: *Set Up Face Unlock*. A passcode must be set first.
  - Set-up starts with an alert that offers **Speed** or **Security**. Then a full-screen circle shows the camera's
    mirrored picture. Four arcs fill as the head turns left, right, up and down; the centre pose is first.
  - After set-up the page has these rows:
    - *Unlock with Face* (a switch);
    - *Speed* or *Security* (a check mark);
    - *Reset Face Unlock* (red, with an alert).

    The footer says what the mode does, and why Face Unlock is not ready when it is not.
- **Lock screen**
  - When the screen wakes and Face Unlock is ready, a scan starts. The padlock above the date shows while the camera
    looks.
  - On a match the padlock opens and the motor gives a light tap. The swipe up then goes straight in, with no passcode
    (as Face ID works).
  - A swipe up before the match shows the passcode screen, and a new scan runs behind it. A match there opens the phone.
  - A failed scan gives an error tap. On the passcode screen it shows "Face not recognised".
  - Security's hints replace "Swipe up to open": "Turn your head slightly", "Move a little closer", "More light is
    needed".
- **When the passcode is required** (iOS's rules):
  - after a restart;
  - after 5 failed scans in a row;
  - after 48 h without the passcode;
  - while a passcode back-off runs.

  Turning the passcode off forgets the face.

## Modes

| | Speed | Security |
|---|---|---|
| Match | the first embedding with cosine ≥ **0.40** against any of the 5 templates | **2** embeddings ≥ **0.50** |
| Liveness | none | a **blink**, or a **head turn**: yaw ratio range ≥ 0.10 **and** nose parallax ≥ 0.035 |
| Gives up | 1.5 s after the first face (2.5 s with no face) | 3 s after the first face (4 s with no face) |
| LFW false accepts, one image pair (6000 official pairs + 4.5 M impostor pairs, the same model in OpenCV) | 0.02 % | 0.0006 % |
| LFW false rejects, one pair (unconstrained photos across years: the phone's own frames are far easier) | 7.7 % | 13.9 % |

With 5 templates the worst-case false accept rate per look is about five times the pair rate.

**Liveness.** Before Security unlocks, it needs one of two signs.
- **A head turn.** The test is geometric. A flat picture keeps its 5 points on one plane, so the nose stays where a
  homography of the eyes and mouth corners puts it, however the picture is turned (host test: a photo turned ±36°
  gives parallax 0.000). A real head turning ~10° moves the nose off that plane (0.067 at ±12°).
- **A blink.** The eyes' dark core fades against the skin below them, then comes back.

A video of the owner played on another screen can still pass. That is a known limit of any 2D camera.

## Pipeline (kern/face.c, kern/facenet.c; no libc, host-tested)

1. **Camera:** cam.c's front camera, portrait 360 × 480 and not mirrored (CAM_ROT270), with auto-exposure. It starts
   at the screen's wake.
2. **Detection:** YuNet 2023mar runs on a 3× box-filtered copy (120 × 160, padded to 128 × 160): 17.5 M
   multiply-adds. Decoding follows OpenCV's FaceDetectorYN. The best face gives 5 points.
3. **Alignment:** a least-squares similarity fit of the 5 points to ArcFace's 112 × 112 template, sampled bilinearly
   from the full frame.
4. **Embedding:** SFace 2021dec (MobileNet style, 128-D): 574 M multiply-adds.
   - The runtime keeps NHWC fp32 activations.
   - The 1 × 1 convs run as an 8 × 8-register GEMM on int8 weights (each job dequantises its 8-channel tile).
   - The final 50176 → 128 layer uses 4-bit weights with one scale per 64 inputs.
   - Work spreads over the four A55 (kern_next.c's band workers). It runs in steps of whole ops, at most ~30 ms of
     face work in a loop turn.
5. **Match:** the best cosine against the 5 templates.

**Models and licences** (checked 2026-11-15): YuNet is MIT (Shiqi Yu, libfacedetection / OpenCV Zoo); SFace is
Apache-2.0 (OpenCV Zoo). `starbite/sbos/tools/face-pack.py` builds the pack:
- it downloads both ONNX files, with pinned sha256;
- folds the batch norms and pads channels to a multiple of 4;
- quantises the weights;
- writes `build/face/sbface.pak` (7.3 MB) with a `.sha256` sidecar and `sbface.LICENSES.txt`.

The pack is **not in git** and **not in the image**. Next is 27.5 MB against check-image.py's 32 MiB budget, and the
kernel's arena has ~1.9 MiB free at "ready". Instead the pack is a file on the card's exFAT partition:
**`/starbite/face/sbface.pak`**. build.sh copies it beside the images (`starbite/out/sbface.pak` + `.sha256`).

kern_next.c loads it into the UI heap, 256 KiB a turn, and checks its own SHA-256 (in its header) as it comes.

**Only while locked** (Track A's condition: a1btn already peaks at 134 of the 135 MiB allowed). The shell tells the
service each change of the lock (`sh_face_lock`):
- **Locked** (the start, every screen-off): the pack loads in the background (~2.5 s at 256 KiB a turn, est.), when a
  face is set up and Unlock with Face is on. A wake before it is in falls back to the passcode silently and logs
  "a wake before the pack was in memory".
- **Unlocked:** the pack and every face buffer are freed before home and apps build (`next: face: pack freed
  (unlock): N KiB back`).
- **Set-up from Settings** (unlocked): the pack loads when *Set Up Face Unlock* is tapped (while the Speed / Security
  alert is read; the set-up screen says "Loading Face Unlock..." until it is in). It is freed when set-up ends.

Without the file, set-up says so. The self test's dry build never reads the card. store-update.sh does not carry the
pack: it goes to the card separately.

**Accuracy against the references** (host, 60 LFW faces):
- YuNet's 12 outputs match onnxruntime's within 1.9e-6;
- SFace with int8 + 4-bit weights gives cosine ≥ 0.9986 to onnxruntime's fp32 on OpenCV's crops;
- the whole pipeline's embeddings reach cosine ≥ 0.998 against OpenCV + onnxruntime;
- the 5 points are within 0.8 px of OpenCV's FaceDetectorYN.

4-bit 1 × 1 convs were tried and rejected (min cosine 0.71).

## Storage: the face never leaves the phone

- **Blob `face.t`:** the 5 templates (unit vectors × 127, int8) and the mode, sealed under a random 256-bit key K. The
  seal is a SHA-256 counter stream plus HMAC-SHA256, with separate sub-keys.
- **Blob `face.k`:** K wrapped under a key derived from the passcode (salted, 20000 rounds of SHA-256: the passcode's
  own cost).
  - The first right passcode after the feature arrives makes K. Each right passcode after a restart unwraps it, in a
    turn of its own, ~45 ms.
  - A passcode change rewraps K. Turning the passcode off deletes both blobs.
- K and the opened templates live only in RAM. After a restart nothing can open the templates before the passcode.
- No backup, USB console or log line contains an embedding, a key or the digits. The logs give times, counts and the
  best score only.
- **Known limit:** K's randomness comes from timer jitter plus a SHA-256 pool, as Wi-Fi's TLS does. The secure world
  holds the TRNG.

## Memory and time

- **Image:** about +40 KB of code; the pack is on the card.
- **Heap:** the pack, 7.3 MB of the UI heap, only while locked or during set-up. Host check (`SBTK_HEAP_MB=160`
  films, the pack + a scan's buffers held while locked: `SBTK_FACE_HOLD=1 SBTK_FACE_PACK=build/face/sbface.pak`):
  - locked: 80 MiB at rest (the baseline is 66);
  - freed at the first unlock;
  - all 85 films then peak as the baseline (a1btn 134), 0 refusals;
  - a1btn's largest free block is 9 MiB (16 in the baseline): the freed hole.
- **Working memory:** ~6.9 MiB from the UI heap, only during a scan or set-up:
  - activations 2 × 3.2 MB;
  - a 256 KB pool of weight tiles;
  - the frame copy and its grey.
- **Set-up screen:** ~1 MB of layers (the ring is drawn at half resolution).
- **Host times** (Apple silicon, one core, ASan): detection 5 ms, embedding 52 ms.
- **The A55's times** come from the face test image, **starbite-next-facetest.img**. It is unattended and writes
  nothing:
  - it loads the pack from the card;
  - logs `next: face: bench: ...` (the networks on a made-up frame);
  - runs the front camera for 4 s with nobody enrolled and logs `next: face: camera test: first frame after N ms ...`;
  - counts the loop's turns of 100 ms or more (any fails it);
  - then restarts into recovery.
- While a face job runs, the A55 sits at 1794 MHz. The clock-down timer brings it back afterwards.

## Tests

| Test | What |
|---|---|
| `sh starbite/sbos/kern/host/face-test.sh -v` | crypto (RFC 4231 HMAC, seal / open / tamper, wrap / unwrap / wrong digits), the scan rules, the liveness geometry, enrolment. With `FACE_REF=` (kern/host/face-ref.py's file) also the runtime against onnxruntime / OpenCV, LFW pairs, memory. 58 checks |
| `FACE_REF=... sh starbite/sbos/kern/host/face-svc-test.sh -v` | the whole service (ui/face_svc.c) on recorded frames: set-up through 5 real head poses (LFW photos of one person, `FACE_REF_MANY=George_W_Bush,Colin_Powell,Tony_Blair face-ref.py ... 50`), unlock, 5 failures, restart, passcode change / removal, Security (a still photo refused, a turning head accepted), nothing in clear in the store, the longest turn (host 25 ms). 27 checks |
| `starbite/sbos/ui/host/build.sh`, stills `face=ok`, `face=setup:N`, `faceset=:` | the lock's open padlock, Settings' group, the set-up screen (the host fakes scans: `SBTK_FACE=fail`) |

LFW is used only on the Mac, for these tests. It never goes into git or onto the phone.

## Not yet measured on the phone (the main session's runs)

1. The face test image (the card needs `/starbite/face/sbface.pak` first): detection and embedding ms on the A55, the
   camera's first frame, the longest turn (must stay < 100 ms).
2. Attended: the camera's wake-to-first-frame time (the Speed target is < 300 ms from wake). Set-up in normal room
   light; unlock in Speed and in Security; a photo of the owner on another phone must not unlock in Security.
3. Tuning that needs real frames:
   - the parallax threshold against real landmark noise (host: ±0.7 px noise gives 0.028 < 0.035);
   - the blink signal;
   - low light (the screen as fill light is not done yet).

## Round 2 (2026-11-16): after 9i on S9 #1

**The user's report on 9i:** set-up worked, but it never recognised them, and the camera picture was "like a really
old Nokia".

**Camera pipeline v2** (`kern/cam_isp.c` `cam_isp_process`, default in `cam.c`; `cam_set_pipeline(1)` = v1):

| | v1 (9i) | v2 |
|---|---|---|
| Resampling | one Bayer cell per pixel, nearest: aliasing, all the noise | every cell averaged in (area resample) |
| Raw reads | column-wise from uncached memory | rows in bulk, the sensor's order; the rotated output in tiles of 8 |
| Lens shading | none (dark corners) | radial gain, 1.4x at the corners |
| White balance | grey world over every pixel (skin pulls it) | grey world over the near-grey pixels |
| Colour | no matrix (dull) | a 3 x 3 matrix |
| Tone | gamma 2.0 | sRGB with a mild S |
| Exposure | 18 % of the whole frame's greens | **the face's box** at 24 % when a face is found, else frame + middle at 16 %; whole steps when far off |

**Host lab** (`kern/host/face-isp-lab.sh synth`): LFW faces made into raw sensor frames, then each pipeline's
embedding compared with the clean face's (cosine):

| Light | v1 | v2 |
|---|---|---|
| bright | 0.954 | 0.948 |
| dim | 0.890 | 0.920 |
| dark | 0.727 | 0.820 |

The lens shading and the colour matrix are first guesses. The face test image's raw frames calibrate them.

**Recognition changes:**
- Scans use a frame only once the exposure has settled, or 0.5 s after the first frame.
- Set-up uses settled frames only, and averages 3 looks for the centre and 2 for each turn. One early dark frame no
  longer defines the template.
- **After installing round 2, reset Face Unlock and set it up again.** The 9i templates come from v1 pictures.

**Face test image round 2** (`starbite-next-*-facetest.img`, attended: the user looks at the phone ~20 s after the
start, for ~10 s):
- the pack, the bench, then the camera for 9 s;
- at 0.4 / 1.5 / 3 / 5 / 7.5 s it captures the whole raw frame (1632 x 1224) and the preview;
- it saves them into the store entry **`sbos-facedbg`** (made once:
  `tools/store/store-update.sh WORKDIR --volatile sbos-facedbg=67108864`; never the user's photos);
- pull with `python3 tools/face/facedbg-pull.py` in TWRP;
- compare with `sh starbite/sbos/kern/host/face-isp-lab.sh raw OUTDIR .../NN-raw.bin`.
