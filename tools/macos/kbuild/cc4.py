#!/usr/bin/env python3
"""Compile the given xnu sources (paths relative to the tree) from SRC with dxb-12377's own compile commands:
same flags and working directory, output to OUT/<name>.cpo|.o, file names mapped back to the tree's path."""
import json, os, shlex, subprocess, sys
O = '/Users/dev/project-starbite/build/dxb-12377/build/xnu.obj/DEVELOPMENT_ARM64_STARLTE'
T = '/Users/dev/project-starbite/build/dxb-12377/xnu/'
SRC, OUT = sys.argv[1].rstrip('/') + '/', sys.argv[2]
db = {x['file']: x for x in json.load(open(O + '/compile_commands.json'))}
for rel in sys.argv[3:]:
    e = db[T + rel]
    args = e.get('arguments') or shlex.split(e['command'])
    out = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == T + rel:
            out.append(SRC + rel)
        elif a == '-o':
            out += ['-o', os.path.join(OUT, os.path.basename(args[i + 1]))]
            i += 1
        elif a in ('-MD', '-MMD'):
            pass
        elif a in ('-MF', '-MT', '-MQ'):
            i += 1
        else:
            out.append(a)
        i += 1
    out += ['-ffile-prefix-map=' + SRC + '=' + T]
    r = subprocess.run(out, cwd=e['directory'], capture_output=True, text=True)
    print(rel, 'exit', r.returncode)
    if r.returncode or r.stderr.strip():
        print(r.stderr[-3000:])
