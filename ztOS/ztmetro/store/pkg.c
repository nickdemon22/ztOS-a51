// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE
#include "pkg.h"

#include <stdlib.h>
#include <string.h>

struct pkg *pkg_new(const char *name, const char *repo) {
	struct pkg *p = calloc(1, sizeof(*p));
	if (!p) {
		abort();
	}
	p->name = xstrdup(name);
	p->repo = xstrdup(repo);
	return p;
}

#define STR_FIELDS(X) X(name) X(repo) X(version) X(desc) X(url) X(license) X(arch) X(packager) \
	X(groups) X(depends) X(makedepends) X(app_name) X(summary) X(icon) X(categories) \
	X(installed_version) X(desktop)

struct pkg *pkg_copy(const struct pkg *p) {
	struct pkg *q = malloc(sizeof(*q));
	if (!q) {
		abort();
	}
	*q = *p;
#define DUP(f) q->f = xstrdup(p->f);
	STR_FIELDS(DUP)
#undef DUP
	return q;
}

void pkg_free(void *v) {
	struct pkg *p = v;
	if (!p) {
		return;
	}
#define FREE(f) free(p->f);
	STR_FIELDS(FREE)
#undef FREE
	free(p);
}

void set_str(char **field, const char *value) {
	char *v = xstrdup(value);
	free(*field);
	*field = v;
}

void append_word(char **field, const char *word) {
	if (!word || !*word) {
		return;
	}
	size_t a = *field ? strlen(*field) : 0, b = strlen(word);
	char *s = realloc(*field, a + b + 2);
	if (!s) {
		abort();
	}
	if (a) {
		s[a++] = ' ';
	}
	memcpy(s + a, word, b + 1);
	*field = s;
}

bool pkg_is_aur(const struct pkg *p) {
	return p->repo && !strcmp(p->repo, "aur");
}

const char *pkg_title(const struct pkg *p) {
	return p->app_name && *p->app_name ? p->app_name : p->name;
}

const char *pkg_blurb(const struct pkg *p) {
	if (p->summary && *p->summary) {
		return p->summary;
	}
	return p->desc ? p->desc : "";
}
