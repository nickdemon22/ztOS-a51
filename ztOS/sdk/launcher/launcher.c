// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The front of every ztOS SDK app binary.
 *
 * An app is one file: this small program, and behind it a zip archive of the
 * app's Python code, its compiled .ui pages, its pictures and the ztos
 * runtime. Python reads a zip from its end, so the program in front does not
 * get in its way.
 *
 * Started, it asks the warm runtime (ztos-zygote: Qt for Python loaded once,
 * asleep on a socket) to fork the app from itself, which skips the second of
 * CPU that loading Qt costs on the phone; this program then stays as the app's
 * stand-in, passing signals on and leaving with its exit code. With no warm
 * runtime there it starts one for next time and runs the app the plain way:
 * hands its own file to the phone's Python.
 *
 *	ZTOS_PYTHON	the Python to run the app with (python3 on the PATH)
 *	ZTOS_ZYGOTE=0	never use the warm runtime
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static pid_t app_pid;

static void pass_on(int sig) {
	if (app_pid > 0) {
		kill(app_pid, sig);
	}
}

static int zygote_path(char *out, size_t size) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	char fallback[64];
	if (!runtime || !runtime[0]) {
		snprintf(fallback, sizeof(fallback), "/run/user/%u", (unsigned)getuid());
		runtime = fallback;
	}
	return snprintf(out, size, "%s/ztos-zygote", runtime) < (int)size ? 0 : -1;
}

/* No warm runtime: start one (detached, in its own session). Returns 1 if started. */
static int start_zygote(const char *python) {
	const char *home = getenv("HOME");
	char script[PATH_MAX], log[PATH_MAX];
	if (!home || snprintf(script, sizeof(script), "%s/.local/share/ztos/zygote.py", home) >= (int)sizeof(script) ||
			access(script, R_OK) != 0) {
		return 0;
	}
	snprintf(log, sizeof(log), "%s/.local/state/ztos/zygote.log", home);
	pid_t pid = fork();
	if (pid != 0) {
		if (pid > 0) {
			while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
			}
		}
		return pid > 0;
	}
	if (fork() != 0) {
		_exit(0);	/* the grandchild below goes on, adopted by init */
	}
	setsid();
	int null = open("/dev/null", O_RDWR);
	int out = open(log, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	dup2(null, 0);
	dup2(out >= 0 ? out : null, 1);
	dup2(out >= 0 ? out : null, 2);
	execlp(python, python, "-I", script, (char *)NULL);
	_exit(127);
}

static int add(char **buf, size_t *len, size_t *cap, const char *s) {
	size_t n = strlen(s) + 1;
	if (*len + n > *cap) {
		*cap = (*len + n) * 2;
		char *more = realloc(*buf, *cap);
		if (!more) {
			return -1;
		}
		*buf = more;
	}
	memcpy(*buf + *len, s, n);
	*len += n;
	return 0;
}

/* Run the app through the warm runtime. Returns only if it cannot. */
static void via_zygote(const char *self, int argc, char **argv) {
	char where[PATH_MAX], cwd[PATH_MAX], count[16];
	if (zygote_path(where, sizeof(where)) < 0 || !getcwd(cwd, sizeof(cwd))) {
		return;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	if (fd < 0 || strlen(where) >= sizeof(addr.sun_path)) {
		return;
	}
	strcpy(addr.sun_path, where);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return;
	}
	/* "ztos1", the length, then: binary, cwd, argc, args..., NAME=VALUE... */
	size_t len = 9, cap = 4096;
	char *buf = malloc(cap);
	if (!buf) {
		close(fd);
		return;
	}
	memcpy(buf, "ztos1", 5);
	snprintf(count, sizeof(count), "%d", argc - 1);
	int bad = add(&buf, &len, &cap, self) || add(&buf, &len, &cap, cwd) || add(&buf, &len, &cap, count);
	for (int i = 1; i < argc && !bad; i++) {
		bad = add(&buf, &len, &cap, argv[i]);
	}
	for (char **e = environ; *e && !bad; e++) {
		bad = add(&buf, &len, &cap, *e);
	}
	if (bad) {
		free(buf);
		close(fd);
		return;
	}
	uint32_t body = (uint32_t)(len - 9);
	for (int i = 0; i < 4; i++) {
		buf[5 + i] = (char)((body >> (8 * i)) & 0xff);
	}
	/* our stdin, stdout and stderr go with it: the app's are ours */
	int fds[3] = { 0, 1, 2 };
	char control[CMSG_SPACE(sizeof(fds))];
	memset(control, 0, sizeof(control));
	struct iovec iov = { .iov_base = buf, .iov_len = len };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control,
		.msg_controllen = sizeof(control) };
	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(fds));
	memcpy(CMSG_DATA(cmsg), fds, sizeof(fds));
	ssize_t sent = sendmsg(fd, &msg, MSG_NOSIGNAL);
	if (sent > 0 && (size_t)sent < len) {
		/* the rest of a long environment, after the part that carried the descriptors */
		size_t done = (size_t)sent;
		while (done < len) {
			ssize_t more = send(fd, buf + done, len - done, MSG_NOSIGNAL);
			if (more <= 0) {
				break;
			}
			done += (size_t)more;
		}
		sent = (ssize_t)done;
	}
	free(buf);
	if (sent != (ssize_t)len) {
		close(fd);
		return;
	}
	/* "pid N" once it is forked, "exit N" when it ends */
	char reply[128];
	size_t have = 0;
	int code = 0;
	struct sigaction pass = { .sa_handler = pass_on };
	sigemptyset(&pass.sa_mask);
	for (;;) {
		ssize_t n = read(fd, reply + have, sizeof(reply) - 1 - have);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			break;		/* the warm runtime went away (the app may still be running) */
		}
		have += (size_t)n;
		reply[have] = 0;
		char *line;
		while ((line = strchr(reply, '\n'))) {
			*line = 0;
			if (sscanf(reply, "pid %d", &app_pid) == 1 && app_pid > 0) {
				sigaction(SIGTERM, &pass, NULL);
				sigaction(SIGINT, &pass, NULL);
				sigaction(SIGHUP, &pass, NULL);
				sigaction(SIGQUIT, &pass, NULL);
			} else if (sscanf(reply, "exit %d", &code) == 1) {
				_exit(code < 0 ? 128 - code : code);
			}
			have -= (size_t)(line + 1 - reply);
			memmove(reply, line + 1, have + 1);
		}
	}
	close(fd);
	if (app_pid > 0) {
		/* the app is running (or ran): it is not to be started a second time */
		while (kill(app_pid, 0) == 0) {
			sleep(1);
		}
		_exit(code);
	}
}

int main(int argc, char **argv) {
	char self[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (n < 0) {
		perror("ztos: where am I");
		return 127;
	}
	self[n] = 0;
	setenv("ZTOS_APP_BINARY", self, 1);
	/* the shell is Wayland; draw no title bar of Qt's own on its windows */
	if (getenv("WAYLAND_DISPLAY")) {
		setenv("QT_QPA_PLATFORM", "wayland", 0);
	}
	setenv("QT_WAYLAND_DISABLE_WINDOWDECORATION", "1", 0);

	const char *python = getenv("ZTOS_PYTHON");
	if (!python || !python[0]) {
		python = "python3";
	}
	const char *zygote = getenv("ZTOS_ZYGOTE");
	if (!zygote || strcmp(zygote, "0") != 0) {
		via_zygote(self, argc, argv);
		/* there was none: start it, and wait for it (it loads Qt once, for
		 * this app and the ones after) rather than load Qt here as well */
		if (start_zygote(python)) {
			for (int i = 0; i < 80; i++) {
				usleep(50 * 1000);
				via_zygote(self, argc, argv);
			}
		}
	}

	char **args = calloc((size_t)argc + 4, sizeof(*args));
	if (!args) {
		return 127;
	}
	int k = 0;
	args[k++] = (char *)python;
	args[k++] = "-I";	/* only the app's own code and the system's: no PYTHONPATH, no user site */
	args[k++] = self;
	for (int i = 1; i < argc; i++) {
		args[k++] = argv[i];
	}
	args[k] = NULL;
	execvp(python, args);
	fprintf(stderr, "ztos: cannot run %s: %s (is Python installed? ztos-sdk doctor --fix)\n", python,
		strerror(errno));
	return 127;
}
