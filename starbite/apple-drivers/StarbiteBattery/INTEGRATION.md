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

`sbbattery=0` (static, no bus access), `sbbatpoll=S`, `sbbattest=1`, `sbbatload=S`, `sbstop=battery`.

## 5. Not done here (needs a decision)

The MAX77705 charger watchdog (NOTES.md, open question 1): kicking it is a write to the charger and is not in this
driver. Without it, charging stops some minutes into an Apple OS session.
