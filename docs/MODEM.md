# MODEM: the Shannon CP driver core ("sbm")

Starbite's OS-independent driver library for the Galaxy S9's integrated Samsung Shannon modem (SM-G960F,
Exynos 9810 CP, Samsung's "ss360ap"). It will serve iOS 16 and Starbite OS the way `bcmw` (the BCM4361 Wi-Fi
core, docs/WIFI-BCM4361.md) serves Windows and Starbite OS: one plain C core, host-tested against a fake device,
thin per-OS glue.

Code: `starbite/modem/`. Research it builds on: the shared notes `starbite-os-modem-research.md` (memory map,
mailbox, PMU/SMC boot, SBD/SIPC5, EFS over RFS, staged plan) and `ios16-cellular-latest.md`; Samsung's GPL kernel
in `ref/starlte-kernel` (`drivers/misc/modem_v1`, `drivers/misc/mcu_ipc`) as the interface reference; this
phone's own boot logs (`build/test-boots/*/dmesg*.txt`, `last_kmsg*.txt`) and RADIO image as ground truth. No code
is copied from Samsung, libsamsung-ipc or an earlier port.

## 0. Rules the code enforces

| Rule | How |
|---|---|
| Never dial anything (no emergency calls in tests) | `sbm_ipc_build` has an allow-list of 11 GET requests. The CALL, SMS, SS, SAT, IMEI and RFS groups are refused before the table is even consulted. There is no switch to add them. Host test: all 65536 × 256 command/type pairs, only the 11 GETs pass. The fake CP counts CALL frames and off-list requests it receives (must be 0). |
| IMEI / IMSI / numbers redacted in every log | `sbm_logf` masks every run of 7 or more decimal digits and every BCD-looking hex run (14+ characters, 10+ of them digits). `0x` literals are exempt: those are addresses and register values. `sbm_frame_line` prints payload bytes only for an allow-list of identifier-free messages; NET_REGIST, NET_SERVING_NETWORK (location), MISC_ME_SN/IMSI and NET_SUBSCRIBER_NUM are always `<n bytes>`. The decoders keep MCC-MNC at most, and only "present" for LAC/cell id. Test data uses the 001-01 test network and made-up numbers. |
| Only read RADIO / EFS / CPEFS, never write | The core has no block I/O at all: it parses caller buffers. The libsb glue has no store path. |
| No hardware access in stage 1 | Stage 1 is pure parsing and memory buffers. Stage 2's probe only reads, through `probe32`. |

## 1. Layout

```
starbite/modem/
  sbm.h          the whole API + this phone's fixed facts
  sbm_util.c     freestanding helpers (memcpy, a small printf, sbm_logf with redaction)
  sbm_toc.c      modem.bin TOC, the CP memory map ("MEM0"), the load plan
  sbm_sipc5.c    SIPC5 link frames (4/5/6-byte headers, padding, multi-frame reassembly)
  sbm_sbd.c      SBD ring layout (as the AP builds it) + frame put/get on a memory buffer
  sbm_ipc.c      Samsung IPC header codec, the request allow-list, names, GEN_PHONE_RES
  sbm_cell.c     SIM / network status decoders -> struct sbm_cell_state; redaction
  sbm_probe.c    stage 2: read-only PMU / mailbox / carve-out probe + report + verdict
  sb/sbm_sb.c    libsb glue (Starbite OS, test images): fault-safe reads incl. pending-SError check
  probe/         the stage 2 phone image (main.c, build.sh)
  host/          fakecp.c (fake CP + register model), test.c, lnk.c (decodes captured traffic), run.sh
```

The core is freestanding C99. `run.sh` builds it with `-ffreestanding` on the host, for bare aarch64 with
`-mstrict-align -mgeneral-regs-only` (as libsb builds), and as part of the probe image in the VM (gcc).

Per-OS glue still to write: iOS 16 needs a kernel-side driver. an earlier port's prebuilt kernel cannot load add-on
drivers, so this means building refport-kernel (the ios16-cellular note). The core's MIT option and its
no-libc, no-allocation shape are what make it usable there. Starbite OS uses `sb/sbm_sb.c` today.

## 2. modem.bin (RADIO), read only

TOC: 0x20-byte entries `{ char name[12]; u32 offset, loadaddr, size, crc, entryid }` from offset 0. Entry 0 is
`TOC` itself: its size is the header size, its entryid the number of entries after it. This phone (RADIO backup of
2026-11-05, 52428800 bytes):

| entry | file offset | CP load addr | size | goes to modem_if + |
|---|---|---|---|---|
| TOC | 0 | 0 | 0x200 | (5 entries follow) |
| BOOT | 0x200 | 0x40000000 | 0x1e40 | 0 |
| MAIN | 0x2040 | 0x40010000 | 0x23d8ea0 | 0x10000 |
| VSS | 0x23daee0 | 0x47900000 | 0x57cbf0 | 0x7900000 (the VSS region) |
| NV | 0 | 0x44900000 | 0x100000 | 0x4900000. The data is EFS's `nv_data.bin`, not RADIO. |
| OFFSET | 0x7aa00 | 0 | 0x85600 | none: 0x7aa00 + 0x85600 = 1 MB, so probably a range inside NV. Meaning unconfirmed. |

**The CP memory map** (`struct cp_reserved_map_table`, what Samsung's `shm_probe` reads at modem_if + 0x2a0) is
inside BOOT, at **RADIO offset 0x2a0** (BOOT + 0xa0). S-Boot leaves RADIO's first bytes at modem_if + 0, so the
same table sits at modem_if + 0x2a0 when Linux probes. Byte order, as found: the id is stored `00 'M' 'E' 'M'`
and +'0' gives "MEM0". Each tag is its name reversed with a trailing NUL (`"SSV\0"` = VSS). This phone:
dram 0x7900000, ext 0x1900000; VSS 0x7900000+0x700000, IPC 0x8000000+0x500000, ZMC 0x8500000+0xb00000,
VPA 0x9000000+0x200000, LOG 0x9200000+0x2000000. This matches the kernel's log exactly.

`sbm_plan_build` turns TOC + map into the copy list cbd performs, checked against the map: BOOT/MAIN/NV inside
cp_size, VSS inside the VSS region, no overlaps. It also returns boot_size/main_size, the arguments of the later
EL3 security request (`0:1e40:23d8ea0:0` on this phone). It copies nothing.

## 3. The IPC

### 3.1 SIPC5
`cfg` (0xF8 start + PADDING 0x04 / EXT 0x02 / CTL 0x01), channel, u16 length (header included). EXT|CTL = a
5-byte multi-frame header with `{id:7, more:1}`; EXT alone = a 6-byte header with a u32 length. Every frame this
phone logged is cfg 0xF8 (one SBD buffer per frame). FMT channels are 235/236, RFS 245, boot 215, dump 225,
PS 10-24.

### 3.2 SBD rings
`sbm_sbd_init` lays out the descriptor region exactly as this phone's kernel logged it. The host test compares
every offset: G_DESC 2048, RB_CH 2060, RB_DESC 2572 + 20 per descriptor, RBP set 3852, each ring's SBD vectors
and buffers (e.g. FMT1 DL vectors 4692, RFS DL buffers 557056, end 4276224). The 9 link channels in DT order:
cass(35), ipc0(235), ipc1(236), rfs0(245), csd(1), router(25), dm0(28), rmnet0(10), multipdp-hi(11).
Indices are u16 buffer indices with one empty slot. Every index, size and address the other side writes is
bounds-checked. A bad entry is skipped and counted, never followed. A fuzz test flips bits in the descriptor
region: `sbm_sbd_attach` either refuses the layout or every ring stays inside the region (ASan watches).

### 3.3 Samsung IPC (FMT)
`u16 length, u8 mseq, u8 aseq, u8 group, u8 index, u8 type`, then data. A response's aseq is the request's mseq.
AP types: EXEC 1, GET 2, SET 3, CFRM 4, EVENT 5. CP types: INDI 1, RESP 2, NOTI 3.

**Correction to the research note's group table:** on this phone's traffic, group **0x04 is SMS** (TX `04 0e 03`
= SMS CBS config SET) and **0x0A is MISC** (ME_VERSION 0x0a01, ME_SN 0x0a03, ...), as in the public Samsung IPC
naming. The note had 0x0A = SMS. Other groups seen: PWR 01, CALL 02 (a call-list GET by Android's RIL), SEC 05,
DISP 07, NET 08, GPRS 0d, CFG 0f, GPS 11, GEN 80, and the vendor groups 13, 26, 27.

`host/lnk.c` runs every captured `LNK-RX/TX` line in `build/test-boots/` through the parsers: 885 frames
(639 complete, 246 cut at 16 bytes by the kernel), **0 that fail to parse**. NET_REGIST frames are 30 bytes, so the
kernel truncated them: CS/PS state from captures stays "?".

### 3.4 Status messages (shapes read off the captures; no SIM in the phone)
| message | data | meaning |
|---|---|---|
| SEC_PIN_STATUS NOTI/RESP | `80 00` | status 0x80 = SIM absent, key 0 |
| SEC_SIM_ICC_TYPE NOTI | `00` | unknown card type |
| PWR_PHONE_PWR_UP NOTI | `53` | raw |
| PWR_PHONE_STATE NOTI | `02` | raw (2 after boot) |
| NET_REGIST NOTI/RESP | `01 02 04 00` + lac(2) cid(4) rej + 8 more | act GSM, domain CS, state 4 = emergency-only. 19 bytes on this firmware; the first 11 are the classic layout. |
| NET_REGIST GET | `ff 02` / `ff 03` | act any, domain CS / PS |
| NET_SERVING_NETWORK RESP | `02 01 ff` + 6 ASCII PLMN digits + ... (26 bytes) | |
| NET_PLMN_SEL RESP | `02` | automatic |
| DISP_ICON_INFO RESP | `01 00 ...` (12 bytes) | icon mask, bars |
| DISP_RSSI_INFO NOTI | `3e 19 ff 63 00 ...` (22 bytes) | raw; meaning unconfirmed |
| GEN_PHONE_RES RESP | group, index, type, u16 code | 0x80xx = accepted (seen 0x8000, 0x8001); 0x000a = refused |

Unconfirmed until a SIM is in: the LTE `act` value (7 assumed), ICON_INFO bars, the RSSI bytes.

## 4. Host test

```bash
sh starbite/modem/host/run.sh
```

It runs about 37,000 checks: the TOC, map and plan against this phone's values, plus refusals; SIPC5 including
>64 KB and multi-frame; the SBD golden layout, full/wrap, hostile indices, fuzz; the allow-list exhaustively;
end-to-end AP ⇄ fake CP (no SIM, then SIM + home network, a multi-frame NOTI, junk frames); redaction; the probe
against a register model (reset / running / online / crashed / booting, a refused mailbox, a refused carve-out,
and a trace check that it reads nothing outside its list and never CP_CTRL_NS/S). It also decodes the captured
traffic. Given a RADIO image (`run.sh path/RADIO.img`, or `backup/*/RADIO.img` when present) it checks the real
TOC and map. The 2026-11-05 backup has since been moved off this disk; its values were checked by hand on
2026-11-08 and are the test's fixtures.

## 5. Stage 2: the read-only phone probe

`starbite/modem/probe/` builds `modemprobe.img` (bare libsb image, in the VM:
`limactl shell starbite -- sh -c "cd $PWD && sh starbite/modem/probe/build.sh"`). It runs:

1. **Self-test** of the core on the phone's CPU in ordinary RAM: SBD layout = logged offsets, SIPC5+IPC round
   trip, 2048 CALL requests refused.
2. **Step 1** (key `modemprobe`): PMU `RESET_SEQUENCER_STATUS` 0x504, `CP_STAT` 0x38, `CP_DEBUG` 0x3c,
   `CENTRAL_SEQ_CP_CONFIGURATION/STATUS` 0x280/0x284; mailbox MCUCTLR, INTMR/INTSR/INTMSR 0 and 1, ISSR0-15,
   decoded (AP/CP status fields, cp2ap msg). **Never** CP_CTRL_NS/S (secure, EL3 SMC only). No SMC at all.
3. **Step 2** (key `modemprobe-shm`): also three small carve-out reads (modem_if + 0, the map at + 0x2a0, the
   IPC region's magic/access), stopping at the first refused read. This answers research unknown #1: is the
   carve-out AP-readable before the EL3 security SMC? Android's kernel reads the map there at probe, so probably
   yes. A refused read shows up as a synchronous abort (sb_probe32) or a pending SError: libsb masks PSTATE.A, the
   glue checks ISR_EL1.A after each read and takes the SError into libsb's counting vector. **Risk:** if EL3 routes
   SErrors to itself, the phone may reset (the loader's crash screen). That is why step 2 is a separate key, runs
   after step 1's lines are in pmsg, and is meant to run with the user present.

The only register the image stores to is the watchdog's WTCNT (the feed, when the loader armed it).

Breadcrumbs: before each block (PMU, mailbox, modem_if + 0, + 0x2a0, IPC) a `probe: reading ...` line goes to pmsg,
and the PMU values are logged right after they are read. If a read hangs and the watchdog resets the phone, the
last pmsg line names the block. **All-zero carve-out:** reads that succeed but return only zeros (head all zero,
no valid map) do not answer unknown #1. An empty carve-out and a TZASC read-as-zero denial look the same. The
report says `SHM: all zero: nothing loaded OR TZASC read-as-zero; not proof of access` and the verdict is
`shm-zero-ambiguous`.

Verdicts: `cp-off`, `cp-in-reset`, `cp-powered` (sequencer 5, cp_status 0), `cp-running`, `cp-booting`,
`cp-online`, `cp-crashed`, `shm-zero-ambiguous`, `unreadable`. Expected under the Starbite loader (S-Boot ran, Android did not):
`cp-off` or `cp-in-reset`, ISSR words 0.

Catalog rows the main session adds (bare, hidden, lab):
`modemprobe` (cmdline `modem_auto=1 modem_return=60`, wdt 30, wdt_feed own) and `modemprobe-shm` (no auto: hold
Volume Up). Results: pmsg (`/sys/fs/pstore/pmsg-ramoops-0` in TWRP) lines starting `modemprobe:` / `PMU:` /
`MBOX:` / `SHM:`.

### Result T1 (2026-11-08 17:52, image 509a2fa, `build/test-boots/20261108-175151-modemprobe/loader-pmsg.txt`)
Under the loader (S-Boot ran, Android did not): verdict **cp-off**, 0 faulted reads, no SError. The on-phone
self-test was ok. PMU: RESET_SEQUENCER_STATUS 0 (not 5: `cp_init` never ran), CENTRAL_SEQ_CP_CONFIGURATION
0x00010000 (bit 16 set: not held by a reset-assert), CENTRAL_SEQ_CP_STATUS 0x00, CP_STAT 0x1, CP_DEBUG 0. The
mailbox is clocked and AP-readable (MCUCTLR, the interrupt registers and ISSR0-15 all 0), so stage 4's polling path
needs no clock work. In short, S-Boot leaves the CP powered but never started, and stage 3 must run the `cp_init`
sequence itself. T2 (the carve-out reads) is next.

### Result T2 (2026-11-08 19:42, image 509a2fa, `build/test-boots/modemprobe-shm-1942/loader-pmsg.txt`)
No faults. **The carve-out is AP-readable at EL1 before any EL3 call** (unknown #1, read side, is answered).
modem_if + 0 holds RADIO's TOC (`54 4f 43 00`), and + 0x2a0 holds the valid MEM0 map with the same values as
the RADIO image. S-Boot copied at least RADIO's first bytes there raw. The IPC region's first words are
0xffcbffdf / 0xffffdfff: uninitialised DRAM, never set up. The verdict is still cp-off.

### 5.1 Steps 3 and 4 (the next probe, image `modemprobe`, same build)
Stage 3 copies BOOT/MAIN/VSS/NV into the carve-out and then calls EL3, so two things must be known first:
how much S-Boot already put there, and whether the AP can store into it.

- **Step 3**, key `modemprobe-map`, read only, unattended-safe. `sbm_probe_survey` reads the carve-out's own
  TOC, then CRC32s 8 ranges (64 KiB at most each) of the carve-out and of the RADIO partition (GPT "RADIO",
  read only): TOC, BOOT, the head and tail of MAIN and VSS, each at its raw file offset and at cbd's place
  (MAIN + 0x10000, VSS + 0x7900000). Output `SURVEY: <range>: MATCH | differs | zero | carve-out refused`. If
  "MAIN tail raw" matches, S-Boot copied the whole of RADIO raw. If only TOC/BOOT match, it copied the head.
- **Step 4**, key `modemprobe-acc`, user present. First CP_CTRL_S / CP_CTRL_NS through EL3's read call
  (`sbm_cp_ctrl_read`: SMC 0x82000700, x1 = 3 READ_CTRL, x2 = half, x3 = 0 S / 1 NS, as PMUCAL reads them;
  value in bits 31:16 of each half, low 16 bits 0 on success). Never the write call: the glue refuses any other
  arguments. Android logged CP_CTRL_S 0x8 / CP_CTRL_NS 0x2016a2 *after* cal_cp_init. Then
  `sbm_probe_write_test` on two fixed words, the IPC command area (modem_if + 0x8000400, unused with IOSM
  off) and the last word of the CP's memory (+ 0x78ffffc). For each: read, store original ^ 0xA5A5A5A5,
  `dc civac` to DRAM, read back, then put the original back and check. Results: `writable`, `write-ignored`
  (TZASC drops stores), `store-faulted` (it stops there), `unreadable`. The glue's `poke32` refuses every
  other address.
  Dead-man: the catalog watchdog (30 s, fed only between lines). A hang in the EL3 call or a store resets into the
  loader's crash screen, and the last `probe:` line in pmsg names the step.

### Result T3 (2026-11-08 20:13, image 1a4c529, `build/test-boots/modemprobe-map-2013/loader-pmsg.txt`)
0 faults. TOC raw, BOOT raw and MAIN head raw MATCH RADIO. MAIN's tail at its raw offset, and VSS at both
places, are mostly zero or other data, not RADIO's. So S-Boot copies only a prefix of RADIO, raw, from offset 0,
at least 0x12040 bytes: TOC + BOOT + the start of MAIN, enough for the MEM0 map. Nothing is at cbd's places.
The non-zero words in the VSS range are leftover DRAM (the IPC region also read 0xffcbffdf in T2 and 0 in T3).
**The TOC's crc fields are zlib CRC-32 of each section:** BOOT in the carve-out = 0x73672490 = the TOC's BOOT
crc. Stage 3 can verify every section it loads (MAIN 0xa98496b5, VSS 0x5e715799 on this phone's RADIO).

## 6. Stage 3: load the CP image and pass EL3's check (plan, 2026-11-08)

Android's order, from this phone's log (`20261108-091233-android-logged/dmesg-1.txt:2159-2296`) and
`link_device_shmem.c` `shmem_security_request`, `modem_ctrl_ss310ap.c` `ss310ap_on`:

1. SSS clock on: `SMC 0x82001011, 0, 0, 0`.
2. `cal_cp_init` (only when RESET_SEQUENCER_STATUS is 0, as T1 found): CP_CTRL_NS |= bit 1 (CP_PWRON) and
   CP_CTRL_S |= bit 3 (CP_START), each as EL3 read-modify-write (`SMC 0x82000700, 3 READ_CTRL / 4 WRITE_CTRL,
   value-or-half, reg`). Delay 1024 us, then wait RESET_SEQUENCER_STATUS[2:0] == 5 and GICD ISPENDR
   (0x10101238) bit 28. Android logged CP_CTRL_NS 0x002016a2, CP_CTRL_S 0x8, RESET_SEQ 5 within 1 ms.
3. The security call, mode 2 (CP_BOOT_RE_INIT): `SMC 0x82000700, 2, 0, 0` -> 0x0b CP_NOT_WORKING, which is
   expected and harmless. SSS clock off: `SMC 0x82001011, 1, 0, 0`.
4. Copy: BOOT (RADIO 0x200, 0x1e40) to + 0, MAIN (0x2040, 0x23d8ea0) to + 0x10000, VSS (0x23daee0, 0x57cbf0)
   to + 0x7900000, NV (EFS `nv_data.bin`, 1 MiB) to + 0x4900000. These are `sbm_plan_build`'s numbers. S-Boot's
   raw prefix (TOC at + 0) is overwritten, as on Android.
5. SSS clock on; the security call mode 0: `SMC 0x82000700, 0 | 0xF0000000, 0x1e40, 0x23d8ea0` ->
   0x00 CP_NO_ERROR after about 127 ms (EL3 checks the signature). SSS clock off. cbd moves the calling thread
   off CPU 0 for both security calls (cpu_set 0xff -> 0xfe): **unknown #8**, whether EL3 requires that.
6. Power on (`ss310ap_on`): all mailbox words 0; ISSR2 = sys_rev 174 << 6 | ds_det 1 << 14 (0x6b80), then
   pda_active (bit 5); **AP2CP_CFG (0x14820000) = 1, the CP CPU starts**; wait 300 ms.
7. Then the link (stage 4): poll ISSR3 cp_status; magic "BOOT" at IPC + 0; the UDL exchange; INIT_START ->
   build the SBD layout (`sbm_sbd_init`) -> PIF_INIT_DONE; PHONE_START -> INIT_END once FMT and RFS are open
   (RFS server needed: stage 5).

### Phone steps (each is a new image key, each with the user present the first time)

| step | what | changes | dead-man / way back |
|---|---|---|---|
| **T5 load-check** (`modemload-dry`) | read RADIO's BOOT/MAIN/VSS into RAM and check each against its TOC CRC-32; read EFS read-only (ext4: the Wi-Fi core's `ext4.c` reader, shared), take `nv_data.bin` (1 MiB) | RAM only. Nothing in the carve-out, no SMC. | unattended-safe; watchdog |
| **T6 EL3 gate** (`modemload-sec`) | T5, then steps 1-5 above: cp_init, mode 2, the store test again (does mode 2 change it?), the copy with a read-back CRC of each section from DRAM, mode 0. **It stops before step 6**: the CP CPU is never released. | PMU CP_CTRL via EL3 (the same two bits Android sets), carve-out DRAM | watchdog 30 s (fed between steps only); EL3 hang -> crash screen. Afterwards the user boots One UI once and checks that the modem still comes up (cbd's log shows ONLINE): Android handles RESET_SEQ 5 ("CP first Init" is skipped). |
| **T7 first breath** (`modemload-run`) | T6, then step 6 and stage 4's first half: wait for cp_status, set BOOT magic, the UDL exchange, then wait for the CP's INIT_START (15 s at most) and log it. Then **park the CP** with `cal_cp_reset_assert` (PMU *_SYS_PWR_REG writes + CP_CTRL_NS bit 2 via EL3 + CENTRAL_SEQ bit 16 = 0, wait status 0x80). The CP runs for seconds only, never reaches PHONE_START / network search, no RFS. | + AP2CP_CFG, mailbox words, PMU reset regs | park on every exit (timeout, Power, done); watchdog; a crash before parking -> watchdog reset (system reset takes the CP down: confirm on T7 by RESET_SEQ in the next boot's T1) |

What changes after T4: if the store test says **writable**, the plan stands. If **write-ignored** or
**store-faulted**, T6 runs the store test after mode 2 and copies only if the window has opened. If it is still
closed, stage 3 waits for a different EL3 mode (research) and nothing is copied.

Rules kept through stage 3: RADIO, EFS and CPEFS are only read. NV goes in from the RAM copy; the CP's own
writes (stage 5, RFS) go to RAM. No dialling: the IPC builder's allow-list is unchanged, and stage 3 sends no
IPC at all. No identifiers in logs: NV is checked by CRC and size only, never printed. The core gets EL3 *write*
calls only for the two CP_CTRL bits in PMUCAL's tables. The glue keeps an exact-argument allow-list, as it does
now for the read call.

### Result T4 (2026-11-08 20:15, image 1a4c529, `build/test-boots/modemprobe-acc-2015/loader-pmsg.txt`)
0 faults. CP_CTRL_S 0x00000000, CP_CTRL_NS 0x002016a0 through EL3's read call: CP_PWRON and CP_START are clear,
and NS is exactly Android's 0x002016a2 without bit 1. Both test words in the carve-out are **writable** and were
put back (0xf8000400, 0xf78ffffc). The main branch of the plan stands.

### 6.1 Stage 3 as built (commit after 28ce91c; `sbm_boot.c`, `sb/sbm_sb.c`, `probe/main.c`)
Keys on the same `modemprobe.img`: `modemload-dry` (T5), `modemload-sec` (T6), `modemload-run` (T7). T7 now
stops at the CP's first status word: release the CP, poll ISSR3 cp_status for up to 2 s, park. It sets no BOOT
magic, makes no UDL exchange and waits for no INIT_START. The CP stays in its own early boot, and the link is stage 4.

- **Load (all keys):** GPT "RADIO" (TOC, MEM0 map, plan, BOOT/MAIN/VSS) and GPT "EFS" ext4 `/nv_data.bin`
  (the Wi-Fi core's read-only ext4 reader, copied to `sb/ext4.c`) go into RAM. Each RADIO section must match its
  TOC CRC-32, and NV must be exactly the TOC's 1 MiB. Its content is never printed, only a CRC.
- **SEC:** refuses unless the CP is untouched (sequencer 0, CP_START and CP_PWRON clear). Then: cp_init (PMUCAL's
  table), mode 2, copy with a CRC read back from DRAM per section, mode 0, **park**.
- **RUN:** as SEC, then power on (ISSR0-63 = 0, ISSR2 = 0x6b80 then 0x6ba0 as Android, AP2CP_CFG = 1), up to 2 s
  for cp_status, **park**.
- **Park** = Android's `ss310ap_off`: INTGR1 bit 1 (wakeup), 5 ms, dump_pc_no_pg (CP_CTRL_NS bit 17 via EL3),
  `cal_cp_reset_assert` (the *_SYS_PWR_REGs, CP_CTRL_NS bit 2 via EL3, CENTRAL_SEQ bit 16 = 0, wait status 0x80).

### Result T5 (2026-11-08 20:34, cebf77f, `build/test-boots/modemload-dry-2034/loader-pmsg.txt`)
RADIO's BOOT/MAIN/VSS loaded, then **EFS: no ext4 /nv_data.bin**, a clean stop with 0 faults. **Cause** (found on
the read-only copy `backup/20261108-efs/EFS.img`, 2026-11-08 20:40): EFS's superblock has `INCOMPAT_ENCRYPT`
(features: has_journal ext_attr filetype extent encrypt sparse_super large_file uninit_bg; 4 KiB blocks; state
clean; journal_checksum + revoke). The reader copied from the Wi-Fi core refused that flag outright. The flag
only allows per-file encryption. nv_data.bin (inode 45, 1 MiB, flags 0x80080 = extents + noatime) is not
encrypted. The reader now refuses encrypted *inodes* (EXT4_ENCRYPT_FL) instead of the whole file system. On the
copy it reads nv_data.bin with the same CRC-32 as e2fsprogs' `debugfs -c ... cat`. The root holds nv_data.bin,
.nv_data.bak, nv_data.bin.md5, .nv_state, nv.log and 42 other names.
TWRP mounts EFS read-write when it starts and replays the journal ("EXT4-fs (sda3): recovery complete" in its
logs), so EFS can be left needing recovery. That is why the in-memory replay below stays. Our code never writes
EFS; TWRP does, on every visit.

### 6.1a EFS: encrypted inodes refused, the journal replayed in memory (T5b)
`sb/jbd2.c` replays the ext4 journal **in memory**. It scans committed transactions from the journal
superblock's start and sequence (descriptor tags with 32/64-bit block numbers, checksum v2/v3 tag sizes,
escaped blocks, revokes, an uncommitted tail dropped) and builds a map from file-system blocks to journal
blocks. `jbd2_read` consults that map for every EFS read. Nothing is written back. The loader logs every step:
superblock features, needs_recovery, the journal summary, the root's names (names only), and the
nv_data.bin inode. **Host fixture** (`host/mk-efs-fixture.sh`, in the VM): ext4 images snapshotted while
mounted after fsync-only commits, with and without metadata_csum. Without the replay the reader returns stale
data; with it, the newest nv_data.bin. Clean images read correctly too. Not exercised by the fixture: revoke
records (the code follows jbd2's rule: a revoke in transaction T masks the block in transactions up to T).

### Result T5b (2026-11-08 21:13, 0ec0bb4, `build/test-boots/modemload-dry-2113/loader-pmsg.txt`)
Clean, 0 faults. EFS: needs_recovery no, 47 root names, nv_data.bin inode 45 (1 MiB, crc 0xf93b2ff6, as on the
backup). BOOT 0x73672490, MAIN 0xa98496b5 and VSS 0x5e715799 all match the TOC. `BOOT: ok (done (dry))`. The
whole image is in RAM and verified; T6 is next.

### Result T6 (2026-11-08 21:15, 0ec0bb4, `build/test-boots/modemload-sec-2116/loader-pmsg.txt`)
Clean, 0 faults. **EL3 accepted our load:** cp_init (CP_CTRL_NS 0x002016a0 -> 0x002016a2, CP_CTRL_S 0 -> 8,
sequencer 5), mode 2 0x0b as on Android, four sections copied and read back from DRAM with matching CRCs, **mode
0 0x00 in 206 ms**, called from CPU 0 (Android: 127 ms, off CPU 0). That answers unknown #8: CPU 0 is fine. Then
the park: the full ss310ap_off sequence, CENTRAL_SEQ_CP_STATUS 0x80. Differences from Android's log:
- the GICD pending bit (0x10101238 bit 28) never showed (Android: set). PMUCAL only waits for it; probably the
  GIC is set up differently under the loader;
- CP_CTRL_S reads 0 again after cp_init (CP_START looks self-clearing once the sequencer runs);
- CP_CTRL_NS gained bits 31:30 while up (0xc02016a2) and lost them in reset;
- after the park CP_CTRL_NS is 0x002216a6 (+ CP_RESET_SET, + dump_pc_no_pg), and RESET_SEQUENCER_STATUS still
  reads 5 while the central sequencer reports 0x80 (in reset).
Rule 2's check is next: One UI's own cbd must bring the modem up after this.

### After T6: One UI, then T7 (2026-11-08 21:21)
One UI brought the modem up normally after T6 (ril.RildInit 1,1, modemSettingDone OK, ril.cpreset 0, SIM
ABSENT,ABSENT: no SIM inserted). **Rule 2 holds.** T7 right after that warm reboot refused as designed: sequencer
0, CP_CTRL_S 0, **CP_CTRL_NS 0x002016a2**. A warm reboot resets the CP's sequencer but keeps CP_PWRON in
CP_CTRL_NS. That corrects 6.2: CP_CTRL bits do survive a warm reboot, and only a cold boot clears them (T1/T4
had run after cold boots). Android's cbd treats exactly this state as "CP first Init" and re-runs cal_cp_init,
which sets CP_PWRON again (no change) and CP_START. Since commit after 252c6a0 the guard follows Android: it
refuses only when the sequencer is up or CP_START is set, and logs a leftover CP_PWRON.

### 6.2 Safety review (hazard -> what prevents it -> how it is tested)
| hazard | prevention | test |
|---|---|---|
| a partition written (RADIO, EFS, CPEFS, any) | the core has no block I/O; the glue only calls `sb_blk_read`; the stage 3 keys set `sb_storage_hold` for the whole run, so libsb refuses every block write | `host/run.sh` greps starbite/modem for block-write calls; `probe/build.sh` fails if a modem object references one (`nm -u`) |
| a wrong or corrupt image handed to EL3 | every section checked against the TOC's CRC-32 in RAM and again after the copy, read back from DRAM; EL3 checks the signature itself | host: corrupt byte -> stops before any EL3 call; refused copy -> no mode 0; EL3 refusal -> not started, parked |
| an EL3 call we did not mean | glue allow-list: CP_CTRL read; CP_CTRL write only when it differs from EL3's freshly read value in bits 1/2/3/17 alone; SSS clock 0/1; security mode 2 (2,0,0) and mode 0 (0xF0000000, BOOT <= 64 KiB, MAIN within the CP memory). Nothing else reaches `smc` | host: every recorded call audited against that list, in every scenario |
| a stray MMIO store | glue allow-list: PMU 0x1320-0x1338 and 0x280, ISSR0-63, INTGR1 = 2, AP2CP_CFG = 1 | host: every recorded store audited |
| the CP left running or half-started | every exit after cp_init goes through park (`stop()`); `sb_before_reset` parks again if a park failed (end, Power, the crash screen's reboot); a hang trips the 30 s watchdog, a system reset. T1 (17:52) ran after the morning's Android boots and read CP_CTRL_NS without CP_PWRON, AP2CP_CFG 0 on Android's own boots, so this state does not survive a reboot; T1 after T6/T7 confirms it | host: never-answering CP, EL3 refusal, copy refusal: all end parked (central status 0x80, CP_CTRL_NS bits 2 and 17, SSS clock off) |
| touching a CP someone else started | refuses unless sequencer 0 and CP_START clear (a CP_PWRON left by a warm reboot is accepted, as Android does) | host: "started before" -> only read calls, no store; "warm PWRON" -> runs and parks |
| network activity, SIM use, a call | no IPC at all in stage 3: no BOOT magic, no UDL, no INIT_END, so the CP never reaches the phase where RIL powers the radio; the IPC builder's allow-list (GETs only, CALL/SMS/SS/SAT/identity refused) is unchanged; nothing in stage 3 builds a frame | the existing exhaustive allow-list test |
| identifiers in logs | nv_data.bin only as size + CRC; logs redacted as before | existing redaction tests |
| a hang without a clue | a "boot: ..." line before every step, in pmsg | host: breadcrumb tests |
| **open:** the security calls on CPU 0 | cbd moves off CPU 0 for them; libsb runs on CPU 0 with interrupts masked, which should be fine. If EL3 objects, mode 2/0 return an error code, the run stops and parks | phone T6 |

Not done by any key: a write to RADIO/EFS/CPEFS; an IPC message; the CP's network phase.

### Result T7 (2026-11-08 21:27, cold boot, 0ec0bb4, `build/test-boots/modemload-run-2127/loader-pmsg.txt`)
**The CP booted from our load.** AP2CP_CFG 0 -> 1, cp_status set at once (ISSR3 0x2), ISSR2 0x6ba0 as on Android,
mode 0 0x00 in 205 ms. It was parked (`released 1 parked 1`, CP_CTRL_NS 0x002216a6), 0 faults.

## 6.5 Stage 4: the link up to PHONE_START (T8)
`sbm_link.c`, key `modemlink-boot` (stop `SBM_BOOT_LINK`), runs after T7's power-on, in Android's order
(this phone's log 19.2105-20.49; `shmem_start_download`, `xmit_udl`, `cmd_init_start_handler`, `ss310ap_boot_off`):
1. DL_START: the legacy map's eight queue words = 0, magic "BOOT" at IPC + 0 (read back), ap_status = 1 in ISSR2
   (0x6ba2, Android's value).
2. UDL handshake on ch 215 through the legacy RAW queue (`shmem_4mb_phys_map` without LEGACY_QOS: raw tx
   head/tail at +24/+28, buffer +12288 (2084864 bytes); raw rx head/tail +32/+36, buffer +2097152 (2 MiB)).
   Frames are SIPC5, config 0xFC (start + PADDING: the boot link is BOOT_ALIGNED), padded to 8.
   0x900D -> 0xA00D, then 0x9F00 -> 0xAF00, 2 s each (Android: 2.4 ms). **Confirmed in cbd** (One UI's
   `/vendor/bin/cbd`, pulled read-only to the git-ignored build/oneui-cbd/; stripped arm64, function at 0x54680):
   `std_udl_req_resp(fd, req, exp)` writes the u32 `req` with `write(fd, &req, 4)`, polls, then reads a 4-byte
   answer and compares it with `exp`. With umts_boot0's SIPC5 header the wire frame is `fc d7 08 00 0d 90 00 00`.
3. BOOT_OFF: dump_pc_no_pg off (CP_CTRL_NS bit 17 = 0 via EL3), then wait for the CP's message (15 s).
4. INIT_START (0xC1 in ISSR1, INTSR0 bit 16, acknowledged through INTCR0): `sbm_sbd_init` (the layout matched
   against this phone's log), then PIF_INIT_DONE (0xCD in ISSR0, INTGR1 bit 0).
5. PHONE_START (0xC8): logged; **stage 4 stops**. It sends no INIT_END and opens no RIL/RFS channel, so the CP
   never goes online. Then the park, as in stage 3. A CP crash command (0x07 / 0x09) stops it at once and parks.

Safety additions over 6.2:
- The glue allows two more stores, only with the stage 4 key: INTGR1 = 1 (the message doorbell) and
  INTCR0 = 1 << 16 (acknowledge).
- The IPC region is written directly (the legacy words, the SBD descriptors). That is the region designed for
  this; stage 3 never touched it.
- Host tests (fake CP): the exact UDL frames; ISSR2 0x6ba2; PIF_INIT_DONE is the only command ever sent, and
  0xC2 never; a silent boot loader, a wrong answer and a crash command all end parked.

### Result T8 (2026-11-08 21:53, warm boot, 265e926, `build/test-boots/modemlink-boot-2156/loader-pmsg.txt`)
**The link comes up to PHONE_START from our own code.** DL_START; UDL 0x900D -> 0xA00D and 0x9F00 -> 0xAF00 on
ch 215, each with a CP data message 0x0083 first; BOOT_OFF; INIT_START (0xC1) after 1055 ms (Android: about 1.2
s); the SBD layout; PIF_INIT_DONE; PHONE_START (0xC8) at once. Parked, 0 faults, mode 0 0x00 in 206 ms. Two
cosmetic bugs in the image's report are fixed since: the END line for modemlink keys printed an unfilled probe
verdict, and the post-park probe said "cp-running" (the reset sequencer stays 5 while the central sequencer
reports 0x80; the verdict now checks 0x80 first).

### 6.6 Tonight, unattended: T8 hold soak (`modemlink-hold`)
T8, plus holding 20 s after PHONE_START (`modem_hold=S`, at most 60) while sending nothing and consuming
nothing: it counts the CP's PHONE_START repeats and compares every DL ring's pointers each second, then parks.
Several runs in a row (warm reboots between them, the guard from bd8cc1b) show that the cycle is repeatable and
that the CP is stable and quiet in its pre-online state. No One UI check is needed for that: each run's "before"
line is the check (sequencer 0, CP_START clear).

Result (2026-11-08 22:28-22:32, 1fbce47, `build/test-boots/modemlink-hold-{1,2,3}-*`): **3 runs, identical and
clean**, warm boots back to back, about 66 s each, back to the loader on their own. `HOLD: 20000 ms: PHONE_START
repeated 203 times (last gap 100 ms), other commands 0, DL frames waiting 0`; parked, after-probe `cp-in-reset`,
0 faults. While it waits for INIT_END, the CP repeats PHONE_START every 100 ms and sends nothing on any ring.

## 7. Stage 5: online without RIL (plan; morning, user present)
INIT_END makes the CP go online. Two things are unknown, and both decide safety:
- **R1, RFS.** After INIT_END the CP reads (and writes) its NV files over ch 245, and without an answer it
  never settles. None of our captures contain a ch 245 frame (the kernel logs only FMT), so the RFS opcodes and
  framing are unknown. Plan: in daytime One UI (KernelSU), trace the RFS daemon's reads and writes on
  /dev/umts_rfs0 (headers, lengths and file names only, never contents), and pull its binary read-only like cbd.
- **R2, the radio.** Does the CP switch its RF on by itself after INIT_END, or only when RIL asks? The SIM-less
  phone camps for emergency-only service under Android, which is RX, and attach is impossible without a SIM. We
  need to know which IPC message turns RF on (pull libsec-ril / trace rild's first FMT frames), so that stage 5
  can keep RF off or send the "low power mode" request explicitly (one more allow-listed SET, radio *off* only).
Stage 5 = T9 `modemlink-online`: the ECT clock table (`sbm_clk.c`, ready: this phone's 52 frequencies at IPC + 40, host-tested against its log), INIT_END, an RFS server against the RAM copy of EFS/CPEFS (the CP's writes go to
RAM only), FMT receive-only with sbm_cell decoding (no request sent unless R2 requires the LPM one), 30 s, park.
Safety additions: never any SMS/CALL/SS (the builder's allow-list); RFS writes only to RAM; a crash command or the
CP's watchdog interrupt parks at once; first run attended.

### 7.1 Stage 5a as built: T9 `modemlink-online` (receive only)
After PHONE_START: the ECT clock table, INIT_END (the only new command), then for `modem_hold` seconds (default
20), receive only. FMT0/FMT1/RFS0 DL rings are drained; the other rings are left alone. FMT messages go through
`sbm_cell` and `sbm_frame_line` (identity payloads never shown). RFS messages are logged by their header
(`u32 length; u8 index; u8 id`) and `offset/length` only; an NV write's data is never shown. Nothing is sent: no
FMT request, no RFS answer. It stops (and parks) at once if the CP reports a SIM that is not absent, any
registration (home/roaming), or a crash command; otherwise after the time or 200 frames. Host-tested with the fake
CP: no SIM (6 FMT, 3 RFS, stops on time, clock table first, INIT_END last, a write's data never in a log), a SIM
present (stops within ms), a crash (parks).
The full path from here to a voice call, with which experiments need One UI, is in the shared notes:
`modem-voice-plan.md`.

### Result T9 (2026-11-09 15:58, 1822faa, `build/modem/t9-online-1/pmsg.txt`)
Stage 4 as before (INIT_START 1056 ms, PHONE_START 0 ms). After the clock table and INIT_END, the CP sent one more
PHONE_START (0xC8) and then **CRASH_EXIT (0xC9)**, with no FMT or RFS frame. Parked, the phone was fine (clean
restart). **Cause:** Android's `cmd_phone_start_handler` first runs `shmem_reset_ipc_link`: access 0, magic 0, the
legacy queue words 0, then **magic 0xAA (IPC) and access 1**, read back. The CP only treats the link as up with both
set. INIT_END follows about 140 ms later (this phone's log: PHONE_START 20.4877, INIT_END 20.6311). We sent
INIT_END with the magic still "BOOT". **T9b** does the reset, waits 100 ms, then the clock table and INIT_END,
and logs ms-since-INIT_END on every online line plus magic, access, ISSR1/3 and INTSR0 at INIT_END and at the
stop. The fake CP now crashes the same way when the reset is missing.

### Result T9b (2026-11-09, 8354e3b, `build/modem/t9b-online-1/pmsg.txt`)
**The link reset fixed the crash.** At INIT_END: magic 0xaa, access 1, cp_status 1. The CP stayed online the full
20 s, was parked, and restarted cleanly. It sent one more PHONE_START and data doorbells (0x0083) at +1170 to
+1415 ms, but all **78 frames counted as bad**. Cause: Android's `recv_data` takes the length as `size_v & 0xFFFF`
on **every** channel, and the CP puts the channel in the high half of FMT/RFS entries too. Ours masked only PS
channels, so every FMT/RFS entry looked longer than its buffer. **T9c** masks it on every channel. The fake CP now
packs the channel the same way. A refused entry logs its size word, and an unparsed frame its first 12 bytes; RFS
open requests show their flags and path.

### Result T9c (2026-11-09, 0f97603, `build/modem/t9c-online-1/pmsg.txt`)
**First real traffic from our own link.** FMT 21 frames on ch 235 and 236: PWR_PHONE_PWR_UP (53), 0x1305, 0x0d0f,
0x2601 INDI, SEC_SIM_ICC_TYPE 00, SEC_PIN_STATUS 80 00 (SIM absent), PWR_PHONE_STATE 02, 0x0827. RFS: first an
**open of `CP_OMC_AUDIO_SLSI.bin`** (flags 0), then an **NV write of 0x15e42 bytes at 0x80000** (an 89,680-byte
message). That message spans many 2048-byte DL buffers: the first carries the SIPC5 header with the whole length,
the rest are raw continuation (Android's `check_more`). Ours took the continuation for new messages (53 "other"),
**and the generic RFS logger printed 8 bytes of NV data from each of them** into pmsg (local, git-ignored file).
That breaks the rule; T10's logger prints header fields only.

### 7.3 T10 `modemlink-rfs`: RFS served from RAM
T9c plus:
- **big DL frames joined** (check_more; up to 640 KB for RFS, 64 KB for FMT);
- **RFS answered** by `sbm_rfs.c` from the RAM copy of nv_data.bin. NV read returns data; NV write lands in RAM only.
  Open answers "no such file" (handle -1, errno 2): Android also has no `CP_OMC_AUDIO_SLSI.bin` in
  /mnt/vendor/efs. Other file operations are counted and not answered;
- **replies sent Android's way**: one SIPC5 frame if it fits 2048 bytes, else multi-frame fragments (0xFB, `{id,
  more}`, 2043 payload bytes each, as `misc_write` with max_tx_size = 2048 does), each with the data doorbell 0x81,
  waiting for ring space;
- **RFS log lines** show header fields only (idx, id, length; NV offset/length; open path), never body bytes.
Host: the fake CP sends the open, a 6000-byte NV write over three buffers and an NV read, then checks our three
replies from the UL ring (ENOENT; write confirmed; read data correct in 3 fragments). The RAM copy holds the
write, and no NV byte appears in a log.

### Result T10 (2026-11-09, 27231dc, `build/modem/t10-rfs-1/pmsg.txt`)
Online 30 s, parked, no crash, no body bytes in the log. The open of CP_OMC_AUDIO_SLSI.bin was answered (errno 2,
+1169 ms), then the CP sent **close (0x06)** and the 89,680-byte NV write. "big frames 0": the write did **not**
come as check_more continuation. Every 2048-byte buffer carried its own ordinary SIPC5 frame (2040 payload
bytes), and the **RFS layer** joins them by the RFS header's u32 length. libsec-ril's `Nv::ProcessRfsPacket`
does exactly that: a pending message, at most 0x80000, appended until complete. **T10b** joins at the RFS layer
(`rfs_join`) and answers close (result -1, errno EBADF: `ProcessCloseFile`'s 14-byte reply; we never hand out a
handle). The fake CP now splits long RFS messages the same way.

### Result T10b (2026-11-09, 588e003, `build/modem/t10b-rfs-1/pmsg.txt`)
**The CP is online and settled with our RFS server.** 9 replies, 3 long messages joined from 99 pieces, no crash,
SIM absent. Sequence:
- open CP_OMC_AUDIO_SLSI.bin -> ENOENT, close -> EBADF; the same for CP_AUDIO_SLSI.bin;
- NV write 0x15e42 at 0x80000 and 0x4ff4 at 0 -> confirmed (RAM), +3857 ms;
- +17821 ms open FLL.bin -> ENOENT; the 0x80000 NV write again; open (flags 0x42, create) err/csdiag_rf_Mipi.dat ->
  ENOENT.
None of these files is in EFS's root. The CP's diagnostics create fails harmlessly instead of storing anything.

### 7.4 T11 `modemlink-query`: the first AP -> CP requests
T10b plus the 11 status GETs of the builder's allow-list (power state, SIM status and card type, CS/PS
registration, serving network, PLMN selection, icon/RSSI, mode, time), sent once 3 s after INIT_END on FMT0 with the
data doorbell. 60 s window. No SET or EXEC (T11 showed the receiver camps anyway: see its result); no CALL/SMS/SS/SAT/identity request
(the allow-list refuses them before a frame exists). Answers (RESP / GEN_PHONE_RES) are counted and decoded.

### Result T11 (2026-11-09 17:50, 436cbf9, `build/modem/t11-query-1/pmsg.txt`)
**The first AP -> CP requests work.** 11 GETs sent at +3000 ms, all answered at +3001 ms: 9 RESP, plus 2
GEN_PHONE_RES errors (SEC_SIM_ICC_TYPE code 0x000a; MISC_TIME_INFO code 0x8001). The decoded state is SIM absent,
CS emergency-only (GSM), PS not registered, a PLMN and 4 bars; PWR_PHONE_STATE 02, PLMN_SEL 02 (automatic). The
PLMN identifies the user's country and operator, so it stays in the local pmsg only, never in git.
**Correction: the CP camps without PWR_PHONE_STATE NORMAL.** Its receiver scans and camps on a cell in the default
state; PHONE_STATE 02 is probably "normal/online". With no SIM that is receive-only "limited service", as in
Android with no SIM ("emergency calls only"). Location updating, GPRS attach and LTE attach all need the SIM's IMSI
and keys, so the CP cannot register. The only transmission allowed in limited service is an emergency call, which
our builder refuses before a frame exists. The logs cannot prove zero RF output, but nothing in them shows a
registration attempt. Once a SIM is in, the CP will try to register by itself as soon as it is online, so **from
then on every online run counts as a transmitting run** (user present, the user's SIM).

### 7.2 Facts from One UI's vendor binaries (2026-11-09; read offline, binaries kept out of git)
VENDOR was copied read-only in TWRP and extracted in the VM (EROFS). From `lib64/libsec-ril.so` (stripped, but its
C++ dynamic symbols remain), `etc/mixer_paths.xml` and `firmware/calliope_*.bin`:
- **RFS** (Nv::ProcessRfsMessage): header `u32 length; u8 index; u8 id`, a 23-way dispatch on the index:
  0x01 NV read, 0x02 NV write, 0x03 read file, 0x04 write file, 0x05 lseek, 0x06 close, 0x07 put file,
  0x08 get file, 0x09 rename, 0x0a file info, 0x0b delete, 0x0c make dir, 0x11 open, 0x13 info by handle,
  0x14 create, 0x15 NV write all, 0x17 NV restore (the rest are refused). NV read: request `offset@6, length@10`
  (at most 0x80000); reply `u8 confirm, u32 offset, u32 n` + data (15-byte header, id echoed). NV write: request
  `offset, length, data`; reply 15 bytes. Open: `u32 flags@6, u32 path_len@10 (<= 1023), path@14`, relative to the
  RFS directory. Android writes nv_data.bin, fsyncs, updates nv_data.bin.md5 and a backup. **Ours writes only to
  RAM** (`sbm_rfs.c`, host-tested).
- **Radio power** (IpcProtocol41Power::IpcTxRadioPower): `PWR_PHONE_STATE (0x0107) EXEC`, u16 data `0x0001` low
  power mode (RF off) / `0x0202` normal (RF on). Modem power-off: `PWR 0x0102 EXEC`. RIL's RadioPowerUpHandler
  sends NORMAL at boot. **Corrected by T11:** without that request the CP still scans and camps (receive). The
  inference "it stays in low power mode until asked" was wrong.
- **Call** (IpcProtocol41Call): OUTGOING `0x0201 EXEC`, 99 bytes: u16 call-type code, CLIR, number length (<= 82),
  prefix (0x11 if the number starts with '+', else 0x21), the number. Also Answer, Release, Hold, DTMF,
  GetCallList, and an emergency-status call that we never build.
- **Sound** (IpcProtocol41Sound): AUDIO_PATH `0x0903 SET` (2 bytes), SPKR_VOLUME `0x0901 SET` (2 bytes),
  CLOCK_CTRL `0x0909` (1 byte), mute, WB-AMR, ringback.
- **Call audio**: the VSS (voice) task runs **inside ABOX's Calliope firmware** ("VSS_init_proc", "SET CALLPATH").
  The audio HAL's in-call handset route is `ABOX UAIF1 SPK = SIFS0`, `ABOX RSRC0 = SIFS0`, `ABOX RECP PIFS0 = 1`,
  `ABOX Sound Type = VOICE`, plus the amp's one-stop mode "Receiver Left/Right". **The earpiece is the MAX98512's
  receiver mode, fed from UAIF1**, not the codec's HPOUT3. Path: CP <-> ABOX (Calliope + VSS) <-> UAIF1 <->
  amp / codec.

### 7.5 Overnight 2026-11-09/10: a soak, SMS and the dial policy (host-only)
- **Soak** (`modemlink-soak`, 2e82690): T11's queries every 10 s for 10 min, and a PLMN-masked status line every
  60 s (`sbm_cell_line_masked`; every ONLINE line now masks the PLMN).
- **Dial / send policy** (`sbm_policy.c`): a number passes only if it is on the user's allow-list digit for digit,
  has at least 7 digits, and is not an emergency number. The emergency check covers a world list, the SIM's and the
  network's lists, and any of those behind +CC / 00CC. No switch disables it.
- **SMS** (`sbm_sms.c`): TP-DELIVER decode (GSM 7-bit with the default and extension tables, UCS-2 with surrogates,
  8-bit by length, concatenation headers with their fill bits), the incoming IPC 0x0402 layout read from
  libsec-ril (data[0] 2 = GSM, data[5] = length, SMSC, PDU), and TP-SUBMIT encode, which calls the policy first.
- **Not built, on purpose:** the IPC send frame (0x0401 EXEC: data `{a, b, f(c), pdu_len}` + PDU; the meaning and
  values of the first three bytes are unconfirmed) and any call frame. They wait for the SIM day, with the user
  present and a real capture (O3/O4) to confirm them. Nothing in this core can send or dial.

### 7.6 The session API for Starbite OS (`sbm_modem_*`)
The online stage is now two functions: `online_enter` (link reset, clock table, INIT_END) and `online_poll` (one pass:
mailbox, rings, RFS, guards, queries). The test images run them in a timed loop as before. Starbite OS gets:
- `sbm_modem_start(m, plat, image)`: load, EL3, start, link, online with the RFS server. About 2 s, blocking; the CP
  stays up (`keep_running`).
- `sbm_modem_poll(m, elapsed_ms)`: non-blocking, from the event loop. A guard (a SIM that is not absent, a
  registration) or a crash command parks the CP and returns 1 / an error.
- `sbm_modem_query`, `sbm_modem_status` (the masked line for a status screen), `sbm_modem_stop` (park).
The platform glue for Starbite OS is `sb/sbm_sb.c` (as for the test images), and the reset hook must park. Host
tests cover a 2 s loop, a query, the masked status, stop/park, and a SIM appearing (the guard parks from poll). The
phone demo is key `modemsess`: a 1 ms loop for 2 minutes, queries every 10 s, the masked status line on the screen.

**Soak run 1 (2026-11-09, 2ecf7cd, `build/modem/soak-1/pmsg.txt`): returned unparked after INIT_END.** `stage3()` in
the probe never zeroed its `struct sbm_boot`. The new `keep_running` input was stack garbage (non-zero), so
`sbm_boot_run` took the session path. It returned "online (session: running, not parked)" with zero traffic, and
the image went to its end. The reset hook still parked the CP before the reboot (`CENTRAL_SEQ_CP_STATUS 0x80`), and
One UI's modem was not affected. Fixed in three places:
- the core keeps the CP only for `keep_running == SBM_KEEP_RUNNING` ("KEEP"); any other value parks;
- `stage3()` zeroes the struct;
- `stage3()` parks the CP itself whenever `sbm_boot_run` returns with it unparked, without waiting for the reset hook.
A host test runs ONLINE with garbage-filled structs and four non-magic values, and every case must park. The host
test's own `struct sbm_boot` had the same flaw, which made its check count depend on the build. It is now zeroed.

**Soak run 2 (2026-11-10, 95e079a, `build/modem/soak-2/pmsg.txt`): passed.** It ran 651 s, parked, FMT 689, RFS 9,
bad 0, with a steady SIM absent / CS emergency-only (GSM) / bars 4 / rssi 0x3d-0x3f. Answers seemed to fall behind
from +180 s (660 sent, 529 counted), but FMT receive rose by exactly 66 a minute the whole time: the CP answered
everything, and our counter missed the answers. The query seq was a u8 from 0x40 and wrapped through 0x00..0x40,
while answers counted only for aseq > 0x40. That is 65 of every 256, so 130 uncounted (one more unexplained). Fixed:
query seqs cycle 0x41..0xFE, each answer is matched to a bitmap of open queries, and the ONLINE line now gives
unanswered / stale (a seq reused while still open) / unmatched counts. A host test runs a 590 s soak (649 GETs, three
wraps) and needs every answer matched.

**modemsess run 1 (2026-11-10, c6573f1, `build/modem/sess-1/pmsg.txt`): passed, came back clean after 166 s, parked.**
SIM absent at +10 s; not registered, bars 0 at +20 s; CS emergency-only (GSM), bars 4, rssi 0x3d from +30 s to
the end. 121 queries in 11 rounds, 110 answers: one round's worth is missing. That is not queries in flight at the
stop: the last round went out at +110 s, the stop came at +120 s. The first round was answered in full (seqs 65-75),
and the frame log ends after 40 lines, so the log does not say which round lost its answers. A host test of the same
11 rounds, 10 s apart, gets every answer. The probe now prints the q / a counts before each round, and the end line
gives unanswered / stale / unmatched / bad, so a re-run shows which round it is.

### 7.7 Session API v2: continuous use, restarts, the small footprint (2026-11-10)
Goal: "Starbite OS with a working modem" without a SIM and without transmitting. The modem boots reliably, camps
emergency-only, reports live status a UI can show, and parks cleanly on every exit.

**Bugs found on the way (all host-tested now):**
- *A session stopped itself after about 8 minutes.* The flood guard was a total frame budget of 200 + hold / 50.
  A session has hold 0, so the budget was 600 frames, about 8 minutes at the session's ~70 frames a minute. It is now
  a rate: 2000 frames a minute. A host test runs a 12-minute session.
- *A second start in the same boot was refused.* After our park, the reset sequencer reads 5 on the phone (soak-1),
  and the core only accepts sequencer 0. The fake SoC set it back to 0 on a park, so the host tests never saw it.
  Starbite OS's Stop then Start, and any stall recovery, would have failed. Now the fake matches the phone, and
  the core restarts the way Android does on the 9810 (modem_ctrl_ss310ap.c ss310ap_reset, link_device_shmem.c
  cp_init_done):
  1. wakeup interrupt, 5 ms;
  2. dump_pc_no_pg on, reset assert;
  3. PMUCAL's cp_reset_release: CP_CTRL_S |= CP_START, wait for CENTRAL_SEQ_CP_STATUS 0, wait for the GIC bit;
  4. mailbox software reset (MCUCTLR bit 0, 5 us);
  5. then the usual security mode 2, copy, mode 0, power on. BOOT_OFF clears dump_pc_no_pg as before.

  This path is only taken for sequencer 5 + central sequencer 0x80 (in reset) + CP_START: a CP parked earlier in
  this boot. A CP that runs is still never touched. If the release never completes, the restart fails and parks.
- *sess-1's 11 missing answers* (121 queries, 110 counted): not explained yet. The host replay of the same 11 rounds
  gets every answer. The rings are drained on every poll, so a missed doorbell can't lose frames. FMT 149 − 121
  answers = 28 other messages, close to soak-2's 29, so the answers may have arrived and gone uncounted. The
  modemsess image now logs q / a before each round and unanswered / stale / unmatched / bad at the end.

**The small footprint.** The old loader kept all of RADIO's BOOT + MAIN + VSS in RAM (about 41 MiB) plus NV.
Starbite OS refused to start below 64 MiB free, which is why the modem and the new UI's ~70 MB couldn't coexist.
Now:
- `sbm_sb_load_image_stream` keeps only nv_data.bin (1 MiB) and its pristine copy (1 MiB) in RAM, plus a 256 KiB
  chunk: 2.25 MiB, taken once from the arena and reused by every start.
- BOOT/MAIN/VSS are read from RADIO (read only) at each start, twice. The first pass CRC-checks against the TOC; the
  second copies into the carve-out and must CRC the same (RADIO read differently: refused before EL3), then the
  read-back from DRAM is checked as before.
- That costs a second read of about 41 MiB per start (well under a second on UFS; to be measured: the start line
  gives the time).
- The core's fixed buffers are in the image's BSS: about 1.8 MiB for the link and RFS (big_rfs 640 KiB, rfs_acc
  512 KiB, o_reply 512 KiB, big_fmt 128 KiB, rings 24 KiB). The glue's EFS reader adds about 420 KiB.
- The CP's own memory is the reserved carve-out at 0xF0000000, outside the arena, and unchanged.
- So the modem's arena need goes from 64 MiB to 2.25 MiB.

**The integration contract (Starbite OS kernel `kern/modem.c`, and the UI on top: the Cellular app, the status bar):**

| When | Call | Notes |
|---|---|---|
| once, first start | `sbm_sb_plat(&plat, log)`, `sbm_sb_load_image_stream(&im, log)`, `sbm_sb_plat_stage4(&plat, im.mm.ipc_off)` | 2.25 MiB arena, once. Drop alpha21's 64 MiB `MIN_ARENA` and its own `nv_pristine`: the image carries `im.nv_pristine` and the core restores NV before every start. |
| user's hold (Start) | `sbm_modem_start(&mo, &plat, &im)` | Blocking about 2-3 s: call it from a timer after the Starting page is drawn. 0 = online. Otherwise it is parked; the snapshot says FAILED and `last_err` why. `mo` is `static`, zeroed once, **never memset between starts**: it holds the counters and the restart window. |
| every 2 ms (1-10 ms) | `r = sbm_modem_poll(&mo, elapsed_ms)` | Non-blocking. 0 nothing new; 1 new data from the CP (take a snapshot); < 0 it went down and is parked: `SBM_ETIME` stall, `SBM_EIO` crash, `SBM_EPERM` a guard (a SIM that is not absent, a registration) or the flood guard. |
| every 10 s while up | `sbm_modem_query(&mo)` | The allow-listed GETs (no SET, no call/SMS/SS). **Required**: stall detection is "a query sent and not one word from the CP for 20 s". Not more often than every 5 s. |
| on poll = 1, and at the UI's own refresh (1 Hz is plenty) | `sbm_modem_snapshot(&mo, &snap)` | No I/O, a few hundred bytes copied. Redraw only when `snap.gen` changed (it ignores the clocks and counters). |
| state STALLED or CRASHED | `sbm_modem_recover(&mo, elapsed_ms)` from a 1 s timer | 1 = wait (5 s parked first); 0 = online again (blocking 2-3 s, as a start); `SBM_EPERM` = stays off: 3 restarts in 30 min used up. A GUARD, a FAILED start and a user's Stop never restart by themselves. |
| Stop, before Heat / Linux, every restart / power-off path | `sbm_modem_stop(&mo)` | Parks. Keep the reset hook (`sb_before_reset` → `sbm_cp_park`) as alpha21 has it. |

Snapshot fields (`struct sbm_modem_snap`, sbm.h) and what the UI says:

| Field | Values | UI |
|---|---|---|
| `state` | OFF / ONLINE / STALLED / GUARD / CRASHED / FAILED | Off; On; "Restarting…" (STALLED / CRASHED while recover returns 1); "Stopped: a SIM or a network registration" (GUARD); "Could not start" (FAILED) |
| `sim` | 0x80 absent, 0x00 ready, 0x03 PIN, …, 0xFF not yet | "No SIM" |
| `cs`, `ps` | SBM_REG_*: 1 none, 2 home, 3 searching, 4 emergency, 6 roaming; 0xFF not yet | registration per domain |
| `emergency_only` | 1 = camped, limited service | **"Emergency calls only"** (the honest line; nothing in this build dials) |
| `rat` | SBM_ACT_*: 1 GSM, 2 GPRS, 3 EDGE, 4 UMTS, 7 LTE; 0 unknown | "2G" / "3G" / "4G" next to the bars |
| `bars` | 0..5, 0xFF unknown | the signal glyph |
| `rssi_raw`, `rssi_dbm` | DISP_RSSI_INFO's byte; dBm = −raw (Samsung's IPC habit, **unconfirmed**: 0x3d = −61 dBm with 4 bars fits) | details page only |
| `have_plmn`, `plmn` | MCC-MNC, e.g. "00101" | **screen only**: never in a log, a file, an app's storage or a screenshot that leaves the phone. No operator-name table in the core; the UI can map MCC-MNC to a name from a public list, or show "Network: MCC MNC" |
| `have_cell` | the CP sent LAC / cell id | the values are deliberately not kept (location); a "cell info" page shows RAT, bars, dBm and registration only. Keeping LAC/CID for the screen is a separate decision for the user. |
| `up_ms`, `last_rx_ms` | since the start; since the CP last said anything | details page |
| `queries`, `answers`, `open`, `starts`, `restarts`, `last_err` | counters | details page; `last_err` → sbm_strerror |
| `line` | the masked one-line status | safe for logs (pmsg, the kernel log) |

Not available yet, and why:
- RSRP / RSRQ: the CP camps on GSM without a SIM; LTE signal values need a signal-info GET that is not in the
  allow-list yet.
- The network's name string: the CP doesn't send one without a SIM (EONS comes from the SIM; NITZ only after
  registration).

Rules unchanged: GET-only, no call / SMS / SS frame exists, no transmit without the user and a SIM, the PLMN and
IMEI never in git or a log, EFS never written (RAM copy, restored from the pristine copy at every start).

## 8. Later stages (not started)


(Stage 4 and stage 5's plan are above.) Then the ECT clock table write that Android does before INIT_END.
6. Status in the UI (SIM, registration, bars); then data (PDP), and SMS to the user's own number only.
7. Voice: research only. Emergency numbers stay refused before any frame is built.
