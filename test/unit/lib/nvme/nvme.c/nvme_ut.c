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

#include "spdk_cunit.h"

#include "spdk/env.h"

#include "nvme/nvme.c"

#include "spdk_internal/mock.h"

#include "lib/test_env.c"

DEFINE_STUB_V(nvme_ctrlr_fail,
	      (struct spdk_nvme_ctrlr *ctrlr, bool hot_remove))

DEFINE_STUB_V(nvme_ctrlr_proc_get_ref, (struct spdk_nvme_ctrlr *ctrlr))

DEFINE_STUB_V(nvme_ctrlr_proc_put_ref, (struct spdk_nvme_ctrlr *ctrlr))

DEFINE_STUB(spdk_pci_nvme_enumerate, int,
	    (spdk_pci_enum_cb enum_cb, void *enum_ctx), -1)

DEFINE_STUB(spdk_pci_device_get_id, struct spdk_pci_id,
	    (struct spdk_pci_device *pci_dev),
	    MOCK_STRUCT_INIT(.vendor_id = 0xffff, .device_id = 0xffff,
			     .subvendor_id = 0xffff, .subdevice_id = 0xffff))

DEFINE_STUB(spdk_nvme_transport_available, bool,
	    (enum spdk_nvme_transport_type trtype), true)

DEFINE_STUB(nvme_transport_ctrlr_scan, int,
	    (const struct spdk_nvme_transport_id *trid,
	     void *cb_ctx,
	     spdk_nvme_probe_cb probe_cb,
	     spdk_nvme_remove_cb remove_c), 0)

DEFINE_STUB(nvme_ctrlr_add_process, int,
	    (struct spdk_nvme_ctrlr *ctrlr, void *devhandle), 0)

DEFINE_STUB(nvme_ctrlr_process_init, int,
	    (struct spdk_nvme_ctrlr *ctrlr), 0)

DEFINE_STUB(nvme_ctrlr_start, int,
	    (struct spdk_nvme_ctrlr *ctrlr), 0)

DEFINE_STUB(spdk_pci_device_get_addr, struct spdk_pci_addr,
	    (struct spdk_pci_device *pci_dev), {0})

DEFINE_STUB(spdk_pci_addr_compare, int,
	    (const struct spdk_pci_addr *a1,
	     const struct spdk_pci_addr *a2), 1)

DEFINE_STUB(nvme_ctrlr_get_ref_count, int,
	    (struct spdk_nvme_ctrlr *ctrlr), 0)

DEFINE_STUB(dummy_probe_cb, bool,
	    (void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	     struct spdk_nvme_ctrlr_opts *opts), false)

DEFINE_STUB_P(nvme_transport_ctrlr_construct, struct spdk_nvme_ctrlr,
	      (const struct spdk_nvme_transport_id *trid,
	       const struct spdk_nvme_ctrlr_opts *opts,
	       void *devhandle), {0})

static bool ut_destruct_called = false;
void
nvme_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	ut_destruct_called = true;
}

void
spdk_nvme_ctrlr_opts_set_defaults(struct spdk_nvme_ctrlr_opts *opts)
{
	memset(opts, 0, sizeof(*opts));
}

static void
memset_trid(struct spdk_nvme_transport_id *trid1, struct spdk_nvme_transport_id *trid2)
{
	memset(trid1, 0, sizeof(struct spdk_nvme_transport_id));
	memset(trid2, 0, sizeof(struct spdk_nvme_transport_id));
}

static bool ut_attach_cb_called = false;
static void
dummy_attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	ut_attach_cb_called = true;
}

static void
test_nvme_init_controllers(void)
{
	int rc = 0;
	struct nvme_driver test_driver;
	void *cb_ctx = NULL;
	spdk_nvme_attach_cb attach_cb = dummy_attach_cb;
	struct spdk_nvme_ctrlr ctrlr;
	pthread_mutexattr_t attr;

	g_spdk_nvme_driver = &test_driver;
	memset(&ctrlr, 0, sizeof(struct spdk_nvme_ctrlr));
	CU_ASSERT(pthread_mutexattr_init(&attr) == 0);
	CU_ASSERT(pthread_mutex_init(&test_driver.lock, &attr) == 0);
	TAILQ_INIT(&test_driver.init_ctrlrs);
	TAILQ_INSERT_TAIL(&test_driver.init_ctrlrs, &ctrlr, tailq);
	TAILQ_INIT(&test_driver.attached_ctrlrs);

	/*
	 * Try to initialize, but nvme_ctrlr_process_init will fail.
	 * Verify correct behavior when it does.
	 */
	MOCK_SET(nvme_ctrlr_process_init, int, 1);
	g_spdk_nvme_driver->initialized = false;
	ut_destruct_called = false;
	rc = nvme_init_controllers(cb_ctx, attach_cb);
	CU_ASSERT(rc == -1);
	CU_ASSERT(g_spdk_nvme_driver->initialized == true);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_nvme_driver->init_ctrlrs));
	CU_ASSERT(ut_destruct_called == true);

	/*
	 * Controller init OK, need to move the controller state machine
	 * forward by setting the ctrl state so that it can be moved
	 * the attached_ctrlrs list.
	 */
	TAILQ_INSERT_TAIL(&test_driver.init_ctrlrs, &ctrlr, tailq);
	ctrlr.state = NVME_CTRLR_STATE_READY;
	MOCK_SET(nvme_ctrlr_process_init, int, 0);
	rc = nvme_init_controllers(cb_ctx, attach_cb);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ut_attach_cb_called == true);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_nvme_driver->init_ctrlrs));
	CU_ASSERT(TAILQ_FIRST(&g_spdk_nvme_driver->attached_ctrlrs) == &ctrlr);

	g_spdk_nvme_driver = NULL;
	pthread_mutexattr_destroy(&attr);
	pthread_mutex_destroy(&test_driver.lock);
}

static void
test_nvme_driver_init(void)
{
	int rc;
	struct nvme_driver dummy;
	g_spdk_nvme_driver = &dummy;

	/* adjust this so testing doesn't take so long */
	g_nvme_driver_timeout_ms = 100;

	/* process is primary and mem already reserved */
	MOCK_SET(spdk_process_is_primary, bool, true);
	dummy.initialized = true;
	rc = nvme_driver_init();
	CU_ASSERT(rc == 0);

	/*
	 * Process is primary and mem not yet reserved but the call
	 * to spdk_memzone_reserve() returns NULL.
	 */
	g_spdk_nvme_driver = NULL;
	MOCK_SET(spdk_process_is_primary, bool, true);
	MOCK_SET_P(spdk_memzone_reserve, void *, NULL);
	rc = nvme_driver_init();
	CU_ASSERT(rc == -1);

	/* process is not primary, no mem already reserved */
	MOCK_SET(spdk_process_is_primary, bool, false);
	MOCK_SET_P(spdk_memzone_lookup, void *, NULL);
	g_spdk_nvme_driver = NULL;
	rc = nvme_driver_init();
	CU_ASSERT(rc == -1);

	/* process is not primary, mem is already reserved & init'd */
	MOCK_SET(spdk_process_is_primary, bool, false);
	MOCK_SET_P(spdk_memzone_lookup, void *, &dummy);
	dummy.initialized = true;
	rc = nvme_driver_init();
	CU_ASSERT(rc == 0);

	/* process is not primary, mem is reserved but not intiialized */
	/* and times out */
	MOCK_SET(spdk_process_is_primary, bool, false);
	MOCK_SET_P(spdk_memzone_reserve, void *, &dummy);
	dummy.initialized = false;
	rc = nvme_driver_init();
	CU_ASSERT(rc == -1);

	/* process is primary, got mem but mutex won't init */
	MOCK_SET(spdk_process_is_primary, bool, true);
	MOCK_SET_P(spdk_memzone_reserve, void *, &dummy);
	MOCK_SET(pthread_mutexattr_init, int, -1);
	g_spdk_nvme_driver = NULL;
	dummy.initialized = true;
	rc = nvme_driver_init();
	/* for FreeBSD we can't can't effectively mock this path */
#ifndef __FreeBSD__
	CU_ASSERT(rc != 0);
#else
	CU_ASSERT(rc == 0);
#endif

	/* process is primary, got mem, mutex OK */
	MOCK_SET(spdk_process_is_primary, bool, true);
	MOCK_SET(pthread_mutexattr_init, int, MOCK_PASS_THRU);
	g_spdk_nvme_driver = NULL;
	rc = nvme_driver_init();
	CU_ASSERT(g_spdk_nvme_driver->initialized == false);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_nvme_driver->init_ctrlrs));
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_nvme_driver->attached_ctrlrs));
	CU_ASSERT(rc == 0);

	g_spdk_nvme_driver = NULL;
	MOCK_SET_P(spdk_memzone_reserve, void *, MOCK_PASS_THRU_P);
	MOCK_SET_P(spdk_memzone_lookup, void *, NULL);
}

static void
test_spdk_nvme_detach(void)
{
	int rc = 1;
	struct spdk_nvme_ctrlr ctrlr;
	struct spdk_nvme_ctrlr *ret_ctrlr;
	struct nvme_driver test_driver;

	g_spdk_nvme_driver = &test_driver;
	TAILQ_INIT(&test_driver.attached_ctrlrs);
	TAILQ_INSERT_TAIL(&test_driver.attached_ctrlrs, &ctrlr, tailq);
	CU_ASSERT_FATAL(pthread_mutex_init(&test_driver.lock, NULL) == 0);

	/*
	 * Controllers are ref counted so mock the function that returns
	 * the ref count so that detach will actually call the destruct
	 * function which we've mocked simply to verify that it gets
	 * called (we aren't testing what the real destuct function does
	 * here.)
	 */
	MOCK_SET(nvme_ctrlr_get_ref_count, int, 0);
	rc = spdk_nvme_detach(&ctrlr);
	ret_ctrlr = TAILQ_FIRST(&test_driver.attached_ctrlrs);
	CU_ASSERT(ret_ctrlr == NULL);
	CU_ASSERT(ut_destruct_called == true);
	CU_ASSERT(rc == 0);

	/*
	 * Mock the ref count to 1 so we confirm that the destruct
	 * function is not called and that attached ctrl list is
	 * not empty.
	 */
	MOCK_SET(nvme_ctrlr_get_ref_count, int, 1);
	TAILQ_INSERT_TAIL(&test_driver.attached_ctrlrs, &ctrlr, tailq);
	ut_destruct_called = false;
	rc = spdk_nvme_detach(&ctrlr);
	ret_ctrlr = TAILQ_FIRST(&test_driver.attached_ctrlrs);
	CU_ASSERT(ret_ctrlr != NULL);
	CU_ASSERT(ut_destruct_called == false);
	CU_ASSERT(rc == 0);

	g_spdk_nvme_driver = NULL;
}

static void
test_nvme_completion_poll_cb(void)
{
	struct nvme_completion_poll_status status;
	struct spdk_nvme_cpl cpl;

	memset(&status, 0x0, sizeof(status));
	memset(&cpl, 0xff, sizeof(cpl));

	nvme_completion_poll_cb(&status, &cpl);
	CU_ASSERT(status.done == true);
	CU_ASSERT(memcmp(&cpl, &status.cpl,
			 sizeof(struct spdk_nvme_cpl)) == 0);
}

/* stub callback used by test_nvme_user_copy_cmd_complete() */
static struct spdk_nvme_cpl ut_spdk_nvme_cpl = {0};
static void
dummy_cb(void *user_cb_arg, struct spdk_nvme_cpl *cpl)
{
	ut_spdk_nvme_cpl  = *cpl;
}

static void
test_nvme_user_copy_cmd_complete(void)
{
	struct nvme_request req;
	int test_data = 0xdeadbeef;
	int buff_size = sizeof(int);
	static struct spdk_nvme_cpl cpl;

	memset(&req, 0, sizeof(req));
	memset(&cpl, 0x5a, sizeof(cpl));

	/* test without a user buffer provided */
	req.user_cb_fn = (void *)dummy_cb;
	nvme_user_copy_cmd_complete(&req, &cpl);
	CU_ASSERT(memcmp(&ut_spdk_nvme_cpl, &cpl, sizeof(cpl)) == 0);

	/* test with a user buffer provided */
	req.user_buffer = malloc(buff_size);
	SPDK_CU_ASSERT_FATAL(req.user_buffer != NULL);
	memset(req.user_buffer, 0, buff_size);
	req.payload_size = buff_size;
	req.payload.type = NVME_PAYLOAD_TYPE_CONTIG;
	req.payload.u.contig = malloc(buff_size);
	SPDK_CU_ASSERT_FATAL(req.payload.u.contig != NULL);
	memcpy(req.payload.u.contig, &test_data, buff_size);
	req.cmd.opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	req.pid = getpid();

	/* zero out the test value set in the callback */
	memset(&ut_spdk_nvme_cpl, 0, sizeof(ut_spdk_nvme_cpl));

	/*
	 * Mocking this to prevent the calling code from freeing the
	 * buff as it confuses either valgrind or the static analyzer.
	 */
	MOCK_SET_P(spdk_dma_zmalloc, void *, NULL);
	nvme_user_copy_cmd_complete(&req, &cpl);
	CU_ASSERT(memcmp(req.user_buffer, &test_data, buff_size) == 0);
	CU_ASSERT(memcmp(&ut_spdk_nvme_cpl, &cpl, sizeof(cpl)) == 0);

	/*
	 * Now test the same path as above but this time choose an opc
	 * that results in a different data transfer type.
	 */
	memset(&ut_spdk_nvme_cpl, 0, sizeof(ut_spdk_nvme_cpl));
	memset(req.user_buffer, 0, buff_size);
	req.cmd.opc = SPDK_NVME_OPC_SET_FEATURES;
	nvme_user_copy_cmd_complete(&req, &cpl);
	CU_ASSERT(memcmp(req.user_buffer, &test_data, buff_size) != 0);
	CU_ASSERT(memcmp(&ut_spdk_nvme_cpl, &cpl, sizeof(cpl)) == 0);

	/* clean up */
	free(req.user_buffer);
	free(req.payload.u.contig);

	/* return spdk_dma_zmalloc/freee to unmocked */
	MOCK_SET_P(spdk_dma_zmalloc, void *, &ut_spdk_dma_zmalloc);
}

static void
test_nvme_allocate_request_null(void)
{
	struct spdk_nvme_qpair qpair;
	spdk_nvme_cmd_cb cb_fn = (spdk_nvme_cmd_cb)0x1234;
	void *cb_arg = (void *)0x5678;
	struct nvme_request *req = NULL;
	struct nvme_request dummy_req;

	STAILQ_INIT(&qpair.free_req);
	STAILQ_INIT(&qpair.queued_req);

	/*
	 * Put a dummy on the queue so we can make a request
	 * and confirm that what comes back is what we expect.
	 */
	STAILQ_INSERT_HEAD(&qpair.free_req, &dummy_req, stailq);

	req = nvme_allocate_request_null(&qpair, cb_fn, cb_arg);

	/*
	 * Compare the req with the parmaters that we passed in
	 * as well as what the function is supposed to update.
	 */
	SPDK_CU_ASSERT_FATAL(req != NULL);
	CU_ASSERT(req->cb_fn == cb_fn);
	CU_ASSERT(req->cb_arg == cb_arg);
	CU_ASSERT(req->pid == getpid());
	CU_ASSERT(req->payload.type == NVME_PAYLOAD_TYPE_CONTIG);
	CU_ASSERT(req->payload.md == NULL);
	CU_ASSERT(req->payload.u.contig == NULL);
}

static void
test_nvme_allocate_request(void)
{
	struct spdk_nvme_qpair qpair;
	struct nvme_payload payload;
	uint32_t payload_struct_size = sizeof(payload);
	spdk_nvme_cmd_cb cb_fn = (spdk_nvme_cmd_cb)0x1234;
	void *cb_arg = (void *)0x6789;
	struct nvme_request *req = NULL;
	struct nvme_request dummy_req;

	/* Fill the whole payload struct with a known pattern */
	memset(&payload, 0x5a, payload_struct_size);
	STAILQ_INIT(&qpair.free_req);
	STAILQ_INIT(&qpair.queued_req);

	/* Test trying to allocate a request when no requests are available */
	req = nvme_allocate_request(&qpair, &payload, payload_struct_size,
				    cb_fn, cb_arg);
	CU_ASSERT(req == NULL);

	/* put a dummy on the queue, and then allocate one */
	STAILQ_INSERT_HEAD(&qpair.free_req, &dummy_req, stailq);
	req = nvme_allocate_request(&qpair, &payload, payload_struct_size,
				    cb_fn, cb_arg);

	/* all the req elements should now match the passed in paramters */
	SPDK_CU_ASSERT_FATAL(req != NULL);
	CU_ASSERT(req->cb_fn == cb_fn);
	CU_ASSERT(req->cb_arg == cb_arg);
	CU_ASSERT(memcmp(&req->payload, &payload, payload_struct_size) == 0);
	CU_ASSERT(req->payload_size == payload_struct_size);
	CU_ASSERT(req->qpair == &qpair);
	CU_ASSERT(req->pid == getpid());
}

static void
test_nvme_free_request(void)
{
	struct nvme_request match_req;
	struct spdk_nvme_qpair qpair;
	struct nvme_request *req;

	/* put a req on the Q, take it off and compare */
	memset(&match_req.cmd, 0x5a, sizeof(struct spdk_nvme_cmd));
	match_req.qpair = &qpair;
	/* the code under tests asserts this condition */
	match_req.num_children = 0;
	STAILQ_INIT(&qpair.free_req);

	nvme_free_request(&match_req);
	req = STAILQ_FIRST(&match_req.qpair->free_req);
	CU_ASSERT(req == &match_req);
}

static void
test_nvme_allocate_request_user_copy(void)
{
	struct spdk_nvme_qpair qpair;
	spdk_nvme_cmd_cb cb_fn = (spdk_nvme_cmd_cb)0x12345;
	void *cb_arg = (void *)0x12345;
	bool host_to_controller = true;
	struct nvme_request *req;
	struct nvme_request dummy_req;
	int test_data = 0xdeadbeef;
	void *buffer = NULL;
	uint32_t payload_size = sizeof(int);

	STAILQ_INIT(&qpair.free_req);
	STAILQ_INIT(&qpair.queued_req);

	/* no buffer or valid payload size, early NULL return */
	req = nvme_allocate_request_user_copy(&qpair, buffer, payload_size, cb_fn,
					      cb_arg, host_to_controller);
	CU_ASSERT(req == NULL);

	/* good buffer and valid payload size */
	buffer = malloc(payload_size);
	SPDK_CU_ASSERT_FATAL(buffer != NULL);
	memcpy(buffer, &test_data, payload_size);

	/* put a dummy on the queue */
	STAILQ_INSERT_HEAD(&qpair.free_req, &dummy_req, stailq);

	req = nvme_allocate_request_user_copy(&qpair, buffer, payload_size, cb_fn,
					      cb_arg, host_to_controller);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	CU_ASSERT(req->user_cb_fn == cb_fn);
	CU_ASSERT(req->user_cb_arg == cb_arg);
	CU_ASSERT(req->user_buffer == buffer);
	CU_ASSERT(req->cb_arg == req);
	CU_ASSERT(memcmp(req->payload.u.contig, buffer, payload_size) == 0);
	spdk_dma_free(req->payload.u.contig);

	/* same thing but additional path coverage, no copy */
	host_to_controller = false;
	STAILQ_INSERT_HEAD(&qpair.free_req, &dummy_req, stailq);

	req = nvme_allocate_request_user_copy(&qpair, buffer, payload_size, cb_fn,
					      cb_arg, host_to_controller);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	CU_ASSERT(req->user_cb_fn == cb_fn);
	CU_ASSERT(req->user_cb_arg == cb_arg);
	CU_ASSERT(req->user_buffer == buffer);
	CU_ASSERT(req->cb_arg == req);
	CU_ASSERT(memcmp(req->payload.u.contig, buffer, payload_size) != 0);
	spdk_dma_free(req->payload.u.contig);

	/* good buffer and valid payload size but make spdk_dma_zmalloc fail */
	/* set the mock pointer to NULL for spdk_dma_zmalloc */
	MOCK_SET_P(spdk_dma_zmalloc, void *, NULL);
	req = nvme_allocate_request_user_copy(&qpair, buffer, payload_size, cb_fn,
					      cb_arg, host_to_controller);
	CU_ASSERT(req == NULL);
	free(buffer);
	/* restore mock function back to the way it was */
	MOCK_SET_P(spdk_dma_zmalloc, void *, &ut_spdk_dma_zmalloc);
}

static void
test_nvme_ctrlr_probe(void)
{
	int rc = 0;
	const struct spdk_nvme_transport_id *trid = NULL;
	void *devhandle = NULL;
	void *cb_ctx = NULL;
	struct spdk_nvme_ctrlr *dummy = NULL;

	/* test when probe_cb returns false */
	MOCK_SET(dummy_probe_cb, bool, false);
	rc = nvme_ctrlr_probe(trid, devhandle, dummy_probe_cb, cb_ctx);
	CU_ASSERT(rc == 1);

	/* probe_cb returns true but we can't construct a ctrl */
	MOCK_SET(dummy_probe_cb, bool, true);
	MOCK_SET_P(nvme_transport_ctrlr_construct,
		   struct spdk_nvme_ctrlr *, NULL);
	rc = nvme_ctrlr_probe(trid, devhandle, dummy_probe_cb, cb_ctx);
	CU_ASSERT(rc == -1);

	/* happy path */
	g_spdk_nvme_driver = malloc(sizeof(struct nvme_driver));
	SPDK_CU_ASSERT_FATAL(g_spdk_nvme_driver != NULL);
	MOCK_SET(dummy_probe_cb, bool, true);
	MOCK_SET_P(nvme_transport_ctrlr_construct,
		   struct spdk_nvme_ctrlr *, &ut_nvme_transport_ctrlr_construct);
	TAILQ_INIT(&g_spdk_nvme_driver->init_ctrlrs);
	rc = nvme_ctrlr_probe(trid, devhandle, dummy_probe_cb, cb_ctx);
	CU_ASSERT(rc == 0);
	dummy = TAILQ_FIRST(&g_spdk_nvme_driver->init_ctrlrs);
	CU_ASSERT(dummy == &ut_nvme_transport_ctrlr_construct);

	free(g_spdk_nvme_driver);
}

static void
test_nvme_robust_mutex_init_shared(void)
{
	pthread_mutex_t mtx;
	int rc = 0;

	/* test where both pthread calls succeed */
	MOCK_SET(pthread_mutexattr_init, int, 0);
	MOCK_SET(pthread_mutex_init, int, 0);
	rc = nvme_robust_mutex_init_shared(&mtx);
	CU_ASSERT(rc == 0);

	/* test where we can't init attr's but init mutex works */
	MOCK_SET(pthread_mutexattr_init, int, -1);
	MOCK_SET(pthread_mutex_init, int, 0);
	rc = nvme_robust_mutex_init_shared(&mtx);
	/* for FreeBSD the only possible return value is 0 */
#ifndef __FreeBSD__
	CU_ASSERT(rc != 0);
#else
	CU_ASSERT(rc == 0);
#endif

	/* test where we can init attr's but the mutex init fails */
	MOCK_SET(pthread_mutexattr_init, int, 0);
	MOCK_SET(pthread_mutex_init, int, -1);
	rc = nvme_robust_mutex_init_shared(&mtx);
	/* for FreeBSD the only possible return value is 0 */
#ifndef __FreeBSD__
	CU_ASSERT(rc != 0);
#else
	CU_ASSERT(rc == 0);
#endif
}

static void
test_opc_data_transfer(void)
{
	enum spdk_nvme_data_transfer xfer;

	xfer = spdk_nvme_opc_get_data_transfer(SPDK_NVME_OPC_FLUSH);
	CU_ASSERT(xfer == SPDK_NVME_DATA_NONE);

	xfer = spdk_nvme_opc_get_data_transfer(SPDK_NVME_OPC_WRITE);
	CU_ASSERT(xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER);

	xfer = spdk_nvme_opc_get_data_transfer(SPDK_NVME_OPC_READ);
	CU_ASSERT(xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST);

	xfer = spdk_nvme_opc_get_data_transfer(SPDK_NVME_OPC_GET_LOG_PAGE);
	CU_ASSERT(xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST);
}

static void
test_trid_parse_and_compare(void)
{
	struct spdk_nvme_transport_id trid1, trid2;
	int ret;

	/* set trid1 trid2 value to id parse */
	ret = spdk_nvme_transport_id_parse(NULL, "trtype:PCIe traddr:0000:04:00.0");
	CU_ASSERT(ret == -EINVAL);
	memset(&trid1, 0, sizeof(trid1));
	ret = spdk_nvme_transport_id_parse(&trid1, NULL);
	CU_ASSERT(ret == -EINVAL);
	ret = spdk_nvme_transport_id_parse(NULL, NULL);
	CU_ASSERT(ret == -EINVAL);
	memset(&trid1, 0, sizeof(trid1));
	ret = spdk_nvme_transport_id_parse(&trid1, "trtype-PCIe traddr-0000-04-00.0");
	CU_ASSERT(ret == -EINVAL);
	memset(&trid1, 0, sizeof(trid1));
	ret = spdk_nvme_transport_id_parse(&trid1, "trtype-PCIe traddr-0000-04-00.0-:");
	CU_ASSERT(ret == -EINVAL);
	memset(&trid1, 0, sizeof(trid1));
	ret = spdk_nvme_transport_id_parse(&trid1, " \t\n:");
	CU_ASSERT(ret == -EINVAL);
	memset(&trid1, 0, sizeof(trid1));
	CU_ASSERT(spdk_nvme_transport_id_parse(&trid1,
					       "trtype:rdma\n"
					       "adrfam:ipv4\n"
					       "traddr:192.168.100.8\n"
					       "trsvcid:4420\n"
					       "subnqn:nqn.2014-08.org.nvmexpress.discovery") == 0);
	CU_ASSERT(trid1.trtype == SPDK_NVME_TRANSPORT_RDMA);
	CU_ASSERT(trid1.adrfam == SPDK_NVMF_ADRFAM_IPV4);
	CU_ASSERT(strcmp(trid1.traddr, "192.168.100.8") == 0);
	CU_ASSERT(strcmp(trid1.trsvcid, "4420") == 0);
	CU_ASSERT(strcmp(trid1.subnqn, "nqn.2014-08.org.nvmexpress.discovery") == 0);

	memset(&trid2, 0, sizeof(trid2));
	CU_ASSERT(spdk_nvme_transport_id_parse(&trid2, "trtype:PCIe traddr:0000:04:00.0") == 0);
	CU_ASSERT(trid2.trtype == SPDK_NVME_TRANSPORT_PCIE);
	CU_ASSERT(strcmp(trid2.traddr, "0000:04:00.0") == 0);

	CU_ASSERT(spdk_nvme_transport_id_compare(&trid1, &trid2) != 0);

	/* set trid1 trid2 and test id_compare */
	memset_trid(&trid1, &trid2);
	trid1.adrfam = SPDK_NVMF_ADRFAM_IPV6;
	trid2.adrfam = SPDK_NVMF_ADRFAM_IPV4;
	ret = spdk_nvme_transport_id_compare(&trid1, &trid2);
	CU_ASSERT(ret > 0);

	memset_trid(&trid1, &trid2);
	snprintf(trid1.traddr, sizeof(trid1.traddr), "192.168.100.8");
	snprintf(trid2.traddr, sizeof(trid2.traddr), "192.168.100.9");
	ret = spdk_nvme_transport_id_compare(&trid1, &trid2);
	CU_ASSERT(ret < 0);

	memset_trid(&trid1, &trid2);
	snprintf(trid1.trsvcid, sizeof(trid1.trsvcid), "4420");
	snprintf(trid2.trsvcid, sizeof(trid2.trsvcid), "4421");
	ret = spdk_nvme_transport_id_compare(&trid1, &trid2);
	CU_ASSERT(ret < 0);

	memset_trid(&trid1, &trid2);
	snprintf(trid1.subnqn, sizeof(trid1.subnqn), "subnqn:nqn.2016-08.org.nvmexpress.discovery");
	snprintf(trid2.subnqn, sizeof(trid2.subnqn), "subnqn:nqn.2017-08.org.nvmexpress.discovery");
	ret = spdk_nvme_transport_id_compare(&trid1, &trid2);
	CU_ASSERT(ret < 0);

	memset_trid(&trid1, &trid2);
	snprintf(trid1.subnqn, sizeof(trid1.subnqn), "subnqn:nqn.2016-08.org.nvmexpress.discovery");
	snprintf(trid2.subnqn, sizeof(trid2.subnqn), "subnqn:nqn.2016-08.org.nvmexpress.discovery");
	ret = spdk_nvme_transport_id_compare(&trid1, &trid2);
	CU_ASSERT(ret == 0);

	memset_trid(&trid1, &trid2);
	snprintf(trid1.subnqn, sizeof(trid1.subnqn), "subnqn:nqn.2016-08.org.nvmexpress.discovery");
	snprintf(trid2.subnqn, sizeof(trid2.subnqn), "subnqn:nqn.2016-08.org.Nvmexpress.discovery");
	ret = spdk_nvme_transport_id_compare(&trid1, &trid2);
	CU_ASSERT(ret > 0);

	memset_trid(&trid1, &trid2);
	ret = spdk_nvme_transport_id_compare(&trid1, &trid2);
	CU_ASSERT(ret == 0);
}

static void
test_spdk_nvme_transport_id_parse_trtype(void)
{

	enum spdk_nvme_transport_type *trtype;
	enum spdk_nvme_transport_type sct;
	char *str;

	trtype = NULL;
	str = "unit_test";

	/* test function returned value when trtype is NULL but str not NULL */
	CU_ASSERT(spdk_nvme_transport_id_parse_trtype(trtype, str) == (-EINVAL));

	/* test function returned value when str is NULL but trtype not NULL */
	trtype = &sct;
	str = NULL;
	CU_ASSERT(spdk_nvme_transport_id_parse_trtype(trtype, str) == (-EINVAL));

	/* test function returned value when str and strtype not NULL, but str value
	 * not "PCIe" or "RDMA" */
	str = "unit_test";
	CU_ASSERT(spdk_nvme_transport_id_parse_trtype(trtype, str) == (-ENOENT));

	/* test trtype value when use function "strcasecmp" to compare str and "PCIe"，not case-sensitive */
	str = "PCIe";
	spdk_nvme_transport_id_parse_trtype(trtype, str);
	CU_ASSERT((*trtype) == SPDK_NVME_TRANSPORT_PCIE);

	str = "pciE";
	spdk_nvme_transport_id_parse_trtype(trtype, str);
	CU_ASSERT((*trtype) == SPDK_NVME_TRANSPORT_PCIE);

	/* test trtype value when use function "strcasecmp" to compare str and "RDMA"，not case-sensitive */
	str = "RDMA";
	spdk_nvme_transport_id_parse_trtype(trtype, str);
	CU_ASSERT((*trtype) == SPDK_NVME_TRANSPORT_RDMA);

	str = "rdma";
	spdk_nvme_transport_id_parse_trtype(trtype, str);
	CU_ASSERT((*trtype) == SPDK_NVME_TRANSPORT_RDMA);

}

static void
test_spdk_nvme_transport_id_parse_adrfam(void)
{

	enum spdk_nvmf_adrfam *adrfam;
	enum spdk_nvmf_adrfam sct;
	char *str;

	adrfam = NULL;
	str = "unit_test";

	/* test function returned value when adrfam is NULL but str not NULL */
	CU_ASSERT(spdk_nvme_transport_id_parse_adrfam(adrfam, str) == (-EINVAL));

	/* test function returned value when str is NULL but adrfam not NULL */
	adrfam = &sct;
	str = NULL;
	CU_ASSERT(spdk_nvme_transport_id_parse_adrfam(adrfam, str) == (-EINVAL));

	/* test function returned value when str and adrfam not NULL, but str value
	 * not "IPv4" or "IPv6" or "IB" or "FC" */
	str = "unit_test";
	CU_ASSERT(spdk_nvme_transport_id_parse_adrfam(adrfam, str) == (-ENOENT));

	/* test adrfam value when use function "strcasecmp" to compare str and "IPv4"，not case-sensitive */
	str = "IPv4";
	spdk_nvme_transport_id_parse_adrfam(adrfam, str);
	CU_ASSERT((*adrfam) == SPDK_NVMF_ADRFAM_IPV4);

	str = "ipV4";
	spdk_nvme_transport_id_parse_adrfam(adrfam, str);
	CU_ASSERT((*adrfam) == SPDK_NVMF_ADRFAM_IPV4);

	/* test adrfam value when use function "strcasecmp" to compare str and "IPv6"，not case-sensitive */
	str = "IPv6";
	spdk_nvme_transport_id_parse_adrfam(adrfam, str);
	CU_ASSERT((*adrfam) == SPDK_NVMF_ADRFAM_IPV6);

	str = "ipV6";
	spdk_nvme_transport_id_parse_adrfam(adrfam, str);
	CU_ASSERT((*adrfam) == SPDK_NVMF_ADRFAM_IPV6);

	/* test adrfam value when use function "strcasecmp" to compare str and "IB"，not case-sensitive */
	str = "IB";
	spdk_nvme_transport_id_parse_adrfam(adrfam, str);
	CU_ASSERT((*adrfam) == SPDK_NVMF_ADRFAM_IB);

	str = "ib";
	spdk_nvme_transport_id_parse_adrfam(adrfam, str);
	CU_ASSERT((*adrfam) == SPDK_NVMF_ADRFAM_IB);

	/* test adrfam value when use function "strcasecmp" to compare str and "FC"，not case-sensitive */
	str = "FC";
	spdk_nvme_transport_id_parse_adrfam(adrfam, str);
	CU_ASSERT((*adrfam) == SPDK_NVMF_ADRFAM_FC);

	str = "fc";
	spdk_nvme_transport_id_parse_adrfam(adrfam, str);
	CU_ASSERT((*adrfam) == SPDK_NVMF_ADRFAM_FC);

}

static void
test_trid_trtype_str(void)
{
	const char *s;

	s = spdk_nvme_transport_id_trtype_str(-5);
	CU_ASSERT(s == NULL);

	s = spdk_nvme_transport_id_trtype_str(SPDK_NVME_TRANSPORT_PCIE);
	SPDK_CU_ASSERT_FATAL(s != NULL);
	CU_ASSERT(strcmp(s, "PCIe") == 0);

	s = spdk_nvme_transport_id_trtype_str(SPDK_NVME_TRANSPORT_RDMA);
	SPDK_CU_ASSERT_FATAL(s != NULL);
	CU_ASSERT(strcmp(s, "RDMA") == 0);
}

static void
test_trid_adrfam_str(void)
{
	const char *s;

	s = spdk_nvme_transport_id_adrfam_str(-5);
	CU_ASSERT(s == NULL);

	s = spdk_nvme_transport_id_adrfam_str(SPDK_NVMF_ADRFAM_IPV4);
	SPDK_CU_ASSERT_FATAL(s != NULL);
	CU_ASSERT(strcmp(s, "IPv4") == 0);

	s = spdk_nvme_transport_id_adrfam_str(SPDK_NVMF_ADRFAM_IPV6);
	SPDK_CU_ASSERT_FATAL(s != NULL);
	CU_ASSERT(strcmp(s, "IPv6") == 0);

	s = spdk_nvme_transport_id_adrfam_str(SPDK_NVMF_ADRFAM_IB);
	SPDK_CU_ASSERT_FATAL(s != NULL);
	CU_ASSERT(strcmp(s, "IB") == 0);

	s = spdk_nvme_transport_id_adrfam_str(SPDK_NVMF_ADRFAM_FC);
	SPDK_CU_ASSERT_FATAL(s != NULL);
	CU_ASSERT(strcmp(s, "FC") == 0);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvme", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test_opc_data_transfer",
			    test_opc_data_transfer) == NULL ||
		CU_add_test(suite, "test_spdk_nvme_transport_id_parse_trtype",
			    test_spdk_nvme_transport_id_parse_trtype) == NULL ||
		CU_add_test(suite, "test_spdk_nvme_transport_id_parse_adrfam",
			    test_spdk_nvme_transport_id_parse_adrfam) == NULL ||
		CU_add_test(suite, "test_trid_parse_and_compare",
			    test_trid_parse_and_compare) == NULL ||
		CU_add_test(suite, "test_trid_trtype_str",
			    test_trid_trtype_str) == NULL ||
		CU_add_test(suite, "test_trid_adrfam_str",
			    test_trid_adrfam_str) == NULL ||
		CU_add_test(suite, "test_nvme_ctrlr_probe",
			    test_nvme_ctrlr_probe) == NULL ||
		CU_add_test(suite, "test_nvme_init_controllers",
			    test_nvme_init_controllers) == NULL ||
		CU_add_test(suite, "test_nvme_driver_init",
			    test_nvme_driver_init) == NULL ||
		CU_add_test(suite, "test_spdk_nvme_detach",
			    test_spdk_nvme_detach) == NULL ||
		CU_add_test(suite, "test_nvme_completion_poll_cb",
			    test_nvme_completion_poll_cb) == NULL ||
		CU_add_test(suite, "test_nvme_user_copy_cmd_complete",
			    test_nvme_user_copy_cmd_complete) == NULL ||
		CU_add_test(suite, "test_nvme_allocate_request_null",
			    test_nvme_allocate_request_null) == NULL ||
		CU_add_test(suite, "test_nvme_allocate_request",
			    test_nvme_allocate_request) == NULL ||
		CU_add_test(suite, "test_nvme_free_request",
			    test_nvme_free_request) == NULL ||
		CU_add_test(suite, "test_nvme_allocate_request_user_copy",
			    test_nvme_allocate_request_user_copy) == NULL ||
		CU_add_test(suite, "test_nvme_robust_mutex_init_shared",
			    test_nvme_robust_mutex_init_shared) == NULL
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
