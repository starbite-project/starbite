// Starbite: turn the Accessibility Keyboard on at login (Ventura 13.5): UAVirtualKeyboardSetEnabled(true) in
// UniversalAccessCore, the call System Settings' toggle makes (the preference alone did not show it).
// The function is not exported: dlsym first, else UniversalAccessCore's load address + its 22G74 cache offset.
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define UACORE "/System/Library/PrivateFrameworks/UniversalAccess.framework/Versions/A/Frameworks/UniversalAccessCore.framework/Versions/A/UniversalAccessCore"
#define OFF_SET_ENABLED 0x3e1c
int main(void)
{
	void *h = dlopen(UACORE, RTLD_NOW);
	void (*set)(int) = h ? (void (*)(int))dlsym(h, "UAVirtualKeyboardSetEnabled") : NULL;
	if (!set) {
		for (uint32_t i = 0; i < _dyld_image_count(); i++) {
			const char *n = _dyld_get_image_name(i);
			if (n && strstr(n, "/UniversalAccessCore.framework/")) {
				set = (void (*)(int))((const char *)_dyld_get_image_header(i) + OFF_SET_ENABLED);
				break;
			}
		}
	}
	fprintf(stderr, "sbkeyboard: UAVirtualKeyboardSetEnabled at %p\n", (void *)set);
	if (!set)
		return 1;
	sleep(20);	/* after the session's agents are up */
	set(1);
	fprintf(stderr, "sbkeyboard: enabled\n");
	return 0;
}
