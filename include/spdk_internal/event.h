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

struct spdk_event {
	uint32_t		lcore;
	spdk_event_fn		fn;
	void			*arg1;
	void			*arg2;
};

int spdk_reactors_init(unsigned int max_delay_us);
int spdk_reactors_fini(void);

void spdk_reactors_start(void);
void spdk_reactors_stop(void);

struct spdk_subsystem {
	const char *name;
	/* User must call spdk_subsystem_init_next() when they are done with their initialization. */
	void (*init)(void);
	int (*fini)(void);
	void (*config)(FILE *fp);
	TAILQ_ENTRY(spdk_subsystem) tailq;
};

struct spdk_subsystem_depend {
	const char *name;
	const char *depends_on;
	struct spdk_subsystem *depends_on_subsystem;
	TAILQ_ENTRY(spdk_subsystem_depend) tailq;
};

void spdk_add_subsystem(struct spdk_subsystem *subsystem);
void spdk_add_subsystem_depend(struct spdk_subsystem_depend *depend);

void spdk_subsystem_init(void *arg1, void *arg2);
int spdk_subsystem_fini(void);
void spdk_subsystem_init_next(int rc);
void spdk_subsystem_config(FILE *fp);

/**
 * \brief Register a new subsystem
 */
#define SPDK_SUBSYSTEM_REGISTER(_name, _init, _fini, _config)			\
	struct spdk_subsystem __spdk_subsystem_ ## _name = {			\
	.name = #_name,								\
	.init = _init,								\
	.fini = _fini,								\
	.config = _config,							\
	};									\
	__attribute__((constructor)) static void _name ## _register(void)	\
	{									\
		spdk_add_subsystem(&__spdk_subsystem_ ## _name);		\
	}

/**
 * \brief Declare that a subsystem depends on another subsystem.
 */
#define SPDK_SUBSYSTEM_DEPEND(_name, _depends_on)						\
	extern struct spdk_subsystem __spdk_subsystem_ ## _depends_on;				\
	static struct spdk_subsystem_depend __subsystem_ ## _name ## _depend_on ## _depends_on = { \
	.name = #_name,										\
	.depends_on = #_depends_on,								\
	.depends_on_subsystem = &__spdk_subsystem_ ## _depends_on,				\
	};											\
	__attribute__((constructor)) static void _name ## _depend_on ## _depends_on(void)	\
	{											\
		spdk_add_subsystem_depend(&__subsystem_ ## _name ## _depend_on ## _depends_on); \
	}

#endif /* SPDK_INTERNAL_EVENT_H */
