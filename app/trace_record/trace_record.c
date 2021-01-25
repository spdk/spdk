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
#include "spdk/util.h"
#include "spdk/barrier.h"

#define TRACE_FILE_COPY_SIZE	(32 * 1024)
#define TRACE_PATH_MAX		2048

static char *g_exe_name;
static int g_verbose = 1;
static uint64_t g_tsc_rate;
static uint64_t g_utsc_rate;
static bool g_shutdown = false;
static uint64_t g_histories_size;

struct lcore_trace_record_ctx {
	char lcore_file[TRACE_PATH_MAX];
	int fd;
	struct spdk_trace_history *in_history;
	struct spdk_trace_history *out_history;

	/* Recorded next entry index in record */
	uint64_t rec_next_entry;

	/* Record tsc for report */
	uint64_t first_entry_tsc;
	uint64_t last_entry_tsc;

	/* Total number of entries in lcore trace file */
	uint64_t num_entries;
};

struct aggr_trace_record_ctx {
	const char *out_file;
	int out_fd;
	int shm_fd;
	struct lcore_trace_record_ctx lcore_ports[SPDK_TRACE_MAX_LCORE];
	struct spdk_trace_histories *trace_histories;
};

static int
input_trace_file_mmap(struct aggr_trace_record_ctx *ctx, const char *shm_name)
{
	void *history_ptr;
	int i;

	ctx->shm_fd = shm_open(shm_name, O_RDONLY, 0);
	if (ctx->shm_fd < 0) {
		fprintf(stderr, "Could not open %s.\n", shm_name);
		return -1;
	}

	/* Map the header of trace file */
	history_ptr = mmap(NULL, sizeof(struct spdk_trace_histories), PROT_READ, MAP_SHARED, ctx->shm_fd,
			   0);
	if (history_ptr == MAP_FAILED) {
		fprintf(stderr, "Could not mmap shm %s.\n", shm_name);
		close(ctx->shm_fd);
		return -1;
	}

	ctx->trace_histories = (struct spdk_trace_histories *)history_ptr;

	g_tsc_rate = ctx->trace_histories->flags.tsc_rate;
	g_utsc_rate = g_tsc_rate / 1000;
	if (g_tsc_rate == 0) {
		fprintf(stderr, "Invalid tsc_rate %ju\n", g_tsc_rate);
		munmap(history_ptr, sizeof(struct spdk_trace_histories));
		close(ctx->shm_fd);
		return -1;
	}

	if (g_verbose) {
		printf("TSC Rate: %ju\n", g_tsc_rate);
	}

	/* Remap the entire trace file */
	g_histories_size = spdk_get_trace_histories_size(ctx->trace_histories);
	munmap(history_ptr, sizeof(struct spdk_trace_histories));
	history_ptr = mmap(NULL, g_histories_size, PROT_READ, MAP_SHARED, ctx->shm_fd, 0);
	if (history_ptr == MAP_FAILED) {
		fprintf(stderr, "Could not remmap shm %s.\n", shm_name);
		close(ctx->shm_fd);
		return -1;
	}

	ctx->trace_histories = (struct spdk_trace_histories *)history_ptr;
	for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
		ctx->lcore_ports[i].in_history = spdk_get_per_lcore_history(ctx->trace_histories, i);

		if (g_verbose) {
			printf("Number of trace entries for lcore (%d): %ju\n", i,
			       ctx->lcore_ports[i].in_history->num_entries);
		}
	}

	return 0;
}

static int
output_trace_files_prepare(struct aggr_trace_record_ctx *ctx, const char *aggr_path)
{
	int flags = O_CREAT | O_EXCL | O_RDWR;
	struct lcore_trace_record_ctx *port_ctx;
	int name_len;
	int i, rc;

	/* Assign file names for related trace files */
	ctx->out_file = aggr_path;
	for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
		port_ctx = &ctx->lcore_ports[i];

		/* Get the length of trace file name for each lcore with format "%s-%d" */
		name_len = snprintf(port_ctx->lcore_file, TRACE_PATH_MAX, "%s-%d", ctx->out_file, i);
		if (name_len >= TRACE_PATH_MAX) {
			fprintf(stderr, "Length of file path (%s) exceeds limitation for lcore file.\n",
				aggr_path);
			goto err;
		}
	}

	/* If output trace file already exists, try to unlink it together with its temporary files */
	if (access(ctx->out_file, F_OK) == 0) {
		rc = unlink(ctx->out_file);
		if (rc) {
			goto err;
		}

		for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
			port_ctx = &ctx->lcore_ports[i];
			if (access(port_ctx->lcore_file, F_OK) == 0) {
				rc = unlink(port_ctx->lcore_file);
				if (rc) {
					goto err;
				}
			}
		}

	}

	for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
		port_ctx = &ctx->lcore_ports[i];

		port_ctx->fd = open(port_ctx->lcore_file, flags, 0600);
		if (port_ctx->fd < 0) {
			fprintf(stderr, "Could not open lcore file %s.\n", port_ctx->lcore_file);
			goto err;
		}

		if (g_verbose) {
			printf("Create tmp lcore trace file %s for lcore %d\n", port_ctx->lcore_file, i);
		}

		port_ctx->out_history = calloc(1, sizeof(struct spdk_trace_history));
		if (port_ctx->out_history == NULL) {
			fprintf(stderr, "Failed to allocate memory for out_history.\n");
			goto err;
		}
	}

	return 0;

err:
	for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
		port_ctx = &ctx->lcore_ports[i];
		free(port_ctx->out_history);

		if (port_ctx->fd > 0) {
			close(port_ctx->fd);
		}
	}

	return -1;
}

static void
output_trace_files_finish(struct aggr_trace_record_ctx *ctx)
{
	struct lcore_trace_record_ctx *port_ctx;
	int i;

	for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
		port_ctx = &ctx->lcore_ports[i];

		free(port_ctx->out_history);
		close(port_ctx->fd);
		unlink(port_ctx->lcore_file);

		if (g_verbose) {
			printf("Remove tmp lcore trace file %s for lcore %d\n", port_ctx->lcore_file, i);
		}
	}
}

static int
cont_write(int fildes, const void *buf, size_t nbyte)
{
	int rc;
	int _nbyte = nbyte;

	while (_nbyte) {
		rc = write(fildes, buf, _nbyte);
		if (rc < 0) {
			if (errno != EINTR) {
				return -1;
			}

			continue;
		}

		_nbyte -= rc;
	}

	return nbyte;
}

static int
cont_read(int fildes, void *buf, size_t nbyte)
{
	int rc;
	int _nbyte = nbyte;

	while (_nbyte) {
		rc = read(fildes, buf, _nbyte);
		if (rc == 0) {
			return nbyte - _nbyte;
		} else if (rc < 0) {
			if (errno != EINTR) {
				return -1;
			}

			continue;
		}

		_nbyte -= rc;
	}

	return nbyte;
}

static int
lcore_trace_last_entry_idx(struct spdk_trace_history *in_history, int cir_next_idx)
{
	int last_idx;

	if (cir_next_idx == 0) {
		last_idx = in_history->num_entries - 1;
	} else {
		last_idx = cir_next_idx - 1;
	}

	return last_idx;
}

static int
circular_buffer_padding_backward(int fd, struct spdk_trace_history *in_history,
				 int cir_start, int cir_end)
{
	int rc;

	if (cir_end <= cir_start) {
		fprintf(stderr, "Wrong using of circular_buffer_padding_back\n");
		return -1;
	}

	rc = cont_write(fd, &in_history->entries[cir_start],
			sizeof(struct spdk_trace_entry) * (cir_end - cir_start));
	if (rc < 0) {
		fprintf(stderr, "Failed to append entries into lcore file\n");
		return rc;
	}

	return 0;
}

static int
circular_buffer_padding_across(int fd, struct spdk_trace_history *in_history,
			       int cir_start, int cir_end)
{
	int rc;
	int num_entries = in_history->num_entries;

	if (cir_end > cir_start) {
		fprintf(stderr, "Wrong using of circular_buffer_padding_across\n");
		return -1;
	}

	rc = cont_write(fd, &in_history->entries[cir_start],
			sizeof(struct spdk_trace_entry) * (num_entries - cir_start));
	if (rc < 0) {
		fprintf(stderr, "Failed to append entries into lcore file backward\n");
		return rc;
	}

	if (cir_end == 0) {
		return 0;
	}

	rc = cont_write(fd, &in_history->entries[0], sizeof(struct spdk_trace_entry) * cir_end);
	if (rc < 0) {
		fprintf(stderr, "Failed to append entries into lcore file forward\n");
		return rc;
	}

	return 0;
}

static int
circular_buffer_padding_all(int fd, struct spdk_trace_history *in_history,
			    int cir_end)
{
	return circular_buffer_padding_across(fd, in_history, cir_end, cir_end);
}

static int
lcore_trace_record(struct lcore_trace_record_ctx *lcore_port)
{
	struct spdk_trace_history	*in_history = lcore_port->in_history;
	uint64_t			rec_next_entry = lcore_port->rec_next_entry;
	uint64_t			rec_num_entries = lcore_port->num_entries;
	int				fd = lcore_port->fd;
	uint64_t			shm_next_entry;
	uint64_t			num_cir_entries;
	uint64_t			shm_cir_next;
	uint64_t			rec_cir_next;
	int				rc;
	int				last_idx;

	shm_next_entry = in_history->next_entry;

	/* Ensure all entries of spdk_trace_history are latest to next_entry */
	spdk_smp_rmb();

	if (shm_next_entry == rec_next_entry) {
		/* There is no update */
		return 0;
	} else if (shm_next_entry < rec_next_entry) {
		/* Error branch */
		fprintf(stderr, "Trace porting error in lcore %d, trace rollback occurs.\n", in_history->lcore);
		fprintf(stderr, "shm_next_entry is %ju, record_next_entry is %ju.\n", shm_next_entry,
			rec_next_entry);
		return -1;
	}

	num_cir_entries = in_history->num_entries;
	shm_cir_next = shm_next_entry & (num_cir_entries - 1);

	/* Record first entry's tsc and corresponding entries when recording first time. */
	if (lcore_port->first_entry_tsc == 0) {
		if (shm_next_entry < num_cir_entries) {
			/* Updates haven't been across circular buffer yet.
			 * The first entry in shared memory is the eldest one.
			 */
			lcore_port->first_entry_tsc = in_history->entries[0].tsc;

			lcore_port->num_entries += shm_cir_next;
			rc = circular_buffer_padding_backward(fd, in_history, 0, shm_cir_next);
		} else {
			/* Updates have already been across circular buffer.
			 * The eldest entry in shared memory is pointed by shm_cir_next.
			 */
			lcore_port->first_entry_tsc = in_history->entries[shm_cir_next].tsc;

			lcore_port->num_entries += num_cir_entries;
			rc = circular_buffer_padding_all(fd, in_history, shm_cir_next);
		}

		goto out;
	}

	if (shm_next_entry - rec_next_entry > num_cir_entries) {
		/* There must be missed updates */
		fprintf(stderr, "Trace-record missed %ju trace entries\n",
			shm_next_entry - rec_next_entry - num_cir_entries);

		lcore_port->num_entries += num_cir_entries;
		rc = circular_buffer_padding_all(fd, in_history, shm_cir_next);
	} else if (shm_next_entry - rec_next_entry == num_cir_entries) {
		/* All circular buffer is updated */
		lcore_port->num_entries += num_cir_entries;
		rc = circular_buffer_padding_all(fd, in_history, shm_cir_next);
	} else {
		/* Part of circular buffer is updated */
		rec_cir_next = rec_next_entry & (num_cir_entries - 1);

		if (shm_cir_next > rec_cir_next) {
			/* Updates are not across circular buffer */
			lcore_port->num_entries += shm_cir_next - rec_cir_next;
			rc = circular_buffer_padding_backward(fd, in_history, rec_cir_next, shm_cir_next);
		} else {
			/* Updates are across circular buffer */
			lcore_port->num_entries += num_cir_entries - rec_cir_next + shm_cir_next;
			rc = circular_buffer_padding_across(fd, in_history, rec_cir_next, shm_cir_next);
		}
	}

out:
	if (rc) {
		return rc;
	}

	if (g_verbose) {
		printf("Append %ju trace_entry for lcore %d\n", lcore_port->num_entries - rec_num_entries,
		       in_history->lcore);
	}

	/* Update tpoint_count info */
	memcpy(lcore_port->out_history, lcore_port->in_history, sizeof(struct spdk_trace_history));

	/* Update last_entry_tsc to align with appended entries */
	last_idx = lcore_trace_last_entry_idx(in_history, shm_cir_next);
	lcore_port->last_entry_tsc = in_history->entries[last_idx].tsc;
	lcore_port->rec_next_entry = shm_next_entry;

	return rc;
}

static int
trace_files_aggregate(struct aggr_trace_record_ctx *ctx)
{
	int flags = O_CREAT | O_EXCL | O_RDWR;
	struct lcore_trace_record_ctx *lcore_port;
	char copy_buff[TRACE_FILE_COPY_SIZE];
	uint64_t lcore_offsets[SPDK_TRACE_MAX_LCORE + 1];
	int rc, i;
	ssize_t len = 0;
	uint64_t len_sum;

	ctx->out_fd = open(ctx->out_file, flags, 0600);
	if (ctx->out_fd < 0) {
		fprintf(stderr, "Could not open aggregation file %s.\n", ctx->out_file);
		return -1;
	}

	if (g_verbose) {
		printf("Create trace file %s for output\n", ctx->out_file);
	}

	/* Write flags of histories into head of converged trace file, except num_entriess */
	rc = cont_write(ctx->out_fd, ctx->trace_histories,
			sizeof(struct spdk_trace_histories) - sizeof(lcore_offsets));
	if (rc < 0) {
		fprintf(stderr, "Failed to write trace header into trace file\n");
		goto out;
	}

	/* Update and append lcore offsets converged trace file */
	lcore_offsets[0] = sizeof(struct spdk_trace_flags);
	for (i = 1; i < (int)SPDK_COUNTOF(lcore_offsets); i++) {
		lcore_offsets[i] = spdk_get_trace_history_size(ctx->lcore_ports[i - 1].num_entries) +
				   lcore_offsets[i - 1];
	}

	rc = cont_write(ctx->out_fd, lcore_offsets, sizeof(lcore_offsets));
	if (rc < 0) {
		fprintf(stderr, "Failed to write lcore offsets into trace file\n");
		goto out;
	}

	/* Append each lcore trace file into converged trace file */
	for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
		lcore_port = &ctx->lcore_ports[i];

		lcore_port->out_history->num_entries = lcore_port->num_entries;
		rc = cont_write(ctx->out_fd, lcore_port->out_history, sizeof(struct spdk_trace_history));
		if (rc < 0) {
			fprintf(stderr, "Failed to write lcore trace header into trace file\n");
			goto out;
		}

		/* Move file offset to the start of trace_entries */
		rc = lseek(lcore_port->fd, 0, SEEK_SET);
		if (rc != 0) {
			fprintf(stderr, "Failed to lseek lcore trace file\n");
			goto out;
		}

		len_sum = 0;
		while ((len = cont_read(lcore_port->fd, copy_buff, TRACE_FILE_COPY_SIZE)) > 0) {
			len_sum += len;
			rc = cont_write(ctx->out_fd, copy_buff, len);
			if (rc != len) {
				fprintf(stderr, "Failed to write lcore trace entries into trace file\n");
				goto out;
			}
		}

		if (len_sum != lcore_port->num_entries * sizeof(struct spdk_trace_entry)) {
			fprintf(stderr, "Len of lcore trace file doesn't match number of entries for lcore\n");
		}
	}

	printf("All lcores trace entries are aggregated into trace file %s\n", ctx->out_file);

out:
	close(ctx->out_fd);

	return rc;
}

static void
__shutdown_signal(int signo)
{
	g_shutdown = true;
}

static int
setup_exit_signal_handler(void)
{
	struct sigaction	sigact;
	int			rc;

	memset(&sigact, 0, sizeof(sigact));
	sigemptyset(&sigact.sa_mask);
	/* Install the same handler for SIGINT and SIGTERM */
	sigact.sa_handler = __shutdown_signal;

	rc = sigaction(SIGINT, &sigact, NULL);
	if (rc < 0) {
		fprintf(stderr, "sigaction(SIGINT) failed\n");

		return rc;
	}

	rc = sigaction(SIGTERM, &sigact, NULL);
	if (rc < 0) {
		fprintf(stderr, "sigaction(SIGTERM) failed\n");
	}

	return rc;
}

static void usage(void)
{
	printf("\n%s is used to record all SPDK generated trace entries\n", g_exe_name);
	printf("from SPDK trace shared-memory to specified file.\n\n");
	printf("usage:\n");
	printf("   %s <option>\n", g_exe_name);
	printf("        option = '-q' to disable verbose mode\n");
	printf("                 '-s' to specify spdk_trace shm name for a\n");
	printf("                      currently running process\n");
	printf("                 '-i' to specify the shared memory ID\n");
	printf("                 '-p' to specify the trace PID\n");
	printf("                      (one of -i or -p must be specified)\n");
	printf("                 '-f' to specify output trace file name\n");
	printf("                 '-h' to print usage information\n");
}

int main(int argc, char **argv)
{
	const char			*app_name = NULL;
	const char			*file_name = NULL;
	int				op;
	char				shm_name[64];
	int				shm_id = -1, shm_pid = -1;
	int				rc = 0;
	int				i;
	struct aggr_trace_record_ctx	ctx = {};
	struct lcore_trace_record_ctx	*lcore_port;

	g_exe_name = argv[0];
	while ((op = getopt(argc, argv, "f:i:p:qs:h")) != -1) {
		switch (op) {
		case 'i':
			shm_id = spdk_strtol(optarg, 10);
			break;
		case 'p':
			shm_pid = spdk_strtol(optarg, 10);
			break;
		case 'q':
			g_verbose = 0;
			break;
		case 's':
			app_name = optarg;
			break;
		case 'f':
			file_name = optarg;
			break;
		case 'h':
			usage();
			exit(EXIT_SUCCESS);
		default:
			usage();
			exit(1);
		}
	}

	if (file_name == NULL) {
		fprintf(stderr, "-f must be specified\n");
		usage();
		exit(1);
	}

	if (app_name == NULL) {
		fprintf(stderr, "-s must be specified\n");
		usage();
		exit(1);
	}

	if (shm_id == -1 && shm_pid == -1) {
		fprintf(stderr, "-i or -p must be specified\n");
		usage();
		exit(1);
	}

	if (shm_id >= 0) {
		snprintf(shm_name, sizeof(shm_name), "/%s_trace.%d", app_name, shm_id);
	} else {
		snprintf(shm_name, sizeof(shm_name), "/%s_trace.pid%d", app_name, shm_pid);
	}

	rc = setup_exit_signal_handler();
	if (rc) {
		exit(1);
	}

	rc = input_trace_file_mmap(&ctx, shm_name);
	if (rc) {
		exit(1);
	}

	rc = output_trace_files_prepare(&ctx, file_name);
	if (rc) {
		exit(1);
	}

	printf("Start to poll trace shm file %s\n", shm_name);
	while (!g_shutdown && rc == 0) {
		for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
			lcore_port = &ctx.lcore_ports[i];

			rc = lcore_trace_record(lcore_port);
			if (rc) {
				break;
			}
		}
	}

	if (rc) {
		exit(1);
	}

	printf("Start to aggregate lcore trace files\n");
	rc = trace_files_aggregate(&ctx);
	if (rc) {
		exit(1);
	}

	/* Summary report */
	printf("TSC Rate: %ju\n", g_tsc_rate);
	for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
		lcore_port = &ctx.lcore_ports[i];

		if (lcore_port->num_entries == 0) {
			continue;
		}

		printf("Port %ju trace entries for lcore (%d) in %ju usec\n",
		       lcore_port->num_entries, i,
		       (lcore_port->last_entry_tsc - lcore_port->first_entry_tsc) / g_utsc_rate);

	}

	munmap(ctx.trace_histories, g_histories_size);
	close(ctx.shm_fd);

	output_trace_files_finish(&ctx);

	return 0;
}
