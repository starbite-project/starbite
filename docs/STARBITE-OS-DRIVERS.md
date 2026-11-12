# Starbite OS drivers: a peripheral audit of the Galaxy S9 (SM-G960F, Exynos 9810)

Draft, 2026-11-12. Read-only research: no phone was touched. Base: trunk 1cc8764 (Next P3v) plus the drivers
track's 49eb6ae (`kern/drvprobe.h`).

**States.**
- **works**: ran on the phone in a Starbite OS image and did its job.
- **partial**: runs on the phone, but part of the hardware's job is missing.
- **offline-built**: code exists and passes host tests, but has not run on the phone.
- **missing**: no Starbite OS code. "Being written" means a drivers-track helper is on it now (drivers-latest.md).
- **out of scope**: we do not plan a driver. The reason is given.

**Sources.**
- DT: `device/dtb/starlte.dts`, with line numbers.
- UHR: the shared notes' `utility-hw-research.md`.
- Docs: `docs/`.
- Notes: the shared notes folder.
- Code: `starbite/` (kern = `starbite/sbos/kern`, libsb = `starbite/libsb`, ui = `starbite/sbos/ui`).

**Other ports.**
- **S**: an earlier port (XNU, `build/refport27/refport-kernel`, its `docs/devices/starlte.md` and CHANGELOG). Its state words
  are its own ("works" = seen on the phone).
- **A**: our Apple driver set (`claude/track-j:starbite/apple-drivers/<Name>`, plan in `docs/APPLE-S9-DRIVERS.md`
  Part 3).
- **W**: Windows 11 (WoA UEFI + our drivers on `claude/win-*`).
- **L**: mainline Linux (the s9p port: NOTES.md 2026-10-27, `docs/LINUX-REBUILD-PLAN.md`).
- **Android** (One UI / LineageOS) drives every part. It is not repeated per row.

## 0. Update 2026-11-12 17:50: the drivers track's new drivers (offline-built, host-tested, not yet run on the phone)

| Driver | Files (starbite/sbos/kern) | Host test | Doc |
|---|---|---|---|
| Media sound: ABOX bypass playback / capture, mixer, tones, speaker guard | abox_pcm.c, snd.c | snd-test 160 | drivers/AUDIO.md |
| Codec media: jack (JD2) + headset buttons, headphones (HPOUT2), app mic | codec_media.c (+ additive codec.c exports) | codec-media-test 543 | drivers/CODEC.md |
| Bluetooth: BCM4361 over UART_BT, PCAL6524 expander (BT_REG_ON), patch from VENDOR, LE + classic scan | bt.c, bt_uart.c, bt_hci.c, pcal6524.c | bt-test 65 | drivers/BLUETOOTH.md |
| Front camera: S5K3H1, CSIS1 DMA, software ISP, preview + BMP still | cam.c, cam_isp.c | cam-test 1207 | drivers/CAMERA.md |
| Proximity, light, pressure, gyroscope on the hub; in-call proximity | shub_more.c (+ shub.c hooks) | shub-more-test 73 | drivers/SENSORS.md |
| Hall, wireless-charger pins / P9320, HRM part ID (read only) | senstat.c | (in shub-more-test) | drivers/SENSORS.md |
| Haptic patterns and intensity | haptic.c (+ vib.c hooks) | haptic-test 66 | drivers/HAPTICS.md |
| Notification LED (MAX77705 RGB at 0x66, not 0x4A) | nled.c | nled-test | drivers/NLED.md |
| Battery health (Maximum Capacity %, cycles; read only) | bathealth.c | bathealth-test 31 | drivers/BATTERY-HEALTH.md |
| USB device: CDC-ACM console (Utility OS's DWC3 driver) | usb.c, usbacm.c | usb-test 19665 | drivers/USB.md |
| The combined on-phone probe | drvprobe.c, uitest.c SBOS_SELFTEST_NEXT_DRV | drvprobe-test | (drivers-request.md in the notes) |

Still missing after this: rear camera, USB host, MFC video, NFC, DisplayPort, HRM measurement, SAR grip, our own GNSS
fix, Bluetooth pairing / audio, media sound during a call (Calliope's RDMA0 window).

## 1. The table

### Display and input

| Peripheral | Part / bus / address (source) | Starbite OS state | Where | Other ports | Notes |
|---|---|---|---|---|---|
| Display panel + DSIM + DECON | S6E3HA8 (`s6e3ha8_star_a3_s0/s1`), 1440 x 2960 AMOLED, command mode. DSIM0 0x16080000, DECON_F 0x16030000 (DT 13287, 5170, 5222) | works | kern/panel.c, ui/kern_next.c (P2r/P2t flips) | S works; A StarbiteDisplay (M8c: 120/120 flips); W sbdisplay/sbwddm (flips); L s9p-decon | We keep S-Boot's panel init and send DCS on DSIM: display on/off, ALPM, register reads. Three scanout buffers, flipped at TE by DECON's window shadow update. Refresh only on change. There is no cold panel init, so sleep-in is never sent. |
| Display PMIC | S2DOS05 at 0x60, hsi2c 0x14350000 (DT 7061) | out of scope | none | none known | S-Boot powers the panel. We only need it for a cold panel init. |
| Backlight / brightness | AOR (B1) by DCS on DSIM (panel.c `send_level`) | partial | kern/panel.c | S works (AOR + gamma, slider); A StarbiteDisplay | 5 user levels, plus a boost to the panel's top for SOS and Extreme, and ALPM at 10 nits. No HBM. No auto-brightness: the light sensor is missing. |
| DPP (6 channels) | 0x16021000..0x16026000 (DT 5093-5143) | partial | ui/kern_next.c | S uses VGF0 scaling (`fb_dpp=1`) | Only IDMA channel 0, as a plain full-screen window. No scaling, rotation or plane blending. The GPU composites instead. |
| Touch | S6SY761 at 0x48, hsi2c 0x104B0000 (USI03), INT gpa1-0 (DT 5989) | works | libsb/touch.c, kern/input.c | S works (10 fingers); A StarbiteTouch; W woa driver + `s6touchdiag`; L (unverified) | Polled at 16 ms. Interrupt mode (v0.2 plan) is unverified. |
| Keys | Power gpa2-4; Vol Up gpa0-3; Vol Down gpa0-4; Bixby gpa0-6 (DT 13069 gpio_keys; libsb/sys.c) | works | libsb/sys.c, kern/input.c | S partly; A StarbiteButtons; W unverified | Polled, two-poll debounce. Power is polled every 100 ms with the screen off. |

### Power, clocks and protection

| Peripheral | Part / bus / address (source) | Starbite OS state | Where | Other ports | Notes |
|---|---|---|---|---|---|
| Fuel gauge | MAX77705 FG at 0x36, hsi2c 0x14360000 (USI_CMGP03) (DT 7145; UHR 5) | works | libsb/max77705.c, kern/batmodel.c | S works; A StarbiteBattery | We read %, VCELL and current; a battery model is learned from them. Not read yet: FullCapNom / DesignCap (health) and Cycles (0x17). |
| Charger | MAX77705 charger at 0x69 | partial | libsb/max77705.c, kern/heater.c, kern/survival.c | S read-only; A StarbiteBattery | Charging is the chip's own, with S-Boot's settings. We read state and temperatures only. There is no charge-current or charge-limit control. The only write is B2SOVRC, during Survival heat. |
| USB-C / PD | MAX77705 CC/USB-C at 0x25 | partial | libsb/max77705.c (`sb_usbc_read`), kern/survival.c | S watches VBUS and the USB-C switch (`max77705_usbsw`); in-repo: starbite/utility/max77705_muic.c | Read only: the VBUS range and BC status. No PD negotiation, no role swap, no alt mode. |
| OTG power out | MAX77705 CHG_CNFG_00 OTG + BOOST, OTG_ILIM up to 1.5 A | offline-built | kern/survival.c ("USB power out") | W: firmware turns on VBUS for USB host (Star2LteUsbDxe) | 5 V for 10 s behind a hold. Staged in survival-selftest-request.md; not yet run on the phone. |
| Wireless charging | `idt,mfc-charger` at 0x3b, hsi2c 0x14370000 (DT 7197); WPC_EN = PCAL6524 P1_0 | missing | none | none known | The DT does not name the part. "P9320" is unverified. It probably charges with no AP help, also unverified. |
| Main PMIC | S2MPS18 on SPEEDY 0x141C0000, reached through ACPM (DT 8623) | works | libsb/acpm.c | S pmic; A StarbiteACPM | Reads are free. Writes go through the gate: LDO34 for the codec, the RTC latch, touch and card rails. There is **no S2MPS19** on this phone; that is the 9820 (STARBITE-MASTER-PLAN.md 862). |
| ACPM | APM mailbox 0x14100000, IPC channel 2 in APM SRAM (DT 9115) | works | libsb/acpm.c, libsb/dvfs_levels.c | S soc/exynos/acpm; A StarbiteACPM | PMIC IPC and the listed DVFS levels. |
| CPU DVFS, clocks, cores | 4 x A55 + 4 x M3; ACPM DVFS; M3 OCP (DT exynos-ocp) | partial | libsb/smp.c, libsb/ocp.c, libsb/tmu.c, kern/hw.c, kern/heater.c | S cpufreq governor; A StarbitePMGR (M5 passed); W runs on A55 or 3 x M3; L s9p-cpufreq | Worker cores and the listed ACPM levels (A55 up to 1794 MHz, M3 per core count). The M3s are used only by heat, the Linux guest and UI drawing helpers. No cpuidle or suspend; screen-off only lowers clocks (gap #5 in proper-os-gaps). |
| Thermals: TMU | TOP 0x10070000 (BIG, LITTLE), SUB 0x10074000 (G3D, ISP) (DT 10499-10571) | works | libsb/tmu.c, kern/heater.c | S thermal; A StarbiteTMU (plan) | The hardware trip is switched off only in Survival "trip off", and put back. |
| Thermals: ADC thermistors | CMGP ADC 0x14230000, ch 0 AP, 1 USB, 2 Wi-Fi, 3 charger, 4 battery, 10 PA (UHR 4i) | works | libsb/adc.c | S battery thermistor | Channel 11 (battery ID / presence) is not read. |
| Watchdog | cl0 0x10050000, cl1 0x10060000 (DT 9361, 9372) | works | libsb/wdt.c | S; A StarbiteWatchdog | cl1 is the dead-man for risky steps, probe images and Survival "trip off". Whether it guards normal use is unverified. |
| RTC | S2MPS18 RTC block (ACPM type 2) | works | libsb/acpm.c, kern/services.c | S works (read); A StarbiteRTC | Read, plus a write from network time (alpha26 on the phone). Alarms run on the kernel's minute tick, not an RTC wake, so the phone stays on while an alarm is set. |

### Storage and USB

| Peripheral | Part / bus / address (source) | Starbite OS state | Where | Other ports | Notes |
|---|---|---|---|---|---|
| UFS | UFS 2.1 host 0x11120000 (DT 8015) | works | libsb/ufs.c, kern/slot.c | S works; A StarbiteUFS; L | Uses S-Boot's running host. Reads, plus fenced single-block writes (the state slot). The OS store is moving to the card. |
| microSD | dwmmc2 0x11500000; CD gpa1-5; LDO31 + LDO2 (DT 9903) | works | libsb/sd.c, kern/files.c, ui/shell_media.inc | A StarbiteSD (plan); L dw_mmc (milestone 2) | Reads exFAT. Writes only through the fence (`sd_write`). 3.3 V signalling only, so no UHS speed. |
| USB device (DWC3) | DWC3 DRD 0x10C00000, PHY 0x11100000 (DT 5276, 5313) | missing | none in sbos. In-repo: starbite/utility/usb_dwc3.c (Utility OS: HID, MSC), shim/starlte loader | S CDC-ACM console; A StarbiteUSB (CDC-NCM plan); L CDC-ECM; W USB link | Two GPL drivers for this controller are already in the repo. |
| USB host | same DWC3 in host role + MAX77705 VBUS | missing | none. starbite/s9usb on `claude/win-usb-host-7efe60` | W: inbox xHCI via an ACPI PNP0D10 table (T1 passed) | Needed for keyboards and sticks. |

### Graphics and media

| Peripheral | Part / bus / address (source) | Starbite OS state | Where | Other ports | Notes |
|---|---|---|---|---|---|
| Mali-G72 MP18 GPU | 0x17500000 (DT 5031) | partial | libsb/mali.c, kern/gpucomp.c, kern/gpuheat.c | S panfrost GLES; A StarbiteMali; W sbmali/maliwddm (D3D11); L panfrost | Compute jobs only: compositing, Liquid Glass, blur, heat. No tiler, fragment or GL. The gpucomp self test passed on the phone (b9347cf). |
| Video codec MFC | mfc0 0x178D0000 (DT 8372) | missing | none | S: H.264 / HEVC decode (`mfc=5`, off by default, seen on the phone) | Photos and Music have no video. |
| G2D, SMFC (JPEG) | g2d 0x17620000, smfc 0x17700000 (DT 8251, 8290) | missing | none | none known | Low value: the GPU and software JPEG (ui/shell_media_img.inc) cover it. |
| IVA / SCORE DSP | 0x17200000, 0x16D00000 (DT 8112, 8152) | out of scope | none | none | Closed firmware; no user-facing use. |

### Radios

| Peripheral | Part / bus / address (source) | Starbite OS state | Where | Other ports | Notes |
|---|---|---|---|---|---|
| Wi-Fi | BCM4361 on PCIe; WLAN_REG_ON gpg0-5 (DT 13328) | works | kern/wifi.c, starbite/wifi (bcmw, sbnet, sbtcp, TLS) | S works (brcmfmac, WPA2); A StarbiteWiFi (plan); W sbwifi; L brcmfmac | WPA2 join, DHCP, TCP, TLS 1.2, HTTPS (P3o on the phone). Open: WPA2 on 2.4 GHz-only networks times out; no hardware RNG; one fetch at a time. |
| Bluetooth | BCM4361 BT on UART_BT 0x10840000 (gpq0-0..3); BT_EN = PCAL6524 P0_7; BT_WAKE gpg2-0; HOST_WAKE gpa2-3 (DT 13795; UHR 6) | offline-built (drivers track) | see section 0 | S partly (patch from VENDOR, 4 Mbaud, inquiry, LE scan, iOS stack, a paired device stays connected) | Needs the PCAL6524 expander first. |
| Modem: mobile data | Shannon CP; shmem 0xF6E00000; mif_pdata (DT 12152, 12518) | works | kern/modem.c, starbite/modem | S: no (CP not started) | LTE data and HTTPS over LTE in Next (P3o on the phone). |
| SIM | through the CP | works | starbite/modem (sbm_cell.c) | none | Registration with the attach APN passed (simtest-a2). Tests ran with the SIM PIN off; on-screen PIN entry is unverified. |
| Modem: calls | CS fallback from LTE | offline-built | kern/modem.c (KMODEM_CALLS), ui/shell_comms.inc | none | In P3u/P3v. No VoLTE (needs Samsung's IMS). Emergency numbers are refused by policy. |
| Modem: SMS | Samsung IPC SMS group | offline-built | starbite/modem (sbm_sms.c), ui/shell_comms.inc | none | Send and receive, concatenation, UCS-2, a RAM inbox. Saving across restarts is in progress. |
| Modem: call audio | ABOX Calliope VSS + codec mic + MAX98512 | offline-built | kern/abox.c, kern/codec.c, kern/amp.c | none | `audio_why` names the missing part on the call screen. |
| GNSS | Broadcom BCM4775 B0 (BCM47752 in GNSS.md); DT says `samsung,exynos54xx-bcm4753` (DT 13787) | missing | kern/gnssfix.c (reads Android's last fix from the card) | none | Samsung's closed gpsd computes the fix on the host side. We have no own fix (docs/GNSS.md). |
| NFC | `sec-nfc` at 0x27, hsi2c 0x108E0000; VEN P1_3, FIRM P1_1, CLK_REQ gpa2-6, LDO38 (DT 6300; UHR 2) | missing | none | none known | Effort L. "S3NRN82" is a guess. |
| Secure element (eSE) | `ese_p3`, spi 0x108A0000 (DT 7649) | out of scope | none | none | Owned by TrustZone. |
| MST | `sec-mst` (gph0-6 / gph0-7; battery `mst_en`) (DT 13773, 820) | out of scope | none | none | Samsung Pay; needs the TEE. |
| FM radio | no DT node | out of scope | none | none | The S9 has none (STARBITE-MASTER-PLAN.md 868). |

### Sensors

| Peripheral | Part / bus / address (source) | Starbite OS state | Where | Other ports | Notes |
|---|---|---|---|---|---|
| Sensor hub | MCU inside the BCM4775; SPI USI_CHUB00 0x13AC0000 (DT 7904 `ssp,BCM4773`); NSTDBY gpg0-2 | partial | kern/shub.c, kern/shub_proto.c | S partly (all six sensors at 50 Hz, seen 3 Nov) | The probe passed on the phone (2026-11-11). Firmware comes from the card (`/STARBITE/FW/BCM4775.BIN`, never in git). |
| Accelerometer | ST LSM6DSL behind the hub | works | kern/shub.c | S | Used for the level. |
| Gyroscope | LSM6DSL behind the hub | offline-built (drivers track) | see section 0 | S read (units not confirmed) | |
| Magnetometer | AKM type behind the hub | works | kern/shub.c | S | Compass. The scale is from the hub's frames. |
| Barometer | LPS22H behind the hub (DT 5995 `pressure-sensor`) | offline-built (drivers track) | see section 0 | S | |
| Proximity + light | TMD4906 behind the hub | offline-built (drivers track) | see section 0 | S (proximity reached iOS) | |
| Step counter / detector | hub sensors 18 / 13 | works | kern/shub.c | S | |
| HRM / SpO2 | MAX86915 at 0x57, hsi2c 0x108E0000; S2MPB02 LDO8 + S2MPS18 LDO38 (DT 6317; UHR 1) | missing | none | none known | A raw PPG is effort S-M. BPM and SpO2 need Samsung's closed algorithm. |
| Hall sensors | flip cover gpa3-6; certify gpa0-2 (DT 12950, 12958) | missing | none | none known | Effort S: GPIO reads only. |
| SAR grip | SX9320 at 0x28, hsi2c 0x104C0000 (DT 6034, status okay) | missing | none | none known | Low user value. Whether the CP needs the grip state for SAR power back-off is unverified. |

### Haptics, lights and audio

| Peripheral | Part / bus / address (source) | Starbite OS state | Where | Other ports | Notes |
|---|---|---|---|---|---|
| Vibration (LRA) | MAX77705 haptic (0x66: MAINCTRL1, MCONFIG) + PWM timer 1 at 0x10510000 on gpp3-1, boost gpg1-7 (DT 12599; UHR 4g) | works | kern/vib.c | S works | Pulses for alarms, timers, calls, SMS and key taps. Patterns are being written. |
| Torch / flash | S2MPB02 at 0x59, hsi2c 0x10920000 (USI10); torch1 gpg2-4, flash1 gpg3-5 (DT 6447, 13178) | partial | kern/torch.c, kern/fled.c | S written, not seen | Torch up to 240 mA works. The 1250 mA flash pulse stays off: reserved register 0x63 reads 0x00, not 0x7F, so the 125 ms hardware timer is unconfirmed (STARBITE-OS-1.1.md). |
| Camera PMICs | S2MPB02 camera LDOs (0x59); S2MPB03 at 0x56, hsi2c 0x14300000 (DT 6888) | missing | none | S drives the front camera's supplies | Needed for any camera. |
| Notification LED | MAX77705 RGB at 0x4A, LEDEN 0x30, BRT 0x31-0x34 (DT 12530 `rgb`, led_w/r/g/b; UHR 4f) | missing | none | none known | **The S9 has one.** Effort S. |
| IR blaster | no DT node | out of scope | none | none | The S9 has none. |
| ABOX audio subsystem | 0x17C50000, GIC 0x17CF0000, RDMA/WDMA/UAIF (DT 9920) | offline-built (call path + media playback) | kern/abox.c | S partly (ABOX with its CPU off: RDMA0 -> SIFS1 -> UAIF1) | Calliope firmware and the VSS call route are read from VENDOR. Playback will bypass ABOX's CPU. |
| Codec | Cirrus CS47L93 ("Madera") on spi 0x10470000 (USI01), reset gpg1-0, LDO32 / LDO34 (DT 7409) | offline-built | kern/audio.c (0.7 read-only probe), kern/codec.c (call mic) | S works (ID, FLL, main mic) | The 0.7 probe's result on the phone is unverified. |
| Amplifiers (speaker / earpiece) | MAX98512 pair at 0x39 + 0x3a, hsi2c 0x108C0000 (USI07) (DT 6210) | offline-built (in-call) | kern/amp.c | S works (tones and iOS sounds played) | Earpiece for calls; the loudspeaker is capped. No media playback yet: Music says "no sound output". |
| Microphones | codec inputs (main mic IN3L) | offline-built (call) | kern/codec.c | S works (main mic recorded) | Mic for apps: offline-built (snd.c / codec_media.c). |
| 3.5 mm jack | codec jack detect + headset mic (DT `sound`, star-madera) | offline-built (drivers track) | see section 0 | none known | |

### Cameras and secure parts

| Peripheral | Part / bus / address (source) | Starbite OS state | Where | Other ports | Notes |
|---|---|---|---|---|---|
| Rear camera | S5K2L3 at 0x10, hsi2c 0x10860000. On hsi2c 0x10870000: actuator AK737x 0x0C, OIS 0x24, aperture AK7372 0x4C. Plus fimc_is spi 0x10850000 (DT 5729-5777, 7329) | missing | none | S missing | Hardest camera: dual aperture, OIS, 12 MP. A second actuator node at 0x0F (hsi2c 0x10890000) is unexplained (unverified). |
| Front camera | S5K3H1 at 0x35, hsi2c 0x108F0000; actuator 0x0C and EEPROM 0x51 on hsi2c 0x10900000 (DT 6358, 6391, 6400) | offline-built (drivers track) | see section 0 | S works (1632 x 1224 at 30 fps, software demosaic, no ISP) | an earlier port's route is the model. |
| Iris camera + IR LED | S5K5F1 at 0x10, hsi2c 0x108D0000 (`fimc-is-secure`); IR LED = S2MPB02 FLED2 (DT 6258) | out of scope | none | S written, no frame | Eye safety and the secure path. fled.c never writes the IR registers. |
| ISP | fimc_is 0x16290000, CSIS 0x16230000..0x16260000 (DT 11467-11539) | out of scope | none | S: not used | Closed ISP firmware. Use the sensor's raw stream plus software or GPU demosaic. |
| Fingerprint | Egis ET510, spi 0x10450000, `secure-mode` (DT 7366) | out of scope | none | S: the `fp_s9` probe panics | TrustZone owns the SPI. |
| TEE (Kinibi) | `samsung,exynos-tee` (DT 10243) | out of scope | none | S `tee_probe` (not run) | Not a user feature on its own. |
| DisplayPort / DeX | displayport 0x11090000, PHY 0x110A0000; mux PCAL6524 P0_1 / P0_4 (DT 5186, 5195) | missing | none | none known | Low priority. |
| GPIO expander | PCAL6524 at 0x22, hsi2c 0x10930000, reset gpg3-6 (DT 6678) | missing | none | S (for BT_EN) | Shared dependency of Bluetooth, NFC, wireless charging and the DP mux. |

## 2. By state

### Works
- Display: DECON flips, DCS, ALPM and refresh-on-change.
- Touch and the four keys.
- Fuel gauge, ADC thermistors, TMU.
- S2MPS18 and ACPM, the RTC (read and write), the watchdog.
- UFS (fenced writes), microSD (read; fenced writes).
- Wi-Fi with WPA2 and HTTPS.
- Mobile data and the SIM.
- Accelerometer, magnetometer and steps through the hub.
- Vibration.

### Partial
- **Brightness**: no HBM, no auto-brightness.
- **DPP**: one plain channel only.
- **Charger**: read-only; no charge control.
- **USB-C**: VBUS and BC status only, no PD.
- **CPU**: no cpuidle or suspend.
- **Mali**: compute jobs only.
- **Torch**: torch works; the flash pulse is held off.
- **Sensor hub**: 3 of its 7 sensors.

### Offline-built (not yet run on the phone)
- Calls and SMS (P3u / P3v).
- Call audio: ABOX, codec mic, MAX98512.
- OTG power out (Survival).
- The codec's 0.7 read-only probe: built, but its phone result is unverified.

### Missing
Being written by the drivers track now:
- media playback, mixer and tones;
- the app mic;
- jack and headphones;
- Bluetooth;
- the front camera;
- gyro, barometer, proximity and light;
- haptic patterns.

Not started:
- USB device and USB host;
- MFC video, G2D and JPEG;
- rear camera and the camera PMICs;
- notification LED, hall sensors, HRM, SAR grip, NFC;
- wireless charging control;
- DisplayPort;
- the PCAL6524 expander;
- our own GNSS fix.

### Out of scope
- S2DOS05: S-Boot powers the panel.
- IVA / SCORE: closed firmware.
- Iris camera and IR LED: eye safety, secure path.
- ISP: closed firmware.
- Fingerprint, eSE, MST, TEE: owned by TrustZone.
- FM radio and IR blaster: the S9 has neither.

## 3. Next drivers, by value for the user

1. **Speaker and earpiece playback, plus the phone test of call audio.**
   - Value: ringtones, alarms with sound, Music, sound for Linux apps.
   - Calls become usable (already built; needs one attended test).
   - Work: ABOX bypass + MAX98512 + mixer (offline-built, 2026-11-12).
2. **The proximity sensor.**
   - A phone call needs it: it turns the screen off at the ear, so the cheek does not tap the screen.
   - The light sensor comes with it (same TMD4906) and gives auto-brightness.
   - Work: hub sensor ids 7 / 9 (offline-built, 2026-11-12).
3. **Calls and SMS on the phone (attended).**
   - The code is done. SMS saved across restarts is in progress.
4. **Jack, headphones and the app mic.**
   - Value: private listening, voice memos, the headset mic in calls (offline-built, 2026-11-12).
5. **Cheap iOS-like polish (each effort S, no risky writes).**
   - Hall sensor: the cover closes, the screen goes off.
   - Notification LED: missed calls and SMS, charging.
   - Battery health page: FullCapNom / DesignCap, cycles.
6. **USB device.**
   - Value: files and logs to the Mac, a network link.
   - Reuse starbite/utility/usb_dwc3.c: same repo, same GPL licence.
7. **Bluetooth.**
   - Expander, UART, patch, HCI, then LE (offline-built, 2026-11-12).
   - Headphones need A2DP + SBC on top. That is a large stack, so say so before promising "music on AirPods".
8. **Front camera, then rear.**
   - Value: the Camera and Photos apps. an earlier port shows the front path works without the ISP.
9. **Gyro and barometer.**
   - Value: steadier compass, altitude in Survival (offline-built, 2026-11-12).
10. **USB host, then MFC video decode.**
    - USB host: keyboards and sticks for Linux apps.
    - MFC: video in Photos and Linux apps.
11. **Later:** NFC, HRM raw PPG, DisplayPort, wireless-charging status.

Not a peripheral, but as important as any row above: standby power (cpuidle, M3 cluster off, RAM sleep). A phone
that lasts days needs it (gap #5).
