// SPDX-License-Identifier: GPL-2.0-or-later
/* ztstore: what makes a package an app: its AppStream entry (a proper name, a
 * summary, an icon and its categories), and the .desktop file of an installed
 * one. Arch publishes AppStream for its repos as archlinux-appstream-data. */
#ifndef ZTSTORE_APPSTREAM_H
#define ZTSTORE_APPSTREAM_H

#include "repo.h"

/* Read every catalogue under the AppStream folders and put what they say on
 * the matching packages in `ri` (sync and local). Returns how many apps it
 * found; 0 when there is no AppStream data on the phone. Then fill in the
 * installed apps' names and icons from their .desktop files. */
int appstream_apply(struct repo_index *ri, const char *const *roots);

/* The folders looked in, in order (the newer "swcatalog" name first). */
extern const char *const APPSTREAM_ROOTS[];

/* Parse one catalogue's text (exposed for the tests): calls `each` per app. */
struct as_app {
	char *pkgname, *name, *summary, *icon_file, *categories;
	int icon_size;
};
void appstream_parse(const char *xml, void (*each)(const struct as_app *app, const char *origin, void *data),
	void *data);

/* An icon by its theme name, or a path if it is one already: the largest
 * hicolor PNG up to 128 px, else an SVG, else one in pixmaps. "" if none. */
void icon_lookup(const char *name, char *out, size_t size);

#endif
