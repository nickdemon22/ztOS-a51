// SPDX-License-Identifier: GPL-2.0-or-later
/* ztstore: the AUR, through its RPC (search, details) and its daily metadata
 * dump (the top lists), plus pacman's way of comparing versions. */
#ifndef ZTSTORE_AUR_H
#define ZTSTORE_AUR_H

#include "json.h"
#include "pkg.h"

#define AUR_BASE	"https://aur.archlinux.org"

/* AUR packages whose name or description holds `term` (2 characters at least) */
bool aur_search(const char *term, struct vec *out);
/* details (dependencies, licence) for these names; fills `out` */
bool aur_info(const char *const *names, int n, struct vec *out);
/* one RPC result (or dump entry) as a package */
struct pkg *aur_pkg_from_json(const struct json *j);

/* the metadata dump, fetched at most once a day; its path, or false */
bool aur_dump(char *path, size_t size);

/* pacman's version order (alpm_pkg_vercmp): <0, 0, >0 */
int vercmp(const char *a, const char *b);

#endif
