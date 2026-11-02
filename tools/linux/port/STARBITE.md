# Starbite's copy of the S9+ port

A subset of SheroAbi/galaxy-s9plus-mainline-linux at bcee6014fc2e (the full
upstream file list is in `.upstream-files.txt`). Files here are kept as
upstream ships them; Starbite's changes are applied to a work copy in the VM
(`~/linux/work`) by `tools/linux/patch-mainline.py --port`, never to this tree.

Left out on purpose:

| Upstream path | Why |
|---|---|
| `device/` | the port's initramfs inits. `device/mainline-init` mounts sda25, which is Starbite's USERDATA (iOS + Kiosk), read-write. Starbite builds no built-in initramfs at all; the initrd is the store file `linux.initrd` |
| `branding/` | the port's boot logo (CONFIG_LOGO is off in `tools/linux/config/starbite.config`) |
| `bootloader/uniLoader/` | the Starbite loader replaces it |
| `image/`, `extras/`, `docs/`, `scripts/flash/` | Ubuntu 24.04 image build and flashing for the S9+; Starbite has its own |
| `firmware/bcmdhd_sta.bin_b2`, `nvram.txt_r02a_b2`, `bcmdhd_clm.blob` | Broadcom BCM4361 firmware (not GPL). Not committed: `tools/linux/build-kernel.sh` copies them from this phone's own vendor partition copy (`build/linux-bundle/firmware/` in the main checkout, git-ignored) and checks them against `firmware/broadcom.sha256`. The upstream port ships the same three files (identical hashes) as "redistributed as shipped on the device" |
| `firmware/bcm4361B2_semco.hcd` | Bluetooth patchram; no Bluetooth in this kernel |

`regulatory.db` and `regulatory.db.p7s` stay: wireless-regdb, ISC licence
(see `THIRD-PARTY.md`).
