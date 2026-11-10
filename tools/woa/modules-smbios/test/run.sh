#!/bin/sh
# Host test of the S9 SMBIOS table (Star2LteSmbiosDxe/SmbiosBuild.c): our own structure/checksum checks,
# then dmidecode (lima VM "starbite") decodes each dump and the fields Windows shows are compared.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/sbsmbios.XXXXXX")
cc -std=c11 -DSB_HOST -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -I"$here/../Star2LteSmbiosDxe" \
    -o "$out/smbios-host" "$here/smbios-host.c" "$here/../Star2LteSmbiosDxe/SmbiosBuild.c"
"$out/smbios-host" "$out"
fail=0
for f in ep3-t7 ep3-t0 ep21-t7 ep21-t0; do
    limactl shell starbite -- bash -c 'f=$(mktemp); cat > $f; dmidecode --from-dump $f; rm -f $f' < "$out/$f.bin" > "$out/$f.txt" 2>&1 || true
    want="Version: Samsung Exynos 9810|Manufacturer: Samsung|Product Name: Galaxy S9|Family: ARMv8|Core Count: 4|Thread Count: 4|Max Speed: 1794 MHz|Current Speed: 1690 MHz|Vendor: Starbite|UEFI is supported|Type: Hand Held|ID: 50 D0 1F 41"
    case $f in *t7) want="$want|Installed Size: 512 kB|Installed Size: 32 kB|System Type: Instruction|Associativity: 16-way Set-associative";; esac
    case $f in ep3*) want="$want|SMBIOS 3.3.0 present";; *) want="$want|SMBIOS 3.3 present";; esac
    IFS='|'
    for w in $want; do
        grep -q "$w" "$out/$f.txt" || { echo "FAIL $f: no '$w'"; fail=1; }
    done
    unset IFS
    if grep -qiE "invalid|bad checksum|truncated|out of spec|wrong" "$out/$f.txt"; then
        echo "FAIL $f: dmidecode complains:"; grep -iE "invalid|bad checksum|truncated|out of spec|wrong" "$out/$f.txt"; fail=1
    fi
done
[ $fail = 0 ] && echo "dmidecode checks ok ($out)" || { echo "dmidecode output in $out"; exit 1; }
