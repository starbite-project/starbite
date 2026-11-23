# Starbite (Galaxy S9): world-first check, 2026-11-23

Method: inventory from the repo and notes, then a web search for earlier work in each area (trying to disprove each claim), then a strict second pass on the 12 strongest claims. Our dates come from internal commits, and the full git history is published in this repo. Verdicts: likely_world_first / first_on_this_class_of_device / not_first / unclear.

## mac-1 [first_on_this_class_of_device] significance 8
**Claim:** Narrowest honest wording: first bare-metal (native, not virtualised, not emulated) macOS desktop on a smartphone, and the first macOS desktop on an Exynos/Mali device (Ventura 13.5 on the Galaxy S9, 2026-11-05).

**Earlier work found:** This is a significant downgrade. macOS-DroidVM (Droid-VM org) ran macOS 27 as a crosvm/Gunyah VM on a real Snapdragon phone running HyperOS (Xiaomi). Its TODO records the login screen, WindowServer on screen, and the Dock and wallpaper on 2026-10-26, with the Dock fixed 10-28. It was GPU-accelerated through a Metal plugin -> metal2vulkan -> Turnip (Adreno), with about 112-118 fps drag measurements on 10-27/28. So the first macOS desktop on a phone came about 10 days before ours, in a VM. Separately, Snapintosh got a native macOS 27 desktop on a Snapdragon X2 laptop by about 10-22. The Pixel crosvm macOS (DroidVM issue #82, 10-11) was 'unusable'. A Galaxy A phone ran High Sierra through x86 emulation earlier. The onejailbreak post about 'macOS Ventura on a Galaxy S9' (r/hackios, about 11-06) matches our dates and is probably this project. [U]

**Skeptic note:** Drop 'first macOS desktop on a phone'. A VM on a Xiaomi Snapdragon phone did it on 10-26. 'First native/bare-metal macOS desktop on a phone' still holds as far as found: Snapintosh is a laptop, and DroidVM is a hypervisor guest. Snapintosh also uses an EL2 hypervisor under an unmodified kernel, so the word 'native' needs care. Ours runs XNU directly. The DroidVM TODO is in Chinese and names HyperOS and 'phone' (手机), not the model. [L] that it is a real phone.

**Sources:** https://github.com/Droid-VM/macOS-DroidVM/blob/main/TODO.md, https://github.com/Droid-VM/macOS-DroidVM, https://github.com/Snapintosh/Snapintosh, https://github.com/Droid-VM/DroidVM/issues/82, https://wccftech.com/a-galaxy-a-smartphone-ran-macos-high-sierra-using-emulation/, https://onejailbreak.com/blog/old-galaxy-phones-are-booting-macos-and-ios/

## mac-2 [first_on_this_class_of_device] significance 7
**Claim:** Narrowest honest wording: first Apple Metal, and the first macOS WindowServer/CoreAnimation compositing, on an ARM Mali (Bifrost) GPU, via StarbiteMetal -> PanVK.

**Earlier work found:** Downgraded further than before. macOS GPU compositing on a non-Apple GPU: Snapintosh (Adreno X2-90, native, about 10-22) and macOS-DroidVM (Adreno, phone VM, 10-26/27). Metal-on-Vulkan used for the macOS compositor is also NOT ours first: macOS-DroidVM's Metal plugin runs through 'metal2vulkan' patches onto Turnip (Vulkan) and was rendering WindowServer by 10-27. The same metal2vulkan name appears in UniXNU and in the Starbite pipeline, and the S9+ fork credits 'UniXNU/AdrenoMetal and metal2vulkan'. On Mali: QiuChenly RK3588 (Mali-G610) reached only Setup Assistant plus a cube test, and its Metal claims are unverified. another S9 project got Metal via PanVK on the S9 later (11-09+).

**Skeptic note:** Remove 'likely first Metal-on-Vulkan translation used to run macOS's compositor'. macOS-DroidVM did that on Adreno/Turnip first. What is left is Mali-specific: the first Metal on Mali under macOS, and the first macOS compositing on Mali. The team should check whether its metal2vulkan is derived from UniXNU's/DroidVM's. If it is, credit them.

**Sources:** https://github.com/Droid-VM/macOS-DroidVM/blob/main/TODO.md, https://github.com/Snapintosh/Snapintosh, https://github.com/UniXNU/UniXNU, https://github.com/0xsharkboy/ios-s9plus-exynos, (another S9 project)

## sb-2 [likely_world_first] significance 9
**Claim:** Narrowest honest wording: first open, independent (non-Samsung-RIL) stack to get LTE mobile data (registration, PDP context, HTTPS) through a Samsung Shannon/Exynos modem, here inside a from-scratch hobby OS on the Galaxy S9. Calls and SMS not included.

**Earlier work found:** Not first as a hobby OS with LTE data: Genode Sculpt has had it on the PinePhone since 2022-23, but through an easy Quectel AT-command USB modem. For Shannon itself, public work is analysis tooling (shannonRE, FirmWire, the Hernandez/Muench emulation, an IDA loader) and libsamsung-ipc/Replicant for old modems. postmarketOS lists Shannon on mainline as not working (Exynos 7870: the driver 'does not work, needs rewrite'). The closest recent example is the postmarketOS Galaxy S24 port. It gets LTE data, but by running Samsung's own modem drivers in a sealed Android container with ModemManager on top, so it is not an independent stack. another S9 project (iOS on the S9) has a modem component that, as of 17 Nov, had not yet run under iOS. I found no open, non-Samsung Shannon data stack before this one.

**Skeptic note:** This survived skeptical searching better than any other claim and is the strongest. Shannon support may exist in a private or closed project, so 'as far as public sources show' should stay in the wording. Also say clearly that this is data only, with no voice or SMS.

**Sources:** https://wiki.postmarketos.org/wiki/Samsung_Exynos_7870_Octa, https://wiki.postmarketos.org/wiki/Samsung_Modem, https://github.com/dotnetdreamer/postmarketos-galaxy-s24, https://genodians.org/ssumpf/2022-05-09-telephony, (another S9 project), https://i.blackhat.com/USA-20/Wednesday/us-20-Hernandez-Emulating-Samsungs-Baseband-For-Security-Testing.pdf

## bt-1 [likely_world_first] significance 9
**Claim:** Narrowest honest wording: first phone, and as far as found the first device of any kind, to natively multi-boot (bare metal, no VMs) Apple OSes (iOS/iPadOS and macOS) next to Windows 11, Linux, ChromiumOS and Android from one boot loader.

**Earlier work found:** Native multi-boot without Apple OSes already exists: Renegade Project EDK2 (Android/Linux/Windows on Snapdragon phones), NexPhone (Jan 2026), DuoWOA on the Surface Duo, and Pocophone F1 guides. Every iOS-on-Android project from October/November 2026 (HackiOS, ios-sweet, UniXNU, SmuOS on Surface Duo 2, another S9 project, Exynos S7/S20/A50 ports) boots one Apple OS in place of Android, or from RAM. A new caveat: DroidVM on Snapdragon 8 Gen 3/8 Elite phones can run Windows and Linux VMs, the macOS 27 guest (macOS-DroidVM, desktop on a Xiaomi phone 10-26) and an iPadOS 17 guest (UniXNU-droidvm). So one phone running Android + Windows + Linux + macOS + iPadOS together under a hypervisor was possible around the same time. That is virtualised, not native boot.

**Skeptic note:** Say 'natively boots' or 'bare-metal multi-boot'. Do not just say 'runs all these OSes on one phone', because DroidVM VMs on a Snapdragon phone make that weaker claim arguable. The size of the OS lineup (iOS + macOS + Windows 11 + ChromiumOS + Android + own OS) is the real distinction.

**Sources:** https://renegade-project.tech/en/faq/, https://m.gsmarena.com/newscomm-71205p2.php, https://github.com/Droid-VM/macOS-DroidVM, https://github.com/Yosaki-sonako/UniXNU-droidvm, https://blog.lyc8503.net/en/post/android-virtualization-on-snapdragon/, https://www.windowslatest.com/2026/10/27/ios-26-can-run-on-an-android-phone-and-a-developer-proved-it-on-microsofts-abandoned-surface-duo-by-turning-it-into-an-iphone-duo/

## ios-4 [first_on_this_class_of_device] significance 7
**Claim:** Narrowest honest wording: likely the first iOS/iPadOS CoreAnimation UI composited on an ARM Mali GPU, via Mesa Panfrost OpenGL ES cross-built for iOS ('sbca' in backboardd), 2026-10-29.

**Earlier work found:** GPU-composited iOS on non-Apple hardware came first on Adreno: UniXNU on the Xiaomi Pad 6S Pro by about 10-20, and another project's EAGL/GL bridge on Snapdragon. On Mali, every dated example I found is later: another S9 project S9 Panfrost GLES (seen 11-01), Galaxy A50 (repo 11-06), the S9+ fork (built on another S9 project/another project, 11-06), Galaxy S7 Mali-T880 forks via the another project EAGL bridge (status 'late Oct to mid Nov', but the only exact date is 11-15), S20 Mali-G77 ports, and hakios-a22 (CoreAnimation still in software). The S7 forks' first GPU date could not be pinned down. Gap: an exact earliest S7/T880 date.

**Skeptic note:** Holds, but only by days, and it rests on internal dates. Wording: 'first known'. The S7 Mali-T880 ports are the one risk I could not date precisely.

**Sources:** https://github.com/UniXNU/UniXNU, https://pbxscience.com/?p=24278, (another S9 project), https://github.com/wardevsa/ios-a5xelte/tree/herolte, https://github.com/nespaces/ios-a50-exynos9610, https://github.com/0xsharkboy/ios-s9plus-exynos, https://github.com/david7xw/hakios-a22

## ios-7 [first_on_this_class_of_device] significance 7
**Claim:** Narrowest honest wording: likely the first Apple Metal running on an ARM Mali GPU under iOS (StarbiteMetal -> metal2vulkan -> PanVK on the Mali-G72, iconservicesagent drawing through Metal, 2026-11-01/02).

**Earlier work found:** Metal on a non-Apple GPU under iOS came first on Adreno (UniXNU AdrenoMetal via metal2vulkan -> Turnip, by about 10-20). New close competitor: Nik300/viva-gpu, a Metal driver for iOS 16 on a Mali-G57 (Redmi Note 11 Pro 4G) built on Mesa Panfrost plus AIR->NIR. Its commit history starts 2026-11-03, with 'first light on the phone' in FINDINGS-2026-11-03 and later the full UI on the Mali at 60/120 Hz. That is only 1-2 days after our internal date. Others are later: another S9 project PanVK Metal on the S9 (about 11-09 to 11-14), A50 (11-06 WIP), Jesvi-iOS (11-14+). rawzoul/a32-xnu (MaliMetal = PanVK for iOS on Valhall) is now 404, and per a search summary it was never verified end to end.

**Skeptic note:** It holds only on internal dates, with a 1-2 day lead over viva-gpu (Mali-G57). Do not claim it without dated public evidence (commit hashes or timestamps). The metal2vulkan part is not original (it comes from UniXNU).

**Sources:** https://github.com/Nik300/viva-gpu, https://github.com/UniXNU/UniXNU, https://pbxscience.com/?p=24278, (another S9 project), https://github.com/nespaces/ios-a50-exynos9610, https://github.com/jesvi-eulah/Jesvi-iOS

## win-3 [first_on_this_class_of_device] significance 7
**Claim:** Narrowest honest wording: first Windows 11 GPU-accelerated desktop (DWM) on an ARM Mali in a phone, and on Bifrost Mali-G72; possibly the first DWM on any Mali (11-07 to 11-09 against RK3576's 11-13).

**Earlier work found:** gahingwoo/wddm-mali-g52 (Rockchip boards, same KMD + Mesa Panfrost-in-UMD design): first Mali job under Windows 2026-10-01, D3D11 triangle via Panfrost 10-02, DWM desktop composed on Mali 11-13, zero-copy scan-out 11-14. Its README says it found no other Windows driver for any Mali. isaacjamessss's Pixel 6 fork (Mali-G78) has phone commits from 11-18 onward.

**Skeptic note:** [V] Not first for Mali jobs or triangles under Windows. 'First on a phone' is solid. 'First DWM on any Mali' depends on Starbite's internal dates (a 4-6 day lead).

**Sources:** https://github.com/gahingwoo/wddm-mali-g52, https://github.com/isaacjamessss/wddm-mali-g52, https://github.com/ahsanihlwn/mt6789-mali-winarm64

## sb-1 [first_on_this_class_of_device] significance 7
**Claim:** Narrowest honest wording: first from-scratch, non-Linux OS (own kernel and shell) to reach a usable phone shell on a mainstream Samsung/Exynos flagship (Galaxy S9).

**Earlier work found:** Genode Sculpt has run on the PinePhone since February 2023, with its own UI, calls and data. Windows Phone/WOA, Fuchsia and seL4 research builds also exist. CrimsonOS and other hobby OSes claim their own kernel and GUI on ARM handsets, but name no Samsung device and show no usable shell. Mainline Linux for the 9810 reached basic device-tree support only in Dec 2024.

**Skeptic note:** Not the first non-Linux phone OS. It is first on a stock Samsung/Exynos flagship as far as found. It becomes much stronger combined with sb-2 (Shannon LTE data).

**Sources:** https://genodians.org/nfeske/2023-02-01-mobile-sculpt, https://dev.to/chad_wiley_9633c3427c2b25/crimsonos-building-a-mobile-os-from-the-firmware-up-5082, https://www.osnews.com/story/26950/where-are-the-hobbyist-mobile-operating-systems/, https://patchwork.kernel.org/project/linux-samsung-soc/cover/20241214-exynos9810-v4-0-4e91fbbc2133@gmail.com

## bt-7 [first_on_this_class_of_device] significance 6
**Claim:** Narrowest honest wording: first port of Mesa's Panfrost/PanVK (Mali) driver to non-Linux OSes, and one Mali driver core shared by iOS, macOS, Windows and Linux on a single phone.

**Earlier work found:** Mesa drivers under Apple OSes on non-Apple GPUs came first on Adreno: Turnip/ir3 under iPadOS (UniXNU, about 10-20), macOS (Snapintosh about 10-22, macOS-DroidVM 10-26/27), and Linux. So a Mesa core across several OSes, including Apple ones, is not unique in itself. On Mali: gahingwoo ran Panfrost inside a Windows UMD (10-02), which is earlier than ours. So Panfrost on Windows is not ours first. viva-gpu ran Panfrost under iOS from 11-03. No project shares Panfrost across iOS + macOS + Windows + Linux on one device.

**Skeptic note:** Downgraded. 'Panfrost under Windows' is not first (gahingwoo, 10-02). What is left is the four-OS combination on one phone, plus Panfrost/PanVK under iOS and macOS (iOS only by days). It is an engineering distinction more than a headline.

**Sources:** https://github.com/gahingwoo/wddm-mali-g52, https://github.com/Nik300/viva-gpu, https://github.com/Droid-VM/macOS-DroidVM/blob/main/TODO.md, https://github.com/Snapintosh/Snapintosh, https://docs.mesa3d.org/drivers/panfrost.html

## ios-2 [first_on_this_class_of_device] significance 6
**Claim:** Narrowest honest wording: first ARM Mali GPU jobs/pixels/triangle under XNU (StarbiteMali kext + Mesa Bifrost compiler on the Mali-G72, 2026-10-27).

**Earlier work found:** A non-Apple GPU under XNU came first: UniXNU's native Adreno 740 driver (imported 10-20, user-space GPU test 10-23), and another project's GL bridge on Adreno. Every dated Mali project I found is later: another S9 project S9 'mali=9 THE GPU WROTE IT' on 11-01, viva-gpu Mali-G57 from 11-03, A50 from 11-06, S9+ fork 11-06, S20 Mali-G77 from 11-14, S7 Mali-T880 forks (exact date not found, status 11-15). hakios-a22 Mali-G52 drew with Panfrost but stalled.

**Skeptic note:** A lead of about 5 days over another S9 project on the same phone. This is a strong Mali-specific claim, but it rests on internal dates. The undated S7 T880 forks are a residual risk.

**Sources:** https://github.com/UniXNU/UniXNU, (another S9 project), https://github.com/Nik300/viva-gpu, https://github.com/nespaces/ios-a50-exynos9610, https://github.com/wardevsa/ios-a5xelte/tree/herolte, https://github.com/david7xw/hakios-a22

## ios-5 [first_on_this_class_of_device] significance 5
**Claim:** Narrowest honest wording: first GPU-accelerated, usable iOS 26 on a Mali / Exynos phone (iOS 26.6.2 on the Exynos 9810, 2026-11-01).

**Earlier work found:** arm64e iOS 26 on a CPU without PAC came first: SmuOS ran iOS 26.5.2 on the Surface Duo 2 (Snapdragon, no PAC) on about 10-26/27, with touch at about 37 fps. UniXNU added iPadOS 26.5.2 profiles on 10-26 (SM8550 has PAC). ios-on-pixel-10 (iOS 26, PowerVR, GPU incomplete) from 11-02. The other Mali ports all run iOS 16.x; another S9 project went 16.5 -> 27.

**Skeptic note:** Drop 'first arm64e iOS without PAC'. What is left is narrow (iOS 26 + Mali GPU).

**Sources:** https://www.techadvisor.com/article/3270754/developer-turned-microsoft-surface-duo-into-iphone-duo.html, https://www.gizmochina.com/2026/10/27/microsofts-2021-surface-duo-2-successfully-runs-ios-26/, https://github.com/UniXNU/UniXNU, https://github.com/TheAndersMadsen/ios-on-pixel-10

## mac-4 [first_on_this_class_of_device] significance 4
**Claim:** Narrowest honest wording: macOS 15.6 Sequoia desktop at 2x HiDPI on a phone touchscreen, natively, with an own touch-to-trackpad gesture layer (long press = right click, four-finger tap = keyboard).

**Earlier work found:** The same prior art as mac-1. macOS-DroidVM ran a macOS 27 desktop on a phone (VM, 10-26) with touch through virtio input (UniXNU-droidvm describes virtio touch). So 'macOS usable with touch on a phone' is not ours first. Snapintosh has a laptop with a real trackpad. A native Sequoia desktop on a phone and this particular gesture mapping were not found elsewhere.

**Skeptic note:** An incremental improvement on mac-1, not a separate world first. Present it as part of the native macOS on a phone milestone.

**Sources:** https://github.com/Droid-VM/macOS-DroidVM/blob/main/TODO.md, https://github.com/Yosaki-sonako/UniXNU-droidvm, https://github.com/Snapintosh/Snapintosh

## win-6 [likely_world_first] significance 7
**Claim:** Windows 11 running on Samsung Exynos M3 Mongoose big cores (no LSE/LRCPC) via own ACPI parking tables and a big-core firmware module.

**Earlier work found:** I found no record of Windows running on any Samsung Mongoose (M1 to M5) core. exynos9810-woa does not mention big cores. Searches for Windows on Exynos Mongoose found only Linux device-tree work.

**Sources:** https://github.com/ntdevlabs/exynos9810-woa, https://patchwork.ozlabs.org/project/linux-gpio/patch/20241026-exynos9810-v3-1-b89de9441ea8@gmail.com/

## sb-6 [first_on_this_class_of_device] significance 7
**Claim:** Linux 7.1 on the four Exynos M3 big cores as a guest of Starbite OS on the A55s, by CPU partitioning with no hypervisor, its output composited onto Starbite's screen with input passed through.

**Earlier work found:** Running Linux beside another OS on separate cores with no hypervisor (AMP / CPU partitioning) is a known technique. It is used on embedded SoCs (TI AM57xx, NXP/Toradex HMP with remoteproc/rpmsg), in Jailhouse/Bao partitioning, and in research on Linux+RTOS boards. I found no case of this on a phone's big.LITTLE SoC with the host OS on the little cores and Linux on the big cores, with display and touch handed through.

**Sources:** https://developer.toradex.com/software/hmp/hmp-overview/, https://e2e.ti.com/support/processors-group/processors/f/processors-forum/818150/linux-am5728-multiple-os-without-hypervisor, https://www.variscite.com/asymmetric-multiprocessing-on-heterogeneous-multiprocessor-systems

## sb-8 [first_on_this_class_of_device] significance 7
**Claim:** Own GPU compositor on Mali-G72 drawing an Apple-style Liquid Glass effect (refraction, dispersion, blur) in a from-scratch OS.

**Earlier work found:** Liquid Glass copies already exist as WebGL, AGSL and Compose shaders and in Linux compositors (for example ShojiWM effects), so the effect itself is not new. Open Bifrost GPU drivers (Panfrost/PanVK) exist only for Linux, with a FreeBSD port. An Arm forum thread asks about writing a Mali driver for a non-Linux OS, but shows no result. I found no hobby non-Linux OS that drives a Mali Bifrost GPU with its own driver.

**Sources:** https://bea4dev.github.io/ShojiWM/docs/configuration/effects, https://dev.to/maxgeris/webgl-based-javascript-library-for-pixel-perfect-ios-liquid-glass-effect-with-refractions-and-3p8m, https://community.arm.com/support-forums/f/automotive-forum/57560/mali-g310-driver-development-for-a-non-linux-custom-os, https://www.collabora.com/news-and-blog/news-and-events/panvk-an-open-source-vulkan-driver-for-arm-mali-midgard-and-bifrost-gpus.html, https://freebsdfoundation.org/wp-content/uploads/2021/08/The-Panfrost-Driver.pdf

## bt-4 [likely_world_first] significance 7
**Claim:** Own code booting Samsung's Shannon CP on Exynos 9810 without Android/RIL (RADIO+EFS NV load, handshake, RFS server, LTE attach), staged up to data.

**Earlier work found:** Replicant's libsamsung-ipc is free code that boots older Samsung modems and talks the Samsung IPC protocol, mostly Intel XMM-based Exynos 4 era devices (ipc-test boots the modem and receives messages). Replicant's tracker for LTE modems on mainline kernels sits at 0%. Shannon work is security research: the SSTIC 2020 baseband debugger on the Galaxy S7 (it reused Samsung's cpboot-daemon), ShannonRE, the Shannon IDA loader, and Hernandez's emulation (FirmWire). BaseMirror reverse-engineers RIL commands. postmarketOS / mainline Exynos ports (7870, 9810) have no working Shannon modem. I found no independent, non-Android boot of an integrated Shannon CP that goes as far as LTE attach and data.

**Sources:** https://redmine.replicant.us/projects/replicant/wiki/Libsamsung-ipc, https://redmine.replicant.us/issues/1954, https://www.sstic.org/media/SSTIC2020/SSTIC-actes/how_to_design_a_baseband_debugger/SSTIC2020-Article-how_to_design_a_baseband_debugger-berard_fargues.pdf, https://hernan.de/research/papers/us-20-Hernandez-Emulating-Samsungs-Baseband-For-Security-Testing.pdf, https://arxiv.org/html/2409.00475v1, https://uithub.com/alexander-pick/shannon_modem_loader

## ios-1 [first_on_this_class_of_device] significance 6
**Claim:** iOS 16 booted to the lock screen and home screen with working touch on a Galaxy S9 (Exynos 9810), using an own XNU build, ADT and IOKit drivers, from its own BOOT-partition loader (2026-10-25).

**Earlier work found:** Booting iOS on an Android phone had already been done a few days earlier. ios-sweet (dipa-studio) ran iOS 16.7.16 to the home screen with touch on a Redmi Note 10 Pro (Snapdragon 732G); Decrypt covered it on 2026-10-19. HackiOS (OOSexGang) claimed iOS 16.5 on a Poco X5 Pro around 10-18..10-23. UniXNU's repo was created 2026-10-19 and marked iOS 'verified' that day, running an iPadOS 17.7.11 image on a Xiaomi Pad 6S Pro. Earlier, partial work: the iOS/Darwin core on a Nokia N900 (2013), and QEMU emulation (alephsecurity 2019, eShard iOS 14 home screen). Everything on Exynos/Samsung that we found is later: another S9 project's iOS 16.5 home screen on the same S9 is dated 1-3 Nov, the Galaxy A50 port started in Nov, and Jesvi-iOS (S20) in mid-Nov. Verdict: not the first iOS on an Android phone, but likely the first on an Exynos/Samsung phone and the first on the Galaxy S9.

**Sources:** https://decrypt.co/381351/hackers-running-apple-ios-android-phones, https://www.gizchina.com/redmi-phones/real-ios-16-now-boots-on-a-redmi-note-12-4g-and-tinkerers-can-try-it, https://smhn.info/202610-hackios-runs-ios-on-android-poco-x5-pro, https://github.com/UniXNU/UniXNU, (link withheld), https://www.phonearena.com/news/iOS-core-successfully-ported-to-a-Nokia-N900-might-we-see-iOS-running-on-more-non-Apple-gear_id49660, https://eshard.com/blog/emulating-ios-14-with-qemu

## ios-6 [first_on_this_class_of_device] significance 6
**Claim:** Vulkan (Mesa PanVK) running on an ARM Mali GPU under iOS 26 on a non-Apple phone (2026-11-01).

**Earlier work found:** UniXNU ran Mesa Turnip (Vulkan for Adreno) under XNU from 2026-10-20 to 10-22 ('let Turnip bind host-import images'), so Mesa Vulkan under iOS came first on Adreno. For PanVK/Mali under iOS, everything we found is later: another S9 project's iOS 27 'Metal renderer through Mesa's PanVK' was first seen 11-09. Likely the first PanVK under XNU.

**Sources:** https://github.com/UniXNU/UniXNU, (link withheld)

## mac-3 [first_on_this_class_of_device] significance 6
**Claim:** macOS 26 reached its first login screen on the Galaxy S9 (captured by frame dump).

**Earlier work found:** macOS 26 Tahoe reached login screens and desktops on Intel hackintoshes (OpenCore/OCLP) in 2025, but that is x86. On non-Apple ARM, Snapintosh reached a macOS 27 desktop on a Snapdragon laptop before 2026-10-25. RK3588 reached macOS 27.2 Setup Assistant on 2026-10-27. I found no report of arm64 macOS 26 reaching login on non-Apple ARM, and none of any macOS login screen on a phone, before 2026-11-04.

**Sources:** https://forums.macrumors.com/threads/macos-tahoe-26-on-unsupported-macs-discussion.2458481/page-19, https://github.com/Snapintosh/Snapintosh, https://en.kocpc.com.tw/archives/26677

## win-5 [likely_world_first] significance 6
**Claim:** DXGI flip-model and DirectComposition swap chains composed by DWM on a Mali with an own driver stack; Geometry Dash at its 60 fps cap on a zero-copy flip swap chain.

**Earlier work found:** I found no other Windows Mali stack with flip-model or DirectComposition presentation. As of 2026-11-23, gahingwoo/wddm-mali-g52 says Edge's GPU process gets feature level 10_1, but presenting to a composition swap chain is still blocked. The Pixel 6 fork's OpenGL path uses a GPU blit plus a CPU copy.

**Sources:** https://github.com/gahingwoo/wddm-mali-g52, https://github.com/gahingwoo/wddm-mali-g52/commits/main, https://github.com/isaacjamessss/wddm-mali-g52

## lx-3 [likely_world_first] significance 6
**Claim:** ChromiumOS on the Galaxy S9 reaching its welcome screen, with Panfrost GPU and a working guest session.

**Earlier work found:** I found no native ChromiumOS, openFyde or FydeOS boot on any smartphone: not on the S9, the OnePlus 6, the Poco F1, the PinePhone or the Librem 5. Known ChromiumOS-on-ARM ports target SBCs and laptops: Raspberry Pi (FydeOS), Pinebook Pro (FydeOS 2020), Rock 5B (openFyde) and ODROID-N2 with Panfrost (2021, Chromium on Debian). Google showed ChromeOS running inside a VM on a Pixel, which is virtualization, not a native boot. A 2016 chromium-os-dev thread treated phone bootloaders as a blocker. Official ChromeOS tablets (Lenovo Duet) are tablets made for ChromeOS, not phones. Hedge: this means 'the first native ChromiumOS on a phone with GPU acceleration that I could find'. Hobbyists may have done it without publishing it.

**Sources:** https://www.androidauthority.com/chrome-os-on-android-hands-on-2-3442510/, https://groups.google.com/a/chromium.org/g/chromium-os-dev/c/hLM4OmA_7cc, https://www.cnx-software.com/2021/09/30/odroid-n2-sbc-gets-ubuntu-21-10-chromium-os-with-panfrost-open-source-gpu-driver/, https://liliputing.com/2020/11/fydeos-beta-brings-chromium-os-to-the-pinebook-pro-android-app-support-too.html?amp, https://github.com/FydeOS/chromium_os-raspberry_pi, https://github.com/openFyde

## sb-7 [first_on_this_class_of_device] significance 6
**Claim:** Debian arm64 Linux desktop apps running as native apps inside Starbite OS Next, with an App Store, NAT internet over Wi-Fi/LTE and zero-copy surfaces, used on the phone.

**Earlier work found:** Running Linux apps inside another OS is well known: Chrome OS Crostini, WSL/WSLg, Genode's Linux VMs, and on phones Samsung DeX for Linux, Termux/proot-distro and Andronix. All of these sit on a Linux or vendor kernel, or on a hypervisor. I found nothing that brings Linux apps into a from-scratch hobby phone OS.

**Sources:** https://genode.org/documentation/release-notes/22.08

## sb-9 [first_on_this_class_of_device] significance 6
**Claim:** From-scratch drivers verified on the phone: BCM4361 Wi-Fi with own WPA2/WPA3-SAE on both bands, Bluetooth HID keyboard pairing, front camera with software ISP, speaker/earpiece sound, sensors.

**Earlier work found:** These drivers exist on Linux: brcmfmac, where WPA3 support was only fixed in 2023-2024 and the firmware does SAE through its own SME. A FreeBSD BCM43xx port is in progress. Genode reuses ported Linux drivers. I found no hobby OS with its own Broadcom FullMAC driver plus WPA3, or its own Samsung camera/CSIS driver.

**Sources:** https://lore.kernel.org/lkml/01bd8c68-1b9c-49b2-8ace-1c7d1b5192ad@marcan.st/T/, https://lists.infradead.org/pipermail/hostap/2024-July/042782.html, https://github.com/narqo/freebsd-brcmfmac

## bt-2 [first_on_this_class_of_device] significance 6
**Claim:** arm64e (PAC) Apple userland running on an ARMv8.0 CPU without PAC via a uniform shared-cache rewrite plus kernel emulation fallback.

**Earlier work found:** SmuOS's iOS 26 port to the Surface Duo 2 (Snapdragon 888, no PAC) was reported Oct 26-27, 2026, a few days before Starbite's date of 2026-11-01. Its repository reportedly strips 29.6 million pointer signatures from the iOS 26 shared cache, and some arm64e parts were rewritten to arm64. So the idea of running arm64e userland on a non-PAC CPU by rewriting the shared cache was published first by someone else. A Snapdragon X2 macOS developer also called PAC a known blocker. That port targets ARMv8.2-class cores; I found no arm64e userland on an ARMv8.0 core (no LSE, no LRCPC) before Starbite.

**Sources:** https://www.windowslatest.com/2026/10/27/ios-26-can-run-on-an-android-phone-and-a-developer-proved-it-on-microsofts-abandoned-surface-duo-by-turning-it-into-an-iphone-duo/, https://notebookcheck.net/The-original-Duo-now-runs-iOS-and-it-s-not-from-Apple.1435366.0.html, https://www.windowscentral.com/hardware/qualcomm/the-man-who-put-macos-on-a-snapdragon-x2-elite-says-hackintosh-on-arm-is-absolutely-possible, https://clang.llvm.org/docs/PointerAuthentication.html

## bt-3 [likely_world_first] significance 6
**Claim:** Binary rewrite of 133,388 LDAPR->LDAR sites in Apple's iOS 26 shared cache (plus LSE hotpatch) to run on cores without LRCPC/LSE.

**Earlier work found:** I found no public rewrite of LDAPR (or LSE atomics) in a dyld shared cache. The other non-Apple iOS 26 port (Surface Duo 2, Snapdragon 888 = Cortex-X1/A78/A55, ARMv8.2 with LSE and LRCPC) did not need it. The iOS 16 ports (ios-sweet, HackiOS) use the arm64 slices for A11/iPhone X, which contain no LDAPR. The usual toolchain approach for older CPUs is runtime dispatch (outline atomics), not patching finished binaries. Windows 11 24H2 requires ARMv8.1 for the same reason (LSE).

**Sources:** https://developer.arm.com/community/arm-community-blogs/b/tools-software-ides-blog/posts/enabling-rcpc-in-gcc-and-llvm, https://src.rcs.uwaterloo.ca/xref/llvm-project/compiler-rt/lib/builtins/aarch64/lse.S, https://www.windowslatest.com/2026/10/27/ios-26-can-run-on-an-android-phone-and-a-developer-proved-it-on-microsofts-abandoned-surface-duo-by-turning-it-into-an-iphone-duo/, https://www.tomshardware.com/software/windows/windows-11-for-arm-can-run-natively-on-specific-android-smartphones-the-test-device-heats-up-very-fast-and-battery-life-substantially-decreases

## ios-8 [likely_world_first] significance 5
**Claim:** iOS 26 running on Samsung's Exynos M3 'Mongoose' big cores, which lack LSE atomics and LRCPC, via kernel emulation of LSE/LDAPR from EL0 plus pinning to an A55 for other missing instructions; iOS 16 also ran on all 8 cores (2026-10-27 / 11-02).

**Earlier work found:** another S9 project ran iOS 16.5 on all eight S9 cores on 2026-11-04 (it has an exynos_m3 CPU driver), and its iOS 27 build 'rewrites some instructions ahead of time'. Both came after our dates of 10-27 (iOS 16 on 8 cores) and 11-02 (iOS 26 on the M3 cores). Other ports target cores with ARMv8.2 or later (LSE present): Snapdragon A76/Kryo, Exynos 990's M5, Dimensity and Tensor. We found no earlier XNU/iOS running on ARMv8.0 cores without LSE, and no earlier EL0 LSE/LDAPR trap emulation for iOS.

**Sources:** (link withheld), https://github.com/jesvi-eulah/Jesvi-iOS, https://github.com/UniXNU/UniXNU

## win-2 [likely_world_first] significance 5
**Claim:** Windows 11 Wi-Fi on the S9's Broadcom BCM4361 via an own WiFiCx driver (SbWifiCx), open networks.

**Earlier work found:** I found no earlier Windows Wi-Fi driver for the BCM4361 or for any Exynos phone. exynos9810-woa does not mention Wi-Fi. The pixel6-woa-drivers set (Pixel 6 Mali port, a different Broadcom chip) lists a Wi-Fi driver, but its gs101 commits are dated 2026-11-18 or later. Earlier Windows-on-phone ports (Lumia, Surface Duo, Renegade) used Qualcomm's own Wi-Fi drivers. A Linux port of the S9+ runs the BCM4361 with brcmfmac, but that is Linux, not Windows.

**Sources:** https://github.com/ntdevlabs/exynos9810-woa, https://github.com/isaacjamessss/wddm-mali-g52, https://github.com/SheroAbi/galaxy-s9plus-mainline-linux

## win-4 [likely_world_first] significance 5
**Claim:** Direct3D 11 from an x64 (emulated) app on the Mali under Windows on ARM.

**Earlier work found:** The closest is gahingwoo/wddm-mali-g52, which added x64 and x86 programs on the Mali (FurMark x64 through OpenGL) on 2026-11-14. Starbite's x64 D3D11 run (T45) is dated 2026-11-12. I found nothing earlier.

**Sources:** https://github.com/gahingwoo/wddm-mali-g52

## win-1 [not_first] significance 4
**Claim:** Windows 11 ARM64 desktop with working touch on the Exynos 9810 Galaxy S9 (ported woa UEFI, own UFS driver, RAM maps, PMIC RTC).

**Earlier work found:** ntdevlabs/exynos9810-woa (by NTDEV): its first public release commit, 'Initial public release: firmware, drivers and installer', is dated 2026-09-27. It installs Windows 11 22H2/23H2 ARM64 from stock Android to the desktop on the Galaxy S9+ (SM-G965F, Exynos 9810). It ships its own Exynos9810Ufs storage driver, an HSI2C driver and an S6SY761 touch driver, and NTDEV announced the installer publicly on X. That is about a month before Starbite's 2026-10-26 desktop. Forks followed: JoshAtticus, ilyatbn, S-wave (v0.2.2 on 28 Oct), and an Exynos 2200 port.

**Sources:** https://github.com/ntdevlabs/exynos9810-woa/commits/main, https://github.com/ntdevlabs/exynos9810-woa/releases, https://github.com/ntdevlabs/exynos9810-woa, https://github.com/JoshAtticus/exynos9810-woa, https://github.com/S-wave/exynos9810-woa/releases/tag/v0.2.2, https://x.com/NTDEV_/status/2105413206965629030, https://github.com/jhinzuo2/exynos2200-woa

## win-7 [likely_world_first] significance 4
**Claim:** x86/x64 apps (Steam) under Windows emulation on Exynos M3 big cores, fixed by an own sbxta.sys driver that toggles XtaCache per core type.

**Earlier work found:** Nothing found. It depends on win-6, which has no known prior art either. x86 emulation on ordinary Windows-on-ARM phones (Snapdragon) is long established, so the new part is only the Mongoose cores and the per-core XtaCache fix.

**Sources:** https://github.com/ntdevlabs/exynos9810-woa

## sb-11 [first_on_this_class_of_device] significance 4
**Claim:** Engineer OS: bare-metal tool that reads every ACPM DVFS domain from S-Boot's ECT and sets or overclocks clocks via ACPM, starting the M3 cores through FlexPMU.

**Earlier work found:** Overclocking the Exynos 9810 is long done in custom Android kernels: DS-ACK, xxmustafacooTR, and AnandTech's 2018 custom-kernel tuning of the S9. Those use Samsung's own kernel DVFS/ECT code. I found nothing public that does it bare-metal, outside Linux, by reading the ECT and talking to ACPM directly.

**Sources:** https://newreleases.io/project/github/duhansysl/exynos9810-kernel/release/V1.9, https://www.anandtech.com/show/12620, https://newreleases.io/project/github/xxmustafacooTR/android_kernel_samsung_universal9810/release/v36

## ios-3 [first_on_this_class_of_device] significance 3
**Claim:** iPadOS 18.7 (iPad 7 build) booted to the lock screen on a non-Apple phone on a port of xnu-11417 (2026-10-28).

**Earlier work found:** UniXNU booted iPadOS 17.7.11 (iPad7,1 image) on a non-Apple Android tablet (Xiaomi Pad 6S Pro) from 2026-10-19/20. It added iPadOS 26.5.2 profiles on 10-26 and iPadOS 27 on xnu-12377 on 10-28. iPadOS on non-Apple hardware is therefore not new. iPadOS 18 / xnu-11417 specifically, on a phone, and on Exynos: we found no earlier example.

**Sources:** https://github.com/UniXNU/UniXNU, https://pbxscience.com/?p=24278

## ios-9 [not_first] significance 3
**Claim:** iOS 27.0.1 booted to the home screen on a Galaxy S9 (full root, own decmpfs LZVN/LZFSE driver), later with 8 cores and a governor (2026-11-11 / 11-14).

**Earlier work found:** another S9 project reports iOS 27 reaching the home screen on the same Galaxy S9 on 2026-11-08 and CoreAnimation's Metal renderer with Liquid Glass on 11-09, both before 11-11, and our build partly uses another S9 project's pieces. UniXNU added iPadOS 27 on xnu-12377 for the Xiaomi Pad 6S Pro on 2026-10-28.

**Sources:** (link withheld), https://github.com/UniXNU/UniXNU

## win-8 [likely_world_first] significance 3
**Claim:** Starbite Link: the phone's Windows 11 desktop shown on a Mac over USB (DeX-like / second monitor), with rotation and mid-session resize.

**Earlier work found:** Windows-on-ARM phones have driven external displays before: Lumia 950 XL dual display and Continuum dock (2020) and dual-screen Surface Duo. I found no Windows-on-phone port that streams its desktop to a Mac or PC over USB as a display. Streaming a desktop as a remote or second display (Spacedesk, RDP and similar) is a well-known general idea.

**Sources:** https://liliputing.com/hacker-brings-dual-display-support-to-a-lumia-950-xl-running-windows-10/, https://liliputing.com/2022/07/now-you-can-install-windows-on-the-microsoft-surface-duo-dual-screen-smartphone.html

## win-9 [first_on_this_class_of_device] significance 3
**Claim:** Windows 11 on the S9 with USB host and device mode, CPU at 1.95 GHz, and a hardware dead-man watchdog driver (sbwdog).

**Earlier work found:** USB host and device, CPU clocking and watchdogs are standard in Windows-on-ARM phone ports (Lumia, Renegade, Surface Duo). On Exynos 9810, the earlier exynos9810-woa port does not document USB, cpufreq or watchdog support in Windows, so Starbite may be first for these on Exynos.

**Sources:** https://github.com/ntdevlabs/exynos9810-woa, https://www.xda-developers.com/renegade-project-boot-arm64-windows-10-11-on-android-devices/

## lx-1 [not_first] significance 3
**Claim:** Mainline Linux 7.1 on the Galaxy S9, booted from our own OS store on UFS.

**Earlier work found:** Upstream Exynos 9810 + Galaxy S9 (SM-G960F, starlte) device-tree support was posted by Markuss Broks in Oct 2024, applied for Linux 6.14, and boots mainline to an initramfs shell through uniLoader. More directly, the public GitHub repo SheroAbi/galaxy-s9plus-mainline-linux (created 2026-09-24, first commit 'Ubuntu 24.04 on the Samsung Galaxy S9+ (star2lte, Exynos 9810) with mainline Linux 7.1') ran mainline 7.1 on the S9+ (same SoC) with its own UFS, DECON, ACPM, G3D clock, cpufreq, PCIe/BCM4361 Wi-Fi and USB drivers, a month before our 10-26 milestone. ztOS (Zerone-Laboratories/ztOS, first commit 2026-10-13) then ran mainline 7.1 on the plain Galaxy S9 (SM-G960N) from internal storage, using SheroAbi's drivers. Only the 'booted from our own multi-OS store' detail is ours, and that is a minor packaging novelty.

**Sources:** https://lists.infradead.org/pipermail/linux-arm-kernel/2024-October/972766.html, https://www.phoronix.com/news/Linux-6.14-Samsung-S9-S20, https://github.com/SheroAbi/galaxy-s9plus-mainline-linux, https://github.com/Zerone-Laboratories/ztOS

## lx-2 [not_first] significance 3
**Claim:** postmarketOS Plasma Mobile on the S9 with Panfrost GPU acceleration on the Mali-G72, DECON display and Wi-Fi.

**Earlier work found:** Panfrost has supported the Mali-G72 (Bifrost) since Mesa 20.3, and openSUSE's table names the Exynos 9810. On this exact SoC, SheroAbi's S9+ mainline port (2026-09-24 to 10-10) already had Panfrost on the Mali-G72 with GNOME Wayland, its own DECON scanout driver at 1440x2960 60 fps, and BCM4361 Wi-Fi over its own PCIe driver. ztOS (from 2026-10-13) ran Panfrost GLES 3.1 on Mesa 26.2 on the SM-G960N S9 and mentions Plasma Mobile sessions, which hit Panfrost job faults. A downstream-kernel postmarketOS starlte port also existed earlier. What could be ours is the specific packaging: postmarketOS plus Plasma Mobile, booted from microSD. I could not prove it was first, and it is minor.

**Sources:** https://github.com/SheroAbi/galaxy-s9plus-mainline-linux, https://github.com/Zerone-Laboratories/ztOS, https://en.opensuse.org/ARM_Mali_GPU, https://docs.mesa3d.org/drivers/panfrost.html

## lx-5 [first_on_this_class_of_device] significance 3
**Claim:** Samsung One UI 8 (UN1CA community port for Exynos 9810) booted straight to its home screen through our own multi-boot loader, with /data on a custom partition shared with Windows/iOS layouts.

**Earlier work found:** The One UI 8 port itself is community work. UN1CA (github.com/mustvfa/UN1CA) produces it, and the S9 has had several One UI ports before: One UI 4.1 for Exynos S9/Note 9, FloydROM (One UI 5.1) and CornROM (One UI 7). So booting One UI 8 on the S9 is not ours. Multi-booting Android with Windows or Linux on one phone is old: Renegade Project / Poco F1 multi-boot (Android + postmarketOS + Windows), Surface Duo dual-boot with Windows 11, MultiROM, and Dual Boot Patcher on the S8/Note 8. The one part I found no prior art for is our own loader putting One UI on the same phone and partition layout as Windows and iOS installs. That is an integration achievement, not a port.

**Sources:** https://github.com/mustvfa/UN1CA, https://r2.community.samsung.com/t5/Galaxy-S/Installed-OneUI-7-on-my-galaxy-S9/td-p/18752756, https://wiki.krisyotam.com/content/wikipedia_en_all_nopic_2026-03/Galaxy_S9, https://www.phonearena.com/news/this-midrange-phone-can-triple-boot-android-linux-and-windows_id177560, https://www.xda-developers.com/windows-11-dual-boot-on-microsoft-surface-duo/, https://appuals.com/?p=44390

## sb-3 [not_first] significance 3
**Claim:** Survival Heat: an emergency hand-warmer mode that loads CPU (measured best M3 combo, 2 cores at 2314 MHz), GPU and charging, with live watts, battery-temperature hard stop and heat log.

**Earlier work found:** Phone hand-warmer apps go back to 2009: Pocket Heat on iPhone, which got 'worst app of the week' coverage. Android has FreeHeater ('Full load of CPU and GPU, heat up device as hand warmer') and others. Starbite's version is much better engineered (measured core combos, battery cutoff, OS-level), but the idea is not first.

**Sources:** https://gizmodo.com/pocket-heat-app-turns-your-iphone-into-a-hand-radiator-5439589, https://digitaltrends.com/mobile/worst-app-of-the-week-pocket-heat-kills-your-phone-not-frostbite/, https://android-stats.com/d/com.sqzsoft.freeheater

## sb-4 [not_first] significance 3
**Claim:** USB-C fire starter: Starbite OS pulses up to 1.5 A OTG power into a heater element (steel wool / nichrome) with live load-ohm measurement.

**Earlier work found:** The Oukitel WP63 rugged phone (shown 2025, WP630 at MWC 2026) has a built-in igniter coil controlled by an app, with fire-time settings and a cooldown. 'Start a fire with your phone battery + steel wool' guides have existed for years (Make, Art of Manliness). I found no earlier project that drives an external element from software through the phone's USB-C OTG port with ohm measurement, but the general idea of a phone that starts fires is taken.

**Sources:** https://www.tomsguide.com/phones/i-tried-this-rugged-android-phone-that-has-night-vision-and-can-intentionally-start-a-fire, https://www.engadget.com/mobile/smartphones/oukitel-wp630-mwc-2026-smartphone-igniter-101046023.html, https://www.artofmanliness.com/skills/outdoor-survival/start-fire-cell-phone-battery/, https://makezine.com/?p=66897

## bt-5 [not_first] significance 3
**Claim:** USB fastboot implemented in own loader on a Samsung phone (no stock fastboot), loading XNU into RAM at 43 MB/s.

**Earlier work found:** Mainline U-Boot already runs chainloaded from Samsung's S-BOOT on Exynos phones (Galaxy A 2017 axy17lte, the Exynos 7870 boards, the generic exynos-mobile target). Its docs describe using U-Boot's own fastboot interface on the Exynos 7870 Samsung boards. lk2nd provides fastboot on Qualcomm Samsung Galaxy phones. Loading XNU into RAM over fastboot was also done by HackiOS (fastboot boot of a UEFI, Oct 2026), although on a phone that already has fastboot.

**Sources:** https://docs.u-boot.org/en/v2026.01/board/samsung/exynos-mobile/exynos7870.html, https://docs.u-boot.org/en/latest/board/samsung/exynos-mobile.html, https://docs.u-boot.org/en/latest/board/samsung/axy17lte.html, https://github.com/Kiciuk/lk2nd, https://book.st-hakky.com/en/news/run-ios-16-5-firmware-on-android

## lx-4 [first_on_this_class_of_device] significance 2
**Claim:** Android 17 (LineageOS 24 / Google GSI 17 QPR3 with Play apps, 'Pixel-style') booted to its home screen on the S9 alongside other OSes through our loader.

**Earlier work found:** Booting new Android releases on old Treble devices through GSIs is routine. Google publishes Android 17 GSIs, including QPR3, and the community has long run GSIs on the Exynos S9 (phhusson treble wiki). The S9 has unofficial LineageOS 23 (Android 16), CornROM (One UI 7, Android 15) and Android 16 QPR2 ports. I found no public LineageOS 24 or Android 17 build for starlte, so this may be the first Android 17 on the S9. Technically that is an incremental step. Hosting it as one of several OSes behind a custom loader is a bit more unusual, but MultiROM and Dual Boot Patcher did Android multi-boot on Samsung phones before.

**Sources:** https://developer.android.com/about/versions/17/qpr3/gsi-release-notes, https://www.github.com/phhusson/treble_experimentations/wiki/Samsung-Galaxy-S9-and-S9-Plus-(Exynos), https://wiki.lineageos.org/devices/starlte/, https://wiki.krisyotam.com/content/wikipedia_en_all_nopic_2026-03/Galaxy_S9, https://appuals.com/?p=44390

## sb-5 [not_first] significance 2
**Claim:** Emergency mode: SOS Morse on torch and vibration, beacon, Medical Card without passcode, compass, level, sun, guide, USB power out.

**Earlier work found:** Every one of these exists in stock Android/iOS or in common apps: Medical ID on the lock screen, flashlight SOS/strobe apps, compass and level apps, reverse charging over OTG. Rugged phones such as the Oukitel WP63 ship emergency strobe/beacon modes.

**Sources:** https://www.androidauthority.com/oukitel-wp63-hand-on-3646152/

## sb-10 [not_first] significance 2
**Claim:** Utility OS 2.0: bare-metal mode making the S9 a USB keyboard, mouse, media controller, gamepad and drive for a computer.

**Earlier work found:** Android phones have acted as USB HID devices for years: pelya's android-keyboard-gadget (keyboard, mouse, joystick), the USB Gadget Tool, Kali NetHunter HID, android-hid-client, and mass-storage gadget apps. Those run on a rooted Linux kernel. Doing it bare-metal with no Linux is new only in method.

**Sources:** https://github.com/pelya/android-keyboard-gadget, https://github.com/tejado/android-usb-gadget, https://www.kali.org/docs/nethunter/nethunter-hid-attacks/, https://www.github.com/Arian04/android-hid-client
