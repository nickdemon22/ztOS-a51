// SPDX-License-Identifier: GPL-2.0-only
/* Reuse the backend's mapping/engine in a separately unloadable diagnostic.
 * This build rejects backend registration and permits only a SAVE probe. */
#define ZRN_TABLE_CHECK_ONLY
#include "platform.c"
