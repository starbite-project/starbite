#!/usr/bin/env python3
"""Check a built kernel .config against Starbite's rules (run by build71.sh via
build-kernel.sh, right after the fragment merge; exits non-zero on any miss).

    check-config.py [--bootargs STR] .config FRAGMENT...

1. Every line of every fragment holds in .config (=y stays =y, "is not set"
   stays unset, strings match). A symbol Kconfig dropped (a missing
   dependency) is a failure, not a silent no.
2. Hard rules, independent of the fragment:
   - CONFIG_CMDLINE_FORCE must not be y (it drops the DTB's bootargs and the
     loader's entry cmdline, so sb.root=... never arrives); FROM_BOOTLOADER=y.
   - CONFIG_INITRAMFS_SOURCE must be empty (the initrd is the store's
     linux.initrd; the port's built-in one mounted sda25 = USERDATA rw).
   - CONFIG_LOCALVERSION="-starbite", LOCALVERSION_AUTO off (modules image name).
   - CONFIG_CMDLINE (the fallback) carries maxcpus=8 and s9p_vendor_m3_defaults
     (otherwise only 4 cores), and equals --bootargs when given.
   - MUST_Y below is built in (=y, a module is not enough): the USB gadget,
     PHY, UFS, ext4, loop, microSD and FAT/exFAT. Without them the initrd cannot
     bring up usb0 or reach the card that holds the modules.
"""
import argparse
import re
import sys

MUST_Y = [
    # USB network/serial gadget: the only way back into the phone
    'USB_SUPPORT', 'USB_GADGET', 'USB_DWC3', 'USB_LIBCOMPOSITE', 'USB_CONFIGFS',
    'USB_CONFIGFS_ACM', 'USB_CONFIGFS_NCM', 'USB_CONFIGFS_ECM', 'USB_CONFIGFS_RNDIS',
    'USB_CONFIGFS_MASS_STORAGE', 'USB_F_ACM', 'USB_F_NCM', 'USB_F_ECM', 'USB_F_RNDIS',
    'USB_F_MASS_STORAGE', 'USB_U_ETHER', 'GENERIC_PHY', 'PHY_EXYNOS5_USBDRD',
    # UFS, ext4, loop
    'SCSI', 'BLK_DEV_SD', 'SCSI_UFSHCD', 'SCSI_UFSHCD_PLATFORM', 'SCSI_UFS_EXYNOS9810',
    'EXT4_FS', 'BLK_DEV_LOOP',
    # microSD and its file systems
    'MMC', 'MMC_BLOCK', 'MMC_DW', 'MMC_DW_PLTFM', 'MMC_DW_EXYNOS',
    'REGULATOR_FIXED_VOLTAGE', 'VFAT_FS', 'EXFAT_FS', 'NLS_CODEPAGE_437', 'NLS_ISO8859_1',
    # the initrd itself
    'BLK_DEV_INITRD', 'RD_ZSTD', 'RD_GZIP', 'DEVTMPFS', 'TMPFS',
    # the port's platform: CPUs, PMIC mailbox, display, watchdog, ramoops
    'ARCH_EXYNOS', 'EXYNOS_PMU', 'S9P_ACPM', 'ARM_S9P_CPUFREQ', 'DRM_S9P_DECON',
    'S3C2410_WATCHDOG', 'PSTORE_RAM', 'CMDLINE_FROM_BOOTLOADER',
]
# one of each group must be =y
ANY_Y = [('USB_DWC3_GADGET', 'USB_DWC3_DUAL_ROLE')]
MUST_ARGS = ('maxcpus=8', 's9p_vendor_m3_defaults')

LINE = re.compile(r'^CONFIG_([A-Za-z0-9_]+)=(.*)$')
UNSET = re.compile(r'^# CONFIG_([A-Za-z0-9_]+) is not set$')


def parse(path, strict=False):
    vals = {}
    for n, raw in enumerate(open(path, encoding='utf-8'), 1):
        line = raw.strip()
        m = LINE.match(line)
        if m:
            vals[m.group(1)] = m.group(2)
            continue
        m = UNSET.match(line)
        if m:
            vals[m.group(1)] = 'n'
            continue
        if strict and line and not line.startswith('#'):
            sys.exit(f'{path}:{n}: not a Kconfig line: {line!r}')
    return vals


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bootargs', help='CONFIG_CMDLINE must equal this (mk-starlte-dts.py --print-bootargs)')
    ap.add_argument('config')
    ap.add_argument('fragments', nargs='*')
    a = ap.parse_args()
    cfg = parse(a.config)
    get = lambda k: cfg.get(k, 'n')  # noqa: E731  absent = not set
    bad = []

    for frag in a.fragments:
        for k, want in parse(frag, strict=True).items():
            have = get(k)
            if have != want:
                bad.append(f'{frag}: CONFIG_{k} wanted {want}, .config has {have}')

    for k in MUST_Y:
        if get(k) != 'y':
            bad.append(f'CONFIG_{k} must be built in (=y), is {get(k)}')
    for group in ANY_Y:
        if not any(get(k) == 'y' for k in group):
            bad.append(f'one of {", ".join("CONFIG_" + k for k in group)} must be y')
    if get('CMDLINE_FORCE') == 'y':
        bad.append('CONFIG_CMDLINE_FORCE=y: the DTB bootargs and the entry cmdline would be ignored')
    if get('INITRAMFS_SOURCE') not in ('""', 'n'):
        bad.append(f'CONFIG_INITRAMFS_SOURCE={get("INITRAMFS_SOURCE")}: must be empty (the initrd is linux.initrd)')
    if get('LOCALVERSION') != '"-starbite"':
        bad.append(f'CONFIG_LOCALVERSION={get("LOCALVERSION")}, want "-starbite"')
    if get('LOCALVERSION_AUTO') == 'y':
        bad.append('CONFIG_LOCALVERSION_AUTO=y: uname -r would carry a git suffix')
    cmdline = get('CMDLINE')
    cmd = cmdline[1:-1] if cmdline.startswith('"') else ''
    for arg in MUST_ARGS:
        if arg not in cmd.split():
            bad.append(f'CONFIG_CMDLINE lacks {arg}: {cmdline}')
    if a.bootargs is not None and cmd != a.bootargs:
        bad.append(f'CONFIG_CMDLINE {cmd!r} != DTB bootargs {a.bootargs!r}')

    if bad:
        print('check-config: FAILED', file=sys.stderr)
        for b in bad:
            print('  ' + b, file=sys.stderr)
        sys.exit(1)
    print(f'check-config: OK ({len(MUST_Y)} built-in symbols, '
          f'{sum(len(parse(f)) for f in a.fragments)} fragment lines, CMDLINE {cmd!r})')


main()
