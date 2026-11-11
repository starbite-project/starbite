#!/bin/sh
# SPDX-License-Identifier: APSL-2.0
# StarbiteUFS host tests: the plain-C core (hw/) against a mock UFS host, with ASan + UBSan. The core is built with
# the strict warning set (-Wconversion too); the tests with the usual one. Writes only to a temporary directory.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/sbufs-test.XXXXXX")
trap 'rm -rf "$out"' EXIT
cc=${CC:-clang}
san="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
common="-std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow $san -I$here/../hw"
for f in sbufs_fence sbufs_proto sbufs_hc; do
	$cc $common -Wconversion -Wno-sign-conversion -c "$here/../hw/$f.c" -o "$out/$f.o"
done
for f in mock_hc t_fence t_proto t_hc t_main; do
	$cc $common -Wno-unused-parameter -c "$here/$f.c" -o "$out/$f.o"
done
$cc $san "$out"/*.o -o "$out/sbufs-test"
"$out/sbufs-test"
# the property packer (tools/sbufs-fence.py) against the C parser, on a synthetic S9-like GPT
python3 "$here/../tools/sbufs-fence.py" --selftest-gpt "$out/lu0-head.bin"
python3 "$here/../tools/sbufs-fence.py" --gpt "$out/lu0-head.bin" --allow USERDATA:root:19200 --out "$out/fence" >/dev/null
"$out/sbufs-test" --parse "$out/fence/allowed-ranges.bin" "$out/fence/denied-ranges.bin" 15616001
# the loader side (shim/starlte/ufs_fence.c + ufs.c's GPT reader) end to end into the driver's parser
L=$here/loader
S=$here/../../../../shim/starlte
sed 's/#include "ufs.h"/#include "host_loader.h"/; s/#include "store.h"//' "$S/ufs_fence.c" > "$out/ufs_fence_host.c"
{ echo '#include "host_loader.h"'; sed -n '/---- GPT on LU0/,$p' "$S/ufs.c"; } > "$out/ufs_gpt_host.c"
mkdir -p "$out/fx"
python3 -I -B "$L/mkfixtures.py" "$out/fx"
$cc -std=c11 -O1 -g -Wall -Wextra -Werror $san -I"$L" -I"$S" -I"$here/../hw" "$L/t_loader.c" "$out/ufs_fence_host.c" \
    "$out/ufs_gpt_host.c" "$here/../hw/sbufs_fence.c" -o "$out/loader-test"
"$out/loader-test" "$out/fx"
