/* SPDX-License-Identifier: MIT
 * Boot self test of the EL0 path (docs/STARBITE-OS.md 14): a hidden app that makes two system calls and then
 * spins forever. The kernel must stop it after its 500 ms budget ("test-loop stopped responding" in the log);
 * if the budget did not work the watchdog would reset the phone. Started only in interrupt mode. */
#include "sbapp.h"

SBAPP_DEFINE("test-loop", "EL0 budget test", 0, 0, 0);

void app_event(const struct sbos_event *ev)
{
    if (ev->type != EV_APP_START)
        return;
    if (sbos_call(SYS_VERSION, 0, 0, 0, 0, 0) == SBOS_API)
        sb_log_app("test-loop: system calls work, now spinning");
    for (volatile unsigned long i = 0;; ++i)
        ;
}
