// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE
#include "repo.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

/* ---- pacman.conf ------------------------------------------------------------ */

static void conf_read(struct repo_index *ri, const char *path, int depth) {
	if (depth > 4) {
		return;
	}
	char *text = read_all(path, NULL);
	if (!text) {
		return;
	}
	char section[128] = "";
	char *save = NULL;
	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *hash = strchr(line, '#');
		if (hash) {
			*hash = 0;
		}
		char *l = trim(line);
		if (!*l) {
			continue;
		}
		if (*l == '[') {
			char *end = strchr(l, ']');
			if (end) {
				*end = 0;
				snprintf(section, sizeof(section), "%s", l + 1);
				if (strcmp(section, "options") != 0 && depth == 0) {
					vec_push(&ri->repos, xstrdup(section));
				}
			}
			continue;
		}
		char *eq = strchr(l, '=');
		if (!eq) {
			continue;
		}
		*eq = 0;
		char *key = trim(l), *value = trim(eq + 1);
		if (!strcmp(section, "options") && !strcmp(key, "DBPath")) {
			set_str(&ri->dbpath, value);
		} else if (!strcmp(key, "Include") && !strcmp(section, "options")) {
			conf_read(ri, value, depth + 1);
		}
	}
	free(text);
}

/* ---- desc records ------------------------------------------------------------ */

size_t repo_parse_desc(const char *text, struct pkg *p) {
	const char *s = text;
	char key[32] = "";
	while (*s) {
		const char *eol = strchr(s, '\n');
		size_t n = eol ? (size_t)(eol - s) : strlen(s);
		const char *next = eol ? eol + 1 : s + n;
		if (n >= 2 && s[0] == '%' && s[n - 1] == '%') {
			/* a new record starts at its first key again */
			if (!strncmp(s, "%FILENAME%", n) && p->name) {
				return s - text;
			}
			if (!strncmp(s, "%NAME%", n) && p->name) {
				return s - text;
			}
			snprintf(key, sizeof(key), "%.*s", (int)(n - 2), s + 1);
			s = next;
			continue;
		}
		if (n == 0) {
			key[0] = 0;
			s = next;
			continue;
		}
		char *value = xstrndup(s, n);
		if (!strcmp(key, "NAME")) {
			set_str(&p->name, value);
		} else if (!strcmp(key, "VERSION")) {
			set_str(&p->version, value);
		} else if (!strcmp(key, "DESC")) {
			set_str(&p->desc, value);
		} else if (!strcmp(key, "URL")) {
			set_str(&p->url, value);
		} else if (!strcmp(key, "ARCH")) {
			set_str(&p->arch, value);
		} else if (!strcmp(key, "PACKAGER")) {
			set_str(&p->packager, value);
		} else if (!strcmp(key, "CSIZE")) {
			p->csize = strtod(value, NULL);
		} else if (!strcmp(key, "ISIZE") || !strcmp(key, "SIZE")) {
			p->isize = strtod(value, NULL);
		} else if (!strcmp(key, "BUILDDATE")) {
			p->builddate = strtol(value, NULL, 10);
		} else if (!strcmp(key, "LICENSE")) {
			append_word(&p->license, value);
		} else if (!strcmp(key, "GROUPS")) {
			append_word(&p->groups, value);
		} else if (!strcmp(key, "DEPENDS")) {
			append_word(&p->depends, value);
		} else if (!strcmp(key, "MAKEDEPENDS")) {
			append_word(&p->makedepends, value);
		} else if (!strcmp(key, "REASON")) {
			p->explicit_install = strcmp(value, "1") != 0;	/* 1: a dependency */
		}
		free(value);
		s = next;
	}
	return s - text;
}

/* A sync database is a compressed tar of one folder per package with a "desc"
 * file in each. bsdtar (libarchive, which pacman itself uses) reads every
 * compression pacman can write and prints the desc files one after another. */
static void sync_read(struct repo_index *ri, const char *repo) {
	char db[4096];
	snprintf(db, sizeof(db), "%s/sync/%s.db", ri->dbpath, repo);
	char *argv[] = { "bsdtar", "-xOf", db, "--include", "*/desc", NULL };
	int st;
	char *text = run_capture(argv, &st);
	if (!text || st != 0) {
		/* no bsdtar: GNU tar reads the same (it finds the compression itself) */
		free(text);
		char *gnu[] = { "tar", "-xOf", db, "--wildcards", "*/desc", NULL };
		text = run_capture(gnu, &st);
		if (!text) {
			return;
		}
	}
	const char *s = text;
	while (*s) {
		struct pkg *p = pkg_new(NULL, repo);
		size_t used = repo_parse_desc(s, p);
		if (p->name) {
			vec_push(&ri->sync, p);
		} else {
			pkg_free(p);
		}
		if (!used) {
			break;
		}
		s += used;
	}
	free(text);
}

/* The .desktop file an installed package brings, from its file list. */
static void local_desktop(struct pkg *p, const char *dir) {
	char path[4096];
	snprintf(path, sizeof(path), "%s/files", dir);
	char *text = read_all(path, NULL);
	if (!text) {
		return;
	}
	char *save = NULL;
	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		size_t n = strlen(line);
		if (starts_with(line, "usr/share/applications/") && n > 8 && !strcmp(line + n - 8, ".desktop") &&
				!strchr(line + 23, '/')) {
			char full[4096];
			snprintf(full, sizeof(full), "/%s", line);
			set_str(&p->desktop, full);
			break;
		}
	}
	free(text);
}

static void local_read(struct repo_index *ri) {
	char dir[4096];
	snprintf(dir, sizeof(dir), "%s/local", ri->dbpath);
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char pdir[4400], path[4500];
		snprintf(pdir, sizeof(pdir), "%s/%s", dir, e->d_name);
		snprintf(path, sizeof(path), "%s/desc", pdir);
		char *text = read_all(path, NULL);
		if (!text) {
			continue;
		}
		struct pkg *p = pkg_new(NULL, "local");
		p->explicit_install = true;
		repo_parse_desc(text, p);
		free(text);
		if (!p->name) {
			pkg_free(p);
			continue;
		}
		p->installed = true;
		set_str(&p->installed_version, p->version);
		local_desktop(p, pdir);
		vec_push(&ri->local, p);
	}
	closedir(d);
}

static int by_name(const void *a, const void *b) {
	const struct pkg *x = *(struct pkg *const *)a, *y = *(struct pkg *const *)b;
	return strcmp(x->name, y->name);
}

static const struct vec *sort_repos;

static int repo_rank(const char *repo) {
	for (int i = 0; i < sort_repos->n; i++) {
		if (!strcmp(sort_repos->v[i], repo)) {
			return i;
		}
	}
	return sort_repos->n;
}

static int by_name_then_repo(const void *a, const void *b) {
	int c = by_name(a, b);
	if (c) {
		return c;
	}
	const struct pkg *x = *(struct pkg *const *)a, *y = *(struct pkg *const *)b;
	return repo_rank(x->repo) - repo_rank(y->repo);
}

static struct pkg *find(const struct vec *v, const char *name) {
	struct pkg key = { .name = (char *)name }, *kp = &key;
	struct pkg **hit = bsearch(&kp, v->v, v->n, sizeof(void *), by_name);
	return hit ? *hit : NULL;
}

bool repo_load(struct repo_index *ri, const char *conf) {
	memset(ri, 0, sizeof(*ri));
	conf_read(ri, conf, 0);
	if (!ri->dbpath) {
		ri->dbpath = xstrdup("/var/lib/pacman");
	}
	size_t n = strlen(ri->dbpath);
	if (n > 1 && ri->dbpath[n - 1] == '/') {
		ri->dbpath[n - 1] = 0;
	}
	for (int i = 0; i < ri->repos.n; i++) {
		sync_read(ri, ri->repos.v[i]);
	}
	local_read(ri);
	/* A package in two repos: the first repo in pacman.conf wins, as in pacman.
	 * Sort by name, then by the repo's place in pacman.conf, and keep the first. */
	qsort(ri->local.v, ri->local.n, sizeof(void *), by_name);
	sort_repos = &ri->repos;
	qsort(ri->sync.v, ri->sync.n, sizeof(void *), by_name_then_repo);
	int w = 0;
	for (int i = 0; i < ri->sync.n; i++) {
		struct pkg *p = ri->sync.v[i];
		if (w > 0 && !strcmp(((struct pkg *)ri->sync.v[w - 1])->name, p->name)) {
			pkg_free(p);
			continue;
		}
		ri->sync.v[w++] = p;
	}
	ri->sync.n = w;
	/* what is installed, on the sync records too */
	for (int i = 0; i < ri->sync.n; i++) {
		struct pkg *p = ri->sync.v[i];
		struct pkg *l = find(&ri->local, p->name);
		if (l) {
			p->installed = true;
			p->explicit_install = l->explicit_install;
			set_str(&p->installed_version, l->version);
			set_str(&p->desktop, l->desktop);
		}
	}
	return ri->repos.n > 0;
}

void repo_free(struct repo_index *ri) {
	free(ri->dbpath);
	vec_free(&ri->repos, free);
	vec_free(&ri->sync, pkg_free);
	vec_free(&ri->local, pkg_free);
}

struct pkg *repo_find_sync(const struct repo_index *ri, const char *name) {
	return find(&ri->sync, name);
}

struct pkg *repo_find_local(const struct repo_index *ri, const char *name) {
	return find(&ri->local, name);
}

void repo_foreign(const struct repo_index *ri, struct vec *out) {
	for (int i = 0; i < ri->local.n; i++) {
		struct pkg *l = ri->local.v[i];
		if (!find(&ri->sync, l->name)) {
			vec_push(out, l);
		}
	}
}
