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
#include "spdk/thread.h"

#include "spdk_internal/event.h"
#include "spdk/env.h"

TAILQ_HEAD(spdk_subsystem_list, spdk_subsystem);
struct spdk_subsystem_list g_subsystems = TAILQ_HEAD_INITIALIZER(g_subsystems);

TAILQ_HEAD(spdk_subsystem_depend_list, spdk_subsystem_depend);
struct spdk_subsystem_depend_list g_subsystems_deps = TAILQ_HEAD_INITIALIZER(g_subsystems_deps);
static struct spdk_subsystem *g_next_subsystem;
static bool g_subsystems_initialized = false;
static bool g_subsystems_init_interrupted = false;
static spdk_subsystem_init_fn g_subsystem_start_fn = NULL;
static void *g_subsystem_start_arg = NULL;
static spdk_msg_fn g_subsystem_stop_fn = NULL;
static void *g_subsystem_stop_arg = NULL;
static struct spdk_thread *g_fini_thread = NULL;

void
spdk_add_subsystem(struct spdk_subsystem *subsystem)
{
	TAILQ_INSERT_TAIL(&g_subsystems, subsystem, tailq);
}

void
spdk_add_subsystem_depend(struct spdk_subsystem_depend *depend)
{
	TAILQ_INSERT_TAIL(&g_subsystems_deps, depend, tailq);
}

static struct spdk_subsystem *
_subsystem_find(struct spdk_subsystem_list *list, const char *name)
{
	struct spdk_subsystem *iter;

	TAILQ_FOREACH(iter, list, tailq) {
		if (strcmp(name, iter->name) == 0) {
			return iter;
		}
	}

	return NULL;
}

struct spdk_subsystem *
spdk_subsystem_find(const char *name)
{
	return _subsystem_find(&g_subsystems, name);
}

struct spdk_subsystem *
spdk_subsystem_get_first(void)
{
	return TAILQ_FIRST(&g_subsystems);
}

struct spdk_subsystem *
spdk_subsystem_get_next(struct spdk_subsystem *cur_subsystem)
{
	return TAILQ_NEXT(cur_subsystem, tailq);
}


struct spdk_subsystem_depend *
spdk_subsystem_get_first_depend(void)
{
	return TAILQ_FIRST(&g_subsystems_deps);
}

struct spdk_subsystem_depend *
spdk_subsystem_get_next_depend(struct spdk_subsystem_depend *cur_depend)
{
	return TAILQ_NEXT(cur_depend, tailq);
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
			TAILQ_FOREACH(subsystem_dep, &g_subsystems_deps, tailq) {
				if (strcmp(subsystem->name, subsystem_dep->name) == 0) {
					depends_on = true;
					depends_on_sorted = !!_subsystem_find(&subsystems_list, subsystem_dep->depends_on);
					if (depends_on_sorted) {
						continue;
					}
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
	/* The initialization is interrupted by the spdk_subsystem_fini, so just return */
	if (g_subsystems_init_interrupted) {
		return;
	}

	if (rc) {
		SPDK_ERRLOG("Init subsystem %s failed\n", g_next_subsystem->name);
		g_subsystem_start_fn(rc, g_subsystem_start_arg);
		return;
	}

	if (!g_next_subsystem) {
		g_next_subsystem = TAILQ_FIRST(&g_subsystems);
	} else {
		g_next_subsystem = TAILQ_NEXT(g_next_subsystem, tailq);
	}

	if (!g_next_subsystem) {
		g_subsystems_initialized = true;
		g_subsystem_start_fn(0, g_subsystem_start_arg);
		return;
	}

	if (g_next_subsystem->init) {
		g_next_subsystem->init();
	} else {
		spdk_subsystem_init_next(0);
	}
}

void
spdk_subsystem_init(spdk_subsystem_init_fn cb_fn, void *cb_arg)
{
	struct spdk_subsystem_depend *dep;

	g_subsystem_start_fn = cb_fn;
	g_subsystem_start_arg = cb_arg;

	/* Verify that all dependency name and depends_on subsystems are registered */
	TAILQ_FOREACH(dep, &g_subsystems_deps, tailq) {
		if (!spdk_subsystem_find(dep->name)) {
			SPDK_ERRLOG("subsystem %s is missing\n", dep->name);
			g_subsystem_start_fn(-1, g_subsystem_start_arg);
			return;
		}
		if (!spdk_subsystem_find(dep->depends_on)) {
			SPDK_ERRLOG("subsystem %s dependency %s is missing\n",
				    dep->name, dep->depends_on);
			g_subsystem_start_fn(-1, g_subsystem_start_arg);
			return;
		}
	}

	subsystem_sort();

	spdk_subsystem_init_next(0);
}

static void
subsystem_fini_next(void *arg1)
{
	assert(g_fini_thread == spdk_get_thread());

	if (!g_next_subsystem) {
		/* If the initialized flag is false, then we've failed to initialize
		 * the very first subsystem and no de-init is needed
		 */
		if (g_subsystems_initialized) {
			g_next_subsystem = TAILQ_LAST(&g_subsystems, spdk_subsystem_list);
		}
	} else {
		if (g_subsystems_initialized || g_subsystems_init_interrupted) {
			g_next_subsystem = TAILQ_PREV(g_next_subsystem, spdk_subsystem_list, tailq);
		} else {
			g_subsystems_init_interrupted = true;
		}
	}

	while (g_next_subsystem) {
		if (g_next_subsystem->fini) {
			g_next_subsystem->fini();
			return;
		}
		g_next_subsystem = TAILQ_PREV(g_next_subsystem, spdk_subsystem_list, tailq);
	}

	g_subsystem_stop_fn(g_subsystem_stop_arg);
	return;
}

void
spdk_subsystem_fini_next(void)
{
	if (g_fini_thread != spdk_get_thread()) {
		spdk_thread_send_msg(g_fini_thread, subsystem_fini_next, NULL);
	} else {
		subsystem_fini_next(NULL);
	}
}

void
spdk_subsystem_fini(spdk_msg_fn cb_fn, void *cb_arg)
{
	g_subsystem_stop_fn = cb_fn;
	g_subsystem_stop_arg = cb_arg;

	g_fini_thread = spdk_get_thread();

	spdk_subsystem_fini_next();
}

void
spdk_subsystem_config_json(struct spdk_json_write_ctx *w, struct spdk_subsystem *subsystem)
{
	if (subsystem && subsystem->write_config_json) {
		subsystem->write_config_json(w);
	} else {
		spdk_json_write_null(w);
	}
}
