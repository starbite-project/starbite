#!/usr/bin/env python3
"""Face Unlock's host references (kern/host/face-test.c): what onnxruntime and OpenCV say for the same inputs.

    face-ref.py PACKDIR LFWDIR OUT.bin [N]      (PACKDIR: build/face, with dl/*.onnx; needs numpy onnx onnxruntime cv2)

Writes OUT.bin, little-endian:
  'FREF', u32 n, then n records:
    YuNet: its input (160 x 128 x 3 float, B G R, 0..255) and onnxruntime's 12 outputs as packed (counts in the order
           of the pack's outputs: cls/obj [rows*cols], bbox [rows*cols*4], kps [rows*cols*10]);
    the picture the C pipeline sees (360 x 480 XRGB u32, the LFW face scaled into a portrait frame);
    OpenCV's best face on that frame (FaceDetectorYN at 1/3 scale as the C code does): 15 floats (x y w h, 5 points,
           score), or all 0;
    OpenCV's aligned crop of it (112 x 112 x 3 u8, B G R) and onnxruntime's SFace embedding of it (128 float);
    the person's id (u32) and the image's index in the person's set (u32).
Only for the host test: LFW stays in the scratchpad, never in git or on the phone.
"""
import os, struct, sys
import numpy as np
import onnx, onnxruntime as ort, cv2

pk, lfw, out = sys.argv[1], sys.argv[2], sys.argv[3]
N = int(sys.argv[4]) if len(sys.argv) > 4 else 60
YH, YW = 160, 128
FH, FW = 480, 360


def session(path, h, w):
    m = onnx.load(path)
    d = m.graph.input[0].type.tensor_type.shape.dim
    d[2].dim_value, d[3].dim_value = h, w
    for o in m.graph.output:
        for dd in o.type.tensor_type.shape.dim:
            dd.dim_param = 'x'
    return ort.InferenceSession(m.SerializeToString(), providers=['CPUExecutionProvider'])


ypath = os.path.join(pk, 'dl', 'face_detection_yunet_2023mar.onnx')
spath = os.path.join(pk, 'dl', 'face_recognition_sface_2021dec.onnx')
ys = session(ypath, YH, YW)
ss = ort.InferenceSession(spath, providers=['CPUExecutionProvider'])
det = cv2.FaceDetectorYN.create(ypath, '', (FW // 3, FH // 3), 0.6, 0.3, 50)
rec = cv2.FaceRecognizerSF.create(spath, '')
onames = ['cls_8', 'cls_16', 'cls_32', 'obj_8', 'obj_16', 'obj_32', 'bbox_8', 'bbox_16', 'bbox_32', 'kps_8', 'kps_16', 'kps_32']
sface_in = [i.name for i in ss.get_inputs()][0]

people = sorted(p for p in os.listdir(lfw) if len(os.listdir(os.path.join(lfw, p))) >= 4)
rng = np.random.default_rng(7)
pick = list(rng.choice(len(people), size=min(len(people), (N + 2) // 3), replace=False))
items = []
for pid in pick:
    fs = sorted(os.listdir(os.path.join(lfw, people[pid])))
    for k in range(min(3, len(fs))):
        items.append((int(pid), k, os.path.join(lfw, people[pid], fs[k])))
items = items[:N]

recs = []
for pid, k, f in items:
    img = cv2.imread(f)                                           # 250 x 250 BGR
    # the frame: the face photo scaled to 300 px wide, centred in a 360 x 480 grey frame (a phone held ~30 cm away)
    frame = np.full((FH, FW, 3), 90, np.uint8)
    s = cv2.resize(img, (300, 300), interpolation=cv2.INTER_AREA)
    y0, x0 = 70 + int(rng.integers(-20, 20)), 30 + int(rng.integers(-20, 20))
    frame[y0:y0 + 300, x0:x0 + 300] = s
    xrgb = (0xFF000000 | (frame[:, :, 2].astype(np.uint32) << 16) | (frame[:, :, 1].astype(np.uint32) << 8)
            | frame[:, :, 0].astype(np.uint32))
    small = cv2.resize(frame, (FW // 3, FH // 3), interpolation=cv2.INTER_AREA)
    # YuNet's raw input: the small frame padded to 128 wide
    yin = np.zeros((YH, YW, 3), np.float32)
    yin[:FH // 3, :FW // 3] = small
    outs = ys.run(onames, {'input': yin.transpose(2, 0, 1)[None]})
    _, faces = det.detect(small)
    best = np.zeros(15, np.float32)
    crop = np.zeros((112, 112, 3), np.uint8)
    emb = np.zeros(128, np.float32)
    if faces is not None and len(faces):
        fbest = faces[np.argmax(faces[:, 14])].copy()
        fbest[:14] *= 3.0                                         # back to the full frame
        best = fbest.astype(np.float32)
        crop = rec.alignCrop(frame, fbest)
        blob = cv2.dnn.blobFromImage(crop, 1.0, (112, 112), (0, 0, 0), True, False)
        emb = ss.run(None, {sface_in: blob})[0].reshape(-1).astype(np.float32)
    r = struct.pack('<II', pid, k) + yin.astype('<f4').tobytes()
    r += b''.join(np.asarray(o, '<f4').reshape(-1).tobytes() for o in outs)
    r += xrgb.astype('<u4').tobytes() + best.astype('<f4').tobytes() + crop.tobytes() + emb.astype('<f4').tobytes()
    recs.append(r)
with open(out, 'wb') as fo:
    fo.write(b'FREF' + struct.pack('<I', len(recs)))
    for r in recs:
        fo.write(r)
print('face-ref: %d records -> %s' % (len(recs), out))
