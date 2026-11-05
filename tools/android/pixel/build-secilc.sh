#!/bin/bash
# build-secilc.sh [VERSION] : build a current secilc (Ubuntu's 3.5 cannot read Android 17's CIL: "nlmsg"
# xperms) from the SELinux Project's release tarballs, in lima. Source: github.com/SELinuxProject/selinux
# releases; the .asc signatures are checked when gpg can fetch the key, the sha256s are printed either way.
# Result: ~/android/pixel/secilc-VERSION/secilc (static libsepol). Nothing outside that directory.
set -euo pipefail
VER=${1:-3.11}
D=~/android/pixel/secilc-$VER
mkdir -p "$D/src"; cd "$D/src"
for p in libsepol secilc; do
  for s in tar.gz tar.gz.asc; do
    [ -s "$p-$VER.$s" ] || curl -fsSL -o "$p-$VER.$s" "https://github.com/SELinuxProject/selinux/releases/download/$VER/$p-$VER.$s"
  done
  sha256sum "$p-$VER.tar.gz"
  if gpg --batch --verify "$p-$VER.tar.gz.asc" "$p-$VER.tar.gz" 2>/dev/null; then echo "$p: signature OK"
  else
    kid=$(gpg --batch --list-packets "$p-$VER.tar.gz.asc" 2>/dev/null | sed -n 's/.*keyid \([0-9A-F]*\).*/\1/p' | head -1)
    gpg --batch --keyserver hkps://keyserver.ubuntu.com --recv-keys "$kid" >/dev/null 2>&1 || true
    gpg --batch --verify "$p-$VER.tar.gz.asc" "$p-$VER.tar.gz" 2>&1 | grep -E 'Good|BAD|key' || echo "$p: signature NOT checked"
  fi
  tar xzf "$p-$VER.tar.gz"
done
make -C "libsepol-$VER" -j8 >/dev/null
make -C "secilc-$VER" -j8 secilc CFLAGS="-O2 -I$PWD/libsepol-$VER/include" LDLIBS="$PWD/libsepol-$VER/src/libsepol.a" >/dev/null
cp "secilc-$VER/secilc" "$D/secilc"
"$D/secilc" --help 2>&1 | head -1 || true
echo "built $D/secilc"
