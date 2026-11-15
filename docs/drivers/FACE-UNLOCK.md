# Face unlock with the front camera: the fallback plan (2026-11-15, drivers track, off the phone)

Why this exists: the iris scanner (docs/drivers/IRIS.md) may be closed to us. Run 17 showed the secure world holding
the iris camera's pins and its receiver (CSIS3) from boot; run 18 (P1c) tries the stock PREPARE call. If the iris
pictures stay on the secure side, or PREPARE is refused, biometrics moves to the **front camera**, which already works
for Starbite (kern/cam.c: S5K3H1, 1632 x 1224 RAW10 at 30 fps over CSIS1, our software ISP, PASS on the phone).

The goal stays the user's: "it feels just like Apple's Face ID": look at the phone, it is open, under ~0.5 s. The
honest difference: Face ID has a depth camera and an IR dot projector; the S9's front camera is a plain colour camera.
So this is a **convenience unlock** with liveness checks, not a vault; the passcode stays the root of trust.

## 1. Pipeline

| Step | How | Cost (one A55 core at full clock, NEON) |
|---|---|---|
| Camera up | cam_open at a small preview (e.g. 480 x 360), started at the wake event (Power, double tap, lift) | first frame ~70-150 ms (to be measured) |
| Light | in the dark, the screen turns white behind the lock screen for ~200 ms as fill light (as front flashes do) | |
| Face find | a small face detector on a 160 x 120 grey copy: either a classical LBP cascade (no learned weights we must license) or a tiny CNN detector | 3-8 ms |
| Landmarks + align | 5 points (eyes, nose, mouth corners), a similarity warp to 112 x 112 | 2-5 ms |
| Embedding | a compact face-embedding CNN (MobileFaceNet-class, ~1 M parameters, 128-D, int8) in our own small C/NEON conv runtime | 20-40 ms |
| Match | cosine similarity against the enrolled set (5-10 vectors per user), threshold tuned on enrolment | < 1 ms |
| **Total** | | **~0.15-0.25 s after the first frame; ~0.3-0.4 s from wake** |

Enrolment: ~20 frames while the user turns the head slowly (left, right, up, down) and once with glasses off if worn;
keep a spread of vectors. Templates: 128 floats x ~10 = ~5 KiB in the state slot, never exported, deleted on reset or
a passcode change. Attention: the eyes must be open and looking (the landmarks give the gaze roughly).

## 2. Liveness (the weak spot of 2D face unlock)

A photo or a video of the owner is the threat. Checks, cheapest first, combined:
1. **Screen-flash reflectance**: the display flashes two colours (e.g. warm then cool) for one frame each; real skin
   and a curved face change colour and shading differently from paper or another phone's screen.
2. **Micro-motion / parallax** across 3-5 frames: a real head moves slightly in 3D (nose against cheeks); a flat photo
   moves as one plane.
3. **Blink** within ~2 s (only when 1-2 are unsure, so the normal unlock stays fast).
No liveness check makes a 2D camera as safe as Face ID; the passcode rules stay: required after a restart, after 5
failed face tries, after 48 h, and for any security setting.

## 3. The hard parts

1. **A face-embedding model we may ship.** Most good pretrained face-recognition weights come from datasets with
   non-commercial licences. Options, in order: (a) a model trained on permissively licensed data (to be searched and
   checked before any download); (b) train our own small model on such data on the Mac (slow but clean); (c) a
   classical method (LBP / HOG features + a metric) as a first, weaker version. Nothing gets downloaded or used before
   its licence is checked and written down here.
2. **A small int8 CNN runtime** in C (conv, depthwise conv, PReLU, pooling, a fully connected layer), host-tested
   against a reference on the Mac, NEON on the phone. About 1500 lines.
3. **Low light**: the screen as fill light, plus auto-exposure from cam.c (already in the driver).

## 4. Steps

| Step | Where | What it settles |
|---|---|---|
| F0 | Mac | the licence choice for the detector and the embedding model, written here |
| F1 | Mac | the runtime + the model, host-tested on stills; speed per stage on the Mac, projected for the A55 |
| F2 | phone, attended | the camera's wake-to-first-frame time and the preview at the chosen size (cam.c as is) |
| F3 | phone, attended | enrol + unlock on the phone with timing per step; liveness checks one by one |
| F4 | Track A | the lock screen hooks (wake starts the camera; the padlock opens on a match; the passcode rules) |

Nothing here needs new hardware access: the front camera driver already passes on this phone.
