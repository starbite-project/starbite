# Starbite Phase 3: Linux rebuild plan

Sources: the three findings from the workflow, the skeptic verdicts on the GPU analysis, and spot checks in the repo. The checks were `tools/store/catalog.toml`, `sbstore2.py` CLI, `shim/starlte/xnu_boot.c:438-449` (memory map), `tools/linux/init:106-153`, `http-proxy.py` (CONNECT is supported) and NOTES.md:1142-1162. Nothing was edited, built or flashed.

**Three facts shape the plan:**
- **The kernel reflash is the slow step.** The UFS store is written from TWRP (`tools/store/store-flash.sh`), which means button presses. Everything after Step 1 is designed to live on the card, so kernel reflashes happen in rare batches.
- **The Mac has 4 GiB free, and the Lima VM's disk only grows.** Large roots are built on the phone, as in milestone 3. The VM only builds the kernel, the initrd and small tarballs.
- **"Keep only the driver work"** means: the port at bcee601, the 28-file in-tree diff, `mk-starlte-dts.py`, `patch-mainline.py` and the loader contract. Everything in userland is rebuilt.

---

## 1. GPU: why Linux runs at "6 fps"

### 1.1 Ranked causes (after the skeptic verdicts)

Two verdicts change how the first analysis should be read:
- The DECON flip bug is a **certain 30 fps ceiling, but it cannot produce 6 fps** by itself.
- "260 MHz for 99.8% of the time" (NOTES.md:1177-1178) means the GPU stayed **under 45% busy**. simple_ondemand jumps to the maximum above 45%. So pinning the clock is unlikely to be the main cure.

The causes below are therefore ranked by how likely each one is to explain the 5x gap below 30 fps. The flip ceiling is listed separately because it is certain.

| # | Cause | Status | Cost if true |
|---|---|---|---|
| C | **DECON flip event armed one TE late.** `s9p_decon_pipe_update` (~/linux/s9p/src/gpu/drm/s9p/s9p-decon.c:469-516) polls for the shadow latch (l.498), then calls `drm_crtc_arm_vblank_event` (l.510). That sets seq = count+1 (drm_vblank.c:1122), but the latching TE has already bumped count (l.301). Result: one flip per two TEs. | Certain from the code. It assumes the latch happens at frame start, which `modetest -v` confirms | Hard cap of 30 fps (20 on a missed edge) |
| 1 | **GPU job faults and resets.** Possibly from undervolt at the 260 MHz step (s9p-g3d.c:82-86 shows 850 mV there; NOTES.md:1212 says jobs need 950 mV). Possibly from the known G72 r0p1 DATA_INVALID / VA 0 faults (patch_tree.py:214-220). Each timeout and reset stalls for hundreds of ms. | Plausible. The inventory says the rail is held at 950 mV with `step_volt` off, which conflicts with the 850 mV reading, so `rail_uv` settles it | Single-digit fps |
| 2 | **Apps rendering in software** (llvmpipe, or wl_shm buffers). Only KWin's renderer was ever checked. | Unverified | Single-digit fps for apps |
| 3 | **G3D power-domain cycling.** 50 ms autosuspend (panfrost_drv.c:996) and `always_on=false` (s9p-g3d-pd.c:80). Every resume costs two TZPC SMCs, a PMU power-up and a Panfrost reset (s9p-g3d-pd.c:143-270). | Recorded config. The cost has not been measured | ms per frame. Busy time resets after each resume, so the clock stays at the bottom |
| 4 | **CPU-bound QML with slow DVFS.** `transition_delay_us` is 20 ms (s9p-cpufreq.c:433), with floors of 350/400 MHz. | Plausible | Large for Plasma, smaller for Phosh |
| 5 | **Memory bandwidth.** MIF is fixed where S-Boot left it, and raising it froze the phone (NOTES.md:1676, recorded under iOS, same hardware). A frame is 1440x2960 with linear buffers, about 34 MB per full-screen pass, at fractional scale 2.25. | Plausible. It cannot be fixed now | Bounds everything; an integer scale helps |
| 6 | **GPU clock stuck at 260 MHz.** DEVFREQ_GOV_PERFORMANCE is not built (kernel.config:4268-4271). | Recorded, but the GPU was under 45% busy | Moderate, only once 1-3 are fixed |
| 7 | **DMA32 squeeze.** The `S9P_GEM_DMA32` hunk (panfrost_gem.c:492) puts every Panfrost buffer in the ~1.9 GB low zone, shared with 320 MiB of CMA. It gives no benefit here: with kmsro, KWin scans out CMA dumb buffers, never Panfrost shmem. | Plausible | Reclaim and compaction stalls |
| 8 | **Synchronous log consoles** (ramoops plus earlycon plus fbcon, loglevel=7, keep_bootcon), and failed KWin direct-scanout test commits (the plane is XRGB8888 only and shmem dma-bufs are rejected). | Minor | Small |

Two items are dropped from the original fix list:
- **The custom `atomic_commit_tail` to remove `wait_for_vblanks`.** Both verdicts show it waits on the same TE as the late event. Removing it gains nothing.
- **"Pin 572 MHz for a large gain".** The prediction is downgraded. Measure it, don't assume it.

### 1.2 Confirmation: one triage boot, in this order

Run it from the Mac over SSH or telnet to 172.16.42.1. Save the output to `build/linux/runs/gpu-triage-<date>/`.

```sh
# 0. Is KWin/phoc really on s9p-decon (not the exynos-bootfb CPU-copy path, also named "exynos")?
for c in /sys/class/drm/card*; do echo $c $(readlink $c/device/driver); done

# 1. Faults and resets (cause 1), and the rail voltage at each clock step
dmesg | grep -Ei 'panfrost|js fault|job owner|timeout|reset|DATA_INVALID' | tail -50
cat /sys/module/s9p_g3d/parameters/{cur_khz,rail_uv,step_volt}

# 2. Software rendering (cause 2)
grep -l llvmpipe /proc/*/task/*/comm 2>/dev/null
grep -h drm-driver /proc/*/fdinfo/* 2>/dev/null | sort | uniq -c
ls -l /proc/*/fd 2>/dev/null | grep renderD128
env | grep -E 'LIBGL_ALWAYS_SOFTWARE|QT_QUICK_BACKEND|PAN_MESA_DEBUG|GALLIUM_DRIVER'; cat /etc/environment
QSG_INFO=1 <app> 2>&1 | grep -i renderer        # per app

# 3. Power-domain cycling (cause 3): read twice, 10 s apart while scrolling
cat /sys/module/s9p_g3d_pd/parameters/power_ons /sys/module/s9p_g3d_pd/parameters/power_offs

# 4. Clocks and load
cat /sys/class/devfreq/*gpu*/{cur_freq,trans_stat}
cat /sys/devices/system/cpu/cpufreq/policy*/stats/time_in_state
top -H -d1 -n5 -b | head -40                     # pegged QML thread?

# 5. A/B steps, measuring fps after each (Phosh: fps from glmark2 or the phoc damage debug; KWin: showfps)
echo 1  > /sys/module/s9p_g3d_pd/parameters/always_on
echo 572000000 > /sys/class/devfreq/<gpu>/min_freq
echo performance | tee /sys/devices/system/cpu/cpufreq/policy*/scaling_governor

# 6. Flip ceiling alone, with the session stopped (libdrm tests; in _tools.img)
modetest -M exynos -s <conn>@<crtc>:1440x2960 -v  # ~30 Hz = cause C confirmed
# 7. GPU without a compositor
kmscube -D /dev/dri/card2 ; glmark2-es2-drm --off-screen=false
# 8. Memory pressure (cause 7)
grep -A5 'zone *DMA32' /proc/zoneinfo; grep -E 'allocstall_dma|compact_stall' /proc/vmstat
```

**How to read the results:**
- If `kmscube` and `glmark2-es2-drm` reach about 30 fps but the shell crawls, the cause is in the compositor or apps (2, 4, 5).
- If faults are present, cause 1 dominates. Fix it before anything else.
- If `modetest -v` shows about 60 Hz, the latch happens at frame done and cause C is not real.

### 1.3 Fixes

All of these go into the one Step 1 kernel (section 2.3), behind runtime switches, so the A/B tests need no reflash.

1. **DECON flip (cause C).** In `s9p_decon_pipe_update`, call `drm_crtc_arm_vblank_event` before the address write and drop the second `s9p_wait_shadow_idle` poll. The alternative is to keep the event pending and send it from the frame-start IRQ once `DECON_SHADOW_REG_UPDATE_REQ` reads clear.
   - Put it behind `module_param(early_event, bool, 0644)` (default on), so `echo 0 > /sys/module/s9p_decon/parameters/early_event` reverts at runtime.
   - Expected result: 60 flips/s. Tearing is still avoided, because the old buffer is released only at the latching edge.
2. **Faults and voltage (cause 1).** If `rail_uv` reads below 950000 at 260 MHz, force 950 mV on every step (the DTS OPPs already say 950 mV). If faults still appear at 950 mV, drop the 260 MHz OPP from `exynos9810-star2lte.dts:832-848`, in mk-starlte-dts.py, so the floor is 455 MHz.
3. **Power domain (cause 3).** Make `s9p_g3d_pd.always_on` writable and set it per distro. Raise Panfrost's autosuspend to about 1000 ms for this compatible (a small hunk added to patch-mainline.py). Measure power after the change.
4. **devfreq (cause 6).** Build `CONFIG_DEVFREQ_GOV_PERFORMANCE=y`. Set the floor from a udev rule installed in every distro:
   `ACTION=="add", SUBSYSTEM=="devfreq", KERNEL=="17500000.gpu", ATTR{min_freq}="455000000"`
5. **CPU (cause 4).** Lower `transition_delay_us` to 5000 in s9p-cpufreq.c:433 (check that ACPM BUCK writes keep up). Use UCLAMP for the compositor and shell: `uclamp.min` via a systemd `CPUSchedulingPolicy` drop-in, or `chrt`/`uclampset` in OpenRC. Port the s9p-stability M3 `scaling_min_freq 1066000` as a per-distro rule.
6. **DMA32 (cause 7).** Drop the `S9P_GEM_DMA32` hunk for the S9 build (patch-mainline.py removes it after patch_tree.py runs).
7. **Logging (cause 8).** Non-bring-up bootargs: `loglevel=4`, no `keep_bootcon`, no `earlycon` (section 2.3).
8. **Userland, every distro:**
   - `PAN_MAX_AFBC_PACKING_RATIO=0` in `/etc/environment`. Never set `PAN_MESA_DEBUG=noafbc` or `sync`.
   - An integer scale of 3 (phoc `scale = 3`, or Plasma at 300%).
   - Make sure apps pick panfrost through kmsro.

**Gate:** no Plasma rebuild until Phosh measures at least 45 fps with the clocks at their defaults.

---

## 2. Driver work to keep, and a reproducible kernel build

### 2.1 Vendor everything into the repo (today it exists only in the VM)

| Into the repo | From |
|---|---|
| `tools/linux/port/` (git subtree of SheroAbi/galaxy-s9plus-mainline-linux @ **bcee6014fc2e**). Kept: `src/` (12 drivers: ufs-exynos9810 plus CAL, s9p-clkgate, s9p-g3d-pd, s9p-g3d, s9p-acpm(.h), s9p-max77705-muic, max77705_charger, phy-exynos9810-usbdrd, pcie-exynos9810, s9p-decon (+Kconfig/Makefile), s9p-cpufreq; exynos-bootfb is generated), `scripts/build/{patch_tree.py,apply_drivers.sh,build71.sh,purge_fbband.py,fetch_mainline.sh,fetch_busybox.py,toolchain.sh}`, `src/mainline-dts/exynos9810-star2lte.dts`, `firmware/` (brcmfmac4361-pcie.bin/.txt/.clm_blob, regulatory.db/.p7s), `src/reference/` | `~/linux/s9p` |
| `tools/linux/patches/0000-reference-kernel-source.patch` (68,980 B, 28 files). This is a reference copy for diffing; the build regenerates the patch from the scripts | `~/linux/work/dist/m71-20261026T223900Z/kernel-source.patch` |
| `tools/linux/config/reference-m71.config` | `~/linux/out/kernel.config` |

Starbite's own layer stays and is extended:
- `tools/linux/mk-starlte-dts.py`: 4 GB memory, 3000 mAh battery, `starbite,rtc-epoch`, the microSD block. Bootargs and CMA change, see 2.3.
- `tools/linux/patch-mainline.py`: keep `starbite,max-data-timeout` (required) and `dma-32bit`. Add the hunks from 1.3: drop DMA32, autosuspend, and the DECON `early_event` patch (applied to the port's s9p-decon.c in the work copy).

The two edits to the 28 mainline files that must not be lost:
- psci.c `s9p_vendor_m3_defaults`. Without it, CPUs 4-7 never come up.
- The brcmfmac commonring H2D stamping. Without it, the firmware returns BADOPTION.

### 2.2 VM setup, written down once (`vm/starbite.yaml` provision or `tools/linux/vm-setup.sh`)

```sh
sudo apt-get install -y gcc-aarch64-linux-gnu flex bison bc libssl-dev libelf-dev dtc cpio \
  python3 rsync kmod e2fsprogs zstd mmdebstrap
printf '#!/bin/sh\nexec "$@"\n' | sudo tee /usr/local/bin/qemu-aarch64-static; sudo chmod +x $_   # arm64 host
BUILD=/mnt/build bash /Users/dev/project-starbite/tools/linux/port/scripts/build/fetch_mainline.sh
git -C /mnt/build/src/mainline rev-parse HEAD   # must be 8cd9520d35a6 (v7.1)
python3 .../port/scripts/build/fetch_busybox.py /mnt/build/out/busybox-arm64
```

Rule: `build-kernel.sh` starts with `git -C /mnt/build/src/mainline reset --hard 8cd9520d && git clean -fdx`, so every build starts from a clean tree. Today the tree is left dirty between builds.

### 2.3 Kernel changes for multi-distro (one build, one TWRP flash)

`tools/linux/build-kernel.sh` keeps its order: rsync port → work, mk-starlte-dts, patch-mainline, build71.sh. It adds a config fragment `tools/linux/config/starbite.config`, merged after build71.sh's enables with `scripts/kconfig/merge_config.sh -m` followed by `olddefconfig`:

```
# command line: the loader's bootargs reach the kernel (arm64 has FROM_BOOTLOADER or FORCE only)
# CONFIG_CMDLINE_FORCE is not set
CONFIG_CMDLINE_FROM_BOOTLOADER=y
CONFIG_LOCALVERSION="-starbite"
# CONFIG_LOCALVERSION_AUTO is not set
CONFIG_INITRAMFS_SOURCE=""            # initrd becomes the store file linux.initrd
CONFIG_BLK_DEV_INITRD=y
CONFIG_RD_GZIP=y
CONFIG_RD_ZSTD=y
CONFIG_DEVFREQ_GOV_PERFORMANCE=y
CONFIG_OVERLAY_FS=y
CONFIG_FUSE_FS=y
CONFIG_SQUASHFS=y
CONFIG_SQUASHFS_ZSTD=y
CONFIG_SQUASHFS_XZ=y
CONFIG_EROFS_FS=y
CONFIG_ZRAM=y
CONFIG_PSI=y
CONFIG_NETFILTER=y
CONFIG_NF_TABLES=y
CONFIG_NFT_NAT=y
CONFIG_NFT_MASQ=y
CONFIG_NF_TABLES_INET=y
CONFIG_BRIDGE=y
CONFIG_VETH=y
CONFIG_HID=y
CONFIG_HID_GENERIC=y
CONFIG_USB_HID=y
CONFIG_INPUT_UINPUT=y
CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED=y
CONFIG_SECURITY_APPARMOR=y            # optional: Debian/Ubuntu/Kali; boot with apparmor=0 if it misbehaves
```

The 355 options that stay =m go into a shared modules image, built without root:

```sh
make -C /mnt/build/src/mainline O=... modules_install INSTALL_MOD_PATH=/mnt/build/out/mods INSTALL_MOD_STRIP=1
mke2fs -t ext4 -L sb-modules -d /mnt/build/out/mods/lib/modules/7.1.0-starbite \
  /mnt/build/out/modules-7.1.0-starbite.img 256M
```

**Bootargs** move from CONFIG_CMDLINE into `mk-starlte-dts.py`'s `/chosen/bootargs`. The loader appends the entry cmdline after them (xnu_boot.c:683-690; the catalog field holds 512 bytes).
- Daily bootargs: `console=ramoops-1 console=tty0 loglevel=4 panic=10 maxcpus=8 s9p_vendor_m3_defaults s3c2410_wdt.tmr_atboot=0`
- Drop `earlycon=s9pram` and `s9p_late_ramlog`: neither is implemented in this tree.
- Bring-up builds add `loglevel=7 keep_bootcon` through a hidden catalog entry.

**CMA and initrd overlap (must fix in the same build).**
- The DT CMA covers 0x98000000-0xAC000000, while the loader puts the initrd at OS_RD 0xA8000000. The initrd's room runs to EDIT_FDT 0xAD000000, so **80 MiB at most** (xnu_boot.c:448-450). Once CMA hands out pages, the initrd can be overwritten.
- The fix is in mk-starlte-dts.py: move the CMA `reg` to 0xB0000000, size 0x14000000 (320 MiB, ending at 0xC4000000, below the simple-framebuffer at 0xCC000000).
- Before committing, check it against every reserved-memory node in the decompiled DTB (`dtc -I dtb -O dts build/linux/exynos9810-starlte.dtb | grep -A3 reserved-memory`) and against `/proc/iomem` from a boot.
- This also lifts the ~95 MB cap on Image plus BSS.

**Outputs** go to `build/linux/` (git-ignored):
- `Image` (text_offset 0x80000 fixup kept, RANDOMIZE_BASE off, 48-bit VA/PA kept)
- `exynos9810-starlte.dtb`, `kernel.config`
- `modules-7.1.0-starbite.img`
- `SHA256SUMS`
- `linux.initrd` (built by `tools/linux/mk-initrd.sh`, 2.4)

### 2.4 The initrd (`tools/linux/mk-initrd.sh`, in the VM, native arm64)

- Build it from the Alpine minirootfs with `apk.static --root` in the VM, adding busybox-static, e2fsprogs, exfatprogs, util-linux-misc (`losetup --direct-io`), dosfstools and evtest.
- Strip it and pack it as `cpio | zstd -19`. The budget is under 20 MiB; the hard limit is 80 MiB.
- `/init` is the rewritten `tools/linux/init` (3.2). Changing the init then only means repacking the store, with no kernel rebuild.

### 2.5 Pack and flash (the only TWRP step)

```sh
python3 tools/store/sbstore2.py build build/store/ufs-store.img --catalog tools/store/catalog.toml \
  linux.image=build/linux/Image linux.dtb=build/linux/exynos9810-starlte.dtb \
  linux.initrd=build/linux/linux.initrd  [existing ios/android/windows files…]
python3 tools/store/sbstore2.py list build/store/ufs-store.img
tools/store/store-flash.sh build/store/ufs-store.img        # phone in TWRP (user presses the buttons)
```

Check first that `sbstore2.py build` gives a `linux.initrd` name the ST_INITRD type (store.h:29). If not, add the mapping.

---

## 3. Distro architecture

### 3.1 The pieces

- **UFS store:** `linux.image`, `linux.dtb` and `linux.initrd` are shared by every Linux entry. An optional `linux-b.image` serves A/B kernel tests.
- **Card p1 exFAT "STARBITE":**
  ```
  /distros/_kernel/modules-7.1.0-starbite.img   ext4, mounted ro, bound on /lib/modules/7.1.0-starbite
  /distros/_tools.img                           Alpine tools root (rescue, installer, GPU benchmarks)
  /distros/<name>.img                           one ext4 root per distro, label sb-<name>, made in one go (contiguous)
  /distros/manifest.txt                         name  size  source-url  sha256  build-date  kernel
  /distros/runs/                                check output written by sb check (copied to the Mac)
  /shared/                                      every distro sees it at /media/starbite-sd/shared
  ```
  The existing `backup/`, `windows/` and planned iOS root stay untouched. Check `df` first: the budget is about 40-55 GB.
- **Card p2:** the 32 GiB raw loader store stays unchanged.
- **Kernel parameters parsed by the init** from `/proc/cmdline`, which after 2.3 is the DTB bootargs plus the entry cmdline:

  | Parameter | Meaning |
  |---|---|
  | `sb.root=` | `/distros/x.img`, `none` (stay in the shell) or `ask` (show the selector) |
  | `sb.init=` | Override the init path |
  | `sb.rw=0` | Mount the root read-only |
  | `sb.shell=1` | Same as holding Vol down |
  | `sb.wdt=<s>` | Watchdog timeout (at least 30 s), default 0 = off until 4.3 step 4 is verified. The only switch: the distro arms nothing itself |

### 3.2 The generic `/init` (replaces the hard-coded `ROOTIMG=starbite-linux.img` at tools/linux/init:106)

1. Mount proc, sys and devtmpfs. Set the clock from `/proc/device-tree/chosen/starbite,rtc-epoch`. Run `blockdev --setro` on every `/dev/sd*` (kept from today).
2. Bring up the configfs gadget: ECM 02:53:39:00:00:01/02 plus ACM, usb0 172.16.42.1/24, udhcpd and telnetd. These are identical for every distro.
3. If Vol down is held (GPA0 DAT 0x14050024 bit 4), or `sb.shell=1`, or `sb.root=none`, start the shell (step 8).
4. If the exFAT dirty flag is set, run `fsck.exfat -p /dev/mmcblk0p1`. Then mount it with `uid=10000,gid=10000,noatime` on /sd.
5. Check that `/sd$SB_ROOT` exists, then `e2fsck -p` it. Exit code 4 or higher drops to the selector. Then `losetup --direct-io=on -f --show` and mount it as ext4 rw,noatime on /newroot.
6. Loop-mount `/sd/distros/_kernel/modules-$(uname -r).img` read-only and bind it on `/newroot/lib/modules/$(uname -r)` (using `/newroot/usr/lib/modules` when `/lib` is a symlink). Move /sd to `/newroot/var/lib/starbite/card` under a root-owned 0700 `/var/lib/starbite`, and bind only its `shared/` on `/newroot/media/starbite-sd/shared`: the desktop user (uid 10000, the exFAT's owner) must not reach `backup/`, `windows/`, the iOS image or the running root image.
7. Pick the init: `sb.init`, then `/sbin/init`, `/usr/lib/systemd/systemd`, `/lib/systemd/systemd`. Arm `/dev/watchdog` with `sb.wdt` through the watchdog ioctl helper (do not close the device, or it stops), and only then write the systemd drop-in that makes systemd feed it (removed again when `sb.wdt=0`). Run `exec switch_root /newroot $init`.
8. **Selector and shell** (also the landing spot after any failure):
   - Show the list of `/distros/*.img` on tty0, chosen with Vol up/down (`/dev/input/event*` from gpio-keys).
   - Read the FIFO `/run/sb-boot`. Writing a name to it does the switch_root with no reboot.
   - Menu items: "Starbite shell" (chroot into `_tools.img`), "Card to Mac" (unmount the exFAT, then the configfs `mass_storage.0` lun.0 file = /dev/mmcblk0p1), "Reboot to menu".
   - Install `/sbin/reboot-to` (the mailbox, 4.1).

### 3.3 Catalog entries (`tools/store/catalog.toml`; replaces the current `linux` entry)

```toml
[[entry]]
key = "linux-shell"
name = "Starbite shell"
version = "Linux 7.1 · rescue · installer"
category = "linux"
method = "linux"
tag = "stable"
files = ["linux.image", "linux.dtb", "linux.initrd"]
cmdline = "sb.root=none"
root = "initramfs + sd:p1 /distros/_tools.img"

[[entry]]
key = "linux-pmos"
name = "postmarketOS"
version = "edge · Phosh · Linux 7.1"
category = "linux"
method = "linux"
tag = "beta"
files = ["linux.image", "linux.dtb", "linux.initrd"]
cmdline = "sb.root=/distros/pmos-phosh.img"
root = "sd:p1 /distros/pmos-phosh.img"

[[entry]]
key = "linux-debian"
name = "Debian"
version = "forky · Phosh · Linux 7.1"
category = "linux"
method = "linux"
tag = "experimental"
files = ["linux.image", "linux.dtb", "linux.initrd"]
cmdline = "sb.root=/distros/debian-phosh.img"
root = "sd:p1 /distros/debian-phosh.img"

[[entry]]
key = "linux-bringup"
name = "Linux bring-up"
version = "loglevel 7 · selector"
category = "linux"
method = "linux"
tag = "lab"
files = ["linux.image", "linux.dtb", "linux.initrd"]
cmdline = "loglevel=7 keep_bootcon sb.root=ask"
root = "sd:p1 /distros"
flags = ["hidden"]
```

Later entries follow the same pattern (keys under 24 chars, names under 32, versions under 48; CAT_MAX is 32):
- `linux-sxmo` → alpine-sxmo.img
- `linux-arch` → arch.img
- `linux-plasma` → pmos-plasma.img
- `linux-kali` → kali.img
- `linux-fedora` → fedora.img
- `linux-ubuntu` → ubuntu.img
- hidden `linux-kernel-b` (`linux-b.image`, `sb.root=ask`)

Icons are separate per-entry sprites in menu6, and must follow each project's trademark guidelines.

### 3.4 The shared integration script (`tools/linux/distros/sb-integrate.sh <name> <openrc|systemd>`, run inside the chroot)

It generalises pmos-setup.sh.

**Users and access:**
- hostname `starbite-<name>`
- user `user` uid/gid 10000, in video, input, render, audio, netdev/plugdev and wheel
- root locked; PIN from `build/linux/<name>-login.txt` (git-ignored)
- doas or sudo for wheel
- `build/linux/ssh/starbite_ed25519.pub` in authorized_keys, sshd enabled

**USB networking and system rules:**
- NetworkManager `[keyfile] unmanaged-devices=interface-name:usb0`. On systemd distros, `/etc/systemd/network/10-usb0.network` with Address=172.16.42.1/24, DHCPServer=yes and no DHCP client on usb0.
- Mask anything that resets the gadget: pmOS `usb-signaller`, ModemManager, systemd usb-gadget units.
- polkit netdev → NetworkManager; udev `UDISKS_IGNORE` for `sd*`.
- The GPU devfreq floor rule (1.3).
- `/etc/environment` with `PAN_MAX_AFBC_PACKING_RATIO=0`.
- `/etc/fstab` with no root line.

**Phone-specific services:**
- reboot-to (4.1).
- Watchdog: off by default (systemd `RuntimeWatchdogSec=off`). With `sb.wdt=N` the initrd writes `60-starbite-watchdog-on.conf` (`RuntimeWatchdogSec=N`); OpenRC starts busybox `watchdog -t 10 -T N` only when `/proc/cmdline` has `sb.wdt=N`, N > 0.
- No fstrim (`fstrim.timer` masked): with no fstab entries it would trim the card's exFAT too.
- reboot-to under systemd: the key goes to `/run/starbite/next`, and a `system-shutdown` hook writes the mailbox just before the reboot (it is the last 32 bytes of the ramoops console ring).
- Disable hwclock and RTC services, bluetooth, fwupd, ModemManager and systemd-firstboot. Seed machine-id and locale.
- The port's charger 500 mA rule (s9p-touch-power) and M3 floor 1066 MHz.
- Integer scale 3.

**Never install a distro kernel or bootloader:**
- Debian/Ubuntu/Kali: no `linux-image-*` or `flash-kernel`, and `apt-mark hold` those.
- Arch: `pacman -Rdd linux-aarch64` plus `IgnorePkg = linux-aarch64 linux-firmware`.
- Fedora: `exclude=kernel* grub2* shim*`.

### 3.5 The first three distros, in order

The order and its reasons:
1. **Starbite shell** is the landing spot and the installer.
2. **pmOS Phosh** is the proven apk path with a lighter compositor than Plasma, and it is the GPU-gate distro.
3. **Debian Phosh** proves the systemd integration that Arch, Kali, Fedora and Ubuntu all reuse, with the same shell for an apples-to-apples fps comparison.

Alpine+Sxmo is cheap and comes 4th as a wlroots/sway GPU A/B test.

On every phone step below:
- The Mac runs `tools/linux/http-proxy.py --bind 172.16.42.2 --port 3128` (CONNECT works for HTTPS).
- It also serves `build/linux` over HTTP on 172.16.42.2:8643, as for phone-prep.sh.
- The phone exports `http_proxy=https_proxy=http://172.16.42.2:3128`.
- Hashes are fetched by the Mac over HTTPS and passed as arguments.

#### Distro 1: Starbite shell and `_tools.img`

```sh
# Mac/VM: kernel + initrd + modules (section 2), store flashed via TWRP once
sbstore2.py settings build/store/settings.img --default linux-shell --timeout 5   # safe default during Phase 3 (then flash with the store)
# phone, booted into linux-shell (telnet 172.16.42.1 via tools/linux/phone.py):
V=3.22.2   # current Alpine at build time; SHA from the Mac: curl -s https://dl-cdn.alpinelinux.org/alpine/v3.22/releases/aarch64/alpine-minirootfs-$V-aarch64.tar.gz.sha256
mkdir -p /sd/distros/_kernel && cd /sd/distros
wget http://172.16.42.2:8643/modules-7.1.0-starbite.img -O _kernel/modules-7.1.0-starbite.img
truncate -s 2G _tools.img && mke2fs -t ext4 -L sb-tools -q _tools.img
mkdir -p /t && mount -o loop _tools.img /t
wget -O /tmp/mr.tgz https://dl-cdn.alpinelinux.org/alpine/v3.22/releases/aarch64/alpine-minirootfs-$V-aarch64.tar.gz
echo "$SHA  /tmp/mr.tgz" | sha256sum -c && tar -xzpf /tmp/mr.tgz -C /t
cp /etc/resolv.conf /t/etc/; mount --bind /dev /t/dev; mount -t proc p /t/proc; mount -t sysfs s /t/sys
chroot /t /bin/sh -c 'apk add --no-cache e2fsprogs exfatprogs util-linux tar xz zstd gnupg curl \
  ca-certificates wget mesa-dri-gallium mesa-egl kmscube glmark2 libdrm-tests evtest debootstrap \
  dosfstools openssh-client'
umount /t/sys /t/proc /t/dev /t; e2fsck -fp _tools.img
```

Done when:
- `kmscube` and `glmark2-es2-drm` run on card2. Their numbers are the GPU baseline (1.2 steps 6-7).
- Card-to-Mac mass storage mounts on the Mac.

#### Distro 2: postmarketOS edge + Phosh (`/distros/pmos-phosh.img`, 6 GiB)

This repeats the milestone 3 method (NOTES.md:1142-1162), now from `_tools.img`. The new script is `tools/linux/distros/mkroot-pmos.sh <ui>`, run on the phone.

```sh
# Mac first: put pmbootstrap's pmb/data/keys/*.pub into build/linux/keys/ (as before); get apk-tools-static sha256 over HTTPS
sh /sd/distros/phone-prep.sh "$(date +%s)" "$APK_SHA"        # gives /tmp/apk/sbin/apk.static + keys
cd /sd/distros && truncate -s 6G pmos-phosh.img && mke2fs -t ext4 -L sb-pmos-phosh -q pmos-phosh.img
mkdir -p /r && mount -o loop pmos-phosh.img /r && mkdir -p /r/etc/apk/keys && cp /tmp/keys/*.pub /r/etc/apk/keys/
A=http://dl-cdn.alpinelinux.org/alpine/edge P=https://mirror.postmarketos.org/postmarketos/master
/tmp/apk/sbin/apk.static --root /r --initdb -X $P -X $A/main -X $A/community -X $A/testing \
  --no-interactive add postmarketos-base postmarketos-ui-phosh openssh-server doas mesa-dri-gallium
printf '%s\n' $P $A/main $A/community $A/testing > /r/etc/apk/repositories
mount --bind /dev /r/dev; mount -t proc p /r/proc; mount -t sysfs s /r/sys; cp /etc/resolv.conf /r/etc/
cp /sd/distros/sb-integrate.sh /r/tmp/ && chroot /r sh /tmp/sb-integrate.sh pmos-phosh openrc
# pmOS specifics inside: deviceinfo from pmos-setup.sh; rc-update del usb-signaller default;
# tinydm autologin uid 10000; phoc.ini [output:DSI-1] scale=3 (name from wlr-randr on first boot)
umount -R /r && e2fsck -fp pmos-phosh.img
echo "pmos-phosh 6G $P sha:n/a $(date -I) 7.1.0-starbite" >> manifest.txt
```

Boot it with `reboot-to linux-pmos`. Then run the GPU triage in 1.2, apply 1.3, and measure again.

Gate: at least 45 fps in Phosh before the Plasma rebuild (`linux-plasma`, same script with `postmarketos-ui-plasma-mobile`, 8 GiB).

#### Distro 3: Debian forky + Phosh (`/distros/debian-phosh.img`, 6 GiB)

```sh
# VM (small output, ~150 MB; signatures checked by mmdebstrap with debian-archive-keyring):
mmdebstrap --arch=arm64 --variant=minbase --include=ca-certificates,systemd-sysv,dbus,udev,locales \
  forky ~/distros/debian-forky-minbase.tar http://deb.debian.org/debian
sha256sum ~/distros/debian-forky-minbase.tar > ~/distros/debian-forky-minbase.tar.sha256
# copy both to build/linux/ (served on :8643), delete from the VM afterwards
# phone (tools root):
cd /sd/distros && truncate -s 6G debian-phosh.img && mke2fs -t ext4 -L sb-debian-phosh -q debian-phosh.img
mount -o loop debian-phosh.img /r
wget -O /tmp/deb.tar http://172.16.42.2:8643/debian-forky-minbase.tar && echo "$SHA  /tmp/deb.tar" | sha256sum -c
tar -xpf /tmp/deb.tar -C /r && rm /tmp/deb.tar
printf 'Acquire::http::Proxy "http://172.16.42.2:3128";\nAcquire::https::Proxy "http://172.16.42.2:3128";\n' > /r/etc/apt/apt.conf.d/90sb-proxy
mount --bind /dev /r/dev; mount -t proc p /r/proc; mount -t sysfs s /r/sys; cp /etc/resolv.conf /r/etc/
chroot /r sh -c 'apt-get update && apt-get install -y --no-install-recommends phosh-phone mesa-vulkan-drivers \
  libgl1-mesa-dri network-manager openssh-server sudo systemd-resolved && \
  apt-mark hold linux-image-arm64 flash-kernel 2>/dev/null; true'
cp /sd/distros/sb-integrate.sh /r/tmp/ && chroot /r sh /tmp/sb-integrate.sh debian-phosh systemd
# systemd specifics inside: phosh.service drop-in User=user; 10-usb0.network; watchdog off unless sb.wdt;
# mask ModemManager bluetooth fwupd systemd-timesyncd? (keep: time is set by the initrd, NTP fine later)
rm /r/etc/apt/apt.conf.d/90sb-proxy   # re-added by sb-proxy-on when needed
umount -R /r && e2fsck -fp debian-phosh.img
```

Done when:
- `systemctl is-system-running` reports `running` (or `degraded` with only known units failing).
- The watchdog is fed (`wdctl`).
- usb0 works without the initrd's udhcpd.
- The Phosh fps matches pmOS within 10%.

**After these three:**
- Alpine+Sxmo (2-3 GiB, the apk script with `postmarketos-ui-sxmo-de-sway`).
- Arch Linux ARM: the official tarball, `pacman-key --init && pacman-key --populate archlinuxarm`, remove linux-aarch64.
- Ubuntu: fold the untracked `tools/linux/distros/mkroot-ubuntu.sh` into the phone-built pattern. Today it builds a 10 GiB root inside the VM, see risk 1.
- Kali and Fedora (`SELINUX=disabled`, ext4, `exclude=kernel*`).

---

## 4. Test loop (no buttons after the single TWRP flash)

### 4.1 Reboot to any entry

`/usr/local/sbin/reboot-to KEY` is installed in the initrd and in every root, using busybox devmem. The mailbox format is from menu.c:1259-1330.

```sh
devmem 0xFED17FE0 32 0x454e4253; devmem 0xFED17FE4 32 0x21325458      # "SBNEXT2!"
# key: 24 bytes NUL padded, little-endian u32 words at 0xFED17FE8..0xFED17FFC
devmem 0x1406080c 32 0x12345670; sync; reboot -f
```

`reboot-linux` is kept as an alias that writes the old "SBNEXTLX".

### 4.2 Mac driver (`tools/linux/sb.py`, built on phone.py and pmos-root.sh)

| Command | What it does |
|---|---|
| `sb boot <key>` | Runs ssh or telnet `reboot-to <key>`, then waits for ping and ssh on 172.16.42.1 (timeout 180 s, after which it reports a watchdog fallback) |
| `sb switch <name>` | `echo <name> > /run/sb-boot` in the shell, for a switch_root with no reboot (about 10 s). One switch per boot |
| `sb check` | Collects `/etc/os-release`, `uname -a`, `systemctl is-system-running` or `rc-status`, `dmesg --level=err,warn`, the section 1.2 GPU block, the glmark2 score and a screenshot (`grim` in the session) into `build/linux/runs/<key>-<date>/` |
| `sb matrix` | Runs `sb boot` and `sb check` for every `linux-*` catalog key and writes a summary table |
| `sb card` | `reboot-to linux-shell`, then switches on card-to-Mac mass storage, for copying big images |

### 4.3 Recovery when a distro hangs

1. The initrd arms the S3C2410 watchdog before switch_root, and the distro feeds it (3.4).
2. On a hang, the watchdog reset leads to the menu. With the settings default `linux-shell` and a 5 s timeout, the shell comes back and telnet returns.
3. `panic=10` covers kernel panics.
4. Check once, early, that a watchdog reset reaches S-Boot and then the loader rather than upload mode. If it lands in upload mode, set `sb.wdt=0` everywhere and rely on the user's buttons.

### 4.4 Iteration costs

| What changes | Path to the phone | Cost |
|---|---|---|
| A root image | Phone-side rebuild, or a copy over mass storage | Seconds to minutes |
| Modules | Copy `modules-*.img` to the card | Seconds |
| The init | Repack `linux.initrd`, then store-flash through TWRP. Later: move `linux.initrd` to the card store (`files = [..., "sd:linux.initrd|linux.initrd"]`) once the loader's microSD reader is out of Engineer gating, so the initrd can be updated from Linux | Minutes today |
| The kernel | Store-flash through TWRP | Minutes plus button presses, so batch these |

Always tell the user what changed and what to expect before and after each boot (memory rule).

---

## 5. Risks

1. **Mac disk.** 4 GiB free, and the VM disk never shrinks. The VM produces only the kernel, the modules image, the initrd and minbase tarballs, which are deleted after copying. The untracked `tools/linux/distros/mkroot-ubuntu.sh` builds a 10 GiB root inside the VM; rework it to the phone pattern before running it.
2. **CMA move.** A wrong `reg` can overlap a vendor carve-out and hang at boot. Diff against the full reserved-memory list, and keep the old DTB in the store as `linux-old.dtb` with a hidden entry for rollback.
3. **CMDLINE_FROM_BOOTLOADER.** If the loader's bootargs edit fails, the kernel boots with no `maxcpus=8 s9p_vendor_m3_defaults` and runs on 4 cores. The loader already refuses a DTB with no bootargs; add an init check that warns when `/proc/cmdline` lacks `s9p_vendor_m3_defaults`.
4. **The GPU fix may not reach 60 fps.** The memory bus (MIF) cannot be raised (it froze the phone). The 1440x2960 linear buffers may cap things below 60 even after cause C and cause 1 are fixed. The mitigations are an integer scale, Phosh/Sxmo as the daily shells, and in the long term AFBC scanout, which needs DECON format support.
5. **Undervolt and faults.** If faults persist at 950 mV, the G72 r0p1 erratum path (`PAN_MESA_DEBUG=sync`) costs throughput. Keep it per-app, never global.
6. **exFAT and loop on hard resets.** Watchdog and panic resets will dirty the exFAT. The init runs `fsck.exfat -p` and `e2fsck -p`; keep `backup/` untouched and never write to it from Linux (mount `/shared` only). TWRP's 4.9 kernel mounts the images only `ro,noload`, so repairs must run from the Starbite shell.
7. **Distro packages fighting the setup.** Kernel and bootloader packages, initramfs generators, usb-gadget and ModemManager units, NetworkManager taking usb0, and SELinux/AppArmor expecting kernel support. All are handled in sb-integrate.sh (3.4); `sb check` flags failed units.
8. **Kernel features still missing.** No audio (SND off), no BT, no suspend, no Type-C or USB host, no regulator or RTC driver. Kali's Wi-Fi tools: no monitor mode on brcmfmac BCM4361, and USB Wi-Fi needs USB host (Phase 4). Set expectations in each entry's `version` string.
9. **Card space.** About 55-60 GiB for all nine distros. Start with the first three plus Sxmo (about 16 GiB) and check `df` before each new image.
10. **Watchdog reset path not yet verified** (4.3). Until it is, a hung distro needs the user's button combination.
11. **Trademarks.** Distro logos in menu sprites follow each project's guidelines. postmarketOS, Debian, Arch and Fedora all publish rules.
12. **Port drift.** The port is pinned at bcee601 and mainline at v7.1. Upstream port updates come in only as deliberate subtree pulls, each followed by a full `sb matrix` run.
---

## 6. Display: black bars (2026-11-05)

**Report.** Under mainline Linux (Phosh; ChromeOS runs the same kernel) there are black lines or bars at the top and bottom of the panel.

**Verdict.** The cause is **not pinned**. Nothing the kernel programs can cut rows off the picture: every register with a size in it is either sboot's (inherited, never written) or matches 1440x2960 in the probe log. What the code and the vendor sources leave is one certain 2-row gap at the bottom, which comes from userspace and is too thin to look like a bar. They also leave one panel-side mechanism that has exactly the reported shape but has not been confirmed. The kernel built for this (`7.1.0-starbite-9f3d455b`) adds diagnostics and a switch, so one boot settles it (checklist below).

### 6.1 What is ruled out (with the numbers)

| Stage | Evidence | Result |
|---|---|---|
| DRM mode | `s9p_decon_get_modes` → `drm_cvt_mode(1440, 2960, 60, false, false, false)`. With `margins=false`, `vdisplay = vdisplay + 2*0` (drm_modes.c), so the mode is 1440x2960 | full height |
| Plane / fb | Simple-pipe primary plane: it must cover the CRTC, and `s9p_decon_pipe_check` (6c) refuses `src_x/src_y != 0` and pitch < 1440 px. Phosh's buffers have a 5760-byte pitch, so `IDMA_SRC_SIZE` is never rewritten for them | full height, no crop |
| DECON window | Probe (build/test-boots/chromeos-12.log): `window 5 ... start 00000000 end 0b8f059f`, so (0,0)..(1439,2959). The probe refuses any other window | full panel |
| Fetch channel | `src_size=0b9005a0 img_size=0b9005a0` (2960 rows x 1440 px), `base=cc000000` in both banks | full height |
| simple-framebuffer | `reg = 1440*2960*4`, `height = 2960`. It is used only until s9p-decon evicts simpledrm | n/a after takeover |
| Compositor content | Phosh screenshots (build/linux/runs/{pmos,arch,kali}-20261103/*.png) have content from row 0 down to the home bar; there is no black band in the picture itself | bars are not in the image |
| S9 vs S9+ | Vendor DT (ref/starlte-kernel, exynos9810-display-lcd.dtsi): both phones are S6E3HA8, 1440x2960, v-porch 15/3/1, DSC 2 slices x 720, slice height 40. Only the size differs: S9 `64 132` mm, S9+ `69 142`. The driver reports 68x140, which only affects DPI-based auto-scale (Phosh and ChromeOS force scale 3) | not a geometry difference |

### 6.2 Hypotheses, ranked

1. **The panel's "self mask", rows 0-149 and 2810-2959. This matches the shape; it is unverified.** The S6E3HA8 multiplies two row bands by a mask image held in its side RAM; that is how it rounds the corners. The vendor enable sequence (panel/s6e3ha8/s6e3ha8_aod_panel.h, `S6E3HA8_AOD_SELF_MASK_ENA`) is `7A 01 0000 0095 0AFA 0B8F`, which gives rows 0x000-0x095 (0-149) and 0xAFA-0xB8F (2810-2959). The factory variant moves both bands below row 2959 (`0B90..0CBB`), which supports reading these values as row ranges. Android re-uploads the image and re-enables the mask after every panel init (`aod_init_panel`, called after `PANEL_INIT_SEQ` in panel_drv.c), and the vendor DT sets `self_mask_en = true`. If the mask is on with no valid image (or the image is lost), the result is two black bars of 150 rows each, about 6.7 mm at 570 ppi, at the top and bottom. Under S-Boot that would be invisible, because its logo sits on black at (180,1144). Our driver does a DCS sleep-in/out (`panel_sleep=1`) on every blank and on the compositor's first modeset, and the vendor never runs that sleep cycle without re-sending the mask. What is unknown: whether S-Boot enables the mask at all, and whether sleep-in clears the side RAM.
2. **2 rows at the bottom: Phosh and ChromeOS at scale 3 (certain, userspace).** 2960 / 3 = 986.67. wlroots truncates the logical height to 986, so the layout covers 986 x 3 = 2958 rows; the screenshots are 1440x**2958**. Rows 2958-2959 are never drawn: they stay black in the zeroed CMA buffer. At 570 ppi that is a line of about 0.09 mm, so it is not a "bar". Exact scales for this panel: 2 or 4 (5 also divides 2960, but 1440/5 = 288 logical px is too narrow), or fractional 2.5 (576x1184). This is not a kernel bug; scale 3 was chosen in 1.3 item 8.
3. **The DSIM/DSC frame is shorter than 2960 rows (bottom only).** `DSIM_NUM_OF_TRANSFER` (vendor: `lcd_info->yres`), `DSIM_RESOL` and the DSC PPS picture height are all sboot's and were never logged. A short value would leave the panel's bottom rows unrefreshed. That fits a bottom bar but not a top one.
4. **Alpha (content-dependent, unlikely).** `IDMA_IN_CON = 0x02000000` means format 0, which is **ARGB8888** in dpu_9810 regs-dpp.h, not XRGB (the comment at 6d says otherwise). If S-Boot's window blend were coverage (`WIN_CONTROL_1` FG=AF, BG=1-AF) on its black background, XRGB pixels with X=0 would come out black. The vendor's default is `DECON_BLENDING_NONE` (ONE/ZERO, alpha ignored), and the effect would follow the content, not two fixed bands.

### 6.3 What the build adds (patch-mainline.py step 6e, all in s9p-decon)

- `geom_log` (default 1, 0644): at probe and after every enable (unblank or modeset), `geom[...]` lines with the DECON blender, splitter and OUTFIFO sizes, the data path and background color, every enabled window's control/start/end/colormap, the fetch channel in both banks (`in_con out_con src_size src_off img_size blk_* base`), and DSIM (`transfers resol vporch hporch config cprs slice cmd te`). Commits log one `geom: fb ...` line whenever the framebuffer or plane geometry changes (size, pitch, offset, fourcc, modifier, src/crtc rects). These are reads only, from blocks the driver already reads or writes.
- `dump_geometry` (0200): `echo 1 > /sys/module/s9p_decon/parameters/dump_geometry` dumps now; `2` adds the DECON shadow (active) bank and the DSC encoders' PPS. Each encoder is read only if `DATA_PATH_CONTROL_2` says it is in the path.
- `panel_mask_off` (default 0, 0644): sends the vendor's own `SELF_MASK_DIS` sequence (`F0 5A 5A`, `7A 00`, `F0 A5 A5`) after every enable, or at once when written at run time. It needs DCS long writes, so the driver gained `s9p_decon_dsi_write` (the vendor `dsim_long_data_wr` pattern: payload words, then a `0x39` header).

Nothing changes on screen unless `panel_mask_off` is set.

Build: `limactl shell starbite -- env PROJ=<worktree> WORK=~/linux/blackbars-work OUT=~/distros/_kernel-blackbars MIN_HOST_FREE_GB=2 tools/linux/build-kernel.sh`, release `7.1.0-starbite-9f3d455b`. Image sha256 `7936828c70d388bf483ab4edf4739158b342483dc89e7becfaa51a5b39341301`, with modules-7.1.0-starbite-9f3d455b.img in the same OUT. Not flashed.

### 6.4 Phone checklist (one boot of this kernel)

1. **Boot console, before any session.** Does the first fbcon text line touch the top edge, and does the text scroll down to the bottom edge? If yes, the panel shows all 2960 rows while there has been no sleep cycle. If the top ~7 mm is blank already, the cause is on the panel side from S-Boot (go to 4).
2. **Measure the bars in the session** on a bright wallpaper (pmOS green; on Arch/Kali's black lockscreen nothing shows):
   - About 2 px at the bottom only: hypothesis 2 (scale 3). Optionally try `[output:DSI-1] scale = 2` or `2.5` in /etc/phosh/phoc.ini.
   - About 150 rows (about 7 mm, symmetric) at both ends: hypothesis 1 (go to 4).
   - Bottom only and thicker than 2 px: hypothesis 3 (read `transfers`/`resol` in step 3).
   - Something else, or bands that follow the content: hypothesis 4 (`win5: ctrl=` in step 3).
3. **`dmesg | grep geom`**, plus `echo 2 > /sys/module/s9p_decon/parameters/dump_geometry; dmesg | grep geom | tail`, saved to build/linux/runs/. Expected for a full frame: dsim `transfers=00000b90` (2960) and `resol` 0x0b90 in the high half, `bg_size=0b9005a0`, `src_off=00000000`, idma `img_size=0b9005a0`, and DSC `pps04` low half 0x0b90. Any other row count is the cause.
4. **Self-mask test (live, no reboot):** `echo 1 > /sys/module/s9p_decon/parameters/panel_mask_off` (dmesg: `panel self mask off (7A 00): 0`). If the bars disappear (the corners turn square), hypothesis 1 is confirmed. The fix is then to make it the default, or to upload the vendor mask image after every sleep-out. If nothing changes, it is not the mask; set it back to 0.
5. **Sleep-cycle test:** `echo 0 > /sys/module/s9p_decon/parameters/panel_sleep`, then press power twice (blank and unblank). If the bars only appeared after unblanks before, and no longer do, the sleep cycle is losing panel state. Keep `panel_sleep=0` until it is fixed; the cost is a dimly lit panel while blanked.
6. With the session stopped: `modetest -M exynos -s <conn>@<crtc>:1440x2960` (from _tools.img). The pattern has to touch all four edges. If it does, the kernel path is clean and the bars come from the compositor.

### 6.5 First phone run of the 6e kernel (2026-11-05, 23:44, S9 #1, hidden key `linux-pmos-bb`)
Read over SSH (usb0), saved in build/linux/runs/blackbars-20261105/. Every stage carries the full 2960 rows:
DECON `bg_size=0b9005a0` (normal and shadow), win5 `0..0b8f059f` (normal and shadow), IDMA src/img `0b9005a0`,
DSIM `transfers=00000b90 resol=0b9001e0` (2960 lines of 480 compressed px), both DSC encoders
`pps04 height=0x0b90`, width 2 x 0x2d0, slice height 40. **Suspect 3 (a short frame) is ruled out**, and the
kernel's fb/plane/window path is clean. Left: suspect 1 (the panel's self mask, rows 0-149 and 2810-2959) and
suspect 2 (Phosh `scale = 3` in /etc/phosh/phoc.ini [output:DSI-1]: 986 logical rows = 2958 px, bottom 2 rows
never drawn). Both need someone looking at the panel: the morning test is `echo 1 >
/sys/module/s9p_decon/parameters/panel_mask_off` (driven over SSH while the user watches); if the bars stay,
try scale 3.2 (exact: 1440/3.2 = 450, 2960/3.2 = 925; 384/120 in wlroots' steps). Defaults now: geom_log on,
panel_mask_off N, panel_sleep Y. grim is not installed in the pmOS root (no screenshot this run).
Returning to TWRP from mainline Linux with INFORM3 = 0x12345674 + `reboot -f` did NOT reach TWRP (the phone
dropped off USB; it needs the keys). Android's `adb reboot recovery` does reach TWRP.

### 6.6 How Samsung handles the self mask, and the fix candidate (2026-11-06)

**Sources.** (a) Samsung's Android 10 kernel for the SM-G960F: LineageOS/android_kernel_samsung_exynos9810,
commit `45f6ec6f199e` "drivers: video: import from samsung G960FXXUHFVB4", files under
`drivers/video/fbdev/exynos/panel/`: `s6e3ha8/s6e3ha8_aod_panel.h`, `s6e3ha8/s6e3ha8_self_mask_img.h`,
`s6e3ha8/s6e3ha8_crown_aod_panel.h` (Note9), `aod/aod_drv.c`, `panel_drv.c`, `panel.c`, `sysfs.c`;
`arch/arm64/configs/exynos9810-starlte_defconfig` (branch lineage-20) has `CONFIG_EXTEND_LIVE_CLOCK=y`, which
compiles the AOD/self-mask path in. (b) The Android 9 tree in ref/starlte-kernel (github.com/vutung2311/starlte,
tw90-android-p): the same code; the mask image is byte-identical (Android 10 only adds a 34 ms side-RAM init delay
for the AOD clock image, not for the mask). (c) The S10 (S6E3HA9): LineageOS/android_kernel_samsung_exynos9820,
lineage-21, `panel/s6e3ha9/s6e3ha9_aod_panel.h`, `s6e3ha9_beyond1_aod_panel.h`, `aod/aod_drv.c`. In all three
trees the mask is part of the AOD driver (`panel/aod/aod_drv.c`); there is no separate self_display driver.
(d) The phone's own S-Boot, build/fw-vg6/bl/sboot.bin (G960FXXUHFVG6).

**What the mask is.** `STAR_SELF_MASK_IMG`, 3552 bytes, run-length coded, 8 bits per pixel: `01nnnnnn v` = n
pixels of value v, `10nnnnnn nnnnnnnn` = a run of 0, `11nnnnnn nnnnnnnn` = a run of 0xff, `00 00` = padding. It
decodes to exactly 300 x 1440 = 432000 pixels: 150 rows for the top band, then 150 for the bottom. 0xff (black)
sits only in the corners, 0 (pass through) everywhere else: row 0 is black for 208 px at each side with an
anti-aliased ramp and 718 px clear in the middle, row 75 is black for 8 px at each edge, row 149 is all clear.
That is the S9's **rounded corners** (wide, because the long edges curve), not bands. The Note9's
`CROWN_SELF_MASK_IMG` decodes the same way with a small corner (48 px at row 0, flat panel). Decoded with
scratch scripts; the image is kept in tools/linux/patches/s6e3ha8-star-self-mask.hex (sha256 `bc4c1d6c...1b440`).

**The DCS sequences** (`s6e3ha8_aod_panel.h`; level-2 key `F0 5A 5A` / `F0 A5 A5` around each):
- `SELF_MASK_IMG_SEQ`: `75 10` (side-RAM data path: mask), then the image as DSI_PKT_TYPE_WR_SR, which `panel.c`
  `panel_dsi_write_mem` sends as DCS long writes (0x39, `common_mipi_lcd.c` `mipi_write`): first byte `4C`
  (WRITE_SIDE_RAM_START), then `5C` (CONTINUE), payload min(remaining, 511) rounded down to 16 bytes, so 3552 bytes
  go as 7 x 496 + 80.
- `SELF_MASK_ENA_SEQ`: `7A 01 0000 0095 0AFA 0B8F 09 0F 00 00 00 00` (16 bytes): rows 0-149 and 2810-2959. The
  factory build (`CONFIG_SELFMASK_FACTORY`) puts both ranges past row 2959 (`0B90..0C25`, `0C26..0CBB`), i.e.
  no mask on screen. The S10 uses `7A 21 0000 0095 0B4A 0BDF ...` = rows 0-149 and 2890-3039 of its 3040 rows,
  which confirms the fields are two row ranges, the last 150 rows of the panel being the second.
- `SELF_MASK_DIS_SEQ`: `7A 00`.

**When Samsung sends them** (`panel_drv.c`, `aod_drv.c`): `panel_sleep_out` -> (power on if the panel was off) ->
`__panel_seq_init`: `PANEL_INIT_SEQ` (DSC + PPS, sleep-out `11`, 10 ms, set_area, TE on, ...), the disp-det
check, then `panel_aod_init_panel` -> `aod_init_panel`: `SELF_MASK_IMG_SEQ` then `SELF_MASK_ENA_SEQ`, every
time, before `PANEL_DISPLAY_ON_SEQ`. AOD entry sends `SELF_MASK_DIS_SEQ`; AOD exit sends only
`SELF_MASK_ENA_SEQ` (the image survives LPM in side RAM). Power-off sets `reset_flag` ("clock image ... on ddi
side ram was remove when ddi was turned off", panel_drv.c `panel_power_off`), so side RAM is known to be lost
with power; whether a DCS sleep-in alone loses it is not documented, and Samsung never relies on it: the image
is re-sent on every init. sysfs `self_mask` (0664) toggles at run time (0 = DIS; 1 = IMG + ENA). The S10 adds
`75 00` after the image and a CRC check, same scheme.

**S-Boot does it too.** sboot.bin VG6 holds, as data, `75 10` at 0x134960, the same 3552-byte image at
0x134968 and the 16-byte `7A 01 ...` enable at 0x135748, right before the `s6e3ha8_panel_read_id` string. So
the panel Linux inherits has the mask uploaded and on, rounded corners included. Our driver then sleep-cycles
the panel (`10`/`11`) on every blank and on the first modeset and never re-sends either; a mask that stays on
while its image is gone or corrupted gives exactly a black band of 150 rows (about 6.7 mm) at each end.

**The fix candidate (patch-mainline.py step 6f, s9p-decon).** `panel_mask` (int, 0644):
- `1` (default) = Samsung's behaviour: after every sleep-out and before display-on (inside `s9p_panel_up`),
  `F0 5A 5A`, `75 10`, the image as 4C/5C long writes of 1 + 496 bytes, `F0 A5 A5`, then `F0 5A 5A`,
  `7A 01 ...` (16 bytes), `F0 A5 A5`. dmesg: `panel self mask restored (sleep-out): 3552 B side RAM, 7A 01
  rows 0-149/2810-2959`. If any write fails it sends `7A 00` instead (`restore failed ..., turning the mask
  off`), so a half-written image never stays on.
- `2` = off (`7A 00`): square corners, nothing masked. `0` = leave the panel alone (the driver before 6f).
- Writing the parameter with the display on applies it at once (`echo 2 > /sys/module/s9p_decon/parameters/
  panel_mask`), so both variants can be compared in one boot over SSH. 6e's `panel_mask_off=1` still wins.
- `s9p_decon_dsi_write` now takes up to 512 bytes and waits for the payload FIFO to drain (DSIM_FIFOCTRL
  EMPTY_PL_SFR) before a long write. The 6e diagnostics (geom_log, dump_geometry, panel_mask_off) are unchanged.
- The image is read from tools/linux/patches/s6e3ha8-star-self-mask.hex and checked (length, sha256);
  build-kernel.sh hashes that file into the build id.

Build (2026-11-06): `limactl shell starbite -- env PROJ=<worktree> WORK=~/linux/mask-work
OUT=~/distros/_kernel-mask REPO_OUT=~/distros/_kernel-mask-repo MIN_HOST_FREE_GB=1 tools/linux/build-kernel.sh`,
release **`7.1.0-starbite-4da38c67`**, in the VM at /home/dev.guest/distros/_kernel-mask/:
`Image` sha256 `d18510ec62d07caf21b72132231035b77543850219179df52eba28ba70ff1762`,
`exynos9810-starlte.dtb` `78e418eb6ab2f8453cb30c2aca5e22959e99f8ba48b4315f2693b89bf0488c76`,
`modules-7.1.0-starbite-4da38c67.img` `fd6e9f33a3063d74b3bb05f3cc5f961a83d0d2a0e34efe6eb587983e433d73d6`
(SHA256SUMS next to them; the REPO_OUT copy and the work tree were deleted). Not flashed.

**Phone test (one boot, nothing to type).** On a bright wallpaper:
- No band at the top or bottom and all four corners softly rounded, like Android: fixed (the mask had lost its
  image across the sleep cycle). Keep the default.
- Bands still there: over SSH, `echo 2 > /sys/module/s9p_decon/parameters/panel_mask`. Bands gone with square
  corners: the mask is the cause but the restore did not take (read `dmesg | grep "self mask"`); make 2 the
  default. Bands unchanged: not the mask; look at the scale (below) and 6.4.
- Only a hairline at the very bottom: the scale-3 rows 2958-2959 (6.2 item 2).

**Phosh scale (separate option, default unchanged).** tools/linux/distros/sb-integrate.sh takes `SB_SCALE`
(default `3`, checked to be a plain number) and writes it into /etc/phosh/phoc.ini `[output:DSI-1] scale =`.
`SB_SCALE=3.2` divides both sides exactly (1440/3.2 = 450, 2960/3.2 = 925, 384/120 in wlroots' fractional
steps), so every row is drawn; `2` and `4` are the integer alternatives. Pass it through the chroot call (e.g.
`sudo env SB_SCALE=3.2 ...`); on an already integrated root the same change is
`sed -i 's/^scale = 3$/scale = 3.2/' /etc/phosh/phoc.ini` and a Phosh restart.
