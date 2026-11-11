# StarbiteWatchdog: integration

## 1. Files

```
starbite/apple-drivers/StarbiteWatchdog/StarbiteWatchdog.cpp      optional iokitcpp
starbite/apple-drivers/StarbiteWatchdog/hw/sbwdt.c                standard
```
`StarbiteWatchdog.cpp` defines `IOWatchdog` and `IOWatchdogUserClient` (the names watchdogd matches). **Exclude
`iokit/Kernel/arm/SweetWatchdog.cpp` on STARLTE** (it defines the same two classes under `#if defined(SWEET)`, and
SWEET is defined for STARLTE), e.g. `#if defined(SWEET) && !defined(STARLTE)`.

## 2. Personality

Replace the `IOWatchdog` / `IOResources` entry of KernelConfigTables.cpp on STARLTE by:
```
"   {"
"     'IOClass'         = StarbiteWatchdog;"
"     'IOProviderClass' = IOPlatformDevice;"
"     'IONameMatch'     = 'wdt-cl1';"
"     'IOMatchCategory' = IOWatchdog;"
"   },"
```
For a kernel booted with an old ADT (no arm-io nubs) use instead the old `IOResources` personality with
`IOClass = IOWatchdog`: the plain front end, check-ins only, no hardware. Never both in one kernel (two
`IOWatchdog` services).

## 3. Optional: the userspace veto (observer mode, rule 6)

Add to herolte_ufs.c (STARLTE) and build StarbiteWatchdog with `-DSTARBITE_WDT_VETO_HOOK`:
```c
void starlte_wdt_veto(const char *why);
void
starlte_wdt_veto(const char *why)
{
	if (!starlte_wdt_stop) {
		starlte_wdt_stop = true;
		printf("STARBITE HEALTH: feeding stopped by StarbiteWatchdog (%s); the watchdog resets in about %u s\n",
		    why, starlte_sbwdt_s);
	}
}
```
It can only stop feeding (fail-safe).

## 4. Later: owner mode

Only after the herolte_ufs.c feeder (0399's thread call, 0421's dead-man) is removed: build with
`-DSTARBITE_WDT_OWNER`, boot with `sbwdtown=1`. Give the driver the PMU mask words first (open question 2).

## 5. Boot-args

`sbwdtuser=T` (userspace check-in timeout, off by default), `sbwdtown=1` (owner build only); reads `sbwdt`,
`sbstop`, `sweetreboot`, `sbnoui` like today's kernel.
