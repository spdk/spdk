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

/**
 * \file
 *
 */

static int nvme_ctrlr_construct_and_submit_aer(struct spdk_nvme_ctrlr *ctrlr,
		struct nvme_async_event_request *aer);

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
		nvme_qpair_process_completions(&ctrlr->adminq, 0);
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
	struct nvme_qpair		*qpair;
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

	ctrlr->ioq = calloc(ctrlr->num_io_queues, sizeof(struct nvme_qpair));

	if (ctrlr->ioq == NULL)
		return -1;

	for (i = 0; i < ctrlr->num_io_queues; i++) {
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
	}

	return 0;
}

static void
nvme_ctrlr_fail(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t i;

	ctrlr->is_failed = true;
	nvme_qpair_fail(&ctrlr->adminq);
	for (i = 0; i < ctrlr->num_io_queues; i++) {
		nvme_qpair_fail(&ctrlr->ioq[i]);
	}
}

static int
_nvme_ctrlr_wait_for_ready(struct spdk_nvme_ctrlr *ctrlr, int desired_ready_value)
{
	int ms_waited, ready_timeout_in_ms;
	union spdk_nvme_csts_register csts;
	union spdk_nvme_cap_lo_register cap_lo;

	/* Get ready timeout value from controller, in units of 500ms. */
	cap_lo.raw = nvme_mmio_read_4(ctrlr, cap_lo.raw);
	ready_timeout_in_ms = cap_lo.bits.to * 500;

	csts.raw = nvme_mmio_read_4(ctrlr, csts);

	ms_waited = 0;

	while (csts.bits.rdy != desired_ready_value) {
		nvme_delay(1000);
		if (ms_waited++ > ready_timeout_in_ms) {
			nvme_printf(ctrlr, "controller ready did not become %d "
				    "within %d ms\n", desired_ready_value, ready_timeout_in_ms);
			return ENXIO;
		}
		csts.raw = nvme_mmio_read_4(ctrlr, csts);
	}

	return 0;
}

static int
nvme_ctrlr_wait_for_ready(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_cc_register cc;

	cc.raw = nvme_mmio_read_4(ctrlr, cc.raw);

	if (!cc.bits.en) {
		nvme_printf(ctrlr, "%s called with cc.en = 0\n", __func__);
		return ENXIO;
	}

	return _nvme_ctrlr_wait_for_ready(ctrlr, 1);
}

static void
nvme_ctrlr_disable(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_cc_register cc;
	union spdk_nvme_csts_register csts;

	cc.raw = nvme_mmio_read_4(ctrlr, cc.raw);
	csts.raw = nvme_mmio_read_4(ctrlr, csts);

	if (cc.bits.en == 1 && csts.bits.rdy == 0) {
		_nvme_ctrlr_wait_for_ready(ctrlr, 1);
	}

	cc.bits.en = 0;
	nvme_mmio_write_4(ctrlr, cc.raw, cc.raw);

	_nvme_ctrlr_wait_for_ready(ctrlr, 0);
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

	csts.raw = nvme_mmio_read_4(ctrlr, csts);
	/*
	 * The NVMe spec does not define a timeout period
	 *  for shutdown notification, so we just pick
	 *  5 seconds as a reasonable amount of time to
	 *  wait before proceeding.
	 */
	while (csts.bits.shst != SPDK_NVME_SHST_COMPLETE) {
		nvme_delay(1000);
		csts.raw = nvme_mmio_read_4(ctrlr, csts);
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
	union spdk_nvme_csts_register	csts;
	union spdk_nvme_aqa_register	aqa;

	cc.raw = nvme_mmio_read_4(ctrlr, cc.raw);
	csts.raw = nvme_mmio_read_4(ctrlr, csts);

	if (cc.bits.en == 1) {
		if (csts.bits.rdy == 1) {
			return 0;
		} else {
			return nvme_ctrlr_wait_for_ready(ctrlr);
		}
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

	return nvme_ctrlr_wait_for_ready(ctrlr);
}

static int
nvme_ctrlr_hw_reset(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t i;
	int rc;
	union spdk_nvme_cc_register cc;

	cc.raw = nvme_mmio_read_4(ctrlr, cc.raw);
	if (cc.bits.en) {
		nvme_qpair_disable(&ctrlr->adminq);
		for (i = 0; i < ctrlr->num_io_queues; i++) {
			nvme_qpair_disable(&ctrlr->ioq[i]);
		}
	} else {
		/*
		 * Ensure we do a transition from cc.en==1 to cc.en==0.
		 *  If we started disabled (cc.en==0), then we have to enable
		 *  first to get a reset.
		 */
		nvme_ctrlr_enable(ctrlr);
	}

	nvme_ctrlr_disable(ctrlr);
	rc = nvme_ctrlr_enable(ctrlr);

	return rc;
}

int
spdk_nvme_ctrlr_reset(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

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
	/* nvme_ctrlr_start() issues a reset as its first step */
	rc = nvme_ctrlr_start(ctrlr);
	if (rc) {
		nvme_ctrlr_fail(ctrlr);
	}

	ctrlr->is_resetting = false;

	nvme_mutex_unlock(&ctrlr->ctrlr_lock);

	return rc;
}

static int
nvme_ctrlr_identify(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_completion_poll_status	status;

	status.done = false;
	nvme_ctrlr_cmd_identify_controller(ctrlr, &ctrlr->cdata,
					   nvme_completion_poll_cb, &status);
	while (status.done == false) {
		nvme_qpair_process_completions(&ctrlr->adminq, 0);
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
	struct nvme_driver			*driver = &g_nvme_driver;
	struct nvme_completion_poll_status	status;
	int					cq_allocated, sq_allocated;
	uint32_t				max_io_queues;

	status.done = false;

	nvme_mutex_lock(&driver->lock);
	max_io_queues = driver->max_io_queues;
	nvme_mutex_unlock(&driver->lock);

	nvme_ctrlr_cmd_set_num_queues(ctrlr, max_io_queues,
				      nvme_completion_poll_cb, &status);
	while (status.done == false) {
		nvme_qpair_process_completions(&ctrlr->adminq, 0);
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

	ctrlr->num_io_queues = nvme_min(sq_allocated, cq_allocated);

	nvme_mutex_lock(&driver->lock);
	driver->max_io_queues = nvme_min(driver->max_io_queues, ctrlr->num_io_queues);
	nvme_mutex_unlock(&driver->lock);

	return 0;
}

static int
nvme_ctrlr_create_qpairs(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_completion_poll_status	status;
	struct nvme_qpair			*qpair;
	uint32_t				i;

	if (nvme_ctrlr_construct_io_qpairs(ctrlr)) {
		nvme_printf(ctrlr, "nvme_ctrlr_construct_io_qpairs failed!\n");
		return ENXIO;
	}

	for (i = 0; i < ctrlr->num_io_queues; i++) {
		qpair = &ctrlr->ioq[i];

		status.done = false;
		nvme_ctrlr_cmd_create_io_cq(ctrlr, qpair,
					    nvme_completion_poll_cb, &status);
		while (status.done == false) {
			nvme_qpair_process_completions(&ctrlr->adminq, 0);
		}
		if (spdk_nvme_cpl_is_error(&status.cpl)) {
			nvme_printf(ctrlr, "nvme_create_io_cq failed!\n");
			return ENXIO;
		}

		status.done = false;
		nvme_ctrlr_cmd_create_io_sq(qpair->ctrlr, qpair,
					    nvme_completion_poll_cb, &status);
		while (status.done == false) {
			nvme_qpair_process_completions(&ctrlr->adminq, 0);
		}
		if (spdk_nvme_cpl_is_error(&status.cpl)) {
			nvme_printf(ctrlr, "nvme_create_io_sq failed!\n");
			return ENXIO;
		}

		nvme_qpair_reset(qpair);
	}

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

	/*
	 * Disable timeout here, since asynchronous event requests should by
	 *  nature never be timed out.
	 */
	req->timeout = false;
	req->cmd.opc = SPDK_NVME_OPC_ASYNC_EVENT_REQUEST;
	nvme_ctrlr_submit_admin_request(ctrlr, req);

	return 0;
}

static int
nvme_ctrlr_configure_aer(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_critical_warning_state	state;
	struct nvme_async_event_request		*aer;
	uint32_t				i;
	struct nvme_completion_poll_status	status;

	status.done = false;

	state.raw = 0xFF;
	state.bits.reserved = 0;
	nvme_ctrlr_cmd_set_async_event_config(ctrlr, state, nvme_completion_poll_cb, &status);

	while (status.done == false) {
		nvme_qpair_process_completions(&ctrlr->adminq, 0);
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

int
nvme_ctrlr_start(struct spdk_nvme_ctrlr *ctrlr)
{
	if (nvme_ctrlr_hw_reset(ctrlr) != 0) {
		return -1;
	}

	nvme_qpair_reset(&ctrlr->adminq);

	nvme_qpair_enable(&ctrlr->adminq);

	if (nvme_ctrlr_identify(ctrlr) != 0) {
		return -1;
	}

	if (nvme_ctrlr_set_num_qpairs(ctrlr) != 0) {
		return -1;
	}

	if (nvme_ctrlr_create_qpairs(ctrlr) != 0) {
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

	return 0;
}

static int
nvme_ctrlr_free_bars(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc = 0;
	void *addr = (void *)ctrlr->regs;

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

	ctrlr->devhandle = devhandle;

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

	nvme_mutex_init_recursive(&ctrlr->ctrlr_lock);

	return 0;
}

void
nvme_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t	i;

	nvme_ctrlr_disable(ctrlr);
	nvme_ctrlr_shutdown(ctrlr);

	nvme_ctrlr_destruct_namespaces(ctrlr);

	for (i = 0; i < ctrlr->num_io_queues; i++) {
		nvme_qpair_destroy(&ctrlr->ioq[i]);
	}

	free(ctrlr->ioq);

	nvme_qpair_destroy(&ctrlr->adminq);

	nvme_ctrlr_free_bars(ctrlr);
	nvme_mutex_destroy(&ctrlr->ctrlr_lock);
}

void
nvme_ctrlr_submit_admin_request(struct spdk_nvme_ctrlr *ctrlr,
				struct nvme_request *req)
{
	nvme_qpair_submit_request(&ctrlr->adminq, req);
}

void
nvme_ctrlr_submit_io_request(struct spdk_nvme_ctrlr *ctrlr,
			     struct nvme_request *req)
{
	struct nvme_qpair       *qpair;

	nvme_assert(nvme_thread_ioq_index >= 0, ("no ioq_index assigned for thread\n"));
	qpair = &ctrlr->ioq[nvme_thread_ioq_index];

	nvme_qpair_submit_request(qpair, req);
}

int32_t
spdk_nvme_ctrlr_process_io_completions(struct spdk_nvme_ctrlr *ctrlr, uint32_t max_completions)
{
	nvme_assert(nvme_thread_ioq_index >= 0, ("no ioq_index assigned for thread\n"));
	return nvme_qpair_process_completions(&ctrlr->ioq[nvme_thread_ioq_index], max_completions);
}

int32_t
spdk_nvme_ctrlr_process_admin_completions(struct spdk_nvme_ctrlr *ctrlr)
{
	int32_t num_completions;

	nvme_mutex_lock(&ctrlr->ctrlr_lock);
	num_completions = nvme_qpair_process_completions(&ctrlr->adminq, 0);
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
