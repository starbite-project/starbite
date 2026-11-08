# Starbite OS 0.6 .. 0.9: from an emergency system to a small, careful phone OS

From the user's phone tour of 0.5.14 (2026-11-08) and the main session's direction. Each stage is host-built and
host-tested first (renders, the self test, the soak, the monkey, the Wi-Fi and settings tests), reviewed
independently, then tried on S9 #1 by the main session: unattended where nothing risky runs, with the user present
for first runs of anything that heats, transmits beyond scans, drives a new rail or talks to the modem.

## Rules that do not change

- **Within the parts' ratings only.** No over-voltage, over-current or clocks above the vendor's tables, for any
  "Extreme" feature: CPU and GPU at their top table steps, the panel at its own maximum brightness (its built-in
  high-brightness mode only if the vendor driver uses it), the LED at its rated torch current and rated flash pulses.
  The skin-temperature burn limit (46 °C hard) stays.
- Never MIF or INT clocks; never EFS/CPEFS/PARAM/... without a verified backup; nothing writes RECOVERY.
- Emergency calls: implemented, **never dialled in a test**. The modem never writes EFS (served a copy).
- Risky first runs with the user present.

## 0.6: the normal-mode shell, the Emergency Mode toggle, Extreme heat (started 2026-11-08)

1. **Two modes.** *Normal mode* (the default): a phone. *Emergency Mode* (switched in the control center): today's
   big-tile emergency UI, and the screen-off power path (after 5 minutes with the screen off: save, then power
   off; real RAM sleep replaces the first part when 4.3 is done). The mode is a setting (saved in the slot).
2. **Normal-mode look**, "glass" in the spirit of iOS, kept cheap:
   - a wallpaper generated at start (a deep gradient with soft aurora light and stars; several to choose from), and
     a blurred copy of it made once; everything is drawn over the wallpaper instead of black;
   - "glass" cards: the blurred wallpaper under each card, tinted, with a light edge (no live blur, no shaders:
     two pre-made buffers and a per-pixel blend, about 20-40 ms for a full screen on an A55);
   - the lock screen: the wallpaper, a large clock, the date, the holds (Light, SOS, Open) as glass circles;
   - home: a clock widget and the apps as glass icons in a 4-column grid (Emergency is one of them);
   - every app screen keeps its layout and gets glass cards over the wallpaper.
3. **Control center** (a tap on the status bar, or swiping down from the top): Emergency Mode (the big red
   switch), Flashlight, Wi-Fi, brightness (Low / Mid / Normal / High), the screen light, the screen-off time.
4. **Extreme heat**, beside Low and High, within ratings: every A55 and M3 core at the top all-core table step, the
   GPU at its top table step with the non-repeating fragment workload, the screen kept on at the panel's top
   brightness level, the LED at its rated torch current once the S2MPB02 driver exists (9.2), the Wi-Fi radio
   scanning once heat and Wi-Fi can run together, the cameras powered once their rails and sensors are understood
   (research). Its limits: battery band 43 °C, hard stop 45 °C; SoC soft 78 °C, hard 85 °C; skin unchanged (43 / 46);
   current unchanged (3.2 / 3.5 A). Asks once per start.
5. **The heater keeps the screen on** while it runs (the user read the screen going off as a failure); the screen is
   part of the heat.
6. **SOS** flashes at the panel's top brightness level and puts the previous level back afterwards.
7. The flashlight's rated maximum (S2MPB02: torch current register, flash pulses with the chip's own timer):
   designed in doc 9.2; needs the I2C bring-up and a phone test with the user present.

## 0.6.x: bursts in Emergency Mode, and a power budget (the user's tour of 0.5.14, 2026-11-08)

Short bursts that push harder, **within the parts' pulse/burst ratings and absolute maximums, never above them, and
never overriding the battery's or the charger's protections** (the fuel gauge's and PMIC's limits stay in charge):

1. **The SOS strobe at the LED's flash-pulse current** (the S2MPB02's camera-flash rating, its flash timer ending each
   pulse in hardware; the duty cycle the datasheet allows). Needs the S2MPB02 driver (doc 9.2) and a first run with
   the user present.
2. **The panel's high-brightness mode in bursts**, only as Samsung's own panel driver uses it (its HBM command and
   ELVSS/gamma tables for this panel, its time limits); the S9 driver's sources are read first. Until then the top
   normal level (0.6).
3. **The M3's top boost step for 1-2 cores with the others off**: only a step in Samsung's own ACPM table for this
   SoC (the boost entries the vendor kernel uses with fewer cores online), never a made-up voltage or clock; the
   heater's thermal and current caps stay.
4. **A power-budget manager** for the bursts: before a burst it switches off what is not essential (Wi-Fi, the extra
   cores, the GPU; the cameras and the modem once they exist) so the battery voltage stays stable for the burst,
   and puts them back after. The budget uses the fuel gauge's voltage and current; a burst is refused below a
   battery voltage and charge floor, and in the heat.

**Extreme heat conserves nothing** (the user's clarification): every core including all 4 M3s at their top normal
step, the GPU, the panel, the LED, and the cameras and radios as their drivers arrive, all on at once. The shedding
above is for the brightness/strobe bursts only. The same limits hold: within ratings, the battery (45 °C) and
thermal hard caps kept.

## 0.7: sound

- The S9's audio path (to be confirmed from the vendor sources): the codec (Cirrus Logic "Madera" family on the
  Exynos 9810 S9) on SPI/I2C, the speaker amplifier on I2C, ABOX (the Exynos audio subsystem) and its firmware.
- Study: Samsung's GPL kernel for SM-G960F and LineageOS's `android_kernel_samsung_exynos9810` (sound/soc/samsung/
  abox, codecs/madera*), the amplifier driver; the firmware files from VENDOR (read only, as Wi-Fi does).
- First target: a tone from the speaker for the alarm (SOS's siren, the timer). Then the earpiece. A volume cap.
- Phone tests: unattended at a low volume for a few seconds; louder only with the user.

## 0.8: the modem

- The Shannon CP: its boot (the CP image from RADIO, loaded through the vendor's sequence), the shared-memory IPC
  (the vendor's `cpif` / `link_device_shmem`), then the RIL messages for the network state, SMS and voice.
- EFS: the modem's NV data; the modem reads a copy; writes go to the copy (never the partition).
- Emergency calls: the code exists and is reviewed; tests stop before any dial; a "test mode" that refuses the
  dial request in the kernel is the default in every image until the user decides otherwise.
- Steps: CP power and boot with the user present, then network registration (no transmission beyond what the
  modem does to register), then SMS to the user's own number with the user present.

## 0.9: a Linux guest for heavier apps

- Questions first: does S-Boot leave EL2 to us (the woa shim's path), or is a hypervisor (Samsung's RKP / uH)
  holding it? If EL2 is ours: a small hypervisor on libsb with Linux as a guest on the M3 cluster, Starbite OS on the
  A55s, a framebuffer window and an input channel between them. If not: Linux alone on the M3 cluster through the
  heater's worker-core path, sharing a message ring with Starbite OS (no isolation; a research build only).
- The existing mainline Linux port for this phone (pmOS) is the guest kernel's starting point.

## Order and size

0.6 is a few days of host work (the shell and the modes first, Extreme heat beside them); 0.7 and 0.8 start with
reading the vendor sources and writing down the sequences (doc sections like WIFI-BCM4361.md), then small phone
steps; 0.9 starts with the EL2 question, answerable with one small test image.
