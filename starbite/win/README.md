# Starbite drivers for Windows 11 on the Galaxy S9 (track D: the Mali GPU)

Design, safety rules and phone tests: `docs/WIN11-GPU.md`.

| Path | What |
|---|---|
| `sbthermal/` | `sbthermal.sys`, the temperature watchdog (read-only TMU, levels, `\Callback\StarbiteThermal`, `\\.\StarbiteThermal`). `sbth_core.c` is the portable part |
| `sbdisplay/` | `sbdisplay.sys` + `sbdisplay.inf`, the display-only driver for DECON (copy mode, tear-free flips, AOR brightness). `sbdc_core.c` is the portable part. Follows wddm-mali-g52's `malidod.c` (GPL-2.0) |
| `sbmali/` | `sbmali.sys` + `sbmali.inf`, the stage 3 Mali bring-up test driver (KMDF; thermal gate, BUCK6 950 mV through ACPM, both page-table formats, WRITE_VALUE + fragment clear). `sbm_core.c` is the portable part |
| `acpi/StarbiteGpu.asl` | the ACPI device `\_SB.GPU0` (`STRB0072`): DECON, DPU_DMA, DSIM0, Mali; no interrupts in stage 2 |
| `tests/run.sh` | host tests on the Mac: the display and thermal cores (fake register files that fail on any write outside the allowed set) and the Mali core (`mali_test.c`, a fake G72 that walks the page tables and runs the jobs, a fake ACPM/PMIC) |
| `build.sh` | builds, signs and packages both drivers with the shared L0 kit (`tools/win/build.sh wdm`, `sign.sh`, `makecat.py`; kit in `$SBWIN`, see `tools/win/README.md`) |

```
starbite/win/tests/run.sh
starbite/win/build.sh            # -> build/win-gpu/: sbthermal.sys, sbdisplay.{sys,inf,cat}, sbmali.{sys,inf,cat}, StarbiteGpu.aml, SHA256SUMS
```

Kill switches for every driver here, in the order to use them: the previous BOOT image (no SSDT, no
device), the file `\Windows\Starbite\<driver>.off`, `tools/win/hive-service.py IN OUT disable <driver>` (clean hives only).

Licence: GPL-2.0-only.
