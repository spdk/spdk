/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019-2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#include "nvme_internal.h"
#include "nvme_io_msg.h"

#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/endian.h"

struct nvme_active_ns_ctx;

static int nvme_ctrlr_construct_and_submit_aer(struct spdk_nvme_ctrlr *ctrlr,
		struct nvme_async_event_request *aer);
static void nvme_ctrlr_identify_active_ns_async(struct nvme_active_ns_ctx *ctx);
static int nvme_ctrlr_identify_ns_async(struct spdk_nvme_ns *ns);
static int nvme_ctrlr_identify_ns_iocs_specific_async(struct spdk_nvme_ns *ns);
static int nvme_ctrlr_identify_id_desc_async(struct spdk_nvme_ns *ns);
static void nvme_ctrlr_init_cap(struct spdk_nvme_ctrlr *ctrlr);
static void nvme_ctrlr_set_state(struct spdk_nvme_ctrlr *ctrlr, enum nvme_ctrlr_state state,
				 uint64_t timeout_in_ms);

static int
nvme_ns_cmp(struct spdk_nvme_ns *ns1, struct spdk_nvme_ns *ns2)
{
	if (ns1->id < ns2->id) {
		return -1;
	} else if (ns1->id > ns2->id) {
		return 1;
	} else {
		return 0;
	}
}

RB_GENERATE_STATIC(nvme_ns_tree, spdk_nvme_ns, node, nvme_ns_cmp);

#define CTRLR_STRING(ctrlr) \
	((ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_TCP || ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_RDMA) ? \
	ctrlr->trid.subnqn : ctrlr->trid.traddr)

#define NVME_CTRLR_ERRLOG(ctrlr, format, ...) \
	SPDK_ERRLOG("[%s] " format, CTRLR_STRING(ctrlr), ##__VA_ARGS__);

#define NVME_CTRLR_WARNLOG(ctrlr, format, ...) \
	SPDK_WARNLOG("[%s] " format, CTRLR_STRING(ctrlr), ##__VA_ARGS__);

#define NVME_CTRLR_NOTICELOG(ctrlr, format, ...) \
	SPDK_NOTICELOG("[%s] " format, CTRLR_STRING(ctrlr), ##__VA_ARGS__);

#define NVME_CTRLR_INFOLOG(ctrlr, format, ...) \
	SPDK_INFOLOG(nvme, "[%s] " format, CTRLR_STRING(ctrlr), ##__VA_ARGS__);

#ifdef DEBUG
#define NVME_CTRLR_DEBUGLOG(ctrlr, format, ...) \
	SPDK_DEBUGLOG(nvme, "[%s] " format, CTRLR_STRING(ctrlr), ##__VA_ARGS__);
#else
#define NVME_CTRLR_DEBUGLOG(ctrlr, ...) do { } while (0)
#endif

#define nvme_ctrlr_get_reg_async(ctrlr, reg, sz, cb_fn, cb_arg) \
	nvme_transport_ctrlr_get_reg_ ## sz ## _async(ctrlr, \
		offsetof(struct spdk_nvme_registers, reg), cb_fn, cb_arg)

#define nvme_ctrlr_set_reg_async(ctrlr, reg, sz, val, cb_fn, cb_arg) \
	nvme_transport_ctrlr_set_reg_ ## sz ## _async(ctrlr, \
		offsetof(struct spdk_nvme_registers, reg), val, cb_fn, cb_arg)

#define nvme_ctrlr_get_cc_async(ctrlr, cb_fn, cb_arg) \
	nvme_ctrlr_get_reg_async(ctrlr, cc, 4, cb_fn, cb_arg)

#define nvme_ctrlr_get_csts_async(ctrlr, cb_fn, cb_arg) \
	nvme_ctrlr_get_reg_async(ctrlr, csts, 4, cb_fn, cb_arg)

#define nvme_ctrlr_get_cap_async(ctrlr, cb_fn, cb_arg) \
	nvme_ctrlr_get_reg_async(ctrlr, cap, 8, cb_fn, cb_arg)

#define nvme_ctrlr_get_vs_async(ctrlr, cb_fn, cb_arg) \
	nvme_ctrlr_get_reg_async(ctrlr, vs, 4, cb_fn, cb_arg)

#define nvme_ctrlr_set_cc_async(ctrlr, value, cb_fn, cb_arg) \
	nvme_ctrlr_set_reg_async(ctrlr, cc, 4, value, cb_fn, cb_arg)

static int
nvme_ctrlr_get_cc(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_cc_register *cc)
{
	return nvme_transport_ctrlr_get_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, cc.raw),
					      &cc->raw);
}

static int
nvme_ctrlr_get_csts(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_csts_register *csts)
{
	return nvme_transport_ctrlr_get_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, csts.raw),
					      &csts->raw);
}

int
nvme_ctrlr_get_cap(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_cap_register *cap)
{
	return nvme_transport_ctrlr_get_reg_8(ctrlr, offsetof(struct spdk_nvme_registers, cap.raw),
					      &cap->raw);
}

int
nvme_ctrlr_get_vs(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_vs_register *vs)
{
	return nvme_transport_ctrlr_get_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, vs.raw),
					      &vs->raw);
}

int
nvme_ctrlr_get_cmbsz(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_cmbsz_register *cmbsz)
{
	return nvme_transport_ctrlr_get_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, cmbsz.raw),
					      &cmbsz->raw);
}

int
nvme_ctrlr_get_pmrcap(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_pmrcap_register *pmrcap)
{
	return nvme_transport_ctrlr_get_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, pmrcap.raw),
					      &pmrcap->raw);
}

int
nvme_ctrlr_get_bpinfo(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_bpinfo_register *bpinfo)
{
	return nvme_transport_ctrlr_get_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, bpinfo.raw),
					      &bpinfo->raw);
}

int
nvme_ctrlr_set_bprsel(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_bprsel_register *bprsel)
{
	return nvme_transport_ctrlr_set_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, bprsel.raw),
					      bprsel->raw);
}

int
nvme_ctrlr_set_bpmbl(struct spdk_nvme_ctrlr *ctrlr, uint64_t bpmbl_value)
{
	return nvme_transport_ctrlr_set_reg_8(ctrlr, offsetof(struct spdk_nvme_registers, bpmbl),
					      bpmbl_value);
}

static int
nvme_ctrlr_set_nssr(struct spdk_nvme_ctrlr *ctrlr, uint32_t nssr_value)
{
	return nvme_transport_ctrlr_set_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, nssr),
					      nssr_value);
}

bool
nvme_ctrlr_multi_iocs_enabled(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->cap.bits.css & SPDK_NVME_CAP_CSS_IOCS &&
	       ctrlr->opts.command_set == SPDK_NVME_CC_CSS_IOCS;
}

/* When the field in spdk_nvme_ctrlr_opts are changed and you change this function, please
 * also update the nvme_ctrl_opts_init function in nvme_ctrlr.c
 */
void
spdk_nvme_ctrlr_get_default_ctrlr_opts(struct spdk_nvme_ctrlr_opts *opts, size_t opts_size)
{
	char host_id_str[SPDK_UUID_STRING_LEN];

	assert(opts);

	opts->opts_size = opts_size;

#define FIELD_OK(field) \
	offsetof(struct spdk_nvme_ctrlr_opts, field) + sizeof(opts->field) <= opts_size

#define SET_FIELD(field, value) \
	if (offsetof(struct spdk_nvme_ctrlr_opts, field) + sizeof(opts->field) <= opts_size) { \
		opts->field = value; \
	} \

	SET_FIELD(num_io_queues, DEFAULT_MAX_IO_QUEUES);
	SET_FIELD(use_cmb_sqs, false);
	SET_FIELD(no_shn_notification, false);
	SET_FIELD(arb_mechanism, SPDK_NVME_CC_AMS_RR);
	SET_FIELD(arbitration_burst, 0);
	SET_FIELD(low_priority_weight, 0);
	SET_FIELD(medium_priority_weight, 0);
	SET_FIELD(high_priority_weight, 0);
	SET_FIELD(keep_alive_timeout_ms, MIN_KEEP_ALIVE_TIMEOUT_IN_MS);
	SET_FIELD(transport_retry_count, SPDK_NVME_DEFAULT_RETRY_COUNT);
	SET_FIELD(io_queue_size, DEFAULT_IO_QUEUE_SIZE);

	if (nvme_driver_init() == 0) {
		if (FIELD_OK(hostnqn)) {
			spdk_uuid_fmt_lower(host_id_str, sizeof(host_id_str),
					    &g_spdk_nvme_driver->default_extended_host_id);
			snprintf(opts->hostnqn, sizeof(opts->hostnqn),
				 "nqn.2014-08.org.nvmexpress:uuid:%s", host_id_str);
		}

		if (FIELD_OK(extended_host_id)) {
			memcpy(opts->extended_host_id, &g_spdk_nvme_driver->default_extended_host_id,
			       sizeof(opts->extended_host_id));
		}

	}

	SET_FIELD(io_queue_requests, DEFAULT_IO_QUEUE_REQUESTS);

	if (FIELD_OK(src_addr)) {
		memset(opts->src_addr, 0, sizeof(opts->src_addr));
	}

	if (FIELD_OK(src_svcid)) {
		memset(opts->src_svcid, 0, sizeof(opts->src_svcid));
	}

	if (FIELD_OK(host_id)) {
		memset(opts->host_id, 0, sizeof(opts->host_id));
	}

	SET_FIELD(command_set, CHAR_BIT);
	SET_FIELD(admin_timeout_ms, NVME_MAX_ADMIN_TIMEOUT_IN_SECS * 1000);
	SET_FIELD(header_digest, false);
	SET_FIELD(data_digest, false);
	SET_FIELD(disable_error_logging, false);
	SET_FIELD(transport_ack_timeout, SPDK_NVME_DEFAULT_TRANSPORT_ACK_TIMEOUT);
	SET_FIELD(admin_queue_size, DEFAULT_ADMIN_QUEUE_SIZE);
	SET_FIELD(fabrics_connect_timeout_us, NVME_FABRIC_CONNECT_COMMAND_TIMEOUT);
	SET_FIELD(disable_read_ana_log_page, false);

#undef FIELD_OK
#undef SET_FIELD
}

const struct spdk_nvme_ctrlr_opts *
spdk_nvme_ctrlr_get_opts(struct spdk_nvme_ctrlr *ctrlr)
{
	return &ctrlr->opts;
}

/**
 * This function will be called when the process allocates the IO qpair.
 * Note: the ctrlr_lock must be held when calling this function.
 */
static void
nvme_ctrlr_proc_add_io_qpair(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr_process	*active_proc;
	struct spdk_nvme_ctrlr		*ctrlr = qpair->ctrlr;

	active_proc = nvme_ctrlr_get_current_process(ctrlr);
	if (active_proc) {
		TAILQ_INSERT_TAIL(&active_proc->allocated_io_qpairs, qpair, per_process_tailq);
		qpair->active_proc = active_proc;
	}
}

/**
 * This function will be called when the process frees the IO qpair.
 * Note: the ctrlr_lock must be held when calling this function.
 */
static void
nvme_ctrlr_proc_remove_io_qpair(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr_process	*active_proc;
	struct spdk_nvme_ctrlr		*ctrlr = qpair->ctrlr;
	struct spdk_nvme_qpair          *active_qpair, *tmp_qpair;

	active_proc = nvme_ctrlr_get_current_process(ctrlr);
	if (!active_proc) {
		return;
	}

	TAILQ_FOREACH_SAFE(active_qpair, &active_proc->allocated_io_qpairs,
			   per_process_tailq, tmp_qpair) {
		if (active_qpair == qpair) {
			TAILQ_REMOVE(&active_proc->allocated_io_qpairs,
				     active_qpair, per_process_tailq);

			break;
		}
	}
}

void
spdk_nvme_ctrlr_get_default_io_qpair_opts(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_io_qpair_opts *opts,
		size_t opts_size)
{
	assert(ctrlr);

	assert(opts);

	memset(opts, 0, opts_size);

#define FIELD_OK(field) \
	offsetof(struct spdk_nvme_io_qpair_opts, field) + sizeof(opts->field) <= opts_size

	if (FIELD_OK(qprio)) {
		opts->qprio = SPDK_NVME_QPRIO_URGENT;
	}

	if (FIELD_OK(io_queue_size)) {
		opts->io_queue_size = ctrlr->opts.io_queue_size;
	}

	if (FIELD_OK(io_queue_requests)) {
		opts->io_queue_requests = ctrlr->opts.io_queue_requests;
	}

	if (FIELD_OK(delay_cmd_submit)) {
		opts->delay_cmd_submit = false;
	}

	if (FIELD_OK(sq.vaddr)) {
		opts->sq.vaddr = NULL;
	}

	if (FIELD_OK(sq.paddr)) {
		opts->sq.paddr = 0;
	}

	if (FIELD_OK(sq.buffer_size)) {
		opts->sq.buffer_size = 0;
	}

	if (FIELD_OK(cq.vaddr)) {
		opts->cq.vaddr = NULL;
	}

	if (FIELD_OK(cq.paddr)) {
		opts->cq.paddr = 0;
	}

	if (FIELD_OK(cq.buffer_size)) {
		opts->cq.buffer_size = 0;
	}

	if (FIELD_OK(create_only)) {
		opts->create_only = false;
	}

	if (FIELD_OK(async_mode)) {
		opts->async_mode = false;
	}

#undef FIELD_OK
}

static struct spdk_nvme_qpair *
nvme_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
			   const struct spdk_nvme_io_qpair_opts *opts)
{
	int32_t					qid;
	struct spdk_nvme_qpair			*qpair;
	union spdk_nvme_cc_register		cc;

	if (!ctrlr) {
		return NULL;
	}

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	cc.raw = ctrlr->process_init_cc.raw;

	if (opts->qprio & ~SPDK_NVME_CREATE_IO_SQ_QPRIO_MASK) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return NULL;
	}

	/*
	 * Only value SPDK_NVME_QPRIO_URGENT(0) is valid for the
	 * default round robin arbitration method.
	 */
	if ((cc.bits.ams == SPDK_NVME_CC_AMS_RR) && (opts->qprio != SPDK_NVME_QPRIO_URGENT)) {
		NVME_CTRLR_ERRLOG(ctrlr, "invalid queue priority for default round robin arbitration method\n");
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return NULL;
	}

	qid = spdk_nvme_ctrlr_alloc_qid(ctrlr);
	if (qid < 0) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return NULL;
	}

	qpair = nvme_transport_ctrlr_create_io_qpair(ctrlr, qid, opts);
	if (qpair == NULL) {
		NVME_CTRLR_ERRLOG(ctrlr, "nvme_transport_ctrlr_create_io_qpair() failed\n");
		spdk_nvme_ctrlr_free_qid(ctrlr, qid);
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return NULL;
	}

	TAILQ_INSERT_TAIL(&ctrlr->active_io_qpairs, qpair, tailq);

	nvme_ctrlr_proc_add_io_qpair(qpair);

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return qpair;
}

int
spdk_nvme_ctrlr_connect_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	int rc;

	if (nvme_qpair_get_state(qpair) != NVME_QPAIR_DISCONNECTED) {
		return -EISCONN;
	}

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	rc = nvme_transport_ctrlr_connect_qpair(ctrlr, qpair);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	if (ctrlr->quirks & NVME_QUIRK_DELAY_AFTER_QUEUE_ALLOC) {
		spdk_delay_us(100);
	}

	return rc;
}

void
spdk_nvme_ctrlr_disconnect_io_qpair(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	nvme_transport_ctrlr_disconnect_qpair(ctrlr, qpair);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
}

struct spdk_nvme_qpair *
spdk_nvme_ctrlr_alloc_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
			       const struct spdk_nvme_io_qpair_opts *user_opts,
			       size_t opts_size)
{

	struct spdk_nvme_qpair		*qpair;
	struct spdk_nvme_io_qpair_opts	opts;
	int				rc;

	if (spdk_unlikely(ctrlr->state != NVME_CTRLR_STATE_READY)) {
		/* When controller is resetting or initializing, free_io_qids is deleted or not created yet.
		 * We can't create IO qpair in that case */
		return NULL;
	}

	/*
	 * Get the default options, then overwrite them with the user-provided options
	 * up to opts_size.
	 *
	 * This allows for extensions of the opts structure without breaking
	 * ABI compatibility.
	 */
	spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
	if (user_opts) {
		memcpy(&opts, user_opts, spdk_min(sizeof(opts), opts_size));

		/* If user passes buffers, make sure they're big enough for the requested queue size */
		if (opts.sq.vaddr) {
			if (opts.sq.buffer_size < (opts.io_queue_size * sizeof(struct spdk_nvme_cmd))) {
				NVME_CTRLR_ERRLOG(ctrlr, "sq buffer size %" PRIx64 " is too small for sq size %zx\n",
						  opts.sq.buffer_size, (opts.io_queue_size * sizeof(struct spdk_nvme_cmd)));
				return NULL;
			}
		}
		if (opts.cq.vaddr) {
			if (opts.cq.buffer_size < (opts.io_queue_size * sizeof(struct spdk_nvme_cpl))) {
				NVME_CTRLR_ERRLOG(ctrlr, "cq buffer size %" PRIx64 " is too small for cq size %zx\n",
						  opts.cq.buffer_size, (opts.io_queue_size * sizeof(struct spdk_nvme_cpl)));
				return NULL;
			}
		}
	}

	qpair = nvme_ctrlr_create_io_qpair(ctrlr, &opts);

	if (qpair == NULL || opts.create_only == true) {
		return qpair;
	}

	rc = spdk_nvme_ctrlr_connect_io_qpair(ctrlr, qpair);
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "nvme_transport_ctrlr_connect_io_qpair() failed\n");
		nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
		nvme_ctrlr_proc_remove_io_qpair(qpair);
		TAILQ_REMOVE(&ctrlr->active_io_qpairs, qpair, tailq);
		spdk_bit_array_set(ctrlr->free_io_qids, qpair->id);
		nvme_transport_ctrlr_delete_io_qpair(ctrlr, qpair);
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return NULL;
	}

	return qpair;
}

int
spdk_nvme_ctrlr_reconnect_io_qpair(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr *ctrlr;
	enum nvme_qpair_state qpair_state;
	int rc;

	assert(qpair != NULL);
	assert(nvme_qpair_is_admin_queue(qpair) == false);
	assert(qpair->ctrlr != NULL);

	ctrlr = qpair->ctrlr;
	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	qpair_state = nvme_qpair_get_state(qpair);

	if (ctrlr->is_removed) {
		rc = -ENODEV;
		goto out;
	}

	if (ctrlr->is_resetting || qpair_state == NVME_QPAIR_DISCONNECTING) {
		rc = -EAGAIN;
		goto out;
	}

	if (ctrlr->is_failed || qpair_state == NVME_QPAIR_DESTROYING) {
		rc = -ENXIO;
		goto out;
	}

	if (qpair_state != NVME_QPAIR_DISCONNECTED) {
		rc = 0;
		goto out;
	}

	rc = nvme_transport_ctrlr_connect_qpair(ctrlr, qpair);
	if (rc) {
		rc = -EAGAIN;
		goto out;
	}

out:
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	return rc;
}

spdk_nvme_qp_failure_reason
spdk_nvme_ctrlr_get_admin_qp_failure_reason(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->adminq->transport_failure_reason;
}

/*
 * This internal function will attempt to take the controller
 * lock before calling disconnect on a controller qpair.
 * Functions already holding the controller lock should
 * call nvme_transport_ctrlr_disconnect_qpair directly.
 */
void
nvme_ctrlr_disconnect_qpair(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;

	assert(ctrlr != NULL);
	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	nvme_transport_ctrlr_disconnect_qpair(ctrlr, qpair);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
}

int
spdk_nvme_ctrlr_free_io_qpair(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr *ctrlr;

	if (qpair == NULL) {
		return 0;
	}

	ctrlr = qpair->ctrlr;

	if (qpair->in_completion_context) {
		/*
		 * There are many cases where it is convenient to delete an io qpair in the context
		 *  of that qpair's completion routine.  To handle this properly, set a flag here
		 *  so that the completion routine will perform an actual delete after the context
		 *  unwinds.
		 */
		qpair->delete_after_completion_context = 1;
		return 0;
	}

	if (qpair->poll_group && qpair->poll_group->in_completion_context) {
		/* Same as above, but in a poll group. */
		qpair->poll_group->num_qpairs_to_delete++;
		qpair->delete_after_completion_context = 1;
		return 0;
	}

	nvme_transport_ctrlr_disconnect_qpair(ctrlr, qpair);

	if (qpair->poll_group) {
		spdk_nvme_poll_group_remove(qpair->poll_group->group, qpair);
	}

	/* Do not retry. */
	nvme_qpair_set_state(qpair, NVME_QPAIR_DESTROYING);

	/* In the multi-process case, a process may call this function on a foreign
	 * I/O qpair (i.e. one that this process did not create) when that qpairs process
	 * exits unexpectedly.  In that case, we must not try to abort any reqs associated
	 * with that qpair, since the callbacks will also be foreign to this process.
	 */
	if (qpair->active_proc == nvme_ctrlr_get_current_process(ctrlr)) {
		nvme_qpair_abort_all_queued_reqs(qpair, 0);
	}

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);

	nvme_ctrlr_proc_remove_io_qpair(qpair);

	TAILQ_REMOVE(&ctrlr->active_io_qpairs, qpair, tailq);
	spdk_nvme_ctrlr_free_qid(ctrlr, qpair->id);

	nvme_transport_ctrlr_delete_io_qpair(ctrlr, qpair);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	return 0;
}

static void
nvme_ctrlr_construct_intel_support_log_page_list(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_intel_log_page_directory *log_page_directory)
{
	if (log_page_directory == NULL) {
		return;
	}

	if (ctrlr->cdata.vid != SPDK_PCI_VID_INTEL) {
		return;
	}

	ctrlr->log_page_supported[SPDK_NVME_INTEL_LOG_PAGE_DIRECTORY] = true;

	if (log_page_directory->read_latency_log_len ||
	    (ctrlr->quirks & NVME_INTEL_QUIRK_READ_LATENCY)) {
		ctrlr->log_page_supported[SPDK_NVME_INTEL_LOG_READ_CMD_LATENCY] = true;
	}
	if (log_page_directory->write_latency_log_len ||
	    (ctrlr->quirks & NVME_INTEL_QUIRK_WRITE_LATENCY)) {
		ctrlr->log_page_supported[SPDK_NVME_INTEL_LOG_WRITE_CMD_LATENCY] = true;
	}
	if (log_page_directory->temperature_statistics_log_len) {
		ctrlr->log_page_supported[SPDK_NVME_INTEL_LOG_TEMPERATURE] = true;
	}
	if (log_page_directory->smart_log_len) {
		ctrlr->log_page_supported[SPDK_NVME_INTEL_LOG_SMART] = true;
	}
	if (log_page_directory->marketing_description_log_len) {
		ctrlr->log_page_supported[SPDK_NVME_INTEL_MARKETING_DESCRIPTION] = true;
	}
}

struct intel_log_pages_ctx {
	struct spdk_nvme_intel_log_page_directory log_page_directory;
	struct spdk_nvme_ctrlr *ctrlr;
};

static void
nvme_ctrlr_set_intel_support_log_pages_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct intel_log_pages_ctx *ctx = arg;
	struct spdk_nvme_ctrlr *ctrlr = ctx->ctrlr;

	if (!spdk_nvme_cpl_is_error(cpl)) {
		nvme_ctrlr_construct_intel_support_log_page_list(ctrlr, &ctx->log_page_directory);
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_SUPPORTED_FEATURES,
			     ctrlr->opts.admin_timeout_ms);
	free(ctx);
}

static int nvme_ctrlr_set_intel_support_log_pages(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc = 0;
	struct intel_log_pages_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_SUPPORTED_FEATURES,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	ctx->ctrlr = ctrlr;

	rc = spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_INTEL_LOG_PAGE_DIRECTORY,
					      SPDK_NVME_GLOBAL_NS_TAG, &ctx->log_page_directory,
					      sizeof(struct spdk_nvme_intel_log_page_directory),
					      0, nvme_ctrlr_set_intel_support_log_pages_done, ctx);
	if (rc != 0) {
		free(ctx);
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_SUPPORTED_FEATURES,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_SUPPORTED_INTEL_LOG_PAGES,
			     ctrlr->opts.admin_timeout_ms);

	return 0;
}

static int
nvme_ctrlr_alloc_ana_log_page(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t ana_log_page_size;

	ana_log_page_size = sizeof(struct spdk_nvme_ana_page) + ctrlr->cdata.nanagrpid *
			    sizeof(struct spdk_nvme_ana_group_descriptor) + ctrlr->active_ns_count *
			    sizeof(uint32_t);

	/* Number of active namespaces may have changed.
	 * Check if ANA log page fits into existing buffer.
	 */
	if (ana_log_page_size > ctrlr->ana_log_page_size) {
		void *new_buffer;

		if (ctrlr->ana_log_page) {
			new_buffer = realloc(ctrlr->ana_log_page, ana_log_page_size);
		} else {
			new_buffer = calloc(1, ana_log_page_size);
		}

		if (!new_buffer) {
			NVME_CTRLR_ERRLOG(ctrlr, "could not allocate ANA log page buffer, size %u\n",
					  ana_log_page_size);
			return -ENXIO;
		}

		ctrlr->ana_log_page = new_buffer;
		if (ctrlr->copied_ana_desc) {
			new_buffer = realloc(ctrlr->copied_ana_desc, ana_log_page_size);
		} else {
			new_buffer = calloc(1, ana_log_page_size);
		}

		if (!new_buffer) {
			NVME_CTRLR_ERRLOG(ctrlr, "could not allocate a buffer to parse ANA descriptor, size %u\n",
					  ana_log_page_size);
			return -ENOMEM;
		}

		ctrlr->copied_ana_desc = new_buffer;
		ctrlr->ana_log_page_size = ana_log_page_size;
	}

	return 0;
}

static int
nvme_ctrlr_update_ana_log_page(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_completion_poll_status *status;
	int rc;

	rc = nvme_ctrlr_alloc_ana_log_page(ctrlr);
	if (rc != 0) {
		return rc;
	}

	status = calloc(1, sizeof(*status));
	if (status == NULL) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	rc = spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS,
					      SPDK_NVME_GLOBAL_NS_TAG, ctrlr->ana_log_page,
					      ctrlr->ana_log_page_size, 0,
					      nvme_completion_poll_cb, status);
	if (rc != 0) {
		free(status);
		return rc;
	}

	if (nvme_wait_for_completion_robust_lock_timeout(ctrlr->adminq, status, &ctrlr->ctrlr_lock,
			ctrlr->opts.admin_timeout_ms * 1000)) {
		if (!status->timed_out) {
			free(status);
		}
		return -EIO;
	}

	free(status);
	return 0;
}

static int
nvme_ctrlr_init_ana_log_page(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	rc = nvme_ctrlr_alloc_ana_log_page(ctrlr);
	if (rc) {
		return rc;
	}

	return nvme_ctrlr_update_ana_log_page(ctrlr);
}

static int
nvme_ctrlr_update_ns_ana_states(const struct spdk_nvme_ana_group_descriptor *desc,
				void *cb_arg)
{
	struct spdk_nvme_ctrlr *ctrlr = cb_arg;
	struct spdk_nvme_ns *ns;
	uint32_t i, nsid;

	for (i = 0; i < desc->num_of_nsid; i++) {
		nsid = desc->nsid[i];
		if (nsid == 0 || nsid > ctrlr->cdata.nn) {
			continue;
		}

		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		assert(ns != NULL);

		ns->ana_group_id = desc->ana_group_id;
		ns->ana_state = desc->ana_state;
	}

	return 0;
}

int
nvme_ctrlr_parse_ana_log_page(struct spdk_nvme_ctrlr *ctrlr,
			      spdk_nvme_parse_ana_log_page_cb cb_fn, void *cb_arg)
{
	struct spdk_nvme_ana_group_descriptor *copied_desc;
	uint8_t *orig_desc;
	uint32_t i, desc_size, copy_len;
	int rc = 0;

	if (ctrlr->ana_log_page == NULL) {
		return -EINVAL;
	}

	copied_desc = ctrlr->copied_ana_desc;

	orig_desc = (uint8_t *)ctrlr->ana_log_page + sizeof(struct spdk_nvme_ana_page);
	copy_len = ctrlr->ana_log_page_size - sizeof(struct spdk_nvme_ana_page);

	for (i = 0; i < ctrlr->ana_log_page->num_ana_group_desc; i++) {
		memcpy(copied_desc, orig_desc, copy_len);

		rc = cb_fn(copied_desc, cb_arg);
		if (rc != 0) {
			break;
		}

		desc_size = sizeof(struct spdk_nvme_ana_group_descriptor) +
			    copied_desc->num_of_nsid * sizeof(uint32_t);
		orig_desc += desc_size;
		copy_len -= desc_size;
	}

	return rc;
}

static int
nvme_ctrlr_set_supported_log_pages(struct spdk_nvme_ctrlr *ctrlr)
{
	int	rc = 0;

	memset(ctrlr->log_page_supported, 0, sizeof(ctrlr->log_page_supported));
	/* Mandatory pages */
	ctrlr->log_page_supported[SPDK_NVME_LOG_ERROR] = true;
	ctrlr->log_page_supported[SPDK_NVME_LOG_HEALTH_INFORMATION] = true;
	ctrlr->log_page_supported[SPDK_NVME_LOG_FIRMWARE_SLOT] = true;
	if (ctrlr->cdata.lpa.celp) {
		ctrlr->log_page_supported[SPDK_NVME_LOG_COMMAND_EFFECTS_LOG] = true;
	}

	if (ctrlr->cdata.cmic.ana_reporting) {
		ctrlr->log_page_supported[SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS] = true;
		if (!ctrlr->opts.disable_read_ana_log_page) {
			rc = nvme_ctrlr_init_ana_log_page(ctrlr);
			if (rc == 0) {
				nvme_ctrlr_parse_ana_log_page(ctrlr, nvme_ctrlr_update_ns_ana_states,
							      ctrlr);
			}
		}
	}

	if (ctrlr->cdata.vid == SPDK_PCI_VID_INTEL && !(ctrlr->quirks & NVME_INTEL_QUIRK_NO_LOG_PAGES)) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_SUPPORTED_INTEL_LOG_PAGES,
				     ctrlr->opts.admin_timeout_ms);

	} else {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_SUPPORTED_FEATURES,
				     ctrlr->opts.admin_timeout_ms);

	}

	return rc;
}

static void
nvme_ctrlr_set_intel_supported_features(struct spdk_nvme_ctrlr *ctrlr)
{
	ctrlr->feature_supported[SPDK_NVME_INTEL_FEAT_MAX_LBA] = true;
	ctrlr->feature_supported[SPDK_NVME_INTEL_FEAT_NATIVE_MAX_LBA] = true;
	ctrlr->feature_supported[SPDK_NVME_INTEL_FEAT_POWER_GOVERNOR_SETTING] = true;
	ctrlr->feature_supported[SPDK_NVME_INTEL_FEAT_SMBUS_ADDRESS] = true;
	ctrlr->feature_supported[SPDK_NVME_INTEL_FEAT_LED_PATTERN] = true;
	ctrlr->feature_supported[SPDK_NVME_INTEL_FEAT_RESET_TIMED_WORKLOAD_COUNTERS] = true;
	ctrlr->feature_supported[SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING] = true;
}

static void
nvme_ctrlr_set_arbitration_feature(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t cdw11;
	struct nvme_completion_poll_status *status;

	if (ctrlr->opts.arbitration_burst == 0) {
		return;
	}

	if (ctrlr->opts.arbitration_burst > 7) {
		NVME_CTRLR_WARNLOG(ctrlr, "Valid arbitration burst values is from 0-7\n");
		return;
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return;
	}

	cdw11 = ctrlr->opts.arbitration_burst;

	if (spdk_nvme_ctrlr_get_flags(ctrlr) & SPDK_NVME_CTRLR_WRR_SUPPORTED) {
		cdw11 |= (uint32_t)ctrlr->opts.low_priority_weight << 8;
		cdw11 |= (uint32_t)ctrlr->opts.medium_priority_weight << 16;
		cdw11 |= (uint32_t)ctrlr->opts.high_priority_weight << 24;
	}

	if (spdk_nvme_ctrlr_cmd_set_feature(ctrlr, SPDK_NVME_FEAT_ARBITRATION,
					    cdw11, 0, NULL, 0,
					    nvme_completion_poll_cb, status) < 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "Set arbitration feature failed\n");
		free(status);
		return;
	}

	if (nvme_wait_for_completion_timeout(ctrlr->adminq, status,
					     ctrlr->opts.admin_timeout_ms * 1000)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Timeout to set arbitration feature\n");
	}

	if (!status->timed_out) {
		free(status);
	}
}

static void
nvme_ctrlr_set_supported_features(struct spdk_nvme_ctrlr *ctrlr)
{
	memset(ctrlr->feature_supported, 0, sizeof(ctrlr->feature_supported));
	/* Mandatory features */
	ctrlr->feature_supported[SPDK_NVME_FEAT_ARBITRATION] = true;
	ctrlr->feature_supported[SPDK_NVME_FEAT_POWER_MANAGEMENT] = true;
	ctrlr->feature_supported[SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD] = true;
	ctrlr->feature_supported[SPDK_NVME_FEAT_ERROR_RECOVERY] = true;
	ctrlr->feature_supported[SPDK_NVME_FEAT_NUMBER_OF_QUEUES] = true;
	ctrlr->feature_supported[SPDK_NVME_FEAT_INTERRUPT_COALESCING] = true;
	ctrlr->feature_supported[SPDK_NVME_FEAT_INTERRUPT_VECTOR_CONFIGURATION] = true;
	ctrlr->feature_supported[SPDK_NVME_FEAT_WRITE_ATOMICITY] = true;
	ctrlr->feature_supported[SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION] = true;
	/* Optional features */
	if (ctrlr->cdata.vwc.present) {
		ctrlr->feature_supported[SPDK_NVME_FEAT_VOLATILE_WRITE_CACHE] = true;
	}
	if (ctrlr->cdata.apsta.supported) {
		ctrlr->feature_supported[SPDK_NVME_FEAT_AUTONOMOUS_POWER_STATE_TRANSITION] = true;
	}
	if (ctrlr->cdata.hmpre) {
		ctrlr->feature_supported[SPDK_NVME_FEAT_HOST_MEM_BUFFER] = true;
	}
	if (ctrlr->cdata.vid == SPDK_PCI_VID_INTEL) {
		nvme_ctrlr_set_intel_supported_features(ctrlr);
	}

	nvme_ctrlr_set_arbitration_feature(ctrlr);
}

bool
spdk_nvme_ctrlr_is_failed(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->is_failed;
}

void
nvme_ctrlr_fail(struct spdk_nvme_ctrlr *ctrlr, bool hot_remove)
{
	/*
	 * Set the flag here and leave the work failure of qpairs to
	 * spdk_nvme_qpair_process_completions().
	 */
	if (hot_remove) {
		ctrlr->is_removed = true;
	}

	if (ctrlr->is_failed) {
		NVME_CTRLR_NOTICELOG(ctrlr, "already in failed state\n");
		return;
	}

	ctrlr->is_failed = true;
	nvme_transport_ctrlr_disconnect_qpair(ctrlr, ctrlr->adminq);
	NVME_CTRLR_ERRLOG(ctrlr, "in failed state.\n");
}

/**
 * This public API function will try to take the controller lock.
 * Any private functions being called from a thread already holding
 * the ctrlr lock should call nvme_ctrlr_fail directly.
 */
void
spdk_nvme_ctrlr_fail(struct spdk_nvme_ctrlr *ctrlr)
{
	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	nvme_ctrlr_fail(ctrlr, false);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
}

static void
nvme_ctrlr_shutdown_set_cc_done(void *_ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_ctrlr_detach_ctx *ctx = _ctx;
	struct spdk_nvme_ctrlr *ctrlr = ctx->ctrlr;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to write CC.SHN\n");
		ctx->shutdown_complete = true;
		return;
	}

	if (ctrlr->opts.no_shn_notification) {
		ctx->shutdown_complete = true;
		return;
	}

	/*
	 * The NVMe specification defines RTD3E to be the time between
	 *  setting SHN = 1 until the controller will set SHST = 10b.
	 * If the device doesn't report RTD3 entry latency, or if it
	 *  reports RTD3 entry latency less than 10 seconds, pick
	 *  10 seconds as a reasonable amount of time to
	 *  wait before proceeding.
	 */
	NVME_CTRLR_DEBUGLOG(ctrlr, "RTD3E = %" PRIu32 " us\n", ctrlr->cdata.rtd3e);
	ctx->shutdown_timeout_ms = SPDK_CEIL_DIV(ctrlr->cdata.rtd3e, 1000);
	ctx->shutdown_timeout_ms = spdk_max(ctx->shutdown_timeout_ms, 10000);
	NVME_CTRLR_DEBUGLOG(ctrlr, "shutdown timeout = %" PRIu32 " ms\n", ctx->shutdown_timeout_ms);

	ctx->shutdown_start_tsc = spdk_get_ticks();
	ctx->state = NVME_CTRLR_DETACH_CHECK_CSTS;
}

static void
nvme_ctrlr_shutdown_get_cc_done(void *_ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_ctrlr_detach_ctx *ctx = _ctx;
	struct spdk_nvme_ctrlr *ctrlr = ctx->ctrlr;
	union spdk_nvme_cc_register cc;
	int rc;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CC register\n");
		ctx->shutdown_complete = true;
		return;
	}

	assert(value <= UINT32_MAX);
	cc.raw = (uint32_t)value;

	if (ctrlr->opts.no_shn_notification) {
		NVME_CTRLR_INFOLOG(ctrlr, "Disable SSD without shutdown notification\n");
		if (cc.bits.en == 0) {
			ctx->shutdown_complete = true;
			return;
		}

		cc.bits.en = 0;
	} else {
		cc.bits.shn = SPDK_NVME_SHN_NORMAL;
	}

	rc = nvme_ctrlr_set_cc_async(ctrlr, cc.raw, nvme_ctrlr_shutdown_set_cc_done, ctx);
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to write CC.SHN\n");
		ctx->shutdown_complete = true;
	}
}

static void
nvme_ctrlr_shutdown_async(struct spdk_nvme_ctrlr *ctrlr,
			  struct nvme_ctrlr_detach_ctx *ctx)
{
	int rc;

	if (ctrlr->is_removed) {
		ctx->shutdown_complete = true;
		return;
	}

	ctx->state = NVME_CTRLR_DETACH_SET_CC;
	rc = nvme_ctrlr_get_cc_async(ctrlr, nvme_ctrlr_shutdown_get_cc_done, ctx);
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CC register\n");
		ctx->shutdown_complete = true;
	}
}

static void
nvme_ctrlr_shutdown_get_csts_done(void *_ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_ctrlr_detach_ctx *ctx = _ctx;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctx->ctrlr, "Failed to read the CSTS register\n");
		ctx->shutdown_complete = true;
		return;
	}

	assert(value <= UINT32_MAX);
	ctx->csts.raw = (uint32_t)value;
	ctx->state = NVME_CTRLR_DETACH_GET_CSTS_DONE;
}

static int
nvme_ctrlr_shutdown_poll_async(struct spdk_nvme_ctrlr *ctrlr,
			       struct nvme_ctrlr_detach_ctx *ctx)
{
	union spdk_nvme_csts_register	csts;
	uint32_t			ms_waited;

	switch (ctx->state) {
	case NVME_CTRLR_DETACH_SET_CC:
	case NVME_CTRLR_DETACH_GET_CSTS:
		/* We're still waiting for the register operation to complete */
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
		return -EAGAIN;

	case NVME_CTRLR_DETACH_CHECK_CSTS:
		ctx->state = NVME_CTRLR_DETACH_GET_CSTS;
		if (nvme_ctrlr_get_csts_async(ctrlr, nvme_ctrlr_shutdown_get_csts_done, ctx)) {
			NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CSTS register\n");
			return -EIO;
		}
		return -EAGAIN;

	case NVME_CTRLR_DETACH_GET_CSTS_DONE:
		ctx->state = NVME_CTRLR_DETACH_CHECK_CSTS;
		break;

	default:
		assert(0 && "Should never happen");
		return -EINVAL;
	}

	ms_waited = (spdk_get_ticks() - ctx->shutdown_start_tsc) * 1000 / spdk_get_ticks_hz();
	csts.raw = ctx->csts.raw;

	if (csts.bits.shst == SPDK_NVME_SHST_COMPLETE) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "shutdown complete in %u milliseconds\n", ms_waited);
		return 0;
	}

	if (ms_waited < ctx->shutdown_timeout_ms) {
		return -EAGAIN;
	}

	NVME_CTRLR_ERRLOG(ctrlr, "did not shutdown within %u milliseconds\n",
			  ctx->shutdown_timeout_ms);
	if (ctrlr->quirks & NVME_QUIRK_SHST_COMPLETE) {
		NVME_CTRLR_ERRLOG(ctrlr, "likely due to shutdown handling in the VMWare emulated NVMe SSD\n");
	}

	return 0;
}

static inline uint64_t
nvme_ctrlr_get_ready_timeout(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->cap.bits.to * 500;
}

static void
nvme_ctrlr_set_cc_en_done(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to set the CC register\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1,
			     nvme_ctrlr_get_ready_timeout(ctrlr));
}

static int
nvme_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_cc_register	cc;
	int				rc;

	rc = nvme_transport_ctrlr_enable(ctrlr);
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "transport ctrlr_enable failed\n");
		return rc;
	}

	cc.raw = ctrlr->process_init_cc.raw;
	if (cc.bits.en != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "called with CC.EN = 1\n");
		return -EINVAL;
	}

	cc.bits.en = 1;
	cc.bits.css = 0;
	cc.bits.shn = 0;
	cc.bits.iosqes = 6; /* SQ entry size == 64 == 2^6 */
	cc.bits.iocqes = 4; /* CQ entry size == 16 == 2^4 */

	/* Page size is 2 ^ (12 + mps). */
	cc.bits.mps = spdk_u32log2(ctrlr->page_size) - 12;

	/*
	 * Since NVMe 1.0, a controller should have at least one bit set in CAP.CSS.
	 * A controller that does not have any bit set in CAP.CSS is not spec compliant.
	 * Try to support such a controller regardless.
	 */
	if (ctrlr->cap.bits.css == 0) {
		NVME_CTRLR_INFOLOG(ctrlr, "Drive reports no command sets supported. Assuming NVM is supported.\n");
		ctrlr->cap.bits.css = SPDK_NVME_CAP_CSS_NVM;
	}

	/*
	 * If the user did not explicitly request a command set, or supplied a value larger than
	 * what can be saved in CC.CSS, use the most reasonable default.
	 */
	if (ctrlr->opts.command_set >= CHAR_BIT) {
		if (ctrlr->cap.bits.css & SPDK_NVME_CAP_CSS_IOCS) {
			ctrlr->opts.command_set = SPDK_NVME_CC_CSS_IOCS;
		} else if (ctrlr->cap.bits.css & SPDK_NVME_CAP_CSS_NVM) {
			ctrlr->opts.command_set = SPDK_NVME_CC_CSS_NVM;
		} else if (ctrlr->cap.bits.css & SPDK_NVME_CAP_CSS_NOIO) {
			ctrlr->opts.command_set = SPDK_NVME_CC_CSS_NOIO;
		} else {
			/* Invalid supported bits detected, falling back to NVM. */
			ctrlr->opts.command_set = SPDK_NVME_CC_CSS_NVM;
		}
	}

	/* Verify that the selected command set is supported by the controller. */
	if (!(ctrlr->cap.bits.css & (1u << ctrlr->opts.command_set))) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Requested I/O command set %u but supported mask is 0x%x\n",
				    ctrlr->opts.command_set, ctrlr->cap.bits.css);
		NVME_CTRLR_DEBUGLOG(ctrlr, "Falling back to NVM. Assuming NVM is supported.\n");
		ctrlr->opts.command_set = SPDK_NVME_CC_CSS_NVM;
	}

	cc.bits.css = ctrlr->opts.command_set;

	switch (ctrlr->opts.arb_mechanism) {
	case SPDK_NVME_CC_AMS_RR:
		break;
	case SPDK_NVME_CC_AMS_WRR:
		if (SPDK_NVME_CAP_AMS_WRR & ctrlr->cap.bits.ams) {
			break;
		}
		return -EINVAL;
	case SPDK_NVME_CC_AMS_VS:
		if (SPDK_NVME_CAP_AMS_VS & ctrlr->cap.bits.ams) {
			break;
		}
		return -EINVAL;
	default:
		return -EINVAL;
	}

	cc.bits.ams = ctrlr->opts.arb_mechanism;
	ctrlr->process_init_cc.raw = cc.raw;

	if (nvme_ctrlr_set_cc_async(ctrlr, cc.raw, nvme_ctrlr_set_cc_en_done, ctrlr)) {
		NVME_CTRLR_ERRLOG(ctrlr, "set_cc() failed\n");
		return -EIO;
	}

	return 0;
}

static const char *
nvme_ctrlr_state_string(enum nvme_ctrlr_state state)
{
	switch (state) {
	case NVME_CTRLR_STATE_INIT_DELAY:
		return "delay init";
	case NVME_CTRLR_STATE_CONNECT_ADMINQ:
		return "connect adminq";
	case NVME_CTRLR_STATE_WAIT_FOR_CONNECT_ADMINQ:
		return "wait for connect adminq";
	case NVME_CTRLR_STATE_READ_VS:
		return "read vs";
	case NVME_CTRLR_STATE_READ_VS_WAIT_FOR_VS:
		return "read vs wait for vs";
	case NVME_CTRLR_STATE_READ_CAP:
		return "read cap";
	case NVME_CTRLR_STATE_READ_CAP_WAIT_FOR_CAP:
		return "read cap wait for cap";
	case NVME_CTRLR_STATE_CHECK_EN:
		return "check en";
	case NVME_CTRLR_STATE_CHECK_EN_WAIT_FOR_CC:
		return "check en wait for cc";
	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1:
		return "disable and wait for CSTS.RDY = 1";
	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1_WAIT_FOR_CSTS:
		return "disable and wait for CSTS.RDY = 1 reg";
	case NVME_CTRLR_STATE_SET_EN_0:
		return "set CC.EN = 0";
	case NVME_CTRLR_STATE_SET_EN_0_WAIT_FOR_CC:
		return "set CC.EN = 0 wait for cc";
	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0:
		return "disable and wait for CSTS.RDY = 0";
	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0_WAIT_FOR_CSTS:
		return "disable and wait for CSTS.RDY = 0 reg";
	case NVME_CTRLR_STATE_ENABLE:
		return "enable controller by writing CC.EN = 1";
	case NVME_CTRLR_STATE_ENABLE_WAIT_FOR_CC:
		return "enable controller by writing CC.EN = 1 reg";
	case NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1:
		return "wait for CSTS.RDY = 1";
	case NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1_WAIT_FOR_CSTS:
		return "wait for CSTS.RDY = 1 reg";
	case NVME_CTRLR_STATE_RESET_ADMIN_QUEUE:
		return "reset admin queue";
	case NVME_CTRLR_STATE_IDENTIFY:
		return "identify controller";
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY:
		return "wait for identify controller";
	case NVME_CTRLR_STATE_CONFIGURE_AER:
		return "configure AER";
	case NVME_CTRLR_STATE_WAIT_FOR_CONFIGURE_AER:
		return "wait for configure aer";
	case NVME_CTRLR_STATE_SET_KEEP_ALIVE_TIMEOUT:
		return "set keep alive timeout";
	case NVME_CTRLR_STATE_WAIT_FOR_KEEP_ALIVE_TIMEOUT:
		return "wait for set keep alive timeout";
	case NVME_CTRLR_STATE_IDENTIFY_IOCS_SPECIFIC:
		return "identify controller iocs specific";
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_IOCS_SPECIFIC:
		return "wait for identify controller iocs specific";
	case NVME_CTRLR_STATE_GET_ZNS_CMD_EFFECTS_LOG:
		return "get zns cmd and effects log page";
	case NVME_CTRLR_STATE_WAIT_FOR_GET_ZNS_CMD_EFFECTS_LOG:
		return "wait for get zns cmd and effects log page";
	case NVME_CTRLR_STATE_SET_NUM_QUEUES:
		return "set number of queues";
	case NVME_CTRLR_STATE_WAIT_FOR_SET_NUM_QUEUES:
		return "wait for set number of queues";
	case NVME_CTRLR_STATE_IDENTIFY_ACTIVE_NS:
		return "identify active ns";
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_ACTIVE_NS:
		return "wait for identify active ns";
	case NVME_CTRLR_STATE_IDENTIFY_NS:
		return "identify ns";
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_NS:
		return "wait for identify ns";
	case NVME_CTRLR_STATE_IDENTIFY_ID_DESCS:
		return "identify namespace id descriptors";
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_ID_DESCS:
		return "wait for identify namespace id descriptors";
	case NVME_CTRLR_STATE_IDENTIFY_NS_IOCS_SPECIFIC:
		return "identify ns iocs specific";
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_NS_IOCS_SPECIFIC:
		return "wait for identify ns iocs specific";
	case NVME_CTRLR_STATE_SET_SUPPORTED_LOG_PAGES:
		return "set supported log pages";
	case NVME_CTRLR_STATE_SET_SUPPORTED_INTEL_LOG_PAGES:
		return "set supported INTEL log pages";
	case NVME_CTRLR_STATE_WAIT_FOR_SUPPORTED_INTEL_LOG_PAGES:
		return "wait for supported INTEL log pages";
	case NVME_CTRLR_STATE_SET_SUPPORTED_FEATURES:
		return "set supported features";
	case NVME_CTRLR_STATE_SET_DB_BUF_CFG:
		return "set doorbell buffer config";
	case NVME_CTRLR_STATE_WAIT_FOR_DB_BUF_CFG:
		return "wait for doorbell buffer config";
	case NVME_CTRLR_STATE_SET_HOST_ID:
		return "set host ID";
	case NVME_CTRLR_STATE_WAIT_FOR_HOST_ID:
		return "wait for set host ID";
	case NVME_CTRLR_STATE_READY:
		return "ready";
	case NVME_CTRLR_STATE_ERROR:
		return "error";
	}
	return "unknown";
};

static void
_nvme_ctrlr_set_state(struct spdk_nvme_ctrlr *ctrlr, enum nvme_ctrlr_state state,
		      uint64_t timeout_in_ms, bool quiet)
{
	uint64_t ticks_per_ms, timeout_in_ticks, now_ticks;

	ctrlr->state = state;
	if (timeout_in_ms == NVME_TIMEOUT_KEEP_EXISTING) {
		if (!quiet) {
			NVME_CTRLR_DEBUGLOG(ctrlr, "setting state to %s (keeping existing timeout)\n",
					    nvme_ctrlr_state_string(ctrlr->state));
		}
		return;
	}

	if (timeout_in_ms == NVME_TIMEOUT_INFINITE) {
		goto inf;
	}

	ticks_per_ms = spdk_get_ticks_hz() / 1000;
	if (timeout_in_ms > UINT64_MAX / ticks_per_ms) {
		NVME_CTRLR_ERRLOG(ctrlr,
				  "Specified timeout would cause integer overflow. Defaulting to no timeout.\n");
		goto inf;
	}

	now_ticks = spdk_get_ticks();
	timeout_in_ticks = timeout_in_ms * ticks_per_ms;
	if (timeout_in_ticks > UINT64_MAX - now_ticks) {
		NVME_CTRLR_ERRLOG(ctrlr,
				  "Specified timeout would cause integer overflow. Defaulting to no timeout.\n");
		goto inf;
	}

	ctrlr->state_timeout_tsc = timeout_in_ticks + now_ticks;
	if (!quiet) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "setting state to %s (timeout %" PRIu64 " ms)\n",
				    nvme_ctrlr_state_string(ctrlr->state), timeout_in_ms);
	}
	return;
inf:
	if (!quiet) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "setting state to %s (no timeout)\n",
				    nvme_ctrlr_state_string(ctrlr->state));
	}
	ctrlr->state_timeout_tsc = NVME_TIMEOUT_INFINITE;
}

static void
nvme_ctrlr_set_state(struct spdk_nvme_ctrlr *ctrlr, enum nvme_ctrlr_state state,
		     uint64_t timeout_in_ms)
{
	_nvme_ctrlr_set_state(ctrlr, state, timeout_in_ms, false);
}

static void
nvme_ctrlr_set_state_quiet(struct spdk_nvme_ctrlr *ctrlr, enum nvme_ctrlr_state state,
			   uint64_t timeout_in_ms)
{
	_nvme_ctrlr_set_state(ctrlr, state, timeout_in_ms, true);
}

static void
nvme_ctrlr_free_zns_specific_data(struct spdk_nvme_ctrlr *ctrlr)
{
	spdk_free(ctrlr->cdata_zns);
	ctrlr->cdata_zns = NULL;
}

static void
nvme_ctrlr_free_iocs_specific_data(struct spdk_nvme_ctrlr *ctrlr)
{
	nvme_ctrlr_free_zns_specific_data(ctrlr);
}

static void
nvme_ctrlr_free_doorbell_buffer(struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr->shadow_doorbell) {
		spdk_free(ctrlr->shadow_doorbell);
		ctrlr->shadow_doorbell = NULL;
	}

	if (ctrlr->eventidx) {
		spdk_free(ctrlr->eventidx);
		ctrlr->eventidx = NULL;
	}
}

static void
nvme_ctrlr_set_doorbell_buffer_config_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = (struct spdk_nvme_ctrlr *)arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_WARNLOG(ctrlr, "Doorbell buffer config failed\n");
	} else {
		NVME_CTRLR_INFOLOG(ctrlr, "Doorbell buffer config enabled\n");
	}
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_HOST_ID,
			     ctrlr->opts.admin_timeout_ms);
}

static int
nvme_ctrlr_set_doorbell_buffer_config(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc = 0;
	uint64_t prp1, prp2, len;

	if (!ctrlr->cdata.oacs.doorbell_buffer_config) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_HOST_ID,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	if (ctrlr->trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_HOST_ID,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	/* only 1 page size for doorbell buffer */
	ctrlr->shadow_doorbell = spdk_zmalloc(ctrlr->page_size, ctrlr->page_size,
					      NULL, SPDK_ENV_LCORE_ID_ANY,
					      SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE);
	if (ctrlr->shadow_doorbell == NULL) {
		rc = -ENOMEM;
		goto error;
	}

	len = ctrlr->page_size;
	prp1 = spdk_vtophys(ctrlr->shadow_doorbell, &len);
	if (prp1 == SPDK_VTOPHYS_ERROR || len != ctrlr->page_size) {
		rc = -EFAULT;
		goto error;
	}

	ctrlr->eventidx = spdk_zmalloc(ctrlr->page_size, ctrlr->page_size,
				       NULL, SPDK_ENV_LCORE_ID_ANY,
				       SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE);
	if (ctrlr->eventidx == NULL) {
		rc = -ENOMEM;
		goto error;
	}

	len = ctrlr->page_size;
	prp2 = spdk_vtophys(ctrlr->eventidx, &len);
	if (prp2 == SPDK_VTOPHYS_ERROR || len != ctrlr->page_size) {
		rc = -EFAULT;
		goto error;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_DB_BUF_CFG,
			     ctrlr->opts.admin_timeout_ms);

	rc = nvme_ctrlr_cmd_doorbell_buffer_config(ctrlr, prp1, prp2,
			nvme_ctrlr_set_doorbell_buffer_config_done, ctrlr);
	if (rc != 0) {
		goto error;
	}

	return 0;

error:
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	nvme_ctrlr_free_doorbell_buffer(ctrlr);
	return rc;
}

static void
nvme_ctrlr_abort_queued_aborts(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_request	*req, *tmp;
	struct spdk_nvme_cpl	cpl = {};

	cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;

	STAILQ_FOREACH_SAFE(req, &ctrlr->queued_aborts, stailq, tmp) {
		STAILQ_REMOVE_HEAD(&ctrlr->queued_aborts, stailq);

		nvme_complete_request(req->cb_fn, req->cb_arg, req->qpair, req, &cpl);
		nvme_free_request(req);
	}
}

int
spdk_nvme_ctrlr_disconnect(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_qpair	*qpair;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	ctrlr->prepare_for_reset = false;

	if (ctrlr->is_resetting || ctrlr->is_removed) {
		/*
		 * Controller is already resetting or has been removed. Return
		 *  immediately since there is no need to kick off another
		 *  reset in these cases.
		 */
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return ctrlr->is_resetting ? -EBUSY : -ENXIO;
	}

	ctrlr->is_resetting = true;
	ctrlr->is_failed = false;

	NVME_CTRLR_NOTICELOG(ctrlr, "resetting controller\n");

	/* Disable keep-alive, it'll be re-enabled as part of the init process */
	ctrlr->keep_alive_interval_ticks = 0;

	/* Abort all of the queued abort requests */
	nvme_ctrlr_abort_queued_aborts(ctrlr);

	nvme_transport_admin_qpair_abort_aers(ctrlr->adminq);

	/* Disable all queues before disabling the controller hardware. */
	TAILQ_FOREACH(qpair, &ctrlr->active_io_qpairs, tailq) {
		qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_LOCAL;
	}

	ctrlr->adminq->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_LOCAL;
	nvme_transport_ctrlr_disconnect_qpair(ctrlr, ctrlr->adminq);

	/* Doorbell buffer config is invalid during reset */
	nvme_ctrlr_free_doorbell_buffer(ctrlr);

	/* I/O Command Set Specific Identify Controller data is invalidated during reset */
	nvme_ctrlr_free_iocs_specific_data(ctrlr);

	spdk_bit_array_free(&ctrlr->free_io_qids);

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	return 0;
}

void
spdk_nvme_ctrlr_reconnect_async(struct spdk_nvme_ctrlr *ctrlr)
{
	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);

	/* Set the state back to INIT to cause a full hardware reset. */
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_INIT, NVME_TIMEOUT_INFINITE);

	/* Return without releasing ctrlr_lock. ctrlr_lock will be released when
	 * spdk_nvme_ctrlr_reset_poll_async() returns 0.
	 */
}

static int
nvme_ctrlr_reset_pre(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	rc = spdk_nvme_ctrlr_disconnect(ctrlr);
	if (rc != 0) {
		return rc;
	}

	spdk_nvme_ctrlr_reconnect_async(ctrlr);
	return 0;
}

/**
 * This function will be called when the controller is being reinitialized.
 * Note: the ctrlr_lock must be held when calling this function.
 */
int
spdk_nvme_ctrlr_reconnect_poll_async(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ns *ns, *tmp_ns;
	struct spdk_nvme_qpair	*qpair;
	int rc = 0, rc_tmp = 0;
	bool async;

	if (nvme_ctrlr_process_init(ctrlr) != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "controller reinitialization failed\n");
		rc = -1;
	}
	if (ctrlr->state != NVME_CTRLR_STATE_READY && rc != -1) {
		return -EAGAIN;
	}

	/*
	 * For non-fabrics controllers, the memory locations of the transport qpair
	 * don't change when the controller is reset. They simply need to be
	 * re-enabled with admin commands to the controller. For fabric
	 * controllers we need to disconnect and reconnect the qpair on its
	 * own thread outside of the context of the reset.
	 */
	if (rc == 0 && !spdk_nvme_ctrlr_is_fabrics(ctrlr)) {
		/* Reinitialize qpairs */
		TAILQ_FOREACH(qpair, &ctrlr->active_io_qpairs, tailq) {
			assert(spdk_bit_array_get(ctrlr->free_io_qids, qpair->id));
			spdk_bit_array_clear(ctrlr->free_io_qids, qpair->id);

			/* Force a synchronous connect. We can't currently handle an asynchronous
			 * operation here. */
			async = qpair->async;
			qpair->async = false;
			rc_tmp = nvme_transport_ctrlr_connect_qpair(ctrlr, qpair);
			qpair->async = async;

			if (rc_tmp != 0) {
				rc = rc_tmp;
				qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_LOCAL;
				continue;
			}
		}
	}

	/*
	 * Take this opportunity to remove inactive namespaces. During a reset namespace
	 * handles can be invalidated.
	 */
	RB_FOREACH_SAFE(ns, nvme_ns_tree, &ctrlr->ns, tmp_ns) {
		if (!ns->active) {
			RB_REMOVE(nvme_ns_tree, &ctrlr->ns, ns);
			spdk_free(ns);
		}
	}

	if (rc) {
		nvme_ctrlr_fail(ctrlr, false);
	}
	ctrlr->is_resetting = false;

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	if (!ctrlr->cdata.oaes.ns_attribute_notices) {
		/*
		 * If controller doesn't support ns_attribute_notices and
		 * namespace attributes change (e.g. number of namespaces)
		 * we need to update system handling device reset.
		 */
		nvme_io_msg_ctrlr_update(ctrlr);
	}

	return rc;
}

static void
nvme_ctrlr_reset_ctx_init(struct spdk_nvme_ctrlr_reset_ctx *ctrlr_reset_ctx,
			  struct spdk_nvme_ctrlr *ctrlr)
{
	ctrlr_reset_ctx->ctrlr = ctrlr;
}

static int
nvme_ctrlr_reset_poll_async(struct spdk_nvme_ctrlr_reset_ctx *ctrlr_reset_ctx)
{
	struct spdk_nvme_ctrlr *ctrlr = ctrlr_reset_ctx->ctrlr;

	return spdk_nvme_ctrlr_reconnect_poll_async(ctrlr);
}

int
spdk_nvme_ctrlr_reset_poll_async(struct spdk_nvme_ctrlr_reset_ctx *ctrlr_reset_ctx)
{
	int rc;
	if (!ctrlr_reset_ctx) {
		return -EINVAL;
	}
	rc = nvme_ctrlr_reset_poll_async(ctrlr_reset_ctx);
	if (rc == -EAGAIN) {
		return rc;
	}

	free(ctrlr_reset_ctx);
	return rc;
}

int
spdk_nvme_ctrlr_reset_async(struct spdk_nvme_ctrlr *ctrlr,
			    struct spdk_nvme_ctrlr_reset_ctx **reset_ctx)
{
	struct spdk_nvme_ctrlr_reset_ctx *ctrlr_reset_ctx;
	int rc;

	ctrlr_reset_ctx = calloc(1, sizeof(*ctrlr_reset_ctx));
	if (!ctrlr_reset_ctx) {
		return -ENOMEM;
	}

	rc = nvme_ctrlr_reset_pre(ctrlr);
	if (rc != 0) {
		free(ctrlr_reset_ctx);
	} else {
		nvme_ctrlr_reset_ctx_init(ctrlr_reset_ctx, ctrlr);
		*reset_ctx = ctrlr_reset_ctx;
	}

	return rc;
}

int
spdk_nvme_ctrlr_reset(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ctrlr_reset_ctx reset_ctx = {};
	int rc;

	rc = nvme_ctrlr_reset_pre(ctrlr);
	if (rc != 0) {
		if (rc == -EBUSY) {
			rc = 0;
		}
		return rc;
	}
	nvme_ctrlr_reset_ctx_init(&reset_ctx, ctrlr);

	while (true) {
		rc = nvme_ctrlr_reset_poll_async(&reset_ctx);
		if (rc != -EAGAIN) {
			break;
		}
	}

	return rc;
}

void
spdk_nvme_ctrlr_prepare_for_reset(struct spdk_nvme_ctrlr *ctrlr)
{
	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	ctrlr->prepare_for_reset = true;
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
}

int
spdk_nvme_ctrlr_reset_subsystem(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_cap_register cap;
	int rc = 0;

	cap = spdk_nvme_ctrlr_get_regs_cap(ctrlr);
	if (cap.bits.nssrs == 0) {
		NVME_CTRLR_WARNLOG(ctrlr, "subsystem reset is not supported\n");
		return -ENOTSUP;
	}

	NVME_CTRLR_NOTICELOG(ctrlr, "resetting subsystem\n");
	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	ctrlr->is_resetting = true;
	rc = nvme_ctrlr_set_nssr(ctrlr, SPDK_NVME_NSSR_VALUE);
	ctrlr->is_resetting = false;

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	/*
	 * No more cleanup at this point like in the ctrlr reset. A subsystem reset will cause
	 * a hot remove for PCIe transport. The hot remove handling does all the necessary ctrlr cleanup.
	 */
	return rc;
}

int
spdk_nvme_ctrlr_set_trid(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_transport_id *trid)
{
	int rc = 0;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);

	if (ctrlr->is_failed == false) {
		rc = -EPERM;
		goto out;
	}

	if (trid->trtype != ctrlr->trid.trtype) {
		rc = -EINVAL;
		goto out;
	}

	if (strncmp(trid->subnqn, ctrlr->trid.subnqn, SPDK_NVMF_NQN_MAX_LEN)) {
		rc = -EINVAL;
		goto out;
	}

	ctrlr->trid = *trid;

out:
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	return rc;
}

void
spdk_nvme_ctrlr_set_remove_cb(struct spdk_nvme_ctrlr *ctrlr,
			      spdk_nvme_remove_cb remove_cb, void *remove_ctx)
{
	if (!spdk_process_is_primary()) {
		return;
	}

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	ctrlr->remove_cb = remove_cb;
	ctrlr->cb_ctx = remove_ctx;
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
}

static void
nvme_ctrlr_identify_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = (struct spdk_nvme_ctrlr *)arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "nvme_identify_controller failed!\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	/*
	 * Use MDTS to ensure our default max_xfer_size doesn't exceed what the
	 *  controller supports.
	 */
	ctrlr->max_xfer_size = nvme_transport_ctrlr_get_max_xfer_size(ctrlr);
	NVME_CTRLR_DEBUGLOG(ctrlr, "transport max_xfer_size %u\n", ctrlr->max_xfer_size);
	if (ctrlr->cdata.mdts > 0) {
		ctrlr->max_xfer_size = spdk_min(ctrlr->max_xfer_size,
						ctrlr->min_page_size * (1 << ctrlr->cdata.mdts));
		NVME_CTRLR_DEBUGLOG(ctrlr, "MDTS max_xfer_size %u\n", ctrlr->max_xfer_size);
	}

	NVME_CTRLR_DEBUGLOG(ctrlr, "CNTLID 0x%04" PRIx16 "\n", ctrlr->cdata.cntlid);
	if (ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
		ctrlr->cntlid = ctrlr->cdata.cntlid;
	} else {
		/*
		 * Fabrics controllers should already have CNTLID from the Connect command.
		 *
		 * If CNTLID from Connect doesn't match CNTLID in the Identify Controller data,
		 * trust the one from Connect.
		 */
		if (ctrlr->cntlid != ctrlr->cdata.cntlid) {
			NVME_CTRLR_DEBUGLOG(ctrlr, "Identify CNTLID 0x%04" PRIx16 " != Connect CNTLID 0x%04" PRIx16 "\n",
					    ctrlr->cdata.cntlid, ctrlr->cntlid);
		}
	}

	if (ctrlr->cdata.sgls.supported) {
		assert(ctrlr->cdata.sgls.supported != 0x3);
		ctrlr->flags |= SPDK_NVME_CTRLR_SGL_SUPPORTED;
		if (ctrlr->cdata.sgls.supported == 0x2) {
			ctrlr->flags |= SPDK_NVME_CTRLR_SGL_REQUIRES_DWORD_ALIGNMENT;
		}
		/*
		 * Use MSDBD to ensure our max_sges doesn't exceed what the
		 *  controller supports.
		 */
		ctrlr->max_sges = nvme_transport_ctrlr_get_max_sges(ctrlr);
		if (ctrlr->cdata.nvmf_specific.msdbd != 0) {
			ctrlr->max_sges = spdk_min(ctrlr->cdata.nvmf_specific.msdbd, ctrlr->max_sges);
		} else {
			/* A value 0 indicates no limit. */
		}
		NVME_CTRLR_DEBUGLOG(ctrlr, "transport max_sges %u\n", ctrlr->max_sges);
	}

	if (ctrlr->cdata.oacs.security && !(ctrlr->quirks & NVME_QUIRK_OACS_SECURITY)) {
		ctrlr->flags |= SPDK_NVME_CTRLR_SECURITY_SEND_RECV_SUPPORTED;
	}

	if (ctrlr->cdata.oacs.directives) {
		ctrlr->flags |= SPDK_NVME_CTRLR_DIRECTIVES_SUPPORTED;
	}

	NVME_CTRLR_DEBUGLOG(ctrlr, "fuses compare and write: %d\n",
			    ctrlr->cdata.fuses.compare_and_write);
	if (ctrlr->cdata.fuses.compare_and_write) {
		ctrlr->flags |= SPDK_NVME_CTRLR_COMPARE_AND_WRITE_SUPPORTED;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_CONFIGURE_AER,
			     ctrlr->opts.admin_timeout_ms);
}

static int
nvme_ctrlr_identify(struct spdk_nvme_ctrlr *ctrlr)
{
	int	rc;

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY,
			     ctrlr->opts.admin_timeout_ms);

	rc = nvme_ctrlr_cmd_identify(ctrlr, SPDK_NVME_IDENTIFY_CTRLR, 0, 0, 0,
				     &ctrlr->cdata, sizeof(ctrlr->cdata),
				     nvme_ctrlr_identify_done, ctrlr);
	if (rc != 0) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return rc;
	}

	return 0;
}

static void
nvme_ctrlr_get_zns_cmd_and_effects_log_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_cmds_and_effect_log_page *log_page;
	struct spdk_nvme_ctrlr *ctrlr = arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "nvme_ctrlr_get_zns_cmd_and_effects_log failed!\n");
		spdk_free(ctrlr->tmp_ptr);
		ctrlr->tmp_ptr = NULL;
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	log_page = ctrlr->tmp_ptr;

	if (log_page->io_cmds_supported[SPDK_NVME_OPC_ZONE_APPEND].csupp) {
		ctrlr->flags |= SPDK_NVME_CTRLR_ZONE_APPEND_SUPPORTED;
	}
	spdk_free(ctrlr->tmp_ptr);
	ctrlr->tmp_ptr = NULL;

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_NUM_QUEUES, ctrlr->opts.admin_timeout_ms);
}

static int
nvme_ctrlr_get_zns_cmd_and_effects_log(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	assert(!ctrlr->tmp_ptr);
	ctrlr->tmp_ptr = spdk_zmalloc(sizeof(struct spdk_nvme_cmds_and_effect_log_page), 64, NULL,
				      SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE | SPDK_MALLOC_DMA);
	if (!ctrlr->tmp_ptr) {
		rc = -ENOMEM;
		goto error;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_GET_ZNS_CMD_EFFECTS_LOG,
			     ctrlr->opts.admin_timeout_ms);

	rc = spdk_nvme_ctrlr_cmd_get_log_page_ext(ctrlr, SPDK_NVME_LOG_COMMAND_EFFECTS_LOG,
			0, ctrlr->tmp_ptr, sizeof(struct spdk_nvme_cmds_and_effect_log_page),
			0, 0, 0, SPDK_NVME_CSI_ZNS << 24,
			nvme_ctrlr_get_zns_cmd_and_effects_log_done, ctrlr);
	if (rc != 0) {
		goto error;
	}

	return 0;

error:
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	spdk_free(ctrlr->tmp_ptr);
	ctrlr->tmp_ptr = NULL;
	return rc;
}

static void
nvme_ctrlr_identify_zns_specific_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = (struct spdk_nvme_ctrlr *)arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		/* no need to print an error, the controller simply does not support ZNS */
		nvme_ctrlr_free_zns_specific_data(ctrlr);
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_NUM_QUEUES,
				     ctrlr->opts.admin_timeout_ms);
		return;
	}

	/* A zero zasl value means use mdts */
	if (ctrlr->cdata_zns->zasl) {
		uint32_t max_append = ctrlr->min_page_size * (1 << ctrlr->cdata_zns->zasl);
		ctrlr->max_zone_append_size = spdk_min(ctrlr->max_xfer_size, max_append);
	} else {
		ctrlr->max_zone_append_size = ctrlr->max_xfer_size;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_GET_ZNS_CMD_EFFECTS_LOG,
			     ctrlr->opts.admin_timeout_ms);
}

/**
 * This function will try to fetch the I/O Command Specific Controller data structure for
 * each I/O Command Set supported by SPDK.
 *
 * If an I/O Command Set is not supported by the controller, "Invalid Field in Command"
 * will be returned. Since we are fetching in a exploratively way, getting an error back
 * from the controller should not be treated as fatal.
 *
 * I/O Command Sets not supported by SPDK will be skipped (e.g. Key Value Command Set).
 *
 * I/O Command Sets without a IOCS specific data structure (i.e. a zero-filled IOCS specific
 * data structure) will be skipped (e.g. NVM Command Set, Key Value Command Set).
 */
static int
nvme_ctrlr_identify_iocs_specific(struct spdk_nvme_ctrlr *ctrlr)
{
	int	rc;

	if (!nvme_ctrlr_multi_iocs_enabled(ctrlr)) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_NUM_QUEUES,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	/*
	 * Since SPDK currently only needs to fetch a single Command Set, keep the code here,
	 * instead of creating multiple NVME_CTRLR_STATE_IDENTIFY_IOCS_SPECIFIC substates,
	 * which would require additional functions and complexity for no good reason.
	 */
	assert(!ctrlr->cdata_zns);
	ctrlr->cdata_zns = spdk_zmalloc(sizeof(*ctrlr->cdata_zns), 64, NULL, SPDK_ENV_SOCKET_ID_ANY,
					SPDK_MALLOC_SHARE | SPDK_MALLOC_DMA);
	if (!ctrlr->cdata_zns) {
		rc = -ENOMEM;
		goto error;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_IOCS_SPECIFIC,
			     ctrlr->opts.admin_timeout_ms);

	rc = nvme_ctrlr_cmd_identify(ctrlr, SPDK_NVME_IDENTIFY_CTRLR_IOCS, 0, 0, SPDK_NVME_CSI_ZNS,
				     ctrlr->cdata_zns, sizeof(*ctrlr->cdata_zns),
				     nvme_ctrlr_identify_zns_specific_done, ctrlr);
	if (rc != 0) {
		goto error;
	}

	return 0;

error:
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	nvme_ctrlr_free_zns_specific_data(ctrlr);
	return rc;
}

enum nvme_active_ns_state {
	NVME_ACTIVE_NS_STATE_IDLE,
	NVME_ACTIVE_NS_STATE_PROCESSING,
	NVME_ACTIVE_NS_STATE_DONE,
	NVME_ACTIVE_NS_STATE_ERROR
};

typedef void (*nvme_active_ns_ctx_deleter)(struct nvme_active_ns_ctx *);

struct nvme_active_ns_ctx {
	struct spdk_nvme_ctrlr *ctrlr;
	uint32_t page_count;
	uint32_t next_nsid;
	uint32_t *new_ns_list;
	nvme_active_ns_ctx_deleter deleter;

	enum nvme_active_ns_state state;
};

static struct nvme_active_ns_ctx *
nvme_active_ns_ctx_create(struct spdk_nvme_ctrlr *ctrlr, nvme_active_ns_ctx_deleter deleter)
{
	struct nvme_active_ns_ctx *ctx;
	uint32_t *new_ns_list = NULL;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate nvme_active_ns_ctx!\n");
		return NULL;
	}

	new_ns_list = spdk_zmalloc(sizeof(struct spdk_nvme_ns_list), ctrlr->page_size,
				   NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_SHARE);
	if (!new_ns_list) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate active_ns_list!\n");
		free(ctx);
		return NULL;
	}

	ctx->page_count = 1;
	ctx->new_ns_list = new_ns_list;
	ctx->ctrlr = ctrlr;
	ctx->deleter = deleter;

	return ctx;
}

static void
nvme_active_ns_ctx_destroy(struct nvme_active_ns_ctx *ctx)
{
	spdk_free(ctx->new_ns_list);
	free(ctx);
}

static int
nvme_ctrlr_destruct_namespace(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct spdk_nvme_ns tmp, *ns;

	assert(ctrlr != NULL);

	tmp.id = nsid;
	ns = RB_FIND(nvme_ns_tree, &ctrlr->ns, &tmp);
	if (ns == NULL) {
		return -EINVAL;
	}

	nvme_ns_destruct(ns);
	ns->active = false;

	return 0;
}

static int
nvme_ctrlr_construct_namespace(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct spdk_nvme_ns *ns;

	if (nsid < 1 || nsid > ctrlr->cdata.nn) {
		return -EINVAL;
	}

	/* Namespaces are constructed on demand, so simply request it. */
	ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL) {
		return -ENOMEM;
	}

	ns->active = true;

	return 0;
}

static void
nvme_ctrlr_identify_active_ns_swap(struct spdk_nvme_ctrlr *ctrlr, uint32_t *new_ns_list,
				   size_t max_entries)
{
	uint32_t active_ns_count = 0;
	size_t i;
	uint32_t nsid;
	struct spdk_nvme_ns *ns, *tmp_ns;
	int rc;

	/* First, remove namespaces that no longer exist */
	RB_FOREACH_SAFE(ns, nvme_ns_tree, &ctrlr->ns, tmp_ns) {
		nsid = new_ns_list[0];
		active_ns_count = 0;
		while (nsid != 0) {
			if (nsid == ns->id) {
				break;
			}

			nsid = new_ns_list[active_ns_count++];
		}

		if (nsid != ns->id) {
			/* Did not find this namespace id in the new list. */
			NVME_CTRLR_DEBUGLOG(ctrlr, "Namespace %u was removed\n", ns->id);
			nvme_ctrlr_destruct_namespace(ctrlr, ns->id);
		}
	}

	/* Next, add new namespaces */
	active_ns_count = 0;
	for (i = 0; i < max_entries; i++) {
		nsid = new_ns_list[active_ns_count];

		if (nsid == 0) {
			break;
		}

		/* If the namespace already exists, this will not construct it a second time. */
		rc = nvme_ctrlr_construct_namespace(ctrlr, nsid);
		if (rc != 0) {
			/* We can't easily handle a failure here. But just move on. */
			assert(false);
			NVME_CTRLR_DEBUGLOG(ctrlr, "Failed to allocate a namespace object.\n");
			continue;
		}

		active_ns_count++;
	}

	ctrlr->active_ns_count = active_ns_count;
}

static void
nvme_ctrlr_identify_active_ns_async_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_active_ns_ctx *ctx = arg;
	uint32_t *new_ns_list = NULL;

	if (spdk_nvme_cpl_is_error(cpl)) {
		ctx->state = NVME_ACTIVE_NS_STATE_ERROR;
		goto out;
	}

	ctx->next_nsid = ctx->new_ns_list[1024 * ctx->page_count - 1];
	if (ctx->next_nsid == 0) {
		ctx->state = NVME_ACTIVE_NS_STATE_DONE;
		goto out;
	}

	ctx->page_count++;
	new_ns_list = spdk_realloc(ctx->new_ns_list,
				   ctx->page_count * sizeof(struct spdk_nvme_ns_list),
				   ctx->ctrlr->page_size);
	if (!new_ns_list) {
		SPDK_ERRLOG("Failed to reallocate active_ns_list!\n");
		ctx->state = NVME_ACTIVE_NS_STATE_ERROR;
		goto out;
	}

	ctx->new_ns_list = new_ns_list;
	nvme_ctrlr_identify_active_ns_async(ctx);
	return;

out:
	if (ctx->deleter) {
		ctx->deleter(ctx);
	}
}

static void
nvme_ctrlr_identify_active_ns_async(struct nvme_active_ns_ctx *ctx)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx->ctrlr;
	uint32_t i;
	int rc;

	if (ctrlr->cdata.nn == 0) {
		ctx->state = NVME_ACTIVE_NS_STATE_DONE;
		goto out;
	}

	assert(ctx->new_ns_list != NULL);

	/*
	 * If controller doesn't support active ns list CNS 0x02 dummy up
	 * an active ns list, i.e. all namespaces report as active
	 */
	if (ctrlr->vs.raw < SPDK_NVME_VERSION(1, 1, 0) || ctrlr->quirks & NVME_QUIRK_IDENTIFY_CNS) {
		uint32_t *new_ns_list;

		/*
		 * Active NS list must always end with zero element.
		 * So, we allocate for cdata.nn+1.
		 */
		ctx->page_count = spdk_divide_round_up(ctrlr->cdata.nn + 1,
						       sizeof(struct spdk_nvme_ns_list) / sizeof(new_ns_list[0]));
		new_ns_list = spdk_realloc(ctx->new_ns_list,
					   ctx->page_count * sizeof(struct spdk_nvme_ns_list),
					   ctx->ctrlr->page_size);
		if (!new_ns_list) {
			SPDK_ERRLOG("Failed to reallocate active_ns_list!\n");
			ctx->state = NVME_ACTIVE_NS_STATE_ERROR;
			goto out;
		}

		ctx->new_ns_list = new_ns_list;
		ctx->new_ns_list[ctrlr->cdata.nn] = 0;
		for (i = 0; i < ctrlr->cdata.nn; i++) {
			ctx->new_ns_list[i] = i + 1;
		}

		ctx->state = NVME_ACTIVE_NS_STATE_DONE;
		goto out;
	}

	ctx->state = NVME_ACTIVE_NS_STATE_PROCESSING;
	rc = nvme_ctrlr_cmd_identify(ctrlr, SPDK_NVME_IDENTIFY_ACTIVE_NS_LIST, 0, ctx->next_nsid, 0,
				     &ctx->new_ns_list[1024 * (ctx->page_count - 1)], sizeof(struct spdk_nvme_ns_list),
				     nvme_ctrlr_identify_active_ns_async_done, ctx);
	if (rc != 0) {
		ctx->state = NVME_ACTIVE_NS_STATE_ERROR;
		goto out;
	}

	return;

out:
	if (ctx->deleter) {
		ctx->deleter(ctx);
	}
}

static void
_nvme_active_ns_ctx_deleter(struct nvme_active_ns_ctx *ctx)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx->ctrlr;
	struct spdk_nvme_ns *ns;

	if (ctx->state == NVME_ACTIVE_NS_STATE_ERROR) {
		nvme_active_ns_ctx_destroy(ctx);
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	assert(ctx->state == NVME_ACTIVE_NS_STATE_DONE);

	RB_FOREACH(ns, nvme_ns_tree, &ctrlr->ns) {
		nvme_ns_free_iocs_specific_data(ns);
	}

	nvme_ctrlr_identify_active_ns_swap(ctrlr, ctx->new_ns_list, ctx->page_count * 1024);
	nvme_active_ns_ctx_destroy(ctx);
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_NS, ctrlr->opts.admin_timeout_ms);
}

static void
_nvme_ctrlr_identify_active_ns(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_active_ns_ctx *ctx;

	ctx = nvme_active_ns_ctx_create(ctrlr, _nvme_active_ns_ctx_deleter);
	if (!ctx) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_ACTIVE_NS,
			     ctrlr->opts.admin_timeout_ms);
	nvme_ctrlr_identify_active_ns_async(ctx);
}

int
nvme_ctrlr_identify_active_ns(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_active_ns_ctx *ctx;
	int rc;

	ctx = nvme_active_ns_ctx_create(ctrlr, NULL);
	if (!ctx) {
		return -ENOMEM;
	}

	nvme_ctrlr_identify_active_ns_async(ctx);
	while (ctx->state == NVME_ACTIVE_NS_STATE_PROCESSING) {
		rc = spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
		if (rc < 0) {
			ctx->state = NVME_ACTIVE_NS_STATE_ERROR;
			break;
		}
	}

	if (ctx->state == NVME_ACTIVE_NS_STATE_ERROR) {
		nvme_active_ns_ctx_destroy(ctx);
		return -ENXIO;
	}

	assert(ctx->state == NVME_ACTIVE_NS_STATE_DONE);
	nvme_ctrlr_identify_active_ns_swap(ctrlr, ctx->new_ns_list, ctx->page_count * 1024);
	nvme_active_ns_ctx_destroy(ctx);

	return 0;
}

static void
nvme_ctrlr_identify_ns_async_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ns *ns = (struct spdk_nvme_ns *)arg;
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;
	uint32_t nsid;
	int rc;

	if (spdk_nvme_cpl_is_error(cpl)) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	nvme_ns_set_identify_data(ns);

	/* move on to the next active NS */
	nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, ns->id);
	ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_ID_DESCS,
				     ctrlr->opts.admin_timeout_ms);
		return;
	}
	ns->ctrlr = ctrlr;
	ns->id = nsid;

	rc = nvme_ctrlr_identify_ns_async(ns);
	if (rc) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	}
}

static int
nvme_ctrlr_identify_ns_async(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;
	struct spdk_nvme_ns_data *nsdata;

	nsdata = &ns->nsdata;

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_NS,
			     ctrlr->opts.admin_timeout_ms);
	return nvme_ctrlr_cmd_identify(ns->ctrlr, SPDK_NVME_IDENTIFY_NS, 0, ns->id, 0,
				       nsdata, sizeof(*nsdata),
				       nvme_ctrlr_identify_ns_async_done, ns);
}

static int
nvme_ctrlr_identify_namespaces(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t nsid;
	struct spdk_nvme_ns *ns;
	int rc;

	nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL) {
		/* No active NS, move on to the next state */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_ID_DESCS,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	ns->ctrlr = ctrlr;
	ns->id = nsid;

	rc = nvme_ctrlr_identify_ns_async(ns);
	if (rc) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	}

	return rc;
}

static int
nvme_ctrlr_identify_namespaces_iocs_specific_next(struct spdk_nvme_ctrlr *ctrlr, uint32_t prev_nsid)
{
	uint32_t nsid;
	struct spdk_nvme_ns *ns;
	int rc;

	if (!prev_nsid) {
		nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	} else {
		/* move on to the next active NS */
		nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, prev_nsid);
	}

	ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL) {
		/* No first/next active NS, move on to the next state */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_SUPPORTED_LOG_PAGES,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	/* loop until we find a ns which has (supported) iocs specific data */
	while (!nvme_ns_has_supported_iocs_specific_data(ns)) {
		nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, ns->id);
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns == NULL) {
			/* no namespace with (supported) iocs specific data found */
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_SUPPORTED_LOG_PAGES,
					     ctrlr->opts.admin_timeout_ms);
			return 0;
		}
	}

	rc = nvme_ctrlr_identify_ns_iocs_specific_async(ns);
	if (rc) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	}

	return rc;
}

static void
nvme_ctrlr_identify_ns_zns_specific_async_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ns *ns = (struct spdk_nvme_ns *)arg;
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;

	if (spdk_nvme_cpl_is_error(cpl)) {
		nvme_ns_free_zns_specific_data(ns);
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	nvme_ctrlr_identify_namespaces_iocs_specific_next(ctrlr, ns->id);
}

static int
nvme_ctrlr_identify_ns_iocs_specific_async(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;
	int rc;

	switch (ns->csi) {
	case SPDK_NVME_CSI_ZNS:
		break;
	default:
		/*
		 * This switch must handle all cases for which
		 * nvme_ns_has_supported_iocs_specific_data() returns true,
		 * other cases should never happen.
		 */
		assert(0);
	}

	assert(!ns->nsdata_zns);
	ns->nsdata_zns = spdk_zmalloc(sizeof(*ns->nsdata_zns), 64, NULL, SPDK_ENV_SOCKET_ID_ANY,
				      SPDK_MALLOC_SHARE);
	if (!ns->nsdata_zns) {
		return -ENOMEM;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_NS_IOCS_SPECIFIC,
			     ctrlr->opts.admin_timeout_ms);
	rc = nvme_ctrlr_cmd_identify(ns->ctrlr, SPDK_NVME_IDENTIFY_NS_IOCS, 0, ns->id, ns->csi,
				     ns->nsdata_zns, sizeof(*ns->nsdata_zns),
				     nvme_ctrlr_identify_ns_zns_specific_async_done, ns);
	if (rc) {
		nvme_ns_free_zns_specific_data(ns);
	}

	return rc;
}

static int
nvme_ctrlr_identify_namespaces_iocs_specific(struct spdk_nvme_ctrlr *ctrlr)
{
	if (!nvme_ctrlr_multi_iocs_enabled(ctrlr)) {
		/* Multi IOCS not supported/enabled, move on to the next state */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_SUPPORTED_LOG_PAGES,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	return nvme_ctrlr_identify_namespaces_iocs_specific_next(ctrlr, 0);
}

static void
nvme_ctrlr_identify_id_desc_async_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ns *ns = (struct spdk_nvme_ns *)arg;
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;
	uint32_t nsid;
	int rc;

	if (spdk_nvme_cpl_is_error(cpl)) {
		/*
		 * Many controllers claim to be compatible with NVMe 1.3, however,
		 * they do not implement NS ID Desc List. Therefore, instead of setting
		 * the state to NVME_CTRLR_STATE_ERROR, silently ignore the completion
		 * error and move on to the next state.
		 *
		 * The proper way is to create a new quirk for controllers that violate
		 * the NVMe 1.3 spec by not supporting NS ID Desc List.
		 * (Re-using the NVME_QUIRK_IDENTIFY_CNS quirk is not possible, since
		 * it is too generic and was added in order to handle controllers that
		 * violate the NVMe 1.1 spec by not supporting ACTIVE LIST).
		 */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_NS_IOCS_SPECIFIC,
				     ctrlr->opts.admin_timeout_ms);
		return;
	}

	nvme_ns_set_id_desc_list_data(ns);

	/* move on to the next active NS */
	nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, ns->id);
	ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_NS_IOCS_SPECIFIC,
				     ctrlr->opts.admin_timeout_ms);
		return;
	}

	rc = nvme_ctrlr_identify_id_desc_async(ns);
	if (rc) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	}
}

static int
nvme_ctrlr_identify_id_desc_async(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;

	memset(ns->id_desc_list, 0, sizeof(ns->id_desc_list));

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_ID_DESCS,
			     ctrlr->opts.admin_timeout_ms);
	return nvme_ctrlr_cmd_identify(ns->ctrlr, SPDK_NVME_IDENTIFY_NS_ID_DESCRIPTOR_LIST,
				       0, ns->id, 0, ns->id_desc_list, sizeof(ns->id_desc_list),
				       nvme_ctrlr_identify_id_desc_async_done, ns);
}

static int
nvme_ctrlr_identify_id_desc_namespaces(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t nsid;
	struct spdk_nvme_ns *ns;
	int rc;

	if ((ctrlr->vs.raw < SPDK_NVME_VERSION(1, 3, 0) &&
	     !(ctrlr->cap.bits.css & SPDK_NVME_CAP_CSS_IOCS)) ||
	    (ctrlr->quirks & NVME_QUIRK_IDENTIFY_CNS)) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Version < 1.3; not attempting to retrieve NS ID Descriptor List\n");
		/* NS ID Desc List not supported, move on to the next state */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_NS_IOCS_SPECIFIC,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL) {
		/* No active NS, move on to the next state */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_NS_IOCS_SPECIFIC,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	rc = nvme_ctrlr_identify_id_desc_async(ns);
	if (rc) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	}

	return rc;
}

static void
nvme_ctrlr_update_nvmf_ioccsz(struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_RDMA ||
	    ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_TCP ||
	    ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_FC) {
		if (ctrlr->cdata.nvmf_specific.ioccsz < 4) {
			NVME_CTRLR_ERRLOG(ctrlr, "Incorrect IOCCSZ %u, the minimum value should be 4\n",
					  ctrlr->cdata.nvmf_specific.ioccsz);
			ctrlr->cdata.nvmf_specific.ioccsz = 4;
			assert(0);
		}
		ctrlr->ioccsz_bytes = ctrlr->cdata.nvmf_specific.ioccsz * 16 - sizeof(struct spdk_nvme_cmd);
		ctrlr->icdoff = ctrlr->cdata.nvmf_specific.icdoff;
	}
}

static void
nvme_ctrlr_set_num_queues_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	uint32_t cq_allocated, sq_allocated, min_allocated, i;
	struct spdk_nvme_ctrlr *ctrlr = (struct spdk_nvme_ctrlr *)arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Set Features - Number of Queues failed!\n");
		ctrlr->opts.num_io_queues = 0;
	} else {
		/*
		 * Data in cdw0 is 0-based.
		 * Lower 16-bits indicate number of submission queues allocated.
		 * Upper 16-bits indicate number of completion queues allocated.
		 */
		sq_allocated = (cpl->cdw0 & 0xFFFF) + 1;
		cq_allocated = (cpl->cdw0 >> 16) + 1;

		/*
		 * For 1:1 queue mapping, set number of allocated queues to be minimum of
		 * submission and completion queues.
		 */
		min_allocated = spdk_min(sq_allocated, cq_allocated);

		/* Set number of queues to be minimum of requested and actually allocated. */
		ctrlr->opts.num_io_queues = spdk_min(min_allocated, ctrlr->opts.num_io_queues);
	}

	ctrlr->free_io_qids = spdk_bit_array_create(ctrlr->opts.num_io_queues + 1);
	if (ctrlr->free_io_qids == NULL) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	/* Initialize list of free I/O queue IDs. QID 0 is the admin queue (implicitly allocated). */
	for (i = 1; i <= ctrlr->opts.num_io_queues; i++) {
		spdk_nvme_ctrlr_free_qid(ctrlr, i);
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_ACTIVE_NS,
			     ctrlr->opts.admin_timeout_ms);
}

static int
nvme_ctrlr_set_num_queues(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	if (ctrlr->opts.num_io_queues > SPDK_NVME_MAX_IO_QUEUES) {
		NVME_CTRLR_NOTICELOG(ctrlr, "Limiting requested num_io_queues %u to max %d\n",
				     ctrlr->opts.num_io_queues, SPDK_NVME_MAX_IO_QUEUES);
		ctrlr->opts.num_io_queues = SPDK_NVME_MAX_IO_QUEUES;
	} else if (ctrlr->opts.num_io_queues < 1) {
		NVME_CTRLR_NOTICELOG(ctrlr, "Requested num_io_queues 0, increasing to 1\n");
		ctrlr->opts.num_io_queues = 1;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_SET_NUM_QUEUES,
			     ctrlr->opts.admin_timeout_ms);

	rc = nvme_ctrlr_cmd_set_num_queues(ctrlr, ctrlr->opts.num_io_queues,
					   nvme_ctrlr_set_num_queues_done, ctrlr);
	if (rc != 0) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return rc;
	}

	return 0;
}

static void
nvme_ctrlr_set_keep_alive_timeout_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	uint32_t keep_alive_interval_us;
	struct spdk_nvme_ctrlr *ctrlr = (struct spdk_nvme_ctrlr *)arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		if ((cpl->status.sct == SPDK_NVME_SCT_GENERIC) &&
		    (cpl->status.sc == SPDK_NVME_SC_INVALID_FIELD)) {
			NVME_CTRLR_DEBUGLOG(ctrlr, "Keep alive timeout Get Feature is not supported\n");
		} else {
			NVME_CTRLR_ERRLOG(ctrlr, "Keep alive timeout Get Feature failed: SC %x SCT %x\n",
					  cpl->status.sc, cpl->status.sct);
			ctrlr->opts.keep_alive_timeout_ms = 0;
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
			return;
		}
	} else {
		if (ctrlr->opts.keep_alive_timeout_ms != cpl->cdw0) {
			NVME_CTRLR_DEBUGLOG(ctrlr, "Controller adjusted keep alive timeout to %u ms\n",
					    cpl->cdw0);
		}

		ctrlr->opts.keep_alive_timeout_ms = cpl->cdw0;
	}

	if (ctrlr->opts.keep_alive_timeout_ms == 0) {
		ctrlr->keep_alive_interval_ticks = 0;
	} else {
		keep_alive_interval_us = ctrlr->opts.keep_alive_timeout_ms * 1000 / 2;

		NVME_CTRLR_DEBUGLOG(ctrlr, "Sending keep alive every %u us\n", keep_alive_interval_us);

		ctrlr->keep_alive_interval_ticks = (keep_alive_interval_us * spdk_get_ticks_hz()) /
						   UINT64_C(1000000);

		/* Schedule the first Keep Alive to be sent as soon as possible. */
		ctrlr->next_keep_alive_tick = spdk_get_ticks();
	}

	if (spdk_nvme_ctrlr_is_discovery(ctrlr)) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_READY, NVME_TIMEOUT_INFINITE);
	} else {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_IOCS_SPECIFIC,
				     ctrlr->opts.admin_timeout_ms);
	}
}

static int
nvme_ctrlr_set_keep_alive_timeout(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	if (ctrlr->opts.keep_alive_timeout_ms == 0) {
		if (spdk_nvme_ctrlr_is_discovery(ctrlr)) {
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_READY, NVME_TIMEOUT_INFINITE);
		} else {
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_IOCS_SPECIFIC,
					     ctrlr->opts.admin_timeout_ms);
		}
		return 0;
	}

	/* Note: Discovery controller identify data does not populate KAS according to spec. */
	if (!spdk_nvme_ctrlr_is_discovery(ctrlr) && ctrlr->cdata.kas == 0) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Controller KAS is 0 - not enabling Keep Alive\n");
		ctrlr->opts.keep_alive_timeout_ms = 0;
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY_IOCS_SPECIFIC,
				     ctrlr->opts.admin_timeout_ms);
		return 0;
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_KEEP_ALIVE_TIMEOUT,
			     ctrlr->opts.admin_timeout_ms);

	/* Retrieve actual keep alive timeout, since the controller may have adjusted it. */
	rc = spdk_nvme_ctrlr_cmd_get_feature(ctrlr, SPDK_NVME_FEAT_KEEP_ALIVE_TIMER, 0, NULL, 0,
					     nvme_ctrlr_set_keep_alive_timeout_done, ctrlr);
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "Keep alive timeout Get Feature failed: %d\n", rc);
		ctrlr->opts.keep_alive_timeout_ms = 0;
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return rc;
	}

	return 0;
}

static void
nvme_ctrlr_set_host_id_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = (struct spdk_nvme_ctrlr *)arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		/*
		 * Treat Set Features - Host ID failure as non-fatal, since the Host ID feature
		 * is optional.
		 */
		NVME_CTRLR_WARNLOG(ctrlr, "Set Features - Host ID failed: SC 0x%x SCT 0x%x\n",
				   cpl->status.sc, cpl->status.sct);
	} else {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Set Features - Host ID was successful\n");
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_READY, NVME_TIMEOUT_INFINITE);
}

static int
nvme_ctrlr_set_host_id(struct spdk_nvme_ctrlr *ctrlr)
{
	uint8_t *host_id;
	uint32_t host_id_size;
	int rc;

	if (ctrlr->trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
		/*
		 * NVMe-oF sends the host ID during Connect and doesn't allow
		 * Set Features - Host Identifier after Connect, so we don't need to do anything here.
		 */
		NVME_CTRLR_DEBUGLOG(ctrlr, "NVMe-oF transport - not sending Set Features - Host ID\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_READY, NVME_TIMEOUT_INFINITE);
		return 0;
	}

	if (ctrlr->cdata.ctratt.host_id_exhid_supported) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Using 128-bit extended host identifier\n");
		host_id = ctrlr->opts.extended_host_id;
		host_id_size = sizeof(ctrlr->opts.extended_host_id);
	} else {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Using 64-bit host identifier\n");
		host_id = ctrlr->opts.host_id;
		host_id_size = sizeof(ctrlr->opts.host_id);
	}

	/* If the user specified an all-zeroes host identifier, don't send the command. */
	if (spdk_mem_all_zero(host_id, host_id_size)) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "User did not specify host ID - not sending Set Features - Host ID\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_READY, NVME_TIMEOUT_INFINITE);
		return 0;
	}

	SPDK_LOGDUMP(nvme, "host_id", host_id, host_id_size);

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_HOST_ID,
			     ctrlr->opts.admin_timeout_ms);

	rc = nvme_ctrlr_cmd_set_host_id(ctrlr, host_id, host_id_size, nvme_ctrlr_set_host_id_done, ctrlr);
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "Set Features - Host ID failed: %d\n", rc);
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return rc;
	}

	return 0;
}

void
nvme_ctrlr_update_namespaces(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t nsid;
	struct spdk_nvme_ns *ns;

	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	     nsid != 0; nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		nvme_ns_construct(ns, nsid, ctrlr);
	}
}

static int
nvme_ctrlr_clear_changed_ns_log(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_completion_poll_status	*status;
	int		rc = -ENOMEM;
	char		*buffer = NULL;
	uint32_t	nsid;
	size_t		buf_size = (SPDK_NVME_MAX_CHANGED_NAMESPACES * sizeof(uint32_t));

	if (ctrlr->disable_read_changed_ns_list_log_page) {
		return 0;
	}

	buffer = spdk_dma_zmalloc(buf_size, 4096, NULL);
	if (!buffer) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate buffer for getting "
				  "changed ns log.\n");
		return rc;
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		goto free_buffer;
	}

	rc = spdk_nvme_ctrlr_cmd_get_log_page(ctrlr,
					      SPDK_NVME_LOG_CHANGED_NS_LIST,
					      SPDK_NVME_GLOBAL_NS_TAG,
					      buffer, buf_size, 0,
					      nvme_completion_poll_cb, status);

	if (rc) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_cmd_get_log_page() failed: rc=%d\n", rc);
		free(status);
		goto free_buffer;
	}

	rc = nvme_wait_for_completion_timeout(ctrlr->adminq, status,
					      ctrlr->opts.admin_timeout_ms * 1000);
	if (!status->timed_out) {
		free(status);
	}

	if (rc) {
		NVME_CTRLR_ERRLOG(ctrlr, "wait for spdk_nvme_ctrlr_cmd_get_log_page failed: rc=%d\n", rc);
		goto free_buffer;
	}

	/* only check the case of overflow. */
	nsid = from_le32(buffer);
	if (nsid == 0xffffffffu) {
		NVME_CTRLR_WARNLOG(ctrlr, "changed ns log overflowed.\n");
	}

free_buffer:
	spdk_dma_free(buffer);
	return rc;
}

void
nvme_ctrlr_process_async_event(struct spdk_nvme_ctrlr *ctrlr,
			       const struct spdk_nvme_cpl *cpl)
{
	union spdk_nvme_async_event_completion event;
	struct spdk_nvme_ctrlr_process *active_proc;
	int rc;

	event.raw = cpl->cdw0;

	if ((event.bits.async_event_type == SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE) &&
	    (event.bits.async_event_info == SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGED)) {
		nvme_ctrlr_clear_changed_ns_log(ctrlr);

		rc = nvme_ctrlr_identify_active_ns(ctrlr);
		if (rc) {
			return;
		}
		nvme_ctrlr_update_namespaces(ctrlr);
		nvme_io_msg_ctrlr_update(ctrlr);
	}

	if ((event.bits.async_event_type == SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE) &&
	    (event.bits.async_event_info == SPDK_NVME_ASYNC_EVENT_ANA_CHANGE)) {
		if (!ctrlr->opts.disable_read_ana_log_page) {
			rc = nvme_ctrlr_update_ana_log_page(ctrlr);
			if (rc) {
				return;
			}
			nvme_ctrlr_parse_ana_log_page(ctrlr, nvme_ctrlr_update_ns_ana_states,
						      ctrlr);
		}
	}

	active_proc = nvme_ctrlr_get_current_process(ctrlr);
	if (active_proc && active_proc->aer_cb_fn) {
		active_proc->aer_cb_fn(active_proc->aer_cb_arg, cpl);
	}
}

static void
nvme_ctrlr_queue_async_event(struct spdk_nvme_ctrlr *ctrlr,
			     const struct spdk_nvme_cpl *cpl)
{
	struct  spdk_nvme_ctrlr_aer_completion_list *nvme_event;
	struct spdk_nvme_ctrlr_process *proc;

	/* Add async event to each process objects event list */
	TAILQ_FOREACH(proc, &ctrlr->active_procs, tailq) {
		/* Must be shared memory so other processes can access */
		nvme_event = spdk_zmalloc(sizeof(*nvme_event), 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
		if (!nvme_event) {
			NVME_CTRLR_ERRLOG(ctrlr, "Alloc nvme event failed, ignore the event\n");
			return;
		}
		nvme_event->cpl = *cpl;

		STAILQ_INSERT_TAIL(&proc->async_events, nvme_event, link);
	}
}

void
nvme_ctrlr_complete_queued_async_events(struct spdk_nvme_ctrlr *ctrlr)
{
	struct  spdk_nvme_ctrlr_aer_completion_list  *nvme_event, *nvme_event_tmp;
	struct spdk_nvme_ctrlr_process	*active_proc;

	active_proc = nvme_ctrlr_get_current_process(ctrlr);

	STAILQ_FOREACH_SAFE(nvme_event, &active_proc->async_events, link, nvme_event_tmp) {
		STAILQ_REMOVE(&active_proc->async_events, nvme_event,
			      spdk_nvme_ctrlr_aer_completion_list, link);
		nvme_ctrlr_process_async_event(ctrlr, &nvme_event->cpl);
		spdk_free(nvme_event);

	}
}

static void
nvme_ctrlr_async_event_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_async_event_request	*aer = arg;
	struct spdk_nvme_ctrlr		*ctrlr = aer->ctrlr;

	if (cpl->status.sct == SPDK_NVME_SCT_GENERIC &&
	    cpl->status.sc == SPDK_NVME_SC_ABORTED_SQ_DELETION) {
		/*
		 *  This is simulated when controller is being shut down, to
		 *  effectively abort outstanding asynchronous event requests
		 *  and make sure all memory is freed.  Do not repost the
		 *  request in this case.
		 */
		return;
	}

	if (cpl->status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC &&
	    cpl->status.sc == SPDK_NVME_SC_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED) {
		/*
		 *  SPDK will only send as many AERs as the device says it supports,
		 *  so this status code indicates an out-of-spec device.  Do not repost
		 *  the request in this case.
		 */
		NVME_CTRLR_ERRLOG(ctrlr, "Controller appears out-of-spec for asynchronous event request\n"
				  "handling.  Do not repost this AER.\n");
		return;
	}

	/* Add the events to the list */
	nvme_ctrlr_queue_async_event(ctrlr, cpl);

	/* If the ctrlr was removed or in the destruct state, we should not send aer again */
	if (ctrlr->is_removed || ctrlr->is_destructed) {
		return;
	}

	/*
	 * Repost another asynchronous event request to replace the one
	 *  that just completed.
	 */
	if (nvme_ctrlr_construct_and_submit_aer(ctrlr, aer)) {
		/*
		 * We can't do anything to recover from a failure here,
		 * so just print a warning message and leave the AER unsubmitted.
		 */
		NVME_CTRLR_ERRLOG(ctrlr, "resubmitting AER failed!\n");
	}
}

static int
nvme_ctrlr_construct_and_submit_aer(struct spdk_nvme_ctrlr *ctrlr,
				    struct nvme_async_event_request *aer)
{
	struct nvme_request *req;

	aer->ctrlr = ctrlr;
	req = nvme_allocate_request_null(ctrlr->adminq, nvme_ctrlr_async_event_cb, aer);
	aer->req = req;
	if (req == NULL) {
		return -1;
	}

	req->cmd.opc = SPDK_NVME_OPC_ASYNC_EVENT_REQUEST;
	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

static void
nvme_ctrlr_configure_aer_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_async_event_request		*aer;
	int					rc;
	uint32_t				i;
	struct spdk_nvme_ctrlr *ctrlr =	(struct spdk_nvme_ctrlr *)arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_NOTICELOG(ctrlr, "nvme_ctrlr_configure_aer failed!\n");
		ctrlr->num_aers = 0;
	} else {
		/* aerl is a zero-based value, so we need to add 1 here. */
		ctrlr->num_aers = spdk_min(NVME_MAX_ASYNC_EVENTS, (ctrlr->cdata.aerl + 1));
	}

	for (i = 0; i < ctrlr->num_aers; i++) {
		aer = &ctrlr->aer[i];
		rc = nvme_ctrlr_construct_and_submit_aer(ctrlr, aer);
		if (rc) {
			NVME_CTRLR_ERRLOG(ctrlr, "nvme_ctrlr_construct_and_submit_aer failed!\n");
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
			return;
		}
	}
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_KEEP_ALIVE_TIMEOUT, ctrlr->opts.admin_timeout_ms);
}

static int
nvme_ctrlr_configure_aer(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_feat_async_event_configuration	config;
	int						rc;

	config.raw = 0;

	if (spdk_nvme_ctrlr_is_discovery(ctrlr)) {
		config.bits.discovery_log_change_notice = 1;
	} else {
		config.bits.crit_warn.bits.available_spare = 1;
		config.bits.crit_warn.bits.temperature = 1;
		config.bits.crit_warn.bits.device_reliability = 1;
		config.bits.crit_warn.bits.read_only = 1;
		config.bits.crit_warn.bits.volatile_memory_backup = 1;

		if (ctrlr->vs.raw >= SPDK_NVME_VERSION(1, 2, 0)) {
			if (ctrlr->cdata.oaes.ns_attribute_notices) {
				config.bits.ns_attr_notice = 1;
			}
			if (ctrlr->cdata.oaes.fw_activation_notices) {
				config.bits.fw_activation_notice = 1;
			}
			if (ctrlr->cdata.oaes.ana_change_notices) {
				config.bits.ana_change_notice = 1;
			}
		}
		if (ctrlr->vs.raw >= SPDK_NVME_VERSION(1, 3, 0) && ctrlr->cdata.lpa.telemetry) {
			config.bits.telemetry_log_notice = 1;
		}
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_CONFIGURE_AER,
			     ctrlr->opts.admin_timeout_ms);

	rc = nvme_ctrlr_cmd_set_async_event_config(ctrlr, config,
			nvme_ctrlr_configure_aer_done,
			ctrlr);
	if (rc != 0) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return rc;
	}

	return 0;
}

struct spdk_nvme_ctrlr_process *
nvme_ctrlr_get_process(struct spdk_nvme_ctrlr *ctrlr, pid_t pid)
{
	struct spdk_nvme_ctrlr_process	*active_proc;

	TAILQ_FOREACH(active_proc, &ctrlr->active_procs, tailq) {
		if (active_proc->pid == pid) {
			return active_proc;
		}
	}

	return NULL;
}

struct spdk_nvme_ctrlr_process *
nvme_ctrlr_get_current_process(struct spdk_nvme_ctrlr *ctrlr)
{
	return nvme_ctrlr_get_process(ctrlr, getpid());
}

/**
 * This function will be called when a process is using the controller.
 *  1. For the primary process, it is called when constructing the controller.
 *  2. For the secondary process, it is called at probing the controller.
 * Note: will check whether the process is already added for the same process.
 */
int
nvme_ctrlr_add_process(struct spdk_nvme_ctrlr *ctrlr, void *devhandle)
{
	struct spdk_nvme_ctrlr_process	*ctrlr_proc;
	pid_t				pid = getpid();

	/* Check whether the process is already added or not */
	if (nvme_ctrlr_get_process(ctrlr, pid)) {
		return 0;
	}

	/* Initialize the per process properties for this ctrlr */
	ctrlr_proc = spdk_zmalloc(sizeof(struct spdk_nvme_ctrlr_process),
				  64, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
	if (ctrlr_proc == NULL) {
		NVME_CTRLR_ERRLOG(ctrlr, "failed to allocate memory to track the process props\n");

		return -1;
	}

	ctrlr_proc->is_primary = spdk_process_is_primary();
	ctrlr_proc->pid = pid;
	STAILQ_INIT(&ctrlr_proc->active_reqs);
	ctrlr_proc->devhandle = devhandle;
	ctrlr_proc->ref = 0;
	TAILQ_INIT(&ctrlr_proc->allocated_io_qpairs);
	STAILQ_INIT(&ctrlr_proc->async_events);

	TAILQ_INSERT_TAIL(&ctrlr->active_procs, ctrlr_proc, tailq);

	return 0;
}

/**
 * This function will be called when the process detaches the controller.
 * Note: the ctrlr_lock must be held when calling this function.
 */
static void
nvme_ctrlr_remove_process(struct spdk_nvme_ctrlr *ctrlr,
			  struct spdk_nvme_ctrlr_process *proc)
{
	struct spdk_nvme_qpair	*qpair, *tmp_qpair;

	assert(STAILQ_EMPTY(&proc->active_reqs));

	TAILQ_FOREACH_SAFE(qpair, &proc->allocated_io_qpairs, per_process_tailq, tmp_qpair) {
		spdk_nvme_ctrlr_free_io_qpair(qpair);
	}

	TAILQ_REMOVE(&ctrlr->active_procs, proc, tailq);

	if (ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
		spdk_pci_device_detach(proc->devhandle);
	}

	spdk_free(proc);
}

/**
 * This function will be called when the process exited unexpectedly
 *  in order to free any incomplete nvme request, allocated IO qpairs
 *  and allocated memory.
 * Note: the ctrlr_lock must be held when calling this function.
 */
static void
nvme_ctrlr_cleanup_process(struct spdk_nvme_ctrlr_process *proc)
{
	struct nvme_request	*req, *tmp_req;
	struct spdk_nvme_qpair	*qpair, *tmp_qpair;
	struct spdk_nvme_ctrlr_aer_completion_list *event;

	STAILQ_FOREACH_SAFE(req, &proc->active_reqs, stailq, tmp_req) {
		STAILQ_REMOVE(&proc->active_reqs, req, nvme_request, stailq);

		assert(req->pid == proc->pid);

		nvme_free_request(req);
	}

	/* Remove async event from each process objects event list */
	while (!STAILQ_EMPTY(&proc->async_events)) {
		event = STAILQ_FIRST(&proc->async_events);
		STAILQ_REMOVE_HEAD(&proc->async_events, link);
		spdk_free(event);
	}

	TAILQ_FOREACH_SAFE(qpair, &proc->allocated_io_qpairs, per_process_tailq, tmp_qpair) {
		TAILQ_REMOVE(&proc->allocated_io_qpairs, qpair, per_process_tailq);

		/*
		 * The process may have been killed while some qpairs were in their
		 *  completion context.  Clear that flag here to allow these IO
		 *  qpairs to be deleted.
		 */
		qpair->in_completion_context = 0;

		qpair->no_deletion_notification_needed = 1;

		spdk_nvme_ctrlr_free_io_qpair(qpair);
	}

	spdk_free(proc);
}

/**
 * This function will be called when destructing the controller.
 *  1. There is no more admin request on this controller.
 *  2. Clean up any left resource allocation when its associated process is gone.
 */
void
nvme_ctrlr_free_processes(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ctrlr_process	*active_proc, *tmp;

	/* Free all the processes' properties and make sure no pending admin IOs */
	TAILQ_FOREACH_SAFE(active_proc, &ctrlr->active_procs, tailq, tmp) {
		TAILQ_REMOVE(&ctrlr->active_procs, active_proc, tailq);

		assert(STAILQ_EMPTY(&active_proc->active_reqs));

		spdk_free(active_proc);
	}
}

/**
 * This function will be called when any other process attaches or
 *  detaches the controller in order to cleanup those unexpectedly
 *  terminated processes.
 * Note: the ctrlr_lock must be held when calling this function.
 */
static int
nvme_ctrlr_remove_inactive_proc(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ctrlr_process	*active_proc, *tmp;
	int				active_proc_count = 0;

	TAILQ_FOREACH_SAFE(active_proc, &ctrlr->active_procs, tailq, tmp) {
		if ((kill(active_proc->pid, 0) == -1) && (errno == ESRCH)) {
			NVME_CTRLR_ERRLOG(ctrlr, "process %d terminated unexpected\n", active_proc->pid);

			TAILQ_REMOVE(&ctrlr->active_procs, active_proc, tailq);

			nvme_ctrlr_cleanup_process(active_proc);
		} else {
			active_proc_count++;
		}
	}

	return active_proc_count;
}

void
nvme_ctrlr_proc_get_ref(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ctrlr_process	*active_proc;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);

	nvme_ctrlr_remove_inactive_proc(ctrlr);

	active_proc = nvme_ctrlr_get_current_process(ctrlr);
	if (active_proc) {
		active_proc->ref++;
	}

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
}

void
nvme_ctrlr_proc_put_ref(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ctrlr_process	*active_proc;
	int				proc_count;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);

	proc_count = nvme_ctrlr_remove_inactive_proc(ctrlr);

	active_proc = nvme_ctrlr_get_current_process(ctrlr);
	if (active_proc) {
		active_proc->ref--;
		assert(active_proc->ref >= 0);

		/*
		 * The last active process will be removed at the end of
		 * the destruction of the controller.
		 */
		if (active_proc->ref == 0 && proc_count != 1) {
			nvme_ctrlr_remove_process(ctrlr, active_proc);
		}
	}

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
}

int
nvme_ctrlr_get_ref_count(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ctrlr_process	*active_proc;
	int				ref = 0;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);

	nvme_ctrlr_remove_inactive_proc(ctrlr);

	TAILQ_FOREACH(active_proc, &ctrlr->active_procs, tailq) {
		ref += active_proc->ref;
	}

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return ref;
}

/**
 *  Get the PCI device handle which is only visible to its associated process.
 */
struct spdk_pci_device *
nvme_ctrlr_proc_get_devhandle(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ctrlr_process	*active_proc;
	struct spdk_pci_device		*devhandle = NULL;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);

	active_proc = nvme_ctrlr_get_current_process(ctrlr);
	if (active_proc) {
		devhandle = active_proc->devhandle;
	}

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return devhandle;
}

static void
nvme_ctrlr_process_init_vs_done(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the VS register\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	assert(value <= UINT32_MAX);
	ctrlr->vs.raw = (uint32_t)value;
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_READ_CAP, NVME_TIMEOUT_INFINITE);
}

static void
nvme_ctrlr_process_init_cap_done(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CAP register\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	ctrlr->cap.raw = value;
	nvme_ctrlr_init_cap(ctrlr);
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_CHECK_EN, NVME_TIMEOUT_INFINITE);
}

static void
nvme_ctrlr_process_init_check_en(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;
	enum nvme_ctrlr_state state;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CC register\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	assert(value <= UINT32_MAX);
	ctrlr->process_init_cc.raw = (uint32_t)value;

	if (ctrlr->process_init_cc.bits.en) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "CC.EN = 1\n");
		state = NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1;
	} else {
		state = NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0;
	}

	nvme_ctrlr_set_state(ctrlr, state, nvme_ctrlr_get_ready_timeout(ctrlr));
}

static void
nvme_ctrlr_process_init_set_en_0(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to write the CC register\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	/*
	 * Wait 2.5 seconds before accessing PCI registers.
	 * Not using sleep() to avoid blocking other controller's initialization.
	 */
	if (ctrlr->quirks & NVME_QUIRK_DELAY_BEFORE_CHK_RDY) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Applying quirk: delay 2.5 seconds before reading registers\n");
		ctrlr->sleep_timeout_tsc = spdk_get_ticks() + (2500 * spdk_get_ticks_hz() / 1000);
	}

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0,
			     nvme_ctrlr_get_ready_timeout(ctrlr));
}

static void
nvme_ctrlr_process_init_set_en_0_read_cc(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;
	union spdk_nvme_cc_register cc;
	int rc;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CC register\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		return;
	}

	assert(value <= UINT32_MAX);
	cc.raw = (uint32_t)value;
	cc.bits.en = 0;
	ctrlr->process_init_cc.raw = cc.raw;

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_EN_0_WAIT_FOR_CC,
			     nvme_ctrlr_get_ready_timeout(ctrlr));

	rc = nvme_ctrlr_set_cc_async(ctrlr, cc.raw, nvme_ctrlr_process_init_set_en_0, ctrlr);
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "set_cc() failed\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
	}
}

static void
nvme_ctrlr_process_init_wait_for_ready_1(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;
	union spdk_nvme_csts_register csts;

	if (spdk_nvme_cpl_is_error(cpl)) {
		/* While a device is resetting, it may be unable to service MMIO reads
		 * temporarily. Allow for this case.
		 */
		if (!ctrlr->is_failed && ctrlr->state_timeout_tsc != NVME_TIMEOUT_INFINITE) {
			NVME_CTRLR_DEBUGLOG(ctrlr, "Failed to read the CSTS register\n");
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1,
					     NVME_TIMEOUT_KEEP_EXISTING);
		} else {
			NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CSTS register\n");
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		}

		return;
	}

	assert(value <= UINT32_MAX);
	csts.raw = (uint32_t)value;
	if (csts.bits.rdy == 1 || csts.bits.cfs == 1) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_EN_0,
				     nvme_ctrlr_get_ready_timeout(ctrlr));
	} else {
		NVME_CTRLR_DEBUGLOG(ctrlr, "CC.EN = 1 && CSTS.RDY = 0 - waiting for reset to complete\n");
		nvme_ctrlr_set_state_quiet(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1,
					   NVME_TIMEOUT_KEEP_EXISTING);
	}
}

static void
nvme_ctrlr_process_init_wait_for_ready_0(void *ctx, uint64_t value, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;
	union spdk_nvme_csts_register csts;

	if (spdk_nvme_cpl_is_error(cpl)) {
		/* While a device is resetting, it may be unable to service MMIO reads
		 * temporarily. Allow for this case.
		 */
		if (!ctrlr->is_failed && ctrlr->state_timeout_tsc != NVME_TIMEOUT_INFINITE) {
			NVME_CTRLR_DEBUGLOG(ctrlr, "Failed to read the CSTS register\n");
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0,
					     NVME_TIMEOUT_KEEP_EXISTING);
		} else {
			NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CSTS register\n");
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		}

		return;
	}

	assert(value <= UINT32_MAX);
	csts.raw = (uint32_t)value;
	if (csts.bits.rdy == 0) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "CC.EN = 0 && CSTS.RDY = 0\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ENABLE,
				     nvme_ctrlr_get_ready_timeout(ctrlr));
		/*
		 * Delay 100us before setting CC.EN = 1.  Some NVMe SSDs miss CC.EN getting
		 *  set to 1 if it is too soon after CSTS.RDY is reported as 0.
		 */
		spdk_delay_us(100);
	} else {
		nvme_ctrlr_set_state_quiet(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0,
					   NVME_TIMEOUT_KEEP_EXISTING);
	}
}

static void
nvme_ctrlr_process_init_enable_wait_for_ready_1(void *ctx, uint64_t value,
		const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;
	union spdk_nvme_csts_register csts;

	if (spdk_nvme_cpl_is_error(cpl)) {
		/* While a device is resetting, it may be unable to service MMIO reads
		 * temporarily. Allow for this case.
		 */
		if (!ctrlr->is_failed && ctrlr->state_timeout_tsc != NVME_TIMEOUT_INFINITE) {
			NVME_CTRLR_DEBUGLOG(ctrlr, "Failed to read the CSTS register\n");
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1,
					     NVME_TIMEOUT_KEEP_EXISTING);
		} else {
			NVME_CTRLR_ERRLOG(ctrlr, "Failed to read the CSTS register\n");
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		}

		return;
	}

	assert(value <= UINT32_MAX);
	csts.raw = value;
	if (csts.bits.rdy == 1) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "CC.EN = 1 && CSTS.RDY = 1 - controller is ready\n");
		/*
		 * The controller has been enabled.
		 *  Perform the rest of initialization serially.
		 */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_RESET_ADMIN_QUEUE,
				     ctrlr->opts.admin_timeout_ms);
	} else {
		nvme_ctrlr_set_state_quiet(ctrlr, NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1,
					   NVME_TIMEOUT_KEEP_EXISTING);
	}
}

/**
 * This function will be called repeatedly during initialization until the controller is ready.
 */
int
nvme_ctrlr_process_init(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t ready_timeout_in_ms;
	uint64_t ticks;
	int rc = 0;

	ticks = spdk_get_ticks();

	/*
	 * May need to avoid accessing any register on the target controller
	 * for a while. Return early without touching the FSM.
	 * Check sleep_timeout_tsc > 0 for unit test.
	 */
	if ((ctrlr->sleep_timeout_tsc > 0) &&
	    (ticks <= ctrlr->sleep_timeout_tsc)) {
		return 0;
	}
	ctrlr->sleep_timeout_tsc = 0;

	ready_timeout_in_ms = nvme_ctrlr_get_ready_timeout(ctrlr);

	/*
	 * Check if the current initialization step is done or has timed out.
	 */
	switch (ctrlr->state) {
	case NVME_CTRLR_STATE_INIT_DELAY:
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_INIT, ready_timeout_in_ms);
		if (ctrlr->quirks & NVME_QUIRK_DELAY_BEFORE_INIT) {
			/*
			 * Controller may need some delay before it's enabled.
			 *
			 * This is a workaround for an issue where the PCIe-attached NVMe controller
			 * is not ready after VFIO reset. We delay the initialization rather than the
			 * enabling itself, because this is required only for the very first enabling
			 * - directly after a VFIO reset.
			 */
			NVME_CTRLR_DEBUGLOG(ctrlr, "Adding 2 second delay before initializing the controller\n");
			ctrlr->sleep_timeout_tsc = ticks + (2000 * spdk_get_ticks_hz() / 1000);
		}
		break;

	case NVME_CTRLR_STATE_CONNECT_ADMINQ: /* synonymous with NVME_CTRLR_STATE_INIT */
		rc = nvme_transport_ctrlr_connect_qpair(ctrlr, ctrlr->adminq);
		if (rc == 0) {
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_WAIT_FOR_CONNECT_ADMINQ,
					     NVME_TIMEOUT_INFINITE);
		} else {
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
		}
		break;

	case NVME_CTRLR_STATE_WAIT_FOR_CONNECT_ADMINQ:
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);

		switch (nvme_qpair_get_state(ctrlr->adminq)) {
		case NVME_QPAIR_CONNECTING:
			break;
		case NVME_QPAIR_CONNECTED:
			nvme_qpair_set_state(ctrlr->adminq, NVME_QPAIR_ENABLED);
		/* Fall through */
		case NVME_QPAIR_ENABLED:
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_READ_VS,
					     NVME_TIMEOUT_INFINITE);
			/* Abort any queued requests that were sent while the adminq was connecting
			 * to avoid stalling the init process during a reset, as requests don't get
			 * resubmitted while the controller is resetting and subsequent commands
			 * would get queued too.
			 */
			nvme_qpair_abort_queued_reqs(ctrlr->adminq, 0);
			break;
		default:
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ERROR, NVME_TIMEOUT_INFINITE);
			break;
		}

		break;

	case NVME_CTRLR_STATE_READ_VS:
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_READ_VS_WAIT_FOR_VS, NVME_TIMEOUT_INFINITE);
		rc = nvme_ctrlr_get_vs_async(ctrlr, nvme_ctrlr_process_init_vs_done, ctrlr);
		break;

	case NVME_CTRLR_STATE_READ_CAP:
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_READ_CAP_WAIT_FOR_CAP, NVME_TIMEOUT_INFINITE);
		rc = nvme_ctrlr_get_cap_async(ctrlr, nvme_ctrlr_process_init_cap_done, ctrlr);
		break;

	case NVME_CTRLR_STATE_CHECK_EN:
		/* Begin the hardware initialization by making sure the controller is disabled. */
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_CHECK_EN_WAIT_FOR_CC, ready_timeout_in_ms);
		rc = nvme_ctrlr_get_cc_async(ctrlr, nvme_ctrlr_process_init_check_en, ctrlr);
		break;

	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1:
		/*
		 * Controller is currently enabled. We need to disable it to cause a reset.
		 *
		 * If CC.EN = 1 && CSTS.RDY = 0, the controller is in the process of becoming ready.
		 *  Wait for the ready bit to be 1 before disabling the controller.
		 */
		nvme_ctrlr_set_state_quiet(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1_WAIT_FOR_CSTS,
					   NVME_TIMEOUT_KEEP_EXISTING);
		rc = nvme_ctrlr_get_csts_async(ctrlr, nvme_ctrlr_process_init_wait_for_ready_1, ctrlr);
		break;

	case NVME_CTRLR_STATE_SET_EN_0:
		NVME_CTRLR_DEBUGLOG(ctrlr, "Setting CC.EN = 0\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_EN_0_WAIT_FOR_CC, ready_timeout_in_ms);
		rc = nvme_ctrlr_get_cc_async(ctrlr, nvme_ctrlr_process_init_set_en_0_read_cc, ctrlr);
		break;

	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0:
		nvme_ctrlr_set_state_quiet(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0_WAIT_FOR_CSTS,
					   NVME_TIMEOUT_KEEP_EXISTING);
		rc = nvme_ctrlr_get_csts_async(ctrlr, nvme_ctrlr_process_init_wait_for_ready_0, ctrlr);
		break;

	case NVME_CTRLR_STATE_ENABLE:
		NVME_CTRLR_DEBUGLOG(ctrlr, "Setting CC.EN = 1\n");
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ENABLE_WAIT_FOR_CC, ready_timeout_in_ms);
		rc = nvme_ctrlr_enable(ctrlr);
		return rc;

	case NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1:
		nvme_ctrlr_set_state_quiet(ctrlr, NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1_WAIT_FOR_CSTS,
					   NVME_TIMEOUT_KEEP_EXISTING);
		rc = nvme_ctrlr_get_csts_async(ctrlr, nvme_ctrlr_process_init_enable_wait_for_ready_1,
					       ctrlr);
		break;

	case NVME_CTRLR_STATE_RESET_ADMIN_QUEUE:
		nvme_transport_qpair_reset(ctrlr->adminq);
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_IDENTIFY, NVME_TIMEOUT_INFINITE);
		break;

	case NVME_CTRLR_STATE_IDENTIFY:
		rc = nvme_ctrlr_identify(ctrlr);
		break;

	case NVME_CTRLR_STATE_CONFIGURE_AER:
		rc = nvme_ctrlr_configure_aer(ctrlr);
		break;

	case NVME_CTRLR_STATE_SET_KEEP_ALIVE_TIMEOUT:
		rc = nvme_ctrlr_set_keep_alive_timeout(ctrlr);
		break;

	case NVME_CTRLR_STATE_IDENTIFY_IOCS_SPECIFIC:
		rc = nvme_ctrlr_identify_iocs_specific(ctrlr);
		break;

	case NVME_CTRLR_STATE_GET_ZNS_CMD_EFFECTS_LOG:
		rc = nvme_ctrlr_get_zns_cmd_and_effects_log(ctrlr);
		break;

	case NVME_CTRLR_STATE_SET_NUM_QUEUES:
		nvme_ctrlr_update_nvmf_ioccsz(ctrlr);
		rc = nvme_ctrlr_set_num_queues(ctrlr);
		break;

	case NVME_CTRLR_STATE_IDENTIFY_ACTIVE_NS:
		_nvme_ctrlr_identify_active_ns(ctrlr);
		break;

	case NVME_CTRLR_STATE_IDENTIFY_NS:
		rc = nvme_ctrlr_identify_namespaces(ctrlr);
		break;

	case NVME_CTRLR_STATE_IDENTIFY_ID_DESCS:
		rc = nvme_ctrlr_identify_id_desc_namespaces(ctrlr);
		break;

	case NVME_CTRLR_STATE_IDENTIFY_NS_IOCS_SPECIFIC:
		rc = nvme_ctrlr_identify_namespaces_iocs_specific(ctrlr);
		break;

	case NVME_CTRLR_STATE_SET_SUPPORTED_LOG_PAGES:
		rc = nvme_ctrlr_set_supported_log_pages(ctrlr);
		break;

	case NVME_CTRLR_STATE_SET_SUPPORTED_INTEL_LOG_PAGES:
		rc = nvme_ctrlr_set_intel_support_log_pages(ctrlr);
		break;

	case NVME_CTRLR_STATE_SET_SUPPORTED_FEATURES:
		nvme_ctrlr_set_supported_features(ctrlr);
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_SET_DB_BUF_CFG,
				     ctrlr->opts.admin_timeout_ms);
		break;

	case NVME_CTRLR_STATE_SET_DB_BUF_CFG:
		rc = nvme_ctrlr_set_doorbell_buffer_config(ctrlr);
		break;

	case NVME_CTRLR_STATE_SET_HOST_ID:
		rc = nvme_ctrlr_set_host_id(ctrlr);
		break;

	case NVME_CTRLR_STATE_READY:
		NVME_CTRLR_DEBUGLOG(ctrlr, "Ctrlr already in ready state\n");
		return 0;

	case NVME_CTRLR_STATE_ERROR:
		NVME_CTRLR_ERRLOG(ctrlr, "Ctrlr is in error state\n");
		return -1;

	case NVME_CTRLR_STATE_READ_VS_WAIT_FOR_VS:
	case NVME_CTRLR_STATE_READ_CAP_WAIT_FOR_CAP:
	case NVME_CTRLR_STATE_CHECK_EN_WAIT_FOR_CC:
	case NVME_CTRLR_STATE_SET_EN_0_WAIT_FOR_CC:
	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1_WAIT_FOR_CSTS:
	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0_WAIT_FOR_CSTS:
	case NVME_CTRLR_STATE_ENABLE_WAIT_FOR_CC:
	case NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1_WAIT_FOR_CSTS:
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY:
	case NVME_CTRLR_STATE_WAIT_FOR_CONFIGURE_AER:
	case NVME_CTRLR_STATE_WAIT_FOR_KEEP_ALIVE_TIMEOUT:
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_IOCS_SPECIFIC:
	case NVME_CTRLR_STATE_WAIT_FOR_GET_ZNS_CMD_EFFECTS_LOG:
	case NVME_CTRLR_STATE_WAIT_FOR_SET_NUM_QUEUES:
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_ACTIVE_NS:
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_NS:
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_ID_DESCS:
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY_NS_IOCS_SPECIFIC:
	case NVME_CTRLR_STATE_WAIT_FOR_SUPPORTED_INTEL_LOG_PAGES:
	case NVME_CTRLR_STATE_WAIT_FOR_DB_BUF_CFG:
	case NVME_CTRLR_STATE_WAIT_FOR_HOST_ID:
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
		break;

	default:
		assert(0);
		return -1;
	}

	/* Note: we use the ticks captured when we entered this function.
	 * This covers environments where the SPDK process gets swapped out after
	 * we tried to advance the state but before we check the timeout here.
	 * It is not normal for this to happen, but harmless to handle it in this
	 * way.
	 */
	if (ctrlr->state_timeout_tsc != NVME_TIMEOUT_INFINITE &&
	    ticks > ctrlr->state_timeout_tsc) {
		NVME_CTRLR_ERRLOG(ctrlr, "Initialization timed out in state %d (%s)\n",
				  ctrlr->state, nvme_ctrlr_state_string(ctrlr->state));
		return -1;
	}

	return rc;
}

int
nvme_robust_mutex_init_recursive_shared(pthread_mutex_t *mtx)
{
	pthread_mutexattr_t attr;
	int rc = 0;

	if (pthread_mutexattr_init(&attr)) {
		return -1;
	}
	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) ||
#ifndef __FreeBSD__
	    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) ||
	    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) ||
#endif
	    pthread_mutex_init(mtx, &attr)) {
		rc = -1;
	}
	pthread_mutexattr_destroy(&attr);
	return rc;
}

int
nvme_ctrlr_construct(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	if (ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_INIT_DELAY, NVME_TIMEOUT_INFINITE);
	} else {
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_INIT, NVME_TIMEOUT_INFINITE);
	}

	if (ctrlr->opts.admin_queue_size > SPDK_NVME_ADMIN_QUEUE_MAX_ENTRIES) {
		NVME_CTRLR_ERRLOG(ctrlr, "admin_queue_size %u exceeds max defined by NVMe spec, use max value\n",
				  ctrlr->opts.admin_queue_size);
		ctrlr->opts.admin_queue_size = SPDK_NVME_ADMIN_QUEUE_MAX_ENTRIES;
	}

	if (ctrlr->opts.admin_queue_size < SPDK_NVME_ADMIN_QUEUE_MIN_ENTRIES) {
		NVME_CTRLR_ERRLOG(ctrlr,
				  "admin_queue_size %u is less than minimum defined by NVMe spec, use min value\n",
				  ctrlr->opts.admin_queue_size);
		ctrlr->opts.admin_queue_size = SPDK_NVME_ADMIN_QUEUE_MIN_ENTRIES;
	}

	ctrlr->flags = 0;
	ctrlr->free_io_qids = NULL;
	ctrlr->is_resetting = false;
	ctrlr->is_failed = false;
	ctrlr->is_destructed = false;

	TAILQ_INIT(&ctrlr->active_io_qpairs);
	STAILQ_INIT(&ctrlr->queued_aborts);
	ctrlr->outstanding_aborts = 0;

	ctrlr->ana_log_page = NULL;
	ctrlr->ana_log_page_size = 0;

	rc = nvme_robust_mutex_init_recursive_shared(&ctrlr->ctrlr_lock);
	if (rc != 0) {
		return rc;
	}

	TAILQ_INIT(&ctrlr->active_procs);
	STAILQ_INIT(&ctrlr->register_operations);

	RB_INIT(&ctrlr->ns);

	return rc;
}

static void
nvme_ctrlr_init_cap(struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr->cap.bits.ams & SPDK_NVME_CAP_AMS_WRR) {
		ctrlr->flags |= SPDK_NVME_CTRLR_WRR_SUPPORTED;
	}

	ctrlr->min_page_size = 1u << (12 + ctrlr->cap.bits.mpsmin);

	/* For now, always select page_size == min_page_size. */
	ctrlr->page_size = ctrlr->min_page_size;

	ctrlr->opts.io_queue_size = spdk_max(ctrlr->opts.io_queue_size, SPDK_NVME_IO_QUEUE_MIN_ENTRIES);
	ctrlr->opts.io_queue_size = spdk_min(ctrlr->opts.io_queue_size, MAX_IO_QUEUE_ENTRIES);
	if (ctrlr->quirks & NVME_QUIRK_MINIMUM_IO_QUEUE_SIZE &&
	    ctrlr->opts.io_queue_size == DEFAULT_IO_QUEUE_SIZE) {
		/* If the user specifically set an IO queue size different than the
		 * default, use that value.  Otherwise overwrite with the quirked value.
		 * This allows this quirk to be overridden when necessary.
		 * However, cap.mqes still needs to be respected.
		 */
		ctrlr->opts.io_queue_size = DEFAULT_IO_QUEUE_SIZE_FOR_QUIRK;
	}
	ctrlr->opts.io_queue_size = spdk_min(ctrlr->opts.io_queue_size, ctrlr->cap.bits.mqes + 1u);

	ctrlr->opts.io_queue_requests = spdk_max(ctrlr->opts.io_queue_requests, ctrlr->opts.io_queue_size);
}

void
nvme_ctrlr_destruct_finish(struct spdk_nvme_ctrlr *ctrlr)
{
	pthread_mutex_destroy(&ctrlr->ctrlr_lock);
}

void
nvme_ctrlr_destruct_async(struct spdk_nvme_ctrlr *ctrlr,
			  struct nvme_ctrlr_detach_ctx *ctx)
{
	struct spdk_nvme_qpair *qpair, *tmp;

	NVME_CTRLR_DEBUGLOG(ctrlr, "Prepare to destruct SSD\n");

	ctrlr->is_destructed = true;

	spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);

	nvme_ctrlr_abort_queued_aborts(ctrlr);
	nvme_transport_admin_qpair_abort_aers(ctrlr->adminq);

	TAILQ_FOREACH_SAFE(qpair, &ctrlr->active_io_qpairs, tailq, tmp) {
		spdk_nvme_ctrlr_free_io_qpair(qpair);
	}

	nvme_ctrlr_free_doorbell_buffer(ctrlr);
	nvme_ctrlr_free_iocs_specific_data(ctrlr);

	nvme_ctrlr_shutdown_async(ctrlr, ctx);
}

int
nvme_ctrlr_destruct_poll_async(struct spdk_nvme_ctrlr *ctrlr,
			       struct nvme_ctrlr_detach_ctx *ctx)
{
	struct spdk_nvme_ns *ns, *tmp_ns;
	int rc = 0;

	if (!ctx->shutdown_complete) {
		rc = nvme_ctrlr_shutdown_poll_async(ctrlr, ctx);
		if (rc == -EAGAIN) {
			return -EAGAIN;
		}
		/* Destruct ctrlr forcefully for any other error. */
	}

	if (ctx->cb_fn) {
		ctx->cb_fn(ctrlr);
	}

	nvme_transport_ctrlr_disconnect_qpair(ctrlr, ctrlr->adminq);

	RB_FOREACH_SAFE(ns, nvme_ns_tree, &ctrlr->ns, tmp_ns) {
		nvme_ctrlr_destruct_namespace(ctrlr, ns->id);
		RB_REMOVE(nvme_ns_tree, &ctrlr->ns, ns);
		spdk_free(ns);
	}

	ctrlr->active_ns_count = 0;

	spdk_bit_array_free(&ctrlr->free_io_qids);

	free(ctrlr->ana_log_page);
	free(ctrlr->copied_ana_desc);
	ctrlr->ana_log_page = NULL;
	ctrlr->copied_ana_desc = NULL;
	ctrlr->ana_log_page_size = 0;

	nvme_transport_ctrlr_destruct(ctrlr);

	return rc;
}

void
nvme_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_ctrlr_detach_ctx ctx = { .ctrlr = ctrlr };
	int rc;

	nvme_ctrlr_destruct_async(ctrlr, &ctx);

	while (1) {
		rc = nvme_ctrlr_destruct_poll_async(ctrlr, &ctx);
		if (rc != -EAGAIN) {
			break;
		}
		nvme_delay(1000);
	}
}

int
nvme_ctrlr_submit_admin_request(struct spdk_nvme_ctrlr *ctrlr,
				struct nvme_request *req)
{
	return nvme_qpair_submit_request(ctrlr->adminq, req);
}

static void
nvme_keep_alive_completion(void *cb_ctx, const struct spdk_nvme_cpl *cpl)
{
	/* Do nothing */
}

/*
 * Check if we need to send a Keep Alive command.
 * Caller must hold ctrlr->ctrlr_lock.
 */
static int
nvme_ctrlr_keep_alive(struct spdk_nvme_ctrlr *ctrlr)
{
	uint64_t now;
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;
	int rc = 0;

	now = spdk_get_ticks();
	if (now < ctrlr->next_keep_alive_tick) {
		return rc;
	}

	req = nvme_allocate_request_null(ctrlr->adminq, nvme_keep_alive_completion, NULL);
	if (req == NULL) {
		return rc;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_KEEP_ALIVE;

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "Submitting Keep Alive failed\n");
		rc = -ENXIO;
	}

	ctrlr->next_keep_alive_tick = now + ctrlr->keep_alive_interval_ticks;
	return rc;
}

int32_t
spdk_nvme_ctrlr_process_admin_completions(struct spdk_nvme_ctrlr *ctrlr)
{
	int32_t num_completions;
	int32_t rc;
	struct spdk_nvme_ctrlr_process	*active_proc;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);

	if (ctrlr->keep_alive_interval_ticks) {
		rc = nvme_ctrlr_keep_alive(ctrlr);
		if (rc) {
			nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
			return rc;
		}
	}

	rc = nvme_io_msg_process(ctrlr);
	if (rc < 0) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return rc;
	}
	num_completions = rc;

	rc = spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);

	/* Each process has an async list, complete the ones for this process object */
	active_proc = nvme_ctrlr_get_current_process(ctrlr);
	if (active_proc) {
		nvme_ctrlr_complete_queued_async_events(ctrlr);
	}

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	if (rc < 0) {
		num_completions = rc;
	} else {
		num_completions += rc;
	}

	return num_completions;
}

const struct spdk_nvme_ctrlr_data *
spdk_nvme_ctrlr_get_data(struct spdk_nvme_ctrlr *ctrlr)
{
	return &ctrlr->cdata;
}

union spdk_nvme_csts_register spdk_nvme_ctrlr_get_regs_csts(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_csts_register csts;

	if (nvme_ctrlr_get_csts(ctrlr, &csts)) {
		csts.raw = SPDK_NVME_INVALID_REGISTER_VALUE;
	}
	return csts;
}

union spdk_nvme_cc_register spdk_nvme_ctrlr_get_regs_cc(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_cc_register cc;

	if (nvme_ctrlr_get_cc(ctrlr, &cc)) {
		cc.raw = SPDK_NVME_INVALID_REGISTER_VALUE;
	}
	return cc;
}

union spdk_nvme_cap_register spdk_nvme_ctrlr_get_regs_cap(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->cap;
}

union spdk_nvme_vs_register spdk_nvme_ctrlr_get_regs_vs(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->vs;
}

union spdk_nvme_cmbsz_register spdk_nvme_ctrlr_get_regs_cmbsz(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_cmbsz_register cmbsz;

	if (nvme_ctrlr_get_cmbsz(ctrlr, &cmbsz)) {
		cmbsz.raw = 0;
	}

	return cmbsz;
}

union spdk_nvme_pmrcap_register spdk_nvme_ctrlr_get_regs_pmrcap(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_pmrcap_register pmrcap;

	if (nvme_ctrlr_get_pmrcap(ctrlr, &pmrcap)) {
		pmrcap.raw = 0;
	}

	return pmrcap;
}

union spdk_nvme_bpinfo_register spdk_nvme_ctrlr_get_regs_bpinfo(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_bpinfo_register bpinfo;

	if (nvme_ctrlr_get_bpinfo(ctrlr, &bpinfo)) {
		bpinfo.raw = 0;
	}

	return bpinfo;
}

uint64_t
spdk_nvme_ctrlr_get_pmrsz(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->pmr_size;
}

uint32_t
spdk_nvme_ctrlr_get_num_ns(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->cdata.nn;
}

bool
spdk_nvme_ctrlr_is_active_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct spdk_nvme_ns tmp, *ns;

	tmp.id = nsid;
	ns = RB_FIND(nvme_ns_tree, &ctrlr->ns, &tmp);

	if (ns != NULL) {
		return ns->active;
	}

	return false;
}

uint32_t
spdk_nvme_ctrlr_get_first_active_ns(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ns *ns;

	ns = RB_MIN(nvme_ns_tree, &ctrlr->ns);
	if (ns == NULL) {
		return 0;
	}

	while (ns != NULL) {
		if (ns->active) {
			return ns->id;
		}

		ns = RB_NEXT(nvme_ns_tree, &ctrlr->ns, ns);
	}

	return 0;
}

uint32_t
spdk_nvme_ctrlr_get_next_active_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t prev_nsid)
{
	struct spdk_nvme_ns tmp, *ns;

	tmp.id = prev_nsid;
	ns = RB_FIND(nvme_ns_tree, &ctrlr->ns, &tmp);
	if (ns == NULL) {
		return 0;
	}

	ns = RB_NEXT(nvme_ns_tree, &ctrlr->ns, ns);
	while (ns != NULL) {
		if (ns->active) {
			return ns->id;
		}

		ns = RB_NEXT(nvme_ns_tree, &ctrlr->ns, ns);
	}

	return 0;
}

struct spdk_nvme_ns *
spdk_nvme_ctrlr_get_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct spdk_nvme_ns tmp;
	struct spdk_nvme_ns *ns;

	if (nsid < 1 || nsid > ctrlr->cdata.nn) {
		return NULL;
	}

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);

	tmp.id = nsid;
	ns = RB_FIND(nvme_ns_tree, &ctrlr->ns, &tmp);

	if (ns == NULL) {
		ns = spdk_zmalloc(sizeof(struct spdk_nvme_ns), 64, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
		if (ns == NULL) {
			nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
			return NULL;
		}

		NVME_CTRLR_DEBUGLOG(ctrlr, "Namespace %u was added\n", nsid);
		ns->id = nsid;
		RB_INSERT(nvme_ns_tree, &ctrlr->ns, ns);
	}

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return ns;
}

struct spdk_pci_device *
spdk_nvme_ctrlr_get_pci_device(struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr == NULL) {
		return NULL;
	}

	if (ctrlr->trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
		return NULL;
	}

	return nvme_ctrlr_proc_get_devhandle(ctrlr);
}

uint32_t
spdk_nvme_ctrlr_get_max_xfer_size(const struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->max_xfer_size;
}

void
spdk_nvme_ctrlr_register_aer_callback(struct spdk_nvme_ctrlr *ctrlr,
				      spdk_nvme_aer_cb aer_cb_fn,
				      void *aer_cb_arg)
{
	struct spdk_nvme_ctrlr_process *active_proc;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);

	active_proc = nvme_ctrlr_get_current_process(ctrlr);
	if (active_proc) {
		active_proc->aer_cb_fn = aer_cb_fn;
		active_proc->aer_cb_arg = aer_cb_arg;
	}

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
}

void
spdk_nvme_ctrlr_disable_read_changed_ns_list_log_page(struct spdk_nvme_ctrlr *ctrlr)
{
	ctrlr->disable_read_changed_ns_list_log_page = true;
}

void
spdk_nvme_ctrlr_register_timeout_callback(struct spdk_nvme_ctrlr *ctrlr,
		uint64_t timeout_io_us, uint64_t timeout_admin_us,
		spdk_nvme_timeout_cb cb_fn, void *cb_arg)
{
	struct spdk_nvme_ctrlr_process	*active_proc;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);

	active_proc = nvme_ctrlr_get_current_process(ctrlr);
	if (active_proc) {
		active_proc->timeout_io_ticks = timeout_io_us * spdk_get_ticks_hz() / 1000000ULL;
		active_proc->timeout_admin_ticks = timeout_admin_us * spdk_get_ticks_hz() / 1000000ULL;
		active_proc->timeout_cb_fn = cb_fn;
		active_proc->timeout_cb_arg = cb_arg;
	}

	ctrlr->timeout_enabled = true;

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
}

bool
spdk_nvme_ctrlr_is_log_page_supported(struct spdk_nvme_ctrlr *ctrlr, uint8_t log_page)
{
	/* No bounds check necessary, since log_page is uint8_t and log_page_supported has 256 entries */
	SPDK_STATIC_ASSERT(sizeof(ctrlr->log_page_supported) == 256, "log_page_supported size mismatch");
	return ctrlr->log_page_supported[log_page];
}

bool
spdk_nvme_ctrlr_is_feature_supported(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature_code)
{
	/* No bounds check necessary, since feature_code is uint8_t and feature_supported has 256 entries */
	SPDK_STATIC_ASSERT(sizeof(ctrlr->feature_supported) == 256, "feature_supported size mismatch");
	return ctrlr->feature_supported[feature_code];
}

int
spdk_nvme_ctrlr_attach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			  struct spdk_nvme_ctrlr_list *payload)
{
	struct nvme_completion_poll_status	*status;
	struct spdk_nvme_ns			*ns;
	int					res;

	if (nsid == 0) {
		return -EINVAL;
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	res = nvme_ctrlr_cmd_attach_ns(ctrlr, nsid, payload,
				       nvme_completion_poll_cb, status);
	if (res) {
		free(status);
		return res;
	}
	if (nvme_wait_for_completion_robust_lock(ctrlr->adminq, status, &ctrlr->ctrlr_lock)) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_attach_ns failed!\n");
		if (!status->timed_out) {
			free(status);
		}
		return -ENXIO;
	}
	free(status);

	res = nvme_ctrlr_identify_active_ns(ctrlr);
	if (res) {
		return res;
	}

	ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	return nvme_ns_construct(ns, nsid, ctrlr);
}

int
spdk_nvme_ctrlr_detach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			  struct spdk_nvme_ctrlr_list *payload)
{
	struct nvme_completion_poll_status	*status;
	int					res;

	if (nsid == 0) {
		return -EINVAL;
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	res = nvme_ctrlr_cmd_detach_ns(ctrlr, nsid, payload,
				       nvme_completion_poll_cb, status);
	if (res) {
		free(status);
		return res;
	}
	if (nvme_wait_for_completion_robust_lock(ctrlr->adminq, status, &ctrlr->ctrlr_lock)) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_detach_ns failed!\n");
		if (!status->timed_out) {
			free(status);
		}
		return -ENXIO;
	}
	free(status);

	return nvme_ctrlr_identify_active_ns(ctrlr);
}

uint32_t
spdk_nvme_ctrlr_create_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns_data *payload)
{
	struct nvme_completion_poll_status	*status;
	int					res;
	uint32_t				nsid;

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return 0;
	}

	res = nvme_ctrlr_cmd_create_ns(ctrlr, payload, nvme_completion_poll_cb, status);
	if (res) {
		free(status);
		return 0;
	}
	if (nvme_wait_for_completion_robust_lock(ctrlr->adminq, status, &ctrlr->ctrlr_lock)) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_create_ns failed!\n");
		if (!status->timed_out) {
			free(status);
		}
		return 0;
	}

	nsid = status->cpl.cdw0;
	free(status);

	assert(nsid > 0);

	/* Return the namespace ID that was created */
	return nsid;
}

int
spdk_nvme_ctrlr_delete_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct nvme_completion_poll_status	*status;
	int					res;

	if (nsid == 0) {
		return -EINVAL;
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	res = nvme_ctrlr_cmd_delete_ns(ctrlr, nsid, nvme_completion_poll_cb, status);
	if (res) {
		free(status);
		return res;
	}
	if (nvme_wait_for_completion_robust_lock(ctrlr->adminq, status, &ctrlr->ctrlr_lock)) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_delete_ns failed!\n");
		if (!status->timed_out) {
			free(status);
		}
		return -ENXIO;
	}
	free(status);

	return nvme_ctrlr_identify_active_ns(ctrlr);
}

int
spdk_nvme_ctrlr_format(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
		       struct spdk_nvme_format *format)
{
	struct nvme_completion_poll_status	*status;
	int					res;

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	res = nvme_ctrlr_cmd_format(ctrlr, nsid, format, nvme_completion_poll_cb,
				    status);
	if (res) {
		free(status);
		return res;
	}
	if (nvme_wait_for_completion_robust_lock(ctrlr->adminq, status, &ctrlr->ctrlr_lock)) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_format failed!\n");
		if (!status->timed_out) {
			free(status);
		}
		return -ENXIO;
	}
	free(status);

	return spdk_nvme_ctrlr_reset(ctrlr);
}

int
spdk_nvme_ctrlr_update_firmware(struct spdk_nvme_ctrlr *ctrlr, void *payload, uint32_t size,
				int slot, enum spdk_nvme_fw_commit_action commit_action, struct spdk_nvme_status *completion_status)
{
	struct spdk_nvme_fw_commit		fw_commit;
	struct nvme_completion_poll_status	*status;
	int					res;
	unsigned int				size_remaining;
	unsigned int				offset;
	unsigned int				transfer;
	void					*p;

	if (!completion_status) {
		return -EINVAL;
	}
	memset(completion_status, 0, sizeof(struct spdk_nvme_status));
	if (size % 4) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_update_firmware invalid size!\n");
		return -1;
	}

	/* Current support only for SPDK_NVME_FW_COMMIT_REPLACE_IMG
	 * and SPDK_NVME_FW_COMMIT_REPLACE_AND_ENABLE_IMG
	 */
	if ((commit_action != SPDK_NVME_FW_COMMIT_REPLACE_IMG) &&
	    (commit_action != SPDK_NVME_FW_COMMIT_REPLACE_AND_ENABLE_IMG)) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_update_firmware invalid command!\n");
		return -1;
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	/* Firmware download */
	size_remaining = size;
	offset = 0;
	p = payload;

	while (size_remaining > 0) {
		transfer = spdk_min(size_remaining, ctrlr->min_page_size);

		memset(status, 0, sizeof(*status));
		res = nvme_ctrlr_cmd_fw_image_download(ctrlr, transfer, offset, p,
						       nvme_completion_poll_cb,
						       status);
		if (res) {
			free(status);
			return res;
		}

		if (nvme_wait_for_completion_robust_lock(ctrlr->adminq, status, &ctrlr->ctrlr_lock)) {
			NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_fw_image_download failed!\n");
			if (!status->timed_out) {
				free(status);
			}
			return -ENXIO;
		}
		p += transfer;
		offset += transfer;
		size_remaining -= transfer;
	}

	/* Firmware commit */
	memset(&fw_commit, 0, sizeof(struct spdk_nvme_fw_commit));
	fw_commit.fs = slot;
	fw_commit.ca = commit_action;

	memset(status, 0, sizeof(*status));
	res = nvme_ctrlr_cmd_fw_commit(ctrlr, &fw_commit, nvme_completion_poll_cb,
				       status);
	if (res) {
		free(status);
		return res;
	}

	res = nvme_wait_for_completion_robust_lock(ctrlr->adminq, status, &ctrlr->ctrlr_lock);

	memcpy(completion_status, &status->cpl.status, sizeof(struct spdk_nvme_status));

	if (!status->timed_out) {
		free(status);
	}

	if (res) {
		if (completion_status->sct != SPDK_NVME_SCT_COMMAND_SPECIFIC ||
		    completion_status->sc != SPDK_NVME_SC_FIRMWARE_REQ_NVM_RESET) {
			if (completion_status->sct == SPDK_NVME_SCT_COMMAND_SPECIFIC  &&
			    completion_status->sc == SPDK_NVME_SC_FIRMWARE_REQ_CONVENTIONAL_RESET) {
				NVME_CTRLR_NOTICELOG(ctrlr,
						     "firmware activation requires conventional reset to be performed. !\n");
			} else {
				NVME_CTRLR_ERRLOG(ctrlr, "nvme_ctrlr_cmd_fw_commit failed!\n");
			}
			return -ENXIO;
		}
	}

	return spdk_nvme_ctrlr_reset(ctrlr);
}

int
spdk_nvme_ctrlr_reserve_cmb(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc, size;
	union spdk_nvme_cmbsz_register cmbsz;

	cmbsz = spdk_nvme_ctrlr_get_regs_cmbsz(ctrlr);

	if (cmbsz.bits.rds == 0 || cmbsz.bits.wds == 0) {
		return -ENOTSUP;
	}

	size = cmbsz.bits.sz * (0x1000 << (cmbsz.bits.szu * 4));

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	rc = nvme_transport_ctrlr_reserve_cmb(ctrlr);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	if (rc < 0) {
		return rc;
	}

	return size;
}

void *
spdk_nvme_ctrlr_map_cmb(struct spdk_nvme_ctrlr *ctrlr, size_t *size)
{
	void *buf;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	buf = nvme_transport_ctrlr_map_cmb(ctrlr, size);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return buf;
}

void
spdk_nvme_ctrlr_unmap_cmb(struct spdk_nvme_ctrlr *ctrlr)
{
	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	nvme_transport_ctrlr_unmap_cmb(ctrlr);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
}

int
spdk_nvme_ctrlr_enable_pmr(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	rc = nvme_transport_ctrlr_enable_pmr(ctrlr);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return rc;
}

int
spdk_nvme_ctrlr_disable_pmr(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	rc = nvme_transport_ctrlr_disable_pmr(ctrlr);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return rc;
}

void *
spdk_nvme_ctrlr_map_pmr(struct spdk_nvme_ctrlr *ctrlr, size_t *size)
{
	void *buf;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	buf = nvme_transport_ctrlr_map_pmr(ctrlr, size);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return buf;
}

int
spdk_nvme_ctrlr_unmap_pmr(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	rc = nvme_transport_ctrlr_unmap_pmr(ctrlr);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return rc;
}

int spdk_nvme_ctrlr_read_boot_partition_start(struct spdk_nvme_ctrlr *ctrlr, void *payload,
		uint32_t bprsz, uint32_t bprof, uint32_t bpid)
{
	union spdk_nvme_bprsel_register bprsel;
	union spdk_nvme_bpinfo_register bpinfo;
	uint64_t bpmbl, bpmb_size;

	if (ctrlr->cap.bits.bps == 0) {
		return -ENOTSUP;
	}

	if (nvme_ctrlr_get_bpinfo(ctrlr, &bpinfo)) {
		NVME_CTRLR_ERRLOG(ctrlr, "get bpinfo failed\n");
		return -EIO;
	}

	if (bpinfo.bits.brs == SPDK_NVME_BRS_READ_IN_PROGRESS) {
		NVME_CTRLR_ERRLOG(ctrlr, "Boot Partition read already initiated\n");
		return -EALREADY;
	}

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);

	bpmb_size = bprsz * 4096;
	bpmbl = spdk_vtophys(payload, &bpmb_size);
	if (bpmbl == SPDK_VTOPHYS_ERROR) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_vtophys of bpmbl failed\n");
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -EFAULT;
	}

	if (bpmb_size != bprsz * 4096) {
		NVME_CTRLR_ERRLOG(ctrlr, "Boot Partition buffer is not physically contiguous\n");
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -EFAULT;
	}

	if (nvme_ctrlr_set_bpmbl(ctrlr, bpmbl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "set_bpmbl() failed\n");
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -EIO;
	}

	bprsel.bits.bpid = bpid;
	bprsel.bits.bprof = bprof;
	bprsel.bits.bprsz = bprsz;

	if (nvme_ctrlr_set_bprsel(ctrlr, &bprsel)) {
		NVME_CTRLR_ERRLOG(ctrlr, "set_bprsel() failed\n");
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -EIO;
	}

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	return 0;
}

int spdk_nvme_ctrlr_read_boot_partition_poll(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc = 0;
	union spdk_nvme_bpinfo_register bpinfo;

	if (nvme_ctrlr_get_bpinfo(ctrlr, &bpinfo)) {
		NVME_CTRLR_ERRLOG(ctrlr, "get bpinfo failed\n");
		return -EIO;
	}

	switch (bpinfo.bits.brs) {
	case SPDK_NVME_BRS_NO_READ:
		NVME_CTRLR_ERRLOG(ctrlr, "Boot Partition read not initiated\n");
		rc = -EINVAL;
		break;
	case SPDK_NVME_BRS_READ_IN_PROGRESS:
		NVME_CTRLR_DEBUGLOG(ctrlr, "Boot Partition read in progress\n");
		rc = -EAGAIN;
		break;
	case SPDK_NVME_BRS_READ_ERROR:
		NVME_CTRLR_ERRLOG(ctrlr, "Error completing Boot Partition read\n");
		rc = -EIO;
		break;
	case SPDK_NVME_BRS_READ_SUCCESS:
		NVME_CTRLR_INFOLOG(ctrlr, "Boot Partition read completed successfully\n");
		break;
	default:
		NVME_CTRLR_ERRLOG(ctrlr, "Invalid Boot Partition read status\n");
		rc = -EINVAL;
	}

	return rc;
}

static void
nvme_write_boot_partition_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	int res;
	struct spdk_nvme_ctrlr *ctrlr = arg;
	struct spdk_nvme_fw_commit fw_commit;
	struct spdk_nvme_cpl err_cpl =
	{.status = {.sct = SPDK_NVME_SCT_GENERIC, .sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR }};

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Write Boot Partition failed\n");
		ctrlr->bp_write_cb_fn(ctrlr->bp_write_cb_arg, cpl);
		return;
	}

	if (ctrlr->bp_ws == SPDK_NVME_BP_WS_DOWNLOADING) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Boot Partition Downloading at Offset %d Success\n", ctrlr->fw_offset);
		ctrlr->fw_payload += ctrlr->fw_transfer_size;
		ctrlr->fw_offset += ctrlr->fw_transfer_size;
		ctrlr->fw_size_remaining -= ctrlr->fw_transfer_size;
		ctrlr->fw_transfer_size = spdk_min(ctrlr->fw_size_remaining, ctrlr->min_page_size);
		res = nvme_ctrlr_cmd_fw_image_download(ctrlr, ctrlr->fw_transfer_size, ctrlr->fw_offset,
						       ctrlr->fw_payload, nvme_write_boot_partition_cb, ctrlr);
		if (res) {
			NVME_CTRLR_ERRLOG(ctrlr, "nvme_ctrlr_cmd_fw_image_download failed!\n");
			ctrlr->bp_write_cb_fn(ctrlr->bp_write_cb_arg, &err_cpl);
			return;
		}

		if (ctrlr->fw_transfer_size < ctrlr->min_page_size) {
			ctrlr->bp_ws = SPDK_NVME_BP_WS_DOWNLOADED;
		}
	} else if (ctrlr->bp_ws == SPDK_NVME_BP_WS_DOWNLOADED) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Boot Partition Download Success\n");
		memset(&fw_commit, 0, sizeof(struct spdk_nvme_fw_commit));
		fw_commit.bpid = ctrlr->bpid;
		fw_commit.ca = SPDK_NVME_FW_COMMIT_REPLACE_BOOT_PARTITION;
		res = nvme_ctrlr_cmd_fw_commit(ctrlr, &fw_commit,
					       nvme_write_boot_partition_cb, ctrlr);
		if (res) {
			NVME_CTRLR_ERRLOG(ctrlr, "nvme_ctrlr_cmd_fw_commit failed!\n");
			NVME_CTRLR_ERRLOG(ctrlr, "commit action: %d\n", fw_commit.ca);
			ctrlr->bp_write_cb_fn(ctrlr->bp_write_cb_arg, &err_cpl);
			return;
		}

		ctrlr->bp_ws = SPDK_NVME_BP_WS_REPLACE;
	} else if (ctrlr->bp_ws == SPDK_NVME_BP_WS_REPLACE) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Boot Partition Replacement Success\n");
		memset(&fw_commit, 0, sizeof(struct spdk_nvme_fw_commit));
		fw_commit.bpid = ctrlr->bpid;
		fw_commit.ca = SPDK_NVME_FW_COMMIT_ACTIVATE_BOOT_PARTITION;
		res = nvme_ctrlr_cmd_fw_commit(ctrlr, &fw_commit,
					       nvme_write_boot_partition_cb, ctrlr);
		if (res) {
			NVME_CTRLR_ERRLOG(ctrlr, "nvme_ctrlr_cmd_fw_commit failed!\n");
			NVME_CTRLR_ERRLOG(ctrlr, "commit action: %d\n", fw_commit.ca);
			ctrlr->bp_write_cb_fn(ctrlr->bp_write_cb_arg, &err_cpl);
			return;
		}

		ctrlr->bp_ws = SPDK_NVME_BP_WS_ACTIVATE;
	} else if (ctrlr->bp_ws == SPDK_NVME_BP_WS_ACTIVATE) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Boot Partition Activation Success\n");
		ctrlr->bp_write_cb_fn(ctrlr->bp_write_cb_arg, cpl);
	} else {
		NVME_CTRLR_ERRLOG(ctrlr, "Invalid Boot Partition write state\n");
		ctrlr->bp_write_cb_fn(ctrlr->bp_write_cb_arg, &err_cpl);
		return;
	}
}

int spdk_nvme_ctrlr_write_boot_partition(struct spdk_nvme_ctrlr *ctrlr,
		void *payload, uint32_t size, uint32_t bpid,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	int res;

	if (ctrlr->cap.bits.bps == 0) {
		return -ENOTSUP;
	}

	ctrlr->bp_ws = SPDK_NVME_BP_WS_DOWNLOADING;
	ctrlr->bpid = bpid;
	ctrlr->bp_write_cb_fn = cb_fn;
	ctrlr->bp_write_cb_arg = cb_arg;
	ctrlr->fw_offset = 0;
	ctrlr->fw_size_remaining = size;
	ctrlr->fw_payload = payload;
	ctrlr->fw_transfer_size = spdk_min(ctrlr->fw_size_remaining, ctrlr->min_page_size);

	res = nvme_ctrlr_cmd_fw_image_download(ctrlr, ctrlr->fw_transfer_size, ctrlr->fw_offset,
					       ctrlr->fw_payload, nvme_write_boot_partition_cb, ctrlr);

	return res;
}

bool
spdk_nvme_ctrlr_is_discovery(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr);

	return !strncmp(ctrlr->trid.subnqn, SPDK_NVMF_DISCOVERY_NQN,
			strlen(SPDK_NVMF_DISCOVERY_NQN));
}

bool
spdk_nvme_ctrlr_is_fabrics(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr);

	return spdk_nvme_trtype_is_fabrics(ctrlr->trid.trtype);
}

int
spdk_nvme_ctrlr_security_receive(struct spdk_nvme_ctrlr *ctrlr, uint8_t secp,
				 uint16_t spsp, uint8_t nssf, void *payload, size_t size)
{
	struct nvme_completion_poll_status	*status;
	int					res;

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	res = spdk_nvme_ctrlr_cmd_security_receive(ctrlr, secp, spsp, nssf, payload, size,
			nvme_completion_poll_cb, status);
	if (res) {
		free(status);
		return res;
	}
	if (nvme_wait_for_completion_robust_lock(ctrlr->adminq, status, &ctrlr->ctrlr_lock)) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_cmd_security_receive failed!\n");
		if (!status->timed_out) {
			free(status);
		}
		return -ENXIO;
	}
	free(status);

	return 0;
}

int
spdk_nvme_ctrlr_security_send(struct spdk_nvme_ctrlr *ctrlr, uint8_t secp,
			      uint16_t spsp, uint8_t nssf, void *payload, size_t size)
{
	struct nvme_completion_poll_status	*status;
	int					res;

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	res = spdk_nvme_ctrlr_cmd_security_send(ctrlr, secp, spsp, nssf, payload, size,
						nvme_completion_poll_cb,
						status);
	if (res) {
		free(status);
		return res;
	}
	if (nvme_wait_for_completion_robust_lock(ctrlr->adminq, status, &ctrlr->ctrlr_lock)) {
		NVME_CTRLR_ERRLOG(ctrlr, "spdk_nvme_ctrlr_cmd_security_send failed!\n");
		if (!status->timed_out) {
			free(status);
		}
		return -ENXIO;
	}

	free(status);

	return 0;
}

uint64_t
spdk_nvme_ctrlr_get_flags(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->flags;
}

const struct spdk_nvme_transport_id *
spdk_nvme_ctrlr_get_transport_id(struct spdk_nvme_ctrlr *ctrlr)
{
	return &ctrlr->trid;
}

int32_t
spdk_nvme_ctrlr_alloc_qid(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t qid;

	assert(ctrlr->free_io_qids);
	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	qid = spdk_bit_array_find_first_set(ctrlr->free_io_qids, 1);
	if (qid > ctrlr->opts.num_io_queues) {
		NVME_CTRLR_ERRLOG(ctrlr, "No free I/O queue IDs\n");
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -1;
	}

	spdk_bit_array_clear(ctrlr->free_io_qids, qid);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	return qid;
}

void
spdk_nvme_ctrlr_free_qid(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid)
{
	assert(qid <= ctrlr->opts.num_io_queues);

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);

	if (spdk_likely(ctrlr->free_io_qids)) {
		spdk_bit_array_set(ctrlr->free_io_qids, qid);
	}

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
}

int
spdk_nvme_ctrlr_get_memory_domains(const struct spdk_nvme_ctrlr *ctrlr,
				   struct spdk_memory_domain **domains, int array_size)
{
	return nvme_transport_ctrlr_get_memory_domains(ctrlr, domains, array_size);
}
