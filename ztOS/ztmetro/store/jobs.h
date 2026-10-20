// SPDX-License-Identifier: GPL-2.0-or-later
/* ztstore: slow work (the network, reading the package databases) off the
 * main loop. `work` runs on a thread of its own; `done` runs afterwards on the
 * main loop, where it may touch what the pages draw. */
#ifndef ZTSTORE_JOBS_H
#define ZTSTORE_JOBS_H

void jobs_init(void);
void job_run(void (*work)(void *arg), void (*done)(void *arg), void *arg);
/* from a job's thread: have `done` run on the main loop now, mid-job */
void job_post(void (*done)(void *arg), void *arg);
int jobs_running(void);

#endif
