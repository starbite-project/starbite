# Starbite App Store (Track F)

Our own app store for the Linux apps that run in the guest on the S9's M3 cores. Each app installs the way the
user picks on its page:

- **Latest release**: the upstream project's newest aarch64 build (GitHub / GitLab / the vendor's site), with
  the checksum upstream publishes.
- **Build from source**: the newest release's source compiled on the four M3 cores, or Debian's own source with
  `dpkg-buildpackage`, which always fits trixie's libraries.
- **Debian package**: apt, from the card's offline repo or the network.

## Pieces
| File | What |
|---|---|
| `FORMAT.md` | the recipe format (one TOML per app) |
| `catalog/*.toml`, `catalog/VERIFIED.md` | the 45 seed recipes, and the check of every release asset against the live APIs |
| `sbstore.py` | the engine (Python 3.11+, stdlib only). CLI: `list info resolve install update rollback remove check status log`; `daemon` = the guest's store daemon (sbpkg's role) |
| `sbshm-store3.h` | the v3 table the daemon publishes next to `struct sbshm_store`: methods, versions, licence, source, description, phases |
| `install-rootfs.sh ROOT` | installs the engine, catalog, icons (and an offline mirror) into a guest root. Track E's `rootfs/base-customize.sh` calls it |
| `tools/catalog.py` | `check`, `build OUT [--resolve]`, `resolve [ID]` |
| `tools/mirror.py`, `tools/mirror-debs.sh` | the offline mirror: release assets, source tarballs, and .debs for build deps (a `file:` apt source) |
| `tools/icons.py` | store icons from the apps' Debian packages: 160 px ARGB for the icon pool, 256 px PNG for launchers |
| `tools/vm-chroot.sh` | a Debian trixie arm64 test root in the Lima VM (aarch64 native) |
| `tests/test_sbstore.py` | host tests: fake GitHub API, a Range-capable HTTP server with a flaky mode, fake apt/dpkg, fake sbwl |
| `tests/vm-test.sh` | real releases, real builds and real apt in the VM's trixie root |

## How an install goes
1. **Resolve.** Ask GitHub, GitLab or the vendor for the latest version, or apt for the candidate. With no
   network, use the offline mirror.
2. **Check space.** The download, the installed size and the build space must fit, with 64 MB kept back.
3. **Fetch.** Download to `/var/lib/sbstore/dl` as a `.part` file, resuming with HTTP Range after a break. Check
   the size and the sha256 (from GitHub's digest, a sums asset or a sums URL). If upstream gives no checksum, the
   app page says "unverified".
4. **Install.**
   - Releases unpack into `/opt/sbstore/<id>/.new-<version>`, which is then renamed to `<version>`.
   - Source builds install straight into `/opt/sbstore/<id>/<version>`.
   - `current` is flipped atomically. Links go into `/usr/local/bin`, and the launcher into
     `/usr/local/share/applications/sbstore-<id>.desktop`; sbwl sees it after `rescan`.
5. **Keep and clean up.** One version back is kept for rollback; older ones are deleted. Debian packages the app
   needed are reference-counted, and a removal never takes a base package (simulated first).
6. **Log.** `/var/log/sbstore/<id>.log` holds every command and its output.

## Tests
```
python3 starbite/appstore/tests/test_sbstore.py            # host, ~1 min
python3 starbite/appstore/tools/catalog.py check
limactl shell starbite -- $PWD/starbite/appstore/tools/vm-chroot.sh make      # once
limactl shell starbite -- $PWD/starbite/appstore/tests/vm-test.sh $PWD/starbite/appstore/catalog neovim-release btop-source gnome-mines-distro
```
