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

#include "nvme/nvme_ctrlr_ocssd_cmd.c"

struct nvme_request g_req;
typedef void (*verify_request_fn_t)(struct nvme_request *req);
verify_request_fn_t verify_fn;

static const uint32_t expected_geometry_ns = 1;

int
nvme_ctrlr_submit_admin_request(struct spdk_nvme_ctrlr *ctrlr, struct nvme_request *req)
{
	verify_fn(req);
	memset(req, 0, sizeof(*req));
	return 0;
}

struct nvme_request *
nvme_allocate_request(struct spdk_nvme_qpair *qpair,
		      const struct nvme_payload *payload, uint32_t payload_size,
		      spdk_nvme_cmd_cb cb_fn,
		      void *cb_arg)
{
	struct nvme_request *req = &g_req;

	memset(req, 0, sizeof(*req));

	req->payload = *payload;
	req->payload_size = payload_size;

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
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
	payload.md = NULL;

	return nvme_allocate_request(qpair, &payload, payload_size, cb_fn, cb_arg);
}

struct nvme_request *
nvme_allocate_request_user_copy(struct spdk_nvme_qpair *qpair, void *buffer, uint32_t payload_size,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg, bool host_to_controller)
{
	/* For the unit test, we don't actually need to copy the buffer */
	return nvme_allocate_request_contig(qpair, buffer, payload_size, cb_fn, cb_arg);
}


static void verify_geometry_cmd(struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_OCSSD_OPC_GEOMETRY);
	CU_ASSERT(req->cmd.nsid == expected_geometry_ns);
}

static void
test_geometry_cmd(void)
{
	struct spdk_nvme_ctrlr	ctrlr = {};
	struct spdk_ocssd_geometry_data geo;

	verify_fn = verify_geometry_cmd;

	spdk_nvme_ocssd_ctrlr_cmd_geometry(&ctrlr, expected_geometry_ns, &geo,
					   sizeof(geo), NULL, NULL);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvme_ctrlr_cmd", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test ocssd ctrlr geometry cmd ", test_geometry_cmd) == NULL
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
