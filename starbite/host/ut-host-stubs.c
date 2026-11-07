/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS host preview (starbite/host/ut-preview.sh): what host.c's Lab tours reference (never run
 * for the Utility OS) and the USB layer (the preview has no controller). UT_HOST_USB=1 in the
 * environment fakes a configured high-speed host link, so the pages show the connected state. */
#include <stdlib.h>
#include "eng.h"
#include "usb_dwc3.h"
#include "usb_phy.h"
#include "max77705_muic.h"

struct eng E;
int cur_tab;
const char *tab_title_of(int t) { (void)t; return ""; }
void eng_build_page(struct page *p, void (*fill)(void)) { (void)p; (void)fill; }
void tab_build(int t) { (void)t; }

volatile unsigned usb_msc_max_lun, usb_msc_reset;
static int host(void) { const char *v = getenv("UT_HOST_USB"); return v && v[0] == '1'; }
int usbd_start(const char *serial, const char **why) { (void)serial; (void)why; return 0; }
void usbd_stop(void) {}
int usbd_poll(struct usbd_ev *ev) { (void)ev; return 0; }
int usbd_queue(unsigned phys, uintptr_t buf, uint32_t len) { (void)phys; (void)buf; (void)len; return 0; }
int usbd_busy(unsigned phys) { (void)phys; return 1; }
int usbd_configured(void) { return host(); }
int usbd_full_speed(void) { return 0; }
int usbd_connected(void) { return host(); }
int usb_phy_start(const char **why) { (void)why; return 0; }
int muic_route_usb(void) { return 1; }
int muic_poll(void) { return 0; }
