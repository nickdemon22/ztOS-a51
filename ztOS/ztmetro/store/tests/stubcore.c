/* A stand-in for ZrnMetroCore with cairo's toy text, for drawing the App
 * Store's pages into PNGs on a machine without pango or Wayland. Close to the
 * real metrics, not exact: for checking layout, not type. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "zrnmetrocore.h"

struct zmc_state zmc;

static void font(cairo_t *cr, const char *f, double size) {
	bool bold = strstr(f, "Semibold") != NULL;
	bool mono = strstr(f, "Mono") != NULL;
	cairo_select_font_face(cr, mono ? "monospace" : "sans", CAIRO_FONT_SLANT_NORMAL,
		bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, size * (strstr(f, "Light") ? 0.95 : 1.0));
}

static cairo_t *measure_cr(void) {
	static cairo_t *m;
	if (!m) {
		m = cairo_create(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 4));
	}
	return m;
}

double zmc_text_width(const char *s, const char *f, double size) {
	cairo_t *cr = measure_cr();
	font(cr, f, size);
	cairo_text_extents_t e;
	cairo_text_extents(cr, s, &e);
	return e.x_advance;
}

double zmc_text(cairo_t *cr, const char *s, const char *f, double size, double x, double y, double wrap) {
	font(cr, f, size);
	double lh = size * 1.33, yy = y;
	if (wrap <= 0) {
		cairo_move_to(cr, x, y + size);
		cairo_show_text(cr, s);
		return lh;
	}
	char line[2048] = "";
	char word[512];
	const char *p = s;
	int lines = 0;
	while (*p) {
		int n = 0;
		while (*p == ' ') p++;
		while (*p && *p != ' ' && n < 510) word[n++] = *p++;
		word[n] = 0;
		if (!n) break;
		char trial[2600];
		snprintf(trial, sizeof(trial), "%s%s%s", line, line[0] ? " " : "", word);
		if (line[0] && zmc_text_width(trial, f, size) > wrap) {
			cairo_move_to(cr, x, yy + size); cairo_show_text(cr, line); yy += lh; lines++;
			snprintf(line, sizeof(line), "%s", word);
		} else {
			snprintf(line, sizeof(line), "%s", trial);
		}
	}
	if (line[0]) { cairo_move_to(cr, x, yy + size); cairo_show_text(cr, line); lines++; }
	return lines * lh;
}

void zmc_text_line(cairo_t *cr, const char *s, const char *f, double size, double x, double y, double width) {
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s", s);
	while (strlen(buf) > 1 && zmc_text_width(buf, f, size) > width) {
		size_t n = strlen(buf);
		buf[n - 1] = 0;
		if (n > 4) { buf[n - 4] = '.'; buf[n - 3] = '.'; buf[n - 2] = '.'; buf[n-1]=0; buf[n - 4 + 3] = 0; }
	}
	font(cr, f, size);
	cairo_move_to(cr, x, y + size);
	cairo_show_text(cr, buf);
}

void zmc_text_centred(cairo_t *cr, const char *s, const char *f, double size, double cx, double y) {
	double w = zmc_text_width(s, f, size);
	zmc_text(cr, s, f, size, cx - w / 2, y, 0);
}

void zmc_add_hit(double x, double y, double w, double h, int action, int arg) {
	if (zmc.n_hits < ZMC_MAX_HITS) zmc.hits[zmc.n_hits++] = (struct zmc_hit){ x, y, w, h, action, arg, false };
}

void zmc_set_rgb(cairo_t *cr, uint32_t c) {
	cairo_set_source_rgb(cr, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0);
}
void zmc_accent(cairo_t *cr) { zmc_set_rgb(cr, zmc.accent); }

double zmc_pivot_next(cairo_t *cr, const char *title, const char *next, int action, int arg) {
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, zmc.app->caps, FONT_SEMIBOLD, 11.5, ZMC_MARGIN, 14, 0);
	zmc_text(cr, title, FONT_SEMILIGHT, 48, ZMC_MARGIN - 3, 24, 0);
	if (next) {
		double w = zmc_text_width(title, FONT_SEMILIGHT, 48);
		cairo_set_source_rgb(cr, 0.38, 0.38, 0.38);
		zmc_text(cr, next, FONT_SEMILIGHT, 48, ZMC_MARGIN - 3 + w + 16, 24, 0);
	}
	zmc_add_hit(0, 0, zmc.width, 90, ZMC_ACT_BACK, 0);
	return 108;
}
double zmc_pivot(cairo_t *cr, const char *t, const char *n) { return zmc_pivot_next(cr, t, n, 0, 0); }
double zmc_header(cairo_t *cr, const char *t) { return zmc_pivot(cr, t, NULL); }

double zmc_entry(cairo_t *cr, double y, const char *title, const char *detail, int action, int arg) {
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, title, FONT_SEMILIGHT, 25.5, ZMC_MARGIN, y, 0);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, detail, FONT_REGULAR, 12, ZMC_MARGIN, y + 31, 0);
	zmc_add_hit(0, y - 2, zmc.width, 57, action, arg);
	return y + 59;
}

double zmc_field(cairo_t *cr, double y, const char *caption, const char *value) {
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, caption, FONT_REGULAR, 13, ZMC_MARGIN, y, 0);
	cairo_set_source_rgb(cr, 1, 1, 1);
	return y + 20 + zmc_text(cr, value, FONT_REGULAR, 16, ZMC_MARGIN, y + 18, zmc.width - 2 * ZMC_MARGIN) + 12;
}

double zmc_textbox(cairo_t *cr, double y, const char *caption, const char *value, bool focused, int action, int arg) {
	double w = zmc.width - 2 * ZMC_MARGIN, h = 38;
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, caption, FONT_REGULAR, 13, ZMC_MARGIN, y, 0);
	y += 22;
	if (focused) cairo_set_source_rgb(cr, 1, 1, 1); else cairo_set_source_rgb(cr, 0.78, 0.78, 0.78);
	cairo_rectangle(cr, ZMC_MARGIN, y, w, h);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
	zmc_text(cr, value, FONT_REGULAR, 17, ZMC_MARGIN + 8, y + 8, 0);
	zmc_add_hit(0, y - 24, zmc.width, h + 30, action, arg);
	return y + h + 16;
}

void zmc_busy(bool b) {}
void zmc_watch_fd(int fd, void (*r)(int, void *), void *d) {}
void zmc_every(int ms, void (*t)(void)) {}
void zmc_go(int page, int dir) { zmc.page = page; }
void zmc_redraw(void) {}
void zmc_keyboard(bool s) {}
void zmc_haptic(const char *k) {}
void zmc_read_file(const char *path, char *out, size_t size) {
	out[0] = 0;
	FILE *f = fopen(path, "r");
	if (f) { if (fgets(out, size, f)) out[strcspn(out, "\n")] = 0; fclose(f); }
}

int zmc_render_png(const struct zmc_app *app, int page, int height, const char *path) {
	zmc.app = app; zmc.width = 360; zmc.page = page; zmc.accent = 0x0050EF;
	cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 360, height);
	cairo_t *cr = cairo_create(probe);
	double h = app->render(cr, page);
	cairo_destroy(cr);
	cairo_surface_destroy(probe);
	int H = (int)ceil(h) + 10;
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 360 * 2, H * 2);
	cr = cairo_create(s);
	cairo_scale(cr, 2, 2);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	zmc.n_hits = 0;
	app->render(cr, page);
	cairo_destroy(cr);
	int r = cairo_surface_write_to_png(s, path) == CAIRO_STATUS_SUCCESS ? 0 : 1;
	cairo_surface_destroy(s);
	printf("%s: %d hits, %.0f points tall\n", path, zmc.n_hits, h);
	return r;
}
int zmc_run(const struct zmc_app *app, int first) { return 1; }
