// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE
#include "aur.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "json.h"
#include "util.h"

static void join_array(char **field, const struct json *arr) {
	if (!arr || arr->type != J_ARR) {
		return;
	}
	for (int i = 0; i < arr->n; i++) {
		if (arr->items[i]->type == J_STR) {
			append_word(field, arr->items[i]->str);
		}
	}
}

struct pkg *aur_pkg_from_json(const struct json *j) {
	const char *name = json_str(j, "Name");
	if (!name) {
		return NULL;
	}
	struct pkg *p = pkg_new(name, "aur");
	set_str(&p->version, json_str(j, "Version"));
	set_str(&p->desc, json_str(j, "Description"));
	set_str(&p->url, json_str(j, "URL"));
	set_str(&p->packager, json_str(j, "Maintainer"));
	p->votes = json_num(j, "NumVotes");
	p->popularity = json_num(j, "Popularity");
	p->submitted = (long)json_num(j, "FirstSubmitted");
	p->modified = (long)json_num(j, "LastModified");
	join_array(&p->depends, json_get(j, "Depends"));
	join_array(&p->makedepends, json_get(j, "MakeDepends"));
	join_array(&p->license, json_get(j, "License"));
	return p;
}

static bool rpc(const char *url, struct vec *out) {
	char *body = fetch(url);
	if (!body) {
		return false;
	}
	struct json *j = json_parse(body, strlen(body));
	free(body);
	if (!j) {
		return false;
	}
	bool ok = false;
	const char *type = json_str(j, "type");
	struct json *results = json_get(j, "results");
	if (type && strcmp(type, "error") && results && results->type == J_ARR) {
		ok = true;
		for (int i = 0; i < results->n; i++) {
			struct pkg *p = aur_pkg_from_json(results->items[i]);
			if (p) {
				vec_push(out, p);
			}
		}
	}
	json_free(j);
	return ok;
}

bool aur_search(const char *term, struct vec *out) {
	if (strlen(term) < 2) {
		return true;
	}
	char esc[512], url[1024];
	url_escape(esc, sizeof(esc), term);
	snprintf(url, sizeof(url), AUR_BASE "/rpc/v5/search/%s?by=name-desc", esc);
	return rpc(url, out);
}

bool aur_info(const char *const *names, int n, struct vec *out) {
	/* in batches, so the address never grows too long */
	bool ok = true;
	for (int start = 0; start < n; start += 100) {
		size_t cap = 64 + 600 * 100;
		char *url = malloc(cap);
		size_t len = snprintf(url, cap, AUR_BASE "/rpc/v5/info?");
		for (int i = start; i < n && i < start + 100; i++) {
			char esc[512];
			url_escape(esc, sizeof(esc), names[i]);
			len += snprintf(url + len, cap - len, "%sarg[]=%s", i > start ? "&" : "", esc);
		}
		ok = rpc(url, out) && ok;
		free(url);
	}
	return ok;
}

bool aur_dump(char *path, size_t size) {
	cache_path(path, size, "aur-packages-meta-v1.json.gz");
	/* curl is not asked to decompress: the file stays gzipped, as served */
	long m = file_mtime(path);
	if (m >= 0 && time(NULL) - m < 86400) {
		return true;
	}
	char tmp[4200];
	snprintf(tmp, sizeof(tmp), "%s.part", path);
	char *argv[] = { "curl", "-fsSL", "--max-time", "300", "-o", tmp, AUR_BASE "/packages-meta-v1.json.gz", NULL };
	int st;
	free(run_capture(argv, &st));
	if (st == 0 && rename(tmp, path) == 0) {
		return true;
	}
	remove(tmp);
	return m >= 0;
}

/* ---- versions, as pacman orders them ----------------------------------------- */

/* rpmvercmp: compare two version strings segment by segment (runs of digits
 * against runs of digits, letters against letters). */
static int rpmvercmp(const char *a, const char *b) {
	if (!strcmp(a, b)) {
		return 0;
	}
	char *s1 = xstrdup(a), *s2 = xstrdup(b);
	char *one = s1, *two = s2, *ptr1 = s1, *ptr2 = s2;
	int ret = 0;
	bool isnum;
	while (*one && *two) {
		while (*one && !isalnum((unsigned char)*one)) {
			one++;
		}
		while (*two && !isalnum((unsigned char)*two)) {
			two++;
		}
		if (!*one || !*two) {
			break;
		}
		/* a different amount of separators: the one with more is newer */
		if ((one - ptr1) != (two - ptr2)) {
			ret = (one - ptr1) < (two - ptr2) ? -1 : 1;
			goto done;
		}
		ptr1 = one;
		ptr2 = two;
		if (isdigit((unsigned char)*ptr1)) {
			while (*ptr1 && isdigit((unsigned char)*ptr1)) {
				ptr1++;
			}
			while (*ptr2 && isdigit((unsigned char)*ptr2)) {
				ptr2++;
			}
			isnum = true;
		} else {
			while (*ptr1 && isalpha((unsigned char)*ptr1)) {
				ptr1++;
			}
			while (*ptr2 && isalpha((unsigned char)*ptr2)) {
				ptr2++;
			}
			isnum = false;
		}
		char c1 = *ptr1, c2 = *ptr2;
		*ptr1 = 0;
		*ptr2 = 0;
		if (one == ptr1) {
			ret = -1;	/* cannot happen: the first is a valid segment */
			goto done;
		}
		if (two == ptr2) {
			ret = isnum ? 1 : -1;	/* numbers are newer than letters */
			goto done;
		}
		if (isnum) {
			while (*one == '0') {
				one++;
			}
			while (*two == '0') {
				two++;
			}
			size_t l1 = strlen(one), l2 = strlen(two);
			if (l1 != l2) {
				ret = l1 > l2 ? 1 : -1;
				goto done;
			}
		}
		int c = strcmp(one, two);
		if (c) {
			ret = c < 0 ? -1 : 1;
			goto done;
		}
		*ptr1 = c1;
		one = ptr1;
		*ptr2 = c2;
		two = ptr2;
	}
	if (!*one && !*two) {
		ret = 0;
	} else if ((!*one && !isalpha((unsigned char)*two)) || isalpha((unsigned char)*one)) {
		/* "1.0" < "1.0.1", but "1.0a" < "1.0" (a letter after is a pre-release) */
		ret = -1;
	} else {
		ret = 1;
	}
done:
	free(s1);
	free(s2);
	return ret;
}

/* epoch:version-release */
static void split_evr(char *evr, const char **e, const char **v, const char **r) {
	char *s = evr;
	while (*s && isdigit((unsigned char)*s)) {
		s++;
	}
	if (*s == ':') {
		*e = evr;
		*s++ = 0;
		*v = s;
		if (!**e) {
			*e = "0";
		}
	} else {
		*e = "0";
		*v = evr;
	}
	char *dash = strrchr(*v, '-');
	if (dash) {
		*dash = 0;
		*r = dash + 1;
	} else {
		*r = NULL;
	}
}

int vercmp(const char *a, const char *b) {
	if (!a || !b) {
		return !a ? (b ? -1 : 0) : 1;
	}
	if (!strcmp(a, b)) {
		return 0;
	}
	char *x = xstrdup(a), *y = xstrdup(b);
	const char *e1, *v1, *r1, *e2, *v2, *r2;
	split_evr(x, &e1, &v1, &r1);
	split_evr(y, &e2, &v2, &r2);
	int ret = rpmvercmp(e1, e2);
	if (!ret) {
		ret = rpmvercmp(v1, v2);
		if (!ret && r1 && r2) {
			ret = rpmvercmp(r1, r2);
		}
	}
	free(x);
	free(y);
	return ret;
}
