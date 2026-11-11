# The S9's sensor hub (BCM4775): how a bare-metal OS reads the accelerometer and magnetometer

Research only, 2026-11-11. No phone was touched. Nothing here has run on our phone yet.

The accelerometer/gyroscope (ST LSM6DSL), the magnetometer (AKM type, `ssp-mag-type = 1`), the barometer (LPS22H)
and light/proximity (TMD4906) are not on an AP bus. They hang off the sensor-hub MCU inside the Broadcom BCM4775 B0
GNSS chip (`compatible = "ssp,BCM4773"`). The AP reaches the hub over one SPI bus and a few GPIOs, and has to load
the hub's firmware into its RAM at every power-up. On Android the closed daemon `lhd` does the loading.

## Sources and markers

| Marker | Source |
|---|---|
| **[K]** | Samsung's GPL kernel, LineageOS `android_kernel_samsung_exynos9810`, branch `lineage-20`, commit `6a9461f9460c`. Sparse checkout in the Lima VM `starbite` at `/mnt/build/shub/k` (64 MB; delete when no longer needed). Paths below are relative to it; line numbers are that commit's. |
| **[DT]** | `arch/arm64/boot/dts/exynos/exynos9810-starlte_eur_open_26.dts` in [K] (a decompiled DT, phandles are numbers). |
| **[LHD]** | strings of our own `~/modem-vendor/x/bin/hw/lhd` ("Broadcom LHD2 ver. 108.20.545200, Jul 20 2022") and `etc/sensor/lhd.conf` in the VM. |
| **[HAKI]** | an earlier port `example-project/refport-starlte` at `5cd68c99`, `SENSORS.md` and `firmware/sensorhub/README.md`. Read for facts only; no code read or copied (the driver is not in that repository anyway). It reports the whole chain working on a Galaxy S9 on 3 November 2026. |
| **[GTA3]** | `exynos-linux/samsung-gtactive3wifi` at `8f4314c5`, `docs/re/sensorhub.md` (Galaxy Tab Active3, same chip and SoC). Its section 6.3 describes lhd's firmware downloader in prose, from a Ghidra reading of lhd. Read for facts only. |
| **[VER]** | checked by me with a script in the VM (`/mnt/build/shub/tools/pp.py`, `crc.py`). |
| **[GUESS]** | inferred, not verified. |

Anything marked only [HAKI] or [GTA3] is somebody else's finding that I did not reproduce.

## 1. Hardware

### 1.1 SPI

| Item | Value | Source |
|---|---|---|
| Controller | `spi@13AC0000` (USI_CHUB00, in the CHUB block), `samsung,exynos-spi`, 64-byte FIFO, interrupt SPI 0x68 | [DT] 7905-7921 |
| USI mode word | `usi@13A13000` (`usi_00_chub`), `usi_v2_mode = "spi"`; the value for SPI is 2 | [DT] 5690-5697; value [HAKI] |
| Slave | `BCM4773@0`, `reg = 0`, `spi-max-frequency = 0x17d7840` = 25 MHz, `spi-cpol` + `spi-cpha` = mode 3, 8-bit words, MSB first | [DT] 7923-7927 |
| Chip select | `cs-gpio = <gph0 3>`. Samsung's SPI driver requests it as a GPIO output, idle high, and drives it low around each message (`gpio_request_one(..., GPIOF_OUT_INIT_HIGH)`, `enable_cs`). Drive gph0-3 by hand as a GPIO, active low | [DT] 7950; [K] `drivers/spi/spi-s3c64xx.c:1203`, 603-640 |
| Bus pins | gph0-0, gph0-1, gph0-3 function 2, no pull, drive 3; gph0-2 (MISO) function 2, pull-down (`pin-pud = 1`), drive 3. Which of gph0-0/1 is clock and which MOSI is not stated (the node is named `sensorhub_mosi_sck_ssn`); it does not matter for the mux | [DT] 799-815 |
| Clocks | `clocks = <0x1a 0x56 0x1a 0x59>` = GATE_USI_CHUB00 (86) and CHUB_USI00 (89). CMU_CHUB base `0x13A00000`: divider `+0x1808` (DIVRATIO bits 3:0, BUSY bit 16), mux `+0x100C`, gates `+0x2010`, `+0x2090`, `+0x20A8` (bit 21 CG_VAL, bit 20 MANUAL), Q-channel `+0x305C` | [DT] 7914; [K] `include/dt-bindings/clock/exynos9810.h:34,37`; `drivers/soc/samsung/cal-if/exynos9810/cmucal-sfr.c:309-344, 2657-2701, 2781` |
| Bit clock | Samsung's driver sets the source clock to 4 x the bit rate ("a quarter-multiplier before the SPI"). an earlier port: source 177.666 MHz at divider 1; it uses divider 16, about 5.5 MHz on the wire | [K] `spi-s3c64xx.c:820`; [HAKI] |
| CHUB power domain | PMU `0x14060000 + 0x4084`, bits 3:0 = 0xF when on (the condition Samsung's PMU sequence uses before touching CMU_CHUB). an earlier port found it on after the boot loader on its phone. If it is off, any access to `0x13Axxxxx` will fault or hang | [K] `cal-if/exynos9810/flexpmu_cal_local_exynos9810.h:923-924`; [HAKI] |
| Controller registers | standard s3c64xx: CH_CFG 0x00 (TX on bit 0, RX on bit 1, CPHA bit 2, CPOL bit 3, SW_RST bit 5), CLK_CFG 0x04, MODE_CFG 0x08, SLAVE_SEL 0x0C, INT_EN 0x10, STATUS 0x14, TX_DATA 0x18, RX_DATA 0x1C, PACKET_CNT 0x20, PENDING_CLR 0x24, SWAP_CFG 0x28, FB_CLK 0x2C | [K] `spi-s3c64xx.c:50-126` |

### 1.2 GPIOs

Bank register layout (both bank types): CON `+0x00` (4 bits a pin), DAT `+0x04`, PUD `+0x08` (4 bits a pin: 0 none,
1 down, 3 up), DRV `+0x0C` ([K] `drivers/pinctrl/samsung/pinctrl-exynos.c:84-95`). Bank offsets from the same file,
lines 1915-1999.

| Signal | Pin | Bank address | Direction, as Samsung sets it | Meaning | Source |
|---|---|---|---|---|---|
| MCU_REQ | gph0-5 | `0x13A80000` (CHUB, bank +0x000) | output, starts 0 | AP raises it before every exchange and drops it after | [DT] 7934, 828-837; [K] `bbdpl/bcm_gps_spi.c:1187`, 495-550 |
| MCU_RESP | gph0-4 | `0x13A80000` | input, no pull | hub answers "awake" | [DT] 7935, 817-826 |
| SPI CS | gph0-3 | `0x13A80000` | GPIO output, idle 1 | see 1.1 | |
| HOST_REQ | gpa1-7 | `0x14050040` (ALIVE, bank +0x040) | input, pull-down, EINT level high | hub has bytes for the AP | [DT] 7933, 414-421; [K] `bcm_gps_spi.c:1241` |
| AP_SLEEP ("batch wake") | gpa0-5 | `0x14050020` | output, 1 while the AP runs | tells the hub the AP is awake | [DT] 7936, 404-412; [K] `ssp_dev.c:385` |
| SHUB_INT | gpa4-0 | `0x140500C0` | input, pull-down, EINT | timestamp strobe for batched gyro; not needed | [DT] 7937, 423-430; [K] `ssp_dev.c` `ssp_shub_int_handler` |
| NSTDBY (`GPS_PWR_EN`) | gpg0-2 | `0x10430080` (PERIC0, bank +0x080) | output, starts 0, pull-down | low holds the whole chip in standby; lhd raises it. This is the hub's reset line in practice | [DT] 13778-13784, 2678-2687; [K] `drivers/gps/sec_gps_bcm47531.c:255`; [LHD] `GpioNStdbyPath=/sys/class/sec/gps/GPS_PWR_EN/value` |
| Supply | S2MPS18 LDO37, `vdd_mcu_1p8`, 1.8 V, `regulator-always-on` | PMIC register `0x6E` (L37CTRL): bits 7:6 = 3 on, bits 5:0 = 0x2C for 1.8 V, so expect `0xEC`. Our `sb_pmic_read(1, 0x6E, &v)` reads it | lhd's `mcu_power` sysfs toggles this regulator and sleeps 20 ms | [DT] 9048-9057, 7947; [K] `include/linux/mfd/samsung/s2mps18-private.h:144,249-272`; `ssp_sysfs.c:533-560`; value 0xEC [HAKI] |

There is no separate reset pin and no `ssp-pwr-en` GPIO on the S9 (the DT has only the regulator name).

Sensor placement: `ssp-acc-position = 4`, `ssp-mag-position = 5`, `ssp-mag-array` (27 bytes, the AKM PDC matrix),
`ssp-mag-type = 1` (AKM) ([DT] 7938-7946). The positions are indexes that the hub's firmware understands; the kernel
only forwards them (3.3). No matrix for them exists in the GPL source.

### 1.3 Power-up sequence

What Android does, in order:

1. Kernel probe: MCU_REQ output low, MCU_RESP input, NSTDBY output low, AP_SLEEP high ([K] lines above).
2. lhd: `mcu_power` = 1 (LDO37 on, 20 ms sleep in the kernel), then NSTDBY = 1. If NSTDBY already read high, lhd
   pulls it low for 20 ms first ([GTA3] 6.3; [LHD] has the strings `Power enable delay: %dms`,
   `enforce NSTDBY init high`, `ClearNstdbyPin() delay: 120us`).
3. `LheAutoBaudDelayMS=10`: 10 ms, then the autobaud burst (section 2).

an earlier port on its S9 found: the boot loader leaves the CHUB domain on, LDO37 on, and gpg0-2 in function 2 reading high.
It drives NSTDBY low 20 ms, then high. MCU_RESP never rose on that phone, and everything worked once the driver
stopped waiting for it ([HAKI], "Runs on the phone"). Treat a silent MCU_RESP as normal, not as a failure.

## 2. Firmware download

### 2.1 Who does it

`lhd`, not the kernel. The kernel only:

- carries the patch as a byte array and serves it read-only as `/dev/bbd_patch` ([K] `bbdpl/bbd.c:131-155`, 783-800);
- moves raw bytes between `/dev/ttyBCM` and SPI, with the SSI framing of 2.3 ([K] `bbdpl/bcm_gps_spi.c`);
- parses the incoming packet stream only to pull sensor RPCs out of it ([K] `bbdpl/bbd_rpc_lh.c`);
- hands outgoing sensor bytes to lhd through `/dev/bbd_sensor`; lhd wraps them into packets
  ([K] `bbd.c:344-370`, comment "gpsd/lhd wrap the packet into RPC and sends to chip directly").

So the host's packet transmitter and the whole ROM dialogue exist only in closed `lhd`
(`firmware_downloader.cpp`, `transport_layer.cpp`, `rpc_engine.cpp` per its strings). Our lhd has the downloader's
state names: `FD_WAIT4AUTOBAUD`, `FD_SENT_AUTOBAUD`, `FD_GOT_SYNC`, `FD_WAIT4VERSION_INIT`, `FD_GOT_VERSION_INIT`
("ROM Version mismatch... Trying ASIC reset"), `FD_REQUEST_DOWNLOAD`, `FD_WAIT4DOWNLOAD_START`,
`FD_WAIT4DOWNLOAD_COMPLETE` ("Starting Scratch APP 0x%X" / "Starting Service APP 0x%X"), `FD_WAIT4APP`, `FD_DONE`
([LHD]).

### 2.2 The patch

| Item | Value |
|---|---|
| Location | [K] `drivers/sensorhub/brcm/bbdpl/q_os/bbd_patch_file_star.h` (Android 10 build; `o_os/` and `p_os/` hold older ones). Selected by `CONFIG_SENSORS_SSP_STAR` and `ANDROID_VERSION` in `bbd.c:131-155`. **Not copied into this repository.** |
| Form | a C string-literal list: text header lines `<Key=Value>` then `<Blob=...hex...>`; 935991 bytes of text |
| Header (q_os) | FormatVersion 0x00010003, Crc 18, BlobLength 413864, AsicVersion 0x004775B0, PatchLevel 0, PostPatchLevel 1, Name bcm_sensorhub_a1, Type Application, PatchAddress 0x00800161 [VER] |
| Blob | 413864 bytes, sha256 `b1a3e138ab808f2be247ad195d71d6aef2f2d3ecc7bd3e13e141b1443da408d0` [VER] (the same hash an earlier port pins). Records `[type u8][len u16 LE][body]`; all 452 records are type 0xF1 = write `[u32 LE address][data]`, at most 916 bytes each, contiguous from 0x00800000 to 0x0086444C, 410700 bytes [VER]. A Cortex-M image: word 0 = 0x00871590 (stack), word 1 = 0x00800161 (Thumb reset vector = PatchAddress) [VER] |
| Matching firmware revision | `SSP_FIRMWARE_REVISION_BCM 21031900` for the q_os patch ([K] `ssp_firmware.c:19-23`); an earlier port read 21031900 back from the hub |
| License | The header file has no license text of its own. It is `#include`d into `bbd.c`, which carries "Copyright 2014 Broadcom Corporation ... GNU General Public License, version 2". The blob itself is Broadcom/Samsung firmware in hex; whether GPL-2 really covers it is unclear. an earlier port decided not to redistribute it ("its terms are still to verify") and builds it from the kernel header at build time. Do the same: fetch from a kernel tree, never commit it. |
| `Crc` field | checked by lhd over the text ("FD:CheckPatch failed"); the chip never sees it ([GTA3] 6.1, [HAKI]). We can ignore it and check the sha256 instead. |

### 2.3 SSI: the SPI framing ([K] `bbdpl/bcm_gps_spi.c`, `bcm_gps_spi.h`)

First MOSI byte of every transfer is a control byte (`bcm_gps_spi.h:25-41`): bit 7 debug/register mode, bit 6 full
duplex, bit 5 read, bit 4 two-byte length fields, bit 3 flow control. Stock settings: full duplex, 2-byte lengths,
flow control with 3 retries (`bcm_gps_spi.c:59-61`).

- **Write** (`bcm_ssi_tx`, 654-782): MOSI `[0x58][M lo][M hi][M bytes][00 00]`. MISO at the same time:
  `[status][N lo][N hi][chip bytes...]` and, at offset 3 + M, a 2-byte count of bytes the chip accepted. If it took
  fewer than M, resend the rest (up to 3 tries). Chip bytes that arrive during a write are valid stream data
  (N of them) and go to the same parser.
- **Read** (`bcm_ssi_rx`, 784-858): two transfers. First 3 bytes `[ctrl][00 00]` -> `[status][len lo][len hi]`.
  Then `len + 3` bytes `[ctrl][00 00][zeros]` -> `[status][len lo][len hi][len bytes]`; use the smaller of the two
  lengths. If len was 0 Samsung reads 254 bytes anyway.
- **The read control byte is uncertain between 0x70 and 0x50.** `bcm_spi_open` line 281 computes
  `duplex | len | (ssi_mode == SSI_MODE_FULL_DUPLEX ? WRITE : READ)` with `ssi_mode = 1` and
  `SSI_MODE_FULL_DUPLEX = 0x40`, which evaluates to **0x70** as written. [GTA3] and [HAKI] both state **0x50**, and
  an earlier port's driver worked on an S9 with it. Both are reads; try 0x50 first, 0x70 second.
- **Status byte** (`bcm_ssi_chk_pzc`, 168-206): bits 2:0 count, bits 4:3 block-size index {32, 256, 2048, 16384},
  bit 6 protocol error. Free space in the chip's receive ring = count x size.
- **Handshake** (`bcm477x_hello`, 495-541): MCU_REQ high, poll MCU_RESP up to about 100 ms, re-pulsing MCU_REQ every
  20 polls; MCU_REQ low when done. Samsung resets the hub on timeout; see 1.3 for why we must not.
- **When to read**: while HOST_REQ is high (`bcm_rxtx_work_func`, 883-1030; level-high interrupt at 1032). Polling
  the pin is enough.
- **Limits**: frame at most 8 KB (`MAX_SPI_FRAME_LEN`), everything 8-bit transfers.
- **Debug mode** (`bbdpl/bcm_gps_regs.c`): control 0x80 / 0xA0 reach the SSI block's own registers and, through
  them, 32-bit reads and writes of chip memory (`RegRead` 204, `RegWrite` 158, `bcm_reg32Iread` 352,
  `bcm_reg32Iwrite` 275; e.g. HSI_CTRL 0x40104090, `bbd.h:74-86`). Not needed. Do not use the write forms.

### 2.4 Packets ("TransportLayer", [K] `bbdpl/bbd_rpc_lh.c`; receive side only is in the GPL source)

- Stuffing (103-109, parser 573-674): `B0 00` start, `B0 01` end, `B0 03` = literal B0, `B0 04` = literal 0x11,
  `B0 05` = literal 0x13. Raw 0x11 / 0x13 are dropped.
- Unstuffed: `[seq][size][flags][one detail byte per set flag, in bit order][payload: size bytes][crc]` (369-474).
- Flags (114-132): bit 0 ACK (detail = seq acknowledged), 1 RELIABLE (reliable seq), 2 RELIABLE_ACK,
  3 RELIABLE_NACK, 4 MSG_LOST, 5 GARBAGE, 6 SIZE_EXTENDED (detail = size high byte), 7 EXTENDED (detail = flag bits
  15:8), 8 INTERNAL, 9 IGNORE_SEQID.
- CRC (232-272, 371-391): CRC-8, polynomial 0x4D, MSB first, initial 0, over everything after `seq` up to the last
  payload byte; sent with its nibbles swapped. [VER]: reproduces 0xB1 and 0x43 for the two sample frames in
  `bcm_gps_regs.c:71,76`.
- Receiver duties (474-561): answer every reliable packet at once with ACK | RELIABLE_ACK (or RELIABLE_NACK when out
  of order); plain packets need a bare ACK only after 200 unacknowledged ones.
- Payload = RPCs `[id][len][data]`; id and len are one byte, or two bytes big-endian with bit 7 of the first set
  (299-369). The GPL enum (137-172) names only 32 = sensor request and 33 = sensor response.

### 2.5 The ROM dialogue (closed; from [GTA3] 6.3, confirmed on an S9 by [HAKI])

The only GPL hint is the unused helper `bcm477x_rpc_get_version` (`bcm_gps_regs.c:48-141`): a burst of 32 bytes
0x80 "in case we are in the ROM and waiting for autobaud", then one packet, then polling reads.

| Step | Host sends | Chip answers |
|---|---|---|
| autobaud | 36 bytes 0x80, raw, no packet | nothing; wait 100 ms |
| sync | INTERNAL packet, payload `[0][n]` (n = attempt from 1), flags EXTENDED + INTERNAL + IGNORE_SEQID. The GPL sample is exactly this: `B0 00 \| 00 02 80 03 00 00 \| 00 01 \| B1 \| B0 01`. Every 100 ms, up to 6 times | INTERNAL payload `[1][n][last seq rx][last ack][last reliable rx][next seq][next reliable seq][last reliable acked]`. an earlier port saw `01 01 06 00 ff 00 00 ff`. Host then sets its next seq = byte 2 + 1, next reliable seq = byte 4 + 1 |
| version | reliable packet, RPC 0x00, no data | RPC 0x03 `[u32 asic][u32 rom][u32 patch level]`. an earlier port: ASIC `004775b0`, ROM `0004527c` |
| download | each 0xF1 record as RPC 0x05 `[u32 addr][u16 n][n bytes]`, one per reliable packet (payload up to 1024) | ACK + RELIABLE_ACK for each. an earlier port: 452 writes in 3.2 s |
| start | plain packet, RPC 0x2E `[u32 PatchAddress]` (Type "Application") | wait 40 ms |
| app sync | autobaud + sync again | sync response from the application |
| confirm | reliable RPC 0x00 | RPC 0x03 from the application. an earlier port: ROM field now `0006431f` |

Reliable retransmit 500 ms, 10 tries. SSI framing is the same 0x58 / read frames throughout; "auto-baud" on SPI is
just the 0x80 burst (the name is a UART leftover). `LheClockMHz=100` and `LhePinMux=P17=M1` concern the GNSS side
(lhd sends RPC 0x34 `[7][2]` for the external LNA after start, [GTA3]); the sensor hub does not need them [GUESS].

The RPC ids 0x00, 0x03, 0x05, 0x2E and the 36-byte burst are not in any GPL source and I did not re-derive them
from our lhd. They rest on [GTA3]'s reading plus [HAKI]'s run.

## 3. After boot: SSP, Samsung's sensor protocol ([K] `drivers/sensorhub/brcm/ssp_*.c`)

### 3.1 Carrier

AP -> hub: RPC 0x20 with `[u16 LE length][SSP bytes]`, plain packets ([GTA3]; [HAKI] calls this shape "inferred",
by symmetry with the response). Hub -> AP: RPC 0x21 `[u16 LE length][SSP bytes]` ([K] `bbd_rpc_lh.c:278-294`).
The first thing the application sends is a data frame holding 0x0A "sensor init done"; Samsung waits for it before
talking ([K] `ssp_bbd.c:379-395`, `ssp_data.c:718-721`).

### 3.2 Messages

AP -> hub header, 9 bytes packed little-endian (`struct ssp_msg`, `ssp.h:591-596`; sent at `ssp_bbd.c:118`):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | cmd |
| 1 | 2 | length of the data phase (0 = header only) |
| 3 | 2 | options: bits 1:0 type (0 AP2HUB_READ, 1 AP2HUB_WRITE, 2 HUB2AP_WRITE), bit 2 RETURN, bits 15:3 index (`ssp.h:581-611`) |
| 5 | 4 | data (immediate argument) |

Hub -> AP header, 4 bytes: `[options u16][length u16]` (`ssp_bbd.c:695-823`):

- READ: hub echoes the options, then `length` bytes of result.
- WRITE with length > 0: hub echoes the options; the AP then sends the `length` data bytes as a second request. With
  RETURN set the hub then sends a READ-type header and 1 result byte.
- WRITE with length 0: no answer.
- HUB2AP_WRITE: `length` bytes of data frame follow (3.4).

After every third data frame Samsung sends its clock: cmd 0xA7, WRITE, length 8, with the 8-byte boot-time
nanoseconds appended to the 9-byte header in one go (`bbd_send_timestamp`, `ssp_bbd.c:673-687`).

### 3.3 Bring-up and enabling (`initialize_mcu`, `ssp_dev.c:216-275`)

| Step | cmd | Type, length | Data | Source |
|---|---|---|---|---|
| who am I | 0x0F | READ 1 | must return 0x55 | `ssp_i2c.c:499`, `ssp.h:78` |
| sensor formation | 0xF1 | WRITE 3 | `[acc pos][acc pos][mag pos]` = `04 04 05` | `ssp_i2c.c:534-551` |
| glass type | 0xEC | WRITE 1 | `00` (optional for us) | `ssp_i2c.c` `set_glass_type` |
| sensor scan | 0xF4 | READ 8 | u64 bitmap of sensors found (an earlier port: `0x1fff07f6bf`) | `ssp_i2c.c:848` |
| mag matrix | 0xFD | WRITE 27 | `ssp-mag-array` from the DT | `factory/magnetic_common.c:307-312`, `initialize_magnetic_sensor` 1137 |
| firmware revision | 0xF0 | READ 4 | u32, expect 21031900 | `ssp_i2c.c:879` |
| **enable a sensor** | 0xA1 | WRITE 10 | `[sensor id][s32 period ms][s32 max batch latency ms][s8 batch options]`; 0 latency = no batching | `ssp_i2c.c:211-300`, `ssp_sysfs.c:76-150` |
| change period | 0xA4 | WRITE 10 | same layout | same |
| disable | 0xA2 | WRITE | `[sensor id]...` | `ssp.h:183` |

Sensor ids (`sensor_list.h:22-70`): 0 accelerometer, 1 gyroscope, 2 magnetometer uncalibrated, 3 magnetometer raw,
4 magnetometer (calibrated), 5 pressure, 7 proximity, 9 light, 13 step detector, 18 step counter.

### 3.4 Data frames (`parse_dataframe`, `ssp_data.c:699-830`)

A frame is a list of records, each starting with one type byte. The one we need:

`0x37 [sensor id u8][sample][u64 LE timestamp, ns]`

| Sensor | Sample | Units |
|---|---|---|
| 0 accelerometer | 6 bytes: s16 x, y, z | 8 g range, 4096 = 1 g (`factory/accel_lsm6dsl.c:35-36`). an earlier port, phone flat: `x 31 y -57 z 4076`, so Android axes after the formation command (x right, y up, z out of the screen) [HAKI] |
| 4 magnetometer | 8 bytes: s16 x, y, z, u8 accuracy, u8 overflow (`CONFIG_SSP_SUPPORT_MAGNETIC_OVERFLOW=y`, `ssp.h:378-381`; an earlier port confirms 8 on this firmware) | scale not stated in the GPL source and unconfirmed by an earlier port [GUESS: the AKM's raw 0.15 uT/LSB or 0.06 uT/LSB]. For a compass heading only the ratio of axes matters |
| 3 magnetometer raw | 6 bytes | |
| 18 step counter | 4 bytes: u32 steps since the last report (`ssp_data.c:330-335`); on-change | steps |
| 13 step detector | 1 byte | |

Other record types: 0x0A init done, 0xA8 timestamp offset, 0x05 meta `[what][sensor]`, 0x03 debug text, 0x06 time
sync, 0x08 / 0x09 gyro / mag calibration, 0x04 big data, 0x0C scontext (`ssp_data.c:20-31`, `ssp.h:244`). A parser
must know each one's length or drop the rest of the frame on an unknown type, as Samsung does.

Mount matrix: none in the sources. Positions 4 and 5 are resolved inside the hub. Verify the axes once on the phone
(flat: z about +4096; top edge up: y about +4096).

## 4. Feasibility for bare metal

| Layer | GPL source alone | Closed (lhd only) |
|---|---|---|
| Pins, SPI, supply, clocks | complete | |
| SSI framing | complete (read control byte 0x50 vs 0x70 to settle on hardware) | |
| Packet format, CRC, RPC TLV | complete for receive; transmit is the same format and one worked example exists | sequence-number rules of the transmitter, sync packet contents |
| ROM dialogue and download | only the 0x80 burst and one sample packet | RPC ids, record-to-RPC mapping, start command, timings |
| Patch bytes | complete (in the kernel tree, record format verified) | |
| SSP commands and data | complete | the 0x20 request wrapper (inferred by symmetry) |

The closed part is small and is described in full prose by [GTA3] 6.3, and an earlier port reports the chain working on an
S9 (ASIC and ROM versions, WHOAMI 0x55, firmware 21031900, accelerometer 4076 counts at rest). So reimplementing is
realistic without touching lhd. What I could check myself agrees: the patch records, the CRC, lhd's state names.
What nobody on our side has checked: the RPC ids and the sync payload. If the version step fails on our phone, the
fallback is to disassemble our own lhd's `firmware_downloader` (about one session; it keeps its log strings).

Effort, honest estimate for Starbite OS (C, polling, no interrupts needed):

| Piece | Size | Sessions |
|---|---|---|
| PIO SPI master for USI_CHUB00 + pins + checks (new; we have no SPI driver) | ~200 lines | probe image, 1 |
| SSI + packets + CRC + RPC, host-testable against the GPL sample frames | ~350 lines | same session |
| Downloader (452 reliable writes, start, re-sync) + patch fetch tool | ~200 lines | 1 |
| SSP bring-up, enable, frame parser for accel / mag / steps | ~250 lines | 1 |
| Compass and level in Survival, axis and scale check on the phone | | 1 |

Four sessions if the documented protocol holds; add one or two if the ROM dialogue differs and lhd must be read.
Main risks: CHUB clock or domain state differing from an earlier port's phone, the read control byte, MCU_RESP, and the
magnetometer scale.

## 5. Read-only first probe image

Goal: prove the chip answers on SPI and read its version. No download, no chip memory writes, no PMIC writes.

Safety frame:

- Arm our own dead-man (`sb_wdt_arm`) before the first CHUB access; a fault or hang then returns to the menu.
- Nothing here is persistent: the hub's code is RAM-loaded at every power-up and Android re-does NSTDBY, the
  supply and the download at each boot. (That the chip has no host-writable non-volatile memory is a [GUESS];
  the probe sends no write RPC and no debug-mode write in any case.)
- Never write the PMIC. LDO37 is expected on; if it is off, log and stop.
- Record the as-found CON/DAT/PUD of every pin and the CMU registers, and restore them at the end (NSTDBY back to
  its as-found state last).
- Log every step to the screen and pmsg so a stall is readable afterwards.

Steps:

0. **Look only.** PMU `0x14064084` & 0xF (stop unless 0xF). Then CMU_CHUB `+0x1808`, `+0x2010`, `+0x2090`,
   `+0x20A8`, `+0x305C`; USI word `0x13A13000`; `sb_pmic_read(1, 0x6E)` (expect 0xEC); CON/DAT/PUD of gph0,
   gpa0-5, gpa1-7, gpa4-0, gpg0-2. Print all. This step alone is a useful first image.
1. **Bus up.** Gates open if closed, divider 16 (about 5.5 MHz), USI word 2 if not already. Controller: software
   reset, mode 3, 8-bit, FIFO polling. gph0-0/1/2 function 2 (MISO pull-down), gph0-3 output high, gph0-5 output
   low, gph0-4 input, gpa1-7 input pull-down, gpa0-5 output high.
2. **Wake.** NSTDBY low 20 ms, high, wait 10 ms. MCU_REQ high; sample MCU_RESP for 100 ms and log it, but go on
   either way. Log MISO idle level and HOST_REQ.
3. **Autobaud + sync.** 36 x 0x80 in one CS frame, wait 100 ms. Send the sync packet in a 0x58 frame; then poll
   reads (0x50; if only zeros or 0xFF come back, 0x70) every 20 ms up to 600 ms, hex-dumping what arrives. Success
   = a packet whose CRC checks and whose payload starts with `01`.
4. **Version.** Reliable RPC 0x00; expect RPC 0x03 with ASIC `0x004775B0` (ROM `0x0004527c` on an earlier port's phone).
   Acknowledge it. Print it.
5. **Leave.** MCU_REQ low, NSTDBY low, pins and clocks as found, disarm the dead-man, back to the menu.

Pass = step 4 prints the ASIC id. If step 3 gets no answer: retry the autobaud up to 5 times, then try slower
(the divider cannot go past 16; use the controller's own prescaler if it is honoured [GUESS]), then stop. Do not
improvise further on the one phone; the next step would be reading lhd.

## 6. Notes for the implementation

- Keep the patch out of the repository: a tool that turns `bbd_patch_file_star.h` from a kernel checkout into the
  binary and checks the sha256 above, the result stored on the card or in the OS store at install time.
- The packet and SSP code is pure and can be unit-tested on the Mac against the three GPL sample frames
  (`bcm_gps_regs.c:71`, `:76`, and the self-test sensor packet `sample1` at the end of `bbd_rpc_lh.c`).
- GNSS shares the chip and the link. We do not start GNSS; after the application boots, non-sensor RPCs may still
  arrive (log text 0x19, exceptions 0x17 / 0x18 per [GTA3]) and must be skipped by length, not treated as errors.
- Mainline Linux's `drivers/iio/common/ssp_sensors` speaks an older SSP to an STM32 over plain SPI; it shares the
  instruction numbers but none of the BBD / packet / download layers, so it is of little use here.
