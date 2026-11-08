#!/usr/bin/env python3
"""Make an (unsigned) Windows driver catalog (.cat) on the Mac, like the WDK's Inf2Cat.

  makecat.py OUT.cat FILE... [--os _v100_ARM64_22H2,...] [--hwid acpi\\xyz ...] [--date YYMMDDhhmmss]

Then sign it: tools/win/sign.sh OUT.cat. A catalog is only needed when Windows itself installs a driver
package from its INF (pnputil /add-driver, DISM, Device Manager); the kernel loads a .sys with an embedded
signature without one (tools/win/README.md).

Format copied from an Inf2Cat catalog (build/woa/ufs-sys/Exynos9810Ufs.cat, the woa project's): a PKCS#7
SignedData whose content is a certificate trust list (szOID_CTL 1.3.6.1.4.1.311.10.1), list type
szOID_CATALOG_LIST, members of type CATALOG_LIST_MEMBER2 (1.3.6.1.4.1.311.12.1.3). Each file gets two members,
tagged by its SHA-1 and by its SHA-256 hash (flat hash for INF and other files, Authenticode hash for PE files),
with the attributes memberinfo2, OSAttr "2:10.0", File "<lower-case name>"; the SHA-256 member also carries
SpcIndirectData (SpcPeImageData or the flat-file link) with the hash. Catalog attributes: OS, HWID1..n.
Members are sorted by tag, SET OF by DER encoding. `--selftest` rebuilds the reference catalog's member list
from Exynos9810Ufs.inf/.sys and compares it byte for byte. No dependencies.
"""
import hashlib
import os
import struct
import sys
import time


# --- minimal DER -------------------------------------------------------------------------------------------
def tlv(tag, body):
    n = len(body)
    if n < 0x80:
        ln = bytes([n])
    else:
        b = n.to_bytes((n.bit_length() + 7) // 8, 'big')
        ln = bytes([0x80 | len(b)]) + b
    return bytes([tag]) + ln + body


def seq(*xs): return tlv(0x30, b''.join(xs))
def set_of(*xs): return tlv(0x31, b''.join(sorted(xs)))   # DER: SET OF sorted by encoding
def octets(b): return tlv(0x04, b)
def bmp(s): return tlv(0x1e, s.encode('utf-16-be'))
def null(): return b'\x05\x00'
def utctime(s): return tlv(0x17, s.encode())


def integer(v):
    b = v.to_bytes((v.bit_length() + 8) // 8 or 1, 'big')
    return tlv(0x02, b)


def oid(dotted):
    p = [int(x) for x in dotted.split('.')]
    out = bytes([40 * p[0] + p[1]])
    for v in p[2:]:
        enc = [v & 0x7f]
        v >>= 7
        while v:
            enc.append(0x80 | (v & 0x7f))
            v >>= 7
        out += bytes(reversed(enc))
    return tlv(0x06, out)


MS = '1.3.6.1.4.1.311'
OID_CTL = MS + '.10.1'
OID_CATALOG_LIST = MS + '.12.1.1'
OID_CATALOG_LIST_MEMBER2 = MS + '.12.1.3'
OID_CAT_NAMEVALUE = MS + '.12.2.1'
OID_CAT_MEMBERINFO2 = MS + '.12.2.3'
OID_SPC_INDIRECT_DATA = MS + '.2.1.4'
OID_SPC_PE_IMAGE_DATA = MS + '.2.1.15'
OID_SPC_CAB_DATA = MS + '.2.1.25'      # Inf2Cat uses it for flat (non-PE) files
OID_SHA256 = '2.16.840.1.101.3.4.2.1'
OID_PKCS7_SIGNED = '1.2.840.113549.1.7.2'
NV_FLAGS = 0x10010001                  # CRYPTCAT_ATTR_AUTHENTICATED | NAMEASCII | DATAASCII, as Inf2Cat writes


def name_value(name, value):
    return seq(bmp(name), integer(NV_FLAGS), octets((value + '\0').encode('utf-16-le')))


def nv_attr(name, value):          # member attribute
    return seq(oid(OID_CAT_NAMEVALUE), set_of(name_value(name, value)))


def nv_ext(name, value):           # catalog-level attribute (CTL extension)
    return seq(oid(OID_CAT_NAMEVALUE), octets(name_value(name, value)))


# --- hashes -------------------------------------------------------------------------------------------------
def pe_ranges(d):
    """Byte ranges covered by the Authenticode hash (header minus CheckSum and the certificate directory entry,
    then everything up to the attribute certificate table), or None for a non-PE file."""
    if len(d) < 0x40 or d[:2] != b'MZ':
        return None
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    if d[pe:pe + 4] != b'PE\0\0':
        return None
    opt = pe + 24
    magic = struct.unpack_from('<H', d, opt)[0]
    dd = opt + (112 if magic == 0x20b else 96)
    cks, cert_entry = opt + 64, dd + 4 * 8
    cert_off, cert_len = struct.unpack_from('<II', d, cert_entry)
    end = cert_off if cert_len else len(d)
    return [(0, cks), (cks + 4, cert_entry), (cert_entry + 8, end)]


def file_hashes(path):
    d = open(path, 'rb').read()
    r = pe_ranges(d)
    h1, h256 = hashlib.sha1(), hashlib.sha256()
    for a, b in (r or [(0, len(d))]):
        h1.update(d[a:b])
        h256.update(d[a:b])
    return r is not None, h1.digest(), h256.digest()


# --- catalog ------------------------------------------------------------------------------------------------
def members(path, osattr='2:10.0'):
    is_pe, s1, s256 = file_hashes(path)
    name = os.path.basename(path).lower()
    info = seq(oid(OID_CAT_MEMBERINFO2), set_of(b'\x80\x00' if is_pe else b'\x82\x00'))
    common = [info, nv_attr('OSAttr', osattr), nv_attr('File', name)]
    if is_pe:
        what = seq(oid(OID_SPC_PE_IMAGE_DATA), seq(b'\x03\x02\x05\xa0', b'\xa0\x04\xa2\x02\x80\x00'))
    else:
        what = seq(oid(OID_SPC_CAB_DATA), b'\xa2\x02\x80\x00')
    indirect = seq(oid(OID_SPC_INDIRECT_DATA),
                   set_of(seq(what, seq(seq(oid(OID_SHA256), null()), octets(s256)))))
    return [seq(octets(s1), set_of(*common)), seq(octets(s256), set_of(*common, indirect))]


def entry_tag(e):
    # e = 30 len 04 len TAG ...; tags are 20 or 32 bytes so all lengths are short-form here
    hl = 2 if e[1] < 0x80 else 2 + (e[1] & 0x7f)
    return e[hl + 2:hl + 2 + e[hl + 1]]


def ctl_entries(files):
    es = [m for f in files for m in members(f)]
    return sorted(es, key=entry_tag)


def make_catalog(files, os_list, hwids, date):
    entries = ctl_entries(files)
    list_id = hashlib.md5(b''.join(entry_tag(e) for e in entries)).digest()   # reproducible "GUID"
    exts = [nv_ext('OS', os_list)] + [nv_ext(f'HWID{i + 1}', h.lower()) for i, h in enumerate(hwids)]
    ctl = seq(seq(oid(OID_CATALOG_LIST)), octets(list_id), utctime(date),
              seq(oid(OID_CATALOG_LIST_MEMBER2), null()), seq(*entries),
              tlv(0xa0, seq(*exts)))
    signed = seq(integer(1), set_of(seq(oid(OID_SHA256), null())),
                 seq(oid(OID_CTL), tlv(0xa0, ctl)), set_of())
    return seq(oid(OID_PKCS7_SIGNED), tlv(0xa0, signed))


def selftest():
    ref_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../build/woa/ufs-sys')
    ref = open(os.path.join(ref_dir, 'Exynos9810Ufs.cat'), 'rb').read()
    mine = b''.join(ctl_entries([os.path.join(ref_dir, 'Exynos9810Ufs.inf'),
                                 os.path.join(ref_dir, 'Exynos9810Ufs.sys')]))
    ext = seq(nv_ext('OS', '_v100_ARM64_Vb,_v100_ARM64_22H2'), nv_ext('HWID1', 'acpi\\exyn9810'))
    ok1, ok2 = mine in ref, ext in ref
    print(f'selftest: member list {"identical" if ok1 else "DIFFERS"} ({len(mine)} bytes), '
          f'catalog attributes {"identical" if ok2 else "DIFFER"}')
    return ok1 and ok2


def main(argv):
    if argv[:1] == ['--selftest']:
        return 0 if selftest() else 1
    os_list, hwids, date, files = '_v100_ARM64_22H2', [], None, []
    it = iter(argv)
    for a in it:
        if a == '--os':
            os_list = next(it)
        elif a == '--hwid':
            hwids.append(next(it))
        elif a == '--date':
            date = next(it)
        else:
            files.append(a)
    if len(files) < 2:
        print(__doc__)
        return 2
    out, files = files[0], files[1:]
    if date is None:
        t = int(os.environ.get('SOURCE_DATE_EPOCH', time.time()))
        date = time.strftime('%y%m%d%H%M%S', time.gmtime(t))
    open(out, 'wb').write(make_catalog(files, os_list, hwids, date + 'Z'))
    print(f'{out}: {len(files)} files, {2 * len(files)} members, OS {os_list}' +
          (f', HWID {", ".join(hwids)}' if hwids else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
