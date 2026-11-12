/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS 1.2-alpha9: notifications. A list of the last 8 (newest first): who, a title, a line, the time. The status
 * bar shows a dot while any is unread; the control center (a tap on the status bar) lists them and has Clear; opening
 * it marks them read. Apps post with SYS_NOTIFY (SDK notify()); the kernel posts a missed alarm, heat that stopped by
 * itself, a low battery. Kept in RAM until a restart. Pure C apart from the clock. */
#include "sb.h"
#include "kern.h"

#define NN 8

static struct notif nl[NN];
static int nn, unread;
static unsigned seq;               /* Starbite OS Next: posts so far (its shell copies the new ones) */

static void cpy(char *d, unsigned cap, const char *s)
{
    unsigned i = 0;
    for (; s && s[i] && i + 1 < cap; ++i)
        d[i] = s[i];
    d[i] = 0;
}

void notif_post(const char *who, const char *title, const char *text)
{
    notif_post_app(who, title, text, 0);
}

/* Track A: the same list; the flags go to Next's shell (the classic UI shows every post alike) */
void notif_post_app(const char *who, const char *title, const char *text, unsigned flags)
{
    for (int i = (nn < NN ? nn : NN - 1); i > 0; --i)
        nl[i] = nl[i - 1];
    struct notif *n = &nl[0];
    cpy(n->who, sizeof(n->who), who);
    cpy(n->title, sizeof(n->title), title);
    cpy(n->text, sizeof(n->text), text);
    struct sbos_time t;
    n->hm = clock_now(&t) ? t.h * 100 + t.mi : -1;
    n->flags = flags & (NOTIF_QUIET | NOTIF_URGENT);
    if (nn < NN)
        ++nn;
    ++unread;
    ++seq;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "notif: ");
    ub_s(&u, n->who);
    ub_s(&u, ": ");
    ub_s(&u, n->title);
    sb_log(u.b);
    ui_mark_dirty();
}

unsigned notif_seq(void)
{
    return seq;
}

int notif_count(void)
{
    return nn;
}

int notif_unread(void)
{
    return unread;
}

const struct notif *notif_get(int i)
{
    return i >= 0 && i < nn ? &nl[i] : 0;
}

void notif_seen(void)
{
    if (unread) {
        unread = 0;
        ui_mark_dirty();
    }
}

void notif_clear(void)
{
    nn = unread = 0;
    ui_mark_dirty();
}
