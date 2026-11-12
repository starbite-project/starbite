# Starbite App Store: the recipe format (catalog v1)

One TOML file per app in `catalog/<id>.toml`. `tools/catalog.py build` validates every recipe and writes one
`catalog.json` for the guest (`/usr/share/starbite/appstore/catalog.json`). Apps run in the Linux guest on the
M3 cores: Debian trixie arm64 (aarch64, glibc), Wayland only (sbwl), no GPU (software rendering), ~1.5 GiB RAM.

An app offers one to three **install methods**. The user picks one per app on its page:

| method | what it does | needs |
|---|---|---|
| `release` | the upstream project's latest release for aarch64 Linux (GitHub / GitLab / a vendor URL) | network or the offline mirror |
| `source` | fetches the source of the latest release (or a pinned tag / branch) and compiles it on the M3s | network or the mirror; build deps from apt |
| `distro` | the Debian trixie arm64 package (apt) | the network or the card's offline repo |

Only offer a method that really works on aarch64. No fake options: an app with no aarch64 release has no
`[release]` table.

## Top level

```toml
id = "mpv"                    # [a-z0-9][a-z0-9.+-]*, unique, = the file name
name = "mpv"                  # the store's title (<= 31 bytes)
summary = "A free, small media player"            # one line (<= 95 bytes)
description = """Longer text, plain words, 1-4 sentences (<= 1000 bytes)."""
category = "Media"            # Internet | Office | Graphics | Media | Games | Utilities | Development | Education | Science | System
licence = "GPL-2.0-or-later"  # SPDX id, or "Proprietary (free)" when upstream binaries are not open source
homepage = "https://mpv.io"
icon = "mpv"                  # a freedesktop icon name (looked up in the installed app / theme), or "icons/<id>.svg|png" in this repo
icon_url = "https://..."     # optional: an icon to fetch when no Debian package has one (the store's tile otherwise)
ram_mb = 150                  # typical resident memory in use (honest; the store warns above what the guest can give)
featured = false              # shown on Today
default = "release"           # the method preselected on the app page
touch = "good"                # good | ok | poor: how usable with touch only (no mouse, no hardware keyboard)
keywords = ["video", "player"]
```

## `[launcher]`: how the app shows on the home screen

```toml
[launcher]
exec = "mpv --player-operation-mode=pseudo-gui"   # relative names resolve on PATH (/usr/local/bin first)
terminal = false              # true: runs inside foot (CLI / TUI apps)
keyboard = false              # true: keep Starbite's keyboard up (terminals, games typed with keys)
desktop = "mpv.desktop"       # optional: use the .desktop file the install brings (Exec / Icon are rewritten)
env = { MPV_HOME = "~/.config/mpv" }
```

Every installed app gets `/usr/local/share/applications/sbstore-<id>.desktop`; sbwl picks it up (`rescan`).

## `[release]`: the latest upstream release

```toml
[release]
provider = "github"           # github | gitlab | url
repo = "neovim/neovim"        # github/gitlab: owner/name (gitlab: also `host = "gitlab.gnome.org"`)
asset = "nvim-linux-arm64.tar.gz"   # glob (fnmatch) on the asset names; {version} expands
prerelease = false            # take pre-releases too
tag_pattern = "v*"            # optional: only tags matching this (fnmatch); first match, newest first
version_from = "tag"          # tag | name | "regex:<python regex with one group>" on the tag
format = "tar"                # tar (any compression) | zip | appimage | deb | binary
strip = 1                     # leading path components to drop (tar/zip)
bin = ["bin/nvim"]            # files to link into /usr/local/bin (relative to the install dir)
size_mb = 12                  # installed size estimate (disk check; the download size comes from the API)

# provider = "url": a vendor endpoint
# version_url = "https://product-details.mozilla.org/1.0/firefox_versions.json"
# version_json = "LATEST_FIREFOX_VERSION"     # a key in the JSON at version_url, or
# version_regex = "..."                       # a regex with one group on version_url's text
# url = "https://archive.mozilla.org/pub/firefox/releases/{version}/linux-aarch64/en-US/firefox-{version}.tar.xz"

[release.verify]              # what upstream gives; all optional, but say which exists
sha256_asset = "{asset}.sha256sum"   # an asset with "<hex>  <name>" lines or one bare hex
sha256sums_url = "https://.../SHA256SUMS"   # a sums file at a URL ({version} expands); the line for our file
sha256_line = "linux-aarch64/en-US/firefox-{version}.tar.xz"   # which line of the sums file (default: the asset's name)
github_digest = true          # GitHub's own asset "digest" (sha256) from the API (2025+)
signature = "{asset}.asc"     # recorded and fetched; checked with gpgv when `key` is set and gpgv exists
key = "keys/<id>.asc"
```

## `[source]`: compile it

```toml
[source]
git = "https://github.com/mpv-player/mpv"   # clone URL (shallow, at the ref)
tarball = "https://github.com/mpv-player/mpv/archive/refs/tags/{tag}.tar.gz"   # preferred over git when set
ref = "release"               # release: the tag of the latest release (same resolver as [release]; needs a
                              # [release] table or `repo`); or a tag / branch name
repo = "mpv-player/mpv"       # for ref = "release" when there is no [release] table
provider = "github"
system = "meson"              # meson | cmake | autotools | make | cargo | go | python | script
build_deps = ["meson", "ninja-build", "pkgconf", "libavcodec-dev"]   # Debian packages (installed, marked auto)
options = ["-Dlibmpv=false"]  # extra configure options for meson / cmake / autotools
steps = []                    # system = "script": shell lines run in the source dir with $PREFIX, $JOBS set
bin = ["bin/mpv"]             # relative to the install prefix
build_min = 15                # typical minutes on the four M3 cores (the store shows it)
build_mb = 900                # disk the build needs (checked before starting)
```

The prefix is `/opt/sbstore/<id>/<version>`; meson/cmake/autotools get `--prefix` set; the engine runs the
standard configure / build / install for the system, then `steps` (if any) after them.

## `[distro]`: the Debian package

```toml
[distro]
apt = ["mpv"]                 # the first is the main one
```

## `[runtime]`: what the app needs at run time

```toml
[runtime]
apt = ["libgtk-3-0t64"]       # Debian runtime libraries for release / source installs (required)
recommends = ["ffmpeg"]       # best effort: installed when apt can get them (offline: noted, the install goes on)
needs = ["network", "audio"]  # network | audio | gpu | camera | files | keyboard (shown on the app page)
note = "Plays local files; streaming needs Wi-Fi."
```

## Install layout (guest)

`/opt/sbstore/<id>/<version>/` (one version back is kept for rollback), `/opt/sbstore/<id>/current -> <version>`,
links in `/usr/local/bin`, `/usr/local/share/applications/sbstore-<id>.desktop`, the icon in
`/usr/local/share/icons/hicolor/<size>/apps/sbstore-<id>.png|svg`, state in `/var/lib/sbstore/state.json`, logs in
`/var/log/sbstore/<id>.log`.
