#!/bin/bash
# Build and run ttn_sample natively in the lima VM against ~/sbmesa/src (with the sb-patches applied) and its
# native build ~/sbmesa/build-libpan:  limactl shell starbite -- tools/win/mesa/tests/host/build-ttn-sample.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
src=~/sbmesa/src; bd=~/sbmesa/build-libpan
ninja -C $bd src/gallium/auxiliary/libgallium.a src/compiler/nir/libnir.a src/compiler/libcompiler.a \
    src/util/libmesa_util.a src/util/libmesa_util_simd.a src/panfrost/compiler/libpanfrost_compiler.a \
    src/panfrost/compiler/bifrost/libpanfrost_bifrost.a src/panfrost/compiler/midgard/libpanfrost_midgard.a \
    src/panfrost/shared/libpanfrost_shared.a src/panfrost/model/libpanfrost_model_lib.a \
    src/compiler/glsl/libglsl_util.a src/panfrost/lib/libpanfrost_lib.a src/c11/impl/libmesa_util_c11.a src/util/blake3/libblake3.a \
    src/panfrost/compiler/bifrost/valhall/libpanfrost_valhall_disasm.a src/panfrost/compiler/bifrost/libpanfrost_bifrost_disasm.a \
    src/panfrost/compiler/midgard/libpanfrost_midgard_disasm.a
inc="-I$src/include -I$src/src -I$src/src/gallium/include -I$src/src/gallium/auxiliary -I$src/src/compiler \
     -I$src/src/compiler/nir -I$src/src/panfrost -I$src/src/panfrost/compiler -I$src/src/mapi -I$src/src/mesa \
     -I$bd/src -I$bd/src/compiler -I$bd/src/compiler/nir -I$bd/src/panfrost -I$bd/src/gallium/auxiliary"
libs=$(cd $bd && ls src/panfrost/lib/libpan*.a \
       src/panfrost/compiler/libpanfrost_compiler.a src/panfrost/compiler/bifrost/libpanfrost_bifrost.a \
       src/panfrost/compiler/bifrost/valhall/*.a src/panfrost/compiler/midgard/libpanfrost_midgard.a \
       src/panfrost/shared/libpanfrost_shared.a src/panfrost/model/libpanfrost_model_lib.a \
       src/panfrost/genxml/*.a src/panfrost/lib/kmod/libpankmod_lib.a \
       src/gallium/auxiliary/libgallium.a src/compiler/nir/libnir.a src/compiler/glsl/libglsl_util.a \
       src/compiler/libcompiler.a src/util/libmesa_util.a src/util/libmesa_util_simd.a src/util/libxmlconfig.a \
       src/c11/impl/libmesa_util_c11.a src/util/blake3/libblake3.a src/panfrost/compiler/bifrost/*disasm.a src/panfrost/compiler/bifrost/valhall/*disasm.a src/panfrost/compiler/midgard/*disasm.a 2>/dev/null | sed "s|^|$bd/|")
# the compile flags meson uses for the Panfrost gallium driver (defines and include paths), warnings as warnings
flags=$(python3 - "$bd" <<'PY'
import json, shlex, sys
bd = sys.argv[1]
for e in json.load(open(bd + "/compile_commands.json")):
    if e["file"].endswith("pan_nir_lower_res_indices.c") and "libpanfrost.a.p" in e["command"]:
        out = []
        for a in shlex.split(e["command"])[2:]:
            if a.startswith("-I"):
                p = a[2:]
                out.append("-I" + (p if p.startswith("/") else bd + "/" + p))
            elif a.startswith("-D") or a.startswith("-std") or a == "-pthread":
                out.append(a)
        print(" ".join(shlex.quote(x) for x in out))
        break
PY
)
eval gcc -O1 -g $flags $inc "$here/ttn_sample.c" \
    $src/src/gallium/drivers/panfrost/pan_nir_lower_res_indices.c $src/src/gallium/drivers/panfrost/pan_nir_lower_sysvals.c \
    -I$src/src/gallium/drivers/panfrost -I$src/src/panfrost/lib -I$src/src/panfrost/shared -I$src/src/panfrost/model -I$src/src/panfrost/genxml \
    -I$bd/src/panfrost/genxml -I$bd/src/util -I$bd/src/util/format -I$src/src/util/format -I/usr/include/libdrm -o /tmp/ttn_sample \
    -Wl,--start-group $libs -Wl,--end-group $(pkg-config --libs zlib libzstd expat 2>/dev/null) -lstdc++ -lm -lpthread -ldl
/tmp/ttn_sample
