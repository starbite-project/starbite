#!/usr/bin/env bash
# build.sh [SUITE...]: Wine's D3D test suites (default: d3d10core) as ARM64 Windows programs for our driver's
# conformance runs (docs/WIN11-GPU-FULL-DRIVER.md item 2). Sources: Wine 10.0 from the wine-mirror GitHub mirror,
# fetched into build/win/winetests/src and checked against the sha256 below (LGPL-2.1; not copied into this repo).
# Each suite is patched in a copy: its test functions get names (sbwt-inject.h), so sbwt.exe can list them and run
# each one in its own process. Output: build/win/winetests/<suite>_test.exe and sbwt.exe.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${SBWT_OUT:-/Users/dev/project-starbite/build/win/winetests}
K=$HOME/sbwin; KV=10.0.26100.0
mkdir -p "$out/src" "$out/gen"
want() { case $1 in
  d3d10core.c) echo a598f60308a4;; d3d11.c) echo 41a5cf4d0fe2;; dxgi.c) echo 2bcf59fcccfb;; test.h) echo 5c9519b4a787;; esac; }
fetch() { # path-in-wine
  local f; f=$(basename "$1")
  [ -s "$out/src/$f" ] || curl -sSL --max-time 180 -o "$out/src/$f" "https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/$1"
  local got; got=$(shasum -a 256 "$out/src/$f" | cut -c1-12)
  [ "$got" = "$(want "$f")" ] || { echo "$f: sha256 $got, expected $(want "$f")"; exit 1; }
}
fetch include/wine/test.h
mkdir -p "$out/gen/wine" "$out/gen/ddk"; cp "$out/src/test.h" "$out/gen/wine/test.h"; cp "$here/shim/wine/"*.h "$out/gen/wine/"; cp "$here/shim/ddk/"*.h "$out/gen/ddk/"; cp "$here/sbwt-inject.h" "$here/sbwt-prelude.h" "$out/gen/"
cc() { # out.exe src.c libs...
  local exe=$1 src=$2; shift 2
  rm -f "$out/$exe"
  # the VM's view of a file the Mac has just rewritten can lag (seen twice: a truncated sbwt.c, "expected '}'"):
  # wait until the VM reads what the Mac wrote
  local want got i; want=$(shasum -a 256 "$out/gen/$src" | cut -c1-64)
  for i in 1 2 3 4 5 6 7 8 9 10; do
    got=$(limactl shell starbite -- sha256sum "$out/gen/$src" 2>/dev/null | cut -c1-64)
    [ "$got" = "$want" ] && break; sleep 1
  done
  [ "$got" = "$want" ] || { echo "$src: the VM does not see the file as written"; exit 1; }
  rm -f "$out/$exe"
  limactl shell starbite -- bash -c "cd '$out/gen' && ~/sbmesa/tc/clang-cl /nologo /O1 /MT /TC /FIsbwt-prelude.h -DSTANDALONE -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_WARNINGS -w -I. /Fe:../$exe $src /link /LIBPATH:$K/msvc/lib/arm64 /LIBPATH:$K/kit/Lib/$KV/um/arm64 /LIBPATH:$K/kit/Lib/$KV/ucrt/arm64 $* 2>&1 | grep -E 'error|undefined' | head -40; rm -f *.obj"
  [ -s "$out/$exe" ] || { echo "$exe: build failed"; exit 1; }
  echo "$exe: $(shasum -a 256 "$out/$exe" | cut -c1-12)"
}
for suite in "${@:-d3d10core}"; do
  case $suite in d3d10core) dir=dlls/d3d10core/tests;; d3d11) dir=dlls/d3d11/tests;; dxgi) dir=dlls/dxgi/tests;; *) echo "unknown suite $suite"; exit 1;; esac
  fetch $dir/$suite.c
  python3 - "$out/src/$suite.c" "$out/gen/$suite.c" "$suite" <<'PY'
import re, sys
src, dst, suite = sys.argv[1:4]
s = open(src, encoding='utf-8', errors='replace').read()
n = s.count('static void queue_test(void (*test)(void))')
if n == 1:
    s = s.replace('static void queue_test(void (*test)(void))', 'static void queue_test_impl(void (*test)(void))')
else:
    # a suite without the queue: give the injected header something to call
    s = s.replace('START_TEST(', 'static void queue_test_impl(void (*test)(void)) { test(); }\nSTART_TEST(', 1)
# the Windows SDK's C macros for COM methods that return a struct take the result as a second argument
s = re.sub(r'(\w+) = (ID3D12\w+_(?:GetAdapterLuid|GetDesc))\(([^;()]*)\);', r'\2(\3, &\1);', s)
i = s.index('START_TEST(')
head, body = s[:i], s[i:]
# direct calls in the entry function get names too
body, k = re.subn(r'(?m)^    (test_\w+)\(\);$', r'    sb_wt_direct(\1, "\1");', body)
# the other ways a suite starts a test (per feature level, per API): named too; the D3D12 variants get a prefix
def wrap(m):
    name = m.group(3) if m.group(1) != 'run_on_d3d12' else 'd3d12.' + m.group(3)
    return f'    if (!sb_wt_skip("{name}")) {m.group(1)}({m.group(2)}{m.group(3)});'
body, k2 = re.subn(r'(?ms)^    (queue_for_each_feature_level_in_range|queue_for_each_feature_level|queue_for_each_9_x_feature_level|run_on_d3d10|run_on_d3d12)\(([^;]*?)(test_\w+)\);', wrap, body)
k += k2
s = head + '#include "sbwt-inject.h"\n' + body
open(dst, 'w').write(s)
print(f'{suite}: queue definition {n}, {body.count("queue_test(")} queued, {k} direct calls named')
PY
  cc ${suite}_test.exe $suite.c d3d10.lib d3d10_1.lib d3d11.lib dxgi.lib dxguid.lib uuid.lib user32.lib gdi32.lib ole32.lib advapi32.lib d3d12.lib d3dcompiler.lib ntdll.lib
done
cp "$here/sbwt.c" "$out/gen/sbwt.c"
cc sbwt.exe sbwt.c
