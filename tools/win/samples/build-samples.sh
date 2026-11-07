#!/usr/bin/env bash
# Build, sign and check the L0 samples into build/win/samples/ (git-ignored; the lima VM sees it too).
#   tools/win/samples/build-samples.sh
# Checks each PE: machine ARM64, subsystem (native for .sys, console for .exe, GUI for the UMDF .dll),
# the imports, and the Starbite signature. Nothing is copied to the phone.
set -euo pipefail
win=$(cd "$(dirname "$0")/.." && pwd)
repo=$(cd "$win/../.." && pwd)
out=$repo/build/win/samples
mkdir -p "$out"
. "$win/env.sh"

"$win/build.sh" kmdf "$out/hello-kmdf.sys" "$win/samples/hello-kmdf/hello-kmdf.c"
"$win/build.sh" exe  "$out/hello-exe.exe"  "$win/samples/hello-exe/hello-exe.c"
"$win/build.sh" umdf "$out/hello-umdf.dll" "$win/samples/hello-umdf/hello-umdf.cpp"
IDDCX=1.9 "$win/build.sh" umdf "$out/hello-idd.dll" "$win/samples/hello-umdf/hello-umdf.cpp"
"$win/sign.sh" "$out/hello-kmdf.sys" "$out/hello-exe.exe" "$out/hello-umdf.dll" "$out/hello-idd.dll"

fail=0
pecheck() {  # file subsystem
    local h; h=$("$SB_READOBJ" --file-headers "$1")
    local m s
    m=$(grep -m1 'Machine:' <<<"$h" | sed 's/.*Machine: //'); s=$(grep -m1 'Subsystem:' <<<"$h" | sed 's/.*Subsystem: //')
    printf '%-16s %8s bytes  %-36s %s\n' "$(basename "$1")" "$(wc -c < "$1" | tr -d ' ')" "$m" "$s"
    printf '                 imports: %s\n' "$("$SB_READOBJ" --coff-imports "$1" | sed -n 's/^ *Name: //p' | tr '\n' ' ')"
    grep -q 'IMAGE_FILE_MACHINE_ARM64' <<<"$m" && grep -q "$2" <<<"$s" || { echo "  ^ WRONG"; fail=1; }
}
echo
pecheck "$out/hello-kmdf.sys" IMAGE_SUBSYSTEM_NATIVE
pecheck "$out/hello-exe.exe"  IMAGE_SUBSYSTEM_WINDOWS_CUI
pecheck "$out/hello-umdf.dll" IMAGE_SUBSYSTEM_WINDOWS_GUI
pecheck "$out/hello-idd.dll"  IMAGE_SUBSYSTEM_WINDOWS_GUI
(cd "$out" && shasum -a 256 hello-kmdf.sys hello-exe.exe hello-umdf.dll hello-idd.dll > SHA256SUMS)
exit $fail
