# Pending 12377 patches for the macOS 26.5 userland

Made on `build/xnu-12377` (branch `port-12377`, one commit per patch, `git format-patch -1`);
context and evidence in `docs/MACOS.md` §11.

| Patch | Port-tree commit | State |
|---|---|---|
| `0351-sweet-a64e-fhm.patch` | 47cd4da6 | copied into `xnu/series-12377` by the main session (5e2f2e3), builds clean |
| `0352-sweet-a64e-sha3-sha512-sb.patch` | f1e38734 | single-file compile check clean (dxb's flags); not linked yet |

To take 0352: `cp xnu/patches/pending-macos26/0352-*.patch xnu/series-12377/` and
`FRESH=1 tools/build-xnu-12377.sh`. Host check of the emulation code:
`tools/a64e/difftest/run.sh`.
