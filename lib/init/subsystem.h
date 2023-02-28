/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.  All rights reserved.
 */

#ifndef SPDK_SUBSYSTEM_H
#define SPDK_SUBSYSTEM_H

struct spdk_subsystem *subsystem_find(const char *name);
struct spdk_subsystem *subsystem_get_first(void);
struct spdk_subsystem *subsystem_get_next(struct spdk_subsystem *cur_subsystem);

struct spdk_subsystem_depend *subsystem_get_first_depend(void);
struct spdk_subsystem_depend *subsystem_get_next_depend(struct spdk_subsystem_depend
		*cur_depend);

/**
 * Save pointed \c subsystem configuration to the JSON write context \c w. In case of
 * error \c null is written to the JSON context.
 *
 * \param w JSON write context
 * \param subsystem the subsystem to query
 */
void subsystem_config_json(struct spdk_json_write_ctx *w, struct spdk_subsystem *subsystem);

#endif
