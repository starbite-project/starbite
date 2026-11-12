// SPDX-License-Identifier: GPL-2.0-only
/*
 * gtktest: hostsim's GTK client (GTK 3 or 4, same source): a window with a text entry over a button. It writes its
 * state to $SBWL_TEST_OUT (default /tmp/sbwl-gtk<N>test.txt) whenever it changes:
 *   text:<the entry's text>
 *   size:<W>x<H>          the window's size (sbwl's configure)
 *   entry:<x> <y> <w> <h> the entry in window coordinates (hostsim taps its middle)
 *   focus:<0|1>           the entry has the keyboard focus
 *   dialogs:<n>           dialogs opened (the button opens one: a transient 400 x 200 toplevel)
 *   cursor:<n>            the entry's cursor position (characters)
 * The button has the focus at the start, so the entry gets it (and text-input-v3 enables) only from a tap.
 */
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#if GTK_MAJOR_VERSION >= 4
#define GTK_WINDOW_TOPLEVEL_ARG
#else
#define GTK_WINDOW_TOPLEVEL_ARG GTK_WINDOW_TOPLEVEL
#endif

static GtkWidget *win, *entry, *button;
static char last[1024];
static int dialogs;

/* the button opens a dialog: a small toplevel with the main window as its parent (sbwl centers it over it) */
static void open_dialog(GtkWidget *w, gpointer data)
{
	(void)w, (void)data;
	GtkWidget *d = gtk_window_new(GTK_WINDOW_TOPLEVEL_ARG);
	gtk_window_set_title(GTK_WINDOW(d), "A dialog");
	gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(win));
	gtk_window_set_default_size(GTK_WINDOW(d), 400, 200);
	GtkWidget *l = gtk_label_new("The dialog, over its window");
#if GTK_MAJOR_VERSION >= 4
	gtk_window_set_child(GTK_WINDOW(d), l);
	gtk_window_present(GTK_WINDOW(d));
#else
	gtk_container_add(GTK_CONTAINER(d), l);
	gtk_widget_show_all(d);
#endif
	++dialogs;
}

static int poll_state(gpointer data)
{
	(void)data;
	char s[1024];
	int w, h;
#if GTK_MAJOR_VERSION >= 4
	w = gtk_widget_get_width(win), h = gtk_widget_get_height(win);
	graphene_point_t p = GRAPHENE_POINT_INIT(0, 0), q;
	if (!gtk_widget_compute_point(entry, win, &p, &q))
		q.x = q.y = -1;
	int ex = (int)q.x, ey = (int)q.y, ew = gtk_widget_get_width(entry), eh = gtk_widget_get_height(entry);
	const char *t = gtk_editable_get_text(GTK_EDITABLE(entry));
	int f = gtk_widget_has_focus(entry) || gtk_widget_has_focus(gtk_widget_get_first_child(entry)) ||
		gtk_widget_is_focus(entry) || (gtk_root_get_focus(GTK_ROOT(win)) &&
					       gtk_widget_is_ancestor(gtk_root_get_focus(GTK_ROOT(win)), entry));
#else
	w = gtk_widget_get_allocated_width(win), h = gtk_widget_get_allocated_height(win);
	int ex = -1, ey = -1;
	gtk_widget_translate_coordinates(entry, win, 0, 0, &ex, &ey);
	int ew = gtk_widget_get_allocated_width(entry), eh = gtk_widget_get_allocated_height(entry);
	const char *t = gtk_entry_get_text(GTK_ENTRY(entry));
	int f = gtk_widget_has_focus(entry);
#endif
	snprintf(s, sizeof(s), "text:%s\nsize:%dx%d\nentry:%d %d %d %d\nfocus:%d\ndialogs:%d\ncursor:%d\n", t, w, h, ex, ey, ew, eh, f,
		 dialogs, gtk_editable_get_position(GTK_EDITABLE(entry)));
	if (strcmp(s, last)) {
		snprintf(last, sizeof(last), "%s", s);
		const char *o = g_getenv("SBWL_TEST_OUT");
		char def[64];
		snprintf(def, sizeof(def), "/tmp/sbwl-gtk%dtest.txt", GTK_MAJOR_VERSION);
		char tmp[300];
		snprintf(tmp, sizeof(tmp), "%s.new", o ? o : def);
		FILE *fp = fopen(tmp, "w");
		if (fp) {
			fputs(s, fp);
			fclose(fp);
			rename(tmp, o ? o : def);
		}
	}
	return G_SOURCE_CONTINUE;
}

#if GTK_MAJOR_VERSION >= 4
static void activate(GtkApplication *app, gpointer data)
{
	(void)data;
	win = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(win), "GTK4 test");
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	entry = gtk_entry_new();
	button = gtk_button_new_with_label("A button (GTK 4)");
	gtk_widget_set_vexpand(button, TRUE);
	g_signal_connect(button, "clicked", G_CALLBACK(open_dialog), NULL);
	gtk_box_append(GTK_BOX(box), entry);
	gtk_box_append(GTK_BOX(box), button);
	gtk_window_set_child(GTK_WINDOW(win), box);
	gtk_window_present(GTK_WINDOW(win));
	gtk_widget_grab_focus(button);
	g_timeout_add(100, poll_state, NULL);
}
int main(int argc, char **argv)
{
	GtkApplication *app = gtk_application_new("org.starbite.GtkTest4", G_APPLICATION_NON_UNIQUE);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	return g_application_run(G_APPLICATION(app), argc, argv);
}
#else
int main(int argc, char **argv)
{
	g_set_prgname("org.starbite.GtkTest3");        /* the app_id */
	gtk_init(&argc, &argv);
	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), "GTK3 test");
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	entry = gtk_entry_new();
	button = gtk_button_new_with_label("A button (GTK 3)");
	gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), button, TRUE, TRUE, 0);
	g_signal_connect(button, "clicked", G_CALLBACK(open_dialog), NULL);
	gtk_container_add(GTK_CONTAINER(win), box);
	gtk_widget_show_all(win);
	gtk_widget_grab_focus(button);
	g_timeout_add(100, poll_state, NULL);
	gtk_main();
	return 0;
}
#endif
