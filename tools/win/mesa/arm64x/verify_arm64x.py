#!/usr/bin/env python3
"""Static checks of an ARM64X DLL (independent of llvm-readobj).

usage: verify_arm64x.py arm64x.dll [plain-arm64.dll] [--write-ec-view out.dll]

Parses the PE, the CHPE metadata and the ARM64X dynamic value relocations; applies them to get the EC view;
then checks exports, entry points, entry thunks, TLS directory, CRT tables and .pdata order for both views.
Prints FAIL lines and exits 1 if a check fails.
"""
import struct, sys

fails = 0
def check(ok, msg):
    global fails
    print(("ok    " if ok else "FAIL  ") + msg)
    if not ok:
        fails += 1

class PE:
    def __init__(self, data):
        self.d = bytearray(data)
        d = self.d
        self.nt = struct.unpack_from("<I", d, 0x3c)[0]
        assert d[self.nt:self.nt+4] == b"PE\0\0"
        self.machine, nsec = struct.unpack_from("<HH", d, self.nt+4)
        opt_size = struct.unpack_from("<H", d, self.nt+20)[0]
        self.opt = self.nt + 24
        assert struct.unpack_from("<H", d, self.opt)[0] == 0x20b
        self.entry = struct.unpack_from("<I", d, self.opt+16)[0]
        self.base = struct.unpack_from("<Q", d, self.opt+24)[0]
        self.size_of_image = struct.unpack_from("<I", d, self.opt+56)[0]
        self.dirs = [struct.unpack_from("<II", d, self.opt+112+8*i) for i in range(16)]
        self.secs = []
        so = self.opt + opt_size
        for i in range(nsec):
            name = bytes(d[so:so+8]).rstrip(b"\0").decode()
            vsz, va, rsz, raw = struct.unpack_from("<IIII", d, so+8)
            ch = struct.unpack_from("<I", d, so+36)[0]
            self.secs.append((name, va, vsz, raw, rsz, ch))
            so += 40
    def off(self, rva):
        if rva < self.secs[0][1]:
            return rva
        for name, va, vsz, raw, rsz, ch in self.secs:
            if va <= rva < va + max(vsz, rsz):
                if rva - va >= rsz:
                    return None       # bss
                return raw + rva - va
        raise ValueError("rva %#x not mapped" % rva)
    def rd(self, rva, n):
        o = self.off(rva)
        return bytes(n) if o is None else bytes(self.d[o:o+n])
    def u16(self, rva): return struct.unpack("<H", self.rd(rva, 2))[0]
    def u32(self, rva): return struct.unpack("<I", self.rd(rva, 4))[0]
    def u64(self, rva): return struct.unpack("<Q", self.rd(rva, 8))[0]
    def cstr(self, rva):
        o = self.off(rva); e = self.d.index(0, o)
        return bytes(self.d[o:e]).decode()
    def exports(self):
        rva, size = self.dirs[0]
        if not rva: return {}
        nfun, nname, afun, aname, aord = struct.unpack("<IIIII", self.rd(rva+20, 20))
        out = {}
        for i in range(nname):
            name = self.cstr(self.u32(aname+4*i))
            out[name] = self.u32(afun + 4*self.u16(aord+2*i))
        return out
    def imports(self):
        rva, size = self.dirs[1]
        out = set()
        while True:
            ilt, _, _, name, iat = struct.unpack("<IIIII", self.rd(rva, 20))
            if not name: break
            dll = self.cstr(name); t = ilt or iat
            while True:
                v = self.u64(t)
                if not v: break
                out.add(dll + "!" + (("#%d" % (v & 0xffff)) if v >> 63 else self.cstr((v & 0x7fffffff) + 2)))
                t += 8
            rva += 20
        return out
    def loadcfg(self):
        rva, size = self.dirs[10]
        f = {}
        f["rva"] = rva
        f["size"] = self.u32(rva)
        f["GuardCFCheck"] = self.u64(rva+0x70)
        f["GuardFlags"] = self.u32(rva+0x90)
        f["CHPE"] = self.u64(rva+0xc8)
        f["DynRelocOff"] = self.u32(rva+0xe0)
        f["DynRelocSec"] = self.u16(rva+0xe4)
        return f
    def tls(self):
        rva, size = self.dirs[9]
        if not rva: return None
        s, e, idx, cb, zf, ch = struct.unpack("<QQQQII", self.rd(rva, 40))
        return dict(rva=rva, start=s-self.base, end=e-self.base, index=idx-self.base, callbacks=cb-self.base, zerofill=zf)
    def chpe(self):
        va = self.loadcfg()["CHPE"]
        if not va: return None
        r = va - self.base
        names = ["Version", "CodeMap", "CodeMapCount", "CodeRangesToEntryPoints", "RedirectionMetadata",
                 "dispatch_call_no_redirect", "dispatch_ret", "dispatch_call", "dispatch_icall", "dispatch_icall_cfg",
                 "AlternateEntryPoint", "AuxiliaryIAT", "CodeRangesToEntryPointsCount", "RedirectionMetadataCount",
                 "GetX64Info", "SetX64Info", "ExtraRFETable", "ExtraRFETableSize", "dispatch_fptr", "AuxiliaryIATCopy"]
        vals = struct.unpack("<%dI" % len(names), self.rd(r, 4*len(names)))
        c = dict(zip(names, vals))
        cm = []
        for i in range(c["CodeMapCount"]):
            s, l = struct.unpack("<II", self.rd(c["CodeMap"]+8*i, 8))
            cm.append((s & ~3, (s & ~3) + l, ["ARM64", "ARM64EC", "X64"][s & 3]))
        c["map"] = cm
        c["redir"] = dict(struct.unpack("<II", self.rd(c["RedirectionMetadata"]+8*i, 8)) for i in range(c["RedirectionMetadataCount"]))
        return c
    def arm64x_relocs(self):
        lc = self.loadcfg()
        if not lc["DynRelocSec"]: return None
        sec = self.secs[lc["DynRelocSec"]-1]
        r = sec[1] + lc["DynRelocOff"]
        ver, size = struct.unpack("<II", self.rd(r, 8))
        assert ver == 1
        p = r + 8; end = p + size; out = []
        while p < end:
            sym, bsize = struct.unpack("<QI", self.rd(p, 12)); p += 12
            bend = p + bsize
            if sym != 6:
                p = bend; continue
            while p < bend:
                page, blk = struct.unpack("<II", self.rd(p, 8))
                q = p + 8; p += blk
                while q < p:
                    e = self.u16(q); q += 2
                    if e == 0: continue          # padding
                    o = page + (e & 0xfff); t = (e >> 12) & 3; s = e >> 14
                    if t == 0:
                        out.append((o, "zero", 1 << s, 0))
                    elif t == 1:
                        n = 1 << s
                        out.append((o, "value", n, int.from_bytes(self.rd(q, n), "little"))); q += n
                    elif t == 2:
                        v = self.u16(q); q += 2
                        v *= 8 if (s & 2) else 4
                        out.append((o, "delta", 4, -v if (s & 1) else v))
                    else:
                        raise ValueError("reloc type 3")
        return out
    def ec_view(self):
        d = bytearray(self.d)
        for o, t, n, v in self.arm64x_relocs():
            fo = self.off(o)
            if t == "zero":
                d[fo:fo+n] = bytes(n)
            elif t == "value":
                d[fo:fo+n] = v.to_bytes(n, "little")
            else:
                cur = struct.unpack_from("<I", d, fo)[0]
                struct.pack_into("<I", d, fo, (cur + v) & 0xffffffff)
        return PE(d)

def kind(cm, rva):
    for s, e, k in cm:
        if s <= rva < e: return k
    return "none"

def pdata_sorted(pe, rva, size, step):
    prev = -1
    for i in range(0, size, step):
        b = pe.u32(rva + i)
        if b <= prev: return False
        prev = b
    return True

def main():
    args = sys.argv[1:]
    out = None
    if "--write-ec-view" in args:
        i = args.index("--write-ec-view"); out = args[i+1]; del args[i:i+2]
    x = PE(open(args[0], "rb").read())
    plain = PE(open(args[1], "rb").read()) if len(args) > 1 else None

    print("== native view (file as stored)")
    check(x.machine == 0xAA64, "PE machine %#x == 0xAA64 (ARM64)" % x.machine)
    lc = x.loadcfg(); c = x.chpe()
    check(c is not None and c["Version"] == 2, "CHPE metadata present, version 2")
    for s, e, k in c["map"]:
        print("      code map %#9x - %#9x %s" % (s, e, k))
    check([k for _, _, k in c["map"]] == ["ARM64", "ARM64EC", "X64"], "code map has ARM64, ARM64EC and X64 ranges")
    rel = x.arm64x_relocs()
    check(rel is not None and len(rel) > 0, "ARM64X dynamic value relocations: %d entries" % (len(rel) if rel else 0))
    hdr = [(o, t, n, v) for o, t, n, v in rel if o < 0x400]
    for o, t, n, v in hdr:
        print("      header fixup rva %#x %s size %d value %#x" % (o, t, n, v))
    nx = x.exports()
    check(all(kind(c["map"], r) == "ARM64" for r in nx.values()), "all %d native exports point into the ARM64 range" % len(nx))
    check(kind(c["map"], x.entry) == "ARM64", "native entry point %#x is in the ARM64 range" % x.entry)
    for n in ("OpenAdapter10", "OpenAdapter10_2"):
        check(n in nx, "native export %s at %#x" % (n, nx.get(n, 0)))
    # native .pdata: ARM64 entries (8 bytes), sorted
    er, es = x.dirs[3]
    check(pdata_sorted(x, er, es, 8), "native exception directory %#x+%#x sorted (ARM64 entries, covers ARM64 and ARM64EC code)" % (er, es))
    g = x.u64(lc["GuardCFCheck"] - x.base) - x.base
    check(kind(c["map"], g) == "ARM64", "native __guard_check_icall_fptr -> %#x in the ARM64 range" % g)
    t = x.tls()
    sec = [s for s in x.secs if s[0] == ".tls"][0]
    check(t["start"] == sec[1], "TLS template starts at the .tls section start (%#x)" % t["start"])
    check(t["end"] >= sec[1] + sec[2] - 2, "TLS template end %#x covers the section (size %#x) but for the end markers" % (t["end"], sec[2]))
    cb = t["callbacks"]; n = 0
    while x.u64(cb + 8*n): n += 1
    check(n == 0, "TLS callback array at %#x is empty" % cb)
    print("      TLS index variable at rva %#x (loader writes the module's TLS index here)" % t["index"])

    print("== EC view (ARM64X fixups applied)")
    e = x.ec_view()
    if out:
        open(out, "wb").write(e.d)
    check(e.machine == 0x8664, "PE machine %#x == 0x8664" % e.machine)
    ex = e.exports()
    check(sorted(ex) == sorted(nx), "EC view export names == native view export names (%d)" % len(ex))
    ce = e.chpe()
    for n in ("OpenAdapter10", "OpenAdapter10_2"):
        r = ex.get(n, 0)
        k = kind(c["map"], r)
        tgt = ce["redir"].get(r)
        ok = k == "X64" and tgt is not None and kind(c["map"], tgt) == "ARM64EC"
        check(ok, "EC export %s: %#x (%s fast-forward thunk) -> %#x (%s)" % (n, r, k, tgt or 0, kind(c["map"], tgt or 0)))
        if tgt:
            w = e.u32(tgt - 4)
            th = tgt + (w & ~3) if (w & 1) else 0
            check((w & 1) == 1 and kind(c["map"], th) == "ARM64EC", "  entry thunk word %#x before it -> thunk at %#x (ARM64EC)" % (w, th))
    bad = [n for n, r in ex.items() if not (kind(c["map"], r) == "X64" and kind(c["map"], ce["redir"].get(r, 0)) == "ARM64EC")]
    check(not bad, "every EC export is an x64 thunk redirected to ARM64EC code" + (" (bad: %s)" % bad if bad else ""))
    k = kind(c["map"], e.entry); tgt = ce["redir"].get(e.entry, 0)
    check(k in ("X64", "ARM64EC"), "EC entry point %#x (%s) -> %#x (%s)" % (e.entry, k, tgt, kind(c["map"], tgt)))
    te = e.tls()
    check(te == t, "EC view uses the same TLS directory as the native view (index var %#x)" % te["index"])
    er2, es2 = e.dirs[3]
    print("      EC exception directory %#x+%#x; ExtraRFETable %#x+%#x" % (er2, es2, ce["ExtraRFETable"], ce["ExtraRFETableSize"]))
    check((ce["ExtraRFETable"], ce["ExtraRFETableSize"]) == (er, es), "ExtraRFETable == the ARM64 .pdata")
    for nm in ("dispatch_call_no_redirect", "dispatch_ret", "dispatch_call", "dispatch_icall", "dispatch_icall_cfg", "dispatch_fptr"):
        check(ce[nm] != 0, "__os_arm64x_%s slot at %#x" % (nm, ce[nm]))
    check(ce["AuxiliaryIAT"] != 0 and ce["AuxiliaryIATCopy"] != 0, "auxiliary IAT %#x, copy %#x" % (ce["AuxiliaryIAT"], ce["AuxiliaryIATCopy"]))

    if plain:
        print("== native view against the plain ARM64 build")
        px = plain.exports()
        check(sorted(px) == sorted(nx), "export names equal (%d)" % len(px))
        pi, ni = plain.imports(), x.imports()
        check(pi == ni, "imports equal (%d symbols)" % len(pi) + ("" if pi == ni else " only-plain %s only-x %s" % (sorted(pi-ni), sorted(ni-pi))))
        ei = e.imports()
        print("      EC view imports: %d symbols; not in native: %s" % (len(ei), sorted(ei - ni)))
        print("      native-only imports: %s" % sorted(ni - ei))
        pt = plain.tls()
        ps = [s for s in plain.secs if s[0] == ".tls"][0]
        check(pt["start"] == ps[1], "plain build: TLS template also starts at its .tls section start")
        n = pt["end"] - pt["start"]
        same = plain.rd(pt["start"], n) == x.rd(t["start"], n)
        print("      first %#x bytes of the TLS template %s the plain build's" % (n, "equal" if same else "differ from"))
        nz = sum(1 for b in x.rd(t["start"], t["end"] - t["start"]) if b)
        print("      non-zero bytes in the ARM64X TLS template: %d (plain: %d)" % (nz, sum(1 for b in plain.rd(pt["start"], n) if b)))
        check(plain.loadcfg()["size"] == lc["size"], "load config size equal (%#x)" % lc["size"])
        print("      GuardFlags plain %#x, ARM64X native %#x" % (plain.loadcfg()["GuardFlags"], lc["GuardFlags"]))
        pc = {s[0]: s[5] for s in plain.secs}; xc = {s[0]: s[5] for s in x.secs}
        check(all(xc.get(k) == v for k, v in pc.items()), "every section of the plain build exists with the same characteristics")
        print("      extra sections: %s" % sorted(set(xc) - set(pc)))
    print("FAILS: %d" % fails)
    sys.exit(1 if fails else 0)

main()
