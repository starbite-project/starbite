#!/bin/sh
# Host test: the loader's boot_args layouts (shim/starlte/xnu_boot.c) against the kernels' own
# pexpert/arm64/boot.h. Nothing here touches the phone.
#   sh tools/loader-test/bootargs-layout.sh [XNU_TREE ...]     (default: build/xnu-12377 build/xnu-11417)
# A version-2 payload (tools/payload-v2.py) gets struct boot_args_1024; it must match every 1024-byte
# tree field by field. The 608-byte struct boot_args (version-1 payloads) is checked against the
# 8796 layout it was written for: same fields, CommandLine[608].
set -eu
cd "$(dirname "$0")/../.."
[ $# -gt 0 ] || set -- build/xnu-12377 build/xnu-11417
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
src=shim/starlte/xnu_boot.c
{
    echo '#include <stddef.h>'
    echo '#include <stdint.h>'
    echo '#include <stdio.h>'
    echo '#define BOOT_LINE_LENGTH 608'
    echo '#define XNU_LINE_MAX 1024'
    # the loader's structs and their asserts, as compiled for the phone
    sed -n '/^struct payload_seg {/,/^_Static_assert(sizeof(struct boot_args_1024)/p' "$src"
    i=0
    for tree in "$@"; do
        h=$tree/pexpert/pexpert/arm64/boot.h
        [ -f "$h" ] || { echo "no $h" >&2; exit 1; }
        len=$(sed -n 's/^#define BOOT_LINE_LENGTH[[:space:]]*\([0-9]*\).*/\1/p' "$h")
        printf '%s\n' "/* $h: BOOT_LINE_LENGTH $len */"
        sed -n '/^struct Boot_Video {/,/^};/p' "$h" | sed "s/Boot_Video/Boot_Video_$i/"
        sed -n '/^typedef struct boot_args {/,/^} boot_args;/p' "$h" |
            sed "s/typedef struct boot_args {/struct xnu_ba_$i {/; s/^} boot_args;/};/; s/Boot_Video /struct Boot_Video_$i /; s/BOOT_LINE_LENGTH/$len/"
        printf '%s\n' "#define LEN_$i $len"
        i=$((i + 1))
    done
    echo 'int main(void) {'
    echo '    int bad = 0;'
    i=0
    for tree in "$@"; do
        printf '%s\n' "    {"
        printf '%s\n' "    const char *t = \"$tree\";"
        printf '%s\n' "    size_t want = LEN_$i == 1024 ? sizeof(struct boot_args_1024) : sizeof(struct boot_args);"
        printf '%s\n' "    if (sizeof(struct xnu_ba_$i) != want) { printf(\"%s: size %zu, loader %zu\\n\", t, sizeof(struct xnu_ba_$i), want); bad = 1; }"
        for f in Revision Version virtBase physBase memSize topOfKernelData Video machineType deviceTreeP deviceTreeLength CommandLine bootFlags memSizeActual; do
            printf '%s\n' "    { size_t a = offsetof(struct xnu_ba_$i, $f), b = LEN_$i == 1024 ? offsetof(struct boot_args_1024, $f) : offsetof(struct boot_args, $f);"
            printf '%s\n' "      if (a != b) { printf(\"%s: $f at %zu, loader %zu\\n\", t, a, b); bad = 1; } }"
        done
        printf '%s\n' "    if (sizeof(((struct xnu_ba_$i *)0)->CommandLine) != (LEN_$i == 1024 ? 1024u : 608u)) { printf(\"%s: CommandLine size\\n\", t); bad = 1; }"
        printf '%s\n' "    printf(\"%s: BOOT_LINE_LENGTH %d, %zu bytes, bootFlags @%zu memSizeActual @%zu: %s\\n\", t, LEN_$i, sizeof(struct xnu_ba_$i),"
        printf '%s\n' "        offsetof(struct xnu_ba_$i, bootFlags), offsetof(struct xnu_ba_$i, memSizeActual), bad ? \"MISMATCH\" : \"matches the loader\");"
        printf '%s\n' "    }"
        i=$((i + 1))
    done
    echo '    return bad;'
    echo '}'
} > "$T/layout.c"
${CC:-cc} -std=c11 -Wall -Werror "$T/layout.c" -o "$T/layout"
"$T/layout"
echo "boot_args layout: PASS"
