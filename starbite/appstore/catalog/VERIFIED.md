# Seed catalog: what was verified

Checked 2026-11-12 13:39 UTC against the live sources, read-only:

- GitHub: `gh api repos/OWNER/NAME/releases/latest`; each `asset` glob (with `{version}` expanded) matches exactly
  one asset of that release; `digest` = the API returns a `sha256:` digest for that asset; checksum = the
  `sha256_asset` exists in the same release.
- Vendor URLs: Mozilla product-details JSON + `curl -sIL` on archive.mozilla.org, GIMP `gimp_versions.json` +
  download.gimp.org, each with its SHA256SUMS line found.
- Debian: `api.ftp-master.debian.org/madison?s=trixie&a=arm64,all` for every package in `[distro]`, `[runtime].apt`
  and `[source].build_deps` (trixie = `stable`). Versions below are the first package of `[distro].apt` and its extras.
- Source: tarball URLs resolve (HTTP 200); Rust `rust-version` / Go `go` directive / meson dependency floors of the
  latest tag compared with trixie's rustc 1.85.1, golang 1.24, libplacebo 7.349, libadwaita 1.7.6.
- Archive layouts (`strip` / `bin`) of the small release tarballs were confirmed with `tar -t` on the stream
  (btop, fzf, lazygit, micro, ripgrep, fd, bat, helix, neovim, syncthing). Firefox, calibre and VSCodium layouts
  are from upstream's documented layout, not unpacked.

"Too big" = Firefox / Thunderbird / LibreOffice / Inkscape / Kdenlive / calibre / VSCodium class builds: hours and
many GB on four cores, so no `[source]`.

| id | methods | latest release: exact asset matched | digest | checksum file | Debian trixie arm64 | notes (methods left out and why) |
|---|---|---|---|---|---|---|
| aisleriot | distro | - | - | - | aisleriot 1:3.22.35-1 | No release (GNOME). |
| bat | release, distro | 0.26.1: `bat-v0.26.1-aarch64-unknown-linux-gnu.tar.gz` | yes | no | bat 0.25.0-2+b2 | No source: rust-version 1.87 > trixie rustc 1.85.1. Debian binary is batcat. |
| btop | release, source, distro | 1.4.8: `btop-aarch64-unknown-linux-musl.tar.gz` | yes | no | btop 1.3.2-0.1 | Static musl build; archive paths start with ./btop/ (strip 2). No checksum asset. Source: make (C++23, gcc 14), tarball HTTP 200. |
| calibre | release, distro | 9.18.0: `calibre-9.18.0-arm64.txz` | yes | no | calibre 8.5.0+ds-1+deb13u3 | Default distro (upstream bundle is 216 MB). No checksum asset (GitHub digest only). Source: too big. |
| celluloid | distro | - | - | - | celluloid 0.28-2 | No release: source tarball only. No source: v0.30 needs libadwaita >= 1.8.0, trixie has 1.7.6. |
| drawing | distro | - | - | - | drawing 1.0.2-1 | No release: GitHub 1.0.2 has no assets (same as Debian). No source: same version as Debian. |
| fd | release, distro | 10.5.2: `fd-v10.5.2-aarch64-unknown-linux-gnu.tar.gz` | yes | no | fd-find 10.2.0-1+b5 | No source: rust-version 1.90.0 > trixie rustc 1.85.1. Debian binary is fdfind. |
| firefox | release, distro | 160.0: `firefox-160.0.tar.xz` | n/a (vendor URL) | yes (`SHA256SUMS`) | firefox-esr 140.15.0esr-1~deb13u1 | Mozilla product-details LATEST_FIREFOX_VERSION; linux-aarch64 tar.xz HTTP 200; line found in SHA256SUMS (per-file .asc also exists, not wired). Source: too big to build here. |
| foliate | distro | - | - | - | foliate 4.~really3.3.0-1 | No release: upstream's only binary is foliate_3.3.0_all.deb, identical to Debian's 3.3.0. No source for the same reason. |
| foot | distro | - | - | - | foot 1.21.0-2 | No release: Codeberg, source only (and the format has no Codeberg provider). No source for that reason. |
| fzf | release, source, distro | 0.75.0: `fzf-0.75.0-linux_arm64.tar.gz` | yes | yes (`fzf_0.75.0_checksums.txt`) | fzf 0.60.3-1+b2 | Source: go build from the tag tarball (go.mod: go 1.23; trixie 1.24), HTTP 200. |
| gcompris-qt | distro | - | - | - | gcompris-qt 25.0.12-1; qt6-wayland 6.8.2-4 | No release from upstream for aarch64 Linux. qt6-wayland added. |
| geary | distro | - | - | - | geary 46.0-7 | No release (GNOME GitLab publishes source only). No source: latest GNOME releases target newer libs than trixie. |
| gimp | release, distro | 3.2.8: `GIMP-3.2.8-aarch64.AppImage` | n/a (vendor URL) | yes (`SHA256SUMS`) | gimp 3.0.4-3+deb13u10 | gimp_versions.json STABLE 3.2.8; aarch64 AppImage HTTP 200 (302 to mirror); line in v3.2/linux/SHA256SUMS. Regex is held to 3.2.x because the URL names v3.2. Source: too big. |
| gnome-calculator | distro | - | - | - | gnome-calculator 1:48.1-2+b1 | No release (GNOME). |
| gnome-chess | distro | - | - | - | gnome-chess 1:48.0-1+b1; gnuchess 6.2.9-0.1 | No release (GNOME). gnuchess added as the engine. |
| gnome-maps | distro | - | - | - | gnome-maps 48.7-0+deb13u1 | No release (GNOME). |
| gnome-mines | distro | - | - | - | gnome-mines 1:48.1-2+b1 | No release (GNOME). |
| gnome-sudoku | distro | - | - | - | gnome-sudoku 1:48.1-1 | No release (GNOME). |
| gnome-text-editor | distro | - | - | - | gnome-text-editor 48.3-3 | No release (GNOME). |
| helix | release, distro | 25.07.1: `helix-25.07.1-aarch64-linux.tar.xz` | yes | no | hx 25.01.1-4 | No checksum asset. Debian package name is hx. No source: long cargo build + grammar fetch, no gain. |
| inkscape | distro | - | - | - | inkscape 1.4-6 | No release: GitLab release INKSCAPE_1_4 lists a source tarball only; AppImage is x86_64. Source: too big (~1 h+, 4 GB). |
| kdenlive | distro | - | - | - | kdenlive 24.12.3-2+deb13u1; qt6-wayland 6.8.2-4 | No release: download.kde.org 26.08 has x86_64 AppImages only. Source: too big. RAM 700+. |
| keepassxc | release, distro | 2.8.0: `KeePassXC-2.8.0-aarch64.AppImage` | yes | yes (`KeePassXC-2.8.0-aarch64.AppImage.DIGEST`) | keepassxc-minimal 2.7.10+dfsg1-1 | Two aarch64 AppImages in 2.8.0: KeePassXC-2.8.0-aarch64.AppImage (21 Oct) and KeePassXC-2.8.0-1-aarch64.AppImage (rebuild, 23 Oct); the glob takes the un-suffixed one. .DIGEST is sha256sum format; .sig present. Source: Qt6 app, release is better. |
| lazygit | release, distro | 0.66.3: `lazygit_0.66.3_linux_arm64.tar.gz` | yes | yes (`checksums.txt`) | lazygit 0.50.0+ds1-1+b2 | No source: go.mod says go 1.26.0, trixie has 1.24 (GOTOOLCHAIN=local in Debian). |
| libreoffice | distro | - | - | - | libreoffice-writer 4:25.2.3-2+deb13u6; libreoffice-calc 4:25.2.3-2+deb13u6; libreoffice-impress 4:25.2.3-2+deb13u6; libreoffice-gtk3 4:25.2.3-2+deb13u6 | No release: TDF's aarch64 LibreOffice_26.8.3_Linux_aarch64_deb.tar.gz resolves (HTTP 302 to mirror, .sha256 available) but it is a tarball of .debs needing dpkg, which the release formats cannot install. Source: too big. |
| lollypop | distro | - | - | - | lollypop 1.4.41-1 | No release (GNOME World GitLab, source only). |
| loupe | distro | - | - | - | loupe 48.1-3 | No release (GNOME). No source: Rust + glycin, needs newer GNOME libs. |
| micro | release, source, distro | 2.0.15: `micro-2.0.15-linux-arm64.tar.gz` | yes | yes (`micro-2.0.15-linux-arm64.tar.gz.sha`) | micro 2.0.14-1+b6 | Repo moved to micro-editor/micro (zyedidia/micro redirects). .sha is sha256sum format. Source: git clone at tag v2.0.15 (go 1.19). |
| mpv | distro | - | - | - | mpv 0.40.0-3+deb13u1 | No release: only Windows builds. No source: v0.42.0 meson needs libplacebo >= 7.387.0, trixie has 7.349. |
| nautilus | distro | - | - | - | nautilus 48.3-2 | No release (GNOME). |
| neovim | release, source, distro | 0.12.6: `nvim-linux-arm64.tar.gz` | yes | no | neovim 0.10.4-8 | No checksum asset (GitHub digest only). Source: Neovim's documented make with its bundled deps (fetched during the build), so trixie's older libtree-sitter 0.22 / libuv are not used; tarball HTTP 200. |
| openttd | distro | - | - | - | openttd 14.1-1+b2; openttd-opengfx 7.1-3; openttd-opensfx 1.0.3-3; openttd-openmsx 0.4.2-2 | No release: cdn.openttd.org 15.3 manifest has linux-generic-amd64 only. Debian 14.1 + OpenGFX/OpenSFX/OpenMSX. |
| papers | distro | - | - | - | papers 48.3-1 | No release (GNOME, source only). No source: GNOME releases track newer libs. |
| qalculate-gtk | distro | - | - | - | qalculate-gtk 5.5.1-1 | No release: v5.13.0 Linux binary is x86_64 only (qalculate-5.13.0-x86_64.tar.xz). |
| ripgrep | release, source, distro | 15.2.0: `ripgrep-15.2.0-aarch64-unknown-linux-gnu.tar.gz` | yes | yes (`ripgrep-15.2.0-aarch64-unknown-linux-gnu.tar.gz.sha256`) | ripgrep 14.1.1-1+b4 | Source: cargo (rust-version 1.85; trixie rustc 1.85.1), tarball HTTP 200. |
| stellarium | distro | - | - | - | stellarium 24.3-1; qt6-wayland 6.8.2-4 | No release: v26.3 Linux AppImages are x86_64 only. qt6-wayland added. Needs OpenGL 3.3 (software only here). |
| supertux | distro | - | - | - | supertux 0.6.3-3 | No release: v0.7.0 Linux asset is an x86_64 AppImage only (arm64 is an Android APK). Source possible but long and asset-heavy; left out. |
| syncthing | release, distro | 2.1.7: `syncthing-linux-arm64-v2.1.7.tar.gz` | yes | yes (`sha256sum.txt.asc`) | syncthing 1.29.5~ds1-2 | sha256sum.txt.asc is a PGP clear-signed sums file containing the asset's line. No source: release is official and static. |
| thunderbird | distro | - | - | - | thunderbird 1:140.12.0esr-1~deb13u1 | No release: archive.mozilla.org 160.0 and 153.7.0esr have linux-x86_64 only. Source: too big. |
| transmission | source, distro | - | - | - | transmission-gtk 4.1.0~beta2+dfsg-3+deb13u2 | No release: upstream ships Windows/macOS binaries only. Source = 4.1.4 release tarball (with submodules), HTTP 200; Debian's is a 4.1.0 beta. |
| vlc | distro | - | - | - | vlc 3.0.23-0+deb13u1; qtwayland5 5.15.15-3 | No release: VideoLAN ships no Linux binaries. qtwayland5 added for the Qt UI on Wayland. |
| vscodium | release | 1.143.17232: `VSCodium-linux-arm64-1.143.17232.tar.gz` | yes | yes (`VSCodium-linux-arm64-1.143.17232.tar.gz.sha256`) | - (not in Debian) | Not in Debian. Upstream also has VSCodium-*-anylinux-aarch64.AppImage and codium_*_arm64.deb; tarball chosen. Source: too big (Electron). |
| xournalpp | release, distro | 1.3.8: `xournalpp-1.3.8-aarch64.AppImage` | yes | no | xournalpp 1.2.6-1 | Upstream also has Ubuntu jammy/noble/resolute aarch64 .debs; AppImage chosen. No checksum asset. No source: release + distro cover it. |
| yt-dlp | release, distro | 2026.10.26: `yt-dlp_linux_aarch64` | yes | yes (`SHA2-256SUMS`) | yt-dlp 2025.04.30-1 | Asset is a single aarch64 glibc binary (also a musl one: yt_dlp_musllinux_aarch64). SHA2-256SUMS has its line. Debian 2025.04.30 is old. |

## Considered and dropped

| app | why |
|---|---|
| Krita | Debian trixie has Krita 5.2.9 (Qt 5); its main.cc sets QT_QPA_PLATFORM=xcb unconditionally on X11-enabled builds; no XWayland in the guest. Upstream AppImages are x86_64 only. |
| Mindustry | Release is `Mindustry.jar` (Java); aarch64 Linux natives for its SDL/GL backend and a usable frame rate on software GL are not verified. Left out per "only if a release truly runs". |
| Shotwell, Rhythmbox, Amberol, Dino, GNOME 2048, Audacity, Thunar, Evince | All exist in trixie arm64; left out to stay at 45 (Loupe, Lollypop, Nautilus, Papers cover the same jobs with better touch layouts). |
