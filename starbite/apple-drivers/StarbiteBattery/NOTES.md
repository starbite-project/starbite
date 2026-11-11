# StarbiteBattery: register facts and sources

Labels: **[V]** checked by us; **[S]** a third party says so; **[I]** our inference.

Sources:
- **RES** = `build/em-research-battery.md` (our research notes; they cite the LineageOS / Samsung kernel's
  `max77705_fuelgauge.c`, `max77705_charger.c`, `max77705-private.h`, `max77705_charger.h`, `i2c-exynos5.c`, the DTB
  and S-Boot's UART log, all read for facts).
- **LIBSB** = `starbite/libsb/max77705.c`, `hsi2c.c` (GPL-2.0-only: **read as register documentation only, no code
  taken**). It has run on the phone (Engineer OS battery page), so its register choices are [V] as facts.
- **TOUCH** = `build/xnu-12377/bsd/dev/starlte_touch.c` (our APSL code: the USI HSI2C sequence, runs on the phone).
- **HERO** = `build/xnu-12377/bsd/dev/herolte_battery.c` (our code: MAX77854, same gauge family, repeated start).
- **DTS** = `device/dtb/starlte.dts`. **SWEET** = `SweetBattery.cpp` (0220/0221): the key set powerd reads.

## Bus

| Fact | Value | Source |
|---|---|---|
| Controller | HSI2C 0x14360000 (USI_CMGP03, CMGP domain), SPI 0x06 (unused: polled) | DTS, board file [V] |
| Owner before us | S-Boot sets clocks, USI mode, pins and talks to the MAX77705 every boot | RES (S-Boot log) [V] |
| Clocks / pins / USI mode | not touched (CMU_CMGP 0x14200000, SYSREG_CMGP 0x14212030 = 4, GPM14/15 fn 2) | RES [V]; not in our nub, so not mapped |
| Timing | S-Boot's FS1/FS2/FS3 saved before the first reset and restored after every reset; 0x21 / 0xe0 / 0xf0 (400 kHz from 200 MHz) only if FS3[23:16] = 0 | RES [V], TOUCH [V] |
| Controller registers | CTL 0x00 (bit 31 reset, 3 master, 6/7 rx/tx channel), FIFO_CTL 0x04, TRAILING 0x08, INT_EN 0x20, INT_STAT 0x24 (0x80 done, 0xf00 errors), FIFO_STAT 0x30 (7 TX full, 8 TX empty, 24 RX empty), TX 0x34, RX 0x38, CONF 0x40 (31 auto), AUTO_CONF 0x44 (count, 16 read, 17 stop, 31 run), TIMEOUT 0x48, TRANS_STATUS 0x50 (17 busy), FS1-3 0x60-0x68, ADDR 0x70 (addr << 10), USI_CON 0xc4 | TOUCH [V]; TX-empty bit 8 LIBSB/RES [V fact] |
| Register reads | pointer byte without STOP, then read with STOP (repeated start, as Linux); fallback with STOP between | HERO [V], RES [V] |
| No TRANSFER_DONE after a no-STOP write | wait for TX empty instead | LIBSB behaviour [V fact] |

## Addresses (7-bit)

PMIC 0x66, USB-C 0x25, charger 0x69, fuel gauge 0x36, debug 0x62 (never) [S RES, from max77705.c].

## Fuel gauge (16-bit little endian), S9 `fg_resistor = 2` (5 mOhm)

| Reg | Name | Conversion | Source |
|---|---|---|---|
| 0x06 | RepSOC | % = raw / 256; 0.1 %: hi x 10 + lo x 10 / 256; plausible <= 0x6500 | RES [S], LIBSB [V] |
| 0x09 / 0x19 | VCELL / AvgVCELL | mV = raw x 78.125 uV = raw x 5 / 64; plausible 32000..60160 (2.5..4.7 V) | RES, LIBSB [V] |
| 0x0a / 0x0b | Current / AvgCurrent | mA = s16 x 15625 x 2 / 100000 = s16 x 5 / 16; + = into the battery | RES [S], LIBSB [V] |
| 0x08 | Temperature | C = s16 / 256; **host-written** on the S9 (CONFIG.TEX; S-Boot writes its ADC reading): stale | RES [S] |
| 0x05 / 0x35 / 0x18 | RepCap / FullCapRep / DesignCap | mAh = raw x fg_resistor / 2 = raw | RES [S] |
| 0x18 | DesignCap | must be 0x0b97 (2967 mAh): S-Boot's "initialized" marker; 0x0b96 = jig reset | RES (DTS 0xb97, S-Boot log) [V] |
| 0xb2 | init result | hi byte 0xff = model load failed | RES [S] |
| 0x00 | Status | bit 3 Bst = battery absent | RES [S] |
| 0x17 | Cycles | cycles = raw / 100 | **[I]** (ModelGauge m5's 1 % LSB; not confirmed for the MAX77705) |

Gates before trusting the gauge (otherwise SweetBattery's static 100 % on AC): RepSOC and VCELL plausible,
DesignCap = 0x0b97, init result != 0xff.., Bst = 0.

## Charger (8-bit)

| Reg | Name | Decode | Source |
|---|---|---|---|
| 0xb0 | CHG_INT | **clear on read: never read** | RES [S] |
| 0xb2 | CHG_INT_OK | b5 WCIN_OK (wireless), b6 CHGIN_OK (USB) | RES [S], LIBSB [V] |
| 0xb3 | CHG_DETAILS_00 | [6:5] CHGIN_DTLS, 3 = valid | RES [S] |
| 0xb4 | CHG_DETAILS_01 | [3:0] CHG_DTLS: 0-2 charging, 3-4 full, 5-7 not charging, 8/0xa/0xb discharging (0xb: charger watchdog expired); [6:4] BAT_DTLS | RES [S], LIBSB [V] |
| 0xb5 | DETAILS_02 | read for the log only | RES [S] |
| 0xb7 | CNFG_00 | [3:0] mode, **b4 WDTEN**; S-Boot writes 0x15 (charge mode + watchdog on) | RES (S-Boot log SBL:237, 267; max77705_charger.h:121) [V log, S bit] |
| 0xbd | CNFG_06 | [3:2] CHGPROT, [1:0] WDTCLR; **the one register written**: `(read & 0x0c) | 0x01` | RES (max77705_charger.c:225-232, 298) [S]; Engineer OS policy / lab (GPL, facts) [V design, write "unverified" on the phone per docs/ENGINEER-OS.md] |

### The charger watchdog clear (allowed by you, 2026-11-11 19:12)

| Item | Value | Source |
|---|---|---|
| Register / bit | charger 0x69, CNFG_06 = 0xbd, WDTCLR [1:0] = 01; CHGPROT [3:2] written back as read; [7:4] = 0 | RES: Linux kicks it with CNFG_06 WDTCLR = 01 on every health poll [S]; Engineer OS writes `(v & 0x0c) | 1` and its policy allows only `(val & 3) == 1`, `val <= 0x0f` [V code] |
| When | only if CNFG_00 WDTEN (bit 4) is set; nothing is written otherwise | RES [S] |
| Interval | **30 s**, from the driver's start (first clear at once), on its own timer, independent of `sbbatpoll` | Engineer OS's feed "every 30 s, as Linux does" (docs/ENGINEER-OS.md, starbite/engineer/lab.c) [V design]; Linux clears on each health poll [S] |
| Timeout it guards | **unknown**: not in the sources (RES says so). The Maxim charger family's watchdog is about 80 s [I]; 30 s leaves margin for anything above ~35 s. M11b measures the real cut-off first (clear off) | [I] |
| Symptom without it | CHG_DTLS = 0x0b (watchdog expired), charging stops on the cable | RES [S] |
| Enforced | `max77705_write_allowed` (one register, one value per read value), `hsi2c_write_reg` (refuses any other address or register before bus traffic), `max77705_wdt_kick` (reads CNFG_00 and CNFG_06 first). Host test: the mock charger's watchdog expires without the clear, never with it over 20 minutes, a 90 s clear is too slow for the mock's 80 s, and no other register is ever written (exhaustive policy check) | |

Other clear-on-read registers refused by `max77705_read_allowed`: PMIC 0x22 / 0x24, USB-C 0x02..0x05 [S RES].

## S9 calibration

`fuelgauge,capacity_max = 1000` (no rescaling of RepSOC), `battery,battery_full_capacity = 3000`,
`chg_float_voltage = 4350`, `fuelgauge,capacity = 0xb97` [V DTS].

## What is published

`CurrentCapacity` in percent with `MaxCapacity` = 100 (Apple's convention on current macOS / iOS); mAh in
`AppleRawCurrentCapacity`, `AppleRawMaxCapacity`, `NominalChargeCapacity`, `DesignCapacity`. `Voltage`, `Amperage`,
`InstantAmperage` (mA, + = charging), `Temperature` (centi-degrees, from the stale FG register: flagged
`StarbiteBatteryTemperatureEstimated`), `CycleCount`, `IsCharging`, `ExternalConnected`, `ExternalChargeCapable`,
`FullyCharged`, `TimeRemaining` / `AvgTimeToEmpty` / `AvgTimeToFull` (computed from RepCap, FullCapRep and
AvgCurrent; 65535 = unknown), `AtWarnLevel` (<= 10 % on battery), `AtCriticalLevel` (<= 2 % and <= 3.45 V on battery,
three samples in a row), `LegacyBatteryInfo`, plus `StarbiteBattery*` diagnostics and `ChargerWatchdogExpired`.

## Hardware-safety notes

- **One register is written in the MAX77705: CNFG_06's WDTCLR (the charger watchdog clear), nothing else.** All
  other traffic is one-byte register pointers before reads. Clear-on-read registers are refused. `sbchgwdt=0`
  turns the clear off (back to fully read only).
- The clear keeps charging going under the charger's own limits as S-Boot configured them (current, voltage, the
  4350 mV float); it changes no charge setting. It is what Linux / Android does on every health poll.
- The HSI2C controller itself is reset and re-programmed with S-Boot's own timing (no other values unless S-Boot
  left none).
- Without the clear (`sbchgwdt=0`, or before this change) charging stops once the charger's watchdog expires and the
  phone runs on battery on the cable; `ChargerWatchdogExpired` shows it.
- Charging under an Apple OS has no thermal supervision of ours (the FG temperature is stale; Android's charging
  temperature limits come from its battery driver, which is not running). The charger's own protections (JEITA,
  thermal regulation, timers as S-Boot set them) still apply. Watch M11b's battery temperature from TWRP afterwards.
- A wrong low reading could make powerd shut down: guarded by the gates (fallback is 100 %, never 0 %) and by the
  three-sample rule for AtCriticalLevel.
- The M11 load test busies every CPU for at most 60 s at the clocks S-Boot / 0302 set (<= 1794 MHz, no
  overclock); the battery temperature rule (<= 65 C) is far away in 60 s, but the FG temperature is stale, so the
  test cannot watch it.

## Open questions

1. Done (allowed 2026-11-11): the charger watchdog clear. Still open: its real timeout (M11b control run).
2. Cycles LSB (1 % assumed).
3. A live battery temperature needs the CMGP ADC (0x14230000, channel 4) and the DTB's temp table: a later
   StarbiteADC.
4. When StarbiteHSI2C exists, hw/exynos_hsi2c.c is replaced by its provider (same read-only rule).
