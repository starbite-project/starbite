// SPDX-License-Identifier: GPL-2.0-only
/*
 * wltest: hostsim's plain Wayland client (no toolkit: it builds where GTK does not, the Ubuntu VM's wlroots 0.17).
 *   sbwl-wltest idle|frame|timer
 * A toplevel filled with a colour at the size sbwl configures. idle: drawn on configure only; frame: redrawn at every
 * wl_surface.frame (as toolkits pace themselves); timer: redrawn every 16 ms whether shown or not (a client that
 * ignores frame callbacks). With wl_keyboard and wl_data_device it records to $SBWL_TEST_OUT (default
 * /tmp/sbwl-wltest.txt, appended, one line per event):
 *   key:<code> mods:<depressed mods>     every key press (mods: Shift 1, Control 4, Mod1 8, Mod4 64)
 *   copied:<text>                        Ctrl+C: it sets the selection to $SBWL_TEST_COPY (default "wltest copy ✓")
 *   pasted:<text>                        Ctrl+V: the selection's text/plain;charset=utf-8, read through a pipe
 *   offer:<mime list>                    the selection it was given (on keyboard focus / a change)
 * App id: $SBWL_TEST_APPID (default org.starbite.WlTest).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wl_seat *seat;
static struct wl_keyboard *kbd;
static struct wl_data_device_manager *ddm;
static struct wl_data_device *ddev;
static struct wl_data_offer *sel_offer;
static struct wl_surface *surf;
static struct xdg_surface *xsurf;
static struct xdg_toplevel *top;
static int width = 720, height = 1344, configured, mode;   /* 0 idle, 1 frame, 2 timer */
static uint32_t mods, nframes;
static char offer_mimes[512];

static void record(const char *fmt, ...)
{
	const char *o = getenv("SBWL_TEST_OUT") ? getenv("SBWL_TEST_OUT") : "/tmp/sbwl-wltest.txt";
	FILE *f = fopen(o, "a");
	if (!f)
		return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

/* ---- drawing: two shm buffers, a colour that moves with the frame count ---- */
struct buf {
	struct wl_buffer *b;
	uint32_t *px;
	int w, h, busy;
};
static struct buf bufs[2];
static void buf_release(void *d, struct wl_buffer *b) { (void)b; ((struct buf *)d)->busy = 0; }
static const struct wl_buffer_listener buf_l = {.release = buf_release};

static struct buf *get_buf(void)
{
	for (int i = 0; i < 2; ++i) {
		struct buf *b = &bufs[i];
		if (b->busy)
			continue;
		if (b->b && (b->w != width || b->h != height)) {
			wl_buffer_destroy(b->b);
			munmap(b->px, (size_t)b->w * b->h * 4);
			b->b = NULL;
		}
		if (!b->b) {
			int size = width * height * 4;
			int fd = memfd_create("wltest", MFD_CLOEXEC);
			if (fd < 0 || ftruncate(fd, size) < 0)
				return NULL;
			b->px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
			b->b = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_XRGB8888);
			wl_shm_pool_destroy(pool);
			close(fd);
			wl_buffer_add_listener(b->b, &buf_l, b);
			b->w = width, b->h = height;
		}
		return b;
	}
	return NULL;
}

static void draw(void);
static void frame_done(void *d, struct wl_callback *cb, uint32_t t)
{
	(void)d, (void)t;
	wl_callback_destroy(cb);
	draw();
}
static const struct wl_callback_listener frame_l = {.done = frame_done};

static void draw(void)
{
	struct buf *b = get_buf();
	if (!b)
		return;
	uint32_t c = 0x204060U + ((nframes * 3U) & 0xffU) * 0x10000U;
	for (int i = 0; i < width * height; ++i)
		b->px[i] = c;
	for (int y = 0; y < 16 && y < height; ++y)     /* cp7: the top-left corner marked (where the content's origin went) */
		for (int x = 0; x < 32 && x < width; ++x)
			b->px[y * width + x] = 0xffffffU;
	nframes++;
	if (mode == 1)
		wl_callback_add_listener(wl_surface_frame(surf), &frame_l, NULL);
	wl_surface_attach(surf, b->b, 0, 0);
	wl_surface_damage_buffer(surf, 0, 0, width, height);
	wl_surface_commit(surf);
	b->busy = 1;
}

/* ---- clipboard ---- */
static void src_target(void *d, struct wl_data_source *s, const char *m) { (void)d, (void)s, (void)m; }
static void src_send(void *d, struct wl_data_source *s, const char *mime, int32_t fd)
{
	(void)s, (void)mime;
	const char *t = d;
	(void)!write(fd, t, strlen(t));
	close(fd);
}
static void src_cancelled(void *d, struct wl_data_source *s) { (void)d; wl_data_source_destroy(s); }
static const struct wl_data_source_listener src_l = {.target = src_target, .send = src_send, .cancelled = src_cancelled};

static void copy(uint32_t serial)
{
	static char text[256];
	snprintf(text, sizeof(text), "%s", getenv("SBWL_TEST_COPY") ? getenv("SBWL_TEST_COPY") : "wltest copy ✓");
	struct wl_data_source *s = wl_data_device_manager_create_data_source(ddm);
	wl_data_source_add_listener(s, &src_l, text);
	wl_data_source_offer(s, "text/plain;charset=utf-8");
	wl_data_source_offer(s, "UTF8_STRING");
	wl_data_device_set_selection(ddev, s, serial);
	record("copied:%s", text);
}

static void paste(void)
{
	if (!sel_offer || !strstr(offer_mimes, "text/plain;charset=utf-8")) {
		record("pasted:(nothing: offer %s)", offer_mimes);
		return;
	}
	int p[2];
	if (pipe2(p, O_CLOEXEC) < 0)
		return;
	wl_data_offer_receive(sel_offer, "text/plain;charset=utf-8", p[1]);
	close(p[1]);
	wl_display_flush(dpy);
	char b[8192];
	size_t n = 0;
	int waited = 0;
	struct pollfd pf = {.fd = p[0], .events = POLLIN};
	while (n < sizeof(b) - 1) {                    /* the compositor writes from its event loop */
		if (poll(&pf, 1, 20) > 0) {
			ssize_t r = read(p[0], b + n, sizeof(b) - 1 - n);
			if (r <= 0)
				break;
			n += (size_t)r;
		} else {
			if (++waited > 150)                   /* 3 s */
				break;
		}
	}
	close(p[0]);
	b[n] = 0;
	record("pasted:%s", b);
}

static void offer_offer(void *d, struct wl_data_offer *o, const char *mime)
{
	(void)d, (void)o;
	size_t l = strlen(offer_mimes);
	snprintf(offer_mimes + l, sizeof(offer_mimes) - l, "%s%s", l ? "," : "", mime);
}
static void offer_src_actions(void *d, struct wl_data_offer *o, uint32_t a) { (void)d, (void)o, (void)a; }
static void offer_action(void *d, struct wl_data_offer *o, uint32_t a) { (void)d, (void)o, (void)a; }
static const struct wl_data_offer_listener offer_l = {.offer = offer_offer, .source_actions = offer_src_actions,
						       .action = offer_action};
static void dd_data_offer(void *d, struct wl_data_device *dd, struct wl_data_offer *o)
{
	(void)d, (void)dd;
	offer_mimes[0] = 0;
	wl_data_offer_add_listener(o, &offer_l, NULL);
}
static void dd_enter(void *d, struct wl_data_device *dd, uint32_t s, struct wl_surface *su, wl_fixed_t x, wl_fixed_t y,
		     struct wl_data_offer *o)
{
	(void)d, (void)dd, (void)s, (void)su, (void)x, (void)y, (void)o;
}
static void dd_leave(void *d, struct wl_data_device *dd) { (void)d, (void)dd; }
static void dd_motion(void *d, struct wl_data_device *dd, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
	(void)d, (void)dd, (void)t, (void)x, (void)y;
}
static void dd_drop(void *d, struct wl_data_device *dd) { (void)d, (void)dd; }
static void dd_selection(void *d, struct wl_data_device *dd, struct wl_data_offer *o)
{
	(void)d, (void)dd;
	if (sel_offer && sel_offer != o)
		wl_data_offer_destroy(sel_offer);
	sel_offer = o;
	record("offer:%s", o ? offer_mimes : "(none)");
}
static const struct wl_data_device_listener dd_l = {.data_offer = dd_data_offer, .enter = dd_enter, .leave = dd_leave,
						     .motion = dd_motion, .drop = dd_drop, .selection = dd_selection};

/* ---- keyboard: raw codes, the Control bit of the standard modifier map (4) ---- */
static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t sz)
{
	(void)d, (void)k, (void)f, (void)sz;
	close(fd);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *su, struct wl_array *keys)
{
	(void)d, (void)k, (void)s, (void)su, (void)keys;
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *su) { (void)d, (void)k, (void)s, (void)su; }
static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t state)
{
	(void)d, (void)k, (void)t;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
		return;
	record("key:%u mods:%u", key, mods);
	if ((mods & 4U) && key == KEY_C)
		copy(serial);
	else if ((mods & 4U) && key == KEY_V)
		paste();
}
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t g)
{
	(void)d, (void)k, (void)s, (void)lat, (void)lock, (void)g;
	mods = dep;
}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t de) { (void)d, (void)k, (void)r, (void)de; }
static const struct wl_keyboard_listener kb_l = {.keymap = kb_keymap, .enter = kb_enter, .leave = kb_leave, .key = kb_key,
						  .modifiers = kb_mods, .repeat_info = kb_repeat};

/* the pointer (sbwl's emulation of a tap): "press:X,Y" in surface coordinates (cp7: landscape's mapped taps) */
static struct wl_pointer *ptr;
static double ptr_x, ptr_y;
static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	(void)d, (void)p, (void)serial, (void)s;
	ptr_x = wl_fixed_to_double(x), ptr_y = wl_fixed_to_double(y);
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) { (void)d, (void)p, (void)serial, (void)s; }
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
	(void)d, (void)p, (void)t;
	ptr_x = wl_fixed_to_double(x), ptr_y = wl_fixed_to_double(y);
}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t b, uint32_t st)
{
	(void)d, (void)p, (void)serial, (void)t, (void)b;
	if (st == WL_POINTER_BUTTON_STATE_PRESSED)
		record("press:%d,%d", (int)ptr_x, (int)ptr_y);
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) { (void)d, (void)p, (void)t, (void)a, (void)v; }
static void p_frame(void *d, struct wl_pointer *p) { (void)d, (void)p; }
static void p_asrc(void *d, struct wl_pointer *p, uint32_t s) { (void)d, (void)p, (void)s; }
static void p_astop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d, (void)p, (void)t, (void)a; }
static void p_adisc(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d, (void)p, (void)a, (void)v; }
static const struct wl_pointer_listener ptr_l = {.enter = p_enter, .leave = p_leave, .motion = p_motion,
						 .button = p_button, .axis = p_axis, .frame = p_frame,
						 .axis_source = p_asrc, .axis_stop = p_astop, .axis_discrete = p_adisc};
static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
	(void)d;
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !kbd) {
		kbd = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(kbd, &kb_l, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ptr) {
		ptr = wl_seat_get_pointer(s);
		wl_pointer_add_listener(ptr, &ptr_l, NULL);
	}
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d, (void)s, (void)n; }
static const struct wl_seat_listener seat_l = {.capabilities = seat_caps, .name = seat_name};

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = {.ping = wm_ping};

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
	(void)d, (void)ver;
	if (!strcmp(iface, wl_compositor_interface.name))
		comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, wl_shm_interface.name))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, wl_seat_interface.name)) {
		seat = wl_registry_bind(r, name, &wl_seat_interface, 5);
		wl_seat_add_listener(seat, &seat_l, NULL);
	} else if (!strcmp(iface, wl_data_device_manager_interface.name))
		ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, 3);
	else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 2);
		xdg_wm_base_add_listener(wm, &wm_l, NULL);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d, (void)r, (void)name; }
static const struct wl_registry_listener reg_l = {.global = reg_global, .global_remove = reg_remove};

static void xs_configure(void *d, struct xdg_surface *x, uint32_t serial)
{
	(void)d;
	xdg_surface_ack_configure(x, serial);
	if (!configured++ || mode == 0)
		draw();
}
static const struct xdg_surface_listener xs_l = {.configure = xs_configure};
static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s)
{
	(void)d, (void)t, (void)s;
	if (w > 0 && h > 0 && (w != width || h != height))
		record("configure:%dx%d", w, h);
	if (w > 0 && h > 0)
		width = w, height = h;
}
static void tl_close(void *d, struct xdg_toplevel *t) { (void)d, (void)t; exit(0); }
static void tl_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d, (void)t, (void)w, (void)h; }
static void tl_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d, (void)t, (void)c; }
static const struct xdg_toplevel_listener tl_l = {.configure = tl_configure, .close = tl_close,
						   .configure_bounds = tl_bounds, .wm_capabilities = tl_caps};

int main(int argc, char **argv)
{
	mode = argc > 1 && !strcmp(argv[1], "frame") ? 1 : argc > 1 && !strcmp(argv[1], "timer") ? 2 : 0;
	dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "wltest: no Wayland display\n");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_l, NULL);
	wl_display_roundtrip(dpy);
	if (!comp || !shm || !wm || !seat || !ddm) {
		fprintf(stderr, "wltest: missing globals\n");
		return 1;
	}
	ddev = wl_data_device_manager_get_data_device(ddm, seat);
	wl_data_device_add_listener(ddev, &dd_l, NULL);
	surf = wl_compositor_create_surface(comp);
	xsurf = xdg_wm_base_get_xdg_surface(wm, surf);
	xdg_surface_add_listener(xsurf, &xs_l, NULL);
	top = xdg_surface_get_toplevel(xsurf);
	xdg_toplevel_add_listener(top, &tl_l, NULL);
	xdg_toplevel_set_title(top, "wltest");
	xdg_toplevel_set_app_id(top, getenv("SBWL_TEST_APPID") ? getenv("SBWL_TEST_APPID") : "org.starbite.WlTest");
	wl_surface_commit(surf);
	struct pollfd pf = {.fd = wl_display_get_fd(dpy), .events = POLLIN};
	for (;;) {
		while (wl_display_prepare_read(dpy) != 0)
			wl_display_dispatch_pending(dpy);
		wl_display_flush(dpy);
		int r = poll(&pf, 1, mode == 2 ? 16 : -1);
		if (r > 0) {
			if (wl_display_read_events(dpy) < 0)
				break;
		} else {
			wl_display_cancel_read(dpy);
		}
		if (wl_display_dispatch_pending(dpy) < 0)
			break;
		if (mode == 2 && configured && r == 0)
			draw();
	}
	return 0;
}
