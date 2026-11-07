#!/usr/bin/env python3
"""Decode an ETW trace of the Windows USB function stack (docs/WIN11-USB.md section 5.2).

  ufx-etl.py manifest DRIVER.sys                 the driver's events: id, level, message, fields
  ufx-etl.py decode TRACE.etl DRIVER.sys [...]   every event of those drivers' providers, decoded

The drivers carry their own event manifest (resource WEVT_TEMPLATE: CRIM > WEVT provider > EVNT
events + TTBL templates) and message table (RT_MESSAGETABLE). This reads both straight from the
.sys files (copied read-only from the phone: build/woa/usb-inf/ of the main checkout), so no
Windows is needed. Layout of the manifest after libfwevt's documentation of the format. Fields of
types this doesn't know (structures) end the decoding of that event; the rest is shown as hex.
The ETL container is read with the etl-parser package (pip install etl-parser==1.0.1, in a venv).
"""
import struct
import sys
import uuid


# ---- PE resources ----------------------------------------------------------------------------

def pe_sections(d):
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    opt = struct.unpack_from('<H', d, pe + 20)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + opt + 40 * i
        name = d[o:o + 8].rstrip(b'\0').decode(errors='replace')
        vsize, va, rsize, raw = struct.unpack_from('<IIII', d, o + 8)
        secs.append((name, va, max(vsize, rsize), raw))
    magic = struct.unpack_from('<H', d, pe + 24)[0]
    ddir = pe + 24 + (112 if magic == 0x20B else 96)
    res_rva = struct.unpack_from('<I', d, ddir + 8 * 2)[0]
    return secs, res_rva


def rva2off(secs, rva):
    for _, va, size, raw in secs:
        if va <= rva < va + size:
            return rva - va + raw
    raise ValueError(f'rva {rva:#x} outside sections')


def resource(d, rtype):
    """The first data blob of a resource type (numeric id or name), as bytes."""
    secs, res_rva = pe_sections(d)
    base = rva2off(secs, res_rva)

    def entries(off):
        named, ids = struct.unpack_from('<HH', d, off + 12)
        for i in range(named + ids):
            name, target = struct.unpack_from('<II', d, off + 16 + 8 * i)
            if name & 0x80000000:
                o = base + (name & 0x7FFFFFFF)
                n = struct.unpack_from('<H', d, o)[0]
                key = d[o + 2:o + 2 + 2 * n].decode('utf-16le')
            else:
                key = name
            yield key, target

    for key, target in entries(base):
        if key != rtype:
            continue
        o = target
        while o & 0x80000000:                     # descend into the first entry of each level
            o = next(entries(base + (o & 0x7FFFFFFF)))[1]
        rva, size = struct.unpack_from('<II', d, base + o)
        off = rva2off(secs, rva)
        return d[off:off + size]
    return None


def messages(d):
    blob = resource(d, 11)                        # RT_MESSAGETABLE
    out = {}
    if not blob:
        return out
    nblocks = struct.unpack_from('<I', blob, 0)[0]
    for b in range(nblocks):
        low, high, off = struct.unpack_from('<III', blob, 4 + 12 * b)
        for mid in range(low, high + 1):
            length, flags = struct.unpack_from('<HH', blob, off)
            text = blob[off + 4:off + length]
            out[mid] = (text.decode('utf-16le') if flags & 1 else text.decode('latin-1')).rstrip('\0\r\n ')
            off += length
    return out


# ---- the WEVT manifest -----------------------------------------------------------------------

def utf16z(b, o):
    e = o
    while b[e:e + 2] != b'\0\0':
        e += 2
    return b[o:e].decode('utf-16le')


def manifest(path):
    d = open(path, 'rb').read()
    w = resource(d, 'WEVT_TEMPLATE')
    msgs = messages(d)
    assert w[:4] == b'CRIM', 'no CRIM'
    nprov = struct.unpack_from('<I', w, 12)[0]
    provs = {}
    for p in range(nprov):
        guid = uuid.UUID(bytes_le=w[16 + 20 * p:32 + 20 * p])
        po = struct.unpack_from('<I', w, 32 + 20 * p)[0]
        assert w[po:po + 4] == b'WEVT'
        ndesc = struct.unpack_from('<I', w, po + 12)[0]
        events, templates = {}, {}
        for i in range(ndesc):
            eo = struct.unpack_from('<I', w, po + 20 + 8 * i)[0]
            sig = w[eo:eo + 4]
            if sig == b'TTBL':
                n = struct.unpack_from('<I', w, eo + 8)[0]
                t = eo + 12
                for _ in range(n):
                    assert w[t:t + 4] == b'TEMP'
                    tsize, ndescr, _nnames, inst_off = struct.unpack_from('<IIII', w, t + 4)
                    fields = []
                    for k in range(ndescr):
                        fo = inst_off + 20 * k
                        in_type, out_type = w[fo + 4], w[fo + 5]
                        count, vsize, name_off = struct.unpack_from('<HHI', w, fo + 12)
                        fields.append((utf16z(w, name_off + 4), in_type, count, vsize))
                    templates[t] = fields
                    t += tsize
        for i in range(ndesc):
            eo = struct.unpack_from('<I', w, po + 20 + 8 * i)[0]
            if w[eo:eo + 4] == b'EVNT':
                n = struct.unpack_from('<I', w, eo + 8)[0]
                for k in range(n):
                    o = eo + 16 + 48 * k
                    eid, ver, chan, lvl, opc, task, kw, mid, toff = struct.unpack_from('<HBBBBHQII', w, o)
                    events[(eid, ver)] = dict(level=lvl, message=msgs.get(mid, f'(message {mid:#x})'),
                                              fields=templates.get(toff, []))
        provs[guid] = dict(file=path, events=events)
    return provs


# ---- payload decoding ------------------------------------------------------------------------

def decode(fields, data):
    vals, o = [], 0
    for name, t, count, vsize in fields:
        if count not in (0, 1):
            break                                 # arrays: not decoded
        try:
            if t == 1:                            # UnicodeString (or a structure: then stop)
                s = utf16z(data, o)
                if not s.isprintable():
                    break
                o += 2 * len(s) + 2
                v = s
            elif t == 2:                          # AnsiString
                e = data.index(b'\0', o)
                v = data[o:e].decode('latin-1')
                o = e + 1
            elif t in (3, 4):
                v = data[o]
                o += 1
            elif t in (5, 6):
                v = struct.unpack_from('<H', data, o)[0]
                o += 2
            elif t in (7, 8, 13):                 # Int32, UInt32, Boolean
                v = struct.unpack_from('<I', data, o)[0]
                o += 4
            elif t == 20:                         # HexInt32
                v = f'{struct.unpack_from("<I", data, o)[0]:#010x}'
                o += 4
            elif t in (9, 10, 17):                # Int64, UInt64, FILETIME
                v = struct.unpack_from('<Q', data, o)[0]
                o += 8
            elif t in (16, 21):                   # Pointer, HexInt64
                v = f'{struct.unpack_from("<Q", data, o)[0]:#x}'
                o += 8
            elif t == 15:
                v = str(uuid.UUID(bytes_le=data[o:o + 16]))
                o += 16
            elif t == 14 and vsize:
                v = data[o:o + vsize].hex()
                o += vsize
            else:
                break                             # structures and the like
        except (struct.error, ValueError, IndexError):
            break
        vals.append((name, v))
    rest = data[o:].hex() if o < len(data) else ''
    return vals, rest


def fmt(msg, vals):
    s = msg
    for i, (_, v) in enumerate(vals, 1):
        s = s.replace(f'%{i}', str(v))
    return s


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == 'manifest':
        for g, p in manifest(sys.argv[2]).items():
            print(f'provider {g} ({p["file"]}): {len(p["events"])} events')
            for (eid, ver), e in sorted(p['events'].items()):
                print(f'  {eid:5d} v{ver} L{e["level"]} {e["message"]!r} '
                      f'[{", ".join(f"{n}:{t}" for n, t, _, _ in e["fields"])}]')
        return
    if len(sys.argv) >= 4 and sys.argv[1] == 'decode':
        from etl.etl import build_from_stream, IEtlFileObserver
        provs = {}
        for drv in sys.argv[3:]:
            provs.update(manifest(drv))

        class Obs(IEtlFileObserver):
            def on_event_record(self, ev):
                h = ev.source.event_header
                p = h.provider_id
                g = uuid.UUID(fields=(p.data1, p.data2, p.data3, p.data4[0], p.data4[1],
                                      int.from_bytes(bytes(p.data4[2:]), 'big')))
                if g not in provs:
                    return
                d = h.event_descriptor
                e = provs[g]['events'].get((d.Id, d.Version))
                data = bytes(ev.source.user_data)
                if not e:
                    print(f'{h.timestamp} {g} id {d.Id} v{d.Version}: {data.hex()}')
                    return
                vals, rest = decode(e['fields'], data)
                line = fmt(e['message'], vals) if e['message'] else ''
                print(f'{h.timestamp} id {d.Id:4d} L{e["level"]} {line} | '
                      + ', '.join(f'{n}={v}' for n, v in vals) + (f' | +{rest}' if rest else ''))

            def on_trace_record(self, ev): pass
            def on_perfinfo_trace(self, ev): pass
            def on_system_trace(self, ev): pass
            def on_win_trace(self, ev): pass

        build_from_stream(open(sys.argv[2], 'rb').read()).parse(Obs())
        return
    sys.exit(__doc__)


if __name__ == '__main__':
    main()
