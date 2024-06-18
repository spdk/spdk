/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/log.h"

static TAILQ_HEAD(spdk_log_flag_head,
		  spdk_log_flag) g_log_flags = TAILQ_HEAD_INITIALIZER(g_log_flags);

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
	int rc = -EINVAL;

	if (strcasecmp(name, "all") == 0) {
		TAILQ_FOREACH(flag, &g_log_flags, tailq) {
			flag->enabled = value;
		}
		return 0;
	}

	TAILQ_FOREACH(flag, &g_log_flags, tailq) {
		if (fnmatch(name, flag->name, FNM_CASEFOLD) == 0) {
			flag->enabled = value;
			rc = 0;
		}
	}

	return rc;
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
#define LINE_PREFIX			"                           "
#define ENTRY_SEPARATOR			", "
#define MAX_LINE_LENGTH			100
	uint64_t prefix_len = strlen(LINE_PREFIX);
	uint64_t separator_len = strlen(ENTRY_SEPARATOR);
	const char *first_entry = "--logflag <flag>      enable log flag (all, ";
	uint64_t curr_line_len;
	uint64_t curr_entry_len;
	struct spdk_log_flag *flag;
	char first_line[MAX_LINE_LENGTH] = {};

	snprintf(first_line, sizeof(first_line), " %s, %s", log_arg, first_entry);
	fprintf(f, "%s", first_line);
	curr_line_len = strlen(first_line);

	TAILQ_FOREACH(flag, &g_log_flags, tailq) {
		curr_entry_len = strlen(flag->name);
		if ((curr_line_len + curr_entry_len + separator_len) > MAX_LINE_LENGTH) {
			fprintf(f, "\n%s", LINE_PREFIX);
			curr_line_len = prefix_len;
		}

		fprintf(f, "%s", flag->name);
		curr_line_len += curr_entry_len;

		if (TAILQ_LAST(&g_log_flags, spdk_log_flag_head) == flag) {
			break;
		}

		fprintf(f, "%s", ENTRY_SEPARATOR);
		curr_line_len += separator_len;
	}

	fprintf(f, ")\n");
}
