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
#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvme.h"
#include "spdk/likely.h"
#include "fuzz_common.h"

#define UNIQUE_OPCODES 256

char *g_conf_file;
uint64_t g_runtime_ticks;
unsigned int g_seed_value = 0;
int g_runtime;

int g_num_active_threads = 0;
uint32_t g_admin_depth = 16;
uint32_t g_io_depth = 8;

bool g_valid_ns_only = false;
bool g_verbose_mode = false;
bool g_run_admin_commands = false;
bool g_run;

struct spdk_poller *g_app_completion_poller;
bool g_successful_io_opcodes[UNIQUE_OPCODES] = {0};
bool g_successful_admin_opcodes[UNIQUE_OPCODES] = {0};

/* I need context objects here because I need to keep track of all I/O that are in flight. */
struct nvme_cmd_ctx {
	struct spdk_nvme_cmd cmd;
	struct qp_ctx *qp;
	TAILQ_ENTRY(nvme_cmd_ctx) link;
};

struct trid_ctx {
	struct spdk_nvme_transport_id		trid;
	TAILQ_ENTRY(trid_ctx)	tailq;
};

struct ctrlr_ctx {
	struct spdk_nvme_ctrlr			*ctrlr;
	TAILQ_ENTRY(ctrlr_ctx)	tailq;
};

struct qp_ctx {
	struct spdk_nvme_qpair		*qpair;
	/* array of context objects equal in length to the queue depth */
	struct nvme_cmd_ctx		*req_ctx;
	TAILQ_HEAD(, nvme_cmd_ctx)	free_ctx_objs;
	TAILQ_HEAD(, nvme_cmd_ctx)	outstanding_ctx_objs;
	unsigned int			random_seed;
	uint64_t			cmd_counter;
	uint64_t			successful_cmd_counter;
	uint64_t			timeout_tsc;
	uint32_t			num_cmds_outstanding;
	bool				timed_out;
	bool				is_admin;
};

struct ns_ctx {
	struct spdk_nvme_ns		*ns;
	struct spdk_nvme_ctrlr		*ctrlr;
	struct spdk_thread		*thread;
	struct spdk_poller		*req_poller;
	struct qp_ctx			io_qp;
	struct qp_ctx			a_qp;
	uint32_t			nsid;
	TAILQ_ENTRY(ns_ctx)		tailq;
};

static TAILQ_HEAD(, ns_ctx) g_ns_list = TAILQ_HEAD_INITIALIZER(g_ns_list);
static TAILQ_HEAD(, ctrlr_ctx) g_ctrlr_list = TAILQ_HEAD_INITIALIZER(g_ctrlr_list);
static TAILQ_HEAD(, trid_ctx) g_trid_list = TAILQ_HEAD_INITIALIZER(g_trid_list);

static void
report_successful_opcodes(bool *array, int length)
{
	int i;

	for (i = 0; i < length; i++) {
		if (array[i] == true) {
			printf("%d, ", i);
		}
	}
	printf("\n");
}

static void
print_nvme_cmd(struct spdk_nvme_cmd *cmd)
{
	fprintf(stderr, "opc %u\n", cmd->opc);
	fprintf(stderr, "fuse %u\n", cmd->fuse);
	fprintf(stderr, "rsvd1 %u\n", cmd->rsvd1);
	fprintf(stderr, "psdt %u\n", cmd->psdt);
	fprintf(stderr, "cid %u\n", cmd->cid);
	fprintf(stderr, "nsid %u\n", cmd->nsid);
	fprintf(stderr, "rsvd2 %u\n", cmd->rsvd2);
	fprintf(stderr, "rsvd3 %u\n", cmd->rsvd3);
	fprintf(stderr, "mptr %lu\n", cmd->mptr);
	fprintf(stderr, "cdw10 %u\n", cmd->cdw10);
	fprintf(stderr, "cdw11 %u\n", cmd->cdw11);
	fprintf(stderr, "cdw12 %u\n", cmd->cdw12);
	fprintf(stderr, "cdw13 %u\n", cmd->cdw13);
	fprintf(stderr, "cdw14 %u\n", cmd->cdw14);
	fprintf(stderr, "cdw15 %u\n", cmd->cdw15);
	fprintf(stderr, "\n");
}

static void
print_cmd_list(struct qp_ctx *qp)
{
	struct nvme_cmd_ctx *ctx;
	int i = 0;

	TAILQ_FOREACH(ctx, &qp->outstanding_ctx_objs, link) {
		print_nvme_cmd(&ctx->cmd);
		i++;
	}
}

static void
handle_timeout(struct qp_ctx *qp, bool is_admin)
{
	fprintf(stderr, "An %s queue has timed out. Dumping all outstanding commands from that queue\n",
		is_admin ? "Admin" : "I/O");
	print_cmd_list(qp);
	qp->timed_out = true;
}

static void submit_ns_cmds(struct ns_ctx *ns_entry);

static void
nvme_fuzz_cpl_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_cmd_ctx *ctx = cb_arg;
	struct qp_ctx *qp = ctx->qp;

	qp->cmd_counter++;
	if (spdk_unlikely(cpl->status.sc == SPDK_NVME_SC_SUCCESS)) {
		fprintf(stderr, "The following %s command (command num %lu) completed successfully\n",
			qp->is_admin ? "Admin" : "I/O", qp->cmd_counter);
		qp->successful_cmd_counter++;
		print_nvme_cmd(&ctx->cmd);

		if (qp->is_admin) {
			__sync_bool_compare_and_swap(&g_successful_admin_opcodes[ctx->cmd.opc], false, true);
		} else {
			__sync_bool_compare_and_swap(&g_successful_io_opcodes[ctx->cmd.opc], false, true);
		}
	} else if (g_verbose_mode == true) {
		fprintf(stderr, "The following %s command (command num %lu) failed as expected.\n",
			qp->is_admin ? "Admin" : "I/O", qp->cmd_counter);
		print_nvme_cmd(&ctx->cmd);
	}

	qp->timeout_tsc = fuzz_refresh_timeout();
	TAILQ_REMOVE(&qp->outstanding_ctx_objs, ctx, link);
	TAILQ_INSERT_HEAD(&qp->free_ctx_objs, ctx, link);
	assert(qp->num_cmds_outstanding > 0);
	qp->num_cmds_outstanding--;
}

static int
poll_for_completions(void *arg)
{
	struct ns_ctx *ns_entry = arg;
	uint64_t current_ticks = spdk_get_ticks();
	if (!ns_entry->io_qp.timed_out) {
		spdk_nvme_qpair_process_completions(ns_entry->io_qp.qpair, 0);
		/* SAlways have to process admin completions for the purposes of keep alive. */
		spdk_nvme_ctrlr_process_admin_completions(ns_entry->ctrlr);
	}

	if (current_ticks > g_runtime_ticks) {
		g_run = 0;
	}

	if (ns_entry->a_qp.timeout_tsc < current_ticks && !ns_entry->a_qp.timed_out &&
	    ns_entry->a_qp.num_cmds_outstanding > 0) {
		handle_timeout(&ns_entry->a_qp, true);
	}

	if (ns_entry->io_qp.timeout_tsc < current_ticks && !ns_entry->io_qp.timed_out &&
	    ns_entry->io_qp.num_cmds_outstanding > 0) {
		handle_timeout(&ns_entry->io_qp, false);
	}

	submit_ns_cmds(ns_entry);

	/* We either processed all I/O properly and can shut down normally */
	if (!g_run && ns_entry->io_qp.num_cmds_outstanding == 0 &&
	    ns_entry->a_qp.num_cmds_outstanding == 0) {
		spdk_poller_unregister(&ns_entry->req_poller);
		__sync_sub_and_fetch(&g_num_active_threads, 1);
		spdk_thread_exit(ns_entry->thread);
	}

	/* Or we had a qp time out and we need to exit without reducing the values to 0. */
	if (!g_run && ns_entry->io_qp.timed_out && (!g_run_admin_commands || ns_entry->a_qp.timed_out)) {
		spdk_poller_unregister(&ns_entry->req_poller);
		__sync_sub_and_fetch(&g_num_active_threads, 1);
		spdk_thread_exit(ns_entry->thread);
	}

	return 0;
}

static void
prep_nvme_cmd(struct ns_ctx *ns_entry, struct qp_ctx *qp, struct nvme_cmd_ctx *ctx)
{
	fuzz_fill_random_bytes((char *)&ctx->cmd, sizeof(ctx->cmd), &qp->random_seed);

	if (g_valid_ns_only) {
		ctx->cmd.nsid = ns_entry->nsid;
	}
}

static int
submit_qp_cmds(struct ns_ctx *ns, struct qp_ctx *qp)
{
	struct nvme_cmd_ctx *ctx;
	int rc;

	if (!qp->timed_out) {
		while (!TAILQ_EMPTY(&qp->free_ctx_objs)) {
			ctx = TAILQ_FIRST(&qp->free_ctx_objs);
			prep_nvme_cmd(ns, qp, ctx);

			TAILQ_REMOVE(&qp->free_ctx_objs, ctx, link);
			TAILQ_INSERT_HEAD(&qp->outstanding_ctx_objs, ctx, link);
			qp->num_cmds_outstanding++;

			if (qp->is_admin) {
				rc = spdk_nvme_ctrlr_cmd_admin_raw(ns->ctrlr, &ctx->cmd, NULL, 0, nvme_fuzz_cpl_cb, ctx);
			} else {
				rc = spdk_nvme_ctrlr_cmd_io_raw(ns->ctrlr, qp->qpair, &ctx->cmd, NULL, 0, nvme_fuzz_cpl_cb, ctx);
			}
			if (rc) {
				return rc;
			}
		}
	}
	return 0;
}

static void
submit_ns_cmds(struct ns_ctx *ns_entry)
{
	int rc;

	if (g_run) {
		if (g_run_admin_commands) {
			rc = submit_qp_cmds(ns_entry, &ns_entry->a_qp);
			if (rc) {
				goto err_exit;
			}
		}
		rc = submit_qp_cmds(ns_entry, &ns_entry->io_qp);
		if (rc) {
			goto err_exit;
		}

err_exit:
		if (rc) {
			/*
			 * I see the prospect of having a broken qpair on one ns as interesting
			 * enough to recommend stopping the application.
			 */
			fprintf(stderr, "Unable to submit command with rc %d\n", rc);
			g_run = 0;
		}
	}
}

static void
free_namespaces(void)
{
	struct ns_ctx *ns, *tmp;

	TAILQ_FOREACH_SAFE(ns, &g_ns_list, tailq, tmp) {
		printf("NS: %p I/O qp, Total commands completed: %lu, total successful commands: %lu\n", ns->ns,
		       ns->io_qp.cmd_counter, ns->io_qp.successful_cmd_counter);
		printf("NS: %p admin qp, Total commands completed: %lu, total successful commands: %lu\n", ns->ns,
		       ns->a_qp.cmd_counter, ns->a_qp.successful_cmd_counter);

		TAILQ_REMOVE(&g_ns_list, ns, tailq);
		if (ns->io_qp.qpair) {
			spdk_nvme_ctrlr_free_io_qpair(ns->io_qp.qpair);
		}
		if (ns->io_qp.req_ctx) {
			free(ns->io_qp.req_ctx);
		}
		if (ns->a_qp.req_ctx) {
			free(ns->a_qp.req_ctx);
		}
		free(ns);
	}
}

static void
free_controllers(void)
{
	struct ctrlr_ctx *ctrlr, *tmp;

	TAILQ_FOREACH_SAFE(ctrlr, &g_ctrlr_list, tailq, tmp) {
		TAILQ_REMOVE(&g_ctrlr_list, ctrlr, tailq);
		spdk_nvme_detach(ctrlr->ctrlr);
		free(ctrlr);
	}
}

static void
free_trids(void)
{
	struct trid_ctx *trid, *tmp;

	TAILQ_FOREACH_SAFE(trid, &g_trid_list, tailq, tmp) {
		TAILQ_REMOVE(&g_trid_list, trid, tailq);
		free(trid);
	}
}

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns, uint32_t nsid)
{
	struct ns_ctx *ns_entry;

	ns_entry = calloc(1, sizeof(struct ns_ctx));
	if (ns_entry == NULL) {
		fprintf(stderr, "Unable to allocate an entry for a namespace\n");
		return;
	}

	ns_entry->ns = ns;
	ns_entry->ctrlr = ctrlr;
	ns_entry->nsid = nsid;

	TAILQ_INIT(&ns_entry->io_qp.free_ctx_objs);
	TAILQ_INIT(&ns_entry->io_qp.outstanding_ctx_objs);
	if (g_run_admin_commands) {
		ns_entry->a_qp.qpair = NULL;
		TAILQ_INIT(&ns_entry->a_qp.free_ctx_objs);
		TAILQ_INIT(&ns_entry->a_qp.outstanding_ctx_objs);
	}
	TAILQ_INSERT_TAIL(&g_ns_list, ns_entry, tailq);
}

static void
register_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	struct ctrlr_ctx *ctrlr_entry;
	uint32_t nsid;
	struct spdk_nvme_ns *ns;

	ctrlr_entry = calloc(1, sizeof(struct ctrlr_ctx));
	if (ctrlr_entry == NULL) {
		fprintf(stderr, "Unable to allocate an entry for a controller\n");
		return;
	}

	ctrlr_entry->ctrlr = ctrlr;
	TAILQ_INSERT_TAIL(&g_ctrlr_list, ctrlr_entry, tailq);

	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns == NULL) {
			continue;
		}
		register_ns(ctrlr, ns, nsid);
	}
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	register_ctrlr(ctrlr);
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Controller trtype %s\ttraddr %s\n", spdk_nvme_transport_id_trtype_str(trid->trtype),
	       trid->traddr);

	return true;
}

static int
prep_qpair(struct ns_ctx *ns, struct qp_ctx *qp, uint32_t max_qdepth)
{
	uint32_t i;

	/* ensure that each qpair gets a unique random seed for maximum command dispersion. */

	if (g_seed_value != 0) {
		qp->random_seed = g_seed_value;
	} else {
		/* Take the low 32 bits of spdk_get_ticks. This should be more granular than time(). */
		qp->random_seed = spdk_get_ticks();
	}

	qp->timeout_tsc = fuzz_refresh_timeout();

	qp->req_ctx = calloc(max_qdepth, sizeof(struct nvme_cmd_ctx));
	if (qp->req_ctx == NULL) {
		fprintf(stderr, "Unable to allocate I/O contexts for I/O qpair.\n");
		return -1;
	}

	for (i = 0; i < max_qdepth; i++) {
		qp->req_ctx[i].qp = qp;
		TAILQ_INSERT_HEAD(&qp->free_ctx_objs, &qp->req_ctx[i], link);
	}

	return 0;
}

static int
prepare_qpairs(void)
{
	struct spdk_nvme_io_qpair_opts opts;
	struct ns_ctx *ns_entry;

	TAILQ_FOREACH(ns_entry, &g_ns_list, tailq) {
		spdk_nvme_ctrlr_get_default_io_qpair_opts(ns_entry->ctrlr, &opts, sizeof(opts));
		ns_entry->io_qp.qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, &opts, sizeof(opts));
		if (ns_entry->io_qp.qpair == NULL) {
			fprintf(stderr, "Unable to create a qpair for a namespace\n");
			return -1;
		}

		ns_entry->io_qp.is_admin = false;
		if (prep_qpair(ns_entry, &ns_entry->io_qp, g_io_depth) != 0) {
			fprintf(stderr, "Unable to allocate request contexts for I/O qpair.\n");
			return -1;
		}

		if (g_run_admin_commands) {
			ns_entry->a_qp.is_admin = true;
			if (prep_qpair(ns_entry, &ns_entry->a_qp, g_admin_depth) != 0) {
				fprintf(stderr, "Unable to allocate request contexts for admin qpair.\n");
				return -1;
			}
		}
	}
	return 0;
}

static void
start_ns_poller(void *ctx)
{
	struct ns_ctx *ns_entry = ctx;

	ns_entry->req_poller = spdk_poller_register(poll_for_completions, ns_entry, 0);
	submit_ns_cmds(ns_entry);
}

static int
check_app_completion(void *ctx)
{

	if (__sync_sub_and_fetch(&g_num_active_threads, 0) <= 0) {
		spdk_poller_unregister(&g_app_completion_poller);
		printf("Fuzzing completed. Shutting down the fuzz application\n\n");
		printf("Dumping successful admin opcodes:\n");
		report_successful_opcodes(g_successful_admin_opcodes, UNIQUE_OPCODES);
		printf("Dumping successful io opcodes:\n");
		report_successful_opcodes(g_successful_io_opcodes, UNIQUE_OPCODES);
		free_namespaces();
		free_controllers();
		free_trids();
		spdk_app_stop(0);
	}
	return 0;
}

static void
begin_fuzz(void *ctx)
{
	struct ns_ctx *ns_entry;
	struct trid_ctx *trid;
	int rc;

	if (!spdk_iommu_is_enabled()) {
		/* Don't set rc to an error code here. We don't want to fail an automated test based on this. */
		fprintf(stderr, "The IOMMU must be enabled to run this program to avoid unsafe memory accesses.\n");
		rc = 0;
		goto out;
	}

	TAILQ_FOREACH(trid, &g_trid_list, tailq) {
		if (spdk_nvme_probe(&trid->trid, trid, probe_cb, attach_cb, NULL) != 0) {
			fprintf(stderr, "spdk_nvme_probe() failed for transport address '%s'\n",
				trid->trid.traddr);
			rc = -1;
			goto out;
		}
	}

	if (TAILQ_EMPTY(&g_ns_list)) {
		fprintf(stderr, "No valid NVMe Namespaces to fuzz\n");
		rc = -EINVAL;
		goto out;
	}

	rc = prepare_qpairs();

	if (rc < 0) {
		fprintf(stderr, "Unable to prepare the qpairs\n");
		goto out;
	}

	g_runtime_ticks = spdk_get_ticks() + g_runtime * spdk_get_ticks_hz();

	/* Assigning all of the threads and then starting them makes cleanup easier. */
	TAILQ_FOREACH(ns_entry, &g_ns_list, tailq) {
		ns_entry->thread = spdk_thread_create(NULL, NULL);
		if (ns_entry->thread == NULL) {
			fprintf(stderr, "Failed to allocate thread for namespace.\n");
			goto out;
		}
	}

	TAILQ_FOREACH(ns_entry, &g_ns_list, tailq) {
		spdk_thread_send_msg(ns_entry->thread, start_ns_poller, ns_entry);
		__sync_add_and_fetch(&g_num_active_threads, 1);
	}

	g_app_completion_poller = spdk_poller_register(check_app_completion, NULL, 0);
	return;
out:
	printf("Shutting down the fuzz application\n");
	free_namespaces();
	free_controllers();
	free_trids();
	spdk_app_stop(rc);
}

static int
parse_trids(void)
{
	struct spdk_conf *config = NULL;
	struct spdk_conf_section *sp;
	const char *trid_char;
	struct trid_ctx *current_trid;
	int num_subsystems = 0;
	int rc = 0;

	if (g_conf_file) {
		config = spdk_conf_allocate();
		if (!config) {
			fprintf(stderr, "Unable to allocate an spdk_conf object\n");
			return -1;
		}

		rc = spdk_conf_read(config, g_conf_file);
		if (rc) {
			fprintf(stderr, "Unable to convert the conf file into a readable system\n");
			rc = -1;
			goto exit;
		}

		sp = spdk_conf_find_section(config, "Nvme");

		if (sp == NULL) {
			fprintf(stderr, "No Nvme configuration in conf file\n");
			goto exit;
		}

		while ((trid_char = spdk_conf_section_get_nmval(sp, "TransportID", num_subsystems, 0)) != NULL) {
			current_trid = malloc(sizeof(struct trid_ctx));
			if (!current_trid) {
				fprintf(stderr, "Unable to allocate memory for transport ID\n");
				rc = -1;
				goto exit;
			}
			rc = spdk_nvme_transport_id_parse(&current_trid->trid, trid_char);

			if (rc < 0) {
				fprintf(stderr, "failed to parse transport ID: %s\n", trid_char);
				free(current_trid);
				rc = -1;
				goto exit;
			}
			TAILQ_INSERT_TAIL(&g_trid_list, current_trid, tailq);
			num_subsystems++;
		}
	}

exit:
	if (config != NULL) {
		spdk_conf_free(config);
	}
	return rc;
}

static void
nvme_fuzz_usage(void)
{
	fprintf(stderr, " -a                        Also perform admin requests\n");
	fprintf(stderr, " -C <path>                 path to a configuration file.\n");
	fprintf(stderr, " -N                        Target only valid namespace with commands. \
This helps dig deeper into other errors besides invalid namespace.\n");
	fprintf(stderr, " -S <integer>              seed value for test.\n");
	fprintf(stderr, " -t <integer>              time in seconds to run the fuzz test.\n");
	fprintf(stderr, " -v                        enable logging of each submitted command.\n");
}

static int
nvme_fuzz_parse(int ch, char *arg)
{
	int64_t error_test;

	switch (ch) {
	case 'a':
		g_run_admin_commands = true;
		break;
	case 'C':
		g_conf_file = optarg;
		break;
	case 'N':
		g_valid_ns_only = true;
		break;
	case 'S':
		error_test = spdk_strtol(arg, 10);
		if (error_test < 0) {
			fprintf(stderr, "Invalid value supplied for the random seed.\n");
			return -1;
		} else {
			g_seed_value = spdk_strtol(arg, 10);
		}
		break;
	case 't':
		g_runtime = spdk_strtol(optarg, 10);
		if (g_runtime < 0 || g_runtime > MAX_RUNTIME_S) {
			fprintf(stderr, "You must supply a positive runtime value less than 86401.\n");
			return -1;
		}
		break;
	case 'v':
		g_verbose_mode = true;
		break;
	case '?':
	default:
		return -EINVAL;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc;

	spdk_app_opts_init(&opts);
	opts.name = "nvme_fuzz";

	g_runtime = DEFAULT_RUNTIME;
	g_run = true;

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "aC:NS:t:v", NULL, nvme_fuzz_parse,
				      nvme_fuzz_usage) != SPDK_APP_PARSE_ARGS_SUCCESS)) {
		return rc;
	}

	if (g_conf_file) {
		parse_trids();
	}

	rc = spdk_app_start(&opts, begin_fuzz, NULL);

	return rc;
}
