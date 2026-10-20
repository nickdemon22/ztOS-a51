// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE
#include "jobs.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

#include "zrnmetrocore.h"

struct job {
	void (*work)(void *);
	void (*done)(void *);
	void *arg;
	int finished;	/* the last message of a job, so it can be counted out */
};

static int pipe_fds[2] = { -1, -1 };
static atomic_int running;

static void post(struct job *j) {
	/* a pointer is far below PIPE_BUF: the write is whole */
	while (write(pipe_fds[1], &j, sizeof(j)) < 0 && errno == EINTR) {
	}
}

static void ready(int fd, void *data) {
	struct job *j;
	ssize_t n;
	while ((n = read(fd, &j, sizeof(j))) == sizeof(j)) {
		if (j->done) {
			j->done(j->arg);
		}
		if (j->finished) {
			atomic_fetch_sub(&running, 1);
		}
		free(j);
	}
}

void jobs_init(void) {
	if (pipe2(pipe_fds, O_CLOEXEC) == 0) {
		fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
		zmc_watch_fd(pipe_fds[0], ready, NULL);
	}
}

static void *thread(void *v) {
	struct job *j = v;
	j->work(j->arg);
	j->finished = 1;
	post(j);
	return NULL;
}

void job_run(void (*work)(void *), void (*done)(void *), void *arg) {
	struct job *j = calloc(1, sizeof(*j));
	if (!j) {
		abort();
	}
	j->work = work;
	j->done = done;
	j->arg = arg;
	atomic_fetch_add(&running, 1);
	pthread_t t;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&t, &attr, thread, j) != 0) {
		/* no thread: do it here, late but done */
		work(arg);
		j->finished = 1;
		post(j);
	}
	pthread_attr_destroy(&attr);
}

void job_post(void (*done)(void *), void *arg) {
	struct job *j = calloc(1, sizeof(*j));
	if (!j) {
		abort();
	}
	j->done = done;
	j->arg = arg;
	post(j);
}

int jobs_running(void) {
	return atomic_load(&running);
}
