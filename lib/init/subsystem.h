/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.  All rights reserved.
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
