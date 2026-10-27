#!/usr/bin/env python3
"""Build the starlte (SM-G960F, Exynos 9810) ADT for the STARLTE XNU config.

Derived from ios-a5xelte tools/mkadt_herolte.py (wardevsa, herolte branch);
uses its adt.py / mkadt.py from ref/ios-herolte/tools. Addresses come from
the starlte DTB inside the official TWRP image (device/dtb/starlte.dts), the
stock kernel cmdline and the shim/starlte probe (see NOTES.md). First XNU
boot: the 472 MiB window below the RKP region; four Cortex-A55 CPUs (boot-arg
cpus=N picks how many start).
"""
import argparse
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'ref/ios-herolte/tools'))
import adt     # noqa: E402
import mkadt   # noqa: E402

DRAM_BASE = 0x92000000      # isolated XNU window (after vendor logs at 0x91202000..0x9179c000)
DRAM_SIZE = 0x1d800000      # 472 MiB, ends at the RKP region 0xaf800000
# --bank2: the S9's second DRAM bank (TWRP /proc/iomem 880000000-8ffffffff, 2 GiB, no
# reserved-memory node in it; Windows uses it as RAM stage 3). Needs the bank-aware
# shim (BANK2_* in shim/starlte/xnu_boot.c) and, above 4 GiB, the SMP trampoline at
# 0x8fffe000 (herolte patch 0223, active for STARLTE).
BANK2_BASE, BANK2_SIZE = 0x880000000, 0x80000000
SOC_BASE = 0x10000000       # arm-io ranges base; child regs are relative
FB_BASE, FB_WIDTH, FB_HEIGHT = 0xcc000000, 1440, 2960
TIMEBASE = 26000000         # CNTFRQ_EL0 is 0 at handoff; OSCCLK is 26 MHz
CONSOLE_TOP = 640             # boot console rows start below the Starbite header
CONSOLE_HEIGHT = 1880         # ... and end above the bottom status card
LITTLE_CORES = 4            # Cortex-A55 cluster; cpus=N boot-arg limits what starts
BIG_CORES = 4               # Mongoose M3 cluster (--big)


def set_prop(node, name, value):
    for i, (key, flags, _) in enumerate(node.props):
        if key == name:
            node.props[i] = (key, flags, value)
            return
    node.props.append((name, 0, value))


def child(node, name):
    for candidate in node.children:
        if candidate.prop('name') == mkadt.cstr(name):
            return candidate
    raise ValueError(f'missing donor ADT node {name}')


def build(dram=(DRAM_BASE, DRAM_SIZE), scale2=False, big=False):
    root = mkadt.build(seed=os.urandom(64), qemu=True)
    canvas = (FB_WIDTH, FB_HEIGHT)
    if scale2:
        # As mkadt_herolte.py --scale2: a whole iPhone XR screen (414x896
        # points @2x = 828x1792), 2.9x fewer pixels than the panel for the CPU
        # renderer; the kernel scales it up (sweetfb_scale=1 sweetfb_lw=828
        # sweetfb_lh=1792). Needs the D22AP /product node for the artwork.
        set_prop(child(root, 'chosen'), 'display-scale', mkadt.u32(2))
        product = child(root, 'product')
        set_prop(product, 'artwork-device-subtype', mkadt.u32(1792))
        set_prop(product, 'artwork-scale-factor', mkadt.u32(2))
        canvas = (828, 1792)
    # iPhone X identity, as herolte; 'samsung,a5xelte' keeps the shared
    # SweetPlatformExpert IONameMatch.
    set_prop(root, 'compatible', mkadt.cstr('D22AP') + mkadt.cstr('iPhone10,3') +
             mkadt.cstr('samsung,a5xelte') + mkadt.cstr('samsung,herolte') +
             mkadt.cstr('samsung,starlte') + mkadt.cstr('samsung,exynos9810') +
             mkadt.cstr('AppleARM'))
    chosen = child(root, 'chosen')
    set_prop(chosen, 'dram-base', mkadt.u64(dram[0]))
    set_prop(chosen, 'dram-size', mkadt.u64(dram[1]))
    set_prop(chosen, 'firmware-version', mkadt.cstr('starlte-entry'))
    set_prop(chosen, 'system-firmware-version', mkadt.cstr('starlte-entry'))
    # Synthetic, locally administered addresses (see mkadt_herolte.py).
    for name, last in (('wifi0', 1), ('bluetooth0', 2), ('ethernet0', 3)):
        set_prop(chosen, f'mac-address-{name}', bytes((0x02, 0x53, 0x54, 0x41, 0x52, last)))
    cpus = child(root, 'cpus')
    cpu0 = child(cpus, 'cpu0')
    # S-Boot starts on Cortex-A55 core 0: MPIDR 0x81000000, Aff1|Aff0 = 0.
    # SweetCPU matches IONameMatch 'arm,cortex-a53'. The other seven cores
    # stay undescribed until their MPIDRs are measured.
    set_prop(cpu0, 'compatible', mkadt.cstr('arm,cortex-a55') + mkadt.cstr('arm,cortex-a53') +
             mkadt.cstr('ARM,v8'))
    set_prop(cpu0, 'reg', mkadt.u32(0))
    set_prop(cpu0, 'timebase-frequency', mkadt.u32(TIMEBASE))
    cpus.children = [cpu0]
    # Cortex-A55 cores 1..3: MPIDR_EL1 Aff1 = core (0x81000n00), measured on
    # the phone with starsmpprobe=1; PSCI CPU_ON takes n (SweetCPU.cpp).
    # One cluster, like mkadt_herolte.py.
    # --big: the Mongoose M3 cores as logical 4..7. They need the vendor PMU
    # defaults before PSCI CPU_ON 0x100..0x103 (Linux S9+ port; SweetCPU.cpp),
    # and their MPIDR is 0x100+n without the MT bit: the same Aff1:Aff0 as
    # A55 core 1, so XNU's reset vector adds bit 16 for them (reg 0x10100+n).
    for logical in range(1, LITTLE_CORES + (BIG_CORES if big else 0)):
        m3 = logical >= LITTLE_CORES
        cpus.children.append(mkadt.node(f'cpu{logical}', [
            ('device_type', mkadt.cstr('cpu')),
            ('compatible', (mkadt.cstr('samsung,mongoose-m3') if m3 else mkadt.cstr('arm,cortex-a55')) +
             mkadt.cstr('arm,cortex-a53') + mkadt.cstr('ARM,v8')),
            ('reg', mkadt.u32(0x10100 + logical - LITTLE_CORES if m3 else logical << 8)),
            ('cpu-id', mkadt.u32(logical)),
            ('cluster-id', mkadt.u32(0)),
            ('cluster-type', mkadt.u32(0)),
            ('die-cluster-id', mkadt.u32(0)),
            ('cluster-core-id', mkadt.u32(logical)),
            ('l2-cache-id', mkadt.u32(1 if m3 else 0)),
            ('l3-cache-id', mkadt.u32(0)),
            ('state', mkadt.cstr('stopped')),
            ('timebase-frequency', mkadt.u32(TIMEBASE)),
            ('clock-frequency', mkadt.u32(2704000000 if m3 else 1794000000)),
            ('bus-frequency', mkadt.u32(100000000)),
            ('memory-frequency', mkadt.u32(1794000000)),
            ('peripheral-frequency', mkadt.u32(TIMEBASE)),
            ('fixed-frequency', mkadt.u32(TIMEBASE)),
        ]))
    arm_io = child(root, 'arm-io')
    set_prop(arm_io, 'device_type', mkadt.cstr('starlte-io'))
    set_prop(arm_io, 'compatible', mkadt.cstr('samsung,exynos9810-io') + mkadt.cstr('arm-io'))
    set_prop(arm_io, 'ranges', mkadt.u64(0, SOC_BASE, 0xf0000000))
    arm_io.children = [n for n in arm_io.children if n.prop('name') == mkadt.cstr('wdt')]
    # GIC-400 (starlte DTB interrupt-controller@10100000): distributor
    # 0x10101000, CPU interface 0x10102000; virtual timer PPI 11 = INTID 27.
    arm_io.children.append(mkadt.node('gic', [
        ('compatible', mkadt.cstr('arm,cortex-a15-gic')),
        ('interrupt-controller', mkadt.cstr('master')),
        ('virtual-timer-intid', mkadt.u32(27)),
        ('reg', mkadt.u64(0x10101000 - SOC_BASE, 0x1000, 0x10102000 - SOC_BASE, 0x1000)),
    ]))
    arm_io.children.append(mkadt.node('disp0', [
        ('device_type', mkadt.cstr('display-subsystem')),
        ('compatible', mkadt.cstr('samsung,a5xelte-display') +
         mkadt.cstr('samsung,herolte-display') + mkadt.cstr('samsung,starlte-display')),
        ('canvas-width', mkadt.u32(canvas[0])),
        ('canvas-height', mkadt.u32(canvas[1])),
    ]))
    root.children = [n for n in root.children if n.prop('name') not in
                     (mkadt.cstr('sweet-fbcon'), mkadt.cstr('sweet-ufs'))]
    # Exynos 9810 UFSHCI (starlte DTB ufs@0x11120000): standard HCI at +0,
    # vendor HCI at +0x1100 (0x11121100). Absolute address, read by
    # bsd/dev/herolte_ufs.c (ufsprobe=1; read-only on STARLTE).
    root.children.append(mkadt.node('herolte-ufs', [
        ('reg', mkadt.u64(0x11120000, 0x1300)),
    ]))
    # pe_serial.c looks this node up by name for the kprintf console.
    root.children.append(mkadt.node('a5xelte-fbcon', [
        # Console rows between the loader's header and bottom card.
        ('reg', mkadt.u64(FB_BASE + CONSOLE_TOP * FB_WIDTH * 4,
                          FB_WIDTH * (CONSOLE_HEIGHT - CONSOLE_TOP) * 4)),
        ('width', mkadt.u32(FB_WIDTH)),
        # The console stops above the loader's bottom band (tools/brand
        # overlay.mjs BAND_TOP), which then stays up through the kernel boot.
        ('height', mkadt.u32(CONSOLE_HEIGHT - CONSOLE_TOP)),
        ('stride', mkadt.u32(FB_WIDTH * 4)),
    ]))
    return root


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--scale2', action='store_true',
                        help='iPhone XR canvas 828x1792 @2x, scaled by the kernel')
    parser.add_argument('--big', action='store_true',
                        help='describe the four Mongoose M3 cores too (logical 4..7, boot-arg cpus=8)')
    parser.add_argument('--bank2', action='store_true',
                        help='XNU RAM = the second bank instead of the 472 MiB window')
    parser.add_argument('--bank2-mb', type=int, default=1024,
                        help='MiB of the second bank; on herolte 1536 boots with boot-arg '
                             'vm_compression_limit=98304 and 2048 panics in write_pte_fast')
    args = parser.parse_args()
    dram = (BANK2_BASE, min(args.bank2_mb << 20, BANK2_SIZE)) if args.bank2 else (DRAM_BASE, DRAM_SIZE)
    blob = adt.serialize(build(dram=dram, scale2=args.scale2, big=args.big))
    args.output.write_bytes(blob)
    print(f'{args.output}: {len(blob)} bytes')
