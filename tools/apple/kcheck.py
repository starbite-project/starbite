#!/usr/bin/env python3
"""Compile-check a Starbite Apple-platform driver source against OUR xnu-12377 STARLTE kernel tree, writing nothing.
    tools/apple/kcheck.py starbite/apple-drivers/StarbiteUFS/StarbiteUFS.cpp [more.cpp|.c ...] [-I DIR ...]
Takes the exact compiler arguments the kernel build used for a built-in IOKit C++ file
(iokit/Kernel/arm/SweetPlatformExpert.cpp) or a BSD C file (bsd/dev/herolte_ufs.c) from
build/dxb-12377's compile_commands.json, swaps in the given source, adds -fsyntax-only and the source's own
folder as an include path, and drops every output/dependency-file flag. Nothing is written into the build tree.
Exit status = number of failing files. Errors and warnings are printed."""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CC = ROOT / 'build/dxb-12377/build/xnu.obj/DEVELOPMENT_ARM64_STARLTE/compile_commands.json'
REF = {'.cpp': 'SweetPlatformExpert.cpp', '.c': 'herolte_ufs.c'}
DROP1 = {'-o', '-MF', '-MT', '-MQ'}
DROP0 = {'-MD', '-MMD', '-c'}


def base_args(ext, db):
    e = [x for x in db if x['file'].endswith('/' + REF[ext])][0]
    a = e.get('arguments') or e['command'].split()
    out, skip = [], False
    for x in a:
        if skip:
            skip = False
            continue
        if x in DROP1:
            skip = True
            continue
        if x in DROP0 or x == e['file'] or x.endswith('/' + REF[ext]):
            continue
        out.append(x)
    return out, e['directory']


def main():
    args = sys.argv[1:]
    inc, files = [], []
    i = 0
    while i < len(args):
        if args[i] == '-I':
            inc += ['-I', str(Path(args[i + 1]).resolve())]
            i += 2
        else:
            files.append(args[i])
            i += 1
    if not files:
        sys.exit(__doc__)
    db = json.load(open(CC))
    env = dict(os.environ, DEVELOPER_DIR='/Applications/Xcode_16.4.app/Contents/Developer')
    bad = 0
    for f in files:
        p = Path(f).resolve()
        a, cwd = base_args(p.suffix, db)
        cmd = a + ['-fsyntax-only', '-I', str(p.parent)] + inc + [str(p)]
        r = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True)
        lines = [l for l in (r.stdout + r.stderr).splitlines()
                 if ('error' in l or 'warning' in l) and 'no such include directory' not in l]
        print(f'== {f}: {"OK" if r.returncode == 0 else "FAIL"} ({len(lines)} diagnostics)')
        for l in lines[:80]:
            print('  ' + l)
        bad += r.returncode != 0
    sys.exit(bad)


if __name__ == '__main__':
    main()
