// SPDX-License-Identifier: GPL-2.0-only
/*
 * notifytest: hostsim's notification client (GIO only). Started by sbwl as the app org.starbite.NotifyTest, it posts
 * over org.freedesktop.Notifications (sbnotify), with an app_name the catalog does not know (sbwl must find the app
 * from the sender's process):
 *   1 "Hello from Linux"  body with markup, buttons Reply / Later, a default action, urgency critical
 *   2 "Withdrawn"         closed by the app itself 500 ms later (CloseNotification)
 *   3 "Tap me"            a default action only
 *   4 "Swipe me"          no actions
 * and records to $SBWL_TEST_OUT (default /tmp/sbwl-notifytest.txt), the whole file again at every change:
 *   tz:<$TZ>  tzoff:<the local UTC offset in s>  caps:<GetCapabilities>  server:<name>
 *   idN:<id>  action:N:<key>  closed:N:<reason>         (N = 1..4 above)
 * It exits after 120 s.
 */
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static GDBusConnection *bus;
static GString *out;
static guint32 ids[5];

static void save(void)
{
	const char *o = g_getenv("SBWL_TEST_OUT") ? g_getenv("SBWL_TEST_OUT") : "/tmp/sbwl-notifytest.txt";
	char *tmp = g_strdup_printf("%s.new", o);
	if (g_file_set_contents(tmp, out->str, (gssize)out->len, NULL))
		rename(tmp, o);
	g_free(tmp);
}

static int which(guint32 id)
{
	for (int i = 1; i <= 4; ++i)
		if (ids[i] == id)
			return i;
	return 0;
}

static void on_signal(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *sig,
		      GVariant *p, gpointer d)
{
	(void)c, (void)sender, (void)path, (void)iface, (void)d;
	guint32 id, reason;
	const char *key;
	if (!strcmp(sig, "ActionInvoked")) {
		g_variant_get(p, "(u&s)", &id, &key);
		g_string_append_printf(out, "action:%d:%s\n", which(id), key);
	} else if (!strcmp(sig, "NotificationClosed")) {
		g_variant_get(p, "(uu)", &id, &reason);
		g_string_append_printf(out, "closed:%d:%u\n", which(id), reason);
	} else {
		return;
	}
	save();
}

static GVariant *call(const char *m, GVariant *args, const char *rtype)
{
	GError *e = NULL;
	GVariant *r = g_dbus_connection_call_sync(bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
						  "org.freedesktop.Notifications", m, args, rtype ? G_VARIANT_TYPE(rtype) : NULL,
						  G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &e);
	if (!r) {
		g_string_append_printf(out, "error:%s:%s\n", m, e->message);
		g_error_free(e);
	}
	return r;
}

static guint32 post(const char *summary, const char *body, const char *const *acts, guchar urgency)
{
	GVariantBuilder h;
	g_variant_builder_init(&h, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&h, "{sv}", "urgency", g_variant_new_byte(urgency));
	GVariant *r = call("Notify", g_variant_new("(susss^as@a{sv}i)", "notifytest-sender", 0, "dialog-information", summary, body,
						    acts, g_variant_builder_end(&h), -1), "(u)");
	guint32 id = 0;
	if (r) {
		g_variant_get(r, "(u)", &id);
		g_variant_unref(r);
	}
	return id;
}

static gboolean withdraw(gpointer d)
{
	(void)d;
	GVariant *r = call("CloseNotification", g_variant_new("(u)", ids[2]), NULL);
	if (r)
		g_variant_unref(r);
	save();
	return G_SOURCE_REMOVE;
}

static gboolean quit(gpointer d)
{
	g_main_loop_quit(d);
	return G_SOURCE_REMOVE;
}

int main(void)
{
	out = g_string_new(NULL);
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	g_string_append_printf(out, "tz:%s\ntzoff:%ld\n", g_getenv("TZ") ? g_getenv("TZ") : "", (long)tm.tm_gmtoff);
	bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
	if (!bus) {
		g_string_append(out, "error:no session bus\n");
		save();
		return 1;
	}
	g_dbus_connection_signal_subscribe(bus, NULL, "org.freedesktop.Notifications", NULL, "/org/freedesktop/Notifications",
					   NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_signal, NULL, NULL);
	GVariant *r = NULL;
	for (int i = 0; i < 50 && !r; ++i) {               /* sbnotify may still be starting */
		GVariant *o = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
							  "NameHasOwner", g_variant_new("(s)", "org.freedesktop.Notifications"),
							  G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
		gboolean has = FALSE;
		if (o) {
			g_variant_get(o, "(b)", &has);
			g_variant_unref(o);
		}
		if (has)
			r = call("GetCapabilities", NULL, "(as)");
		else
			g_usleep(100000);
	}
	if (r) {
		const char **caps = NULL;
		g_variant_get(r, "(^a&s)", &caps);
		char *j = g_strjoinv(",", (char **)caps);
		g_string_append_printf(out, "caps:%s\n", j);
		g_free(j);
		g_free(caps);
		g_variant_unref(r);
	}
	if ((r = call("GetServerInformation", NULL, "(ssss)"))) {
		const char *name;
		g_variant_get(r, "(&s&s&s&s)", &name, NULL, NULL, NULL);
		g_string_append_printf(out, "server:%s\n", name);
		g_variant_unref(r);
	}
	static const char *const a1[] = {"default", "Open", "reply", "Reply", "later", "Later", NULL};
	static const char *const a3[] = {"default", "Open", NULL};
	static const char *const none[] = {NULL};
	ids[1] = post("Hello from Linux", "<b>Bold</b> &amp; <i>plain</i> &lt;3", a1, 2);
	ids[2] = post("Withdrawn", "this one goes away", none, 1);
	ids[3] = post("Tap me", "the default action", a3, 1);
	ids[4] = post("Swipe me", "no actions", none, 0);
	for (int i = 1; i <= 4; ++i)
		g_string_append_printf(out, "id%d:%u\n", i, ids[i]);
	save();
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	g_timeout_add(500, withdraw, NULL);
	g_timeout_add(120000, quit, loop);
	g_main_loop_run(loop);
	return 0;
}
