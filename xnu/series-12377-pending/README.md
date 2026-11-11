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
