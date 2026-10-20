// SPDX-License-Identifier: GPL-2.0-or-later
/* ztstore: small things every part of the App Store uses. */
#ifndef ZTSTORE_UTIL_H
#define ZTSTORE_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* a growable array of pointers */
struct vec {
	void **v;
	int n, cap;
};
void vec_push(struct vec *a, void *p);
void vec_free(struct vec *a, void (*free_item)(void *));

char *xstrdup(const char *s);		/* NULL stays NULL; aborts when memory runs out */
char *xstrndup(const char *s, size_t n);
char *trim(char *s);			/* in place: leading and trailing blanks */
bool starts_with(const char *s, const char *prefix);
bool contains_nocase(const char *haystack, const char *needle);

/* a file's whole contents, NUL-terminated (NULL if it cannot be read) */
char *read_all(const char *path, size_t *len);
bool write_atomic(const char *path, const char *data, size_t len);
bool mkdir_p(const char *path, mode_t mode);
long file_mtime(const char *path);	/* -1 if it is not there */

/* ~/.cache/ztstore/<name>, ~/.config/ztstore/<name> (folders made as needed) */
void cache_path(char *out, size_t size, const char *name);
void config_path(char *out, size_t size, const char *name);

/* Run argv (no shell), give back what it printed, NUL-terminated; NULL if it
 * could not start. *status gets its exit status (or -1). */
char *run_capture(char *const argv[], int *status);

/* Fetch a URL with curl into a file (its proxy settings are the user's own).
 * Only fetches again when the copy is older than max_age seconds. */
bool fetch_to_file(const char *url, const char *path, long max_age);
/* Fetch a URL with curl and give back the body, NUL-terminated. */
char *fetch(const char *url);
/* %-escape for a URL path segment or query value */
void url_escape(char *out, size_t size, const char *s);

/* one line to the shell's control pipe: "launch CMD", "apps", "notify A|B|C" */
bool shell_ctl(const char *line);

/* "12.3 MB" */
void human_size(char *out, size_t size, double bytes);
/* "3 days ago" from a Unix time */
void human_age(char *out, size_t size, long when);

#endif
