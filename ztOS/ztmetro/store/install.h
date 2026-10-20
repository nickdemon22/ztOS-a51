// SPDX-License-Identifier: GPL-2.0-or-later
/* ztstore: putting a package on the phone, or taking one off, one at a time.
 *
 * From the repos: ztstore-install@USER.service does it as root (no password:
 * the packages are signed). From the AUR: the recipe is fetched and built here,
 * as the person (ztstore-aur), with what it needs from the repos installed
 * first, and the built package goes in through ztstore-admin@USER.service,
 * which asks their password. Removing goes through that one too. */
#ifndef ZTSTORE_INSTALL_H
#define ZTSTORE_INSTALL_H

#include <stdbool.h>

#define INST_LOG_LINES	14

enum inst_state { INST_IDLE, INST_RUNNING, INST_DONE, INST_FAILED };

struct install {
	enum inst_state state;
	char name[128];
	bool aur, removing;
	char stage[200];	/* what is happening now, in a few words */
	char error[240];
	char aur_needs[240];	/* AUR packages to install first, when that stopped it */
	char log[INST_LOG_LINES][160];	/* the newest lines of what pacman or makepkg said */
	int log_n, log_head;
	bool cancellable;
};

extern struct install inst;

/* `changed` runs on the main loop whenever there is something new to show;
 * `finished` once, when it is done or has failed */
void inst_init(void (*changed)(void), void (*finished)(bool ok));
bool inst_start(const char *name, bool aur, bool remove);
void inst_cancel(void);
void inst_clear(void);	/* forget a finished one */
/* call every half a second while something runs */
void inst_tick(void);

#endif
