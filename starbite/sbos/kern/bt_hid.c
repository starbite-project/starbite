/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: boot-protocol keyboard reports into key events and characters (US layout). Sources: bt_hid.h. Pure C;
 * host-tested in kern/host/bthid-test.c; not seen on a phone yet. */
#include "bt_hid.h"

/* usages 0x1E..0x38: unshifted, shifted */
static const char row_n[] = "1234567890\n\x1b\x08\t -=[]\\#;'`,./";
static const char row_s[] = "!@#$%^&*()\n\x1b\x08\t _+{}|~:\"~<>?";
static const char pad[] = "/*-+\n1234567890.";   /* 0x54..0x63 */

uint32_t bthid_char(uint8_t u, uint8_t mods, int caps)
{
    int shift = (mods & 0x22) != 0;
    if (mods & 0x99)                        /* Ctrl or GUI: a shortcut, not a character */
        return 0;
    if (u >= 0x04 && u <= 0x1D) {
        int up = shift ^ (caps != 0);
        return (uint32_t)((up ? 'A' : 'a') + (u - 0x04));
    }
    if (u >= 0x1E && u <= 0x38)
        return (uint32_t)(uint8_t)(shift ? row_s : row_n)[u - 0x1E];
    if (u == HID_KEY_DELETE)
        return 0x7F;
    if (u >= 0x54 && u <= 0x63)
        return (uint32_t)(uint8_t)pad[u - 0x54];
    return 0;
}

void bthid_init(struct bthid_kbd *k)
{
    void (*cb)(int, int, uint32_t) = k->cb;
    uint8_t *p = (uint8_t *)k;
    for (unsigned i = 0; i < sizeof(*k); i++)
        p[i] = 0;
    k->cb = cb;
}

static void tell(struct bthid_kbd *k, int usage, int down, uint32_t uc)
{
    if (k->cb)
        k->cb(usage, down, uc);
}

void bthid_release(struct bthid_kbd *k)
{
    for (unsigned i = 0; i < 6; i++)
        if (k->keys[i] >= 4)
            tell(k, k->keys[i], 0, 0);
    for (unsigned b = 0; b < 8; b++)
        if (k->mods & (1U << b))
            tell(k, 0xE0 + (int)b, 0, 0);
    for (unsigned i = 0; i < 6; i++)
        k->keys[i] = 0;
    k->mods = 0;
}

static int has(const uint8_t *keys, uint8_t u)
{
    for (unsigned i = 0; i < 6; i++)
        if (keys[i] == u)
            return 1;
    return 0;
}

int bthid_frame(struct bthid_kbd *k, const uint8_t *p, unsigned n)
{
    if (n < 1) {
        k->malformed++;
        return -1;
    }
    if (p[0] != HIDP_DATA_INPUT) {
        k->ignored++;
        return 0;
    }
    const uint8_t *r = p + 1;
    unsigned rn = n - 1;
    if (rn >= 9 && r[0] == 0x01)
        r++;                                /* report ID 1: the boot keyboard (longer ones: the first 8 bytes) */
    else if (rn == 8)
        ;                                   /* no report ID */
    else {
        if (rn && r[0] != 0x01)
            k->ignored++;                   /* another report (consumer keys, a mouse) */
        else
            k->malformed++;
        return rn && r[0] != 0x01 ? 0 : -1;
    }
    k->reports++;
    int roll = 1;
    for (unsigned i = 2; i < 8; i++)
        if (r[i] != 0x01)
            roll = 0;
    if (roll) {                             /* ErrorRollOver: too many keys; keep the last good state */
        k->rollover++;
        return 1;
    }
    uint8_t mods = r[0], now[6];
    for (unsigned i = 0; i < 6; i++)
        now[i] = r[2 + i] >= 4 ? r[2 + i] : 0;
    for (unsigned i = 0; i < 6; i++)        /* ups first */
        if (k->keys[i] && !has(now, k->keys[i]))
            tell(k, k->keys[i], 0, 0);
    for (unsigned b = 0; b < 8; b++)
        if ((k->mods ^ mods) & (1U << b))
            tell(k, 0xE0 + (int)b, (mods >> b) & 1, 0);
    k->mods = mods;
    for (unsigned i = 0; i < 6; i++) {
        uint8_t u = now[i];
        if (!u || has(k->keys, u))
            continue;
        if (u == HID_KEY_CAPS) {
            k->caps = !k->caps;
            k->leds = (uint8_t)((k->leds & ~2U) | (k->caps ? 2U : 0U));
            k->leds_changed = 1;
        }
        k->downs++;
        tell(k, u, 1, bthid_char(u, mods, k->caps));
    }
    for (unsigned i = 0; i < 6; i++)
        k->keys[i] = now[i];
    return 1;
}
