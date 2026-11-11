/* SPDX-License-Identifier: GPL-2.0-only
 * bcmw on libsb (bcmw_sb.c). */
#pragma once
#include "../bcmw.h"

/* VENDOR opened as ext4 for the radio's files; 1 if it is. say() gets one line on failure. */
int bcmw_sb_vendor(void (*say)(const char *));
long bcmw_sb_vendor_read(const char *path, void *dst, uint32_t cap);   /* a VENDOR file into dst; its length or -1 */
/* the OS functions for libsb; log and event are the image's */
void bcmw_sb_os(struct bcmw_os *os, void (*log)(void *, const char *), void (*event)(void *, const struct bcmw_event *));
