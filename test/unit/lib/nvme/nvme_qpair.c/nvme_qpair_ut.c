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

#include "spdk_cunit.h"

#include "lib/test_env.c"

bool trace_flag = false;
#define SPDK_TRACE_NVME trace_flag

#include "nvme/nvme_qpair.c"

struct nvme_driver _g_nvme_driver = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
};

struct nvme_request *
nvme_allocate_request(struct spdk_nvme_qpair *qpair,
		      const struct nvme_payload *payload, uint32_t payload_size,
		      spdk_nvme_cmd_cb cb_fn,
		      void *cb_arg)
{
	struct nvme_request *req;

	req = STAILQ_FIRST(&qpair->free_req);
	if (req == NULL) {
		return NULL;
	}

	STAILQ_REMOVE_HEAD(&qpair->free_req, stailq);

	/*
	 * Only memset up to (but not including) the children
	 *  TAILQ_ENTRY.  children, and following members, are
	 *  only used as part of I/O splitting so we avoid
	 *  memsetting them until it is actually needed.
	 *  They will be initialized in nvme_request_add_child()
	 *  if the request is split.
	 */
	memset(req, 0, offsetof(struct nvme_request, children));
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->payload = *payload;
	req->payload_size = payload_size;
	req->qpair = qpair;
	req->pid = getpid();

	return req;
}

struct nvme_request *
nvme_allocate_request_contig(struct spdk_nvme_qpair *qpair, void *buffer, uint32_t payload_size,
			     spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_payload payload;

	payload.type = NVME_PAYLOAD_TYPE_CONTIG;
	payload.u.contig = buffer;

	return nvme_allocate_request(qpair, &payload, payload_size, cb_fn, cb_arg);
}

struct nvme_request *
nvme_allocate_request_null(struct spdk_nvme_qpair *qpair, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return nvme_allocate_request_contig(qpair, NULL, 0, cb_fn, cb_arg);
}

void
nvme_free_request(struct nvme_request *req)
{
	SPDK_CU_ASSERT_FATAL(req != NULL);
	SPDK_CU_ASSERT_FATAL(req->qpair != NULL);
	STAILQ_INSERT_HEAD(&req->qpair->free_req, req, stailq);
}

void
nvme_request_remove_child(struct nvme_request *parent,
			  struct nvme_request *child)
{
	parent->num_children--;
	TAILQ_REMOVE(&parent->children, child, child_tailq);
}

int
nvme_transport_qpair_enable(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

int
nvme_transport_qpair_disable(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

int
nvme_transport_qpair_fail(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

int
nvme_transport_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	// TODO
	return 0;
}

int32_t
nvme_transport_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	// TODO
	return 0;
}

int
spdk_nvme_ctrlr_free_io_qpair(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

static void
prepare_submit_request_test(struct spdk_nvme_qpair *qpair,
			    struct spdk_nvme_ctrlr *ctrlr)
{
	memset(ctrlr, 0, sizeof(*ctrlr));
	ctrlr->free_io_qids = NULL;
	TAILQ_INIT(&ctrlr->active_io_qpairs);
	TAILQ_INIT(&ctrlr->active_procs);
	nvme_qpair_init(qpair, 1, ctrlr, 0, 32);
}

static void
cleanup_submit_request_test(struct spdk_nvme_qpair *qpair)
{
	free(qpair->req_buf);
}

static void
expected_success_callback(void *arg, const struct spdk_nvme_cpl *cpl)
{
	CU_ASSERT(!spdk_nvme_cpl_is_error(cpl));
}

static void
expected_failure_callback(void *arg, const struct spdk_nvme_cpl *cpl)
{
	CU_ASSERT(spdk_nvme_cpl_is_error(cpl));
}

static void
test3(void)
{
	struct spdk_nvme_qpair		qpair = {};
	struct nvme_request		*req;
	struct spdk_nvme_ctrlr		ctrlr = {};

	prepare_submit_request_test(&qpair, &ctrlr);

	req = nvme_allocate_request_null(&qpair, expected_success_callback, NULL);
	SPDK_CU_ASSERT_FATAL(req != NULL);

	CU_ASSERT(nvme_qpair_submit_request(&qpair, req) == 0);

	nvme_free_request(req);

	cleanup_submit_request_test(&qpair);
}

static void
test_ctrlr_failed(void)
{
	struct spdk_nvme_qpair		qpair = {};
	struct nvme_request		*req;
	struct spdk_nvme_ctrlr		ctrlr = {};
	char				payload[4096];

	prepare_submit_request_test(&qpair, &ctrlr);

	req = nvme_allocate_request_contig(&qpair, payload, sizeof(payload), expected_failure_callback,
					   NULL);
	SPDK_CU_ASSERT_FATAL(req != NULL);

	/* Set the controller to failed.
	 * Set the controller to resetting so that the qpair won't get re-enabled.
	 */
	ctrlr.is_failed = true;
	ctrlr.is_resetting = true;

	CU_ASSERT(nvme_qpair_submit_request(&qpair, req) != 0);

	cleanup_submit_request_test(&qpair);
}

static void struct_packing(void)
{
	/* ctrlr is the first field in nvme_qpair after the fields
	 * that are used in the I/O path. Make sure the I/O path fields
	 * all fit into two cache lines.
	 */
	CU_ASSERT(offsetof(struct spdk_nvme_qpair, ctrlr) <= 128);
}

static void test_nvme_qpair_process_completions(void)
{
	struct spdk_nvme_qpair		qpair = {};
	struct spdk_nvme_ctrlr		ctrlr = {};

	prepare_submit_request_test(&qpair, &ctrlr);
	qpair.ctrlr->is_resetting = true;

	spdk_nvme_qpair_process_completions(&qpair, 0);
	cleanup_submit_request_test(&qpair);
}

static void test_nvme_completion_is_retry(void)
{
	struct spdk_nvme_cpl	cpl = {};

	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	cpl.status.sc = SPDK_NVME_SC_NAMESPACE_NOT_READY;
	cpl.status.dnr = 0;
	CU_ASSERT_TRUE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_FORMAT_IN_PROGRESS;
	cpl.status.dnr = 1;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));
	cpl.status.dnr = 0;
	CU_ASSERT_TRUE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_INVALID_FIELD;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_COMMAND_ID_CONFLICT;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_DATA_TRANSFER_ERROR;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_ABORTED_POWER_LOSS;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_ABORTED_FAILED_FUSED;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_ABORTED_MISSING_FUSED;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_INVALID_SGL_SEG_DESCRIPTOR;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_INVALID_NUM_SGL_DESCIRPTORS;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_METADATA_SGL_LENGTH_INVALID;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_INVALID_CONTROLLER_MEM_BUF;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_INVALID_PRP_OFFSET;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_ATOMIC_WRITE_UNIT_EXCEEDED;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_CAPACITY_EXCEEDED;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_RESERVATION_CONFLICT;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = 0x70;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sct = SPDK_NVME_SCT_MEDIA_ERROR;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sct = SPDK_NVME_SCT_VENDOR_SPECIFIC;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sct = 0x4;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));
}

#ifdef DEBUG
static void
test_get_status_string(void)
{
	const char	*status_string;

	status_string = get_status_string(SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_SUCCESS);
	CU_ASSERT(strcmp(status_string, "SUCCESS") == 0);

	status_string = get_status_string(SPDK_NVME_SCT_COMMAND_SPECIFIC,
					  SPDK_NVME_SC_COMPLETION_QUEUE_INVALID);
	CU_ASSERT(strcmp(status_string, "INVALID COMPLETION QUEUE") == 0);

	status_string = get_status_string(SPDK_NVME_SCT_MEDIA_ERROR, SPDK_NVME_SC_UNRECOVERED_READ_ERROR);
	CU_ASSERT(strcmp(status_string, "UNRECOVERED READ ERROR") == 0);

	status_string = get_status_string(SPDK_NVME_SCT_VENDOR_SPECIFIC, 0);
	CU_ASSERT(strcmp(status_string, "VENDOR SPECIFIC") == 0);

	status_string = get_status_string(100, 0);
	CU_ASSERT(strcmp(status_string, "RESERVED") == 0);
}
#endif

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvme_qpair", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (CU_add_test(suite, "test3", test3) == NULL
	    || CU_add_test(suite, "ctrlr_failed", test_ctrlr_failed) == NULL
	    || CU_add_test(suite, "struct_packing", struct_packing) == NULL
	    || CU_add_test(suite, "spdk_nvme_qpair_process_completions",
			   test_nvme_qpair_process_completions) == NULL
	    || CU_add_test(suite, "nvme_completion_is_retry", test_nvme_completion_is_retry) == NULL
#ifdef DEBUG
	    || CU_add_test(suite, "get_status_string", test_get_status_string) == NULL
#endif
	   ) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
