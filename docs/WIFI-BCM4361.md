# Wi-Fi on the Galaxy S9: the BCM4361 for Windows 11 and Starbite OS

Track C of `docs/PLAN-2026-11-07.md`. One portable C core (`starbite/wifi/`, "bcmw") that brings up the S9's
Broadcom BCM4361 by itself: the Exynos 9810's PCIe controller, the chip, its firmware, brcmfmac's msgbuf protocol.
Windows gets it through a KMDF driver with WiFiCx on top; Starbite OS through a libsb shim.

Status 2026-11-07:

| Part | State |
|---|---|
| Core (`starbite/wifi/`: 3,600 lines, plus 870 for the VENDOR readers and the libsb shim) | **Works on S9 #1 to a scan** (2026-11-07, runs 2 and 3: firmware up, CLM loaded, 29 access points heard). Reviewed adversarially (section 10), host-tested on a register-level model |
| NVRAM processing | Byte-identical to Linux's brcmfmac on all 10 of the phone's NVRAM files |
| Reading the files from VENDOR | ext4 (LineageOS) and EROFS (One UI 8, S9 #1 now). The EROFS reader reads all 13 radio files from a copy of S9 #1's own VENDOR image, byte-identical |
| libsb shim + test image `wifitest.img` | T1, T2, T3 passed on the phone (section 9) |
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

## 4b. The data path and joining (link.c, wpa.c)

After step 10 the core can send and receive frames and join a network:

- **Tx:** one flow ring for everything (brcmfmac makes one per priority for a station; here all goes as best
  effort, fifo 1): `FLOW_RING_CREATE` on the control ring (512 items of 48 bytes, flow ring id 2, its indices in
  the slots after the two common H2D rings), answered by `FLOW_RING_CREATE_CMPLT`. A frame is a `TX_POST` item
  (the Ethernet header in the item, the payload from one of 32 host buffers of 2 KiB), answered by `TX_STATUS`.
  Flow ring items are stamped like every H2D item. `bcmw_tx()` copies the frame.
- **Rx:** as before, on the rx completion ring to `os->rx`; EAPOL (0x888e) goes to the handshake while joining.
- **Join** (`bcmw_join(ssid, passphrase)`): a directed escan (v1 or v2, as `scan_ver` says) picks the strongest
  BSS of that name whose RSN element offers WPA2-PSK with CCMP (WPA3-only, TKIP-only and open networks are
  refused unless the passphrase is empty, which asks for an open one); infrastructure, open-system
  authentication, `wsec` AES, `wpa_auth` WPA2-PSK; the PMK from the passphrase (PBKDF2; 64 hex digits are taken
  as the PSK itself); then either
  - the **host's supplicant**, the default (an earlier port's way on this phone; Samsung builds the firmware without
    `BCMSUP_4WAY_HANDSHAKE`): our RSN element as `wpaie` (`sup_wpa` untouched: brcmfmac and bcmdhd set it only
    for a firmware supplicant, and a firmware without one may not know it), the four-way handshake by `wpa.c`
    (an earlier port's `lx_wpa.c`, MIT) on EAPOL frames from the chosen AP only, its answers sent from the join loop,
    message 4 out before the keys go in (`wsec_key`: pairwise index 0 with the AP's address, flags 0; the
    group key primary, as `brcmf_cfg80211_add_key`); group-key renewals later answered from `bcmw_poll`; or
  - the **firmware's supplicant**, opt-in (`fw_supplicant`, wifitest `wifi_fwsup=1`) when `cap` lists `idsup`
    (`sup_wpa` 1, `WLC_SET_WSEC_PMK` with the 32-byte PMK, done at `E_PSK_SUP` status 6), as the plan asked;
    not used until the host path has worked on the phone.
  A step-10 scan still running is waited for (escan events are told apart by their sync id; the firmware ends
  an old scan with status 9 when a new one starts), a refused escan is retried. Roaming is off (`roam_off`: the
  host holds one AP's keys). A failed join is left (`DISASSOC` with a 12-byte `scb_val`).
  Which one the firmware has is only known from the phone (its iovar tables are in the chip's ROM: the image has
  no names); the log says which was used. `WLC_SET_SSID` with the name and BSSID; `SET_SSID`, `LINK`, `DEAUTH`,
  `DISASSOC`, `PSK_SUP` events. `bcmw_leave()` disassociates.
- Nothing secret is logged (passphrase, PMK, keys), nor a network's name; of the AP's address only its maker's
  half. The PMK is wiped after the join.
- The nonce: `os->random`; the libsb and Windows shims do not give one yet, so the core's fallback (clock jitter,
  hashed) is used. Good enough for a test; a real source is listed in section 6.

Host test: the model's access point (`host/model_ap.c`, its own AES encryption and RFC 3394 key wrap checked
against the RFC) answers a directed scan with an RSN element and plays the authenticator: message 1, message 2's
MIC and RSN element checked, message 3 with the GTK wrapped, message 4's MIC; the keys the host installs must be
its TK and GTK. Both supplicant paths, a wrong passphrase on each, 40 data frames out. PBKDF2 against the IEEE
vectors, the PRF against Python's `hmac`.

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
7. The firmware's supplicant (`wifi_fwsup=1`) is untried; the host's is the default (T4).
8. A real random source for the handshake's nonce in the shims (libsb: e.g. the TRNG if reachable from EL1;
   Windows: `BCryptGenRandom`).
6. Deep sleep (`D2H_DEV_DS_ENTER_REQ`) is acknowledged as brcmfmac does; power saving is not set yet.

## 7. Windows 11

Windows cannot use this radio on its own: the Exynos PCIe controller is not ECAM (configuration space is reached
through the iATU's 4 KiB viewport, one function at a time), the link must be trained by software after the chip is
powered by a GPIO, and the ACPI tables of our UEFI (`ref/exynos9810-woa/.../AcpiTables`) describe no PCIe host. So
the driver runs the controller itself and Windows sees one ACPI device, not a PCI bus.

**Device.** `starbite/wifi/win/wlan.asl`: an SSDT (OEM `STRBTE`, table `WLAN4361`) with `\_SB.WLAN`, `_HID`/`_CID`
`STBW4361`, `_UID 0` (instance `ACPI\STBW4361\0`), `_CCA 0` (non-coherent DMA), `_CRS`: the PCIe block's
ELBI/PCS/PHY/DBI pages, the endpoint's BAR window (0x11800000, 15 MiB), the configuration window and
`Interrupt(Level, ActiveHigh, Exclusive) {285}`. The PMU, SYSREG, CMU_FSYS1 and GPIO words are mapped by the driver
(single words of pages that belong to other DSDT devices). Installed as a plain blob by the USB session's
`Star2LteSsdtDxe`.

**Drivers** (both built on the Mac with `tools/win`, signed with the Starbite test certificate):

| | W1 `sbwifi.sys` (`build.sh`) | W2 `sbwificx.sys` (`build-cx.sh`) |
|---|---|---|
| Kind | one-shot KMDF service, no device | KMDF + NetAdapterCx 2.1 + WiFiCx 1.0 client on `ACPI\STBW4361` |
| Does | bring-up to `Parameters\Step` (default 10: a scan), logs, radio off | bring-up in PrepareHardware, WDI commands, data path: Windows shows Wi-Fi, scans, joins |
| Log | `C:\starbite\sbwifi.txt` | `C:\starbite\sbwificx.txt` (every WDI command id once; no names, keys or addresses) |
| Install | `hive-service.py add sbwifi` | `make-package.sh` package + the one-shot `sbpnp` service (pnputil) |

Their `struct bcmw_os`:

| bcmw_os | Windows |
|---|---|
| rd32/wr32/rd16/wr16 | `MmMapIoSpaceEx(PAGE_READWRITE \| PAGE_NOCACHE)` once per 64 KiB, `READ/WRITE_REGISTER_*`; writes only through the core's allow-list |
| probe32 | none (the core reads the gates only after the PMU step) |
| wmb/mb/rmb | `KeMemoryBarrier()` |
| udelay / now_us | `KeStallExecutionProcessor` (< 1 ms), `KeDelayExecutionThread`; `KeQueryInterruptTimePrecise` / 10 |
| dma_alloc | `MmAllocateContiguousMemorySpecifyCache(MmNonCached)` below 4 GiB, bus address = `MmGetPhysicalAddress` (SysMMU in bypass) |
| file | `\SystemRoot\System32\drivers\starbite\bcm4361\` (below) |
| random | `BCryptGenRandom` (system RNG) |
| interrupts | none yet: the worker polls every 5 ms (W3: WdfInterrupt on GSIV 285) |

**Files.** Windows cannot read EROFS. The packages carry no Broadcom file: the firmware, NVRAM and CLM are copied
offline from the phone's own VENDOR into `C:\Windows\System32\drivers\starbite\bcm4361\` (never committed,
never leave the phone). The MAC is the firmware's (`cur_etheraddr`, from the OTP), never logged.

**SbWifiCx design** (`starbite/wifi/win/sbwificx.cpp`):
- One worker thread owns the core: every `bcmw_*` call happens there at PASSIVE. It takes WDI requests queued by
  `EvtWifiDeviceSendCommand`, frames queued by the tx queue, calls `bcmw_poll`, and turns what the events said
  (scan done, associated, link lost) into WDI indications. The core's callbacks (`os->event`, `os->rx`) only
  record and queue.
- WDI: `OPEN`/`CLOSE`/`DOT11_RESET` answered at once; `SCAN` → `bcmw_scan_start` (escan, one directed name at
  most), every result kept (beacon body rebuilt from the escan's fixed fields + IEs) and given as one
  `BSS_ENTRY_LIST` + `SCAN_COMPLETE`; `CONNECT` (open or WPA2-PSK, CCMP; TKIP group) → `bcmw_connect`
  (`wsec`/`wpa_auth`/`wpaie` + `SET_SSID`), `ASSOCIATION_RESULT` with the request/response IEs from `assoc_info`;
  `ADD_CIPHER_KEYS` → `bcmw_set_key` (pairwise/group, with the RSC); `DISCONNECT` → `bcmw_leave`. Everything else
  is answered NOT_SUPPORTED (or success for the harmless sets) and its id logged once.
- Windows' own supplicant does the four-way handshake: EAPOL frames go through the data path (the core passes
  them to `os->rx` when it is not joining by itself).
- Data path: 802.3 frames, NetAdapterCx tx/rx queues copied to and from 64-frame rings; tx only while associated.
- Boot guard: `Services\sbwificx\Parameters\GuardBoots` counts starts without a clean power-down; at 2 the driver
  leaves the hardware alone (Windows boots without Wi-Fi) until it is set to 0. `Parameters\Off = 1` also.

| Stage | What | State |
|---|---|---|
| W1 | `sbwifi.sys`: the bring-up under Windows up to a scan | **passed** (W1b, 2026-11-07: 23 access points) |
| W2 | `sbwificx.sys`: WiFiCx client, Windows lists networks and joins WPA2 | **open networks pass** (W2l-a, 2026-11-07); WPA2 next |
| W3 | interrupts (WdfInterrupt), roaming, statistics, WPA3-SAE | after W2 |
| W4 | Power: PM2/`mpc`, D3 on idle, the radio off when Wi-Fi is turned off | radio switch done (W2o: WDI_TASK_SET_RADIO_STATE, WLC_DOWN/UP); PM2 and D3 open |

## 8. Starbite OS

The shim is `starbite/wifi/sb/bcmw_sb.c`; `starbite/wifitest/` is a test image on it. For Starbite OS proper
(to agree with the `starbite-os` session through the main session):

- **Memory**: a dedicated DMA carve-out in the OS's memory map, non-cacheable, below 4 GiB, about 1 MiB (more
  with more rx buffers or flow rings). The test image borrows 0xa6000000..0xa6800000 of libsb's RAM-test window.
- **Interrupts**: SPI 253 (INTID 285) level; the handler calls `bcmw_isr()`, a task calls `bcmw_poll()`. Until
  then, polling every few ms (as an earlier port did).
- **Files**: VENDOR read at the first Wi-Fi use (ext4 or EROFS), kept in RAM (about 870 KiB) or not at all after
  the firmware is up (the chip keeps it).
- **Network**: the flow rings and frames (as for Windows), the four-way handshake (`wpa.c`, from an earlier port's `lx_wpa.c`,
  MIT), then **sbnet** (`net.c`/`net.h`, 2026-11-07, MIT, freestanding): DHCP with renewal/rebinding/NAK/expiry,
  ARP (8-entry cache, one packet held per resolution), ICMP echo both ways, UDP with a receive callback, DNS A
  queries. Host test `host/net-test.c` on a simulated LAN (31 checks, 25,000 mangled frames under ASan/UBSan).
  TCP is not there yet. The app API: scan, join, status, sockets. `wifitest` step 11 uses sbnet: lease, three
  pings to the router, a DNS lookup (test T4-open).
- **Power**: the radio off (PERST# low, WLAN_REG_ON low) unless an app needs it; `quiesce` on suspend and on the
  way back to the loader; PM2 while associated.
- **Safety**: the bring-up writes only the allow-list. libsb's rule that register pokes need a confirmed
  authorisation: the test image asks for a held Volume Up; Starbite OS needs a policy decision (the radio as a
  system service the user turned on, like the flashlight).

## 9. The phone tests

All on S9 #1 by the main session; the image is `starbite/out/wifitest.img` (`sh starbite/wifitest/build.sh` in the
VM), OS-store entries `wifitest` / `wifitest-fw` / `wifitest-scan` sharing the image, each with its cmdline
(read by the image from the launched row: the loader does not pass a bare entry's cmdline on). The image writes nothing to storage; it reads
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

**T4 join** (`wifi_step=11`): needs the store entry `wifinet` (two lines: the network's name, its passphrase;
made on the phone from the user's input, never committed). Expected: `link: joining a network ... WPA2-PSK, the
host's supplicant`, `the access point xx:xx:xx:..:..:..`, `LINK UP`, `handshake message 1 ... message 3 ... both
keys given to the firmware (keyed)`, `THE NETWORK IS JOINED`, `A DHCP OFFER CAME BACK`. If not:
`no suitable access point` (the name, or a WPA3-only network), `a handshake message refused: its signature is not
right` (the passphrase), `TIMEOUT: associated, but the handshake did not finish` (no EAPOL reached us: then a run
with `wifi_fwsup=1`).

### Run 1 (2026-11-07, launch #56, image 60f3d908)

T1 passed and T2 reached step 8: PHY PLL locked, **LINK UP after 70 us: Gen1 x1** (Gen2 was asked for; the mainline
port also links at Gen1), 14e4:441f class 028000 rev 05, BAR0 32 KiB at 0x11800000, BAR2 4 MiB at 0x11c00000; chip
4361 rev 5, every core found, watchdog reset, **RAM 1344 KiB** at 0x170000; **module ID a1 33 (SEMCO)** →
`nvram.txt_r02b_b2`; firmware 13.50.37 known. Then the NVRAM was refused: the known-sums table had a hand-typed
prefix wrong (`...c3af` for `...c2af`). The radio was unpowered on the way back; clean restart.
Fixed after run 1: the table is generated from full `sha256sum` output and compares all 32 bytes, and the host test
now requires every NVRAM and CLM file of the directory to be in it. The image also ran at the default step: the
loader enters bare images with S-Boot's FDT unchanged, so a catalog row's `cmdline` never reached `sb_bootarg`.
`wifitest` now reads the cmdline of the catalog row the loader launched (several hidden rows share the image:
`wifitest` step 6, `wifitest-fw` 9, `wifitest-scan` 10). The key comes from the loader's launch record (block 0
of the store's `crashlog`: "SBCRASH1", CRC, state running, method bare), else the last `STARBITE: launch <key>`
line in pmsg, else `wifitest`, and only a key starting with `wifitest` is trusted; then the bootargs. The first
lines say `wifitest: settings: <key>: <cmdline>` and where the key came from.

### Runs 2 and 3 (2026-11-07, launches #59 and #60, image 57148abf, commit 5115467): T2 and T3 passed

- Settings from the loader's launch record (`wifitest-fw: wifi_step=9`, `wifitest-scan: wifi_step=10`).
- Files from VENDOR (EROFS): firmware 13.50.37 (e3c66440...), `nvram.txt_r02b_b2` (68cb9039..., 7723 bytes of
  text, 5544 for the chip, boardrev 0x1101), CLM 1c147786...; all known.
- **The firmware up after 150 ms**; shared area 0x2012e4, flags 0x30040007 (protocol 7, D2H XOR-checksum sync,
  host-ready support, no DMA indices), max rx buffers 511, console 2 KiB; ring info 0x244b18, 40 flow rings.
- Console: `wl1: Broadcom BCM4361 802.11 Wireless Controller 13.50.37 (B2 Network/rsdb)`, ThreadX v5.6, some
  `MALLOC warning` lines and `dngl_sendpkt: dropped pkt` (at boot, before the host has posted anything, and once
  after the first commands), `Hostready Enabled?N`.
- First ioctls answered: `cur_etheraddr`, `ver` = `wl0: Aug  9 2021 21:48:22 version 13.50.37 (B2 Network/rsdb)
  FWID 01-c47464bc`; CLM loaded in 1400-byte chunks, `clmload_status` 0.
- T3: `scan_ver` unsupported → **escan v1** accepted; 32 events, **29 access points heard, scan complete**; the
  radio unpowered at the end, clean restart. (Network names and addresses are not logged.)
- So on this phone: link at Gen1 with Samsung's tables, DMA through the SysMMU's bypass with uncached memory,
  ring indices in the chip's RAM, DHD's stamping (no BADOPTION), polling.

Open from these runs: `OTP Control 1` reads back 0 after the write (the CIS was read anyway); the
`dngl_sendpkt: dropped pkt` lines (probably events with no event buffer posted yet: to check once events matter
for joining); the D2H XOR-checksum sync mode the firmware announces is not verified by the host (brcmfmac does
not either).

### Windows runs (2026-11-07): W1 failed at step 8, W1b PASSED

**W1** (`sbwifi.sys` d88e5098, plain service, radio files in `drivers\starbite\bcm4361`): steps 4-7 as on bare
metal (LINK UP Gen1 x1, 14e4:441f, chip 4361 rev 5, module a1 33), the download "read back" (its first 64 bytes),
then `the firmware did not start in 5 s (no shared area)`. Cause, in the Windows glue only: registers were mapped
64 KiB at a time into a 24-slot table, and a full table dropped accesses silently. The download needs ~22 slots of
BAR1 alone, so the end of the RAM (the NVRAM with it) was never written and the shared-area word read ffffffff
before and after the start. Also the clock used `KeQueryInterruptTimePrecise`'s QPC out-parameter (~1.5x fast).

**W1b** (`sbwifi.sys` 6c7bffc0, commit 1726559: 1 MiB mappings x 48, a full table logged, the right clock,
`cfg.diag`): `the firmware read back: 212825 words, 0 wrong`, `the NVRAM read back: 1386 words, 0 wrong`, CR4 ioctl
21 (halted) before the start and 01 after, the RAM's last word the NVRAM token `fa960569` at +73 ms and `002012e4`
at +154 ms: `the firmware is up after 160 ms` (bare metal 150 ms), protocol 7, `26 events; the scan heard 23 access
points (complete)`, step 10, clean restart. Under Windows every sleep rounds up to the 15.6 ms timer tick (W1's
"10 s" of 5 ms polls took 35 s; SbWifiCx's worker polls every ~16 ms); the full read-back costs 1.9 s.

### W2: SbWifiCx under Windows 11 (2026-11-07) — open networks WORK with W2l-a

Baseline: **W2l-a**, commit aa4a8ef (tag `wifi-w2l-a`), sbwificx.sys `4296f319…` (driver 0.3.1.0). On the open
network: AutoConfig connected, DHCP, gateway ping, DNS and HTTP 200 within 12 s of the connect. Found and fixed on
the way (each one blocked everything after it):

| Build | What was wrong | Evidence |
|---|---|---|
| W2b | the BSS list was indicated with the scan's transaction id; WDI defines it as unsolicited (id 0) | Windows listed 0 networks while the radio heard 10-13 |
| W2c | every scan used one escan sync id: a cancelled scan's late end event ended the next ones | scans 2 and 3 "complete" at once, empty |
| W2e | `assoc_info` asked with 8 bytes (brcmfmac: 512); the association's elements never reached Windows | "request elements 0 bytes" |
| W2j | the **association result** was sent with the connect task's id; the connect task lists it as unsolicited | AutoConfig 8002 "the specific network is not available" (0x2800B, RSSI 255) the moment it came, every attempt |
| W2l | WiFiCx's data path carries **802.11 frames** (24-byte header + LLC/SNAP), as WDI's did; the radio takes and gives Ethernet | Windows' first frame: 60 bytes (an ARP in 802.11 form), "ethertype" e1f5; Windows counted 0 bytes received |

Diagnostics that settled them: Windows' own `Microsoft-Windows-WLAN-AutoConfig/Operational` log (pulled offline),
the firmware's `sta_info` for the AP, the rx/tx ring state, and a per-join log of every firmware event and WDI
command. W2m (9545b0d) hardens the WPA2 key path (parsing in either WDI version, pairwise keys without an address,
IGTK accepted). W2m-b/W2n: 1 ms polling, 256 rx / 128 tx buffers, frames requeued instead of dropped when every tx
buffer is with the firmware (the open test network itself caps at ~4 Mbit/s: the host was idle). W2o (8bbdde7):
the radio switch (airplane mode). WPA2 is host-tested end to end on Windows' path (bcmw_connect, the handshake
outside the core through os->rx/bcmw_tx, keys through bcmw_set_key, a group-key renewal) against the model AP;
not yet on a real WPA2 network (needs the user's hotspot).

## 10. Review

An independent review (2026-11-07) read the code against brcmfmac, bcmdhd, Samsung's PCIe driver, the mainline
port and an earlier port. It confirmed the allow-list, the register sequences, the byte layouts and the stamping, and found
nine problems, all fixed the same day: no barriers around the ring indices (the most likely first-run failure:
stale items, ioctl timeouts), the PHY left running into an unpowered chip at the end, firmware-given addresses
not bounded, EROFS coverage not checked and its feature mask mislabelled, stamping after the cache clean, a late
ioctl answer taken for the next request, the test image waiting forever in unattended runs and leaving the radio
powered on a crash, two gates read late, and doc/comment mismatches.

A second review (2026-11-07) of the data path and join, before any phone run, found that the join's scan could be
ended by step 10's still-running scan (sync ids now told apart), that `sup_wpa 0` would fail the host path on a
firmware without a supplicant (no longer set), that the untried firmware supplicant was the default (now opt-in),
the flow ring's tid (0, best effort), no group-key renewals after the join (now from `bcmw_poll`) and roaming on
(off), no disconnect after a failed join, an IE-length overflow, EAPOL accepted from any sender, a DHCP option
read past the frame, and which key is the primary one (the model had made the same mistake). All fixed; the model
now refuses `sup_wpa` without `idsup`, ends the old scan first, and renews the group key.

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
