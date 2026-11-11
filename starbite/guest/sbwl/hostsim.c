// SPDX-License-Identifier: GPL-2.0-only
/*
 * hostsim: a user-space stand-in for the host (Starbite OS) to test sbwl without QEMU or the phone.
 * It creates a plain file the size of the shared region, fills the header as starbite/guest/host/sbghost.c load()
 * does (contract v2, the surface 720 x 1344 by default), starts sbwl with SBWL_SHM=<that file> (sbwl maps it instead
 * of /dev/sbshm; no doorbells: both sides poll), then plays a script of host messages and checks expectations.
 *   hostsim [-o OUTDIR] [-s SCRIPT] [-W 720] [-H 1344] -- sbwl [args...]
 * Every check prints "PASS ..." or "FAIL ...", the end "RESULT PASS|FAIL (n passed, m failed)".
 * Script lines (# comments; NAME = a catalog id, or a name, case-insensitive; T = timeout in ms):
 *   wait_hello T | wait_catalog T | catalog FILE | expect_app NAME | expect_no_app NAME | expect_icon NAME
 *   icon NAME FILE.png | expect_flag NAME FLAG 0|1 T (FLAG: running window terminal keyboard system)
 *   launch NAME | launch_stale NAME | activate NAME|none | close NAME | viewport ROWS WIDTH
 *   wait_window NAME 0|1 T          SBG_MSG_WINDOW with that app's index and b
 *   wait_frame T                    a new frame (surf_seq moved) after the last command
 *   dump FILE                       the front buffer: FILE.ppm and FILE.png (the viewport's rows)
 *   tap X Y | tap_file PATH KEY     (KEY: a line "KEY:x y w h" in PATH: a tap at its middle)
 *   type TEXT                       SBG_MSG_TEXT, 8 bytes at most per message, whole characters
 *   key CODE                        down + up
 *   expect_focus 0|1 T              the last SBG_MSG_TEXT_FOCUS
 *   expect_file PATH TEXT T         PATH contains TEXT
 *   expect_line PATH LINE T         a whole line of PATH is LINE
 *   expect_rows_beyond ROWS         no row at or past ROWS changed since the viewport went to ROWS
 *   expect_pixel X Y RRGGBB [TOL]   | expect_not_black X Y W H (some pixel in the box is not black)
 *   sock LINE                       one line to /run/sbwl.sock (as sbpkg)
 *   sock_listen | expect_sock LINE T  a client that stays connected / a line it got
 *   pkg IDX OP SEQ                  SBG_MSG_PKG
 *   wait_msg TYPE A T               any guest -> host message of TYPE (A: or * for any)
 *   sh CMD                          a shell command (set-up steps)
 *   sleep T | stop T
 */
#define _GNU_SOURCE
#include <cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../sbshm.h"

#define SHM_SIZE SBG_QEMU_SHM_SIZE

static uint8_t *shm;
static volatile struct sbshm_hdr *hdr;
static struct sbshm_ring *h2g, *g2h;
static volatile struct sbshm_cat *cat;
static uint32_t W = 720, H = 1344, VR;
static pid_t sbwl;
static int npass, nfail, hello, sock_fd = -1;
static uint32_t win_a = SBSHM_NONE, win_b, tf_a, tf_b, last_cat_seq, rows_floor;
static const char *outdir = ".";
static char sock_got[4096];
static size_t sock_n;

/* the last guest -> host messages, for wait_msg */
static struct sbshm_msg seen[256];
static unsigned nseen;

static uint64_t ms_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static void result(int ok, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("%s ", ok ? "PASS" : "FAIL");
	vprintf(fmt, ap);
	printf("\n");
	fflush(stdout);
	va_end(ap);
	ok ? ++npass : ++nfail;
}

static void note(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("hostsim: ");
	vprintf(fmt, ap);
	printf("\n");
	fflush(stdout);
	va_end(ap);
}

static const char *msg_name(unsigned t)
{
	switch (t) {
	case SBG_MSG_HELLO: return "HELLO";
	case SBG_MSG_BYE: return "BYE";
	case SBG_MSG_TITLE: return "TITLE";
	case SBG_MSG_CATALOG: return "CATALOG";
	case SBG_MSG_WINDOW: return "WINDOW";
	case SBG_MSG_TEXT_FOCUS: return "TEXT_FOCUS";
	case SBG_MSG_PKG_STATUS: return "PKG_STATUS";
	case SBG_MSG_STORE: return "STORE";
	default: return "?";
	}
}

static void pump(void)
{
	struct sbshm_msg m;
	while (sbshm_get(g2h, &m)) {
		seen[nseen++ % 256] = m;
		if (m.type == SBG_MSG_HELLO)
			hello = 1;
		else if (m.type == SBG_MSG_WINDOW)
			win_a = m.a, win_b = m.b;
		else if (m.type == SBG_MSG_TEXT_FOCUS)
			tf_a = m.a, tf_b = m.b;
		else if (m.type == SBG_MSG_CATALOG)
			last_cat_seq = m.a;
		if (m.type != SBG_MSG_CATALOG || getenv("HOSTSIM_VERBOSE"))
			note("<- %s a=%u b=%u c=%u%s%.32s", msg_name(m.type), m.a, m.b, m.c, m.type == SBG_MSG_TITLE ? " title " : "",
			     m.type == SBG_MSG_TITLE ? (const char *)hdr->title : "");
	}
	if (sock_fd >= 0) {
		ssize_t n = read(sock_fd, sock_got + sock_n, sizeof(sock_got) - 1 - sock_n);
		if (n > 0) {
			sock_n += (size_t)n;
			sock_got[sock_n] = 0;
		}
	}
	hdr->host_time_ms = ms_now();
	int st;
	if (sbwl > 0 && waitpid(sbwl, &st, WNOHANG) == sbwl) {
		note("sbwl ended (%s %d)", WIFSIGNALED(st) ? "signal" : "status", WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
		sbwl = 0;
	}
}

static void put(uint16_t type, uint32_t a, uint32_t b, uint32_t c, int32_t x, int32_t y)
{
	struct sbshm_msg m = {.type = type, .a = a, .b = b, .c = c, .x = x, .y = y, .t_ms = (uint32_t)ms_now()};
	while (!sbshm_put(h2g, &m)) {
		pump();
		usleep(1000);
	}
}

static void wait_ms(unsigned ms)
{
	uint64_t end = ms_now() + ms;
	while (ms_now() < end) {
		pump();
		usleep(5000);
	}
}

/* a whole copy of the catalog (seqlock) */
static struct sbshm_cat catc;
static int cat_read(void)
{
	for (int i = 0; i < 100; ++i) {
		uint32_t s = sbshm_seq_begin(&cat->seq);
		if (s & 1U) {
			usleep(1000);
			continue;
		}
		memcpy(&catc, (const void *)cat, sizeof(catc));
		if (sbshm_seq_ok(&cat->seq, s)) {
			catc.seq = s;
			return 1;
		}
	}
	return 0;
}

static int app_find(const char *name)
{
	if (!cat_read())
		return -1;
	for (uint32_t i = 0; i < catc.n && i < SBSHM_APPS_MAX; ++i)
		if (!strcasecmp(catc.app[i].id, name) || !strcasecmp(catc.app[i].name, name))
			return (int)i;
	return -1;
}

static uint32_t flag_bit(const char *f)
{
	return !strcmp(f, "running") ? SBSHM_APP_RUNNING : !strcmp(f, "window") ? SBSHM_APP_WINDOW :
	       !strcmp(f, "terminal") ? SBSHM_APP_TERMINAL : !strcmp(f, "keyboard") ? SBSHM_APP_KEYBOARD :
	       !strcmp(f, "system") ? SBSHM_APP_SYSTEM : 0;
}

static void flags_str(uint32_t f, char *s, size_t n)
{
	snprintf(s, n, "%s%s%s%s%s", f & SBSHM_APP_RUNNING ? "RUNNING " : "", f & SBSHM_APP_WINDOW ? "WINDOW " : "",
		 f & SBSHM_APP_TERMINAL ? "TERMINAL " : "", f & SBSHM_APP_KEYBOARD ? "KEYBOARD " : "",
		 f & SBSHM_APP_SYSTEM ? "SYSTEM " : "");
}

static void path_in_out(const char *f, char *p, size_t n) { snprintf(p, n, "%s%s%s", f[0] == '/' ? "" : outdir, f[0] == '/' ? "" : "/", f); }

static void cat_dump(const char *file)
{
	char p[512];
	path_in_out(file, p, sizeof(p));
	FILE *f = fopen(p, "w");
	if (!f || !cat_read()) {
		result(0, "catalog %s: cannot write or read", p);
		if (f)
			fclose(f);
		return;
	}
	fprintf(f, "seq %u, %u apps, active %d, MemTotal %u kB, MemAvailable %u kB, SwapTotal %u kB, SwapFree %u kB\n",
		catc.seq, catc.n, (int)catc.active, catc.mem_total_kb, catc.mem_avail_kb, catc.swap_total_kb, catc.swap_free_kb);
	for (uint32_t i = 0; i < catc.n && i < SBSHM_APPS_MAX; ++i) {
		char fl[80];
		flags_str(catc.app[i].flags, fl, sizeof(fl));
		fprintf(f, "%2u %-40s %-32s icon %-4d rss %6u kB  %s\n", i, catc.app[i].id, catc.app[i].name, (int)catc.app[i].icon,
			catc.app[i].rss_kb, fl);
	}
	fclose(f);
	note("catalog -> %s (%u apps, seq %u)", p, catc.n, catc.seq);
}

static void write_png(const char *file, const uint32_t *px, int w, int h, int stride_px, int argb)
{
	cairo_surface_t *s = cairo_image_surface_create(argb ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24, w, h);
	uint8_t *d = cairo_image_surface_get_data(s);
	int ds = cairo_image_surface_get_stride(s);
	for (int y = 0; y < h; ++y)
		memcpy(d + (size_t)y * ds, px + (size_t)y * stride_px, (size_t)w * 4);
	cairo_surface_mark_dirty(s);
	cairo_surface_write_to_png(s, file);
	cairo_surface_destroy(s);
}

static const uint32_t *front(void) { return (const uint32_t *)(shm + hdr->surf_off[hdr->surf_front & 1U]); }

static void dump(const char *file)
{
	char p[512], q[520];
	path_in_out(file, p, sizeof(p));
	uint32_t rows = VR;
	const uint32_t *px = front();
	snprintf(q, sizeof(q), "%s.ppm", p);
	FILE *f = fopen(q, "wb");
	if (!f) {
		result(0, "dump %s: %s", q, strerror(errno));
		return;
	}
	fprintf(f, "P6\n%u %u\n255\n", W, rows);
	for (uint32_t i = 0; i < W * rows; ++i) {
		uint8_t rgb[3] = {(uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i]};
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
	snprintf(q, sizeof(q), "%s.png", p);
	write_png(q, px, (int)W, (int)rows, (int)W, 0);
	note("dump -> %s.ppm / .png (%ux%u, frame %u)", p, W, rows, hdr->surf_seq);
}

static void icon_save(const char *name, const char *file)
{
	int i = app_find(name);
	if (i < 0 || catc.app[i].icon == SBSHM_NONE || catc.app[i].icon >= SBSHM_ICONS_MAX) {
		result(0, "icon %s: no such app or no icon", name);
		return;
	}
	char p[512];
	path_in_out(file, p, sizeof(p));
	write_png(p, (const uint32_t *)(shm + SBSHM_ICON_OFF + (size_t)catc.app[i].icon * SBSHM_ICON_BYTES), SBSHM_ICON_PX,
		  SBSHM_ICON_PX, SBSHM_ICON_PX, 1);
	note("icon of %s (pool %u) -> %s", name, catc.app[i].icon, p);
}

static int file_has(const char *path, const char *text)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	char b[4096];
	size_t n = fread(b, 1, sizeof(b) - 1, f);
	fclose(f);
	b[n] = 0;
	return strstr(b, text) != NULL;
}

static void tap(int x, int y)
{
	put(SBG_MSG_TOUCH, 0, SBG_TOUCH_DOWN, 0, x, y);
	wait_ms(40);
	put(SBG_MSG_TOUCH, 0, SBG_TOUCH_UP, 0, x, y);
	note("tap %d,%d", x, y);
}

static void type_text(const char *s)
{
	size_t n = strlen(s), i = 0;
	while (i < n) {
		size_t k = 0;
		while (i + k < n) {                    /* whole characters, 8 bytes at most */
			size_t cl = 1;
			uint8_t c = (uint8_t)s[i + k];
			cl = c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
			if (k + cl > 8)
				break;
			k += cl;
		}
		uint8_t b[8] = {0};
		memcpy(b, s + i, k);
		uint32_t a = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24, w = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
		put(SBG_MSG_TEXT, a, w, (uint32_t)k, 0, 0);
		i += k;
	}
	note("typed \"%s\"", s);
}

static int sock_connect(void)
{
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un sa = {.sun_family = AF_UNIX};
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", getenv("SBWL_SOCK") ? getenv("SBWL_SOCK") : "/run/sbwl.sock");
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa))) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	return fd;
}

static uint32_t frame_mark;

static void run_line(char *l)
{
	char c[32] = "", a1[256] = "", a2[512] = "", a3[64] = "";
	l[strcspn(l, "\r\n")] = 0;
	char *s = l;
	while (*s == ' ' || *s == '\t')
		++s;
	if (!*s || *s == '#')
		return;
	sscanf(s, "%31s %255s %511s %63s", c, a1, a2, a3);
	unsigned T;
	if (!strcmp(c, "wait_hello")) {
		T = (unsigned)atoi(a1);
		uint64_t end = ms_now() + T;
		while (!hello && ms_now() < end)
			wait_ms(10);
		result(hello, "HELLO from sbwl (guest_state %u)", hdr->guest_state);
	} else if (!strcmp(c, "wait_catalog")) {
		T = (unsigned)atoi(a1);
		uint64_t end = ms_now() + T;
		while (!(last_cat_seq && cat_read() && catc.n) && ms_now() < end)
			wait_ms(10);
		result(last_cat_seq && catc.n, "catalog: seq %u, %u apps", catc.seq, catc.n);
	} else if (!strcmp(c, "catalog")) {
		cat_dump(a1);
	} else if (!strcmp(c, "expect_app") || !strcmp(c, "expect_no_app")) {
		int i = app_find(a1), want = !strcmp(c, "expect_app");
		result((i >= 0) == want, "%s %s in the catalog (index %d)", a1, want ? "listed" : "not listed", i);
	} else if (!strcmp(c, "expect_icon")) {
		int i = app_find(a1);
		int ok = i >= 0 && catc.app[i].icon < SBSHM_ICONS_CAT;
		int opaque = 0;
		if (ok) {
			const uint32_t *px = (const uint32_t *)(shm + SBSHM_ICON_OFF + (size_t)catc.app[i].icon * SBSHM_ICON_BYTES);
			for (unsigned k = 0; k < SBSHM_ICON_PX * SBSHM_ICON_PX; ++k)
				opaque += (px[k] >> 24) > 0x80;
		}
		result(ok && opaque > 1000, "%s has an icon (pool %d, %d opaque px)", a1, ok ? (int)catc.app[i].icon : -1, opaque);
	} else if (!strcmp(c, "icon")) {
		icon_save(a1, a2);
	} else if (!strcmp(c, "expect_flag")) {
		uint32_t bit = flag_bit(a2), want = (uint32_t)atoi(a3);
		char *t = strrchr(s, ' ');
		T = t ? (unsigned)atoi(t + 1) : 0;
		uint64_t end = ms_now() + T;
		int i;
		for (;;) {
			i = app_find(a1);
			if (i >= 0 && !!(catc.app[i].flags & bit) == !!want)
				break;
			if (ms_now() >= end)
				break;
			wait_ms(20);
		}
		char fl[80] = "";
		if (i >= 0)
			flags_str(catc.app[i].flags, fl, sizeof(fl));
		result(i >= 0 && !!(catc.app[i].flags & bit) == !!want, "%s: %s %u (flags %s, rss %u kB)", a1, a2, want, fl,
		       i >= 0 ? catc.app[i].rss_kb : 0);
	} else if (!strcmp(c, "launch") || !strcmp(c, "launch_stale")) {
		int i = app_find(a1);
		if (i < 0) {
			result(0, "launch %s: not in the catalog", a1);
			return;
		}
		uint32_t seq = !strcmp(c, "launch") ? catc.seq : catc.seq - 1000;
		frame_mark = hdr->surf_seq;
		put(SBG_MSG_LAUNCH, (uint32_t)i, seq, 0, 0, 0);
		note("-> LAUNCH %d (%s) at seq %u", i, a1, seq);
	} else if (!strcmp(c, "activate")) {
		int i = !strcmp(a1, "none") ? -1 : app_find(a1);
		frame_mark = hdr->surf_seq;
		put(SBG_MSG_ACTIVATE, i < 0 ? SBSHM_NONE : (uint32_t)i, 0, 0, 0, 0);
		note("-> ACTIVATE %d (%s)", i, a1);
	} else if (!strcmp(c, "close")) {
		int i = app_find(a1);
		put(SBG_MSG_APP_CLOSE, (uint32_t)i, 0, 0, 0, 0);
		note("-> APP_CLOSE %d (%s)", i, a1);
	} else if (!strcmp(c, "viewport")) {
		VR = (uint32_t)atoi(a1);
		rows_floor = hdr->surf_seq;
		frame_mark = hdr->surf_seq;
		put(SBG_MSG_VIEWPORT, VR, (uint32_t)atoi(a2), 0, 0, 0);
		note("-> VIEWPORT %u rows, width %d", VR, atoi(a2));
	} else if (!strcmp(c, "wait_window")) {
		int i = app_find(a1);
		uint32_t want = (uint32_t)atoi(a2);
		T = (unsigned)atoi(a3);
		uint64_t end = ms_now() + T, t0 = ms_now();
		while (!(win_a == (uint32_t)i && win_b == want) && ms_now() < end)
			wait_ms(10);
		result(win_a == (uint32_t)i && win_b == want, "WINDOW %s (index %d) has window %u (got a=%d b=%u) after %llu ms", a1, i,
		       want, (int)win_a, win_b, (unsigned long long)(ms_now() - t0));
	} else if (!strcmp(c, "wait_frame")) {
		T = (unsigned)atoi(a1);
		uint64_t end = ms_now() + T, t0 = ms_now();
		while (hdr->surf_seq == frame_mark && ms_now() < end)
			wait_ms(10);
		wait_ms(300);                          /* the app's next frames settle */
		result(hdr->surf_seq != frame_mark, "a new frame (seq %u -> %u, %llu ms)", frame_mark, hdr->surf_seq,
		       (unsigned long long)(ms_now() - t0));
		frame_mark = hdr->surf_seq;
	} else if (!strcmp(c, "dump")) {
		dump(a1);
	} else if (!strcmp(c, "tap")) {
		frame_mark = hdr->surf_seq;
		tap(atoi(a1), atoi(a2));
	} else if (!strcmp(c, "tap_file")) {
		char b[4096] = "", key[80];
		FILE *f = fopen(a1, "r");
		size_t n = f ? fread(b, 1, sizeof(b) - 1, f) : 0;
		if (f)
			fclose(f);
		b[n] = 0;
		snprintf(key, sizeof(key), "%s:", a2);
		char *k = strstr(b, key);
		int x, y, w, h;
		if (!k || sscanf(k + strlen(key), "%d %d %d %d", &x, &y, &w, &h) != 4) {
			result(0, "tap_file %s %s: no such line", a1, a2);
			return;
		}
		frame_mark = hdr->surf_seq;
		tap(x + w / 2, y + h / 2);
	} else if (!strcmp(c, "type")) {
		frame_mark = hdr->surf_seq;
		type_text(s + 5);
	} else if (!strcmp(c, "key")) {
		put(SBG_MSG_KEY, (uint32_t)atoi(a1), 1, 0, 0, 0);
		put(SBG_MSG_KEY, (uint32_t)atoi(a1), 0, 0, 0, 0);
	} else if (!strcmp(c, "expect_focus")) {
		uint32_t want = (uint32_t)atoi(a1);
		T = (unsigned)atoi(a2);
		uint64_t end = ms_now() + T;
		while (tf_a != want && ms_now() < end)
			wait_ms(10);
		result(tf_a == want, "TEXT_FOCUS %u (purpose %u)", tf_a, tf_b);
	} else if (!strcmp(c, "expect_file")) {
		/* expect_file PATH TEXT... T: the text is everything between the path and the last word */
		char *p = s + strlen("expect_file") + 1 + strlen(a1) + 1, *t = strrchr(s, ' ');
		T = (unsigned)atoi(t + 1);
		char text[512];
		snprintf(text, sizeof(text), "%.*s", (int)(t - p), p);
		uint64_t end = ms_now() + T;
		while (!file_has(a1, text) && ms_now() < end)
			wait_ms(20);
		result(file_has(a1, text), "%s has \"%s\"", a1, text);
	} else if (!strcmp(c, "expect_line")) {
		/* expect_line PATH LINE... T: a whole line of PATH is LINE */
		char *p = s + strlen("expect_line") + 1 + strlen(a1) + 1, *t = strrchr(s, ' ');
		T = (unsigned)atoi(t + 1);
		char text[512];
		snprintf(text, sizeof(text), "\n%.*s\n", (int)(t - p), p);
		uint64_t end = ms_now() + T;
		int ok;
		for (;;) {
			char b[4096] = "\n";
			FILE *f = fopen(a1, "r");
			size_t n = f ? fread(b + 1, 1, sizeof(b) - 2, f) : 0;
			if (f)
				fclose(f);
			b[n + 1] = 0;
			ok = strstr(b, text) != NULL;
			if (ok || ms_now() >= end)
				break;
			wait_ms(20);
		}
		result(ok, "%s has the line \"%.*s\"", a1, (int)strlen(text) - 2, text + 1);
	} else if (!strcmp(c, "expect_rows_beyond")) {
		uint32_t r = (uint32_t)atoi(a1), bad = 0;
		volatile uint32_t *rows = (volatile uint32_t *)(shm + SBSHM_ROWS_OFF);
		for (uint32_t y = r; y < H; ++y)
			bad += sbshm_row_newer(rows[y], rows_floor);
		result(!bad && hdr->damage_h == r, "rows %u..%u untouched since the viewport (%u newer), damage_h %u", r, H - 1, bad,
		       hdr->damage_h);
	} else if (!strcmp(c, "expect_pixel")) {
		int x = atoi(a1), y = atoi(a2), tol = a3[0] ? 0 : 0;
		char *t = strrchr(s, ' ');
		unsigned want = (unsigned)strtoul(a3[0] ? a3 : t + 1, NULL, 16);
		(void)tol;
		uint32_t got = front()[(size_t)y * W + (size_t)x] & 0xffffffU;
		result(got == want, "pixel %d,%d = %06x (want %06x)", x, y, got, want);
	} else if (!strcmp(c, "expect_not_black")) {
		int x, y, w, h, n = 0;
		sscanf(s + 17, "%d %d %d %d", &x, &y, &w, &h);
		for (int j = y; j < y + h; ++j)
			for (int i = x; i < x + w; ++i)
				n += (front()[(size_t)j * W + (size_t)i] & 0xffffffU) != 0;
		result(n > 0, "%d non-black px in %d,%d %dx%d", n, x, y, w, h);
	} else if (!strcmp(c, "sock")) {
		int fd = sock_connect();
		const char *line = s + 5;
		char b[600];
		snprintf(b, sizeof(b), "%s\n", line);
		int ok = fd >= 0 && write(fd, b, strlen(b)) == (ssize_t)strlen(b);
		if (fd >= 0) {
			wait_ms(50);
			close(fd);
		}
		result(ok, "sbwl.sock <- \"%s\"", line);
	} else if (!strcmp(c, "sock_listen")) {
		sock_fd = sock_connect();
		if (sock_fd >= 0)
			fcntl(sock_fd, F_SETFL, O_NONBLOCK);
		result(sock_fd >= 0, "connected to sbwl.sock (as sbpkg)");
	} else if (!strcmp(c, "expect_sock")) {
		char *t = strrchr(s, ' ');
		T = (unsigned)atoi(t + 1);
		char text[256];
		snprintf(text, sizeof(text), "%.*s", (int)(t - (s + 12)), s + 12);
		uint64_t end = ms_now() + T;
		while (!strstr(sock_got, text) && ms_now() < end)
			wait_ms(10);
		result(strstr(sock_got, text) != NULL, "sbwl.sock -> \"%s\"", text);
	} else if (!strcmp(c, "pkg")) {
		put(SBG_MSG_PKG, (uint32_t)atoi(a1), (uint32_t)atoi(a2), (uint32_t)atoi(a3), 0, 0);
		note("-> PKG %s %s %s", a1, a2, a3);
	} else if (!strcmp(c, "wait_msg")) {
		unsigned type = (unsigned)atoi(a1), mark = nseen;
		(void)mark;
		T = (unsigned)atoi(a3);
		uint64_t end = ms_now() + T;
		int ok = 0;
		for (;;) {
			for (unsigned k = nseen > 256 ? nseen - 256 : 0; k < nseen && !ok; ++k) {
				const struct sbshm_msg *m = &seen[k % 256];
				ok = m->type == type && (a2[0] == '*' || m->a == (uint32_t)atoi(a2));
			}
			if (ok || ms_now() >= end)
				break;
			wait_ms(10);
		}
		result(ok, "message %s (%u) a=%s", msg_name(type), type, a2);
	} else if (!strcmp(c, "sh")) {
		int r = system(s + 3);
		note("sh %s: %d", s + 3, r);
	} else if (!strcmp(c, "sleep")) {
		wait_ms((unsigned)atoi(a1));
	} else if (!strcmp(c, "stop")) {
		put(SBG_MSG_STOP, 0, 0, 0, 0, 0);
		T = (unsigned)atoi(a1);
		uint64_t end = ms_now() + T;
		while (sbwl > 0 && ms_now() < end)
			wait_ms(10);
		result(sbwl == 0 && hdr->guest_state == SBG_GUEST_OFF, "sbwl stopped (guest_state %u)", hdr->guest_state);
	} else {
		note("unknown command: %s", s);
	}
}

int main(int argc, char **argv)
{
	const char *script = NULL;
	int o;
	while ((o = getopt(argc, argv, "o:s:W:H:")) != -1) {
		if (o == 'o')
			outdir = optarg;
		else if (o == 's')
			script = optarg;
		else if (o == 'W')
			W = (uint32_t)atoi(optarg);
		else if (o == 'H')
			H = (uint32_t)atoi(optarg);
	}
	if (optind >= argc) {
		fprintf(stderr, "hostsim [-o OUTDIR] [-s SCRIPT] [-W w] [-H h] -- sbwl [args]\n");
		return 2;
	}
	mkdir(outdir, 0755);
	VR = H;
	char path[512];
	snprintf(path, sizeof(path), "%s/shm.bin", outdir);
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0 || ftruncate(fd, SHM_SIZE)) {
		perror(path);
		return 1;
	}
	shm = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (shm == MAP_FAILED)
		return 1;
	/* sbghost.c load(): the header, version 2, the tables empty */
	uint64_t stride = (uint64_t)W * 4, surf = stride * H;
	if (SBSHM_SURF_OFF + 2 * surf > SBSHM_CAT_OFF) {
		fprintf(stderr, "hostsim: the surface does not fit below the v2 tables\n");
		return 1;
	}
	hdr = (volatile struct sbshm_hdr *)(shm + SBSHM_HDR_OFF);
	h2g = (struct sbshm_ring *)(shm + SBSHM_H2G_OFF);
	g2h = (struct sbshm_ring *)(shm + SBSHM_G2H_OFF);
	cat = (volatile struct sbshm_cat *)(shm + SBSHM_CAT_OFF);
	hdr->version = SBSHM_VERSION_V2;
	hdr->hdr_size = sizeof(struct sbshm_hdr);
	hdr->boot_count = 1;
	hdr->db_to_guest = hdr->db_to_host = 0;    /* poll */
	hdr->screen_on = 1;
	hdr->surf_w = W;
	hdr->surf_h = H;
	hdr->surf_stride = (uint32_t)stride;
	hdr->surf_format = SBSHM_FMT_XRGB8888;
	hdr->surf_nbuf = 2;
	hdr->surf_off[0] = SBSHM_SURF_OFF;
	hdr->surf_off[1] = (uint32_t)(SBSHM_SURF_OFF + surf);
	hdr->oops_off = SBSHM_OOPS_OFF;
	hdr->oops_size = SBSHM_OOPS_SIZE;
	hdr->host_state = SBG_HOST_STARTED;
	sbshm_dmb();
	hdr->magic = SBSHM_MAGIC;
	note("shared region %s (%u MiB), surface %ux%u, contract v2", path, (unsigned)(SHM_SIZE >> 20), W, H);

	sbwl = fork();
	if (!sbwl) {
		setenv("SBWL_SHM", path, 1);
		execv(argv[optind], argv + optind);
		_exit(127);
	}
	FILE *sf = script ? fopen(script, "r") : stdin;
	if (!sf) {
		perror(script);
		return 1;
	}
	char line[1024];
	while (fgets(line, sizeof(line), sf)) {
		pump();
		run_line(line);
	}
	if (sbwl > 0) {
		kill(sbwl, SIGTERM);
		wait_ms(500);
		if (sbwl > 0)
			kill(sbwl, SIGKILL);
	}
	printf("RESULT %s (%d passed, %d failed)\n", nfail || !npass ? "FAIL" : "PASS", npass, nfail);
	return nfail ? 1 : 0;
}
