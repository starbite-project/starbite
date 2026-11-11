# GNSS on the S9: can our own OS get a position fix?

Date: 2026-11-11. Research only, no phone touched. Goal: a receive-only fix (no modem, no SUPL/LTO/XTRA, no
network) for the survival map and Starbite OS's sunrise/sunset (today it takes a typed-in location,
commit ba38858).

Labels: **[read]** checked in a file named next to it; **[web]** a public source, linked; **[guess]** an
inference, not checked; **[unverified]** needs the phone.

Sources read (VM `starbite`, paths relative to `~`):
- `modem-vendor/x/` = the Lineage-built VENDOR with Samsung's G960F blobs (`build.prop`:
  `ro.vendor.build.fingerprint=samsung/starltexx/starlte:10/QP1A.190711.020/G960FXXUHFVG4`).
  `bin/hw/{gpsd,lhd,lhd.sh,gps.sh}`, `etc/gnss/gps.xml`, `etc/sensor/lhd.conf`, `etc/init/init.gps.rc`,
  `ueventd.rc`, `lib64/hw/gps.default.so`, `etc/vintf/manifest.xml`.
- The LineageOS 24 kernel (`android/lineage-24.0-20261025-unofficial-starlte.zip` → `boot.img` → Image,
  Linux 4.9.337) and the hub patch embedded in it (decoded to 413,864 bytes).
- Public: Broadcom's BCM4775X product brief, an earlier port's `SENSORS.md`, the Galaxy Tab Active3 port's
  `docs/re/sensorhub.md`, Pixel 6's BCM47765 `gps.xml`, Replicant's BCM4751 wiki, LKML. Links in §6.

---

## 1. The chip and the protocol

### 1.1 Which chip

- **BCM47752 (BCM4775 B0)**, GNSS plus sensor hub in one package. The kernel's patch header says
  `<AsicVersion=0x004775B0>` and `<Name=bcm_sensorhub_a1>`. The kernel also contains the part number
  `BCM47752KUB1G`. **[read]** (Lineage Image, offset 0x2364890)
- The hub firmware's version string is `Broadcom 4775.B0 FlagtailPatch ver. 410399`
  (`//depot/client/core/rel/Samsung/Beyond/20.22.351328/...`). **[read]** (decoded patch)
- It receives L1 only on the S9: `MultiCarrRFMode="GL_MULTI_CARR_RF_MODE_L1"`, `RfType="GL_RF_4755_BRCM_EXT_LNA"`,
  `FrqPlan="FRQ_PLAN_26MHZ_2PPM_49_152MHZ_300PPB"`. GPS, GLONASS, BeiDou and Galileo are on
  (`EnableBeidou`, `EnableGalileo`). **[read]** (`etc/gnss/gps.xml`)
- The chip: Broadcom's brief describes a Cortex-M4F (150 MHz, 1.125 MB SRAM, 1 MB ROM with a
  bootloader) plus a Cortex-M0, with SPI, UART and I2C to the host. **On-chip PVT is listed only for the
  BCM47758** ("On-chip PVT calculations available on BCM47758"). The S9 has the 47752. **[web]**
- The chip has no flash. A ~400 KB RAM patch is loaded over SPI after every power-up. On Android, `lhd`
  reads it from `/dev/bbd_patch` (`lhd.conf`: `LhePatch=/dev/bbd_patch`), and the kernel serves it from
  a table compiled into `drivers/sensorhub/brcm/bbdpl/`. **[read]**

### 1.2 Who computes the fix: the host (gpsd)

The split, read from the binaries:

| Piece | Runs on | Evidence |
|---|---|---|
| RF, correlators, tracking, the **measurement engine** ("ME") | chip (the patch) | patch strings `GulpMeChinookEngine`, `GlMe_PeIfImpl::...`, `patch_glmesrd_receiver_parameters_programmer.cpp`, `$MEIF:CtrlMsg(%u)` **[read]** |
| ME control: RF type, frequency plan, acquisition, signal aiding | host, in `gpsd` | gpsd strings `GlDineCtrl::SetRfType(%d)`, `GlDineCtrl::SetPMMSettings`, `GlMeSrdAcqMgrSm::autonomous_2000`, `PMM::peSigAid(...)`; `ERROR :Asic Type Not Supported: Check your RfType` **[read]** |
| **Position engine** ("PE"): ephemeris, Kalman filter, least squares, DOP, NMEA | host, in `gpsd` | gpsd's source paths `gll_dev/glposeng/glpe_inc/glpe_*.h` and the classes `GlPeLsq`, `GlPeFirstFix`, `GlPeFiltPos`, `GlPeNmeaGen` **[read]** |
| Transport, patch download, MCU resets | host, in `lhd` | lhd strings `Downloading Patch`, `LH Patch download Progress`, `FD:FD_WAIT4DOWNLOAD_START`, `Bbd::BbdPacket::OnRead` **[read]** |
| Low-power on-chip navigation ("MicroNav"): batching, geofences, breadcrumbs | chip, **only when gpsd asks** | patch: `AcquireGnssCore : Create MicroNav`, `GlOnChipReqMgr::...`, `4775 lat: %f, lon: %f`; text commands `$pglirm,req_breadcrumb,pvtOnChip,periodSec,%d,batchOption,%d` and `$pglirm,stop,pvtOnChip` in lhd, gpsd **and** the patch **[read]** |

**Answer to Q1.** The BCM4775 on the S9 is **host-based** for normal fixes. The chip produces measurements,
and Broadcom's closed `gpsd` controls the ME and computes the position. The 4775 firmware does contain an
on-chip navigator (MicroNav) for batching and geofencing. gpsd starts it, and it reads aiding that gpsd
provides (gpsd: `LTO reading complete for MicroNav`). Whether it can get a cold fix with no gpsd is
**unknown [guess: no]**. Broadcom sells cold-capable on-chip PVT only as the 47758 (§1.1).

### 1.3 The protocol stack (Android)

```
app / framework ── HIDL android.hardware.gnss@2.0/1.1 (etc/vintf/manifest.xml)
   └ vendor.samsung.hardware.gnss@2.0-service + lib64/hw/gps.default.so (legacy libhardware module:
       HMI, gps_get_hardware_interface; class GpsiHook)
        ⇅ named pipes /data/vendor/gps/.gps.interface.pipe.to_gpsd / .to_jni  ("GPSI" IPC)
   gpsd -c /vendor/etc/gnss/gps.xml   (8.2 MB, bionic, /system/bin/linker64)
        ⇅ <hal PortName="lhd">  →  named pipe %s/.pipe.gpsd_to_lhd
   lhd /vendor/etc/sensor/lhd.conf    (LheName=bbd, LheBbdPacket=/dev/ttyBCM, LheBbdControl=/dev/bbd_control,
        ⇅                              LheBbdSensor=/dev/bbd_sensor, LhePatch=/dev/bbd_patch)
   kernel BBD (GPL): bbdpl/bbd.c, bbd_rpc_lh.c, bcm_gps_spi.c, ssp_bbd.c
        ⇅ SPI (USI_CHUB00, spi@13AC0000, "ssp,BCM4773") + MCU_REQ / MCU_RESP / HOST_REQ / NSTDBY pins
   BCM47752 (ROM + RAM patch: ME + MicroNav + Samsung sensor-hub app)
```
All of this is **[read]** from `init.gps.rc`, `gps.xml`, `lhd.conf`, `ueventd.rc` (lines 223–229: `/dev/bbd_*`,
`/dev/ttyBCM*`), the gpsd, lhd and gps.default.so strings, and the kernel's source-path strings. The SPI node
and pins come from an earlier port's `SENSORS.md` (Samsung DT `spi@13AC0000` / `BCM4773@0`). **[web]**

The layers on the wire (documented from GPL kernel code by an earlier port and the Tab Active3 port) **[web]**:
1. SSI SPI frames: read `[0x50]`, write `[0x58]`.
2. Packets: `0xB0 0x00 … 0xB0 0x01`, escaped, with seq, flags and CRC-8.
3. RPCs: `[id][len][data]`.
4. Samsung's SSP sensor protocol, for the hub.

The Tab Active3 notes list "**GNSS (gpsd, RPC ids 0x00-0x1F) | gpsd/lhd | proprietary, out of scope**".
No GPL file documents the GNSS RPCs. gpsd's ME/PE traffic travels through lhd as opaque RPC payloads.

**NMEA.** The chip does not stream NMEA to the host. gpsd **generates** NMEA on the host
(`GlPeNmeaGen`, `$GPGGA,%s,%02ld%02ld.%06ld,%c,`, plus Broadcom's `$PGLOR,…` sentences) **[read]**. gpsd
accepts an `NmeaOutName` config key, along with `NmeaToLogCat` and `DisablePglorNmeaCallback`
(the S9 sets the last one to true) **[read]**. Pixel 6 (BCM47765, same gpsd family) sets
`NmeaOutName="/data/vendor/gps/nmea_out"`, so gpsd writes NMEA to a path there **[web]**. The S9's
`gps.xml` has no `NmeaOutName`. NMEA reaches Android only through the HAL's `gpsNmeaCb`
(`GpsiHook::gpsNmeaCb` in gps.default.so) **[read]**. The `$pglirm,…` commands are NMEA-style **text
commands** to Broadcom's "IRM" engine (gpsd: `IRM: Wrong NMEA address field: "%s", expected: $pglirm`)
**[read]**.

---

## 2. A cold fix with no network

**Yes in principle: autonomous mode exists and needs no network.**
- gpsd's own acquisition modes include `autonomous`, `autonomous_100`, `autonomous_300`, `autonomous_2000`
  and `autonomous_fast_start_300` **[read]**. Assistance can be switched off per item: `AsstIgnoreLto`,
  `AsstIgnoreCbee`, `AsstIgnoreAll`, `LbsEnable`, `SuplEnable` **[read]**.
- The S9's config turns network aids **on** (`SuplEnable="true"`, `LbsEnable="true"`,
  `LbsServer="BCMLS2.glpals.com"`, `RtiConfig="gllto.glpals.com:80/rtistatus3.dat"`, `HttpSyncLto="true"`).
  With no network those downloads simply fail. Every Android phone in airplane mode still gets fixes
  **[guess: general GNSS behaviour; not timed on this phone]**.
- `IgnoreJniTime="true"`: gpsd ignores the time Android injects **[read]**. So a cold start does not depend
  on network time anyway.
- gpsd keeps state across sessions in `NvStorageDir=/data/vendor/gps/`: the last position, time,
  almanac and ephemeris (`NvStorageName`, `NvmemWritePeriodMs`, the GLL startup line
  `Tim[%c],Osc[%c],Pos[%c],Eph[%c],Cbee[%c],Lto[%c],AEph[%c]`) **[read]**. CBEE is Broadcom's
  self-extended ephemeris: the `CbeeDir` / `cbee.cbee` / `AsstIgnoreCbee` keys exist. It is computed on
  the phone from earlier broadcast ephemeris, so it helps offline **[guess, from the name and keys]**.

**Time to first fix [web + guess].** A receiver must decode each satellite's ephemeris from subframes 1–3,
which repeat every 30 s. That takes 18–36 s once a satellite is tracked. Vendor cold-start figures are
39 s (NovAtel OEM7, boot included) to over 45 s (Orolia's tester guide). With a weak or obstructed sky
it can take minutes. If the almanac is needed from scratch, the full almanac takes 12.5 min, although a
fix does not need it. For the S9, outdoors with a clear sky:

| Start | Expected TTFF | Why |
|---|---|---|
| Cold (no NV data, or data weeks old) | ~30–60 s, minutes under trees or at a window | ephemeris must be decoded off the air |
| Warm (almanac + rough position/time in NV, ephemeris > 4 h old) | ~25–40 s | the search is narrowed, but ephemeris is still needed |
| Hot (ephemeris < ~2–4 h old in NV) | ~1–5 s | no decode needed |

None of these numbers was measured on our phone **[unverified]**. Multi-constellation helps, because more
satellites are in view at once. GPS, GLONASS, BeiDou and Galileo are all enabled.

---

## 3. Paths

### (i) Bare metal: reimplement the chip protocol in Starbite OS. **Not feasible as a project.**

What is known and doable:
- The hub boot: SPI, pins, ROM sync, the 452-write patch download, the application start, SSP.
- an earlier port runs it from its own XNU kernel on this exact phone: "ASIC `004775b0` ... the 452 writes in 3.2 s
  ... WHOAMI 0x55". The Tab Active3 port runs it from mainline Linux **[web]**.
- Starbite OS needs this anyway for the compass and level (commit ba38858: "Compass and level need the
  BCM4775 sensor hub driver").

What is missing:
- The GNSS half: RPC ids 0x00–0x1F carry gpsd's control of the ME and the measurements back. They have
  no GPL reference, no public documentation and no open implementation **[web]**.
- gpsd does far more than "read measurements". It sets the RF type, the frequency plan and the LNA mux.
  It runs acquisition state machines and power-mode (PMM) policy, and it feeds signal aiding back to the
  chip. Then it is the whole position engine (§1.2) **[read]**.
- To replace it we would have to reverse engineer an 8.2 MB stripped C++ binary and its traffic, then
  write an ME driver plus a PE. The PE part is textbook: RTKLIB-class least squares and ephemeris
  decoding. The ME interface is the unknown.

Precedent:
- Replicant started a free driver for the much simpler BCM4751 in 2012 (the "MEIF" protocol). It got as
  far as serial setup, MEIF parsing and patch upload, and its tracker issue for GPS support is still open
  **[web]**.

Estimate and verdict:
- Many months of reverse engineering with captures, and it may never produce a fix **[guess]**.
- Rejected for now. The one cheaper variant is the on-chip MicroNav, in (iv).

### (ii) Samsung's lhd + gpsd on the Linux guest (M3 cores). **Possible, 3–6 weeks [guess], medium-high risk.**

This is the Tab Active3 port's "Option C: stopgap with stock lhd". It avoids reverse engineering
entirely **[web]**.

1. **Kernel.** Port Samsung's GPL BBD (`bbdpl/bbd.c`, `bbd_rpc_lh.c`, `bcm_gps_spi.c`, and the small part
   of `ssp_bbd.c` that lhd needs) from 4.9 to our Linux 7.1. It must expose `/dev/ttyBCM`,
   `/dev/bbd_control`, `/dev/bbd_sensor` and `/dev/bbd_patch` (the patch is a data table, ~400 KB). It
   also needs the sysfs files that `lhd.conf` names: `/sys/class/sec/sensorhub/mcu_power` (LDO37) and
   `/sys/class/sec/gps/GPS_PWR_EN/value` (NSTDBY).
   The guest's device tree must give Linux:
   - USI_CHUB00 / spi@13AC0000 and its CMU_CHUB gates
   - the four pins
   - the LDOs: LDO37, plus LDO35/42/43 if the sensors are wanted. These need on/off control, so
     **the S2MPS18 PMIC bus is shared with Starbite OS.** That is a two-kernel conflict to design for:
     Starbite OS switches them on before the guest starts **[guess]**.
2. **Bionic user space in the guest.** Copy Lineage's `linker64` plus the libraries gpsd and lhd need
   (`readelf -d`) **[read]**:
   - liblog, libutils, libcutils, libhardware_legacy, libz, libssl, libcrypto, libc++, libc, libm, libdl
   - vendor `libwrappergps.so`, which pulls in libsecril-client, libsqlite and libbinder
   - `android.frameworks.sensorservice@1.0`, libhidlbase and libandroid_net

   Then:
   - Provide an Android property area: gpsd reads `ro.spid.gps.RfType` and `ro.spid.gps.FrqPlan`
     **[read]**.
   - Possibly run hwservicemanager, if gpsd blocks on the sensor service. Unknown **[guess]**.
   - Write a ~200-line bionic client: it `dlopen`s `gps.default.so` (`HMI`), calls
     `init/start/set_position_mode`, and forwards `gps_location_callback` to Starbite OS over the sbshm
     ring.
   - Run `lhd` and `gpsd` as the Android rc does.

   libhybris (Ubuntu Touch, Sailfish) is the public precedent for running Android GPS HALs on Linux.
   It assumes the downstream Android kernel, though, which we would replace with the port above
   **[guess, from general knowledge]**.

Gains:
- A live fix inside Starbite OS while it runs, with no reboot.
- The same BBD port gives lhd's sensor path too.

Risks:
- The PMIC is shared between the two kernels.
- The guest has no memory isolation (STARBITE-OS-LINUX-M3.md §1).
- Proprietary Samsung binaries end up in a guest image. They must stay private and never be committed
  (same rule as the vendor blobs).
- An Android 10 blob runs on a hand-made environment, so each missing service is a debugging session.
- Every test needs the phone outdoors with sky view.

### (iii) Android as the "GPS box" and a file on the card. **Recommended: 1–3 evenings, low risk.**

What is in place:
- Android's stack on this phone is Samsung's own and complete. LineageOS 24 is installed with the vendor
  above. One UI (Android 10) is the known-good fallback.
- Starbite OS already reads the card's exFAT (Files app, `SYS_FILE_LIST`; STARBITE-OS-1.2.md) and has
  the state slot.

Design:
1. **Producer (Android).** A tiny app, one Activity plus a foreground Service, ~150 lines. It runs in
   airplane mode, with no SIM and Wi-Fi off.
   - It requests `LocationManager.GPS_PROVIDER` updates.
   - On the first fix, then every N minutes while open, it writes one text line to the removable card,
     e.g. `STARBITE/GNSS/LASTFIX.TXT`. It writes atomically: a temp file, then rename.
   - The line format is
     `SBGNSS1 <UTC ISO time> lat=<deg> lon=<deg> alt=<m> acc=<m> sats=<used>/<seen> ttff=<s> src=gps`.
   - It also appends raw NMEA from `OnNmeaMessageListener` to `NMEA-<date>.LOG` for the record.
   - The app-specific directory on the removable volume (`getExternalFilesDirs()[1]`) needs no storage
     permission. A fixed top-level path needs a privileged copy. SELinux is permissive in our bootargs
     (ANDROID-PLAN.md §1), so the master plan's "vendor-side overlay service" can do that copy **[guess]**.
   - No-build stopgap: the open-source GPSTest app shows TTFF and satellites and logs NMEA. It needs a
     download, which needs approval. It would only verify the hardware, not deliver to Starbite OS.
2. **Consumer (Starbite OS).**
   - Read `LASTFIX.TXT` at boot.
   - Show the coordinates and their age.
   - Feed the sun-times calculator instead of the typed-in location.
   - Seed the state slot's "last known time" from the fix's UTC time when it is newer.
3. **Cost per fix.** One Android boot, the fix (30–60 s cold, seconds when hot), then a reboot back.
   Repeating it within ~2–4 h is a hot start, because gpsd keeps ephemeris in `/data/vendor/gps/`.

Risks:
- **[unverified] Whether LineageOS 24 (Android 17) still serves the HIDL `android.hardware.gnss@2.0`
  that this vendor declares.** If it does not, One UI is the fallback. One UI has the loud boot chime,
  so it cannot be used at night (boot-safe.sh rule).
- Android must not "format" a card it partly cannot read. The OS store is moving to the card, so check
  how vold treats the extra partitions before the store moves **[guess]**.
- The tests need sky view. The USB-tethered workflow means a window sill at best, which gives a slower
  or degraded fix.

### (iv) Other leads

- **On-chip MicroNav breadcrumbs (research lead, speculative).**
  - The 4775 patch on this phone contains Broadcom's on-chip navigator (`AcquireGnssCore : Create MicroNav`,
    `GlOnChipReqMgr`, `4775 lat: %f, lon: %f`, `GetIndoorStatus: Disable it because it's PVT test mode`).
  - lhd, gpsd and the patch all carry `$pglirm,req_breadcrumb,pvtOnChip,periodSec,%d,batchOption,%d`
    **[read]**.
  - If a bare-metal hub driver, which Starbite OS needs for the compass anyway, could send that text
    command and read breadcrumb positions back, we would get fixes with no gpsd.
  - Unknowns:
    - the RPC id that carries the command
    - the response format
    - whether MicroNav runs without gpsd configuring the ME first (RfType, frequency plan, LNA mux)
    - whether it can do a cold start without ephemeris from the host. Broadcom's brief markets on-chip
      PVT only for the 47758 **[web]**.
  - Cheapest test: on One UI, have an app request batched locations (FLP) and capture the SPI traffic
    with the stock kernel's own frame dumper (sysfs `/sys/devices/platform/bbd.0/ssi_trace`,
    `.../passthru`, `.../debug`; init.gps.rc). Then look for `$pglirm` and the breadcrumb replies.
    Days of work, low odds **[guess]**.
- **Existing open drivers:**
  - an earlier port (iOS 16.5 on this phone) claims the sensor hub ("the sensors, the hub in the BCM4775 read at
    50 Hz", `shub=6`) and orientation/proximity in iOS. It claims **no GNSS** (SENSORS.md and its
    website mention none) **[web]**.
  - The Tab Active3 mainline work boots the same hub family lhd-free and lists GNSS as "out of scope"
    **[web]**.
  - Mainline Linux's GNSS subsystem only covers power and `/dev/gnss0` for chips whose protocol is
    known (LKML thread on the BCM4751 binding) **[web]**.
  - Nobody has an open GNSS driver for BCM477x as far as found.
- **External receiver.** A USB GNSS dongle (NMEA over CDC-ACM) is the only open-protocol path for
  Starbite OS. It needs USB host mode, which is untested on our units (master plan §0.4) and adds hardware.
  It is noted for completeness and is outside the locked lineup.

---

## 4. Recommendation

**Path (iii): Android (LineageOS 24) is the GPS box, and it writes `STARBITE/GNSS/LASTFIX.TXT` to the
card, which Starbite OS reads.**
- It is the only path that gets a real fix in days rather than months.
- It uses Samsung's own, complete GNSS stack.
- It changes nothing in the store or the boot chain.
- It matches the master plan's "Last known location: Android only" row.

Path (ii) stays the route to a *live* fix inside Starbite OS, if (iii) proves the hardware and the need
grows. The MicroNav lead fits into the sensor-hub driver work as a capture-first experiment.

**First concrete step.** Verify that GNSS works at all on LineageOS 24 before writing anything. This is
read-only and installs nothing, so path (iii)'s app is not built on an untested stack.

The test (a phone session, by day):
- Boot Lineage, then switch on airplane mode, Wi-Fi off, location on.
- Over adb, run `logcat -b all -s gpsd lhd GnssLocationProvider GnssManager` and check
  `dumpsys location`, `ps -A | grep -E 'gpsd|lhd|gnss'` and `lshal | grep gnss`.
- Then open any location consumer: Lineage's built-in apps, or the Settings → Location page.

What to look for:
- `ESW:READY` / `LH Patch download Progress … 100%` from lhd.
- gpsd's `GLL_Startup Type:` line.
- `IGnss` being registered.

If gpsd starts and HIDL GNSS is registered, build the ~150-line app (the next step, in the VM). If not,
repeat the check on One UI (daytime only).

---

## 5. Facts for later (all **[read]**)

- Vendor GNSS files:
  - `bin/hw/gpsd` (8,246,208 B, BuildID md5 49eca4bd…, Broadcom build `477611066_545200`,
    project `customers/samsung/StarFlagtailandroid`)
  - `bin/hw/lhd` (318,024 B, BuildID 11d688c9…)
  - `bin/hw/gps.sh`, which picks `gps.debug.xml`, `gps.issuetracker.xml` or `gps.gsi.xml` by flag
    directories in `/data/vendor/gps/`
  - `lib64/hw/gps.default.so` (264,895 B)
  - `etc/gnss/gps.cer` (SUPL TLS certificates)
- lhd: `PortName=tcp:127.0.0.1:300`, `LheClockMHz=100`, `LhePinMux=P17=M1` (external LNA),
  `LheMaxMcuResetTries=3` (set twice: 4, then 3), `Lhe477xDebugFlags=RPC:FACILITY=33:STDOUT_PUTS:STDOUT_LOG`.
- lhd's patch loader logs `<AsicVersion=0x`, `<PatchLevel=`, `<CustomerVersion=` and supports an "urgent
  patch" override (`Urgent patch found in %s`).
- The kernel's patch: `FormatVersion=0x00010003`, `BlobLength=413864`, `PatchAddress=0x00800161`,
  `PatchLevel=0`, `PostPatchLevel=1`, `Type=Application`. The blob is hex text in the kernel image.
- The hub firmware also carries the barometer driver (`Barometer_LPS22HB.c`), light/proximity code and
  `GPS_Register_Position_Callback`. The sensor hub and the GNSS share one MCU and one patch.

## 6. Public sources

- Broadcom BCM4775X product brief (4775X-PB100, 2017):
  https://faculty-web.msoe.edu/johnsontimoj/EE4980/files4980/BCM47755.pdf
- an earlier port, `SENSORS.md` (facts only, no code taken): https://github.com/example-project/refport-starlte ;
  site https://example.invalid
- Galaxy Tab Active3 port, `docs/re/sensorhub.md` (§7 "Proprietary vs GPL", §8 option C):
  https://github.com/exynos-linux/samsung-gtactive3wifi
- Pixel 6 BCM47765 `gps.xml` (NmeaOutName):
  https://android.googlesource.com/device/google/gs101/+/5bc5698/gnss/47765/config/gps.xml
- Replicant BCM4751 (MEIF, patch upload): https://redmine.replicant.us/projects/replicant/wiki/BCM4751 ,
  https://redmine.replicant.us/issues/1473
- LKML, BCM4751 GNSS binding (kernel = power + `/dev/gnss0`): https://lkml.iu.edu/2204.2/09646.html
- TTFF background: Penn State GEOG 862 "The Almanac, Time to First Fix and Satellite Health"
  https://courses.ems.psu.edu/geog862/node/1739 ; NovAtel APN-107
  https://hexagondownloads.blob.core.windows.net/public/Novatel/assets/Documents/Papers/APN-107-HotStart-on-OEM7/APN-107-HotStart-on-OEM7.pdf ;
  Orolia "A Guide for Testers of GPS Devices and Systems" https://www.orolia.com/documents/guide-testers-gps-devices-and-systems
