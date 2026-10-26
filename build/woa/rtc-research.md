# S9 (SM-G960F) PMIC RTC: facts for a SPEEDY-backed edk2 RealTimeClockLib

Sources (all at one commit of the stock-based starlte kernel, cross-checked against LineageOS):

- K = `https://raw.githubusercontent.com/vutung2311/starlte/a0f1434a20ad7479193d2b7f385f5f585078ffdb/`
  (the same tree as the local partial clone `ref/starlte-kernel`)
- L = `https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9810/lineage-20/`.
  `include/linux/mfd/samsung/rtc-s2mp.h` there is byte-identical to K, and lines 92-230 of
  `drivers/rtc/rtc-s2mps18.c` (the conversion, update, read and set functions) are identical too.

## 1. Which driver, which PMIC

- The PMIC is the **S2MPS18**. Driver: `drivers/rtc/rtc-s2mps18.c`, header `include/linux/mfd/samsung/rtc-s2mp.h`.
  - K `arch/arm64/configs/exynos9810-starlte_defconfig` L4185-4186: `CONFIG_RTC_ALARM_BOOT=y`, `CONFIG_RTC_DRV_S2MPS18=y`. Its RTC section (L4087-4194) does not set `CONFIG_RTC_DRV_S5M` or `CONFIG_RTC_DRV_SEC`.
  - K `drivers/rtc/rtc-s2mps18.c` L1061: `info->rtc_dev = devm_rtc_device_register(&pdev->dev, "s2mp-rtc",`. This is where the name of `/sys/class/rtc/rtc0` comes from. The platform driver itself is named `"s2mps18-rtc"` (L1154).
- **The header has two register maps; this phone uses the second one (the `#else` branch).** `rtc-s2mp.h` L19 reads `#if defined(CONFIG_RTC_DRV_S2MP)`. That symbol does not exist in K `drivers/rtc/Kconfig` or in the defconfig, so the `#else` map (L45-73) applies. That map has an MSEC register at 0x04 and SEC at 0x05. Do not use the first map (SEC at 0x04).

## 2. Bus addressing: the RTC is SPEEDY device 0x2, the PMIC is 0x1

- K `drivers/mfd/s2mps18_core.c` L40-43:
  `#define I2C_ADDR_TOP 0x00` / `I2C_ADDR_PMIC 0x01` / `I2C_ADDR_RTC 0x02` / `I2C_ADDR_DEBUG 0x0F`
- L477-478: `s2mps18->pmic = i2c_new_dummy(i2c->adapter, I2C_ADDR_PMIC);` and `s2mps18->rtc = i2c_new_dummy(i2c->adapter, I2C_ADDR_RTC);`
- L481-485: when the DT has `i2c-speedy-address`, the `pmic`, `rtc` and `debug_i2c` clients all get `I2C_CLIENT_SPEEDY`. The starlte DT sets it: `exynos9810-starlte_common.dtsi` L546-551 has `speedy@141C0000 { status = "okay"; s2mps18mfd@00 { ... acpm-ipc-channel = <2>; i2c-speedy-address; ...`.
- How the 12-bit SPEEDY address is built, K `drivers/i2c/i2c-core.c` L2615-2618:
  `The upper 4bit of 12bit speedy slave address is for device id. 8bit is for device register offset.`
  `msg.addr = buf[0] + ((client->addr & 0xf) << 8);`
- K `drivers/i2c/busses/i2c-speedy.c` L77-80: `SPEEDY_ADDRESS(x) ((x & 0xFFF) << 7)`, `SPEEDY_ACCESS_RANDOM (1 << 19)`, `SPEEDY_DIRECTION_WRITE (1 << 20)`.
- The stock kernel does not drive SPEEDY from the AP. Every `s2mps18_read_reg`, `bulk_read`, `write_reg` and `bulk_write` call goes through APM firmware IPC (K `drivers/mfd/s2mps18_core.c` L58: `exynos_acpm_read_reg(i2c->addr, reg, dest)`), using the same address layout. K `drivers/soc/samsung/acpm/acpm_mfd.c` L28: `config.cmd[0] = set_protocol(type, TYPE) | set_protocol(reg, REG);` with `SHIFT_TYPE (8)` and `MASK_TYPE (0xF)` (`include/soc/samsung/acpm_mfd.h` L7, L20).

This matches the firmware's existing command word exactly: `((Slave << 8) | Register) << 7 | RANDOM [| WRITE]`. The firmware hardcodes `STAR2LTE_SPEEDY_PMIC_SLAVE 0x01`. **For the RTC, use slave 0x02.**

## 3. RTC register map (device 0x2; starlte = `#else` branch of `rtc-s2mp.h`)

The header lines below are from K `include/linux/mfd/samsung/rtc-s2mp.h`. The encodings come from `s2m_data_to_tm` and `s2m_tm_to_data` in K `drivers/rtc/rtc-s2mps18.c` L92-128.

| Reg | Name (hdr line) | Encoding as used by Linux on starlte |
|---|---|---|
| 0x00 | `S2MP_RTC_REG_CTRL` (L46) | bit0 `BCD_EN` (L76-77), bit1 `MODEL24` (L78-79). Linux writes `MODEL24_MASK` (0x02): "Binary mode, 24hour mode" (rtc-s2mps18.c L903-905). |
| 0x01 | `WTSR_SMPL` (L47) | Not time related. Linux leaves it alone apart from the WTSR/SMPL enable. |
| 0x02 | `S2MP_RTC_REG_UPDATE` (L48) | bit0 `RTC_RUDR` (L90-91), bit1 `RTC_WUDR_MASK_REV` (L96-97), bit2 `RTC_FREEZE` (L94-95), bit3 `RTC_WAKE` (alarm-boot enable, L108-110), bit4 `RTC_AUDR_MASK_REV` (L92-93). The other bits are unknown and must be preserved (see below). |
| 0x03 | `CAPSEL` (L49) | Linux treats an upper nibble of 0xF as "already initialised" (rtc-s2mps18.c L896, L916). |
| 0x04 | `MSEC` (L50) | Linux never uses it (its decode is commented out, L94). Format not confirmed. |
| 0x05 | `SEC` (L51) | Binary 0-59, read mask `& 0x7f` (L95). |
| 0x06 | `MIN` (L52) | Binary 0-59, `& 0x7f` (L96). |
| 0x07 | `HOUR` (L53) | Binary 0-23 (24 h), read mask `& 0x1f` (L97). On write Linux also sets bit6 `HOUR_PM_MASK` (L99-100) when hour >= 12 (L112-115). |
| 0x08 | `WEEK` (L54) | **One-hot bitmask**: write `BIT(tm_wday)` (L117), where bit0 = Sunday. Read `__fls(x & 0x7f)` (L98). |
| 0x09 | `DAY` (L55) | Binary 1-31, `& 0x1f` (L99). |
| 0x0A | `MON` (L56) | Binary **1-12**: Linux reads `(x & 0x0f) - 1` (L100) and writes `tm_mon + 1` (L119). |
| 0x0B | `YEAR` (L57) | Binary, **offset 2000**, 7 bits: read `(x & 0x7f) + 100` as years since 1900 (L101). Linux refuses years before 2000 (L120-126). The usable range is therefore 2000-2127. |
| 0x0C-0x12 / 0x13-0x19 | Alarm0 and Alarm1 SEC..YEAR (L58-71) | Same encodings, plus bit7 `ALARM_ENABLE` (L102-103). Alarm1 is the power-off alarm (`CONFIG_RTC_ALARM_BOOT`). |
| 0x1A | `OSCCTRL` (L72) | Oscillator trim. Do not touch. |

The time block is 7 consecutive bytes (`NR_RTC_CNT_REGS`, enum L139-149): SEC, MIN, HOUR, WEEKDAY, DATE, MONTH, YEAR at 0x05-0x0B.

### Update-register usage (K `drivers/rtc/rtc-s2mps18.c`)

- `s2m_rtc_update` (L130-166): `case S2M_RTC_READ: data = RTC_RUDR_MASK;`, `case S2M_RTC_WRITE_TIME: data = RTC_WUDR_MASK_REV;`, then `data |= info->update_reg;`, `s2mps18_write_reg(info->i2c, S2MP_RTC_REG_UPDATE, data);` and `usleep_range(1000, 2000);`.
- `info->update_reg` is a shadow of the UPDATE register with the strobe bits cleared. At probe (L862-872) Linux sets `update_reg = update_val & ~(info->wudr_mask | RTC_FREEZE_MASK | RTC_RUDR_MASK | info->audr_mask);` and writes it back. When the power-off alarm is changed, Linux sets or clears bit3 `RTC_WAKE` in that shadow (L547-559). **The firmware must read-modify-write the register: keep bit3 and the unknown high bits, clear bits 0/1/2/4, then OR in the strobe.**
- Linux does **no polling**. It just waits 1-2 ms after each strobe write. Nothing in the source shows a readable "busy" or "done" bit, or whether the strobes clear themselves. The comment at L231-235 says "clear signal of RUDR & WUDR is 1 clock delay", which suggests they self-clear, but this is not confirmed.

### Coherent read (`s2m_rtc_read_time`, L168-198)

1. Write UPDATE = `(UPDATE & ~0x17) | 0x01` (RUDR). This latches the counter into the read buffer.
2. Wait 1-2 ms.
3. Read 0x05..0x0B (Linux reads all 7 in one bulk read).
4. Decode with the masks in the table. Linux then calls `rtc_valid_tm`.

### Write (`s2m_rtc_set_time`, L200-229)

1. Encode the 7 bytes. The weekday byte must be computed.
2. Write them to 0x05..0x0B. This fills the write buffer; Linux uses one bulk write.
3. Write UPDATE = `(UPDATE & ~0x17) | 0x02` (WUDR). This commits the buffer to the counter.
4. Wait 1-2 ms.

### "RTC was reset" check (`s2m_rtc_init_reg`, L881-930)

- If `CTRL & 0x02` (MODEL24) is set and `(CAPSEL & 0xF0) == 0xF0`, the RTC is valid.
- Otherwise Linux:
  1. writes CTRL = 0x02,
  2. strobes AUDR (bit4) (L912),
  3. sets CAPSEL |= 0xF0 (L916-917),
  4. sets the time to the DT `init_time`, which is 2018-01-01 12:00:00 Mon (`exynos9810-starlte_common.dtsi` L582-589, `init_time,year = <118>`).

### Not confirmed

- **Byte-at-a-time access.** The firmware helper can only do single-byte random access. Linux always uses one bulk transfer (an APM burst) for the 7 bytes. Since RUDR and WUDR latch through buffers, 7 separate single-byte reads after one RUDR should be coherent, and 7 separate writes followed by one WUDR should commit atomically. That follows from the register design and the driver's sequence, not from a datasheet (none is public). A cheap guard: after decoding, run a second RUDR and SEC read, and retry if the time moved backwards or the decode fails validation.
- **MSEC (0x04) format.** Unknown.

## 4. UTC or local time

- The Linux RTC core treats the RTC as **UTC**. K `drivers/rtc/hctosys.c` L50-56 converts with `rtc_tm_to_time64` and prints `... UTC`. K `drivers/rtc/systohc.c` L36 has the comment "rtc_hctosys exclusively uses UTC". The starlte defconfig enables both `CONFIG_RTC_HCTOSYS=y` and `CONFIG_RTC_SYSTOHC=y` with `rtc0` (L4089-4092), so Android/Linux writes **UTC** into this RTC.
- The Samsung sources do not show a local-time mode for this RTC.
- General knowledge, not from Samsung sources: Windows treats the firmware RTC as **local time** unless `HKLM\SYSTEM\CurrentControlSet\Control\TimeZoneInformation\RealTimeIsUniversal` = 1. Either set that value in the Windows image, or accept that Windows and Android will each shift the clock by the UTC offset when the user switches between them. The lib should report `TimeZone = 0` (UTC) or `EFI_UNSPECIFIED_TIMEZONE`, with `Daylight = 0`. I did not check how this tree's EmbeddedPkg RealTimeClockRuntimeDxe stores TimeZone/Daylight (no edk2 checkout on disk).

## 5. Mapping onto the existing firmware SPEEDY helper

Source: `ref/exynos9810-woa/firmware/Platform/Star2LtePkg/Library/PlatformBootManagerLib/PlatformBootManagerLib.c` L478-686. Base: `EXYNOS_SPEEDY_BASE 0x141C0000` (`Silicon/Exynos9810Pkg/Include/Platform/Exynos9810.h` L102), which matches DT `reg = <0x0 0x141C0000 0x2000>`.

### Changes needed

- **Slave parameter.** `Star2LteSpeedyTransfer8` hardcodes `STAR2LTE_SPEEDY_PMIC_SLAVE` (0x01) at L635. Add a `Slave` argument and pass 0x02 for the RTC.
  - Resulting command words (`(0x2RR << 7) | BIT19`, plus BIT20 for writes):
    - UPDATE (0x02): read 0x00090100, write 0x00190100
    - SEC (0x05): read 0x00090280
    - YEAR (0x0B): read 0x00090580
- **Move the helper out of PlatformBootManagerLib.** The helper is `STATIC`, lives inside `#if STAR2LTE_TOUCH_RAIL_FIX`, and depends on `Star2LteInitializeSpeedy` (L726-749). Move it into a shared SPEEDY library that both PlatformBootManagerLib and the new RealTimeClockLib link.
- **Runtime services.** `LibGetTime` and `LibSetTime` run as runtime services under Windows.
  - Replace `gBS->Stall` (L667) with `MicroSecondDelay` from TimerLib (the arch timer works at runtime).
  - Map the SPEEDY page `EFI_MEMORY_RUNTIME`: `gDS->AddMemorySpace` / `SetMemorySpaceAttributes`.
  - Convert the base pointer with `EfiConvertPointer` on SetVirtualAddressMap.
  - Use the existing `Star2LteSpeedyQuiesce` / soft-reset path on any error.

### Concurrency risk (not resolved)

On stock Android the AP never touches SPEEDY; APM (ACPM IPC channel 2) owns it. The controller has an IP-batcher hardware semaphore (`IPBATCHER_CON 0x500`, `MP_APBSEMA_*`, i2c-speedy.c L137-190) for sharing it with APM. The current firmware ignores the semaphore. That works at boot, but under Windows an RTC runtime call could race with APM PMIC traffic (DVFS) if APM is still active. Whether APM is active under Windows is unconfirmed. A safer first version: do the RTC access during DXE/BDS and keep runtime SetTime minimal.

### GetTime / SetTime sketch

- **GetTime**
  1. `u = rd(2,0x02)`
  2. `wr(2,0x02,(u & ~0x17)|0x01)`
  3. delay 2000 us
  4. read 0x05..0x0B
  5. decode the fields:
     - Second = s&0x7f, Minute = m&0x7f, Hour = h&0x1f
     - Day = d&0x1f, Month = mo&0x0f, Year = 2000+(y&0x7f)
     - Nanosecond = 0
  6. Validate. If invalid, or if `!(CTRL&0x02) || (CAPSEL&0xF0)!=0xF0`, return EFI_DEVICE_ERROR. Alternatively, run the Linux init sequence.
- **SetTime**
  1. Reject years outside 2000..2127.
  2. Compute the weekday (0 = Sunday), e.g. EmbeddedPkg TimeBaseLib `EfiTimeToWday`.
  3. Encode: hour byte = `Hour | (Hour>=12 ? 0x40 : 0)`, week byte = `1<<wday`, month 1-12, year byte = Year-2000.
  4. Write 0x05..0x0B.
  5. `u = rd(0x02)`, `wr(0x02,(u & ~0x17)|0x02)`.
  6. Delay 2000 us.
