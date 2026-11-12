#!/bin/sh
# sbwl's hostsim run in the Lima VM's own Ubuntu 24.04 (wlroots 0.17; no GTK, no cairo: the plain clients only), as
# the VM's user (no "user" account, not root: sbwl starts the apps as itself, with a private session bus):
#   limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/sbwl/run-u17.sh [OUT]
# Builds into OUT (default /mnt/build/sbguest3/desk/u17), the test apps' .desktop files into OUT/home's
# ~/.local/share/applications, runs hostsim-u17.script: PASS / FAIL lines, the whole log in OUT/hostsim.log.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-/mnt/build/sbguest3/desk/u17}
"$here/build.sh" "$OUT" >/dev/null
rm -rf "$OUT/home" "$OUT/rt"
mkdir -p "$OUT/home/.local/share/applications" "$OUT/rt"
chmod 700 "$OUT/rt"
cp "$here"/test-apps/org.starbite.WlTest.desktop "$here"/test-apps/org.starbite.Spin*.desktop \
    "$here"/test-apps/org.starbite.NotifyTest.desktop "$OUT/home/.local/share/applications/"
rm -f /tmp/sbwl-wltest.txt /tmp/sbwl-notifytest.txt /tmp/sbwl-spin*.txt
cd "$OUT"
env -i PATH="$OUT:/usr/local/bin:/usr/bin:/bin" HOME="$OUT/home" LANG=C.UTF-8 XDG_RUNTIME_DIR="$OUT/rt" ${SBWL_HIDDEN_COMMITS:+SBWL_HIDDEN_COMMITS=1} \
    SBWL_SOCK="$OUT/rt/sbwl.sock" "$OUT/hostsim" -o "$OUT/hs" -s "$here/hostsim-u17.script" -- "$OUT/sbwl" 2>&1 |
    tee "$OUT/hostsim.log" | grep -a 'PASS\|FAIL\|RESULT\|hostsim:'
