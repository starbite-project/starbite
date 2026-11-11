// SPDX-License-Identifier: GPL-2.0-only
/*
 * sbwl: the guest's Wayland compositor for Starbite OS (docs/STARBITE-LINUX-APPS.md 3, milestone L1).
 * wlroots 0.17 (after its tinywl example), headless backend, pixman renderer: no GPU (the Mali stays Starbite's).
 *
 * One output = one app window slot: the output is the size Starbite gave the surface (hdr surf_w x surf_h), the
 * app's toplevel is maximized into it, so the output's frame IS the window (popups, subsurfaces and decorations
 * included, through wlroots' scene renderer). Every committed frame is published to Starbite exactly as guestd
 * does: the changed rows into the back buffer, row_seq for them, the flip (sbshm.h "damage per row").
 * Starbite's touch arrives as SBG_MSG_TOUCH (surface px) and becomes wl_touch for clients that bound it, pointer
 * motion + left button otherwise; its keys (SBG_MSG_KEY, Linux codes) go through an xkb keyboard (us layout).
 * sbwl owns the shared ring in this mode (guestd does not run): HELLO, the heartbeat, title, STOP (the app is
 * asked to close, sync, power off: sbguest.ko takes the cluster down).
 *   sbwl [app [args...]]          default: foot (restarted when it exits)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <drm_fourcc.h>
#include <xkbcommon/xkbcommon.h>

#include "../sbshm.h"

/* ---- the shared region (as guestd) ----------------------------------------------------------------------- */
static volatile struct sbshm_hdr *hdr;
static uint8_t *shm;
static struct sbshm_ring *h2g, *g2h;
static int sbfd = -1, kfd = -1;
static uint32_t W, H, stride;

static void say(const char *fmt, ...)
{
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b + 6, sizeof(b) - 6, fmt, ap);
	va_end(ap);
	memcpy(b, "sbwl: ", 6);
	n += 6;
	if (n > (int)sizeof(b) - 1)
		n = sizeof(b) - 1;
	if (kfd >= 0)
		(void)!write(kfd, b, (size_t)n);
}

static uint64_t ms_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static int map_shm(void)
{
	uint64_t base = 0, size = 0;
	unsigned char be[16];
	int fd = open("/proc/device-tree/chosen/starbite,shm", O_RDONLY);
	if (fd < 0 || read(fd, be, 16) != 16)
		return -1;
	close(fd);
	for (int i = 0; i < 8; ++i) {
		base = base << 8 | be[i];
		size = size << 8 | be[8 + i];
	}
	sbfd = open("/dev/sbshm", O_RDWR | O_CLOEXEC);
	if (sbfd < 0) {
		say("no /dev/sbshm (sbguest.ko): %s", strerror(errno));
		return -1;
	}
	shm = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, sbfd, 0);
	if (shm == MAP_FAILED)
		return -1;
	hdr = (volatile struct sbshm_hdr *)(shm + SBSHM_HDR_OFF);
	h2g = (struct sbshm_ring *)(shm + SBSHM_H2G_OFF);
	g2h = (struct sbshm_ring *)(shm + SBSHM_G2H_OFF);
	for (int i = 0; i < 50 && hdr->magic != SBSHM_MAGIC; ++i)
		usleep(100000);
	if (hdr->magic != SBSHM_MAGIC || hdr->surf_format != SBSHM_FMT_XRGB8888)
		return -1;
	W = hdr->surf_w;
	H = hdr->surf_h;
	stride = hdr->surf_stride;
	return 0;
}

static void g2h_send(uint16_t type, uint32_t a, uint32_t b)
{
	struct sbshm_msg m = {.type = type, .a = a, .b = b, .t_ms = (uint32_t)ms_now()};
	sbshm_put(g2h, &m);
	(void)!write(sbfd, "1", 1);
}

/* the frame into the back buffer, only the changed rows (guestd's publish, sbshm.h "damage per row") */
static uint32_t *shadow[2];
static uint64_t frames, rows_out;
static unsigned nframe, nbuilt, nbuf;
static uint32_t last_px;
static const void *last_buf;
static void publish(const uint8_t *src, uint32_t src_stride)
{
	volatile uint32_t *rows = (volatile uint32_t *)(shm + SBSHM_ROWS_OFF);
	size_t rb = (size_t)W * 4;
	if (!shadow[0]) {
		shadow[0] = calloc((size_t)W * H, 4);
		shadow[1] = calloc((size_t)W * H, 4);
		if (!shadow[0] || !shadow[1])
			return;
		hdr->rows_valid = H <= SBSHM_ROWS_MAX;
	}
	uint32_t front = hdr->surf_front & 1U, back = front ^ 1U, seq = hdr->surf_seq + 1U, n = 0;
	uint8_t *dst = shm + hdr->surf_off[back];
	for (uint32_t y = 0; y < H; ++y) {
		const uint8_t *s = src + (size_t)y * src_stride;
		int changed = !hdr->surf_seq || memcmp(s, shadow[front] + (size_t)y * W, rb);
		if (memcmp(s, shadow[back] + (size_t)y * W, rb)) {
			memcpy(dst + (size_t)y * stride, s, rb);
			memcpy(shadow[back] + (size_t)y * W, s, rb);
		}
		if (changed) {
			if (H <= SBSHM_ROWS_MAX)
				rows[y] = seq;
			++n;
		}
	}
	if (!n)
		return;
	sbshm_dmb();
	hdr->surf_front = back;
	hdr->damage_x = 0;
	hdr->damage_w = W;
	hdr->damage_y = 0;
	hdr->damage_h = H;
	sbshm_dmb();
	hdr->surf_seq = seq;
	frames++;
	rows_out += n;
}

/* ---- the compositor ------------------------------------------------------------------------------------ */
static struct {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output *scene_output;
	struct wlr_output *output;
	struct wlr_xdg_shell *xdg_shell;
	struct wlr_seat *seat;
	struct wlr_keyboard kbd;
	struct wlr_xdg_toplevel *top;            /* the app's window (the last mapped) */
	struct wl_listener new_output, output_frame, new_xdg_surface, kbd_key, kbd_mods;
	pid_t app;
	char **argv;
	int stopping, touch_down, ptr_down;
	struct wlr_surface *touch_surface;
} S;

/* the toplevel's committed buffer into the slot: 1 published (or nothing changed), 0 no window to show */
static uint32_t *winframe;
static int publish_window(void)
{
	static int why_logged;
	if (!S.top)
		return 0;
	/* the window's buffer as the scene holds it (the client buffer's source: the client's wl_shm buffer) */
	double sx, sy;
	struct wlr_scene_node *n = wlr_scene_node_at(&S.scene->tree.node, W / 2.0, H / 2.0, &sx, &sy);
	struct wlr_scene_buffer *sb = n && n->type == WLR_SCENE_NODE_BUFFER ? wlr_scene_buffer_from_node(n) : NULL;
	struct wlr_client_buffer *cb = sb && sb->buffer ? wlr_client_buffer_get(sb->buffer) : NULL;
	struct wlr_buffer *src = cb ? cb->source : (sb ? sb->buffer : NULL);
	if (!src) {
		if (!why_logged++)
			say("publish_window: no buffer under the window's centre (node %p, scene buffer %p, client buffer %p)",
			    (void *)n, (void *)sb, (void *)cb);
		return 0;
	}
	void *dp;
	uint32_t f;
	size_t bs;
	if (!wlr_buffer_begin_data_ptr_access(src, WLR_BUFFER_DATA_PTR_ACCESS_READ, &dp, &f, &bs)) {
		if (!why_logged++)
			say("publish_window: the window's buffer has no CPU access");
		return 0;
	}
	int ok = (f == DRM_FORMAT_ARGB8888 || f == DRM_FORMAT_XRGB8888);
	if (!ok && !why_logged++)
		say("publish_window: format %#x not copied", f);
	if (ok) {
		if (!winframe)
			winframe = calloc((size_t)W * H, 4);
		uint32_t w = (uint32_t)src->width < W ? (uint32_t)src->width : W;
		uint32_t h = (uint32_t)src->height < H ? (uint32_t)src->height : H;
		if (winframe) {
			for (uint32_t y = 0; y < H; ++y) {
				uint32_t *d = winframe + (size_t)y * W;
				if (y < h) {
					memcpy(d, (const uint8_t *)dp + (size_t)y * bs, (size_t)w * 4);
					for (uint32_t x = 0; x < w; ++x)
						d[x] |= 0xff000000U;          /* opaque: Starbite composites XRGB */
					for (uint32_t x = w; x < W; ++x)
						d[x] = 0xff000000U;
				} else {
					for (uint32_t x = 0; x < W; ++x)
						d[x] = 0xff000000U;
				}
			}
			last_px = winframe[(size_t)(H / 8) * W + W / 8];
			publish((const uint8_t *)winframe, W * 4);
		}
	}
	wlr_buffer_end_data_ptr_access(src);
	return ok;
}

static void output_frame(struct wl_listener *l, void *data)
{
	(void)l, (void)data;
	struct wlr_output_state st;
	wlr_output_state_init(&st);
	bool built = wlr_scene_output_build_state(S.scene_output, &st, NULL);
	nbuilt += built;
	nbuf += built && (st.committed & WLR_OUTPUT_STATE_BUFFER) && st.buffer;
	if (++nframe <= 3)
		say("frame event %u: built %d, buffer %d", nframe, built, (st.committed & WLR_OUTPUT_STATE_BUFFER) && st.buffer);
	/* L1: the toplevel's own committed buffer (exact for single-surface apps such as foot; wlroots 0.17's pixman
	 * scene left client textures out of the headless output here, and the output buffer's CPU access fails after
	 * the first frames: docs/STARBITE-LINUX-APPS.md 9) */
	int win = publish_window();
	if (built && !win) {
		if ((st.committed & WLR_OUTPUT_STATE_BUFFER) && st.buffer) {
			void *data_ptr;
			uint32_t fmt;
			size_t bstride;
			bool acc = wlr_buffer_begin_data_ptr_access(st.buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data_ptr, &fmt,
								  &bstride);
			if (!acc && nframe <= 3)
				say("the output buffer has no CPU access");
			if (acc) {
				uint64_t f0 = frames;
				last_buf = st.buffer;
				last_px = ((const uint32_t *)data_ptr)[(size_t)(H / 8) * (bstride / 4) + W / 8];
				publish(data_ptr, (uint32_t)bstride);
				if (!f0 && frames)
					say("first frame published (%ux%u, format %#x, stride %zu)", W, H, fmt, bstride);
				wlr_buffer_end_data_ptr_access(st.buffer);
			}
		}
	}
	if (built)
		wlr_output_commit_state(S.output, &st);
	wlr_output_state_finish(&st);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(S.scene_output, &now);
}

static void new_output(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_output *out = data;
	wlr_output_init_render(out, S.allocator, S.renderer);
	struct wlr_output_state st;
	wlr_output_state_init(&st);
	wlr_output_state_set_enabled(&st, true);
	wlr_output_state_set_custom_mode(&st, (int32_t)W, (int32_t)H, 60000);
	bool ok = wlr_output_commit_state(out, &st);
	wlr_output_state_finish(&st);
	S.output = out;
	say("output %s %dx%d: enable commit %s, enabled %d", out->name, out->width, out->height, ok ? "ok" : "FAILED",
	    out->enabled);
	S.output_frame.notify = output_frame;
	wl_signal_add(&out->events.frame, &S.output_frame);
	S.scene_output = wlr_scene_output_create(S.scene, out);
	wlr_scene_output_set_position(S.scene_output, 0, 0);
	wlr_output_create_global(out);
	wlr_output_schedule_frame(out);
}

/* the toplevel fills the slot; popups hang off their parent's scene tree */
struct view {
	struct wlr_xdg_surface *xdg;
	struct wlr_scene_tree *tree;
	struct wl_listener map, destroy, title;
};

static void focus(struct wlr_xdg_toplevel *t)
{
	S.top = t;
	wlr_xdg_toplevel_set_activated(t, true);
	wlr_seat_keyboard_notify_enter(S.seat, t->base->surface, S.kbd.keycodes, S.kbd.num_keycodes, &S.kbd.modifiers);
}

static void view_map(struct wl_listener *l, void *data)
{
	(void)data;
	struct view *v = wl_container_of(l, v, map);
	if (v->xdg->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL)
		return;
	focus(v->xdg->toplevel);
	struct wlr_surface *su = v->xdg->surface;
	say("window mapped: %s, surface %dx%d, buffer %p, tree enabled %d", v->xdg->toplevel->title ? v->xdg->toplevel->title : "(no title)",
	    su->current.width, su->current.height, (void *)su->buffer, v->tree->node.enabled);
}

static void view_title(struct wl_listener *l, void *data)
{
	(void)data;
	struct view *v = wl_container_of(l, v, title);
	const char *t = v->xdg->toplevel->title;
	if (!t)
		return;
	for (unsigned i = 0; i < sizeof(hdr->title) - 1; ++i) {
		hdr->title[i] = t[i];
		if (!t[i])
			break;
	}
	hdr->title[sizeof(hdr->title) - 1] = 0;
	g2h_send(SBG_MSG_TITLE, 0, 0);
}

static void view_destroy(struct wl_listener *l, void *data)
{
	(void)data;
	struct view *v = wl_container_of(l, v, destroy);
	if (v->xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL && S.top == v->xdg->toplevel)
		S.top = NULL;
	wl_list_remove(&v->map.link);
	wl_list_remove(&v->destroy.link);
	if (v->xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL)
		wl_list_remove(&v->title.link);
	free(v);
}

static void new_xdg_surface(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_xdg_surface *xdg = data;
	say("new xdg surface (%s)", xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL ? "toplevel" : "popup");
	struct view *v = calloc(1, sizeof(*v));
	v->xdg = xdg;
	if (xdg->role == WLR_XDG_SURFACE_ROLE_POPUP) {
		struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg->popup->parent);
		struct wlr_scene_tree *pt = parent ? parent->data : &S.scene->tree;
		v->tree = wlr_scene_xdg_surface_create(pt, xdg);
	} else {
		v->tree = wlr_scene_xdg_surface_create(&S.scene->tree, xdg);
		wlr_xdg_toplevel_set_size(xdg->toplevel, (int32_t)W, (int32_t)H);
		wlr_xdg_toplevel_set_maximized(xdg->toplevel, true);
		v->title.notify = view_title;
		wl_signal_add(&xdg->toplevel->events.set_title, &v->title);
	}
	xdg->data = v->tree;
	v->map.notify = view_map;
	wl_signal_add(&xdg->surface->events.map, &v->map);
	v->destroy.notify = view_destroy;
	wl_signal_add(&xdg->events.destroy, &v->destroy);
}

/* ---- input from Starbite ------------------------------------------------------------------------------ */
static void kbd_key(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_keyboard_key_event *e = data;
	wlr_seat_set_keyboard(S.seat, &S.kbd);
	wlr_seat_keyboard_notify_key(S.seat, e->time_msec, e->keycode, e->state);
}

static void kbd_mods(struct wl_listener *l, void *data)
{
	(void)l, (void)data;
	wlr_seat_set_keyboard(S.seat, &S.kbd);
	wlr_seat_keyboard_notify_modifiers(S.seat, &S.kbd.modifiers);
}

static struct wlr_surface *surface_at(double x, double y, double *sx, double *sy)
{
	struct wlr_scene_node *n = wlr_scene_node_at(&S.scene->tree.node, x, y, sx, sy);
	if (!n || n->type != WLR_SCENE_NODE_BUFFER)
		return NULL;
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(n));
	return ss ? ss->surface : NULL;
}

static int client_has_touch(struct wlr_surface *s)
{
	struct wlr_seat_client *c = wlr_seat_client_for_wl_client(S.seat, wl_resource_get_client(s->resource));
	return c && !wl_list_empty(&c->touches);
}

static void touch(uint32_t phase, int32_t x, int32_t y, uint32_t t)
{
	double sx = 0, sy = 0;
	struct wlr_surface *s = surface_at(x, y, &sx, &sy);
	if (phase == SBG_TOUCH_DOWN) {
		if (!s) {
			say("touch at %d,%d: no surface there", x, y);
			return;
		}
		say("touch at %d,%d: surface %p at %.0f,%.0f, as %s", x, y, (void *)s, sx, sy,
		    client_has_touch(s) ? "wl_touch" : "pointer (the client has no wl_touch)");
		if (client_has_touch(s)) {
			S.touch_surface = s;
			S.touch_down = 1;
			wlr_seat_touch_notify_down(S.seat, s, t, 0, sx, sy);
			wlr_seat_touch_notify_frame(S.seat);
		} else {                                       /* pointer emulation: move there, press */
			wlr_seat_pointer_notify_enter(S.seat, s, sx, sy);
			wlr_seat_pointer_notify_motion(S.seat, t, sx, sy);
			wlr_seat_pointer_notify_button(S.seat, t, BTN_LEFT, WLR_BUTTON_PRESSED);
			wlr_seat_pointer_notify_frame(S.seat);
			S.ptr_down = 1;
		}
		return;
	}
	if (S.touch_down) {
		if (phase == SBG_TOUCH_MOVE) {
			wlr_seat_touch_notify_motion(S.seat, t, 0, sx, sy);
		} else {
			wlr_seat_touch_notify_up(S.seat, t, 0);
			S.touch_down = 0;
		}
		wlr_seat_touch_notify_frame(S.seat);
	} else if (S.ptr_down) {
		if (s)
			wlr_seat_pointer_notify_motion(S.seat, t, sx, sy);
		if (phase == SBG_TOUCH_UP) {
			wlr_seat_pointer_notify_button(S.seat, t, BTN_LEFT, WLR_BUTTON_RELEASED);
			S.ptr_down = 0;
		}
		wlr_seat_pointer_notify_frame(S.seat);
	}
}

static void key(uint32_t code, int down, uint32_t t)
{
	struct wlr_keyboard_key_event e = {
		.time_msec = t, .keycode = code, .update_state = true,
		.state = down ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED,
	};
	wlr_keyboard_notify_key(&S.kbd, &e);
}

static void stop_now(void)
{
	if (S.stopping)
		return;
	S.stopping = 1;
	hdr->guest_state = SBG_GUEST_STOPPING;
	say("MSG_STOP: closing the app, syncing, powering the M3 cluster off");
	g2h_send(SBG_MSG_BYE, 0, 0);
	if (S.top)
		wlr_xdg_toplevel_send_close(S.top);
	wl_display_flush_clients(S.display);
	if (S.app > 0) {
		for (int i = 0; i < 20 && waitpid(S.app, NULL, WNOHANG) == 0; ++i)
			usleep(25000);
		kill(S.app, SIGKILL);
	}
	sync();
	reboot(RB_POWER_OFF);                                  /* sbguest.ko: CPU_OFF on every guest CPU */
}

static uint32_t touches;
static void drain(void)
{
	struct sbshm_msg m;
	while (sbshm_get(h2g, &m)) {
		if (m.type == SBG_MSG_TOUCH) {
			touch(m.b, m.x, m.y, m.t_ms);
			hdr->touches_seen = ++touches;
		} else if (m.type == SBG_MSG_KEY) {
			key(m.a, (int)m.b, m.t_ms);
		} else if (m.type == SBG_MSG_STOP) {
			stop_now();
		}
	}
}

static uint64_t t0;
static struct wl_event_source *tick_src;
static int tick(void *data)
{
	(void)data;
	drain();
	hdr->guest_heartbeat++;
	hdr->guest_uptime_ms = ms_now() - t0;
	static uint64_t t_stat;
	if (ms_now() - t_stat >= 10000) {
		t_stat = ms_now();
		say("stats: frame events %u, built %u, with a buffer %u, published %llu, rows %llu, px %08x, buffer %p %dx%d",
		    nframe, nbuilt, nbuf, (unsigned long long)frames, (unsigned long long)rows_out, last_px, last_buf,
		    last_buf ? ((const struct wlr_buffer *)last_buf)->width : 0,
		    last_buf ? ((const struct wlr_buffer *)last_buf)->height : 0);
	}
	/* the app: started again when it ends (the window must not vanish under Starbite) */
	if (S.app > 0 && waitpid(S.app, NULL, WNOHANG) == S.app && !S.stopping) {
		say("the app ended: starting it again");
		S.app = 0;
	}
	if (S.app <= 0 && !S.stopping) {
		static uint64_t last_start;
		static unsigned quick;
		uint64_t now = ms_now();
		if (last_start && now - last_start < 5000)
			++quick;
		else
			quick = 0;
		if (quick >= 3 && now - last_start < 30000U)   /* ended three times in a row within 5 s: back off */
			return 0 * wl_event_source_timer_update(tick_src, 10);
		last_start = now;
		S.app = fork();
		if (S.app == 0) {
			setsid();
			int k = open("/dev/kmsg", O_WRONLY);       /* the app's complaints into the guest's log */
			if (k >= 0) {
				dup2(k, 1);
				dup2(k, 2);
			}
			execvp(S.argv[0], S.argv);
			_exit(127);
		}
	}
	wl_event_source_timer_update(tick_src, 10);
	return 0;
}

static int doorbell(int fd, uint32_t mask, void *data)
{
	(void)mask, (void)data;
	uint32_t n;
	(void)!read(fd, &n, sizeof(n));
	drain();
	return 0;
}

int main(int argc, char **argv)
{
	static char *def[] = {"foot", "-o", "csd.preferred=none", NULL};
	S.argv = argc > 1 ? argv + 1 : def;
	kfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	wlr_log_init(WLR_ERROR, NULL);
	if (map_shm() < 0) {
		say("no shared region");
		return 1;
	}
	t0 = ms_now();
	hdr->guest_state = SBG_GUEST_UP;
	setenv("XDG_RUNTIME_DIR", "/run", 0);

	S.display = wl_display_create();
	S.loop = wl_display_get_event_loop(S.display);
	S.backend = wlr_headless_backend_create(S.display);
	S.renderer = wlr_pixman_renderer_create();
	S.allocator = S.backend && S.renderer ? wlr_allocator_autocreate(S.backend, S.renderer) : NULL;
	if (!S.allocator) {
		say("no headless backend / pixman renderer / allocator");
		return 1;
	}
	wlr_renderer_init_wl_display(S.renderer, S.display);
	wlr_compositor_create(S.display, 5, S.renderer);
	wlr_subcompositor_create(S.display);
	wlr_data_device_manager_create(S.display);
	S.scene = wlr_scene_create();

	S.new_output.notify = new_output;
	wl_signal_add(&S.backend->events.new_output, &S.new_output);
	S.xdg_shell = wlr_xdg_shell_create(S.display, 3);
	S.new_xdg_surface.notify = new_xdg_surface;
	wl_signal_add(&S.xdg_shell->events.new_surface, &S.new_xdg_surface);

	S.seat = wlr_seat_create(S.display, "seat0");
	wlr_seat_set_capabilities(S.seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_TOUCH);
	static const struct wlr_keyboard_impl kimpl = {.name = "starbite-keys"};
	wlr_keyboard_init(&S.kbd, &kimpl, "starbite-keys");
	struct xkb_context *xc = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_rule_names names = {.layout = "us"};
	struct xkb_keymap *km = xc ? xkb_keymap_new_from_names(xc, &names, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
	if (km)
		wlr_keyboard_set_keymap(&S.kbd, km);
	else
		say("no xkb keymap (/usr/share/X11/xkb): keys will not reach apps");
	S.kbd_key.notify = kbd_key;
	wl_signal_add(&S.kbd.events.key, &S.kbd_key);
	S.kbd_mods.notify = kbd_mods;
	wl_signal_add(&S.kbd.events.modifiers, &S.kbd_mods);
	wlr_seat_set_keyboard(S.seat, &S.kbd);

	const char *sock = wl_display_add_socket_auto(S.display);
	if (!sock || !wlr_backend_start(S.backend)) {
		say("no Wayland socket or the backend did not start");
		return 1;
	}
	wlr_headless_add_output(S.backend, W, H);
	setenv("WAYLAND_DISPLAY", sock, 1);
	unsetenv("DISPLAY");

	tick_src = wl_event_loop_add_timer(S.loop, tick, NULL);
	wl_event_source_timer_update(tick_src, 10);
	wl_event_loop_add_fd(S.loop, sbfd, WL_EVENT_READABLE, doorbell, NULL);

	struct utsname u;
	uname(&u);
	for (unsigned i = 0; i < 63 && u.release[i]; ++i)
		hdr->guest_kver[i] = u.release[i];
	hdr->guest_ncpus = (uint32_t)sysconf(_SC_NPROCESSORS_ONLN);
	hdr->guest_state = SBG_GUEST_RUNNING;
	g2h_send(SBG_MSG_HELLO, hdr->guest_ncpus, 1);
	say("HELLO: Wayland %s, output %ux%u, app %s, kernel %s", sock, W, H, S.argv[0], u.release);
	wl_display_run(S.display);
	return 0;
}
