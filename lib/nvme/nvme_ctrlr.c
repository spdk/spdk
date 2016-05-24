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
#include "spdk/pci.h"

static int nvme_ctrlr_construct_and_submit_aer(struct spdk_nvme_ctrlr *ctrlr,
		struct nvme_async_event_request *aer);


void
spdk_nvme_ctrlr_opts_set_defaults(struct spdk_nvme_ctrlr_opts *opts)
{
	opts->num_io_queues = DEFAULT_MAX_IO_QUEUES;
	opts->use_cmb_sqs = false;
}

static int
spdk_nvme_ctrlr_create_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_completion_poll_status	status;
	int rc;

	status.done = false;
	rc = nvme_ctrlr_cmd_create_io_cq(ctrlr, qpair, nvme_completion_poll_cb, &status);
	if (rc != 0) {
		return rc;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		nvme_printf(ctrlr, "nvme_create_io_cq failed!\n");
		return -1;
	}

	status.done = false;
	rc = nvme_ctrlr_cmd_create_io_sq(qpair->ctrlr, qpair, nvme_completion_poll_cb, &status);
	if (rc != 0) {
		return rc;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		nvme_printf(ctrlr, "nvme_create_io_sq failed!\n");
		/* Attempt to delete the completion queue */
		status.done = false;
		rc = nvme_ctrlr_cmd_delete_io_cq(qpair->ctrlr, qpair, nvme_completion_poll_cb, &status);
		if (rc != 0) {
			return -1;
		}
		while (status.done == false) {
			spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
		}
		return -1;
	}

	nvme_qpair_reset(qpair);

	return 0;
}

struct spdk_nvme_qpair *
spdk_nvme_ctrlr_alloc_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
			       enum spdk_nvme_qprio qprio)
{
	struct spdk_nvme_qpair			*qpair;

	/* Only the low 2 bits (values 0, 1, 2, 3) of QPRIO are valid. */
	if ((qprio & 3) != qprio) {
		return NULL;
	}

	nvme_mutex_lock(&ctrlr->ctrlr_lock);

	/*
	 * Get the first available qpair structure.
	 */
	qpair = TAILQ_FIRST(&ctrlr->free_io_qpairs);
	if (qpair == NULL) {
		/* No free queue IDs */
		nvme_mutex_unlock(&ctrlr->ctrlr_lock);
		return NULL;
	}

	/*
	 * At this point, qpair contains a preallocated submission and completion queue and a
	 *  unique queue ID, but it is not yet created on the controller.
	 *
	 * Fill out the submission queue priority and send out the Create I/O Queue commands.
	 */
	qpair->qprio = qprio;
	if (spdk_nvme_ctrlr_create_qpair(ctrlr, qpair) != 0) {
		/*
		 * spdk_nvme_ctrlr_create_qpair() failed, so the qpair structure is still unused.
		 * Exit here so we don't insert it into the active_io_qpairs list.
		 */
		nvme_mutex_unlock(&ctrlr->ctrlr_lock);
		return NULL;
	}
	TAILQ_REMOVE(&ctrlr->free_io_qpairs, qpair, tailq);
	TAILQ_INSERT_TAIL(&ctrlr->active_io_qpairs, qpair, tailq);

	nvme_mutex_unlock(&ctrlr->ctrlr_lock);

	return qpair;
}

int
spdk_nvme_ctrlr_free_io_qpair(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_completion_poll_status status;
	int rc;

	if (qpair == NULL) {
		return 0;
	}

	ctrlr = qpair->ctrlr;

	nvme_mutex_lock(&ctrlr->ctrlr_lock);

	/* Delete the I/O submission queue and then the completion queue */

	status.done = false;
	rc = nvme_ctrlr_cmd_delete_io_sq(ctrlr, qpair, nvme_completion_poll_cb, &status);
	if (rc != 0) {
		nvme_mutex_unlock(&ctrlr->ctrlr_lock);
		return rc;
	}
	while (status.done == false) {
		spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		nvme_mutex_unlock(&ctrlr->ctrlr_lock);
		return -1;
	}

	status.done = false;
	rc = nvme_ctrlr_cmd_delete_io_cq(ctrlr, qpair, nvme_completion_poll_cb, &status);
	if (rc != 0) {
		nvme_mutex_unlock(&ctrlr->ctrlr_lock);
		return rc;
	}
	while (status.done == false) {
		spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		nvme_mutex_unlock(&ctrlr->ctrlr_lock);
		return -1;
	}

	TAILQ_REMOVE(&ctrlr->active_io_qpairs, qpair, tailq);
	TAILQ_INSERT_HEAD(&ctrlr->free_io_qpairs, qpair, tailq);

	nvme_mutex_unlock(&ctrlr->ctrlr_lock);
	return 0;
}

static void
nvme_ctrlr_construct_intel_support_log_page_list(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_intel_log_page_directory *log_page_directory)
{
	struct spdk_pci_device *dev;
	struct pci_id pci_id;

	if (ctrlr->cdata.vid != SPDK_PCI_VID_INTEL || log_page_directory == NULL)
		return;

	dev = ctrlr->devhandle;
	pci_id.vendor_id = spdk_pci_device_get_vendor_id(dev);
	pci_id.dev_id = spdk_pci_device_get_device_id(dev);
	pci_id.sub_vendor_id = spdk_pci_device_get_subvendor_id(dev);
	pci_id.sub_dev_id = spdk_pci_device_get_subdevice_id(dev);

	ctrlr->log_page_supported[SPDK_NVME_INTEL_LOG_PAGE_DIRECTORY] = true;

	if (log_page_directory->read_latency_log_len ||
	    nvme_intel_has_quirk(&pci_id, NVME_INTEL_QUIRK_READ_LATENCY)) {
		ctrlr->log_page_supported[SPDK_NVME_INTEL_LOG_READ_CMD_LATENCY] = true;
	}
	if (log_page_directory->write_latency_log_len ||
	    nvme_intel_has_quirk(&pci_id, NVME_INTEL_QUIRK_WRITE_LATENCY)) {
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

	log_page_directory = nvme_malloc("nvme_log_page_directory",
					 sizeof(struct spdk_nvme_intel_log_page_directory),
					 64, &phys_addr);
	if (log_page_directory == NULL) {
		nvme_printf(NULL, "could not allocate log_page_directory\n");
		return ENXIO;
	}

	status.done = false;
	spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_INTEL_LOG_PAGE_DIRECTORY, SPDK_NVME_GLOBAL_NS_TAG,
					 log_page_directory, sizeof(struct spdk_nvme_intel_log_page_directory),
					 nvme_completion_poll_cb,
					 &status);
	while (status.done == false) {
		spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		nvme_free(log_page_directory);
		nvme_printf(ctrlr, "nvme_ctrlr_cmd_get_log_page failed!\n");
		return ENXIO;
	}

	nvme_ctrlr_construct_intel_support_log_page_list(ctrlr, log_page_directory);
	nvme_free(log_page_directory);
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

static int
nvme_ctrlr_construct_admin_qpair(struct spdk_nvme_ctrlr *ctrlr)
{
	return nvme_qpair_construct(&ctrlr->adminq,
				    0, /* qpair ID */
				    NVME_ADMIN_ENTRIES,
				    NVME_ADMIN_TRACKERS,
				    ctrlr);
}

static int
nvme_ctrlr_construct_io_qpairs(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_qpair		*qpair;
	union spdk_nvme_cap_lo_register	cap_lo;
	uint32_t			i, num_entries, num_trackers;
	int				rc;

	if (ctrlr->ioq != NULL) {
		/*
		 * io_qpairs were already constructed, so just return.
		 *  This typically happens when the controller is
		 *  initialized a second (or subsequent) time after a
		 *  controller reset.
		 */
		return 0;
	}

	/*
	 * NVMe spec sets a hard limit of 64K max entries, but
	 *  devices may specify a smaller limit, so we need to check
	 *  the MQES field in the capabilities register.
	 */
	cap_lo.raw = nvme_mmio_read_4(ctrlr, cap_lo.raw);
	num_entries = nvme_min(NVME_IO_ENTRIES, cap_lo.bits.mqes + 1);

	/*
	 * No need to have more trackers than entries in the submit queue.
	 *  Note also that for a queue size of N, we can only have (N-1)
	 *  commands outstanding, hence the "-1" here.
	 */
	num_trackers = nvme_min(NVME_IO_TRACKERS, (num_entries - 1));

	ctrlr->max_xfer_size = NVME_MAX_XFER_SIZE;

	ctrlr->ioq = calloc(ctrlr->opts.num_io_queues, sizeof(struct spdk_nvme_qpair));

	if (ctrlr->ioq == NULL)
		return -1;

	for (i = 0; i < ctrlr->opts.num_io_queues; i++) {
		qpair = &ctrlr->ioq[i];

		/*
		 * Admin queue has ID=0. IO queues start at ID=1 -
		 *  hence the 'i+1' here.
		 */
		rc = nvme_qpair_construct(qpair,
					  i + 1, /* qpair ID */
					  num_entries,
					  num_trackers,
					  ctrlr);
		if (rc)
			return -1;

		TAILQ_INSERT_TAIL(&ctrlr->free_io_qpairs, qpair, tailq);
	}

	return 0;
}

static void
nvme_ctrlr_fail(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t i;

	ctrlr->is_failed = true;
	nvme_qpair_fail(&ctrlr->adminq);
	if (ctrlr->ioq) {
		for (i = 0; i < ctrlr->opts.num_io_queues; i++) {
			nvme_qpair_fail(&ctrlr->ioq[i]);
		}
	}
}

static void
nvme_ctrlr_shutdown(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_cc_register	cc;
	union spdk_nvme_csts_register	csts;
	int				ms_waited = 0;

	cc.raw = nvme_mmio_read_4(ctrlr, cc.raw);
	cc.bits.shn = SPDK_NVME_SHN_NORMAL;
	nvme_mmio_write_4(ctrlr, cc.raw, cc.raw);

	csts.raw = nvme_mmio_read_4(ctrlr, csts.raw);
	/*
	 * The NVMe spec does not define a timeout period
	 *  for shutdown notification, so we just pick
	 *  5 seconds as a reasonable amount of time to
	 *  wait before proceeding.
	 */
	while (csts.bits.shst != SPDK_NVME_SHST_COMPLETE) {
		nvme_delay(1000);
		csts.raw = nvme_mmio_read_4(ctrlr, csts.raw);
		if (ms_waited++ >= 5000)
			break;
	}
	if (csts.bits.shst != SPDK_NVME_SHST_COMPLETE)
		nvme_printf(ctrlr, "did not shutdown within 5 seconds\n");
}

static int
nvme_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_cc_register	cc;
	union spdk_nvme_aqa_register	aqa;

	cc.raw = nvme_mmio_read_4(ctrlr, cc.raw);

	if (cc.bits.en != 0) {
		nvme_printf(ctrlr, "%s called with CC.EN = 1\n", __func__);
		return EINVAL;
	}

	nvme_mmio_write_8(ctrlr, asq, ctrlr->adminq.cmd_bus_addr);
	nvme_mmio_write_8(ctrlr, acq, ctrlr->adminq.cpl_bus_addr);

	aqa.raw = 0;
	/* acqs and asqs are 0-based. */
	aqa.bits.acqs = ctrlr->adminq.num_entries - 1;
	aqa.bits.asqs = ctrlr->adminq.num_entries - 1;
	nvme_mmio_write_4(ctrlr, aqa.raw, aqa.raw);

	cc.bits.en = 1;
	cc.bits.css = 0;
	cc.bits.ams = 0;
	cc.bits.shn = 0;
	cc.bits.iosqes = 6; /* SQ entry size == 64 == 2^6 */
	cc.bits.iocqes = 4; /* CQ entry size == 16 == 2^4 */

	/* Page size is 2 ^ (12 + mps). */
	cc.bits.mps = nvme_u32log2(PAGE_SIZE) - 12;

	nvme_mmio_write_4(ctrlr, cc.raw, cc.raw);

	return 0;
}

static void
nvme_ctrlr_set_state(struct spdk_nvme_ctrlr *ctrlr, enum nvme_ctrlr_state state,
		     uint64_t timeout_in_ms)
{
	ctrlr->state = state;
	if (timeout_in_ms == NVME_TIMEOUT_INFINITE) {
		ctrlr->state_timeout_tsc = NVME_TIMEOUT_INFINITE;
	} else {
		ctrlr->state_timeout_tsc = nvme_get_tsc() + (timeout_in_ms * nvme_get_tsc_hz()) / 1000;
	}
}

int
spdk_nvme_ctrlr_reset(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc = 0;
	uint32_t i;
	struct spdk_nvme_qpair *qpair;

	nvme_mutex_lock(&ctrlr->ctrlr_lock);

	if (ctrlr->is_resetting || ctrlr->is_failed) {
		/*
		 * Controller is already resetting or has failed.  Return
		 *  immediately since there is no need to kick off another
		 *  reset in these cases.
		 */
		nvme_mutex_unlock(&ctrlr->ctrlr_lock);
		return 0;
	}

	ctrlr->is_resetting = true;

	nvme_printf(ctrlr, "resetting controller\n");

	/* Disable all queues before disabling the controller hardware. */
	nvme_qpair_disable(&ctrlr->adminq);
	for (i = 0; i < ctrlr->opts.num_io_queues; i++) {
		nvme_qpair_disable(&ctrlr->ioq[i]);
	}

	/* Set the state back to INIT to cause a full hardware reset. */
	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_INIT, NVME_TIMEOUT_INFINITE);

	while (ctrlr->state != NVME_CTRLR_STATE_READY) {
		if (nvme_ctrlr_process_init(ctrlr) != 0) {
			nvme_printf(ctrlr, "%s: controller reinitialization failed\n", __func__);
			nvme_ctrlr_fail(ctrlr);
			rc = -1;
			break;
		}
	}

	if (!ctrlr->is_failed) {
		/* Reinitialize qpairs */
		TAILQ_FOREACH(qpair, &ctrlr->active_io_qpairs, tailq) {
			if (spdk_nvme_ctrlr_create_qpair(ctrlr, qpair) != 0) {
				nvme_ctrlr_fail(ctrlr);
				rc = -1;
			}
		}
	}

	ctrlr->is_resetting = false;

	nvme_mutex_unlock(&ctrlr->ctrlr_lock);

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
		spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		nvme_printf(ctrlr, "nvme_identify_controller failed!\n");
		return ENXIO;
	}

	/*
	 * Use MDTS to ensure our default max_xfer_size doesn't exceed what the
	 *  controller supports.
	 */
	if (ctrlr->cdata.mdts > 0) {
		ctrlr->max_xfer_size = nvme_min(ctrlr->max_xfer_size,
						ctrlr->min_page_size * (1 << (ctrlr->cdata.mdts)));
	}

	return 0;
}

static int
nvme_ctrlr_set_num_qpairs(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_completion_poll_status	status;
	int					cq_allocated, sq_allocated;
	int					rc;

	status.done = false;

	if (ctrlr->opts.num_io_queues > SPDK_NVME_MAX_IO_QUEUES) {
		nvme_printf(ctrlr, "Limiting requested num_io_queues %u to max %d\n",
			    ctrlr->opts.num_io_queues, SPDK_NVME_MAX_IO_QUEUES);
		ctrlr->opts.num_io_queues = SPDK_NVME_MAX_IO_QUEUES;
	} else if (ctrlr->opts.num_io_queues < 1) {
		nvme_printf(ctrlr, "Requested num_io_queues 0, increasing to 1\n");
		ctrlr->opts.num_io_queues = 1;
	}

	rc = nvme_ctrlr_cmd_set_num_queues(ctrlr, ctrlr->opts.num_io_queues,
					   nvme_completion_poll_cb, &status);
	if (rc != 0) {
		return rc;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		nvme_printf(ctrlr, "nvme_set_num_queues failed!\n");
		return ENXIO;
	}

	/*
	 * Data in cdw0 is 0-based.
	 * Lower 16-bits indicate number of submission queues allocated.
	 * Upper 16-bits indicate number of completion queues allocated.
	 */
	sq_allocated = (status.cpl.cdw0 & 0xFFFF) + 1;
	cq_allocated = (status.cpl.cdw0 >> 16) + 1;

	ctrlr->opts.num_io_queues = nvme_min(sq_allocated, cq_allocated);

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

		free(ctrlr->ns);
		ctrlr->ns = NULL;
		ctrlr->num_ns = 0;
	}

	if (ctrlr->nsdata) {
		nvme_free(ctrlr->nsdata);
		ctrlr->nsdata = NULL;
	}
}

static int
nvme_ctrlr_construct_namespaces(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t i, nn = ctrlr->cdata.nn;
	uint64_t phys_addr = 0;

	if (nn == 0) {
		nvme_printf(ctrlr, "controller has 0 namespaces\n");
		return -1;
	}

	/* ctrlr->num_ns may be 0 (startup) or a different number of namespaces (reset),
	 * so check if we need to reallocate.
	 */
	if (nn != ctrlr->num_ns) {
		nvme_ctrlr_destruct_namespaces(ctrlr);

		ctrlr->ns = calloc(nn, sizeof(struct spdk_nvme_ns));
		if (ctrlr->ns == NULL) {
			goto fail;
		}

		ctrlr->nsdata = nvme_malloc("nvme_namespaces",
					    nn * sizeof(struct spdk_nvme_ns_data), 64,
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
		nvme_printf(ctrlr, "resubmitting AER failed!\n");
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
		spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		nvme_printf(ctrlr, "nvme_ctrlr_cmd_set_async_event_config failed!\n");
		return ENXIO;
	}

	/* aerl is a zero-based value, so we need to add 1 here. */
	ctrlr->num_aers = nvme_min(NVME_MAX_ASYNC_EVENTS, (ctrlr->cdata.aerl + 1));

	for (i = 0; i < ctrlr->num_aers; i++) {
		aer = &ctrlr->aer[i];
		if (nvme_ctrlr_construct_and_submit_aer(ctrlr, aer)) {
			nvme_printf(ctrlr, "nvme_ctrlr_construct_and_submit_aer failed!\n");
			return -1;
		}
	}

	return 0;
}

/**
 * This function will be called repeatedly during initialization until the controller is ready.
 */
int
nvme_ctrlr_process_init(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_cc_register cc;
	union spdk_nvme_csts_register csts;
	union spdk_nvme_cap_lo_register cap_lo;
	uint32_t ready_timeout_in_ms;
	int rc;

	cc.raw = nvme_mmio_read_4(ctrlr, cc.raw);
	csts.raw = nvme_mmio_read_4(ctrlr, csts.raw);
	cap_lo.raw = nvme_mmio_read_4(ctrlr, cap_lo.raw);

	ready_timeout_in_ms = 500 * cap_lo.bits.to;

	/*
	 * Check if the current initialization step is done or has timed out.
	 */
	switch (ctrlr->state) {
	case NVME_CTRLR_STATE_INIT:
		/* Begin the hardware initialization by making sure the controller is disabled. */
		if (cc.bits.en) {
			/*
			 * Controller is currently enabled. We need to disable it to cause a reset.
			 *
			 * If CC.EN = 1 && CSTS.RDY = 0, the controller is in the process of becoming ready.
			 *  Wait for the ready bit to be 1 before disabling the controller.
			 */
			if (csts.bits.rdy == 0) {
				nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1, ready_timeout_in_ms);
				return 0;
			}

			/* CC.EN = 1 && CSTS.RDY == 1, so we can immediately disable the controller. */
			cc.bits.en = 0;
			nvme_mmio_write_4(ctrlr, cc.raw, cc.raw);
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0, ready_timeout_in_ms);
			return 0;
		} else {
			if (csts.bits.rdy == 1) {
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
			nvme_ctrlr_enable(ctrlr);
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1, ready_timeout_in_ms);
			return 0;
		}
		break;

	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1:
		if (csts.bits.rdy == 1) {
			/* CC.EN = 1 && CSTS.RDY = 1, so we can set CC.EN = 0 now. */
			cc.bits.en = 0;
			nvme_mmio_write_4(ctrlr, cc.raw, cc.raw);
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0, ready_timeout_in_ms);
			return 0;
		}
		break;

	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0:
		if (csts.bits.rdy == 0) {
			/* CC.EN = 0 && CSTS.RDY = 0, so we can enable the controller now. */
			nvme_ctrlr_enable(ctrlr);
			nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1, ready_timeout_in_ms);
			return 0;
		}
		break;

	case NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1:
		if (csts.bits.rdy == 1) {
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
		nvme_assert(0, ("unhandled ctrlr state %d\n", ctrlr->state));
		nvme_ctrlr_fail(ctrlr);
		return -1;
	}

	if (ctrlr->state_timeout_tsc != NVME_TIMEOUT_INFINITE &&
	    nvme_get_tsc() > ctrlr->state_timeout_tsc) {
		nvme_printf(ctrlr, "Initialization timed out in state %d\n", ctrlr->state);
		nvme_ctrlr_fail(ctrlr);
		return -1;
	}

	return 0;
}

int
nvme_ctrlr_start(struct spdk_nvme_ctrlr *ctrlr)
{
	nvme_qpair_reset(&ctrlr->adminq);

	nvme_qpair_enable(&ctrlr->adminq);

	if (nvme_ctrlr_identify(ctrlr) != 0) {
		return -1;
	}

	if (nvme_ctrlr_set_num_qpairs(ctrlr) != 0) {
		return -1;
	}

	if (nvme_ctrlr_construct_io_qpairs(ctrlr)) {
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

	return 0;
}

static void
nvme_ctrlr_map_cmb(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;
	void *addr;
	uint32_t bir;
	union spdk_nvme_cmbsz_register cmbsz;
	union spdk_nvme_cmbloc_register cmbloc;
	uint64_t size, unit_size, offset, bar_size, bar_phys_addr;

	cmbsz.raw = nvme_mmio_read_4(ctrlr, cmbsz.raw);
	cmbloc.raw = nvme_mmio_read_4(ctrlr, cmbloc.raw);
	if (!cmbsz.bits.sz)
		goto exit;

	bir = cmbloc.bits.bir;
	/* Values 0 2 3 4 5 are valid for BAR */
	if (bir > 5 || bir == 1)
		goto exit;

	/* unit size for 4KB/64KB/1MB/16MB/256MB/4GB/64GB */
	unit_size = (uint64_t)1 << (12 + 4 * cmbsz.bits.szu);
	/* controller memory buffer size in Bytes */
	size = unit_size * cmbsz.bits.sz;
	/* controller memory buffer offset from BAR in Bytes */
	offset = unit_size * cmbloc.bits.ofst;

	nvme_pcicfg_get_bar_addr_len(ctrlr->devhandle, bir, &bar_phys_addr, &bar_size);

	if (offset > bar_size)
		goto exit;

	if (size > bar_size - offset)
		goto exit;

	rc = nvme_pcicfg_map_bar_write_combine(ctrlr->devhandle, bir, &addr);
	if (addr == NULL || (rc != 0))
		goto exit;

	ctrlr->cmb_bar_virt_addr = addr;
	ctrlr->cmb_bar_phys_addr = bar_phys_addr;
	ctrlr->cmb_size = size;
	ctrlr->cmb_current_offset = offset;

	if (!cmbsz.bits.sqs) {
		ctrlr->opts.use_cmb_sqs = false;
	}

	return;
exit:
	ctrlr->cmb_bar_virt_addr = NULL;
	ctrlr->opts.use_cmb_sqs = false;
	return;
}

static int
nvme_ctrlr_unmap_cmb(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc = 0;
	union spdk_nvme_cmbloc_register cmbloc;
	void *addr = ctrlr->cmb_bar_virt_addr;

	if (addr) {
		cmbloc.raw = nvme_mmio_read_4(ctrlr, cmbloc.raw);
		rc = nvme_pcicfg_unmap_bar(ctrlr->devhandle, cmbloc.bits.bir, addr);
	}
	return rc;
}

int
nvme_ctrlr_alloc_cmb(struct spdk_nvme_ctrlr *ctrlr, uint64_t length, uint64_t aligned,
		     uint64_t *offset)
{
	uint64_t round_offset;

	round_offset = ctrlr->cmb_current_offset;
	round_offset = (round_offset + (aligned - 1)) & ~(aligned - 1);

	if (round_offset + length > ctrlr->cmb_size)
		return -1;

	*offset = round_offset;
	ctrlr->cmb_current_offset = round_offset + length;

	return 0;
}

static int
nvme_ctrlr_allocate_bars(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;
	void *addr;

	rc = nvme_pcicfg_map_bar(ctrlr->devhandle, 0, 0 /* writable */, &addr);
	ctrlr->regs = (volatile struct spdk_nvme_registers *)addr;
	if ((ctrlr->regs == NULL) || (rc != 0)) {
		nvme_printf(ctrlr, "pci_device_map_range failed with error code %d\n", rc);
		return -1;
	}

	nvme_ctrlr_map_cmb(ctrlr);

	return 0;
}

static int
nvme_ctrlr_free_bars(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc = 0;
	void *addr = (void *)ctrlr->regs;

	rc = nvme_ctrlr_unmap_cmb(ctrlr);
	if (rc != 0) {
		nvme_printf(ctrlr, "nvme_ctrlr_unmap_cmb failed with error code %d\n", rc);
		return -1;
	}

	if (addr) {
		rc = nvme_pcicfg_unmap_bar(ctrlr->devhandle, 0, addr);
	}
	return rc;
}

int
nvme_ctrlr_construct(struct spdk_nvme_ctrlr *ctrlr, void *devhandle)
{
	union spdk_nvme_cap_hi_register	cap_hi;
	uint32_t			cmd_reg;
	int				status;
	int				rc;

	nvme_ctrlr_set_state(ctrlr, NVME_CTRLR_STATE_INIT, NVME_TIMEOUT_INFINITE);
	ctrlr->devhandle = devhandle;
	ctrlr->flags = 0;

	status = nvme_ctrlr_allocate_bars(ctrlr);
	if (status != 0) {
		return status;
	}

	/* Enable PCI busmaster. */
	nvme_pcicfg_read32(devhandle, &cmd_reg, 4);
	cmd_reg |= 0x4;
	nvme_pcicfg_write32(devhandle, cmd_reg, 4);

	cap_hi.raw = nvme_mmio_read_4(ctrlr, cap_hi.raw);

	/* Doorbell stride is 2 ^ (dstrd + 2),
	 * but we want multiples of 4, so drop the + 2 */
	ctrlr->doorbell_stride_u32 = 1 << cap_hi.bits.dstrd;

	ctrlr->min_page_size = 1 << (12 + cap_hi.bits.mpsmin);

	rc = nvme_ctrlr_construct_admin_qpair(ctrlr);
	if (rc)
		return rc;

	ctrlr->is_resetting = false;
	ctrlr->is_failed = false;

	TAILQ_INIT(&ctrlr->free_io_qpairs);
	TAILQ_INIT(&ctrlr->active_io_qpairs);

	nvme_mutex_init_recursive(&ctrlr->ctrlr_lock);

	return 0;
}

void
nvme_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t	i;

	while (!TAILQ_EMPTY(&ctrlr->active_io_qpairs)) {
		struct spdk_nvme_qpair *qpair = TAILQ_FIRST(&ctrlr->active_io_qpairs);

		spdk_nvme_ctrlr_free_io_qpair(qpair);
	}

	nvme_ctrlr_shutdown(ctrlr);

	nvme_ctrlr_destruct_namespaces(ctrlr);
	if (ctrlr->ioq) {
		for (i = 0; i < ctrlr->opts.num_io_queues; i++) {
			nvme_qpair_destroy(&ctrlr->ioq[i]);
		}
	}

	free(ctrlr->ioq);

	nvme_qpair_destroy(&ctrlr->adminq);

	nvme_ctrlr_free_bars(ctrlr);
	nvme_mutex_destroy(&ctrlr->ctrlr_lock);
}

int
nvme_ctrlr_submit_admin_request(struct spdk_nvme_ctrlr *ctrlr,
				struct nvme_request *req)
{
	return nvme_qpair_submit_request(&ctrlr->adminq, req);
}

int32_t
spdk_nvme_ctrlr_process_admin_completions(struct spdk_nvme_ctrlr *ctrlr)
{
	int32_t num_completions;

	nvme_mutex_lock(&ctrlr->ctrlr_lock);
	num_completions = spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
	nvme_mutex_unlock(&ctrlr->ctrlr_lock);

	return num_completions;
}

const struct spdk_nvme_ctrlr_data *
spdk_nvme_ctrlr_get_data(struct spdk_nvme_ctrlr *ctrlr)
{
	return &ctrlr->cdata;
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
		nvme_mutex_lock(&ctrlr->ctrlr_lock);
		spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
		nvme_mutex_unlock(&ctrlr->ctrlr_lock);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		nvme_printf(ctrlr, "spdk_nvme_ctrlr_attach_ns failed!\n");
		return ENXIO;
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
		nvme_mutex_lock(&ctrlr->ctrlr_lock);
		spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
		nvme_mutex_unlock(&ctrlr->ctrlr_lock);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		nvme_printf(ctrlr, "spdk_nvme_ctrlr_detach_ns failed!\n");
		return ENXIO;
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
		nvme_mutex_lock(&ctrlr->ctrlr_lock);
		spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
		nvme_mutex_unlock(&ctrlr->ctrlr_lock);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		nvme_printf(ctrlr, "spdk_nvme_ctrlr_create_ns failed!\n");
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
		nvme_mutex_lock(&ctrlr->ctrlr_lock);
		spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
		nvme_mutex_unlock(&ctrlr->ctrlr_lock);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		nvme_printf(ctrlr, "spdk_nvme_ctrlr_delete_ns failed!\n");
		return ENXIO;
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
		nvme_mutex_lock(&ctrlr->ctrlr_lock);
		spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
		nvme_mutex_unlock(&ctrlr->ctrlr_lock);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		nvme_printf(ctrlr, "spdk_nvme_ctrlr_format failed!\n");
		return ENXIO;
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
		nvme_printf(ctrlr, "spdk_nvme_ctrlr_update_firmware invalid size!\n");
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
			nvme_mutex_lock(&ctrlr->ctrlr_lock);
			spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
			nvme_mutex_unlock(&ctrlr->ctrlr_lock);
		}
		if (spdk_nvme_cpl_is_error(&status.cpl)) {
			nvme_printf(ctrlr, "spdk_nvme_ctrlr_fw_image_download failed!\n");
			return ENXIO;
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
		nvme_mutex_lock(&ctrlr->ctrlr_lock);
		spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
		nvme_mutex_unlock(&ctrlr->ctrlr_lock);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		nvme_printf(ctrlr, "nvme_ctrlr_cmd_fw_commit failed!\n");
		return ENXIO;
	}

	return spdk_nvme_ctrlr_reset(ctrlr);
}
