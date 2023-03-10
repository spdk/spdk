/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_cunit.h"

#include "common/lib/test_env.c"

pid_t g_spdk_nvme_pid;

bool trace_flag = false;
#define SPDK_LOG_NVME trace_flag

#include "nvme/nvme_qpair.c"

SPDK_LOG_REGISTER_COMPONENT(nvme)

struct nvme_driver _g_nvme_driver = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
};

DEFINE_STUB_V(nvme_transport_qpair_abort_reqs, (struct spdk_nvme_qpair *qpair));
DEFINE_STUB(nvme_transport_qpair_submit_request, int,
	    (struct spdk_nvme_qpair *qpair, struct nvme_request *req), 0);
DEFINE_STUB(spdk_nvme_ctrlr_free_io_qpair, int, (struct spdk_nvme_qpair *qpair), 0);
DEFINE_STUB_V(nvme_transport_ctrlr_disconnect_qpair, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair));
DEFINE_STUB_V(nvme_ctrlr_disconnect_qpair, (struct spdk_nvme_qpair *qpair));

DEFINE_STUB_V(nvme_ctrlr_complete_queued_async_events, (struct spdk_nvme_ctrlr *ctrlr));
DEFINE_STUB_V(nvme_ctrlr_abort_queued_aborts, (struct spdk_nvme_ctrlr *ctrlr));

void
nvme_ctrlr_fail(struct spdk_nvme_ctrlr *ctrlr, bool hot_remove)
{
	if (hot_remove) {
		ctrlr->is_removed = true;
	}
	ctrlr->is_failed = true;
}

static bool g_called_transport_process_completions = false;
static int32_t g_transport_process_completions_rc = 0;
int32_t
nvme_transport_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	g_called_transport_process_completions = true;
	return g_transport_process_completions_rc;
}

static void
prepare_submit_request_test(struct spdk_nvme_qpair *qpair,
			    struct spdk_nvme_ctrlr *ctrlr)
{
	memset(ctrlr, 0, sizeof(*ctrlr));
	ctrlr->free_io_qids = NULL;
	TAILQ_INIT(&ctrlr->active_io_qpairs);
	TAILQ_INIT(&ctrlr->active_procs);
	MOCK_CLEAR(spdk_zmalloc);
	nvme_qpair_init(qpair, 1, ctrlr, 0, 32, false);
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

	qpair.state = NVME_QPAIR_ENABLED;
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

static void
struct_packing(void)
{
	/* ctrlr is the first field in nvme_qpair after the fields
	 * that are used in the I/O path. Make sure the I/O path fields
	 * all fit into two cache lines.
	 */
	CU_ASSERT(offsetof(struct spdk_nvme_qpair, ctrlr) <= 128);
}

static int g_num_cb_failed = 0;
static int g_num_cb_passed = 0;

static void
dummy_cb_fn(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	if (cpl->status.sc == SPDK_NVME_SC_SUCCESS) {
		g_num_cb_passed++;
	} else {
		g_num_cb_failed++;
	}
}

static void
test_nvme_qpair_process_completions(void)
{
	struct spdk_nvme_qpair		admin_qp = {0};
	struct spdk_nvme_qpair		qpair = {0};
	struct spdk_nvme_ctrlr		ctrlr = {{0}};
	struct nvme_request		dummy_1 = {{0}};
	struct nvme_request		dummy_2 = {{0}};
	int				rc;

	dummy_1.cb_fn = dummy_cb_fn;
	dummy_2.cb_fn = dummy_cb_fn;
	dummy_1.qpair = &qpair;
	dummy_2.qpair = &qpair;

	TAILQ_INIT(&ctrlr.active_io_qpairs);
	TAILQ_INIT(&ctrlr.active_procs);
	CU_ASSERT(pthread_mutex_init(&ctrlr.ctrlr_lock, NULL) == 0);
	nvme_qpair_init(&qpair, 1, &ctrlr, 0, 32, false);
	nvme_qpair_init(&admin_qp, 0, &ctrlr, 0, 32, false);

	ctrlr.adminq = &admin_qp;

	STAILQ_INIT(&qpair.queued_req);
	STAILQ_INSERT_TAIL(&qpair.queued_req, &dummy_1, stailq);
	STAILQ_INSERT_TAIL(&qpair.queued_req, &dummy_2, stailq);
	qpair.num_outstanding_reqs = 2;

	/* If the controller is failed, return -ENXIO */
	ctrlr.is_failed = true;
	ctrlr.is_removed = false;
	rc = spdk_nvme_qpair_process_completions(&qpair, 0);
	CU_ASSERT(rc == -ENXIO);
	CU_ASSERT(!STAILQ_EMPTY(&qpair.queued_req));
	CU_ASSERT(g_num_cb_passed == 0);
	CU_ASSERT(g_num_cb_failed == 0);
	CU_ASSERT(qpair.num_outstanding_reqs == 2);

	/* Same if the qpair is failed at the transport layer. */
	ctrlr.is_failed = false;
	ctrlr.is_removed = false;
	qpair.state = NVME_QPAIR_DISCONNECTED;
	rc = spdk_nvme_qpair_process_completions(&qpair, 0);
	CU_ASSERT(rc == -ENXIO);
	CU_ASSERT(!STAILQ_EMPTY(&qpair.queued_req));
	CU_ASSERT(g_num_cb_passed == 0);
	CU_ASSERT(g_num_cb_failed == 0);
	CU_ASSERT(qpair.num_outstanding_reqs == 2);

	/* If the controller is removed, make sure we abort the requests. */
	ctrlr.is_failed = true;
	ctrlr.is_removed = true;
	qpair.state = NVME_QPAIR_CONNECTED;
	rc = spdk_nvme_qpair_process_completions(&qpair, 0);
	CU_ASSERT(rc == -ENXIO);
	CU_ASSERT(STAILQ_EMPTY(&qpair.queued_req));
	CU_ASSERT(g_num_cb_passed == 0);
	CU_ASSERT(g_num_cb_failed == 2);
	CU_ASSERT(qpair.num_outstanding_reqs == 0);

	/* If we are resetting, make sure that we don't call into the transport. */
	STAILQ_INSERT_TAIL(&qpair.queued_req, &dummy_1, stailq);
	dummy_1.queued = true;
	STAILQ_INSERT_TAIL(&qpair.queued_req, &dummy_2, stailq);
	dummy_2.queued = true;
	g_num_cb_failed = 0;
	ctrlr.is_failed = false;
	ctrlr.is_removed = false;
	ctrlr.is_resetting = true;
	rc = spdk_nvme_qpair_process_completions(&qpair, 0);
	CU_ASSERT(rc == -ENXIO);
	CU_ASSERT(g_called_transport_process_completions == false);
	/* We also need to make sure we didn't abort the requests. */
	CU_ASSERT(!STAILQ_EMPTY(&qpair.queued_req));
	CU_ASSERT(g_num_cb_passed == 0);
	CU_ASSERT(g_num_cb_failed == 0);

	/* The case where we aren't resetting, but are enabling the qpair is the same as above. */
	ctrlr.is_resetting = false;
	qpair.state = NVME_QPAIR_ENABLING;
	rc = spdk_nvme_qpair_process_completions(&qpair, 0);
	CU_ASSERT(rc == -ENXIO);
	CU_ASSERT(g_called_transport_process_completions == false);
	CU_ASSERT(!STAILQ_EMPTY(&qpair.queued_req));
	CU_ASSERT(g_num_cb_passed == 0);
	CU_ASSERT(g_num_cb_failed == 0);

	/* For other qpair states, we want to enable the qpair. */
	qpair.state = NVME_QPAIR_CONNECTED;
	rc = spdk_nvme_qpair_process_completions(&qpair, 1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_called_transport_process_completions == true);
	/* These should have been submitted to the lower layer. */
	CU_ASSERT(STAILQ_EMPTY(&qpair.queued_req));
	CU_ASSERT(g_num_cb_passed == 0);
	CU_ASSERT(g_num_cb_failed == 0);
	CU_ASSERT(nvme_qpair_get_state(&qpair) == NVME_QPAIR_ENABLED);

	g_called_transport_process_completions = false;
	g_transport_process_completions_rc = -ENXIO;

	/* Fail the controller if we get an error from the transport on admin qpair. */
	admin_qp.state = NVME_QPAIR_ENABLED;
	rc = spdk_nvme_qpair_process_completions(&admin_qp, 0);
	CU_ASSERT(rc == -ENXIO);
	CU_ASSERT(g_called_transport_process_completions == true);
	CU_ASSERT(ctrlr.is_failed == true);

	/* Don't fail the controller for regular qpairs. */
	ctrlr.is_failed = false;
	g_called_transport_process_completions = false;
	rc = spdk_nvme_qpair_process_completions(&qpair, 0);
	CU_ASSERT(rc == -ENXIO);
	CU_ASSERT(g_called_transport_process_completions == true);
	CU_ASSERT(ctrlr.is_failed == false);

	/* Make sure we don't modify the return value from the transport. */
	ctrlr.is_failed = false;
	g_called_transport_process_completions = false;
	g_transport_process_completions_rc = 23;
	rc = spdk_nvme_qpair_process_completions(&qpair, 0);
	CU_ASSERT(rc == 23);
	CU_ASSERT(g_called_transport_process_completions == true);
	CU_ASSERT(ctrlr.is_failed == false);

	free(qpair.req_buf);
	free(admin_qp.req_buf);
}

static void
test_nvme_completion_is_retry(void)
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

	cpl.status.sct = SPDK_NVME_SCT_PATH;
	cpl.status.sc = SPDK_NVME_SC_INTERNAL_PATH_ERROR;
	cpl.status.dnr = 0;
	CU_ASSERT_TRUE(nvme_completion_is_retry(&cpl));

	cpl.status.sct = SPDK_NVME_SCT_PATH;
	cpl.status.sc = SPDK_NVME_SC_INTERNAL_PATH_ERROR;
	cpl.status.dnr = 1;
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
	struct spdk_nvme_status status;

	status.sct = SPDK_NVME_SCT_GENERIC;
	status.sc = SPDK_NVME_SC_SUCCESS;
	status_string = spdk_nvme_cpl_get_status_string(&status);
	CU_ASSERT(strcmp(status_string, "SUCCESS") == 0);

	status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
	status.sc = SPDK_NVME_SC_COMPLETION_QUEUE_INVALID;
	status_string = spdk_nvme_cpl_get_status_string(&status);
	CU_ASSERT(strcmp(status_string, "INVALID COMPLETION QUEUE") == 0);

	status.sct = SPDK_NVME_SCT_MEDIA_ERROR;
	status.sc = SPDK_NVME_SC_UNRECOVERED_READ_ERROR;
	status_string = spdk_nvme_cpl_get_status_string(&status);
	CU_ASSERT(strcmp(status_string, "UNRECOVERED READ ERROR") == 0);

	status.sct = SPDK_NVME_SCT_VENDOR_SPECIFIC;
	status.sc = 0;
	status_string = spdk_nvme_cpl_get_status_string(&status);
	CU_ASSERT(strcmp(status_string, "VENDOR SPECIFIC") == 0);

	status.sct = 0x4;
	status.sc = 0;
	status_string = spdk_nvme_cpl_get_status_string(&status);
	CU_ASSERT(strcmp(status_string, "RESERVED") == 0);
}
#endif

static void
test_nvme_qpair_add_cmd_error_injection(void)
{
	struct spdk_nvme_qpair qpair = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	pthread_mutexattr_t attr;
	int rc;

	prepare_submit_request_test(&qpair, &ctrlr);
	ctrlr.adminq = &qpair;

	SPDK_CU_ASSERT_FATAL(pthread_mutexattr_init(&attr) == 0);
	SPDK_CU_ASSERT_FATAL(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0);
	SPDK_CU_ASSERT_FATAL(pthread_mutex_init(&ctrlr.ctrlr_lock, &attr) == 0);
	pthread_mutexattr_destroy(&attr);

	/* Admin error injection at submission path */
	MOCK_CLEAR(spdk_zmalloc);
	rc = spdk_nvme_qpair_add_cmd_error_injection(&ctrlr, NULL,
			SPDK_NVME_OPC_GET_FEATURES, true, 5000, 1,
			SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_INVALID_FIELD);

	CU_ASSERT(rc == 0);
	CU_ASSERT(!TAILQ_EMPTY(&qpair.err_cmd_head));

	/* Remove cmd error injection */
	spdk_nvme_qpair_remove_cmd_error_injection(&ctrlr, NULL, SPDK_NVME_OPC_GET_FEATURES);

	CU_ASSERT(TAILQ_EMPTY(&qpair.err_cmd_head));

	/* IO error injection at completion path */
	rc = spdk_nvme_qpair_add_cmd_error_injection(&ctrlr, &qpair,
			SPDK_NVME_OPC_READ, false, 0, 1,
			SPDK_NVME_SCT_MEDIA_ERROR, SPDK_NVME_SC_UNRECOVERED_READ_ERROR);

	CU_ASSERT(rc == 0);
	CU_ASSERT(!TAILQ_EMPTY(&qpair.err_cmd_head));

	/* Provide the same opc, and check whether allocate a new entry */
	rc = spdk_nvme_qpair_add_cmd_error_injection(&ctrlr, &qpair,
			SPDK_NVME_OPC_READ, false, 0, 1,
			SPDK_NVME_SCT_MEDIA_ERROR, SPDK_NVME_SC_UNRECOVERED_READ_ERROR);

	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&qpair.err_cmd_head));
	CU_ASSERT(TAILQ_NEXT(TAILQ_FIRST(&qpair.err_cmd_head), link) == NULL);

	/* Remove cmd error injection */
	spdk_nvme_qpair_remove_cmd_error_injection(&ctrlr, &qpair, SPDK_NVME_OPC_READ);

	CU_ASSERT(TAILQ_EMPTY(&qpair.err_cmd_head));

	rc = spdk_nvme_qpair_add_cmd_error_injection(&ctrlr, &qpair,
			SPDK_NVME_OPC_COMPARE, true, 0, 5,
			SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_COMPARE_FAILURE);

	CU_ASSERT(rc == 0);
	CU_ASSERT(!TAILQ_EMPTY(&qpair.err_cmd_head));

	/* Remove cmd error injection */
	spdk_nvme_qpair_remove_cmd_error_injection(&ctrlr, &qpair, SPDK_NVME_OPC_COMPARE);

	CU_ASSERT(TAILQ_EMPTY(&qpair.err_cmd_head));

	pthread_mutex_destroy(&ctrlr.ctrlr_lock);
	cleanup_submit_request_test(&qpair);
}

static struct nvme_request *
allocate_request_tree(struct spdk_nvme_qpair *qpair)
{
	struct nvme_request	*req, *req1, *req2, *req3, *req2_1, *req2_2, *req2_3;

	/*
	 *  Build a request chain like the following:
	 *            req
	 *             |
	 *      ---------------
	 *     |       |       |
	 *    req1    req2    req3
	 *             |
	 *      ---------------
	 *     |       |       |
	 *   req2_1  req2_2  req2_3
	 */
	req = nvme_allocate_request_null(qpair, NULL, NULL);
	CU_ASSERT(req != NULL);
	TAILQ_INIT(&req->children);

	req1 = nvme_allocate_request_null(qpair, NULL, NULL);
	CU_ASSERT(req1 != NULL);
	req->num_children++;
	TAILQ_INSERT_TAIL(&req->children, req1, child_tailq);
	req1->parent = req;

	req2 = nvme_allocate_request_null(qpair, NULL, NULL);
	CU_ASSERT(req2 != NULL);
	TAILQ_INIT(&req2->children);
	req->num_children++;
	TAILQ_INSERT_TAIL(&req->children, req2, child_tailq);
	req2->parent = req;

	req3 = nvme_allocate_request_null(qpair, NULL, NULL);
	CU_ASSERT(req3 != NULL);
	req->num_children++;
	TAILQ_INSERT_TAIL(&req->children, req3, child_tailq);
	req3->parent = req;

	req2_1 = nvme_allocate_request_null(qpair, NULL, NULL);
	CU_ASSERT(req2_1 != NULL);
	req2->num_children++;
	TAILQ_INSERT_TAIL(&req2->children, req2_1, child_tailq);
	req2_1->parent = req2;

	req2_2 = nvme_allocate_request_null(qpair, NULL, NULL);
	CU_ASSERT(req2_2 != NULL);
	req2->num_children++;
	TAILQ_INSERT_TAIL(&req2->children, req2_2, child_tailq);
	req2_2->parent = req2;

	req2_3 = nvme_allocate_request_null(qpair, NULL, NULL);
	CU_ASSERT(req2_3 != NULL);
	req2->num_children++;
	TAILQ_INSERT_TAIL(&req2->children, req2_3, child_tailq);
	req2_3->parent = req2;

	return req;
}

static void
test_nvme_qpair_submit_request(void)
{
	int				rc;
	struct spdk_nvme_qpair		qpair = {};
	struct spdk_nvme_ctrlr		ctrlr = {};
	struct nvme_request		*req;

	prepare_submit_request_test(&qpair, &ctrlr);

	req = allocate_request_tree(&qpair);
	ctrlr.is_failed = true;
	rc = nvme_qpair_submit_request(&qpair, req);
	SPDK_CU_ASSERT_FATAL(rc == -ENXIO);

	req = allocate_request_tree(&qpair);
	ctrlr.is_failed = false;
	qpair.state = NVME_QPAIR_DISCONNECTING;
	rc = nvme_qpair_submit_request(&qpair, req);
	SPDK_CU_ASSERT_FATAL(rc == -ENXIO);

	cleanup_submit_request_test(&qpair);
}

static void
test_nvme_qpair_resubmit_request_with_transport_failed(void)
{
	int				rc;
	struct spdk_nvme_qpair		qpair = {};
	struct spdk_nvme_ctrlr		ctrlr = {};
	struct nvme_request		*req;

	prepare_submit_request_test(&qpair, &ctrlr);

	req = nvme_allocate_request_null(&qpair, dummy_cb_fn, NULL);
	CU_ASSERT(req != NULL);
	TAILQ_INIT(&req->children);

	STAILQ_INSERT_TAIL(&qpair.queued_req, req, stailq);
	req->queued = true;

	g_transport_process_completions_rc = 1;
	qpair.state = NVME_QPAIR_ENABLED;
	g_num_cb_failed = 0;
	MOCK_SET(nvme_transport_qpair_submit_request, -EINVAL);
	rc = spdk_nvme_qpair_process_completions(&qpair, g_transport_process_completions_rc);
	MOCK_CLEAR(nvme_transport_qpair_submit_request);
	CU_ASSERT(rc == g_transport_process_completions_rc);
	CU_ASSERT(STAILQ_EMPTY(&qpair.queued_req));
	CU_ASSERT(g_num_cb_failed == 1);

	cleanup_submit_request_test(&qpair);
}

static void
ut_spdk_nvme_cmd_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	CU_ASSERT(cb_arg == (void *)0xDEADBEEF);
	CU_ASSERT(cpl->sqid == 1);
	CU_ASSERT(cpl->status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(cpl->status.sc == SPDK_NVME_SC_SUCCESS);
	CU_ASSERT(cpl->status.dnr == 1);
}

static void
test_nvme_qpair_manual_complete_request(void)
{
	struct spdk_nvme_qpair qpair = {};
	struct nvme_request req = {};
	struct spdk_nvme_ctrlr ctrlr = {};

	qpair.ctrlr = &ctrlr;
	qpair.id = 1;
	req.cb_fn = ut_spdk_nvme_cmd_cb;
	req.cb_arg = (void *) 0xDEADBEEF;
	req.qpair = &qpair;
	req.num_children = 0;
	qpair.ctrlr->opts.disable_error_logging = false;
	STAILQ_INIT(&qpair.free_req);
	SPDK_CU_ASSERT_FATAL(STAILQ_EMPTY(&qpair.free_req));
	qpair.num_outstanding_reqs = 1;

	nvme_qpair_manual_complete_request(&qpair, &req, SPDK_NVME_SCT_GENERIC,
					   SPDK_NVME_SC_SUCCESS, 1, true);
	CU_ASSERT(!STAILQ_EMPTY(&qpair.free_req));
	CU_ASSERT(qpair.num_outstanding_reqs == 0);
}

static void
ut_spdk_nvme_cmd_cb_empty(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{

}

static void
test_nvme_qpair_init_deinit(void)
{
	struct spdk_nvme_qpair qpair = {};
	struct nvme_request *reqs[3] = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_error_cmd *cmd = NULL;
	struct nvme_request *var_req = NULL;
	int rc, i = 0;

	ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;

	rc = nvme_qpair_init(&qpair, 1, &ctrlr, SPDK_NVME_QPRIO_HIGH, 3, false);
	CU_ASSERT(rc == 0);
	CU_ASSERT(qpair.id == 1);
	CU_ASSERT(qpair.qprio == SPDK_NVME_QPRIO_HIGH);
	CU_ASSERT(qpair.in_completion_context == 0);
	CU_ASSERT(qpair.delete_after_completion_context == 0);
	CU_ASSERT(qpair.no_deletion_notification_needed == 0);
	CU_ASSERT(qpair.ctrlr == &ctrlr);
	CU_ASSERT(qpair.trtype == SPDK_NVME_TRANSPORT_PCIE);
	CU_ASSERT(qpair.req_buf != NULL);

	SPDK_CU_ASSERT_FATAL(!STAILQ_EMPTY(&qpair.free_req));
	STAILQ_FOREACH(var_req, &qpair.free_req, stailq) {
		/* Check requests address alignment */
		CU_ASSERT((uint64_t)var_req % 64 == 0);
		CU_ASSERT(var_req->qpair == &qpair);
		reqs[i++] = var_req;
	}
	CU_ASSERT(i == 3);

	/* Allocate cmd memory for deinit using */
	cmd = spdk_zmalloc(sizeof(*cmd), 64, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
	SPDK_CU_ASSERT_FATAL(cmd != NULL);
	TAILQ_INSERT_TAIL(&qpair.err_cmd_head, cmd, link);
	for (int i = 0; i < 3; i++) {
		reqs[i]->cb_fn = ut_spdk_nvme_cmd_cb_empty;
		reqs[i]->cb_arg = (void *) 0xDEADBEEF;
		reqs[i]->num_children = 0;
	}

	/* Emulate requests into various type queues */
	STAILQ_REMOVE(&qpair.free_req, reqs[0], nvme_request, stailq);
	STAILQ_INSERT_TAIL(&qpair.queued_req, reqs[0], stailq);
	STAILQ_REMOVE(&qpair.free_req, reqs[1], nvme_request, stailq);
	STAILQ_INSERT_TAIL(&qpair.aborting_queued_req, reqs[1], stailq);
	STAILQ_REMOVE(&qpair.free_req, reqs[2], nvme_request, stailq);
	STAILQ_INSERT_TAIL(&qpair.err_req_head, reqs[2], stailq);
	CU_ASSERT(STAILQ_EMPTY(&qpair.free_req));
	qpair.num_outstanding_reqs = 3;

	nvme_qpair_deinit(&qpair);
	CU_ASSERT(STAILQ_EMPTY(&qpair.queued_req));
	CU_ASSERT(STAILQ_EMPTY(&qpair.aborting_queued_req));
	CU_ASSERT(STAILQ_EMPTY(&qpair.err_req_head));
	CU_ASSERT(TAILQ_EMPTY(&qpair.err_cmd_head));
	CU_ASSERT(qpair.num_outstanding_reqs == 0);
}

static void
test_nvme_get_sgl_print_info(void)
{
	char buf[NVME_CMD_DPTR_STR_SIZE] = {};
	struct spdk_nvme_cmd cmd = {};

	cmd.dptr.sgl1.keyed.length = 0x1000;
	cmd.dptr.sgl1.keyed.key = 0xababccdd;

	nvme_get_sgl_keyed(buf, NVME_CMD_DPTR_STR_SIZE, &cmd);
	CU_ASSERT(!strncmp(buf, " len:0x1000 key:0xababccdd", NVME_CMD_DPTR_STR_SIZE));

	memset(&cmd.dptr.sgl1, 0, sizeof(cmd.dptr.sgl1));
	cmd.dptr.sgl1.unkeyed.length = 0x1000;

	nvme_get_sgl_unkeyed(buf, NVME_CMD_DPTR_STR_SIZE, &cmd);
	CU_ASSERT(!strncmp(buf, " len:0x1000", NVME_CMD_DPTR_STR_SIZE));

	memset(&cmd.dptr.sgl1, 0, sizeof(cmd.dptr.sgl1));
	cmd.dptr.sgl1.generic.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	cmd.dptr.sgl1.generic.subtype = 0;
	cmd.dptr.sgl1.address = 0xdeadbeef;
	cmd.dptr.sgl1.unkeyed.length = 0x1000;

	nvme_get_sgl(buf, NVME_CMD_DPTR_STR_SIZE, &cmd);
	CU_ASSERT(!strncmp(buf, "SGL DATA BLOCK ADDRESS 0xdeadbeef len:0x1000",
			   NVME_CMD_DPTR_STR_SIZE));

	memset(&cmd.dptr.sgl1, 0, sizeof(cmd.dptr.sgl1));
	cmd.dptr.sgl1.generic.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
	cmd.dptr.sgl1.generic.subtype = 0;
	cmd.dptr.sgl1.address = 0xdeadbeef;
	cmd.dptr.sgl1.keyed.length = 0x1000;
	cmd.dptr.sgl1.keyed.key = 0xababccdd;

	nvme_get_sgl(buf, NVME_CMD_DPTR_STR_SIZE, &cmd);
	CU_ASSERT(!strncmp(buf, "SGL KEYED DATA BLOCK ADDRESS 0xdeadbeef len:0x1000 key:0xababccdd",
			   NVME_CMD_DPTR_STR_SIZE));
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_qpair", NULL, NULL);

	CU_ADD_TEST(suite, test3);
	CU_ADD_TEST(suite, test_ctrlr_failed);
	CU_ADD_TEST(suite, struct_packing);
	CU_ADD_TEST(suite, test_nvme_qpair_process_completions);
	CU_ADD_TEST(suite, test_nvme_completion_is_retry);
#ifdef DEBUG
	CU_ADD_TEST(suite, test_get_status_string);
#endif
	CU_ADD_TEST(suite, test_nvme_qpair_add_cmd_error_injection);
	CU_ADD_TEST(suite, test_nvme_qpair_submit_request);
	CU_ADD_TEST(suite, test_nvme_qpair_resubmit_request_with_transport_failed);
	CU_ADD_TEST(suite, test_nvme_qpair_manual_complete_request);
	CU_ADD_TEST(suite, test_nvme_qpair_init_deinit);
	CU_ADD_TEST(suite, test_nvme_get_sgl_print_info);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
