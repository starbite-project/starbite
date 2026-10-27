#!/usr/bin/env python3
"""Rewrite xnu/patches/0306 from xnu/src/starlte_overlay.c (new file) and the
hooks in tools/overlay/0306-hooks.diff (bsd/conf/files, bsd_init.c, vfs_subr.c)."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
name = 'bsd/dev/starlte_overlay.c'
src = (ROOT / 'xnu/src/starlte_overlay.c').read_text().splitlines()
new = (f'diff --git a/{name} b/{name}\nnew file mode 100644\n--- /dev/null\n+++ b/{name}\n'
       f'@@ -0,0 +1,{len(src)} @@\n' + ''.join('+' + l + '\n' for l in src))
hooks = (ROOT / 'tools/overlay/0306-hooks.diff').read_text()
(ROOT / 'xnu/patches/0306-starlte-ios-overlay.patch').write_text(new + hooks)
print('0306 regenerated')
