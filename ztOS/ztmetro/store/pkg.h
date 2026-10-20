// SPDX-License-Identifier: GPL-2.0-or-later
/* ztstore: what the App Store knows about one package, from wherever it came. */
#ifndef ZTSTORE_PKG_H
#define ZTSTORE_PKG_H

#include <stdbool.h>
#include "util.h"

struct pkg {
	char *name;
	char *repo;		/* a repo from pacman.conf, or "aur" */
	char *version;
	char *desc;
	char *url, *license, *arch, *packager;	/* packager: the AUR's Maintainer */
	char *groups;		/* space separated */
	char *depends, *makedepends;	/* space separated */
	double csize, isize;	/* download and installed size, bytes */
	long builddate;		/* repos */
	double votes, popularity;	/* AUR */
	long submitted, modified;	/* AUR */
	/* from AppStream (repos) or the installed .desktop file */
	char *app_name, *summary, *icon, *categories;	/* icon: a file path */
	/* this phone */
	bool installed, explicit_install;
	char *installed_version;
	char *desktop;		/* the installed .desktop file, if it has one */
};

struct pkg *pkg_new(const char *name, const char *repo);
struct pkg *pkg_copy(const struct pkg *p);
void pkg_free(void *p);
void set_str(char **field, const char *value);	/* replaces, copying */
/* append a word to a space-separated list */
void append_word(char **field, const char *word);
bool pkg_is_aur(const struct pkg *p);
const char *pkg_title(const struct pkg *p);	/* the app's name, else the package's */
const char *pkg_blurb(const struct pkg *p);	/* the summary, else the description */

#endif
