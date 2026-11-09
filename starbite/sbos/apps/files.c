/* SPDX-License-Identifier: MIT
 * Files (Starbite OS 1.2-alpha21): the microSD card's exFAT files, read only. Folders open; a text file (.txt .md
 * .log .csv .cfg .ini .toml .json .conf) shows its first 4 KiB, any other file its size. Nothing is ever written,
 * and names are never logged. The card is read only while this app is open (the first look mounts it). */
#include "sbapp.h"
#include "sbfmt.h"

SBAPP_DEFINE("files", "Files", SBOS_P_FILES, 0xff5ac8faU, 212);

#define PER 10
#define GREY 0xff8e8e93U

static char path[256] = "/";
static struct sbos_dirent ent[PER];
static int n, total, first, visible, viewing = -1;
static char text[4097];
static int text_n;

static unsigned slen(const char *s)
{
    unsigned k = 0;
    while (s[k])
        ++k;
    return k;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int is_text(const char *name)
{
    static const char *const ext[] = { ".txt", ".md", ".log", ".csv", ".cfg", ".ini", ".toml", ".json", ".conf" };
    unsigned l = slen(name);
    for (unsigned e = 0; e < sizeof(ext) / sizeof(ext[0]); ++e) {
        unsigned m = slen(ext[e]);
        if (l <= m)
            continue;
        unsigned i = 0;
        while (i < m && lower(name[l - m + i]) == ext[e][i])
            ++i;
        if (i == m)
            return 1;
    }
    return 0;
}

static void size_s(struct fmt *f, uint64_t b)
{
    if (b < 1024) {
        fmt_d(f, (int64_t)b);
        fmt_s(f, " bytes");
    } else if (b < 1024 * 1024) {
        fmt_d(f, (int64_t)(b / 1024));
        fmt_s(f, " KB");
    } else if (b < 1024ULL * 1024 * 1024) {
        fmt_d(f, (int64_t)(b / (1024 * 1024)));
        fmt_s(f, " MB");
    } else {
        fmt_d(f, (int64_t)(b / (1024ULL * 1024 * 1024)));
        fmt_s(f, " GB");
    }
}

static int join(char *out, unsigned cap, const char *name)
{
    unsigned a = slen(path), b = slen(name);
    if (a + b + 2 > cap)
        return 0;
    unsigned k = 0;
    for (unsigned i = 0; i < a; ++i)
        out[k++] = path[i];
    if (k == 0 || out[k - 1] != '/')
        out[k++] = '/';
    for (unsigned i = 0; i < b; ++i)
        out[k++] = name[i];
    out[k] = 0;
    return 1;
}

static void up(void)
{
    unsigned l = slen(path);
    while (l > 1 && path[l - 1] != '/')
        --l;
    if (l > 1)
        --l;                                /* the slash before the last name */
    path[l ? l : 1] = 0;
    if (!l)
        path[0] = '/';
}

static void load(void)
{
    n = file_list(path, ent, PER, first, &total);
    struct fmt f;                           /* counts only: names are never logged */
    fmt_init(&f);
    if (n < 0) {
        fmt_s(&f, "files: the folder could not be read: ");
        fmt_d(&f, n);
    } else {
        fmt_s(&f, "files: a folder of ");
        fmt_d(&f, total);
        fmt_s(&f, " items");
    }
    sb_log_app(f.b);
}

static void show(void)
{
    if (!visible)
        return;
    struct fmt f;
    ui_begin();
    if (viewing >= 0 && viewing < n) {
        ui_title(ent[viewing].name);
        fmt_init(&f);
        size_s(&f, ent[viewing].size);
        if (text_n >= 0 && (uint64_t)text_n < ent[viewing].size)
            fmt_s(&f, " (the first 4 KB)");
        ui_textline(f.b, GREY);
        if (text_n < 0) {
            ui_textline("Not a text file: only its size is shown.", GREY);
        } else {
            /* one text line per line of the file, at most 40 */
            int lines = 0;
            for (int i = 0; i < text_n && lines < 40;) {
                char l[200];
                unsigned k = 0;
                while (i < text_n && text[i] != '\n' && k + 1 < sizeof(l)) {
                    char c = text[i++];
                    l[k++] = c == '\t' ? ' ' : (c == '\r' || ((unsigned char)c < 0x20)) ? ' ' : c;
                }
                while (i < text_n && text[i] != '\n')
                    ++i;
                ++i;
                l[k] = 0;
                ui_textline(k ? l : " ", 0xffffffffU);
                ++lines;
            }
        }
        ui_button(3, "Back to the folder", 0, 0);
        ui_end();
        return;
    }
    ui_title("Files");
    ui_textline(path, GREY);
    if (n < 0) {
        ui_textline(n == SBOS_EAGAIN ? "No card, or no exFAT volume on it." : "This folder could not be read.",
                    0xffff9f0aU);
        if (path[1])
            ui_button(2, "Up", 0, UI_HALF);
        ui_end();
        return;
    }
    if (path[1])
        ui_button(2, "Up", 0, UI_HALF);
    if (!total)
        ui_textline("This folder is empty.", GREY);
    for (int i = 0; i < n; ++i) {
        fmt_init(&f);
        if (ent[i].dir)
            fmt_s(&f, "Folder");
        else
            size_s(&f, ent[i].size);
        ui_button((uint32_t)(10 + i), ent[i].name, f.b, ent[i].dir ? UI_ACCENT : 0);
    }
    if (first > 0)
        ui_button(4, "Previous", 0, UI_HALF);
    if (first + n < total)
        ui_button(5, "More", 0, UI_HALF);
    fmt_init(&f);
    fmt_d(&f, total);
    fmt_s(&f, total == 1 ? " item" : " items");
    if (total >= 128)
        fmt_s(&f, " (the first 128)");
    fmt_s(&f, ". Read only: nothing on the card is changed.");
    ui_textline(f.b, GREY);
    ui_end();
}

void app_event(const struct sbos_event *ev)
{
    switch (ev->type) {
    case EV_APP_START:
        ui_tile("Read only");
        break;
    case EV_APP_SHOW:
        visible = 1;
        viewing = -1;
        load();                             /* the first look mounts the card */
        show();
        break;
    case EV_APP_HIDE:
        visible = 0;
        viewing = -1;
        break;
    case EV_APP_BUTTON:
        if (ev->id == 2) {
            up();
            first = 0;
            load();
        } else if (ev->id == 3) {
            viewing = -1;
        } else if (ev->id == 4 && first > 0) {
            first = first >= PER ? first - PER : 0;
            load();
        } else if (ev->id == 5 && first + n < total) {
            first += PER;
            load();
        } else if (ev->id >= 10 && ev->id < 10 + (uint32_t)(n > 0 ? n : 0)) {
            int i = (int)ev->id - 10;
            if (ent[i].dir) {
                char p[256];
                if (join(p, sizeof(p), ent[i].name)) {
                    for (unsigned k = 0; k <= slen(p); ++k)
                        path[k] = p[k];
                    first = 0;
                    load();
                }
            } else {
                viewing = i;
                text_n = -1;
                if (is_text(ent[i].name)) {
                    char p[256];
                    text_n = join(p, sizeof(p), ent[i].name) ? file_read(p, text, sizeof(text) - 1) : -1;
                    if (text_n >= 0) {
                        text[text_n] = 0;
                        struct fmt f;
                        fmt_init(&f);
                        fmt_s(&f, "files: a text file shown, bytes ");
                        fmt_d(&f, text_n);
                        sb_log_app(f.b);
                    }
                }
            }
        }
        show();
        break;
    default:
        break;
    }
}
