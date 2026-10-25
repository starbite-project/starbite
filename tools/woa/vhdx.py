#!/usr/bin/env python3
"""Minimal read-only VHDX reader (dynamic, no parent) for tools/woa/write-windows.sh.
qemu's vhdx driver refuses our 4Kn VHDX ("Operation not supported"), so we read it ourselves.
  vhdx.py info FILE
  vhdx.py present FILE OFF LEN   byte runs inside [OFF, OFF+LEN) backed by payload blocks,
                                 relative to OFF, as "start length" lines (everything else is zero)
  vhdx.py cat FILE OFF LEN       the virtual bytes of [OFF, OFF+LEN) to stdout
  vhdx.py sha FILE OFF LEN       their sha256
"""
import hashlib, struct, sys, uuid

BAT = uuid.UUID('2DC27766-F623-4200-9D64-115E9BFD4A08')
META = uuid.UUID('8B7CA206-4790-4B9A-B8FE-575F050F886E')
FILE_PARAMS = uuid.UUID('CAA16737-FA36-4D43-B3B6-33F0AA44E76B')
DISK_SIZE = uuid.UUID('2FA54224-CD1B-4876-B211-5DBED83BF4B8')
LOGICAL_SECTOR = uuid.UUID('8141BF1D-A96F-4709-BA47-F233A8FAAB5F')
FULLY_PRESENT = 6


class Vhdx:
    def __init__(self, path):
        self.f = open(path, 'rb')
        heads = []
        for off in (0x10000, 0x20000):
            h = self.pread(off, 4096)
            if h[:4] == b'head':
                heads.append((struct.unpack('<Q', h[8:16])[0], h))
        _, h = max(heads)
        if h[48:64] != b'\0' * 16:
            sys.exit('vhdx: log needs replay (not cleanly detached)')
        rt = self.pread(0x30000, 65536)
        assert rt[:4] == b'regi'
        regions = {}
        for i in range(struct.unpack('<I', rt[8:12])[0]):
            e = rt[16 + 32 * i:48 + 32 * i]
            regions[uuid.UUID(bytes_le=e[:16])] = struct.unpack('<QI', e[16:28])
        moff, mlen = regions[META]
        m = self.pread(moff, mlen)
        assert m[:8] == b'metadata'
        items = {}
        for i in range(struct.unpack('<H', m[10:12])[0]):
            e = m[32 + 32 * i:64 + 32 * i]
            o, n = struct.unpack('<II', e[16:24])
            items[uuid.UUID(bytes_le=e[:16])] = m[o:o + n]
        self.block, flags = struct.unpack('<II', items[FILE_PARAMS][:8])
        if flags & 2:
            sys.exit('vhdx: differencing disks are not supported')
        self.size = struct.unpack('<Q', items[DISK_SIZE][:8])[0]
        self.sector = struct.unpack('<I', items[LOGICAL_SECTOR][:4])[0]
        self.chunk = (1 << 23) * self.sector // self.block
        boff, blen = regions[BAT]
        self.bat = self.pread(boff, blen)

    def pread(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def entry(self, i):
        e = struct.unpack('<Q', self.bat[8 * (i + i // self.chunk):8 * (i + i // self.chunk) + 8])[0]
        return e & 7, (e >> 20) << 20

    def present(self, off, n):
        runs = []
        for i in range(off // self.block, (off + n + self.block - 1) // self.block):
            if self.entry(i)[0] != FULLY_PRESENT:
                continue
            s, t = max(i * self.block, off) - off, min((i + 1) * self.block, off + n) - off
            if runs and runs[-1][0] + runs[-1][1] == s:
                runs[-1][1] += t - s
            else:
                runs.append([s, t - s])
        return runs

    def chunks(self, off, n, step=1 << 20):
        while n:
            i, within = divmod(off, self.block)
            k = min(step, n, self.block - within)
            state, foff = self.entry(i)
            yield self.pread(foff + within, k) if state == FULLY_PRESENT else bytes(k)
            off += k
            n -= k


def main():
    cmd, v = sys.argv[1], Vhdx(sys.argv[2])
    if cmd == 'info':
        print(f'size {v.size} block {v.block} sector {v.sector} chunk {v.chunk}')
        return
    off, n = int(sys.argv[3]), int(sys.argv[4])
    if cmd == 'present':
        for s, k in v.present(off, n):
            print(s, k)
    elif cmd == 'cat':
        for c in v.chunks(off, n):
            sys.stdout.buffer.write(c)
    elif cmd == 'sha':
        h = hashlib.sha256()
        for c in v.chunks(off, n):
            h.update(c)
        print(h.hexdigest())


if __name__ == '__main__':
    main()
