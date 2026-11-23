# Starbite ✦ every OS on a Galaxy S9

**One Samsung Galaxy S9 from 2018 running iOS, iPadOS, macOS, Windows 11, Linux, ChromiumOS, Android, and an operating system we wrote ourselves.** One boot loader picks between them, and our own GPU drivers draw the screen on the phone's Mali GPU.

Built in October–November 2026. **Website: https://starbite-s9.github.io**

## Highlights

- **macOS on a phone, natively.** A macOS Ventura and Sequoia desktop (menu bar, Dock, Finder) booted bare metal on the S9, composited on the Mali through our own Metal-on-Vulkan bridge. As far as we can find, the first native macOS desktop on a smartphone.
- **LTE through Samsung's Shannon modem with open code.** Network registration, a data connection and HTTPS on a real SIM, from our own OS, without Samsung's modem software.
- **10+ systems from one loader:** iOS 16 / 26 / 27, iPadOS 18, macOS 13 / 15, Windows 11, mainline Linux, postmarketOS, ChromiumOS, Android 17, One UI 8 and Starbite OS. All bare metal. No VMs.
- **Windows 11 with its own GPU driver on the Mali** (WDDM + Mesa), on all cores including Samsung's Mongoose big cores, with x86 apps, Steam, and DXGI flip model at 60 fps.
- **iOS drawn by the Mali GPU:** CoreAnimation first through OpenGL ES, then Metal through Vulkan (PanVK).
- **Starbite OS Next:** a phone OS written from scratch, not Linux. Its own kernel, GPU compositor, Wi-Fi with WPA3, Bluetooth, camera, sound, LTE data, Linux apps from its own App Store, and an emergency mode with Survival Heat, SOS and a medical card.
- **Engineer OS:** bare-metal tuning of every clock on the Exynos 9810. The Mali-G72 ran at 858 MHz, 43% past Samsung's highest table entry.

The full, honest world-first check (what was first, what wasn't, with sources) is in [WORLD-FIRSTS.md](WORLD-FIRSTS.md).

## ⚠ Read before you try anything

- **There is no installer. You figure it out.** This repo holds the source, scripts and notes we used on our own two phones. Getting it onto yours means reading code, fixing paths and working out the gaps yourself.
- **Exynos Galaxy S9 (SM-G960F) only.**
- **No Apple, Samsung, Google, Microsoft or Broadcom binaries are included**, and we won't share any. You need your own copies of every OS and of the phone's firmware (for example Wi-Fi firmware and the sensor-hub patch from your own phone's vendor partition).
- **It can brick your phone.** It repartitions storage and runs overclocking and heat tests. Back up everything. Use at your own risk.
- Some parts were removed before publishing because they contained other people's code or files we may not redistribute (see "Known gaps"). A few build steps therefore need work before they run.

## Branches

Each branch is one line of work, with its full history.

| Branch | Contents |
|---|---|
| `main` | Loader and OS store, XNU patch series, Apple drivers, macOS Sequoia work, tools |
| `starbite-os` | Starbite OS Next: kernel, shell, drivers, modem, Survival Heat, Face Unlock |
| `starbite-os-appstore` | The App Store engine (Debian packages as native apps) |
| `starbite-os-next-ui` | The Next UI track and startup previews |
| `starbite-os-design` | Design specs |
| `usb-control` | USB control channel for unattended work |
| `face-unlock` | Front-camera face unlock |
| `wifi-core` | Wi-Fi core: TCP, HTTPS on BearSSL |
| `win11-mali-gpu` | Windows WDDM GPU driver on Mesa for the Mali-G72 |
| `win11-m3-bigcores` | Windows on the Exynos M3 big cores, and the x86 fixes |
| `win11-bigcores-research` | The research that led there |
| `installer` | The scripts and steps we used on our second S9 (not a finished installer) |
| `ios27-drivers` | iOS 27 notes and soak runs |
| `utility-os-2` | Utility OS: the phone as keyboard, mouse, gamepad and drive |
| `linux-mainline-port` | Early mainline Linux port |
| `engineer-os-wip` | First Engineer OS scaffolding |

Start with `docs/` and `NOTES.md` (the raw working log) on the branch you care about.

## Known gaps

Removed before publishing, so these parts will not build as is:

- Code from another iOS-on-S9 project that we may not republish: the CoreAnimation GL path (`tools/ca-gpu`), the Mesa iOS DRM shim (`tools/mesa-ios/starbite-drm`), the iOS 27 driver tree and its tools, and the WPA module of the Wi-Fi core (`starbite/wifi/wpa.c`/`.h`). Writing clean replacements is a good first contribution.
- Broadcom Wi-Fi firmware and NVRAM, Samsung device trees, and Apple device-tree data and artwork. Take them from your own phone and your own copies.
- An old unrelated kiosk side project and its branding.

## Contributing

**Contributions are welcome.** The project is finished on our side, but the code is here for anyone who wants to take it further: clean replacements for the known gaps, an actual installer, support for the S9+ (SM-G965F), more drivers, ports to newer OS versions. Open a pull request. We may be slow to answer, but we will look.

## The next chapter: Starfold

Starbite is done. **Starfold** is next: everything we learned here, built properly on a brand-new flagship foldable, the Google Pixel 11 Pro Fold. Every OS again with full graphics on our own drivers, laptop modes, and much more. The name keeps its roots: "star" from *starlte*, the S9's codename, where it all began.

## Licence

Mixed, file by file. See [LICENSE](LICENSE) and [THIRD-PARTY.md](THIRD-PARTY.md). In short: the SPDX header in each file wins. XNU patches stay APSL-2.0, kernel-side and driver code mostly GPL-2.0-only, and our files without a header are MIT.

Not affiliated with or endorsed by Apple, Samsung, Google, Microsoft, Arm or Broadcom. All names are trademarks of their owners. Built by the Starbite project with Claude.
