# Wi-Fi on the Galaxy S9: the BCM4361 for Windows 11 and Starbite OS

Track C of `docs/PLAN-2026-11-07.md`. One portable C core (`starbite/wifi/`, "bcmw") that brings up the S9's
Broadcom BCM4361 by itself: the Exynos 9810's PCIe controller, the chip, its firmware, brcmfmac's msgbuf protocol.
Windows gets it through a KMDF driver with WiFiCx on top; Starbite OS through a libsb shim.

Status 2026-11-07:

| Part | State |
|---|---|
| Core (`starbite/wifi/`: 3,600 lines, plus 870 for the VENDOR readers and the libsb shim) | Written, reviewed adversarially (section 10). Host-tested on a register-level model (steps 1 to 10). Not yet run on the phone |
| NVRAM processing | Byte-identical to Linux's brcmfmac on all 10 of the phone's NVRAM files |
| Reading the files from VENDOR | ext4 (LineageOS) and EROFS (One UI 8, S9 #1 now). The EROFS reader reads all 13 radio files from a copy of S9 #1's own VENDOR image, byte-identical |
| libsb shim + test image `wifitest.img` | Built (4.1 MiB, header checked). First phone test requested (section 9) |
| Windows driver | `SbWifi.sys` (W1 skeleton, section 7) builds and signs with `tools/win`; not run (Windows on the phone is being repaired) |
| Starbite OS | Shim done; integration plan in section 8, to agree with the `starbite-os` session |

## 1. The hardware

| What | Where / value | Source |
|---|---|---|
| Radio | Broadcom BCM4361 rev 5 (B2) in a SEMCO or Murata module; PCI ID 14e4:441f; chipcommon says 0x4361 or 0x4347 | bcmdhd, Linux logs on this phone (`BCM4361/5`) |
| Link | PCIe channel 0 of the Exynos 9810, one lane. an earlier port trains Gen2; mainline Linux trains Gen1 | an earlier port, pmOS/ChromeOS logs |
| Controller | DesignWare core + Samsung PHY: ELBI 0x116A0000, PCS 0x116C0000, PHY 0x116D0000, DBI 0x11700000, config window 0x127FE000 (4 KiB, via the iATU), memory window 0x11800000..0x126FFFFF | Samsung `pci-exynos.c`, device tree `pcie0@116A0000` |
| Power and reset | WLAN_REG_ON = gpg0-5 (0x10430080), PERST# = gpf2-2 (0x11430020), CLKREQ# = gpf2-0 (function 2, pull-up). PMU 0x1406071C bit 0 takes the PHY out of isolation | `dhd_custom_exynos.c`, device tree |
| Clocks | CMU_FSYS1 gates (0x11402000.., hardware-controlled by default); for DMA also the PCIe SysMMU gate (+0x2098) must be on | an earlier port wifi-a4..a6 |
| Interrupt | SPI 253 = GIC INTID 285, level high; INTx is latched in ELBI 0x000 (INTA bit 0). No MSI on this chip | Samsung, an earlier port, mainline port |
| BARs | BAR0: 64-bit, 32 KiB register window (a 4 KiB view of the chip's backplane, selected by config 0x80). BAR2: 64-bit, 4 MiB view of the chip's RAM (RAM base 0x170000) | brcmfmac, bcmdhd |
| Files | `bcmdhd_sta.bin_b2` (firmware 13.50.37 on S9 #1), `nvram.txt_<module>` (board data), `bcmdhd_clm.blob` (regulatory). In VENDOR `/firmware/` (One UI 8 and LineageOS) or `/etc/wifi/` (stock) | S9 #1's VENDOR image |
| Which NVRAM | From the module's ID in OTP: write 0x18010324 = 0x00fa0000 and 0x18012618 = 0x0e4fffff, read the CIS at 0x18011110 (22 words); tag 0x81 = VID. 91 33 → `nvram.txt_r02a_b2`, a1 33 → `_r02b_b2`, other Murata → `_murata_r041_b2` | bcmdhd `dhd_custom_cis.c`, via an earlier port |
| MAC address | `/efs/wifi/.mac.info` (EFS: read only, never logged). The NVRAM's own is a placeholder (00:90:4c:12:d0:01) | bcmdhd, an earlier port |

S9 #1's VENDOR (One UI 8, since 2026-11-07) is **EROFS** (lz4hc, compact indexes). Its `/firmware/` has
`bcmdhd_sta.bin_b2` sha256 `e3c66440...` (13.50.37, the same file an earlier port used) and `nvram.txt_r02a_b2`
`5bb574b0...`; the module ID is not known yet (the first test prints it).

## 2. How an earlier port does it (XNU, iOS 16.5, works: WPA2, DHCP, ping)

an earlier port (`build/refport/hackios-xnu-kernel`, `devices/starlte/WIFI.md`) ran Linux's brcmfmac inside XNU through a
compatibility layer, on its own root-complex driver `drivers/wifi/exynos_pcie.c`:

- Steps `wifi_pcie=1..6`: look (PMU, gates, pins), power (PERST# low, CLKREQ#, WLAN_REG_ON high, 200 ms), PHY
  (PMU isolation off, power-down off, soft reset, PERST# high 20 ms, Samsung's PHY tables, PCS/MAC/PHY resets, CDR
  reset), link (setup_rc for x1 Gen2, LTSSM on, L0 within 20 ms, 3 tries), enumerate (iATU, ID, BARs into the
  window), hand over (memory decoding, the DMA path's gates).
- Then brcmfmac 7.0.14 with a small patch (device 0x441f, chip IDs 0x4361/0x4347, RAM base 0x170000, file names).
- What it learned on the phone, all kept in bcmw:
  - wifi-a4/a5: reading the PCIe SysMMU's registers (0x11420000) stops the bus. Its clock gate must be on (the
    radio's DMA passes it, in bypass), its registers must never be touched.
  - wifi-a6: the control buffers must be 8 KiB (`BRCMF_MSGBUF_MAX_CTL_PKT_SIZE`), not 4 KiB, or the first ioctl's
    answer has nowhere to go ("Dongle setup failed").
  - wifi-a7..a9: firmware up, CLM loaded, scan: 7 access points on 2.4 and 5 GHz.
  - wifi-d1..d4: WPA2 needs the host's supplicant (Samsung's firmware is built without `BCMSUP_4WAY_HANDSHAKE`),
    and the network device must have the real MAC (a zero MAC gave a bad MIC).
- Its working settings: **polling** (`brcmfmac_poll=1`), **ring indices in the chip's RAM**
  (`brcmfmac_tcm_idx=1`), **uncached** DMA memory, SysMMU in bypass.

## 3. How Linux does it (postmarketOS, ChromeOS, mainline 7.1)

`pcie-exynos9810.c` (SheroAbi's port, in the VM at `~/linux/s9p/src/pci/`) plus mainline brcmfmac with three
patches (`/mnt/build/src/mainline`, `git diff`): the 4361 IDs and RAM base, a deferred probe, and **H2D stamping**
in `commonring.c`. It differs from an earlier port in a few places:

| Point | an earlier port | Mainline port | bcmw |
|---|---|---|---|
| WLAN_REG_ON | high, 200 ms | **low 50 ms first** (a warm chip still runs the old firmware), then high 200 ms | low 50 ms, high 200 ms |
| CMU_FSYS1 gates | read at step 1 (before the PMU); forced open only if closed | forced open (`|= 0x300000`) **after** the PMU, because a gate access with the PHY isolated faulted | read only after the PMU (step 1 skips them while isolated, and reads through `probe32` when the OS has it); forced only if closed (`ungate=1`) |
| Link speed | Gen2 | Gen1 (log: "PCIe Gen.1 x1 link up") | Gen2, `link_speed=1` for Gen1 |
| Coherency | uncached memory (SYSREG +0x700 left alone) | SYSREG_FSYS1 +0x700 bits 9:8 = 3 (snoop), cached memory | uncached by default; `coherent=1` |
| H2D stamping | none (worked) | needed ("without it the firmware returns BADOPTION") | always, as Broadcom's DHD does |
| Interrupts | polling, later INTx | INTx through ELBI | polling first; `bcmw_isr()` for INTx |
| Firmware | 13.50.37 | 13.50.28 (from the pmOS/ChromeOS image) | whatever VENDOR has |

The stamping: DHD writes, in every host-to-chip item, an epoch (sequence % 253, from 254) into byte 3 and a phase
bit (0x80, flipped each time an item lands on the ring's base, so the first pass carries 0x80) into byte 2
(`dhd_msgbuf.c`: `dhd_prot_alloc_ring_space`, `cmn_hdr.epoch`). an earlier port's brcmfmac did not stamp and still worked; the
mainline kernel did not work without it. bcmw stamps every H2D item exactly as DHD does; the host test checks the
sequence over three passes of a ring and the model's firmware refuses unstamped items with BADOPTION.

## 4. The bring-up, step by step (what bcmw does)

Each step logs what it does before it does it; `stop_after` stops after any step (as an earlier port's `wifi_pcie=` /
`brcmfmac=`), so a phone run can go one step at a time.

1. **Look** (reads only): PMU 0x1406071C, the pins; the controller's CMU gates only when the PHY is out of
   isolation.
2. **Power**: PERST# low, CLKREQ# to the controller with pull-up, WLAN_REG_ON low 50 ms, high, 200 ms; read back.
3. **PHY**: PMU bit 0 on (read back), the controller's gates checked (forced only with `ungate`), PCS power-down
   bits off, PHY +0x80/+0x15C, LTSSM history on; with `coherent` the SYSREG shareability.
4. **Link** (up to 3 tries): soft core reset, PERST# high 20 ms, L1 exit in hardware, no clock gating, SYSREG
   +0x1044/+0x1050 (lane 0, high speed), PCS global reset, Samsung's 48 + 80 PHY table bytes, PCS/MAC/PHY resets,
   CDR reset; root port: x1, BAR0/1 off, INTA, bus 0/1/0xff, command 0x107, class bridge, LNKCAP (L1 exit 64 us,
   x1, Gen2), aux clock 26 MHz, L1 substates, LNKCTL2 target speed; LTSSM on; L0 (state 0x0D..0x14) within 20 ms.
   On failure: the LTSSM's 32-entry history is printed, PERST# low, LTSSM off.
5. **Enumerate**: iATU region 1 = type-0 config of bus 1 at 0x127FE000, region 0 = memory window; the bridge window;
   1:00.0's ID and class (must be Broadcom); every BAR sized and placed in the window.
6. **Decode**: memory decoding and bus mastering on; the data path's gates (master port, XIU, **SysMMU clock**,
   PPMU, BTM, ACE bridge, SYSREG) forced on if closed (the SysMMU's always); SYSREG shareability said.
7. **Chip**: chipcommon's ID through BAR0's window (config 0x80), the enumeration ROM (cores, wrappers), the ARM
   CR4 halted and the 802.11 cores disabled, the **watchdog reset** (100 ms, as brcmfmac), the CR4's TCM banks →
   RAM size, BAR1's window fix (config 0x4E0), the module's CIS from OTP → the NVRAM's name.
8. **Firmware**: the files read (firmware, NVRAM, CLM); the NVRAM processed (key=value\0..., padded, length
   token; the phone's MAC put in if given); the firmware written at RAM base 0x170000 and read back, the last word of
   RAM zeroed, the NVRAM at the end of RAM, the ARM released from the reset vector (first word of the image,
   written at TCM 0). The firmware replaces the last word with its **shared area** address within 5 s.
   Shared area: protocol version (5..7), flags (DMA indices offered? host-ready doorbell? D2H sequence numbers?),
   max rx buffers, rx data offset, the mailbox data words, the ring info, the console.
9. **Rings**: the five common rings in host memory (control submit 64 x 40 B, rx post 1024 x 32, control complete
   64 x 24, tx complete 1024 x 24 (16 before v7), rx complete 1024 x 40 (32)), their addresses and sizes written
   into the chip's ring descriptors; indices in the chip's RAM (default) or in host memory by DMA
   (`dma_indices=1`); scratch and ring-update buffers; mailbox interrupts unmasked; host ready; 8 ioctl response
   buffers, 8 event buffers (8 KiB each) and N rx buffers (2 KiB) posted. Then the first ioctls: `cur_etheraddr`
   (get; set to the phone's MAC if given), `ver`, and the CLM blob in 1400-byte `clmload` chunks, `clmload_status`.
10. **Up**: `event_msgs` (join, auth, link, IF, escan result), `mpc` 0, `C_UP`, an `escan` on all channels:
   the v2 parameters when the firmware answers `scan_ver`, else v1 (brcmfmac's rule). Results arrive as events.

The way back (`bcmw_quiesce`, which the caller calls, also after a failed bring-up): INTx off, PERST# low (stops
the chip's DMA), then, if this run powered the PHY, the LTSSM off, the PHY powered down and the NCLK guard on
(Samsung's `exynos_pcie_poweroff`: TRSV 0xFE, CMN 0xC1, PCS +0x100 bit 6, +0x104 bit 7, ELBI L12ERR bit 2), then
WLAN_REG_ON low. Configuration space is not touched then, so a dead link cannot hang it. In the test image a crash
anywhere also drops PERST# and WLAN_REG_ON (libsb's new `sb_crash_hook`).

## 5. The core

`starbite/wifi/` (GPL-2.0-only; the files adapted from brcmfmac are ISC with Broadcom's notice):

| File | What |
|---|---|
| `bcmw.h` | The API and the OS interface (`struct bcmw_os`) |
| `exynos_pcie.c` | Steps 1..6, INTx, the way back, **the write allow-list** (`bcmw_write_allowed`) |
| `chip.c` | BAR0 window, EROM, AI core reset/disable, CR4 passive/active, RAM size, watchdog reset, OTP CIS |
| `fw.c` | NVRAM, module table, firmware banner, CLM chunks, iovars, event frames (pure functions) |
| `ring.c` | Ring index arithmetic and DHD's H2D stamping |
| `msgbuf.c` | Shared area, ring setup, posting, completions, ioctls, mailbox data, console |
| `core.c` | The steps, poll/isr/quiesce, ioctl/iovar helpers, a log-line builder (no C library) |
| `ext4.c`, `erofs.c` | Read-only readers for VENDOR (any OS can use them on raw partition bytes) |
| `sb/bcmw_sb.c` | The libsb shim |
| `host/` | The host test: `sh starbite/wifi/host/run.sh [dir-with-the-files]` |

What the OS gives (`struct bcmw_os`): 32/16-bit register reads and writes by physical address, optional
`probe32` (a read that survives a bus fault), a store barrier, `udelay` and a microsecond clock, DMA memory
(contiguous, below 4 GiB, uncached unless `coherent`), cache maintenance (if cached), a file by name, a log line,
an event callback and a received-frame callback, and three barriers (`wmb`, `mb`, `rmb`). The core needs 64 KiB
for its state (mostly the NVRAM buffer) and about 370 KiB of DMA memory with 64 rx buffers, asked for once at
step 9 and reused by a later bring-up.

Safety in the core:
- Every register write goes through `bcmw_write_allowed()`: the PCIe block's own pages, the endpoint's memory
  window and config window, and named single words (PMU 0x1406071C, SYSREG_FSYS1 +0x1044/+0x1050/+0x700, CMU_TOP
  +0x2074, 15 CMU_FSYS1 gate words, the WLAN_REG_ON and PERST#/CLKREQ# GPIO words). Anything else stops the core
  ("REFUSED", `dead`). No MIF/INT clock, no PMIC, no storage, no SysMMU register, no RECOVERY. The libsb shim
  and the Windows driver check the list again before each write.
- Every address the firmware hands over (shared area, ring info, ring memory, index arrays, mailbox words,
  console) must lie in its RAM, and every access through the RAM window is bounded by the window: otherwise the
  core stops once (`BCMW_E_PROTO`) rather than read or write elsewhere. BAR0 accesses stay inside its 4 KiB.
- Ordering: a full barrier before every ring index the host writes, a read barrier after every index it reads
  (DMA memory is Normal non-cacheable, the chip's RAM Device memory; Linux's `iowrite16`/`readw` carry the same
  barriers for brcmfmac). H2D items are stamped before the ring's cache clean.
- The firmware, NVRAM and CLM must match the SHA-256 of one of the phone's known files (13.50.37 / 13.50.28 /
  B0, their NVRAMs and CLMs), so a file reader's mistake never reaches the chip; `any_files=1` lifts this.
- DMA memory above 4 GiB is refused. Ring indices past the ring's end are ignored. A late ioctl answer (after a
  timeout) is dropped and its buffer reposted, not taken for the next request. A mailbox reading all ones (link
  gone) or a firmware halt stops the core.
- The MAC address is never logged, not even its first half.

The host test (`host/run.sh`; 278 checks with S9 #1's files on the Mac and in the VM, 312 with the older vendor
set and Linux's parser):
- NVRAM: all `nvram.txt_*` files of a directory, against Linux's parser byte for byte with
  `BCMW_LINUX=<brcmfmac dir>` (10 files of the phone's two vendor releases: identical); synthetic edge cases.
- The module table, iovars, clmload chunk flags, event frames, the allow-list (forbidden and allowed addresses).
- The ring: three passes with DHD's stamping, full, cancel, wrap, nonsense indices.
- The whole bring-up on `host/model.c`, a register-level model of the RC and the chip with a firmware that checks
  the download (reset vector, image, NVRAM token), refuses unstamped items, checks the clmload chunks and answers
  ioctls, events, a scan and a data frame: steps 1..10 with indices in the chip's RAM and by DMA, each step on its
  own, the power sequence's order and delays (REG_ON low 50 ms, PERST# 200 ms after REG_ON, the PMU before any
  CMU_FSYS1 access, the LTSSM after PERST#), no SysMMU access, no MAC in the log, buffers reposted (20 ioctls
  after the bring-up), both escan versions, a late ioctl answer dropped, the PHY powered down on the way back, a
  second bring-up after `quiesce` without new DMA memory, a shared area pointing outside the RAM refused.
- ext4 on mke2fs images (4 KiB extents + 64-bit, 1 KiB block maps with double indirect, 4 KiB block maps); EROFS
  on mkfs.erofs images (lz4hc compact, lz4 full indexes, uncompressed) and, in the VM, on S9 #1's VENDOR image
  (`BCMW_VENDOR_IMG=/mnt/oneui/imgs/vendor.raw BCMW_VENDOR_REF=<fsck.erofs --extract>`).
- The core also compiles freestanding (`-ffreestanding -fno-builtin`), with no C library calls.

What the host test cannot show: the real PHY and link, real timing, memory ordering (the model has none), whether
the chip accepts our ring descriptors and stamping, DMA through the real SysMMU path.

## 6. Open points (each is checked by a phone test, section 9)

1. Gen2 (an earlier port) against Gen1 (mainline): Gen2 first; `wifi_gen=1` if training fails.
2. The gate reads: the core reads CMU_FSYS1 only after the PMU and through `sb_probe32`; if the first look
   faults, the log says so and the core stops.
3. The module ID of S9 #1, so the right NVRAM: printed at step 7. Without one, the first B2 NVRAM present is taken
   and called a guess.
4. Escan v1 vs v2: chosen by `scan_ver` as brcmfmac does; both are host-tested, neither has run on this firmware.
5. Interrupts: polling first; INTx (SPI 253) when the OS has interrupts.
6. Deep sleep (`D2H_DEV_DS_ENTER_REQ`) is acknowledged as brcmfmac does; power saving is not set yet.

## 7. Windows 11

Windows cannot use this radio on its own: the Exynos PCIe controller is not ECAM (configuration space is reached
through the iATU's 4 KiB viewport, one function at a time), the link must be trained by software after the chip is
powered by a GPIO, and the ACPI tables of our UEFI (`ref/exynos9810-woa/.../AcpiTables`) describe no PCIe host. So
the driver runs the controller itself and Windows sees one ACPI device, not a PCI bus.

**Device.** A DSDT entry in our UEFI, e.g. `\_SB.WLAN` with `_HID "STBW4361"`, `_CCA 0` (non-coherent DMA),
`_CRS`: memory ELBI/PCS/PHY/DBI/config (one 4 KiB page each), the 16 MiB memory window, the PMU, SYSREG, CMU_FSYS1
and GPIO pages (only the words in the allow-list are written), and `Interrupt(ResourceConsumer, Level,
ActiveHigh, Exclusive) {285}`. Until the UEFI change lands, a root-enumerated test driver can map the same
physical pages (testsigning).

**Driver.** `SbWifi.sys`, KMDF, built with `tools/win` (`build.sh kmdf`), signed with the Starbite test
certificate. Its `struct bcmw_os`:

| bcmw_os | Windows |
|---|---|
| rd32/wr32/rd16/wr16 | `MmMapIoSpaceEx(PAGE_READWRITE \| PAGE_NOCACHE)` once per page, `READ/WRITE_REGISTER_ULONG/USHORT` |
| probe32 | none (the core then reads directly; the gates are read only after the PMU) |
| wmb | `KeMemoryBarrier()` (dsb) |
| udelay / now_us | `KeStallExecutionProcessor` (short), `KeDelayExecutionThread` for the ms waits at PASSIVE; `KeQueryInterruptTimePrecise` / 10 |
| dma_alloc | `MmAllocateContiguousNodeMemory` (or `MmAllocateContiguousMemorySpecifyCache`) with `MmNonCached`, highest 0xFFFFFFFF; bus address = `MmGetPhysicalAddress` (no IOMMU on this path: SysMMU in bypass). Later a WDF common buffer if Windows' DMA remapping must know |
| file | `ZwCreateFile`/`ZwReadFile` from `\SystemRoot\System32\drivers\StarbiteWifi\` (below) |
| log | `DbgPrintEx` + an in-memory ring a user tool reads (IOCTL), as the GPU session's |
| interrupts | `WdfInterruptCreate` on GSIV 285; ISR = `bcmw_isr()`, DPC = `bcmw_poll()` |

**Files.** Windows cannot read EROFS or ext4. The driver package carries no Broadcom file. A small tool
(`sbwifi-fetch.exe`, user mode, run once as administrator on the phone) opens the phone's own disk raw
(`\\.\PhysicalDrive0`, the GPT entry named VENDOR), reads the three files with `ext4.c`/`erofs.c` (the same code),
and the MAC from EFS (`/efs/wifi/.mac.info`, read only), and writes them to
`C:\Windows\System32\drivers\StarbiteWifi\`. They never leave the phone. (Alternative later: our UEFI reads them
at boot and hands them over in memory.)

**Network stack, in stages:**

| Stage | What | Done when |
|---|---|---|
| W1 | The driver loads, maps the hardware, runs bcmw to step 9 from a registry value (`Step`, default 0 = nothing), logs | The firmware's `ver` in the log under Windows |
| W2 | INTx by WdfInterrupt, a scan by a test IOCTL | Access points counted under Windows |
| W3 | **WiFiCx** client (`km/wificx` headers in the kit; NetAdapterCx underneath): the WDI command set mapped onto brcmfmac's iovars (scan → escan, connect → `wsec`/`wpa_auth`/`join` or `SET_SSID`, keys → `wsec_key`), the host's 802.1X/WPA supplicant (Windows does the four-way handshake; the firmware has no supplicant), NetAdapter Tx/Rx queues on flow rings and the rx completion ring | Settings lists networks and joins WPA2 |
| W4 | Power: PM2/`mpc`, D3 on idle, the radio off when Wi-Fi is turned off | Battery cost measured |

Data path (W3): tx needs flow rings (`FLOW_RING_CREATE` per destination/priority, 512 x 48-byte items,
`TX_POST` with the Ethernet header in the item and the payload by address); the core has the ring code, the
flow-ring messages are the next core step. Before WiFiCx, a fallback that gives Windows networking earlier: an
"Ethernet" NetAdapter whose link comes up when the driver has joined a network named in the registry, with the
four-way handshake in the driver (an earlier port's `lx_wpa.c`, MIT, can be ported).

## 8. Starbite OS

The shim is `starbite/wifi/sb/bcmw_sb.c`; `starbite/wifitest/` is a test image on it. For Starbite OS proper
(to agree with the `starbite-os` session through the main session):

- **Memory**: a dedicated DMA carve-out in the OS's memory map, non-cacheable, below 4 GiB, about 1 MiB (more
  with more rx buffers or flow rings). The test image borrows 0xa6000000..0xa6800000 of libsb's RAM-test window.
- **Interrupts**: SPI 253 (INTID 285) level; the handler calls `bcmw_isr()`, a task calls `bcmw_poll()`. Until
  then, polling every few ms (as an earlier port did).
- **Files**: VENDOR read at the first Wi-Fi use (ext4 or EROFS), kept in RAM (about 870 KiB) or not at all after
  the firmware is up (the chip keeps it).
- **Network**: the flow rings and frames (as for Windows), the four-way handshake (port an earlier port's `lx_wpa.c`, MIT),
  then a small IPv4 stack (ARP, DHCP, UDP, DNS, TCP) or lwIP (BSD). The app API: scan, join, status, sockets.
- **Power**: the radio off (PERST# low, WLAN_REG_ON low) unless an app needs it; `quiesce` on suspend and on the
  way back to the loader; PM2 while associated.
- **Safety**: the bring-up writes only the allow-list. libsb's rule that register pokes need a confirmed
  authorisation: the test image asks for a held Volume Up; Starbite OS needs a policy decision (the radio as a
  system service the user turned on, like the flashlight).

## 9. The phone tests

All on S9 #1 by the main session; the image is `starbite/out/wifitest.img` (`sh starbite/wifitest/build.sh` in the
VM), an OS-store entry with catalog key `wifitest` and a cmdline (read by the image from its own catalog row:
the loader does not pass a bare entry's cmdline on). The image writes nothing to storage; it reads
VENDOR (read only). The user holds Volume Up 2 s to start (or `wifi_auto=1` when the user agreed to an
unattended run); at the end the radio is unpowered and Power reboots to the menu (`wifi_return=S` does it after S
seconds; 30 s by default with `wifi_auto=1`). If VENDOR cannot be read, the run stops after step 7. Every line is
on the screen and in pmsg (TWRP: `/sys/fs/pstore/pmsg-ramoops-0`); the firmware's console is cut after 48 lines
so the 16 KiB pmsg ring keeps the start.

| Test | cmdline | Expected | If not |
|---|---|---|---|
| T1 link | `wifi_step=6` | `pcie: 4: LINK UP after N us: Gen2 x1`, `1:00.0 is 14e4:441f (Broadcom)`, BAR0 32 KiB, BAR2 4096 KiB, `END: reached step 6` | no link: the LTSSM history in the log; again with `wifi_gen=1`. A gate fault: the log says which read |
| T2 firmware | `wifi_step=9` | `chip 4361 revision 5`, the cores, `RAM ... KiB at 170000`, `the module's ID xx 33 (semco)`, `VENDOR is EROFS`, `read from VENDOR: /firmware/...`, `Firmware ... 13.50.37`, `the firmware is up after N ms`, `protocol version 6 or 7`, `Firmware: wl0: ... 13.50.37`, `CLM loaded` | `TIMEOUT: no answer to ioctl`: DMA does not reach memory (the SysMMU path; try `coherent`). BADOPTION: the stamping |
| T3 scan | `wifi_step=10` | `scan_ver ...`, `up: 0 (fine)`, `a scan asked for`, `the scan heard N access points (complete)` | `escan` refused: the other parameter version |

Risks: the radio and the PCIe block are powered and clocked by this image (as Android, an earlier port and Linux do). The
chip is only given addresses in the image's DMA window (uncached, 0xa6000000..0xa6800000); with the SysMMU in
bypass nothing enforces that beyond the protocol being right. A fault stops the image with the radio unpowered
(the crash hook), and a reboot clears everything (WLAN_REG_ON is a GPIO and falls with the SoC). Nothing touches EFS, PARAM, the boot LUNs,
RECOVERY, MIF/INT clocks or the PMIC. Undo: reboot.

### Run 1 (2026-11-07, launch #56, image 60f3d908)

T1 passed and T2 reached step 8: PHY PLL locked, **LINK UP after 70 us: Gen1 x1** (Gen2 was asked for; the mainline
port also links at Gen1), 14e4:441f class 028000 rev 05, BAR0 32 KiB at 0x11800000, BAR2 4 MiB at 0x11c00000; chip
4361 rev 5, every core found, watchdog reset, **RAM 1344 KiB** at 0x170000; **module ID a1 33 (SEMCO)** →
`nvram.txt_r02b_b2`; firmware 13.50.37 known. Then the NVRAM was refused: the known-sums table had a hand-typed
prefix wrong (`...c3af` for `...c2af`). The radio was unpowered on the way back; clean restart.
Fixed after run 1: the table is generated from full `sha256sum` output and compares all 32 bytes, and the host test
now requires every NVRAM and CLM file of the directory to be in it. The image also ran at the default step: the
loader enters bare images with S-Boot's FDT unchanged, so a catalog row's `cmdline` never reached `sb_bootarg`.
`wifitest` now reads its own catalog row (`cat_find("wifitest")`) first, then the bootargs, and logs the settings.

## 10. Review

An independent review (2026-11-07) read the code against brcmfmac, bcmdhd, Samsung's PCIe driver, the mainline
port and an earlier port. It confirmed the allow-list, the register sequences, the byte layouts and the stamping, and found
nine problems, all fixed the same day: no barriers around the ring indices (the most likely first-run failure:
stale items, ioctl timeouts), the PHY left running into an unpowered chip at the end, firmware-given addresses
not bounded, EROFS coverage not checked and its feature mask mislabelled, stamping after the cache clean, a late
ioctl answer taken for the next request, the test image waiting forever in unattended runs and leaving the radio
powered on a crash, two gates read late, and doc/comment mismatches.

## 11. Sources and licences

- Samsung's GPL kernel for the S9 (`ref/starlte-kernel`): `drivers/pci/host/pci-exynos*.c`, `bcmdhd_100_10`
  (`dhd_custom_exynos.c`, `dhd_custom_cis.c`, `dhd_msgbuf.c`, `bcmpcie.h`, `bcmmsgbuf.h`): facts and Samsung's PHY
  tables (GPL-2.0; bcmw's own files are GPL-2.0-only).
- Linux brcmfmac (`drivers/net/wireless/broadcom/brcm80211/brcmfmac`, Linux 7.1, ISC, Broadcom): `chip.c`,
  `ring.c`, `msgbuf.c` are adaptations and keep the ISC notice; `fw.c` reproduces the NVRAM parser's behaviour.
- Linux EROFS (`fs/erofs`, GPL-2.0): the on-disk format and the compact index decoding.
- SHA-256 per FIPS 180-4, written here.
- an earlier port (`build/refport/hackios-xnu-kernel`): `drivers/` is APSL 2.0, the rest MIT. Read for the sequence
  and the lessons; no code copied.
- The mainline port `pcie-exynos9810.c` (GPL-2.0, SheroAbi): read for the differences in section 3.
- Firmware, NVRAM and CLM are Broadcom's and Samsung's: read from the phone's VENDOR at run time, never committed.
