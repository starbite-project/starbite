/* Stand-in for Wine's wine/debug.h: the few helpers Wine's test framework (include/wine/test.h) and the D3D tests
 * use, for building those tests against the Windows SDK (tools/win/winetests/README.md). */
#ifndef SB_WINE_DEBUG_H
#define SB_WINE_DEBUG_H
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windef.h>
#include <winbase.h>

#define __WINE_PRINTF_ATTR(fmt, args)

static inline char *sb_dbg_slot(void)
{
    static __declspec(thread) char buf[16][320];
    static __declspec(thread) unsigned pos;
    return buf[pos++ & 15];
}

static inline const char *wine_dbg_sprintf(const char *fmt, ...)
{
    char *s = sb_dbg_slot();
    va_list a;
    va_start(a, fmt);
    vsnprintf(s, 320, fmt, a);
    va_end(a);
    return s;
}

static inline const char *wine_dbgstr_w(const WCHAR *w)
{
    char *s = sb_dbg_slot();
    unsigned i = 0;
    if (!w)
        return "(null)";
    s[i++] = 'L';
    s[i++] = '"';
    for (; *w && i < 300; w++)
        s[i++] = (*w >= 32 && *w < 127) ? (char)*w : '?';
    s[i++] = '"';
    s[i] = 0;
    return s;
}
#define debugstr_w wine_dbgstr_w

static inline const char *wine_dbgstr_rect(const RECT *r)
{
    if (!r)
        return "(null)";
    return wine_dbg_sprintf("(%ld,%ld)-(%ld,%ld)", r->left, r->top, r->right, r->bottom);
}

static inline const char *wine_dbgstr_longlong(ULONGLONG ll)
{
    if (sizeof(ll) > sizeof(unsigned long) && ll >> 32)
        return wine_dbg_sprintf("%lx%08lx", (unsigned long)(ll >> 32), (unsigned long)ll);
    return wine_dbg_sprintf("%lx", (unsigned long)ll);
}

static inline const char *wine_dbgstr_a(const char *a)
{
    return a ? wine_dbg_sprintf("\"%.300s\"", a) : "(null)";
}

static inline const char *wine_dbgstr_guid(const GUID *g)
{
    if (!g)
        return "(null)";
    return wine_dbg_sprintf("{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", g->Data1, g->Data2, g->Data3,
                            g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5],
                            g->Data4[6], g->Data4[7]);
}
#define debugstr_guid wine_dbgstr_guid
#define debugstr_a wine_dbgstr_a
#endif
