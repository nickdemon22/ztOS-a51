// SPDX-License-Identifier: GPL-2.0-or-later
/* ztstore: the phone's own pacman databases, read directly (no root needed).
 * Which repos there are, and where, comes from pacman.conf. */
#ifndef ZTSTORE_REPO_H
#define ZTSTORE_REPO_H

#include "pkg.h"

struct repo_index {
	char *dbpath;
	struct vec repos;	/* char *: repo names, in pacman.conf's order */
	struct vec sync;	/* struct pkg *: every package the repos offer */
	struct vec local;	/* struct pkg *: every installed package */
};

/* Read pacman.conf (`conf`, normally /etc/pacman.conf, following Include
 * lines only for their [options]), the sync databases and the local one. */
bool repo_load(struct repo_index *ri, const char *conf);
void repo_free(struct repo_index *ri);
struct pkg *repo_find_sync(const struct repo_index *ri, const char *name);
struct pkg *repo_find_local(const struct repo_index *ri, const char *name);
/* installed packages that no repo offers: from the AUR, or built by hand */
void repo_foreign(const struct repo_index *ri, struct vec *out);

/* Parse one pacman "desc" record ("%NAME%\nfoo\n\n%VERSION%..."); returns how
 * much of `text` it used, filling `p`. Exposed for the tests. */
size_t repo_parse_desc(const char *text, struct pkg *p);

#endif
