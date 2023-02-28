/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/log.h"

static TAILQ_HEAD(, spdk_log_flag) g_log_flags = TAILQ_HEAD_INITIALIZER(g_log_flags);

static struct spdk_log_flag *
get_log_flag(const char *name)
{
	struct spdk_log_flag *flag;

	TAILQ_FOREACH(flag, &g_log_flags, tailq) {
		if (strcasecmp(name, flag->name) == 0) {
			return flag;
		}
	}

	return NULL;
}

void
spdk_log_register_flag(const char *name, struct spdk_log_flag *flag)
{
	struct spdk_log_flag *iter;

	if (name == NULL || flag == NULL) {
		SPDK_ERRLOG("missing spdk_log_flag parameters\n");
		assert(false);
		return;
	}

	if (get_log_flag(name)) {
		SPDK_ERRLOG("duplicate spdk_log_flag '%s'\n", name);
		assert(false);
		return;
	}

	TAILQ_FOREACH(iter, &g_log_flags, tailq) {
		if (strcasecmp(iter->name, flag->name) > 0) {
			TAILQ_INSERT_BEFORE(iter, flag, tailq);
			return;
		}
	}

	TAILQ_INSERT_TAIL(&g_log_flags, flag, tailq);
}

bool
spdk_log_get_flag(const char *name)
{
	struct spdk_log_flag *flag = get_log_flag(name);

	if (flag && flag->enabled) {
		return true;
	}

	return false;
}

static int
log_set_flag(const char *name, bool value)
{
	struct spdk_log_flag *flag;

	if (strcasecmp(name, "all") == 0) {
		TAILQ_FOREACH(flag, &g_log_flags, tailq) {
			flag->enabled = value;
		}
		return 0;
	}

	flag = get_log_flag(name);
	if (flag == NULL) {
		return -1;
	}

	flag->enabled = value;

	return 0;
}

int
spdk_log_set_flag(const char *name)
{
	return log_set_flag(name, true);
}

int
spdk_log_clear_flag(const char *name)
{
	return log_set_flag(name, false);
}

struct spdk_log_flag *
spdk_log_get_first_flag(void)
{
	return TAILQ_FIRST(&g_log_flags);
}

struct spdk_log_flag *
spdk_log_get_next_flag(struct spdk_log_flag *flag)
{
	return TAILQ_NEXT(flag, tailq);
}

void
spdk_log_usage(FILE *f, const char *log_arg)
{
	struct spdk_log_flag *flag;
	fprintf(f, " %s, --logflag <flag>    enable log flag (all", log_arg);

	TAILQ_FOREACH(flag, &g_log_flags, tailq) {
		fprintf(f, ", %s", flag->name);
	}

	fprintf(f, ")\n");
}
