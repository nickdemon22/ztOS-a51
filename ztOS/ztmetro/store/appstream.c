// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE
#include "appstream.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

const char *const APPSTREAM_ROOTS[] = { "/usr/share/swcatalog", "/usr/share/app-info", NULL };

/* ---- the XML, as much of it as is needed ------------------------------------ */

static char *unescape(const char *s, size_t n) {
	char *out = xstrndup(s, n), *o = out;
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '&') {
			static const struct { const char *e, c; } ents[] = {
				{ "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' },
			};
			bool done = false;
			for (size_t k = 0; k < 5; k++) {
				size_t l = strlen(ents[k].e);
				if (i + l <= n && !strncmp(s + i, ents[k].e, l)) {
					*o++ = ents[k].c;
					i += l - 1;
					done = true;
					break;
				}
			}
			if (done) {
				continue;
			}
		}
		/* collapse runs of white space, as the text is shown on one line */
		if ((s[i] == '\n' || s[i] == '\t' || s[i] == ' ') && (o == out || o[-1] == ' ')) {
			continue;
		}
		*o++ = (s[i] == '\n' || s[i] == '\t') ? ' ' : s[i];
	}
	while (o > out && o[-1] == ' ') {
		o--;
	}
	*o = 0;
	return out;
}

/* The value of attribute `name` inside the tag text [tag, tag_end). */
static bool attr(const char *tag, const char *tag_end, const char *name, char *out, size_t size) {
	size_t n = strlen(name);
	for (const char *p = tag; p + n + 2 < tag_end; p++) {
		if ((p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n') && !strncmp(p, name, n) && p[n] == '=' &&
				(p[n + 1] == '"' || p[n + 1] == '\'')) {
			char q = p[n + 1];
			const char *v = p + n + 2, *e = memchr(v, q, tag_end - v);
			if (!e) {
				return false;
			}
			snprintf(out, size, "%.*s", (int)(e - v), v);
			return true;
		}
	}
	return false;
}

/* Find the next element called `name` in [p, end): its opening tag and text. */
static const char *element(const char *p, const char *end, const char *name, const char **tag_end,
		const char **text, const char **text_end) {
	size_t n = strlen(name);
	while ((p = memmem(p, end - p, "<", 1)) && p < end) {
		if (!strncmp(p + 1, name, n) && (p[n + 1] == '>' || p[n + 1] == ' ' || p[n + 1] == '/')) {
			const char *te = memchr(p, '>', end - p);
			if (!te) {
				return NULL;
			}
			*tag_end = te;
			*text = te + 1;
			if (te[-1] == '/') {	/* <x/> */
				*text_end = te + 1;
				return p;
			}
			char close[64];
			snprintf(close, sizeof(close), "</%s>", name);
			const char *c = memmem(te, end - te, close, strlen(close));
			*text_end = c ? c : end;
			return p;
		}
		p++;
	}
	return NULL;
}

void appstream_parse(const char *xml, void (*each)(const struct as_app *app, const char *origin, void *data),
		void *data) {
	const char *end = xml + strlen(xml);
	char origin[128] = "";
	const char *tag_end, *text, *text_end;
	const char *top = element(xml, end, "components", &tag_end, &text, &text_end);
	if (top) {
		attr(top + 1, tag_end, "origin", origin, sizeof(origin));
	}
	const char *p = xml;
	const char *comp;
	while ((comp = element(p, end, "component", &tag_end, &text, &text_end))) {
		const char *cs = text, *ce = text_end;
		p = text_end;
		char type[64] = "";
		attr(comp + 1, tag_end, "type", type, sizeof(type));
		/* apps only: not fonts, codecs, add-ons or libraries */
		if (strcmp(type, "desktop-application") && strcmp(type, "desktop") && strcmp(type, "console-application")) {
			continue;
		}
		struct as_app app = { 0 };
		const char *te, *t, *tend, *q;
		if ((q = element(cs, ce, "pkgname", &te, &t, &tend))) {
			app.pkgname = unescape(t, tend - t);
		}
		/* the first name and summary without a language: the untranslated ones */
		for (q = cs; (q = element(q, ce, "name", &te, &t, &tend)); q = tend) {
			char lang[16];
			if (!attr(q + 1, te, "xml:lang", lang, sizeof(lang))) {
				app.name = unescape(t, tend - t);
				break;
			}
		}
		for (q = cs; (q = element(q, ce, "summary", &te, &t, &tend)); q = tend) {
			char lang[16];
			if (!attr(q + 1, te, "xml:lang", lang, sizeof(lang))) {
				app.summary = unescape(t, tend - t);
				break;
			}
		}
		/* the largest cached icon */
		for (q = cs; (q = element(q, ce, "icon", &te, &t, &tend)); q = tend) {
			char kind[16] = "", width[16] = "";
			attr(q + 1, te, "type", kind, sizeof(kind));
			if (strcmp(kind, "cached")) {
				continue;
			}
			int w = attr(q + 1, te, "width", width, sizeof(width)) ? atoi(width) : 64;
			if (w > app.icon_size) {
				free(app.icon_file);
				app.icon_file = unescape(t, tend - t);
				app.icon_size = w;
			}
		}
		const char *cats_e;
		if ((q = element(cs, ce, "categories", &te, &t, &cats_e))) {
			const char *c = t;
			while ((q = element(c, cats_e, "category", &te, &t, &tend))) {
				char *one = unescape(t, tend - t);
				append_word(&app.categories, one);
				free(one);
				c = tend;
			}
		}
		if (app.pkgname) {
			each(&app, origin, data);
		}
		free(app.pkgname);
		free(app.name);
		free(app.summary);
		free(app.icon_file);
		free(app.categories);
	}
}

/* ---- putting it on the packages ---------------------------------------------- */

struct apply_ctx {
	struct repo_index *ri;
	const char *root;
	int found;
};

static void icon_path(const char *root, const char *origin, const struct as_app *a, char *out, size_t size) {
	out[0] = 0;
	if (!a->icon_file) {
		return;
	}
	int sizes[] = { a->icon_size, 128, 64, 48 };
	for (int i = 0; i < 4; i++) {
		char path[4096];
		snprintf(path, sizeof(path), "%s/icons/%s/%dx%d/%s", root, origin, sizes[i], sizes[i], a->icon_file);
		if (access(path, R_OK) == 0) {
			snprintf(out, size, "%s", path);
			return;
		}
	}
}

static void put(struct pkg *p, const struct as_app *a, const char *icon) {
	if (!p) {
		return;
	}
	if (a->name) {
		set_str(&p->app_name, a->name);
	}
	if (a->summary) {
		set_str(&p->summary, a->summary);
	}
	if (icon[0]) {
		set_str(&p->icon, icon);
	}
	if (a->categories && !p->categories) {
		set_str(&p->categories, a->categories);
	}
}

static void apply_one(const struct as_app *a, const char *origin, void *data) {
	struct apply_ctx *c = data;
	char icon[4096];
	icon_path(c->root, origin, a, icon, sizeof(icon));
	put(repo_find_sync(c->ri, a->pkgname), a, icon);
	put(repo_find_local(c->ri, a->pkgname), a, icon);
	c->found++;
}

static char *gunzip_file(const char *path) {
	gzFile gz = gzopen(path, "rb");
	if (!gz) {
		return NULL;
	}
	size_t cap = 1 << 20, n = 0;
	char *buf = malloc(cap);
	int got;
	while (buf && (got = gzread(gz, buf + n, cap - n - 1)) > 0) {
		n += got;
		if (cap - n < 4096) {
			cap *= 2;
			buf = realloc(buf, cap);
		}
	}
	gzclose(gz);
	if (buf) {
		buf[n] = 0;
	}
	return buf;
}

static void desktop_fill(struct pkg *p) {
	if (!p->desktop) {
		return;
	}
	char *text = read_all(p->desktop, NULL);
	if (!text) {
		return;
	}
	bool in_entry = false;
	char *save = NULL;
	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (line[0] == '[') {
			in_entry = starts_with(line, "[Desktop Entry]");
			continue;
		}
		if (!in_entry) {
			continue;
		}
		if (starts_with(line, "Name=") && !p->app_name) {
			set_str(&p->app_name, trim(line + 5));
		} else if (starts_with(line, "Comment=") && !p->summary) {
			set_str(&p->summary, trim(line + 8));
		} else if (starts_with(line, "Icon=") && !p->icon) {
			char path[4096];
			icon_lookup(trim(line + 5), path, sizeof(path));
			if (path[0]) {
				set_str(&p->icon, path);
			}
		}
	}
	free(text);
}

int appstream_apply(struct repo_index *ri, const char *const *roots) {
	struct apply_ctx c = { ri, NULL, 0 };
	for (int r = 0; roots[r]; r++) {
		const char *subdirs[] = { "xml", "xmls" };
		for (int s = 0; s < 2; s++) {
			char dir[4096];
			snprintf(dir, sizeof(dir), "%s/%s", roots[r], subdirs[s]);
			DIR *d = opendir(dir);
			if (!d) {
				continue;
			}
			struct dirent *e;
			while ((e = readdir(d))) {
				size_t n = strlen(e->d_name);
				if (n < 7 || (strcmp(e->d_name + n - 7, ".xml.gz") && strcmp(e->d_name + n - 4, ".xml"))) {
					continue;
				}
				char path[4400];
				snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
				char *xml = gunzip_file(path);	/* (reads plain files too) */
				if (xml) {
					c.root = roots[r];
					appstream_parse(xml, apply_one, &c);
					free(xml);
				}
			}
			closedir(d);
		}
	}
	for (int i = 0; i < ri->local.n; i++) {
		desktop_fill(ri->local.v[i]);
	}
	/* the sync copies of installed apps get the same name and icon */
	for (int i = 0; i < ri->sync.n; i++) {
		struct pkg *p = ri->sync.v[i];
		struct pkg *l = p->installed ? repo_find_local(ri, p->name) : NULL;
		if (l) {
			if (!p->app_name && l->app_name) {
				set_str(&p->app_name, l->app_name);
			}
			if (!p->icon && l->icon) {
				set_str(&p->icon, l->icon);
			}
		}
	}
	return c.found;
}

/* ---- icons by name ------------------------------------------------------------ */

void icon_lookup(const char *name, char *out, size_t size) {
	out[0] = 0;
	if (!name || !*name) {
		return;
	}
	if (name[0] == '/') {
		if (access(name, R_OK) == 0) {
			snprintf(out, size, "%s", name);
		}
		return;
	}
	const char *bases[] = { "/usr/share/icons/hicolor", "/usr/local/share/icons/hicolor", NULL };
	const char *dirs[] = { "128x128", "96x96", "64x64", "256x256", "48x48", "scalable", NULL };
	for (int d = 0; dirs[d]; d++) {
		for (int b = 0; bases[b]; b++) {
			const char *exts[] = { "png", "svg", NULL };
			for (int x = 0; exts[x]; x++) {
				char path[4096];
				snprintf(path, sizeof(path), "%s/%s/apps/%s.%s", bases[b], dirs[d], name, exts[x]);
				if (access(path, R_OK) == 0) {
					snprintf(out, size, "%s", path);
					return;
				}
			}
		}
	}
	const char *exts[] = { "png", "svg", "xpm", NULL };
	for (int x = 0; exts[x]; x++) {
		char path[4096];
		snprintf(path, sizeof(path), "/usr/share/pixmaps/%s.%s", name, exts[x]);
		if (x < 2 && access(path, R_OK) == 0) {	/* (no xpm reader) */
			snprintf(out, size, "%s", path);
			return;
		}
	}
}
