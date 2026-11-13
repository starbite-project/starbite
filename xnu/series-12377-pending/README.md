# xnu-12377 patches held back from the series

Patches here are applied to test kernels only (the port tree build/xnu-12377 has them as commits); tools/build-xnu-12377.sh
applies only xnu/series-12377/*.patch, so daily kernels built from the series do not get them. Move one into
xnu/series-12377/ once its phone test has passed.

| Patch | Why held | Moves when |
|---|---|---|
| 0361-starbite-metal-accelerator-macos.patch | StarbiteAcceleratorMac (an IOAccelerator, `sbmtl=2`): macOS Metal's own discovery loads StarbiteMetal.bundle (no sbmac injection). Only active with sbmtl=2, but it is a macOS-only test path, written for Sequoia's S2 boot | S2 (Sequoia: WindowServer finds the Mali's Metal device) passed on the phone |
| 0428-starbite-m8a-display.patch | Replaces SweetFramebuffer with StarbiteDisplay on every STARLTE boot (a 3 s wait then CPU copy without the board ADT; sweetfbdump/sweetfbcrop/a5fb_* gone) | M8a passed on the phone, and daily command lines have sbdisp=copy until M8b |
| 0429-starbite-m4-iostoragefamily.patch | Apple's IOStorageFamily built in (IOMedia, IOMediaBSDClient, GPT/MBR schemes); its lines sit on 0428's in KernelConfigTables / files.arm64. No IOMedia exists without StarbiteUFS, but it changes every STARLTE kernel | M4a passed, together with 0428 |
| 0430-starbite-m4-ufs.patch | StarbiteUFS (fenced UFS host, read-only) on the board ADT's ufs node; needs 0429 | M4a + M4b passed |
| 0431-starbite-m11b-ionetworkingfamily.patch | Apple's IONetworkingFamily built in: IONetworkStack starts on every boot of this kernel (IOResources + IOBSD), so daily macOS/iOS boots would meet it; needs 0428-0430 underneath (context) | M11b-NCM passed, and one daily boot checked with it |
| 0432-starbite-m11b-ncm.patch | StarbiteUSBNCM (needs 0431) and the sbstop=ncm test (kernel-side naming and IPv6 link-local of the NCM interface); only active with sbusb=1 / sbstop=ncm | with 0431 |

0429-0432 apply in order on top of 0428 (port tree build/xnu-12377, commits 342af625, a1f4d51e, beb4d4c6, 9c5bdfc2).
Test kernels: out/kernel.development.starlte-12377-sd430 (0428 + 0429 + 0430: M4a, M4b) and -sd432 (all four: M11b-NCM).

0361 sits on 0427 (port tree branch seq-12377, commit adf389d3, on 63f51800) and also applies to the 0432 tree (checked
with git apply). Test kernel: out/kernel.development.starlte-12377-seq1 = 0003-0427 (sd427, with 0360's IOSurfaceRoot) +
0361, SweetFramebuffer kept (no 0428). Built incrementally in build/dxb-12377 (the 0427 versions of KernelConfigTables,
SweetFramebuffer and SweetPlatformExpert + 0361, compiled with the tree's own flags, linked from link.filelist without
0428-0432's objects; the same recipe without 0361 reproduces sd427 except the three objects' N_OSO mtimes), then the
tree was restored and relinked to sd432's sha256.
| 0433-starbite-m8b-display-fb-from-adt.patch | Fix to 0428 (boot framebuffer from the ADT); sits on 0432 | with 0428 |
| 0434-starbite-m8c-display-carveout.patch | Fix to 0428: flip/scaler buffers from the board ADT's scan-out carve-out (decon starbite,scanout-carveout, 0xa0000000 + 64 MiB); sits on 0433 | with 0428 |
| 0435-starlte-sd-store-gap.patch | sdroot= ranges in the home card store's gap (head .. first data block, no directory entry there) allowed: the card store's used end now covers the Sequoia and Ventura roots. Applies on 0427 alone (starlte_sd.c unchanged by 0428-0434); changes every card-root boot (mac13 too), so held here until S1b passes | candidate for the daily series |
| 0436-starlte-sd-raw-user-io.patch | raw-device I/O with a user buffer (physio, fsck_hfs on /dev/rsdroot0) through a kernel bounce buffer (copyin/copyout) in the issuing thread; on 0435 | with 0435 |
| 0437-starlte-deadman-flush.patch | the 0421 dead-man saves the kernel log to the card (sdlog=1) and the console tail to pmsg, then resets into TWRP, instead of only stopping the feed; on 0436 | daily candidate (test boots only: sweetreboot= set) |
| 0438-sweet-sandbox-sentinel.patch | security.mac.sandbox.sentinel string sysctl from the Sandbox stand-in (libsystem_sandbox _amkrtemp; MDS DbCreate; securityd) | daily candidate |
| 0439-a5xelte-iokit-trace.patch | the SWEET IOKit trace (sweetiolog=NAMES) on A5XELTE/STARLTE too: matching + counts, path lookups, user-client opens (Metal discovery in WindowServer) | diagnostic; daily OK (off unless named) |
| 0440-sweet-fb-swap-rec-sequoia.patch | SweetFramebuffer: a swap record of 0x514 bytes or more is the shifted layout (macOS 15 Sequoia puts 2 at 0x50, so the old "0x98 set, 0x50 zero" test failed and no frame reached the panel). Applies on seq-12377; kernel out/kernel.development.starlte-12377-seq1k = seq1j + 0440, built outside the tree (the four seq sources + 0360's personality, the tree's other objects, -lcc_kext last: seq1j reproduced section for section first). StarbiteDisplay's hw/sbd_swap.c has the same test and needs the same fix before 0428 meets Sequoia | S2t (Sequoia frames on the panel) passed |
| 0441-sweet-sample-children.patch | sweetsample=NAMES also samples the children of those processes ("SMP x[pid] is a child of y[ppid]"); diagnostic, off unless sweetsample is set. Kernel seq1l = seq1k + 0441 | daily OK (diagnostic) |
| 0442-sweet-copyio-silent-recover-log.patch | "SWEET RECOVER" for copyio recoveries that return EFAULT with no line (kernel-address faults, alignment faults: with PAR_EL1 attributes) and "SWEET COPYWIN" for copies through a CPU copy window (pages with non-default cache attributes); 24 lines then powers of two. Diagnostic for S3t's silent EFAULT. Kernel seq1m = seq1l + 0442 | daily OK (diagnostic, few lines) |
| 0443-starlte-sd-cluster-direct-io.patch | Fix to 0436: physio bufs only (B_PHYS without B_CLUSTER) take the kernel bounce buffer; the cluster layer's direct I/O (B_PHYS + UPL) got EFAULT from the bounce's copyout to a kernel address (S3t/S3u: containermanagerd's sandbox profile read, so no menu bar). Kernel seq1n = seq1m + 0443 | with 0436 (any kernel carrying 0436 needs it) |
| 0444-sweet-fb-cursor-surface-log.patch | "SWEET CURSOR" lines: the pointer surface's alpha/brightness (12 logs at most). Diagnostic for S3v's black box around the cursor. Kernel seq1o = seq1n + 0444 | daily OK (diagnostic) |
| 0445-sweet-hid-trackpad.patch | macOS touch = real trackpad by default (sweettouchrel=2): relative motion, tap = click, tap-and-move = drag, two-finger tap = right click, two-finger drag = scroll; =1 keeps direct touch. Kernel seq1p = seq1o + 0445; seq1q = tap-then-drag threshold 30 px | after S3z4 passes on the phone |
| 0446-sweet-a64e-hot-pcs.patch | "STARLTE LSE hot:" also lists arm64e-emulation trap PCs (which instructions cost the kernel ~60 % on the Sequoia desktop). Diagnostic. Kernel seq1r = seq1q + 0446 | diagnostic |
| 0447-sweet-topk-kernel-threads.patch | "STARLTE TOPK" lines: the busiest kernel_task threads every 10 s. Diagnostic. Kernel seq1s = seq1r + 0447 | diagnostic |
| 0448-sweet-fb-persistent-pointer.patch | SweetFramebuffer keeps the pointer layer across frames (mask = updated layers), merges unshown swaps, SWEET FB STATS; trackpad gain x1, acceleration capped x1.8 (S3z3-5: flashing, jumping pointer). Kernel seq1t = seq1s + 0448 | after the user confirms a steady pointer |
| 0449-sweet-fb-changed-rows.patch | SweetFramebuffer rescales only changed rows of a full frame (S3z7: ~60 % of a core rescaling unchanged frames 11x a second). Kernel seq1u = seq1t + 0449 | with 0448 |
