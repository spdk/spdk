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

#include "nvme_internal.h"

#include "spdk/env.h"

#include <uuid/uuid.h>

static int nvme_ctrlr_construct_and_submit_aer(struct spdk_nvme_ctrlr *ctrlr,
		struct nvme_async_event_request *aer);

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

static int
nvme_ctrlr_get_vs(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_vs_register *vs)
{
	return nvme_transport_ctrlr_get_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, vs.raw),
					      &vs->raw);
}

static int
nvme_ctrlr_set_cc(struct spdk_nvme_ctrlr *ctrlr, const union spdk_nvme_cc_register *cc)
{
	return nvme_transport_ctrlr_set_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, cc.raw),
					      cc->raw);
}

void
spdk_nvme_ctrlr_get_default_ctrlr_opts(struct spdk_nvme_ctrlr_opts *opts, size_t opts_size)
{
	char host_id_str[37];

	assert(opts);

	memset(opts, 0, opts_size);

#define FIELD_OK(field) \
	offsetof(struct spdk_nvme_ctrlr_opts, field) + sizeof(opts->field) <= opts_size

	if (FIELD_OK(num_io_queues)) {
		opts->num_io_queues = DEFAULT_MAX_IO_QUEUES;
	}

	if (FIELD_OK(use_cmb_sqs)) {
		opts->use_cmb_sqs = true;
	}

	if (FIELD_OK(arb_mechanism)) {
		opts->arb_mechanism = SPDK_NVME_CC_AMS_RR;
	}

	if (FIELD_OK(keep_alive_timeout_ms)) {
		opts->keep_alive_timeout_ms = 10 * 1000;
	}

	if (FIELD_OK(io_queue_size)) {
		opts->io_queue_size = DEFAULT_IO_QUEUE_SIZE;
	}

	if (FIELD_OK(io_queue_requests)) {
		opts->io_queue_requests = DEFAULT_IO_QUEUE_REQUESTS;
	}

	if (FIELD_OK(host_id)) {
		memset(opts->host_id, 0, sizeof(opts->host_id));
	}

	if (FIELD_OK(extended_host_id)) {
		memcpy(opts->extended_host_id, g_spdk_nvme_driver->default_extended_host_id,
		       sizeof(opts->extended_host_id));
	}

	if (FIELD_OK(hostnqn)) {
		uuid_unparse(g_spdk_nvme_driver->default_extended_host_id, host_id_str);
		snprintf(opts->hostnqn, sizeof(opts->hostnqn), "2014-08.org.nvmexpress:uuid:%s", host_id_str);
	}

	if (FIELD_OK(src_addr)) {
		memset(opts->src_addr, 0, sizeof(opts->src_addr));
	}

	if (FIELD_OK(src_svcid)) {
		memset(opts->src_svcid, 0, sizeof(opts->src_svcid));
	}
#undef FIELD_OK
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
	pid_t				pid = getpid();

	TAILQ_FOREACH(active_proc, &ctrlr->active_procs, tailq) {
		if (active_proc->pid == pid) {
			TAILQ_INSERT_TAIL(&active_proc->allocated_io_qpairs, qpair,
					  per_process_tailq);
			break;
		}
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
	pid_t				pid = getpid();
	bool				proc_found = false;

	TAILQ_FOREACH(active_proc, &ctrlr->active_procs, tailq) {
		if (active_proc->pid == pid) {
			proc_found = true;
			break;
		}
	}

	if (proc_found == false) {
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

#undef FIELD_OK
}

struct spdk_nvme_qpair *
spdk_nvme_ctrlr_alloc_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
			       const struct spdk_nvme_io_qpair_opts *user_opts,
			       size_t opts_size)
{
	uint32_t				qid;
	struct spdk_nvme_qpair			*qpair;
	union spdk_nvme_cc_register		cc;
	struct spdk_nvme_io_qpair_opts		opts;

	if (!ctrlr) {
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
	}

	if (nvme_ctrlr_get_cc(ctrlr, &cc)) {
		SPDK_ERRLOG("get_cc failed\n");
		return NULL;
	}

	/* Only the low 2 bits (values 0, 1, 2, 3) of QPRIO are valid. */
	if ((opts.qprio & 3) != opts.qprio) {
		return NULL;
	}

	/*
	 * Only value SPDK_NVME_QPRIO_URGENT(0) is valid for the
	 * default round robin arbitration method.
	 */
	if ((cc.bits.ams == SPDK_NVME_CC_AMS_RR) && (opts.qprio != SPDK_NVME_QPRIO_URGENT)) {
		SPDK_ERRLOG("invalid queue priority for default round robin arbitration method\n");
		return NULL;
	}

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);

	/*
	 * Get the first available I/O queue ID.
	 */
	qid = spdk_bit_array_find_first_set(ctrlr->free_io_qids, 1);
	if (qid > ctrlr->opts.num_io_queues) {
		SPDK_ERRLOG("No free I/O queue IDs\n");
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return NULL;
	}

	qpair = nvme_transport_ctrlr_create_io_qpair(ctrlr, qid, &opts);
	if (qpair == NULL) {
		SPDK_ERRLOG("nvme_transport_ctrlr_create_io_qpair() failed\n");
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return NULL;
	}
	spdk_bit_array_clear(ctrlr->free_io_qids, qid);
	TAILQ_INSERT_TAIL(&ctrlr->active_io_qpairs, qpair, tailq);

	nvme_ctrlr_proc_add_io_qpair(qpair);

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	if (ctrlr->quirks & NVME_QUIRK_DELAY_AFTER_QUEUE_ALLOC) {
		spdk_delay_us(100);
	}

	return qpair;
}

int
spdk_nvme_ctrlr_free_io_qpair(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr *ctrlr;
	void *req_buf;

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

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);

	nvme_ctrlr_proc_remove_io_qpair(qpair);

	TAILQ_REMOVE(&ctrlr->active_io_qpairs, qpair, tailq);
	spdk_bit_array_set(ctrlr->free_io_qids, qpair->id);

	req_buf = qpair->req_buf;

	if (nvme_transport_ctrlr_delete_io_qpair(ctrlr, qpair)) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -1;
	}

	spdk_dma_free(req_buf);

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

static int nvme_ctrlr_set_intel_support_log_pages(struct spdk_nvme_ctrlr *ctrlr)
{
	uint64_t phys_addr = 0;
	struct nvme_completion_poll_status	status;
	struct spdk_nvme_intel_log_page_directory *log_page_directory;

	log_page_directory = spdk_dma_zmalloc(sizeof(struct spdk_nvme_intel_log_page_directory),
					      64, &phys_addr);
	if (log_page_directory == NULL) {
		SPDK_ERRLOG("could not allocate log_page_directory\n");
		return -ENXIO;
	}

	status.done = false;
	spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_INTEL_LOG_PAGE_DIRECTORY, SPDK_NVME_GLOBAL_NS_TAG,
					 log_page_directory, sizeof(struct spdk_nvme_intel_log_page_directory), 0,
					 nvme_completion_poll_cb,
					 &status);
	while (status.done == false) {
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		spdk_dma_free(log_page_directory);
		SPDK_ERRLOG("nvme_ctrlr_cmd_get_log_page failed!\n");
		return -ENXIO;
	}

	nvme_ctrlr_construct_intel_support_log_page_list(ctrlr, log_page_directory);
	spdk_dma_free(log_page_directory);
	return 0;
}

static void
nvme_ctrlr_set_supported_log_pages(struct spdk_nvme_ctrlr *ctrlr)
{
	memset(ctrlr->log_page_supported, 0, sizeof(ctrlr->log_page_supported));
	/* Mandatory pages */
	ctrlr->log_page_supported[SPDK_NVME_LOG_ERROR] = true;
	ctrlr->log_page_supported[SPDK_NVME_LOG_HEALTH_INFORMATION] = true;
	ctrlr->log_page_supported[SPDK_NVME_LOG_FIRMWARE_SLOT] = true;
	if (ctrlr->cdata.lpa.celp) {
		ctrlr->log_page_supported[SPDK_NVME_LOG_COMMAND_EFFECTS_LOG] = true;
	}
	if (ctrlr->cdata.vid == SPDK_PCI_VID_INTEL) {
		nvme_ctrlr_set_intel_support_log_pages(ctrlr);
	}
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
	ctrlr->is_failed = true;
	SPDK_ERRLOG("ctrlr %s in failed state.\n", ctrlr->trid.traddr);
}

static void
nvme_ctrlr_shutdown(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_cc_register	cc;
	union spdk_nvme_csts_register	csts;
	uint32_t			ms_waited = 0;
	uint32_t			shutdown_timeout_ms;

	if (ctrlr->is_removed) {
		return;
	}

	if (nvme_ctrlr_get_cc(ctrlr, &cc)) {
		SPDK_ERRLOG("get_cc() failed\n");
		return;
	}

	cc.bits.shn = SPDK_NVME_SHN_NORMAL;

	if (nvme_ctrlr_set_cc(ctrlr, &cc)) {
		SPDK_ERRLOG("set_cc() failed\n");
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
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "RTD3E = %" PRIu32 " us\n", ctrlr->cdata.rtd3e);
	shutdown_timeout_ms = (ctrlr->cdata.rtd3e + 999) / 1000;
	shutdown_timeout_ms = spdk_max(shutdown_timeout_ms, 10000);
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "shutdown timeout = %" PRIu32 " ms\n", shutdown_timeout_ms);

	do {
		if (nvme_ctrlr_get_csts(ctrlr, &csts)) {
			SPDK_ERRLOG("get_csts() failed\n");
			return;
		}

		if (csts.bits.shst == SPDK_NVME_SHST_COMPLETE) {
			SPDK_DEBUGLOG(SPDK_LOG_NVME, "shutdown complete in %u milliseconds\n",
				      ms_waited);
			return;
		}

		nvme_delay(1000);
		ms_waited++;
	} while (ms_waited < shutdown_timeout_ms);

	SPDK_ERRLOG("did not shutdown within %u milliseconds\n", shutdown_timeout_ms);
}

static int
nvme_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_cc_register	cc;
	int				rc;

	rc = nvme_transport_ctrlr_enable(ctrlr);
	if (rc != 0) {
		SPDK_ERRLOG("transport ctrlr_enable failed\n");
		return rc;
	}

	if (nvme_ctrlr_get_cc(ctrlr, &cc)) {
		SPDK_ERRLOG("get_cc() failed\n");
		return -EIO;
	}

	if (cc.bits.en != 0) {
		SPDK_ERRLOG("%s called with CC.EN = 1\n", __func__);
		return -EINVAL;
	}

	cc.bits.en = 1;
	cc.bits.css = 0;
	cc.bits.shn = 0;
	cc.bits.iosqes = 6; /* SQ entry size == 64 == 2^6 */
	cc.bits.iocqes = 4; /* CQ entry size == 16 == 2^4 */

	/* Page size is 2 ^ (12 + mps). */
	cc.bits.mps = spdk_u32log2(ctrlr->page_size) - 12;

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

	if (nvme_ctrlr_set_cc(ctrlr, &cc)) {
		SPDK_ERRLOG("set_cc() failed\n");
		return -EIO;
	}

	return 0;
}

#ifdef DEBUG
static const char *
nvme_ctrlr_state_string(enum nvme_ctrlr_state state)
{
	switch (state) {
	case NVME_CTRLR_STATE_INIT:
		return "init";
	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1:
		return "disable and wait for CSTS.RDY = 1";
	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0:
		return "disable and wait for CSTS.RDY = 0";
	case NVME_CTRLR_STATE_ENABLE:
		return "enable controller by writing CC.EN = 1";
	case NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1:
		return "wait for CSTS.RDY = 1";
	case NVME_CTRLR_STATE_READY:
		return "ready";
	}
	return "unknown";
};
#endif /* DEBUG */

static void
nvme_ctrlr_set_state(struct spdk_nvme_ctrlr *ctrlr, enum nvme_ctrlr_state state,
		     uint64_t timeout_in_ms)
{
	ctrlr->state = state;
	if (timeout_in_ms == NVME_TIMEOUT_INFINITE) {
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "setting state to %s (no timeout)\n",
			      nvme_ctrlr_state_string(ctrlr->state));
		ctrlr->state_timeout_tsc = NVME_TIMEOUT_INFINITE;
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "setting state to %s (timeout %" PRIu64 " ms)\n",
			      nvme_ctrlr_state_string(ctrlr->state), timeout_in_ms);
		ctrlr->state_timeout_tsc = spdk_get_ticks() + (timeout_in_ms * spdk_get_ticks_hz()) / 1000;
	}
}

static void
nvme_ctrlr_free_doorbell_buffer(struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr->shadow_doorbell) {
		spdk_dma_free(ctrlr->shadow_doorbell);
		ctrlr->shadow_doorbell = NULL;
	}

	if (ctrlr->eventidx) {
		spdk_dma_free(ctrlr->eventidx);
		ctrlr->eventidx = NULL;
	}
}

static int
nvme_ctrlr_set_doorbell_buffer_config(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;
	struct nvme_completion_poll_status status;
	uint64_t prp1, prp2;

	if (ctrlr->trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
		return 0;
	}

	/* only 1 page size for doorbell buffer */
	ctrlr->shadow_doorbell = spdk_dma_zmalloc(ctrlr->page_size, ctrlr->page_size,
				 &prp1);
	if (ctrlr->shadow_doorbell == NULL) {
		return -1;
	}

	ctrlr->eventidx = spdk_dma_zmalloc(ctrlr->page_size, ctrlr->page_size, &prp2);
	if (ctrlr->eventidx == NULL) {
		goto error;
	}

	status.done = false;
	rc = nvme_ctrlr_cmd_doorbell_buffer_config(ctrlr, prp1, prp2,
			nvme_completion_poll_cb, &status);
	if (rc != 0) {
		goto error;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		goto error;
	}

	SPDK_INFOLOG(SPDK_LOG_NVME, "NVMe controller: %s doorbell buffer config enabled\n",
		     ctrlr->trid.traddr);

	return 0;

error:
	nvme_ctrlr_free_doorbell_buffer(ctrlr);
	return -1;
}

int
spdk_nvme_ctrlr_reset(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc = 0;
	struct spdk_nvme_qpair	*qpair;
	struct nvme_request	*req, *tmp;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);

	if (ctrlr->is_resetting || ctrlr->is_failed) {
		/*
		 * Controller is already resetting or has failed.  Return
		 *  immediately since there is no need to kick off another
		 *  reset in these cases.
		 */
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return 0;
	}

	ctrlr->is_resetting = true;

	SPDK_NOTICELOG("resetting controller\n");

	/* Free all of the queued abort requests */
	STAILQ_FOREACH_SAFE(req, &ctrlr->queued_aborts, stailq, tmp) {
		STAILQ_REMOVE_HEAD(&ctrlr->queued_aborts, stailq);
		nvme_free_request(req);
		ctrlr->outstanding_aborts--;
	}

	/* Disable all queues before disabling the controller hardware. */
	nvme_qpair_disable(ctrlr->adminq);
	TAILQ_FOREACH(qpair, &ctrlr->active_io_qpairs, tailq) {
		nvme_qpair_disable(qpair);
	}

	/* Doorbell buffer config is invalid during reset */
	nvme_ctrlr_free_doorbell_buffer(ctrlr);

	/* Set the state back to INIT to cause a full hardware reset. */
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_INIT, NVME_TIMEOUT_INFINITE);

	while (ctrlr->state != NVME_CTRLR_STATE_READY) {
		if (nvme_ctrlr_process_init(ctrlr) != 0) {
			SPDK_ERRLOG("%s: controller reinitialization failed\n", __func__);
			nvme_ctrlr_fail(ctrlr, false);
			rc = -1;
			break;
		}
	}

	if (!ctrlr->is_failed) {
		/* Reinitialize qpairs */
		TAILQ_FOREACH(qpair, &ctrlr->active_io_qpairs, tailq) {
			if (nvme_transport_ctrlr_reinit_io_qpair(ctrlr, qpair) != 0) {
				nvme_ctrlr_fail(ctrlr, false);
				rc = -1;
			}
		}
	}

	ctrlr->is_resetting = false;

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return rc;
}

static int
nvme_ctrlr_identify(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_completion_poll_status	status;
	int					rc;

	status.done = false;
	rc = nvme_ctrlr_cmd_identify_controller(ctrlr, &ctrlr->cdata,
						nvme_completion_poll_cb, &status);
	if (rc != 0) {
		return rc;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("nvme_identify_controller failed!\n");
		return -ENXIO;
	}

	/*
	 * Use MDTS to ensure our default max_xfer_size doesn't exceed what the
	 *  controller supports.
	 */
	ctrlr->max_xfer_size = nvme_transport_ctrlr_get_max_xfer_size(ctrlr);
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "transport max_xfer_size %u\n", ctrlr->max_xfer_size);
	if (ctrlr->cdata.mdts > 0) {
		ctrlr->max_xfer_size = spdk_min(ctrlr->max_xfer_size,
						ctrlr->min_page_size * (1 << (ctrlr->cdata.mdts)));
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "MDTS max_xfer_size %u\n", ctrlr->max_xfer_size);
	}

	return 0;
}

static int
nvme_ctrlr_set_num_qpairs(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_completion_poll_status	status;
	uint32_t cq_allocated, sq_allocated, min_allocated, i;
	int rc;

	status.done = false;

	if (ctrlr->opts.num_io_queues > SPDK_NVME_MAX_IO_QUEUES) {
		SPDK_NOTICELOG("Limiting requested num_io_queues %u to max %d\n",
			       ctrlr->opts.num_io_queues, SPDK_NVME_MAX_IO_QUEUES);
		ctrlr->opts.num_io_queues = SPDK_NVME_MAX_IO_QUEUES;
	} else if (ctrlr->opts.num_io_queues < 1) {
		SPDK_NOTICELOG("Requested num_io_queues 0, increasing to 1\n");
		ctrlr->opts.num_io_queues = 1;
	}

	rc = nvme_ctrlr_cmd_set_num_queues(ctrlr, ctrlr->opts.num_io_queues,
					   nvme_completion_poll_cb, &status);
	if (rc != 0) {
		return rc;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("nvme_set_num_queues failed!\n");
		return -ENXIO;
	}

	/* Obtain the number of queues allocated using Get Features. */
	status.done = false;
	rc = nvme_ctrlr_cmd_get_num_queues(ctrlr, nvme_completion_poll_cb, &status);
	if (rc != 0) {
		return rc;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("nvme_set_num_queues failed!\n");
		return -ENXIO;
	}

	/*
	 * Data in cdw0 is 0-based.
	 * Lower 16-bits indicate number of submission queues allocated.
	 * Upper 16-bits indicate number of completion queues allocated.
	 */
	sq_allocated = (status.cpl.cdw0 & 0xFFFF) + 1;
	cq_allocated = (status.cpl.cdw0 >> 16) + 1;

	/*
	 * For 1:1 queue mapping, set number of allocated queues to be minimum of
	 * submission and completion queues.
	 */
	min_allocated = spdk_min(sq_allocated, cq_allocated);

	/* Set number of queues to be minimum of requested and actually allocated. */
	ctrlr->opts.num_io_queues = spdk_min(min_allocated, ctrlr->opts.num_io_queues);

	ctrlr->free_io_qids = spdk_bit_array_create(ctrlr->opts.num_io_queues + 1);
	if (ctrlr->free_io_qids == NULL) {
		return -ENOMEM;
	}

	/* Initialize list of free I/O queue IDs. QID 0 is the admin queue. */
	spdk_bit_array_clear(ctrlr->free_io_qids, 0);
	for (i = 1; i <= ctrlr->opts.num_io_queues; i++) {
		spdk_bit_array_set(ctrlr->free_io_qids, i);
	}

	return 0;
}

static int
nvme_ctrlr_set_keep_alive_timeout(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_completion_poll_status status;
	uint32_t keep_alive_interval_ms;
	int rc;

	if (ctrlr->opts.keep_alive_timeout_ms == 0) {
		return 0;
	}

	if (ctrlr->cdata.kas == 0) {
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "Controller KAS is 0 - not enabling Keep Alive\n");
		ctrlr->opts.keep_alive_timeout_ms = 0;
		return 0;
	}

	/* Retrieve actual keep alive timeout, since the controller may have adjusted it. */
	status.done = false;
	rc = spdk_nvme_ctrlr_cmd_get_feature(ctrlr, SPDK_NVME_FEAT_KEEP_ALIVE_TIMER, 0, NULL, 0,
					     nvme_completion_poll_cb, &status);
	if (rc != 0) {
		SPDK_ERRLOG("Keep alive timeout Get Feature failed: %d\n", rc);
		ctrlr->opts.keep_alive_timeout_ms = 0;
		return rc;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("Keep alive timeout Get Feature failed: SC %x SCT %x\n",
			    status.cpl.status.sc, status.cpl.status.sct);
		ctrlr->opts.keep_alive_timeout_ms = 0;
		return -ENXIO;
	}

	if (ctrlr->opts.keep_alive_timeout_ms != status.cpl.cdw0) {
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "Controller adjusted keep alive timeout to %u ms\n",
			      status.cpl.cdw0);
	}

	ctrlr->opts.keep_alive_timeout_ms = status.cpl.cdw0;

	keep_alive_interval_ms = ctrlr->opts.keep_alive_timeout_ms / 2;
	if (keep_alive_interval_ms == 0) {
		keep_alive_interval_ms = 1;
	}
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "Sending keep alive every %u ms\n", keep_alive_interval_ms);

	ctrlr->keep_alive_interval_ticks = (keep_alive_interval_ms * spdk_get_ticks_hz()) / UINT64_C(1000);

	/* Schedule the first Keep Alive to be sent as soon as possible. */
	ctrlr->next_keep_alive_tick = spdk_get_ticks();

	return 0;
}

static int
nvme_ctrlr_set_host_id(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_completion_poll_status status;
	bool all_zeroes;
	uint8_t *host_id;
	uint32_t host_id_size;
	uint32_t i;
	int rc;

	if (ctrlr->trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
		/*
		 * NVMe-oF sends the host ID during Connect and doesn't allow
		 * Set Features - Host Identifier after Connect, so we don't need to do anything here.
		 */
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "NVMe-oF transport - not sending Set Features - Host ID\n");
		return 0;
	}

	if (ctrlr->cdata.ctratt.host_id_exhid_supported) {
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "Using 128-bit extended host identifier\n");
		host_id = ctrlr->opts.extended_host_id;
		host_id_size = sizeof(ctrlr->opts.extended_host_id);
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "Using 64-bit host identifier\n");
		host_id = ctrlr->opts.host_id;
		host_id_size = sizeof(ctrlr->opts.host_id);
	}

	/* If the user specified an all-zeroes host identifier, don't send the command. */
	all_zeroes = true;
	for (i = 0; i < host_id_size; i++) {
		if (host_id[i] != 0) {
			all_zeroes = false;
			break;
		}
	}

	if (all_zeroes) {
		SPDK_DEBUGLOG(SPDK_LOG_NVME,
			      "User did not specify host ID - not sending Set Features - Host ID\n");
		return 0;
	}

	SPDK_TRACEDUMP(SPDK_LOG_NVME, "host_id", host_id, host_id_size);

	status.done = false;
	rc = nvme_ctrlr_cmd_set_host_id(ctrlr, host_id, host_id_size, nvme_completion_poll_cb, &status);
	if (rc != 0) {
		SPDK_ERRLOG("Set Features - Host ID failed: %d\n", rc);
		return rc;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_WARNLOG("Set Features - Host ID failed: SC 0x%x SCT 0x%x\n",
			     status.cpl.status.sc, status.cpl.status.sct);
		/*
		 * Treat Set Features - Host ID failure as non-fatal, since the Host ID feature
		 * is optional.
		 */
		return 0;
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "Set Features - Host ID was successful\n");
	return 0;
}

static void
nvme_ctrlr_destruct_namespaces(struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr->ns) {
		uint32_t i, num_ns = ctrlr->num_ns;

		for (i = 0; i < num_ns; i++) {
			nvme_ns_destruct(&ctrlr->ns[i]);
		}

		spdk_dma_free(ctrlr->ns);
		ctrlr->ns = NULL;
		ctrlr->num_ns = 0;
	}

	if (ctrlr->nsdata) {
		spdk_dma_free(ctrlr->nsdata);
		ctrlr->nsdata = NULL;
	}
}

static int
nvme_ctrlr_construct_namespaces(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t i, nn = ctrlr->cdata.nn;
	uint64_t phys_addr = 0;

	if (nn == 0) {
		SPDK_ERRLOG("controller has 0 namespaces\n");
		return -1;
	}

	/* ctrlr->num_ns may be 0 (startup) or a different number of namespaces (reset),
	 * so check if we need to reallocate.
	 */
	if (nn != ctrlr->num_ns) {
		nvme_ctrlr_destruct_namespaces(ctrlr);

		ctrlr->ns = spdk_dma_zmalloc(nn * sizeof(struct spdk_nvme_ns), 64,
					     &phys_addr);
		if (ctrlr->ns == NULL) {
			goto fail;
		}

		ctrlr->nsdata = spdk_dma_zmalloc(nn * sizeof(struct spdk_nvme_ns_data), 64,
						 &phys_addr);
		if (ctrlr->nsdata == NULL) {
			goto fail;
		}

		ctrlr->num_ns = nn;
	}

	for (i = 0; i < nn; i++) {
		struct spdk_nvme_ns	*ns = &ctrlr->ns[i];
		uint32_t 		nsid = i + 1;

		if (nvme_ns_construct(ns, nsid, ctrlr) != 0) {
			goto fail;
		}
	}

	return 0;

fail:
	nvme_ctrlr_destruct_namespaces(ctrlr);
	return -1;
}

static void
nvme_ctrlr_async_event_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_async_event_request	*aer = arg;
	struct spdk_nvme_ctrlr		*ctrlr = aer->ctrlr;

	if (cpl->status.sc == SPDK_NVME_SC_ABORTED_SQ_DELETION) {
		/*
		 *  This is simulated when controller is being shut down, to
		 *  effectively abort outstanding asynchronous event requests
		 *  and make sure all memory is freed.  Do not repost the
		 *  request in this case.
		 */
		return;
	}

	if (ctrlr->aer_cb_fn != NULL) {
		ctrlr->aer_cb_fn(ctrlr->aer_cb_arg, cpl);
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
		SPDK_ERRLOG("resubmitting AER failed!\n");
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

static int
nvme_ctrlr_configure_aer(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_critical_warning_state	state;
	struct nvme_async_event_request		*aer;
	uint32_t				i;
	struct nvme_completion_poll_status	status;
	int					rc;

	status.done = false;

	state.raw = 0xFF;
	state.bits.reserved = 0;
	rc = nvme_ctrlr_cmd_set_async_event_config(ctrlr, state, nvme_completion_poll_cb, &status);
	if (rc != 0) {
		return rc;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("nvme_ctrlr_cmd_set_async_event_config failed!\n");
		return 0;
	}

	/* aerl is a zero-based value, so we need to add 1 here. */
	ctrlr->num_aers = spdk_min(NVME_MAX_ASYNC_EVENTS, (ctrlr->cdata.aerl + 1));

	for (i = 0; i < ctrlr->num_aers; i++) {
		aer = &ctrlr->aer[i];
		if (nvme_ctrlr_construct_and_submit_aer(ctrlr, aer)) {
			SPDK_ERRLOG("nvme_ctrlr_construct_and_submit_aer failed!\n");
			return -1;
		}
	}

	return 0;
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
	struct spdk_nvme_ctrlr_process	*ctrlr_proc, *active_proc;
	pid_t				pid = getpid();

	/* Check whether the process is already added or not */
	TAILQ_FOREACH(active_proc, &ctrlr->active_procs, tailq) {
		if (active_proc->pid == pid) {
			return 0;
		}
	}

	/* Initialize the per process properties for this ctrlr */
	ctrlr_proc = spdk_dma_zmalloc(sizeof(struct spdk_nvme_ctrlr_process), 64, NULL);
	if (ctrlr_proc == NULL) {
		SPDK_ERRLOG("failed to allocate memory to track the process props\n");

		return -1;
	}

	ctrlr_proc->is_primary = spdk_process_is_primary();
	ctrlr_proc->pid = pid;
	STAILQ_INIT(&ctrlr_proc->active_reqs);
	ctrlr_proc->devhandle = devhandle;
	ctrlr_proc->ref = 0;
	TAILQ_INIT(&ctrlr_proc->allocated_io_qpairs);

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

	spdk_dma_free(proc);
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

	STAILQ_FOREACH_SAFE(req, &proc->active_reqs, stailq, tmp_req) {
		STAILQ_REMOVE(&proc->active_reqs, req, nvme_request, stailq);

		assert(req->pid == proc->pid);

		nvme_free_request(req);
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

	spdk_dma_free(proc);
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

		spdk_dma_free(active_proc);
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
			SPDK_ERRLOG("process %d terminated unexpected\n", active_proc->pid);

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
	pid_t				pid = getpid();

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);

	nvme_ctrlr_remove_inactive_proc(ctrlr);

	TAILQ_FOREACH(active_proc, &ctrlr->active_procs, tailq) {
		if (active_proc->pid == pid) {
			active_proc->ref++;
			break;
		}
	}

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
}

void
nvme_ctrlr_proc_put_ref(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ctrlr_process	*active_proc, *tmp;
	pid_t				pid = getpid();
	int				proc_count;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);

	proc_count = nvme_ctrlr_remove_inactive_proc(ctrlr);

	TAILQ_FOREACH_SAFE(active_proc, &ctrlr->active_procs, tailq, tmp) {
		if (active_proc->pid == pid) {
			active_proc->ref--;
			assert(active_proc->ref >= 0);

			/*
			 * The last active process will be removed at the end of
			 * the destruction of the controller.
			 */
			if (active_proc->ref == 0 && proc_count != 1) {
				nvme_ctrlr_remove_process(ctrlr, active_proc);
			}

			break;
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
	pid_t				pid = getpid();
	struct spdk_pci_device		*devhandle = NULL;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);

	TAILQ_FOREACH(active_proc, &ctrlr->active_procs, tailq) {
		if (active_proc->pid == pid) {
			devhandle = active_proc->devhandle;
			break;
		}
	}

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return devhandle;
}

/**
 * This function will be called repeatedly during initialization until the controller is ready.
 */
int
nvme_ctrlr_process_init(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_cc_register cc;
	union spdk_nvme_csts_register csts;
	uint32_t ready_timeout_in_ms;
	int rc;

	/*
	 * May need to avoid accessing any register on the target controller
	 * for a while. Return early without touching the FSM.
	 * Check sleep_timeout_tsc > 0 for unit test.
	 */
	if ((ctrlr->sleep_timeout_tsc > 0) &&
	    (spdk_get_ticks() <= ctrlr->sleep_timeout_tsc)) {
		return 0;
	}
	ctrlr->sleep_timeout_tsc = 0;

	if (nvme_ctrlr_get_cc(ctrlr, &cc) ||
	    nvme_ctrlr_get_csts(ctrlr, &csts)) {
		if (ctrlr->state_timeout_tsc != NVME_TIMEOUT_INFINITE) {
			/* While a device is resetting, it may be unable to service MMIO reads
			 * temporarily. Allow for this case.
			 */
			SPDK_ERRLOG("Get registers failed while waiting for CSTS.RDY == 0\n");
			goto init_timeout;
		}
		SPDK_ERRLOG("Failed to read CC and CSTS in state %d\n", ctrlr->state);
		nvme_ctrlr_fail(ctrlr, false);
		return -EIO;
	}

	ready_timeout_in_ms = 500 * ctrlr->cap.bits.to;

	/*
	 * Check if the current initialization step is done or has timed out.
	 */
	switch (ctrlr->state) {
	case NVME_CTRLR_STATE_INIT:
		/* Begin the hardware initialization by making sure the controller is disabled. */
		if (cc.bits.en) {
			SPDK_DEBUGLOG(SPDK_LOG_NVME, "CC.EN = 1\n");
			/*
			 * Controller is currently enabled. We need to disable it to cause a reset.
			 *
			 * If CC.EN = 1 && CSTS.RDY = 0, the controller is in the process of becoming ready.
			 *  Wait for the ready bit to be 1 before disabling the controller.
			 */
			if (csts.bits.rdy == 0) {
				SPDK_DEBUGLOG(SPDK_LOG_NVME, "CC.EN = 1 && CSTS.RDY = 0 - waiting for reset to complete\n");
				nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1, ready_timeout_in_ms);
				return 0;
			}

			/* CC.EN = 1 && CSTS.RDY == 1, so we can immediately disable the controller. */
			SPDK_DEBUGLOG(SPDK_LOG_NVME, "Setting CC.EN = 0\n");
			cc.bits.en = 0;
			if (nvme_ctrlr_set_cc(ctrlr, &cc)) {
				SPDK_ERRLOG("set_cc() failed\n");
				nvme_ctrlr_fail(ctrlr, false);
				return -EIO;
			}
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0, ready_timeout_in_ms);

			/*
			 * Wait 2 secsonds before accessing PCI registers.
			 * Not using sleep() to avoid blocking other controller's initialization.
			 */
			if (ctrlr->quirks & NVME_QUIRK_DELAY_BEFORE_CHK_RDY) {
				SPDK_DEBUGLOG(SPDK_LOG_NVME, "Applying quirk: delay 2 seconds before reading registers\n");
				ctrlr->sleep_timeout_tsc = spdk_get_ticks() + 2 * spdk_get_ticks_hz();
			}
			return 0;
		} else {
			if (csts.bits.rdy == 1) {
				SPDK_DEBUGLOG(SPDK_LOG_NVME, "CC.EN = 0 && CSTS.RDY = 1 - waiting for shutdown to complete\n");
			}

			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0, ready_timeout_in_ms);
			return 0;
		}
		break;

	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1:
		if (csts.bits.rdy == 1) {
			SPDK_DEBUGLOG(SPDK_LOG_NVME, "CC.EN = 1 && CSTS.RDY = 1 - disabling controller\n");
			/* CC.EN = 1 && CSTS.RDY = 1, so we can set CC.EN = 0 now. */
			SPDK_DEBUGLOG(SPDK_LOG_NVME, "Setting CC.EN = 0\n");
			cc.bits.en = 0;
			if (nvme_ctrlr_set_cc(ctrlr, &cc)) {
				SPDK_ERRLOG("set_cc() failed\n");
				nvme_ctrlr_fail(ctrlr, false);
				return -EIO;
			}
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0, ready_timeout_in_ms);
			return 0;
		}
		break;

	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0:
		if (csts.bits.rdy == 0) {
			SPDK_DEBUGLOG(SPDK_LOG_NVME, "CC.EN = 0 && CSTS.RDY = 0\n");
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ENABLE, ready_timeout_in_ms);
			/*
			 * Delay 100us before setting CC.EN = 1.  Some NVMe SSDs miss CC.EN getting
			 *  set to 1 if it is too soon after CSTS.RDY is reported as 0.
			 */
			spdk_delay_us(100);
			return 0;
		}
		break;

	case NVME_CTRLR_STATE_ENABLE:
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "Setting CC.EN = 1\n");
		rc = nvme_ctrlr_enable(ctrlr);
		nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1, ready_timeout_in_ms);
		return rc;

	case NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1:
		if (csts.bits.rdy == 1) {
			SPDK_DEBUGLOG(SPDK_LOG_NVME, "CC.EN = 1 && CSTS.RDY = 1 - controller is ready\n");
			/*
			 * The controller has been enabled.
			 *  Perform the rest of initialization in nvme_ctrlr_start() serially.
			 */
			rc = nvme_ctrlr_start(ctrlr);
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_READY, NVME_TIMEOUT_INFINITE);
			return rc;
		}
		break;

	case NVME_CTRLR_STATE_READY:
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "Ctrlr already in ready state\n");
		return 0;

	default:
		assert(0);
		nvme_ctrlr_fail(ctrlr, false);
		return -1;
	}

init_timeout:
	if (ctrlr->state_timeout_tsc != NVME_TIMEOUT_INFINITE &&
	    spdk_get_ticks() > ctrlr->state_timeout_tsc) {
		SPDK_ERRLOG("Initialization timed out in state %d\n", ctrlr->state);
		nvme_ctrlr_fail(ctrlr, false);
		return -1;
	}

	return 0;
}

int
nvme_ctrlr_start(struct spdk_nvme_ctrlr *ctrlr)
{
	nvme_transport_qpair_reset(ctrlr->adminq);

	nvme_qpair_enable(ctrlr->adminq);

	if (nvme_ctrlr_identify(ctrlr) != 0) {
		return -1;
	}

	if (nvme_ctrlr_set_num_qpairs(ctrlr) != 0) {
		return -1;
	}

	if (nvme_ctrlr_construct_namespaces(ctrlr) != 0) {
		return -1;
	}

	if (nvme_ctrlr_configure_aer(ctrlr) != 0) {
		return -1;
	}

	nvme_ctrlr_set_supported_log_pages(ctrlr);
	nvme_ctrlr_set_supported_features(ctrlr);

	if (ctrlr->cdata.sgls.supported) {
		ctrlr->flags |= SPDK_NVME_CTRLR_SGL_SUPPORTED;
		ctrlr->max_sges = nvme_transport_ctrlr_get_max_sges(ctrlr);
	}

	if (ctrlr->cdata.oacs.doorbell_buffer_config) {
		if (nvme_ctrlr_set_doorbell_buffer_config(ctrlr)) {
			SPDK_WARNLOG("Doorbell buffer config failed\n");
		}
	}


	if (nvme_ctrlr_set_keep_alive_timeout(ctrlr) != 0) {
		SPDK_ERRLOG("Setting keep alive timeout failed\n");
		return -1;
	}

	if (nvme_ctrlr_set_host_id(ctrlr) != 0) {
		return -1;
	}

	return 0;
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

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_INIT, NVME_TIMEOUT_INFINITE);
	ctrlr->flags = 0;
	ctrlr->free_io_qids = NULL;
	ctrlr->is_resetting = false;
	ctrlr->is_failed = false;

	TAILQ_INIT(&ctrlr->active_io_qpairs);
	STAILQ_INIT(&ctrlr->queued_aborts);
	ctrlr->outstanding_aborts = 0;

	rc = nvme_robust_mutex_init_recursive_shared(&ctrlr->ctrlr_lock);
	if (rc != 0) {
		return rc;
	}

	TAILQ_INIT(&ctrlr->active_procs);
	ctrlr->timeout_cb_fn = NULL;
	ctrlr->timeout_cb_arg = NULL;
	ctrlr->timeout_ticks = 0;

	return rc;
}

/* This function should be called once at ctrlr initialization to set up constant properties. */
void
nvme_ctrlr_init_cap(struct spdk_nvme_ctrlr *ctrlr, const union spdk_nvme_cap_register *cap)
{
	ctrlr->cap = *cap;

	ctrlr->min_page_size = 1u << (12 + ctrlr->cap.bits.mpsmin);

	/* For now, always select page_size == min_page_size. */
	ctrlr->page_size = ctrlr->min_page_size;

	ctrlr->opts.io_queue_size = spdk_max(ctrlr->opts.io_queue_size, SPDK_NVME_IO_QUEUE_MIN_ENTRIES);
	ctrlr->opts.io_queue_size = spdk_min(ctrlr->opts.io_queue_size, ctrlr->cap.bits.mqes + 1u);

	ctrlr->opts.io_queue_requests = spdk_max(ctrlr->opts.io_queue_requests, ctrlr->opts.io_queue_size);
}

void
nvme_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_qpair *qpair, *tmp;

	TAILQ_FOREACH_SAFE(qpair, &ctrlr->active_io_qpairs, tailq, tmp) {
		spdk_nvme_ctrlr_free_io_qpair(qpair);
	}

	nvme_ctrlr_free_doorbell_buffer(ctrlr);

	nvme_ctrlr_shutdown(ctrlr);

	nvme_ctrlr_destruct_namespaces(ctrlr);

	spdk_bit_array_free(&ctrlr->free_io_qids);

	pthread_mutex_destroy(&ctrlr->ctrlr_lock);

	nvme_transport_ctrlr_destruct(ctrlr);
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
static void
nvme_ctrlr_keep_alive(struct spdk_nvme_ctrlr *ctrlr)
{
	uint64_t now;
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;
	int rc;

	now = spdk_get_ticks();
	if (now < ctrlr->next_keep_alive_tick) {
		return;
	}

	req = nvme_allocate_request_null(ctrlr->adminq, nvme_keep_alive_completion, NULL);
	if (req == NULL) {
		return;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_KEEP_ALIVE;

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
	if (rc != 0) {
		SPDK_ERRLOG("Submitting Keep Alive failed\n");
	}

	ctrlr->next_keep_alive_tick = now + ctrlr->keep_alive_interval_ticks;
}

int32_t
spdk_nvme_ctrlr_process_admin_completions(struct spdk_nvme_ctrlr *ctrlr)
{
	int32_t num_completions;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	if (ctrlr->keep_alive_interval_ticks) {
		nvme_ctrlr_keep_alive(ctrlr);
	}
	num_completions = spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

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
		csts.raw = 0xFFFFFFFFu;
	}
	return csts;
}

union spdk_nvme_cap_register spdk_nvme_ctrlr_get_regs_cap(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->cap;
}

union spdk_nvme_vs_register spdk_nvme_ctrlr_get_regs_vs(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_vs_register vs;

	if (nvme_ctrlr_get_vs(ctrlr, &vs)) {
		vs.raw = 0xFFFFFFFFu;
	}
	return vs;
}

uint32_t
spdk_nvme_ctrlr_get_num_ns(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->num_ns;
}

struct spdk_nvme_ns *
spdk_nvme_ctrlr_get_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t ns_id)
{
	if (ns_id < 1 || ns_id > ctrlr->num_ns) {
		return NULL;
	}

	return &ctrlr->ns[ns_id - 1];
}

void
spdk_nvme_ctrlr_register_aer_callback(struct spdk_nvme_ctrlr *ctrlr,
				      spdk_nvme_aer_cb aer_cb_fn,
				      void *aer_cb_arg)
{
	ctrlr->aer_cb_fn = aer_cb_fn;
	ctrlr->aer_cb_arg = aer_cb_arg;
}

void
spdk_nvme_ctrlr_register_timeout_callback(struct spdk_nvme_ctrlr *ctrlr,
		uint32_t nvme_timeout, spdk_nvme_timeout_cb cb_fn, void *cb_arg)
{
	ctrlr->timeout_ticks = nvme_timeout * spdk_get_ticks_hz();
	ctrlr->timeout_cb_fn = cb_fn;
	ctrlr->timeout_cb_arg = cb_arg;
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
	struct nvme_completion_poll_status	status;
	int					res;

	status.done = false;
	res = nvme_ctrlr_cmd_attach_ns(ctrlr, nsid, payload,
				       nvme_completion_poll_cb, &status);
	if (res) {
		return res;
	}
	while (status.done == false) {
		nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("spdk_nvme_ctrlr_attach_ns failed!\n");
		return -ENXIO;
	}

	return spdk_nvme_ctrlr_reset(ctrlr);
}

int
spdk_nvme_ctrlr_detach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			  struct spdk_nvme_ctrlr_list *payload)
{
	struct nvme_completion_poll_status	status;
	int					res;

	status.done = false;
	res = nvme_ctrlr_cmd_detach_ns(ctrlr, nsid, payload,
				       nvme_completion_poll_cb, &status);
	if (res) {
		return res;
	}
	while (status.done == false) {
		nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("spdk_nvme_ctrlr_detach_ns failed!\n");
		return -ENXIO;
	}

	return spdk_nvme_ctrlr_reset(ctrlr);
}

uint32_t
spdk_nvme_ctrlr_create_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns_data *payload)
{
	struct nvme_completion_poll_status	status;
	int					res;

	status.done = false;
	res = nvme_ctrlr_cmd_create_ns(ctrlr, payload, nvme_completion_poll_cb, &status);
	if (res) {
		return 0;
	}
	while (status.done == false) {
		nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("spdk_nvme_ctrlr_create_ns failed!\n");
		return 0;
	}

	res = spdk_nvme_ctrlr_reset(ctrlr);
	if (res) {
		return 0;
	}

	/* Return the namespace ID that was created */
	return status.cpl.cdw0;
}

int
spdk_nvme_ctrlr_delete_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct nvme_completion_poll_status	status;
	int					res;

	status.done = false;
	res = nvme_ctrlr_cmd_delete_ns(ctrlr, nsid, nvme_completion_poll_cb, &status);
	if (res) {
		return res;
	}
	while (status.done == false) {
		nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("spdk_nvme_ctrlr_delete_ns failed!\n");
		return -ENXIO;
	}

	return spdk_nvme_ctrlr_reset(ctrlr);
}

int
spdk_nvme_ctrlr_format(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
		       struct spdk_nvme_format *format)
{
	struct nvme_completion_poll_status	status;
	int					res;

	status.done = false;
	res = nvme_ctrlr_cmd_format(ctrlr, nsid, format, nvme_completion_poll_cb,
				    &status);
	if (res) {
		return res;
	}
	while (status.done == false) {
		nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("spdk_nvme_ctrlr_format failed!\n");
		return -ENXIO;
	}

	return spdk_nvme_ctrlr_reset(ctrlr);
}

int
spdk_nvme_ctrlr_update_firmware(struct spdk_nvme_ctrlr *ctrlr, void *payload, uint32_t size,
				int slot, enum spdk_nvme_fw_commit_action commit_action, struct spdk_nvme_status *completion_status)
{
	struct spdk_nvme_fw_commit		fw_commit;
	struct nvme_completion_poll_status	status;
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
		SPDK_ERRLOG("spdk_nvme_ctrlr_update_firmware invalid size!\n");
		return -1;
	}

	/* Current support only for SPDK_NVME_FW_COMMIT_REPLACE_IMG
	 * and SPDK_NVME_FW_COMMIT_REPLACE_AND_ENABLE_IMG
	 */
	if ((commit_action != SPDK_NVME_FW_COMMIT_REPLACE_IMG) &&
	    (commit_action != SPDK_NVME_FW_COMMIT_REPLACE_AND_ENABLE_IMG)) {
		SPDK_ERRLOG("spdk_nvme_ctrlr_update_firmware invalid command!\n");
		return -1;
	}

	/* Firmware download */
	size_remaining = size;
	offset = 0;
	p = payload;

	while (size_remaining > 0) {
		transfer = spdk_min(size_remaining, ctrlr->min_page_size);
		status.done = false;

		res = nvme_ctrlr_cmd_fw_image_download(ctrlr, transfer, offset, p,
						       nvme_completion_poll_cb,
						       &status);
		if (res) {
			return res;
		}

		while (status.done == false) {
			nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
			spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
			nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		}
		if (spdk_nvme_cpl_is_error(&status.cpl)) {
			SPDK_ERRLOG("spdk_nvme_ctrlr_fw_image_download failed!\n");
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

	status.done = false;

	res = nvme_ctrlr_cmd_fw_commit(ctrlr, &fw_commit, nvme_completion_poll_cb,
				       &status);
	if (res) {
		return res;
	}

	while (status.done == false) {
		nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	}
	memcpy(completion_status, &status.cpl.status, sizeof(struct spdk_nvme_status));
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		if (status.cpl.status.sct != SPDK_NVME_SCT_COMMAND_SPECIFIC ||
		    status.cpl.status.sc != SPDK_NVME_SC_FIRMWARE_REQ_NVM_RESET) {
			if (status.cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC  &&
			    status.cpl.status.sc == SPDK_NVME_SC_FIRMWARE_REQ_CONVENTIONAL_RESET) {
				SPDK_NOTICELOG("firmware activation requires conventional reset to be performed. !\n");
			} else {
				SPDK_ERRLOG("nvme_ctrlr_cmd_fw_commit failed!\n");
			}
			return -ENXIO;
		}
	}

	return spdk_nvme_ctrlr_reset(ctrlr);
}

void *
spdk_nvme_ctrlr_alloc_cmb_io_buffer(struct spdk_nvme_ctrlr *ctrlr, size_t size)
{
	void *buf;

	if (size == 0) {
		return NULL;
	}

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	buf = nvme_transport_ctrlr_alloc_cmb_io_buffer(ctrlr, size);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return buf;
}

void
spdk_nvme_ctrlr_free_cmb_io_buffer(struct spdk_nvme_ctrlr *ctrlr, void *buf, size_t size)
{
	if (buf && size) {
		nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
		nvme_transport_ctrlr_free_cmb_io_buffer(ctrlr, buf, size);
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	}
}
