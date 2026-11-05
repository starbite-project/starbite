# macOS 13.5 Ventura on the S9: status (2026-11-05, 13:10)

Boot: menu entry **macOS Ventura** (catalog key `mac13`, payload `out/xnu-mac13-v27.xnu`, command line
`build/macos13-user.txt`: no test reset (sweetreboot=86400), no debug dumps). Root: the microSD, LBA 216442880,
15 GiB HFSX, image `build/macos13/root-desktop.img`. User `starbite`, password `starbite`, logs in automatically.

## Works
- Boot to the desktop in about 3 to 4 minutes: wallpaper, menu bar (battery, Wi-Fi icon, Control Center, clock), Dock with all
  17 icons, Finder, Notification Center, Control Center, System Settings.
- Mali-G72 compositing through the Metal bridge (StarbiteMetal + PanVK), 60 Hz pointer updates.
- Touch as a trackpad: one finger moves the pointer, tap = click, two-finger drag = scroll (natural), two-finger
  tap = secondary click (kernel 0378, 0394). No pointer trails (kernel 0392).
- Correct date and time (kernel 0393/0395: from the card's last sector; saved every 10 minutes and at resets).
- Booting again after a reset (the /var/run fix, tools/macos/ventura-dirs-cleaner.sh).
- Idle load about 17 to 25 % of the 8 cores after the first minutes.

## Known problems
- No Wi-Fi, Bluetooth or sound driver on macOS (Safari has nothing to load; it also crashes at launch, a NULL
  version string in Safari.framework +0x44dd60).
- No keyboard: the Accessibility Keyboard does not appear on its own (Assistive Control runs, pref
  `virtualKeyboardOnOff` set); turn it on in System Settings > Accessibility > Keyboard.
- Apps take minutes to open the first time (sandbox/container setup, CPU emulation of LSE atomics and PAC).
- Icons re-render for 5 to 8 minutes after each boot (iconservicesagent through software OpenGL), cached icons show at once.
- If WindowServer ever restarts, the login window asks for the password (no keyboard: see above).

## Overnight / today's fixes (all committed)
kernel 0387 (swap record size), 0388/0389/0391 (trace/dump tools), 0390 (touch test), 0392 (pointer trails),
0393/0395 (clock), 0394 (two-finger gestures); sbmac pixel format at QuartzCore+0x2d6548 and refused-pipeline logs;
Metal bridge: unset function constants zeroed, unknown selectors answer 0; Mesa RA spill overflow fix; securityd
patch (tools/macos/ventura-securityd-patch.py); UserAccountUpdater no-op; kernelmanagerd no panic; Finder, SF Symbols,
app binaries, Safari cryptex, Input Methods fetched; icon caches harvested (tools/macos/ventura-harvest.py);
background jobs and widgets off (tools/macos/ventura-smooth.list, tools/macos/ventura-widgets-off.list).
