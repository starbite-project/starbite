/* SPDX-License-Identifier: GPL-2.0-only
 * The Dashboard (EN2): live CPU clusters and clocks, temperatures, battery % / voltage / current
 * with a current graph, RAM, uptime. Sampled once a second (main.c tick); each tile is a custom
 * page item whose hash is its values, so a refresh redraws only the tiles that changed.
 */
#include "eng.h"

static struct sb_temps T;
static struct sb_bat B;
static uint64_t a55_hz, m3_hz, gpu_hz;
static int m3_off, gpu_off, sampled;
static uint64_t dram_total;
static struct ui_chart ch_ma, ch_mv, ch_temp;
static uint64_t last_sample;

void dash_sample(void)
{
    uint64_t now = sb_ms();
    if (sampled && now - last_sample < 900)
        return;
    last_sample = now;
    sb_temps_read(&T);
    a55_hz = sb_a55_hz();
    m3_hz = sb_m3_hz(&m3_off);
    gpu_hz = sb_gpu_hz(&gpu_off);
    if (sb_bat_read(&B)) {
        chart_push(&ch_ma, B.ma);
        chart_push(&ch_mv, B.mv);
    }
    if (T.hottest > -100)
        chart_push(&ch_temp, T.hottest);
    if (!dram_total) {
        uint64_t base[8], size[8];
        int n = fdt_memory_banks(sb_boot.fdt, base, size, 8);
        for (int i = 0; i < n; ++i)
            dram_total += size[i];
    }
    sampled = 1;
}

/* ---- tiles ---------------------------------------------------------------------------------- */

struct tile {
    const char *label;
    char value[24], sub[48];
    uint32_t c;
};

static uint32_t tile_hash(const struct tile *t)
{
    uint32_t h = 2166136261U;
    for (const char *s = t->value; *s; ++s)
        h = (h ^ (uint8_t)*s) * 16777619U;
    for (const char *s = t->sub; *s; ++s)
        h = (h ^ (uint8_t)*s) * 16777619U;
    return h ^ t->c;
}

static struct tile tiles[8];

static void draw_tile(const struct tile *t, int x, int y, int w, int h)
{
    ui_round(x, y, w, h, 40, C_CARD);
    ui_text(F_HEAD, x + 44, y + 70, t->label, C_L2);
    ui_text_cut(F_BIG, x + 40, y + 196, t->value, w - 80, t->c ? t->c : C_L1);
    ui_text_cut(F_DETAIL, x + 44, y + 262, t->sub, w - 88, C_L2);
}

static void draw_pair(const struct pitem *it, int x, int y, int w, int focused)
{
    (void)focused;
    const struct tile *a = &tiles[it->arg * 2], *b = &tiles[it->arg * 2 + 1];
    ui_fill(x, y, w, it->h, C_BG);
    int tw = (w - 32) / 2;
    draw_tile(a, x, y + 8, tw, it->h - 32);
    draw_tile(b, x + tw + 32, y + 8, tw, it->h - 32);
}

static void mhz(char *out, uint64_t hz)
{
    struct ub u;
    ub_init(&u);
    ub_dec(&u, hz / 1000000U);
    sb_strcpy(out, 24, u.b);
}

static void deg(char *out, int t)
{
    struct ub u;
    ub_init(&u);
    if (t <= -100)
        ub_s(&u, U_DASH);
    else {
        ub_sdec(&u, t);
        ub_s(&u, U_DEG);
    }
    sb_strcpy(out, 24, u.b);
}

static uint32_t temp_c(int t)
{
    return t >= 85 ? C_RED : t >= 65 ? C_ORANGE : C_L1;
}

static void make_tiles(void)
{
    struct ub u;
    struct tile *t = tiles;
    /* CPU */
    t[0].label = "CORTEX-A55";
    mhz(t[0].value, a55_hz);
    sb_strcpy(t[0].sub, sizeof(t[0].sub), "MHz " U_DOT " cluster 0 " U_DOT " this core");
    t[0].c = 0;
    t[1].label = "MONGOOSE M3";
    if (m3_off) {
        sb_strcpy(t[1].value, sizeof(t[1].value), "off");
        sb_strcpy(t[1].sub, sizeof(t[1].sub), "cluster 1 powered down");
        t[1].c = C_L3;
    } else {
        mhz(t[1].value, m3_hz);
        sb_strcpy(t[1].sub, sizeof(t[1].sub), "MHz " U_DOT " cluster 1");
        t[1].c = 0;
    }
    /* temperatures */
    t[2].label = "HOTTEST";
    deg(t[2].value, T.hottest);
    ub_init(&u);
    ub_s(&u, "big ");
    if (T.top) {
        ub_sdec(&u, T.big_max);
        ub_s(&u, U_DEG " " U_DOT " little ");
        ub_sdec(&u, T.little);
        ub_s(&u, U_DEG);
    } else {
        ub_s(&u, "TMU off");
    }
    sb_strcpy(t[2].sub, sizeof(t[2].sub), u.b);
    t[2].c = temp_c(T.hottest);
    t[3].label = "GPU";
    deg(t[3].value, T.sub ? T.gpu : -100);
    ub_init(&u);
    if (gpu_off)
        ub_s(&u, "Mali-G72 powered off");
    else {
        ub_s(&u, "Mali-G72 " U_DOT " ");
        ub_dec(&u, gpu_hz / 1000000U);
        ub_s(&u, " MHz");
    }
    sb_strcpy(t[3].sub, sizeof(t[3].sub), u.b);
    t[3].c = T.sub ? temp_c(T.gpu) : C_L3;
    /* battery */
    t[4].label = "BATTERY";
    ub_init(&u);
    if (B.ok) {
        ub_dec(&u, (uint64_t)B.soc_pct);
        ub_c(&u, '%');
    } else {
        ub_s(&u, U_DASH);
    }
    sb_strcpy(t[4].value, sizeof(t[4].value), u.b);
    ub_init(&u);
    if (B.ok) {
        ub_fix(&u, (uint64_t)B.mv, 2);
        ub_s(&u, " V " U_DOT " ");
        ub_s(&u, B.input == 2 ? "wireless" : B.input == 1 ? "USB" : "on battery");
    } else {
        ub_s(&u, sb_max_why ? "fuel gauge not answering" : "not read yet");
    }
    sb_strcpy(t[4].sub, sizeof(t[4].sub), u.b);
    t[4].c = B.ok && B.soc_pct < 20 ? C_RED : B.ok && B.input ? C_GREEN : 0;
    t[5].label = "CURRENT";
    ub_init(&u);
    if (B.ok) {
        if (B.ma > 0)
            ub_c(&u, '+');
        ub_sdec(&u, B.ma);
    } else {
        ub_s(&u, U_DASH);
    }
    sb_strcpy(t[5].value, sizeof(t[5].value), u.b);
    ub_init(&u);
    if (B.ok) {
        ub_s(&u, "mA " U_DOT " avg ");
        ub_sdec(&u, B.avg_ma);
        ub_s(&u, B.ma >= 0 ? " " U_DOT " charging" : " " U_DOT " draining");
    } else {
        ub_s(&u, "mA");
    }
    sb_strcpy(t[5].sub, sizeof(t[5].sub), u.b);
    t[5].c = B.ok ? (B.ma >= 0 ? C_GREEN : C_ORANGE) : C_L3;
    /* memory, uptime */
    t[6].label = "RAM";
    ub_init(&u);
    ub_dec(&u, dram_total ? (dram_total + (1ULL << 29)) >> 30 : 4);
    ub_s(&u, " GB");
    sb_strcpy(t[6].value, sizeof(t[6].value), u.b);
    ub_init(&u);
    ub_size(&u, sb_arena_free());
    ub_s(&u, " free (arena)");
    sb_strcpy(t[6].sub, sizeof(t[6].sub), u.b);
    t[6].c = 0;
    t[7].label = "UPTIME";
    uint64_t s = sb_ms() / 1000U;
    ub_init(&u);
    ub_dec(&u, s / 3600U);
    ub_c(&u, ':');
    ub_dec2(&u, (unsigned)((s / 60U) % 60U));
    ub_c(&u, ':');
    ub_dec2(&u, (unsigned)(s % 60U));
    sb_strcpy(t[7].value, sizeof(t[7].value), u.b);
    sb_strcpy(t[7].sub, sizeof(t[7].sub), "since power-on (counter)");
    t[7].c = 0;
}

/* ---- the current graph -------------------------------------------------------------------- */

static void draw_chart(const struct pitem *it, int x, int y, int w, int focused)
{
    (void)focused;
    const struct ui_chart *c = it->arg == 0 ? &ch_ma : it->arg == 1 ? &ch_mv : &ch_temp;
    const char *title = it->arg == 0 ? "BATTERY CURRENT (mA, + = CHARGING)" : it->arg == 1 ? "CELL VOLTAGE (mV)" :
                        "HOTTEST SENSOR (" U_DEG "C)";
    uint32_t col = it->arg == 0 ? C_GREEN : it->arg == 1 ? C_STAR : C_ORANGE;
    ui_text(F_HEAD, x + 44, y + 64, title, C_L2);
    chart_draw(c, x + 24, y + 90, w - 48, it->h - 120, col, C_CARD, "");
}

static uint32_t chart_hash(const struct ui_chart *c)
{
    return (uint32_t)c->n * 2654435761U ^ (uint32_t)c->head * 40503U ^
           (uint32_t)(c->n ? c->v[(c->head + CHART_N - 1) % CHART_N] : 0);
}

void dash_fill(void)
{
    dash_sample();
    make_tiles();
    kv_hdr("LIVE");
    for (unsigned i = 0; i < 4; ++i)
        kv_custom(320, draw_pair, i, tile_hash(&tiles[2 * i]) * 31U ^ tile_hash(&tiles[2 * i + 1]));
    kv_gap(24);
    kv_custom(520, draw_chart, 0, chart_hash(&ch_ma));
    kv_note("Sampled every second while Engineer OS runs: about 4 minutes of history.");
}

/* ---- the battery graph page -------------------------------------------------------------- */

static void fill_battery_graph(void)
{
    struct ub u;
    dash_sample();
    ub_init(&u);
    if (B.ok) {
        ub_dec(&u, (uint64_t)B.soc_pct);
        ub_s(&u, " %");
    } else {
        ub_s(&u, U_DASH);
    }
    kv_bigc(B.ok ? (B.input ? "charging input present" : "on battery") : "fuel gauge not answering", u.b,
            B.ok && B.soc_pct < 20 ? C_RED : C_L1);
    kv_custom(560, draw_chart, 0, chart_hash(&ch_ma));
    kv_custom(560, draw_chart, 1, chart_hash(&ch_mv));
    kv_custom(560, draw_chart, 2, chart_hash(&ch_temp));
    kv_hdr("NOW");
    ub_init(&u);
    ub_sdec(&u, B.ma);
    ub_s(&u, " mA (avg ");
    ub_sdec(&u, B.avg_ma);
    ub_s(&u, ")");
    kv_add("Current", u.b);
    ub_init(&u);
    ub_fix(&u, (uint64_t)B.mv, 3);
    ub_s(&u, " V (avg ");
    ub_fix(&u, (uint64_t)B.avg_mv, 3);
    ub_s(&u, ")");
    kv_add("Voltage", u.b);
    kv_dec("Samples", (uint64_t)ch_ma.n, " (1 per second)");
}

void dash_battery_page(void)
{
    info_screen("Battery graph", fill_battery_graph, 1000);
}
