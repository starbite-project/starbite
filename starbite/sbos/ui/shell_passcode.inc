/* SPDX-License-Identifier: MIT
 * Track A (sbos-core-api.md 3 and 6): the passcode screen, Settings > Face ID & Passcode (with Auto-Lock), and the
 * app lifecycle's lock side (the open app comes back after the unlock). A draft in plain drawing: the UI track
 * restyles it. Included from shell_lock.c after the other .inc files.
 *
 * The screen (iOS's): a lock glyph, "Enter Passcode", 4 or 6 dots, a 3 x 4 keypad of glass circles (digits with
 * their letters), "Emergency" bottom-left and "Cancel" / "Delete" bottom-right. A wrong code shakes the dots (and a
 * tap of the motor); during a back-off the keypad goes and the screen says how long to wait. It shows:
 *   - on the lock's swipe up, when sh_pass_on(): the lock flies off under it; the right code goes to home, or to the
 *     app that was open when the phone locked; Cancel brings the lock back;
 *   - on a notification opened from the lock (shell_nc.inc's nc_launch): the right code opens it;
 *   - in Settings > Face ID & Passcode: Turn Passcode On (a new code twice; 6 digits, or 4 under the dots' option),
 *     Change Passcode (the old one first), Turn Passcode Off (the code).
 * Never behind it (the safety rule): Emergency mode and its cards (SOS, the medical card, ...), the torch. Its
 * Emergency button turns Emergency mode on over the lock without the code. The digits live in PC.d only while typed
 * (wiped after each try) and go to the platform's sh_pass_* only: nothing here logs or draws them. */

enum { PCP_UNLOCK, PCP_LAUNCH, PCP_SET, PCP_CHANGE, PCP_OFF };          /* what the right code does */
enum { PCM_ENTER, PCM_NEW, PCM_AGAIN, PCM_WAIT };                        /* what the screen asks for */
static struct {
    int on, built, purpose, mode, n, want, setlen, key, hidhome, reorder, resume, nc, shown_min, err;
    unsigned nc_id;
    float a, at, sh, ev;            /* the screen's opacity, its target; the shake (0..1, < 0 none); the check's delay */
    char d[8], first[8], old[8];
    char msg[64];
} PC = { .resume = -1, .sh = -1, .key = -1 };
static tk_layer pcl, pcbg, pchead, pcdots, pckeys, pcbot, pchl;
static tk_bmp pckey;                /* one key's glass circle, baked once a show, copied ten times */

#define PC_KY 318.0f                /* the keypad's first row (its circles' tops) */
#define PC_KP 94.0f                 /* the rows' pitch */
#define PC_KD 78.0f                 /* a key's diameter */
#define PC_DOT_Y 226.0f             /* the dots' centres */
#define PC_BOT 735.0f               /* the bottom words' baseline */
static const char *const PCL[10] = { "", "", "ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ" };   /* digit k's */

static void pc_wipe(char *p, int n) { for (int i = 0; i < n; i++) ((volatile char *)p)[i] = 0; }
static float pc_kx(int k) { return 195 + 100.0f * ((k == 0 ? 1 : (k - 1) % 3) - 1) - PC_KD / 2; }   /* key k's left */
static float pc_ky(int k) { return PC_KY + PC_KP * (k == 0 ? 3 : (k - 1) / 3); }                    /* and top */

/* ---- drawing ---------------------------------------------------------------------------------------------------- */
static const char *pc_title(void)
{
    if (PC.mode == PCM_WAIT) return "Starbite OS Unavailable";
    if (PC.mode == PCM_NEW) return PC.purpose == PCP_CHANGE ? "Enter your new passcode" : "Enter a passcode";
    if (PC.mode == PCM_AGAIN) return PC.purpose == PCP_CHANGE ? "Re-enter your new passcode" : "Re-enter your passcode";
    if (PC.purpose == PCP_CHANGE) return "Enter your old passcode";
    return "Enter Passcode";
}
static void pc_draw_head(void)
{
    relayer(&pchead);
    dc d = mk(&pchead, "pc-head", 20, 96, 350, 196, 0, 0);
    if (!d.b->px) return;
    d_icon(&d, "lock-c", 195 - 7.5f, 104, .9f);
    d_text_box(&d, TK_IT500, 20, 195, 172, 340, 1, pc_title(), 0, 0, INK, 0);
    char line[64];
    line[0] = 0;
    if (PC.mode == PCM_WAIT) {
        int w = sh_pass_wait(), m = (w + 59) / 60;
        if (m < 1) m = 1;
        PC.shown_min = m;
        int k = 0;
        const char *a = "Try again in ";
        while (*a) line[k++] = *a++;
        if (m >= 60 && m % 60 == 0) { k += put_dec(line + k, m / 60); a = m == 60 ? " hour" : " hours"; }
        else { k += put_dec(line + k, m); a = m == 1 ? " minute" : " minutes"; }
        while (*a) line[k++] = *a++;
        line[k] = 0;
        d_text_box(&d, TK_IT400, 16, 195, 214, 340, 1, line, 0, 0, W(.85f), 0);
    } else if (PC.msg[0]) {
        d_text_box(&d, TK_IT400, 14, 195, 270, 340, 1, PC.msg, 0, 0, PC.err ? tk_hex(0xff6961, 1) : W(.75f), 0);
    } else if (PC.mode == PCM_NEW) {
        d_text_box(&d, TK_IT500, 14, 195, 270, 340, 1, PC.setlen == 6 ? "Use a 4-Digit Code" : "Use a 6-Digit Code", 0, 0, ACCENT(), 0);
    }
    trim(&pchead);                                 /* what it holds only (the heap) */
    tk_layer_prepare(&pchead);
}
static void pc_draw_dots(void)
{
    relayer(&pcdots);
    pcdots.hidden = PC.mode == PCM_WAIT;
    if (pcdots.hidden) return;
    dc d = mk(&pcdots, "pc-dots", 100, PC_DOT_Y - 12, 190, 24, 4, 0);
    if (!d.b->px) return;
    float x0 = 195 - 13.0f * (PC.want - 1);
    for (int i = 0; i < PC.want; i++) {
        float cx = x0 + 26.0f * i;
        if (i < PC.n) d_rrect(&d, cx - 6.5f, PC_DOT_Y - 6.5f, 13, 13, 6.5f, W(1));
        else d_ring(&d, cx, PC_DOT_Y, 5.9f, 1.2f, 1, W(.9f), 0);
    }
    tk_layer_prepare(&pcdots);
}
static void pc_draw_keys(void)
{
    relayer(&pckeys);
    pckeys.hidden = PC.mode == PCM_WAIT;
    if (pckeys.hidden) return;
    dc d = mk(&pckeys, "pc-keys", 56 - 20, PC_KY - 20, 278 + 40, 3 * PC_KP + PC_KD + 40, 0, 0);
    if (!d.b->px) return;
    /* one circle baked (the flat ground's glass, as the back button's: the scrim is a flat colour), then copied */
    if (!pckey.px) {
        float mg = 14;
        if (tk_bmp_new(&pckey, (int)PT(PC_KD + 2 * mg) + 2, (int)PT(PC_KD + 2 * mg) + 2)) {
            dc g = { &pckey, 0, tk_floorf(PT(pc_kx(1) - mg)), tk_floorf(PT(pc_ky(1) - mg)) };
            d_lg_flat(&g, pc_kx(1), pc_ky(1), PC_KD, PC_KD, PC_KD / 2, 1);
        }
    }
    for (int k = 0; k < 10; k++) {
        float x = pc_kx(k), y = pc_ky(k);
        if (pckey.px) tk_blit(d.b, &pckey, (int)tk_floorf(PT(x - 14) - d.ox), (int)tk_floorf(PT(y - 14) - d.oy), 1);
        else d_rrect(&d, x, y, PC_KD, PC_KD, PC_KD / 2, W(.14f));
        char s[2] = { (char)('0' + k), 0 };
        float dw = tw(TK_IT400, 34, s, 0, 0);
        d_text(&d, TK_IT400, 34, x + (PC_KD - dw) / 2, y + (PCL[k][0] ? 41 : 50), s, 0, 0, W(1), 0);
        if (PCL[k][0]) {
            float lw = tw(TK_IT700, 10, PCL[k], 1.4f, 0);
            d_text(&d, TK_IT700, 10, x + (PC_KD - lw) / 2, y + 58, PCL[k], 1.4f, 0, W(.8f), 0);
        }
    }
    trim(&pckeys);                                 /* what it holds only (the heap) */
    tk_layer_prepare(&pckeys);
}
static void pc_draw_bot(void)
{
    relayer(&pcbot);
    dc d = mk(&pcbot, "pc-bot", 20, PC_BOT - 24, 350, 34, 0, 0);
    if (!d.b->px) return;
    if (PC.purpose == PCP_UNLOCK || PC.purpose == PCP_LAUNCH) d_text(&d, TK_IT400, 17, 44, PC_BOT, "Emergency", 0, 0, W(1), 0);
    const char *r = PC.n && PC.mode != PCM_WAIT ? "Delete" : "Cancel";
    d_text(&d, TK_IT400, 17, 346 - tw(TK_IT400, 17, r, 0, 0), PC_BOT, r, 0, 0, W(1), 0);
    trim(&pcbot);                                 /* what it holds only (the heap) */
    tk_layer_prepare(&pcbot);
}
static void pc_free(void)
{
    relayer(&pchead); relayer(&pcdots); relayer(&pckeys); relayer(&pcbot); relayer(&pchl);
    tk_bmp_free(&pckey);
    pcl.child = 0;
    pcl.hidden = 1;
    PC.built = 0;
}
static void pc_build(void)
{
    tk_layer_init(&pcl, "passcode");
    tk_layer_init(&pcbg, "pc-scrim");
    /* over the lock: #0a0f1f at .92 (premultiplied), the wallpaper's star faint under it; over Settings: opaque */
    int modal = PC.purpose == PCP_SET || PC.purpose == PCP_CHANGE || PC.purpose == PCP_OFF;
    pcbg.solid = modal ? 0xff0a0f1fu : 0xeb090e1du;
    pcbg.opaque = modal;
    pcbg.solid_w = TK_W;
    pcbg.solid_h = TK_H;
    pc_draw_head();
    pc_draw_dots();
    pc_draw_keys();
    pc_draw_bot();
    relayer(&pchl);
    dc h = mk(&pchl, "pc-key-down", pc_kx(1), pc_ky(1), PC_KD, PC_KD, 1, 0);
    if (h.b->px) d_rrect(&h, pc_kx(1), pc_ky(1), PC_KD, PC_KD, PC_KD / 2, W(.32f));
    pchl.hidden = 1;
    PC.built = 1;
}
static void pc_kids(void)
{
    tk_layer *k[] = { &pcbg, &pchead, &pcdots, &pckeys, &pchl, &pcbot };
    link(&pcl, k, 6);
}
/* relink(): the screen over everything but the call screen, the boot overlay, the toast and the sleep's black (just
 * under the call screen); while an app comes back on the unlock (no passcode), the lock's sliding cover over it */
static void pc_link(void)
{
    if (PC.built) {
        pc_kids();
        tk_layer *p = 0;
        for (tk_layer *l = root.child; l; p = l, l = l->next)
            if (l == &cvl) break;
        pcl.next = p ? p->next : root.child;
        if (p) p->next = &pcl; else root.child = &pcl;
    }
    if (PC.reorder) {
        tk_layer *p = 0, *l = root.child;
        for (; l && l != &lockflat; p = l, l = l->next) ;
        if (l && p) {
            p->next = l->next;                       /* out of its place */
            for (p = root.child; p && p != &apicon; p = p->next) ;
            if (p) { lockflat.next = p->next; p->next = &lockflat; }
            else { lockflat.next = l->next; }
        }
    }
}

/* ---- showing and hiding --------------------------------------------------------------------------------------- */
static void pc_view(void)
{
    if (sh_pass_on() && sh_pass_wait() > 0 && PC.mode == PCM_ENTER) PC.mode = PCM_WAIT;
    PC.want = PC.mode == PCM_NEW || PC.mode == PCM_AGAIN ? PC.setlen : sh_pass_len() ? sh_pass_len() : 6;
}
static void pc_show(int purpose)
{
    pc_wipe(PC.d, sizeof PC.d);
    pc_wipe(PC.first, sizeof PC.first);
    if (purpose != PCP_CHANGE) pc_wipe(PC.old, sizeof PC.old);
    PC.purpose = purpose;
    PC.mode = purpose == PCP_SET ? PCM_NEW : PCM_ENTER;
    PC.setlen = 6;
    PC.n = 0;
    PC.msg[0] = 0;
    PC.err = 0;
    PC.sh = -1;
    PC.ev = 0;
    PC.key = -1;
    pc_view();
    if (PC.built) pc_free();
    pc_build();
    PC.on = 1;
    PC.at = 1;
    pcl.alpha = PC.a;
    pcl.hidden = 0;
    if (purpose == PCP_UNLOCK && !PC.hidhome) { home.hidden = 1; PC.hidhome = 1; }   /* the lock flies off over the wallpaper */
    relink();
}
static void pc_hide(void)
{
    PC.on = 0;
    PC.at = 0;
    PC.key = -1;
    PC.ev = 0;
    pchl.hidden = 1;
    pc_wipe(PC.d, sizeof PC.d);
    pc_wipe(PC.first, sizeof PC.first);
    pc_wipe(PC.old, sizeof PC.old);
    PC.n = 0;
}
/* at once (the lock in the dark, a film's reset): nothing of it left */
static void pc_reset(void)
{
    pc_hide();
    PC.a = 0;
    PC.sh = -1;
    PC.nc = 0;
    PC.reorder = 0;
    if (PC.built) pc_free();
    if (PC.hidhome) { home.hidden = 0; PC.hidhome = 0; }
}

/* ---- Track A lifecycle: the open app over the lock and back ---------------------------------------------------- */
/* back_to_lock: the app open on home (not one opened over the lock from Emergency's cards, not a Linux window, not
 * in the switcher) is the one the unlock brings back; a film's reset forgets it */
static void pc_lock_note(int exit_em)
{
    if (exit_em) PC.resume = -1;
    else if (!locked) PC.resume = AP.on && !AP.closing && !SW.on && !lx_is(APPS[AP.a].k) ? AP.a : -1;
    pc_reset();
}
/* the unlock is decided: the kept app at once, the whole panel (its page drawn from its kept state over the next
 * frames), under the lock's cover while that slides off */
static void pc_resume_now(int under_cover)
{
    int a = PC.resume;
    PC.resume = -1;
    if (a < 0 || a >= NAPPS || AP.on) return;
    app_open(a);
    AP.icon = AP.from = FULLR;                       /* its open spring from the whole panel to it: at rest, unclipped */
    set_app_rect(FULLR);
    if (under_cover && lockY < 0) { PC.reorder = 1; relink(); }
}
/* the lock's swipe up was decided (shell_lock.c's G_LOCK): 1 the passcode first (the lock stays locked), 0 unlocked */
static int pc_unlock_begin(void)
{
    if (sh_pass_on()) {
        pc_show(PCP_UNLOCK);
        return 1;
    }
    locked = 0;
    pc_resume_now(1);
    return 0;
}
/* sh_tick's settle: the lock off the screen means unlocked unless the passcode screen holds it */
static int pc_holds(void) { return PC.on && locked; }
/* the right code: unlocked, the notification's app or the kept app or home */
static void pc_unlocked(void)
{
    sp_lock.live = 0;
    set_lock(-H_PT);
    locked = 0;
    lockflat.hidden = 1;
    home.sx = home.sy = 1;
    if (PC.hidhome) { home.hidden = 0; PC.hidhome = 0; }
    if (PC.nc) {
        unsigned id = PC.nc_id;
        PC.nc = 0;
        PC.resume = -1;                              /* the notification's app instead */
        (void)nc_launch(id);
    } else
        pc_resume_now(0);
}
/* shell_nc.inc's nc_launch from the lock: 1 the passcode first (the launch runs after the right code) */
static int pc_gate_launch(unsigned id)
{
    if (!locked || !sh_pass_on()) return 0;
    PC.nc = 1;
    PC.nc_id = id;
    pc_show(PCP_LAUNCH);
    return 1;
}
static void pc_cancel(void)
{
    int p = PC.purpose;
    pc_hide();
    PC.nc = 0;
    if ((p == PCP_UNLOCK || p == PCP_LAUNCH) && locked && (lockY < 0 || sp_lock.live))
        tk_spring_start(&sp_lock, lockY, 0, 0, TK_SPR_UNLOCK);   /* the lock comes back (home stays hidden until it is) */
}

/* ---- typing --------------------------------------------------------------------------------------------------- */
static void pc_wrong(const char *msg, int err)
{
    sh_haptic();
    PC.sh = 0;                                       /* the shake; the dots clear at its end */
    tk_memcpy(PC.msg, msg, tk_strlen(msg) + 1);
    PC.err = err;
    pc_draw_head();
    pc_kids();
}
static void pc_wait_view(void)
{
    PC.mode = PCM_WAIT;
    PC.n = 0;
    pc_wipe(PC.d, sizeof PC.d);
    PC.msg[0] = 0;
    pc_draw_head(); pc_draw_dots(); pc_draw_keys(); pc_draw_bot();
    pc_kids();
}
static void pc_settings_done(const char *t)
{
    pc_hide();
    if (AP.on && is_app("settings")) build_app_page(AP.a);
    toast(t);
}
/* all the digits are in: what they do */
static void pc_eval(void)
{
    char d[8];
    tk_memcpy(d, PC.d, sizeof d);
    pc_wipe(PC.d, sizeof PC.d);
    if (PC.mode == PCM_NEW) {
        tk_memcpy(PC.first, d, sizeof d);
        PC.mode = PCM_AGAIN;
        PC.n = 0;
        PC.msg[0] = 0;
        pc_draw_head(); pc_draw_dots(); pc_draw_bot();
    } else if (PC.mode == PCM_AGAIN) {
        int same = !tk_strcmp(d, PC.first);
        if (!same) {
            pc_wipe(PC.first, sizeof PC.first);
            PC.mode = PCM_NEW;
            tk_memcpy(PC.d, d, sizeof d);            /* the shake shows them, then they clear */
            pc_wrong("Passcodes did not match. Try again.", 1);
        } else if (sh_pass_set(PC.purpose == PCP_CHANGE ? PC.old : "", PC.first) == 0)
            pc_settings_done(PC.purpose == PCP_CHANGE ? "Passcode changed" : "Passcode on");
        else
            pc_settings_done("The passcode wasn't changed");
    } else if (PC.purpose == PCP_OFF) {
        if (sh_pass_set(d, "") == 0) pc_settings_done("Passcode off");
        else if (sh_pass_wait() > 0) pc_wait_view();
        else { tk_memcpy(PC.d, d, sizeof d); pc_wrong("", 0); }
    } else {
        int r = sh_pass_check(d);
        if (r == 1) {
            if (PC.purpose == PCP_CHANGE) {
                tk_memcpy(PC.old, d, sizeof d);
                PC.mode = PCM_NEW;
                PC.n = 0;
                PC.setlen = 6;
                PC.want = 6;
                pc_draw_head(); pc_draw_dots(); pc_draw_bot();
            } else {
                pc_hide();
                pc_unlocked();
            }
        } else if (r < 0 || sh_pass_wait() > 0) pc_wait_view();
        else {
            tk_memcpy(PC.d, d, sizeof d);
            int f = sh_pass_fails();
            pc_wrong(f >= 3 ? (f == 5 ? "Wrong passcode. The next wrong one waits a minute." : "Wrong passcode") : "", f == 5);
        }
    }
    pc_wipe(d, sizeof d);
    pc_kids();
}
static void pc_digit(int k)
{
    if (PC.mode == PCM_WAIT || PC.sh >= 0 || PC.ev > 0 || PC.n >= PC.want) return;
    PC.d[PC.n++] = (char)('0' + k);
    PC.d[PC.n] = 0;
    if (PC.n == 1 && PC.msg[0]) { PC.msg[0] = 0; pc_draw_head(); }
    pc_draw_dots();
    if (PC.n == 1) pc_draw_bot();
    if (PC.n == PC.want) PC.ev = .12f;               /* the last dot shows first */
    pc_kids();
}
static int pc_key_at(float x, float y)
{
    if (PC.mode == PCM_WAIT) return -1;
    for (int k = 0; k < 10; k++) {
        float cx = pc_kx(k) + PC_KD / 2, cy = pc_ky(k) + PC_KD / 2, dx = x - cx, dy = y - cy;
        if (dx * dx + dy * dy <= 44 * 44) return k;
    }
    return -1;
}
/* sh_touch: every touch while the screen is up is its own */
static int pc_touch(int phase, float x, float y, float t)
{
    (void)t;
    if (!PC.on) return 0;
    if (phase == 0) {
        PC.key = pc_key_at(x, y);
        if (PC.key >= 0) {
            pchl.x = pchl.bx + PT(pc_kx(PC.key) - pc_kx(1));
            pchl.y = pchl.by + PT(pc_ky(PC.key) - pc_ky(1));
            pchl.hidden = 0;
            pc_digit(PC.key);                        /* iOS: the digit at the touch, the light while held */
        }
        return 1;
    }
    if (phase == 1) {
        if (PC.key >= 0 && pc_key_at(x, y) != PC.key) pchl.hidden = 1;
        return 1;
    }
    pchl.hidden = 1;
    int was = PC.key;
    PC.key = -1;
    if (was >= 0) return 1;
    if (y >= PC_BOT - 34 && y < PC_BOT + 20) {
        if (x < 150 && (PC.purpose == PCP_UNLOCK || PC.purpose == PCP_LAUNCH)) {
            /* Emergency: never behind the passcode */
            pc_cancel();
            if (!EMG) set_emergency(1, 0);
        } else if (x >= 270) {
            if (PC.n && PC.mode != PCM_WAIT && PC.sh < 0 && PC.ev <= 0) {
                PC.d[--PC.n] = 0;
                pc_draw_dots();
                if (!PC.n) pc_draw_bot();
                pc_kids();
            } else
                pc_cancel();
        }
        return 1;
    }
    if (PC.mode == PCM_NEW && !PC.msg[0] && y >= 252 && y < 280 && x >= 110 && x < 280) {   /* the length's option */
        PC.setlen = PC.setlen == 6 ? 4 : 6;
        PC.want = PC.setlen;
        PC.n = 0;
        pc_wipe(PC.d, sizeof PC.d);
        pc_draw_head(); pc_draw_dots(); pc_draw_bot();
        pc_kids();
    }
    return 1;
}

/* ---- time ------------------------------------------------------------------------------------------------------ */
static int pc_tick(float dt, float t)
{
    (void)t;
    int mv = 0;
    if (PC.a != PC.at) {
        PC.a = PC.at > PC.a ? tk_minf(PC.at, PC.a + dt / .22f) : tk_maxf(PC.at, PC.a - dt / .22f);
        pcl.alpha = PC.a;
        if (!PC.on && PC.a <= 0) { pc_free(); relink(); }
        mv = 1;
    }
    if (PC.sh >= 0) {
        PC.sh += dt / .5f;
        float s = tk_minf(PC.sh, 1);
        pcdots.x = pcdots.bx + PT(16 * tk_sinf(s * 6 * 3.14159265f) * (1 - s));
        if (PC.sh >= 1) {
            PC.sh = -1;
            pcdots.x = pcdots.bx;
            PC.n = 0;
            pc_wipe(PC.d, sizeof PC.d);
            if (PC.built) { pc_draw_dots(); pc_draw_bot(); pc_kids(); }
        }
        mv = 1;
    }
    if (PC.ev > 0) {
        PC.ev -= dt;
        if (PC.ev <= 0) { PC.ev = 0; pc_eval(); }
        mv = 1;
    }
    if (PC.reorder && !sp_lock.live) { PC.reorder = 0; relink(); }
    if (PC.hidhome && !PC.on && !sp_lock.live && lockY >= 0) { home.hidden = 0; PC.hidhome = 0; }
    if (PC.on && PC.mode == PCM_WAIT && !sh_pass_wait()) {     /* the wait is over: the keypad again */
        PC.mode = PCM_ENTER;
        pc_view();
        pc_draw_head(); pc_draw_dots(); pc_draw_keys(); pc_draw_bot();
        pc_kids();
        mv = 1;
    }
    return mv | PC.reorder;
}
static int pc_moving(void) { return PC.a != PC.at || PC.sh >= 0 || PC.ev > 0 || PC.reorder; }
/* the back-off's words change by the minute: the platform comes back then (sh_next_update / sh_refresh) */
static float pc_next_update(float t)
{
    (void)t;
    if (!PC.on || PC.mode != PCM_WAIT) return -1;
    int w = sh_pass_wait();
    return w <= 0 ? 50 : (float)((w % 60 ? w % 60 : 60) * 1000 + 50);
}
static int pc_refresh(float t)
{
    (void)t;
    if (!PC.on || PC.mode != PCM_WAIT) return 0;
    int w = sh_pass_wait(), m = (w + 59) / 60;
    if (w <= 0) {
        PC.mode = PCM_ENTER;
        pc_view();
        pc_draw_head(); pc_draw_dots(); pc_draw_keys(); pc_draw_bot();
    } else if (m != PC.shown_min)
        pc_draw_head();
    else
        return 0;
    pc_kids();
    return 1;
}

/* ---- Settings > Passcode (AS.sub PC_SUB: shell_lock.c maps it to B2's SE_PASSCODE_PAGE): the rows and Auto-Lock -- */
#ifndef PC_SUB
#define PC_SUB 5
#endif
#define PC_G1 (PUSHED_TOP + 8)
#define PC_G2 (PC_G1 + 2 * 47 + 76)
static const int PC_AL[7] = { 30, 60, 120, 180, 240, 300, 0 };
static const char *const PC_ALN[7] = { "30 Seconds", "1 Minute", "2 Minutes", "3 Minutes", "4 Minutes", "5 Minutes", "Never" };
static const char *pc_live(const char *s)
{
    if (!tk_strcmp(s, "@pass")) return sh_pass_on() ? "On" : "Off";
    return 0;
}
static void pc_page(dc *d)
{
    d_back(d, W(1));
    d_navtitle(d, "Passcode");                    /* (B2's row: the S9 has no Face ID) */
    int on = sh_pass_on(), n1 = on ? 2 : 1;
    d_rrect(d, GROUP_X, PC_G1, GROUP_W, 47.0f * n1 - 1, 16, C_CARD);
    d_text(d, TK_IT400, 16, ROW_TX, PC_G1 + 29, on ? "Turn Passcode Off" : "Turn Passcode On", 0, 0, ACCENT(), 0);
    if (on) {
        d_rrect(d, ROW_TX, PC_G1 + 46, CONTENT_R - ROW_TX, .5f, 0, W(.08f));
        d_text(d, TK_IT400, 16, ROW_TX, PC_G1 + 47 + 29, "Change Passcode", 0, 0, ACCENT(), 0);
    }
    float fy = PC_G1 + 47.0f * n1 + 8;
    d_text(d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, fy), "Emergency mode, SOS, the medical card and the torch", 0, 0, W(.45f), 0);
    d_text(d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, fy + 17), "always work without the passcode.", 0, 0, W(.45f), 0);
    d_text(d, TK_IT400, 13, ROW_TX, PC_G2 - 8, "AUTO-LOCK", .52f, 0, W(.55f), 0);
    d_rrect(d, GROUP_X, PC_G2, GROUP_W, 47.0f * 7 - 1, 16, C_CARD);
    int cur = sh_autolock();
    for (int i = 0; i < 7; i++) {
        float ry = PC_G2 + 47.0f * i;
        d_text(d, TK_IT400, 16, ROW_TX, ry + 29, PC_ALN[i], 0, 0, INK, 0);
        if (PC_AL[i] == cur) {                      /* iOS's check mark */
            d_seg(d, CONTENT_R - 30, ry + 24, CONTENT_R - 25, ry + 29, 2.2f, ACCENT());
            d_seg(d, CONTENT_R - 25, ry + 29, CONTENT_R - 16, ry + 17, 2.2f, ACCENT());
        }
        if (i < 6) d_rrect(d, ROW_TX, ry + 46, CONTENT_R - ROW_TX, .5f, 0, W(.08f));
    }
    d_text(d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, PC_G2 + 47 * 7 + 8), "The screen turns off and the phone locks after this", 0, 0, W(.45f), 0);
    d_text(d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, PC_G2 + 47 * 7 + 25), "long without a touch.", 0, 0, W(.45f), 0);
}
static float pc_row_main(void)
{
    const appdef *A = &APPS[AP.a];
    for (int s = 0; s < A->ns; s++)
        for (int i = 0; i < A->sec[s].n; i++)
            if (!tk_strcmp(A->sec[s].rows[i].l, "Face ID & Passcode")) return gen_row_y(A, s, i);
    return -1;
}
/* app_tap: 1 taken (Settings' row, the sub-page) */
static int pc_app_tap(float x, float y)
{
    if (!is_app("settings")) return 0;
    if (!AS.sub) {
        float ry = pc_row_main();
        if (ry < 0 || x < GROUP_X || x >= CONTENT_R || y < ry || y >= ry + 47) return 0;
        AS.sub = PC_SUB;
        build_app_page(AP.a);
        return 1;
    }
    if (AS.sub != PC_SUB) return 0;
    if (BACK_HIT(x, y)) { settings_back(); return 1; }   /* B2's root, where it was */
    if (x < GROUP_X || x >= CONTENT_R) return 1;
    int on = sh_pass_on();
    if (y >= PC_G1 && y < PC_G1 + 47) { pc_show(on ? PCP_OFF : PCP_SET); return 1; }
    if (on && y >= PC_G1 + 47 && y < PC_G1 + 94) { pc_show(PCP_CHANGE); return 1; }
    if (y >= PC_G2 && y < PC_G2 + 47 * 7) {
        sh_autolock_set(PC_AL[(int)((y - PC_G2) / 47)]);
        build_app_page(AP.a);
    }
    return 1;
}
/* app_press: the row under the finger lights */
static int pc_press(float x, float y, tk_rect *r, tk_rect *g, float *rad, tk_col *c)
{
    (void)c;
    if (!is_app("settings") || x < GROUP_X || x >= CONTENT_R) return 0;
    float top = -1, gy = 0, gh = 0;
    if (!AS.sub) {
        float ry = pc_row_main();
        if (ry >= 0 && y >= ry && y < ry + 47) { top = ry; gy = ry; gh = 47; }   /* (its group's clip: the row alone) */
    } else if (AS.sub == PC_SUB) {
        int n1 = sh_pass_on() ? 2 : 1;
        if (y >= PC_G1 && y < PC_G1 + 47 * n1) { top = PC_G1 + 47 * (int)((y - PC_G1) / 47); gy = PC_G1; gh = 47.0f * n1 - 1; }
        else if (y >= PC_G2 && y < PC_G2 + 47 * 7) { top = PC_G2 + 47 * (int)((y - PC_G2) / 47); gy = PC_G2; gh = 47 * 7 - 1; }
    }
    if (top < 0) return 0;
    *r = (tk_rect){ GROUP_X, top, GROUP_W, 47 };
    *g = (tk_rect){ GROUP_X, gy, GROUP_W, gh };
    *rad = 16;
    return 1;
}

/* ---- the host's stills ----------------------------------------------------------------------------------------- */
/* passcode=unlock:N   the unlock's screen with N digits typed      passcode=wrong:P   a wrong code, the shake at P
 * passcode=set:N      Turn Passcode On's first screen               passcode=off       gone
 * passcode=settings   Settings' Face ID & Passcode page (with app=settings) */
static int pc_set(const char *what, const char *key, float v)
{
    if (tk_strcmp(what, "passcode")) return 0;
    if (!tk_strcmp(key, "off")) { pc_reset(); return 1; }
    if (!tk_strcmp(key, "settings")) {                       /* Settings > Face ID & Passcode (Settings open) */
        AS.sub = PC_SUB;
        if (AP.on) build_app_page(AP.a);
        return 1;
    }
    int p = !tk_strcmp(key, "set") ? PCP_SET : !tk_strcmp(key, "change") ? PCP_CHANGE : PCP_UNLOCK;
    if (p == PCP_UNLOCK && locked) { sp_lock.live = 0; set_lock(-H_PT); }
    pc_show(p);
    PC.a = PC.at = 1;
    pcl.alpha = 1;
    if (!tk_strcmp(key, "wrong")) {
        PC.n = PC.want;
        pc_wrong(sh_pass_fails() >= 3 ? "Wrong passcode" : "", 0);
        PC.sh = v;
        float s = tk_minf(v, 1);
        pcdots.x = pcdots.bx + PT(16 * tk_sinf(s * 6 * 3.14159265f) * (1 - s));
    } else {
        PC.n = tk_mini((int)v, PC.want);
        for (int i = 0; i < PC.n; i++) PC.d[i] = '0';
    }
    pc_draw_dots();
    pc_draw_bot();
    pc_kids();
    return 1;
}

/* ---- Track A lifecycle: the kernel's save before a restart or a switch-off ------------------------------------- */
void sh_save_all(void)
{
    for (int i = 0; APPHOOK[i].k; i++)
        if (APPHOOK[i].on_save) APPHOOK[i].on_save();
}
