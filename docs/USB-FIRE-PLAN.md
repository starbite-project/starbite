# USB fire starter: what the two tests showed, and what will work (plan for another day)

Track D, 2026-11-12. Sources: `docs/USB-FIRE-RESEARCH.md` (the MAX77705's OTG behaviour, from its register-identical
sibling's datasheet and Samsung's driver), the pmsg of the two attended runs `build/starbite-os/fire-1` (19:07,
alpha29-b392f02) and `fire-2` (19:25, alpha29-7f0c13e4).

## What the logs show
- The chip ran at its top current limit, 1500 mA, in both runs (`OTG limit 1500 mA`; the write guard was already
  open). Nothing was ever over-heated: the charger peaked at 33 C, the USB connector at 27 C, the battery at 30 C.
- fire-2: the current-limit events (BYP_DTLS bit 0) rose **about 3 a second** for as long as an element was connected
  (118 -> 147 in ~10 s; ~350 in 15 s by the user's count). That is exactly the chip's own retry cycle: when the load
  wants more than ~1.5-1.65 A the output switch holds the limit for ~30 ms, opens, and retries after ~300 ms. The
  battery-current samples caught "on" pulses (up to 2.67 A from the battery, ~1.46 A out of the port) and "off" ones.
- So the element got power only ~10 % of the time (~0.7 W average): enough to spark at every retry and to smoke the
  lead's binder once, never enough to glow.

## Why "far apart" still tripped
Not a soft-start or an eager short detector: the MAX77705 has neither on this path (research section 1). It is the
current limit itself, which on this chip is a 30 ms limit then off, not a steady constant-current source. Any element
below about **3.4 ohm (5.1 V / 1.5 A)** sits in that regime. The leads used were simply too conductive:
- 0.7 mm lead: ~0.3-0.9 ohm/cm, so even 4-5 cm is ~1.2-4.5 ohm, and graphite's resistance **falls as it heats**
  (negative temperature coefficient), so a borderline length slides into the limit as soon as it warms.
- 0.5 mm lead: ~0.7-1.8 ohm/cm: borderline at 3-4 cm, and the same fall with heat.
- The earlier advice to move the contacts *closer* was backwards for these leads; I have corrected it.

## What works within the 1.5 A rating (nothing protective changed, ever)
The element must stay **above ~3.6-4 ohm when hot** (start at 5-6 ohm cold), and be **as thin as possible** so the
~5-7 W lands in a small mass. In order of preference:
1. **Nichrome or other resistance wire** (a toaster, hair dryer, heat gun or e-cigarette coil): 0.2 mm nichrome is
   ~34 ohm/m, so ~12-15 cm (coiled to ~2 cm) is ~4-5 ohm and glows orange at ~1.3 A. The ideal element.
2. **A 0.3 mm pencil lead** (mechanical pencils sold for 0.3 mm): ~2-5 ohm/cm, so 1.5-2.5 cm: thin, small mass,
   glows. The best household option.
3. **A 0.5 mm HB lead, longer than tonight**: 5-7 cm between wrapped contacts, then shorter in 1 cm steps, stopping
   at the first length where the screen counts limit events again (and going back 1 cm). 0.7 mm: 8-12 cm, which
   spreads the heat: unlikely to glow; use it last.
4. **A thin tuft of fine steel wool (0000)** stretched between the contacts (a few fibres, not a pad): high resistance
   and tiny mass; it glows and burns at a few watts. A thick wad is a short.
5. **A series resistor** only rescues a too-short element at a cost: element 1.5 ohm + series 2 ohm draws 1.46 A but
   puts only ~3.2 W into the element. Useful only with a very thin element; option 1-2 are better.
Foil, a copper strand, a thick wad: shorts, always in the 30 ms limit regime.

## Software for the next build (small, all within the rules)
- **Say it on screen:** when limit events come at ~2-4 a second for 2 s, show "The element draws more than 1.5 A:
  make it longer or thinner" (today it only shows the count).
- **Measure before heating:** at the start, 200 ms at the chip's 500 mA step, the battery's extra draw gives a rough
  load figure: "too low (< 3.5 ohm): lengthen" before the user waits for a glow. Same chip limits throughout.
- **A watts readout** (about W = the battery's extra draw x the battery voltage x 0.85) to tune by: aim for the
  highest steady number without limit events.
- A ramped start does **not** help (the limit is not an inrush issue); faster retries would defeat the chip's own
  protection interval and are not done.

## Test plan (attended, another day)
Same safety as tonight (non-flammable surface, insulated parts only, paper and water ready, the Mac cable out).
1. 0.3 mm lead, 2.5 cm, wrapped contacts; then 2 cm, 1.5 cm.
2. 0.5 mm HB lead, 6 cm, then 5, 4 cm.
3. If available: a nichrome coil of ~4-5 ohm.
Pass: a steady ~1.2-1.4 A with no limit events for >5 s, and a visible glow that lights paper.
