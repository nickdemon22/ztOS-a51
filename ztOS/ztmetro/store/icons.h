// SPDX-License-Identifier: GPL-2.0-or-later
/* ztstore: app icons, read once and kept, and the tile every package gets. */
#ifndef ZTSTORE_ICONS_H
#define ZTSTORE_ICONS_H

#include <cairo.h>
#include "pkg.h"

/* An accent square `size` across with the app's icon on it, or the first
 * letter of its name where it has none (the AUR keeps no icons). */
void draw_tile(cairo_t *cr, double x, double y, double size, const struct pkg *p);
/* the same on an accent background: a darker square, so it still shows */
void draw_tile_shaded(cairo_t *cr, double x, double y, double size, const struct pkg *p);
void icons_clear(void);

#endif
