// SPDX-License-Identifier: GPL-2.0-only
/*
 * sbwl: the guest's Wayland compositor for Starbite OS (docs/STARBITE-LINUX-APPS.md 3-5, milestones L1-L3).
 * wlroots 0.17 (Ubuntu 24.04: the L1/L2 initramfs) or 0.18 (Debian trixie: the desktop root), after tinywl;
 * headless backend, pixman renderer: no GPU (the Mali stays Starbite's).
 *
 * The shared surface shows ONE app at a time, the active one: its toplevels (the main window maximized to the
 * viewport, dialogs centered over it, the most recently mapped on top), their subsurfaces and popups, composited
 * here with pixman from the clients' buffers. Every frame is published as guestd does: the changed rows into the
 * back buffer, row_seq for them, the flip (sbshm.h "damage per row"); only the viewport's rows (SBG_MSG_VIEWPORT).
 * Starbite's touch arrives as SBG_MSG_TOUCH (surface px) and becomes wl_touch for clients that bound it, pointer
 * motion + left button otherwise; its keys (SBG_MSG_KEY, Linux codes) go through an xkb keyboard (us layout), its
 * words (SBG_MSG_TEXT) through text-input-v3. sbwl owns the shared rings (guestd does not run).
 *
 * Contract v1 (hdr.version 1: run-term.sh, the L2 Terminal): one app, started at once and again when it ends:
 *   sbwl [app [args...]]          default: foot; or sbwl.app=NAME on the kernel command line
 * Contract v2 (hdr.version 2): the catalog (the .desktop files: struct sbshm_cat + the icon pool), launch / activate
 * / close by catalog index, the viewport, text-input-v3, and /run/sbwl.sock for sbpkg (sbshm.h). An app named on
 * the command line is still started at once (and kept running) in v2.
 * Contract v3 (hdr.version 3, each part only when hdr.host_services has its bit): the clipboard both ways (CLIP), the
 * notifications of org.freedesktop.Notifications through sbnotify (../notify/sbnotify.c, NOTIF), the wall clock and
 * TZ for the apps (CLOCK); always in v3: the key bar's modifiers (KEYMOD), host_surf_reading (zero-copy display: the
 * buffer the host's GPU samples is never written) and opaque pixels. Background apps get no frame callbacks and
 * their commits do not make sbwl composite.
 *
 * SBWL_SHM=/path: map that file instead of /dev/sbshm (hostsim.c, the user-space stand-in for the host; no doorbells,
 * the log on stderr, MSG_STOP exits instead of powering off).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/render/drm_format_set.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <wlr/version.h>
#include <drm_fourcc.h>
#include <pixman.h>
#include <xkbcommon/xkbcommon.h>
#ifndef SBWL_NO_ICONS
#include <cairo.h>
#include <librsvg/rsvg.h>
#endif

#include "../sbshm.h"

#if WLR_VERSION_MAJOR == 0 && WLR_VERSION_MINOR >= 18
#define WLR18 1
#define XDG_READY(x) ((x)->initialized)
#define BTN_PRESSED WL_POINTER_BUTTON_STATE_PRESSED
#define BTN_RELEASED WL_POINTER_BUTTON_STATE_RELEASED
#else
#define WLR18 0
#define XDG_READY(x) 1
#define BTN_PRESSED WLR_BUTTON_PRESSED
#define BTN_RELEASED WLR_BUTTON_RELEASED
#endif

#define DESK_ID "Starbite"          /* our XDG_CURRENT_DESKTOP (OnlyShowIn / NotShowIn) */

/* ---- the shared region: every access to the rings and the doorbell goes through here ---------------------- */
static volatile struct sbshm_hdr *hdr;
static uint8_t *shm;
static uint64_t shm_size;
static struct sbshm_ring *h2g, *g2h;
static int sbfd = -1, kfd = -1, sim, v2, v3;
static uint32_t svc;                       /* v3: hdr.host_services (0 from a v1 / v2 host) */
static uint32_t W, H, stride;
static uint32_t VR, VW;                    /* the viewport: the rows and the width the host shows */
/* cp7 landscape (SBG_MSG_ROTATE): ROT quarter turns clockwise of the content on the panel (0, 1, 3), LROWS the
 * content rows shown when turned. Apps get the area AW() x AH() (the logical space); the surface stays W x H. */
static uint32_t ROT, LROWS;
static int turned(void) { return ROT == 1 || ROT == 3; }
static uint32_t AW(void) { return turned() ? H : VW; }
static uint32_t AH(void) { return turned() ? (LROWS && LROWS <= W ? LROWS : W) : VR; }
/* 8i (the user: "the UI is super small"): the apps' logical space is the surface's pixels / SC: sbwl.scale=N on the
 * kernel command line (1..4), else SBWL_SCALE, else the surface's width / 360 on the phone (720 -> 2, 1080 -> 3,
 * 1440 -> 4: 360 x 672 logical, a phone's ~360 dp, libadwaita's narrow layout) and 1 under hostsim (its pixel
 * checks). wl_output says SC, so GTK, Qt and foot draw their buffers at SC; sbwl composites in surface pixels
 * (positions and sizes x SC) and maps touches back. */
static uint32_t SC = 1;
static uint32_t LW(void) { return AW() / SC; }
static uint32_t LH(void) { return AH() / SC; }

static void say(const char *fmt, ...)
{
	char b[320];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b + 6, sizeof(b) - 7, fmt, ap);
	va_end(ap);
	memcpy(b, "sbwl: ", 6);
	n += 6;
	if (n > (int)sizeof(b) - 2)
		n = sizeof(b) - 2;
	if (sim || kfd < 0) {
		b[n++] = '\n';
		(void)!write(2, b, (size_t)n);
	} else {
		(void)!write(kfd, b, (size_t)n);
	}
}

static uint64_t ms_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static int map_shm(void)
{
	const char *simf = getenv("SBWL_SHM");
	if (simf) {
		sim = 1;
		sbfd = open(simf, O_RDWR | O_CLOEXEC);
		struct stat st;
		if (sbfd < 0 || fstat(sbfd, &st) < 0) {
			say("SBWL_SHM %s: %s", simf, strerror(errno));
			return -1;
		}
		shm_size = (uint64_t)st.st_size;
	} else {
		uint64_t base = 0;
		unsigned char be[16];
		int fd = open("/proc/device-tree/chosen/starbite,shm", O_RDONLY);
		if (fd < 0 || read(fd, be, 16) != 16)
			return -1;
		close(fd);
		for (int i = 0; i < 8; ++i) {
			base = base << 8 | be[i];
			shm_size = shm_size << 8 | be[8 + i];
		}
		sbfd = open("/dev/sbshm", O_RDWR | O_CLOEXEC);
		if (sbfd < 0) {
			say("no /dev/sbshm (sbguest.ko): %s", strerror(errno));
			return -1;
		}
	}
	shm = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, sbfd, 0);
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
	VR = H;
	VW = W;
	v2 = hdr->version >= SBSHM_VERSION_V2 && shm_size >= SBSHM_ICON_OFF + (uint64_t)SBSHM_ICONS_CAT * SBSHM_ICON_BYTES;
	v3 = v2 && hdr->version >= SBSHM_VERSION_V3 && shm_size >= SBSHM_V3_FREE_OFF;
	svc = v3 ? hdr->host_services : 0;
	return 0;
}

static void doorbell_ring(void)
{
	if (!sim)
		(void)!write(sbfd, "1", 1);
}

static unsigned g2h_dropped;
static void g2h_send3(uint16_t type, uint32_t a, uint32_t b, uint32_t c)
{
	struct sbshm_msg m = {.type = type, .a = a, .b = b, .c = c, .t_ms = (uint32_t)ms_now()};
	if (!sbshm_put(g2h, &m))
		++g2h_dropped;
	doorbell_ring();
}
static void g2h_send(uint16_t type, uint32_t a, uint32_t b) { g2h_send3(type, a, b, 0); }
static int h2g_get(struct sbshm_msg *m) { return sbshm_get(h2g, m); }

/* the frame into the back buffer, only the changed rows of the viewport (guestd's publish, sbshm.h).
 * Zero-copy display (v3): the host's GPU samples the front buffer in place, so every pixel is opaque (X = 0xff: the
 * frames are ORed opaque, and each buffer is filled opaque black before its first use), and sbwl never writes the
 * buffer hdr.host_surf_reading names: that frame is deferred (-1), retried 2 ms later from the event loop. */
static uint32_t *shadow[2];                /* what each shared buffer holds (once buf_ready) */
static int buf_ready[2];
/* 8o (native resolution: a caret's blink cost a whole frame's compare): the rows outside the frame's damage band are
 * unchanged by definition; the back buffer needs such a row only if it changed after the frame that buffer last
 * held (lastchg[y] > bufseq[back]), copied from the front's shadow, no compare */
static uint32_t *lastchg;                  /* per row: the seq of the frame in which it last changed */
static int pub_or;                         /* publish's source is an app's buffer: X / A set to 0xff while copying */
static uint32_t bufseq[2];                 /* the seq of the frame each buffer holds whole */
static uint64_t frames, rows_out;
static unsigned nframe, nbuilt, nbuf, defers;
static unsigned long nband;                /* frames composited as a damage band (8o), not whole */
static uint32_t last_px;
static int publish(const uint8_t *src, uint32_t src_stride, uint32_t nrows, uint32_t b0, uint32_t b1)
{
	volatile uint32_t *rows = (volatile uint32_t *)(shm + SBSHM_ROWS_OFF);
	size_t rb = (size_t)W * 4;
	if (!shadow[0]) {
		shadow[0] = malloc((size_t)W * H * 4);
		shadow[1] = malloc((size_t)W * H * 4);
		if (!shadow[0] || !shadow[1])
			return 0;
		for (size_t i = 0; i < (size_t)W * H; ++i)
			shadow[0][i] = shadow[1][i] = 0xff000000U;
		hdr->rows_valid = H <= SBSHM_ROWS_MAX;
	}
	if (!lastchg && !(lastchg = calloc(H, sizeof(*lastchg))))
		return 0;
	if (nrows > H)
		nrows = H;
	if (!hdr->surf_seq || b1 > nrows || b0 >= b1)
		b0 = 0, b1 = nrows;                    /* the first frame, or no band: every row */
	uint32_t front = hdr->surf_front & 1U, back = front ^ 1U, seq = hdr->surf_seq + 1U, n = 0;
	if (v3) {
		sbshm_dmb();
		if (hdr->host_surf_reading == back) {
			++defers;
			return -1;
		}
	}
	uint8_t *dst = shm + hdr->surf_off[back];
	if (!buf_ready[back]) {                    /* its first use: opaque black, as its shadow says */
		for (uint32_t y = 0; y < H; ++y)
			memcpy(dst + (size_t)y * stride, shadow[back] + (size_t)y * W, rb);
		buf_ready[back] = 1;
	}
	for (uint32_t y = 0; y < nrows; ++y) {
		if (y < b0 || y >= b1) {               /* outside the damage: the front's row, if the back is older */
			if (lastchg[y] > bufseq[back]) {
				memcpy(dst + (size_t)y * stride, shadow[front] + (size_t)y * W, rb);
				memcpy(shadow[back] + (size_t)y * W, shadow[front] + (size_t)y * W, rb);
			}
			continue;
		}
		const uint8_t *s = src + (size_t)y * src_stride;
		int changed = !hdr->surf_seq || memcmp(s, shadow[front] + (size_t)y * W, rb);
		if (memcmp(s, shadow[back] + (size_t)y * W, rb)) {
			if (pub_or) {                      /* opaque for Starbite's XRGB (an app's ARGB: its alpha) */
				const uint32_t *sp = (const uint32_t *)(const void *)s;
				uint32_t *sh = shadow[back] + (size_t)y * W;
				for (uint32_t x = 0; x < W; ++x)
					sh[x] = sp[x] | 0xff000000U;
				memcpy(dst + (size_t)y * stride, sh, rb);
			} else {
				memcpy(dst + (size_t)y * stride, s, rb);
				memcpy(shadow[back] + (size_t)y * W, s, rb);
			}
		}
		if (changed) {
			if (H <= SBSHM_ROWS_MAX)
				rows[y] = seq;
			lastchg[y] = seq;
			++n;
		}
	}
	/* rows past the viewport: the back buffer keeps whole frames (sbshm.h), so it gets the front's rows */
	for (uint32_t y = nrows; y < H; ++y)
		if (lastchg[y] > bufseq[back] && memcmp(shadow[front] + (size_t)y * W, shadow[back] + (size_t)y * W, rb)) {
			memcpy(dst + (size_t)y * stride, shadow[front] + (size_t)y * W, rb);
			memcpy(shadow[back] + (size_t)y * W, shadow[front] + (size_t)y * W, rb);
		}
	if (!n)
		return 0;
	bufseq[back] = seq;
	sbshm_dmb();
	hdr->surf_front = back;
	hdr->damage_x = 0;
	hdr->damage_w = W;
	hdr->damage_y = 0;
	hdr->damage_h = nrows;
	sbshm_dmb();
	hdr->surf_seq = seq;
	frames++;
	rows_out += n;
	return 1;
}

/* ---- state ----------------------------------------------------------------------------------------------- */
struct app {                               /* one catalog entry (a .desktop file) */
	char id[128], name[64], exec[1024], icon[256], wmclass[128], bin[128], path[512];
	int terminal, termemu, system;
};

struct inst {                              /* a process (group) sbwl started */
	struct wl_list link;
	char desk[128];                        /* the app (desktop id; v1: the program's name) */
	pid_t pid;                             /* = its process group (setsid) */
	int exited, legacy;
	uint64_t t_start, t_close;             /* t_close: APP_CLOSE asked (SIGTERM at +3 s, SIGKILL at +6 s) */
	int termed;
};

struct view {                              /* one xdg toplevel */
	struct wl_list link;                   /* S.views: in map order (the most recent last) */
	struct wlr_xdg_toplevel *top;
	struct wlr_scene_tree *tree;
	struct wlr_xdg_toplevel_decoration_v1 *deco;
	struct wl_listener map, unmap, destroy, title, commit, deco_destroy, deco_mode;
	char desk[128];                        /* its app ("" until it maps) */
	pid_t pid;                             /* its client's */
	int mapped, dialog, x, y, gw, gh;
	int gtk;                               /* its client is GTK 3 / 4 (view_gtk): 0 not looked yet, 1 yes, 2 no */
	int asked, aw, ah, adialog;            /* the last configure sbwl asked for */
};

struct popup {
	struct wlr_xdg_popup *p;
	struct wl_listener commit, destroy;
};

struct tinput {
	struct wl_list link;
	struct wlr_text_input_v3 *ti;
	struct wl_listener enable, commit, disable, destroy;
};

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
	struct wlr_text_input_manager_v3 *tim;
	struct wlr_xdg_decoration_manager_v1 *xdeco;
	struct wl_listener new_output, output_frame, new_toplevel, new_popup, new_xdg_surface, kbd_key, kbd_mods,
		new_text_input, new_deco, new_surface, req_sel, req_psel, set_sel;
	struct wl_list views, insts, tinputs;
	char **argv;                           /* the legacy app (v1, or named on the command line) */
	int stopping, touch_down, ptr_down, dirty;
	struct wlr_surface *touch_surface;
	char active[128];                      /* the active app's desktop id; "" = none */
	struct wlr_surface *kfocus;            /* the keyboard focus */
	struct tinput *ti_on;                  /* the enabled text input the host's keyboard writes to */
	uint32_t ti_purpose;
	char wl_sock[108];                     /* the Wayland socket's path */
	struct passwd user;                    /* "user", when the root has one */
	int have_user;
	char user_rt[64];                      /* its XDG_RUNTIME_DIR */
	pid_t dbus;
} S;

static void mark_dirty(void)
{
	S.dirty = 1;
	if (S.output)
		wlr_output_schedule_frame(S.output);
}

/* ---- the catalog --------------------------------------------------------------------------------------- */
static struct app *apps;
static int napps;
static uint32_t lay_seq;                   /* the catalog seq at which the ids / their order last changed */
static uint64_t rescan_at;
static int cat_dirty;

static int app_index(const char *desk)
{
	for (int i = 0; i < napps; ++i)
		if (!strcmp(apps[i].id, desk))
			return i;
	return -1;
}

static void view_layout(struct view *v);
static void refocus(void);
static void key(uint32_t code, int down, uint32_t t);
/* keys before the active app has a window (it is still starting): held, then sent when its window takes the focus */
static struct { uint32_t code, down; } kq[256];
static unsigned kq_n;
static void window_state(void);

/* ---- compositing the active app -------------------------------------------------------------------------- */
static uint32_t *winframe;
static pixman_image_t *winimg;
static pixman_image_t *comp_dst;           /* where comp_surface draws: winimg, or limg when turned */
static uint32_t *lframe;                   /* turned: the logical frame, H wide and W tall (stride H) */
static pixman_image_t *limg;
static int win_surfaces, win_skipped;
struct comp_off {
	int x, y;
};

/* (the dmabuf helpers below: a surface's current GPU-drawn frame, read between these two) */
static const uint8_t *dmab_pixels(struct wlr_surface *s, uint32_t *w, uint32_t *h, uint32_t *stride, pixman_format_code_t *pf,
				  int *fd);
static void dmab_done(int fd);
static void comp_surface(struct wlr_surface *surface, int sx, int sy, void *data)
{
	const struct comp_off *o = data;
	struct wlr_texture *tex = wlr_surface_get_texture(surface);
	pixman_image_t *ti = tex && wlr_texture_is_pixman(tex) ? wlr_pixman_texture_get_image(tex) : NULL;
	pixman_image_t *si = NULL;
	struct wlr_buffer *src = NULL;
	if (ti) {
		si = pixman_image_create_bits_no_clear(pixman_image_get_format(ti), pixman_image_get_width(ti),
						       pixman_image_get_height(ti), pixman_image_get_data(ti),
						       pixman_image_get_stride(ti));
	} else if (surface->buffer && surface->buffer->source) {
		src = surface->buffer->source;
		void *dp;
		uint32_t f;
		size_t bs;
		if (!wlr_buffer_begin_data_ptr_access(src, WLR_BUFFER_DATA_PTR_ACCESS_READ, &dp, &f, &bs)) {
			win_skipped++;
			return;
		}
		pixman_format_code_t pf = f == DRM_FORMAT_ARGB8888 ? PIXMAN_a8r8g8b8 : f == DRM_FORMAT_XRGB8888 ? PIXMAN_x8r8g8b8 : 0;
		if (pf)
			si = pixman_image_create_bits_no_clear(pf, src->width, src->height, dp, (int)bs);
	}
	int dfd = -1;
	if (!si) {                                 /* a GPU-drawn frame (a dmabuf: no texture, no data pointer) */
		uint32_t w, h, st;
		pixman_format_code_t pf;
		const uint8_t *px = dmab_pixels(surface, &w, &h, &st, &pf, &dfd);
		if (px)
			si = pixman_image_create_bits_no_clear(pf, (int)w, (int)h, (uint32_t *)(uintptr_t)px, (int)st);
	}
	if (si) {
		int bw = pixman_image_get_width(si), bh = pixman_image_get_height(si);
		/* the buffer's size vs the surface's in pixels (its logical size x SC): a client drawing at SC is copied
		 * 1:1, one at another buffer scale is scaled to fit */
		int dw = surface->current.width * (int)SC, dh = surface->current.height * (int)SC;
		if (dw > 0 && dh > 0 && (dw != bw || dh != bh)) {
			pixman_transform_t t;
			pixman_transform_init_scale(&t, pixman_double_to_fixed((double)bw / dw), pixman_double_to_fixed((double)bh / dh));
			pixman_image_set_transform(si, &t);
			pixman_image_set_filter(si, PIXMAN_FILTER_BILINEAR, NULL, 0);
		} else {
			dw = bw, dh = bh;
		}
		pixman_image_composite32(PIXMAN_OP_OVER, si, NULL, comp_dst, 0, 0, 0, 0, (o->x + sx) * (int)SC,
					 (o->y + sy) * (int)SC, dw, dh);
		pixman_image_unref(si);
		win_surfaces++;
	} else {
		win_skipped++;
	}
	if (src)
		wlr_buffer_end_data_ptr_access(src);
	if (dfd >= 0)
		dmab_done(dfd);
}

static int view_active(const struct view *v) { return S.active[0] && !strcmp(v->desk, S.active); }

/* the active app's window has been in a published frame (SBG_MSG_WINDOW b = 1 waits for it: the host shows the
 * window layer only once the surface holds that app's pixels, not the previous app's last frame) */
static int win_shown;
static void window_state(void);
/* the layout a band is valid for: the active app, the viewport, the turn, each shown window's place, size and
 * dialog state; any change: a whole frame (a dialog's scrim covers the window; wlr_scene's damage knows no scrim) */
static uint64_t layout_sig(void)
{
	uint64_t h = 1469598103934665603ULL;
#define MIX(x) (h = (h ^ (uint64_t)(x)) * 1099511628211ULL)
	for (const char *p = S.active; *p; ++p)
		MIX((uint8_t)*p);
	MIX(VR), MIX(VW), MIX(ROT), MIX(LROWS);
	struct view *v;
	wl_list_for_each(v, &S.views, link)
		if (v->mapped && view_active(v)) {
			struct wlr_box geo;
			wlr_xdg_surface_get_geometry(v->top->base, &geo);
			MIX((uintptr_t)v), MIX(v->x), MIX(v->y), MIX(v->dialog), MIX(geo.x), MIX(geo.y), MIX(geo.width), MIX(geo.height);
		}
#undef MIX
	return h;
}
static void count_surface(struct wlr_surface *surface, int sx, int sy, void *data)
{
	(void)surface, (void)sx, (void)sy;
	++*(int *)data;
}
static unsigned long ndirect;              /* frames published straight from an app's buffer */
static void window_state(void);
/* 1 published (or nothing new), -1 the host reads the back buffer, 0 not this case (composite instead) */
static int publish_direct(uint32_t b0, uint32_t b1)
{
	struct view *v, *only = NULL;
	int nv = 0, ns = 0;
	wl_list_for_each(v, &S.views, link)
		if (v->mapped && view_active(v))
			only = v, ++nv;
	if (nv != 1 || only->dialog || getenv("SBWL_NO_DIRECT"))
		return 0;
	wlr_xdg_surface_for_each_surface(only->top->base, count_surface, &ns);
	struct wlr_surface *surf = only->top->base->surface;
	struct wlr_box geo;
	wlr_xdg_surface_get_geometry(only->top->base, &geo);
	if (ns != 1 || only->x - geo.x || only->y - geo.y || (uint32_t)surf->current.width * SC != W ||
	    (uint32_t)surf->current.height * SC < VR)
		return 0;
	struct wlr_texture *tex = wlr_surface_get_texture(surf);
	pixman_image_t *ti = tex && wlr_texture_is_pixman(tex) ? wlr_pixman_texture_get_image(tex) : NULL;
	struct wlr_buffer *src = NULL;
	const uint8_t *px = NULL;
	uint32_t pstride = 0, bw = 0, bh = 0;
	pixman_format_code_t pf = 0;
	if (ti) {
		px = (const uint8_t *)pixman_image_get_data(ti);
		pstride = (uint32_t)pixman_image_get_stride(ti);
		bw = (uint32_t)pixman_image_get_width(ti), bh = (uint32_t)pixman_image_get_height(ti);
		pf = pixman_image_get_format(ti);
	} else if (surf->buffer && surf->buffer->source) {
		void *dp;
		uint32_t f;
		size_t bs;
		src = surf->buffer->source;
		if (!wlr_buffer_begin_data_ptr_access(src, WLR_BUFFER_DATA_PTR_ACCESS_READ, &dp, &f, &bs))
			return 0;
		px = dp, pstride = (uint32_t)bs, bw = (uint32_t)src->width, bh = (uint32_t)src->height;
		pf = f == DRM_FORMAT_ARGB8888 ? PIXMAN_a8r8g8b8 : f == DRM_FORMAT_XRGB8888 ? PIXMAN_x8r8g8b8 : 0;
	}
	int dfd = -1;
	if (!px)
		px = dmab_pixels(surf, &bw, &bh, &pstride, &pf, &dfd);
	int r = 0;
	if (px && bw == W && bh >= VR && (pf == PIXMAN_a8r8g8b8 || pf == PIXMAN_x8r8g8b8)) {
		uint64_t f0 = frames;
		pub_or = 1;
		r = publish(px, pstride, VR, b0, b1) < 0 ? -1 : 1;
		pub_or = 0;
		if (r > 0) {
			++ndirect;
			last_px = ((const uint32_t *)(const void *)(px + (size_t)(VR / 8) * pstride))[W / 8] | 0xff000000U;
			hdr->guest_rot = ROT;
			if (!f0 && frames)
				say("first frame published (%ux%u, viewport %ux%u, straight from the app's buffer)", W, H, VW, VR);
			if (!win_shown) {
				win_shown = 1;
				window_state();
			}
		}
	}
	if (src)
		wlr_buffer_end_data_ptr_access(src);
	if (dfd >= 0)
		dmab_done(dfd);
	return r;
}

/* b0 .. b1: the rows (surface pixels) that changed since the last published frame (wlr_scene's damage); b1 = 0: all */
static int publish_window(uint32_t b0, uint32_t b1)
{
	if (!winframe) {
		winframe = calloc((size_t)W * H, 4);
		winimg = winframe ? pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, (int)W, (int)H, winframe, (int)W * 4) : NULL;
		if (!winimg)
			return 0;
	}
	int any = 0;
	struct view *v;
	wl_list_for_each(v, &S.views, link)
		any |= v->mapped && view_active(v);
	if (!any)
		return 0;                          /* nothing to show: the last frame stays */
	int tn = turned();
	if (tn && !lframe) {
		lframe = calloc((size_t)W * H, 4);
		limg = lframe ? pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, (int)H, (int)W, lframe, (int)H * 4) : NULL;
		if (!limg)
			return 0;
	}
	comp_dst = tn ? limg : winimg;
	uint32_t *cpx = tn ? lframe : winframe;
	static uint64_t sig_last;
	uint64_t sig = layout_sig();
	if (tn || sig != sig_last || b1 > VR || b0 >= b1)
		b0 = 0, b1 = tn ? H : VR;              /* a whole frame */
	sig_last = sig;
	int band = !tn && (b0 > 0 || b1 < VR);
	/* 8o night ("super slow"): one opaque window covering the surface at the output's scale, with no popup or
	 * subsurface (the common case: an app full screen): no fill, no composite, no alpha pass; publish compares
	 * and copies the app's own buffer's rows (X / A set on the copy) */
	if (!tn) {
		int r = publish_direct(b0, b1);
		if (r)
			return r < 0 ? -1 : 1;
	}
	pixman_region32_t clip;
	if (band) {                                /* only the band composited (fill, surfaces, scrim): pixman clips */
		pixman_region32_init_rect(&clip, 0, (int)b0, (int)W, (int)(b1 - b0));
		pixman_image_set_clip_region32(comp_dst, &clip);
	}
	for (size_t i = tn ? 0 : (size_t)W * b0; i < (tn ? (size_t)W * H : (size_t)W * b1); ++i)
		cpx[i] = 0xff000000U;
	win_surfaces = win_skipped = 0;
	wl_list_for_each(v, &S.views, link) {  /* oldest first: the most recent ends on top */
		if (!v->mapped || !view_active(v))
			continue;
		if (v->dialog && win_surfaces) {       /* a dialog: what is under it dimmed (it has no frame of its own) */
			pixman_color_t scrim = {0, 0, 0, 0x6000};
			pixman_rectangle16_t r = {0, 0, (uint16_t)AW(), (uint16_t)AH()};
			pixman_image_fill_rectangles(PIXMAN_OP_OVER, comp_dst, &scrim, 1, &r);
		}
		struct wlr_box geo;
		wlr_xdg_surface_get_geometry(v->top->base, &geo);
		struct comp_off o = {v->x - geo.x, v->y - geo.y};
		wlr_xdg_surface_for_each_surface(v->top->base, comp_surface, &o);
	}
	if (band) {
		pixman_image_set_clip_region32(comp_dst, NULL);
		pixman_region32_fini(&clip);
	}
	if (!win_surfaces)
		return 0;
	uint32_t prows = VR;
	if (tn) {                                  /* the logical frame (x along the panel's rows) turned into the surface */
		uint32_t ah = AH();
		for (uint32_t py = 0; py < H; ++py) {
			uint32_t *d = winframe + (size_t)py * W;
			for (uint32_t px = 0; px < W; ++px) {
				uint32_t lx = ROT == 1 ? py : H - 1 - py, ly = ROT == 1 ? W - 1 - px : px;
				d[px] = ly < ah ? lframe[(size_t)ly * H + lx] : 0xff000000U;
			}
		}
		prows = H;
	}
	for (size_t i = tn ? 0 : (size_t)W * b0; i < (tn ? (size_t)W * prows : (size_t)W * b1); ++i)
		winframe[i] |= 0xff000000U;            /* opaque: Starbite composites XRGB */
	last_px = winframe[(size_t)(prows / 8) * W + W / 8];
	uint64_t f0 = frames;
	nband += band;
	if (publish((const uint8_t *)winframe, W * 4, prows, tn ? 0 : b0, tn ? 0 : b1) < 0)
		return -1;                             /* the host reads the back buffer: again in 2 ms */
	hdr->guest_rot = ROT;                      /* (after the flip: the host turns its chrome with this frame) */
	if (!f0 && frames)
		say("first frame published (%ux%u, viewport %ux%u)", W, H, VW, VR);
	if (!win_shown) {
		win_shown = 1;
		window_state();
	}
	return 1;
}

/* a frame deferred by host_surf_reading: tried again from a 2 ms timer (the loop never blocks on the host) */
static struct wl_event_source *defer_src;
static int defer_cb(void *data)
{
	(void)data;
	if (S.dirty && S.output)
		wlr_output_schedule_frame(S.output);
	return 0;
}
static void defer_frame(void)
{
	S.dirty = 1;
	if (!defer_src)
		defer_src = wl_event_loop_add_timer(S.loop, defer_cb, NULL);
	if (defer_src)
		wl_event_source_timer_update(defer_src, 2);
}

/* 8o night (measure): SBWL_NO_SCENE=1: wlr_scene is not rendered into the headless output (nobody reads that buffer;
 * with GTK's whole-window damage it was a second full-frame render each frame); frames are paced by a timer that
 * sends the frame callbacks a refresh after a published frame */
static int no_scene = -1;
static struct wl_event_source *pace_src;
static int pace_cb(void *data)
{
	(void)data;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(S.scene_output, &now);
	return 0;
}
static void output_frame(struct wl_listener *l, void *data)
{
	(void)l, (void)data;
	struct wlr_output_state st;
	wlr_output_state_init(&st);
	if (no_scene < 0)
		no_scene = getenv("SBWL_NO_SCENE") != NULL;
	/* the output is committed (the scene rendered) only on frames with something new: a commit with a buffer makes the
	 * headless backend send the next frame event a refresh later, which paces an animating app at 60 Hz, and without
	 * one the frames stop until the next mark_dirty (idle: no frame events at all; was ~20 renders / s) */
	bool busy = S.dirty || nframe < 3;
	bool built = busy && !no_scene && wlr_scene_output_build_state(S.scene_output, &st, NULL);
	int published = 0;
	nbuilt += built;
	nbuf += built && (st.committed & WLR_OUTPUT_STATE_BUFFER) && st.buffer;
	if (++nframe <= 3)
		say("frame event %u: built %d, buffer %d", nframe, built, (st.committed & WLR_OUTPUT_STATE_BUFFER) && st.buffer);
	/* the rows wlr_scene re-rendered (its damage, in output pixels): kept until a frame is published (a deferred one
	 * comes back with only its own new damage); none known: the whole frame */
	static int32_t dmg0 = -1, dmg1 = -1;
	static int dmg_all;
	if (built && (st.committed & WLR_OUTPUT_STATE_DAMAGE) && !turned()) {
		const pixman_box32_t *e = pixman_region32_extents(&st.damage);
		if (pixman_region32_not_empty(&st.damage)) {
			dmg0 = dmg0 < 0 || e->y1 < dmg0 ? e->y1 : dmg0;
			dmg1 = e->y2 > dmg1 ? e->y2 : dmg1;
		}
	} else if (S.dirty) {
		dmg_all = 1;
	}
	if (S.dirty) {
		S.dirty = 0;
		uint32_t b0 = 0, b1 = 0;
		if (!dmg_all && dmg0 >= 0 && dmg1 > dmg0)
			b0 = (uint32_t)dmg0, b1 = (uint32_t)dmg1;
		int r = publish_window(b0, b1);
		if (r < 0) {
			defer_frame();
		} else {
			dmg0 = dmg1 = -1;
			dmg_all = 0;
			published = 1;
		}
	}
	if (!v2 && !frames && winimg) {            /* v1: a black frame until the app's first (as L1/L2's sbwl did) */
		for (size_t i = 0; i < (size_t)W * VR; ++i)
			winframe[i] = 0xff000000U;
		if (publish((const uint8_t *)winframe, W * 4, VR, 0, 0) < 0)
			defer_frame();
	}
	if (built)
		wlr_output_commit_state(S.output, &st);
	wlr_output_state_finish(&st);
	if (no_scene && published) {               /* the apps' next frame a refresh from now */
		if (!pace_src)
			pace_src = wl_event_loop_add_timer(S.loop, pace_cb, NULL);
		if (pace_src) {
			wl_event_source_timer_update(pace_src, 16);
			return;
		}
	}
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
	wlr_output_state_set_scale(&st, (float)SC);
	bool ok = wlr_output_commit_state(out, &st);
	wlr_output_state_finish(&st);
	S.output = out;
	if (!winframe) {                           /* the composite buffer */
		winframe = calloc((size_t)W * H, 4);
		winimg = winframe ? pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, (int)W, (int)H, winframe, (int)W * 4) : NULL;
	}
	say("output %s %dx%d scale %u (apps: %ux%u): enable commit %s, enabled %d", out->name, out->width, out->height, SC,
	    W / SC, H / SC, ok ? "ok" : "FAILED", out->enabled);
	S.output_frame.notify = output_frame;
	wl_signal_add(&out->events.frame, &S.output_frame);
	S.scene_output = wlr_scene_output_create(S.scene, out);
	wlr_scene_output_set_position(S.scene_output, 0, 0);
#if WLR18
	wlr_output_create_global(out, S.display);
#else
	wlr_output_create_global(out);
#endif
	wlr_output_schedule_frame(out);
}

/* every commit of a shown surface: the next frame composites again. Background apps (the host's switcher keeps them
 * alive) cost ~nothing: their scene nodes are disabled, so wlr_scene sends them no frame callbacks (a client that
 * paces itself on wl_surface.frame, as GTK, Qt, foot and SDL do, stops drawing), and a commit of theirs (a client
 * drawing on its own timer) no longer makes sbwl composite and compare the active app's frame again.
 * SBWL_HIDDEN_COMMITS=1: the old behaviour (every commit composites), for measuring. */
/* 8o night (sbgpu, GPU-drawn apps): a GL / Vulkan app presents dmabufs (linux-dmabuf, offered with the GPU's render
 * node), which the pixman renderer cannot read: each surface's dmabufs are mapped read-only once (Mesa rotates 2-4
 * of them) and read with DMA_BUF_IOCTL_SYNC around it; a mapping goes when its buffer does */
#define DMAB_PER_SURF 4
struct dmab {
	struct wlr_buffer *b;                  /* NULL: free */
	struct wl_listener destroy;
	const uint8_t *map;
	size_t len;
	int fd;
	uint32_t w, h, stride, off;
	pixman_format_code_t pf;
};
struct surf_track {
	struct wl_listener commit, destroy;
	struct wlr_surface *s;
	struct wl_list link;                   /* tracks */
	struct dmab db[DMAB_PER_SURF];
	int cur;                               /* the dmab of the last commit, -1 none */
	int said;
};
static struct wl_list tracks = {&tracks, &tracks};
static struct surf_track *track_of(struct wlr_surface *s)
{
	struct surf_track *t;
	wl_list_for_each(t, &tracks, link)
		if (t->s == s)
			return t;
	return NULL;
}
static void dmab_drop(struct dmab *d)
{
	if (!d->b)
		return;
	wl_list_remove(&d->destroy.link);
	if (d->map)
		munmap((void *)d->map, d->len);
	d->b = NULL, d->map = NULL;
}
static void dmab_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct dmab *d = wl_container_of(l, d, destroy);
	dmab_drop(d);
}
/* the surface's committed buffer, when it is a dmabuf: mapped (cached), cur set; else cur -1 */
static void dmab_commit(struct surf_track *t)
{
	struct wlr_buffer *b = t->s->current.buffer;
	struct wlr_dmabuf_attributes a;
	t->cur = -1;
	if (!b || !wlr_buffer_get_dmabuf(b, &a))
		return;
	for (int i = 0; i < DMAB_PER_SURF; ++i)
		if (t->db[i].b == b) {
			t->cur = i;
			return;
		}
	pixman_format_code_t pf = a.format == DRM_FORMAT_ARGB8888 ? PIXMAN_a8r8g8b8 : a.format == DRM_FORMAT_XRGB8888 ? PIXMAN_x8r8g8b8 : 0;
	if (a.n_planes != 1 || !pf || (a.modifier != DRM_FORMAT_MOD_LINEAR && a.modifier != DRM_FORMAT_MOD_INVALID))
		return;
	int k = 0;
	for (int i = 0; i < DMAB_PER_SURF; ++i)
		if (!t->db[i].b) {
			k = i;
			break;
		} else if (i == DMAB_PER_SURF - 1) {
			k = (t->said + 1) % DMAB_PER_SURF;     /* all taken: replace one */
		}
	struct dmab *d = &t->db[k];
	dmab_drop(d);
	size_t len = (size_t)a.offset[0] + (size_t)a.stride[0] * (size_t)a.height;
	len = (len + 4095U) & ~(size_t)4095U;
	void *m = mmap(NULL, len, PROT_READ, MAP_SHARED, a.fd[0], 0);
	if (m == MAP_FAILED)
		return;
	d->b = b, d->map = m, d->len = len, d->fd = a.fd[0], d->w = (uint32_t)a.width, d->h = (uint32_t)a.height;
	d->stride = a.stride[0], d->off = a.offset[0], d->pf = pf;
	d->destroy.notify = dmab_destroyed;
	wl_signal_add(&b->events.destroy, &d->destroy);
	t->cur = k;
	if (t->said++ < 1)
		say("a dmabuf frame (GPU-drawn): %ux%u, stride %u, format %08x", d->w, d->h, d->stride, a.format);
}
/* the surface's current dmabuf mapped for reading (sync start done: call dmab_done), else NULL */
static const struct dmab *dmab_begin(struct wlr_surface *s)
{
	struct surf_track *t = track_of(s);
	if (!t || t->cur < 0 || !t->db[t->cur].b)
		return NULL;
	struct dma_buf_sync sy = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
	(void)ioctl(t->db[t->cur].fd, DMA_BUF_IOCTL_SYNC, &sy);
	return &t->db[t->cur];
}
static const uint8_t *dmab_pixels(struct wlr_surface *s, uint32_t *w, uint32_t *h, uint32_t *stride, pixman_format_code_t *pf,
				  int *fd)
{
	const struct dmab *d = dmab_begin(s);
	if (!d)
		return NULL;
	*w = d->w, *h = d->h, *stride = d->stride, *pf = d->pf, *fd = d->fd;
	return d->map + d->off;
}
static void dmab_done(int fd)
{
	struct dma_buf_sync sy = {DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
	(void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sy);
}
static unsigned long hidden_commits;
static struct view *view_of(struct wlr_xdg_toplevel *t);
static int surface_hidden(struct wlr_surface *s)
{
	static int keep = -1;
	if (keep < 0)
		keep = getenv("SBWL_HIDDEN_COMMITS") != NULL;
	if (keep)
		return 0;
	struct wlr_xdg_surface *x = wlr_xdg_surface_try_from_wlr_surface(wlr_surface_get_root_surface(s));
	for (int i = 0; x && x->role == WLR_XDG_SURFACE_ROLE_POPUP && i < 16; ++i)
		x = x->popup->parent ? wlr_xdg_surface_try_from_wlr_surface(wlr_surface_get_root_surface(x->popup->parent)) : NULL;
	if (!x || x->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL || !x->toplevel)
		return 0;
	struct view *v = view_of(x->toplevel);
	return v && !view_active(v);
}
static void surf_commit(struct wl_listener *l, void *data)
{
	(void)data;
	struct surf_track *t = wl_container_of(l, t, commit);
	dmab_commit(t);
	if (surface_hidden(t->s)) {
		++hidden_commits;
		return;
	}
	mark_dirty();
}
static void surf_destroy(struct wl_listener *l, void *data)
{
	(void)data;
	struct surf_track *t = wl_container_of(l, t, destroy);
	wl_list_remove(&t->commit.link);
	wl_list_remove(&t->destroy.link);
	wl_list_remove(&t->link);
	for (int i = 0; i < DMAB_PER_SURF; ++i)
		dmab_drop(&t->db[i]);
	free(t);
}
static void new_surface(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_surface *s = data;
	struct surf_track *t = calloc(1, sizeof(*t));
	if (!t)
		return;
	t->s = s;
	t->cur = -1;
	wl_list_insert(&tracks, &t->link);
	t->commit.notify = surf_commit;
	wl_signal_add(&s->events.commit, &t->commit);
	t->destroy.notify = surf_destroy;
	wl_signal_add(&s->events.destroy, &t->destroy);
}

/* ---- text-input-v3 ------------------------------------------------------------------------------------- */
static void text_focus_send(int on, uint32_t purpose)
{
	static int last_on = -1;
	static uint32_t last_purpose;
	if (on == last_on && (!on || purpose == last_purpose))
		return;
	last_on = on;
	last_purpose = purpose;
	if (on < 0)
		return;                            /* text_focus_resend: forgotten, nothing sent */
	say("text focus %d (purpose %u)", on, purpose);
	if (v2)
		g2h_send(SBG_MSG_TEXT_FOCUS, (uint32_t)on, purpose);
}

/* after ACTIVATE: the current state again, even unchanged (the host resets its keyboard per app) */
static void text_focus_resend(void)
{
	text_focus_send(-1, 0);                    /* forget the last one */
	text_focus_send(S.ti_on ? 1 : 0, S.ti_on ? S.ti_purpose : 0);
}

/* 8o (the user: the keyboard "is hard to close"; Track A adds the dismiss): once the host has hidden its keyboard over
 * a field that still has the focus, a tap on that field must bring it back. GTK asks for that with a text-input
 * commit (not an enable: the field never lost it), and both sides dropped a repeated TEXT_FOCUS 1. So a touch on
 * the client whose text input is enabled is taken as the request: 150 ms after the lift (the app's own focus change
 * comes first), if that text input is still enabled, TEXT_FOCUS 1 again (gwin passes a repeated 1 to the shell). */
static struct wl_event_source *retap_src;
static struct wl_client *retap_client;
static struct wlr_surface *retap_surface;
static double retap_y;
/* the tap was on the field's line: within 24 logical px of the caret's rectangle, vertically (GTK keeps a field's
 * focus when the user taps a label or a button: no keyboard for those); no caret rectangle: any tap */
static int retap_on_field(void)
{
	struct wlr_text_input_v3 *ti = S.ti_on->ti;
	if (!(ti->active_features & WLR_TEXT_INPUT_V3_FEATURE_CURSOR_RECTANGLE) || ti->current.cursor_rectangle.height <= 0)
		return 1;
	if (ti->focused_surface != retap_surface)
		return 0;
	const struct wlr_box *r = &ti->current.cursor_rectangle;
	return retap_y >= r->y - 24 && retap_y <= r->y + r->height + 24;
}
static int retap_cb(void *data)
{
	(void)data;
	if (S.ti_on && retap_client && wl_resource_get_client(S.ti_on->ti->resource) == retap_client && retap_on_field()) {
		say("text focus: a tap in the focused field: the keyboard asked again");
		text_focus_resend();
	}
	retap_client = NULL;
	return 0;
}
static void retap_arm(struct wl_client *c, struct wlr_surface *surf, double y)
{
	if (!c || !S.ti_on || wl_resource_get_client(S.ti_on->ti->resource) != c)
		return;
	if (!retap_src)
		retap_src = wl_event_loop_add_timer(S.loop, retap_cb, NULL);
	retap_client = c;
	retap_surface = surf;
	retap_y = y;
	if (retap_src)
		wl_event_source_timer_update(retap_src, 150);
}

static void ti_off(struct tinput *t)
{
	if (S.ti_on == t) {
		S.ti_on = NULL;
		text_focus_send(0, 0);
	}
}

static void ti_enable(struct wl_listener *l, void *data)
{
	(void)data;
	struct tinput *t = wl_container_of(l, t, enable);
	if (!t->ti->focused_surface || t->ti->focused_surface != S.kfocus)
		return;
	S.ti_on = t;
	S.ti_purpose = t->ti->current.content_type.purpose;
	text_focus_send(1, S.ti_purpose);
}
static void ti_commit(struct wl_listener *l, void *data)
{
	(void)data;
	struct tinput *t = wl_container_of(l, t, commit);
	if (S.ti_on == t && t->ti->current.content_type.purpose != S.ti_purpose) {
		S.ti_purpose = t->ti->current.content_type.purpose;
		text_focus_send(1, S.ti_purpose);
	}
}
static void ti_disable(struct wl_listener *l, void *data)
{
	(void)data;
	struct tinput *t = wl_container_of(l, t, disable);
	ti_off(t);
}
static void ti_destroy(struct wl_listener *l, void *data)
{
	(void)data;
	struct tinput *t = wl_container_of(l, t, destroy);
	ti_off(t);
	wl_list_remove(&t->enable.link);
	wl_list_remove(&t->commit.link);
	wl_list_remove(&t->disable.link);
	wl_list_remove(&t->destroy.link);
	wl_list_remove(&t->link);
	free(t);
}

/* the text inputs follow the keyboard focus: enter on the focused surface's client's, leave elsewhere */
static void ti_follow(void)
{
	struct tinput *t;
	wl_list_for_each(t, &S.tinputs, link) {
		struct wlr_text_input_v3 *ti = t->ti;
		if (ti->focused_surface && ti->focused_surface != S.kfocus) {
			ti_off(t);
			wlr_text_input_v3_send_leave(ti);
		}
		if (S.kfocus && !ti->focused_surface &&
		    wl_resource_get_client(ti->resource) == wl_resource_get_client(S.kfocus->resource))
			wlr_text_input_v3_send_enter(ti, S.kfocus);
	}
}

static void new_text_input(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_text_input_v3 *ti = data;
	struct tinput *t = calloc(1, sizeof(*t));
	if (!t)
		return;
	t->ti = ti;
	t->enable.notify = ti_enable;
	wl_signal_add(&ti->events.enable, &t->enable);
	t->commit.notify = ti_commit;
	wl_signal_add(&ti->events.commit, &t->commit);
	t->disable.notify = ti_disable;
	wl_signal_add(&ti->events.disable, &t->disable);
	t->destroy.notify = ti_destroy;
	wl_signal_add(&ti->events.destroy, &t->destroy);
	wl_list_insert(&S.tinputs, &t->link);
	ti_follow();
	cat_dirty = 1;                             /* KEYBOARD may change */
}

/* 8i ("half the screen"): GTK 3 / 4 make their text-input-v3 object only when a text field first gets the focus, so
 * Weather and Celluloid had none, were flagged KEYBOARD (the fallback for toolkits without text-input), and the shell
 * raised its keyboard and gave them the rows above it. A GTK client never needs the fallback: its text fields ask for
 * the keyboard themselves (SBG_MSG_TEXT_FOCUS). Looked up once per window in the process's mapped libraries. */
static int view_gtk(struct view *v)
{
	if (v->gtk)
		return v->gtk == 1;
	v->gtk = 2;
	if (v->pid <= 0)
		return 0;
	char fn[64], line[512];
	snprintf(fn, sizeof(fn), "/proc/%d/maps", (int)v->pid);
	FILE *f = fopen(fn, "re");
	if (!f) {
		v->gtk = 0;                        /* (not readable yet: again at the next look) */
		return 0;
	}
	while (fgets(line, sizeof(line), f))
		if (strstr(line, "/libgtk-4.so") || strstr(line, "/libgtk-3.so")) {
			v->gtk = 1;
			break;
		}
	fclose(f);
	return v->gtk == 1;
}

static int client_has_text_input(struct wl_client *c)
{
	struct tinput *t;
	wl_list_for_each(t, &S.tinputs, link)
		if (wl_resource_get_client(t->ti->resource) == c)
			return 1;
	return 0;
}

static void text_commit(const char *s)
{
	if (!S.ti_on) {
		say("text \"%s\" but no text field is enabled", s);
		return;
	}
	wlr_text_input_v3_send_commit_string(S.ti_on->ti, s);
	wlr_text_input_v3_send_done(S.ti_on->ti);
}

/* ---- views ------------------------------------------------------------------------------------------- */
static struct view *view_of(struct wlr_xdg_toplevel *t)
{
	struct view *v;
	wl_list_for_each(v, &S.views, link)
		if (v->top == t)
			return v;
	return NULL;
}

/* the main window: maximized to the viewport at 0,0; a dialog (a parent, or a fixed size): its own size (at most the
 * viewport), centered in the viewport */
static void view_layout(struct view *v)
{
	if (!XDG_READY(v->top->base))
		return;
	struct wlr_xdg_toplevel_state *cs = &v->top->current;
	int dialog = v->top->parent || (cs->min_width && cs->min_width == cs->max_width && cs->min_height &&
					cs->min_height == cs->max_height);
	int w, h;
	int aw = (int)LW(), ah = (int)LH();
	if (!dialog) {
		w = aw, h = ah;
	} else {
		w = v->gw > aw ? aw : 0;
		h = v->gh > ah ? ah : 0;
		if (w || h) {
			w = w ? w : v->gw;
			h = h ? h : v->gh;
		}
	}
	v->dialog = dialog;
	if (!v->asked || dialog != v->adialog || w != v->aw || h != v->ah) {
		v->asked = 1, v->adialog = dialog, v->aw = w, v->ah = h;
		wlr_xdg_toplevel_set_maximized(v->top, !dialog);
		wlr_xdg_toplevel_set_tiled(v->top, dialog ? WLR_EDGE_NONE
						   : WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
		wlr_xdg_toplevel_set_size(v->top, w, h);
	}
	if (v->deco)
		wlr_xdg_toplevel_decoration_v1_set_mode(v->deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	int x = 0, y = 0;
	if (dialog) {
		x = (aw - v->gw) / 2;
		y = (ah - v->gh) / 2;
		x = x < 0 ? 0 : x;
		y = y < 0 ? 0 : y;
	}
	struct wlr_box geo;
	wlr_xdg_surface_get_geometry(v->top->base, &geo);
	v->x = x, v->y = y;
	wlr_scene_node_set_position(&v->tree->node, x - geo.x, y - geo.y);
}

static void title_publish(void)
{
	const char *t = NULL;
	struct view *v;
	wl_list_for_each(v, &S.views, link)
		if (v->mapped && view_active(v) && v->top->title)
			t = v->top->title;
	if (!t)
		return;
	if (!strncmp((const char *)hdr->title, t, sizeof(hdr->title) - 1))
		return;
	for (unsigned i = 0; i < sizeof(hdr->title) - 1; ++i) {
		hdr->title[i] = t[i];
		if (!t[i])
			break;
	}
	hdr->title[sizeof(hdr->title) - 1] = 0;
	g2h_send(SBG_MSG_TITLE, 0, 0);
}

/* the keyboard (and the text inputs) go to the active app's top window */
static void refocus(void)
{
	struct view *top = NULL, *v;
	wl_list_for_each(v, &S.views, link) {
		int on = view_active(v);
		wlr_scene_node_set_enabled(&v->tree->node, on);
		if (on && v->mapped)
			top = v;
	}
	struct wlr_surface *s = top ? top->top->base->surface : NULL;
	if (s != S.kfocus) {
		struct view *old = NULL;
		wl_list_for_each(v, &S.views, link)
			if (v->mapped && v->top->base->surface == S.kfocus)
				old = v;
		if (old)
			wlr_xdg_toplevel_set_activated(old->top, false);
		S.kfocus = s;
		if (s) {
			wlr_xdg_toplevel_set_activated(top->top, true);
			wlr_seat_keyboard_notify_enter(S.seat, s, S.kbd.keycodes, S.kbd.num_keycodes, &S.kbd.modifiers);
			if (kq_n)
				say("%u keys held while the app started: sent now", kq_n);
			for (unsigned i = 0; i < kq_n; ++i)
				key(kq[i].code, (int)kq[i].down, (uint32_t)ms_now());
			kq_n = 0;
		} else {
			wlr_seat_keyboard_notify_clear_focus(S.seat);
		}
		ti_follow();
	}
	title_publish();
	mark_dirty();
}

/* which app a window belongs to: a window of the same client; the process tree sbwl started; the app_id against the
 * desktop id / StartupWMClass / the Exec's program; else the app launched most recently */
static pid_t ppid_of(pid_t p)
{
	char fn[64], b[512];
	snprintf(fn, sizeof(fn), "/proc/%d/stat", p);
	int fd = open(fn, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	ssize_t n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	b[n] = 0;
	char *r = strrchr(b, ')');
	int pp = 0;
	if (!r || sscanf(r + 1, " %*c %d", &pp) != 1)
		return 0;
	return pp;
}

static struct inst *inst_of_pid(pid_t pid)
{
	for (int depth = 0; pid > 1 && depth < 64; ++depth) {
		struct inst *in;
		wl_list_for_each(in, &S.insts, link)
			if (in->pid == pid || (!in->exited && getpgid(pid) == in->pid))
				return in;
		pid = ppid_of(pid);
	}
	return NULL;
}

static int id_match(const char *app_id, const struct app *a)
{
	if (!app_id || !*app_id)
		return 0;
	if (!strcasecmp(app_id, a->id) || (a->wmclass[0] && !strcasecmp(app_id, a->wmclass)) ||
	    (a->bin[0] && !strcasecmp(app_id, a->bin)))
		return 1;
	const char *dot = strrchr(a->id, '.');     /* org.gnome.Calculator vs "gnome-calculator": no; "Calculator": yes */
	return dot && !strcasecmp(app_id, dot + 1);
}

static void view_match(struct view *v)
{
	struct wl_client *c = wl_resource_get_client(v->top->resource);
	pid_t pid = 0;
	wl_client_get_credentials(c, &pid, NULL, NULL);
	v->pid = pid;
	const char *how = "same client";
	struct view *o;
	wl_list_for_each(o, &S.views, link)
		if (o != v && o->desk[0] && wl_resource_get_client(o->top->resource) == c) {
			snprintf(v->desk, sizeof(v->desk), "%s", o->desk);
			goto done;
		}
	struct inst *in = inst_of_pid(pid);
	if (in) {
		how = "process tree";
		snprintf(v->desk, sizeof(v->desk), "%s", in->desk);
		goto done;
	}
	for (int i = 0; i < napps; ++i)
		if (id_match(v->top->app_id, &apps[i])) {
			how = "app_id";
			snprintf(v->desk, sizeof(v->desk), "%s", apps[i].id);
			goto done;
		}
	struct inst *last = NULL;
	wl_list_for_each(in, &S.insts, link)
		if (!last || in->t_start > last->t_start)
			last = in;
	if (last) {
		how = "the latest launch";
		snprintf(v->desk, sizeof(v->desk), "%s", last->desk);
	} else {
		how = "nothing (unknown app)";
		snprintf(v->desk, sizeof(v->desk), "?%s", v->top->app_id ? v->top->app_id : "unknown");
	}
done:
	say("window \"%s\" (app_id %s, pid %d) -> %s (%s)", v->top->title ? v->top->title : "", v->top->app_id ? v->top->app_id : "-",
	    pid, v->desk, how);
}

static void view_map(struct wl_listener *l, void *data)
{
	(void)data;
	struct view *v = wl_container_of(l, v, map);
	if (!v->desk[0])
		view_match(v);
	struct wlr_box geo;
	wlr_xdg_surface_get_geometry(v->top->base, &geo);
	v->gw = geo.width, v->gh = geo.height;
	v->mapped = 1;
	wl_list_remove(&v->link);                  /* the most recent mapped last (on top) */
	wl_list_insert(S.views.prev, &v->link);
	wlr_scene_node_raise_to_top(&v->tree->node);
	view_layout(v);
	struct wlr_surface *su = v->top->base->surface;
	say("window mapped: %s, %s, surface %dx%d at %d,%d%s", v->top->title ? v->top->title : "(no title)", v->desk,
	    su->current.width, su->current.height, v->x, v->y, v->dialog ? " (dialog)" : "");
	cat_dirty = 1;
	refocus();
	window_state();
}

static void view_unmap(struct wl_listener *l, void *data)
{
	(void)data;
	struct view *v = wl_container_of(l, v, unmap);
	v->mapped = 0;
	cat_dirty = 1;
	refocus();
	window_state();
}

static void view_commit(struct wl_listener *l, void *data)
{
	(void)data;
	struct view *v = wl_container_of(l, v, commit);
#if WLR18
	if (v->top->base->initial_commit) {
		v->asked = 0;                          /* the first configure: now allowed (0.18) */
		view_layout(v);
		return;
	}
#endif
	if (!v->mapped)
		return;
	struct wlr_box geo;
	wlr_xdg_surface_get_geometry(v->top->base, &geo);
	if (geo.width != v->gw || geo.height != v->gh || v->top->parent != NULL) {
		v->gw = geo.width, v->gh = geo.height;
		view_layout(v);
	} else {
		struct wlr_box g2;
		wlr_xdg_surface_get_geometry(v->top->base, &g2);
		wlr_scene_node_set_position(&v->tree->node, v->x - g2.x, v->y - g2.y);
	}
}

static void view_title(struct wl_listener *l, void *data)
{
	(void)l, (void)data;
	title_publish();
}

static void deco_destroy(struct wl_listener *l, void *data)
{
	(void)data;
	struct view *v = wl_container_of(l, v, deco_destroy);
	wl_list_remove(&v->deco_destroy.link);
	wl_list_remove(&v->deco_mode.link);
	v->deco = NULL;
}
static void deco_mode(struct wl_listener *l, void *data)
{
	(void)data;
	struct view *v = wl_container_of(l, v, deco_mode);
	if (XDG_READY(v->top->base))
		wlr_xdg_toplevel_decoration_v1_set_mode(v->deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
static void new_deco(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_xdg_toplevel_decoration_v1 *d = data;
	struct view *v = view_of(d->toplevel);
	if (!v)
		return;
	v->deco = d;
	v->deco_destroy.notify = deco_destroy;
	wl_signal_add(&d->events.destroy, &v->deco_destroy);
	v->deco_mode.notify = deco_mode;
	wl_signal_add(&d->events.request_mode, &v->deco_mode);
	deco_mode(&v->deco_mode, NULL);
}

static void view_destroy(struct wl_listener *l, void *data)
{
	(void)data;
	struct view *v = wl_container_of(l, v, destroy);
	wl_list_remove(&v->map.link);
	wl_list_remove(&v->unmap.link);
	wl_list_remove(&v->destroy.link);
	wl_list_remove(&v->title.link);
	wl_list_remove(&v->commit.link);
	if (v->deco)
		deco_destroy(&v->deco_destroy, NULL);
	wl_list_remove(&v->link);
	free(v);
	cat_dirty = 1;
	refocus();
	window_state();
}

static void new_toplevel_cb(struct wlr_xdg_toplevel *t)
{
	struct view *v = calloc(1, sizeof(*v));
	if (!v)
		return;
	v->top = t;
	v->tree = wlr_scene_xdg_surface_create(&S.scene->tree, t->base);
	t->base->data = v->tree;
	wlr_scene_node_set_enabled(&v->tree->node, 0);
	wl_list_insert(S.views.prev, &v->link);
	v->map.notify = view_map;
	wl_signal_add(&t->base->surface->events.map, &v->map);
	v->unmap.notify = view_unmap;
	wl_signal_add(&t->base->surface->events.unmap, &v->unmap);
	v->commit.notify = view_commit;
	wl_signal_add(&t->base->surface->events.commit, &v->commit);
	v->destroy.notify = view_destroy;
	wl_signal_add(&t->base->events.destroy, &v->destroy);
	v->title.notify = view_title;
	wl_signal_add(&t->events.set_title, &v->title);
	view_layout(v);                            /* 0.17: configure now; 0.18: at the initial commit */
}

/* a popup stays inside the viewport: the box in its toplevel's surface coordinates */
static void popup_place(struct wlr_xdg_popup *p)
{
	struct wlr_xdg_surface *x = p->base;
	while (x && x->role == WLR_XDG_SURFACE_ROLE_POPUP)
		x = wlr_xdg_surface_try_from_wlr_surface(x->popup->parent);
	struct view *v = x && x->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL ? view_of(x->toplevel) : NULL;
	struct wlr_box box = {0, 0, (int)LW(), (int)LH()};
	if (v) {
		struct wlr_box geo;
		wlr_xdg_surface_get_geometry(v->top->base, &geo);
		box.x = geo.x - v->x;
		box.y = geo.y - v->y;
	}
	wlr_xdg_popup_unconstrain_from_box(p, &box);
}
static void popup_commit(struct wl_listener *l, void *data)
{
	(void)data;
	struct popup *pp = wl_container_of(l, pp, commit);
#if WLR18
	if (pp->p->base->initial_commit) {
		popup_place(pp->p);
		wlr_xdg_surface_schedule_configure(pp->p->base);
	}
#else
	(void)pp;
#endif
}
static void popup_destroy(struct wl_listener *l, void *data)
{
	(void)data;
	struct popup *pp = wl_container_of(l, pp, destroy);
	wl_list_remove(&pp->commit.link);
	wl_list_remove(&pp->destroy.link);
	free(pp);
}
static void new_popup_cb(struct wlr_xdg_popup *p)
{
	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(p->parent);
	if (!parent || !parent->data)
		return;
	p->base->data = wlr_scene_xdg_surface_create(parent->data, p->base);
	struct popup *pp = calloc(1, sizeof(*pp));
	if (!pp)
		return;
	pp->p = p;
	pp->commit.notify = popup_commit;
	wl_signal_add(&p->base->surface->events.commit, &pp->commit);
	pp->destroy.notify = popup_destroy;
	wl_signal_add(&p->base->events.destroy, &pp->destroy);
#if !WLR18
	popup_place(p);
#endif
}

#if WLR18
static void new_toplevel(struct wl_listener *l, void *data) { (void)l; new_toplevel_cb(data); }
static void new_popup(struct wl_listener *l, void *data) { (void)l; new_popup_cb(data); }
#else
static void new_xdg_surface(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_xdg_surface *xdg = data;
	if (xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL)
		new_toplevel_cb(xdg->toplevel);
	else if (xdg->role == WLR_XDG_SURFACE_ROLE_POPUP)
		new_popup_cb(xdg->popup);
}
#endif

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
	if (ROT == 1 || ROT == 3) {               /* surface pixels to the turned content's (publish_window's inverse) */
		int32_t px = x, py = y;
		x = ROT == 1 ? py : (int32_t)H - 1 - py;
		y = ROT == 1 ? (int32_t)W - 1 - px : px;
	}
	double sx = 0, sy = 0;
	struct wlr_surface *s = surface_at((double)x / SC, (double)y / SC, &sx, &sy);
	static struct wl_client *down_client;
	static struct wlr_surface *down_surface;
	static double down_y;
	if (phase == SBG_TOUCH_UP && down_client) {
		retap_arm(down_client, down_surface, down_y);
		down_client = NULL;
	}
	if (phase == SBG_TOUCH_DOWN) {
		down_client = s ? wl_resource_get_client(s->resource) : NULL;
		down_surface = s;
		down_y = sy;
		if (!s) {
			static int t_none;
			say("%stouch at %d,%d: no surface there", t_none++ < 3 ? "STARBITE-TOUCH: " : "", x, y);
			return;
		}
		say("touch at %d,%d: surface %p at %.0f,%.0f, as %s", x, y, (void *)s, sx, sy,
		    client_has_touch(s) ? "wl_touch" : "pointer (the client has no wl_touch)");
		/* 8i ("no touch" on the phone, fine in hostsim): the first three per app into pmsg (guest.c keeps lines
		 * with STARBITE): whether taps arrive, where, and how they are delivered */
		static char t_app[128];
		static int t_n;
		if (strcmp(t_app, S.active)) {
			snprintf(t_app, sizeof(t_app), "%s", S.active);
			t_n = 0;
		}
		if (t_n++ < 3)
			say("STARBITE-TOUCH: %s: px %d,%d -> logical %.0f,%.0f in its surface, %s (scale %u)", S.active[0] ? S.active : "-",
			    x, y, sx, sy, client_has_touch(s) ? "wl_touch" : "pointer", SC);
		if (client_has_touch(s)) {
			S.touch_surface = s;
			S.touch_down = 1;
			wlr_seat_touch_notify_down(S.seat, s, t, 0, sx, sy);
			wlr_seat_touch_notify_frame(S.seat);
		} else {                                       /* pointer emulation: move there, press */
			wlr_seat_pointer_notify_enter(S.seat, s, sx, sy);
			wlr_seat_pointer_notify_motion(S.seat, t, sx, sy);
			wlr_seat_pointer_notify_button(S.seat, t, BTN_LEFT, BTN_PRESSED);
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
			wlr_seat_pointer_notify_button(S.seat, t, BTN_LEFT, BTN_RELEASED);
			S.ptr_down = 0;
		}
		wlr_seat_pointer_notify_frame(S.seat);
	}
}

static void key(uint32_t code, int down, uint32_t t)
{
	if (!S.kfocus && S.active[0]) {
		if (kq_n < sizeof(kq) / sizeof(kq[0]))
			kq[kq_n].code = code, kq[kq_n++].down = (uint32_t)down;
		return;
	}
	struct wlr_keyboard_key_event e = {
		.time_msec = t, .keycode = code, .update_state = true,
		.state = down ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED,
	};
	wlr_keyboard_notify_key(&S.kbd, &e);
}

/* ---- v3: the key bar's modifiers (SBG_MSG_KEYMOD) ----------------------------------------------------------------
 * The host's state: a = the SBSHM_MOD_* held from now on, until the next KEYMOD (a latched one-shot modifier is the
 * host's: KEYMOD m, the key, KEYMOD 0). sbwl presses / releases real modifier keys on the virtual keyboard, so the
 * xkb state and every client's wl_keyboard.modifiers agree (foot, GTK, Qt read them there). The right-hand keys,
 * so a host's own KEY_LEFTSHIFT around a capital letter does not release the bar's Shift. */
static uint32_t keymods;
static const struct { uint32_t bit, code; } modkeys[] = {
	{SBSHM_MOD_CTRL, KEY_RIGHTCTRL}, {SBSHM_MOD_ALT, KEY_LEFTALT}, {SBSHM_MOD_SUPER, KEY_LEFTMETA}, {SBSHM_MOD_SHIFT, KEY_RIGHTSHIFT},
};
static void keymod_set(uint32_t m, uint32_t t)
{
	for (unsigned i = 0; i < sizeof(modkeys) / sizeof(modkeys[0]); ++i)
		if ((m ^ keymods) & modkeys[i].bit)
			key(modkeys[i].code, !!(m & modkeys[i].bit), t);
	if (m != keymods)
		say("key modifiers %s%s%s%s", m & SBSHM_MOD_CTRL ? "Ctrl " : "", m & SBSHM_MOD_ALT ? "Alt " : "",
		    m & SBSHM_MOD_SUPER ? "Super " : "", m & SBSHM_MOD_SHIFT ? "Shift " : m ? "" : "none");
	keymods = m;
}

/* SBG_MSG_TEXT as key presses (US layout): with Ctrl / Alt / Super held (a text-input commit carries no modifiers:
 * Ctrl+C must be a key), or when no text-input-v3 field is enabled (terminals, plain clients). 0: not all ASCII */
static int text_as_keys(const char *s, uint32_t t)
{
	int sh;
	for (const char *p = s; *p; ++p)
		if ((uint8_t)*p >= 0x80 || !sbshm_ascii_key(*p, &sh))
			return 0;
	for (const char *p = s; *p; ++p) {
		uint32_t code = sbshm_ascii_key(*p, &sh);
		int shift = sh && !(keymods & SBSHM_MOD_SHIFT);
		if (shift)
			key(SBSHM_KEY_LEFTSHIFT, 1, t);
		key(code, 1, t);
		key(code, 0, t);
		if (shift)
			key(SBSHM_KEY_LEFTSHIFT, 0, t);
	}
	return 1;
}

/* ---- v3: the clipboard both ways (struct sbshm_clip) ------------------------------------------------------------
 * A Wayland app sets the selection: sbwl reads it through a pipe from the app (event-loop fd, never blocking; 3 s at
 * most), text as UTF-8 ("text/plain;charset=utf-8", or UTF8_STRING / text/plain / TEXT / STRING), else image/png,
 * at most SBSHM_CLIP_MAX bytes (bigger: len 0, the mime says what it was), into the g2h slot, then SBG_MSG_CLIP_SET.
 * SBG_MSG_CLIP: sbwl becomes the selection's owner with the host's bytes (text under the usual text names); that
 * selection is never sent back to the host. The primary selection works between the guest's apps only. */
static volatile struct sbshm_clip *clip;           /* set when the host serves SBSHM_SVC_CLIP */
static struct wlr_data_source *host_src;           /* the selection sbwl made from the host's clipboard */
static struct {
	int fd, png, over;
	struct wl_event_source *src;
	uint8_t *buf;
	size_t n;
	uint64_t t0;
} crd = {.fd = -1};
static const char *const text_mimes[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING", NULL};

static void clip_read_end(int ok)
{
	if (crd.src)
		wl_event_source_remove(crd.src);
	if (crd.fd >= 0)
		close(crd.fd);
	crd.src = NULL;
	crd.fd = -1;
	if (ok && clip) {
		volatile struct sbshm_clip_slot *c = &clip->g2h;
		const char *mime = crd.png ? "image/png" : "text/plain;charset=utf-8";
		uint32_t len = crd.over ? 0 : (uint32_t)crd.n, s = c->seq | 1U;
		c->seq = s;                            /* odd: being written */
		sbshm_dmb();
		for (unsigned i = 0; i < sizeof(c->mime); ++i)
			c->mime[i] = i < strlen(mime) ? mime[i] : 0;
		if (len)
			memcpy((void *)c->data, crd.buf, len);
		c->len = len;
		sbshm_dmb();
		c->seq = s + 1U;
		g2h_send(SBG_MSG_CLIP_SET, s + 1U, 0);
		say("clipboard -> host: %u bytes of %s%s", len, mime, crd.over ? " (too big: offered as empty)" : "");
	}
	free(crd.buf);
	crd.buf = NULL;
	crd.n = 0;
}

static int clip_readable(int fd, uint32_t mask, void *data)
{
	(void)mask, (void)data;
	char scratch[4096];
	for (;;) {
		ssize_t r = crd.n <= SBSHM_CLIP_MAX ? read(fd, crd.buf + crd.n, SBSHM_CLIP_MAX + 1 - crd.n)
						     : read(fd, scratch, sizeof(scratch));
		if (r > 0) {
			if (crd.n <= SBSHM_CLIP_MAX)
				crd.n += (size_t)r;
			crd.over |= crd.n > SBSHM_CLIP_MAX;
			continue;
		}
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0 && errno == EAGAIN)
			return 0;
		clip_read_end(r == 0);                 /* EOF: the whole selection; an error: dropped */
		return 0;
	}
}

static void seat_set_selection(struct wl_listener *l, void *data)
{
	(void)l, (void)data;
	struct wlr_data_source *src = S.seat->selection_source;
	if (!clip || !src || src == host_src)
		return;
	if (crd.fd >= 0)
		clip_read_end(0);                      /* a newer selection replaces the one being read */
	const char *mime = NULL;
	char **m;
	for (int k = 0; text_mimes[k] && !mime; ++k)
		wl_array_for_each(m, &src->mime_types)
			if (!strcmp(*m, text_mimes[k])) {
				mime = *m;
				break;
			}
	int png = 0;
	if (!mime)
		wl_array_for_each(m, &src->mime_types)
			if (!strcmp(*m, "image/png")) {
				mime = *m, png = 1;
				break;
			}
	if (!mime) {
		say("clipboard: the app offers no text or PNG: not passed on");
		return;
	}
	int p[2];
	crd.buf = malloc(SBSHM_CLIP_MAX + 1);
	if (!crd.buf || pipe2(p, O_CLOEXEC) < 0) {
		free(crd.buf);
		crd.buf = NULL;
		return;
	}
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	crd.fd = p[0], crd.png = png, crd.n = 0, crd.over = 0, crd.t0 = ms_now();
	crd.src = wl_event_loop_add_fd(S.loop, p[0], WL_EVENT_READABLE, clip_readable, NULL);
	wlr_data_source_send(src, mime, p[1]);     /* sends the fd to the app, closes ours */
}

/* the host's clipboard as a data source sbwl owns */
struct hsrc {
	struct wlr_data_source base;
	uint8_t *data;
	size_t len;
};
struct hwrite {
	int fd;
	struct wl_event_source *src;
	uint8_t *data;
	size_t len, off;
};
static int hw_writable(int fd, uint32_t mask, void *d)
{
	(void)mask;
	struct hwrite *w = d;
	while (w->off < w->len) {
		ssize_t r = write(fd, w->data + w->off, w->len - w->off);
		if (r > 0) {
			w->off += (size_t)r;
			continue;
		}
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0 && errno == EAGAIN)
			return 0;
		break;                                 /* EPIPE: the app stopped reading */
	}
	wl_event_source_remove(w->src);
	close(fd);
	free(w->data);
	free(w);
	return 0;
}
static void hsrc_send(struct wlr_data_source *s, const char *mime, int32_t fd)
{
	(void)mime;
	struct hsrc *h = wl_container_of(s, h, base);
	struct hwrite *w = calloc(1, sizeof(*w));
	if (w)
		w->data = malloc(h->len ? h->len : 1);
	if (!w || !w->data) {
		if (w)
			free(w);
		close(fd);
		return;
	}
	memcpy(w->data, h->data, h->len);
	w->len = h->len, w->fd = fd;
	fcntl(fd, F_SETFL, O_NONBLOCK);
	w->src = wl_event_loop_add_fd(S.loop, fd, WL_EVENT_WRITABLE, hw_writable, w);
	if (!w->src) {
		close(fd);
		free(w->data);
		free(w);
	}
}
static void hsrc_destroy(struct wlr_data_source *s)
{
	struct hsrc *h = wl_container_of(s, h, base);
	if (host_src == s)
		host_src = NULL;
	free(h->data);
	free(h);
}
static const struct wlr_data_source_impl hsrc_impl = {.send = hsrc_send, .destroy = hsrc_destroy};

static void clip_from_host(void)
{
	if (!clip)
		return;
	volatile struct sbshm_clip_slot *c = &clip->h2g;
	uint8_t *b = malloc(SBSHM_CLIP_MAX);
	char mime[40];
	uint32_t len = 0;
	int ok = 0;
	for (int tries = 0; b && tries < 50 && !ok; ++tries) {
		uint32_t s0 = sbshm_seq_begin(&c->seq);
		if (s0 & 1U) {
			usleep(200);
			continue;
		}
		len = c->len;
		if (len > SBSHM_CLIP_MAX)
			len = 0;
		for (unsigned i = 0; i < sizeof(mime); ++i)
			mime[i] = c->mime[i];
		mime[sizeof(mime) - 1] = 0;
		memcpy(b, (const void *)c->data, len);
		ok = sbshm_seq_ok(&c->seq, s0);
	}
	if (!ok || !len) {
		say("clipboard <- host: %s", !ok ? "no stable copy of the slot" : "empty (or too big)");
		free(b);
		return;
	}
	struct hsrc *h = calloc(1, sizeof(*h));
	if (!h) {
		free(b);
		return;
	}
	wlr_data_source_init(&h->base, &hsrc_impl);
	h->data = b, h->len = len;
	int text = !mime[0] || !strncmp(mime, "text/plain", 10);
	for (int k = 0; text ? text_mimes[k] != NULL : k < 1; ++k) {
		char **p = wl_array_add(&h->base.mime_types, sizeof(*p));
		if (p)
			*p = strdup(text ? text_mimes[k] : mime);
	}
	host_src = &h->base;
	wlr_seat_set_selection(S.seat, &h->base, wl_display_next_serial(S.display));
	say("clipboard <- host: %u bytes of %s", len, text ? "text" : mime);
}

static void req_set_selection(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_seat_request_set_selection_event *e = data;
	wlr_seat_set_selection(S.seat, e->source, e->serial);
}
static void req_set_primary(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_seat_request_set_primary_selection_event *e = data;
	wlr_seat_set_primary_selection(S.seat, e->source, e->serial);
}

/* ---- the .desktop files ----------------------------------------------------------------------------------- */
static const char *user_home(void) { return S.have_user ? S.user.pw_dir : (getenv("HOME") ? getenv("HOME") : "/root"); }

static void unescape_value(char *s)
{
	char *d = s;
	for (; *s; ++s) {
		if (*s == '\\' && s[1]) {
			++s;
			*d++ = *s == 's' ? ' ' : *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == 'r' ? '\r' : *s;
		} else {
			*d++ = *s;
		}
	}
	*d = 0;
}

static int list_has(const char *list, const char *word)
{
	size_t n = strlen(word);
	for (const char *p = list; *p;) {
		const char *e = strchr(p, ';');
		size_t l = e ? (size_t)(e - p) : strlen(p);
		if (l == n && !strncmp(p, word, n))
			return 1;
		p += l + (e ? 1 : 0);
	}
	return 0;
}

static int in_path(const char *prog)
{
	if (strchr(prog, '/'))
		return access(prog, X_OK) == 0;
	const char *path = getenv("PATH") ? getenv("PATH") : "/usr/local/bin:/usr/bin:/bin";
	char buf[512];
	for (const char *p = path; *p;) {
		const char *e = strchr(p, ':');
		size_t l = e ? (size_t)(e - p) : strlen(p);
		snprintf(buf, sizeof(buf), "%.*s/%s", (int)l, p, prog);
		if (access(buf, X_OK) == 0)
			return 1;
		p += l + (e ? 1 : 0);
	}
	return 0;
}

/* Exec= into argv: quoting per the Desktop Entry spec, field codes dropped (%c = Name, %k = the file, %% = %) */
static int exec_split(const struct app *a, char *buf, size_t bn, char **argv, int max)
{
	int argc = 0, q = 0, started = 0;
	size_t o = 0;
	const char *s = a->exec;
	char *cur = buf;
	for (;; ++s) {
		char c = *s;
		if (!c || (!q && (c == ' ' || c == '\t'))) {
			if (started && o < bn && argc < max - 1) {
				buf[o++] = 0;
				argv[argc++] = cur;
				cur = buf + o;
			}
			started = 0;
			if (!c)
				break;
			continue;
		}
		if (c == '"') {
			q = !q;
			started = 1;
			continue;
		}
		if (q && c == '\\' && s[1] && strchr("\"`$\\", s[1])) {
			c = *++s;
		} else if (c == '%' && s[1]) {
			char f = *++s;
			const char *ins = f == '%' ? "%" : f == 'c' ? a->name : f == 'k' ? a->path : NULL;
			for (; ins && *ins && o + 1 < bn; ++ins)
				buf[o++] = *ins, started = 1;
			continue;
		}
		if (o + 1 < bn)
			buf[o++] = c, started = 1;
	}
	argv[argc] = NULL;
	return argc;
}

static int parse_desktop(const char *path, const char *id, struct app *a)
{
	FILE *f = fopen(path, "re");
	if (!f)
		return 0;
	memset(a, 0, sizeof(*a));
	snprintf(a->id, sizeof(a->id), "%s", id);
	snprintf(a->path, sizeof(a->path), "%s", path);
	char line[2048], type[32] = "", tryexec[256] = "", only[256] = "", notin[256] = "", cats[512] = "";
	int sect = 0, nodisplay = 0, hidden = 0;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '[') {
			sect = !strcmp(line, "[Desktop Entry]");
			continue;
		}
		if (!sect || line[0] == '#')
			continue;
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		char *k = line, *v = eq + 1, *ke = eq;
		while (ke > k && ke[-1] == ' ')
			--ke;
		*ke = 0;
		while (*v == ' ')
			++v;
		unescape_value(v);
#define KEY(name, dst) else if (!strcmp(k, name)) snprintf(dst, sizeof(dst), "%s", v)
		if (0) {}
		KEY("Type", type);
		KEY("Name", a->name);
		KEY("Exec", a->exec);
		KEY("Icon", a->icon);
		KEY("StartupWMClass", a->wmclass);
		KEY("TryExec", tryexec);
		KEY("OnlyShowIn", only);
		KEY("NotShowIn", notin);
		KEY("Categories", cats);
		else if (!strcmp(k, "Terminal")) a->terminal = !strcmp(v, "true");
		else if (!strcmp(k, "NoDisplay")) nodisplay = !strcmp(v, "true");
		else if (!strcmp(k, "Hidden")) hidden = !strcmp(v, "true");
#undef KEY
	}
	fclose(f);
	if (strcmp(type, "Application") || !a->exec[0] || !a->name[0] || nodisplay || hidden)
		return 0;
	if ((only[0] && !list_has(only, DESK_ID)) || list_has(notin, DESK_ID))
		return 0;
	if (tryexec[0] && !in_path(tryexec))
		return 0;
	a->termemu = list_has(cats, "TerminalEmulator");
	char buf[1024], *av[64];
	if (exec_split(a, buf, sizeof(buf), av, 64) < 1)
		return 0;
	const char *b = strrchr(av[0], '/');
	snprintf(a->bin, sizeof(a->bin), "%s", b ? b + 1 : av[0]);
	return 1;
}

static const char *system_list = "/etc/sbwl/system-apps";

/* SYSTEM: listed in /etc/sbwl/system-apps (the base image ships it: the desktop ids it has); without that list,
 * every app under /usr/share/applications */
static int is_system(const char *id, int under_usr)
{
	FILE *f = fopen(system_list, "re");
	if (!f)
		return under_usr;
	char line[256];
	int yes = 0;
	while (!yes && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *s = line, *e;
		if ((e = strrchr(s, '/')))
			s = e + 1;                          /* full paths are fine too */
		size_t n = strlen(s);
		if (n > 8 && !strcmp(s + n - 8, ".desktop"))
			s[n - 8] = 0;
		yes = !strcmp(s, id);
	}
	fclose(f);
	return yes;
}

static void scan_dir(const char *root, const char *rel, struct app **list, int *n, int *cap, int under_usr)
{
	char dn[600];
	snprintf(dn, sizeof(dn), "%s%s%s", root, rel[0] ? "/" : "", rel);
	DIR *d = opendir(dn);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char path[800], sub[300];
		snprintf(path, sizeof(path), "%s/%s", dn, e->d_name);
		struct stat st;
		if (stat(path, &st) < 0)
			continue;
		if (S_ISDIR(st.st_mode)) {                 /* desktop ids of subdirectories: "dir-name" */
			snprintf(sub, sizeof(sub), "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
			if (strlen(sub) < 200)
				scan_dir(root, sub, list, n, cap, under_usr);
			continue;
		}
		size_t l = strlen(e->d_name);
		if (l <= 8 || strcmp(e->d_name + l - 8, ".desktop"))
			continue;
		char id[256];
		snprintf(id, sizeof(id), "%s%s%.*s", rel, rel[0] ? "/" : "", (int)(l - 8), e->d_name);
		for (char *p = id; *p; ++p)
			if (*p == '/')
				*p = '-';
		int at = -1;
		for (int i = 0; i < *n; ++i)
			if (!strcmp((*list)[i].id, id))
				at = i;
		struct app a;
		int ok = parse_desktop(path, id, &a);
		if (at >= 0) {                             /* a later directory overrides (the user's ~/.local) */
			if (ok) {
				a.system = (*list)[at].system;
				(*list)[at] = a;
			} else {
				(*list)[at] = (*list)[--*n];
			}
			continue;
		}
		if (!ok)
			continue;
		a.system = is_system(id, under_usr);
		if (*n == *cap) {
			*cap = *cap ? *cap * 2 : 64;
			*list = realloc(*list, sizeof(**list) * (size_t)*cap);
		}
		(*list)[(*n)++] = a;
	}
	closedir(d);
}

static int app_cmp(const void *x, const void *y)
{
	const struct app *a = x, *b = y;
	int c = strcasecmp(a->name, b->name);
	return c ? c : strcmp(a->id, b->id);
}

static int inotify_fd = -1;
static void watch_dirs(void)
{
	if (inotify_fd < 0)
		return;
	char p[600];
	const char *dirs[] = {"/usr/share/applications", "/usr/local/share/applications", NULL, NULL};
	snprintf(p, sizeof(p), "%s/.local/share/applications", user_home());
	dirs[2] = p;
	for (int i = 0; dirs[i]; ++i)
		inotify_add_watch(inotify_fd, dirs[i], IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM | IN_CLOSE_WRITE |
							IN_ATTRIB);
	if (access("/etc/sbwl", F_OK) == 0)
		inotify_add_watch(inotify_fd, "/etc/sbwl", IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_CLOSE_WRITE);
}

static void cat_publish(void);
static void scan_apps(void)
{
	struct app *list = NULL;
	int n = 0, cap = 0;
	char p[600];
	scan_dir("/usr/share/applications", "", &list, &n, &cap, 1);
	scan_dir("/usr/local/share/applications", "", &list, &n, &cap, 0);
	snprintf(p, sizeof(p), "%s/.local/share/applications", user_home());
	scan_dir(p, "", &list, &n, &cap, 0);
	if (n)
		qsort(list, (size_t)n, sizeof(*list), app_cmp);
	if (n > (int)SBSHM_APPS_MAX) {
		say("catalog: %d apps, the first %u kept", n, SBSHM_APPS_MAX);
		n = SBSHM_APPS_MAX;
	}
	int same = n == napps;
	for (int i = 0; same && i < n; ++i)
		same = !strcmp(list[i].id, apps[i].id);
	free(apps);
	apps = list;
	napps = n;
	if (!same)
		lay_seq = 0;                           /* cat_publish sets it to the new seq */
	say("catalog: %d apps", n);
	watch_dirs();
	cat_dirty = 2;
}

/* ---- icons: the theme (hicolor, Adwaita), PNG >= 128 px, else SVG, else a smaller PNG; 160 x 160 premultiplied ---- */
static uint32_t *icon_px[SBSHM_ICONS_CAT];    /* what each pool slot holds */
static uint8_t icon_new[SBSHM_ICONS_CAT];

#ifndef SBWL_NO_ICONS
static char icon_key[SBSHM_ICONS_CAT][300];
static void icon_try(const char *dir, int size, const char *name, char *best, size_t bn, int *score)
{
	char p[700];
	struct stat st;
	if (size) {
		snprintf(p, sizeof(p), "%s/%s.png", dir, name);
		if (stat(p, &st) == 0) {
			int s = size >= 160 ? 3000 - (size - 160) / 4 : size >= 128 ? 2500 + size : 1000 + size;
			if (s > *score)
				*score = s, snprintf(best, bn, "%s", p);
		}
	}
	snprintf(p, sizeof(p), "%s/%s.svg", dir, name);
	if (stat(p, &st) == 0 && 2000 > *score)
		*score = 2000, snprintf(best, bn, "%s", p);
}

static int icon_find(const char *name, char *out, size_t on)
{
	if (!name[0])
		return 0;
	if (name[0] == '/')
		return access(name, R_OK) == 0 ? (snprintf(out, on, "%s", name), 1) : 0;
	char nm[256];
	snprintf(nm, sizeof(nm), "%s", name);
	size_t l = strlen(nm);
	if (l > 4 && (!strcmp(nm + l - 4, ".png") || !strcmp(nm + l - 4, ".svg") || !strcmp(nm + l - 4, ".xpm")))
		nm[l - 4] = 0;
	int score = 0;
	char roots[3][300];
	snprintf(roots[0], sizeof(roots[0]), "%s/.local/share/icons", user_home());
	snprintf(roots[1], sizeof(roots[1]), "/usr/local/share/icons");
	snprintf(roots[2], sizeof(roots[2]), "/usr/share/icons");
	static const char *themes[] = {"hicolor", "Adwaita"};
	for (int t = 0; t < 2; ++t)
		for (int r = 0; r < 3; ++r) {
			char td[400];
			snprintf(td, sizeof(td), "%s/%s", roots[r], themes[t]);
			DIR *d = opendir(td);
			if (!d)
				continue;
			struct dirent *e;
			while ((e = readdir(d))) {
				int sz = 0, sc = 1;
				if (!strcmp(e->d_name, "scalable"))
					sz = 0;
				else if (sscanf(e->d_name, "%dx%*d@%d", &sz, &sc) >= 1)
					sz *= sc;
				else
					continue;
				char sd[700];
				snprintf(sd, sizeof(sd), "%s/%s", td, e->d_name);
				DIR *d2 = opendir(sd);
				if (!d2)
					continue;
				struct dirent *c;
				while ((c = readdir(d2))) {
					if (c->d_name[0] == '.')
						continue;
					char cd[800];
					snprintf(cd, sizeof(cd), "%s/%s", sd, c->d_name);
					icon_try(cd, sz, nm, out, on, &score);
				}
				closedir(d2);
			}
			closedir(d);
		}
	if (!score)
		icon_try("/usr/share/pixmaps", 128, nm, out, on, &score);
	return score > 0;
}

static int icon_render(const struct app *a, const char *file, uint32_t *px)
{
	memset(px, 0, SBSHM_ICON_BYTES);
	cairo_surface_t *dst = cairo_image_surface_create_for_data((unsigned char *)px, CAIRO_FORMAT_ARGB32, SBSHM_ICON_PX,
								   SBSHM_ICON_PX, SBSHM_ICON_PX * 4);
	cairo_t *cr = cairo_create(dst);
	int ok = 0;
	size_t l = strlen(file);
	if (l > 4 && !strcmp(file + l - 4, ".svg")) {
		GError *err = NULL;
		RsvgHandle *h = rsvg_handle_new_from_file(file, &err);
		if (h) {
			RsvgRectangle vp = {0, 0, SBSHM_ICON_PX, SBSHM_ICON_PX};
			ok = rsvg_handle_render_document(h, cr, &vp, &err);
			g_object_unref(h);
		}
		if (err)
			g_error_free(err);
	} else if (file[0]) {
		cairo_surface_t *s = cairo_image_surface_create_from_png(file);
		if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) {
			double w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
			double k = SBSHM_ICON_PX / (w > h ? w : h);
			cairo_translate(cr, (SBSHM_ICON_PX - w * k) / 2, (SBSHM_ICON_PX - h * k) / 2);
			cairo_scale(cr, k, k);
			cairo_set_source_surface(cr, s, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr), k < 1 ? CAIRO_FILTER_BEST : CAIRO_FILTER_GOOD);
			cairo_paint(cr);
			ok = 1;
		}
		cairo_surface_destroy(s);
	}
	if (!ok) {                                 /* no icon: a tile with the name's first letter */
		uint32_t hsh = 2166136261U;
		for (const char *p = a->id; *p; ++p)
			hsh = (hsh ^ (uint8_t)*p) * 16777619U;
		cairo_new_path(cr);
		double r = 34, s = SBSHM_ICON_PX - 16;
		cairo_identity_matrix(cr);
		cairo_translate(cr, 8, 8);
		cairo_arc(cr, s - r, r, r, -1.5708, 0);
		cairo_arc(cr, s - r, s - r, r, 0, 1.5708);
		cairo_arc(cr, r, s - r, r, 1.5708, 3.1416);
		cairo_arc(cr, r, r, r, 3.1416, 4.7124);
		cairo_close_path(cr);
		cairo_set_source_rgb(cr, 0.25 + (hsh & 0xff) / 512.0, 0.25 + (hsh >> 8 & 0xff) / 512.0, 0.25 + (hsh >> 16 & 0xff) / 512.0);
		cairo_fill(cr);
		char letter[2] = {(char)toupper((unsigned char)a->name[0]), 0};
		cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
		cairo_set_font_size(cr, 84);
		cairo_text_extents_t te;
		cairo_text_extents(cr, letter, &te);
		cairo_move_to(cr, s / 2 - te.x_bearing - te.width / 2, s / 2 - te.y_bearing - te.height / 2);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_show_text(cr, letter);
	}
	cairo_destroy(cr);
	cairo_surface_flush(dst);
	cairo_surface_destroy(dst);
	return ok;
}
#endif

/* instant start (2026-11-13, the phone's card emulated in QEMU: 5.8 s of a 15.8 s HELLO was these renders, librsvg
 * and the theme read from the card): icons_budget renders at most that many icons a call (-1: all). 0 before HELLO
 * (the catalog goes out without them: Starbite can launch at once); after HELLO one per icons_tick, the catalog
 * published again each time. icons_pending: some app still without its icon. */
static int icons_budget = -1, icons_pending;
/* the icons for the catalog's first SBSHM_ICONS_CAT apps (pool index = catalog index), rendered outside the seqlock */
static void icons_prepare(void)
{
#ifndef SBWL_NO_ICONS
	/* the rendered icons by key: an app that moved to another index (one was added before it) takes its pixels along
	 * instead of being rendered again. Pass 1 matches the keys, pass 2 renders the rest into the buffers left over. */
	static uint32_t *old_px[SBSHM_ICONS_CAT];
	static char old_key[SBSHM_ICONS_CAT][300], file[SBSHM_ICONS_CAT][512], key[SBSHM_ICONS_CAT][300];
	if (icons_budget == 0) {                   /* before HELLO: not even the theme's folders (card reads) */
		icons_pending = 1;
		return;
	}
	memcpy(old_px, icon_px, sizeof(old_px));
	memcpy(old_key, icon_key, sizeof(old_key));
	memset(icon_px, 0, sizeof(icon_px));
	int n = napps < (int)SBSHM_ICONS_CAT ? napps : (int)SBSHM_ICONS_CAT;
	for (int i = 0; i < n; ++i) {
		file[i][0] = 0;
		icon_find(apps[i].icon, file[i], sizeof(file[i]));
		snprintf(key[i], sizeof(key[i]), "%s|%s", file[i][0] ? file[i] : "tile", file[i][0] ? "" : apps[i].id);
		for (int j = 0; j < (int)SBSHM_ICONS_CAT; ++j)
			if (old_px[j] && !strcmp(key[i], old_key[j])) {
				icon_px[i] = old_px[j];
				old_px[j] = NULL;
				snprintf(icon_key[i], sizeof(icon_key[i]), "%s", key[i]);
				icon_new[i] |= j != i;     /* the pool slot gets it at the next publish */
				break;
			}
	}
	int budget = icons_budget;
	icons_pending = 0;
	for (int i = 0; i < n; ++i) {
		if (icon_px[i])
			continue;
		if (budget == 0) {
			icons_pending = 1;             /* (rendered at a later icons_tick) */
			continue;
		}
		if (budget > 0)
			--budget;
		for (int j = 0; j < (int)SBSHM_ICONS_CAT && !icon_px[i]; ++j)
			if (old_px[j]) {                   /* a buffer nobody wants any more */
				icon_px[i] = old_px[j];
				old_px[j] = NULL;
			}
		if (!icon_px[i])
			icon_px[i] = malloc(SBSHM_ICON_BYTES);
		if (!icon_px[i])
			continue;
		int ok = icon_render(&apps[i], file[i], icon_px[i]);
		snprintf(icon_key[i], sizeof(icon_key[i]), "%s", key[i]);
		icon_new[i] = 1;
		if (!ok)
			say("icon of %s: %s", apps[i].id, file[i][0] ? "unreadable, a letter tile" : "none in the theme, a letter tile");
	}
	for (int j = 0; j < (int)SBSHM_ICONS_CAT; ++j)
		free(old_px[j]);                       /* icons of apps that are gone */
	memset(old_px, 0, sizeof(old_px));
#endif
}

/* ---- processes: RUNNING, the resident memory of each app's process tree ----------------------------------- */
struct proc {
	pid_t pid, ppid, pgrp;
	uint32_t rss_kb;
};
static int proc_cmp(const void *x, const void *y)
{
	const struct proc *a = x, *b = y;
	return (a->pid > b->pid) - (a->pid < b->pid);
}
static struct proc *procs;
static int nprocs;
static void proc_scan(void)
{
	DIR *d = opendir("/proc");
	if (!d)
		return;
	int cap = 0;
	nprocs = 0;
	long pk = sysconf(_SC_PAGESIZE) / 1024;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!isdigit((unsigned char)e->d_name[0]))
			continue;
		char fn[64], b[1024];
		snprintf(fn, sizeof(fn), "/proc/%s/stat", e->d_name);
		int fd = open(fn, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		ssize_t n = read(fd, b, sizeof(b) - 1);
		close(fd);
		if (n <= 0)
			continue;
		b[n] = 0;
		char *r = strrchr(b, ')');
		struct proc p = {.pid = atoi(e->d_name)};
		long rss = 0;
		if (!r || sscanf(r + 1, " %*c %d %d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %*u %*u %ld",
				 &p.ppid, &p.pgrp, &rss) != 3)
			continue;
		p.rss_kb = (uint32_t)(rss * pk);
		if (nprocs == cap) {
			cap = cap ? cap * 2 : 256;
			procs = realloc(procs, sizeof(*procs) * (size_t)cap);
		}
		procs[nprocs++] = p;
	}
	closedir(d);
	qsort(procs, (size_t)nprocs, sizeof(*procs), proc_cmp);
}
static const struct proc *proc_get(pid_t pid)
{
	struct proc k = {.pid = pid};
	return nprocs ? bsearch(&k, procs, (size_t)nprocs, sizeof(*procs), proc_cmp) : NULL;
}

/* the app a process belongs to: its group or an ancestor is one sbwl started, or a window's client is an ancestor */
static const char *proc_app(const struct proc *p)
{
	for (int depth = 0; p && depth < 64; ++depth) {
		struct inst *in;
		wl_list_for_each(in, &S.insts, link)
			if (p->pid == in->pid || p->pgrp == in->pid)
				return in->desk;
		struct view *v;
		wl_list_for_each(v, &S.views, link)
			if (v->pid == p->pid && v->desk[0])
				return v->desk;
		if (p->ppid <= 1)
			break;
		p = proc_get(p->ppid);
	}
	return NULL;
}

/* ---- the catalog in the shared region (seqlock: sbshm.h) --------------------------------------------------- */
static uint32_t cat_seq;
static struct sbshm_app cat_last[SBSHM_APPS_MAX];
static uint32_t cat_last_n = SBSHM_NONE, cat_last_active = SBSHM_NONE;

static void meminfo(uint32_t *tot, uint32_t *avail, uint32_t *stot, uint32_t *sfree)
{
	FILE *f = fopen("/proc/meminfo", "re");
	char l[128];
	unsigned long v;
	*tot = *avail = *stot = *sfree = 0;
	while (f && fgets(l, sizeof(l), f)) {
		if (sscanf(l, "MemTotal: %lu", &v) == 1) *tot = (uint32_t)v;
		else if (sscanf(l, "MemAvailable: %lu", &v) == 1) *avail = (uint32_t)v;
		else if (sscanf(l, "SwapTotal: %lu", &v) == 1) *stot = (uint32_t)v;
		else if (sscanf(l, "SwapFree: %lu", &v) == 1) *sfree = (uint32_t)v;
	}
	if (f)
		fclose(f);
}

static uint32_t app_flags(int i, uint32_t *rss)
{
	const struct app *a = &apps[i];
	uint32_t f = 0;
	if (a->terminal)
		f |= SBSHM_APP_TERMINAL | SBSHM_APP_KEYBOARD;
	if (a->termemu)
		f |= SBSHM_APP_KEYBOARD;
	if (a->system)
		f |= SBSHM_APP_SYSTEM;
	struct inst *in;
	wl_list_for_each(in, &S.insts, link)
		if (!strcmp(in->desk, a->id))
			f |= SBSHM_APP_RUNNING;
	struct view *v;
	int win = 0, ti = 0;
	wl_list_for_each(v, &S.views, link)
		if (v->mapped && !strcmp(v->desk, a->id)) {
			win = 1;
			ti |= client_has_text_input(wl_resource_get_client(v->top->resource)) || view_gtk(v);
		}
	if (win) {
		f |= SBSHM_APP_WINDOW | SBSHM_APP_RUNNING;
		if (!ti)
			f |= SBSHM_APP_KEYBOARD;           /* no text-input-v3: the host keeps its keyboard up */
	}
	uint32_t kb = 0;
	if (f & SBSHM_APP_RUNNING)
		for (int k = 0; k < nprocs; ++k) {
			const char *d = proc_app(&procs[k]);
			if (d && !strcmp(d, a->id))
				kb += procs[k].rss_kb;
		}
	*rss = kb;
	return f;
}

/* the App Store's map of its installed apps' launchers (sbstore / sbpkg write /run/sbpkg/apps.tsv: "store index
 * <TAB> key <TAB> desktop id" per launcher; 8i: never read, so every app's .pkg stayed NONE and the store's OPEN
 * said "It has no window of its own"). Read again whenever the file changes (an install's rescan comes after it). */
#define AMAP_MAX 256
static struct { char desk[128]; uint32_t idx; } amap[AMAP_MAX];
static int amap_n;
static void amap_load(void)
{
	static struct timespec mt;
	static off_t sz = -1;
	struct stat st;
	if (stat("/run/sbpkg/apps.tsv", &st)) {
		amap_n = 0, sz = -1;
		return;
	}
	if (st.st_size == sz && st.st_mtim.tv_sec == mt.tv_sec && st.st_mtim.tv_nsec == mt.tv_nsec)
		return;
	sz = st.st_size, mt = st.st_mtim;
	FILE *f = fopen("/run/sbpkg/apps.tsv", "re");
	amap_n = 0;
	if (!f)
		return;
	char line[512];
	while (amap_n < AMAP_MAX && fgets(line, sizeof(line), f)) {
		char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
		if (!t2)
			continue;
		char *e = t2 + 1 + strcspn(t2 + 1, "\r\n");
		*e = 0;
		char *end;
		unsigned long idx = strtoul(line, &end, 10);
		if (end == line || end != t1 || idx >= SBSHM_PKGS_MAX || !t2[1])
			continue;
		snprintf(amap[amap_n].desk, sizeof(amap[amap_n].desk), "%s", t2 + 1);
		amap[amap_n++].idx = (uint32_t)idx;
	}
	fclose(f);
	say("catalog: the App Store's launchers: %d (/run/sbpkg/apps.tsv)", amap_n);
}
static uint32_t amap_pkg(const char *desk)
{
	for (int i = 0; i < amap_n; ++i)
		if (!strcmp(amap[i].desk, desk))
			return amap[i].idx;
	return SBSHM_NONE;
}

static void cat_publish(void)
{
	if (!v2)
		return;
	amap_load();
	volatile struct sbshm_cat *c = (volatile struct sbshm_cat *)(shm + SBSHM_CAT_OFF);
	static struct sbshm_app now[SBSHM_APPS_MAX];
	memset(now, 0, sizeof(now));
	if (cat_dirty >= 2)
		icons_prepare();
	int running = 0;
	struct inst *in;
	wl_list_for_each(in, &S.insts, link)
		running = 1;
	if (running || !wl_list_empty(&S.views))
		proc_scan();
	else
		nprocs = 0;
	uint32_t active = S.active[0] && app_index(S.active) >= 0 ? (uint32_t)app_index(S.active) : SBSHM_NONE;
	for (int i = 0; i < napps; ++i) {
		struct sbshm_app *o = &now[i];
		snprintf(o->id, sizeof(o->id), "%s", apps[i].id);
		/* the name cut at a character boundary */
		size_t l = strlen(apps[i].name);
		if (l > sizeof(o->name) - 1) {
			l = sizeof(o->name) - 1;
			while (l && ((uint8_t)apps[i].name[l] & 0xc0) == 0x80)
				--l;
		}
		memcpy(o->name, apps[i].name, l);
		o->flags = app_flags(i, &o->rss_kb);
		o->icon = i < (int)SBSHM_ICONS_CAT && icon_px[i] ? (uint32_t)i : SBSHM_NONE;
		o->pkg = amap_pkg(apps[i].id);
	}
	uint32_t mt, ma, st, sf;
	meminfo(&mt, &ma, &st, &sf);
	int icons = 0;
	for (int i = 0; i < (int)SBSHM_ICONS_CAT; ++i)
		icons |= icon_new[i];
	int changed = icons || cat_last_n != (uint32_t)napps || active != cat_last_active ||
		      memcmp(now, cat_last, sizeof(now)) || cat_dirty >= 2;
	if (!changed && c->seq == cat_seq) {       /* meminfo alone: written in place, no new seq */
		c->mem_total_kb = mt, c->mem_avail_kb = ma, c->swap_total_kb = st, c->swap_free_kb = sf;
		cat_dirty = 0;
		return;
	}
	uint32_t s = cat_seq + 1U;                 /* odd: being written */
	/* the layout (ids and their order) changed: LAUNCH indices read before are stale from here (sbshm.h layout_seq) */
	int relaid = !lay_seq || cat_last_n != (uint32_t)napps;
	for (int i = 0; !relaid && i < napps; ++i)
		relaid = strncmp(cat_last[i].id, now[i].id, sizeof(now[i].id)) != 0;
	if (relaid)
		lay_seq = s + 1U;
	c->seq = s;
	sbshm_dmb();
	c->layout_seq = lay_seq;
	c->n = (uint32_t)napps;
	c->active = active;
	c->mem_total_kb = mt, c->mem_avail_kb = ma, c->swap_total_kb = st, c->swap_free_kb = sf;
	memcpy((void *)c->app, now, sizeof(now));
	for (int i = 0; i < (int)SBSHM_ICONS_CAT; ++i)
		if (icon_new[i] && icon_px[i]) {
			memcpy(shm + SBSHM_ICON_OFF + (size_t)i * SBSHM_ICON_BYTES, icon_px[i], SBSHM_ICON_BYTES);
			icon_new[i] = 0;
		}
	sbshm_dmb();
	cat_seq = s + 1U;
	c->seq = cat_seq;
	memcpy(cat_last, now, sizeof(now));
	cat_last_n = (uint32_t)napps;
	cat_last_active = active;
	cat_dirty = 0;
	g2h_send(SBG_MSG_CATALOG, cat_seq, 0);
}

/* SBG_MSG_WINDOW whenever the active app's window state changes */
static void window_state(void)
{
	if (!v2)
		return;
	static uint32_t la = SBSHM_NONE - 1, lb;
	int ai = S.active[0] ? app_index(S.active) : -1;
	uint32_t a = ai >= 0 ? (uint32_t)ai : SBSHM_NONE, b = 0;
	struct view *v;
	wl_list_for_each(v, &S.views, link)
		b |= v->mapped && view_active(v);
	if (!b)
		win_shown = 0;                     /* its next mapped window counts once it is published */
	b &= (uint32_t)win_shown;
	if (a == la && b == lb)
		return;
	la = a, lb = b;
	say("window: active %d, has window %u", (int)a, b);
	g2h_send(SBG_MSG_WINDOW, a, b);
}

static void text_focus_resend(void);
static void activate(const char *desk)
{
	if (strcmp(S.active, desk ? desk : ""))
		win_shown = 0;                     /* another app: WINDOW b = 1 after its first published frame */
	snprintf(S.active, sizeof(S.active), "%s", desk ? desk : "");
	struct view *v;
	wl_list_for_each(v, &S.views, link)
		if (v->mapped && view_active(v))
			view_layout(v);
	refocus();
	cat_dirty |= 1;
	cat_publish();                             /* the catalog's "active" before the WINDOW message */
	window_state();
	text_focus_resend();                       /* the host learns the new app's text focus at once */
}

/* ---- starting apps ------------------------------------------------------------------------------------- */
static char tzenv[32];                     /* v3: TZ for the apps, POSIX form of hdr.host_tz_min ("" = none) */
static int spawn_fd = -1;                  /* spawn(): passed to the child as fd 3 (sbnotify's line to sbwl) */
/* 8o night (sbgpu): the apps get the Mali through sbguest.ko's render node ("panfrost": Mesa's Panfrost) when
 * sbwl.gpu=1 is on the kernel command line, SBWL_GPU is set or ~/.config/starbite/gpu-apps exists, and the node is
 * there: linux-dmabuf is offered (its
 * main device the node; XRGB / ARGB, linear) and child_env no longer forces cairo / software GL. Off by default until
 * the phone's probe (sbgl-probe) has passed. */
static int gpu_apps;
static bool dmab_check(struct wlr_dmabuf_attributes *a, void *data)
{
	(void)data;
	return a->n_planes == 1 && (a->format == DRM_FORMAT_ARGB8888 || a->format == DRM_FORMAT_XRGB8888) &&
	       (a->modifier == DRM_FORMAT_MOD_LINEAR || a->modifier == DRM_FORMAT_MOD_INVALID);
}
static void gpu_apps_init(void)
{
	char cmd[1024] = "";
	int cf = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
	ssize_t n = cf >= 0 ? read(cf, cmd, sizeof(cmd) - 1) : -1;
	if (cf >= 0)
		close(cf);
	cmd[n > 0 ? n : 0] = 0;
	gpu_apps = (!sim && strstr(cmd, "sbwl.gpu=1")) || getenv("SBWL_GPU");
	if (!gpu_apps && S.have_user) {            /* or the user's switch: touch ~/.config/starbite/gpu-apps (then a restart) */
		char f[600];
		snprintf(f, sizeof(f), "%s/.config/starbite/gpu-apps", S.user.pw_dir);
		gpu_apps = !access(f, F_OK);
	}
	struct stat st;
	if (!gpu_apps || stat("/dev/dri/renderD128", &st) || !S_ISCHR(st.st_mode)) {
		gpu_apps = 0;
		return;
	}
	struct wlr_linux_dmabuf_feedback_v1 fb = {.main_device = st.st_rdev};
	wl_array_init(&fb.tranches);
	struct wlr_linux_dmabuf_feedback_v1_tranche *tr = wlr_linux_dmabuf_feedback_add_tranche(&fb);
	struct wlr_linux_dmabuf_v1 *dm = NULL;
	if (tr) {
		tr->target_device = st.st_rdev;
		wlr_drm_format_set_add(&tr->formats, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR);
		wlr_drm_format_set_add(&tr->formats, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR);
		dm = wlr_linux_dmabuf_v1_create(S.display, 4, &fb);
	}
	wlr_linux_dmabuf_feedback_v1_finish(&fb);
	if (!dm) {
		say("GPU for the apps: linux-dmabuf could not be offered: software rendering");
		gpu_apps = 0;
		return;
	}
	wlr_linux_dmabuf_v1_set_check_dmabuf_callback(dm, dmab_check, NULL);
	say("GPU for the apps: linux-dmabuf on /dev/dri/renderD128 (%u:%u)", major(st.st_rdev), minor(st.st_rdev));
}
static pid_t notify_pid;                   /* sbnotify */
static void child_env(int as_user)
{
	if (tzenv[0])
		setenv("TZ", tzenv, 1);
	setenv("XDG_SESSION_TYPE", "wayland", 1);
	setenv("XDG_CURRENT_DESKTOP", DESK_ID, 1);
	setenv("GDK_BACKEND", "wayland", 1);
	setenv("QT_QPA_PLATFORM", "wayland", 1);
	setenv("SDL_VIDEODRIVER", "wayland", 1);
	setenv("MOZ_ENABLE_WAYLAND", "1", 1);
	if (!gpu_apps) {                           /* no GPU for the apps (sbwl.gpu=1 and sbgpu's render node give one) */
		setenv("GSK_RENDERER", "cairo", 0);
		setenv("LIBGL_ALWAYS_SOFTWARE", "1", 0);
	} else {                                   /* (sbinit exports both for the session: the GPU's apps drop them) */
		unsetenv("GSK_RENDERER");
		unsetenv("LIBGL_ALWAYS_SOFTWARE");
	}
	setenv("NO_AT_BRIDGE", "1", 0);
	setenv("GTK_A11Y", "none", 0);
	setenv("GTK_IM_MODULE", "wayland", 1);     /* GTK3: text-input-v3 */
	setenv("LANG", getenv("LANG") ? getenv("LANG") : "C.UTF-8", 1);
	if (!getenv("PATH"))
		setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);
	unsetenv("DISPLAY");
	if (as_user) {
		setenv("HOME", S.user.pw_dir, 1);
		setenv("USER", S.user.pw_name, 1);
		setenv("LOGNAME", S.user.pw_name, 1);
		setenv("SHELL", S.user.pw_shell && S.user.pw_shell[0] ? S.user.pw_shell : "/bin/sh", 1);
		setenv("XDG_RUNTIME_DIR", S.user_rt, 1);
		setenv("WAYLAND_DISPLAY", "wayland-0", 1);
		char bus[128];
		snprintf(bus, sizeof(bus), "unix:path=%s/bus", S.user_rt);
		if (S.dbus > 0)
			setenv("DBUS_SESSION_BUS_ADDRESS", bus, 1);
	}
}

static pid_t spawn(char *const argv[], int as_user)
{
	pid_t p = fork();
	if (p)
		return p;
	setsid();
	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	signal(SIGPIPE, SIG_DFL);
	signal(SIGCHLD, SIG_DFL);
	if (!sim) {
		int k = open("/dev/kmsg", O_WRONLY);   /* the app's complaints into the guest's log */
		if (k >= 0) {
			dup2(k, 1);
			dup2(k, 2);
		}
	}
	int nul = open("/dev/null", O_RDONLY);
	if (nul >= 0)
		dup2(nul, 0);
	if (spawn_fd >= 0) {                       /* dup2 clears close-on-exec; fd 3 itself: by hand */
		if (spawn_fd == 3)
			fcntl(3, F_SETFD, 0);
		else
			dup2(spawn_fd, 3);
		setenv("SBNOTIFY_FD", "3", 1);
	}
	child_env(as_user);
	if (as_user) {
		if (initgroups(S.user.pw_name, S.user.pw_gid) || setgid(S.user.pw_gid) || setuid(S.user.pw_uid))
			_exit(126);
		if (chdir(S.user.pw_dir))
			(void)!chdir("/");
	}
	execvp(argv[0], argv);
	_exit(127);
}

static void notify_start(void);
static void user_session(void);
/* the apps' session: the user's (below), or, sbwl not root (hostsim as a user) with the host's notifications and no
 * bus in sbwl's environment, a private session bus in $XDG_RUNTIME_DIR; then sbnotify (v3, SBSHM_SVC_NOTIF) */
static void session_start(void)
{
	static int done;
	if (done)
		return;
	done = 1;
	if (S.have_user) {
		user_session();
	} else if ((svc & SBSHM_SVC_NOTIF) && !getenv("DBUS_SESSION_BUS_ADDRESS") && access("/usr/bin/dbus-daemon", X_OK) == 0) {
		char path[200], addr[220];
		snprintf(path, sizeof(path), "%s/sbwl-bus", getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "/run");
		unlink(path);
		snprintf(addr, sizeof(addr), "--address=unix:path=%s", path);
		char *av[] = {"dbus-daemon", "--session", "--nofork", "--nopidfile", addr, NULL};
		S.dbus = spawn(av, 0);
		for (int i = 0; i < 100 && access(path, F_OK); ++i)
			usleep(10000);
		setenv("DBUS_SESSION_BUS_ADDRESS", addr + 10, 1);
		say("session bus (private): pid %d, %s", S.dbus, access(path, F_OK) ? "no socket" : "up");
	}
	notify_start();
}

/* the user's runtime dir, the Wayland socket in it, its session bus */
static void user_session(void)
{
	if (!S.have_user || S.user_rt[0])
		return;
	snprintf(S.user_rt, sizeof(S.user_rt), "/run/user/%u", S.user.pw_uid);
	mkdir("/run/user", 0755);
	mkdir(S.user_rt, 0700);
	(void)!chown(S.user_rt, S.user.pw_uid, S.user.pw_gid);
	char l[128];
	snprintf(l, sizeof(l), "%s/wayland-0", S.user_rt);
	unlink(l);
	(void)!symlink(S.wl_sock, l);
	if (access("/usr/bin/dbus-daemon", X_OK) == 0) {
		char addr[128];
		snprintf(addr, sizeof(addr), "--address=unix:path=%s/bus", S.user_rt);
		char *av[] = {"dbus-daemon", "--session", "--nofork", "--nopidfile", addr, NULL};
		S.dbus = spawn(av, 1);
		snprintf(l, sizeof(l), "%s/bus", S.user_rt);
		for (int i = 0; i < 100 && access(l, F_OK); ++i)
			usleep(10000);
		say("session bus: pid %d, %s", S.dbus, access(l, F_OK) ? "no socket" : "up");
	}
}

static struct inst *inst_find(const char *desk)
{
	struct inst *in;
	wl_list_for_each(in, &S.insts, link)
		if (!strcmp(in->desk, desk))
			return in;
	return NULL;
}

static struct inst *start_argv(const char *desk, char *const argv[], int legacy)
{
	int as_user = S.have_user;
	session_start();
	struct inst *in = calloc(1, sizeof(*in));
	if (!in)
		return NULL;
	snprintf(in->desk, sizeof(in->desk), "%s", desk);
	in->legacy = legacy;
	in->t_start = ms_now();
	in->pid = spawn(argv, as_user);
	wl_list_insert(&S.insts, &in->link);
	say("started %s: pid %d (%s%s)", desk, in->pid, argv[0], as_user ? ", as user" : "");
	cat_dirty |= 1;
	return in;
}

static void launch(int idx)
{
	const struct app *a = &apps[idx];
	activate(a->id);
	if (inst_find(a->id))
		return;                                /* running: raised */
	struct view *v;
	wl_list_for_each(v, &S.views, link)
		if (!strcmp(v->desk, a->id))
			return;                            /* a window of it from elsewhere (D-Bus activation) */
	char buf[1024], *av[72];
	int n;
	if (a->terminal) {                         /* Terminal=true: inside foot, with the app's id as its app_id */
		static char appid[160];
		snprintf(appid, sizeof(appid), "--app-id=%s", a->id);
		av[0] = "foot", av[1] = appid, av[2] = "--";
		n = exec_split(a, buf, sizeof(buf), av + 3, 64) + 3;
		av[n] = NULL;
	} else {
		n = exec_split(a, buf, sizeof(buf), av, 64);
	}
	if (n < 1)
		return;
	start_argv(a->id, av, 0);
}

static void close_app(const char *desk)
{
	struct view *v;
	int nwin = 0;
	wl_list_for_each(v, &S.views, link)
		if (!strcmp(v->desk, desk)) {
			wlr_xdg_toplevel_send_close(v->top);
			++nwin;
		}
	struct inst *in = inst_find(desk);
	say("close %s: %d windows asked, %s", desk, nwin, in ? "SIGTERM at 3 s, SIGKILL at 6 s" : "not started by sbwl");
	if (in && !in->t_close) {
		in->t_close = ms_now();
		in->legacy = 0;                        /* closed on purpose: not restarted */
	}
	if (!in)                                   /* a window from elsewhere: its client */
		wl_list_for_each(v, &S.views, link)
			if (!strcmp(v->desk, desk) && v->pid > 1) {
				struct inst *t = calloc(1, sizeof(*t));
				if (!t)
					break;
				snprintf(t->desk, sizeof(t->desk), "%s", desk);
				t->pid = v->pid;
				t->t_start = t->t_close = ms_now();
				t->exited = 1;                 /* not our child: no waitpid; signalled as a process */
				wl_list_insert(&S.insts, &t->link);
				break;
			}
}

static int group_alive(pid_t pg) { return pg > 1 && (kill(-pg, 0) == 0 || kill(pg, 0) == 0); }

/* reap, the close timers, the legacy app kept running */
static void procs_tick(void)
{
	int st;
	pid_t p;
	while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
		struct inst *in;
		wl_list_for_each(in, &S.insts, link)
			if (in->pid == p) {
				in->exited = 1;
				say("%s (pid %d) ended: %s %d", in->desk, p, WIFSIGNALED(st) ? "signal" : "status",
				    WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
			}
		if (p == S.dbus)
			S.dbus = 0;
		if (p == notify_pid)
			notify_pid = 0;
	}
	uint64_t now = ms_now();
	struct inst *in, *tmp;
	wl_list_for_each_safe(in, tmp, &S.insts, link) {
		if (in->t_close) {
			if (now - in->t_close >= 6000 && in->termed < 2) {
				say("%s: SIGKILL", in->desk);
				kill(-in->pid, SIGKILL);
				kill(in->pid, SIGKILL);
				in->termed = 2;
			} else if (now - in->t_close >= 3000 && !in->termed) {
				say("%s: SIGTERM", in->desk);
				kill(-in->pid, SIGTERM);
				kill(in->pid, SIGTERM);
				in->termed = 1;
			}
		}
		if (!in->exited || group_alive(in->pid))
			continue;
		int legacy = in->legacy;
		wl_list_remove(&in->link);
		free(in);
		cat_dirty |= 1;
		if (legacy && !S.stopping) {           /* the window must not vanish under Starbite */
			static uint64_t last_start;
			static unsigned quick;
			if (last_start && now - last_start < 5000)
				++quick;
			else
				quick = 0;
			if (quick >= 3 && now - last_start < 30000U)
				continue;                      /* ended three times in a row within 5 s: no more */
			last_start = now;
			say("the app ended: starting it again");
			const char *b = strrchr(S.argv[0], '/');
			start_argv(b ? b + 1 : S.argv[0], S.argv, 1);
		}
	}
}

/* ---- /run/sbwl.sock: sbpkg and friends (one text line per message, sbshm.h) -------------------------------
 * v3, sbnotify (org.freedesktop.Notifications on the session bus; sbwl starts it as the user with its end of a
 * socketpair as fd 3, the same lines as the socket):
 *   sbnotify -> sbwl  "notify ID PID URGENCY TIMEOUT_MS<TAB>APP_NAME<TAB>DESKTOP_ENTRY<TAB>SUMMARY<TAB>BODY<TAB>A1<TAB>A2<TAB>A3"
 *                     (fields with \\, \t, \n escaped; BODY plain text; A1..A3 the button labels),
 *                     "notify-close ID" (the app withdrew it, or it expired), "notify-ready" (the bus name is ours)
 *   sbwl -> sbnotify  "notif-act ID B" (SBG_MSG_NOTIF_ACT: B 0 = the tap, 1..3 a button, 4294967295 = dismissed) */
#define SOCK_CLIENTS 8
static struct sclient {
	int fd, notify, spawned;
	struct wl_event_source *src;
	char buf[4096];
	size_t n;
} scl[SOCK_CLIENTS];

/* ---- v3: notifications (struct sbshm_notifs) ------------------------------------------------------------------ */
static volatile struct sbshm_notifs *notifs;       /* set when the host serves SBSHM_SVC_NOTIF */
static struct {
	uint32_t id;
	char desk[128];                                /* the app it came from ("" = unknown) */
} nmap[SBSHM_NOTIF_SLOTS];
static uint64_t notify_restart_at;
static unsigned notify_starts;

static void unesc(char *s)
{
	char *d = s;
	for (; *s; ++s) {
		if (*s == '\\' && s[1]) {
			++s;
			*d++ = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s;
		} else {
			*d++ = *s;
		}
	}
	*d = 0;
}

/* src into a fixed field: cut at a character boundary, NUL-terminated */
static void field_cpy(char *dst, size_t cap, const char *src)
{
	size_t l = src ? strlen(src) : 0;
	if (l > cap - 1) {
		l = cap - 1;
		while (l && ((uint8_t)src[l] & 0xc0) == 0x80)
			--l;
	}
	memset(dst, 0, cap);
	if (l)
		memcpy(dst, src, l);
}

/* the app that sent it: the desktop-entry hint; the sender's process (one sbwl started, or a window's client, or
 * their children); the app_name against the catalog's names and ids */
static int notif_app(pid_t pid, const char *desk, const char *name)
{
	if (desk && *desk) {
		int i = app_index(desk);
		if (i >= 0)
			return i;
		for (int j = 0; j < napps; ++j)
			if (id_match(desk, &apps[j]))
				return j;
	}
	struct inst *in = pid > 1 ? inst_of_pid(pid) : NULL;
	if (in && app_index(in->desk) >= 0)
		return app_index(in->desk);
	for (pid_t p = pid; p > 1;) {
		struct view *v;
		wl_list_for_each(v, &S.views, link)
			if (v->pid == p && v->desk[0] && app_index(v->desk) >= 0)
				return app_index(v->desk);
		p = ppid_of(p);
	}
	if (name && *name)
		for (int j = 0; j < napps; ++j)
			if (!strcasecmp(name, apps[j].name) || id_match(name, &apps[j]))
				return j;
	return -1;
}

static void notify_line(char *l)
{
	unsigned id, urg, tmo;
	int pid;
	char *f[7] = {0}, *t = strchr(l, '\t');
	if (!t || sscanf(l, "%u %d %u %u", &id, &pid, &urg, &tmo) != 4 || !id) {
		say("sbnotify: a bad notify line");
		return;
	}
	for (int i = 0; i < 7 && t; ++i) {
		*t++ = 0;
		f[i] = t;
		t = strchr(t, '\t');
	}
	for (int i = 0; i < 7; ++i)
		if (f[i])
			unesc(f[i]);
	int ai = notif_app(pid, f[1], f[0]);
	uint32_t slot = id % SBSHM_NOTIF_SLOTS;
	struct sbshm_notif n;
	memset(&n, 0, sizeof(n));
	n.id = id;
	n.app = ai >= 0 ? (uint32_t)ai : SBSHM_NONE;
	n.urgency = urg > SBSHM_NOTIF_CRITICAL ? SBSHM_NOTIF_CRITICAL : urg;
	n.timeout_ms = tmo;
	field_cpy(n.app_name, sizeof(n.app_name), f[0] && *f[0] ? f[0] : ai >= 0 ? apps[ai].name : "");
	field_cpy(n.summary, sizeof(n.summary), f[2]);
	field_cpy(n.body, sizeof(n.body), f[3]);
	for (unsigned k = 0; k < SBSHM_NOTIF_ACTIONS; ++k)
		field_cpy(n.action[k], sizeof(n.action[k]), f[4 + k]);
	n.icon = ai >= 0 && ai < (int)SBSHM_ICONS_CAT && icon_px[ai] ? (uint32_t)ai : SBSHM_NONE;
	uint32_t s = notifs->seq | 1U;
	notifs->seq = s;                           /* odd: being written */
	sbshm_dmb();
	volatile uint32_t *d = (volatile uint32_t *)&notifs->n[slot];
	const uint32_t *w = (const uint32_t *)&n;
	for (unsigned i = 0; i < sizeof(n) / 4; ++i)
		d[i] = w[i];
	sbshm_dmb();
	notifs->seq = s + 1U;
	nmap[slot].id = id;
	snprintf(nmap[slot].desk, sizeof(nmap[slot].desk), "%s", ai >= 0 ? apps[ai].id : "");
	g2h_send(SBG_MSG_NOTIFY, slot, id);
	say("notification %u (slot %u) from %s (pid %d -> app %d): \"%s\"", id, slot, n.app_name, pid, ai, n.summary);
}

static void sock_line(struct sclient *cl, char *l)
{
	unsigned a, b, c;
	if (!strncmp(l, "notify ", 7)) {
		cl->notify = 1;
		if (notifs)
			notify_line(l + 7);
	} else if (sscanf(l, "notify-close %u", &a) == 1) {
		cl->notify = 1;
		if (notifs) {
			g2h_send(SBG_MSG_NOTIF_CLOSE, a, 0);
			say("notification %u withdrawn", a);
		}
	} else if (!strcmp(l, "notify-ready")) {
		cl->notify = 1;
		say("sbnotify: org.freedesktop.Notifications is up");
	} else if (sscanf(l, "store %u", &a) == 1) {
		g2h_send(SBG_MSG_STORE, a, 0);
	} else if (sscanf(l, "pkg %u %u %u", &a, &b, &c) == 3) {
		g2h_send3(SBG_MSG_PKG_STATUS, a, b, c);
	} else if (!strncmp(l, "rescan", 6)) {
		rescan_at = ms_now();
	} else {
		say("sbwl.sock: unknown line \"%.80s\"", l);
	}
}

static int sock_read(int fd, uint32_t mask, void *data)
{
	struct sclient *c = data;
	ssize_t n = (mask & WL_EVENT_READABLE) ? read(fd, c->buf + c->n, sizeof(c->buf) - 1 - c->n) : 0;
	if (n <= 0) {
		wl_event_source_remove(c->src);
		close(fd);
		c->fd = -1;
		if (c->spawned) {                      /* sbnotify ended: again in 3 s (5 times at most) */
			c->spawned = 0;
			say("sbnotify: its line closed");
			notify_restart_at = ms_now() + 3000;
		}
		c->notify = 0;
		return 0;
	}
	c->n += (size_t)n;
	c->buf[c->n] = 0;
	char *s = c->buf, *e;
	while ((e = strchr(s, '\n'))) {
		*e = 0;
		sock_line(c, s);
		s = e + 1;
	}
	c->n = strlen(s);
	memmove(c->buf, s, c->n + 1);
	if (c->n == sizeof(c->buf) - 1)
		c->n = 0;                              /* a line too long: dropped */
	return 0;
}

static int sock_accept(int fd, uint32_t mask, void *data)
{
	(void)mask, (void)data;
	int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
	if (c < 0)
		return 0;
	for (int i = 0; i < SOCK_CLIENTS; ++i)
		if (scl[i].fd < 0) {
			scl[i].fd = c;
			scl[i].n = 0;
			scl[i].notify = scl[i].spawned = 0;
			scl[i].src = wl_event_loop_add_fd(S.loop, c, WL_EVENT_READABLE, sock_read, &scl[i]);
			return 0;
		}
	close(c);
	return 0;
}

static void sock_broadcast(const char *line)
{
	int sent = 0;
	for (int i = 0; i < SOCK_CLIENTS; ++i)              /* sbpkg's lines: not to sbnotify */
		if (scl[i].fd >= 0 && !scl[i].notify && write(scl[i].fd, line, strlen(line)) > 0)
			++sent;
	if (!sent)
		say("sbwl.sock: nobody to tell \"%.*s\"", (int)strcspn(line, "\n"), line);
}

/* a line to sbnotify (the clients that sent notify lines) */
static void notify_send(const char *line)
{
	int sent = 0;
	for (int i = 0; i < SOCK_CLIENTS; ++i)
		if (scl[i].fd >= 0 && scl[i].notify && write(scl[i].fd, line, strlen(line)) > 0)
			++sent;
	if (!sent)
		say("sbnotify: nobody to tell \"%.*s\"", (int)strcspn(line, "\n"), line);
}

/* sbnotify: as the user (the session bus is the user's), its line to sbwl a socketpair (/run/sbwl.sock is root's) */
static void notify_start(void)
{
	if (!(svc & SBSHM_SVC_NOTIF) || notify_pid > 0 || notify_starts >= 6)
		return;
	int i = 0;
	while (i < SOCK_CLIENTS && scl[i].fd >= 0)
		++i;
	int sv[2];
	if (i == SOCK_CLIENTS || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0)
		return;
	static char path[600];
	snprintf(path, sizeof(path), "sbnotify");
	if (getenv("SBNOTIFY")) {
		snprintf(path, sizeof(path), "%s", getenv("SBNOTIFY"));
	} else if (!in_path("sbnotify")) {         /* next to sbwl (a build directory) */
		char exe[512];
		ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
		char *sl = n > 0 ? (exe[n] = 0, strrchr(exe, '/')) : NULL;
		if (sl)
			snprintf(path, sizeof(path), "%.*s/sbnotify", (int)(sl - exe), exe);
	}
	char *av[] = {path, NULL};
	spawn_fd = sv[1];
	notify_pid = spawn(av, S.have_user);
	spawn_fd = -1;
	close(sv[1]);
	fcntl(sv[0], F_SETFL, O_NONBLOCK);
	scl[i].fd = sv[0];
	scl[i].n = 0;
	scl[i].notify = scl[i].spawned = 1;
	scl[i].src = wl_event_loop_add_fd(S.loop, sv[0], WL_EVENT_READABLE, sock_read, &scl[i]);
	notify_starts++;
	say("started %s: pid %d", path, notify_pid);
}

static void sock_start(void)
{
	const char *path = getenv("SBWL_SOCK") ? getenv("SBWL_SOCK") : "/run/sbwl.sock";
	for (int i = 0; i < SOCK_CLIENTS; ++i)
		scl[i].fd = -1;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	struct sockaddr_un sa = {.sun_family = AF_UNIX};
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
	unlink(path);
	if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof(sa)) || listen(fd, 4)) {
		say("%s: %s", path, strerror(errno));
		return;
	}
	chmod(path, 0600);
	wl_event_loop_add_fd(S.loop, fd, WL_EVENT_READABLE, sock_accept, NULL);
}

/* ---- the host's messages -------------------------------------------------------------------------------- */
static void stop_now(void)
{
	if (S.stopping)
		return;
	S.stopping = 1;
	hdr->guest_state = SBG_GUEST_STOPPING;
	say("MSG_STOP: closing the apps, syncing, powering the M3 cluster off");
	g2h_send(SBG_MSG_BYE, 0, 0);
	struct view *v;
	wl_list_for_each(v, &S.views, link)
		wlr_xdg_toplevel_send_close(v->top);
	wl_display_flush_clients(S.display);
	for (int i = 0; i < 20; ++i) {
		int any = 0;
		struct inst *in;
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;
		wl_list_for_each(in, &S.insts, link)
			any |= group_alive(in->pid);
		if (!any)
			break;
		usleep(25000);
	}
	struct inst *in;
	wl_list_for_each(in, &S.insts, link) {
		kill(-in->pid, SIGKILL);
		kill(in->pid, SIGKILL);
	}
	if (notify_pid > 0)
		kill(notify_pid, SIGTERM);
	if (S.dbus > 0)
		kill(S.dbus, SIGTERM);
	sync();
	if (sim) {
		hdr->guest_state = SBG_GUEST_OFF;
		exit(0);
	}
	reboot(RB_POWER_OFF);                      /* sbguest.ko: CPU_OFF on every guest CPU */
}

static int idx_ok(uint32_t idx) { return idx < (uint32_t)napps; }

static uint32_t touches;
static void drain(void)
{
	struct sbshm_msg m;
	while (h2g_get(&m)) {
		switch (m.type) {
		case SBG_MSG_TOUCH:
			touch(m.b, m.x, m.y, m.t_ms);
			hdr->touches_seen = ++touches;
			break;
		case SBG_MSG_KEY:
			key(m.a, (int)m.b, m.t_ms);
			break;
		case SBG_MSG_STOP:
			stop_now();
			break;
		case SBG_MSG_ROTATE: {
			uint32_t q = m.a == 1 || m.a == 3 ? m.a : 0, lr = q ? (m.b && m.b <= W ? m.b : W) : 0;
			if (q == ROT && lr == LROWS)
				break;
			say("rotate %u (content %ux%u)", q, q ? H : VW, q ? lr : VR);
			if (S.output && (q != 0) != turned()) {  /* the output landscape too: frame callbacks reach the whole */
				struct wlr_output_state os;      /* logical area, and clients see a landscape wl_output */
				wlr_output_state_init(&os);
				wlr_output_state_set_custom_mode(&os, (int32_t)(q ? H : W), (int32_t)(q ? W : H), 60000);
				if (!wlr_output_commit_state(S.output, &os))
					say("rotate: the output mode was not taken (frame callbacks only inside %ux%u)", W, H);
				wlr_output_state_finish(&os);
			}
			ROT = q, LROWS = lr;
			struct view *v;
			wl_list_for_each(v, &S.views, link)
				view_layout(v);
			mark_dirty();
			break;
		}
		case SBG_MSG_VIEWPORT: {
			uint32_t r = m.a && m.a <= H ? m.a : H, w = m.b && m.b <= W ? m.b : W;
			if (r == VR && w == VW)
				break;
			say("viewport %ux%u", w, r);
			VR = r, VW = w;
			struct view *v;
			wl_list_for_each(v, &S.views, link)
				view_layout(v);
			mark_dirty();
			break;
		}
		case SBG_MSG_LAUNCH:
			/* indices hold while the ids and their order do: a seq from before the last such change is stale */
			if (!idx_ok(m.a) || (int32_t)(m.b - lay_seq) < 0 || (int32_t)(m.b - cat_seq) > 0) {
				say("launch %u at seq %u: stale (catalog seq %u, its order since %u)", m.a, m.b, cat_seq, lay_seq);
				break;
			}
			say("launch %u: %s", m.a, apps[m.a].id);
			launch((int)m.a);
			break;
		case SBG_MSG_ACTIVATE:
			if (m.a == SBSHM_NONE || !idx_ok(m.a)) {
				say("activate: none");
				activate(NULL);
			} else {
				say("activate %u: %s", m.a, apps[m.a].id);
				activate(apps[m.a].id);
			}
			break;
		case SBG_MSG_APP_CLOSE:
			if (idx_ok(m.a))
				close_app(apps[m.a].id);
			break;
		case SBG_MSG_TEXT: {
			char s[9] = {0};
			uint32_t n = m.c > 8 ? 8 : m.c;
			for (uint32_t i = 0; i < n; ++i)
				s[i] = (char)((i < 4 ? m.a >> (8 * i) : m.b >> (8 * (i - 4))) & 0xff);
			/* with Ctrl / Alt / Super held, or no text field enabled: as keys (ASCII) */
			if (((keymods & (SBSHM_MOD_CTRL | SBSHM_MOD_ALT | SBSHM_MOD_SUPER)) || !S.ti_on) && text_as_keys(s, m.t_ms))
				break;
			text_commit(s);
			break;
		}
		case SBG_MSG_KEYMOD:
			keymod_set(m.a, m.t_ms);
			break;
		case SBG_MSG_CLIP:
			clip_from_host();
			break;
		case SBG_MSG_NOTIF_ACT: {
			char l[64];
			snprintf(l, sizeof(l), "notif-act %u %u\n", m.a, m.b);
			notify_send(l);
			uint32_t slot = m.a % SBSHM_NOTIF_SLOTS;
			int ai = nmap[slot].id == m.a && nmap[slot].desk[0] ? app_index(nmap[slot].desk) : -1;
			say("notification %u: %s%s%s", m.a, m.b == 0 ? "tapped" : m.b == SBSHM_NONE ? "dismissed" : "a button",
			    m.b == 0 && ai >= 0 ? ", raising " : "", m.b == 0 && ai >= 0 ? apps[ai].id : "");
			if (m.b == 0 && ai >= 0)
				launch(ai);                    /* raised (started again if it has ended) */
			break;
		}
		case SBG_MSG_PKG: {
			char l[96];
			snprintf(l, sizeof(l), "op %u %u %u\n", m.a, m.b, m.c);
			sock_broadcast(l);
			break;
		}
		default:
			break;
		}
	}
}

/* ---- v3: the wall clock (SBSHM_SVC_CLOCK) ---------------------------------------------------------------------
 * sbwl (root) sets the guest's clock from hdr.host_unix_s at start and whenever they differ by more than 2 s (checked
 * every 3 s); the apps' local time: TZ in POSIX form from hdr.host_tz_min (POSIX counts west: UTC+2 = "<+02>-2"), for
 * the apps started from then on. Under hostsim (SBWL_SHM) or SBWL_NO_SETTIME the clock is not set, only logged. */
static void tz_posix(int32_t m, char *o, size_t n)
{
	int a = m < 0 ? -m : m;
	char sg = m < 0 ? '-' : '+', west = m < 0 ? '+' : '-';
	if (!m)
		snprintf(o, n, "UTC0");
	else if (a % 60)
		snprintf(o, n, "<%c%02d%02d>%c%d:%02d", sg, a / 60, a % 60, west, a / 60, a % 60);
	else
		snprintf(o, n, "<%c%02d>%c%d", sg, a / 60, west, a / 60);
}

static void clock_tick(uint64_t now)
{
	static uint64_t t_clk;
	static int logged, tz_known;
	static int32_t tz_last;
	if (!(svc & SBSHM_SVC_CLOCK) || (t_clk && now - t_clk < 3000))
		return;
	t_clk = now;
	uint32_t hs = hdr->host_unix_s;
	int32_t tz = hdr->host_tz_min;
	if (!tz_known || tz != tz_last) {
		tz_known = 1, tz_last = tz;
		tz_posix(tz, tzenv, sizeof(tzenv));
		say("time zone: UTC%+d min, TZ=%s for the apps started from now", tz, tzenv);
	}
	if (!hs)
		return;
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long long d = (long long)hs - (long long)ts.tv_sec;
	if (d >= -2 && d <= 2)
		return;
	if (sim || getenv("SBWL_NO_SETTIME")) {
		if (!logged++)
			say("clock: host %u, guest %lld (%+lld s): would set it (not under hostsim: that is the machine's clock)", hs,
			    (long long)ts.tv_sec, d);
		return;
	}
	struct timespec ns = {.tv_sec = (time_t)hs, .tv_nsec = 500000000};    /* host_unix_s is whole seconds */
	if (clock_settime(CLOCK_REALTIME, &ns) == 0)
		say("clock: set to %u from the host (was %+lld s off)", hs, -d);
	else if (!logged++)
		say("clock: cannot set it: %s", strerror(errno));
}

static uint64_t t0;
static struct wl_event_source *tick_src;
/* after HELLO: the catalog's icons one per call, the catalog published again each time, until none is missing */
static struct wl_event_source *icons_src;
static int icons_tick(void *data)
{
	(void)data;
	cat_dirty = 2;
	cat_publish();
	if (icons_pending) {
		wl_event_source_timer_update(icons_src, 30);
	} else {
		icons_budget = -1;                     /* (later changes: all at once, as before) */
		say("catalog: every icon rendered");
	}
	return 0;
}

static int tick(void *data)
{
	(void)data;
	drain();
	hdr->guest_heartbeat++;
	hdr->guest_uptime_ms = ms_now() - t0;
	uint64_t now = ms_now();
	static uint64_t t_stat, t_cat;
	if (now - t_stat >= 10000) {
		t_stat = now;
		say("stats: frame events %u, built %u, with a buffer %u, published %llu, rows %llu, px %08x, apps %d, ring drops %u, "
		    "deferred %u, background commits %lu, banded %lu, direct %lu", nframe, nbuilt, nbuf, (unsigned long long)frames,
		    (unsigned long long)rows_out, last_px, napps, g2h_dropped, defers, hidden_commits, nband, ndirect);
	}
	procs_tick();
	clock_tick(now);
	if (crd.fd >= 0 && now - crd.t0 > 3000) {
		say("clipboard: the app did not send its selection within 3 s");
		clip_read_end(0);
	}
	if (notify_restart_at && now >= notify_restart_at && !notify_pid) {
		notify_restart_at = 0;
		notify_start();
	}
	if (v2) {
		if (rescan_at && now >= rescan_at) {
			rescan_at = 0;
			scan_apps();
			window_state();
		}
		/* the flags at once, the memory every 2 s while anything runs */
		int busy = !wl_list_empty(&S.insts) || !wl_list_empty(&S.views);
		if (cat_dirty || (now - t_cat >= 2000 && (busy || cat_last_n == SBSHM_NONE)) || now - t_cat >= 10000) {
			t_cat = now;
			cat_publish();
		}
	}
	wl_event_source_timer_update(tick_src, 10);
	return 0;
}

/* hostsim (SBWL_SHM) ended or was killed: as MSG_STOP (the apps, the session bus, sbnotify end too) */
static int sim_sigterm(int sig, void *data)
{
	(void)sig, (void)data;
	stop_now();
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

static int inotify_ev(int fd, uint32_t mask, void *data)
{
	(void)mask, (void)data;
	char b[4096];
	while (read(fd, b, sizeof(b)) > 0)
		;
	rescan_at = ms_now() + 300;                /* a package install writes several files: once, after them */
	return 0;
}

int main(int argc, char **argv)
{
	static char *def[] = {"foot", "-o", "csd.preferred=none", NULL};
	static char *one[2];
	char **legacy = argc > 1 ? argv + 1 : NULL;
	if (argc <= 1) {                           /* sbwl.app=NAME on the kernel command line */
		static char cmd[512], app[64];
		int cf = open("/proc/cmdline", O_RDONLY);
		ssize_t n = cf >= 0 ? read(cf, cmd, sizeof(cmd) - 1) : -1;
		if (cf >= 0)
			close(cf);
		char *a = n > 0 ? (cmd[n] = 0, strstr(cmd, "sbwl.app=")) : NULL;
		if (a && sscanf(a + 9, "%63s", app) == 1) {
			one[0] = app;
			legacy = one;
		}
	}
	signal(SIGPIPE, SIG_IGN);
	kfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	wlr_log_init(getenv("SBWL_DEBUG") ? WLR_DEBUG : WLR_ERROR, NULL);
	if (map_shm() < 0) {
		say("no shared region");
		return 1;
	}
	if (!v2 && !legacy)
		legacy = def;                          /* v1: one app, foot by default */
	{
		char cmd[1024] = "";
		int cf = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
		ssize_t n = cf >= 0 ? read(cf, cmd, sizeof(cmd) - 1) : -1;
		if (cf >= 0)
			close(cf);
		cmd[n > 0 ? n : 0] = 0;
		const char *a = strstr(cmd, "sbwl.scale="), *e = getenv("SBWL_SCALE");
		int sc = a && !sim ? atoi(a + 11) : e ? atoi(e) : sim ? 1 : W >= 720 ? (int)(W / 360U) : 1;   /* ~360 logical wide */
		SC = sc >= 1 && sc <= 4 && !(W % (uint32_t)sc) && !(H % (uint32_t)sc) ? (uint32_t)sc : 1;
	}
	S.argv = legacy;
	t0 = ms_now();
	hdr->guest_state = SBG_GUEST_UP;
	setenv("XDG_RUNTIME_DIR", "/run", 0);
	wl_list_init(&S.views);
	wl_list_init(&S.insts);
	wl_list_init(&S.tinputs);
	struct passwd *pw = getpwnam("user");
	if (pw && pw->pw_uid != 0 && !getenv("SBWL_AS_ROOT")) {
		S.user = *pw;
		S.user.pw_name = strdup(pw->pw_name);
		S.user.pw_dir = strdup(pw->pw_dir);
		S.user.pw_shell = strdup(pw->pw_shell ? pw->pw_shell : "/bin/sh");
		S.have_user = getuid() == 0;
	}

	S.display = wl_display_create();
	S.loop = wl_display_get_event_loop(S.display);
#if WLR18
	S.backend = wlr_headless_backend_create(S.loop);
#else
	S.backend = wlr_headless_backend_create(S.display);
#endif
	S.renderer = wlr_pixman_renderer_create();
	S.allocator = S.backend && S.renderer ? wlr_allocator_autocreate(S.backend, S.renderer) : NULL;
	if (!S.allocator) {
		say("no headless backend / pixman renderer / allocator");
		return 1;
	}
	wlr_renderer_init_wl_display(S.renderer, S.display);
	struct wlr_compositor *comp = wlr_compositor_create(S.display, 5, S.renderer);
	S.new_surface.notify = new_surface;
	wl_signal_add(&comp->events.new_surface, &S.new_surface);
	wlr_subcompositor_create(S.display);
	gpu_apps_init();                           /* sbgpu: before the first app starts (child_env reads gpu_apps) */
	wlr_data_device_manager_create(S.display);
	wlr_primary_selection_v1_device_manager_create(S.display);
	S.scene = wlr_scene_create();

	S.new_output.notify = new_output;
	wl_signal_add(&S.backend->events.new_output, &S.new_output);
	S.xdg_shell = wlr_xdg_shell_create(S.display, 3);
#if WLR18
	S.new_toplevel.notify = new_toplevel;
	wl_signal_add(&S.xdg_shell->events.new_toplevel, &S.new_toplevel);
	S.new_popup.notify = new_popup;
	wl_signal_add(&S.xdg_shell->events.new_popup, &S.new_popup);
#else
	S.new_xdg_surface.notify = new_xdg_surface;
	wl_signal_add(&S.xdg_shell->events.new_surface, &S.new_xdg_surface);
#endif
	/* no decorations: xdg-decoration server-side (we draw none), KDE's server-decoration for GTK3 */
	S.xdeco = wlr_xdg_decoration_manager_v1_create(S.display);
	S.new_deco.notify = new_deco;
	wl_signal_add(&S.xdeco->events.new_toplevel_decoration, &S.new_deco);
	struct wlr_server_decoration_manager *sdm = wlr_server_decoration_manager_create(S.display);
	wlr_server_decoration_manager_set_default_mode(sdm, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	S.tim = wlr_text_input_manager_v3_create(S.display);
	S.new_text_input.notify = new_text_input;
	wl_signal_add(&S.tim->events.text_input, &S.new_text_input);

	S.seat = wlr_seat_create(S.display, "seat0");
	wlr_seat_set_capabilities(S.seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_TOUCH);
	static const struct wlr_keyboard_impl kimpl = {.name = "starbite-keys"};
	wlr_keyboard_init(&S.kbd, &kimpl, "starbite-keys");
	/* instant start: the compiled keymap kept in one file after the first boot (xkbcommon's compile reads dozens of
	 * small files under /usr/share/X11/xkb, each a card read on the phone: inside 8c's 10.3 s from sbwl to its output) */
	static const char kcache[] = "/var/cache/sbwl/keymap-us.xkb";
	struct xkb_keymap *km = NULL;
	struct xkb_context *xc = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES | XKB_CONTEXT_NO_ENVIRONMENT_NAMES);
	if (xc && !sim) {
		FILE *kf = fopen(kcache, "re");
		if (kf) {
			static char kb[256 * 1024];
			size_t kn = fread(kb, 1, sizeof(kb) - 1, kf);
			fclose(kf);
			kb[kn] = 0;
			if (kn > 0 && kn < sizeof(kb) - 1)
				km = xkb_keymap_new_from_string(xc, kb, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
			if (!km)
				say("keymap: the cached %s did not load: compiling it again", kcache);
		}
	}
	if (!km) {
		if (xc)
			xkb_context_unref(xc);
		xc = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		struct xkb_rule_names names = {.layout = "us"};
		km = xc ? xkb_keymap_new_from_names(xc, &names, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
		char *ks = km && !sim ? xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
		if (ks) {                              /* for the next boots */
			mkdir("/var/cache/sbwl", 0755);
			char tmp[sizeof(kcache) + 8];
			snprintf(tmp, sizeof(tmp), "%s.new", kcache);
			FILE *kf = fopen(tmp, "we");
			if (kf) {
				int ok = fputs(ks, kf) >= 0;
				ok &= fclose(kf) == 0;
				if (ok && rename(tmp, kcache) == 0)
					say("keymap: compiled and kept in %s", kcache);
				else
					unlink(tmp);
			}
			free(ks);
		}
	}
	if (km)
		wlr_keyboard_set_keymap(&S.kbd, km);
	else
		say("no xkb keymap (/usr/share/X11/xkb): keys will not reach apps");
	S.kbd_key.notify = kbd_key;
	wl_signal_add(&S.kbd.events.key, &S.kbd_key);
	S.kbd_mods.notify = kbd_mods;
	wl_signal_add(&S.kbd.events.modifiers, &S.kbd_mods);
	wlr_seat_set_keyboard(S.seat, &S.kbd);
	S.req_sel.notify = req_set_selection;
	wl_signal_add(&S.seat->events.request_set_selection, &S.req_sel);
	S.req_psel.notify = req_set_primary;
	wl_signal_add(&S.seat->events.request_set_primary_selection, &S.req_psel);
	S.set_sel.notify = seat_set_selection;
	wl_signal_add(&S.seat->events.set_selection, &S.set_sel);
	if (svc & SBSHM_SVC_CLIP)
		clip = (volatile struct sbshm_clip *)(shm + SBSHM_CLIP_OFF);
	if (svc & SBSHM_SVC_NOTIF)
		notifs = (volatile struct sbshm_notifs *)(shm + SBSHM_NOTIF_OFF);

	const char *sock = wl_display_add_socket_auto(S.display);
	if (!sock || !wlr_backend_start(S.backend)) {
		say("no Wayland socket or the backend did not start");
		return 1;
	}
	snprintf(S.wl_sock, sizeof(S.wl_sock), "%s/%s", getenv("XDG_RUNTIME_DIR"), sock);
	if (S.have_user)
		chmod(S.wl_sock, 0666);                /* the apps run as "user" */
	wlr_headless_add_output(S.backend, W, H);
	setenv("WAYLAND_DISPLAY", sock, 1);
	unsetenv("DISPLAY");

	tick_src = wl_event_loop_add_timer(S.loop, tick, NULL);
	wl_event_source_timer_update(tick_src, 10);
	if (!sim)
		wl_event_loop_add_fd(S.loop, sbfd, WL_EVENT_READABLE, doorbell, NULL);
	else
		wl_event_loop_add_signal(S.loop, SIGTERM, sim_sigterm, NULL);

	struct utsname u;
	uname(&u);
	for (unsigned i = 0; i < 63 && u.release[i]; ++i)
		hdr->guest_kver[i] = u.release[i];
	hdr->guest_ncpus = (uint32_t)sysconf(_SC_NPROCESSORS_ONLN);
	if (v2) {
		sock_start();
		inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
		if (inotify_fd >= 0)
			wl_event_loop_add_fd(S.loop, inotify_fd, WL_EVENT_READABLE, inotify_ev, NULL);
		scan_apps();
		icons_budget = 0;                      /* the catalog before HELLO without icons; they follow (icons_tick) */
		cat_publish();
	}
	clock_tick(ms_now());                      /* the clock and TZ before the first app */
	if (S.argv) {
		const char *b = strrchr(S.argv[0], '/');
		struct inst *in = start_argv(b ? b + 1 : S.argv[0], S.argv, 1);
		if (in)
			snprintf(S.active, sizeof(S.active), "%s", in->desk);
	}
	hdr->guest_state = SBG_GUEST_RUNNING;
	hdr->guest_caps |= SBSHM_CAP_ROTATE;
	g2h_send(SBG_MSG_HELLO, hdr->guest_ncpus, 1);
	say("HELLO: Wayland %s, output %ux%u, contract v%u (services %#x), %s%s, kernel %s", sock, W, H, v3 ? 3 : v2 ? 2 : 1, svc,
	    S.argv ? "app " : "catalog", S.argv ? S.argv[0] : "", u.release);
	if (!sim) {                                /* sbinit starts the services after this (instant start: their reads */
		int hf = open("/run/sbwl.hello", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);   /* no longer race ours) */
		if (hf >= 0)
			close(hf);
	}
	if (icons_pending) {                       /* the catalog's icons now, one per tick */
		icons_budget = 1;
		icons_src = wl_event_loop_add_timer(S.loop, icons_tick, NULL);
		if (icons_src)
			wl_event_source_timer_update(icons_src, 30);
		else
			icons_budget = -1;
	} else {
		icons_budget = -1;
	}
	if (svc & SBSHM_SVC_NOTIF)
		session_start();                       /* the session bus and sbnotify now, before the host's first launch */
	wl_display_run(S.display);
	return 0;
}
