# Starbite drivers for Windows 11 on the Galaxy S9 (track D: the Mali GPU)

Design, safety rules and phone tests: `docs/WIN11-GPU.md`.

| Path | What |
|---|---|
| `sbthermal/` | `sbthermal.sys`, the temperature watchdog (read-only TMU, levels, `\Callback\StarbiteThermal`, `\\.\StarbiteThermal`). `sbth_core.c` is the portable part |
| `sbdisplay/` | `sbdisplay.sys` + `sbdisplay.inf`, the display-only driver for DECON (copy mode, tear-free flips, AOR brightness). `sbdc_core.c` is the portable part. Follows wddm-mali-g52's `malidod.c` (GPL-2.0) |
| `acpi/StarbiteGpu.asl` | the ACPI device `\_SB.GPU0` (`STRB0072`): DECON, DPU_DMA, DSIM0, Mali; no interrupts in stage 2 |
| `tests/run.sh` | host tests of both cores on the Mac (fake register files that fail on any write outside the allowed set) |
| `build.sh` | ARM64 kernel build with clang-cl + lld-link; `WDK_KIT` = Microsoft's WDK/SDK 10.0.26100 NuGet packages unpacked |
| `offline-service.sh` | offline SYSTEM-hive edits in lima (install the watchdog service, disable a driver, reset a boot guard); refuses a dirty hive |

```
starbite/win/tests/run.sh
starbite/win/build.sh            # -> build/win-gpu/{sbthermal,sbdisplay}.sys
```

Kill switches for every driver here, in the order to use them: the previous BOOT image (no SSDT, no
device), the file `\Windows\Starbite\<driver>.off`, `offline-service.sh disable`.

Licence: GPL-2.0-only.
