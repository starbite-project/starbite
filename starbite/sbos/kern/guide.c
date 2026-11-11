/* SPDX-License-Identifier: MIT
 * 1.2-alpha28: the offline survival guide for Starbite OS Next's Emergency mode (the classic UI has apps/guide.c);
 * the text is guide/sbguide.h, shared by both. */
#include "survival.h"
#include "../guide/sbguide.h"

const struct sbg_chapter *surv_guide(int *n)
{
    if (n)
        *n = SBG_NCH;
    return sbg_chapters;
}
