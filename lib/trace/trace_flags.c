/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/trace.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "trace_internal.h"
#include "spdk/bit_array.h"

static struct spdk_trace_register_fn *g_reg_fn_head = NULL;
static struct {
	uint16_t *ring;
	uint32_t head;
	uint32_t tail;
	uint32_t size;
	pthread_spinlock_t lock;
} g_owner_ids;

SPDK_LOG_REGISTER_COMPONENT(trace)

uint64_t
spdk_trace_get_tpoint_mask(uint32_t group_id)
{
	if (group_id >= SPDK_TRACE_MAX_GROUP_ID) {
		SPDK_ERRLOG("invalid group ID %d\n", group_id);
		return 0ULL;
	}

	if (g_trace_file == NULL) {
		return 0ULL;
	}

	return g_trace_file->tpoint_mask[group_id];
}

void
spdk_trace_set_tpoints(uint32_t group_id, uint64_t tpoint_mask)
{
	if (g_trace_file == NULL) {
		SPDK_ERRLOG("trace is not initialized\n");
		return;
	}

	if (group_id >= SPDK_TRACE_MAX_GROUP_ID) {
		SPDK_ERRLOG("invalid group ID %d\n", group_id);
		return;
	}

	g_trace_file->tpoint_mask[group_id] |= tpoint_mask;
}

void
spdk_trace_clear_tpoints(uint32_t group_id, uint64_t tpoint_mask)
{
	if (g_trace_file == NULL) {
		SPDK_ERRLOG("trace is not initialized\n");
		return;
	}

	if (group_id >= SPDK_TRACE_MAX_GROUP_ID) {
		SPDK_ERRLOG("invalid group ID %d\n", group_id);
		return;
	}

	g_trace_file->tpoint_mask[group_id] &= ~tpoint_mask;
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

	if (g_trace_file == NULL) {
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

	if (g_trace_file == NULL) {
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

uint64_t
spdk_trace_create_tpoint_group_mask(const char *group_name)
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

	if (g_trace_file == NULL) {
		return -1;
	}

	tpoint_group_mask = spdk_trace_create_tpoint_group_mask(group_name);
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

	if (g_trace_file == NULL) {
		return -1;
	}

	tpoint_group_mask = spdk_trace_create_tpoint_group_mask(group_name);
	if (tpoint_group_mask == 0) {
		return -1;
	}

	spdk_trace_clear_tpoint_group_mask(tpoint_group_mask);
	return 0;
}

void
spdk_trace_mask_usage(FILE *f, const char *tmask_arg)
{
#define LINE_PREFIX			"                           "
#define ENTRY_SEPARATOR			", "
#define MAX_LINE_LENGTH			100
	uint64_t prefix_len = strlen(LINE_PREFIX);
	uint64_t separator_len = strlen(ENTRY_SEPARATOR);
	const char *first_entry = "group_name - tracepoint group name for spdk trace buffers (";
	const char *last_entry = "all).";
	uint64_t curr_line_len;
	uint64_t curr_entry_len;
	struct spdk_trace_register_fn *register_fn;

	fprintf(f, " %s, --tpoint-group <group-name>[:<tpoint_mask>]\n", tmask_arg);
	fprintf(f, "%s%s", LINE_PREFIX, first_entry);
	curr_line_len = prefix_len + strlen(first_entry);

	register_fn = g_reg_fn_head;
	while (register_fn) {
		curr_entry_len = strlen(register_fn->name);
		if ((curr_line_len + curr_entry_len + separator_len > MAX_LINE_LENGTH)) {
			fprintf(f, "\n%s", LINE_PREFIX);
			curr_line_len = prefix_len;
		}

		fprintf(f, "%s%s", register_fn->name, ENTRY_SEPARATOR);
		curr_line_len += curr_entry_len + separator_len;

		if (register_fn->next == NULL) {
			if (curr_line_len + strlen(last_entry) > MAX_LINE_LENGTH) {
				fprintf(f, " ");
			}
			fprintf(f, "%s\n", last_entry);
			break;
		}

		register_fn = register_fn->next;
	}

	fprintf(f, "%stpoint_mask - tracepoint mask for enabling individual tpoints inside\n",
		LINE_PREFIX);
	fprintf(f, "%sa tracepoint group. First tpoint inside a group can be enabled by\n",
		LINE_PREFIX);
	fprintf(f, "%ssetting tpoint_mask to 1 (e.g. bdev:0x1). Groups and masks can be\n",
		LINE_PREFIX);
	fprintf(f, "%scombined (e.g. thread,bdev:0x1). All available tpoints can be found\n",
		LINE_PREFIX);
	fprintf(f, "%sin /include/spdk_internal/trace_defs.h\n", LINE_PREFIX);
}

void
spdk_trace_register_owner_type(uint8_t type, char id_prefix)
{
	struct spdk_trace_owner_type *owner_type;

	assert(type != OWNER_TYPE_NONE);

	if (g_trace_file == NULL) {
		SPDK_ERRLOG("trace is not initialized\n");
		return;
	}

	/* 'owner_type' has 256 entries and since 'type' is a uint8_t, it
	 * can't overrun the array.
	 */
	owner_type = &g_trace_file->owner_type[type];
	assert(owner_type->type == 0);

	owner_type->type = type;
	owner_type->id_prefix = id_prefix;
}

static void
_owner_set_description(uint16_t owner_id, const char *description, bool append)
{
	struct spdk_trace_owner *owner;
	char old[256] = {};

	assert(sizeof(old) >= g_trace_file->owner_description_size);
	owner = spdk_get_trace_owner(g_trace_file, owner_id);
	assert(owner != NULL);
	if (append) {
		memcpy(old, owner->description, g_trace_file->owner_description_size);
	}

	snprintf(owner->description, g_trace_file->owner_description_size,
		 "%s%s%s", old, append ? " " : "", description);
}

uint16_t
spdk_trace_register_owner(uint8_t owner_type, const char *description)
{
	struct spdk_trace_owner *owner;
	uint32_t owner_id;

	if (g_owner_ids.ring == NULL) {
		/* Help the unit test environment by simply returning instead
		 * of requiring it to initialize the trace library.
		 */
		return 0;
	}

	pthread_spin_lock(&g_owner_ids.lock);

	if (g_owner_ids.head == g_owner_ids.tail) {
		/* No owner ids available. Return 0 which means no owner. */
		pthread_spin_unlock(&g_owner_ids.lock);
		return 0;
	}

	owner_id = g_owner_ids.ring[g_owner_ids.head];
	if (++g_owner_ids.head == g_owner_ids.size) {
		g_owner_ids.head = 0;
	}

	owner = spdk_get_trace_owner(g_trace_file, owner_id);
	owner->tsc = spdk_get_ticks();
	owner->type = owner_type;
	_owner_set_description(owner_id, description, false);
	pthread_spin_unlock(&g_owner_ids.lock);
	return owner_id;
}

void
spdk_trace_unregister_owner(uint16_t owner_id)
{
	if (g_owner_ids.ring == NULL) {
		/* Help the unit test environment by simply returning instead
		 * of requiring it to initialize the trace library.
		 */
		return;
	}

	if (owner_id == 0) {
		/* owner_id 0 means no owner. Allow this to be passed here, it
		 * avoids caller having to do extra checking.
		 */
		return;
	}

	pthread_spin_lock(&g_owner_ids.lock);
	g_owner_ids.ring[g_owner_ids.tail] = owner_id;
	if (++g_owner_ids.tail == g_owner_ids.size) {
		g_owner_ids.tail = 0;
	}
	pthread_spin_unlock(&g_owner_ids.lock);
}

void
spdk_trace_owner_set_description(uint16_t owner_id, const char *description)
{
	if (g_owner_ids.ring == NULL) {
		/* Help the unit test environment by simply returning instead
		 * of requiring it to initialize the trace library.
		 */
		return;
	}

	pthread_spin_lock(&g_owner_ids.lock);
	_owner_set_description(owner_id, description, false);
	pthread_spin_unlock(&g_owner_ids.lock);
}

void
spdk_trace_owner_append_description(uint16_t owner_id, const char *description)
{
	if (g_owner_ids.ring == NULL) {
		/* Help the unit test environment by simply returning instead
		 * of requiring it to initialize the trace library.
		 */
		return;
	}

	if (owner_id == 0) {
		/* owner_id 0 means no owner. Allow this to be passed here, it
		 * avoids caller having to do extra checking.
		 */
		return;
	}

	pthread_spin_lock(&g_owner_ids.lock);
	_owner_set_description(owner_id, description, true);
	pthread_spin_unlock(&g_owner_ids.lock);
}

void
spdk_trace_register_object(uint8_t type, char id_prefix)
{
	struct spdk_trace_object *object;

	assert(type != OBJECT_NONE);

	if (g_trace_file == NULL) {
		SPDK_ERRLOG("trace is not initialized\n");
		return;
	}

	/* 'object' has 256 entries and since 'type' is a uint8_t, it
	 * can't overrun the array.
	 */
	object = &g_trace_file->object[type];
	assert(object->type == 0);

	object->type = type;
	object->id_prefix = id_prefix;
}

static void
trace_register_description(const struct spdk_trace_tpoint_opts *opts)
{
	struct spdk_trace_tpoint *tpoint;
	size_t i, max_name_length;

	assert(opts->tpoint_id < SPDK_TRACE_MAX_TPOINT_ID);

	if (strnlen(opts->name, sizeof(tpoint->name)) == sizeof(tpoint->name)) {
		SPDK_ERRLOG("name (%s) too long\n", opts->name);
	}

	tpoint = &g_trace_file->tpoint[opts->tpoint_id];
	assert(tpoint->tpoint_id == 0);

	snprintf(tpoint->name, sizeof(tpoint->name), "%s", opts->name);
	tpoint->tpoint_id = opts->tpoint_id;
	tpoint->object_type = opts->object_type;
	tpoint->owner_type = opts->owner_type;
	tpoint->new_object = opts->new_object;

	max_name_length = sizeof(tpoint->args[0].name);
	for (i = 0; i < SPDK_TRACE_MAX_ARGS_COUNT; ++i) {
		if (!opts->args[i].name || opts->args[i].name[0] == '\0') {
			break;
		}

		switch (opts->args[i].type) {
		case SPDK_TRACE_ARG_TYPE_INT:
		case SPDK_TRACE_ARG_TYPE_PTR:
			/* The integers and pointers have to be exactly 4 or 8 bytes */
			assert(opts->args[i].size == 4 || opts->args[i].size == 8);
			break;
		case SPDK_TRACE_ARG_TYPE_STR:
			/* Strings need to have at least one byte for the NULL terminator */
			assert(opts->args[i].size > 0);
			break;
		default:
			assert(0 && "invalid trace argument type");
			break;
		}

		if (strnlen(opts->args[i].name, max_name_length) == max_name_length) {
			SPDK_ERRLOG("argument name (%s) is too long\n", opts->args[i].name);
		}

		snprintf(tpoint->args[i].name, sizeof(tpoint->args[i].name),
			 "%s", opts->args[i].name);
		tpoint->args[i].type = opts->args[i].type;
		tpoint->args[i].size = opts->args[i].size;
	}

	tpoint->num_args = i;
}

void
spdk_trace_register_description_ext(const struct spdk_trace_tpoint_opts *opts, size_t num_opts)
{
	size_t i;

	if (g_trace_file == NULL) {
		SPDK_ERRLOG("trace is not initialized\n");
		return;
	}

	for (i = 0; i < num_opts; ++i) {
		trace_register_description(&opts[i]);
	}
}

void
spdk_trace_register_description(const char *name, uint16_t tpoint_id, uint8_t owner_type,
				uint8_t object_type, uint8_t new_object,
				uint8_t arg1_type, const char *arg1_name)
{
	struct spdk_trace_tpoint_opts opts = {
		.name = name,
		.tpoint_id = tpoint_id,
		.owner_type = owner_type,
		.object_type = object_type,
		.new_object = new_object,
		.args = {{
				.name = arg1_name,
				.type = arg1_type,
				.size = sizeof(uint64_t)
			}
		}
	};

	spdk_trace_register_description_ext(&opts, 1);
}

void
spdk_trace_tpoint_register_relation(uint16_t tpoint_id, uint8_t object_type, uint8_t arg_index)
{
	struct spdk_trace_tpoint *tpoint;
	uint16_t i;

	assert(object_type != OBJECT_NONE);
	assert(tpoint_id != OBJECT_NONE);

	if (g_trace_file == NULL) {
		SPDK_ERRLOG("trace is not initialized\n");
		return;
	}

	/* We do not check whether a tpoint_id exists here, because
	 * there is no order in which trace definitions are registered.
	 * This way we can create relations between tpoint and objects
	 * that will be declared later. */
	tpoint = &g_trace_file->tpoint[tpoint_id];
	for (i = 0; i < SPDK_COUNTOF(tpoint->related_objects); ++i) {
		if (tpoint->related_objects[i].object_type == OBJECT_NONE) {
			tpoint->related_objects[i].object_type = object_type;
			tpoint->related_objects[i].arg_index = arg_index;
			return;
		}
	}
	SPDK_ERRLOG("Unable to register new relation for tpoint %" PRIu16 ", object %" PRIu8 "\n",
		    tpoint_id, object_type);
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
			SPDK_ERRLOG("group %d, %s has duplicate tgroup_id with %s\n",
				    reg_fn->tgroup_id, reg_fn->name, _reg_fn->name);
			assert(false);
			return;
		}

		if (strcmp(reg_fn->name, _reg_fn->name) == 0) {
			SPDK_ERRLOG("name %s is duplicated between groups with ids %d and %d\n",
				    reg_fn->name, reg_fn->tgroup_id, _reg_fn->tgroup_id);
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

int
trace_flags_init(void)
{
	struct spdk_trace_register_fn *reg_fn;
	uint16_t i;
	uint16_t owner_id_start;
	int rc;

	reg_fn = g_reg_fn_head;
	while (reg_fn) {
		reg_fn->reg_fn();
		reg_fn = reg_fn->next;
	}

	/* We will not use owner_id 0, it will be reserved to mean "no owner".
	 * But for now, we will start with owner_id 256 instead of owner_id 1.
	 * This will account for some libraries and modules which pass a
	 * "poller_id" to spdk_trace_record() which is now an owner_id. Until
	 * all of those libraries and modules are converted, we will start
	 * owner_ids at 256 to avoid collisions.
	 */
	owner_id_start = 256;
	g_owner_ids.ring = calloc(g_trace_file->num_owners, sizeof(uint16_t));
	if (g_owner_ids.ring == NULL) {
		SPDK_ERRLOG("could not allocate g_owner_ids.ring\n");
		return -ENOMEM;
	}
	g_owner_ids.head = 0;
	g_owner_ids.tail = g_trace_file->num_owners - owner_id_start;
	g_owner_ids.size = g_trace_file->num_owners;
	for (i = 0; i < g_owner_ids.tail; i++) {
		g_owner_ids.ring[i] = i + owner_id_start;
	}

	rc = pthread_spin_init(&g_owner_ids.lock, PTHREAD_PROCESS_PRIVATE);
	if (rc != 0) {
		free(g_owner_ids.ring);
		g_owner_ids.ring = NULL;
	}

	return rc;
}

void
trace_flags_fini(void)
{
	if (g_owner_ids.ring == NULL) {
		return;
	}
	pthread_spin_destroy(&g_owner_ids.lock);
	free(g_owner_ids.ring);
	g_owner_ids.ring = NULL;
}
