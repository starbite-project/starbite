/* SPDX-License-Identifier: GPL-2.0-only
 * Track A: Software Update, Starbite OS's half (docs/STARBITE-FIRMWARE-LAYER.md 10.5; the loader's half is Track J's).
 *
 *   - the card: /STARBITE/UPDATE/MANIFEST.TXT (lines key=, version=, image=, size=, sha256=, notes=) names an image in
 *     the same folder; only an image for this start's catalog row (sb_fw.key, the loader's SBFW table) is offered;
 *   - staging: into the update slot that is not active ("sbos-slot-a" = 1, "sbos-slot-b" = 2: volatile home-store
 *     entries, 64 MiB each), a chunk per call so the UI stays live: read from the card, SHA-256 and CRC32 over the
 *     bytes, written at +4096 through a grant for exactly that window (libsb's store_volatile_grant_part: the fence
 *     takes at most 4 MiB). The SHA-256 must match the manifest before anything else happens; then the header
 *     (sbslot_build) is written LAST, read back and checked, and the image is read back once for its CRC32;
 *   - install: sbfw_update_arm (slot_try, the one-shot SBFW_REQ_UPDATE for this row), then power_boot_os (saves first);
 *   - the next start: when the loader says it is the trial (the table's req_action) and the home screen has been up
 *     10 s, sbfw_update_commit makes the slot the active one: "Updated to <version>". Without the commit the loader's
 *     next start rolls back (REFUSED): the OS says "The update didn't start; the previous version was restored" once.
 * The fence has ONE standing grant, the state slot's (slot.c): every window taken here is lent (hw_slot_lend) and
 * slot.c takes it back at its next write. The settings record is written only with the store gate open for that one
 * write (the user's tap on Download and Install / Restart; the commit finishes that same install).
 * In a self-test image (sbos_selftest_enabled) nothing is read from the card and every write and restart is a
 * logged dry run. The host test kern/host/update-test.c builds this file with fakes around it. */
#include "sb.h"
#include "kern.h"
#include "store.h"
#include "sbfw_api.h"
#include "passcode.h"

#define UPD_DIR "/STARBITE/UPDATE"
#define UPD_MAN UPD_DIR "/MANIFEST.TXT"
#define UPD_PIECE (64U << 10)                   /* the card's reads (kfile_read_at) */
#define UPD_PREF "sbos.update.seen"             /* the req_seq of the last rollback the user was told about */

static struct {
    struct kupdate m;                           /* the manifest and where it stands (kern.h) */
    unsigned slot;                              /* the slot being staged: 1 A, 2 B */
    struct store_ent e;                         /* its store entry */
    unsigned med;                               /* SB_MED_* of the entry */
    int phase;                                  /* 0 idle, 1 copy, 2 header, 3 read back, 4 ready */
    uint64_t off;                               /* bytes copied (phase 1) or read back (phase 3) */
    struct pass_sha sha;
    uint32_t crc, vcrc;
    int dry_said;
    int committed;                              /* this start committed an update */
    char cver[32];
} U;
static int force_dry;

void kupdate_dry(int on) { force_dry = on; }
static int dry(void) { return force_dry || sbos_selftest_enabled; }

static const char *slot_name(unsigned s) { return s == 2 ? "sbos-slot-b" : "sbos-slot-a"; }

static void fail(struct kupdate *u, const char *why)
{
    u->state = KUP_FAILED;
    sb_strcpy(u->why, sizeof(u->why), why);
    U.phase = 0;
    sb_logf("update: ", why);
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int same_ci(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) a++, b++;
    return !*a && !*b;
}
static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* ---- the manifest (pure: the host test feeds it text) ----------------------------------------------------------- */
int kupdate_parse(const char *t, int n, struct kupdate *u)
{
    uint8_t *z = (uint8_t *)u;
    for (unsigned i = 0; i < sizeof(*u); i++) z[i] = 0;
    int have = 0;                               /* bits: key, version, image, size, sha256 */
    int i = 0;
    while (i < n) {
        int a = i;
        while (i < n && t[i] != '\n') i++;
        int b = i++;
        if (b > a && t[b - 1] == '\r') b--;
        while (a < b && (t[a] == ' ' || t[a] == '\t')) a++;
        if (a >= b || t[a] == '#') continue;
        int eq = a;
        while (eq < b && t[eq] != '=') eq++;
        if (eq >= b) continue;
        int ke = eq;
        while (ke > a && (t[ke - 1] == ' ' || t[ke - 1] == '\t')) ke--;
        int v = eq + 1;
        while (v < b && (t[v] == ' ' || t[v] == '\t')) v++;
        int ve = b;
        while (ve > v && (t[ve - 1] == ' ' || t[ve - 1] == '\t')) ve--;
        char k[12], val[164];
        int kn = ke - a, vn = ve - v;
        if (kn <= 0 || kn >= (int)sizeof(k)) continue;
        for (int j = 0; j < kn; j++) k[j] = (char)lower(t[a + j]);
        k[kn] = 0;
        if (vn >= (int)sizeof(val)) vn = (int)sizeof(val) - 1;
        for (int j = 0; j < vn; j++) val[j] = t[v + j];
        val[vn] = 0;
        if (sb_streq(k, "key")) {
            if (vn >= (int)sizeof(u->key)) return -1;
            sb_strcpy(u->key, sizeof(u->key), val);
            have |= 1;
        } else if (sb_streq(k, "version")) {
            sb_strcpy(u->version, sizeof(u->version), val);
            have |= 2;
        } else if (sb_streq(k, "image")) {
            if (vn >= (int)sizeof(u->image) || !vn) return -1;
            for (int j = 0; j < vn; j++)
                if (val[j] == '/' || val[j] == '\\' || (val[j] == '.' && j + 1 < vn && val[j + 1] == '.')) return -1;
            sb_strcpy(u->image, sizeof(u->image), val);
            have |= 4;
        } else if (sb_streq(k, "size")) {
            uint64_t s = 0;
            if (!vn) return -1;
            for (int j = 0; j < vn; j++) {
                if (val[j] < '0' || val[j] > '9' || s > 0xffffffffffULL) return -1;
                s = s * 10 + (uint64_t)(val[j] - '0');
            }
            u->size = s;
            have |= 8;
        } else if (sb_streq(k, "sha256")) {
            if (vn != 64) return -1;
            for (int j = 0; j < 32; j++) {
                int h = hexval(val[2 * j]), l = hexval(val[2 * j + 1]);
                if (h < 0 || l < 0) return -1;
                u->sha[j] = (uint8_t)(h << 4 | l);
            }
            have |= 16;
        } else if (sb_streq(k, "notes"))
            sb_strcpy(u->notes, sizeof(u->notes), val);
    }
    return have == 31 && u->size && u->key[0] && u->version[0] ? 0 : -1;
}

/* the slot that is not active (0 = the row's normal file: A) */
unsigned kupdate_target(unsigned active) { return active == 1 ? 2U : 1U; }

static int slot_entry(unsigned s, struct store_ent *e)
{
    char nm[24] = "home:";
    sb_strcpy(nm + 5, sizeof(nm) - 5, slot_name(s));
    return store_find(nm, e) && (e->flags & 1U) && e->size > 4096U && !(e->size & 4095U);
}

/* the version the active slot holds, if its header is good ("" none) */
static void active_version(unsigned active, char *v, unsigned cap)
{
    static uint8_t hb[4096];
    struct store_ent e;
    struct sbslot_info si;
    v[0] = 0;
    if ((active == 1 || active == 2) && slot_entry(active, &e) && !store_read_block(&e, 0, hb) &&
        sbslot_check(hb, e.size, sb_fw.key, &si))
        sb_strcpy(v, cap, si.vstr);
}

/* ---- check ------------------------------------------------------------------------------------------------------- */
int kupdate_check(struct kupdate *out)
{
    if (!U.committed && (U.m.state == KUP_STAGING || U.m.state == KUP_READY)) {   /* under way, or staged: kept */
        *out = U.m;
        return 1;
    }
    struct kupdate *u = &U.m;
    uint8_t *z = (uint8_t *)u;
    for (unsigned i = 0; i < sizeof(*u); i++) z[i] = 0;
    U.phase = 0;
    if (U.committed) {
        u->state = KUP_INSTALLED;
        sb_strcpy(u->version, sizeof(u->version), U.cver);
        sb_strcpy(u->why, sizeof(u->why), "Installed. Starbite OS is up to date.");
        *out = *u;
        return 0;
    }
    if (!sb_fw.valid || !sb_fw.key[0]) {
        fail(u, "Updates need the new boot loader.");
        *out = *u;
        return 0;
    }
    if (dry()) {
        sb_strcpy(u->why, sizeof(u->why), "The self test reads no card.");
        *out = *u;
        return 0;
    }
    kfile_mount();
    const char *why = "";
    int cs = kfile_state(&why);
    if (cs <= 0) {
        sb_strcpy(u->why, sizeof(u->why), cs == -1 || cs == 0 ? "No microSD card." : why);
        *out = *u;
        return 0;
    }
    static char txt[2048];
    int n = kfile_read_at(UPD_MAN, 0, txt, sizeof(txt) - 1);
    if (n <= 0) {
        sb_strcpy(u->why, sizeof(u->why), "No update on the card.");
        *out = *u;
        return 0;
    }
    if (kupdate_parse(txt, n, u)) {
        fail(u, "The update's manifest is incomplete or damaged.");
        *out = *u;
        return 0;
    }
    if (!sb_streq(u->key, sb_fw.key)) {         /* another OS's update: not offered */
        struct ub b;
        ub_init(&b);
        ub_s(&b, "update: the card's update is for ");
        ub_s(&b, u->key);
        ub_s(&b, ", this is ");
        ub_s(&b, sb_fw.key);
        sb_log(b.b);
        u->state = KUP_NONE;
        sb_strcpy(u->why, sizeof(u->why), "The update on the card is for another system.");
        *out = *u;
        return 0;
    }
    /* the image file and its size (the folder's listing: the manifest's size must be the file's) */
    static struct kfile_ent le[16];
    int found = 0, first = 0;
    for (;;) {
        int k = kfile_list_at(UPD_DIR, le, 16, first);
        if (k <= 0) break;
        for (int i = 0; i < k && !found; i++)
            if (!le[i].dir && same_ci(le[i].name, u->image)) {
                found = 1;
                if (le[i].size != u->size) {
                    fail(u, "The update file's size doesn't match its manifest.");
                    *out = *u;
                    return 0;
                }
            }
        if (found || k < 16) break;
        first += k;
    }
    if (!found) {
        fail(u, "The update file named in the manifest is missing.");
        *out = *u;
        return 0;
    }
    unsigned act = 0, tr = 0, tries = 0;
    sbfw_slot_get(&act, &tr, &tries);
    unsigned s = kupdate_target(act);
    struct store_ent e;
    if (!slot_entry(s, &e)) {
        fail(u, "This phone has no update slots (sbos-slot-a / sbos-slot-b).");
        *out = *u;
        return 0;
    }
    if (u->size > e.size - 4096U) {
        fail(u, "The update is larger than the update slot.");
        *out = *u;
        return 0;
    }
    char cur[32];
    active_version(act, cur, sizeof(cur));
    if (cur[0] && sb_streq(cur, u->version)) {
        u->state = KUP_NONE;
        sb_strcpy(u->why, sizeof(u->why), "This version is installed.");
        *out = *u;
        return 0;
    }
    U.slot = s;
    U.e = e;
    U.med = e.medium == STORE_SD ? SB_MED_SD : SB_MED_UFS;
    u->state = KUP_FOUND;
    sb_logf("update: found ", u->version);
    *out = *u;
    return 1;
}

/* ---- staging: one step a call ------------------------------------------------------------------------------------- */
static int wr(uint64_t first, uint64_t blocks, const void *src)
{
    if (dry()) {
        if (!U.dry_said) {
            U.dry_said = 1;
            sb_logf("update: would write the image and its header into ", slot_name(U.slot));
        }
        return 0;
    }
    struct store_ent e;
    if (!store_volatile_grant_part(slot_name(U.slot), U.m.size + 4096U, first, blocks, &e) || e.lba != U.e.lba)
        return -1;
    hw_slot_lend();
    int r = sb_write_volatile(U.med, U.e.lba + first, blocks, src);
    return r == 0 || r == SB_W_DRYRUN ? 0 : -1;
}

int kupdate_stage(struct kupdate *out, uint8_t *buf, uint32_t cap)
{
    struct kupdate *u = &U.m;
    cap &= ~4095U;
    if (cap > (4U << 20)) cap = 4U << 20;      /* the fence's largest grant */
    if (u->state == KUP_FOUND) {
        u->state = KUP_STAGING;
        u->pct = 0;
        U.phase = 1;
        U.off = 0;
        U.crc = U.vcrc = 0;
        U.dry_said = 0;
        pass_sha_init(&U.sha);
        sb_logf("update: staging into ", slot_name(U.slot));
    }
    if (u->state != KUP_STAGING || !buf || cap < UPD_PIECE) {
        if (u->state == KUP_STAGING) fail(u, "No memory to copy the update.");
        *out = *u;
        return u->state;
    }
    struct ub b;
    if (U.phase == 1) {                         /* copy a chunk: card -> SHA-256, CRC32 -> the slot at +4096 */
        uint64_t left = u->size - U.off;
        uint32_t n = left < cap ? (uint32_t)left : cap, got = 0;
        char path[96];
        ub_init(&b);
        ub_s(&b, UPD_DIR "/");
        ub_s(&b, u->image);
        sb_strcpy(path, sizeof(path), b.b);
        while (got < n) {
            uint32_t want = n - got < UPD_PIECE ? n - got : UPD_PIECE;
            int r = dry() ? (int)want : kfile_read_at(path, U.off + got, buf + got, want);
            if (r != (int)want) {
                fail(u, "The card could not be read. Nothing was installed.");
                *out = *u;
                return u->state;
            }
            got += want;
        }
        uint32_t blocks = (n + 4095U) / 4096U;
        for (uint32_t i = n; i < blocks * 4096U; i++) buf[i] = 0;
        pass_sha_update(&U.sha, buf, n);
        U.crc = sb_crc32_more(U.crc, buf, n);
        if (wr(1 + U.off / 4096U, blocks, buf)) {
            fail(u, "The update slot could not be written. Nothing was installed.");
            *out = *u;
            return u->state;
        }
        U.off += n;
        u->pct = (int)(U.off * 90U / u->size);
        if (U.off >= u->size) {
            uint8_t h[32];
            pass_sha_final(&U.sha, h);
            int ok = 1;
            for (int i = 0; i < 32; i++) ok &= h[i] == u->sha[i];
            if (!ok && !dry()) {
                fail(u, "The update file is damaged (its SHA-256 doesn't match). Nothing was installed.");
                *out = *u;
                return u->state;
            }
            U.phase = 2;
        }
    } else if (U.phase == 2) {                  /* the header, last; read back and checked */
        struct sbslot_info si;
        uint8_t *z = (uint8_t *)&si;
        for (unsigned i = 0; i < sizeof(si); i++) z[i] = 0;
        si.version = 1;
        si.size = u->size;
        si.crc = U.crc;
        sb_strcpy(si.vstr, sizeof(si.vstr), u->version);
        sb_strcpy(si.key, sizeof(si.key), u->key);
        sbslot_build(buf, &si);
        if (wr(0, 1, buf)) {
            fail(u, "The update slot's header could not be written. Nothing was installed.");
            *out = *u;
            return u->state;
        }
        if (!dry()) {
            struct sbslot_info g;
            if (sb_blk_read(U.med, 0, U.e.lba, 1, buf) || !sbslot_check(buf, U.e.size, sb_fw.key, &g) ||
                g.size != u->size || g.crc != U.crc || !sb_streq(g.vstr, u->version)) {
                fail(u, "The update slot's header did not read back. Nothing was installed.");
                *out = *u;
                return u->state;
            }
        }
        U.phase = dry() ? 4 : 3;
        U.off = 0;
    } else if (U.phase == 3) {                  /* the image read back once: its CRC32 as written */
        uint64_t left = u->size - U.off;
        uint32_t n = left < cap ? (uint32_t)left : cap, blocks = (n + 4095U) / 4096U;
        if (sb_blk_read(U.med, 0, U.e.lba + 1 + U.off / 4096U, blocks, buf)) {
            fail(u, "The update slot could not be read back. Nothing was installed.");
            *out = *u;
            return u->state;
        }
        U.vcrc = sb_crc32_more(U.vcrc, buf, n);
        U.off += n;
        u->pct = 90 + (int)(U.off * 10U / u->size);
        if (U.off >= u->size) {
            if (U.vcrc != U.crc) {
                fail(u, "The update slot read back different. Nothing was installed.");
                *out = *u;
                return u->state;
            }
            U.phase = 4;
        }
    }
    if (U.phase == 4) {
        u->state = KUP_READY;
        u->pct = 100;
        ub_init(&b);
        ub_s(&b, "update: staged ");
        ub_s(&b, u->version);
        ub_s(&b, " in ");
        ub_s(&b, slot_name(U.slot));
        ub_s(&b, ", CRC32 ");
        ub_hex(&b, U.crc, 8);
        sb_log(b.b);
    }
    *out = *u;
    return u->state;
}

/* ---- the settings record: one write under the store gate -------------------------------------------------------- */
static int gate(const char *what)
{
    int h = store_home();
    if (h < 0) return SBFW_E_NOHOME;
    struct sb_auth a;
    sb_auth_init(&a, h == (int)STORE_SD ? SB_MED_SD : SB_MED_UFS, 0, 0, 0, SB_TIER1, SB_KIND_STORE, what);
    sb_auth_step(&a, sb_steps_needed(SB_TIER1, SB_KIND_STORE));   /* the user's own tap (Install / Restart) */
    return sb_gate_open(&a) ? 0 : SB_W_AUTH;
}

int kupdate_install(char *why, unsigned cap)
{
    struct kupdate *u = &U.m;
    if (u->state != KUP_READY) {
        sb_strcpy(why, cap, "No update is ready to install.");
        return -1;
    }
    if (dry()) {
        sb_logf("update: would arm the update and restart into ", sb_fw.key);
        sb_strcpy(why, cap, "A dry run: the self test.");
        return 0;
    }
    static uint8_t hb[4096];
    struct sbslot_info si;
    if (store_read_block(&U.e, 0, hb) || !sbslot_check(hb, U.e.size, sb_fw.key, &si) || si.crc != U.crc) {
        fail(u, "The staged update is no longer there. Try again.");
        sb_strcpy(why, cap, u->why);
        return -1;
    }
    int e = gate("update: arm the staged image");
    if (!e) {
        e = sbfw_update_arm(sb_fw.key, U.slot);
        sb_gate_close();
    }
    if (e) {
        struct ub b;
        ub_init(&b);
        ub_s(&b, "update: not armed (");
        ub_sdec(&b, e);
        ub_s(&b, ")");
        sb_log(b.b);
        sb_strcpy(why, cap, "The boot loader's settings could not be written.");
        return -1;
    }
    sb_logf("update: armed; restarting into ", sb_fw.key);
    power_boot_os(sb_fw.key);                   /* saves first; does not return */
    sb_strcpy(why, cap, "The phone did not restart.");
    return -1;
}

/* ---- the start after: commit, or say the rollback ---------------------------------------------------------------- */
int kupdate_after_start(void)
{
    if (!sb_fw.valid) return 0;
    unsigned act = 0, st = 0, a = 0, tr = 0, tries = 0;
    char key[24];
    int seq = sbfw_request_status(&a, &st, key);
    sbfw_slot_get(&act, &tr, &tries);
    if (sb_fw.req_action == SBFW_REQ_UPDATE && a == SBFW_REQ_UPDATE && st == SBFW_ST_APPLIED && tr) {
        char v[32];
        active_version(tr, v, sizeof(v));
        if (dry()) {
            sb_logf("update: would commit the update (a dry run: the self test): ", v);
            return 0;
        }
        int e = gate("update: commit the new version");
        if (!e) {
            e = sbfw_update_commit();
            sb_gate_close();
        }
        if (e) {
            struct ub b;
            ub_init(&b);
            ub_s(&b, "update: the commit was not written (");
            ub_sdec(&b, e);
            ub_s(&b, "): the loader rolls back at its next start");
            sb_log(b.b);
            return -1;
        }
        U.committed = 1;
        sb_strcpy(U.cver, sizeof(U.cver), v[0] ? v : "the new version");
        struct ub b;
        ub_init(&b);
        ub_s(&b, "Updated to ");
        ub_s(&b, U.cver);
        ub_s(&b, ".");
        notif_post("Starbite OS", "Software Update", b.b);
        sb_logf("update: committed ", U.cver);
        return 1;
    }
    if (a == SBFW_REQ_UPDATE && st == SBFW_ST_REFUSED && seq > 0) {
        char seen[12] = "", now[12];
        struct ub b;
        ub_init(&b);
        ub_dec(&b, (uint64_t)seq);
        sb_strcpy(now, sizeof(now), b.b);
        if (prefs_get(UPD_PREF, seen, sizeof(seen)) > 0 && sb_streq(seen, now)) return 0;   /* said already */
        if (dry()) {
            sb_log("update: the last update was rolled back (a dry run: no notice)");
            return 0;
        }
        notif_post("Starbite OS", "Software Update", "The update didn't start; the previous version was restored.");
        if (prefs_set(UPD_PREF, now) > 0) settings_changed();
        sb_log("update: the last update was rolled back by the loader");
        return 2;
    }
    return 0;
}

int kupdate_committed(void) { return U.committed; }

void kupdate_reset(void)
{
    uint8_t *z = (uint8_t *)&U;
    for (unsigned i = 0; i < sizeof(U); i++) z[i] = 0;
}
