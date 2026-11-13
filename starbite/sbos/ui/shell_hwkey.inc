/* cp7: a hardware keyboard's keys (Track A's sh_hw_key: the Bluetooth keyboard, kern/bt_hid.c). Down only (the kernel
 * sends no repeat). A Linux window in front (the Terminal, a Linux app): its keys, arrows, Esc and Tab included (the
 * key bar's codes, LXKC); else the focused text field takes them as the on-screen keyboard's own (kb_emit: the page's
 * kb_edit / kb_take), characters, Delete and Return; nothing focused or the lock: dropped (iOS wakes on a key; the
 * kernel decides the screen). The platform draws the frame after the call (kern_next's bt_key_cb).
 * Included by shell_lock.c after shell_linux.inc and shell_kb.inc. */

static int hwk_dirty;                           /* a key changed the page: the next refresh draws it */
static int hwk_key_index(int kind)
{
    for (int i = 0; i < KB.nk; i++) if (KB.k[i].kind == kind) return i;
    return KB.nk > 0 ? 0 : -1;
}
void sh_hw_key(int usage, int down, uint32_t unicode)
{
    if (!down || locked || SL.black || !AP.on || AP.closing) return;
    const char *k = APPS[AP.a].k;
    if (lx_is(k) && tk_strcmp(k, "store")) {           /* a Linux window: its own keys */
        int code = usage == 0x29 ? 1 : usage == 0x2B ? 15 : usage == 0x50 ? 105 : usage == 0x52 ? 103 :
                   usage == 0x51 ? 108 : usage == 0x4F ? 106 : 0;   /* Esc, Tab, Left, Up, Down, Right */
        if (code) (void)sh_linux_key(code, 0);
        else if (unicode == 0x08) (void)sh_linux_char('\b');
        else if (unicode == '\n' || unicode == '\r') (void)sh_linux_char('\n');
        else if (unicode >= 0x20 && unicode < 0x7f) (void)sh_linux_char((char)unicode);
        return;
    }
    if (!KB.focused) return;
    char s[2] = { 0, 0 };
    int ev = 0, i = -1;
    if (unicode == 0x08) { ev = KBE_DEL; i = hwk_key_index(KK_DEL); }
    else if (unicode == '\n' || unicode == '\r') { ev = KBE_RET; s[0] = '\n'; i = hwk_key_index(KK_RET); }
    else if (unicode >= 0x20 && unicode < 0x7f) { ev = KBE_TEXT; s[0] = (char)unicode; i = hwk_key_index(unicode == ' ' ? KK_SPACE : KK_CHAR); }
    if (!ev || i < 0) return;
    if (KB.shift == 1) KB.shift = 0;                   /* the on-screen shift (once) is spent by any typed letter */
    kb_emit(ev, s, i);
    hwk_dirty = 1;
}
static int hwk_refresh(void) { int d = hwk_dirty; hwk_dirty = 0; return d; }
