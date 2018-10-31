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
#include "spdk/nvme_spec.h"
#include "spdk/nvme.h"
#include "spdk/likely.h"
#include "../../../lib/nvme/nvme_internal.h"

#define DEFAULT_RUNTIME 30 /* seconds */
#define S_TO_US 1000000
#define IO_TIMEOUT_S 5
#define IO_TIMEOUT_US (S_TO_US * IO_TIMEOUT_S) /* wait up to 5 seconds before abandoning the controller */

char *g_conf_file;
uint64_t g_io_counter;
uint64_t g_successful_io;
uint64_t g_prev_io_counter;
uint32_t g_runtime;
uint64_t g_runtime_ticks;
uint32_t g_outstanding_io;

bool g_local_nvme = false;
bool g_valid_ns_only = false;
bool g_run;

struct nvme_cmd_ctx {
	struct nvme_request	*req;
	uint64_t		timeout_tsc;
};

struct spdk_trid_list_entry {
	struct spdk_nvme_transport_id		trid;
	TAILQ_ENTRY(spdk_trid_list_entry)	tailq;
};

struct spdk_ctrlr_list_entry {
	struct spdk_nvme_ctrlr			*ctrlr;
	TAILQ_ENTRY(spdk_ctrlr_list_entry)	tailq;
};

struct spdk_ns_list_entry {
	struct spdk_nvme_ns		*ns;
	struct spdk_nvme_ctrlr		*ctrlr;
	struct spdk_nvme_qpair		*qpair;
	struct nvme_cmd_ctx		ctx;
	TAILQ_ENTRY(spdk_ns_list_entry)	tailq;
};

static TAILQ_HEAD(, spdk_ns_list_entry) g_ns_list = TAILQ_HEAD_INITIALIZER(g_ns_list);
static TAILQ_HEAD(, spdk_ctrlr_list_entry) g_ctrlr_list = TAILQ_HEAD_INITIALIZER(g_ctrlr_list);
static TAILQ_HEAD(, spdk_trid_list_entry) g_trid_list = TAILQ_HEAD_INITIALIZER(g_trid_list);

static void
print_nvme_cmd(struct spdk_nvme_cmd *cmd)
{
	SPDK_NOTICELOG("opc %u\n", cmd->opc);
	SPDK_NOTICELOG("fuse %u\n", cmd->fuse);
	SPDK_NOTICELOG("rsvd1 %u\n", cmd->rsvd1);
	SPDK_NOTICELOG("psdt %u\n", cmd->psdt);
	SPDK_NOTICELOG("cid %u\n", cmd->cid);
	SPDK_NOTICELOG("nsid %u\n", cmd->nsid);
	SPDK_NOTICELOG("rsvd2 %u\n", cmd->rsvd2);
	SPDK_NOTICELOG("rsvd3 %u\n", cmd->rsvd3);
	SPDK_NOTICELOG("mptr %lu\n", cmd->mptr);
	SPDK_NOTICELOG("cdw10 %u\n", cmd->cdw10);
	SPDK_NOTICELOG("cdw11 %u\n", cmd->cdw11);
	SPDK_NOTICELOG("cdw12 %u\n", cmd->cdw12);
	SPDK_NOTICELOG("cdw13 %u\n", cmd->cdw13);
	SPDK_NOTICELOG("cdw14 %u\n", cmd->cdw14);
	SPDK_NOTICELOG("cdw15 %u\n", cmd->cdw15);
}

static void
report_timeout(struct spdk_ns_list_entry *ns_entry)
{
	SPDK_ERRLOG("The following command has timed out. It is possible that it caused a target/drive to hang.\n");
	SPDK_ERRLOG("Number of completed I/O previous to this: %lu\n", g_io_counter);
	print_nvme_cmd(&ns_entry->ctx.req->cmd);
	--g_outstanding_io;
	nvme_free_request(ns_entry->ctx.req);

	SPDK_ERRLOG("Stopping I/O\n");
	g_run = 0;
}

static void submit_next_io(struct spdk_ns_list_entry *ns_entry);

static void
nvme_fuzz_cpl_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_ns_list_entry *ns_entry = cb_arg;

	++g_io_counter;
	if (spdk_unlikely(cpl->status.sc == SPDK_NVME_SC_SUCCESS)) {
		SPDK_NOTICELOG("The following io command (i/o num %lu) completed successfully\n", g_io_counter);
		g_successful_io++;
		print_nvme_cmd(&ns_entry->ctx.req->cmd);
	}
	--g_outstanding_io;

	nvme_free_request(ns_entry->ctx.req);

	submit_next_io(ns_entry);
}

static void
seed_random(void)
{
	time_t seed_time;
	seed_time = time(0);
	SPDK_NOTICELOG("Seed value for this run %lu\n", seed_time);
	srand(time(0));
}

static uint8_t
random_character(void)
{
	return rand() % UINT8_MAX;
}

static struct nvme_request *
prep_nvme_req(struct spdk_ns_list_entry *ns_entry)
{
	size_t cmd_size = sizeof(struct spdk_nvme_cmd);
	struct nvme_request *req = nvme_allocate_request_null(ns_entry->qpair, nvme_fuzz_cpl_cb, ns_entry);
	char *character_repr = (char *)&req->cmd;
	size_t i;

	for (i = 0; i < cmd_size; i++) {
		character_repr[i] = random_character();
	}

	if (g_valid_ns_only) {
		req->cmd.nsid = ns_entry->ns->id;
	}

	return req;
}

static void poll_for_completions(void)
{
	struct spdk_ns_list_entry *ns_entry;
	uint64_t current_ticks = spdk_get_ticks();

	TAILQ_FOREACH(ns_entry, &g_ns_list, tailq) {
		spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
		if (current_ticks > ns_entry->ctx.timeout_tsc) {
			report_timeout(ns_entry);
		}
		if (current_ticks > g_runtime_ticks) {
			g_run = 0;
		}
	}
}

static void
submit_next_io(struct spdk_ns_list_entry *ns_entry)
{
	int rc;
	uint64_t tsc;

	if (g_run) {

		ns_entry->ctx.req = prep_nvme_req(ns_entry);

		tsc = spdk_get_ticks();
		ns_entry->ctx.timeout_tsc = tsc + spdk_get_ticks_hz() * IO_TIMEOUT_S;

		assert(tsc < ns_entry->ctx.timeout_tsc);

		if ((rc = nvme_qpair_submit_request(ns_entry->qpair, ns_entry->ctx.req))) {
			/* TODO: add timeout information here */
			SPDK_ERRLOG("Unable to submit command with %lu total io and %u outstanding io and rc %d\n",
				    g_io_counter, g_outstanding_io, rc);
			free(ns_entry->ctx.req);
			g_run = 0;
			return;
		}
		g_outstanding_io++;
	}
}

static void
begin_fuzz(void)
{
	struct spdk_ns_list_entry *ns_entry;

	seed_random();

	g_runtime_ticks = spdk_get_ticks() + g_runtime * spdk_get_ticks_hz();

	assert(g_runtime_ticks > spdk_get_ticks());

	TAILQ_FOREACH(ns_entry, &g_ns_list, tailq) {
		submit_next_io(ns_entry);
	}

	while (g_run || g_outstanding_io) {
		poll_for_completions();
		if (!g_run) {
			printf("draining I/O. Remaining: %d\n", g_outstanding_io);
		}
	}
}

static int
prepare_io_qpairs(void)
{
	struct spdk_nvme_io_qpair_opts opts;
	struct spdk_ns_list_entry *ns_entry;

	TAILQ_FOREACH(ns_entry, &g_ns_list, tailq) {
		spdk_nvme_ctrlr_get_default_io_qpair_opts(ns_entry->ctrlr, &opts, sizeof(opts));
		ns_entry->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, &opts, sizeof(opts));
		if (ns_entry->qpair == NULL) {
			SPDK_ERRLOG("Unable to create a qpair for a namespace\n");
			return -1;
		}
	}
	return 0;
}

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct spdk_ns_list_entry *ns_entry;

	ns_entry = calloc(1, sizeof(struct spdk_ns_list_entry));
	if (ns_entry == NULL) {
		SPDK_ERRLOG("Unable to allocat an entry for a namespace\n");
		return;
	}

	ns_entry->ns = ns;
	ns_entry->ctrlr = ctrlr;
	TAILQ_INSERT_TAIL(&g_ns_list, ns_entry, tailq);
}

static void
register_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_ctrlr_list_entry *ctrlr_entry;
	uint32_t nsid;
	struct spdk_nvme_ns *ns;

	ctrlr_entry = calloc(1, sizeof(struct spdk_ctrlr_list_entry));
	if (ctrlr_entry == NULL) {
		SPDK_ERRLOG("Unable to allocate an entry for a controller\n");
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
		register_ns(ctrlr, ns);
	}
}

static int
parse_trids(void)
{
	struct spdk_conf *config = NULL;
	struct spdk_conf_section *sp;
	const char *trid_char;
	struct spdk_trid_list_entry *current_trid;
	int num_subsystems = 0;
	int rc = 0;

	if (g_conf_file) {
		config = spdk_conf_allocate();
		if (!config) {
			SPDK_ERRLOG("Unable to allocate an spdk_conf object\n");
			return -1;
		}

		rc = spdk_conf_read(config, g_conf_file);
		if (rc) {
			SPDK_ERRLOG("Unable to convert the conf file into a readable system\n");
			rc = -1;
			goto exit;
		}

		sp = spdk_conf_find_section(config, "Nvme");

		if (sp == NULL) {
			SPDK_ERRLOG("No Nvme configuration in conf file\n");
			goto exit;
		}

		while ((trid_char = spdk_conf_section_get_nmval(sp, "TransportID", num_subsystems, 0)) != NULL) {
			current_trid = malloc(sizeof(struct spdk_trid_list_entry));
			if (!current_trid) {
				SPDK_ERRLOG("Unable to allocate memory for transport ID\n");
				rc = -1;
				goto exit;
			}
			rc = spdk_nvme_transport_id_parse(&current_trid->trid, trid_char);

			if (rc < 0) {
				SPDK_ERRLOG("failed to parse transport ID: %s\n", trid_char);
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

static void free_namespaces(void)
{
	struct spdk_ns_list_entry *ns, *tmp;

	TAILQ_FOREACH_SAFE(ns, &g_ns_list, tailq, tmp) {
		TAILQ_REMOVE(&g_ns_list, ns, tailq);
		if (ns->qpair) {
			spdk_nvme_ctrlr_free_io_qpair(ns->qpair);
		}
		free(ns);
	}
}

static void free_controllers(void)
{
	struct spdk_ctrlr_list_entry *ctrlr, *tmp;

	TAILQ_FOREACH_SAFE(ctrlr, &g_ctrlr_list, tailq, tmp) {
		TAILQ_REMOVE(&g_ctrlr_list, ctrlr, tailq);
		spdk_nvme_detach(ctrlr->ctrlr);
		free(ctrlr);
	}
}

static void free_trids(void)
{
	struct spdk_trid_list_entry *trid, *tmp;

	TAILQ_FOREACH_SAFE(trid, &g_trid_list, tailq, tmp) {
		TAILQ_REMOVE(&g_trid_list, trid, tailq);
		free(trid);
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
	SPDK_NOTICELOG("Controller trtype %s\ttraddr %s\n", spdk_nvme_transport_id_trtype_str(trid->trtype),
		       trid->traddr);

	return true;
}

static void
nvme_fuzz_usage(void)
{
	SPDK_ERRLOG(" -t <integer>              time in second to run the fuzz test.\n");
	SPDK_ERRLOG(" -c <path>                 path to a configuration file.\n");
	SPDK_ERRLOG(" -n                        Target only valid namespace with commands. \
This helps dig deeper into other errors besides invalid namespace.\n");
	SPDK_ERRLOG(" -p                        Include local NVMe drives\n");
	SPDK_ERRLOG(" -h                        print this message.\n");
}

static int
nvme_fuzz_parse(int argc, char **argv)
{
	int ch;

	while ((ch = getopt(argc, argv, "t:c:nph")) != -1) {
		switch (ch) {
		case 't':
			g_runtime = atoi(optarg);
			break;
		case 'c':
			g_conf_file = optarg;
			break;
		case 'p':
			g_local_nvme = true;
			break;
		case 'h':
			nvme_fuzz_usage();
			return -1;
		case 'n':
			g_valid_ns_only = true;
			break;
		case '?':
			nvme_fuzz_usage();
			return -1;
		default:
			nvme_fuzz_usage();
			break;
		}
	}
	return 0;
}

int
main(int argc, char **argv)
{
	struct spdk_env_opts opts = {};
	struct spdk_trid_list_entry *trid;
	int rc;

	spdk_env_opts_init(&opts);
	opts.name = "nvme_fuzz";
	opts.mem_size = 2048;
	g_runtime = DEFAULT_RUNTIME;

	if (nvme_fuzz_parse(argc, argv) < 0) {
		return -1;
	}

	if (g_conf_file) {
		SPDK_ERRLOG("Found a configuration file\n");
		parse_trids();
	}

	rc = spdk_env_init(&opts);

	if (rc < 0) {
		SPDK_ERRLOG("Unable to initialize the SPDK environment\n");
		return rc;
	}

	g_run = true;
	g_outstanding_io = 0;
	g_prev_io_counter = 0;
	g_io_counter = 0;
	g_successful_io = 0;

	if (g_local_nvme) {
		rc = spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL);
		if (rc) {
			SPDK_ERRLOG("unable to probe local NVMe drives\n");
			goto out;
		}
	}

	TAILQ_FOREACH(trid, &g_trid_list, tailq) {
		if (spdk_nvme_probe(&trid->trid, trid, probe_cb, attach_cb, NULL) != 0) {
			SPDK_ERRLOG("spdk_nvme_probe() failed for transport address '%s'\n",
				    trid->trid.traddr);
			rc = -1;
			goto out;
		}
	}

	if (TAILQ_EMPTY(&g_ns_list)) {
		SPDK_ERRLOG("No valid NVMe Namespaces to fuzz\n");
		goto out;
	}

	rc = prepare_io_qpairs();

	if (rc < 0) {
		SPDK_ERRLOG("Unable to prepare the Qpairs\n");
		goto out;
	}

	begin_fuzz();

	SPDK_NOTICELOG("Total I/O completed: %lu, total successful I/O: %lu\n", g_io_counter,
		       g_successful_io);

out:
	SPDK_NOTICELOG("Shutting down the fuzz application\n");
	free_namespaces();
	free_controllers();
	free_trids();
	return rc;
}
