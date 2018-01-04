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

#include "spdk/env.h"
#include "spdk/trace.h"
#include "spdk/log.h"

struct spdk_trace_flags *g_trace_flags = NULL;
static struct spdk_trace_register_fn *g_reg_fn_head = NULL;

uint64_t
spdk_trace_get_tpoint_mask(uint32_t group_id)
{
	if (group_id >= SPDK_TRACE_MAX_GROUP_ID) {
		SPDK_ERRLOG("%s: invalid group ID %d\n", __func__, group_id);
		return 0ULL;
	}

	return g_trace_flags->tpoint_mask[group_id];
}

void
spdk_trace_set_tpoints(uint32_t group_id, uint64_t tpoint_mask)
{
	if (group_id >= SPDK_TRACE_MAX_GROUP_ID) {
		SPDK_ERRLOG("%s: invalid group ID %d\n", __func__, group_id);
		return;
	}

	g_trace_flags->tpoint_mask[group_id] |= tpoint_mask;
}

void
spdk_trace_clear_tpoints(uint32_t group_id, uint64_t tpoint_mask)
{
	if (group_id >= SPDK_TRACE_MAX_GROUP_ID) {
		SPDK_ERRLOG("%s: invalid group ID %d\n", __func__, group_id);
		return;
	}

	g_trace_flags->tpoint_mask[group_id] &= ~tpoint_mask;
}

uint64_t
spdk_trace_get_tpoint_group_mask(void)
{
	uint64_t mask = 0x0;
	int i;

	for (i = 0; i < SPDK_TRACE_MAX_GROUP_ID; i++) {
		if (spdk_trace_get_tpoint_mask(i) != 0) {
			mask |= (1ULL << i);
		}
	}

	return mask;
}

void
spdk_trace_set_tpoint_group_mask(uint64_t tpoint_group_mask)
{
	int i;

	for (i = 0; i < 64; i++) {
		if (tpoint_group_mask & (1ULL << i)) {
			spdk_trace_set_tpoints(i, -1ULL);
		}
	}
}

void
spdk_trace_register_owner(uint8_t type, char id_prefix)
{
	struct spdk_trace_owner *owner;

	assert(type != OWNER_NONE);

	/* 'owner' has 256 entries and since 'type' is a uint8_t, it
	 * can't overrun the array.
	 */
	owner = &g_trace_flags->owner[type];
	assert(owner->type == 0);

	owner->type = type;
	owner->id_prefix = id_prefix;
}

void
spdk_trace_register_object(uint8_t type, char id_prefix)
{
	struct spdk_trace_object *object;

	assert(type != OBJECT_NONE);

	/* 'object' has 256 entries and since 'type' is a uint8_t, it
	 * can't overrun the array.
	 */
	object = &g_trace_flags->object[type];
	assert(object->type == 0);

	object->type = type;
	object->id_prefix = id_prefix;
}

void
spdk_trace_register_description(const char *name, const char *short_name,
				uint16_t tpoint_id, uint8_t owner_type,
				uint8_t object_type, uint8_t new_object,
				uint8_t arg1_is_ptr, uint8_t arg1_is_alias,
				const char *arg1_name)
{
	struct spdk_trace_tpoint *tpoint;

	assert(tpoint_id != 0);
	assert(tpoint_id < SPDK_TRACE_MAX_TPOINT_ID);

	tpoint = &g_trace_flags->tpoint[tpoint_id];
	assert(tpoint->tpoint_id == 0);

	snprintf(tpoint->name, sizeof(tpoint->name), "%s", name);
	snprintf(tpoint->short_name, sizeof(tpoint->short_name), "%s", short_name);
	tpoint->tpoint_id = tpoint_id;
	tpoint->object_type = object_type;
	tpoint->owner_type = owner_type;
	tpoint->new_object = new_object;
	tpoint->arg1_is_ptr = arg1_is_ptr;
	tpoint->arg1_is_alias = arg1_is_alias;
	snprintf(tpoint->arg1_name, sizeof(tpoint->arg1_name), "%s", arg1_name);
}

void
spdk_trace_add_register_fn(struct spdk_trace_register_fn *reg_fn)
{
	reg_fn->next = g_reg_fn_head;
	g_reg_fn_head = reg_fn;
}


void
spdk_trace_flags_init(void)
{
	struct spdk_trace_register_fn *reg_fn;

	reg_fn = g_reg_fn_head;
	while (reg_fn) {
		reg_fn->reg_fn();
		reg_fn = reg_fn->next;
	}
}
