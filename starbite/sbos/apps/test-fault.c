/* SPDX-License-Identifier: MIT
 * Boot self test of the EL0 path (docs/STARBITE-OS.md 14): a hidden app that reads kernel memory (the kernel
 * image at 0x92080000, EL1-only). The read must fault and the kernel must stop the app with the ESR in the log
 * ("test-fault: stopped (a fault, ESR 0x92......)"); a value read back would mean EL0 can see the kernel. */
#include "sbapp.h"

SBAPP_DEFINE("test-fault", "EL0 fault test", 0, 0, 0);

void app_event(const struct sbos_event *ev)
{
    if (ev->type != EV_APP_START)
        return;
    volatile const unsigned *k = (volatile const unsigned *)0x92080000UL;
    unsigned v = *k;
    sb_log_app(v ? "test-fault: READ KERNEL MEMORY (EL0 isolation broken)" : "test-fault: read 0 from the kernel");
}
