# Battery health (MAX77705 fuel gauge, read only)

Driver: `starbite/sbos/kern/bathealth.c`, `bathealth.h`. Host test: `starbite/sbos/kern/host/bathealth-test.sh`.
Status: written 2026-11-12. The register reads match what Engineer OS `lab.c` already shows raw. The maths has not
run on a phone yet.

## Wiring

The fuel gauge (ModelGauge m5) is at 7-bit 0x36 on HSI2C 0x14360000. It is read through libsb's
`sb_fg_word_checked`, which retries a failed word. **Nothing is ever written.** The file references no writer at
all, and the host test checks this with `nm`.

| Word | Reg | Unit | Source |
|---|---|---|---|
| STATUS | 0x00 | bit 1 POR (gauge restarted), bit 3 Bst (no battery) | Maxim m5 map, Engineer `lab.c` |
| Cycles | 0x17 | 1/100 cycle | an earlier port's facts (ran on this phone), `utility-hw-research.md` §5 |
| DesignCap | 0x18 | 1 mAh | DT `fuelgauge,capacity = 0xB97` (2967 mAh) |
| FullCapNom | 0x23 | 1 mAh | Samsung `max77705_fuelgauge.h` (FG_FULLCAPNOM) |
| FullCapRep | 0x35 | 1 mAh | Samsung (FG_FULLCAPREP), an earlier port |

**1 mAh a step.** The phone has a 5 mΩ sense resistor (DT `fuelgauge,fg_resistor = 2`). libsb's current step of
312.5 µA also implies 5 mΩ. DesignCap reads 2967 for a 3000 mAh cell.

Note: `kern/services.c` (battery_poll) divides RepCap and FullCap by 2 and says "0.5 mAh per LSB with the 10 mOhm
sense resistor". That disagrees with the facts above, so it probably shows half the real mAh. This driver does not
depend on it. It is flagged to the lead.

## The maths

- **Maximum capacity % = FullCapNom / DesignCap**, rounded. FullCapNom is the full charge the gauge has learned at
  nominal temperature and rate, so it follows the cell's age, not today's temperature. FullCapRep / DesignCap is kept
  as a second number (`rep_pct`).
- If FullCapNom is 0 or above 1.5 × design, FullCapRep is used instead. If both are, the capacity is "not known".
- If DesignCap is outside 1500..6000, the DT's 2967 mAh is used (`design_from_dt`).
- The UI shows `shown_pct` = the result capped at 100, like iOS ("Maximum Capacity 87 %"). A new cell can learn
  above its design.
- **Cycles** = raw / 100 (`cycles`; `cycles_x100` keeps the fraction). The register stops at 655.35 cycles in this
  unit and wraps. A very old cell may show too few.
- Words (`h.words`):
  - 80 % or more: "Your battery is in good health."
  - 60..79 %: holds noticeably less, still works.
  - Under 60 %: a new battery is recommended.
  - Extra cases: no battery (Bst), not known yet, and "the charge meter restarted and is still learning" (POR).

## API (bathealth.h)

- `int bathealth_read(struct bathealth *h)` returns 1 when read. It fills `design_mah`, `fullnom_mah`, `fullrep_mah`,
  `cycles`, `max_pct`, `shown_pct`, `rep_pct`, the flags and `words`. `bathealth_why()` gives the reason when it
  fails.
- `bathealth_compute(...)` is the pure maths, for tests.

It reads five words, about 1-2 ms on the bus. Call it when the Battery page opens, not every frame.

## Probe

`int drv_bathealth_probe(int level, unsigned *wait_ms); void drv_bathealth_abort(void);` Budget: 2000 ms
(`BATH_PROBE_BUDGET_MS`). Each level finishes in one call.

- **Levels 0 and 1** (reads only): first a log line with design, full nominal, full reported and cycles, then
  `drvprobe: bathealth: PASS maximum capacity 87 %, 412 cycles`. It prints
  `FAIL <why>` when the gauge does not answer, there is no battery, or the capacity is not known.
- **Level 2**: PASS `nothing at level 2 (read only)`.
- Abort: nothing to put back.

## Risks and open points

- Samsung's Android driver re-learns and may rewrite FullCapNom / FullCapRep after a model reload. Under Starbite OS
  we only read them. After the gauge loses power (POR), the numbers are its defaults until a few full charges.
- The battery ID ADC channel is not read. The ADC channel for it on this board is not known, and finding it is not
  cheap.
- Cycles wraps at 655.35 (see above).

## Host test

`sh starbite/sbos/kern/host/bathealth-test.sh`: 31 checks.

- Register sets for a new cell (103 %, shown 100), a typical two-year-old cell (2581 / 2967 = 87 %, 412 cycles) and a
  worn one (54 %).
- The rounding at 80 % and 79 %.
- FullCapNom not plausible, both capacities unknown, DesignCap 0 / 0xFFFF, POR, no battery.
- Each needed word failing, the optional words failing.
- The probe at every level.
- `nm`: no writer referenced.
