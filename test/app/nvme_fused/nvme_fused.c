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

#define IO_TIMEOUT_S 1

char *g_conf_file;
int g_app_rc = 0;
bool g_exit = false;
bool g_valid_ns_only = false;
bool g_verbose_mode = false;

struct nvme_fused_qp;

struct nvme_fused_ctx {
	int			index;
	struct nvme_fused_qp	*qp;
	bool			first_complete;
	bool			second_complete;
	struct			spdk_nvme_cpl cpl_first;
	struct			spdk_nvme_cpl cpl_second;
	struct			spdk_nvme_status status_first;
	struct			spdk_nvme_status status_second;
	uint8_t			*cmp_buf;
	uint8_t			*write_buf;
	int			rv;
	uint64_t		timeout_tsc;
	void	(*done)(struct nvme_fused_ctx *ctx);
	bool			is_done;
};

struct nvme_fused_trid {
	struct spdk_nvme_transport_id	trid;
	TAILQ_ENTRY(nvme_fused_trid)	tailq;
};

struct nvme_fused_ctrlr {
	struct spdk_nvme_ctrlr		*ctrlr;
	TAILQ_ENTRY(nvme_fused_ctrlr)	tailq;
};

struct nvme_fused_qp {
	struct spdk_nvme_qpair          *qpair1;
	struct spdk_nvme_qpair          *qpair2;
	int				req_pending;
	int				req_num;
	struct nvme_fused_ctx           ctx[1024];
};

struct nvme_fused_ns {
	struct spdk_nvme_ns		*ns;
	struct spdk_nvme_ctrlr		*ctrlr;
	struct spdk_thread		*thread;
	struct spdk_poller		*req_poller;
	uint32_t			nsid;
	struct nvme_fused_qp		qp;
	TAILQ_ENTRY(nvme_fused_ns)	tailq;
};

static TAILQ_HEAD(, nvme_fused_ns) g_ns_list = TAILQ_HEAD_INITIALIZER(g_ns_list);
static TAILQ_HEAD(, nvme_fused_ctrlr) g_ctrlr_list = TAILQ_HEAD_INITIALIZER(g_ctrlr_list);
static TAILQ_HEAD(, nvme_fused_trid) g_trid_list = TAILQ_HEAD_INITIALIZER(g_trid_list);

struct spdk_poller *g_app_completion_poller;
static int g_num_active_threads;
static int g_counter;
static int g_number_of_loops;

static void
nvme_fused_first_cpl_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_fused_ctx *ctx = cb_arg;

	if (ctx->first_complete) {
		SPDK_ERRLOG("fused first command received twice\n");
		ctx->rv = -1;
		ctx->done(ctx);
		return;
	}

	memcpy(&ctx->cpl_first, cpl, sizeof(struct spdk_nvme_cpl));
	ctx->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();

	ctx->first_complete = true;
	if (ctx->second_complete) {
		ctx->done(ctx);
	}
}

static void
nvme_fused_second_cpl_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_fused_ctx *ctx = cb_arg;

	if (ctx->second_complete == true) {
		SPDK_ERRLOG("fused second command received twice\n");
		ctx->rv = -1;
		ctx->done(ctx);
		return;
	}

	memcpy(&ctx->cpl_second, cpl, sizeof(struct spdk_nvme_cpl));
	ctx->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();

	ctx->second_complete = true;
	if (ctx->first_complete) {
		ctx->done(ctx);
	}
}

static bool
compare_status(struct spdk_nvme_status *status1, struct spdk_nvme_status *status2)
{

	return status1->sct == status2->sct && status1->sc == status2->sc;
}

static void
nvme_cpl_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_fused_ctx *ctx = cb_arg;

	memcpy(&ctx->cpl_first, cpl, sizeof(struct spdk_nvme_cpl));
	ctx->done(ctx);
}

static void
compare_and_write_done(struct nvme_fused_ctx *ctx)
{
	ctx->is_done = true;
	ctx->qp->req_pending--;

	if ((!compare_status(&ctx->cpl_first.status, &ctx->status_first) &&
	     ctx->status_first.sc != 0xFF && ctx->status_first.sct != 0x6) ||
	    (!compare_status(&ctx->cpl_second.status, &ctx->status_second) &&
	     ctx->status_second.sc != 0xFF && ctx->status_second.sct != 0x6)) {
		printf("Comapre and write request #%d (%d)\n", ctx->index, ctx->rv);
		printf("[First] Status: %s\n", spdk_nvme_cpl_get_status_string(&ctx->cpl_first.status));
		printf("[Second] Status: %s\n", spdk_nvme_cpl_get_status_string(&ctx->cpl_second.status));
		printf(" --> [First] Expected status: %s\n", spdk_nvme_cpl_get_status_string(&ctx->status_first));
		printf(" --> [Second] Expected status: %s\n", spdk_nvme_cpl_get_status_string(&ctx->status_second));

		g_app_rc = -1;
	}
}

static void
cmd_done(struct nvme_fused_ctx *ctx)
{
	ctx->is_done = true;
	ctx->qp->req_pending--;

	if (!compare_status(&ctx->cpl_first.status, &ctx->status_first)) {
		printf("Request #%d (%d)\n", ctx->index, ctx->rv);
		printf("[CMD] Status: %s\n", spdk_nvme_cpl_get_status_string(&ctx->cpl_first.status));
		printf(" --> [CMD] Expected status: %s\n", spdk_nvme_cpl_get_status_string(&ctx->status_first));
		g_app_rc = -1;
	}
}

static struct nvme_fused_ctx *
fused_ctx_get(struct nvme_fused_ns *ns_entry, struct nvme_fused_qp *qp)
{
	struct nvme_fused_ctx *ctx;

	qp->req_pending++;
	ctx = &qp->ctx[qp->req_num++];
	ctx->index = qp->req_num - 1;
	ctx->qp = qp;

	ctx->cmp_buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (ctx->cmp_buf == NULL) {
		SPDK_ERRLOG("Cannot allocate memory");
		exit(-1);
	}

	ctx->write_buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (ctx->write_buf == NULL) {
		SPDK_ERRLOG("Cannot allocate memory");
		exit(-1);
	}

	return ctx;
}

enum cmd_to_submit {
	fused_first_cmd,
	fused_second_cmd,
	fused_both_cmd,
	read_cmd,
	write_cmd,
	cmp_cmd,
};

static int
submit_cmd(struct nvme_fused_ns *ns_entry, struct spdk_nvme_qpair *qpair, char *cmp_buf,
	   char *write_buf, uint64_t lba, uint32_t lba_count, uint16_t first_exp_sc, uint16_t second_exp_sc,
	   enum cmd_to_submit cmd, struct nvme_fused_ctx **_ctx)
{
	struct nvme_fused_ctx *ctx;
	int rc;

	if (cmd != fused_second_cmd) {
		ctx = fused_ctx_get(ns_entry, &ns_entry->qp);
	}

	if (cmd == fused_first_cmd) {
		*_ctx = ctx;
		ctx->status_first.sc = first_exp_sc;
		ctx->status_first.sct = first_exp_sc == SPDK_NVME_SC_COMPARE_FAILURE ? SPDK_NVME_SCT_MEDIA_ERROR :
					SPDK_NVME_SCT_GENERIC;
		snprintf(ctx->cmp_buf, 0x1000, "%s\n", cmp_buf);
	} else if (cmd == fused_second_cmd) {
		assert(*_ctx != NULL);
		ctx = *_ctx;
		ctx->status_second.sc = second_exp_sc;
		ctx->status_second.sct = SPDK_NVME_SCT_GENERIC;
		snprintf(ctx->write_buf, 0x1000, "%s\n", write_buf);
	} else {
		ctx->status_first.sc = first_exp_sc;
		ctx->status_first.sct = first_exp_sc == SPDK_NVME_SC_COMPARE_FAILURE ? SPDK_NVME_SCT_MEDIA_ERROR :
					SPDK_NVME_SCT_GENERIC;
		ctx->status_second.sc = second_exp_sc;
		ctx->status_second.sct = SPDK_NVME_SCT_GENERIC;
		snprintf(ctx->cmp_buf, 0x1000, "%s\n", cmp_buf);
		snprintf(ctx->write_buf, 0x1000, "%s\n", write_buf);
	}

	if (cmd == read_cmd || cmd == write_cmd || cmd == cmp_cmd) {
		ctx->done = cmd_done;
	} else {
		ctx->done = compare_and_write_done;
	}

	ctx->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();

	switch (cmd) {
	case read_cmd:
		return spdk_nvme_ns_cmd_read(ns_entry->ns, qpair, ctx->cmp_buf, lba, lba_count, nvme_cpl_cb, ctx,
					     0);
	case  write_cmd:
		return spdk_nvme_ns_cmd_write(ns_entry->ns, qpair, ctx->write_buf, lba, lba_count, nvme_cpl_cb, ctx,
					      0);
	case  cmp_cmd:
		return spdk_nvme_ns_cmd_compare(ns_entry->ns, qpair, ctx->cmp_buf, lba, lba_count, nvme_cpl_cb, ctx,
						0);
	case fused_first_cmd:
		return spdk_nvme_ns_cmd_compare(ns_entry->ns, qpair, ctx->cmp_buf, lba, lba_count,
						nvme_fused_first_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_FIRST);
	case fused_second_cmd:
		return spdk_nvme_ns_cmd_write(ns_entry->ns, qpair, ctx->write_buf, lba, lba_count,
					      nvme_fused_second_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_SECOND);
	case fused_both_cmd:
		rc = spdk_nvme_ns_cmd_compare(ns_entry->ns, qpair, ctx->cmp_buf, lba, lba_count,
					      nvme_fused_first_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_FIRST);
		if (rc) {
			return rc;
		}
		return spdk_nvme_ns_cmd_write(ns_entry->ns, qpair, ctx->write_buf, lba, lba_count,
					      nvme_fused_second_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_SECOND);
	default:
		assert(false);
	}
}

static int
compare_and_write(void *arg)
{
	int rc;
	struct nvme_fused_ns *ns_entry = (struct nvme_fused_ns *)arg;
	struct nvme_fused_ctx *ctx;

	memset(&ns_entry->qp.ctx, 0, sizeof(ns_entry->qp.ctx));

	/* Prepare device buffer */
	rc = submit_cmd(ns_entry, ns_entry->qp.qpair1, NULL, "Starting buffer", 0, 1, SPDK_NVME_SC_SUCCESS,
			0, write_cmd, NULL);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}


	/*
	 * Case 1: Successful fused command
	 */

	rc = submit_cmd(ns_entry, ns_entry->qp.qpair1, "Starting buffer", "Hello world!", 0, 1,
			SPDK_NVME_SC_SUCCESS, SPDK_NVME_SC_SUCCESS, fused_both_cmd, NULL);
	if (rc != 0) {
		fprintf(stderr, "starting I/O failed\n");
		exit(1);
	}


	/*
	 * Case 2: Reading pending fused op
	 */

	/* Prepare device buffer */
	rc = submit_cmd(ns_entry, ns_entry->qp.qpair2, NULL, "Starting buffer", 1, 1, SPDK_NVME_SC_SUCCESS,
			0, write_cmd, NULL);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/* First fused compare and write operation */
	rc = submit_cmd(ns_entry, ns_entry->qp.qpair2, "Starting buffer", NULL, 1, 1, SPDK_NVME_SC_SUCCESS,
			0, fused_first_cmd, &ctx);
	if (rc != 0) {
		fprintf(stderr, "starting compare I/O failed\n");
		exit(1);
	}

	/* Read operation */
	rc = submit_cmd(ns_entry, ns_entry->qp.qpair1, NULL, NULL, 1, 1, SPDK_NVME_SC_SUCCESS, 0, read_cmd,
			NULL);
	if (rc != 0) {
		fprintf(stderr, "starting read I/O failed\n");
		exit(1);
	}

	/* Second part of first fused command */
	rc = submit_cmd(ns_entry, ns_entry->qp.qpair2, NULL, "2 fused commands", 1, 1, 0,
			SPDK_NVME_SC_SUCCESS, fused_second_cmd, &ctx);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}


	/*
	 * Case 3: Fused compare didn't match
	 */

	rc = submit_cmd(ns_entry, ns_entry->qp.qpair2, "Wrong buffer", NULL, 2, 1,
			SPDK_NVME_SC_COMPARE_FAILURE, SPDK_NVME_SC_ABORTED_FAILED_FUSED, fused_both_cmd, NULL);
	if (rc != 0) {
		fprintf(stderr, "starting I/O failed\n");
		exit(1);
	}


	/*
	 * Case 4: Fused commands not in sequence
	 */

	/* Fused compare and write operation - first part */
	rc = submit_cmd(ns_entry, ns_entry->qp.qpair1, NULL, NULL, 3, 1, SPDK_NVME_SC_ABORTED_MISSING_FUSED,
			0, fused_first_cmd, &ctx);
	if (rc != 0) {
		fprintf(stderr, "starting compare I/O failed\n");
		exit(1);
	}

	/* Not fused write op */
	rc = submit_cmd(ns_entry, ns_entry->qp.qpair1, NULL, "Not fused", 3, 1, SPDK_NVME_SC_SUCCESS, 0,
			write_cmd, NULL);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/* Fused compare and write operation - second part */
	rc = submit_cmd(ns_entry, ns_entry->qp.qpair1, NULL, "Fused", 3, 1, 0,
			SPDK_NVME_SC_ABORTED_MISSING_FUSED, fused_second_cmd, &ctx);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/* Not fused compare op */
	rc = submit_cmd(ns_entry, ns_entry->qp.qpair1, "Not fused", NULL, 3, 1, SPDK_NVME_SC_SUCCESS, 0,
			cmp_cmd, NULL);
	if (rc != 0) {
		fprintf(stderr, "starting compare I/O failed\n");
		exit(1);
	}


	/*
	 * Case 5: Regions do not match
	 */

	/* Fused compare and write operation - first part (lba = 4) */
	rc = submit_cmd(ns_entry, ns_entry->qp.qpair1, NULL, NULL, 4, 1, SPDK_NVME_SC_INVALID_FIELD, 0,
			fused_first_cmd, &ctx);
	if (rc != 0) {
		fprintf(stderr, "starting compare I/O failed\n");
		exit(1);
	}

	/* Fused compare and write operation - second part (lba = 5) */
	rc = submit_cmd(ns_entry, ns_entry->qp.qpair1, NULL, NULL, 5, 1, 0,
			SPDK_NVME_SC_ABORTED_FAILED_FUSED, fused_second_cmd, &ctx);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	return 0;
}

static void
cleanup_queue(struct nvme_fused_qp *qp)
{
	int i;
	for (i = 0; i < qp->req_num; i++) {
		spdk_free(qp->ctx[i].cmp_buf);
		spdk_free(qp->ctx[i].write_buf);
	}
	qp->req_num = 0;
}

static int
poll_for_completions(void *arg)
{
	struct nvme_fused_ns *ns_entry = arg;
	int32_t rv;
	uint64_t current_ticks;
	int i;

	if (ns_entry->qp.req_pending == 0) {
		g_counter--;
		if (g_counter <= 0) {
			goto exit_handler;
		}

		cleanup_queue(&ns_entry->qp);
		compare_and_write(arg);
	}

	rv = spdk_nvme_qpair_process_completions(ns_entry->qp.qpair1, 0);
	if (rv < 0) {
		goto exit_handler;
	}

	rv = spdk_nvme_qpair_process_completions(ns_entry->qp.qpair2, 0);
	if (rv < 0) {
		goto exit_handler;
	}

	rv = spdk_nvme_ctrlr_process_admin_completions(ns_entry->ctrlr);
	if (rv < 0) {
		goto exit_handler;
	}

	current_ticks = spdk_get_ticks();

	for (i = 0; i < ns_entry->qp.req_num; i++) {
		if (ns_entry->qp.ctx[i].is_done) {
			continue;
		}
		if (ns_entry->qp.ctx[i].rv < 0) {
			goto exit_handler;
		}
		if (ns_entry->qp.ctx[i].timeout_tsc < current_ticks) {
			SPDK_NOTICELOG("Request #%d IO Timeout\n", ns_entry->qp.ctx[i].index);
			goto exit_handler;
		}
	}
	return 0;

exit_handler:

	SPDK_NOTICELOG("Finishing queue IO poller\n");
	spdk_poller_unregister(&ns_entry->req_poller);
	__sync_sub_and_fetch(&g_num_active_threads, 1);
	spdk_thread_exit(ns_entry->thread);
	return 0;
}

static void
free_namespaces(void)
{
	struct nvme_fused_ns *ns, *tmp;

	TAILQ_FOREACH_SAFE(ns, &g_ns_list, tailq, tmp) {

		cleanup_queue(&ns->qp);

		if (ns->qp.qpair1) {
			spdk_nvme_ctrlr_free_io_qpair(ns->qp.qpair1);
		}
		if (ns->qp.qpair2) {
			spdk_nvme_ctrlr_free_io_qpair(ns->qp.qpair2);
		}

		TAILQ_REMOVE(&g_ns_list, ns, tailq);
		free(ns);
	}
}

static void
free_controllers(void)
{
	struct nvme_fused_ctrlr *ctrlr, *tmp;

	TAILQ_FOREACH_SAFE(ctrlr, &g_ctrlr_list, tailq, tmp) {
		TAILQ_REMOVE(&g_ctrlr_list, ctrlr, tailq);
		spdk_nvme_detach(ctrlr->ctrlr);
		free(ctrlr);
	}
}

static void
free_trids(void)
{
	struct nvme_fused_trid *trid, *tmp;

	TAILQ_FOREACH_SAFE(trid, &g_trid_list, tailq, tmp) {
		TAILQ_REMOVE(&g_trid_list, trid, tailq);
		free(trid);
	}
}

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns, uint32_t nsid)
{
	struct nvme_fused_ns *ns_entry;

	ns_entry = calloc(1, sizeof(struct nvme_fused_ns));
	if (ns_entry == NULL) {
		fprintf(stderr, "Unable to allocate an entry for a namespace\n");
		return;
	}

	ns_entry->ns = ns;
	ns_entry->ctrlr = ctrlr;
	ns_entry->nsid = nsid;

	TAILQ_INSERT_TAIL(&g_ns_list, ns_entry, tailq);
}

static void
register_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_fused_ctrlr *ctrlr_entry;
	uint32_t nsid;
	struct spdk_nvme_ns *ns;

	ctrlr_entry = calloc(1, sizeof(struct nvme_fused_ctrlr));
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
	printf("Controller trtype %s\ttraddr %s\ttrsvcid %s\n",
	       spdk_nvme_transport_id_trtype_str(trid->trtype),
	       trid->traddr, trid->trsvcid);

	return true;
}

static int
prepare_qpairs(void)
{
	struct spdk_nvme_io_qpair_opts opts;
	struct nvme_fused_ns *ns_entry;

	TAILQ_FOREACH(ns_entry, &g_ns_list, tailq) {
		spdk_nvme_ctrlr_get_default_io_qpair_opts(ns_entry->ctrlr, &opts, sizeof(opts));
		ns_entry->qp.qpair1 = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, &opts, sizeof(opts));
		ns_entry->qp.qpair2 = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, &opts, sizeof(opts));
		if (ns_entry->qp.qpair1 == NULL || ns_entry->qp.qpair2 == NULL) {
			fprintf(stderr, "Unable to create a qpair for a namespace\n");
			return -1;
		}

	}
	return 0;
}

static void
start_ns_poller(void *ctx)
{
	struct nvme_fused_ns *ns_entry = ctx;

	ns_entry->req_poller = spdk_poller_register(poll_for_completions, ns_entry, 0);
}

static int
check_app_completion(void *ctx)
{
	if (g_num_active_threads <= 0) {
		spdk_poller_unregister(&g_app_completion_poller);
		printf("End of test\n");
		free_namespaces();
		free_controllers();
		free_trids();
		spdk_app_stop(g_app_rc);
	}
	return 1;
}

static void
begin_fused(void *ctx)
{
	struct nvme_fused_ns *ns_entry;
	struct nvme_fused_trid *trid;
	int rc;

	TAILQ_FOREACH(trid, &g_trid_list, tailq) {
		if (spdk_nvme_probe(&trid->trid, trid, probe_cb, attach_cb, NULL) != 0) {
			fprintf(stderr, "spdk_nvme_probe() failed for transport address '%s'\n",
				trid->trid.traddr);
			rc = -1;
			goto out;
		}
	}

	if (TAILQ_EMPTY(&g_ns_list)) {
		fprintf(stderr, "No valid NVMe Namespaces to fused\n");
		rc = -EINVAL;
		goto out;
	}

	rc = prepare_qpairs();

	if (rc < 0) {
		fprintf(stderr, "Unable to prepare the qpairs\n");
		goto out;
	}

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

	g_app_completion_poller = spdk_poller_register(check_app_completion, NULL, 1000000);

	return;
out:
	printf("Shutting down the fused application\n");
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
	struct nvme_fused_trid *current_trid;
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
			current_trid = malloc(sizeof(struct nvme_fused_trid));
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
nvme_fused_usage(void)
{
	fprintf(stderr, " -C <path>                 Path to a configuration file.\n");
	fprintf(stderr, " -N                        Target only valid namespace with commands. \
This helps dig deeper into other errors besides invalid namespace.\n");
	fprintf(stderr, " -V                        Enable logging of each submitted command.\n");
}

static int
nvme_fused_parse(int ch, char *arg)
{
	switch (ch) {
	case 'C':
		g_conf_file = optarg;
		break;
	case 'N':
		g_valid_ns_only = true;
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

	g_counter = 1000;
	g_number_of_loops = g_counter;

	spdk_app_opts_init(&opts);
	opts.name = "nvme_fused";

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "C:NV", NULL, nvme_fused_parse,
				      nvme_fused_usage) != SPDK_APP_PARSE_ARGS_SUCCESS)) {
		return rc;
	}

	if (g_conf_file) {
		parse_trids();
	}

	rc = spdk_app_start(&opts, begin_fused, NULL);

	return rc;
}
