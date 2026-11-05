#!/usr/bin/env python3
"""catalog-pixel.py IN.toml OUT.toml [--back]

Rewrite only the `version` and `root` lines of the catalog entry key = "android" (docs/ANDROID-PIXEL.md):
the files, method, key and every other entry stay byte-identical, so the menu row keeps working with the
same android.kernel / android.ramdisk / android.cmdline store files. --back writes the LineageOS 24 text
again (rollback). IN must be the TOML the live store's catalog was built from (store-pull.sh says STOP
when tools/store/catalog.toml is not); OUT goes to sbstore2.py build --catalog."""
import re, sys, tomllib

PIXEL = {'version': 'Google GSI 17 QPR3 · Play · Linux 4.9',
         'root': 'ufs:SYSTEM (Google GSI) + VENDOR + A16DATA'}
LINEAGE = {'version': 'LineageOS 24.0 · Linux 4.9', 'root': 'ufs:SYSTEM + A16DATA'}
FILES = ['android.kernel', 'android.ramdisk', 'android.cmdline']


def main():
    args = [a for a in sys.argv[1:] if a != '--back']
    if len(args) != 2:
        sys.exit(__doc__)
    want = LINEAGE if '--back' in sys.argv else PIXEL
    src, dst = args
    text = open(src, encoding='utf-8').read()
    blocks = re.split(r'(?m)^(?=\[\[entry\]\]\s*$)', text)
    hits = [i for i, b in enumerate(blocks) if re.search(r'(?m)^key = "android"\s*$', b)]
    if len(hits) != 1:
        sys.exit(f'REFUSED: {len(hits)} entries with key = "android" in {src}')
    b = blocks[hits[0]]
    entry = tomllib.loads(b)['entry'][0]
    if entry.get('method') != 'android' or entry.get('files') != FILES:
        sys.exit(f'REFUSED: android entry has method {entry.get("method")!r} files {entry.get("files")!r}')
    for k, v in want.items():
        b, n = re.subn(rf'(?m)^{k} = ".*"$', f'{k} = "{v}"', b)
        if n != 1:
            sys.exit(f'REFUSED: {n} "{k} =" lines in the android entry')
    blocks[hits[0]] = b
    out = ''.join(blocks)
    tomllib.loads(out)
    a = text.splitlines(); c = out.splitlines()
    changed = [i for i in range(len(a)) if a[i] != c[i]] if len(a) == len(c) else None
    if changed is None or len(changed) > 2:
        sys.exit('REFUSED: more than the two lines would change')
    open(dst, 'w', encoding='utf-8').write(out)
    for i in changed:
        print(f'{i + 1}: {a[i]}\n{" " * len(str(i + 1))}  -> {c[i]}')
    print(f'wrote {dst}')


if __name__ == '__main__':
    main()
