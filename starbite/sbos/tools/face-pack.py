#!/usr/bin/env python3
"""Face Unlock's model pack (kern/facenet.h): YuNet (face + 5 points) and SFace (a 128-D face embedding).

    python3 starbite/sbos/tools/face-pack.py [OUT]          (default OUT: build/face/sbface.pak in this checkout)

Needs numpy and onnx (a venv: python3.14 -m venv v && v/bin/pip install numpy onnx). Downloads the two ONNX files from
OpenCV Zoo once (pinned sha256, kept in build/face/dl/), then:
  - folds every BatchNormalization into the conv / fully connected layer it follows or feeds, fuses Relu / PRelu;
  - pads channels to a multiple of 4 (the runtime's NHWC tensors), reorders SFace's fully connected weights from
    NCHW to NHWC flattening;
  - 1x1 convs and the fully connected layer as int8 with one scale per output channel (symmetric), the rest float32;
  - gives each tensor a slot (reused once its last reader has run), so the runtime needs only the slots' memory.
Licences: YuNet MIT (Shiqi Yu, libfacedetection / OpenCV Zoo), SFace Apache-2.0 (OpenCV Zoo); both texts are copied
next to the pack (sbface.LICENSES.txt). The pack never goes into git (9.7 MB): build.sh finds it like the sbtk pack.
"""
import hashlib, os, struct, sys, urllib.request

ZOO = 'https://media.githubusercontent.com/media/opencv/opencv_zoo/main/models/'
FILES = {
    'yunet': ('face_detection_yunet/face_detection_yunet_2023mar.onnx',
              '8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4'),
    'sface': ('face_recognition_sface/face_recognition_sface_2021dec.onnx',
              '0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79'),
}
LICS = [('YuNet (MIT)', 'https://raw.githubusercontent.com/opencv/opencv_zoo/main/models/face_detection_yunet/LICENSE'),
        ('SFace (Apache-2.0)', 'https://raw.githubusercontent.com/opencv/opencv_zoo/main/models/face_recognition_sface/LICENSE')]

OP_CONV3, OP_PW, OP_DW, OP_POOL, OP_UPADD, OP_FC = 1, 2, 3, 4, 5, 6
ACT_NONE, ACT_RELU, ACT_PRELU = 0, 1, 2
MAGIC = b'SBFACE1\0'
VERSION = 1
OPSZ = 36                     # struct fn_op in facenet.h
NETHDR = 96                   # struct fn_net in facenet.h


def c4(n):
    return (n + 3) // 4 * 4


def fetch(dl, key):
    path, sha = FILES[key]
    dst = os.path.join(dl, os.path.basename(path))
    if not (os.path.exists(dst) and hashlib.sha256(open(dst, 'rb').read()).hexdigest() == sha):
        print('face-pack: downloading', path)
        data = urllib.request.urlopen(ZOO + path, timeout=120).read()
        if hashlib.sha256(data).hexdigest() != sha:
            sys.exit('face-pack: %s: the sha256 is not the pinned one' % path)
        open(dst, 'wb').write(data)
    return dst


class Net:
    """One network: its ops (dicts) and the float/int8 arrays they point at."""

    def __init__(self, name):
        self.name, self.ops, self.prod, self.outs = name, [], {}, []
        self.mean, self.scale, self.bgr = 0.0, 1.0, 1

    def add(self, op, out):
        op['out_name'] = out
        self.ops.append(op)
        self.prod[out] = len(self.ops) - 1


def bn_ab(inits, n):
    import numpy as np
    from onnx import helper
    eps = 1e-5
    for a in n.attribute:
        if a.name == 'epsilon':
            eps = helper.get_attribute_value(a)
    g, b, m, v = (inits[x].astype(np.float64) for x in n.input[1:5])
    s = g / np.sqrt(v + eps)
    return s, b - m * s


def convert(path, name):
    import numpy as np
    import onnx
    from onnx import helper, numpy_helper
    model = onnx.load(path)
    g = model.graph
    inits = {i.name: numpy_helper.to_array(i) for i in g.initializer}
    net = Net(name)
    alias = {}
    inp = [i.name for i in g.input if i.name not in inits][0]
    alias[inp] = inp
    net.prod[inp] = -1                          # slot 0: the input
    shape_c = {inp: 3}
    post = {}                                   # tensor -> (scale, shift) still to apply (BN after an activation)

    def src(t):
        return alias.get(t, t)

    consumers = {}
    for n in g.node:
        for i in n.input:
            consumers.setdefault(i, []).append(n)
    outputs = [o.name for o in g.output]
    head_of = {}                                # final output name -> the tensor (a conv's output) it comes from

    for n in g.node:
        at = {a.name: helper.get_attribute_value(a) for a in n.attribute}
        t = n.op_type
        if t in ('Sub', 'Mul') and src(n.input[0]) == inp:
            v = float(inits[n.input[1]].reshape(-1)[0])
            if t == 'Sub':
                net.mean = v
            else:
                net.scale = v
            alias[n.output[0]] = inp
            net.bgr = 0                         # SFace: OpenCV feeds it RGB (blobFromImage swapRB)
            continue
        if t == 'Conv':
            x = src(n.input[0])
            w = inits[n.input[1]].astype(np.float64)
            b = inits[n.input[2]].astype(np.float64) if len(n.input) > 2 else np.zeros(w.shape[0])
            grp = at.get('group', 1)
            k = at['kernel_shape'][0]
            st = at['strides'][0]
            cout, cin_g = w.shape[0], w.shape[1]
            cin = cin_g * grp
            assert at.get('pads', [0] * 4) == ([1] * 4 if k == 3 else [0] * 4), (n.name, at)
            if k == 1 and grp == 1:
                typ = OP_PW
            elif k == 3 and grp == cin == cout:
                typ = OP_DW
            elif k == 3 and grp == 1:
                typ = OP_CONV3
            else:
                raise SystemExit('face-pack: %s: a conv this runtime has no kernel for (k %d group %d)' % (n.name, k, grp))
            assert x not in post
            op = dict(type=typ, act=ACT_NONE, stride=st, cin=cin, cout=cout, w=w, b=b, inp=x, in2=None, a=None)
            net.add(op, n.output[0])
            shape_c[n.output[0]] = cout
            continue
        if t == 'BatchNormalization':
            x = src(n.input[0])
            s, sh = bn_ab(inits, n)
            p = net.prod.get(x, -1)
            if p >= 0 and net.ops[p]['type'] in (OP_PW, OP_DW, OP_CONV3, OP_FC) and net.ops[p]['act'] == ACT_NONE \
                    and len(consumers.get(n.input[0], [])) == 1:
                op = net.ops[p]
                op['w'] = op['w'] * s.reshape((-1,) + (1,) * (op['w'].ndim - 1))
                op['b'] = op['b'] * s + sh
                alias[n.output[0]] = x
            else:
                post[n.output[0]] = (s, sh, x)  # BN on an activation's output: folded into its reader
                alias[n.output[0]] = n.output[0]
                shape_c[n.output[0]] = shape_c[x]
            continue
        if t in ('Relu', 'PRelu'):
            x = src(n.input[0])
            op = net.ops[net.prod[x]]
            assert op['act'] == ACT_NONE and len(consumers.get(n.input[0], [])) == 1
            if t == 'Relu':
                op['act'] = ACT_RELU
            else:
                op['act'] = ACT_PRELU
                op['a'] = inits[n.input[1]].reshape(-1).astype(np.float64)
            alias[n.output[0]] = x
            continue
        if t in ('Dropout', 'Flatten'):
            alias[n.output[0]] = src(n.input[0])
            continue
        if t == 'Gemm':
            x = src(n.input[0])
            assert at.get('transB') == 1
            w = inits[n.input[1]].astype(np.float64)      # [out, C*H*W] in NCHW order
            b = inits[n.input[2]].astype(np.float64)
            pre = None
            if x in post:
                s, sh, x0 = post[x]
                pre = (s, sh)
                x = x0
            c = shape_c[x]
            hw = w.shape[1] // c
            assert hw * c == w.shape[1]
            w = w.reshape(w.shape[0], c, hw)
            if pre is not None:                 # y = W (s x + sh) + b
                b = b + (w * pre[1].reshape(1, c, 1)).sum(axis=(1, 2))
                w = w * pre[0].reshape(1, c, 1)
            w = w.transpose(0, 2, 1)            # [out, HW, C]: NHWC flattening
            wp = np.zeros((w.shape[0], hw, c4(c)))
            wp[:, :, :c] = w
            op = dict(type=OP_FC, act=ACT_NONE, stride=1, cin=c, cout=w.shape[0], w=wp.reshape(w.shape[0], -1), b=b,
                      inp=x, in2=None, a=None, hw=hw)
            net.add(op, n.output[0])
            shape_c[n.output[0]] = w.shape[0]
            continue
        if t == 'MaxPool':
            assert at['kernel_shape'] == [2, 2] and at['strides'] == [2, 2] and not at.get('ceil_mode', 0)
            x = src(n.input[0])
            c = shape_c[x]
            net.add(dict(type=OP_POOL, act=ACT_NONE, stride=2, cin=c, cout=c, w=None, b=None, inp=x, in2=None, a=None),
                    n.output[0])
            shape_c[n.output[0]] = c
            continue
        if t == 'Resize':
            assert at.get('mode') == b'nearest'
            alias[n.output[0]] = ('up', src(n.input[0]))
            continue
        if t == 'Add':
            a0, a1 = src(n.input[0]), src(n.input[1])
            if isinstance(a0, tuple):
                a0, a1 = a1, a0
            assert isinstance(a1, tuple) and a1[0] == 'up'
            c = shape_c[a0]
            net.add(dict(type=OP_UPADD, act=ACT_NONE, stride=1, cin=c, cout=c, w=None, b=None, inp=a0, in2=a1[1],
                         a=None), n.output[0])
            shape_c[n.output[0]] = c
            continue
        if t in ('Transpose', 'Reshape', 'Sigmoid'):
            alias[n.output[0]] = src(n.input[0])
            continue
        raise SystemExit('face-pack: %s: op %s is not handled' % (name, t))
    for o in outputs:
        x = src(o)
        assert x in net.prod and net.prod[x] >= 0, o
        net.outs.append((o, x))
    return net


def plan_slots(net):
    """Slots: 0 the input; each op's output gets a free slot; a slot frees after its tensor's last reader."""
    last = {}
    for i, op in enumerate(net.ops):
        for x in (op['inp'], op['in2']):
            if x is not None:
                last[x] = i
    keep = {x for _, x in net.outs}
    slot_of = {next(k for k, v in net.prod.items() if v == -1): 0}
    free, nslots = [], 1
    for i, op in enumerate(net.ops):
        # inputs whose last reader is this op are freed after it: the output never shares a slot with its inputs
        if free:
            s = free.pop(0)
        else:
            s = nslots
            nslots += 1
        slot_of[op['out_name']] = s
        for x in (op['inp'], op['in2']):
            if x is not None and last.get(x) == i and x not in keep and slot_of[x] != 0 and slot_of[x] not in free:
                free.append(slot_of[x])
        free.sort()
    return slot_of, nslots


def q8(w):
    import numpy as np
    w2 = w.reshape(w.shape[0], -1)
    m = np.abs(w2).max(axis=1)
    s = np.where(m > 0, m / 127.0, 1.0)
    q = np.clip(np.rint(w2 / s[:, None]), -127, 127).astype(np.int8)
    return q, s.astype(np.float32)


FC_GROUP = 64                 # facenet.h FN_FC_GROUP


def q4(w, g):
    """signed 4-bit (-7..7) with one float scale per g inputs of a row; two weights a byte, the first in the low nibble"""
    import numpy as np
    assert w.shape[1] % g == 0
    wr = w.reshape(w.shape[0], -1, g)
    s = np.abs(wr).max(axis=2, keepdims=True) / 7.0
    s[s == 0] = 1.0
    q = np.clip(np.rint(wr / s), -7, 7).astype(np.int8).reshape(w.shape[0], -1)
    nib = (q & 0xF).astype(np.uint8)
    packed = nib[:, 0::2] | (nib[:, 1::2] << 4)
    return packed, s.reshape(w.shape[0], -1).astype(np.float32)


def pack(nets, out):
    import numpy as np
    blob = bytearray()

    def put(arr):
        nonlocal blob
        while len(blob) % 16:
            blob.append(0)
        off = len(blob)
        blob += arr.tobytes()
        return off

    hdr_len = 64
    blob += b'\0' * hdr_len
    net_offs = []
    for net in nets:
        slot_of, nslots = plan_slots(net)
        recs = []
        for op in net.ops:
            cinp, coutp = c4(op['cin']), c4(op['cout'])
            woff = wsoff = boff = aoff = 0
            wbits = 0
            t = op['type']
            if t == OP_PW and net.name == 'yunet':
                # YuNet's 1 x 1 layers are small (16..64 channels): float, as trained (ws 0 = float weights)
                w = np.zeros((coutp, cinp), np.float32)
                w[:op['cout'], :op['cin']] = op['w'].reshape(op['cout'], op['cin'])
                woff = put(w)
            elif t == OP_PW:
                w = np.zeros((coutp, cinp))
                w[:op['cout'], :op['cin']] = op['w'].reshape(op['cout'], op['cin'])
                q, s = q8(w)
                woff, wsoff = put(q), put(s)
            elif t == OP_FC:
                # SFace's 128 x 50176 layer (two thirds of the weights): 4 bits with a scale per 64 (FC_GROUP)
                # inputs; on LFW the embedding stays at cosine >= 0.998 to fp32 and the pair accuracy is unchanged
                w = np.zeros((coutp, op['w'].shape[1]))
                w[:op['cout']] = op['w']
                q, s = q4(w, FC_GROUP)
                woff, wsoff = put(q), put(s)
                wbits = 4
            elif t == OP_DW:
                w = np.zeros((9, coutp), np.float32)
                w[:, :op['cout']] = op['w'].reshape(op['cout'], 9).T
                woff = put(w)
            elif t == OP_CONV3:
                w = np.zeros((3, 3, op['cin'], coutp), np.float32)      # [ky][kx][ci][co]
                w[:, :, :, :op['cout']] = op['w'].transpose(2, 3, 1, 0)
                woff = put(w)
            if op['b'] is not None:
                b = np.zeros(coutp, np.float32)
                b[:op['cout']] = op['b']
                boff = put(b)
            if op['act'] == ACT_PRELU:
                a = np.zeros(coutp, np.float32)
                a[:op['cout']] = op['a']
                aoff = put(a)
            in2 = slot_of[op['in2']] if op['in2'] is not None else 0xFFFF
            recs.append(struct.pack('<BBBBHHHHHHHHIIII', t, op['act'], op['stride'], wbits, slot_of[op['inp']], in2,
                                    slot_of[op['out_name']], op['cin'], op['cout'], cinp, coutp, op.get('hw', 0),
                                    woff, wsoff, boff, aoff))
        while len(blob) % 16:
            blob.append(0)
        noff = len(blob)
        outs = [slot_of[x] for _, x in net.outs] + [0xFFFF] * (16 - len(net.outs))
        oc = [net.ops[net.prod[x]]['cout'] for _, x in net.outs] + [0] * (16 - len(net.outs))
        h = struct.pack('<IIII', len(net.ops), nslots, len(net.outs), net.bgr) + struct.pack('<ff', net.mean, net.scale)
        h += struct.pack('<16H', *outs) + struct.pack('<16H', *oc)
        h += b'\0' * (NETHDR - len(h))
        assert len(h) == NETHDR
        blob += h
        for r in recs:
            assert len(r) == OPSZ
            blob += r
        net_offs.append(noff)
        print('face-pack: %s: %d ops, %d slots, outputs %s' % (net.name, len(net.ops), nslots,
                                                               ', '.join(o for o, _ in net.outs)))
    struct.pack_into('<8sIII', blob, 0, MAGIC, VERSION, len(nets), len(blob))
    for i, o in enumerate(net_offs):
        struct.pack_into('<I', blob, 20 + 4 * i, o)
    digest = hashlib.sha256(bytes(blob[64:])).digest()
    blob[32:64] = digest
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    open(out, 'wb').write(blob)
    open(out + '.sha256', 'w').write('%s  %s\n' % (hashlib.sha256(blob).hexdigest(), os.path.basename(out)))
    print('face-pack: %s: %d bytes' % (out, len(blob)))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, '..', '..', '..'))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, 'build', 'face', 'sbface.pak')
    dl = os.path.join(os.path.dirname(os.path.abspath(out)), 'dl')
    os.makedirs(dl, exist_ok=True)
    nets = [convert(fetch(dl, 'yunet'), 'yunet'), convert(fetch(dl, 'sface'), 'sface')]
    pack(nets, out)
    lic = []
    for title, url in LICS:
        try:
            lic.append('==== %s: %s ====\n%s' % (title, url, urllib.request.urlopen(url, timeout=60).read().decode()))
        except Exception as e:  # the pack is still good; the licences' names are in this file's docstring
            lic.append('==== %s: %s (not fetched: %s) ====\n' % (title, url, e))
    open(out[:-4] + '.LICENSES.txt', 'w').write('\n'.join(lic))


if __name__ == '__main__':
    main()
