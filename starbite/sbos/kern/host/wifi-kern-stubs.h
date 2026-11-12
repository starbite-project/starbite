/* SPDX-License-Identifier: GPL-2.0-only
 * wifi-kern-stubs.c's knobs for wifi-kern-test.c */
#ifndef WIFI_KERN_STUBS_H
#define WIFI_KERN_STUBS_H
#include <stdint.h>
extern uint64_t wk_now_ms;
extern int wk_verbose, wk_heater_busy, wk_spi_ok;
extern unsigned wk_settings_changed, wk_notified, wk_wdt_feeds;
extern int64_t wk_clock_utc, wk_net_utc;
extern int wk_clock_approx;
unsigned wk_log_n(void);
const char *wk_log_line(unsigned i);
/* host/model.c's harness knobs */
extern int model_ap_hidden, model_ap_listed24, model_link_gone;
#endif
