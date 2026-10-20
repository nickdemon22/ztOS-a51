// SPDX-License-Identifier: GPL-2.0-or-later
/* ztstore: a small JSON reader, enough for the AUR's and pkgstats' answers. */
#ifndef ZTSTORE_JSON_H
#define ZTSTORE_JSON_H

#include <stdbool.h>
#include <stddef.h>

enum json_type { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

struct json {
	enum json_type type;
	double num;		/* J_NUM, and J_BOOL as 0/1 */
	char *str;		/* J_STR */
	struct json **items;	/* J_ARR values, J_OBJ values */
	char **keys;		/* J_OBJ */
	int n;
};

struct json *json_parse(const char *text, size_t len);	/* NULL on bad input */
void json_free(struct json *j);
struct json *json_get(const struct json *obj, const char *key);
const char *json_str(const struct json *obj, const char *key);	/* NULL if not a string */
double json_num(const struct json *obj, const char *key);	/* 0 if not a number */

/* Read a (gzipped or plain) file holding one big array of objects and call
 * `each` with every element, parsed one at a time so the whole array never sits
 * in memory. Returns how many were read, or -1 if the file could not be opened. */
long json_each_gz(const char *path, void (*each)(const struct json *item, void *data), void *data);

#endif
