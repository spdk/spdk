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
#include "spdk/string.h"
#include "spdk/trace.h"

#include <rte_config.h>
#include <rte_lcore.h>

static char g_shm_name[64];

static struct spdk_trace_histories *g_trace_histories;
static struct spdk_trace_register_fn *g_reg_fn_head = NULL;

void
spdk_trace_record(uint16_t tpoint_id, uint16_t poller_id, uint32_t size,
		  uint64_t object_id, uint64_t arg1)
{
	struct spdk_trace_history *lcore_history;
	struct spdk_trace_entry *next_entry;
	uint64_t tsc;
	unsigned lcore;

	/*
	 * Tracepoint group ID is encoded in the tpoint_id.  Lower 6 bits determine the tracepoint
	 *  within the group, the remaining upper bits determine the tracepoint group.  Each
	 *  tracepoint group has its own tracepoint mask.
	 */
	if (g_trace_histories == NULL ||
	    !((1ULL << (tpoint_id & 0x3F)) & g_trace_histories->tpoint_mask[tpoint_id >> 6])) {
		return;
	}

	lcore = rte_lcore_id();
	if (lcore >= SPDK_TRACE_MAX_LCORE) {
		return;
	}

	lcore_history = &g_trace_histories->per_lcore_history[lcore];
	tsc = spdk_get_ticks();

	lcore_history->tpoint_count[tpoint_id]++;

	next_entry = &lcore_history->entries[lcore_history->next_entry];
	next_entry->tsc = tsc;
	next_entry->tpoint_id = tpoint_id;
	next_entry->poller_id = poller_id;
	next_entry->size = size;
	next_entry->object_id = object_id;
	next_entry->arg1 = arg1;

	lcore_history->next_entry++;
	if (lcore_history->next_entry == SPDK_TRACE_SIZE)
		lcore_history->next_entry = 0;
}

uint64_t
spdk_trace_get_tpoint_mask(uint32_t group_id)
{
	if (group_id >= SPDK_TRACE_MAX_GROUP_ID) {
		fprintf(stderr, "%s: invalid group ID %d\n", __func__, group_id);
		return 0ULL;
	}

	return g_trace_histories->tpoint_mask[group_id];
}

void
spdk_trace_set_tpoints(uint32_t group_id, uint64_t tpoint_mask)
{
	if (group_id >= SPDK_TRACE_MAX_GROUP_ID) {
		fprintf(stderr, "%s: invalid group ID %d\n", __func__, group_id);
		return;
	}

	g_trace_histories->tpoint_mask[group_id] |= tpoint_mask;
}

void
spdk_trace_clear_tpoints(uint32_t group_id, uint64_t tpoint_mask)
{
	if (group_id >= SPDK_TRACE_MAX_GROUP_ID) {
		fprintf(stderr, "%s: invalid group ID %d\n", __func__, group_id);
		return;
	}

	g_trace_histories->tpoint_mask[group_id] &= ~tpoint_mask;
}

uint64_t
spdk_trace_get_tpoint_group_mask(void)
{
	uint64_t mask = 0x0;
	int i;

	for (i = 0; i < 64; i++) {
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
spdk_trace_init(const char *shm_name)
{
	struct spdk_trace_register_fn *reg_fn;
	int trace_fd;
	int i = 0;
	char buf[64];

	snprintf(g_shm_name, sizeof(g_shm_name), "%s", shm_name);

	trace_fd = shm_open(shm_name, O_RDWR | O_CREAT, 0600);
	if (trace_fd == -1) {
		spdk_strerror_r(errno, buf, sizeof(buf));
		fprintf(stderr, "could not shm_open spdk_trace\n");
		fprintf(stderr, "errno=%d %s\n", errno, buf);
		exit(EXIT_FAILURE);
	}

	if (ftruncate(trace_fd, sizeof(*g_trace_histories)) != 0) {
		fprintf(stderr, "could not truncate shm\n");
		exit(EXIT_FAILURE);
	}

	g_trace_histories = mmap(NULL, sizeof(*g_trace_histories), PROT_READ | PROT_WRITE,
				 MAP_SHARED, trace_fd, 0);
	if (g_trace_histories == NULL) {
		fprintf(stderr, "could not mmap shm\n");
		exit(EXIT_FAILURE);
	}

	memset(g_trace_histories, 0, sizeof(*g_trace_histories));

	g_trace_histories->tsc_rate = spdk_get_ticks_hz();

	for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
		g_trace_histories->per_lcore_history[i].lcore = i;
	}

	reg_fn = g_reg_fn_head;
	while (reg_fn) {
		reg_fn->reg_fn();
		reg_fn = reg_fn->next;
	}
}

void
spdk_trace_cleanup(void)
{
	munmap(g_trace_histories, sizeof(struct spdk_trace_histories));
	shm_unlink(g_shm_name);
}

void
spdk_trace_register_owner(uint8_t type, char id_prefix)
{
	struct spdk_trace_owner *owner;

	assert(type != OWNER_NONE);

	/* 'owner' has 256 entries and since 'type' is a uint8_t, it
	 * can't overrun the array.
	 */
	owner = &g_trace_histories->owner[type];
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
	object = &g_trace_histories->object[type];
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

	tpoint = &g_trace_histories->tpoint[tpoint_id];
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
