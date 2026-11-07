/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS host preview: what host.c's Lab tours reference (never run for Starbite OS). */
#include "eng.h"

struct eng E;
int cur_tab;
const char *tab_title_of(int t) { (void)t; return ""; }
void eng_build_page(struct page *p, void (*fill)(void)) { (void)p; (void)fill; }
void tab_build(int t) { (void)t; }
