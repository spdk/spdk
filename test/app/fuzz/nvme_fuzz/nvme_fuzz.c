/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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
#include "spdk/json.h"
#include "fuzz_common.h"

#define UNIQUE_OPCODES 256

const char g_nvme_cmd_json_name[] = "struct spdk_nvme_cmd";
char *g_conf_file;
char *g_json_file = NULL;
uint64_t g_runtime_ticks;
unsigned int g_seed_value = 0;
int g_runtime;

int g_num_active_threads = 0;
uint32_t g_admin_depth = 16;
uint32_t g_io_depth = 128;

bool g_valid_ns_only = false;
bool g_verbose_mode = false;
bool g_run_admin_commands = false;
bool g_run;

struct spdk_poller *g_app_completion_poller;
bool g_successful_io_opcodes[UNIQUE_OPCODES] = {0};
bool g_successful_admin_opcodes[UNIQUE_OPCODES] = {0};

struct spdk_nvme_cmd *g_cmd_array;
size_t g_cmd_array_size;

/* I need context objects here because I need to keep track of all I/O that are in flight. */
struct nvme_fuzz_request {
	struct spdk_nvme_cmd		cmd;
	struct nvme_fuzz_qp		*qp;
	TAILQ_ENTRY(nvme_fuzz_request)	link;
};

struct nvme_fuzz_trid {
	struct spdk_nvme_transport_id	trid;
	TAILQ_ENTRY(nvme_fuzz_trid)	tailq;
};

struct nvme_fuzz_ctrlr {
	struct spdk_nvme_ctrlr		*ctrlr;
	TAILQ_ENTRY(nvme_fuzz_ctrlr)	tailq;
};

struct nvme_fuzz_qp {
	struct spdk_nvme_qpair		*qpair;
	/* array of context objects equal in length to the queue depth */
	struct nvme_fuzz_request	*req_ctx;
	TAILQ_HEAD(, nvme_fuzz_request)	free_ctx_objs;
	TAILQ_HEAD(, nvme_fuzz_request)	outstanding_ctx_objs;
	unsigned int			random_seed;
	uint64_t			completed_cmd_counter;
	uint64_t			submitted_cmd_counter;
	uint64_t			successful_completed_cmd_counter;
	uint64_t			timeout_tsc;
	uint32_t			num_cmds_outstanding;
	bool				timed_out;
	bool				is_admin;
};

struct nvme_fuzz_ns {
	struct spdk_nvme_ns		*ns;
	struct spdk_nvme_ctrlr		*ctrlr;
	struct spdk_thread		*thread;
	struct spdk_poller		*req_poller;
	struct nvme_fuzz_qp		io_qp;
	struct nvme_fuzz_qp		a_qp;
	uint32_t			nsid;
	TAILQ_ENTRY(nvme_fuzz_ns)	tailq;
};

static TAILQ_HEAD(, nvme_fuzz_ns) g_ns_list = TAILQ_HEAD_INITIALIZER(g_ns_list);
static TAILQ_HEAD(, nvme_fuzz_ctrlr) g_ctrlr_list = TAILQ_HEAD_INITIALIZER(g_ctrlr_list);
static TAILQ_HEAD(, nvme_fuzz_trid) g_trid_list = TAILQ_HEAD_INITIALIZER(g_trid_list);

static bool
parse_nvme_cmd_obj(void *item, struct spdk_json_val *value, size_t num_values)
{
	struct spdk_nvme_cmd *cmd = item;
	struct spdk_json_val *next_val;
	uint64_t tmp_val;
	size_t i = 0;

	while (i < num_values) {
		if (value->type == SPDK_JSON_VAL_NAME) {
			next_val = value + 1;
			if (!strncmp(value->start, "opc", value->len)) {
				if (next_val->type == SPDK_JSON_VAL_NUMBER) {
					if (fuzz_parse_json_num(next_val, UNSIGNED_8BIT_MAX, &tmp_val)) {
						goto invalid;
					}
					cmd->opc = tmp_val;
				}
			} else if (!strncmp(value->start, "fuse", value->len)) {
				if (next_val->type == SPDK_JSON_VAL_NUMBER) {
					if (fuzz_parse_json_num(next_val, UNSIGNED_2BIT_MAX, &tmp_val)) {
						goto invalid;
					}
					cmd->fuse = tmp_val;
				}
			} else if (!strncmp(value->start, "rsvd1", value->len)) {
				if (next_val->type == SPDK_JSON_VAL_NUMBER) {
					if (fuzz_parse_json_num(next_val, UNSIGNED_4BIT_MAX, &tmp_val)) {
						goto invalid;
					}
					cmd->rsvd1 = tmp_val;
				}
			} else if (!strncmp(value->start, "psdt", value->len)) {
				if (next_val->type == SPDK_JSON_VAL_NUMBER) {
					if (fuzz_parse_json_num(next_val, UNSIGNED_2BIT_MAX, &tmp_val)) {
						goto invalid;
					}
					cmd->psdt = tmp_val;
				}
			} else if (!strncmp(value->start, "cid", value->len)) {
				if (next_val->type == SPDK_JSON_VAL_NUMBER) {
					if (fuzz_parse_json_num(next_val, UINT16_MAX, &tmp_val)) {
						goto invalid;
					}
					cmd->cid = tmp_val;
				}
			} else if (!strncmp(value->start, "nsid", value->len)) {
				if (next_val->type == SPDK_JSON_VAL_NUMBER) {
					if (fuzz_parse_json_num(next_val, UINT32_MAX, &tmp_val)) {
						goto invalid;
					}
					cmd->nsid = tmp_val;
				}
			} else if (!strncmp(value->start, "rsvd2", value->len)) {
				if (next_val->type == SPDK_JSON_VAL_NUMBER) {
					if (fuzz_parse_json_num(next_val, UINT32_MAX, &tmp_val)) {
						goto invalid;
					}
					cmd->rsvd2 = tmp_val;
				}
			} else if (!strncmp(value->start, "rsvd3", value->len)) {
				if (next_val->type == SPDK_JSON_VAL_NUMBER) {
					if (fuzz_parse_json_num(next_val, UINT32_MAX, &tmp_val)) {
						goto invalid;
					}
					cmd->rsvd3 = tmp_val;
				}
			} else if (!strncmp(value->start, "mptr", value->len)) {
				if (next_val->type == SPDK_JSON_VAL_NUMBER) {
					if (fuzz_parse_json_num(next_val, UINT64_MAX, &tmp_val)) {
						goto invalid;
					}
					cmd->mptr = tmp_val;
				}
			} else if (!strncmp(value->start, "dptr", value->len)) {
				if (next_val->type == SPDK_JSON_VAL_STRING) {
					if (fuzz_get_base_64_buffer_value(&cmd->dptr, sizeof(cmd->dptr), (char *)next_val->start,
									  next_val->len)) {
						goto invalid;
					}
				}
			} else if (!strncmp(value->start, "cdw10", value->len)) {
				if (next_val->type == SPDK_JSON_VAL_NUMBER) {
					if (fuzz_parse_json_num(next_val, UINT32_MAX, &tmp_val)) {
						goto invalid;
					}
					cmd->cdw10 = tmp_val;
				}
			} else if (!strncmp(value->start, "cdw11", value->len)) {
				if (next_val->type == SPDK_JSON_VAL_NUMBER) {
					if (fuzz_parse_json_num(next_val, UINT32_MAX, &tmp_val)) {
						goto invalid;
					}
					cmd->cdw11 = tmp_val;
				}
			} else if (!strncmp(value->start, "cdw12", value->len)) {
				if (next_val->type == SPDK_JSON_VAL_NUMBER) {
					if (fuzz_parse_json_num(next_val, UINT32_MAX, &tmp_val)) {
						goto invalid;
					}
					cmd->cdw12 = tmp_val;
				}
			} else if (!strncmp(value->start, "cdw13", value->len)) {
				if (next_val->type == SPDK_JSON_VAL_NUMBER) {
					if (fuzz_parse_json_num(next_val, UINT32_MAX, &tmp_val)) {
						goto invalid;
					}
					cmd->cdw13 = tmp_val;
				}
			} else if (!strncmp(value->start, "cdw14", value->len)) {
				if (next_val->type == SPDK_JSON_VAL_NUMBER) {
					if (fuzz_parse_json_num(next_val, UINT32_MAX, &tmp_val)) {
						goto invalid;
					}
					cmd->cdw14 = tmp_val;
				}
			} else if (!strncmp(value->start, "cdw15", value->len)) {
				if (next_val->type == SPDK_JSON_VAL_NUMBER) {
					if (fuzz_parse_json_num(next_val, UINT32_MAX, &tmp_val)) {
						goto invalid;
					}
					cmd->cdw15 = tmp_val;
				}
			}
		}
		i++;
		value++;
	}
	return true;

invalid:
	fprintf(stderr, "Invalid value supplied for cmd->%.*s: %.*s\n", value->len, (char *)value->start,
		next_val->len, (char *)next_val->start);
	return false;
}

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

static int
print_nvme_cmd(void *cb_ctx, const void *data, size_t size)
{
	fprintf(stderr, "%s\n", (const char *)data);
	return 0;
}

static void
json_dump_nvme_cmd(struct spdk_nvme_cmd *cmd)
{
	struct spdk_json_write_ctx *w;
	char *dptr_value;

	dptr_value = fuzz_get_value_base_64_buffer(&cmd->dptr, sizeof(cmd->dptr));
	if (dptr_value == NULL) {
		fprintf(stderr, "Unable to allocate buffer context for printing command.\n");
		return;
	}

	w = spdk_json_write_begin(print_nvme_cmd, cmd, SPDK_JSON_WRITE_FLAG_FORMATTED);
	if (w == NULL) {
		fprintf(stderr, "Unable to allocate json context for printing command.\n");
		free(dptr_value);
		return;
	}

	spdk_json_write_named_object_begin(w, g_nvme_cmd_json_name);
	spdk_json_write_named_uint32(w, "opc", cmd->opc);
	spdk_json_write_named_uint32(w, "fuse", cmd->fuse);
	spdk_json_write_named_uint32(w, "rsvd1", cmd->rsvd1);
	spdk_json_write_named_uint32(w, "psdt", cmd->psdt);
	spdk_json_write_named_uint32(w, "cid", cmd->cid);
	spdk_json_write_named_uint32(w, "nsid", cmd->nsid);
	spdk_json_write_named_uint32(w, "rsvd2", cmd->rsvd2);
	spdk_json_write_named_uint32(w, "rsvd3", cmd->rsvd3);
	spdk_json_write_named_uint32(w, "mptr", cmd->mptr);
	spdk_json_write_named_string(w, "dptr", dptr_value);
	spdk_json_write_named_uint32(w, "cdw10", cmd->cdw10);
	spdk_json_write_named_uint32(w, "cdw11", cmd->cdw11);
	spdk_json_write_named_uint32(w, "cdw12", cmd->cdw12);
	spdk_json_write_named_uint32(w, "cdw13", cmd->cdw13);
	spdk_json_write_named_uint32(w, "cdw14", cmd->cdw14);
	spdk_json_write_named_uint32(w, "cdw15", cmd->cdw15);
	spdk_json_write_object_end(w);

	free(dptr_value);
	spdk_json_write_end(w);
}

static void
json_dump_nvme_cmd_list(struct nvme_fuzz_qp *qp)
{
	struct nvme_fuzz_request *ctx;

	TAILQ_FOREACH(ctx, &qp->outstanding_ctx_objs, link) {
		json_dump_nvme_cmd(&ctx->cmd);
	}
}

static void
handle_timeout(struct nvme_fuzz_qp *qp, bool is_admin)
{
	fprintf(stderr, "An %s queue has timed out. Dumping all outstanding commands from that queue\n",
		is_admin ? "Admin" : "I/O");
	json_dump_nvme_cmd_list(qp);
	qp->timed_out = true;
}

static void submit_ns_cmds(struct nvme_fuzz_ns *ns_entry);

static void
nvme_fuzz_cpl_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_fuzz_request *ctx = cb_arg;
	struct nvme_fuzz_qp *qp = ctx->qp;

	qp->completed_cmd_counter++;
	if (spdk_unlikely(cpl->status.sc == SPDK_NVME_SC_SUCCESS)) {
		fprintf(stderr, "The following %s command (command num %" PRIu64 ") completed successfully\n",
			qp->is_admin ? "Admin" : "I/O", qp->completed_cmd_counter);
		qp->successful_completed_cmd_counter++;
		json_dump_nvme_cmd(&ctx->cmd);

		if (qp->is_admin) {
			__sync_bool_compare_and_swap(&g_successful_admin_opcodes[ctx->cmd.opc], false, true);
		} else {
			__sync_bool_compare_and_swap(&g_successful_io_opcodes[ctx->cmd.opc], false, true);
		}
	} else if (g_verbose_mode == true) {
		fprintf(stderr, "The following %s command (command num %" PRIu64 ") failed as expected.\n",
			qp->is_admin ? "Admin" : "I/O", qp->completed_cmd_counter);
		json_dump_nvme_cmd(&ctx->cmd);
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
	struct nvme_fuzz_ns *ns_entry = arg;
	uint64_t current_ticks = spdk_get_ticks();
	uint64_t *counter;
	if (!ns_entry->io_qp.timed_out) {
		spdk_nvme_qpair_process_completions(ns_entry->io_qp.qpair, 0);
		/* SAlways have to process admin completions for the purposes of keep alive. */
		spdk_nvme_ctrlr_process_admin_completions(ns_entry->ctrlr);
	}

	if (g_cmd_array) {
		if (g_run_admin_commands) {
			counter = &ns_entry->a_qp.submitted_cmd_counter;
		} else {
			counter = &ns_entry->io_qp.submitted_cmd_counter;
		}

		if (*counter >= g_cmd_array_size) {
			g_run = false;
		}
	} else {
		if (current_ticks > g_runtime_ticks) {
			g_run = false;
		}
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

	if (g_run) {
		return 0;
	}
	/*
	 * We either processed all I/O properly and can shut down normally, or we
	 * had a qp time out and we need to exit without reducing the values to 0.
	 */
	if (ns_entry->io_qp.num_cmds_outstanding == 0 &&
	    ns_entry->a_qp.num_cmds_outstanding == 0) {
		goto exit_handler;
	} else if (ns_entry->io_qp.timed_out && (!g_run_admin_commands || ns_entry->a_qp.timed_out)) {
		goto exit_handler;
	} else {
		return 0;
	}

exit_handler:
	spdk_poller_unregister(&ns_entry->req_poller);
	__sync_sub_and_fetch(&g_num_active_threads, 1);
	spdk_thread_exit(ns_entry->thread);
	return 0;
}

static void
prep_nvme_cmd(struct nvme_fuzz_ns *ns_entry, struct nvme_fuzz_qp *qp, struct nvme_fuzz_request *ctx)
{
	if (g_cmd_array) {
		memcpy(&ctx->cmd, &g_cmd_array[qp->submitted_cmd_counter], sizeof(ctx->cmd));
	} else {
		fuzz_fill_random_bytes((char *)&ctx->cmd, sizeof(ctx->cmd), &qp->random_seed);

		if (g_valid_ns_only) {
			ctx->cmd.nsid = ns_entry->nsid;
		}
	}
}

static int
submit_qp_cmds(struct nvme_fuzz_ns *ns, struct nvme_fuzz_qp *qp)
{
	struct nvme_fuzz_request *ctx;
	int rc;

	if (qp->timed_out) {
		return 0;
	}
	/* If we are reading from an array, we need to stop after the last one. */
	while ((qp->submitted_cmd_counter < g_cmd_array_size || g_cmd_array_size == 0) &&
	       !TAILQ_EMPTY(&qp->free_ctx_objs)) {
		ctx = TAILQ_FIRST(&qp->free_ctx_objs);
		do {
			prep_nvme_cmd(ns, qp, ctx);
		} while (qp->is_admin && ctx->cmd.opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST);

		TAILQ_REMOVE(&qp->free_ctx_objs, ctx, link);
		TAILQ_INSERT_HEAD(&qp->outstanding_ctx_objs, ctx, link);
		qp->num_cmds_outstanding++;
		qp->submitted_cmd_counter++;
		if (qp->is_admin) {
			rc = spdk_nvme_ctrlr_cmd_admin_raw(ns->ctrlr, &ctx->cmd, NULL, 0, nvme_fuzz_cpl_cb, ctx);
		} else {
			rc = spdk_nvme_ctrlr_cmd_io_raw(ns->ctrlr, qp->qpair, &ctx->cmd, NULL, 0, nvme_fuzz_cpl_cb, ctx);
		}
		if (rc) {
			return rc;
		}
	}
	return 0;
}

static void
submit_ns_cmds(struct nvme_fuzz_ns *ns_entry)
{
	int rc;

	if (!g_run) {
		return;
	}

	if (g_run_admin_commands) {
		rc = submit_qp_cmds(ns_entry, &ns_entry->a_qp);
		if (rc) {
			goto err_exit;
		}
	}

	if (g_cmd_array == NULL || !g_run_admin_commands) {
		rc = submit_qp_cmds(ns_entry, &ns_entry->io_qp);
	}
err_exit:
	if (rc) {
		/*
		 * I see the prospect of having a broken qpair on one ns as interesting
		 * enough to recommend stopping the application.
		 */
		fprintf(stderr, "Unable to submit command with rc %d\n", rc);
		g_run = false;
	}
}

static void
free_namespaces(void)
{
	struct nvme_fuzz_ns *ns, *tmp;

	TAILQ_FOREACH_SAFE(ns, &g_ns_list, tailq, tmp) {
		printf("NS: %p I/O qp, Total commands completed: %" PRIu64 ", total successful commands: %" PRIu64
		       ", random_seed: %u\n",
		       ns->ns,
		       ns->io_qp.completed_cmd_counter, ns->io_qp.successful_completed_cmd_counter, ns->io_qp.random_seed);
		printf("NS: %p admin qp, Total commands completed: %" PRIu64 ", total successful commands: %" PRIu64
		       ", random_seed: %u\n",
		       ns->ns,
		       ns->a_qp.completed_cmd_counter, ns->a_qp.successful_completed_cmd_counter, ns->a_qp.random_seed);

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
	struct nvme_fuzz_ctrlr *ctrlr, *tmp;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	TAILQ_FOREACH_SAFE(ctrlr, &g_ctrlr_list, tailq, tmp) {
		TAILQ_REMOVE(&g_ctrlr_list, ctrlr, tailq);
		spdk_nvme_detach_async(ctrlr->ctrlr, &detach_ctx);
		free(ctrlr);
	}

	while (detach_ctx && spdk_nvme_detach_poll_async(detach_ctx) == -EAGAIN) {
		;
	}
}

static void
free_trids(void)
{
	struct nvme_fuzz_trid *trid, *tmp;

	TAILQ_FOREACH_SAFE(trid, &g_trid_list, tailq, tmp) {
		TAILQ_REMOVE(&g_trid_list, trid, tailq);
		free(trid);
	}
}

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns, uint32_t nsid)
{
	struct nvme_fuzz_ns *ns_entry;

	ns_entry = calloc(1, sizeof(struct nvme_fuzz_ns));
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
	struct nvme_fuzz_ctrlr *ctrlr_entry;
	uint32_t nsid;
	struct spdk_nvme_ns *ns;

	ctrlr_entry = calloc(1, sizeof(struct nvme_fuzz_ctrlr));
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
prep_qpair(struct nvme_fuzz_ns *ns, struct nvme_fuzz_qp *qp, uint32_t max_qdepth)
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

	qp->req_ctx = calloc(max_qdepth, sizeof(struct nvme_fuzz_request));
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
	struct nvme_fuzz_ns *ns_entry;

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
	struct nvme_fuzz_ns *ns_entry = ctx;

	ns_entry->req_poller = SPDK_POLLER_REGISTER(poll_for_completions, ns_entry, 0);
	submit_ns_cmds(ns_entry);
}

static int
check_app_completion(void *ctx)
{

	if (g_num_active_threads <= 0) {
		spdk_poller_unregister(&g_app_completion_poller);
		if (g_cmd_array) {
			free(g_cmd_array);
		}
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
	struct nvme_fuzz_ns *ns_entry;
	struct nvme_fuzz_trid *trid;
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

	g_app_completion_poller = SPDK_POLLER_REGISTER(check_app_completion, NULL, 1000000);
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
	struct nvme_fuzz_trid *current_trid;
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
			current_trid = malloc(sizeof(struct nvme_fuzz_trid));
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
	fprintf(stderr, " -a                        Perform admin commands. if -j is specified, \
only admin commands will run. Otherwise they will be run in tandem with I/O commands.\n");
	fprintf(stderr, " -C <path>                 Path to a configuration file.\n");
	fprintf(stderr,
		" -j <path>                 Path to a json file containing named objects of type spdk_nvme_cmd. If this option is specified, -t will be ignored.\n");
	fprintf(stderr, " -N                        Target only valid namespace with commands. \
This helps dig deeper into other errors besides invalid namespace.\n");
	fprintf(stderr, " -S <integer>              Seed value for test.\n");
	fprintf(stderr,
		" -t <integer>              Time in seconds to run the fuzz test. Only valid if -j is not specified.\n");
	fprintf(stderr, " -V                        Enable logging of each submitted command.\n");
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
	case 'j':
		g_json_file = optarg;
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
			g_seed_value = error_test;
		}
		break;
	case 't':
		g_runtime = spdk_strtol(optarg, 10);
		if (g_runtime < 0 || g_runtime > MAX_RUNTIME_S) {
			fprintf(stderr, "You must supply a positive runtime value less than 86401.\n");
			return -1;
		}
		break;
	case 'V':
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

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "aC:j:NS:t:V", NULL, nvme_fuzz_parse,
				      nvme_fuzz_usage) != SPDK_APP_PARSE_ARGS_SUCCESS)) {
		return rc;
	}

	if (g_conf_file) {
		parse_trids();
	}

	if (g_json_file != NULL) {
		g_cmd_array_size = fuzz_parse_args_into_array(g_json_file, (void **)&g_cmd_array,
				   sizeof(struct spdk_nvme_cmd), g_nvme_cmd_json_name, parse_nvme_cmd_obj);
		if (g_cmd_array_size == 0) {
			fprintf(stderr, "The provided json file did not contain any valid commands. Exiting.");
			return -EINVAL;
		}
	}

	rc = spdk_app_start(&opts, begin_fuzz, NULL);

	return rc;
}
