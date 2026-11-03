# STARLTE kernel series on xnu-7195.141.2 (iOS 14 / macOS 11.5)

The iOS 14 kernel (STARBITE-2-PLAN.md section 6, "iOS 14: the stable Apple
OS"): the iOS 16 STARLTE series (xnu-8796.141.3: the fork's SWEET/A5XELTE/
HEROLTE patches plus `xnu/patches/03xx`) ported *back* to xnu-7195.141.2, the
public source of macOS 11.5, Apple's `macos-115` release.json (the iOS 14.7
kernel generation).

- Port tree: `build/xnu-7195`, one git repo with both upstream tags. Branch
  `base-8796` = xnu-8796.141.3 + the iOS 16 series, one commit per patch,
  exactly as `tools/build-xnu-local.sh` builds it; branch `port-7195` =
  xnu-7195.141.2 + the same commits cherry-picked in order
  (`tools/port-7195-next.sh`, which also sets the tree up from scratch).
- `tools/export-7195.sh` writes the port commits here; `tools/fix-7195.sh
  "note"` amends a build fix into the newest patch, applies it to the build
  tree and re-exports.
- Built by `tools/build-xnu-7195.sh`: blacktop/darwin-xnu-build at the pinned
  commit, taught `MACOS_VERSION=11.5` (libdispatch-1271.120.2, dtrace-370.40.1,
  AvailabilityVersions-80.3, bootstrap_cmds-121.100.1, Libsystem-1292.120.1,
  libplatform-254.80.2), Xcode 15.4, plain arm64 (no PAC), 4 KiB pages, 39-bit
  kernel VA as on iOS 16. Output `out/kernel.development.starlte-7195`.
- Staging scripts pinned to macOS 11.5's sources: libpthread-454.120.2
  (1cc19984; its kern_synch.c has one `psynch_wait_wakeup(kwq,` instead of
  two), hfs-556.100.11 (ba1a9cdc), BearSSL unchanged.
- Not carried, or carried in part: `SKIPPED.txt` (with the reason for each).

## Port notes (patches that needed changes; everything else applied unchanged)

- 0003: 7195's `pe_serial_functions` is the old uart_init/tr0/td0/rr0/rd0 table chained by
  `next`, no `serial_device_t`: the framebuffer console is written against it, no IRQ hooks
- 0004: SWEET added to 7195's board list; its cache line goes to `osfmk/arm/proc_reg.h`
  (7195 keeps them there); the SCTLR check in start.s skipped for SWEET (7195 compares with PAC bits)
- 0009: re-applied as line removals on 7195's export lists (Private.arm64.exports)
- 0010: arm_init.c and arm_vm_init.c are `standard` in 7195's files.arm64 (8796: config_pmap_ppl)
- 0012: serial_init() is called explicitly before STARTUP_SUB_KPRINTF (7195 calls it from the
  kprintf startup; the second call is the "reinit" path); vmsa_lock kept
- 0014: 7195 has no kmem RNG but the same entropy.c; the 8796 SHA-256 swap merged by hand, and
  0401 moves the SWEET HMAC DRBG state out of 7195's Fortuna-shaped `cckprng_ctx`
- 0017: pe_fiq.c taken whole from the 8796 series (it is 0086's GICv2 base) with a plain
  `gicr_wake_timeout_ns` (7195 has no MACHINE_TIMEOUT)
- 0032, 0041, 0043, 0047, 0065, 0080, 0084, 0085, 0089, 0200, 0202, 0300: context only
  (7195 code around the hunk differs); same code inserted
- 0202: 7195 checks `pageSize < PAGE_SHIFT_4K || > PAGE_SHIFT`; the 16 KiB-signature exception
  is added to that form
- 0304: 7195 sets commpage capabilities from ID_AA64ISAR0 in `commpage_init_cpu_capabilities`
  (no per-register feature functions): the M3 common-ID mask is applied there, and FP16 is not
  advertised (the M3 cores have none)

New on 7195:

- `0400-port-7195-starlte-config.patch`: `MASTER.arm64.starlte` rebuilt from 7195's
  MASTER.arm64.MacOSX with the herolte deltas (nos_arm_asm nos_arm_pmap instead of Apple's
  internal ARM_EXTRAS_BASE, mockfs, sweet_hfs, bindfs, no config_ext_resolver), and a stub
  `osfmk/arm64/amcc_rorgn.c` (listed for nos_arm_asm but not in the public source; only
  KTRR/CTRR use it)
- `0401-port-7195-build-fixes.patch`: see its commit message (missing PMAP_CS declarations in
  the public pmap.c, 8796 allocator names, classic IOUserClient for the HID user client, ...)
- `0402-port-4k-pages-above-1g.patch`, `0403-port-sync-page-phys-64bit.patch`: the 11417
  series' 0314 and 0315 (4 KiB VM pages at any RAM size; 64-bit PA in
  pmap_sync_page_attributes_phys), needed for bank-2 RAM; not in the 8796 series

## Userland (iOS 14)

See `docs/IOS14.md`.
