# StarbiteBattery: integration

## 1. Files

```
starbite/apple-drivers/StarbiteBattery/StarbiteBattery.cpp        optional iokitcpp
starbite/apple-drivers/StarbiteBattery/hw/exynos_hsi2c.c          standard
starbite/apple-drivers/StarbiteBattery/hw/max77705.c              standard
```
Include path: `starbite/apple-drivers/StarbiteBattery`.

## 2. Personality (replaces SweetBattery's on STARLTE)

```
"   {"
"     'IOClass'         = StarbiteBattery;"
"     'IOProviderClass' = IOPlatformDevice;"
"     'IONameMatch'     = 'hsi2c-pmic';"
"     'IOMatchCategory' = StarbiteBattery;"
"   },"
```
`hsi2c-pmic` is the arm-io node name from `tools/board/starlte.board` (compatible `starbite,hsi2c` is shared with
the other HSI2C blocks, so the name is what picks this one). Remove the SweetBattery personality for STARLTE (two
IOPMPowerSources would both be read by powerd). The IOMatchCategory leaves room for StarbiteHSI2C on the same nub.

## 3. Bus ownership

Nothing else in XNU touches HSI2C 0x14360000 today (SweetBattery is static on STARLTE). When StarbiteHSI2C (row 13)
lands, it must own the controller: replace `hw/exynos_hsi2c.c` by its read API (a register read with repeated
start) and keep `max77705_read_allowed` in front of it.

## 4. Boot-args

`sbbattery=0` (static, no bus access), `sbbatpoll=S`, `sbbattest=1`, `sbbatload=S`, `sbstop=battery`,
`sbchgwdt=0` (no charger watchdog clear: fully read only), `sbchgtest=S` (M11b verdict).

## 5. The one write: charger watchdog clear

On by default (allowed 2026-11-11): CNFG_06 (0x69:0xbd) = (read & 0x0c) | 0x01 every 30 s on the driver's work loop,
only while CNFG_00 WDTEN is set. Logged once: `StarbiteBattery: charger watchdog: cleared every 30 s (...)`;
`ChargerWatchdogClears` counts them. No other driver may write the charger (when StarbiteHSI2C lands, keep this the
only write it forwards to 0x69).
