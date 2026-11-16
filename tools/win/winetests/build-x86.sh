#!/usr/bin/env bash
# winetests-x86.sh [SUITE...]: tools/win/winetests/build.sh for 32-bit x86 (default suite: d3d10core), plus the two
# small programs that say in seconds whether a 32-bit process gets our adapter. A copy of build.sh adapted, not a
# change to it: same sources (Wine 10.0, sha256-pinned), same patching (every test function gets a name), same
# sbwt.c runner, built with the VM's ~/sbmesa/tc/x86/clang-cl (i686-pc-windows-msvc) against the x86 libraries of
# the kit in ~/sbwin.
#
# Output, in build/win/umd-x86 of this worktree (SBWT_OUT overrides):
#   d3d10core_test32.exe   the suite (x86)         sbwt32.exe    the runner (x86; one test function per process)
#   sbd3d32.exe            tools/win/mesa/tests/sbd3d.cpp (x86): the adapters, the one a D3D11 device really uses,
#                          whether sbumd.dll is loaded in the process, a clear and a triangle read back.
#                          Exit 0 = on our driver and right; 1 = drew, but not on our driver (WARP); 3 = no device.
# The "32" in the names keeps them apart from the ARM64 programs in the phone's test folder. Working files go to
# build/win/umd-x86/wt (sources are taken from the main checkout's build/win/winetests/src when they are there
# and match, so nothing is downloaded; otherwise fetched as build.sh does).
# Compiled only: none of this has run on Windows.
set -euo pipefail
pw=$(cd "$(dirname "$0")" && pwd)
wt=$(cd "$pw/../../.." && pwd)                       # the worktree
here=$wt/tools/win/winetests                         # build.sh's own folder: shim headers, sbwt.c, the inject header
out=${SBWT_OUT:-$wt/build/win/umd-x86}
work=$out/wt
have=/Users/dev/project-starbite/build/win/winetests/src
K=$HOME/sbwin; KV=10.0.26100.0
CL='~/sbmesa/tc/x86/clang-cl'
LIBS="/LIBPATH:$K/msvc/lib/x86 /LIBPATH:$K/kit/Lib/$KV/um/x86 /LIBPATH:$K/kit/Lib/$KV/ucrt/x86"
mkdir -p "$work/src" "$work/gen"
want() { case $1 in
  d3d10core.c) echo a598f60308a4;; d3d11.c) echo 41a5cf4d0fe2;; dxgi.c) echo 2bcf59fcccfb;; test.h) echo 5c9519b4a787;; esac; }
fetch() { # path-in-wine
  local f; f=$(basename "$1")
  if [ ! -s "$work/src/$f" ] && [ -s "$have/$f" ]; then cp "$have/$f" "$work/src/$f"; fi
  [ -s "$work/src/$f" ] || curl -sSL --max-time 180 -o "$work/src/$f" "https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/$1"
  local got; got=$(shasum -a 256 "$work/src/$f" | cut -c1-12)
  [ "$got" = "$(want "$f")" ] || { echo "$f: sha256 $got, expected $(want "$f")"; exit 1; }
}
fetch include/wine/test.h
mkdir -p "$work/gen/wine" "$work/gen/ddk"; cp "$work/src/test.h" "$work/gen/wine/test.h"; cp "$here/shim/wine/"*.h "$work/gen/wine/"; cp "$here/shim/ddk/"*.h "$work/gen/ddk/"; cp "$here/sbwt-inject.h" "$here/sbwt-prelude.h" "$work/gen/"
seen() { # file: wait until the VM reads what the Mac wrote (build.sh: its view of a rewritten file can lag)
  local want got i; want=$(shasum -a 256 "$1" | cut -c1-64)
  for i in 1 2 3 4 5 6 7 8 9 10; do
    got=$(limactl shell starbite -- sha256sum "$1" 2>/dev/null | cut -c1-64)
    [ "$got" = "$want" ] && return 0; sleep 1
  done
  echo "$1: the VM does not see the file as written"; exit 1
}
is_x86() { # exe: PE32 for i386, or stop
  python3 - "$1" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
machine = struct.unpack_from('<H', d, pe + 4)[0]
magic = struct.unpack_from('<H', d, pe + 24)[0]
if machine != 0x14c or magic != 0x10b:
    raise SystemExit(f'{sys.argv[1]}: machine 0x{machine:x} magic 0x{magic:x}: not a 32-bit x86 program')
PY
}
cc() { # out.exe src.c libs...
  local exe=$1 src=$2; shift 2
  rm -f "$out/$exe"
  seen "$work/gen/$src"
  # as build.sh, with the x86 compiler and libraries; /Fe two levels up (gen -> wt -> umd-x86)
  limactl shell starbite -- bash -c "cd '$work/gen' && $CL /nologo /O1 /MT /TC /FIsbwt-prelude.h -DSTANDALONE -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_WARNINGS -w -I. /Fe:../../$exe $src /link $LIBS $* 2>&1 | grep -E 'error|undefined' | head -40; rm -f *.obj"
  [ -s "$out/$exe" ] || { echo "$exe: build failed"; exit 1; }
  is_x86 "$out/$exe"
  echo "$exe: $(shasum -a 256 "$out/$exe" | cut -c1-12) (x86)"
}
for suite in "${@:-d3d10core}"; do
  case $suite in d3d10core) dir=dlls/d3d10core/tests;; d3d11) dir=dlls/d3d11/tests;; dxgi) dir=dlls/dxgi/tests;; *) echo "unknown suite $suite"; exit 1;; esac
  fetch $dir/$suite.c
  python3 - "$work/src/$suite.c" "$work/gen/$suite.c" "$suite" <<'PY'
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
  cc ${suite}_test32.exe $suite.c d3d10.lib d3d10_1.lib d3d11.lib dxgi.lib dxguid.lib uuid.lib user32.lib gdi32.lib ole32.lib advapi32.lib d3d12.lib d3dcompiler.lib ntdll.lib
done
cp "$here/sbwt.c" "$work/gen/sbwt.c"
cc sbwt32.exe sbwt.c

# sbd3d (C++), as tools/win/mesa/tests/build-test.sh builds it, for x86
cp "$wt/tools/win/mesa/tests/sbd3d.cpp" "$work/gen/sbd3d.cpp"
seen "$work/gen/sbd3d.cpp"
rm -f "$out/sbd3d32.exe"
limactl shell starbite -- bash -c "cd '$work/gen' && $CL /nologo /O2 /EHsc /MT -Wno-deprecated-declarations -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH /Fe:../../sbd3d32.exe sbd3d.cpp /link $LIBS 2>&1 | grep -E 'error|warning' | head -30; rm -f *.obj"
[ -s "$out/sbd3d32.exe" ] || { echo "sbd3d32.exe: build failed"; exit 1; }
is_x86 "$out/sbd3d32.exe"
echo "sbd3d32.exe: $(shasum -a 256 "$out/sbd3d32.exe" | cut -c1-12) (x86)"
