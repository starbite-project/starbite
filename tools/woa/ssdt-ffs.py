#!/usr/bin/env python3
"""Pack an SSDT (.aml) as a blob for Star2LteSsdtDxe (docs/WIN11-USB.md section 4.1).

  ssdt-ffs.py [--gated] IN.aml OUT.ffs     pack
  ssdt-ffs.py [--gated] --guid IN.aml      print the blob's file GUID (= its veto/enable protocol GUID)

The blob is one FFS file of type FREEFORM (0x02) holding a RAW section with the table exactly as
iasl wrote it, and a UI section "SSDT <table id>". Its name GUID is
53534454-5342-5445-<the 8 bytes of the OEM Table ID> for a plain blob (installed unless a driver
vetoes it) or 53534454-5342-4754-<...> for a gated one (installed only when its driver enables
it, so it fails closed), so the GUID follows from the table and two blobs with the same Table ID
cannot coexist. Checked here: signature SSDT, length, checksum,
OEM ID STRBTE, a non-empty Table ID. Add the result to the release DXE volume with
tools/woa/dxe-replace.py --add OUT.ffs; leaving it out is the kill switch.
"""
import importlib.util
import os
import sys
import uuid

_spec = importlib.util.spec_from_file_location(
    'dxe_replace', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dxe-replace.py'))
dr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(dr)

FREEFORM = 0x02
SEC_RAW = 0x19
SEC_UI = 0x15


def check(aml):
    if len(aml) < 36 or aml[:4] != b'SSDT':
        sys.exit('not an SSDT')
    if int.from_bytes(aml[4:8], 'little') != len(aml):
        sys.exit('length field does not match the file')
    if sum(aml) & 0xFF:
        sys.exit('bad checksum')
    if aml[10:16] != b'STRBTE':
        sys.exit(f'OEM ID must be STRBTE, is {aml[10:16]!r}')
    if not aml[16:24].strip(b'\0 '):
        sys.exit('empty OEM Table ID')
    return aml[16:24]


def blob_guid(table_id, gated=False):
    return uuid.UUID(bytes_le=bytes.fromhex('54445353' '4253' + ('5447' if gated else '4554')) + table_id)


def section(kind, data):
    size = 4 + len(data)
    return size.to_bytes(3, 'little') + bytes([kind]) + data


def pack(aml, gated=False):
    table_id = check(aml)
    name = ('Gated SSDT ' if gated else 'SSDT ') + table_id.rstrip(b'\0 ').decode('ascii', 'replace')
    raw = section(SEC_RAW, aml)
    raw += b'\0' * (-len(raw) % 4)
    body = raw + section(SEC_UI, (name + '\0').encode('utf-16le'))
    size = 24 + len(body)
    f = bytearray(blob_guid(table_id, gated).bytes_le + bytes([0, 0xAA, FREEFORM, 0x00]) + size.to_bytes(3, 'little') + b'\0')
    f[16] = dr.header_checksum(f, 0, 24)
    return bytes(f) + body, table_id


def main():
    args = sys.argv[1:]
    gated = '--gated' in args
    args = [a for a in args if a != '--gated']
    if args[:1] == ['--guid']:
        print(blob_guid(check(open(args[1], 'rb').read()), gated))
        return
    aml = open(args[0], 'rb').read()
    ffs, table_id = pack(aml, gated)
    open(args[1], 'wb').write(ffs)
    tid = table_id.rstrip(b'\0 ').decode('ascii', 'replace')
    kind = 'gated SSDT' if gated else 'SSDT'
    print(f'{args[1]}: {kind} {tid} ({len(aml)} bytes), GUID {blob_guid(table_id, gated)}, {len(ffs)} bytes')


if __name__ == '__main__':
    main()
