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
mkdir -p "$out/gen/wine"; cp "$out/src/test.h" "$out/gen/wine/test.h"; cp "$here/shim/wine/"*.h "$out/gen/wine/"; cp "$here/sbwt-inject.h" "$out/gen/"
cc() { # out.exe src.c libs...
  local exe=$1 src=$2; shift 2
  rm -f "$out/$exe"
  rm -f "$out/$exe"
  limactl shell starbite -- bash -c "cd '$out/gen' && ~/sbmesa/tc/clang-cl /nologo /O1 /MT /TC /FIstdbool.h -DSTANDALONE -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_WARNINGS -w -I. /Fe:../$exe $src /link /LIBPATH:$K/msvc/lib/arm64 /LIBPATH:$K/kit/Lib/$KV/um/arm64 /LIBPATH:$K/kit/Lib/$KV/ucrt/arm64 $* 2>&1 | grep -E 'error|undefined' | head -40; rm -f *.obj"
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
i = s.index('START_TEST(')
head, body = s[:i], s[i:]
# direct calls in the entry function get names too
body, k = re.subn(r'(?m)^    (test_\w+)\(\);$', r'    sb_wt_direct(\1, "\1");', body)
s = head + '#include "sbwt-inject.h"\n' + body
open(dst, 'w').write(s)
print(f'{suite}: queue definition {n}, {body.count("queue_test(")} queued, {k} direct calls named')
PY
  cc ${suite}_test.exe $suite.c d3d10.lib d3d10_1.lib d3d11.lib dxgi.lib dxguid.lib uuid.lib user32.lib gdi32.lib ole32.lib advapi32.lib
done
cp "$here/sbwt.c" "$out/gen/sbwt.c"
cc sbwt.exe sbwt.c
