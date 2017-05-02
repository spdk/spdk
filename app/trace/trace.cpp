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

#include <map>

extern "C" {
#include "spdk/trace.h"
}

static struct spdk_trace_histories *g_histories;

static void usage(void);

struct entry_key {
	entry_key(uint16_t _lcore, uint64_t _tsc) : lcore(_lcore), tsc(_tsc) {}
	uint16_t lcore;
	uint64_t tsc;
};

class compare_entry_key
{
public:
	bool operator()(const entry_key &first, const entry_key &second) const
	{
		if (first.tsc == second.tsc) {
			return first.lcore < second.lcore;
		} else {
			return first.tsc < second.tsc;
		}
	}
};

typedef std::map<entry_key, spdk_trace_entry *, compare_entry_key> entry_map;

entry_map g_entry_map;

struct object_stats {

	std::map<uint64_t, uint64_t> start;
	std::map<uint64_t, uint64_t> index;
	std::map<uint64_t, uint64_t> size;
	std::map<uint64_t, uint64_t> tpoint_id;
	uint64_t counter;

	object_stats() : start(), index(), size(), tpoint_id(), counter(0) {}
};

struct object_stats g_stats[SPDK_TRACE_MAX_OBJECT];

static char *exe_name;
static int verbose = 1;
static int g_fudge_factor = 20;

static uint64_t tsc_rate;
static uint64_t first_tsc = 0x0;
static uint64_t last_tsc = -1ULL;

static float
get_us_from_tsc(uint64_t tsc, uint64_t tsc_rate)
{
	return ((float)tsc) * 1000 * 1000 / tsc_rate;
}

static void
print_ptr(const char *arg_string, uint64_t arg)
{
	printf("%-7.7s0x%-14jx ", arg_string, arg);
}

static void
print_uint64(const char *arg_string, uint64_t arg)
{
	/*
	 * Print arg as signed, since -1 is a common value especially
	 *  for FLUSH WRITEBUF when writev() returns -1 due to full
	 *  socket buffer.
	 */
	printf("%-7.7s%-16jd ", arg_string, arg);
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
print_object_id(uint8_t type, uint64_t id)
{
	printf("id:    %c%-15jd ", g_histories->object[type].id_prefix, id);
}

static void
print_float(const char *arg_string, float arg)
{
	printf("%-7s%-16.3f ", arg_string, arg);
}

static void
print_arg(bool arg_is_ptr, const char *arg_string, uint64_t arg)
{
	if (arg_string[0] == 0)
		return;

	if (arg_is_ptr)
		print_ptr(arg_string, arg);
	else
		print_uint64(arg_string, arg);
}

static void
print_event(struct spdk_trace_entry *e, uint64_t tsc_rate,
	    uint64_t tsc_offset, uint16_t lcore)
{
	struct spdk_trace_tpoint	*d;
	struct object_stats		*stats;
	float				us;

	d = &g_histories->tpoint[e->tpoint_id];
	stats = &g_stats[d->object_type];

	if (d->new_object) {
		stats->index[e->object_id] = stats->counter++;
		stats->tpoint_id[e->object_id] = e->tpoint_id;
		stats->start[e->object_id] = e->tsc;
		stats->size[e->object_id] = e->size;
	}

	if (d->arg1_is_alias) {
		stats->index[e->arg1] = stats->index[e->object_id];
		stats->start[e->arg1] = stats->start[e->object_id];
		stats->size[e->arg1] = stats->size[e->object_id];
	}

	us = get_us_from_tsc(e->tsc - tsc_offset, tsc_rate);

	printf("%2d: %10.3f (%9ju) ", lcore, us, e->tsc - tsc_offset);
	if (g_histories->owner[d->owner_type].id_prefix) {
		printf("%c%02d ", g_histories->owner[d->owner_type].id_prefix, e->poller_id);
	} else {
		printf("%4s", " ");
	}

	printf("%-*s ", (int)sizeof(d->name), d->name);
	print_size(e->size);

	if (d->new_object) {
		print_arg(d->arg1_is_ptr, d->arg1_name, e->arg1);
		print_object_id(d->object_type, stats->index[e->object_id]);
	} else if (d->object_type != OBJECT_NONE) {
		if (stats->start.find(e->object_id) != stats->start.end()) {
			struct spdk_trace_tpoint *start_description;

			us = get_us_from_tsc(e->tsc - stats->start[e->object_id],
					     tsc_rate);
			print_object_id(d->object_type, stats->index[e->object_id]);
			print_float("time:", us);
			start_description = &g_histories->tpoint[stats->tpoint_id[e->object_id]];
			if (start_description->short_name[0] != 0) {
				printf(" (%.4s)", start_description->short_name);
			}
		} else {
			printf("id:    N/A");
		}
	} else {
		print_arg(d->arg1_is_ptr, d->arg1_name, e->arg1);
	}
	printf("\n");
}

static void
process_event(struct spdk_trace_entry *e, uint64_t tsc_rate,
	      uint64_t tsc_offset, uint16_t lcore)
{
	if (verbose) {
		print_event(e, tsc_rate, tsc_offset, lcore);
	}
}

static int
populate_events(struct spdk_trace_history *history)
{
	int i, entry_size, history_size, num_entries, num_entries_filled;
	struct spdk_trace_entry *e;
	int first, last, lcore;

	lcore = history->lcore;

	entry_size = sizeof(history->entries[0]);
	history_size = sizeof(history->entries);
	num_entries = history_size / entry_size;

	e = history->entries;

	num_entries_filled = num_entries;
	while (e[num_entries_filled - 1].tsc == 0) {
		num_entries_filled--;
	}

	if (num_entries == num_entries_filled) {
		first = last = 0;
		for (i = 1; i < num_entries; i++) {
			if (e[i].tsc < e[first].tsc)
				first = i;
			if (e[i].tsc > e[last].tsc)
				last = i;
		}

		first += g_fudge_factor;
		if (first >= num_entries)
			first -= num_entries;

		last -= g_fudge_factor;
		if (last < 0)
			last += num_entries;
	} else {
		first = 0;
		last = num_entries_filled - 1;
	}

	/*
	 * We keep track of the highest first TSC out of all reactors and
	 *  the lowest last TSC out of all reactors.  We will ignore any
	 *  events outside the range of these two TSC values.  This will
	 *  ensure we only print data for the subset of time where we have
	 *  data across all reactors.
	 */
	if (e[first].tsc > first_tsc) {
		first_tsc = e[first].tsc;
	}
	if (e[last].tsc < last_tsc) {
		last_tsc = e[last].tsc;
	}

	i = first;
	while (1) {
		g_entry_map[entry_key(lcore, e[i].tsc)] = &e[i];
		if (i == last) {
			break;
		}
		i++;
		if (i == num_entries_filled) {
			i = 0;
		}
	}

	return (0);
}

static void usage(void)
{
	fprintf(stderr, "usage:\n");
	fprintf(stderr, "   %s <option> <lcore#>\n", exe_name);
	fprintf(stderr, "        option = '-q' to disable verbose mode\n");
	fprintf(stderr, "                 '-s' to specify spdk_trace shm name\n");
	fprintf(stderr, "                 '-c' to display single lcore history\n");
	fprintf(stderr, "                 '-f' to specify number of events to ignore at\n");
	fprintf(stderr, "                      beginning and end of trace (default: 20)\n");
	fprintf(stderr, "                 '-i' to specify the shared memory ID\n");
	fprintf(stderr, "                 '-p' to specify the trace PID\n");
	fprintf(stderr, "                 (One of -i or -p must be specified)\n");
}

int main(int argc, char **argv)
{
	void			*history_ptr;
	struct spdk_trace_history *history_entries, *history;
	int			fd, i;
	int			lcore = SPDK_TRACE_MAX_LCORE;
	uint64_t		tsc_offset;
	const char		*app_name = "ids";
	int			op;
	char			shm_name[64];
	int			shm_id = -1, shm_pid = -1;

	exe_name = argv[0];
	while ((op = getopt(argc, argv, "c:f:i:p:qs:")) != -1) {
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
		case 'f':
			g_fudge_factor = atoi(optarg);
			break;
		case 'i':
			shm_id = atoi(optarg);
			break;
		case 'p':
			shm_pid = atoi(optarg);
			break;
		case 'q':
			verbose = 0;
			break;
		case 's':
			app_name = optarg;
			break;
		default:
			usage();
			exit(1);
		}
	}

	if (shm_id >= 0) {
		snprintf(shm_name, sizeof(shm_name), "/%s_trace.%d", app_name, shm_id);
	} else {
		snprintf(shm_name, sizeof(shm_name), "/%s_trace.pid%d", app_name, shm_pid);
	}

	fd = shm_open(shm_name, O_RDONLY, 0600);
	if (fd < 0) {
		fprintf(stderr, "Could not open shm %s.\n", shm_name);
		usage();
		exit(-1);
	}

	history_ptr = mmap(NULL, sizeof(*g_histories), PROT_READ, MAP_SHARED, fd, 0);
	if (history_ptr == MAP_FAILED) {
		fprintf(stderr, "Could not mmap shm %s.\n", shm_name);
		usage();
		exit(-1);
	}

	g_histories = (struct spdk_trace_histories *)history_ptr;

	tsc_rate = g_histories->tsc_rate;
	if (tsc_rate == 0) {
		fprintf(stderr, "Invalid tsc_rate %ju\n", tsc_rate);
		usage();
		exit(-1);
	}

	if (verbose) {
		printf("TSC Rate: %ju\n", tsc_rate);
	}

	history_entries = (struct spdk_trace_history *)malloc(sizeof(g_histories->per_lcore_history));
	if (history_entries == NULL) {
		goto cleanup;
	}
	memcpy(history_entries, g_histories->per_lcore_history,
	       sizeof(g_histories->per_lcore_history));

	if (lcore == SPDK_TRACE_MAX_LCORE) {
		for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
			history = &history_entries[i];
			if (history->entries[0].tsc == 0) {
				continue;
			}
			populate_events(history);
		}
	} else {
		history = &history_entries[lcore];
		if (history->entries[0].tsc != 0) {
			populate_events(history);
		}
	}

	tsc_offset = first_tsc;
	for (entry_map::iterator it = g_entry_map.begin(); it != g_entry_map.end(); it++) {
		if (it->first.tsc < first_tsc || it->first.tsc > last_tsc) {
			continue;
		}
		process_event(it->second, tsc_rate, tsc_offset, it->first.lcore);
	}

	free(history_entries);

cleanup:
	munmap(history_ptr, sizeof(*g_histories));
	close(fd);

	return (0);
}
