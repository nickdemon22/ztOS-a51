// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void *xrealloc(void *p, size_t n) {
	void *q = realloc(p, n ? n : 1);
	if (!q) {
		fprintf(stderr, "ztstore: out of memory\n");
		abort();
	}
	return q;
}

void vec_push(struct vec *a, void *p) {
	if (a->n == a->cap) {
		a->cap = a->cap ? a->cap * 2 : 16;
		a->v = xrealloc(a->v, a->cap * sizeof(void *));
	}
	a->v[a->n++] = p;
}

void vec_free(struct vec *a, void (*free_item)(void *)) {
	for (int i = 0; free_item && i < a->n; i++) {
		free_item(a->v[i]);
	}
	free(a->v);
	a->v = NULL;
	a->n = a->cap = 0;
}

char *xstrdup(const char *s) {
	return s ? xstrndup(s, strlen(s)) : NULL;
}

char *xstrndup(const char *s, size_t n) {
	char *d = xrealloc(NULL, n + 1);
	memcpy(d, s, n);
	d[n] = 0;
	return d;
}

char *trim(char *s) {
	while (*s && isspace((unsigned char)*s)) {
		s++;
	}
	size_t n = strlen(s);
	while (n && isspace((unsigned char)s[n - 1])) {
		s[--n] = 0;
	}
	return s;
}

bool starts_with(const char *s, const char *prefix) {
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool contains_nocase(const char *haystack, const char *needle) {
	return haystack && needle && strcasestr(haystack, needle) != NULL;
}

char *read_all(const char *path, size_t *len) {
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return NULL;
	}
	size_t cap = 4096, n = 0;
	char *buf = xrealloc(NULL, cap);
	for (;;) {
		if (n + 1 >= cap) {
			cap *= 2;
			buf = xrealloc(buf, cap);
		}
		ssize_t r = read(fd, buf + n, cap - n - 1);
		if (r < 0 && errno == EINTR) {
			continue;
		}
		if (r <= 0) {
			break;
		}
		n += r;
	}
	close(fd);
	buf[n] = 0;
	if (len) {
		*len = n;
	}
	return buf;
}

bool write_atomic(const char *path, const char *data, size_t len) {
	char tmp[4096];
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) {
		return false;
	}
	bool ok = write(fd, data, len) == (ssize_t)len;
	ok = close(fd) == 0 && ok;
	return ok && rename(tmp, path) == 0;
}

bool mkdir_p(const char *path, mode_t mode) {
	char buf[4096];
	snprintf(buf, sizeof(buf), "%s", path);
	for (char *p = buf + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(buf, mode);
			*p = '/';
		}
	}
	return mkdir(buf, mode) == 0 || errno == EEXIST;
}

long file_mtime(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 ? (long)st.st_mtime : -1;
}

static void base_dir(char *out, size_t size, const char *xdg, const char *fallback, const char *name) {
	const char *env = getenv(xdg);
	const char *home = getenv("HOME");
	char dir[4096];
	if (env && *env) {
		snprintf(dir, sizeof(dir), "%s/ztstore", env);
	} else {
		snprintf(dir, sizeof(dir), "%s/%s/ztstore", home ? home : "/tmp", fallback);
	}
	mkdir_p(dir, 0700);
	snprintf(out, size, "%s/%s", dir, name);
}

void cache_path(char *out, size_t size, const char *name) {
	base_dir(out, size, "XDG_CACHE_HOME", ".cache", name);
}

void config_path(char *out, size_t size, const char *name) {
	base_dir(out, size, "XDG_CONFIG_HOME", ".config", name);
}

char *run_capture(char *const argv[], int *status) {
	int fds[2];
	if (status) {
		*status = -1;
	}
	if (pipe2(fds, O_CLOEXEC) < 0) {
		return NULL;
	}
	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return NULL;
	}
	if (pid == 0) {
		dup2(fds[1], 1);
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 2);
		}
		setenv("LC_ALL", "C", 1);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(fds[1]);
	size_t cap = 8192, n = 0;
	char *buf = xrealloc(NULL, cap);
	for (;;) {
		if (n + 1 >= cap) {
			cap *= 2;
			buf = xrealloc(buf, cap);
		}
		ssize_t r = read(fds[0], buf + n, cap - n - 1);
		if (r < 0 && errno == EINTR) {
			continue;
		}
		if (r <= 0) {
			break;
		}
		n += r;
	}
	close(fds[0]);
	buf[n] = 0;
	int st;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
	}
	if (status) {
		*status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
	}
	return buf;
}

bool fetch_to_file(const char *url, const char *path, long max_age) {
	long m = file_mtime(path);
	if (m >= 0 && time(NULL) - m < max_age) {
		return true;
	}
	char tmp[4096];
	snprintf(tmp, sizeof(tmp), "%s.part", path);
	char *argv[] = { "curl", "-fsSL", "--compressed", "--max-time", "120", "-o", tmp, (char *)url, NULL };
	int st;
	free(run_capture(argv, &st));
	if (st != 0) {
		unlink(tmp);
		return m >= 0;	/* an old copy is better than none */
	}
	return rename(tmp, path) == 0;
}

char *fetch(const char *url) {
	char *argv[] = { "curl", "-fsSL", "--compressed", "--max-time", "30", (char *)url, NULL };
	int st;
	char *body = run_capture(argv, &st);
	if (st != 0) {
		free(body);
		return NULL;
	}
	return body;
}

void url_escape(char *out, size_t size, const char *s) {
	static const char hex[] = "0123456789ABCDEF";
	size_t o = 0;
	for (; *s && o + 4 < size; s++) {
		unsigned char c = *s;
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out[o++] = c;
		} else {
			out[o++] = '%';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 15];
		}
	}
	out[o] = 0;
}

bool shell_ctl(const char *line) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime) {
		return false;
	}
	char path[4096];
	snprintf(path, sizeof(path), "%s/ztmetro.ctl", runtime);
	int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	char buf[1024];
	int n = snprintf(buf, sizeof(buf), "%s\n", line);
	bool ok = n > 0 && n < (int)sizeof(buf) && write(fd, buf, n) == n;
	close(fd);
	return ok;
}

void human_size(char *out, size_t size, double bytes) {
	const char *units[] = { "bytes", "KB", "MB", "GB" };
	int u = 0;
	while (bytes >= 1024 && u < 3) {
		bytes /= 1024;
		u++;
	}
	if (u == 0) {
		snprintf(out, size, "%.0f %s", bytes, units[u]);
	} else {
		snprintf(out, size, bytes < 10 ? "%.1f %s" : "%.0f %s", bytes, units[u]);
	}
}

void human_age(char *out, size_t size, long when) {
	long d = (long)time(NULL) - when;
	if (when <= 0) {
		snprintf(out, size, "unknown");
	} else if (d < 3600) {
		snprintf(out, size, "within the hour");
	} else if (d < 86400) {
		snprintf(out, size, "%ld hour%s ago", d / 3600, d / 3600 == 1 ? "" : "s");
	} else if (d < 86400 * 60) {
		snprintf(out, size, "%ld day%s ago", d / 86400, d / 86400 == 1 ? "" : "s");
	} else if (d < 86400 * 730) {
		snprintf(out, size, "%ld months ago", d / (86400 * 30));
	} else {
		snprintf(out, size, "%ld years ago", d / (86400 * 365));
	}
}
