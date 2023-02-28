/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.  All rights reserved.
 */

#ifndef SPDK_INIT_INTERNAL_H
#define SPDK_INIT_INTERNAL_H

#include "spdk/stdinc.h"
#include "spdk/queue.h"

struct spdk_json_write_ctx;

struct spdk_subsystem {
	const char *name;
	/* User must call spdk_subsystem_init_next() when they are done with their initialization. */
	void (*init)(void);
	void (*fini)(void);

	/**
	 * Write JSON configuration handler.
	 *
	 * \param w JSON write context
	 */
	void (*write_config_json)(struct spdk_json_write_ctx *w);
	TAILQ_ENTRY(spdk_subsystem) tailq;
};

struct spdk_subsystem_depend {
	const char *name;
	const char *depends_on;
	TAILQ_ENTRY(spdk_subsystem_depend) tailq;
};

void spdk_add_subsystem(struct spdk_subsystem *subsystem);
void spdk_add_subsystem_depend(struct spdk_subsystem_depend *depend);

void spdk_subsystem_init_next(int rc);
void spdk_subsystem_fini_next(void);

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

#endif
