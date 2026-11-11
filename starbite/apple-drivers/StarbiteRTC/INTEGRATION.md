# StarbiteRTC: integration

## 1. Files

```
starbite/apple-drivers/StarbiteRTC/StarbiteRTC.cpp            optional iokitcpp
starbite/apple-drivers/StarbiteRTC/hw/s2mps18_rtc.c           standard
```
Needs StarbiteACPM (its header is included as `../StarbiteACPM/StarbiteACPM.h`). Include path:
`starbite/apple-drivers/StarbiteRTC`.

## 2. Personality

```
"   {"
"     'IOClass'         = StarbiteRTC;"
"     'IOProviderClass' = StarbiteACPM;"
"     'IOMatchCategory' = StarbiteRTC;"
"   },"
```

## 3. Platform expert: forward the time of day

XNU's calendar comes from `PEGetUTCTimeOfDay` -> `gIOPlatform->getUTCTimeOfDay` -> `getGMTTimeOfDay`, which is 0
in `IOPlatformExpert` (and in SweetPlatformExpert). StarbitePE (or SweetPlatformExpert until then) must forward,
as Apple's AppleARMPE does with its RTC:

```cpp
#include "../StarbiteRTC/StarbiteRTC.h"   // or a forward declaration + the two calls
long StarbitePE::getGMTTimeOfDay(void)
{
	StarbiteRTC *rtc = StarbiteRTC::shared();
	return rtc ? rtc->getGMTTimeOfDay() : 0;
}
void StarbitePE::setGMTTimeOfDay(long secs)
{
	StarbiteRTC *rtc = StarbiteRTC::shared();
	if (rtc) {
		rtc->setGMTTimeOfDay(secs);   // writes only with sbrtcwrite=1
	}
}
```
`clock_set_calendar_microtime` (settimeofday, and the card clock 0393 / 0395) calls `PESetUTCTimeOfDay` with
interrupts enabled and no lock held, so the ACPM round trips (up to ~2 s worst case) are allowed there.

## 4. 0402 (no IORTC wait)

StarbiteRTC publishes the `IORTC` resource after its first read. Once M5a passed, change 0402 so that on STARLTE
`IOKitInitializeTime` waits for `IORTC` with a short timeout (5 s) instead of not at all (`sbrtcwait` stays as the
override). Then `clock_initialize_calendar` reads the RTC.

## 5. The card clock (0393 / 0395)

Keep it as the fallback. Once M5c passed: the card restore still runs after the RTC; with writes on it also
corrects the RTC through `PESetUTCTimeOfDay`. Retire 0393 / 0395 only after a week of boots with correct dates.

## 6. Boot-args

`sbrtc=0` off; `sbrtcwrite=1` writes; `sbrtcset=SECS` test write; `sbstop=rtc` self-stop. Make writes the default
(flip `fWrite`) after M5b.
