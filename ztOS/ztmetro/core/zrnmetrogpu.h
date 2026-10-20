// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ZRNMETROGPU_H
#define ZRNMETROGPU_H

#include <stdbool.h>
#include <cairo.h>
#include <wayland-client.h>

/* Corners in points: top left, top right, bottom left, bottom right.
 * Positive z is toward the viewer. The shader does the perspective divide. */
struct zmg_vertex { float x, y, z; };

bool zmg_init(struct wl_display *display, struct wl_surface *surface, int w, int h, int scale);
bool zmg_ready(void);
void zmg_begin(int w, int h, int scale);
bool zmg_end(void);
void zmg_clip(double x, double y, double w, double h);
void zmg_rect(double x, double y, double w, double h, float r, float g, float b);
void zmg_prepare(cairo_surface_t *source, unsigned revision);
/* Pixel coordinates in the Cairo source; out-of-bounds parts are clipped.
 * A source is uploaded only when its identity or revision changes. */
void zmg_surface(cairo_surface_t *source, unsigned revision,
		double x0, double y0, double x1, double y1,
		const struct zmg_vertex corners[4], float alpha);
void zmg_report(const char *app_id);
void zmg_fini(void);

#endif
