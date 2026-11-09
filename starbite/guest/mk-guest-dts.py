#!/usr/bin/env python3
"""Device tree for the guest Linux on the M3 cluster (docs/STARBITE-OS-LINUX-M3.md 3).

    mk-guest-dts.py --profile s9|qemu [--rd-size N] [--db-guest INTID] [--bootargs-extra S] OUT.dts

The guest sees only: its RAM (+ the shared region, reserved), the four M3 cores (PSCI), the GIC (marked
starbite,shared-distributor for the patched driver; the stock driver ignores it), the generic timer, the
pstore/ramoops console zone in the shared region and the starbite,guest-shm node (sbguest.ko). No clocks,
no pinctrl, no UART, no display controller: Starbite OS owns every SoC block.
Addresses come from sbshm.h (parsed here, so the C side and the DT cannot drift).
"""
import argparse
import re
from pathlib import Path

here = Path(__file__).resolve().parent
H = (here / 'sbshm.h').read_text()


def c(name):
    m = re.search(r'#define\s+' + name + r'\s+(0x[0-9a-fA-F]+)U?L*L*', H)
    if not m:
        raise SystemExit(f'mk-guest-dts: {name} not in sbshm.h')
    return int(m.group(1), 16)


ap = argparse.ArgumentParser()
ap.add_argument('--profile', choices=['s9', 'qemu'], required=True)
ap.add_argument('--rd-size', type=lambda s: int(s, 0), default=0)
ap.add_argument('--db-guest', type=lambda s: int(s, 0), default=0, help='INTID of the host->guest doorbell SPI')
ap.add_argument('--bootargs-extra', default='')
ap.add_argument('--psci01', action='store_true', help='qemu: the S9-style PSCI 0.1 node (listed ids)')
ap.add_argument('--fb', metavar='WxH', default='',
                help='the guest kernel of its own (linux/guest.config): a simple-framebuffer node on surface buffer 0 '
                     '(simpledrm, fbcon), the surface region no-map (simpledrm maps it write-combine)')
ap.add_argument('--term', action='store_true', help='with --fb: sbguest.mode=term (a shell on tty1, guestd presents)')
ap.add_argument('out')
a = ap.parse_args()

P = 'S9' if a.profile == 's9' else 'QEMU'
LB, LS = c(f'SBG_{P}_LINUX_BASE'), c(f'SBG_{P}_LINUX_SIZE')
SB, SS = c(f'SBG_{P}_SHM_BASE'), c(f'SBG_{P}_SHM_SIZE')
GICD, GICC = c(f'SBG_{P}_GICD'), c(f'SBG_{P}_GICC')
TRAMP = c(f'SBG_{P}_TRAMP') + c('SBG_TRAMP_GUEST_OFF')
DTB_OFF, RD_OFF = c('SBG_DTB_OFF'), c('SBG_RD_OFF')
OOPS_OFF, OOPS_SIZE, SURF_OFF = c('SBSHM_OOPS_OFF'), c('SBSHM_OOPS_SIZE'), c('SBSHM_SURF_OFF')
assert LB % (2 << 20) == 0 and LB + LS == SB, 'the shared region must follow the guest RAM'
assert RD_OFF > DTB_OFF and RD_OFF + a.rd_size < LS

if a.profile == 's9':
    cpus = [0x100, 0x101, 0x102, 0x103]     # the M3 cluster (MPIDR Aff1 = 1)
    conduit = 'smc'                          # EL3 PSCI (Samsung's EL2 passes SMC through: Starbite's CPU_ON works)
    gic_reg = f'<0x0 {GICD:#x} 0x0 0x1000>, <0x0 {GICC:#x} 0x0 0x1000>'
    ppi_mask = 0xf0                          # CPU interfaces 4..7 (checked by the probe, docs 4)
    timer_freq = '\n\t\tclock-frequency = <26000000>;'
    compat = 'samsung,mongoose-m3'
else:
    cpus = [4, 5, 6, 7]                      # -smp 8: 0..3 are sbsim's ("the A55s"), 4..7 the guest's
    conduit = 'hvc'                          # QEMU virt without EL2/EL3 emulates PSCI behind HVC
    gic_reg = f'<0x0 {GICD:#x} 0x0 0x10000>, <0x0 {GICC:#x} 0x0 0x10000>'
    ppi_mask = 0xf0
    timer_freq = ''
    compat = 'arm,cortex-a53'

if a.profile == 's9' or a.psci01:
    # the S9's own node (exynos9810-starlte.dtb): PSCI 0.1, function ids listed. Linux then has no PSCI
    # SYSTEM_OFF / SYSTEM_RESET at all: a guest power off or panic can never switch off or reset the phone.
    # No cpu_suspend: no cpuidle states here (the M3s idle in WFI; cluster power-down is docs 8, later).
    psci_body = (f'\t\tcompatible = "arm,psci";\n\t\tmethod = "{conduit}";\n'
                 '\t\tcpu_off = <0x84000002>;\n\t\tcpu_on = <0xc4000003>;')
    if a.profile == 's9':
        # Samsung's FlexPMU core / cluster power bookkeeping (linux/0002 + sbguest.ko): <PMU, its interrupt
        # block, the APM mailbox>. Without it CPU_OFF leaves the M3s powered (P2b on the phone).
        psci_body += '\n\t\tstarbite,flexpmu = <0x14060000 0x14070000 0x14100000>;'
else:
    # QEMU's PSCI 1.0 (it has SYSTEM_OFF: sbguest.ko's handlers must win, run-qemu.sh "nomod" shows why)
    psci_body = f'\t\tcompatible = "arm,psci-1.0", "arm,psci-0.2";\n\t\tmethod = "{conduit}";'

# initcall_blacklist: the S9 port's s9p_acpm_init needs no DT node; on the phone it would find the APM table
# and power-cycle the touch LDOs through ACPM (Starbite OS's touch rails, and its ACPM mailbox). The port's
# other initcalls look for their DT nodes or for compatible "samsung,exynos9810" (not this guest's).
# the guest's own CPU_ONs (its secondaries, hotplug) through a slot below 4 GiB (linux/0002 patch; P2 on the
# phone: secondary_entry in the 0x8_8000_0000 bank never ran). QEMU uses it for every CPU_ON, to exercise it.
psci_body += (f'\n\t\tstarbite,cpu-on-trampoline = /bits/ 64 <{TRAMP:#x}>;' +
              ('\n\t\tstarbite,cpu-on-trampoline-always;' if a.profile == 'qemu' else ''))

bootargs = ('loglevel=7 printk.devkmsg=on rdinit=/init nokaslr cpuidle.off=1 '
            'initcall_blacklist=s9p_acpm_init '
            f'maxcpus=4 nr_cpus=4 {a.bootargs_extra}').strip()
rd = ''
if a.rd_size:
    rd = (f'\n\t\tlinux,initrd-start = /bits/ 64 <{LB + RD_OFF:#x}>;'
          f'\n\t\tlinux,initrd-end = /bits/ 64 <{LB + RD_OFF + a.rd_size:#x}>;')
cpu_nodes = ''.join(f'''
		cpu@{r:x} {{
			device_type = "cpu";
			compatible = "{compat}";
			reg = <{r:#x}>;
			enable-method = "psci";
		}};
''' for r in cpus)
ppi = lambda n: f'1 {n} {(ppi_mask << 8) | 8:#x}'
db = f'\n\t\tinterrupts = <0 {a.db_guest - 32} 1>;' if a.db_guest else ''


SURF_AT = SB + c('SBSHM_SURF_OFF')
surf_nomap = fb_node = ''
if a.fb:
    fw, fh = (int(v) for v in a.fb.split('x'))
    assert SURF_AT + fw * fh * 4 <= SB + SS
    surf_nomap = '\n\t\t\tno-map;'
    fb_node = f'''	/* the guest's display: surface buffer 0 (guestd --present publishes its changed rows to Starbite OS) */
	framebuffer@{SURF_AT:x} {{
		compatible = "simple-framebuffer";
		reg = <{SURF_AT >> 32:#x} {SURF_AT & 0xffffffff:#x} 0x0 {fw * fh * 4:#x}>;
		width = <{fw}>;
		height = <{fh}>;
		stride = <{fw * 4}>;
		format = "x8r8g8b8";
	}};

'''
if a.term:
    assert a.fb, '--term needs --fb'
    # the kernel's console: the ramoops zone only (Starbite reads it), not the VT: tty1 shows the shell alone
    bootargs += ' sbguest.mode=term console=ramoops-1'


def h64(v):
    return f'{v >> 32:#x} {v & 0xffffffff:#x}'


dts = f'''/dts-v1/;
/* Generated by starbite/guest/mk-guest-dts.py --profile {a.profile}: the guest Linux on the M3 cluster */
/ {{
	model = "Starbite OS guest ({a.profile}, M3 cluster)";
	compatible = "starbite,m3-guest";
	#address-cells = <2>;
	#size-cells = <2>;
	interrupt-parent = <&gic>;

	chosen {{
		bootargs = "{bootargs}";{rd}
		starbite,shm = /bits/ 64 <{SB:#x} {SS:#x}>;
	}};

	/* the guest RAM and the shared region (reserved below, mapped: /dev/mem O_SYNC is then write-combine) */
	memory@{LB:x} {{
		device_type = "memory";
		reg = <{h64(LB)} {h64(LS + SS)}>;
	}};

	reserved-memory {{
		#address-cells = <2>;
		#size-cells = <2>;
		ranges;

		sbshm_ctl: sbshm-ctl@{SB:x} {{
			reg = <{h64(SB)} {h64(OOPS_OFF)}>;
		}};

		ramoops@{SB + OOPS_OFF:x} {{
			compatible = "ramoops";
			reg = <{h64(SB + OOPS_OFF)} {h64(OOPS_SIZE)}>;
			console-size = <{OOPS_SIZE:#x}>;
			record-size = <0>;
			ecc-size = <0>;
		}};

		sbshm_surf: sbshm-surf@{SB + OOPS_OFF + OOPS_SIZE:x} {{
			reg = <{h64(SB + OOPS_OFF + OOPS_SIZE)} {h64(SS - OOPS_OFF - OOPS_SIZE)}>;{surf_nomap}
		}};
	}};

	cpus {{
		#address-cells = <1>;
		#size-cells = <0>;
{cpu_nodes}	}};

	psci {{
{psci_body}
	}};

	timer {{
		compatible = "arm,armv8-timer";
		interrupts = <{ppi(13)}>, <{ppi(14)}>, <{ppi(11)}>, <{ppi(10)}>;{timer_freq}
	}};

	gic: interrupt-controller@{GICD:x} {{
		compatible = "arm,gic-400";
		#interrupt-cells = <3>;
		#address-cells = <0>;
		interrupt-controller;
		reg = {gic_reg};
		/* Starbite OS owns the distributor: the patched driver (linux/0001-*.patch) leaves GICD_CTLR and
		 * every SPI it does not request alone. */
		starbite,shared-distributor;
	}};

{fb_node}	starbite-guest@{SB:x} {{
		compatible = "starbite,guest-shm";
		reg = <{h64(SB)} 0x0 0x1000>;{db}
	}};
}};
'''
Path(a.out).write_text(dts)
print(f'mk-guest-dts: {a.out}: RAM {LB:#x}+{LS:#x}, shm {SB:#x}+{SS:#x}, cpus {[hex(x) for x in cpus]}, '
      f'initrd {a.rd_size} bytes')
