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

SPDK_LOG_REGISTER_COMPONENT(trace)

uint64_t
spdk_trace_get_tpoint_mask(uint32_t group_id)
{
	if (group_id >= SPDK_TRACE_MAX_GROUP_ID) {
		SPDK_ERRLOG("invalid group ID %d\n", group_id);
		return 0ULL;
	}

	if (g_trace_flags == NULL) {
		return 0ULL;
	}

	return g_trace_flags->tpoint_mask[group_id];
}

void
spdk_trace_set_tpoints(uint32_t group_id, uint64_t tpoint_mask)
{
	if (g_trace_flags == NULL) {
		SPDK_ERRLOG("trace is not initialized\n");
		return;
	}

	if (group_id >= SPDK_TRACE_MAX_GROUP_ID) {
		SPDK_ERRLOG("invalid group ID %d\n", group_id);
		return;
	}

	g_trace_flags->tpoint_mask[group_id] |= tpoint_mask;
}

void
spdk_trace_clear_tpoints(uint32_t group_id, uint64_t tpoint_mask)
{
	if (g_trace_flags == NULL) {
		SPDK_ERRLOG("trace is not initialized\n");
		return;
	}

	if (group_id >= SPDK_TRACE_MAX_GROUP_ID) {
		SPDK_ERRLOG("invalid group ID %d\n", group_id);
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

	if (g_trace_flags == NULL) {
		SPDK_ERRLOG("trace is not initialized\n");
		return;
	}

	for (i = 0; i < SPDK_TRACE_MAX_GROUP_ID; i++) {
		if (tpoint_group_mask & (1ULL << i)) {
			spdk_trace_set_tpoints(i, -1ULL);
		}
	}
}

void
spdk_trace_clear_tpoint_group_mask(uint64_t tpoint_group_mask)
{
	int i;

	if (g_trace_flags == NULL) {
		SPDK_ERRLOG("trace is not initialized\n");
		return;
	}

	for (i = 0; i < SPDK_TRACE_MAX_GROUP_ID; i++) {
		if (tpoint_group_mask & (1ULL << i)) {
			spdk_trace_clear_tpoints(i, -1ULL);
		}
	}
}

struct spdk_trace_register_fn *
spdk_trace_get_first_register_fn(void)
{
	return g_reg_fn_head;
}

struct spdk_trace_register_fn *
spdk_trace_get_next_register_fn(struct spdk_trace_register_fn *register_fn)
{
	return register_fn->next;
}

static uint64_t
trace_create_tpoint_group_mask(const char *group_name)
{
	uint64_t tpoint_group_mask = 0;
	struct spdk_trace_register_fn *register_fn;

	register_fn = spdk_trace_get_first_register_fn();
	if (strcmp(group_name, "all") == 0) {
		while (register_fn) {
			tpoint_group_mask |= (1UL << register_fn->tgroup_id);

			register_fn = spdk_trace_get_next_register_fn(register_fn);
		}
	} else {
		while (register_fn) {
			if (strcmp(group_name, register_fn->name) == 0) {
				break;
			}

			register_fn = spdk_trace_get_next_register_fn(register_fn);
		}

		if (register_fn != NULL) {
			tpoint_group_mask |= (1UL << register_fn->tgroup_id);
		}
	}

	return tpoint_group_mask;
}

int
spdk_trace_enable_tpoint_group(const char *group_name)
{
	uint64_t tpoint_group_mask = 0;

	if (g_trace_flags == NULL) {
		return -1;
	}

	tpoint_group_mask = trace_create_tpoint_group_mask(group_name);
	if (tpoint_group_mask == 0) {
		return -1;
	}

	spdk_trace_set_tpoint_group_mask(tpoint_group_mask);
	return 0;
}

int
spdk_trace_disable_tpoint_group(const char *group_name)
{
	uint64_t tpoint_group_mask = 0;

	if (g_trace_flags == NULL) {
		return -1;
	}

	tpoint_group_mask = trace_create_tpoint_group_mask(group_name);
	if (tpoint_group_mask == 0) {
		return -1;
	}

	spdk_trace_clear_tpoint_group_mask(tpoint_group_mask);
	return 0;
}

void
spdk_trace_mask_usage(FILE *f, const char *tmask_arg)
{
	struct spdk_trace_register_fn *register_fn;

	fprintf(f, " %s, --tpoint-group-mask <mask>\n", tmask_arg);
	fprintf(f, "                           tracepoint group mask for spdk trace buffers (default 0x0");

	register_fn = g_reg_fn_head;
	while (register_fn) {
		fprintf(f, ", %s 0x%x", register_fn->name, 1 << register_fn->tgroup_id);
		register_fn = register_fn->next;
	}

	fprintf(f, ", all 0xffff)\n");
}

void
spdk_trace_register_owner(uint8_t type, char id_prefix)
{
	struct spdk_trace_owner *owner;

	assert(type != OWNER_NONE);

	if (g_trace_flags == NULL) {
		SPDK_ERRLOG("trace is not initialized\n");
		return;
	}

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

	if (g_trace_flags == NULL) {
		SPDK_ERRLOG("trace is not initialized\n");
		return;
	}

	/* 'object' has 256 entries and since 'type' is a uint8_t, it
	 * can't overrun the array.
	 */
	object = &g_trace_flags->object[type];
	assert(object->type == 0);

	object->type = type;
	object->id_prefix = id_prefix;
}

void
spdk_trace_register_description(const char *name, uint16_t tpoint_id, uint8_t owner_type,
				uint8_t object_type, uint8_t new_object,
				uint8_t arg1_type, const char *arg1_name)
{
	struct spdk_trace_tpoint *tpoint;

	assert(tpoint_id != 0);
	assert(tpoint_id < SPDK_TRACE_MAX_TPOINT_ID);

	if (g_trace_flags == NULL) {
		SPDK_ERRLOG("trace is not initialized\n");
		return;
	}

	if (strnlen(name, sizeof(tpoint->name)) == sizeof(tpoint->name)) {
		SPDK_ERRLOG("name (%s) too long\n", name);
	}

	tpoint = &g_trace_flags->tpoint[tpoint_id];
	assert(tpoint->tpoint_id == 0);

	snprintf(tpoint->name, sizeof(tpoint->name), "%s", name);
	tpoint->tpoint_id = tpoint_id;
	tpoint->object_type = object_type;
	tpoint->owner_type = owner_type;
	tpoint->new_object = new_object;
	tpoint->arg1_type = arg1_type;
	snprintf(tpoint->arg1_name, sizeof(tpoint->arg1_name), "%s", arg1_name);
}

void
spdk_trace_add_register_fn(struct spdk_trace_register_fn *reg_fn)
{
	struct spdk_trace_register_fn *_reg_fn;

	if (reg_fn->name == NULL) {
		SPDK_ERRLOG("missing name for registering spdk trace tpoint group\n");
		assert(false);
		return;
	}

	if (strcmp(reg_fn->name, "all") == 0) {
		SPDK_ERRLOG("illegal name (%s) for tpoint group\n", reg_fn->name);
		assert(false);
		return;
	}

	/* Ensure that no trace point group IDs and names are ever duplicated */
	for (_reg_fn = g_reg_fn_head; _reg_fn; _reg_fn = _reg_fn->next) {
		if (reg_fn->tgroup_id == _reg_fn->tgroup_id) {
			SPDK_ERRLOG("duplicate tgroup_id (%d) with %s\n", _reg_fn->tgroup_id, _reg_fn->name);
			assert(false);
			return;
		}

		if (strcmp(reg_fn->name, _reg_fn->name) == 0) {
			SPDK_ERRLOG("duplicate name with %s\n", _reg_fn->name);
			assert(false);
			return;
		}
	}

	/* Arrange trace registration in order on tgroup_id */
	if (g_reg_fn_head == NULL || reg_fn->tgroup_id < g_reg_fn_head->tgroup_id) {
		reg_fn->next = g_reg_fn_head;
		g_reg_fn_head = reg_fn;
		return;
	}

	for (_reg_fn = g_reg_fn_head; _reg_fn; _reg_fn = _reg_fn->next) {
		if (_reg_fn->next == NULL || reg_fn->tgroup_id < _reg_fn->next->tgroup_id) {
			reg_fn->next = _reg_fn->next;
			_reg_fn->next = reg_fn;
			return;
		}
	}
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
