/* SPDX-License-Identifier: GPL-2.0-only
 * The write fence and the authorisation gate (docs/ENGINEER-OS.md section 8, Engineer OS v2).
 *
 * Every block write libsb makes goes through sb_write (or sb_write_store, the OS store's own
 * append-only writer). The fence classifies the target against the live GPT by name and range
 * and says which tier of confirmation it needs:
 *   tier 3  EFS, CPEFS, PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOTA0-2, DTB, DTBO, the GPT/PIT head
 *           (blocks before the first partition) and tail (after the last), every boot LU (LU 1..7)
 *           -> typed name, slide, a second confirmation, and a verified backup first
 *   tier 2  BOOT, RECOVERY, UP_PARAM, RADIO, any partition not listed, the live part of the
 *           Starbite OS store (its header, directory and entries) -> typed name and slide
 *   tier 1  SYSTEM, VENDOR, ODM, A16DATA, WINESP, WINDOWS, USERDATA (outside the live store),
 *           CACHE, OMR, HIDDEN, CP_DEBUG, the microSD card -> warning and slide
 *   tier 4  (v2.1) a raw LU0 write that crosses a partition boundary, lies between partitions or
 *           has no readable GPT to name it -> a red full-screen warning, "<target> I ACCEPT"
 *           typed, slide, Power held 5 s, and a verified backup unless "no backup" was picked
 *   refused only what is not a write at all: an empty or wrapping range, a medium that does not
 *           exist
 * Nothing is refused because it is risky: a risky target needs more confirmation, not less power.
 *
 * The confirmation itself is an authorisation (struct sb_auth) that engineer/armed.c fills while the
 * user goes through the warning screen, the slide, the typed name, the second confirmation and the
 * backup; each step is recorded and the record is sealed. One authorisation at a time is open (the
 * gate, for the length of one confirmed action). sb_write writes only when the gate's authorisation
 * covers the whole range, has the tier the fence asks for and every step that tier needs. The
 * storage drivers (ufs.c ufs_write_blocks, sd.c sd_write) refuse unless sb_write is running them.
 * Disarming (or a reboot: it is RAM) voids every authorisation made so far.
 * The same gate guards the other writes (registers, PMIC, MAX77705): their policies are in
 * engineer/policy.c, the gate check is sb_gate_check.
 */
#include "sb.h"
#include "ufs.h"
#include "sd.h"

static int eq(const char *a, const char *b)
{
    return sb_streq(a, b);
}

int sb_never_part(const char *g)
{
    return eq(g, "EFS") || eq(g, "CPEFS") || eq(g, "PARAM") || eq(g, "KEYSTORAGE") || eq(g, "PERSISTENT") ||
           eq(g, "STEADY") || eq(g, "BOTA0") || eq(g, "BOTA1") || eq(g, "BOTA2") || eq(g, "DTB") ||
           eq(g, "DTBO");
}

static int tier2_part(const char *g)
{
    return eq(g, "BOOT") || eq(g, "RECOVERY") || eq(g, "UP_PARAM") || eq(g, "RADIO");
}

static int tier1_part(const char *g)
{
    return eq(g, "SYSTEM") || eq(g, "VENDOR") || eq(g, "ODM") || eq(g, "A16DATA") || eq(g, "WINESP") ||
           eq(g, "WINDOWS") || eq(g, "USERDATA") || eq(g, "CACHE") || eq(g, "OMR") || eq(g, "HIDDEN") ||
           eq(g, "CP_DEBUG");
}

/* The live OS store per medium (store.c reports it when it opens one). */
static struct {
    uint64_t base, used, capacity;
} live[2];

static void vg_clear(void);

void sb_fence_store(unsigned medium, uint64_t base, uint64_t used, uint64_t capacity)
{
    if (medium < 2 && (live[medium].base != base || live[medium].used != used))
        vg_clear();                         /* a store read again: a standing grant must be asked for again */
    if (medium < 2) {
        live[medium].base = base;
        live[medium].used = used;
        live[medium].capacity = capacity;
    }
}

static int in_live_store(unsigned m, uint64_t lba, uint64_t n)
{
    return m < 2 && live[m].used && lba < live[m].base + live[m].used && lba + n > live[m].base;
}

static void decide_init(struct sb_wdecide *d)
{
    d->tier = SB_TIER_NO;
    d->part[0] = 0;
    d->first = d->last = 0;
    d->why = "unknown target";
    d->undo = "-";
}

/* v2.1: what v2.0 refused as "not one partition" is a tier 4 raw write of LU0 (the tools still
 * write one partition at a time; this is for a hand-made range in the sector viewer). */
static void raw_lu0(struct sb_wdecide *d, const char *why)
{
    sb_strcpy(d->part, sizeof(d->part), "LU0 raw");
    d->tier = SB_TIER4;
    d->why = why;
    d->undo = "only a backup of the whole range (made first unless you pick \"no backup\"), then a restore";
}

int sb_write_decide(unsigned medium, unsigned lu, uint64_t lba, uint64_t n, struct sb_wdecide *d)
{
    decide_init(d);
    if (!n || lba + n < lba) {
        d->why = "empty or wrapping range";
        return SB_W_RANGE;
    }
    if (medium == SB_MED_SD) {
        sb_strcpy(d->part, sizeof(d->part), "microSD");
        if (in_live_store(SB_MED_SD, lba, n)) {
            d->tier = SB_TIER2;
            d->why = "the card's live OS store (header, directory, entries)";
            d->undo = "restore the store from a backup or repack it (tools/store)";
        } else {
            d->tier = SB_TIER1;
            d->why = "the microSD card";
            d->undo = "restore the card from a backup";
        }
        return 0;
    }
    if (medium != SB_MED_UFS) {
        d->why = "not a block medium";
        return SB_W_RANGE;
    }
    if (lu != 0) {
        sb_strcpy(d->part, sizeof(d->part), "boot LU");
        d->part[7] = (char)('0' + (lu & 7));
        d->part[8] = 0;
        d->tier = SB_TIER3;
        d->why = "a boot LU (S-Boot, CM, ECT): a bad write stops the phone booting";
        d->undo = "only Download mode with a signed BL, or this backup through a working Engineer OS";
        return 0;
    }
    if (gpt_load() <= 0) {
        raw_lu0(d, "no readable GPT: the target cannot be named (raw LU0 write)");
        return 0;
    }
    uint64_t lo = ~0ULL, hi = 0;
    for (int i = 0; i < gpt_n; ++i) {
        if (gpt_first[i] < lo)
            lo = gpt_first[i];
        if (gpt_last[i] > hi)
            hi = gpt_last[i];
    }
    if (lba < lo || lba + n - 1 > hi) {
        if (lba < lo && lba + n - 1 >= lo) {
            raw_lu0(d, "crosses from the partition table into a partition (raw LU0 write)");
            return 0;
        }
        sb_strcpy(d->part, sizeof(d->part), lba < lo ? "GPT/PIT head" : "GPT tail");
        d->tier = SB_TIER3;
        d->why = "the partition table: every OS and S-Boot read it";
        d->undo = "restore the GPT/PIT backup (Odin with the PIT, or this backup)";
        return 0;
    }
    for (int i = 0; i < gpt_n; ++i) {
        if (lba < gpt_first[i] || lba > gpt_last[i])
            continue;
        sb_strcpy(d->part, sizeof(d->part), gpt_name[i]);
        d->first = gpt_first[i];
        d->last = gpt_last[i];
        if (lba + n - 1 > gpt_last[i]) {
            raw_lu0(d, "crosses the end of the partition into the next (raw LU0 write)");
            return 0;
        }
        if (sb_never_part(gpt_name[i])) {
            d->tier = SB_TIER3;
            d->why = "irreplaceable per-phone data (IMEI, keys, calibration) or the boot chain";
            d->undo = "restore this phone's verified backup (never another phone's)";
            return 0;
        }
        if (tier2_part(gpt_name[i])) {
            d->tier = SB_TIER2;
            d->why = "boot chain or modem firmware";
            d->undo = "restore the backup, or flash the stock image with Odin";
            return 0;
        }
        if (tier1_part(gpt_name[i])) {
            if (in_live_store(SB_MED_UFS, lba, n)) {
                d->tier = SB_TIER2;
                d->why = "the live Starbite OS store (the boot menu reads it)";
                d->undo = "store-flash.sh from the Mac, or restore the store backup";
                return 0;
            }
            d->tier = SB_TIER1;
            d->why = "an OS partition";
            d->undo = "restore the backup or reinstall that OS";
            return 0;
        }
        d->tier = SB_TIER2;
        d->why = "a partition Starbite does not know";
        d->undo = "restore the backup";
        return 0;
    }
    raw_lu0(d, "between partitions (raw LU0 write)");
    return 0;
}

/* ---- authorisations -------------------------------------------------------------------------- */

#define AUTH_MAGIC 0x31485455414c4253ULL     /* "SBLAUTH1" */

static uint64_t epoch = 1, secret, nonce_ctr;
static struct sb_auth gate;
static int gate_open, fence_active, dry_run;

static uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h *= 0xbf58476d1ce4e5b9ULL;
    return h ^ (h >> 31);
}

static uint64_t seal_of(const struct sb_auth *a)
{
    if (!secret)
        secret = mix(sb_ticks() | 1U, 0x5354415242495445ULL);
    uint64_t h = mix(secret, a->magic);
    h = mix(h, a->space);
    h = mix(h, a->lu);
    h = mix(h, a->first);
    h = mix(h, a->count);
    h = mix(h, (uint64_t)(int64_t)a->tier);
    h = mix(h, a->steps);
    h = mix(h, a->kind);
    h = mix(h, a->epoch);
    return mix(h, a->nonce);
}

unsigned sb_steps_needed(int tier, unsigned kind)
{
    unsigned s = 0;
    if (tier >= SB_TIER1)
        s |= SB_STEP_WARN | SB_STEP_HELD;
    if (tier >= SB_TIER2)
        s |= SB_STEP_TYPED;
    if (tier >= SB_TIER3) {
        s |= SB_STEP_SECOND;
        if ((kind & SB_KIND_STORAGE) && tier == SB_TIER3)
            s |= SB_STEP_BACKUP;
    }
    if (tier >= SB_TIER4)
        s |= SB_STEP_PHRASE | SB_STEP_HOLD5;
    if (tier >= SB_TIER1 && (kind & SB_KIND_FREEFORM))
        s |= SB_STEP_ARMED;
    return s;
}

/* Every step the tier needs; tier 4 storage also a verified backup or the explicit "no backup". */
int sb_steps_ok(int tier, unsigned kind, unsigned steps)
{
    unsigned need = sb_steps_needed(tier, kind);
    if ((steps & need) != need)
        return 0;
    if (tier >= SB_TIER4 && (kind & SB_KIND_STORAGE) && !(steps & (SB_STEP_BACKUP | SB_STEP_WAIVE)))
        return 0;
    return 1;
}

void sb_auth_init(struct sb_auth *a, unsigned space, unsigned lu, uint64_t first, uint64_t count, int tier,
                  unsigned kind, const char *what)
{
    a->magic = AUTH_MAGIC;
    a->space = space;
    a->lu = lu;
    a->first = first;
    a->count = count;
    a->tier = tier;
    a->steps = 0;
    a->kind = kind;
    a->epoch = epoch;
    a->nonce = ++nonce_ctr;
    sb_strcpy(a->what, sizeof(a->what), what ? what : "");
    a->seal = seal_of(a);
}

void sb_auth_step(struct sb_auth *a, unsigned step)
{
    if (!sb_auth_valid(a))
        return;
    a->steps |= step;
    a->seal = seal_of(a);
}

int sb_auth_valid(const struct sb_auth *a)
{
    return a && a->magic == AUTH_MAGIC && a->epoch == epoch && a->tier >= SB_TIER1 && a->tier <= SB_TIER4 &&
           a->seal == seal_of(a);
}

int sb_auth_complete(const struct sb_auth *a)
{
    return sb_auth_valid(a) && sb_steps_ok(a->tier, a->kind, a->steps);
}

void sb_auth_revoke_all(void)
{
    ++epoch;
    gate_open = 0;
}

int sb_gate_open(const struct sb_auth *a)
{
    gate_open = 0;
    if (!sb_auth_complete(a))
        return 0;
    gate = *a;
    gate_open = 1;
    return 1;
}

void sb_gate_close(void)
{
    gate_open = 0;
}

int sb_gate_check(unsigned space, unsigned lu, uint64_t first, uint64_t count, int need, const char **why)
{
    const char *w = "allowed";
    int r = 0;
    if (need >= SB_TIER_NO) {
        w = "impossible target";
        r = SB_W_NO;
    } else if (need <= SB_TIER0) {
        r = 0;
    } else if (!gate_open || !sb_auth_complete(&gate)) {
        w = "no confirmed authorisation is open";
        r = SB_W_AUTH;
    } else if (gate.space != space || gate.lu != lu || first < gate.first || !count ||
               first + count < first || first + count > gate.first + gate.count) {
        w = "outside the range the user confirmed";
        r = SB_W_AUTH;
    } else if (gate.tier < need) {
        w = "the confirmation was for a lower tier";
        r = SB_W_TIER;
    } else if (!sb_steps_ok(need, gate.kind, gate.steps)) {
        unsigned miss = sb_steps_needed(need, gate.kind) & ~gate.steps;
        int nobak = (miss & SB_STEP_BACKUP) ||
                    (!miss && need >= SB_TIER4 && (gate.kind & SB_KIND_STORAGE));
        w = nobak ? "no verified backup yet" : "a confirmation step is missing";
        r = nobak ? SB_W_BACKUP : SB_W_AUTH;
    }
    if (why)
        *why = w;
    return r;
}

int sb_fence_active(void)
{
    return fence_active;
}

void sb_fence_dry_run(int on)
{
    dry_run = on;
}

int sb_fence_is_dry_run(void)
{
    return dry_run;
}

/* ---- the writes ---------------------------------------------------------------------------- */

static void log_write(const char *verb, unsigned medium, unsigned lu, uint64_t lba, uint64_t n,
                      const struct sb_wdecide *d, const char *tail)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "fence: ");
    ub_s(&u, verb);
    ub_c(&u, ' ');
    ub_dec(&u, n);
    ub_s(&u, " blk at ");
    ub_s(&u, medium == SB_MED_SD ? "sd:" : "ufs:");
    if (medium == SB_MED_UFS && lu) {
        ub_s(&u, "lu");
        ub_dec(&u, lu);
        ub_c(&u, ':');
    }
    ub_dec(&u, lba);
    ub_s(&u, " (");
    ub_s(&u, d->part[0] ? d->part : "?");
    ub_s(&u, ", tier ");
    ub_dec(&u, (uint64_t)d->tier);
    ub_s(&u, "): ");
    ub_s(&u, tail);
    sb_log(u.b);
}

static const char *hold_why;

void sb_storage_hold(const char *why)
{
    if (why && !hold_why) {
        struct ub u;                        /* sb_log only: the host unit tests stub it alone */
        ub_init(&u);
        ub_s(&u, "storage held: ");
        ub_s(&u, why);
        sb_log(u.b);
    } else if (!why && hold_why)
        sb_log("storage hold lifted");
    hold_why = why;
}

const char *sb_storage_held(void)
{
    return hold_why;
}

static int do_write(unsigned medium, unsigned lu, uint64_t lba, uint64_t n, const void *src)
{
    if (hold_why)
        return SB_W_HOLD;
    fence_active = 1;
    int r = sb_blk_write(medium, lu, lba, n, src);
    fence_active = 0;
    return r ? SB_W_IO : 0;
}

int sb_write(unsigned medium, unsigned lu, uint64_t lba, uint64_t n, const void *src)
{
    struct sb_wdecide d;
    const char *why = 0;
    int r = sb_write_decide(medium, lu, lba, n, &d);
    if (r) {
        log_write("refused", medium, lu, lba, n, &d, d.why);
        return r;
    }
    if (hold_why) {
        log_write("refused", medium, lu, lba, n, &d, hold_why);
        return SB_W_HOLD;
    }
    r = sb_gate_check(medium, lu, lba, n, d.tier, &why);
    if (r) {
        log_write("refused", medium, lu, lba, n, &d, why);
        return r;
    }
    if (!src) {
        log_write("refused", medium, lu, lba, n, &d, "no data");
        return SB_W_RANGE;
    }
    if (dry_run) {
        log_write("would write", medium, lu, lba, n, &d, "DRY RUN, nothing written");
        return SB_W_DRYRUN;
    }
    r = do_write(medium, lu, lba, n, src);
    log_write(r ? "FAILED" : "wrote", medium, lu, lba, n, &d, r ? "I/O error" : gate.what);
    return r;
}

/* store.c's writer: blocks of the store region on its medium (block 0 .. capacity), append-only by
 * contract (new data beyond the used end, a directory slot beyond the count, then the header as
 * the single commit; the settings block). It needs an open, complete authorisation whose kind has
 * SB_KIND_STORE: the user confirmed an action whose card named the store as the destination. */
int sb_write_store(unsigned medium, uint64_t lba, uint64_t n, const void *src)
{
    struct sb_wdecide d;
    decide_init(&d);
    sb_strcpy(d.part, sizeof(d.part), "OS store");
    d.tier = SB_TIER1;
    if (medium > SB_MED_SD || !n || !src || !live[medium].capacity || lba < live[medium].base ||
        lba + n > live[medium].base + live[medium].capacity) {
        log_write("refused", medium, 0, lba, n, &d, "outside the OS store");
        return SB_W_RANGE;
    }
    if (!gate_open || !sb_auth_complete(&gate) || !(gate.kind & SB_KIND_STORE)) {
        log_write("refused", medium, 0, lba, n, &d, "no confirmed authorisation names the store");
        return SB_W_AUTH;
    }
    if (hold_why) {
        log_write("refused", medium, 0, lba, n, &d, hold_why);
        return SB_W_HOLD;
    }
    if (dry_run) {
        log_write("would write", medium, 0, lba, n, &d, "DRY RUN, nothing written");
        return SB_W_DRYRUN;
    }
    int r = do_write(medium, 0, lba, n, src);
    log_write(r ? "FAILED" : "wrote", medium, 0, lba, n, &d, r ? "I/O error" : "store writer");
    return r;
}

/* ---- the standing write of one volatile store entry (docs/STARBITE-OS.md 6.5) ----------------------
 * Not the gate: no confirmation per write. The consent is the entry itself: made on the Mac
 * (sbstore2.py build --volatile NAME=BYTES) with the directory's volatile flag, like the loader's crash
 * log, which the loader writes in place the same way. store.c grants exactly that entry's blocks after
 * checking the flag; the grant must lie in the live store's data area (never its header, settings
 * block or directory: blocks 0..6), on the home store's medium (UFS, or the card once its store is home:
 * docs/STORE-ON-CARD.md), at most 4 MiB. Every write is logged, honours the dry run, and on UFS uses
 * forced unit access and ends with SYNCHRONIZE CACHE; on the card (no FUA) the driver returns once the
 * card has programmed the blocks and blkio.c reads them back. A reboot clears the grant. */
static struct {
    int on;
    unsigned medium;
    uint64_t first, count;
    char name[32];
} vg;

static void vg_clear(void)
{
    vg.on = 0;
}

int sb_volatile_grant(unsigned medium, uint64_t first, uint64_t count, const char *name)
{
    struct sb_wdecide d;
    decide_init(&d);
    sb_strcpy(d.part, sizeof(d.part), "volatile entry");
    d.tier = SB_TIER1;
    vg.on = 0;
    if (!count)
        return 0;
    if (medium > SB_MED_SD || count > 1024 || !live[medium].used || first < live[medium].base + 7 ||
        first + count < first || first + count > live[medium].base + live[medium].used) {
        log_write("refused a standing grant for", medium, 0, first, count, &d, "outside the store's data area");
        return 0;
    }
    vg.on = 1;
    vg.medium = medium;
    vg.first = first;
    vg.count = count;
    sb_strcpy(vg.name, sizeof(vg.name), name ? name : "?");
    log_write("standing grant for", medium, 0, first, count, &d, vg.name);
    return 1;
}

int sb_write_volatile(unsigned medium, uint64_t lba, uint64_t n, const void *src)
{
    struct sb_wdecide d;
    decide_init(&d);
    sb_strcpy(d.part, sizeof(d.part), vg.on ? vg.name : "volatile entry");
    d.tier = SB_TIER1;
    if (!vg.on || medium != vg.medium || !n || !src || lba < vg.first || lba + n < lba ||
        lba + n > vg.first + vg.count) {
        log_write("refused", medium, 0, lba, n, &d, vg.on ? "outside the granted entry" : "no standing grant");
        return vg.on ? SB_W_RANGE : SB_W_AUTH;
    }
    if (dry_run) {
        log_write("would write", medium, 0, lba, n, &d, "DRY RUN, nothing written");
        return SB_W_DRYRUN;
    }
    fence_active = 1;
    int r = sb_blk_write_sync(medium, 0, lba, n, src);
    fence_active = 0;
    log_write(r ? "FAILED" : "wrote", medium, 0, lba, n, &d,
              r ? "I/O error" : medium == SB_MED_SD ? "programmed + read back" : "FUA + cache flush");
    return r ? SB_W_IO : 0;
}

/* ---- the standing write of one file's extent on the microSD card (sb.h; Linux apps' overlay image) ----
 * Not the gate and not the volatile grant above (Starbite OS's state slot keeps that one). The consent is the
 * file: the user put /starbite/linux/overlay.img on the card's exFAT partition for exactly this. The caller
 * resolves the file to its extent; the fence does not take its word for where that is: it reads the MBR and the
 * exFAT boot sector and grants only a range wholly inside that partition's cluster heap. So whatever the caller
 * got wrong, a write can never land in the card's other partitions (p2: the OS store, the parked roots), the
 * partition table, the exFAT boot region or the FAT. Writes honour the storage hold and the dry run, go through
 * the card driver only (sb_blk_write on SB_MED_SD under fence_active) and are not logged one by one (a guest
 * writes thousands of blocks): refusals and I/O errors are. */
static struct {
    int on;
    uint64_t first, count;                  /* 4 KiB blocks of the card */
    uint32_t cid[4];
    char name[32];
} cg;
static uint64_t cg_sec[64];                 /* one sector (the driver writes 64-bit words) */
static unsigned cg_refused;

static uint32_t cg_le32(const volatile uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void sb_card_grant_clear(void)
{
    cg.on = 0;
}

int sb_card_grant(uint64_t first, uint64_t count, const char *name)
{
    struct sb_wdecide d;
    decide_init(&d);
    sb_strcpy(d.part, sizeof(d.part), "card file");
    d.tier = SB_TIER1;
    cg.on = 0;
    if (!count)
        return 0;
    const char *no = 0;
    const volatile uint8_t *b = (const volatile uint8_t *)cg_sec;
    uint64_t s0 = first * 8U, s1 = (first + count) * 8U, vol = 0, vlen = 0;
    if (first + count < first || first + count > (1ULL << 40)) {
        no = "a wrapping range";
    } else if (in_live_store(SB_MED_SD, first, count)) {
        no = "inside the card's OS store";
    } else if (sd_read(0, 1, (uintptr_t)cg_sec)) {
        no = "the card's partition table cannot be read";
    } else if (b[510] != 0x55 || b[511] != 0xaa) {
        no = "the card has no partition table";
    } else {
        int home = -1;
        for (int i = 0; i < 4 && !no; ++i) {
            const volatile uint8_t *e = b + 446 + 16 * i;
            uint64_t p0 = cg_le32(e + 8), p1 = p0 + cg_le32(e + 12);
            if (p1 == p0 || s0 >= p1 || s1 <= p0)
                continue;                   /* an empty entry, or one the range does not touch */
            if (e[4] != 0x07)
                no = "touches a partition that is not the exFAT one (the OS store's p2?)";
            else if (s0 < p0 || s1 > p1 || home >= 0)
                no = "crosses the exFAT partition's boundary";
            else {
                home = i;
                vol = p0;
                vlen = p1 - p0;
            }
        }
        if (!no && home < 0)
            no = "outside every partition";
    }
    if (!no) {
        /* the exFAT boot sector: the cluster heap is where files live (exFAT specification 3.1) */
        if (sd_read(vol, 1, (uintptr_t)cg_sec)) {
            no = "the exFAT boot sector cannot be read";
        } else if (b[3] != 'E' || b[4] != 'X' || b[5] != 'F' || b[6] != 'A' || b[7] != 'T' || b[510] != 0x55 ||
                   b[511] != 0xaa || b[108] != 9 || b[109] > 16) {
            no = "the partition is not exFAT with 512-byte sectors";
        } else {
            uint64_t heap0 = vol + cg_le32(b + 88), heap1 = heap0 + ((uint64_t)cg_le32(b + 92) << b[109]);
            if (heap0 <= vol || heap1 > vol + vlen || s0 < heap0 || s1 > heap1)
                no = "outside the exFAT cluster heap (the boot region or the FAT)";
        }
    }
    if (no) {
        log_write("refused a card-file grant for", SB_MED_SD, 0, first, count, &d, no);
        return 0;
    }
    cg.on = 1;
    cg.first = first;
    cg.count = count;
    for (unsigned i = 0; i < 4; ++i)
        cg.cid[i] = sd_rep.cid[i];
    sb_strcpy(cg.name, sizeof(cg.name), name ? name : "?");
    cg_refused = 0;
    log_write("card-file grant for", SB_MED_SD, 0, first, count, &d, cg.name);
    return 1;
}

int sb_write_card(uint64_t lba, uint64_t n, const void *src)
{
    struct sb_wdecide d;
    decide_init(&d);
    sb_strcpy(d.part, sizeof(d.part), cg.on ? cg.name : "card file");
    d.tier = SB_TIER1;
    const char *no = 0;
    int r = 0;
    if (!cg.on) {
        no = "no card-file grant";
        r = SB_W_AUTH;
    } else if (cg.cid[0] != sd_rep.cid[0] || cg.cid[1] != sd_rep.cid[1] || cg.cid[2] != sd_rep.cid[2] ||
               cg.cid[3] != sd_rep.cid[3]) {
        cg.on = 0;                          /* the grant was for the card that was in the slot then */
        no = "another card is in the slot: the grant is void";
        r = SB_W_AUTH;
    } else if (!n || !src || lba < cg.first || lba + n < lba || lba + n > cg.first + cg.count) {
        no = "outside the granted file";
        r = SB_W_RANGE;
    } else if (hold_why) {
        no = hold_why;
        r = SB_W_HOLD;
    } else if (dry_run) {
        no = "DRY RUN, nothing written";
        r = SB_W_DRYRUN;
    }
    if (!r) {
        r = do_write(SB_MED_SD, 0, lba, n, src);
        if (r)
            no = "I/O error";
    }
    if (r && cg_refused < 8) {              /* a guest can ask thousands of times: the first few are enough */
        ++cg_refused;
        log_write(r == SB_W_DRYRUN ? "would write" : r == SB_W_IO ? "FAILED" : "refused", SB_MED_SD, 0, lba, n, &d, no);
    }
    return r;
}
