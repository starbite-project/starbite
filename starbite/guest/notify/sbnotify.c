// SPDX-License-Identifier: GPL-2.0-only
/*
 * sbnotify: org.freedesktop.Notifications (Desktop Notifications spec 1.2) on the guest's session bus, for the Linux
 * apps of Starbite OS (contract v3, sbshm.h "notifications"). It owns no shared memory: it hands every notification
 * to sbwl, which writes the slot and sends SBG_MSG_NOTIFY to the host (Starbite's Notification Centre), and it turns
 * the host's taps / buttons / swipes (SBG_MSG_NOTIF_ACT, from sbwl) into ActionInvoked / NotificationClosed.
 *
 * sbwl starts it as the desktop user (the session bus is the user's) with its end of a socketpair as fd 3
 * ($SBNOTIFY_FD); without that it connects to $SBWL_SOCK or /run/sbwl.sock (root). Lines (sbwl.c, "/run/sbwl.sock"):
 *   -> sbwl  "notify ID PID URGENCY TIMEOUT_MS<TAB>APP_NAME<TAB>DESKTOP_ENTRY<TAB>SUMMARY<TAB>BODY<TAB>A1<TAB>A2<TAB>A3"
 *            "notify-close ID", "notify-ready"
 *   <- sbwl  "notif-act ID B"   (0 = the tap: ActionInvoked "default"; 1..3 the buttons; 4294967295 = dismissed)
 * Capabilities: body, body-markup (stripped to plain text here: <b> <i> <u> <a> <img> <br>, entities), actions,
 * persistence (the host keeps them in its Notification Centre). Expired notifications (expire_timeout > 0) are
 * closed with reason 1 and withdrawn from the host. GLib / GIO only (the Debian base has them for GTK).
 */
#include <errno.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define N_ACT 3                                     /* SBSHM_NOTIF_ACTIONS */
#define SBSHM_NONE 0xffffffffU

static const char xml[] =
	"<node><interface name='org.freedesktop.Notifications'>"
	"<method name='GetCapabilities'><arg type='as' name='caps' direction='out'/></method>"
	"<method name='Notify'><arg type='s' name='app_name' direction='in'/><arg type='u' name='replaces_id' direction='in'/>"
	"<arg type='s' name='app_icon' direction='in'/><arg type='s' name='summary' direction='in'/>"
	"<arg type='s' name='body' direction='in'/><arg type='as' name='actions' direction='in'/>"
	"<arg type='a{sv}' name='hints' direction='in'/><arg type='i' name='expire_timeout' direction='in'/>"
	"<arg type='u' name='id' direction='out'/></method>"
	"<method name='CloseNotification'><arg type='u' name='id' direction='in'/></method>"
	"<method name='GetServerInformation'><arg type='s' name='name' direction='out'/><arg type='s' name='vendor' direction='out'/>"
	"<arg type='s' name='version' direction='out'/><arg type='s' name='spec_version' direction='out'/></method>"
	"<signal name='NotificationClosed'><arg type='u' name='id'/><arg type='u' name='reason'/></signal>"
	"<signal name='ActionInvoked'><arg type='u' name='id'/><arg type='s' name='action_key'/></signal>"
	"</interface></node>";

struct note {
	guint32 id;
	char *keys[N_ACT + 1];                          /* [0] "default" if offered, [1..3] the buttons' keys */
	guint timer;
	gboolean resident;
};

static GDBusConnection *bus;
static GHashTable *notes;                           /* id -> struct note */
static guint32 next_id = 1;
static int sfd = -1;
static GMainLoop *loop;

static void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "sbnotify: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}

static void line_out(const char *l)
{
	size_t n = strlen(l), o = 0;
	while (sfd >= 0 && o < n) {
		ssize_t w = write(sfd, l + o, n - o);
		if (w > 0)
			o += (size_t)w;
		else if (w < 0 && errno == EINTR)
			continue;
		else
			break;
	}
}

static void note_free(gpointer p)
{
	struct note *n = p;
	if (n->timer)
		g_source_remove(n->timer);
	for (int i = 0; i <= N_ACT; ++i)
		g_free(n->keys[i]);
	g_free(n);
}

static void emit(const char *sig, GVariant *args)
{
	GError *e = NULL;
	if (!g_dbus_connection_emit_signal(bus, NULL, "/org/freedesktop/Notifications", "org.freedesktop.Notifications", sig,
					   args, &e)) {
		say("%s: %s", sig, e->message);
		g_error_free(e);
	}
}

static void closed(guint32 id, guint32 reason)
{
	emit("NotificationClosed", g_variant_new("(uu)", id, reason));
	g_hash_table_remove(notes, GUINT_TO_POINTER(id));
}

/* "a\tb\nc\\" -> "a\\tb\\nc\\\\", cut to cap bytes of the original at a character boundary first */
static void esc_cat(GString *o, const char *s, size_t cap)
{
	char *v = g_utf8_make_valid(s ? s : "", -1);
	size_t l = strlen(v);
	if (l > cap) {
		l = cap;
		while (l && ((unsigned char)v[l] & 0xc0) == 0x80)
			--l;
	}
	for (size_t i = 0; i < l; ++i) {
		char c = v[i];
		if (c == '\\')
			g_string_append(o, "\\\\");
		else if (c == '\t')
			g_string_append(o, "\\t");
		else if (c == '\n')
			g_string_append(o, "\\n");
		else if (c != '\r')
			g_string_append_c(o, c);
	}
	g_free(v);
}

/* body-markup to plain text: the spec's tags (b i u a img br) dropped (<br> = a new line, <img alt> = its alt), the
 * entities decoded; anything else that looks like a tag is text */
static char *strip_markup(const char *s)
{
	static const char *const tags[] = {"b", "/b", "i", "/i", "u", "/u", "a", "/a", "img", "br", "/br", "br/", NULL};
	GString *o = g_string_new(NULL);
	for (const char *p = s; *p;) {
		if (*p == '<') {
			const char *e = strchr(p, '>');
			if (e) {
				size_t nl = strcspn(p + 1, " \t\n/>");
				if (p[1] == '/')
					nl = 1 + strcspn(p + 2, " \t\n/>");
				int known = 0;
				for (int k = 0; tags[k] && !known; ++k)
					known = strlen(tags[k]) == nl && !g_ascii_strncasecmp(p + 1, tags[k], nl);
				if (known) {
					if (!g_ascii_strncasecmp(p + 1, "br", 2))
						g_string_append_c(o, '\n');
					else if (!g_ascii_strncasecmp(p + 1, "img", 3)) {
						const char *alt = g_strstr_len(p, e - p, "alt=\"");
						if (alt) {
							const char *q = strchr(alt + 5, '"');
							if (q && q < e)
								g_string_append_len(o, alt + 5, q - alt - 5);
						}
					}
					p = e + 1;
					continue;
				}
			}
		} else if (*p == '&') {
			static const struct { const char *n; const char *v; } ent[] = {
				{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&nbsp;", " "}};
			int done = 0;
			for (unsigned k = 0; k < G_N_ELEMENTS(ent) && !done; ++k)
				if (!strncmp(p, ent[k].n, strlen(ent[k].n))) {
					g_string_append(o, ent[k].v);
					p += strlen(ent[k].n);
					done = 1;
				}
			if (!done && p[1] == '#') {
				char *end;
				gunichar c = (gunichar)(p[2] == 'x' || p[2] == 'X' ? strtoul(p + 3, &end, 16) : strtoul(p + 2, &end, 10));
				if (*end == ';' && c && g_unichar_validate(c)) {
					g_string_append_unichar(o, c);
					p = end + 1;
					done = 1;
				}
			}
			if (done)
				continue;
		}
		g_string_append_c(o, *p++);
	}
	return g_string_free(o, FALSE);
}

static gboolean expire(gpointer p)
{
	guint32 id = GPOINTER_TO_UINT(p);
	struct note *n = g_hash_table_lookup(notes, p);
	if (n)
		n->timer = 0;
	char l[48];
	snprintf(l, sizeof(l), "notify-close %u\n", id);
	line_out(l);
	closed(id, 1);
	return G_SOURCE_REMOVE;
}

static guint32 sender_pid(const char *sender)
{
	GVariant *r = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
						  "GetConnectionUnixProcessID", g_variant_new("(s)", sender), G_VARIANT_TYPE("(u)"),
						  G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
	guint32 pid = 0;
	if (r) {
		g_variant_get(r, "(u)", &pid);
		g_variant_unref(r);
	}
	return pid;
}

static guint32 notify(const char *sender, GVariant *p)
{
	const char *app, *icon, *summary, *body;
	guint32 rid;
	gint32 tmo;
	GVariant *acts, *hints;
	g_variant_get(p, "(&su&s&s&s@as@a{sv}i)", &app, &rid, &icon, &summary, &body, &acts, &hints, &tmo);
	(void)icon;                                     /* the host shows the app's own icon (the catalog's) */
	guint32 urg = 1, pid = 0;
	const char *desk = "";
	gboolean resident = FALSE;
	GVariant *v;
	if ((v = g_variant_lookup_value(hints, "urgency", NULL))) {
		if (g_variant_is_of_type(v, G_VARIANT_TYPE_BYTE))
			urg = g_variant_get_byte(v);
		else if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32))
			urg = g_variant_get_uint32(v);
		else if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT32))
			urg = (guint32)g_variant_get_int32(v);
		g_variant_unref(v);
	}
	g_variant_lookup(hints, "desktop-entry", "&s", &desk);
	g_variant_lookup(hints, "resident", "b", &resident);
	if ((v = g_variant_lookup_value(hints, "sender-pid", NULL))) {
		if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT64))
			pid = (guint32)g_variant_get_int64(v);
		else if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32))
			pid = g_variant_get_uint32(v);
		else if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT32))
			pid = (guint32)g_variant_get_int32(v);
		g_variant_unref(v);
	}
	if (!pid)
		pid = sender_pid(sender);

	guint32 id = rid && g_hash_table_contains(notes, GUINT_TO_POINTER(rid)) ? rid : 0;
	if (!id) {
		id = next_id++;
		if (!next_id)
			next_id = 1;
	}
	struct note *n = g_new0(struct note, 1);
	n->id = id;
	n->resident = resident;
	const char *label[N_ACT] = {"", "", ""};
	gsize na = 0;
	const char **a = g_variant_get_strv(acts, &na);
	for (gsize i = 0, b = 1; i + 1 < na; i += 2) {
		if (!strcmp(a[i], "default"))
			n->keys[0] = g_strdup("default");
		else if (b <= N_ACT) {
			n->keys[b] = g_strdup(a[i]);
			label[b - 1] = a[i + 1];
			++b;
		}
	}
	char *plain = strip_markup(body);
	guint32 ms = tmo > 0 ? (guint32)tmo : 0;        /* -1 (the server's default) and 0: kept until dismissed */
	if (urg >= 2)
		ms = 0;                                     /* critical: never expires (spec) */
	GString *l = g_string_new(NULL);
	g_string_printf(l, "notify %u %u %u %u\t", id, pid, urg > 2 ? 2 : urg, ms);
	esc_cat(l, app, 31);
	g_string_append_c(l, '\t');
	esc_cat(l, desk, 127);
	g_string_append_c(l, '\t');
	esc_cat(l, summary, 95);
	g_string_append_c(l, '\t');
	esc_cat(l, plain, 255);
	for (int k = 0; k < N_ACT; ++k) {
		g_string_append_c(l, '\t');
		esc_cat(l, label[k], 23);
	}
	g_string_append_c(l, '\n');
	line_out(l->str);
	g_string_free(l, TRUE);
	g_free(plain);
	g_free(a);
	g_variant_unref(acts);
	g_variant_unref(hints);
	g_hash_table_replace(notes, GUINT_TO_POINTER(id), n);  /* a replaced one: its old timer goes with it */
	if (ms)
		n->timer = g_timeout_add(ms, expire, GUINT_TO_POINTER(id));
	say("notification %u from %s (pid %u%s%s): %s", id, app, pid, desk[0] ? ", " : "", desk, summary);
	return id;
}

static void method(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *name,
		   GVariant *p, GDBusMethodInvocation *inv, gpointer d)
{
	(void)c, (void)path, (void)iface, (void)d;
	if (!strcmp(name, "GetCapabilities")) {
		const char *caps[] = {"body", "body-markup", "actions", "persistence", NULL};
		g_dbus_method_invocation_return_value(inv, g_variant_new("(^as)", caps));
	} else if (!strcmp(name, "Notify")) {
		g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", notify(sender, p)));
	} else if (!strcmp(name, "CloseNotification")) {
		guint32 id;
		g_variant_get(p, "(u)", &id);
		if (g_hash_table_contains(notes, GUINT_TO_POINTER(id))) {
			char l[48];
			snprintf(l, sizeof(l), "notify-close %u\n", id);
			line_out(l);
			closed(id, 3);
		}
		g_dbus_method_invocation_return_value(inv, NULL);
	} else if (!strcmp(name, "GetServerInformation")) {
		g_dbus_method_invocation_return_value(inv, g_variant_new("(ssss)", "Starbite", "Starbite OS", "1.0", "1.2"));
	} else {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "no method %s", name);
	}
}

/* the host acted on one: "notif-act ID B" */
static void act(guint32 id, guint32 b)
{
	struct note *n = g_hash_table_lookup(notes, GUINT_TO_POINTER(id));
	if (!n) {
		say("notif-act %u %u: no such notification (closed already)", id, b);
		return;
	}
	if (b == SBSHM_NONE) {
		closed(id, 2);
		return;
	}
	if (b > N_ACT || (b && !n->keys[b])) {
		say("notif-act %u %u: no such action", id, b);
		return;
	}
	emit("ActionInvoked", g_variant_new("(us)", id, b ? n->keys[b] : "default"));
	if (!n->resident)
		closed(id, 2);                              /* the host removed it on the tap / button */
}

static gboolean sbwl_in(gint fd, GIOCondition cond, gpointer d)
{
	(void)cond, (void)d;
	static char buf[1024];
	static size_t n;
	ssize_t r = read(fd, buf + n, sizeof(buf) - 1 - n);
	if (r <= 0) {
		if (r < 0 && (errno == EAGAIN || errno == EINTR))
			return G_SOURCE_CONTINUE;
		say("sbwl closed the line: exiting");
		g_main_loop_quit(loop);
		return G_SOURCE_REMOVE;
	}
	n += (size_t)r;
	buf[n] = 0;
	char *s = buf, *e;
	while ((e = strchr(s, '\n'))) {
		*e = 0;
		unsigned id, b;
		if (sscanf(s, "notif-act %u %u", &id, &b) == 2)
			act(id, b);
		s = e + 1;
	}
	n = strlen(s);
	memmove(buf, s, n + 1);
	if (n == sizeof(buf) - 1)
		n = 0;
	return G_SOURCE_CONTINUE;
}

static void name_acquired(GDBusConnection *c, const char *name, gpointer d)
{
	(void)c, (void)d;
	say("%s is ours", name);
	line_out("notify-ready\n");
}
static void name_lost(GDBusConnection *c, const char *name, gpointer d)
{
	(void)c, (void)d;
	say("%s: lost (or never had it): exiting", name);
	g_main_loop_quit(loop);
}

int main(void)
{
	const char *fds = getenv("SBNOTIFY_FD");
	if (fds) {
		sfd = atoi(fds);
	} else {
		struct sockaddr_un sa = {.sun_family = AF_UNIX};
		snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", getenv("SBWL_SOCK") ? getenv("SBWL_SOCK") : "/run/sbwl.sock");
		sfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (sfd < 0 || connect(sfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
			say("%s: %s", sa.sun_path, strerror(errno));
			return 1;
		}
	}
	GError *e = NULL;
	bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &e);
	if (!bus) {
		say("no session bus: %s", e ? e->message : "?");
		return 1;
	}
	notes = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, note_free);
	GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(xml, &e);
	static const GDBusInterfaceVTable vt = {.method_call = method};
	if (!info || !g_dbus_connection_register_object(bus, "/org/freedesktop/Notifications", info->interfaces[0], &vt, NULL,
							NULL, &e)) {
		say("register: %s", e ? e->message : "?");
		return 1;
	}
	loop = g_main_loop_new(NULL, FALSE);
	g_unix_fd_add(sfd, G_IO_IN | G_IO_HUP | G_IO_ERR, sbwl_in, NULL);
	g_bus_own_name_on_connection(bus, "org.freedesktop.Notifications", G_BUS_NAME_OWNER_FLAGS_REPLACE, name_acquired,
				     name_lost, NULL, NULL);
	g_main_loop_run(loop);
	return 0;
}
