// SPDX-License-Identifier: GPL-2.0-only
/*
 * gtkbench: a GTK 4 window that changes every frame (Track E, 2026-11-14: "Linux apps have no GPU, they're super
 * slow"): a grid of cards (an icon, two labels, a progress bar) whose texts and bars change on each frame-clock tick,
 * so every frame is a relayout and a full repaint, as a scrolling list or an animation is. It measures what one GSK
 * renderer (GSK_RENDERER=cairo / ngl / vulkan) achieves on the guest's CPUs and writes, every 2 s, to the file given
 * (default /tmp/gtkbench.txt):  fps <frames per second over the last 2 s> renderer <GSK renderer's type name>
 *   gtkbench [OUTFILE] [CARDS=96]
 */
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>

static GtkWidget *win, *labels[512], *bars[512];
static int ncards = 96;
static unsigned long frames;
static gint64 t_mark;
static unsigned long f_mark;
static const char *out = "/tmp/gtkbench.txt";

static gboolean tick(GtkWidget *w, GdkFrameClock *fc, gpointer data)
{
	(void)w, (void)data;
	char b[32];
	++frames;
	for (int i = 0; i < ncards; ++i) {
		snprintf(b, sizeof(b), "%lu", (frames * 7 + (unsigned long)i * 13) % 1000);
		gtk_label_set_text(GTK_LABEL(labels[i]), b);
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bars[i]), (double)((frames + (unsigned long)i * 3) % 100) / 100.0);
	}
	gint64 now = gdk_frame_clock_get_frame_time(fc);
	if (!t_mark)
		t_mark = now, f_mark = frames;
	if (now - t_mark >= 2000000) {
		double fps = (double)(frames - f_mark) * 1e6 / (double)(now - t_mark);
		GskRenderer *r = gtk_native_get_renderer(GTK_NATIVE(win));
		FILE *f = fopen(out, "a");
		if (f) {
			fprintf(f, "fps %.1f renderer %s\n", fps, r ? G_OBJECT_TYPE_NAME(r) : "-");
			fclose(f);
		}
		t_mark = now, f_mark = frames;
	}
	return G_SOURCE_CONTINUE;
}

static void activate(GtkApplication *app, gpointer data)
{
	(void)data;
	win = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(win), "GTK bench");
	GtkWidget *sc = gtk_scrolled_window_new();
	GtkWidget *grid = gtk_flow_box_new();
	gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(grid), 3);
	gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(grid), GTK_SELECTION_NONE);
	static const char *icons[] = {"folder", "user-home", "document-open", "edit-find", "emblem-favorite",
				      "help-browser", "media-playback-start", "preferences-system"};
	for (int i = 0; i < ncards; ++i) {
		GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
		gtk_widget_add_css_class(card, "card");
		gtk_widget_set_margin_start(card, 6), gtk_widget_set_margin_end(card, 6);
		GtkWidget *img = gtk_image_new_from_icon_name(icons[i % 8]);
		gtk_image_set_pixel_size(GTK_IMAGE(img), 40);
		char t[32];
		snprintf(t, sizeof(t), "Item %d", i + 1);
		GtkWidget *title = gtk_label_new(t);
		gtk_widget_add_css_class(title, "heading");
		labels[i] = gtk_label_new("0");
		bars[i] = gtk_progress_bar_new();
		gtk_box_append(GTK_BOX(card), img);
		gtk_box_append(GTK_BOX(card), title);
		gtk_box_append(GTK_BOX(card), labels[i]);
		gtk_box_append(GTK_BOX(card), bars[i]);
		gtk_flow_box_append(GTK_FLOW_BOX(grid), card);
	}
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), grid);
	gtk_window_set_child(GTK_WINDOW(win), sc);
	gtk_widget_add_tick_callback(win, tick, NULL, NULL);
	gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char **argv)
{
	if (argc > 1)
		out = argv[1];
	if (argc > 2)
		ncards = atoi(argv[2]) > 0 && atoi(argv[2]) <= 512 ? atoi(argv[2]) : 96;
	GtkApplication *app = gtk_application_new("org.starbite.GtkBench", G_APPLICATION_NON_UNIQUE);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	char *a[] = {argv[0], NULL};
	return g_application_run(G_APPLICATION(app), 1, a);
}
