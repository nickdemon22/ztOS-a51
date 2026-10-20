// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE
#include "install.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "util.h"
#include "zrnmetrocore.h"

#define AUR_SCRIPT	"/usr/local/libexec/ztstore/ztstore-aur"

struct install inst;

enum step {
	S_NONE,
	S_HELPER,	/* a root service is working on a request */
	S_PREPARE,	/* ztstore-aur prepare */
	S_DEPS,		/* the recipe's needs, from the repos */
	S_BUILD,	/* ztstore-aur build */
	S_PUT,		/* the built package going in */
};

static struct {
	enum step step;
	void (*changed)(void);
	void (*finished)(bool ok);
	/* the root service */
	long request_id;
	pid_t starter;		/* `systemctl start`: says whether it was allowed */
	bool started;
	time_t asked;		/* when the request was made */
	/* ztstore-aur */
	pid_t child;
	int fd;
	char partial[512];
	size_t partial_len;
	char needs[1024];	/* from the repos */
	char built[1024];	/* the package file */
	bool perf_big;
} st = { .fd = -1 };

static void tell(void) {
	if (st.changed) {
		st.changed();
	}
}

static void log_line(const char *line) {
	snprintf(inst.log[inst.log_head], sizeof(inst.log[0]), "%s", line);
	inst.log_head = (inst.log_head + 1) % INST_LOG_LINES;
	if (inst.log_n < INST_LOG_LINES) {
		inst.log_n++;
	}
}

/* zrnperfd: let the build have the big cores while it runs */
static void perf(bool big) {
	if (big == st.perf_big) {
		return;
	}
	int fd = open("/run/zrnperfd.ctl", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0) {
		char line[64];
		int n = snprintf(line, sizeof(line), "policy %d %s\n", (int)getpid(), big ? "big" : "normal");
		if (write(fd, line, n) == n) {
			st.perf_big = big;
		}
		close(fd);
	}
}

static void finish(bool ok, const char *why) {
	perf(false);
	st.step = S_NONE;
	inst.cancellable = false;
	inst.state = ok ? INST_DONE : INST_FAILED;
	if (!ok) {
		snprintf(inst.error, sizeof(inst.error), "%s", why ? why : "it did not work");
	}
	snprintf(inst.stage, sizeof(inst.stage), "%s", ok ? (inst.removing ? "removed" : "installed") : "stopped");
	zmc_busy(false);
	if (ok) {
		shell_ctl("apps");	/* the app list has something new (or less) */
	}
	tell();
	if (st.finished) {
		st.finished(ok);
	}
}

/* ---- the root services ---------------------------------------------------------- */

static const char *user_name(void) {
	struct passwd *pw = getpwuid(getuid());
	return pw ? pw->pw_name : "";
}

static bool request(const char *unit_kind, const char *line) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime) {
		return false;
	}
	char dir[4096], path[4200], text[2048];
	snprintf(dir, sizeof(dir), "%s/ztstore", runtime);
	mkdir(dir, 0700);
	snprintf(path, sizeof(path), "%s/request", dir);
	/* at most 12 digits: ztstore-helper parses the id with [0-9]\{1,12\} and a
	 * longer one would not match, so it would answer "bad request" and the app
	 * would wait for a reply that never comes */
	st.request_id = (long)(time(NULL) % 1000000000) * 1000 + (rand() % 1000);
	int n = snprintf(text, sizeof(text), "id %ld\n%s\n", st.request_id, line);
	unlink(path);	/* never write through a link left there */
	if (n <= 0 || n >= (int)sizeof(text) || !write_atomic(path, text, n)) {
		return false;
	}
	char unit[256];
	snprintf(unit, sizeof(unit), "ztstore-%s@%s.service", unit_kind, user_name());
	st.starter = fork();
	if (st.starter == 0) {
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 1);
			dup2(null, 2);
		}
		execlp("systemctl", "systemctl", "start", "--no-block", unit, (char *)NULL);
		_exit(127);
	}
	st.started = false;
	st.asked = time(NULL);
	st.step = S_HELPER;
	/* the admin service asks for the password on the shell's sheet; if the app
	 * is in the background a word tells the person to come back to it */
	if (!strcmp(unit_kind, "admin")) {
		shell_ctl("notify App Store|Requires your attention|Enter your password to finish.");
	}
	return st.starter > 0;
}

/* the service's status line: "ID working|done|failed WHAT" */
static void helper_poll(enum step next_ok) {
	if (st.starter > 0) {
		int status;
		pid_t r = waitpid(st.starter, &status, WNOHANG);
		if (r == st.starter) {
			st.starter = 0;
			if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
				finish(false, "the system said no (was the password given?)");
				return;
			}
			st.started = true;
		}
	}
	char path[4096];
	snprintf(path, sizeof(path), "/run/ztstore/%s/log", user_name());
	char *log = read_all(path, NULL);
	if (log) {
		/* the last lines of it */
		inst.log_n = inst.log_head = 0;
		char *save = NULL;
		for (char *l = strtok_r(log, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
			if (*l) {
				log_line(l);
			}
			if (strstr(l, "downloading") || strstr(l, "retrieving")) {
				snprintf(inst.stage, sizeof(inst.stage), "downloading");
			} else if (strstr(l, "checking")) {
				snprintf(inst.stage, sizeof(inst.stage), "checking");
			} else if (starts_with(l, "installing") || starts_with(l, "upgrading")) {
				snprintf(inst.stage, sizeof(inst.stage), "installing");
			} else if (starts_with(l, "removing")) {
				snprintf(inst.stage, sizeof(inst.stage), "removing");
			}
		}
		free(log);
	}
	snprintf(path, sizeof(path), "/run/ztstore/%s/status", user_name());
	char line[512];
	zmc_read_file(path, line, sizeof(line));
	char *end;
	long id = strtol(line, &end, 10);
	if (id != st.request_id || end == line) {
		/* not ours yet: the service has not started on it. A request it
		 * could not even read leaves no answer: do not wait for ever. */
		if (st.started && time(NULL) - st.asked > 90) {
			finish(false, "the system service did not answer");
			return;
		}
		tell();
		return;
	}
	const char *what = end + (*end == ' ');
	if (starts_with(what, "done")) {
		if (next_ok == S_NONE) {
			finish(true, NULL);
		} else {
			st.step = next_ok;
		}
	} else if (starts_with(what, "failed")) {
		finish(false, what[6] ? what + 7 : NULL);
	} else {
		tell();
	}
}

/* ---- ztstore-aur ------------------------------------------------------------------ */

static void child_ready(int fd, void *data);

static bool child_start(const char *step) {
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) < 0) {
		return false;
	}
	st.child = fork();
	if (st.child < 0) {
		close(fds[0]);
		close(fds[1]);
		return false;
	}
	if (st.child == 0) {
		setsid();	/* its own group: cancelling stops makepkg and all under it */
		dup2(fds[1], 1);
		dup2(fds[1], 2);
		int null = open("/dev/null", O_RDONLY);
		if (null >= 0) {
			dup2(null, 0);
		}
		execl(AUR_SCRIPT, "ztstore-aur", inst.name, step, (char *)NULL);
		printf("@failed the App Store's build script is missing\n");
		_exit(127);
	}
	close(fds[1]);
	st.fd = fds[0];
	st.partial_len = 0;
	fcntl(st.fd, F_SETFL, O_NONBLOCK);
	zmc_watch_fd(st.fd, child_ready, NULL);
	return true;
}

static void child_stop(void) {
	if (st.fd >= 0) {
		zmc_watch_fd(st.fd, NULL, NULL);
		close(st.fd);
		st.fd = -1;
	}
	if (st.child > 0) {
		kill(-st.child, SIGTERM);
		waitpid(st.child, NULL, 0);
		st.child = 0;
	}
}

static void child_line(char *line) {
	if (line[0] != '@') {
		log_line(line);
		return;
	}
	if (starts_with(line, "@stage ")) {
		snprintf(inst.stage, sizeof(inst.stage), "%s", line + 7);
	} else if (starts_with(line, "@needs")) {
		snprintf(st.needs, sizeof(st.needs), "%s", trim(line + 6));
	} else if (starts_with(line, "@aur")) {
		snprintf(inst.aur_needs, sizeof(inst.aur_needs), "%s", trim(line + 4));
	} else if (starts_with(line, "@built ")) {
		snprintf(st.built, sizeof(st.built), "%s", line + 7);
	} else if (starts_with(line, "@failed")) {
		snprintf(inst.error, sizeof(inst.error), "%s", trim(line + 7));
	}
}

static void start_build(void) {
	st.request_id = 0;
	snprintf(inst.stage, sizeof(inst.stage), "building (this can take a while)");
	inst.cancellable = true;
	perf(true);
	if (!child_start("build")) {
		finish(false, "could not start the build");
		return;
	}
	st.step = S_BUILD;
	tell();
}

static void child_done(void) {
	int status = 0;
	if (st.child > 0) {
		waitpid(st.child, &status, 0);
		st.child = 0;
	}
	zmc_watch_fd(st.fd, NULL, NULL);
	close(st.fd);
	st.fd = -1;
	bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	if (!ok) {
		finish(false, inst.error[0] ? inst.error : "the build script stopped");
		return;
	}
	if (st.step == S_PREPARE) {
		if (inst.aur_needs[0]) {
			char why[300];
			snprintf(why, sizeof(why), "it needs these from the AUR first: %s", inst.aur_needs);
			finish(false, why);
			return;
		}
		if (st.needs[0]) {
			char line[1100];
			snprintf(line, sizeof(line), "deps %s", st.needs);
			snprintf(inst.stage, sizeof(inst.stage), "installing what it needs");
			if (!request("install", line)) {
				finish(false, "could not ask the system to install");
				return;
			}
			st.step = S_DEPS;
			return;
		}
		start_build();	/* nothing to install first */
		return;
	}
	if (st.step == S_BUILD) {
		perf(false);
		inst.cancellable = false;
		if (!st.built[0]) {
			finish(false, "built, but no package came out");
			return;
		}
		char line[1100];
		snprintf(line, sizeof(line), "file %s", st.built);
		snprintf(inst.stage, sizeof(inst.stage), "installing (asks your password)");
		if (!request("admin", line)) {
			finish(false, "could not ask the system to install");
			return;
		}
		st.step = S_PUT;
	}
	tell();
}

static void child_ready(int fd, void *data) {
	char buf[4096];
	ssize_t n;
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		for (ssize_t i = 0; i < n; i++) {
			char c = buf[i];
			if (c == '\n' || c == '\r' || st.partial_len == sizeof(st.partial) - 1) {
				st.partial[st.partial_len] = 0;
				if (st.partial_len) {
					child_line(st.partial);
				}
				st.partial_len = 0;
				if (c == '\n' || c == '\r') {
					continue;
				}
			}
			st.partial[st.partial_len++] = c;
		}
	}
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		child_done();
		return;
	}
	tell();
}

/* ---- the steps -------------------------------------------------------------------- */

void inst_init(void (*changed)(void), void (*finished)(bool ok)) {
	st.changed = changed;
	st.finished = finished;
	srand((unsigned)time(NULL) ^ (unsigned)getpid());
}

bool inst_start(const char *name, bool aur, bool remove) {
	if (inst.state == INST_RUNNING) {
		return false;
	}
	memset(&inst, 0, sizeof(inst));
	st.needs[0] = st.built[0] = 0;
	snprintf(inst.name, sizeof(inst.name), "%s", name);
	inst.aur = aur;
	inst.removing = remove;
	inst.state = INST_RUNNING;
	zmc_busy(true);
	char line[300];
	if (remove) {
		snprintf(inst.stage, sizeof(inst.stage), "removing (asks your password)");
		snprintf(line, sizeof(line), "remove %s", name);
		if (!request("admin", line)) {
			finish(false, "could not ask the system to remove it");
			return false;
		}
	} else if (!aur) {
		snprintf(inst.stage, sizeof(inst.stage), "starting");
		snprintf(line, sizeof(line), "repo %s", name);
		if (!request("install", line)) {
			finish(false, "could not ask the system to install");
			return false;
		}
	} else {
		snprintf(inst.stage, sizeof(inst.stage), "fetching the recipe");
		inst.cancellable = true;
		if (!child_start("prepare")) {
			finish(false, "could not start the build");
			return false;
		}
		st.step = S_PREPARE;
	}
	tell();
	return true;
}

void inst_cancel(void) {
	if (!inst.cancellable) {
		return;
	}
	child_stop();
	finish(false, "cancelled");
}

void inst_clear(void) {
	if (inst.state != INST_RUNNING) {
		memset(&inst, 0, sizeof(inst));
	}
}

void inst_tick(void) {
	switch (st.step) {
	case S_HELPER:
		helper_poll(S_NONE);
		break;
	case S_DEPS:
		helper_poll(S_BUILD);	/* on "done" the step moves to S_BUILD */
		if (st.step == S_BUILD && inst.state == INST_RUNNING) {
			start_build();
		}
		break;
	case S_PUT:
		helper_poll(S_NONE);
		break;
	default:
		break;
	}
}
