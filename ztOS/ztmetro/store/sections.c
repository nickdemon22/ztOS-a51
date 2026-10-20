// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE
#include "sections.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aur.h"
#include "json.h"

void sections_path(char *out, size_t size) {
	config_path(out, size, "sections");
	if (access(out, R_OK) != 0) {
		snprintf(out, size, "/etc/ztstore/sections");
	}
}

bool sections_read(struct vec *out, const char *path) {
	char *text = read_all(path, NULL);
	if (!text) {
		return false;
	}
	char *save = NULL;
	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *l = trim(line);
		if (!*l || *l == '#') {
			continue;
		}
		char *field[5] = { 0 };
		int n = 0;
		for (char *f = l; f && n < 5; n++) {
			char *bar = strchr(f, '|');
			if (bar) {
				*bar = 0;
			}
			field[n] = trim(f);
			f = bar ? bar + 1 : NULL;
		}
		if (n < 4 || !*field[0] || atoi(field[3]) <= 0) {
			continue;
		}
		struct section *s = calloc(1, sizeof(*s));
		s->title = xstrdup(field[0]);
		s->source = xstrdup(field[1]);
		s->sort = xstrdup(field[2]);
		s->count = atoi(field[3]);
		if (n == 5 && *field[4]) {
			s->caption = xstrdup(field[4]);
		} else {
			char cap[160];
			snprintf(cap, sizeof(cap), "%s · by %s", !strcmp(s->source, "aur") ? "AUR" : "your repos", s->sort);
			s->caption = xstrdup(cap);
		}
		vec_push(out, s);
	}
	free(text);
	return true;
}

void section_free(void *v) {
	struct section *s = v;
	free(s->title);
	free(s->source);
	free(s->sort);
	free(s->caption);
	free(s->note);
	vec_free(&s->items, pkg_free);
	free(s);
}

/* ---- the AUR's lists: one pass over the dump fills them all ------------------- */

static double aur_key(const struct pkg *p, const char *sort) {
	if (!strcmp(sort, "votes")) {
		return p->votes;
	}
	if (!strcmp(sort, "modified")) {
		return p->modified;
	}
	if (!strcmp(sort, "submitted")) {
		return p->submitted;
	}
	return p->popularity;
}

struct dump_ctx {
	struct vec *aur;	/* the sections fed from the dump */
};

/* keep each section's best `count`, best first */
static void offer(struct section *s, const struct pkg *p) {
	double k = aur_key(p, s->sort);
	struct vec *v = &s->items;
	if (v->n == s->count && k <= aur_key(v->v[v->n - 1], s->sort)) {
		return;
	}
	struct pkg *c = pkg_copy(p);
	if (v->n == s->count) {
		pkg_free(v->v[--v->n]);
	}
	vec_push(v, c);
	for (int i = v->n - 1; i > 0 && aur_key(v->v[i - 1], s->sort) < k; i--) {
		void *t = v->v[i];
		v->v[i] = v->v[i - 1];
		v->v[i - 1] = t;
	}
}

static void dump_each(const struct json *j, void *data) {
	struct dump_ctx *c = data;
	struct json *ood = json_get(j, "OutOfDate");
	if (ood && ood->type == J_NUM) {
		return;	/* flagged out of date: not one to recommend */
	}
	struct pkg *p = aur_pkg_from_json(j);
	if (!p) {
		return;
	}
	for (int i = 0; i < c->aur->n; i++) {
		offer(c->aur->v[i], p);
	}
	pkg_free(p);
}

/* ---- the repos' lists ----------------------------------------------------------- */

static bool is_app(const struct pkg *p) {
	return p->app_name != NULL;
}

static int by_builddate(const void *a, const void *b) {
	const struct pkg *x = *(struct pkg *const *)a, *y = *(struct pkg *const *)b;
	return (y->builddate > x->builddate) - (y->builddate < x->builddate);
}

static void fill_repo(struct section *s, const struct repo_index *ri, const struct json *popular) {
	if (!strcmp(s->sort, "popular")) {
		struct json *list = json_get(popular, "packagePopularities");
		if (!list || list->type != J_ARR) {
			s->note = xstrdup("could not reach pkgstats.archlinux.org");
			return;
		}
		for (int i = 0; i < list->n && s->items.n < s->count; i++) {
			const char *name = json_str(list->items[i], "name");
			struct pkg *p = name ? repo_find_sync(ri, name) : NULL;
			if (p && is_app(p)) {
				vec_push(&s->items, pkg_copy(p));
			}
		}
	} else {	/* builddate */
		struct vec apps = { 0 };
		for (int i = 0; i < ri->sync.n; i++) {
			if (is_app(ri->sync.v[i])) {
				vec_push(&apps, ri->sync.v[i]);
			}
		}
		qsort(apps.v, apps.n, sizeof(void *), by_builddate);
		for (int i = 0; i < apps.n && i < s->count; i++) {
			vec_push(&s->items, pkg_copy(apps.v[i]));
		}
		vec_free(&apps, NULL);
	}
	if (!s->items.n && !s->note) {
		s->note = xstrdup("no app data on this phone: install archlinux-appstream-data");
	}
}

void sections_fill(struct vec *sections, const struct repo_index *ri, const char *aur_dump, const char *pkgstats) {
	struct vec aur = { 0 };
	struct json *popular = NULL;
	if (pkgstats) {
		size_t n;
		char *text = read_all(pkgstats, &n);
		if (text) {
			popular = json_parse(text, n);
			free(text);
		}
	}
	for (int i = 0; i < sections->n; i++) {
		struct section *s = sections->v[i];
		vec_free(&s->items, pkg_free);
		free(s->note);
		s->note = NULL;
		if (!strcmp(s->source, "aur")) {
			vec_push(&aur, s);
		} else if (!strcmp(s->source, "repo")) {
			fill_repo(s, ri, popular);
		} else {
			s->note = xstrdup("unknown source in the sections file");
		}
	}
	if (aur.n) {
		struct dump_ctx c = { &aur };
		if (!aur_dump || json_each_gz(aur_dump, dump_each, &c) < 0) {
			for (int i = 0; i < aur.n; i++) {
				struct section *s = aur.v[i];
				s->note = xstrdup("could not reach the AUR");
			}
		}
		/* mark what is installed */
		for (int i = 0; i < aur.n; i++) {
			struct section *s = aur.v[i];
			for (int k = 0; k < s->items.n; k++) {
				struct pkg *p = s->items.v[k];
				struct pkg *l = repo_find_local(ri, p->name);
				if (l) {
					p->installed = true;
					set_str(&p->installed_version, l->version);
					set_str(&p->icon, l->icon);
					set_str(&p->desktop, l->desktop);
				}
			}
		}
	}
	json_free(popular);
	vec_free(&aur, NULL);
}

/* ---- categories and groups ----------------------------------------------------- */

void facet_free(void *v) {
	struct facet *f = v;
	free(f->key);
	free(f->label);
	free(f);
}

/* "AudioVideo" -> "audio video" */
static char *label_of(const char *key) {
	size_t n = strlen(key);
	char *out = malloc(n * 2 + 1), *o = out;
	for (size_t i = 0; i < n; i++) {
		if (i && isupper((unsigned char)key[i]) && islower((unsigned char)key[i - 1])) {
			*o++ = ' ';
		}
		*o++ = tolower((unsigned char)key[i]);
	}
	*o = 0;
	return out;
}

static void count_word(struct vec *out, const char *word, bool make_label) {
	for (int i = 0; i < out->n; i++) {
		struct facet *f = out->v[i];
		if (!strcmp(f->key, word)) {
			f->count++;
			return;
		}
	}
	struct facet *f = calloc(1, sizeof(*f));
	f->key = xstrdup(word);
	f->label = make_label ? label_of(word) : xstrdup(word);
	f->count = 1;
	vec_push(out, f);
}

static int by_count(const void *a, const void *b) {
	const struct facet *x = *(struct facet *const *)a, *y = *(struct facet *const *)b;
	if (x->count != y->count) {
		return y->count - x->count;
	}
	return strcmp(x->label, y->label);
}

static void facets(const struct repo_index *ri, struct vec *out, int max, bool group) {
	for (int i = 0; i < ri->sync.n; i++) {
		struct pkg *p = ri->sync.v[i];
		const char *words = group ? p->groups : p->categories;
		if (!words || (!group && !is_app(p))) {
			continue;
		}
		char *copy = xstrdup(words), *save = NULL;
		for (char *w = strtok_r(copy, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
			count_word(out, w, !group);
		}
		free(copy);
	}
	qsort(out->v, out->n, sizeof(void *), by_count);
	while (out->n > max) {
		facet_free(out->v[--out->n]);
	}
}

void facets_categories(const struct repo_index *ri, struct vec *out, int max) {
	facets(ri, out, max, false);
}

void facets_groups(const struct repo_index *ri, struct vec *out, int max) {
	facets(ri, out, max, true);
}

static bool has_word(const char *words, const char *w) {
	size_t n = strlen(w);
	for (const char *p = words; p && (p = strstr(p, w)); p += n) {
		if ((p == words || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) {
			return true;
		}
	}
	return false;
}

void facet_members(const struct repo_index *ri, bool group, const char *key, struct vec *out) {
	for (int i = 0; i < ri->sync.n; i++) {
		struct pkg *p = ri->sync.v[i];
		if (has_word(group ? p->groups : p->categories, key) && (group || is_app(p))) {
			vec_push(out, pkg_copy(p));
		}
	}
}
