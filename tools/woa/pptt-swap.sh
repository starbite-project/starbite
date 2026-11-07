#!/usr/bin/env bash
# Put a new PPTT into an existing Windows UEFI image and change nothing else (docs/WIN11-24H2-A55.md, "PPTT").
#   tools/woa/pptt-swap.sh IN.bin OUT.bin [PPTT.aml]
# IN.bin: a starbite-uefi-*.bin (384-byte bootshim + FD). PPTT.aml defaults to tools/woa/pptt-gen.py's OEM-shaped table.
# The platform ACPI file (FREEFORM 11111111-...-501: FACP, GTDT, APIC, PPTT, DSDT, SSDT) is taken from IN itself, its
# PPTT section replaced (pptt-patch.py --table), the old file blanked to a pad file and the new one added; dxe-diff
# must then report exactly that one file as changed. Needs the lima VM (LzmaCompress), like every DXE splice.
set -euo pipefail
cd "$(dirname "$0")/../.."
in=${1:?usage: pptt-swap.sh IN.bin OUT.bin [PPTT.aml]} out=${2:?usage: pptt-swap.sh IN.bin OUT.bin [PPTT.aml]}
ACPIFILE=11111111-2222-3333-4444-555555555501
t=$(mktemp -d "$HOME/.pptt-swap.XXXXXX")
trap 'rm -rf "$t"' EXIT
if [ -n "${3:-}" ]; then cp "$3" "$t/pptt.aml"; else python3 -I tools/woa/pptt-gen.py "$t/pptt.aml"; fi
python3 -I tools/woa/pptt-gen.py --decode "$t/pptt.aml" | head -1
head -c 384 "$in" > "$t/shim.bin"
tail -c +385 "$in" > "$t/in.fd"
python3 tools/woa/dxe-replace.py "$t/in.fd" "$t/in.fd" /dev/null --extract "$ACPIFILE" "$t/acpi.ffs" >/dev/null
python3 -I tools/woa/pptt-patch.py "$t/acpi.ffs" "$t/acpi-new.ffs" --table "$t/pptt.aml"
python3 tools/woa/dxe-replace.py "$t/in.fd" "$t/in.fd" "$t/out.fd" --kill-blank "$ACPIFILE" --add "$t/acpi-new.ffs" >/dev/null
python3 tools/woa/dxe-diff.py "$t/in.fd" "$t/out.fd" --replaced "$ACPIFILE"
cat "$t/shim.bin" "$t/out.fd" > "$out"
# the image must carry the new table byte for byte
python3 - "$t/out.fd" "$t/pptt.aml" <<'PY'
import importlib.util, sys
spec = importlib.util.spec_from_file_location('dd', 'tools/woa/dxe-diff.py')
dd = importlib.util.module_from_spec(spec); spec.loader.exec_module(dd)
_, files = dd.load(sys.argv[1])
t = open(sys.argv[2], 'rb').read()
acpi = [body for g, (typ, body, name) in files.items() if g == '11111111-2222-3333-4444-555555555501']
assert len(acpi) == 1, f'{len(acpi)} live platform ACPI files'
assert acpi[0].count(t) == 1 and acpi[0].count(b'PPTT') == 1, 'the new PPTT is not the one PPTT in the live ACPI file'
print('checked: the live platform ACPI file carries exactly the new PPTT')
PY
shasum -a 256 "$out"
rm -rf tools/woa/__pycache__
