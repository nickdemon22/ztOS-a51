// SPDX-License-Identifier: GPL-2.0-or-later
/* ztstore: the lists on the "top" page. Which lists there are, what they sort
 * by and how long they are comes from a file, never from the code:
 *
 *   ~/.config/ztstore/sections, else /etc/ztstore/sections
 *   title | source | sort | count [| caption]
 *
 * source "aur": sort popularity, votes, modified or submitted (the AUR's daily
 * metadata dump). source "repo": sort popular (pkgstats.archlinux.org: how many
 * Arch systems have it) or builddate; only packages that are apps (AppStream). */
#ifndef ZTSTORE_SECTIONS_H
#define ZTSTORE_SECTIONS_H

#include "repo.h"

struct section {
	char *title, *source, *sort, *caption;
	int count;
	struct vec items;	/* struct pkg *, copies this section owns */
	char *note;		/* why it is empty, when it is */
};

/* the file's sections, in its order; false if there is no file */
bool sections_read(struct vec *out, const char *path);
/* where the file is: the user's, else the system's */
void sections_path(char *out, size_t size);
void section_free(void *s);

/* fill every section: aur_dump and pkgstats are file paths, or NULL when
 * they could not be fetched */
void sections_fill(struct vec *sections, const struct repo_index *ri, const char *aur_dump,
	const char *pkgstats);

/* the browsing lists on the "categories" page, from the data on the phone */
struct facet {
	char *key, *label;
	int count;
};
void facets_categories(const struct repo_index *ri, struct vec *out, int max);
void facets_groups(const struct repo_index *ri, struct vec *out, int max);
void facet_free(void *f);
/* the packages under one category or group */
void facet_members(const struct repo_index *ri, bool group, const char *key, struct vec *out);

#endif
