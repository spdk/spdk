/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/util.h"
#include "spdk/barrier.h"
#include "spdk/log.h"
#include "spdk/cpuset.h"
#include "spdk/likely.h"
#include "spdk/bit_array.h"
#include "trace_internal.h"

static int g_trace_fd = -1;
static char g_shm_name[64];

static __thread uint32_t t_ut_array_index;
static __thread struct spdk_trace_history *t_ut_lcore_history;

static uint32_t g_user_thread_index_start;
struct spdk_trace_file *g_trace_file;
static struct spdk_bit_array *g_ut_array;
static pthread_mutex_t g_ut_array_mutex;

#define TRACE_NUM_OWNERS (16 * 1024)
#define TRACE_OWNER_DESCRIPTION_SIZE (119)
SPDK_STATIC_ASSERT(sizeof(struct spdk_trace_owner) == 9, "incorrect size");
SPDK_STATIC_ASSERT(sizeof(struct spdk_trace_owner) + TRACE_OWNER_DESCRIPTION_SIZE == 128,
		   "incorrect size");

static inline struct spdk_trace_entry *
get_trace_entry(struct spdk_trace_history *history, uint64_t offset)
{
	return &history->entries[offset & (history->num_entries - 1)];
}

void
_spdk_trace_record(uint64_t tsc, uint16_t tpoint_id, uint16_t owner_id, uint32_t size,
		   uint64_t object_id, int num_args, ...)
{
	struct spdk_trace_history *lcore_history;
	struct spdk_trace_entry *next_entry;
	struct spdk_trace_entry_buffer *buffer;
	struct spdk_trace_tpoint *tpoint;
	struct spdk_trace_argument *argument;
	unsigned lcore, i, offset, num_entries, arglen, argoff, curlen;
	uint64_t intval;
	void *argval;
	va_list vl;

	lcore = spdk_env_get_current_core();
	if (spdk_likely(lcore != SPDK_ENV_LCORE_ID_ANY)) {
		lcore_history = spdk_get_per_lcore_history(g_trace_file, lcore);
	} else if (t_ut_lcore_history != NULL) {
		lcore_history = t_ut_lcore_history;
	} else {
		return;
	}

	if (tsc == 0) {
		tsc = spdk_get_ticks();
	}

	lcore_history->tpoint_count[tpoint_id]++;

	tpoint = &g_trace_file->tpoint[tpoint_id];
	/* Make sure that the number of arguments passed matches tracepoint definition */
	if (spdk_unlikely(tpoint->num_args != num_args)) {
		assert(0 && "Unexpected number of tracepoint arguments");
		return;
	}

	/* Get next entry index in the circular buffer */
	next_entry = get_trace_entry(lcore_history, lcore_history->next_entry);
	next_entry->tsc = tsc;
	next_entry->tpoint_id = tpoint_id;
	next_entry->owner_id = owner_id;
	next_entry->size = size;
	next_entry->object_id = object_id;

	num_entries = 1;
	buffer = (struct spdk_trace_entry_buffer *)next_entry;
	/* The initial offset needs to be adjusted by the fields present in the first entry
	 * (owner_id, size, etc.).
	 */
	offset = offsetof(struct spdk_trace_entry, args) -
		 offsetof(struct spdk_trace_entry_buffer, data);

	va_start(vl, num_args);
	for (i = 0; i < tpoint->num_args; ++i) {
		argument = &tpoint->args[i];
		switch (argument->type) {
		case SPDK_TRACE_ARG_TYPE_STR:
			argval = va_arg(vl, void *);
			arglen = strnlen((const char *)argval, argument->size - 1) + 1;
			break;
		case SPDK_TRACE_ARG_TYPE_INT:
		case SPDK_TRACE_ARG_TYPE_PTR:
			if (argument->size == 8) {
				intval = va_arg(vl, uint64_t);
			} else {
				intval = va_arg(vl, uint32_t);
			}
			argval = &intval;
			arglen = argument->size;
			break;
		default:
			assert(0 && "Invalid trace argument type");
			return;
		}

		/* Copy argument's data. For some argument types (strings) user is allowed to pass a
		 * value that is either larger or smaller than what's defined in the tracepoint's
		 * description. If the value is larger, we'll truncate it, while if it's smaller,
		 * we'll only fill portion of the buffer, without touching the rest. For instance,
		 * if the definition marks an argument as 40B and user passes 12B string, we'll only
		 * copy 13B (accounting for the NULL terminator).
		 */
		argoff = 0;
		while (argoff < argument->size) {
			/* Current buffer is full, we need to acquire another one */
			if (spdk_unlikely(offset == sizeof(buffer->data))) {
				buffer = (struct spdk_trace_entry_buffer *) get_trace_entry(
						 lcore_history,
						 lcore_history->next_entry + num_entries);
				buffer->tpoint_id = SPDK_TRACE_MAX_TPOINT_ID;
				buffer->tsc = tsc;
				num_entries++;
				offset = 0;
			}

			curlen = spdk_min(sizeof(buffer->data) - offset, argument->size - argoff);
			if (spdk_likely(argoff < arglen)) {
				assert(argval != NULL);
				memcpy(&buffer->data[offset], (uint8_t *)argval + argoff,
				       spdk_min(curlen, arglen - argoff));
			}

			offset += curlen;
			argoff += curlen;
		}

		/* Make sure that truncated strings are NULL-terminated */
		if (spdk_unlikely(argument->type == SPDK_TRACE_ARG_TYPE_STR)) {
			assert(offset > 0);
			buffer->data[offset - 1] = '\0';
		}
	}
	va_end(vl);

	/* Ensure all elements of the trace entry are visible to outside trace tools */
	spdk_smp_wmb();
	lcore_history->next_entry += num_entries;
}

int
spdk_trace_register_user_thread(void)
{
	int ret;
	uint32_t ut_index;
	pthread_t tid;

	if (!g_ut_array) {
		SPDK_ERRLOG("user thread array not created\n");
		return -ENOMEM;
	}

	if (spdk_env_get_current_core() != SPDK_ENV_LCORE_ID_ANY) {
		SPDK_ERRLOG("cannot register an user thread from a dedicated cpu %d\n",
			    spdk_env_get_current_core());
		return -EINVAL;
	}

	pthread_mutex_lock(&g_ut_array_mutex);

	t_ut_array_index = spdk_bit_array_find_first_clear(g_ut_array, 0);
	if (t_ut_array_index == UINT32_MAX) {
		SPDK_ERRLOG("could not find an entry in the user thread array\n");
		pthread_mutex_unlock(&g_ut_array_mutex);
		return -ENOENT;
	}

	ut_index = t_ut_array_index + g_user_thread_index_start;

	t_ut_lcore_history = spdk_get_per_lcore_history(g_trace_file, ut_index);

	assert(t_ut_lcore_history != NULL);

	memset(g_trace_file->tname[ut_index], 0, SPDK_TRACE_THREAD_NAME_LEN);

	tid = pthread_self();
	ret = pthread_getname_np(tid, g_trace_file->tname[ut_index], SPDK_TRACE_THREAD_NAME_LEN);
	if (ret) {
		SPDK_ERRLOG("cannot get thread name\n");
		pthread_mutex_unlock(&g_ut_array_mutex);
		return ret;
	}

	spdk_bit_array_set(g_ut_array, t_ut_array_index);

	pthread_mutex_unlock(&g_ut_array_mutex);

	return 0;
}

int
spdk_trace_unregister_user_thread(void)
{
	if (!g_ut_array) {
		SPDK_ERRLOG("user thread array not created\n");
		return -ENOMEM;
	}

	if (spdk_env_get_current_core() != SPDK_ENV_LCORE_ID_ANY) {
		SPDK_ERRLOG("cannot unregister an user thread from a dedicated cpu %d\n",
			    spdk_env_get_current_core());
		return -EINVAL;
	}

	pthread_mutex_lock(&g_ut_array_mutex);

	spdk_bit_array_clear(g_ut_array, t_ut_array_index);

	pthread_mutex_unlock(&g_ut_array_mutex);

	return 0;
}

int
spdk_trace_init(const char *shm_name, uint64_t num_entries, uint32_t num_threads)
{
	uint32_t i = 0, max_dedicated_cpu = 0;
	uint64_t file_size;
	uint64_t lcore_offsets[SPDK_TRACE_MAX_LCORE] = { 0 };
	uint64_t owner_offset;
	struct spdk_cpuset cpuset = {};

	/* 0 entries requested - skip trace initialization */
	if (num_entries == 0) {
		return 0;
	}

	if (num_threads >= SPDK_TRACE_MAX_LCORE) {
		SPDK_ERRLOG("cannot alloc trace entries for %d user threads\n", num_threads);
		SPDK_ERRLOG("supported maximum %d threads\n", SPDK_TRACE_MAX_LCORE - 1);
		return 1;
	}

	spdk_cpuset_zero(&cpuset);
	file_size = sizeof(struct spdk_trace_file);
	SPDK_ENV_FOREACH_CORE(i) {
		spdk_cpuset_set_cpu(&cpuset, i, true);
		lcore_offsets[i] = file_size;
		file_size += spdk_get_trace_history_size(num_entries);
		max_dedicated_cpu = i;
	}

	g_user_thread_index_start = max_dedicated_cpu + 1;

	if (g_user_thread_index_start + num_threads > SPDK_TRACE_MAX_LCORE) {
		SPDK_ERRLOG("user threads overlap with the threads on dedicated cpus\n");
		return 1;
	}

	g_ut_array = spdk_bit_array_create(num_threads);
	if (!g_ut_array) {
		SPDK_ERRLOG("could not create bit array for threads\n");
		return 1;
	}

	for (i = g_user_thread_index_start; i < g_user_thread_index_start + num_threads; i++) {
		lcore_offsets[i] = file_size;
		file_size += spdk_get_trace_history_size(num_entries);
	}
	owner_offset = file_size;
	file_size += TRACE_NUM_OWNERS *
		     (sizeof(struct spdk_trace_owner) + TRACE_OWNER_DESCRIPTION_SIZE);

	snprintf(g_shm_name, sizeof(g_shm_name), "%s", shm_name);

	g_trace_fd = shm_open(shm_name, O_RDWR | O_CREAT, 0600);
	if (g_trace_fd == -1) {
		SPDK_ERRLOG("could not shm_open spdk_trace\n");
		SPDK_ERRLOG("errno=%d %s\n", errno, spdk_strerror(errno));
		spdk_bit_array_free(&g_ut_array);
		return 1;
	}

	if (ftruncate(g_trace_fd, file_size) != 0) {
		SPDK_ERRLOG("could not truncate shm\n");
		goto trace_init_err;
	}

	g_trace_file = mmap(NULL, file_size, PROT_READ | PROT_WRITE,
			    MAP_SHARED, g_trace_fd, 0);
	if (g_trace_file == MAP_FAILED) {
		SPDK_ERRLOG("could not mmap shm\n");
		goto trace_init_err;
	}

	/* TODO: On FreeBSD, mlock on shm_open'd memory doesn't seem to work.  Docs say that kern.ipc.shm_use_phys=1
	 * should allow it, but forcing that doesn't seem to work either.  So for now just skip mlock on FreeBSD
	 * altogether.
	 */
#if defined(__linux__)
	if (mlock(g_trace_file, file_size) != 0) {
		SPDK_ERRLOG("Could not mlock shm for tracing - %s.\n", spdk_strerror(errno));
		if (errno == ENOMEM) {
			SPDK_ERRLOG("Check /dev/shm for old tracing files that can be deleted.\n");
		}
		goto trace_init_err;
	}
#endif

	memset(g_trace_file, 0, file_size);

	g_trace_file->tsc_rate = spdk_get_ticks_hz();

	for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
		struct spdk_trace_history *lcore_history;

		g_trace_file->lcore_history_offsets[i] = lcore_offsets[i];
		if (lcore_offsets[i] == 0) {
			continue;
		}

		if (i <= max_dedicated_cpu) {
			assert(spdk_cpuset_get_cpu(&cpuset, i));
		}

		lcore_history = spdk_get_per_lcore_history(g_trace_file, i);
		lcore_history->lcore = i;
		lcore_history->num_entries = num_entries;
	}
	g_trace_file->file_size = file_size;
	g_trace_file->num_owners = TRACE_NUM_OWNERS;
	g_trace_file->owner_description_size = TRACE_OWNER_DESCRIPTION_SIZE;
	g_trace_file->owner_offset = owner_offset;

	if (trace_flags_init()) {
		goto trace_init_err;
	}

	return 0;

trace_init_err:
	if (g_trace_file != MAP_FAILED) {
		munmap(g_trace_file, file_size);
	}
	close(g_trace_fd);
	g_trace_fd = -1;
	shm_unlink(shm_name);
	spdk_bit_array_free(&g_ut_array);
	g_trace_file = NULL;

	return 1;

}

void
spdk_trace_cleanup(void)
{
	bool unlink = true;
	int i;
	struct spdk_trace_history *lcore_history;

	if (g_trace_file == NULL) {
		return;
	}

	trace_flags_fini();

	/*
	 * Only unlink the shm if there were no trace_entry recorded. This ensures the file
	 * can be used after this process exits/crashes for debugging.
	 * Note that we have to calculate this value before g_trace_file gets unmapped.
	 */
	for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
		lcore_history = spdk_get_per_lcore_history(g_trace_file, i);
		if (lcore_history == NULL) {
			continue;
		}
		unlink = lcore_history->entries[0].tsc == 0;
		if (!unlink) {
			break;
		}
	}

	munmap(g_trace_file, sizeof(struct spdk_trace_file));
	g_trace_file = NULL;
	close(g_trace_fd);
	spdk_bit_array_free(&g_ut_array);

	if (unlink) {
		shm_unlink(g_shm_name);
	}
}

const char *
trace_get_shm_name(void)
{
	return g_shm_name;
}
