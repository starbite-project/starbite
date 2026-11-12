#!/usr/bin/env python3
"""Mark finished arm64 Mach-O images as arm64e, so dyld loads them into arm64e processes (WindowServer) on the S9.

  arm64e-tag.py FILE...      in place; re-sign afterwards (codesign -f -s -): the header page is hashed

Why: dyld grades an arm64e process (keys on, i.e. not started with ptrauth_disabled=1) as arm64e-only and refuses
an arm64 image ("incompatible architecture (have 'arm64', need 'arm64e')", checked on macOS 27.2). Our code has no
pointer authentication to offer: the S9's Cortex-A75/A55 have no PAC, and the Sequoia root's a64e rewrite turns
every PAC instruction of Apple's arm64e binaries into its plain form. Plain arm64 code in an image marked arm64e
is therefore exactly what every rewritten binary on that root is. Checked on macOS 27.2: an arm64 dylib marked
this way (cpusubtype 0x80000002, Apple's: arm64e, versioned ptrauth ABI 0) is taken by dlopen in an arm64e
process, its DYLD_CHAINED_PTR_64 fixups applied, and its functions run.

What it does per image (thin 64-bit Mach-O, or the arm64 slice of a fat file):
  - refuses one that holds non-HINT PAC instructions (an image built as arm64e: run tools/a64e/a64e-rewrite.py on
    it instead); HINT-space ones (PACIASP, AUTIASP, XPACLRI ...) are NOPs on the S9 and are allowed
  - refuses one whose binds are dyld-info opcodes (LC_DYLD_INFO[_ONLY] with bind data): dyld rejects them in an
    arm64e image ("old arm64e bind opcodes not supported"; S2g 2026-11-12, PanVK linked with -undefined
    dynamic_lookup): link with -Wl,-fixup_chains
  - cputype ARM64, cpusubtype ARM64_ALL -> 0x80000002 (an arm64e image is left as it is)
"""
import struct
import sys

CPU_TYPE_ARM64 = 0x0100000C
ARM64E = 0x80000002
S_ATTR_PURE_INSTRUCTIONS = 0x80000000
S_ATTR_SOME_INSTRUCTIONS = 0x00000400

# Non-HINT pointer-authentication encodings (mask, value, name): UNDEFINED on ARMv8.2 without FEAT_PAuth.
PAC = [
    (0xFFFFC000, 0xDAC10000, 'PAC*/AUT*/XPAC* (register)'),
    (0xFE1FF800, 0xD61F0800, 'BRA*/BLRA*/RETA*/ERETA*'),
    (0xFF200400, 0xF8200400, 'LDRAA/LDRAB'),
    (0xFFE0FC00, 0x9AC03000, 'PACGA'),
]


def pac_sites(b, off):
    """(count, first name) of non-HINT PAC instructions in the code sections of the thin image at off."""
    magic, ct, st, ft, ncmds, sz, flags, _ = struct.unpack_from('<8I', b, off)
    p, n, first = off + 32, 0, None
    for _ in range(ncmds):
        cmd, csz = struct.unpack_from('<II', b, p)
        if cmd == 0x19:	# LC_SEGMENT_64
            nsects = struct.unpack_from('<I', b, p + 64)[0]
            for i in range(nsects):
                s = p + 72 + i * 80
                size, offset = struct.unpack_from('<QI', b, s + 40)
                sflags = struct.unpack_from('<I', b, s + 64)[0]
                if not sflags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS) or not offset:
                    continue
                words = struct.unpack_from('<%dI' % (size // 4), b, off + offset)
                for w in words:
                    for mask, val, name in PAC:
                        if w & mask == val:
                            n += 1
                            first = first or name
        p += csz
    return n, first


def opcode_binds(b, off):
    """Bytes of bind / weak-bind / lazy-bind opcodes in LC_DYLD_INFO[_ONLY], 0 with chained fixups or none."""
    ncmds = struct.unpack_from('<I', b, off + 16)[0]
    p, n = off + 32, 0
    for _ in range(ncmds):
        cmd, csz = struct.unpack_from('<II', b, p)
        if cmd in (0x22, 0x80000022):	# LC_DYLD_INFO, LC_DYLD_INFO_ONLY
            _, _, _, bsz, _, wsz, _, lsz = struct.unpack_from('<8I', b, p + 8)
            n += bsz + wsz + lsz
        p += csz
    return n


def tag(b, off, path):
    magic, ct, st = struct.unpack_from('<III', b, off)
    if magic != 0xFEEDFACF or ct != CPU_TYPE_ARM64:
        sys.exit(f'{path}: slice at {off:#x} is not a 64-bit arm64 Mach-O')
    if st == ARM64E:
        return 'already arm64e'
    if st & 0xFFFFFF != 0:
        sys.exit(f'{path}: cpusubtype {st:#x}: not arm64 (ARM64_ALL)')
    ob = opcode_binds(b, off)
    if ob:
        sys.exit(f'{path}: {ob} bytes of dyld-info bind opcodes: dyld refuses them in an arm64e image '
                 '("old arm64e bind opcodes not supported"); link with -Wl,-fixup_chains')
    n, first = pac_sites(b, off)
    if n:
        sys.exit(f'{path}: {n} non-HINT PAC instructions ({first} ...): an arm64e build, rewrite it with '
                 'tools/a64e/a64e-rewrite.py instead')
    struct.pack_into('<I', b, off + 8, ARM64E)
    return 'arm64 -> arm64e (0x80000002), 0 PAC sites'


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for path in sys.argv[1:]:
        b = bytearray(open(path, 'rb').read())
        magic = struct.unpack_from('>I', b, 0)[0]
        if magic == 0xCAFEBABE:		# fat: the arm64 slice (an arm64e slice beside it would clash)
            nfat = struct.unpack_from('>I', b, 4)[0]
            arches = [struct.unpack_from('>IIIII', b, 8 + i * 20) for i in range(nfat)]
            if any(ct == CPU_TYPE_ARM64 and st & 0xFFFFFF == 2 for ct, st, _, _, _ in arches):
                sys.exit(f'{path}: fat file with an arm64e slice already')
            msgs = []
            for i, (ct, st, off, size, align) in enumerate(arches):
                if ct == CPU_TYPE_ARM64:
                    msgs.append(tag(b, off, path))
                    struct.pack_into('>I', b, 8 + i * 20 + 4, ARM64E)
            msg = '; '.join(msgs) or 'no arm64 slice'
        else:
            msg = tag(b, 0, path)
        open(path, 'r+b').write(b)
        print(f'{path}: {msg}')


if __name__ == '__main__':
    main()
