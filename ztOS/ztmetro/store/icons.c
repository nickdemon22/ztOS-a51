// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE
#include "icons.h"

#include <stdlib.h>
#include <string.h>

#include "zrnmetrocore.h"
#ifdef HAVE_RSVG
#include <librsvg/rsvg.h>
#endif

#define ICON_PX		128	/* drawn from at most this; tiles are smaller */
#define MAX_ICONS	256

static struct {
	char *path;
	cairo_surface_t *surface;	/* NULL: it could not be read */
} cache[MAX_ICONS];
static int n_cache;

static cairo_surface_t *load(const char *path) {
	size_t n = strlen(path);
	if (n > 4 && !strcmp(path + n - 4, ".png")) {
		cairo_surface_t *s = cairo_image_surface_create_from_png(path);
		if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
			cairo_surface_destroy(s);
			return NULL;
		}
		return s;
	}
#ifdef HAVE_RSVG
	if (n > 4 && !strcmp(path + n - 4, ".svg")) {
		RsvgHandle *h = rsvg_handle_new_from_file(path, NULL);
		if (!h) {
			return NULL;
		}
		cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ICON_PX, ICON_PX);
		cairo_t *cr = cairo_create(s);
		RsvgRectangle box = { 0, 0, ICON_PX, ICON_PX };
		bool ok = rsvg_handle_render_document(h, cr, &box, NULL);
		cairo_destroy(cr);
		g_object_unref(h);
		if (!ok) {
			cairo_surface_destroy(s);
			return NULL;
		}
		return s;
	}
#endif
	return NULL;
}

static cairo_surface_t *icon(const char *path) {
	for (int i = 0; i < n_cache; i++) {
		if (!strcmp(cache[i].path, path)) {
			return cache[i].surface;
		}
	}
	if (n_cache == MAX_ICONS) {
		icons_clear();	/* rarely: start over rather than grow without end */
	}
	cache[n_cache].path = strdup(path);
	cache[n_cache].surface = load(path);
	return cache[n_cache++].surface;
}

void icons_clear(void) {
	for (int i = 0; i < n_cache; i++) {
		free(cache[i].path);
		if (cache[i].surface) {
			cairo_surface_destroy(cache[i].surface);
		}
	}
	n_cache = 0;
}

static void tile(cairo_t *cr, double x, double y, double size, const struct pkg *p, bool shaded) {
	if (shaded) {
		cairo_set_source_rgba(cr, 0, 0, 0, 0.25);
	} else {
		zmc_accent(cr);
	}
	cairo_rectangle(cr, x, y, size, size);
	cairo_fill(cr);
	cairo_surface_t *s = p->icon && *p->icon ? icon(p->icon) : NULL;
	if (s) {
		double w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
		double fit = size * 0.72, scale = fit / (w > h ? w : h);
		cairo_save(cr);
		cairo_translate(cr, x + (size - w * scale) / 2, y + (size - h * scale) / 2);
		cairo_scale(cr, scale, scale);
		cairo_set_source_surface(cr, s, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_restore(cr);
		return;
	}
	/* the first letter (a whole UTF-8 character) of the name */
	const char *t = pkg_title(p);
	char letter[8] = "";
	size_t len = 1;
	unsigned char c = (unsigned char)t[0];
	if (c >= 0xf0) {
		len = 4;
	} else if (c >= 0xe0) {
		len = 3;
	} else if (c >= 0xc0) {
		len = 2;
	}
	if (strlen(t) >= len) {
		memcpy(letter, t, len);
		letter[len] = 0;
	}
	cairo_set_source_rgb(cr, 1, 1, 1);
	double fs = size * 0.46;
	zmc_text_centred(cr, letter, FONT_LIGHT, fs, x + size / 2, y + (size - fs * 1.33) / 2);
}

void draw_tile(cairo_t *cr, double x, double y, double size, const struct pkg *p) {
	tile(cr, x, y, size, p, false);
}

void draw_tile_shaded(cairo_t *cr, double x, double y, double size, const struct pkg *p) {
	tile(cr, x, y, size, p, true);
}
