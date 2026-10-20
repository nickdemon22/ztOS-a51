// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * tickle: the laptop zrn_perfd's tickle, for the phone.
 *
 * An app that is not in front is paused, after a grace period, and let run
 * for a moment in every pulse (the laptop's modes):
 *
 *	mode		grace	paused	running
 *	Nominal		30 s	500 ms	100 ms
 *	Moderate	15 s	1000 ms	50 ms
 *	High		5 s	2000 ms	20 ms
 *
 * Differences from the laptop:
 *
 *   - The unit is the app, not the process. ztMETRO says which app is in
 *     front ("focus PID APP_ID"); that app's process and all its children
 *     go into a cgroup of their own (made inside the cgroup they were in, so
 *     their login session is unchanged), and the kernel's cgroup freezer
 *     stops and starts them together. Firefox's content processes pause with
 *     its main one, nothing sees a signal, and nothing is half stopped.
 *   - Playing audio exempts the app precisely. The laptop polled `pactl list
 *     sink-inputs`, counted a stream as playing whenever it existed (paused
 *     ones too) and exempted the processes above the playing one, not the
 *     app around it. Here PipeWire is asked, through `pactl subscribe`, to
 *     say when streams change (no polling); only uncorked playback and
 *     recording streams count; the playing process is found in its app by
 *     its cgroup, so the whole app is exempt; and the exemption lasts 10 s
 *     past the end of playback, so the gap between two tracks does not
 *     pause the player.
 *   - ztMETRO and everything of its own (app ids zt*, zrn*) are never
 *     touched, nor is anything that is not an app ztMETRO has shown.
 *   - With the screen off, no app is in front: the grace period starts then.
 */
#define _GNU_SOURCE
#include "tickle.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_APPS	32
#define CG_PREFIX	"zrn-tickle-"
#define AUDIO_HOLD_MS	10000
#define RETRY_MS	5000

const char *const tickle_names[4] = { "none", "nominal", "moderate", "high" };
static const struct { int grace, paused, running; } timing[4] = {
	{ 0, 0, 0 }, { 30000, 500, 100 }, { 15000, 1000, 50 }, { 5000, 2000, 20 },
};

static struct app {
	pid_t pid;
	char id[64];
	char cg[320];		/* its cgroup directory, "" when exempt */
	long left_front;	/* when it stopped being in front; 0 while in front */
	long audio_until;	/* exempt until then: playing, or just stopped */
	bool audio;		/* a stream is playing right now */
	bool frozen;
} apps[MAX_APPS];
static int n_apps;

static enum tickle_mode mode = TICKLE_MODERATE;
static pid_t front;
static bool screen_on = true;
static bool pulse_paused;	/* the pulse is in its paused part */
static long pulse_next;

static long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

static void put(const char *path, const char *value) {
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd >= 0) {
		if (write(fd, value, strlen(value)) < 0) {
			fprintf(stderr, "zrnperfd: tickle: %s <- %s: %s\n", path, value, strerror(errno));
		}
		close(fd);
	}
}

static void freeze(struct app *a, bool on) {
	char path[340];
	if (!a->cg[0] || a->frozen == on) {
		return;
	}
	snprintf(path, sizeof(path), "%s/cgroup.freeze", a->cg);
	put(path, on ? "1" : "0");
	a->frozen = on;
}

static bool own(const char *id) {
	return strncmp(id, "zt", 2) == 0 || strncmp(id, "zrn", 3) == 0;
}

/* /proc/PID/cgroup's path, "" if gone */
static void cgroup_of(pid_t pid, char *out, size_t size) {
	char path[64], line[256];
	snprintf(path, sizeof(path), "/proc/%d/cgroup", pid);
	FILE *f = fopen(path, "r");
	out[0] = 0;
	if (f && fgets(line, sizeof(line), f) && strncmp(line, "0::", 3) == 0) {
		line[strcspn(line, "\n")] = 0;
		snprintf(out, size, "%s", line + 3);
	}
	if (f) {
		fclose(f);
	}
}

static pid_t parent_of(pid_t pid) {
	char path[64], line[512];
	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	FILE *f = fopen(path, "r");
	pid_t ppid = 0;
	if (f && fgets(line, sizeof(line), f)) {
		char *p = strrchr(line, ')');	/* the name may hold spaces and brackets */
		if (p) {
			sscanf(p + 2, "%*c %d", &ppid);
		}
	}
	if (f) {
		fclose(f);
	}
	return ppid;
}

static bool descends(pid_t pid, pid_t from) {
	for (int depth = 0; pid > 1 && depth < 64; depth++) {
		if (pid == from) {
			return true;
		}
		pid = parent_of(pid);
	}
	return false;
}

/* The app and its children into a cgroup of their own, inside the one the app
 * is in now (its login session stays what it was). */
static void adopt(struct app *a) {
	char current[256], procs[340];
	cgroup_of(a->pid, current, sizeof(current));
	if (!current[0]) {
		return;
	}
	char *leaf = strrchr(current, '/');
	if (leaf && strncmp(leaf + 1, CG_PREFIX, strlen(CG_PREFIX)) == 0) {
		snprintf(a->cg, sizeof(a->cg), "/sys/fs/cgroup%s", current);	/* from a previous run */
		return;
	}
	snprintf(a->cg, sizeof(a->cg), "/sys/fs/cgroup%s/" CG_PREFIX "%d",
		strcmp(current, "/") == 0 ? "" : current, a->pid);
	if (mkdir(a->cg, 0755) < 0 && errno != EEXIST) {
		fprintf(stderr, "zrnperfd: tickle: %s: %s\n", a->cg, strerror(errno));
		a->cg[0] = 0;
		return;
	}
	snprintf(procs, sizeof(procs), "%s/cgroup.procs", a->cg);
	DIR *d = opendir("/proc");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		pid_t pid = atoi(e->d_name);
		if (pid > 0 && descends(pid, a->pid)) {
			char text[16];
			snprintf(text, sizeof(text), "%d", pid);
			put(procs, text);
		}
	}
	if (d) {
		closedir(d);
	}
	fprintf(stderr, "zrnperfd: tickle: %s (%d) in %s\n", a->id, a->pid, a->cg);
}

static void forget(int i) {
	freeze(&apps[i], false);
	if (apps[i].cg[0]) {
		rmdir(apps[i].cg);	/* only goes once empty */
	}
	apps[i] = apps[--n_apps];
}

/* drop apps that have exited */
static void sweep(void) {
	for (int i = n_apps - 1; i >= 0; i--) {
		if (kill(apps[i].pid, 0) < 0 && errno == ESRCH) {
			forget(i);
		}
	}
}

/* Every leftover cgroup of ours running again (a previous run that ended
 * with apps paused). Under the user slices, where apps live. */
static void thaw_leftovers(void) {
	static const char *const where[] = {
		"/sys/fs/cgroup/user.slice/*/*/" CG_PREFIX "*/cgroup.freeze",
		"/sys/fs/cgroup/user.slice/*/*/*/" CG_PREFIX "*/cgroup.freeze",
		"/sys/fs/cgroup/user.slice/*/*/*/*/" CG_PREFIX "*/cgroup.freeze",
	};
	for (int w = 0; w < 3; w++) {
		glob_t g;
		if (glob(where[w], 0, NULL, &g) == 0) {
			for (size_t i = 0; i < g.gl_pathc; i++) {
				put(g.gl_pathv[i], "0");
			}
		}
		globfree(&g);
	}
}

/* --- audio: PipeWire, through pactl, as the user it runs for ------------- */

static int audio_fd = -1;
static pid_t audio_child;
static long audio_retry, audio_rescan;

static bool session_user(uid_t *uid) {
	glob_t g;
	bool found = false;
	if (glob("/run/user/*/pulse/native", 0, NULL, &g) == 0) {
		struct stat st;
		if (stat(g.gl_pathv[0], &st) == 0) {
			*uid = st.st_uid;
			found = true;
		}
	}
	globfree(&g);
	return found;
}

/* Start pactl as the session's user, its output on a pipe. */
static pid_t pactl(char *const argv[], int *out) {
	uid_t uid;
	int p[2];
	if (!session_user(&uid) || pipe2(p, O_CLOEXEC) < 0) {
		return -1;
	}
	pid_t pid = fork();
	if (pid == 0) {
		char env[64];
		snprintf(env, sizeof(env), "XDG_RUNTIME_DIR=/run/user/%d", uid);
		putenv(env);
		dup2(p[1], 1);
		int null = open("/dev/null", O_WRONLY);
		dup2(null, 2);
		struct passwd *pw = getpwuid(uid);
		if (setgroups(0, NULL) < 0 || setgid(pw ? pw->pw_gid : uid) < 0 || setuid(uid) < 0) {
			_exit(1);
		}
		execv("/usr/bin/pactl", argv);
		_exit(127);
	}
	close(p[1]);
	if (pid < 0) {
		close(p[0]);
		return -1;
	}
	*out = p[0];
	return pid;
}

static struct app *app_holding(pid_t pid) {
	char cg[256];
	cgroup_of(pid, cg, sizeof(cg));
	for (int i = 0; i < n_apps; i++) {
		const char *leaf = strrchr(apps[i].cg, '/');
		size_t n = strlen(cg);
		if (leaf && n > strlen(leaf) && strcmp(cg + n - strlen(leaf), leaf) == 0) {
			return &apps[i];
		}
	}
	for (int i = 0; i < n_apps; i++) {	/* not moved yet: by family */
		if (descends(pid, apps[i].pid)) {
			return &apps[i];
		}
	}
	return NULL;
}

/* Which apps have a stream that is playing (or recording) now. */
static void audio_scan(void) {
	static char *const kinds[] = { "sink-inputs", "source-outputs" };
	long now = now_ms();
	for (int i = 0; i < n_apps; i++) {
		if (apps[i].audio) {
			apps[i].audio = false;
			apps[i].audio_until = now + AUDIO_HOLD_MS;	/* it just stopped */
		}
	}
	for (int k = 0; k < 2; k++) {
		char *argv[] = { "pactl", "-f", "json", "list", kinds[k], NULL };
		int fd;
		pid_t child = pactl(argv, &fd);
		if (child < 0) {
			continue;
		}
		static char json[65536];
		size_t len = 0;
		ssize_t n;
		while (len < sizeof(json) - 1 && (n = read(fd, json + len, sizeof(json) - 1 - len)) > 0) {
			len += n;
		}
		json[len] = 0;
		close(fd);
		waitpid(child, NULL, 0);
		/* one object per stream, each starting with its "index" */
		for (char *obj = strstr(json, "\"index\":"); obj; ) {
			char *next = strstr(obj + 8, "\"index\":");
			if (next) {
				*next = 0;
			}
			char *pidkey = strstr(obj, "\"application.process.id\":\"");
			if (strstr(obj, "\"corked\":false") && pidkey) {
				struct app *a = app_holding(atoi(pidkey + 26));
				if (a) {
					a->audio = true;
					a->audio_until = 0;
				}
			}
			if (next) {
				*next = '"';
			}
			obj = next;
		}
	}
}

static void audio_start(void) {
	char *argv[] = { "pactl", "subscribe", NULL };
	audio_child = pactl(argv, &audio_fd);
	if (audio_child < 0) {
		audio_fd = -1;
		audio_retry = now_ms() + RETRY_MS;	/* PipeWire not up yet */
		return;
	}
	fcntl(audio_fd, F_SETFL, O_NONBLOCK);
	audio_retry = 0;
	audio_scan();
}

static void audio_lost(void) {
	close(audio_fd);
	audio_fd = -1;
	kill(audio_child, SIGTERM);
	waitpid(audio_child, NULL, 0);
	audio_retry = now_ms() + RETRY_MS;
}

/* --- the pulse ------------------------------------------------------------- */

static bool exempt(const struct app *a, long now) {
	return !a->cg[0] || (screen_on && a->pid == front) || a->audio || now < a->audio_until;
}

static bool due(const struct app *a, long now) {
	return mode != TICKLE_NONE && !exempt(a, now) && a->left_front &&
		now - a->left_front >= timing[mode].grace;
}

/* what tickle_status() reports, to tell when it changes */
static int counts(long now) {
	int paused = 0, audio = 0, watched = 0;
	for (int i = 0; i < n_apps; i++) {
		if (apps[i].cg[0]) {
			watched++;
			paused += due(&apps[i], now);
			audio += apps[i].audio || now < apps[i].audio_until;
		}
	}
	return watched << 16 | paused << 8 | audio;
}

static bool pulse(long now);

bool tickle_run(void) {
	static int reported = -1;
	long now = now_ms();
	pulse(now);
	int c = counts(now);
	bool changed = c != reported;
	reported = c;
	return changed;
}

static bool pulse(long now) {
	if (audio_fd < 0 && audio_retry && now >= audio_retry) {
		audio_start();
	}
	if (audio_rescan && now >= audio_rescan) {
		audio_rescan = 0;
		audio_scan();
	}
	sweep();
	bool any = false;
	for (int i = 0; i < n_apps; i++) {
		if (due(&apps[i], now)) {
			any = true;
		} else {
			freeze(&apps[i], false);	/* in front, playing, or still in grace */
		}
	}
	if (!any) {
		pulse_paused = false;
		pulse_next = 0;
		return false;
	}
	if (now < pulse_next) {
		return false;
	}
	pulse_paused = !pulse_paused;
	for (int i = 0; i < n_apps; i++) {
		if (due(&apps[i], now)) {
			freeze(&apps[i], pulse_paused);
		}
	}
	pulse_next = now + (pulse_paused ? timing[mode].paused : timing[mode].running);
	return true;
}

int tickle_timeout(void) {
	long now = now_ms(), next = -1;
	if (audio_fd < 0 && audio_retry) {
		next = audio_retry;
	}
	if (audio_rescan && (next < 0 || audio_rescan < next)) {
		next = audio_rescan;
	}
	for (int i = 0; i < n_apps; i++) {
		long at = -1;
		const struct app *a = &apps[i];
		if (due(a, now)) {
			at = pulse_next ? pulse_next : now;
		} else if (mode != TICKLE_NONE && a->cg[0] && a->left_front && !exempt(a, now)) {
			at = a->left_front + timing[mode].grace;	/* grace ends */
		} else if (a->audio_until > now) {
			at = a->audio_until;			/* the hold ends */
		}
		if (at >= 0 && (next < 0 || at < next)) {
			next = at;
		}
	}
	return next < 0 ? -1 : next <= now ? 0 : (int)(next - now);
}

int tickle_pollfds(struct pollfd *fds, int max) {
	if (audio_fd < 0 || max < 1) {
		return 0;
	}
	fds[0] = (struct pollfd){ .fd = audio_fd, .events = POLLIN };
	return 1;
}

void tickle_ready(const struct pollfd *fds, int n) {
	if (n < 1 || audio_fd < 0 || !(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
		return;
	}
	char buf[4096];
	ssize_t got = read(audio_fd, buf, sizeof(buf) - 1);
	if (got <= 0) {
		if (got == 0 || errno != EAGAIN) {
			audio_lost();
		}
		return;
	}
	buf[got] = 0;
	/* a burst of events comes with every stream change: one look, shortly after */
	if ((strstr(buf, "sink-input") || strstr(buf, "source-output")) && !audio_rescan) {
		audio_rescan = now_ms() + 150;
	}
}

void tickle_focus(pid_t pid, const char *app_id) {
	long now = now_ms();
	for (int i = 0; i < n_apps; i++) {
		if (apps[i].pid == front && pid != front) {
			apps[i].left_front = now;
		}
	}
	front = pid;
	if (pid <= 0) {
		return;
	}
	for (int i = 0; i < n_apps; i++) {
		if (apps[i].pid == pid) {
			apps[i].left_front = 0;
			freeze(&apps[i], false);
			return;
		}
	}
	if (n_apps == MAX_APPS) {
		sweep();
		if (n_apps == MAX_APPS) {
			return;
		}
	}
	struct app *a = &apps[n_apps++];
	*a = (struct app){ .pid = pid };
	snprintf(a->id, sizeof(a->id), "%s", app_id);
	if (!own(app_id)) {
		adopt(a);
		audio_scan();	/* it may already be playing */
	}
}

void tickle_screen(bool on) {
	long now = now_ms();
	screen_on = on;
	for (int i = 0; i < n_apps; i++) {
		if (apps[i].pid == front) {
			apps[i].left_front = on ? 0 : now;
		}
	}
}

void tickle_set_mode(enum tickle_mode m) {
	if (m != mode) {
		mode = m;
		pulse_next = 0;
	}
}

void tickle_status(FILE *f) {
	long now = now_ms();
	int paused = 0, audio = 0, watched = 0;
	for (int i = 0; i < n_apps; i++) {
		if (!apps[i].cg[0]) {
			continue;
		}
		watched++;
		paused += due(&apps[i], now);
		audio += apps[i].audio || now < apps[i].audio_until;
	}
	fprintf(f, "tickle=%s\ntickle_apps=%d\ntickle_paused=%d\ntickle_audio=%d\n",
		tickle_names[mode], watched, paused, audio);
}

void tickle_init(void) {
	thaw_leftovers();
	audio_start();
}

void tickle_stop(void) {
	for (int i = 0; i < n_apps; i++) {
		freeze(&apps[i], false);
	}
	thaw_leftovers();
	if (audio_fd >= 0) {
		audio_lost();
	}
}
