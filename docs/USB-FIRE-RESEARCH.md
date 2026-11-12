# USB-C fire starter: research notes (MAX77705 OTG, S9 thermistors, heating elements)

Research only, 2026-11-12. No phone, no hardware. Every claim has a source. **[GUESS]** marks an inference or
estimate that no source states directly. **[CALC]** marks arithmetic from sourced numbers.

## Sources used

| Tag | What | Where |
|---|---|---|
| **[S-CHG]** | Samsung GPL charger driver, Galaxy S9 (starlte) kernel, LineageOS fork of Samsung's source | <https://github.com/LineageOS/android_kernel_samsung_universal9810/blob/lineage-17.1/drivers/battery_v2/max77705_charger.c> |
| **[S-CHGH]** | Its header, with register bits | <https://github.com/LineageOS/android_kernel_samsung_universal9810/blob/lineage-17.1/drivers/battery_v2/include/charger/max77705_charger.h> |
| **[S-PRIV]** | MFD register map and IRQ list | <https://github.com/LineageOS/android_kernel_samsung_universal9810/blob/lineage-17.1/include/linux/mfd/max77705-private.h>, `drivers/mfd/max77705-irq.c` (same tree) |
| **[S-NEW]** | Newer Samsung MAX77705 driver (2023/24 Galaxy, SM8650 tree) | <https://github.com/samsung-sm8650/android_kernel_samsung_sm8650/blob/HEAD/drivers/battery/charger/max77705_charger/max77705_charger.c> |
| **[S-DTS]** | S9 battery device tree | <https://github.com/LineageOS/android_kernel_samsung_universal9810/blob/lineage-17.1/arch/arm64/boot/dts/exynos/battery_data_starlte_common.dtsi> |
| **[S-BAT]** | `sec_battery.c` and `sec_adc.c` | same tree, `drivers/battery_v2/sec_battery.c`, `drivers/battery_v2/sec_adc.c` |
| **[S-CCIC]** | USB-C controller (CCIC/MUIC) drivers | same tree, `drivers/ccic/max77705_usbc.c`, `drivers/ccic/max77705_pd.c`, `drivers/muic/max77705-muic.c`, `drivers/usb/notify/usb_notify.c` |
| **[S-DEF]** | starlte defconfig | same tree, `arch/arm64/configs/exynos9810-starlte_defconfig` |
| **[DS-818]** | **MAX77818** datasheet, 19-6902 Rev 2 (2/16). Its charger has the same register map as the MAX77705 at 0xB0..0xC3: CHG_CNFG_00 at 0xB7, CHG_CNFG_02 at 0xB9 with OTG_ILIM in bits 7:6 = 500/900/1200/1500 mA, and BYP_DTLS in 0xB5[3:0]. | mirror <https://datasheet.chipsfind.com/MAX77818EWZ-337996.pdf> (ADI original: analog.com, `max77818.pdf`) |
| **[DS-975]** | **MAX77975/MAX77976** datasheet, 19-100876 Rev 1 (11/20). A newer Maxim charger with the same OTG/BYP scheme and a published timing table. | mirror <https://docs.rs-online.com/8c09/A700000007304228.pdf> (ADI original: `max77975-max77976.pdf`) |

There is no public MAX77705 datasheet. It is a Samsung-specific part, and I found no copy. analog.com timed out from this
machine, so the two sibling datasheets came from distributor mirrors. MAX77865, MAX77729 and MAX77693/77843 were not
needed: the MAX77818 register descriptions match the MAX77705 header bit for bit wherever both are defined
(compare [S-CHGH] lines 53-162 with [DS-818] pp. 48-56). **[GUESS]** The MAX77705 behaves like the MAX77818 in OTG
mode. This is very likely, because it shares the register map, the OTG_ILIM steps and the BYP_DTLS family, but it is
not proven for this exact die.

---

## 1. What the MAX77705 itself does on an OTG overload or short

### 1.1 Current limit and fault timing (sibling datasheets)

- **The limit.** The current limit sits on the BYP-to-CHGIN switch (the "OTG switch"), not on the boost
  converter. In OTG mode the boost regulates BYP to V_BYP.OTG = 5.1 V typ (4.94..5.26 V), and "the current through the
  BYP to CHGIN switch is limited to the value programmed by OTG_ILIM" ([DS-818] p. 32, EC table p. 9).
- **OTG_ILIM accuracy.** [DS-818] p. 9 lists I_CHGIN.OTG.LIM as 500/550, 900/990, 1200/1320 and **1500/1650 mA**
  (typ/max, 3.4 V < VBATT < 4.5 V, 25 C). [DS-975] p. 16 lists the 1500 mA setting as 1500 min, 1612 typ, 1725 max.
- **Overload response in the MAX77818.** "If the external OTG load at CHGIN exceeds I_CHGIN.OTG.ILIM, then a
  BYP_I interrupt is generated, BYP_OK = 0, and BYP_DTLS = 0bxxx1. In response to an overload at CHGIN during OTG mode
  operation, the BYP to CHGIN switch is latched off. The BYP to CHGIN switch will automatically try to retry in
  ~300ms. If the overload at CHGIN persists, then the switch will toggle on and off with ~30ms on and ~300ms off."
  ([DS-818] p. 32, "OTG Mode").
- **Overload timing in the MAX77975** ([DS-975] pp. 16-17, 37-38, Table 1 and Table 6):
  - t_OTG_ALARM = 20 ms, from overcurrent to the BYP_I interrupt (debounce).
  - t_OTG_FAULT = 30 ms, from overcurrent to the Q_CHGIN FET opening.
  - t_OTG_RETRY = 300 ms, from the FET opening to its re-closing, only when OTG_REC_EN = 1.
  - With OTG_REC_EN = 0, "the BYP to CHGIN switch remains off and the switcher is turned off until MODE is toggled."
  - BYP_DTLS[0] is latched until the AP reads CHG_DETAILS_02.
  - The BST_ILIM debounce is 30 ms and the buck negative-ILIM debounce is 0.5 ms.
- **Hiccup or latch on the MAX77705.** The MAX77705 header has no OTG_REC_EN bit
  ([S-CHGH]; CHG_CNFG_10 at 0xC1 is undocumented there). **[GUESS]** The MAX77705 behaves like the MAX77818: it
  hiccups 30 ms on and 300 ms off by itself, and MODE stays at 0x0A. That gives an average delivered power into a dead
  short of about 9 % of the limit, while the FET carries the current-limited 5 V x 1.5 A during each 30 ms burst.
- **Thermal cost of sitting in current limit [GUESS].** While the current limit is active, the switch drops the voltage
  difference itself. Into a near-short that is several watts inside the PMIC, which is presumably why the fault timer
  is only about 30 ms. **Design rule:** stay below the limit. Never use the limit as a constant-current source.
- **No separate VBUS short detector.** I found no VBUS/CHGIN short-to-ground threshold for OTG mode in either
  datasheet. A short shows up only as the OTG_ILIM event. The CHGIN UVLO (4.5..5.1 V programmable, [DS-818] p. 5)
  and CHGIN_DTLS apply to an *input* at CHGIN. They are not an OTG short detector.

### 1.2 Status registers in OTG mode

- **CHG_INT 0xB0 bit 0 BYP_I.** Set when BYP_OK changed since the last read (read-to-clear) ([DS-818] p. 47; same bit in
  [S-CHGH] line 53).
- **CHG_INT_OK 0xB2 bit 0 BYP_OK.** "0 = Something powered by the bypass node has hit current limit. i.e., BYP_DTLS ≠
  0x00" ([DS-818] p. 49).
- **CHG_DETAILS_02 0xB5 bits 3:0 BYP_DTLS** ([DS-818] p. 52; [DS-975] register table). The bits are independent, and
  any change raises BYP_I.
  - bit0 **OTGILIM**: "The BYP to CHGIN switch (OTG switch) current limit was reached within the last 37.5ms."
    This is the short/overload bit. Samsung's driver tests exactly this bit (1.3 below).
  - bit1 **BSTILIM**: "The BYP reverse boost converter has hit its current limit -- this condition persists for
    37.5ms" (30 ms in [DS-975]). This is the boost inductor limit, set by heavy BYP load or a sagging battery.
  - bit2 **BCKNegILIM**: the buck hit its maximum negative current. This is a buck-mode bit, not used in OTG.
  - bit3: on the MAX77818 this is *input* voltage regulation (VCHGIN_REG) active, a charging-side event
    ([DS-818] p. 26). On the MAX77975 it is BST_SWON_DONE, MODE 0x9 only.
  - Samsung names the four bits only BYP_DTLS0..3 ([S-CHGH] lines 107-112).
- **CHG_DETAILS_02 bit 7 TREG** (MAX77818): the die is above REGTEMP and the charge current is folding back
  ([DS-818] p. 52). **[GUESS]** This is the only "die is hot" indication short of shutdown. It is not defined in
  Samsung's MAX77705 header, and it is about the charge loop, so it may not assert in OTG mode.
- **CHG_DETAILS_00 0xB3 bits 6:5 CHGIN_DTLS:**
  - 0 = CHGIN < UVLO
  - 1 = above UVLO but < VMBAT + CHGIN2SYS
  - 2 = > OVLO
  - 3 = valid
  - Sources: [DS-818] p. 50; same strings in [S-NEW] `max77705_get_vbus_state()`, lines ~152-167.
  - **[GUESS]** In OTG mode this field measures the voltage on CHGIN, which we are driving. It should read 3 while the
    output is healthy and fall to 0/1 when a short pulls VBUS down. It is useful as a second short indicator, but no
    source documents it for OTG mode.
- **CHG_DETAILS_01 0xB4:**
  - CHG_DTLS[3:0] ([DS-818] p. 51):
    - 0x08 = charger off (normal in OTG mode)
    - **0x0A = off because T_J > T_JSHDN (thermal shutdown)**
    - 0x0B = watchdog expired
  - BAT_DTLS[6:4]: 0x06 = battery overcurrent (B2SOVRC).
  - Samsung's decode is the same ([S-CHG] `max77705_get_charger_state()` around line 175; [S-NEW] lines 172-200,
    which treat 0x0B as "WDT expired").

### 1.3 Thermal limits of the charger die

- **MAX77818** ([DS-818] p. 12 and p. 59):
  - REGTEMP (CHG_CNFG_07 0xBE bits 6:5) foldback setpoint: 85/100/115/130 C, default 100 C. The charge current
    reaches 0 at about +20 C above the setpoint.
  - Thermal shutdown: CHG_DTLS = 0x0A. "O-type" registers, which include CHG_CNFG_00, reset at die temperature
    > 165 C ([DS-818] p. 43).
  - The top-level SYSTEM_INT bit TSHDN_INT exists ([DS-818] p. 45).
- **MAX77975**: TSHDN 155 C rising, 15 C hysteresis. Thermal shutdown resets MODE to its default (OTG off) and all
  O-type registers ([DS-975] pp. 19, 37, 47).
- **MAX77705**: the TSHDN interrupt exists as SYSTEM_INT (0x24) bit 6 ([S-PRIV] IRQ table, `max77705-irq.c`
  lines 66-70). The threshold is unpublished. **[GUESS]** It is about 150..165 C, like its siblings.
- **Is the die temperature readable?** No die-temperature ADC is exposed. The fuel-gauge TEMPERATURE register (FG 0x08)
  is *written by the AP* with the battery thermistor value, so it is not a die sensor ([S-BAT]
  `max77705_fuelgauge.c` lines 250-298). In OTG mode the usable proxies are:
  - the board "chg_temp" thermistor next to the charger, ADC 3 (section 3);
  - TREG, if it asserts;
  - SYSTEM_INT TSHDN;
  - CHG_DTLS = 0x0A, or CHG_CNFG_00.MODE reading back changed (the O-type reset).

### 1.4 Re-enabling OTG after a fault

- **MAX77818 / MAX77975 with OTG_REC_EN=1:** the switch retries by itself every ~300 ms. Software only needs to
  read CHG_DETAILS_02 to clear the BYP_DTLS[0] latch ([DS-975] p. 38).
- **MAX77975 with OTG_REC_EN=0:** toggle MODE (write 0x0A to something else, then back) ([DS-975] p. 38).
- **After thermal shutdown or O-type reset:** MODE has returned to default. Rewrite CHG_CNFG_00 and CHG_CNFG_02 after
  the die cools by the hysteresis ([DS-975] p. 47). The writes to CNFG_01..03 need CHGPROT unlocked
  (CHG_CNFG_06 bits 3:2 = 0b11), which Samsung's driver re-checks on every BYP_I (`check_charger_unlock_state()` in
  `max77705_bypass_irq`, [S-CHG] ~line 1861). **Note:** our `starbite/sbos/kern/survival.c` already logs "OTG_ILIM
  1.5 A did not read back (write-protected?)". This is the CHGPROT case.
- **Stock Samsung re-enable path:** the CCIC calls `max77705_vbus_turn_on_ctrl(ON)` on the next sink attach, which sets
  psy "otg" online = 1 ([S-CCIC] `max77705_pd.c` lines 152-245).

---

## 2. Why a stock Galaxy S9 cuts OTG "instantly" on a short

The cut is the **Samsung driver**, triggered by the chip's OTGILIM status. It is neither the chip alone nor the
CCIC firmware.

1. **Stock configuration.**
   - The starlte defconfig sets `CONFIG_CHARGER_MAX77705_OTG_LIMIT=y` ([S-DEF] line 4871).
   - So the charger init programs **OTG_ILIM = 900 mA**, not 1500 ([S-CHG] lines 941-950).
   - While OTG is on, the big cluster is capped at 1.69 GHz: `charger,cpu_max_freq = <0 1690000>` ([S-DTS] line 308,
     `max77705_set_otg_limit()` [S-CHG] lines 540-567).
2. **On OTG enable** (`max77705_set_otg()`, [S-CHG] lines 799-899), the driver:
   - unmasks **BYP_I**, masks CHG_I/CHGIN_I;
   - sets the switching frequency to 3 MHz;
   - disables skip mode;
   - sets CNFG_00 OTG|BOOST.
3. **BYP_I handler** `max77705_bypass_irq()` ([S-CHG] lines 1847-1881):
   - reads CHG_DETAILS_02;
   - `if (byp_dtls & 0x1)` logs "bypass overcurrent limit";
   - sends `NOTIFY_EVENT_OVERCURRENT`;
   - **sets psy "otg" ONLINE = 0**, which clears OTG|BOOST in CNFG_00.

   [S-NEW] lines 2242-2280 has the same logic in the 2023 driver, guarded by `if (charger->otg_on)`. The USB notify
   layer turns the event into `NOTIFY_HOST_OVERCURRENT` and logs "OTG overcurrent!!!!!!" ([S-CCIC] `usb_notify.c`
   lines 962-969). Android then shows its "accessory uses too much power" style message. **[GUESS]** That message is
   from the framework, not seen in source here.
4. **Timing [GUESS from DS-975].** About 20 ms of overcurrent to BYP_I (t_OTG_ALARM), plus the IRQ thread and I2C write
   (a few ms). The output dies in roughly 25..35 ms, and it stays off instead of hiccuping. To a person that is
   "instant".
5. **The CCIC does not cut VBUS on a VBUS-GND short.**
   - The CCIC firmware detects only **CC-VBUS short, SBU-GND short and SBU-VBUS short** (opcode 0x56 bits 0..2,
     `max77705_muic_enable_detecting_short()`, [S-CCIC] `max77705-muic.c` lines 517-536; handlers in
     `max77705_usbc.c` lines 2283-2320). These raise events and counters only.
   - The CCIC's "auto VBUS" (opcode 0x54 = 2) is **disabled** by Samsung at probe (opcode 0x54 = 0,
     `max77705_usbc_disable_auto_vbus()`, [S-CCIC] `max77705_usbc.c` lines 250-276, called at line 2962). So the
     kernel, not the firmware, switches VBUS.
   - **[GUESS]** The CCIC firmware's default after a cold reset, when no Samsung kernel ran, is unknown. If it came up
     in auto-VBUS mode, it might switch OTG by itself on Rd attach and detach. Worth a read-back on our side.
6. **The usual trigger needs a sink.** On a stock phone VBUS is only sourced after the CCIC sees a sink (Rd on CC)
   ([S-CCIC] `max77705_pd.c`: VBUS on through `max77705_vbus_turn_on_ctrl()` on attach). A bare-ended cable with no
   Rd gets no VBUS at all from stock Android. Our bare-metal path writes MODE directly, so this gate does not apply
   to us. **[GUESS]** A bare C-to-A cable or an OTG adapter carries the Rd.

**Consequence for Starbite.** We do not run Samsung's BYP_I handler. **[GUESS]** With every protection left on, we
get the chip's own behaviour: a 30 ms/300 ms hiccup into a short, or OTG staying latched off if the MAX77705's retry
default is "off". To match stock safety, treat **BYP_DTLS bit 0 = 1 as a fault and turn OTG off**. That is exactly
Samsung's rule. Today `survival.c` only logs BYP_DTLS and waits 3 s of VBUS < 3.5 V. Also treat bit 1 (BSTILIM) and
CHG_DTLS = 0x0A as stop conditions.

---

## 3. USB-connector thermistor on the S9

**Yes.** The S9 has a USB thermistor read by the AP's ADC.

- **Device-tree entries** ([S-DTS] lines 98-114, 300-301):
  - `battery,usb_thermal_source = <2>; /* SEC_BATTERY_THERMAL_SOURCE_ADC */`
  - `io-channels = <&exynos_adc 4>, <&exynos_adc 3>, <&exynos_adc 11>, <&exynos_adc 4>, <&exynos_adc 1>;`
  - `io-channel-names = "adc-temp", "adc-chg-temp", "adc-in-bat", "adc-wpc-temp", "adc-usb-temp";`
- **The channel map** that follows from these entries:

  | Function | Exynos ADC channel |
  |---|---|
  | Battery thermistor | ch 4 |
  | Charger ("chg_temp") thermistor | ch 3 |
  | in-bat voltage | ch 11 |
  | WPC (coil) | ch 4 (same channel as the battery; that is what the DT says) |
  | **USB** | **ch 1** |

  `sec_adc.c` looks the channels up by these names ([S-BAT] `sec_adc.c` line 35, lines 284-288). This matches our own
  `starbite/sbos/kern/kern.h` (`usb_ok, usb_c10, chg_ok, chg_c10; /* ADC 1, 3 */`).
- **Conversion table.** `battery,usb_temp_table_adc` and `_data` ([S-DTS] lines 111-114) are identical to the
  battery, chg and wpc tables. Temperatures are in 0.1 C; raw values are the 12-bit Exynos ADC.

  | ADC | 275 | 327 | 385 | 451 | 535 | 626 | 737 | 864 | 1013 | 1210 | 1361 | 1554 |
  |---|---|---|---|---|---|---|---|---|---|---|---|---|
  | T (C) | 90 | 85 | 80 | 75 | 70 | 65 | 60 | 55 | 50 | 45 | 40 | 35 |

  | ADC | 1769 | 2007 | 2239 | 2476 | 2702 | 2924 | 3116 | 3294 | 3446 | 3564 | 3676 |
  |---|---|---|---|---|---|---|---|---|---|---|---|
  | T (C) | 30 | 25 | 20 | 15 | 10 | 5 | 0 | -5 | -10 | -15 | -20 |

  - Lower ADC means hotter (NTC).
  - Linear interpolation between points, clamped at both ends ([S-BAT] `sec_adc.c` lines 321-345). So **anything
    hotter than 90 C reads as 90 C**: the table saturates.
  - The driver averages `battery,adc_check_count = <5>` samples.
- **What stock does with it.**
  - USB temperature ≥ `temp_highlimit_threshold_normal = 800` (80.0 C) → `POWER_SUPPLY_HEALTH_OVERHEATLIMIT`.
  - Recovery below `750` (75.0 C) ([S-DTS] lines 158-159; [S-BAT] `sec_battery.c` lines 1855-1871, 1947-1950).
  - The action is charging-side only: buck off, and AFC to 0 V (lines 1995-2006).
  - **[GUESS]** Nothing in the code shown turns off *OTG* on USB overheat.
  - **Suggested Starbite rule:** stop OTG at USB thermistor ≥ 70 C, safely before the 90 C saturation. Also stop if the
    reading jumps faster than the thermistor's plausible slew.
- **Charger-die temperature in OTG mode.** It cannot be read directly (1.3). The nearest proxy is the "chg_temp"
  thermistor, ADC ch 3, same table. Stock limits: `chg_high_temp = 460`, recovery 440 ([S-DTS] lines 180-181).

---

## 4. Candidate heating elements vs the ~3.5 ohm floor

### 4.1 The floor [CALC]

- **The rule.** To stay *below* OTG_ILIM, never in it (1.1), we need R_load ≥ V_out,max / I_lim,min.
- **The inputs.** With V_BYP.OTG max 5.26 V ([DS-818] p. 9) and a 1.5 A limit that is ≥ 1.5 A min ([DS-975] p. 16),
  the floor is **R ≥ 3.5 ohm** at OTG_ILIM = 1500.

| OTG_ILIM setting | Minimum R | R with a 1.3x margin | Power at that R (5.1 V) |
|---|---|---|---|
| 1500 mA | 3.5 ohm | 4.6 ohm | 5.7 W |
| 1200 mA | 4.4 ohm | 5.7 ohm | 4.6 W |
| 900 mA | 5.8 ohm | 7.6 ohm | 3.4 W |
| 500 mA | 10.5 ohm | 13.7 ohm | 1.9 W |

- The cable and connector add roughly 0.1..0.3 ohm in series, which helps a little. **[GUESS]**
- **Graphite gets *less* resistive when hot.** Carbon (graphite) α = **-0.0005 /C**, ρ = 3..60 x 10^-5 ohm·m
  (HyperPhysics resistivity table, <https://hyperphysics.gsu.edu/hbase/Tables/rstiv.html>). A lead heated by about
  600 C would then lose about 30 % of its cold resistance. **[GUESS]** That linear extrapolation is rough. **So size
  graphite at ≥ 5 ohm cold for the 1.5 A setting.**

### 4.2 Pencil lead (graphite, 0.5 / 0.7 mm, HB / 2B)

No manufacturer publishes ohm/cm for mechanical-pencil leads. I found nothing for Pentel or any 0.5 mm lead
specifically.

**The sourced data points:**

- **Bulk graphite.** "Pure graphite ... has a typical electrical resistivity of 3 x 10^-5 ohm m" (Physics Education,
  March 2003, p. 94, "Pencil leads: 20 project ideas",
  <http://users.df.uba.ar/sgil/physics_paper_doc/papers_phys/e&m/pe_Pencilleads.pdf>).
  - The same paper says the resistance of a 15 cm lead "will vary by an order of magnitude from the softest to the
    hardest" grade, because harder grades have more clay.
- **Classroom measurement.** About 2 ohm for 60 mm of 0.7 mm HB, quoted via search summary from the Science Buddies
  forum thread "Graphite Experiment" (<https://www.sciencebuddies.org/science-fair-projects/ask-an-expert/viewtopic.php?t=11023>).
  The page itself returned HTTP 403 to my fetch, so this is unverified. It works out to ρ ≈ 1.3 x 10^-5 ohm·m.
- **Whole wood pencils (175 mm, about 2 mm core):** 2B ≈ 6 ohm, HB ≈ 19 ohm, H ≈ 25 ohm. Source:
  troelsgravesen.dk "Graphite resistors" (<http://www.troelsgravesen.dk/graphite.htm>), quoted via search summary.
  The site's certificate had expired, so I could not fetch it. **[CALC]** The HB value implies ρ ≈ 3.4 x 10^-4 ohm·m,
  10x the bulk value. Clay-bonded wood-pencil cores are much more resistive than graphite.

**Estimated resistance [CALC / GUESS: polymer mechanical leads are assumed to sit near the low, graphite-rich end]:**

| Lead | ρ = 1.3e-5 | ρ = 3e-5 | ρ = 3.4e-4 (clay-rich) | Length for 5 ohm (polymer range) |
|---|---|---|---|---|
| 0.5 mm | 0.66 ohm/cm | 1.5 ohm/cm | 17 ohm/cm | ~3..8 cm |
| 0.7 mm | 0.34 ohm/cm | 0.8 ohm/cm | 9 ohm/cm | ~6..15 cm |

- 2B should be lower than HB, by up to 2..3x going by the wood-pencil numbers. **[GUESS]**
- **Verdict.** A 0.5 mm HB lead of a few cm is the one candidate that **can sit above the 3.5..5 ohm floor at a usable
  length**. 4..6 W into 3..5 cm of 0.5 mm graphite is about 1..2 W/cm. **[GUESS]** That should take it to a visible
  red glow and ignite tinder, but it will burn off its polymer binder (smoke), weaken and snap.
- **Contact resistance at the clips is significant** at these values. The Science Buddies replies note that meter-lead
  resistance alone is about 0.9 ohm. Measure the actual assembly; do not rely on the table.

### 4.3 Single copper strand (0.08..0.15 mm)

**Resistance** (Wikipedia AWG table, <https://en.wikipedia.org/wiki/American_wire_gauge>):

| Strand | Resistance (sourced) | Length for 3.5 ohm [CALC] |
|---|---|---|
| AWG 40, 0.0799 mm | 3.441 ohm/m | 1.0 m |
| AWG 38, 0.101 mm | 2.164 ohm/m | 1.6 m |
| AWG 36, 0.127 mm | 1.361 ohm/m | 2.6 m |
| 0.15 mm | ~0.97 ohm/m [CALC, ρ_Cu 1.72e-8] | 3.6 m |

**Fusing current** (Wikipedia AWG table, 25 C ambient, columns ~10 s / 1 s / 32 ms):

| Strand | ~10 s | 1 s | 32 ms |
|---|---|---|---|
| AWG 40 | 1 A | 1.5 A | 8.5 A |
| AWG 38 | 3 A | 2.4 A | 14 A |
| AWG 36 | 4 A | 3.9 A | 22 A |

- **Preece's law:** I_f = 10244·d^1.5, with d in inches. The coefficient is from Ness Engineering, "Fuse Equations",
  <https://www.nessengr.com/?p=295>. [CALC] It gives 1.8 A (0.08 mm), 2.5 A (0.10 mm), 3.6 A (0.127 mm) and
  4.6 A (0.15 mm).
- **Caveat on thin wires.** A Physics Forums thread reports that 37 AWG did not fuse at 3 A as Onderdonk predicts
  (<https://www.physicsforums.com/threads/clarification-of-onderdonks-fuse-equation-assumptions.774743/>). The
  heat-loss conditions dominate.
- **No glow-current source found.** **[GUESS]** A visible red glow (about 600 C) needs on the order of 60..80 % of the
  fusing current.
- **Verdict.** A strand short enough to use (centimetres) is about 0.1 ohm, a **dead short** to the boost. That means
  OTGILIM and a hiccup or shut-off.
  - In a 30 ms burst at 1.5 A, a 0.08 mm strand warms by only about 13 C [CALC, adiabatic: ρJ²/(ρc), Cu]. It will
    not ignite anything.
  - At the 1.5 A limit, the 0.10..0.15 mm strands would stay well under fusing even if held continuously.
  - **Copper strands do not work behind a protected 5 V/1.5 A source.** Only metres of wire reach the resistance
    floor, and then the heat is spread out.

### 4.4 Gum-wrapper foil

- **Foil thickness.** Paper-backed aluminium about 5..7 µm thick.
  - Hydro, a supplier to Wrigley, makes foil down to 6.35 µm
    (<https://www.hydro.com/en/global/media/news/2007/all-wrapped-up/>).
  - Constantia lists 5.0..5.5 µm confectionery foil (via search summary).
  - Patent US4082594 describes paper bonded to 0.006 mm foil
    (<https://patents.google.com/patent/US4082594A/en>).
- **[CALC]** Sheet resistance = ρ_Al (2.65e-8 ohm·m, HyperPhysics) / t ≈ **4..5 milliohm per square**.
  - A 1 mm x 10 mm bridge is 10 squares, about **0.04 ohm**.
  - Reaching 3.5 ohm needs about 700..900 squares, for example 0.1 mm wide by 80 mm long. That is impractical.
- **Verdict.** Foil is a **dead short** to the OTG output. At 1.5 A the 1 x 10 mm bridge would dissipate only about
  0.1 W. The trick works with a bare AA or phone cell only because an unprotected cell pushes tens of amps. Survival
  guides themselves stress cutting a very narrow waist (<https://www.instructables.com/Start-Fire-with-Gum-Wrapper-and-Battery>).

### 4.5 Summary for design

| Element | Typical R as used | Above 3.5..5 ohm? | Expected behaviour on protected OTG |
|---|---|---|---|
| 0.5 mm HB/2B lead, 3..8 cm | ~2..12 ohm **[GUESS]** | **yes, if measured** | Glows; R falls as it heats (keep ≥ 5 ohm cold); binder smokes; snaps |
| 0.7 mm lead | ~0.3..0.9 ohm/cm **[GUESS]** | needs 6..15 cm | Workable but long and fragile |
| Copper strand, cm length | ~0.03..0.2 ohm | no (needs 1..3.6 m) | OTGILIM short → hiccup/off; no ignition |
| Gum foil bridge | ~0.01..0.1 ohm | no | Short → hiccup/off; no ignition |
| 0000 steel wool (our current UI text) | unknown | unknown | Not researched in depth. No source gave fibre diameter or resistance. **[GUESS]** A loose wisp of fine fibres can be a few ohm, but contacts are erratic. |

**Implementation suggestions, all [GUESS] and not tested:**

1. Before a burst, measure the load. Enable OTG at OTG_ILIM 500 mA for about 100 ms and compute R from the
   battery-current step at the fuel gauge: P_out ≈ η·V_cell·ΔI_bat, with η about 0.85..0.9 unsourced. Refuse when
   R < 5 ohm, or when BYP_DTLS[0] sets.
2. During a burst:
   - stop on BYP_DTLS bit 0 or bit 1 (Samsung's rule; 2);
   - stop on CHG_DTLS = 0x0A, or CNFG_00.MODE not reading back 0x0A (the O-type reset);
   - stop on the USB thermistor (ADC 1) ≥ 70 C;
   - stop on the chg thermistor (ADC 3) ≥ 60 C.
3. **Our existing `survival.c` UI text** suggests "fine steel wool (0000) or a thin wire". Per 4.3, a thin copper wire
   will not work behind the limit. It should say "a few cm of 0.5 mm pencil lead (measure ≥ 5 ohm)" instead.
