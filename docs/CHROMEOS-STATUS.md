# ChromiumOS on the S9: status (2026-11-05)

Boot: menu entry **ChromiumOS** (catalog key `chromeos`, method linux). The init (tools/linux/init, boot_disk)
attaches `/distros/chromeos.img` on the microSD with `losetup -P`, mounts ROOT-A (p3) read-only, starts swtpm
(software TPM 2.0 through the vTPM proxy, state in `/distros/chromeos-tpm`), copies the kernel log to
`/distros/chromeos-boot.log` (previous boot: `.1`) and switches to ChromiumOS's upstart.
Command line: `sb.root=/distros/chromeos.img printk.devkmsg=on log_buf_len=4M s9p_decon.drm_name=vkms cros_debug
lsm=landlock,lockdown,yama,safesetid,integrity,selinux,bpf enforcing=0`.

The software TPM is not secure: sign in only with a throwaway Google account.

## Image
Google's `arm64-generic-public` R158-16868.12.0 test image (6,807,372,800 bytes; original SHA-256 starts 1f17d26f).
Copies: the Lima VM `~/chromeos/chromeos.img` and the card. Both carry the same edits; same-length edits go into
both with tools/chromeos/inplace-edit.py (one 4 KiB block, before/after hashes checked) instead of re-sending 6.8 GB.

| Edit | Where | Why |
|---|---|---|
| ROOT-A RO lock cleared | p3 superblock byte 0x467 = 0 | ext2 FEATURE_R24-31 lock blocks mounting rw for edits |
| Wi-Fi firmware | /lib/firmware/brcm | BCM4361 |
| `--force-device-scale-factor=3` | /etc/chrome_dev.conf | phone-sized UI |
| no idle suspend / dark resume | power_manager board_specific | no suspend support |
| STATE grown to 4 GiB | GPT p1 4907008-13295615 | room for the user's data |
| librootdev patches | tools/chromeos/patch-rootdev.py | stateful/root found on loop0p3 (259:x) |
| ui.conf output to /dev/kmsg | /etc/init/ui.conf line 75 | Chrome's log in the card's boot log (stateful is encrypted) |
| `--enable-logging=stderr --log-level=0` | /etc/chrome_dev.conf | same |
| `--render-node-override=/dev/dri/card2` | /etc/chrome_dev.conf | minigbm's vkms backend (dumb buffers, scanout) on the DECON |
| Mesa `vgem` -> `vkms` | libEGL.so.1.0.0 +0x32bd | Mesa 21.3 only uses kms_swrast (dma-buf import) on vgem/virtio_gpu |

## Kernel (tools/linux/config/starbite.config, ChromeOS section)
dm/verity/crypt, fscrypt, keys, TPM + vTPM proxy, SafeSetID, uinput, SELinux (picked with lsm=, permissive),
cgroup v1 cpuset/memory and CFS bandwidth (ui-pre-start), xt_owner, hidraw. No `loop.max_part`: libbrillo opens
`/dev/loop<minor>`.

## Boots
1. panic: no selinuxfs -> SELinux. 2-3. "repairing itself": rootdev missed the loop root -> librootdev patch.
4. no TPM -> swtpm. 5. encstateful: no loop device (max_part) -> drop it, rootdev patch 2.
6. logo forever: ui pre-start exit 2 (cpu.cfs_quota_us) -> kernel cgroup options.
7. black: Chrome runs OOBE, but no scanout buffer (minigbm on Panfrost's render node, Mesa swrast without DRM),
   GPU process gave up after 6 crashes -> render node override + Mesa kms_swrast on the DECON.

## Next
CR2: Mesa Panfrost (kmsro with the DECON) and minigbm on Panfrost for GPU acceleration; then login, Wi-Fi, touch.
