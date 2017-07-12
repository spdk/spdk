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

#include "lib/test_env.c"

#include "spdk_internal/mock.h"

DEFINE_STUB_V(nvme_ctrlr_destruct, (struct spdk_nvme_ctrlr *ctrlr))

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

/* stub callback used by test_nvme_user_copy_cmd_complete() */
static int ut_dummy_cb_ret = 0;
static void
dummy_cb(void *user_cb_arg, struct spdk_nvme_cpl *cpl)
{
	ut_dummy_cb_ret  = 1;
}

static void
test_nvme_user_copy_cmd_complete(void)
{
	struct nvme_request req;
	struct spdk_nvme_cpl cpl;
	int test_data = 0xdeadbeef;
	int buff_size = sizeof(int);

	memset(&req, 0, sizeof(struct nvme_request));
	memset(&cpl, 0, sizeof(struct spdk_nvme_cpl));

	/* without user buffer */
	req.user_cb_fn = (void *)dummy_cb;
	ut_dummy_cb_ret = 0;

	nvme_user_copy_cmd_complete(&req, &cpl);
	CU_ASSERT(ut_dummy_cb_ret == 1);

	/* with user buffer */
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

	ut_dummy_cb_ret = 0;
	/* mocking this to prevent the calling code from freeing the buff */
	/* buff as it confuses either valgrind or the static analyzer */
	MOCK_SET_P(spdk_dma_zmalloc, void *, NULL);
	nvme_user_copy_cmd_complete(&req, &cpl);
	CU_ASSERT(memcmp(req.user_buffer, &test_data, buff_size) == 0);
	CU_ASSERT(ut_dummy_cb_ret == 1);
	free(req.user_buffer);
	free(req.payload.u.contig);
	/* return spdk_dma_zmalloc/freee to unmocked */
	MOCK_SET_P(spdk_dma_zmalloc, void *, &ut_spdk_dma_zmalloc);
}

static void
test_nvme_allocate_request(void)
{
	struct spdk_nvme_qpair qpair;
	struct nvme_payload payload;
	uint32_t payload_size = sizeof(struct nvme_payload);
	spdk_nvme_cmd_cb cb_fn = NULL;
	void *cb_arg = NULL;
	struct nvme_request *req = NULL;
	struct nvme_request match_req;

	memset(&payload, 0, sizeof(struct nvme_payload));
	STAILQ_INIT(&qpair.free_req);
	STAILQ_INIT(&qpair.queued_req);

	/* empty queue */
	req = nvme_allocate_request(&qpair, &payload, payload_size,
				    cb_fn, cb_arg);
	CU_ASSERT(req == NULL);

	/* put a dummy on the queue, make sure it matches upon return */
	memset(&match_req, 0, sizeof(struct nvme_request));
	match_req.cb_fn = cb_fn;
	match_req.cb_arg = cb_arg;
	match_req.payload = payload;
	match_req.payload_size = payload_size;
	match_req.payload.type = NVME_PAYLOAD_TYPE_SGL;
	match_req.qpair = &qpair;
	match_req.pid = getpid();
	STAILQ_INSERT_HEAD(&qpair.free_req, &match_req, stailq);

	req = nvme_allocate_request(&qpair, &payload, payload_size,
				    cb_fn, cb_arg);
	CU_ASSERT(req != NULL);
	CU_ASSERT(memcmp(req, &match_req,
			 sizeof(struct nvme_request)) == 0);
}

static void
test_nvme_allocate_request_null(void)
{
	struct spdk_nvme_qpair qpair;
	struct nvme_payload payload;
	spdk_nvme_cmd_cb cb_fn = NULL;
	void *cb_arg = NULL;
	struct nvme_request *req = NULL;
	struct nvme_request match_req;

	memset(&payload, 0, sizeof(struct nvme_payload));
	STAILQ_INIT(&qpair.free_req);
	STAILQ_INIT(&qpair.queued_req);

	/* put a dummy on the queue, make sure it matches upon return */
	memset(&match_req, 0x5a, sizeof(struct nvme_request));
	match_req.payload.type = NVME_PAYLOAD_TYPE_CONTIG;
	match_req.qpair = &qpair;
	match_req.pid = getpid();
	STAILQ_INSERT_HEAD(&qpair.free_req, &match_req, stailq);

	req = nvme_allocate_request_null(&qpair, cb_fn, cb_arg);
	CU_ASSERT(req != NULL);
	/* set req->paylaod simply to make valgrind happy */
	req->payload = payload;
	CU_ASSERT(memcmp(req, &match_req,
			 sizeof(struct nvme_request)) == 0);
}

static void
test_nvme_free_request(void)
{
	struct nvme_request dummy_req;
	struct nvme_request *return_req;

	/* init req qpair and put a dummy on it, make sure it gets */
	/* moved to freeq */
	memset(&dummy_req.cmd, 0x5a, sizeof(struct spdk_nvme_cmd));
	dummy_req.num_children = 0;
	dummy_req.qpair = malloc(sizeof(*dummy_req.qpair));
	STAILQ_INIT(&dummy_req.qpair->free_req);

	nvme_free_request(&dummy_req);
	return_req = STAILQ_FIRST(&dummy_req.qpair->free_req);
	CU_ASSERT(memcmp(return_req, &dummy_req,
			 sizeof(struct spdk_nvme_cmd)) == 0);
	free(dummy_req.qpair);
}

static void
test_nvme_allocate_request_user_copy(void)
{
	struct spdk_nvme_qpair qpair;
	void *buffer = NULL;
	uint32_t payload_size = 0;
	spdk_nvme_cmd_cb cb_fn = {0};
	void *cb_arg = NULL;
	bool host_to_controller = true;
	struct nvme_request *req;
	struct nvme_request dummy_req;
	int test_data = 0xdeadbeef;
	int buff_size = sizeof(int);

	memset(&qpair, 0, sizeof(struct spdk_nvme_qpair));

	/* no buffer or valid payload size, early NULL return */
	req = nvme_allocate_request_user_copy(&qpair, buffer, payload_size, cb_fn,
					      cb_arg, host_to_controller);
	CU_ASSERT(req == NULL);

	/* good buffer and valid payload size */
	buffer = malloc(buff_size);
	SPDK_CU_ASSERT_FATAL(buffer != NULL);
	memcpy(buffer, &test_data, buff_size);
	payload_size = buff_size;
	/* put a dummy on the queue */
	STAILQ_INIT(&qpair.free_req);
	STAILQ_INIT(&qpair.queued_req);
	memset(&dummy_req, 0, sizeof(struct nvme_request));
	STAILQ_INSERT_HEAD(&qpair.free_req, &dummy_req, stailq);

	req = nvme_allocate_request_user_copy(&qpair, buffer, payload_size, cb_fn,
					      cb_arg, host_to_controller);
	CU_ASSERT(req->user_cb_fn == cb_fn);
	CU_ASSERT(req->user_cb_arg == cb_arg);
	CU_ASSERT(req->user_buffer == buffer);
	CU_ASSERT(req->cb_arg == req);
	CU_ASSERT(memcmp(req->payload.u.contig, buffer, sizeof(buff_size)) == 0);
	spdk_dma_free(req->payload.u.contig);

	/* same thing but additional path coverage, no copy */
	host_to_controller = false;
	memset(&dummy_req, 0, sizeof(struct nvme_request));
	STAILQ_INSERT_HEAD(&qpair.free_req, &dummy_req, stailq);

	req = nvme_allocate_request_user_copy(&qpair, buffer, payload_size, cb_fn,
					      cb_arg, host_to_controller);
	CU_ASSERT(req->user_cb_fn == cb_fn);
	CU_ASSERT(req->user_cb_arg == cb_arg);
	CU_ASSERT(req->user_buffer == buffer);
	CU_ASSERT(req->cb_arg == req);
	CU_ASSERT(memcmp(req->payload.u.contig, buffer, sizeof(buff_size)) != 0);
	spdk_dma_free(req->payload.u.contig);

	/* good buffer and valid payload size but make spdk_dma_zmalloc fail */
	payload_size = buff_size;
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
		CU_add_test(suite, "test_nvme_allocate_request_user_copy",
			    test_nvme_allocate_request_user_copy) == NULL ||
		CU_add_test(suite, "test_nvme_free_request",
			    test_nvme_free_request) == NULL ||
		CU_add_test(suite, "test_nvme_allocate_request",
			    test_nvme_allocate_request) == NULL ||
		CU_add_test(suite, "test_nvme_allocate_request_null",
			    test_nvme_allocate_request_null) == NULL ||
		CU_add_test(suite, "test_nvme_user_copy_cmd_complete",
			    test_nvme_user_copy_cmd_complete) == NULL ||
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
