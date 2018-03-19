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

#ifndef SPDK_INTERNAL_EVENT_H
#define SPDK_INTERNAL_EVENT_H

#include "spdk/stdinc.h"

#include "spdk/event.h"
#include "spdk/json.h"

struct spdk_event {
	uint32_t		lcore;
	spdk_event_fn		fn;
	void			*arg1;
	void			*arg2;
};

int spdk_reactors_init(unsigned int max_delay_us);
void spdk_reactors_fini(void);

void spdk_reactors_start(void);
void spdk_reactors_stop(void *arg1, void *arg2);

struct spdk_subsystem {
	const char *name;
	/* User must call spdk_subsystem_init_next() when they are done with their initialization. */
	void (*init)(void);
	void (*fini)(void);
	void (*config)(FILE *fp);

	/**
	 * Write JSON configuration handler.
	 *
	 * \param w JSON write context
	 * \param done_ev Done event to be called when writing is done.
	 */
	void (*write_config_json)(struct spdk_json_write_ctx *w, struct spdk_event *done_ev);
	TAILQ_ENTRY(spdk_subsystem) tailq;
};

TAILQ_HEAD(spdk_subsystem_list, spdk_subsystem);
extern struct spdk_subsystem_list g_subsystems;

struct spdk_subsystem *spdk_subsystem_find(struct spdk_subsystem_list *list, const char *name);

struct spdk_subsystem_depend {
	const char *name;
	const char *depends_on;
	TAILQ_ENTRY(spdk_subsystem_depend) tailq;
};

TAILQ_HEAD(spdk_subsystem_depend_list, spdk_subsystem_depend);
extern struct spdk_subsystem_depend_list g_subsystems_deps;

void spdk_add_subsystem(struct spdk_subsystem *subsystem);
void spdk_add_subsystem_depend(struct spdk_subsystem_depend *depend);

void spdk_subsystem_init(struct spdk_event *app_start_event);
void spdk_subsystem_fini(struct spdk_event *app_finish_event);
void spdk_subsystem_init_next(int rc);
void spdk_subsystem_fini_next(void);
void spdk_subsystem_config(FILE *fp);

/**
 * Save pointed \c subsystem configuration to the JSON write context \c w. In case of
 * error \c null is written to the JSON context. Writing might be done in async way
 * so caller need to pass event that subsystem will call when it finish writing
 * configuration.
 *
 * \param w JSON write context
 * \param subsystem the subsystem to query
 * \param done_ev event to be called when writing is done
 */
void spdk_subsystem_config_json(struct spdk_json_write_ctx *w, struct spdk_subsystem *subsystem,
				struct spdk_event *done_ev);

void spdk_rpc_initialize(const char *listen_addr);
void spdk_rpc_finish(void);

/**
 * \brief Register a new subsystem
 */
#define SPDK_SUBSYSTEM_REGISTER(_name) \
	__attribute__((constructor)) static void _name ## _register(void)	\
	{									\
		spdk_add_subsystem(&_name);					\
	}

/**
 * \brief Declare that a subsystem depends on another subsystem.
 */
#define SPDK_SUBSYSTEM_DEPEND(_name, _depends_on)						\
	static struct spdk_subsystem_depend __subsystem_ ## _name ## _depend_on ## _depends_on = { \
	.name = #_name,										\
	.depends_on = #_depends_on,								\
	};											\
	__attribute__((constructor)) static void _name ## _depend_on ## _depends_on(void)	\
	{											\
		spdk_add_subsystem_depend(&__subsystem_ ## _name ## _depend_on ## _depends_on); \
	}

#endif /* SPDK_INTERNAL_EVENT_H */
