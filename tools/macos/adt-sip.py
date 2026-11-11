#!/usr/bin/env python3
"""SIP (System Integrity Protection) in a Mac ADT, the way our kernel reads it (bsd/kern/kern_csr.c, CONFIG_CSR_FROM_DT).

  adt-sip.py IN.adt                       print the /chosen/asmb local policy and the csr config csr_bootstrap() makes of it
  adt-sip.py IN.adt OUT.adt [--sip0 V] [--sip1 0|1]
                                          write a copy with /chosen/asmb lp-sip0 = V (default 0x6f) and lp-sip1 (default 1)

On Apple silicon there is no csr-active-config NVRAM variable: iBoot copies the local boot policy into /chosen/asmb, and
csr_bootstrap() (STARTUP TUNABLES, before panic_init) takes:
  lp-sip0  uint64, exactly 8 bytes (another size panics: "unexpected size ... for uint64 property"): the CSR_ALLOW_* bits
  lp-sip1  uint32 bool, exactly 4 bytes (another size panics): CSR_ALLOW_UNAUTHENTICATED_ROOT (0x800), which is only
           ever synthesized from it ("csrutil authenticated-root disable")
CSR_ALLOW_APPLE_INTERNAL (0x10) is dropped unless /chosen internal-use-only-unit or /chosen/manifest-properties iuos is
set, so `csrutil disable`'s CSR_DISABLE_FLAGS 0x7f and 0x6f give the same config here; 0x6f (tools: mkadt.py's iPhone ADT,
inherited by every Mac ADT made from it) is the default. csr_check() also grants CSR_ALLOW_KERNEL_DEBUGGER whenever
UNTRUSTED_KEXTS or APPLE_INTERNAL is set.

Without an asmb node csr_bootstrap() reads lp-sip1 through an uninitialized DTEntry: always write both.
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'ref/ios-herolte/tools'))
import adt     # noqa: E402

CSR = [  # bsd/sys/csr.h
    (0x1, 'UNTRUSTED_KEXTS'), (0x2, 'UNRESTRICTED_FS'), (0x4, 'TASK_FOR_PID'), (0x8, 'KERNEL_DEBUGGER'),
    (0x10, 'APPLE_INTERNAL'), (0x20, 'UNRESTRICTED_DTRACE'), (0x40, 'UNRESTRICTED_NVRAM'),
    (0x80, 'DEVICE_CONFIGURATION'), (0x100, 'ANY_RECOVERY_OS'), (0x200, 'UNAPPROVED_KEXTS'),
    (0x400, 'EXECUTABLE_POLICY_OVERRIDE'), (0x800, 'UNAUTHENTICATED_ROOT'), (0x1000, 'RESEARCH_GUESTS'),
]
CSR_VALID = 0x1fff


def names(v):
    return '|'.join(n for b, n in CSR if v & b) or 'none'


def child(node, name):
    return next((c for c in node.children if c.name == name), None)


def u32(node, key):
    v = node.prop(key) if node else None
    if v is None:
        return None
    if len(v) != 4:
        raise SystemExit(f'{key}: {len(v)} bytes, kern_csr.c panics on anything but 4')
    return struct.unpack('<I', v)[0]


def effective(root):
    """csr_bootstrap() + csr_check()'s debugger rule, for a boot without -restore."""
    chosen = child(root, 'chosen')
    asmb = child(chosen, 'asmb') if chosen else None
    notes = []
    csr = 0
    sip0 = asmb.prop('lp-sip0') if asmb else None
    if sip0 is not None:
        if len(sip0) != 8:
            raise SystemExit(f'lp-sip0: {len(sip0)} bytes, kern_csr.c panics on anything but 8')
        csr = struct.unpack('<Q', sip0)[0] & 0xffffffff
    elif asmb is None:
        notes.append('no /chosen/asmb: lp-sip1 is read through an uninitialized DTEntry')
    mp = child(chosen, 'manifest-properties') if chosen else None
    if not (u32(chosen, 'internal-use-only-unit') or u32(mp, 'iuos')):
        csr &= ~0x10
    elif sip0 is None:
        csr |= 0x10
    osenv = (chosen.prop('osenvironment') or b'').rstrip(b'\0') if chosen else b''
    if osenv in (b'recoveryos', b'darwinos-ramdisk'):
        csr |= 0x2
        notes.append(f'osenvironment {osenv.decode()}: UNRESTRICTED_FS added')
    if u32(chosen, 'csr-allow-device-configuration') or osenv == b'recoveryos':
        csr |= 0x80
    if u32(asmb, 'lp-sip1'):
        csr |= 0x800
    else:
        csr &= ~0x800
    active = csr & CSR_VALID
    checked = active | (0x8 if active & 0x11 else 0)
    return sip0, asmb, active, checked, notes


def report(path, root):
    sip0, asmb, active, checked, notes = effective(root)
    s0 = f'{struct.unpack("<Q", sip0)[0]:#x}' if sip0 is not None else 'absent'
    s1 = u32(asmb, 'lp-sip1')
    print(f'{path}: /chosen/asmb lp-sip0 {s0}, lp-sip1 {s1 if s1 is not None else "absent"}')
    print(f'  csr_get_active_config = {active:#x} ({names(active)})')
    print(f'  csr_check grants        {checked:#x} ({names(checked)})')
    print(f'  SIP for gpu_bundle_find_trusted / rootless (CSR_ALLOW_UNRESTRICTED_FS): '
          f'{"OFF (trusted)" if checked & 0x2 else "ON (restricted)"}')
    for n in notes:
        print('  note: ' + n)
    return checked


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('adt', type=Path)
    ap.add_argument('out', type=Path, nargs='?')
    ap.add_argument('--sip0', type=lambda s: int(s, 0), default=0x6f)
    ap.add_argument('--sip1', type=int, choices=(0, 1), default=1)
    a = ap.parse_args()
    blob = a.adt.read_bytes()
    root, _ = adt.parse(blob)
    assert adt.serialize(root) == blob, f'{a.adt}: does not re-serialize to the same bytes'
    if a.out is None:
        report(a.adt, root)
        return
    chosen = child(root, 'chosen')
    if chosen is None:
        sys.exit(f'{a.adt}: no /chosen')
    asmb = child(chosen, 'asmb')
    if asmb is None:
        phs = []

        def walk(n):
            v = n.prop('AAPL,phandle')
            if v is not None and len(v) == 4:
                phs.append(struct.unpack('<I', v)[0])
            for c in n.children:
                walk(c)
        walk(root)
        asmb = adt.Node()
        asmb.props = [('name', 0, b'asmb\0'), ('AAPL,phandle', 0, struct.pack('<I', max(phs, default=0) + 1))]
        chosen.children.append(asmb)

    def setp(key, val):
        for i, (k, f, _) in enumerate(asmb.props):
            if k == key:
                asmb.props[i] = (k, 0, val)
                return
        asmb.props.append((key, 0, val))
    setp('lp-sip0', struct.pack('<Q', a.sip0))
    setp('lp-sip1', struct.pack('<I', a.sip1))
    out = adt.serialize(root)
    a.out.write_bytes(out)
    print(f'{a.out}: {len(out)} bytes' + (' (byte-identical to the input)' if out == blob else ''))
    report(a.out, adt.parse(out)[0])


if __name__ == '__main__':
    main()
