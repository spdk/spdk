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

#include "nvme_internal.h"
#include "spdk/env.h"
#include <signal.h>

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
spdk_nvme_ctrlr_opts_set_defaults(struct spdk_nvme_ctrlr_opts *opts)
{
	opts->num_io_queues = DEFAULT_MAX_IO_QUEUES;
	opts->use_cmb_sqs = false;
	opts->arb_mechanism = SPDK_NVME_CC_AMS_RR;
	opts->keep_alive_timeout_ms = 10 * 1000;
	opts->io_queue_size = DEFAULT_IO_QUEUE_SIZE;
	strncpy(opts->hostnqn, DEFAULT_HOSTNQN, sizeof(opts->hostnqn));
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

struct spdk_nvme_qpair *
spdk_nvme_ctrlr_alloc_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
			       enum spdk_nvme_qprio qprio)
{
	uint32_t				qid;
	struct spdk_nvme_qpair			*qpair;
	union spdk_nvme_cc_register		cc;

	if (nvme_ctrlr_get_cc(ctrlr, &cc)) {
		SPDK_ERRLOG("get_cc failed\n");
		return NULL;
	}

	/* Only the low 2 bits (values 0, 1, 2, 3) of QPRIO are valid. */
	if ((qprio & 3) != qprio) {
		return NULL;
	}

	/*
	 * Only value SPDK_NVME_QPRIO_URGENT(0) is valid for the
	 * default round robin arbitration method.
	 */
	if ((cc.bits.ams == SPDK_NVME_CC_AMS_RR) && (qprio != SPDK_NVME_QPRIO_URGENT)) {
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

	qpair = nvme_transport_ctrlr_create_io_qpair(ctrlr, qid, qprio);
	if (qpair == NULL) {
		SPDK_ERRLOG("transport->ctrlr_create_io_qpair() failed\n");
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return NULL;
	}
	spdk_bit_array_clear(ctrlr->free_io_qids, qid);
	TAILQ_INSERT_TAIL(&ctrlr->active_io_qpairs, qpair, tailq);

	nvme_ctrlr_proc_add_io_qpair(qpair);

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return qpair;
}

int
spdk_nvme_ctrlr_free_io_qpair(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr *ctrlr;

	if (qpair == NULL) {
		return 0;
	}

	ctrlr = qpair->ctrlr;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);

	nvme_ctrlr_proc_remove_io_qpair(qpair);

	TAILQ_REMOVE(&ctrlr->active_io_qpairs, qpair, tailq);
	spdk_bit_array_set(ctrlr->free_io_qids, qpair->id);

	if (nvme_transport_ctrlr_delete_io_qpair(ctrlr, qpair)) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -1;
	}

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

	log_page_directory = spdk_zmalloc(sizeof(struct spdk_nvme_intel_log_page_directory),
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
		spdk_free(log_page_directory);
		SPDK_ERRLOG("nvme_ctrlr_cmd_get_log_page failed!\n");
		return -ENXIO;
	}

	nvme_ctrlr_construct_intel_support_log_page_list(ctrlr, log_page_directory);
	spdk_free(log_page_directory);
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
	struct spdk_nvme_qpair *qpair;

	if (hot_remove) {
		ctrlr->is_removed = true;
	}
	ctrlr->is_failed = true;
	nvme_qpair_fail(ctrlr->adminq);
	TAILQ_FOREACH(qpair, &ctrlr->active_io_qpairs, tailq) {
		nvme_qpair_fail(qpair);
	}
}

static void
nvme_ctrlr_shutdown(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_cc_register	cc;
	union spdk_nvme_csts_register	csts;
	int				ms_waited = 0;

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
	 * The NVMe spec does not define a timeout period
	 *  for shutdown notification, so we just pick
	 *  5 seconds as a reasonable amount of time to
	 *  wait before proceeding.
	 */
	do {
		if (nvme_ctrlr_get_csts(ctrlr, &csts)) {
			SPDK_ERRLOG("get_csts() failed\n");
			return;
		}

		if (csts.bits.shst == SPDK_NVME_SHST_COMPLETE) {
			SPDK_TRACELOG(SPDK_TRACE_NVME, "shutdown complete\n");
			return;
		}

		nvme_delay(1000);
		ms_waited++;
	} while (ms_waited < 5000);

	SPDK_ERRLOG("did not shutdown within 5 seconds\n");
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
	cc.bits.mps = nvme_u32log2(PAGE_SIZE) - 12;

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
	case NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1:
		return "enable and wait for CSTS.RDY = 1";
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
	SPDK_TRACELOG(SPDK_TRACE_NVME, "setting state to %s (timeout %" PRIu64 " ms)\n",
		      nvme_ctrlr_state_string(ctrlr->state), timeout_in_ms);

	ctrlr->state = state;
	if (timeout_in_ms == NVME_TIMEOUT_INFINITE) {
		ctrlr->state_timeout_tsc = NVME_TIMEOUT_INFINITE;
	} else {
		ctrlr->state_timeout_tsc = spdk_get_ticks() + (timeout_in_ms * spdk_get_ticks_hz()) / 1000;
	}
}

int
spdk_nvme_ctrlr_reset(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc = 0;
	struct spdk_nvme_qpair *qpair;

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

	/* Disable all queues before disabling the controller hardware. */
	nvme_qpair_disable(ctrlr->adminq);
	TAILQ_FOREACH(qpair, &ctrlr->active_io_qpairs, tailq) {
		nvme_qpair_disable(qpair);
	}

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
	SPDK_TRACELOG(SPDK_TRACE_NVME, "transport max_xfer_size %u\n", ctrlr->max_xfer_size);
	if (ctrlr->cdata.mdts > 0) {
		ctrlr->max_xfer_size = nvme_min(ctrlr->max_xfer_size,
						ctrlr->min_page_size * (1 << (ctrlr->cdata.mdts)));
		SPDK_TRACELOG(SPDK_TRACE_NVME, "MDTS max_xfer_size %u\n", ctrlr->max_xfer_size);
	}

	return 0;
}

static int
nvme_ctrlr_set_num_qpairs(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_completion_poll_status	status;
	int					cq_allocated, sq_allocated;
	int					rc;
	uint32_t				i;

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

	/*
	 * Data in cdw0 is 0-based.
	 * Lower 16-bits indicate number of submission queues allocated.
	 * Upper 16-bits indicate number of completion queues allocated.
	 */
	sq_allocated = (status.cpl.cdw0 & 0xFFFF) + 1;
	cq_allocated = (status.cpl.cdw0 >> 16) + 1;

	ctrlr->opts.num_io_queues = nvme_min(sq_allocated, cq_allocated);

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
		SPDK_TRACELOG(SPDK_TRACE_NVME, "Controller KAS is 0 - not enabling Keep Alive\n");
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
		SPDK_TRACELOG(SPDK_TRACE_NVME, "Controller adjusted keep alive timeout to %u ms\n",
			      status.cpl.cdw0);
	}

	ctrlr->opts.keep_alive_timeout_ms = status.cpl.cdw0;

	keep_alive_interval_ms = ctrlr->opts.keep_alive_timeout_ms / 2;
	if (keep_alive_interval_ms == 0) {
		keep_alive_interval_ms = 1;
	}
	SPDK_TRACELOG(SPDK_TRACE_NVME, "Sending keep alive every %u ms\n", keep_alive_interval_ms);

	ctrlr->keep_alive_interval_ticks = (keep_alive_interval_ms * spdk_get_ticks_hz()) / UINT64_C(1000);

	/* Schedule the first Keep Alive to be sent as soon as possible. */
	ctrlr->next_keep_alive_tick = spdk_get_ticks();

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

		spdk_free(ctrlr->ns);
		ctrlr->ns = NULL;
		ctrlr->num_ns = 0;
	}

	if (ctrlr->nsdata) {
		spdk_free(ctrlr->nsdata);
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

		ctrlr->ns = spdk_zmalloc(nn * sizeof(struct spdk_nvme_ns), 64,
					 &phys_addr);
		if (ctrlr->ns == NULL) {
			goto fail;
		}

		ctrlr->nsdata = spdk_zmalloc(nn * sizeof(struct spdk_nvme_ns_data), 64,
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
	req = nvme_allocate_request_null(nvme_ctrlr_async_event_cb, aer);
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
	ctrlr->num_aers = nvme_min(NVME_MAX_ASYNC_EVENTS, (ctrlr->cdata.aerl + 1));

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
	ctrlr_proc = spdk_zmalloc(sizeof(struct spdk_nvme_ctrlr_process), 64, NULL);
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

	STAILQ_FOREACH_SAFE(req, &proc->active_reqs, stailq, tmp_req) {
		STAILQ_REMOVE(&proc->active_reqs, req, nvme_request, stailq);

		assert(req->pid == proc->pid);

		nvme_free_request(req);
	}

	TAILQ_FOREACH_SAFE(qpair, &proc->allocated_io_qpairs, per_process_tailq, tmp_qpair) {
		TAILQ_REMOVE(&proc->allocated_io_qpairs, qpair, per_process_tailq);

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
		SPDK_ERRLOG("get registers failed\n");
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
			SPDK_TRACELOG(SPDK_TRACE_NVME, "CC.EN = 1\n");
			/*
			 * Controller is currently enabled. We need to disable it to cause a reset.
			 *
			 * If CC.EN = 1 && CSTS.RDY = 0, the controller is in the process of becoming ready.
			 *  Wait for the ready bit to be 1 before disabling the controller.
			 */
			if (csts.bits.rdy == 0) {
				SPDK_TRACELOG(SPDK_TRACE_NVME, "CC.EN = 1 && CSTS.RDY = 0 - waiting for reset to complete\n");
				nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1, ready_timeout_in_ms);
				return 0;
			}

			/* CC.EN = 1 && CSTS.RDY == 1, so we can immediately disable the controller. */
			SPDK_TRACELOG(SPDK_TRACE_NVME, "Setting CC.EN = 0\n");
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
				SPDK_TRACELOG(SPDK_TRACE_NVME, "Applying quirk: delay 2 seconds before reading registers\n");
				ctrlr->sleep_timeout_tsc = spdk_get_ticks() + 2 * spdk_get_ticks_hz();
			}
			return 0;
		} else {
			if (csts.bits.rdy == 1) {
				SPDK_TRACELOG(SPDK_TRACE_NVME, "CC.EN = 0 && CSTS.RDY = 1 - waiting for shutdown to complete\n");
				/*
				 * Controller is in the process of shutting down.
				 * We need to wait for RDY to become 0.
				 */
				nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0, ready_timeout_in_ms);
				return 0;
			}

			/*
			 * Controller is currently disabled. We can jump straight to enabling it.
			 */
			SPDK_TRACELOG(SPDK_TRACE_NVME, "CC.EN = 0 && CSTS.RDY = 0 - enabling controller\n");
			rc = nvme_ctrlr_enable(ctrlr);
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1, ready_timeout_in_ms);
			return rc;
		}
		break;

	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1:
		if (csts.bits.rdy == 1) {
			SPDK_TRACELOG(SPDK_TRACE_NVME, "CC.EN = 1 && CSTS.RDY = 1 - disabling controller\n");
			/* CC.EN = 1 && CSTS.RDY = 1, so we can set CC.EN = 0 now. */
			SPDK_TRACELOG(SPDK_TRACE_NVME, "Setting CC.EN = 0\n");
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
			SPDK_TRACELOG(SPDK_TRACE_NVME, "CC.EN = 0 && CSTS.RDY = 0 - enabling controller\n");
			/* CC.EN = 0 && CSTS.RDY = 0, so we can enable the controller now. */
			SPDK_TRACELOG(SPDK_TRACE_NVME, "Setting CC.EN = 1\n");
			rc = nvme_ctrlr_enable(ctrlr);
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1, ready_timeout_in_ms);
			return rc;
		}
		break;

	case NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1:
		if (csts.bits.rdy == 1) {
			SPDK_TRACELOG(SPDK_TRACE_NVME, "CC.EN = 1 && CSTS.RDY = 1 - controller is ready\n");
			/*
			 * The controller has been enabled.
			 *  Perform the rest of initialization in nvme_ctrlr_start() serially.
			 */
			rc = nvme_ctrlr_start(ctrlr);
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_READY, NVME_TIMEOUT_INFINITE);
			return rc;
		}
		break;

	default:
		assert(0);
		nvme_ctrlr_fail(ctrlr, false);
		return -1;
	}

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
	}

	if (nvme_ctrlr_set_keep_alive_timeout(ctrlr) != 0) {
		SPDK_ERRLOG("Setting keep alive timeout failed\n");
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
	uint32_t max_io_queue_size = nvme_transport_ctrlr_get_max_io_queue_size(ctrlr);

	ctrlr->cap = *cap;

	ctrlr->min_page_size = 1u << (12 + ctrlr->cap.bits.mpsmin);

	ctrlr->opts.io_queue_size = nvme_min(ctrlr->opts.io_queue_size, ctrlr->cap.bits.mqes + 1u);
	ctrlr->opts.io_queue_size = nvme_min(ctrlr->opts.io_queue_size, max_io_queue_size);
}

void
nvme_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	while (!TAILQ_EMPTY(&ctrlr->active_io_qpairs)) {
		struct spdk_nvme_qpair *qpair = TAILQ_FIRST(&ctrlr->active_io_qpairs);

		spdk_nvme_ctrlr_free_io_qpair(qpair);
	}

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

	req = nvme_allocate_request_null(nvme_keep_alive_completion, NULL);
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
		csts.raw = 0;
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
		vs.raw = 0;
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
	if (res)
		return res;
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
	if (res)
		return res;
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
	if (res)
		return 0;
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
	if (res)
		return res;
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
	if (res)
		return res;
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
				int slot)
{
	struct spdk_nvme_fw_commit		fw_commit;
	struct nvme_completion_poll_status	status;
	int					res;
	unsigned int				size_remaining;
	unsigned int				offset;
	unsigned int				transfer;
	void					*p;

	if (size % 4) {
		SPDK_ERRLOG("spdk_nvme_ctrlr_update_firmware invalid size!\n");
		return -1;
	}

	/* Firmware download */
	size_remaining = size;
	offset = 0;
	p = payload;

	while (size_remaining > 0) {
		transfer = nvme_min(size_remaining, ctrlr->min_page_size);
		status.done = false;

		res = nvme_ctrlr_cmd_fw_image_download(ctrlr, transfer, offset, p,
						       nvme_completion_poll_cb,
						       &status);
		if (res)
			return res;

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
	fw_commit.ca = SPDK_NVME_FW_COMMIT_REPLACE_IMG;

	status.done = false;

	res = nvme_ctrlr_cmd_fw_commit(ctrlr, &fw_commit, nvme_completion_poll_cb,
				       &status);
	if (res)
		return res;

	while (status.done == false) {
		nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("nvme_ctrlr_cmd_fw_commit failed!\n");
		return -ENXIO;
	}

	return spdk_nvme_ctrlr_reset(ctrlr);
}
