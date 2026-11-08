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

Verdicts: `cp-off`, `cp-in-reset`, `cp-powered` (sequencer 5, cp_status 0), `cp-running`, `cp-booting`,
`cp-online`, `cp-crashed`, `unreadable`. Expected under the Starbite loader (S-Boot ran, Android did not):
`cp-off` or `cp-in-reset`, ISSR words 0.

Catalog rows the main session adds (bare, hidden, lab):
`modemprobe` (cmdline `modem_auto=1 modem_return=60`, wdt 30, wdt_feed own) and `modemprobe-shm` (no auto: hold
Volume Up). Results: pmsg (`/sys/fs/pstore/pmsg-ramoops-0` in TWRP) lines starting `modemprobe:` / `PMU:` /
`MBOX:` / `SHM:`.

## 6. Next stages (not started)

3. CP boot: copy BOOT/MAIN/VSS + NV from RAM copies (RADIO, EFS `nv_data.bin`, read once) per `sbm_plan`, the
   security SMC (mode 2 probe, then mode 0 with boot/main sizes), AP2CP_CFG release. User present.
4. Link: mailbox doorbells + polling, INIT_START → PIF_INIT_DONE → PHONE_START → INIT_END, the ECT clock table.
5. RFS server on ch 245 against a RAM copy of EFS/CPEFS (CP writes land in RAM only). Without it the CP never
   goes online.
6. Status in the UI (SIM, registration, bars); then data (PDP), and SMS to the user's own number only.
7. Voice: research only. Emergency numbers stay refused before any frame is built.
