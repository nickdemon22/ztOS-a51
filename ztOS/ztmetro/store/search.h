// SPDX-License-Identifier: GPL-2.0-or-later
/* ztstore: finding packages in the repos by name and description. */
#ifndef ZTSTORE_SEARCH_H
#define ZTSTORE_SEARCH_H

#include "repo.h"

/* copies of the best `max` matches, best first: the name itself, then names
 * that start with it, then names and app names that hold it, then
 * descriptions; apps before other packages within each */
void search_repo(const struct repo_index *ri, const char *term, int max, struct vec *out);

#endif
