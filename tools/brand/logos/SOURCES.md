# Menu v7 logos: sources, licences, trademarks

The real OS and company logos on menu v7's provider tiles and OS tiles. The source files are committed
in `src/` here, each in its own directory, with the sha256 below pinned in `menu7_logos.FILES`;
`tools/brand/menu7_logos.py --fetch` downloads them again and fails (exit 1, nothing replaced) if any
upstream file changed. `tools/brand/menu7-assets.py` rasterises them (rsvg-convert, straight to each
target size) into `build/brand/menu7.sb7`. Fetched 2026-11-03; licences as Commons records them (imageinfo extmetadata, same day). Nothing from the files is executed: they are checked for
scripts and external references, edited only by removing the whole elements named below, and rendered.

Every logo is a trademark of its owner. They are used here only to label the operating system or
vendor that the boot entry starts (nominative use), unmodified apart from colour where the owner's
guidelines use a monochrome version (Apple: white) and the removals listed. No endorsement is implied.

| Logo | Used for | Source URL | sha256 | Licence / trademark note | Edits |
|---|---|---|---|---|---|
| Apple logo | provider Apple (`SB7_P_APPLE`), tiles iOS 16, macOS Ventura, iOS 26 (told apart by the tile colours) and the Apple default | https://upload.wikimedia.org/wikipedia/commons/f/fa/Apple_logo_black.svg | `9d00ea77a3240f291356c36261c5f45d7fa456f29c97a2f060caf0ed4b9c3231` | Commons: public domain (original design Rob Janoff); trademark of Apple Inc. | painted white (Apple's logo is monochrome, black or white) |
| Windows 11 logo (2021) | tile Windows 11 and the Microsoft default | https://upload.wikimedia.org/wikipedia/commons/8/87/Windows_logo_-_2021.svg | `0f6b74d8d8ede1ec0e454b3c6fcdfba65fbe989af9a49ad4ff0c498f4dc3afbb` | Commons: public domain (file by OAOV); trademark of Microsoft Corporation | none (#0078d4) |
| Microsoft logo | provider Microsoft (`SB7_P_MICROSOFT`) | https://upload.wikimedia.org/wikipedia/commons/4/44/Microsoft_logo.svg | `3d41251f93127b4b42c2f69fa423d204946cf9c307d786ea36b8d9bef4179282` | Commons: public domain; trademark of Microsoft Corporation | the light grey backdrop square removed (the logo is the four squares) |
| Google "G" | provider Google (`SB7_P_GOOGLE`) | https://upload.wikimedia.org/wikipedia/commons/c/c1/Google_%22G%22_logo.svg | `ed9087d76cdc6d1c53698f6068f79872e77e87c8d012c0cfdad13b05b6ccb37c` | Commons: public domain (Google LLC); trademark of Google LLC | none |
| Android robot head | tile Android 17 and the Android default | https://upload.wikimedia.org/wikipedia/commons/3/31/Android_robot_head.svg | `0d3d8e90f448472d65eafefc7aaf4b6836517d840b96e0613ca7078879c279f2` | Commons: public domain; Google's brand guidelines let the robot be used and modified with attribution (CC BY 3.0 terms); "Android" is a trademark of Google LLC | none (#3DDC84) |
| Ubuntu Circle of Friends (2022) | tiles Ubuntu, Ubuntu Desktop, Ubuntu Lomiri (and `linux`) | https://upload.wikimedia.org/wikipedia/commons/0/00/Ubuntu-logo-no-wordmark-2022.svg | `808267226514c1ad3a75269de5b6fd2ecf1bc362fb1a244c57d85c798a823ce9` | Commons: GPLv3 (Canonical Ltd.); Ubuntu and the Circle of Friends are trademarks of Canonical Ltd. (Canonical's trademark policy allows use to refer to Ubuntu) | the orange tag rectangle removed: the white Circle of Friends on the Ubuntu-orange tile |
| Arch Linux logo | tile Arch Linux ARM | https://archlinux.org/static/logos/archlinux-logo-dark-scalable.svg | `a7f37c9c87f0e0e37454b47ec1221b61416aa9e319cc46a1f0b5b70c4a1dcb6e` | The project's own file (archlinux.org/art); the Arch Linux name and logo are trademarks of the Arch Linux project, used under its trademark policy to refer to Arch Linux | the "archlinux" wordmark and (TM) removed: the icon only (#1793d1) |
| Kali Linux dragon | tile Kali Linux | https://www.kali.org/images/kali-dragon-icon.svg | `9a3ed29a936b48cffe54795f9cabfd351a79dfcf861024db200fb569e6fcc2a6` | Kali's own file (kali.org, the site's dragon icon); Kali Linux and its dragon are trademarks of OffSec Services Ltd., used to refer to Kali Linux | only the crisp white dragon kept (the blue sphere, the shadows, the blurred copy and their filters removed), on the Kali-blue tile |
| postmarketOS logo | tile postmarketOS | https://gitlab.com/postmarketOS/artwork/-/raw/ede3978936131a44e121db1d4e642af405a2a8e9/logo/logo.svg | `a4162f754b853293fbe2be804466c16406765f2cf6959a0447b5c90ce588b4b5` | The project's own artwork repository (gitlab.com/postmarketOS/artwork, at the commit that added the logo, 2017-08-26; the repository's LICENCE: CC BY-SA 3.0), postmarketOS developers; same bytes as the Commons copy | none (#009900) |
| Tux | provider Linux (`SB7_P_LINUX`), the Linux default tile | https://upload.wikimedia.org/wikipedia/commons/3/35/Tux.svg | `cd503ad510e16ff2869f959cf57b892bb2175a6874ff696b495bd94fd7db9743` | Commons: attribution: Tux by Larry Ewing (lewing@isc.tamu.edu), Simon Budig, Garrett LeSage, made with The GIMP; use permitted with this credit (same file as `tools/brand/icons/tux.svg`) | none |
| Debian swirl | tile Debian | https://upload.wikimedia.org/wikipedia/commons/4/4a/Debian-OpenLogo.svg | `a759c381560e82e8d719e0038d0d95209b85c00b0d0f6b92c5fc510b959226b7` | Commons: CC BY-SA 3.0 (Debian); the Debian Open Use Logo, (c) Software in the Public Interest, Inc. | the "debian" wordmark cut away (render cropped above it): the swirl (#D70751) |
| Fedora logo (2021) | tile Fedora | https://upload.wikimedia.org/wikipedia/commons/4/41/Fedora_icon_%282021%29.svg | `e0cad9aa4736a492cbeed7f875cb6a7ddfeab54879fc9e218fc00933ef18b6b6` | Commons: public domain; Fedora and its logo are trademarks of Red Hat, Inc. (used under the Fedora trademark guidelines to refer to Fedora) | none |
| Chromium logo (Material) | tile ChromiumOS (`chromeos`) | https://upload.wikimedia.org/wikipedia/commons/f/fe/Chromium_Material_Icon.svg | `72a4e02e7032106bc04e21c4133b42dde6903d6b8b7c3ef8164967a945883c54` | Commons: CC BY 2.5; Chromium is a trademark of Google LLC, used to refer to ChromiumOS. Added 2026-11-05 (download approved by the user; checked: no scripts or external references, one local `url(#clipPath5127)`) | none |

Not real logos (kept from the concept, they are Starbite's own): the Starbite star, the Lab flask, the
`>_` tile of the Starbite shell, the Display Test bars, and the "other" ring.

## Release icons (`src/release/<catalog key>/`)

A tile shows the icon of its release instead of the vendor logo when the file is in
`src/release/<key>/` and pinned in `menu7_logos.RELEASE[<key>]` (file, URL, sha256, credit line for
INFO, mode). An unpinned file stops the build and prints its sha256; without a file the vendor-logo
tile above stays. Mode `bleed`: an app icon with its own background, cropped to its bounds and
filling the tile, cut to the squircle; a number: the icon at that fraction of the tile, centred on
the vendor tile's colours. Downloaded 2026-11-05 (approved by the user) and placed by the
coordinating session; hashes checked here, the SVGs checked for scripts and external references
(none). Every icon is Apple's / Canonical's artwork and trademark, used only to label the release
the entry starts (nominative use, no endorsement).

| Key | File | Source URL | sha256 | Licence / trademark note | Mode |
|---|---|---|---|---|---|
| ios16 | `release/ios16/ios-16-num-256x256_2x.png` (512 x 512 PNG) | https://developer.apple.com/assets/elements/icons/ios-16-num/ios-16-num-256x256_2x.png | `8bb8a4512731a939c86eef1b20bd68ec7fea930e3c9497569b2ed4320e8c3616` | Apple's iOS 16 icon (developer.apple.com); copyright and trademark Apple Inc. | bleed |
| ios26 | `release/ios26/ios-26-256x256_2x.png` (512 x 512 PNG) | https://developer.apple.com/assets/elements/icons/ios-26/ios-26-256x256_2x.png | `b6752a31066de2f524a31302f6954b89d6a29b797499b81c874e4fd4d97a4e28` | Apple's iOS 26 icon (developer.apple.com); copyright and trademark Apple Inc. | bleed |
| mac13 | `release/mac13/InstallAssistant-512x512bb.png` (512 x 512 PNG) | https://is1-ssl.mzstatic.com/image/thumb/Purple211/v4/0d/53/b3/0d53b3bb-70ee-a0f9-6ecd-237260e13725/InstallAssistant.png/512x512bb.png | `3de4f8f255bba5b1ac5675c4d678489a529bae03e044b619e9533d3e2ef44ecf` | the Install macOS Ventura app's App Store artwork (Apple's lookup API, app id 1638787999); copyright and trademark Apple Inc. A round badge with transparent corners | .80 on the Ventura tile (#6a1d45 -> #f7a03a) |
| linux-ubuntu | `release/linux-ubuntu/ubuntu-mascot-light.svg` (12 117 bytes) | https://www.omgubuntu.co.uk/wp-content/uploads/2026/02/ubuntu-mascot-light.svg | `b89ff17308d8a6e8bcaff394e6ad740eae3d7d2fa9be2d751966e64deacbcad9` | Canonical's official Ubuntu 26.04 "Resolute Raccoon" mascot, rehosted by OMG! Ubuntu (Canonical's own copy is in its artwork Google Drive); Ubuntu is a trademark of Canonical Ltd. | .74 on the Ubuntu-orange tile |
| (linux-ubuntu, unused) | `release/linux-ubuntu/ubuntu-mascot-dark.svg` (12 093 bytes) | https://www.omgubuntu.co.uk/wp-content/uploads/2026/02/ubuntu-mascot-dark.svg | `f342664e8506bb55b5c0439925982e0e6fcfe5e9e371bfcc711814e3e9e3d38a` | as above; the dark variant, kept for a light tile | not used |

No release icon: Android 17 (no official badge file on developer.android.com / android.com, only a
wordmark: the robot head stays), Windows 11 (no 23H2-specific icon), ChromiumOS (no separate release
mark: the Chromium logo stays), postmarketOS (no release icons). The `linux` key (the generic
Ubuntu entry) keeps the Circle of Friends.

## Starbite-drawn (`src/starbite/`, 2026-11-05)

Only Starbite's own systems use Starbite-drawn art; every other OS shows its official logo above.
Drawn for this project as plain SVG paths (nothing downloaded or traced), not pinned by sha256
(they are ours to change), rendered by `CUSTOM_G` in `tools/brand/menu7-assets.py`:

| File / art | Used for | What it is |
|---|---|---|
| `starbite/engineer.svg` | tile Engineer OS (`engineer`) | an eight-tooth gear with the concept's Starbite star in its hub |
| the charging bolt (no file) | `SB7_I_BOLT_30`, the status bar battery | a filled bolt path in `menu7-assets.py` |

## Colours and sizes

- Provider glyphs (96 px box, drawn 1:1 on the provider's glass tile, drop shadow baked): Apple white
  (.90 of the box), Microsoft four squares (.82), Google "G" (.86), Tux (.98).
- OS tiles (92, 124, 220, 380): the concept's squircle, gradient, sheen and rim, with the logo in its own
  colours at about .5 to .64 of the tile. Tile colours: iOS 26 the concept's blue, iOS 16 a lighter era blue (#0a58d0 -> #5ac8fa),
  macOS Ventura its wallpaper's magenta -> orange (#6a1d45 -> #f7a03a); ChromiumOS a light tile; Windows 11, postmarketOS
  and Debian a light tile (their logos are blue, green and red); Android the brand's navy (#073042-ish);
  Ubuntu the concept's orange / aubergine gradients; Arch a dark tile; Kali the concept's Kali blue;
  Fedora Fedora blue (#294172). The table is `LOGO_G` and `tile_table()` in `tools/brand/menu7-assets.py`.

## Distributing the built files

`menu7.sb7` and `menu-v7.sbm` contain rasterised derivatives of these files, some under CC BY-SA
(postmarketOS, Debian), CC BY (the Android robot, the Chromium logo) and GPLv3 (the Ubuntu Circle of Friends). The
container's INFO section (`SB7_T_INFO`) carries one credit line per logo, so the notice travels with
the binary. Whenever the ramdisk, a boot image holding it, or the UP_PARAM / assets bundle is shared
outside this repository, ship this file next to it (or paste its table into the release notes).
