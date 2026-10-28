# STARLTE kernel series on xnu-11417.140.69 (iPadOS 18.7 / macOS 15.6)

The herolte fork's series (`build/ios-a5xelte/xnu/patches`, 0001-0238, built on
xnu-8796.141.3) plus our `xnu/patches/03xx`, ported to xnu-11417.140.69, the
kernel of iPadOS 18.7.11 (22H384) for J171AP. Built by `tools/build-xnu-11417.sh`
(darwin-xnu-build, Xcode 16.4, KDK 15.6 24G84).

- `0019-stage-libpthread.sh`, `0020-stage-bearssl.sh`, `0026-stage-hfs.sh`:
  the fork's staging scripts pinned to macOS 15.6's sources (libpthread-536
  69fb92b6, hfs-683.120.3 1ce7f99f; BearSSL unchanged). They run before the patches.
- One patch per fork/03xx patch, same file names, each exported from the port
  tree (`build/xnu-11417`, one commit per patch) with `git format-patch`; the
  port note is in the header (ignored by `git apply`).
- `0307-port-board-master-configs-11417.patch`: the four board MASTER configs
  (8796 MacOSX copies) three-way merged onto 11417's MacOSX config.
- `0308-port-sweetcpu-processor-boot.patch` (found on the phone): CPUs are booted
  with processor_boot() from SweetCPU::startAll (processor_start from IOKit is a
  no-op returning KERN_FAILURE in 11417; ml_processor_register's own
  processor_boot is skipped for the generic boards), and initCPU no longer opens
  an IRQ window for the self-SGI (processor_cpu_reinit asserts SIGPdisabled is
  still set, then unmasks and waits itself; the window tripped it on CPU 4).
- Not exported, obsolete on 11417 (upstream already): 0001, 0002, 0005, 0083.

Check: the staging scripts + every patch here, in name order, on a clean
xnu-11417.140.69 reproduce the port tree exactly; darwin-xnu-build's own
patches/15.0 then apply on top (link_kdk, link_kdk146 and remove_tightbeam skip:
Apple-SoC-only link flags, and a file 11417 no longer has).

Builds with Xcode 16.4 (clang 17) + KDK 15.6: `tools/build-xnu-11417.sh` ->
`out/kernel.development.starlte-11417` (19.9 MB). Boot-tested on the S9 with the
iOS 16 root (2026-10-28): 8/8 CPUs, UFS root mounted, launchd + libignition run;
iOS 16's launchd then aborts (SIGABRT), expected on this kernel.

## Build fixes (folded into the patch that introduced the code)

- 0004: `apple_arm64_cpu.h` (new in 11417) includes `apple_arm64_regs.h` for
  MPIDR_CORETYPE_*; only Apple SoC headers pulled it in before.
- 0006: the KDK's closed `lib<board>.os.<config>.a` is force-loaded for every
  config in 11417; skipped for SWEET A5XELTE HEROLTE STARLTE (open-source asm/pmap).
- 0008: empty stubs for `amcc_rorgn_ppl_amcc.c` / `amcc_rorgn_ppl_ctrr3.c`
  (11417's files.arm64 split of the closed AMCC read-only-region code).
- 0009: `libTightbeam.exports` is not added to the Private symbol set for the
  generic boards (Apple's closed Tightbeam library is not linked).
- 0043: `vm_shared_region_xnu.h`, `vm_map_internal.h` for the crash log (VM
  headers split in 11417).
- 0081, 0300 (sweet_ufs.c in 0028): `vm_kern_xnu.h` for `kmem_alloc_contig` / `KMA_*`.
- 0238: `primary_map` exists only with CONFIG_SCHED_SMT: dropped from the fast-core
  pick (every CPU is primary here); the realtime fast-core preference had landed
  in the SMT-only `choose_processor_for_realtime_thread_smt`, now also in the
  non-SMT `choose_processor_for_realtime_thread`.

## Port notes (patches that needed changes)

- `0001-new-clang-drop-removed-auto-var-init-flag`: obsolete: xnu-11417 no longer passes -enable-trivial-auto-var-init-zero-knowing-it-will-be-removed-from-clang
- `0002-new-clang-backport-UIO_STACKBUF`: obsolete: UIO_STACKBUF is upstream in xnu-11417 (bsd/sys/uio_internal.h)
- `0003-sweet-framebuffer-serial-console`: serial_init now matches a serial-device phandle and returns early without one; fbcon is probed before that and initialised on its own. SERIAL_SWEET_FBCON = 0x10 (PI3 UART became PL011 0x4, DCC 0x8).
- `0004-sweet-machine-config`: BCM2837 target is gone in xnu-11417 (list now T6000..T8132 VMAPPLE): SWEET appended after VMAPPLE in the config list, flags, Makefile and board_config.
- `0005-non-pac-build-fixes`: obsolete: the non-PAC pmap field is reserved11 upstream (no reserved10 clash)
- `0007-sweet-stub-apple-internal-headers`: task.c hunk obsolete: t_kpc is under CONFIG_CPU_COUNTERS upstream. MASTER configs: see 0307.
- `0008-sweet-fill-closed-lib-gaps-kpc-stub`: clean_mmu_dcache label placed before the new ARM64_PROLOG. kpc.c stub dropped: xnu-11417 ships the real osfmk/arm64/kpc.c, built only with config_cpu_counters (off for sweet); add a stub under another name only if the link needs one.
- `0009-sweet-drop-apple-soc-only-exports`: same 35 Apple-SoC-only symbols dropped from the 11417 export list (line context moved)
- `0010-sweet-early-framebuffer-debug`: mark 7 moved: arm_vm_init now follows the gDramBase/pmap_first_pnum setup
- `0011-sweet-physmap-alignment-padding`: physmap_l1_entries hunk: same change, context moved
- `0012-sweet-keep-debug-console-after-ttbr0`: include and export-list lines, context moved
- `0014-sweet-kernel-hmac-prng`: entropy.c part dropped: 11417 accumulates with SHA512_* initialised in entropy_init (no lazy corecrypto wait), and 0033 backs SHA512_* with BearSSL on SWEET. PRNG block unchanged (cckprng_funcs identical); CCKPRNG_ENTROPY_SIZE is 64.
- `0015-sweet-unavailable-trust-cache-runtime`: same SWEET NULL-runtime guards, placed before the new CONFIG_SPTM / PPL / plain dispatch
- `0016-sweet-device-tree-platform-expert`: files.arm64 entry appended after the new Exclaves block
- `0047-sweet-persistent-console-log`: disableConsoleOutput is now a file-scope extern; hook placed the same way
- `0065-sweet-smp-psci`: topology: die-cluster-id / cluster-core-id now split by cluster->die_cluster_id; cpu_start cleans placed before the new CONFIG_SPTM block
- `0067-sweet-console-mute`: mute check added after the ported 0047 hook
- `0070-sweet-pid-hibernate`: pid_hibernate moved from bsd/vm/vm_unix.c to bsd/kern/kern_proc.c; same guard there
- `0080-sweet-net-trace`: rtm is now the RTM macro in route_output
- `0083-xcode27-iig-kernel-content`: obsolete: IORPC.kernelContent is upstream in xnu-11417 (the Xcode 16+ iig ABI)
- `0085-a5xelte-board-target`: board lists: BCM2837 gone, A5XELTE appended after SWEET. MASTER configs: see 0307.
- `0089-a5xelte-decon-screen-checkpoints`: checkpoint block placed before the ported SCTLR check (MOV64 x1 spacing differs upstream)
- `0091-a5xelte-assembly-screen-stages`: after APPLY_TUNABLES; the following guard is now HAS_CLUSTER && !NO_CPU_OVRD
- `0093-a5xelte-39bit-kernel-va`: same A5XELTE override after the MacOSX ARM_LARGE_MEMORY block
- `0094-a5xelte-cortex-a53-codegen`: same flag change on the ported line
- `0097-a5xelte-zone-va-size`: same, whitespace of the #if differs upstream
- `0100-a5xelte-counter-epoch`: context only
- `0109-a5xelte-decon-iosurface-bridge`: context only (ported 0047/0067 lines)
- `0118-a5xelte-home-hid-display-probe`: pid_hibernate guard lives in bsd/kern/kern_proc.c now (see 0070)
- `0200-herolte-board-target`: board lists: HEROLTE appended (BCM2837 gone)
- `0224-herolte-parallel-copy-kmem-log`: claims log kept; the locate-space panic hunk dropped: 11417's panic already prints size and range
- `0238-herolte-fast-cores-aks-wrap`: choose_processor split into choose_processor_smt / choose_processor; both hunks go into the non-SMT one. Idle pick: 11417 tries perfcontrol preferred / non-avoided masks first (both 0 without CLPC), the fast-core choice replaces the final lsb_first(idle_map) fallback (no primary-only map there any more).
- `0300-starlte-board-target`: STARLTE appended to the board list; SCTLR SPAN bit goes into the new SCTLR_EL1_REQUIRED (SCTLR_EL1_DEFAULT = REQUIRED | OPTIONAL upstream)
