/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sbm: modem.bin (the RADIO partition) read-only: the TOC, the CP memory map ("MEMn" table inside BOOT) and
 * the load plan (which section goes where in modem_if). docs/MODEM.md section 2. This file only parses caller
 * buffers; it never reads or writes a partition.
 *
 * This phone's RADIO (backup 2026-11-05) for reference:
 *   TOC    off 0          load 0           size 0x200      id 5 (entries after it)
 *   BOOT   off 0x200      load 0x40000000  size 0x1e40
 *   MAIN   off 0x2040     load 0x40010000  size 0x23d8ea0
 *   VSS    off 0x23daee0  load 0x47900000  size 0x57cbf0
 *   NV     off 0          load 0x44900000  size 0x100000   (data: EFS nv_data.bin)
 *   OFFSET off 0x7aa00    load 0           size 0x85600    (a range inside NV's 1 MB; meaning unconfirmed)
 *   MEM0 at 0x2a0: dram 0x7900000, ext 0x1900000, VSS@0x7900000+0x700000, IPC@0x8000000+0x500000,
 *                  ZMC@0x8500000+0xb00000, VPA@0x9000000+0x200000, LOG@0x9200000+0x2000000
 */
#include "sbm.h"

static int name_is(const char *a, const char *n)
{
    int i = 0;
    for (; n[i]; ++i)
        if (a[i] != n[i])
            return 0;
    return a[i] == 0;
}

int sbm_toc_parse(const uint8_t *b, uint64_t image_size, struct sbm_toc *t)
{
    sbm_memset(t, 0, sizeof(*t));
    t->boot = t->main = t->vss = t->nv = t->offset_ent = -1;
    if (!b)
        return SBM_EINVAL;
    for (unsigned i = 0; i < SBM_TOC_MAX; ++i) {
        const uint8_t *r = b + SBM_TOC_ENT * i;
        if (r[0] == 0 || r[0] == 0xFF)
            break;
        struct sbm_toc_ent *e = &t->e[t->n];
        int ended = 0;
        for (int k = 0; k < 12; ++k) {
            uint8_t c = r[k];
            if (c == 0) {
                ended = 1;
                continue;
            }
            if (ended || c < 0x21 || c > 0x7E)
                return SBM_EINVAL;          /* not a TOC (or garbage after the name's NUL) */
            e->name[k] = (char)c;
        }
        e->offset = sbm_le32(r + 12);
        e->loadaddr = sbm_le32(r + 16);
        e->size = sbm_le32(r + 20);
        e->crc = sbm_le32(r + 24);
        e->entryid = sbm_le32(r + 28);
        int from_radio = !name_is(e->name, "NV") && !name_is(e->name, "OFFSET");
        if (from_radio && image_size && (uint64_t)e->offset + e->size > image_size)
            return SBM_ERANGE;
        if (name_is(e->name, "BOOT"))
            t->boot = (int)t->n;
        else if (name_is(e->name, "MAIN"))
            t->main = (int)t->n;
        else if (name_is(e->name, "VSS"))
            t->vss = (int)t->n;
        else if (name_is(e->name, "NV"))
            t->nv = (int)t->n;
        else if (name_is(e->name, "OFFSET"))
            t->offset_ent = (int)t->n;
        t->n++;
    }
    if (t->n == 0 || !name_is(t->e[0].name, "TOC"))
        return SBM_EINVAL;
    t->hdr_size = t->e[0].size;
    if (t->hdr_size < SBM_TOC_ENT * t->n || t->hdr_size > 0x10000)
        return SBM_EINVAL;
    if (t->boot < 0 || t->main < 0)
        return SBM_ENOENT;
    /* sections read from modem.bin start after the TOC and do not overlap each other */
    for (unsigned i = 1; i < t->n; ++i) {
        const struct sbm_toc_ent *a = &t->e[i];
        if ((int)i == t->nv || (int)i == t->offset_ent)
            continue;
        if (a->offset < t->hdr_size)
            return SBM_ERANGE;
        for (unsigned j = i + 1; j < t->n; ++j) {
            const struct sbm_toc_ent *c = &t->e[j];
            if ((int)j == t->nv || (int)j == t->offset_ent)
                continue;
            if ((uint64_t)a->offset < (uint64_t)c->offset + c->size && (uint64_t)c->offset < (uint64_t)a->offset + a->size)
                return SBM_ERANGE;
        }
    }
    return 0;
}

/* The table (Samsung's struct cp_reserved_map_table): u32 table_id_ver, dram_size, ext_mem_size, ext_bin_count,
 * then 5 x { u32 tag, addr, size }, u32 end flag. Byte order of the strings (seen in this phone's RADIO):
 * the id is stored "\0MEM" with the version as the first byte (+'0' makes "MEM0"), each tag as its name
 * reversed with a trailing NUL ("SSV\0" = VSS). */
void sbm_memmap_parse(const uint8_t *p, struct sbm_memmap *m)
{
    sbm_memset(m, 0, sizeof(*m));
    m->cp_size = SBM_DT_CP_SIZE;
    m->ipc_off = SBM_DT_IPC_OFF;
    m->ipc_size = SBM_DT_IPC_SIZE;
    m->vss_off = SBM_DT_CP_SIZE;
    m->vss_size = SBM_DT_VSS_SIZE;
    m->zmb_off = SBM_DT_ZMB_OFF;
    m->zmb_size = SBM_DT_ZMB_SIZE;
    if (!p)
        return;
    uint32_t id = sbm_le32(p) + 0x30;
    m->id[0] = (char)(id >> 24);
    m->id[1] = (char)(id >> 16);
    m->id[2] = (char)(id >> 8);
    m->id[3] = (char)id;
    for (int i = 0; i < 4; ++i)
        if ((uint8_t)m->id[i] < 0x20 || (uint8_t)m->id[i] > 0x7E)
            m->id[i] = '?';
    m->dram_size = sbm_le32(p + 4);
    m->ext_mem_size = sbm_le32(p + 8);
    m->ext_bin_count = sbm_le32(p + 12);
    if (m->id[0] != 'M' || m->id[1] != 'E' || m->id[2] != 'M')
        return;
    if (m->ext_bin_count == 0 || m->ext_bin_count > SBM_EXTBIN_MAX)
        return;
    for (unsigned i = 0; i < m->ext_bin_count; ++i) {
        const uint8_t *r = p + 16 + 12 * i;
        uint32_t tag = sbm_le32(r);
        m->bin[i].tag[0] = (char)(tag >> 16);
        m->bin[i].tag[1] = (char)(tag >> 8);
        m->bin[i].tag[2] = (char)tag;
        m->bin[i].tag[3] = 0;
        m->bin[i].addr = sbm_le32(r + 4);
        m->bin[i].size = sbm_le32(r + 8);
        if (i && (uint64_t)m->bin[i - 1].addr + m->bin[i - 1].size != m->bin[i].addr)
            return;                         /* Samsung refuses a non-contiguous map */
    }
    /* everything has to sit inside the carve-out */
    uint64_t end = (uint64_t)m->bin[m->ext_bin_count - 1].addr + m->bin[m->ext_bin_count - 1].size;
    if (m->dram_size == 0 || m->dram_size > SBM_MODEM_IF_SIZE || m->bin[0].addr < m->dram_size ||
        end > (uint64_t)SBM_MODEM_IF_SIZE + SBM_CPLOG_SIZE)
        return;
    m->cp_size = m->dram_size;
    m->vss_off = m->dram_size;
    for (unsigned i = 0; i < m->ext_bin_count; ++i) {
        const char *t = m->bin[i].tag;
        uint32_t a = m->bin[i].addr, s = m->bin[i].size;
        if (name_is(t, "IPC")) {
            m->ipc_off = a;
            m->ipc_size = s;
        } else if (name_is(t, "VSS")) {
            m->vss_off = a;
            m->vss_size = s;
        } else if (name_is(t, "VPA")) {
            m->vparam_off = a;
            m->vparam_size = s;
        } else if (name_is(t, "ZMC")) {
            m->zmb_off = a;
            m->zmb_size = s;
        } else if (name_is(t, "LOG")) {
            m->log_off = a;
            m->log_size = s;
        }
    }
    m->valid = 1;
}

int sbm_plan_build(const struct sbm_toc *t, const struct sbm_memmap *m, struct sbm_plan *p)
{
    static const char *const order[] = { "BOOT", "MAIN", "VSS", "NV" };
    sbm_memset(p, 0, sizeof(*p));
    if (t->boot < 0 || t->main < 0)
        return SBM_ENOENT;
    uint32_t base = t->e[t->boot].loadaddr;
    if (base != SBM_CP_VIEW_BASE)
        return SBM_EINVAL;
    const int idx[4] = { t->boot, t->main, t->vss, t->nv };
    for (unsigned k = 0; k < 4; ++k) {
        if (idx[k] < 0)
            continue;
        const struct sbm_toc_ent *e = &t->e[idx[k]];
        if (e->loadaddr < base || e->size == 0)
            return SBM_ERANGE;
        uint64_t dst = (uint64_t)e->loadaddr - base;
        /* BOOT, MAIN and NV live in the CP's own memory (cp_size); VSS in the VSS region after it */
        uint64_t lo = 0, hi = m->cp_size;
        if (k == 2) {
            lo = m->vss_off;
            hi = (uint64_t)m->vss_off + m->vss_size;
        }
        if (dst < lo || dst + e->size > hi)
            return SBM_ERANGE;
        struct sbm_load *l = &p->l[p->n++];
        sbm_strlcpy(l->name, order[k], sizeof(l->name));
        l->src = k == 3 ? SBM_SRC_NV : SBM_SRC_RADIO;
        l->src_off = k == 3 ? 0 : e->offset;
        l->size = e->size;
        l->dst_off = (uint32_t)dst;
    }
    /* no two destinations overlap */
    for (unsigned i = 0; i < p->n; ++i)
        for (unsigned j = i + 1; j < p->n; ++j) {
            const struct sbm_load *a = &p->l[i], *c = &p->l[j];
            if ((uint64_t)a->dst_off < (uint64_t)c->dst_off + c->size && (uint64_t)c->dst_off < (uint64_t)a->dst_off + a->size)
                return SBM_ERANGE;
        }
    p->boot_size = t->e[t->boot].size;
    p->main_size = t->e[t->main].size;
    return 0;
}
