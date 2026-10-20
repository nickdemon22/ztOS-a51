// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE
#include "json.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "util.h"

struct parser {
	const char *p, *end;
	int depth;
};

static void skip_ws(struct parser *ps) {
	while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) {
		ps->p++;
	}
}

static struct json *node(enum json_type t) {
	struct json *j = calloc(1, sizeof(*j));
	if (!j) {
		abort();
	}
	j->type = t;
	return j;
}

static void put_utf8(char **o, uint32_t c) {
	char *s = *o;
	if (c < 0x80) {
		*s++ = c;
	} else if (c < 0x800) {
		*s++ = 0xc0 | (c >> 6);
		*s++ = 0x80 | (c & 0x3f);
	} else if (c < 0x10000) {
		*s++ = 0xe0 | (c >> 12);
		*s++ = 0x80 | ((c >> 6) & 0x3f);
		*s++ = 0x80 | (c & 0x3f);
	} else {
		*s++ = 0xf0 | (c >> 18);
		*s++ = 0x80 | ((c >> 12) & 0x3f);
		*s++ = 0x80 | ((c >> 6) & 0x3f);
		*s++ = 0x80 | (c & 0x3f);
	}
	*o = s;
}

static int hex4(const char *p, const char *end, uint32_t *out) {
	if (end - p < 4) {
		return -1;
	}
	uint32_t v = 0;
	for (int i = 0; i < 4; i++) {
		char c = p[i];
		v <<= 4;
		if (c >= '0' && c <= '9') {
			v |= c - '0';
		} else if (c >= 'a' && c <= 'f') {
			v |= c - 'a' + 10;
		} else if (c >= 'A' && c <= 'F') {
			v |= c - 'A' + 10;
		} else {
			return -1;
		}
	}
	*out = v;
	return 0;
}

/* a string at ps->p (on its opening quote); NULL on bad input */
static char *parse_string(struct parser *ps) {
	ps->p++;
	const char *start = ps->p;
	size_t len = 0;
	while (ps->p < ps->end && *ps->p != '"') {
		if (*ps->p == '\\') {
			ps->p++;
		}
		ps->p++;
		len++;
	}
	if (ps->p >= ps->end) {
		return NULL;
	}
	/* escapes only ever shrink, so len + the worst \u growth is enough */
	char *out = malloc((ps->p - start) + 8);
	if (!out) {
		abort();
	}
	char *o = out;
	for (const char *s = start; s < ps->p; s++) {
		if (*s != '\\') {
			*o++ = *s;
			continue;
		}
		s++;
		switch (*s) {
		case 'n': *o++ = '\n'; break;
		case 't': *o++ = '\t'; break;
		case 'r': *o++ = '\r'; break;
		case 'b': *o++ = '\b'; break;
		case 'f': *o++ = '\f'; break;
		case 'u': {
			uint32_t c;
			if (hex4(s + 1, ps->p, &c) < 0) {
				free(out);
				return NULL;
			}
			s += 4;
			if (c >= 0xd800 && c < 0xdc00 && s + 6 < ps->p && s[1] == '\\' && s[2] == 'u') {
				uint32_t lo;
				if (hex4(s + 3, ps->p, &lo) == 0 && lo >= 0xdc00 && lo < 0xe000) {
					c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
					s += 6;
				}
			}
			put_utf8(&o, c);
			break;
		}
		default: *o++ = *s; break;	/* \" \\ \/ */
		}
	}
	*o = 0;
	ps->p++;	/* the closing quote */
	return out;
}

static struct json *parse_value(struct parser *ps);

static struct json *parse_container(struct parser *ps, bool object) {
	struct json *j = node(object ? J_OBJ : J_ARR);
	int cap = 0;
	ps->p++;
	if (++ps->depth > 64) {
		goto bad;
	}
	skip_ws(ps);
	if (ps->p < ps->end && *ps->p == (object ? '}' : ']')) {
		ps->p++;
		ps->depth--;
		return j;
	}
	for (;;) {
		char *key = NULL;
		skip_ws(ps);
		if (object) {
			if (ps->p >= ps->end || *ps->p != '"' || !(key = parse_string(ps))) {
				goto bad;
			}
			skip_ws(ps);
			if (ps->p >= ps->end || *ps->p != ':') {
				free(key);
				goto bad;
			}
			ps->p++;
		}
		struct json *v = parse_value(ps);
		if (!v) {
			free(key);
			goto bad;
		}
		if (j->n == cap) {
			cap = cap ? cap * 2 : 8;
			j->items = realloc(j->items, cap * sizeof(*j->items));
			if (object) {
				j->keys = realloc(j->keys, cap * sizeof(*j->keys));
			}
			if (!j->items || (object && !j->keys)) {
				abort();
			}
		}
		if (object) {
			j->keys[j->n] = key;
		}
		j->items[j->n++] = v;
		skip_ws(ps);
		if (ps->p < ps->end && *ps->p == ',') {
			ps->p++;
			continue;
		}
		if (ps->p < ps->end && *ps->p == (object ? '}' : ']')) {
			ps->p++;
			ps->depth--;
			return j;
		}
		goto bad;
	}
bad:
	json_free(j);
	return NULL;
}

static struct json *parse_value(struct parser *ps) {
	skip_ws(ps);
	if (ps->p >= ps->end) {
		return NULL;
	}
	char c = *ps->p;
	if (c == '{' || c == '[') {
		return parse_container(ps, c == '{');
	}
	if (c == '"') {
		char *s = parse_string(ps);
		if (!s) {
			return NULL;
		}
		struct json *j = node(J_STR);
		j->str = s;
		return j;
	}
	if (ps->end - ps->p >= 4 && !strncmp(ps->p, "true", 4)) {
		ps->p += 4;
		struct json *j = node(J_BOOL);
		j->num = 1;
		return j;
	}
	if (ps->end - ps->p >= 5 && !strncmp(ps->p, "false", 5)) {
		ps->p += 5;
		return node(J_BOOL);
	}
	if (ps->end - ps->p >= 4 && !strncmp(ps->p, "null", 4)) {
		ps->p += 4;
		return node(J_NULL);
	}
	if (c == '-' || (c >= '0' && c <= '9')) {
		char buf[64];
		size_t n = 0;
		while (ps->p < ps->end && n < sizeof(buf) - 1 && strchr("+-0123456789.eE", *ps->p)) {
			buf[n++] = *ps->p++;
		}
		buf[n] = 0;
		struct json *j = node(J_NUM);
		j->num = strtod(buf, NULL);
		return j;
	}
	return NULL;
}

struct json *json_parse(const char *text, size_t len) {
	struct parser ps = { text, text + len, 0 };
	struct json *j = parse_value(&ps);
	skip_ws(&ps);
	if (j && ps.p != ps.end) {
		json_free(j);
		return NULL;
	}
	return j;
}

void json_free(struct json *j) {
	if (!j) {
		return;
	}
	for (int i = 0; i < j->n; i++) {
		json_free(j->items[i]);
		if (j->keys) {
			free(j->keys[i]);
		}
	}
	free(j->items);
	free(j->keys);
	free(j->str);
	free(j);
}

struct json *json_get(const struct json *obj, const char *key) {
	if (!obj || obj->type != J_OBJ) {
		return NULL;
	}
	for (int i = 0; i < obj->n; i++) {
		if (!strcmp(obj->keys[i], key)) {
			return obj->items[i];
		}
	}
	return NULL;
}

const char *json_str(const struct json *obj, const char *key) {
	struct json *v = json_get(obj, key);
	return v && v->type == J_STR ? v->str : NULL;
}

double json_num(const struct json *obj, const char *key) {
	struct json *v = json_get(obj, key);
	return v && (v->type == J_NUM || v->type == J_BOOL) ? v->num : 0;
}

/* Walk the top-level array a character at a time, cutting out each element
 * (counting brackets outside strings) and parsing just that. */
long json_each_gz(const char *path, void (*each)(const struct json *item, void *data), void *data) {
	gzFile gz = gzopen(path, "rb");
	if (!gz) {
		return -1;
	}
	gzbuffer(gz, 1 << 16);
	size_t cap = 4096, n = 0;
	char *elem = malloc(cap);
	if (!elem) {
		abort();
	}
	int depth = 0;		/* 1 inside the outer array */
	bool in_str = false, esc = false;
	long count = 0;
	char chunk[1 << 16];
	int got;
	while ((got = gzread(gz, chunk, sizeof(chunk))) > 0) {
		for (int i = 0; i < got; i++) {
			char c = chunk[i];
			if (depth >= 2) {
				if (n + 1 >= cap) {
					cap *= 2;
					elem = realloc(elem, cap);
					if (!elem) {
						abort();
					}
				}
				elem[n++] = c;
			}
			if (in_str) {
				if (esc) {
					esc = false;
				} else if (c == '\\') {
					esc = true;
				} else if (c == '"') {
					in_str = false;
				}
				continue;
			}
			if (c == '"') {
				in_str = true;
			} else if (c == '[' || c == '{') {
				if (++depth == 2) {
					n = 0;
					elem[n++] = c;
				}
			} else if (c == ']' || c == '}') {
				if (depth-- == 2) {
					struct json *j = json_parse(elem, n);
					if (j) {
						each(j, data);
						json_free(j);
						count++;
					}
					n = 0;
				}
			}
		}
	}
	gzclose(gz);
	free(elem);
	return count;
}
