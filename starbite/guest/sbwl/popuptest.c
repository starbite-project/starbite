// SPDX-License-Identifier: GPL-2.0-only
/*
 * popuptest: the smallest Wayland client with an xdg popup, for sbwl's L2 check (docs/STARBITE-LINUX-APPS.md 9).
 * A toplevel filled blue (0x2040c0) at the size sbwl configures; after its first frame a popup (200x100, yellow
 * 0xffd700) anchored at (100, 100) of the window, gravity bottom-right. sbsim then expects yellow at (150, 150) and
 * blue at (500, 500) in the composited window. Prints its steps to stderr (sbwl sends that to the guest's log).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wl_surface *top_surf, *pop_surf;
static struct xdg_surface *top_xdg, *pop_xdg;
static struct xdg_toplevel *top;
static struct xdg_popup *popup;
static int width = 720, height = 1440, configured, popped;

static struct wl_buffer *fill(int w, int h, uint32_t argb)
{
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("popuptest", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0)
		return NULL;
	uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED)
		return NULL;
	for (int i = 0; i < w * h; ++i)
		px[i] = argb;
	munmap(px, (size_t)size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return b;
}

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = {.ping = wm_ping};

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
	(void)d, (void)ver;
	if (!strcmp(iface, wl_compositor_interface.name))
		comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, wl_shm_interface.name))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 2);
		xdg_wm_base_add_listener(wm, &wm_l, NULL);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d, (void)r, (void)name; }
static const struct wl_registry_listener reg_l = {.global = reg_global, .global_remove = reg_remove};

static void pop_configure(void *d, struct xdg_surface *x, uint32_t serial)
{
	(void)d;
	xdg_surface_ack_configure(x, serial);
	wl_surface_attach(pop_surf, fill(200, 100, 0xffffd700U), 0, 0);
	wl_surface_damage(pop_surf, 0, 0, 200, 100);
	wl_surface_commit(pop_surf);
	if (!popped++)
		fprintf(stderr, "popuptest: the popup is up (200x100 yellow at 100,100)\n");
}
static const struct xdg_surface_listener pop_xdg_l = {.configure = pop_configure};
static void popup_configure(void *d, struct xdg_popup *p, int32_t x, int32_t y, int32_t w, int32_t h)
{
	(void)d, (void)p;
	fprintf(stderr, "popuptest: popup placed at %d,%d %dx%d\n", x, y, w, h);
}
static void popup_done(void *d, struct xdg_popup *p) { (void)d, (void)p; }
static void popup_repositioned(void *d, struct xdg_popup *p, uint32_t t) { (void)d, (void)p, (void)t; }
static const struct xdg_popup_listener popup_l = {.configure = popup_configure, .popup_done = popup_done,
						   .repositioned = popup_repositioned};

static void open_popup(void)
{
	struct xdg_positioner *pos = xdg_wm_base_create_positioner(wm);
	xdg_positioner_set_size(pos, 200, 100);
	xdg_positioner_set_anchor_rect(pos, 100, 100, 1, 1);
	xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
	xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
	pop_surf = wl_compositor_create_surface(comp);
	pop_xdg = xdg_wm_base_get_xdg_surface(wm, pop_surf);
	xdg_surface_add_listener(pop_xdg, &pop_xdg_l, NULL);
	popup = xdg_surface_get_popup(pop_xdg, top_xdg, pos);
	xdg_popup_add_listener(popup, &popup_l, NULL);
	xdg_positioner_destroy(pos);
	wl_surface_commit(pop_surf);
}

static void top_configure(void *d, struct xdg_surface *x, uint32_t serial)
{
	(void)d;
	xdg_surface_ack_configure(x, serial);
	wl_surface_attach(top_surf, fill(width, height, 0xff2040c0U), 0, 0);
	wl_surface_damage(top_surf, 0, 0, width, height);
	wl_surface_commit(top_surf);
	if (!configured++) {
		fprintf(stderr, "popuptest: the window is up (%dx%d blue)\n", width, height);
		open_popup();
	}
}
static const struct xdg_surface_listener top_xdg_l = {.configure = top_configure};
static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s)
{
	(void)d, (void)t, (void)s;
	if (w > 0 && h > 0)
		width = w, height = h;
}
static void tl_close(void *d, struct xdg_toplevel *t) { (void)d, (void)t; exit(0); }
static void tl_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d, (void)t, (void)w, (void)h; }
static void tl_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d, (void)t, (void)c; }
static const struct xdg_toplevel_listener tl_l = {.configure = tl_configure, .close = tl_close,
						   .configure_bounds = tl_bounds, .wm_capabilities = tl_caps};

int main(void)
{
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "popuptest: no Wayland display\n");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_l, NULL);
	wl_display_roundtrip(dpy);
	if (!comp || !shm || !wm) {
		fprintf(stderr, "popuptest: missing globals\n");
		return 1;
	}
	top_surf = wl_compositor_create_surface(comp);
	top_xdg = xdg_wm_base_get_xdg_surface(wm, top_surf);
	xdg_surface_add_listener(top_xdg, &top_xdg_l, NULL);
	top = xdg_surface_get_toplevel(top_xdg);
	xdg_toplevel_add_listener(top, &tl_l, NULL);
	xdg_toplevel_set_title(top, "popuptest");
	wl_surface_commit(top_surf);
	while (wl_display_dispatch(dpy) != -1)
		;
	return 0;
}
