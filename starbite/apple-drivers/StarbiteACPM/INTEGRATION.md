# StarbiteACPM: integration (for the integrating session)

## 1. Board / ADT (tools/board)

The `acpm-ipc` block needs the APM SRAM, which is outside arm-io's range (0x02039000 < `[soc].base`), so it cannot
be a second `reg` pair today. Either:
- (preferred) emit a property on `/arm-io/acpm-ipc`: `sram` = two little-endian u64 `<0x02039000, 0x30000>`
  (absolute physical) and `initdata-base` = u32 `0x7f00` (both from the DTS `acpm_ipc` node); or
- widen arm-io's ranges so reg 1 can hold it (affects every node; not recommended).

The driver takes reg 1 if present, else `sram`; `initdata-base` defaults to 0x7f00.

## 2. Files (iokit/conf/files.arm64 or a KC)

```
starbite/apple-drivers/StarbiteACPM/StarbiteACPM.cpp          optional iokitcpp
starbite/apple-drivers/StarbiteACPM/hw/acpm_ipc.c             standard
starbite/apple-drivers/StarbiteACPM/hw/s2mps18_policy.c       standard
```
Include path: `starbite/apple-drivers/StarbiteACPM` (the sources include `"hw/..."`).

## 3. Personality (KernelConfigTables.cpp, STARLTE)

```
"   {"
"     'IOClass'         = StarbiteACPM;"
"     'IOProviderClass' = IOPlatformDevice;"
"     'IONameMatch'     = 'starbite,acpm-ipc';"
"     'IOProbeScore'    = 10000:32;"
"   },"
```
If StarbiteARMIO publishes its nubs under another class, use that class (it must be an IOPlatformDevice or
IOService subclass with `getDeviceMemory` filled from `reg`).

## 4. One owner per ACPM channel (hazard H1)

`starlte_touch.c` (0302) has its own ACPM client with its own sequence numbers and lock; `starlte_gpu.c` and
`starlte_sd.c` call it. **Two clients on one channel corrupt each other's queue indices** (a lost request, or an
answer queue that fills and stops the APM until a restart). Before StarbiteACPM runs in the same boot as any of them:
- replace the bodies of `starlte_pmic_read`, `starlte_pmic_write`, `starlte_acpm_dvfs` in starlte_touch.c by calls to
  `starbite_acpm_pmic_read / _write / starbite_acpm_dvfs` (C entry points in StarbiteACPM.h), and make
  `acpm_ready()` / `acpm_send*` unused; or
- keep StarbiteACPM out of that kernel.
The C entry points return false until StarbiteACPM has started; callers that run before IOKit matching (the SD
rails if the store is read early) must then retry later or wait for the service. The same write policy and DVFS
limits apply to them, so today's writes keep working and nothing new gets through.

## 5. Boot-args

- `sbacpm=0`: not started.
- `sbstop=acpm`: 0423 self-stop after the report line (PHONE-TEST.md).

## 6. Clients

- StarbiteRTC matches on `IOProviderClass = StarbiteACPM` (StarbiteRTC/INTEGRATION.md).
- Others: `OSDynamicCast(StarbiteACPM, ...)` on the matched service, or `callPlatformFunction("starbite-acpm-pmic-read"
  | "starbite-acpm-pmic-write" | "starbite-acpm-dvfs", ...)`.
- The DVFS governor (row 21, StarbiteTMU) calls `setThermalGovernor(true)` while it runs and `false` when it stops
  (0411's rule: never above 1794 MHz without it).
