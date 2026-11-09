#!/usr/bin/env python3
"""Starbite OS 1.2-alpha6: print the last heating session's log (kern.h struct hlog, the state slot's
"heatlog0".."heatlog4" records) from a pulled store image or an sbos-state extract.
  python3 -I starbite/tools/heatlog.py build/store/ufs-store-liveNN.img [--csv]"""
import struct, sys, zlib

MAGIC_HALF = b"SBOSSTA1"
HLOG_MAGIC = 0x31474C48
HLOG_MAGIC2 = 0x32474C48   # 1.2-alpha18: mA = the second's peak, mV = its lowest
HLOG_N, HLOG_FAST = 1024, 600
REC_MAX = 4096


def halves(data):
    """every valid state half: (seq, {id: bytes})"""
    out = []
    i = data.find(MAGIC_HALF)
    while i >= 0:
        try:
            seq, length, crc = struct.unpack_from("<QII", data, i + 8)
            body = data[i + 24:i + length]
            if 24 <= length <= 512 * 1024 and len(body) == length - 24 and (zlib.crc32(body) & 0xFFFFFFFF) == crc:
                recs, o = {}, 24
                while o + 28 <= length:
                    rid = data[i + o:i + o + 24].split(b"\0")[0].decode("ascii", "replace")
                    n = struct.unpack_from("<I", data, i + o + 24)[0]
                    recs[rid] = data[i + o + 28:i + o + 28 + n]
                    o += 28 + ((n + 7) & ~7)
                out.append((seq, recs))
        except struct.error:
            pass
        i = data.find(MAGIC_HALF, i + 8)
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    data = open(sys.argv[1], "rb").read()
    hs = [h for h in halves(data) if "heatlog0" in h[1]]
    if not hs:
        print("heatlog: no state half with a heat log (alpha6 or later, after a heating session)")
        return 1
    seq, recs = max(hs, key=lambda h: h[0])
    blob = b"".join(recs.get("heatlog%d" % k, b"") for k in range(8))
    magic, n, level, a55, m3n, gpu, cnfg05 = struct.unpack_from("<IIIIHHI", blob, 0)
    if magic not in (HLOG_MAGIC, HLOG_MAGIC2):
        print("heatlog: bad magic %#x" % magic)
        return 1
    print("heatlog: state sequence %d, level %d, %d samples, A55 %d MHz, %d M3 cores, GPU %d MHz" % (seq, level, n, a55, m3n, gpu))
    if magic == HLOG_MAGIC2:
        print("heatlog: (alpha18+) mA is each second's peak discharge, mV its lowest cell voltage")
    if cnfg05 & 0x100:
        names = {0: "off", 6: "4.5 A", 8: "4.8 A", 9: "5.0 A", 0xB: "5.4 A", 0xC: "5.6 A"}
        print("heatlog: MAX77705 CHG_CNFG_05 %#04x: B2SOVRC (battery to system over-current) %s" %
              (cnfg05 & 0xFF, names.get(cnfg05 & 0xF, "code %d" % (cnfg05 & 0xF))))
    samples = []
    for i in range(min(n, HLOG_N)):
        samples.append(struct.unpack_from("<HhhhHHBBBB", blob, 24 + 16 * i))
    if n > HLOG_N:   # the slow ring wrapped: oldest first
        k = HLOG_FAST + (n - HLOG_FAST) % (HLOG_N - HLOG_FAST)
        samples = samples[:HLOG_FAST] + samples[k:] + samples[HLOG_FAST:k]
    csv = "--csv" in sys.argv
    print("t_s,soc_c,bat_c,ma,mv,w,m3_mhz,cores,gpu,trip_off,white,hold,pause,ocp_d3,thr" if csv else
          "   t   SoC C  bat C     mA     mV     W   M3  cores flags        OCP_WARN1 thr")
    for t, soc, bat, ma, mv, m3, cores, fl, d3, thr in samples:
        w = -ma * mv / 1e6
        if csv:
            print("%d,%.1f,%.1f,%d,%d,%.2f,%d,%d,%d,%d,%d,%d,%d,%#04x,%s" % (
                t, soc / 10, bat / 10, ma, mv, w, m3, cores, fl & 1, fl >> 1 & 1, fl >> 2 & 1, fl >> 3 & 1, fl >> 4 & 1,
                d3, thr if fl & 32 else ""))
        else:
            f = "".join(c if fl & b else "-" for c, b in zip("GTWHP", (1, 2, 4, 8, 16)))
            print("%5d %6.1f %6.1f %6d %6d %5.2f %4d %5d  %s       %#04x    %s" % (
                t, soc / 10, bat / 10, ma, mv, w, m3, cores, f, d3, thr if fl & 32 else "-"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
