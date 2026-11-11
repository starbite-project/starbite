# StarbiteACPM: register facts and sources

Labels: **[V]** checked by us (repo code that ran on the phone, a phone log, the vendor DTB); **[S]** a third
party says so (an earlier port's APSL file, which cites the vendor kernel and the S9+ mainline port); **[I]** our inference.

Sources, short names:
- **0302** = `build/xnu-12377/bsd/dev/starlte_touch.c` (acpm_* and st_power_on; patch 0302, runs on the phone).
- **GPU** = `build/xnu-12377/bsd/dev/starlte_gpu.c`, **SD** = `build/xnu-12377/bsd/dev/starlte_sd.c`.
- **DTS** = `device/dtb/starlte.dts` (the stock DTB TWRP holds).
- **S2P** = `ref/starlte-kernel/include/linux/mfd/samsung/s2mps18-private.h` (Samsung GPL header: register
  numbers only).
- **REFPORT** = an earlier port's `drivers/soc/exynos/exynos_acpm.c` and `socs/exynos9810/include/exynos9810_soc.h`
  (APSL-2.0 per an earlier port's `LICENSES.md` row "its `*` files": "an earlier port's (Claudius for an earlier port)"). Used for
  facts only, no code copied. **Credit: an earlier port project (Claudius for an earlier port), APSL-2.0.**

## Addresses

| Fact | Value | Source |
|---|---|---|
| Mailbox | 0x14100000, 0x1000 | DTS `acpm_ipc` reg [V]; board block `acpm-ipc` |
| APM SRAM | 0x02039000, 0x30000 | DTS `acpm_ipc` reg second pair [V]; 0302 [V] |
| initdata-base | 0x7f00 (SRAM offset) | DTS `initdata-base` [V]; 0302 [V] |
| IPC interrupt | SPI 0x44 (not used: polled) | DTS [V] |
| SRAM below +0x3000 faults | the first three pages; never touched | REFPORT [S] (measured on the S9+ port). DTS `dump-base = 0x203c000` starts exactly there [V] |
| 32-bit SRAM access only | narrower is a bus error | REFPORT [S] |
| `acpm` node 0x14020000 ("timer_apm") | not used | DTS [V] |

The board file (`tools/board/starlte.board`) has `acpm-ipc` reg = mailbox only: 0x02039000 is below `[soc].base`
(0x10000000), so the ADT cannot carry it as an arm-io `reg`. The driver reads reg 1 **or** a `sram` property
(INTEGRATION.md).

## Mailbox registers

| Offset | Name | Use | Source |
|---|---|---|---|
| 0x08 | INTGR0 | ring the APM: `1 << (16 + channel id)` | 0302 [V], REFPORT [S] |
| 0x20 | INTCR1 | clear our pending bit: `1 << id` | 0302 [V], REFPORT [S] |
| 0x24 | INTMR1 | mask (read only here; Linux masks what it polls) | REFPORT [S] |
| 0x28 | INTSR1 | pending for us: bit = id | 0302 (logged) [V], REFPORT [S] |

## initdata (SRAM + 0x7f00)

| Offset | Meaning | Source |
|---|---|---|
| +0x08 | channel table (SRAM offset) | 0302 [V] |
| +0x0c | number of channels | REFPORT [S] |
| +0x18 | ipc_ap_max: channels that are the AP's | 0302 [V] |
| +0x60 | version / build text (24 bytes logged) | 0302 [V]; REFPORT puts the build string at +0x6c [S] |

## Channel descriptor (0x48 bytes)

| Offset | Field | Source |
|---|---|---|
| +0x00 | id (low byte) | 0302 [V]; "low byte" REFPORT [S] |
| +0x08 / +0x0c | owner / type | REFPORT [S] (logged only) |
| +0x10 / +0x14 / +0x18 | AP->APM rear (APM reads) / front (we write) / base | 0302 [V] |
| +0x28 / +0x2c / +0x30 | APM->AP rear (we read) / front (APM writes) / base | 0302 [V] |
| +0x34 / +0x38 | queue length / element size | 0302 [V] |
| +0x44 | AP polls | 0302 [V] |
| len <= 1 | a single-slot buffer channel: words at the bases, completion in INTSR1 | REFPORT [S] |

## Protocol

- Request: 4 words at `in_base + elem * front`; `front = (front + 1) % len`; DSB; INTGR0. Bits 21:16 of word 0 =
  our sequence 1..63, echoed by the answer [V 0302].
- Answer: scan `out_rear..out_front` for our sequence, copy 4 words, consume [V 0302].
- **Stale answers** (from a kernel before ours, or a missed one) must be drained before a request; a full answer
  queue stops the APM until the next restart [S REFPORT]. 0302 instead skipped and repacked them [V]. Here: drained at
  setup and before each request; tested with stale entries in the mock.
- Timing: 600 ms per answer, three sends, 20 ms for queue room [S REFPORT]; 0302 used 100 ms, one send [V].
- PMIC (channel 2 = `s2mps18mfd acpm-ipc-channel` [V DTS]): w0 = `type << 8 | reg`; w1 = `func | value << 8 |
  mask << 16`, func 0 read, 1 write [V 0302], 2 update [S REFPORT, unused here]; answer w1 [15:8] value,
  [31:24] status, 0 = good [V 0302].
- Types: 0 TOP, 1 PMIC, 2 RTC [V 0302 for 0/1; S REFPORT for 2, = Samsung's "i2c address 0x02" of the RTC].
- DVFS: `{ domain, kHz, 0 set | 1 get, 0 }` on the channel at **table index 0** (Linux's acpm_dvfs.ch_num is never
  set) [V 0302, phone]; domains cpucl0 2, cpucl1 3, g3d 4 [V 0302, GPU]. FREQ_GET answered 0 kHz once [V 0302].

## S2MPS18 registers touched

| Type:reg | Name | Read | Write (policy) | Source |
|---|---|---|---|---|
| 0:0x00 | PMICID | yes | never | 0302 [V], S2P |
| 1:0x00..0x06 | INT1..INT7 | **refused** (clear on read) | never | S2P [V header] |
| 1:0x2b | B6OUT (vdd_g3d, 300 mV + n x 6.25 mV) | yes | 0x68 (950 mV), or its boot value if 0x20..0x70 | GPU [V] |
| 1:0x4a | L2CTRL1 (SD vqmmc) | yes | 0xc0, 0xfc, 0x3c, off at its vsel | SD [V] |
| 1:0x68 | L31CTRL (SD vmmc) | yes | 0x28, 0xe8, if vsel 0 / 0x24..0x3c | SD [V] |
| 1:0x6c | L35CTRL (tsp_io) | yes | 0xec, if vsel 0x2c or 0 | 0302 [V] |
| 1:0x74 | L43CTRL (tsp_avdd) | yes | 0xf0, if vsel 0 or 0x28..0x3c | 0302 [V] |
| 2:0x02 | RTC UPDATE | yes | +RUDR (bit 0) or +WUDR (bit 1) only | StarbiteRTC/NOTES.md |
| 2:0x05..0x0b | RTC SEC..YEAR | yes | a value in its field's range | StarbiteRTC/NOTES.md |
| everything else | | yes | **refused** | |

LDOxCTRL: [7:6] enable (3 = on), [5:0] vsel, 25 mV steps (LDO35 base 0.7 V; LDO43, LDO2, LDO31 base 1.8 V)
[V 0302, SD]. The exhaustive host test proves no other PMIC register and no other voltage passes the policy.

## DVFS limits (policy)

cpucl0 / cpucl1 <= 1794000 kHz ("not without thermal control", 0302); cpucl1 <= 2704000 kHz only after
`setThermalGovernor(true)` (0411 starcpugov's ceiling); g3d only 572000 / 546000 / 455000 / 338000 / 299000 /
260000 (GPU); **no other domain**: MIF 2093000 / INT 667000 froze the phone, display green (boot69) [V 0302].

## Not covered

- **S2MPB02** (camera/flash PMIC) is on HSI2C 0x10920000 at 0x59 [V DTS], not behind ACPM. No code today touches
  it; nothing here does. It belongs to a StarbiteHSI2C client if ever needed.
- The update function (func 2, value + mask) is not used: read-check-write instead, as 0302 does.
- The IPC interrupt: polled, as 0302. An interrupt path needs StarbiteGICv2 and is not needed for these rates.

## Open questions

1. Is the S9's ACPM firmware happy with several AP clients on channel 2 once the bsd/dev drivers and this one
   coexist? They must not: one owner per channel (INTEGRATION.md, hazard H1).
2. INTMR1 at boot (logged by the driver's first run): if the APM's answer interrupt is unmasked with nobody
   handling it, the GIC sees a pending SPI 0x44 forever (harmless while not enabled).
