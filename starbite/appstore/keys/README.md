# Upstream signing keys (public), pinned for the App Store's signature checks

| File | Signer | Fingerprint | From | Checked |
|---|---|---|---|---|
| `mozilla.asc` | Mozilla Software Releases (signs Firefox's SHA256SUMS) | 14F26682D0916CDD81E37B6D61B7B526D98F0353 | archive.mozilla.org/pub/firefox/releases/160.0/KEY | sqv verified 160.0's SHA256SUMS.asc, 2026-11-12 |
| `libreoffice.asc` | LibreOffice Build Team (code signing) | C2839ECAD9408FBE9531C3E9F434A1EFAFEEAEA3 | keyserver.ubuntu.com (keys.openpgp.org's copy lacks the binding signature Sequoia needs) | sqv verified 26.8.3's aarch64 deb help pack .asc, 2026-11-12 |
| `keepassxc.asc` | KeePassXC Release <release@keepassxc.org> | BF5A669F2272CF4324C1FDA8CFB4C2166397D0D2 (signing subkey C1E4CBA3AD78D3AFD894F9E0B7A66F03B59076A8) | keyserver.ubuntu.com | primary fingerprint as KeePassXC documents it, 2026-11-12 |

A recipe names its key and the expected fingerprint (`[release.verify] key`, `fingerprint`); sbstore checks with `sqv`
(the guest base has it; apt uses it) or `gpgv`, and refuses the install when the signature or the signer is wrong.
