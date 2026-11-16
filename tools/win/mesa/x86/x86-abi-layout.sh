#!/usr/bin/env bash
# x86-abi-layout.sh: compiles x86-abi-layout.c (syntax only, in the lima VM) for the three Windows targets against
# both copies of the sbumd <-> sbwddm private ABI header. Prints one line per combination; exit 1 if any differs.
# Nothing is written anywhere.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
wt=$(cd "$here/../../.." && pwd)
mesa=${SB_MESA_SRC:-'$HOME/sbmesa/src-x86'}
bad=0
run() { # label target include-dir extra...
  local label=$1 target=$2 inc=$3; shift 3
  if limactl shell starbite -- bash -c "clang-20 --target=$target -fms-extensions -fsyntax-only -Wno-pragma-once-outside-header -I $inc $* '$here/x86-abi-layout.c'" 2>&1 | grep -E 'error|warning'; then
    echo "DIFFERENT  $label  $target"; bad=1
  else
    echo "same       $label  $target"
  fi
}
for t in i686-pc-windows-msvc x86_64-pc-windows-msvc aarch64-pc-windows-msvc arm64ec-pc-windows-msvc; do
  run "Mesa copy (user mode)       " $t "$mesa/include/win32-compat"
  run "KMD copy (user mode)        " $t "'$wt/starbite/win/sbwddm/maliwddm'" -DSB_KMD_HEADER
done
run "KMD copy (-D_KERNEL_MODE)   " aarch64-pc-windows-msvc "'$wt/starbite/win/sbwddm/maliwddm'" -DSB_KMD_HEADER -D_KERNEL_MODE
exit $bad
