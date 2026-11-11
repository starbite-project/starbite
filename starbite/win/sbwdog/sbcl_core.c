/* SPDX-License-Identifier: GPL-2.0-only
 * sbcl: the crash ledger's portable part, see sbcl_core.h. No C library: it runs inside a bugcheck callback.
 */
#include "sbcl_core.h"

sbcl_u32 sbcl_check(const struct sbcl_page *p)
{
    const sbcl_u32 *w = (const sbcl_u32 *)p;
    unsigned n = (unsigned)(((const char *)&p->check - (const char *)p) / 4);
    sbcl_u32 c = 0x9E3779B9U;
    for (unsigned i = 0; i < n; i++)
        c = (c ^ w[i]) * 0x01000193U + i;
    return c;
}

void sbcl_seal(struct sbcl_page *p)
{
    p->check = sbcl_check(p);
}

int sbcl_valid(const struct sbcl_page *p)
{
    return p->magic == SBCL_MAGIC && p->version == SBCL_VERSION && p->state >= SBCL_RUNNING &&
           p->state <= SBCL_BUGCHECK && p->check == sbcl_check(p);
}

struct buf {
    char *s;
    unsigned cap, len;
};

static void put_c(struct buf *b, char c)
{
    if (b->len + 1 < b->cap)
        b->s[b->len++] = c;
    if (b->cap)
        b->s[b->len < b->cap ? b->len : b->cap - 1] = 0;
}

static void put_s(struct buf *b, const char *s)
{
    while (s && *s)
        put_c(b, *s++);
}

static void put_hex(struct buf *b, sbcl_u64 v, unsigned min_digits)
{
    char t[16];
    unsigned n = 0;
    do {
        t[n++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v && n < 16);
    while (n < min_digits && n < 16)
        t[n++] = '0';
    put_s(b, "0x");
    while (n)
        put_c(b, t[--n]);
}

static void put_dec(struct buf *b, sbcl_u32 v)
{
    char t[10];
    unsigned n = 0;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        put_c(b, t[--n]);
}

void sbcl_resolve(const struct sbcl_mod *m, unsigned n, sbcl_u64 addr, char *out)
{
    struct buf b = { out, SBCL_WHERE, 0 };
    out[0] = 0;
    for (unsigned i = 0; m && i < n; i++)
        if (m[i].size && addr >= m[i].base && addr - m[i].base < m[i].size) {
            char name[sizeof m[i].name + 1];
            unsigned k;
            for (k = 0; k < sizeof m[i].name && m[i].name[k]; k++)
                name[k] = m[i].name[k];
            name[k] = 0;
            put_s(&b, name);
            put_c(&b, '+');
            put_hex(&b, addr - m[i].base, 1);
            return;
        }
}

const char *sbcl_bc_name(sbcl_u32 code)
{
    static const struct { sbcl_u32 code; const char *name; } t[] = {
        { 0x0A, "IRQL_NOT_LESS_OR_EQUAL" }, { 0x19, "BAD_POOL_HEADER" }, { 0x1A, "MEMORY_MANAGEMENT" },
        { 0x1E, "KMODE_EXCEPTION_NOT_HANDLED" }, { 0x3B, "SYSTEM_SERVICE_EXCEPTION" },
        { 0x50, "PAGE_FAULT_IN_NONPAGED_AREA" }, { 0x51, "REGISTRY_ERROR" },
        { 0x7A, "KERNEL_DATA_INPAGE_ERROR" }, { 0x7E, "SYSTEM_THREAD_EXCEPTION_NOT_HANDLED" },
        { 0x9F, "DRIVER_POWER_STATE_FAILURE" }, { 0xA0, "INTERNAL_POWER_ERROR" },
        { 0xC2, "BAD_POOL_CALLER" }, { 0xC4, "DRIVER_VERIFIER_DETECTED_VIOLATION" },
        { 0xD1, "DRIVER_IRQL_NOT_LESS_OR_EQUAL" }, { 0xE2, "MANUALLY_INITIATED_CRASH" }, { 0xEA, "THREAD_STUCK_IN_DEVICE_DRIVER" },
        { 0xEF, "CRITICAL_PROCESS_DIED" }, { 0xF4, "CRITICAL_OBJECT_TERMINATION" },
        { 0x101, "CLOCK_WATCHDOG_TIMEOUT" }, { 0x109, "CRITICAL_STRUCTURE_CORRUPTION" },
        { 0x10D, "WDF_VIOLATION" }, { 0x10E, "VIDEO_MEMORY_MANAGEMENT_INTERNAL" },
        { 0x113, "VIDEO_DXGKRNL_FATAL_ERROR" }, { 0x116, "VIDEO_TDR_FAILURE" },
        { 0x117, "VIDEO_TDR_TIMEOUT_DETECTED" }, { 0x119, "VIDEO_SCHEDULER_INTERNAL_ERROR" },
        { 0x124, "WHEA_UNCORRECTABLE_ERROR" }, { 0x133, "DPC_WATCHDOG_VIOLATION" },
        { 0x139, "KERNEL_SECURITY_CHECK_FAILURE" }, { 0x13A, "KERNEL_MODE_HEAP_CORRUPTION" },
        { 0x141, "VIDEO_ENGINE_TIMEOUT_DETECTED" }, { 0x154, "UNEXPECTED_STORE_EXCEPTION" },
        { 0x15F, "CONNECTED_STANDBY_WATCHDOG_TIMEOUT_LIVEDUMP" }, { 0x1C8, "MANUALLY_INITIATED_POWER_BUTTON_HOLD" },
        { 0x1CA, "SYNTHETIC_WATCHDOG_TIMEOUT" },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (t[i].code == code)
            return t[i].name;
    return "";
}

unsigned sbcl_describe(const struct sbcl_page *p, char *out, unsigned cap)
{
    struct buf b = { out, cap, 0 };
    if (cap)
        out[0] = 0;
    if (!sbcl_valid(p)) {
        put_s(&b, "no record");
        return b.len;
    }
    put_s(&b, "boot ");
    put_dec(&b, p->boot_seq);
    put_s(&b, ": ");
    if (p->state == SBCL_SHUTDOWN) {
        put_s(&b, "clean shutdown or restart after ");
        put_dec(&b, p->shutdown_uptime_s);
        put_s(&b, " s");
    } else if (p->state == SBCL_RUNNING) {
        put_s(&b, "ended without a shutdown or bugcheck after ~");
        put_dec(&b, p->uptime_s);
        put_s(&b, " s (a hang reset by the watchdog, a hard reset, or power loss)");
    } else {
        const char *name = sbcl_bc_name(p->bc_code);
        put_s(&b, "BUGCHECK ");
        put_hex(&b, p->bc_code, 8);
        if (name[0]) {
            put_c(&b, ' ');
            put_s(&b, name);
        }
        put_s(&b, " (");
        for (unsigned i = 0; i < 4; i++) {
            if (i)
                put_s(&b, ", ");
            put_hex(&b, p->p[i], 1);
        }
        put_s(&b, ") at ");
        put_dec(&b, p->bc_uptime_s);
        put_s(&b, " s");
        for (unsigned i = 0; i < 4; i++)
            if (p->where[i][0]) {
                char w[SBCL_WHERE + 1];
                unsigned k;
                for (k = 0; k < SBCL_WHERE && p->where[i][k]; k++)
                    w[k] = p->where[i][k];
                w[k] = 0;
                put_s(&b, "; p");
                put_dec(&b, i + 1);
                put_s(&b, " = ");
                put_s(&b, w);
            }
        if (!p->mods)
            put_s(&b, "; no module snapshot yet");
    }
    return b.len;
}
