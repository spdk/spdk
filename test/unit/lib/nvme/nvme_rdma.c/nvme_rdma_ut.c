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
#include "nvme/nvme_rdma.c"
#include "common/lib/test_env.c"

static void
test_nvme_rdma_qpair_submit_request(void)
{
	struct nvme_rdma_qpair dummy_rqpair = {0};
	struct spdk_nvme_rdma_req dummy_req;
	struct nvme_request req, *temp;
	struct spdk_mem_map *dummy_map;
	int rc;

	STAILQ_INIT(&dummy_rqpair.qpair.free_req);
	STAILQ_INIT(&dummy_rqpair.qpair.queued_req);
	TAILQ_INIT(&dummy_rqpair.qpair.err_cmd_head);
	STAILQ_INIT(&dummy_rqpair.qpair.err_req_head);

	/* No available requests, return 0, req is now queued */
	nvme_rdma_qpair_submit_request(&dummy_rqpair.qpair, &req);
	CU_ASSERT(rc = 0);
	temp = STAILQ_FIRST(&dummy_rqpair.qpair.queued_req);
	CU_ASSERT(temp = &req);
	STAILQ_REMOVE(&dummy_rqpair.qpair.queued_req, temp, nvme_request, stailq);
	CU_ASSERT(TAILQ_EMPTY(&dummy_rqpair.qpair.queued_req));

	/* nvme_rdma_req_init failure invalid request type */
	TAILQ_INSERT_HEAD(&rqpair.free_reqs, dummy_req, link);

	/* nvme_rdma_req_init failure contig, buffer larger than memory region */

	/* nvme_rdma_req_init failure sgl, buffer larger than memory region */

	/* successfully send request contig */

	/* successfully send request sgl */

	/* successfully send request null */

}

int main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvme_rdma", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (

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
