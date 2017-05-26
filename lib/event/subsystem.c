/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include "spdk/log.h"

#include "spdk_internal/event.h"

static TAILQ_HEAD(spdk_subsystem_list, spdk_subsystem) g_subsystems =
	TAILQ_HEAD_INITIALIZER(g_subsystems);
static TAILQ_HEAD(subsystem_depend, spdk_subsystem_depend) g_depends =
	TAILQ_HEAD_INITIALIZER(g_depends);
static struct spdk_subsystem *g_next_subsystem;
static struct spdk_event *g_app_start_event;

void
spdk_add_subsystem(struct spdk_subsystem *subsystem)
{
	TAILQ_INSERT_TAIL(&g_subsystems, subsystem, tailq);
}

void
spdk_add_subsystem_depend(struct spdk_subsystem_depend *depend)
{
	TAILQ_INSERT_TAIL(&g_depends, depend, tailq);
}

static struct spdk_subsystem *
spdk_subsystem_find(struct spdk_subsystem_list *list, const char *name)
{
	struct spdk_subsystem *iter;

	TAILQ_FOREACH(iter, list, tailq) {
		if (strcmp(name, iter->name) == 0) {
			return iter;
		}
	}

	return NULL;
}

static void
subsystem_sort(void)
{
	bool depends_on, depends_on_sorted;
	struct spdk_subsystem *subsystem, *subsystem_tmp;
	struct spdk_subsystem_depend *subsystem_dep;

	struct spdk_subsystem_list subsystems_list = TAILQ_HEAD_INITIALIZER(subsystems_list);

	while (!TAILQ_EMPTY(&g_subsystems)) {
		TAILQ_FOREACH_SAFE(subsystem, &g_subsystems, tailq, subsystem_tmp) {
			depends_on = false;
			TAILQ_FOREACH(subsystem_dep, &g_depends, tailq) {
				if (strcmp(subsystem->name, subsystem_dep->name) == 0) {
					depends_on = true;
					depends_on_sorted = !!spdk_subsystem_find(&subsystems_list, subsystem_dep->depends_on);
					if (depends_on_sorted)
						continue;
					break;
				}
			}

			if (depends_on == false) {
				TAILQ_REMOVE(&g_subsystems, subsystem, tailq);
				TAILQ_INSERT_TAIL(&subsystems_list, subsystem, tailq);
			} else {
				if (depends_on_sorted == true) {
					TAILQ_REMOVE(&g_subsystems, subsystem, tailq);
					TAILQ_INSERT_TAIL(&subsystems_list, subsystem, tailq);
				}
			}
		}
	}

	TAILQ_FOREACH_SAFE(subsystem, &subsystems_list, tailq, subsystem_tmp) {
		TAILQ_REMOVE(&subsystems_list, subsystem, tailq);
		TAILQ_INSERT_TAIL(&g_subsystems, subsystem, tailq);
	}
}

void
spdk_subsystem_init_next(int rc)
{
	if (rc) {
		spdk_app_stop(rc);
		assert(g_next_subsystem != NULL);
		SPDK_ERRLOG("Init subsystem %s failed\n", g_next_subsystem->name);
		return;
	}

	if (!g_next_subsystem) {
		g_next_subsystem = TAILQ_FIRST(&g_subsystems);
	} else {
		g_next_subsystem = TAILQ_NEXT(g_next_subsystem, tailq);
	}

	if (!g_next_subsystem) {
		spdk_event_call(g_app_start_event);
		return;
	}

	if (g_next_subsystem->init) {
		g_next_subsystem->init();
	} else {
		spdk_subsystem_init_next(0);
	}
}

void
spdk_subsystem_init(void *arg1, void *arg2)
{
	struct spdk_subsystem_depend *dep;

	g_app_start_event = (struct spdk_event *)arg1;

	/* Verify that all dependency name and depends_on subsystems are registered */
	TAILQ_FOREACH(dep, &g_depends, tailq) {
		if (!spdk_subsystem_find(&g_subsystems, dep->name)) {
			SPDK_ERRLOG("subsystem %s is missing\n", dep->name);
			spdk_app_stop(-1);
			return;
		}
		if (!spdk_subsystem_find(&g_subsystems, dep->depends_on)) {
			SPDK_ERRLOG("subsystem %s dependency %s is missing\n",
				    dep->name, dep->depends_on);
			spdk_app_stop(-1);
			return;
		}
	}

	subsystem_sort();

	spdk_subsystem_init_next(0);
}

int
spdk_subsystem_fini(void)
{
	int rc = 0;
	struct spdk_subsystem *cur;

	cur = TAILQ_LAST(&g_subsystems, spdk_subsystem_list);

	while (cur) {
		if (cur->fini) {
			rc = cur->fini();
			if (rc)
				return rc;
		}
		cur = TAILQ_PREV(cur, spdk_subsystem_list, tailq);
	}

	return rc;
}

void
spdk_subsystem_config(FILE *fp)
{
	struct spdk_subsystem *subsystem;

	TAILQ_FOREACH(subsystem, &g_subsystems, tailq) {
		if (subsystem->config)
			subsystem->config(fp);
	}
}
