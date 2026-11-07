/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS host preview (starbite/host/ut-preview.sh): what host.c's Lab tours reference (never run
 * for the Utility OS) and the USB layer (the preview has no controller). UT_HOST_USB=1 in the
 * environment fakes a configured high-speed host link, so the pages show the connected state. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "sd.h"
#include "sb.h"
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

/* the microSD card: UT_HOST_CARD, an image file (starbite/tools/fixtures/exfat-card.img.gz unpacked) */
static FILE *card;
static int card_ok;
static FILE *cardf(void)
{
    if (!card) {
        const char *p = getenv("UT_HOST_CARD");
        card = p ? fopen(p, "rb") : 0;
    }
    return card;
}
void sd_allow(void) { card_ok = 1; }
int sd_allowed(void) { return card_ok; }
int sd_up(void) { return cardf() != 0; }
int sd_read(uint64_t s, uint32_t c, uintptr_t d)
{
    FILE *f = cardf();
    if (!f || fseek(f, (long)(s * 512), SEEK_SET))
        return -1;
    size_t n = fread((void *)d, 512, c, f);
    if (n < c)
        memset((uint8_t *)d + n * 512, 0, (c - n) * 512);
    return 0;
}
int sd_store_area(uint64_t *f, uint64_t *b) { (void)f; (void)b; return 0; }
int sd_read4k(uint64_t l, uint64_t c, uintptr_t d) { return sd_read(l * 8, (uint32_t)(c * 8), d); }
int sd_write(uint64_t s, uint32_t c, uintptr_t d) { (void)s; (void)c; (void)d; return -9; }
int sd_write4k(uint64_t l, uint64_t c, uintptr_t d) { (void)l; (void)c; (void)d; return -9; }
uint64_t sd_sectors_hint(void)
{
    FILE *f = cardf();
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    return (uint64_t)ftell(f) / 512;
}
