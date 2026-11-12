# Starbite OS: gaps against iOS as a real OS (2026-11-12, Track A)

The question: what still stops Starbite OS (Next, the GPU line, trunk `claude/starbite-os-05b163` @ 1cc8764) from
being a full phone operating system like iOS? It comes from a read of the shell (`starbite/sbos/ui/`) and of the
kernel (`starbite/sbos/kern/`, `starbite/libsb/`). Each gap has a size, an owner track (from
`starbite-os-8h-plan.md`), and a status for the 8-hour push.

**Size:** S = hours, M = a day, L = days, XL = a project.
**Status:** *now* = this push (by ~23:00), *next* = after the 1.0 candidate, *out* = not planned.

## 0. In one paragraph

Starbite OS Next looks and moves like iOS, and the frame path is GPU-composited (passed on the phone 2026-11-12).
What makes it feel like a demo rather than an OS is underneath:
- **almost nothing survives a restart:** notes, notifications, recents, call log, Control Centre toggles, brightness;
- **there is no passcode;**
- **locking or switching apps throws the app's screen away;**
- **Settings has three working rows;**
- **time zone, 12/24 h and language are fixed;**
- **the OS cannot update itself;**
- **native third-party apps cannot be installed** (Linux apps can).

The drivers go deeper than the OS-services layer. So this push's Track A work is services first: persistence,
passcode, lifecycle, notifications, time and locale, update, startup. Pages are Track B's; Track A writes their API
(`sbos-core-api.md` in the notes folder).

## 1. App model and multitasking

| Gap | Today | iOS | Size | Owner | Status |
|---|---|---|---|---|---|
| App state kept on switch / lock | `app_open` resets Phone tab, number, thread and Settings sub-page; closing resets Search and Notes; locking calls `app_finish_close` (`shell_lock.c:3101-3161`, `back_to_lock`). | An app comes back exactly as left, also after unlock. | S | A (lifecycle) + B (pages) | **now** |
| Lifecycle events for built-in pages | none: pages are C state | willResignActive / didEnterBackground / will terminate | S | A | **now** (`sh_app_*` hooks: show, hide, save) |
| Switcher depth | 4 recents (`SW_MAX`), Search and Linux apps left out | all running apps | S | B | now (B4) |
| Native third-party apps (.sba) | the SDK, EL0 slots and syscalls exist (`kern/app.c`, `sdk/sbapp.h`), but only apps linked into the image load; classic UI only; no signing | App Store installs | L | A + F | next (Linux apps are the install path this push, Track F) |
| Home: rearrange, folders, delete, App Library, badges | fixed layout; badges compiled out (`if (0)`, `shell_lock.c:1206`) | yes | M | B | next |
| Per-app memory / kill | Linux guest is the only real process model | jetsam | L | E | out |

## 2. Notifications

| Gap | Today | iOS | Size | Owner | Status |
|---|---|---|---|---|---|
| Persistence across restart | RAM only (kernel 8, shell 20) | kept until cleared | S | A | **now** |
| One posting API for everything | kernel `notif_post(who,title,text)`; shell `sh_notify`; Linux apps can't post | UNUserNotificationCenter | S | A (+E for Linux) | **now** (kernel side; Linux bridge = E) |
| Lock-screen list | only an "N Notifications" pill | the list on the lock screen | S | B | now |
| Per-app settings (allow, banners, sounds), Focus / Do Not Disturb | none | yes | M | A (store) + B (pages) | now: allow and DND keys; pages from B |
| Actions / reply / scheduled summary | none | yes | M | B | next |
| Sound | no media audio path | yes | M | I | next |

## 3. Lock, passcode, security

| Gap | Today | iOS | Size | Owner | Status |
|---|---|---|---|---|---|
| **Passcode** | none: a swipe unlocks (`set_lock`) | 6-digit passcode, back-off, wipe option | M | A (kernel + page draft) + B (restyle) | **now** |
| Auto-lock setting | kernel `screen_off_s` 30 s, no page | 30 s .. Never | S | A + B | **now** |
| Emergency access while locked | n/a | Emergency SOS, Medical ID from the lock | S | A + D | **now**: Emergency mode, SOS and the medical card never need the passcode |
| Biometrics | no driver | Face ID / Touch ID | XL | I | out |
| Encryption at rest | none; Wi-Fi passphrases in a plaintext store entry | Data Protection | L | A | next (the passcode hash design leaves room for a key) |
| Permissions (camera / mic / location / network per app) | static manifest bits in .sba; no user grant | prompts + Settings > Privacy | M | A + F | next |
| Image signing | CRC only | signed boot chain | L | J | next (Software Update checks a sha256 manifest now) |

## 4. Settings

| Gap | Today | Size | Owner | Status |
|---|---|---|---|---|
| A general settings store | one fixed binary struct `struct saved` v1 with flag bits packed into padding (`kern/settings.c`); apps have numbered `SET_*` ids | S | A | **now**: a key/value store (`sh_pref_*`) on the state slot |
| Working pages | Wi-Fi, Cellular, Display (4 steps, not even saved explicitly), Medical Card | M | B (B2) | now |
| Missing pages | General/About, Auto-lock & Passcode, Notifications, Date & Time, Language & Region, Accessibility, Wallpaper, Storage, Privacy, Software Update, Startup (OS chooser, firmware, recovery) | M | B pages; A + J APIs | now |

## 5. Persistence and files

| Gap | Today | Size | Owner | Status |
|---|---|---|---|---|
| **The store moved to the card** | libsb `store.c` still opens only the UFS store for `settings_save`, `sbos-state`, `wifinet`; after the iOS 27 USERDATA wipe nothing is saved at all | M | J (libsb home store) | **now**: critical path |
| What survives a restart | settings record, SOS / medical card / Last 2 % / location, last time | S | A | **now**: + notes, notifications, call log, recents, CC toggles, brightness, alarms, prefs |
| Files: write, create, delete | read-only exFAT on the card (`kern/files.c`, 64 KiB per read) | L | A | next (notes and prefs go in the state slot this push) |
| Per-app storage for native apps | one 4 KiB blob per app | M | A | next |
| Crash-safe writes | A/B halves with sequence + CRC, FUA + flush, read back | done | | |

## 6. Time and locale

| Gap | Today | Size | Owner | Status |
|---|---|---|---|---|
| **Time zone** | fixed to Helsinki (`utility/tz.c`); 7 zones compiled in, no picker | S | A | **now**: zones with DST rules + a setting |
| 12 / 24 h | always 24 h (`fmt_hm`) | S | A + B | **now** |
| Language / region formats | English only, strings in code | L | B | next (region: date order and first weekday now, with the zone) |
| Network time | NTP over Wi-Fi and LTE: done | | | |

## 7. Updates

| Gap | Today | Size | Owner | Status |
|---|---|---|---|---|
| **Software Update on the phone** | none: images go in from the Mac (`sbstore2.py`) or the loader's fastboot | M | A + J | **now**: from the card (`/STARBITE/UPDATE/`, image + sha256 manifest), applied by the loader at the next start, the previous image kept for rollback |
| Over the air (Wi-Fi / LTE) | the HTTP + TLS client exists (one fetch at a time) | M | A | next (same manifest, downloaded) |
| A/B with automatic rollback | the loader's crash judge exists | M | J | now if J's shell-mode judge lands; otherwise next |

## 8. Crash safety and recovery

| Gap | Today | Size | Owner | Status |
|---|---|---|---|---|
| Restart reason inside the OS | not shown | S | A + J | **now** (SBFW table: boot reason, last crash) → a notification "Starbite OS restarted because ..." |
| OS safe mode | none | S | A | **now**: after a crash, start without Linux / modem autostart / Wi-Fi auto-join once |
| Recovery from the OS | Bixby-held loader recovery only | M | J | **now** (Startup > Recovery pages) |
| Watchdog, panic screen, pmsg, loader crash judge | done | | | |

## 9. GPU everywhere

| Gap | Today | Size | Owner | Status |
|---|---|---|---|---|
| Fallback over 127 quads | `gpu_frame` returns -1 above `TK_GPU_MAX`; the CPU draws the frame | S | A | **now** |
| Scrims blur on the CPU | the focus sheet and CC scrims use `tk_comp_shown` + a CPU blur | S | A | **now**: the GPU's live backdrop blur (gpucomp v10) |
| Layer content drawn on the CPU | text, icons, glass bakes are CPU-rastered into layers, then composited on the GPU | XL | A | out (as iOS's CoreAnimation does too: layer contents are mostly CPU-drawn; compositing and effects are the GPU's) |
| Glass style per shape | V2 fields (variant, tint, press) in B's toolkit | S | A | **now** (lens quads use them) |
| Motion at full resolution | half resolution while moving | M | A | now/next |

## 10. Apps and drivers (other tracks; listed so the whole picture is in one place)

| Gap | Today | Owner |
|---|---|---|
| Camera | placeholder; no driver | I |
| Media audio (music, sounds, ringtones) | in-call route only | I |
| Bluetooth | none | I |
| Browser, Weather, Maps | empty screens / status page | E (Linux apps) + B |
| Contacts | "No Contacts" | B (+ A store) |
| Alarms editor in Next | `kern/alarm.c` has 4 alarms; Next only shows the next one | B page + A API (**now**) |
| Accessibility (text size, bold, reduce motion, reduce transparency) | none | B (A stores the keys) |
| Live GNSS | last fix from Android's card file | I |
| Deep idle / suspend to RAM | wfi only; screen-off A55 455 MHz | A | next |
| USB in the OS (MTP, tethering) | none (loader fastboot only) | I | next |

## 11. This push's Track A order

1. Persistence: the key/value prefs store and notes / notifications / call log / recents / CC / brightness kept
   (on Track J's home store).
2. Passcode + auto-lock + emergency access.
3. Lifecycle: apps kept across switch and lock.
4. Time zone, 12 / 24 h, region basics.
5. Notifications service v2 (kept, per-app allow, DND, restart reason).
6. GPU: no 127-quad fallback, GPU scrims, glass V2 styles.
7. Software Update from the card (with J), safe mode after a crash.
8. Integration: merge every track into the 1.0 candidate (~23:00), with one test sheet.

## 12. Status after checkpoint 1 (2026-11-12 18:45, trunk b8a2261; candidate 5624bca staged)

| Gap | State |
|---|---|
| Persistence (prefs, blobs; notes, NC, recents, call log, brightness, Wi-Fi) | **done** (kern/prefs.c, 75 host checks); on the card via Track J's home store + the card-only fix |
| Passcode, back-off, Auto-Lock, Emergency without the code | **done** (kern/passcode.c 86 checks, shell_passcode.inc) |
| Apps kept across switch and lock; on_save hooks | **done** (lifecycle in shell_lock.c, APPHOOK) |
| Time zones (69, DST), 12 / 24 h, alarms API + Clock's Alarm tab | **done** (kern/tzdb.c 3008 checks, shell_time.inc) |
| Notifications kept, per-app allow / banners, DND, notif_post_app | **done** (kernel + shell; pages: Track B) |
| Safe start, restart reason notification | **done** (sh_safe_mode, sh_boot_reason; the loader's table) |
| Restart / Shut Down | kernel side **done** (sh_restart, sh_power_off, sh_power_held); the sheet: Track B's P3x |
| Startup: OS chooser, firmware, recovery | **done** by Track J (Settings row: P3x) |
| GPU: 200 quads, glass V2 / V3, zero-copy Linux window | **done** (V3: LIQUID-GLASS G1 G2 G4 G5 G14 G15) |
| Software Update | proposed to Track J (stage on the card, the loader applies, -prev rollback) |
| Files write, encryption, permissions UI, native app install, accessibility pages, language | next |
| Drivers (sound, haptics, LED, camera, BT, sensors, battery health, USB) | merged, dormant until their phone probe; Track A wires them after it |
