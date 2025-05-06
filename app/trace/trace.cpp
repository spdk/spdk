/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/json.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include <map>

extern "C" {
#include "spdk/trace_parser.h"
#include "spdk/util.h"
}

enum print_format_type {
	PRINT_FMT_JSON,
	PRINT_FMT_DEFAULT,
};

static struct spdk_trace_parser *g_parser;
static const struct spdk_trace_file *g_file;
static struct spdk_json_write_ctx *g_json;
static bool g_print_tsc = false;
static bool g_time_diff;

/* This is a bit ugly, but we don't want to include env_dpdk in the app, while spdk_util, which we
 * do need, uses some of the functions implemented there.  We're not actually using the functions
 * that depend on those, so just define them as no-ops to allow the app to link.
 */
extern "C" {
	void *
	spdk_realloc(void *buf, size_t size, size_t align)
	{
		assert(false);

		return NULL;
	}

	void
	spdk_free(void *buf)
	{
		assert(false);
	}

	uint64_t
	spdk_get_ticks(void)
	{
		return 0;
	}
} /* extern "C" */

static void usage(void);

static char *g_exe_name;

static float
get_us_from_tsc(uint64_t tsc, uint64_t tsc_rate)
{
	return ((float)tsc) * 1000 * 1000 / tsc_rate;
}

static const char *
format_argname(const char *name)
{
	static char namebuf[16];

	snprintf(namebuf, sizeof(namebuf), "%s: ", name);
	return namebuf;
}

static void
print_ptr(const char *arg_string, uint64_t arg)
{
	printf("%-7.7s0x%-14jx ", format_argname(arg_string), arg);
}

static void
print_uint64(const char *arg_string, uint64_t arg)
{
	/*
	 * Print arg as signed, since -1 is a common value especially
	 *  for FLUSH WRITEBUF when writev() returns -1 due to full
	 *  socket buffer.
	 */
	printf("%-7.7s%-16jd ", format_argname(arg_string), arg);
}

static void
print_string(const char *arg_string, const char *arg)
{
	printf("%-7.7s%-16.16s ", format_argname(arg_string), arg);
}

static void
print_size(uint32_t size)
{
	if (size > 0) {
		printf("size: %6u ", size);
	} else {
		printf("%13s", " ");
	}
}

static void
print_object_id(const struct spdk_trace_tpoint *d, struct spdk_trace_parser_entry *entry)
{
	/* Set size to 128 and 256 bytes to make sure we can fit all the characters we need */
	char related_id[128] = {'\0'};
	char ids[256] = {'\0'};

	if (entry->related_type != OBJECT_NONE) {
		snprintf(related_id, sizeof(related_id), " (%c%jd)",
			 g_file->object[entry->related_type].id_prefix,
			 entry->related_index);
	}

	snprintf(ids, sizeof(ids), "%c%jd%s", g_file->object[d->object_type].id_prefix,
		 entry->object_index, related_id);
	printf("id:    %-17s", ids);
}

static void
print_float(const char *arg_string, float arg)
{
	printf("%-7s%-16.3f ", format_argname(arg_string), arg);
}

static void
print_event(struct spdk_trace_parser_entry *entry, uint64_t tsc_rate, uint64_t tsc_offset)
{
	struct spdk_trace_entry		*e = entry->entry;
	struct spdk_trace_owner		*owner;
	const struct spdk_trace_tpoint	*d;
	float				us;
	size_t				i;

	d = &g_file->tpoint[e->tpoint_id];
	us = get_us_from_tsc(e->tsc - tsc_offset, tsc_rate);

	printf("%-*s ", (int)sizeof(g_file->tname[entry->lcore]), g_file->tname[entry->lcore]);
	printf("%2d: %10.3f ", entry->lcore, us);
	if (g_print_tsc) {
		printf("(%9ju) ", e->tsc - tsc_offset);
	}
	owner = spdk_get_trace_owner(g_file, e->owner_id);
	/* For now, only try to print first 64 bytes of description. */
	if (e->owner_id > 0 && owner->tsc < e->tsc) {
		printf("%-*s ", 64, owner->description);
	} else {
		printf("%-*s ", 64, "");
	}

	printf("%-*s ", (int)sizeof(d->name), d->name);
	print_size(e->size);

	if (d->new_object) {
		print_object_id(d, entry);
	} else if (d->object_type != OBJECT_NONE) {
		if (entry->object_index != UINT64_MAX) {
			us = get_us_from_tsc(e->tsc - entry->object_start, tsc_rate);
			print_object_id(d, entry);
			print_float("time", us);
		} else {
			printf("id:    %-17s", "N/A");
		}
	} else if (e->object_id != 0) {
		print_ptr("object", e->object_id);
	}

	for (i = 0; i < d->num_args; ++i) {
		if (entry->args[i].is_related) {
			/* This argument was already implicitly shown by its
			 * associated related object ID.
			 */
			continue;
		}
		switch (d->args[i].type) {
		case SPDK_TRACE_ARG_TYPE_PTR:
			print_ptr(d->args[i].name, (uint64_t)entry->args[i].u.pointer);
			break;
		case SPDK_TRACE_ARG_TYPE_INT:
			print_uint64(d->args[i].name, entry->args[i].u.integer);
			break;
		case SPDK_TRACE_ARG_TYPE_STR:
			print_string(d->args[i].name, entry->args[i].u.string);
			break;
		}
	}
	printf("\n");
}

static void
print_event_json(struct spdk_trace_parser_entry *entry, uint64_t tsc_rate, uint64_t tsc_offset)
{
	struct spdk_trace_entry *e = entry->entry;
	const struct spdk_trace_tpoint *d;
	size_t i;

	d = &g_file->tpoint[e->tpoint_id];

	spdk_json_write_object_begin(g_json);
	spdk_json_write_named_uint64(g_json, "lcore", entry->lcore);
	spdk_json_write_named_uint64(g_json, "tpoint", e->tpoint_id);
	spdk_json_write_named_uint64(g_json, "tsc", e->tsc);

	if (g_file->owner_type[d->owner_type].id_prefix) {
		spdk_json_write_named_string_fmt(g_json, "poller", "%c%02d",
						 g_file->owner_type[d->owner_type].id_prefix,
						 e->owner_id);
	}
	if (e->size != 0) {
		spdk_json_write_named_uint32(g_json, "size", e->size);
	}
	if (d->new_object || d->object_type != OBJECT_NONE || e->object_id != 0) {
		char object_type;

		spdk_json_write_named_object_begin(g_json, "object");
		if (d->new_object) {
			object_type =  g_file->object[d->object_type].id_prefix;
			spdk_json_write_named_string_fmt(g_json, "id", "%c%" PRIu64, object_type,
							 entry->object_index);
		} else if (d->object_type != OBJECT_NONE) {
			object_type =  g_file->object[d->object_type].id_prefix;
			if (entry->object_index != UINT64_MAX) {
				spdk_json_write_named_string_fmt(g_json, "id", "%c%" PRIu64,
								 object_type,
								 entry->object_index);
				spdk_json_write_named_uint64(g_json, "time",
							     e->tsc - entry->object_start);
			}
		}
		spdk_json_write_named_uint64(g_json, "value", e->object_id);
		spdk_json_write_object_end(g_json);
	}

	/* Print related objects array */
	if (entry->related_index != UINT64_MAX) {
		spdk_json_write_named_string_fmt(g_json, "related", "%c%" PRIu64,
						 g_file->object[entry->related_type].id_prefix,
						 entry->related_index);
	}

	if (d->num_args > 0) {
		spdk_json_write_named_array_begin(g_json, "args");
		for (i = 0; i < d->num_args; ++i) {
			switch (d->args[i].type) {
			case SPDK_TRACE_ARG_TYPE_PTR:
				spdk_json_write_uint64(g_json, (uint64_t)entry->args[i].u.pointer);
				break;
			case SPDK_TRACE_ARG_TYPE_INT:
				spdk_json_write_uint64(g_json, entry->args[i].u.integer);
				break;
			case SPDK_TRACE_ARG_TYPE_STR:
				spdk_json_write_string(g_json, entry->args[i].u.string);
				break;
			}
		}
		spdk_json_write_array_end(g_json);
	}

	spdk_json_write_object_end(g_json);
}

static void
print_tpoint_definitions(void)
{
	const struct spdk_trace_tpoint *tpoint;
	size_t i, j;

	/* We only care about these when printing JSON */
	if (!g_json) {
		return;
	}

	spdk_json_write_named_uint64(g_json, "tsc_rate", g_file->tsc_rate);
	spdk_json_write_named_array_begin(g_json, "tpoints");

	for (i = 0; i < SPDK_COUNTOF(g_file->tpoint); ++i) {
		tpoint = &g_file->tpoint[i];
		if (tpoint->tpoint_id == 0) {
			continue;
		}

		spdk_json_write_object_begin(g_json);
		spdk_json_write_named_string(g_json, "name", tpoint->name);
		spdk_json_write_named_uint32(g_json, "id", tpoint->tpoint_id);
		spdk_json_write_named_bool(g_json, "new_object", tpoint->new_object);

		spdk_json_write_named_array_begin(g_json, "args");
		for (j = 0; j < tpoint->num_args; ++j) {
			spdk_json_write_object_begin(g_json);
			spdk_json_write_named_string(g_json, "name", tpoint->args[j].name);
			spdk_json_write_named_uint32(g_json, "type", tpoint->args[j].type);
			spdk_json_write_named_uint32(g_json, "size", tpoint->args[j].size);
			spdk_json_write_object_end(g_json);
		}
		spdk_json_write_array_end(g_json);
		spdk_json_write_object_end(g_json);
	}

	spdk_json_write_array_end(g_json);
}

static int
print_json(void *cb_ctx, const void *data, size_t size)
{
	ssize_t rc;

	while (size > 0) {
		rc = write(STDOUT_FILENO, data, size);
		if (rc < 0) {
			fprintf(stderr, "%s: %s\n", g_exe_name, spdk_strerror(errno));
			abort();
		}

		size -= rc;
	}

	return 0;
}

static int
trace_print(int lcore)
{
	struct spdk_trace_parser_entry	entry;
	int		i;
	uint64_t	tsc_offset, entry_count, tsc_base_offset;
	uint64_t	tsc_rate = g_file->tsc_rate;

	printf("TSC Rate: %ju\n", tsc_rate);
	for (i = 0; i < SPDK_TRACE_MAX_LCORE; ++i) {
		if (lcore == SPDK_TRACE_MAX_LCORE || i == lcore) {
			entry_count = spdk_trace_parser_get_entry_count(g_parser, i);
			if (entry_count > 0) {
				printf("Trace Size of lcore (%d): %ju\n", i, entry_count);
			}
		}
	}

	tsc_base_offset = tsc_offset = spdk_trace_parser_get_tsc_offset(g_parser);
	while (spdk_trace_parser_next_entry(g_parser, &entry)) {
		if (entry.entry->tsc < tsc_base_offset) {
			continue;
		}
		print_event(&entry, tsc_rate, tsc_offset);
		if (g_time_diff) {
			tsc_offset = entry.entry->tsc;
		}
	}

	return 0;
}

static int
trace_print_json(void)
{
	struct spdk_trace_parser_entry	entry;
	uint64_t	tsc_offset, tsc_base_offset;
	uint64_t	tsc_rate = g_file->tsc_rate;

	g_json = spdk_json_write_begin(print_json, NULL, 0);
	if (g_json == NULL) {
		fprintf(stderr, "Failed to allocate JSON write context\n");
		return -1;
	}

	spdk_json_write_object_begin(g_json);
	print_tpoint_definitions();
	spdk_json_write_named_array_begin(g_json, "entries");

	tsc_base_offset = tsc_offset = spdk_trace_parser_get_tsc_offset(g_parser);
	while (spdk_trace_parser_next_entry(g_parser, &entry)) {
		if (entry.entry->tsc < tsc_base_offset) {
			continue;
		}
		print_event_json(&entry, tsc_rate, tsc_offset);
		if (g_time_diff) {
			tsc_offset = entry.entry->tsc;
		}
	}

	spdk_json_write_array_end(g_json);
	spdk_json_write_object_end(g_json);
	spdk_json_write_end(g_json);

	return 0;
}

static void
usage(void)
{
	fprintf(stderr, "usage:\n");
	fprintf(stderr, "   %s <option> <lcore#>\n", g_exe_name);
	fprintf(stderr, "                 '-c' to display single lcore history\n");
	fprintf(stderr, "                 '-t' to display TSC offset for each event\n");
	fprintf(stderr, "                 '-s' to specify spdk_trace shm name for a\n");
	fprintf(stderr, "                      currently running process\n");
	fprintf(stderr, "                 '-i' to specify the shared memory ID\n");
	fprintf(stderr, "                 '-p' to specify the trace PID\n");
	fprintf(stderr, "                      (If -s is specified, then one of\n");
	fprintf(stderr, "                       -i or -p must be specified)\n");
	fprintf(stderr, "                 '-f' to specify a tracepoint file name\n");
	fprintf(stderr, "                      (-s and -f are mutually exclusive)\n");
	fprintf(stderr, "                 '-T' to show time as delta between current and previous event\n");
#if defined(__linux__)
	fprintf(stderr, "                 Without -s or -f, %s will look for\n", g_exe_name);
	fprintf(stderr, "                      newest trace file in /dev/shm\n");
#endif
	fprintf(stderr, "                 '-j' to use JSON to format the output\n");
}

#if defined(__linux__)
static time_t g_mtime = 0;
static char g_newest_file[PATH_MAX] = {};

static int
get_newest(const char *path, const struct stat *sb, int tflag, struct FTW *ftw)
{
	if (tflag == FTW_F && sb->st_mtime > g_mtime &&
	    strstr(path, SPDK_TRACE_SHM_NAME_BASE) != NULL) {
		g_mtime = sb->st_mtime;
		strncpy(g_newest_file, path, PATH_MAX - 1);
	}
	return 0;
}
#endif

int
main(int argc, char **argv)
{
	struct spdk_trace_parser_opts	opts;
	enum print_format_type	print_format = PRINT_FMT_DEFAULT;
	int				lcore = SPDK_TRACE_MAX_LCORE;
	const char			*app_name = NULL;
	const char			*file_name = NULL;
	int				op;
	int				rc = 0;
	char				shm_name[64];
	int				shm_id = -1, shm_pid = -1;

	g_exe_name = argv[0];
	while ((op = getopt(argc, argv, "c:f:i:jp:s:tT")) != -1) {
		switch (op) {
		case 'c':
			lcore = atoi(optarg);
			if (lcore > SPDK_TRACE_MAX_LCORE) {
				fprintf(stderr, "Selected lcore: %d "
					"exceeds maximum %d\n", lcore,
					SPDK_TRACE_MAX_LCORE);
				exit(1);
			}
			break;
		case 'i':
			shm_id = atoi(optarg);
			break;
		case 'p':
			shm_pid = atoi(optarg);
			break;
		case 's':
			app_name = optarg;
			break;
		case 'f':
			file_name = optarg;
			break;
		case 't':
			g_print_tsc = true;
			break;
		case 'T':
			g_time_diff = true;
			break;
		case 'j':
			print_format = PRINT_FMT_JSON;
			break;
		default:
			usage();
			exit(1);
		}
	}

	if (file_name != NULL && app_name != NULL) {
		fprintf(stderr, "-f and -s are mutually exclusive\n");
		usage();
		exit(1);
	}

	if (file_name == NULL && app_name == NULL) {
#if defined(__linux__)
		nftw("/dev/shm", get_newest, 1, 0);
		if (strlen(g_newest_file) > 0) {
			file_name = g_newest_file;
			printf("Using newest trace file found: %s\n", file_name);
		} else {
			fprintf(stderr, "No shm file found and -f not specified\n");
			usage();
			exit(1);
		}
#else
		fprintf(stderr, "One of -f and -s must be specified\n");
		usage();
		exit(1);
#endif
	}

	if (!file_name) {
		if (shm_id >= 0) {
			snprintf(shm_name, sizeof(shm_name), "/%s_trace.%d", app_name, shm_id);
		} else {
			snprintf(shm_name, sizeof(shm_name), "/%s_trace.pid%d", app_name, shm_pid);
		}
		file_name = shm_name;
	}

	opts.filename = file_name;
	opts.lcore = lcore;
	opts.mode = app_name == NULL ? SPDK_TRACE_PARSER_MODE_FILE : SPDK_TRACE_PARSER_MODE_SHM;
	g_parser = spdk_trace_parser_init(&opts);
	if (g_parser == NULL) {
		fprintf(stderr, "Failed to initialize trace parser\n");
		exit(1);
	}

	g_file = spdk_trace_parser_get_file(g_parser);
	switch (print_format) {
	case PRINT_FMT_JSON:
		rc = trace_print_json();
		break;
	case PRINT_FMT_DEFAULT:
	default:
		rc = trace_print(lcore);
		break;
	}

	spdk_trace_parser_cleanup(g_parser);

	return rc;
}
