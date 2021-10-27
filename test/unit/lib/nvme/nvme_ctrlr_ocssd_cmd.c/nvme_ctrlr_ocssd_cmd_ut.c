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
#include "common/lib/test_env.c"

#include "nvme/nvme_ctrlr_ocssd_cmd.c"

DEFINE_STUB(spdk_nvme_ctrlr_get_first_active_ns, uint32_t,
	    (struct spdk_nvme_ctrlr *ctrlr), 1);

#define DECLARE_AND_CONSTRUCT_CTRLR()	\
	struct spdk_nvme_ctrlr	ctrlr = {};	\
	struct spdk_nvme_qpair	adminq = {};	\
	struct nvme_request	req;		\
						\
	STAILQ_INIT(&adminq.free_req);		\
	STAILQ_INSERT_HEAD(&adminq.free_req, &req, stailq);	\
	ctrlr.adminq = &adminq;	\
	CU_ASSERT(pthread_mutex_init(&ctrlr.ctrlr_lock, NULL) == 0);

#define DECONSTRUCT_CTRLR() \
	CU_ASSERT(pthread_mutex_destroy(&ctrlr.ctrlr_lock) == 0);

pid_t g_spdk_nvme_pid;
struct nvme_request g_req;
typedef void (*verify_request_fn_t)(struct nvme_request *req);
verify_request_fn_t verify_fn;

static const uint32_t expected_geometry_ns = 1;

static int
nvme_ns_cmp(struct spdk_nvme_ns *ns1, struct spdk_nvme_ns *ns2)
{
	return ns1->id - ns2->id;
}

RB_GENERATE_STATIC(nvme_ns_tree, spdk_nvme_ns, node, nvme_ns_cmp);

static struct spdk_nvme_ns g_inactive_ns = {};

struct spdk_nvme_ns *
spdk_nvme_ctrlr_get_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct spdk_nvme_ns tmp;
	struct spdk_nvme_ns *ns;

	if (nsid < 1 || nsid > ctrlr->cdata.nn) {
		return NULL;
	}

	tmp.id = nsid;
	ns = RB_FIND(nvme_ns_tree, &ctrlr->ns, &tmp);

	if (ns == NULL) {
		return &g_inactive_ns;
	}

	return ns;
}

int
nvme_ctrlr_submit_admin_request(struct spdk_nvme_ctrlr *ctrlr, struct nvme_request *req)
{
	verify_fn(req);
	memset(req, 0, sizeof(*req));
	return 0;
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
	DECLARE_AND_CONSTRUCT_CTRLR();

	struct spdk_ocssd_geometry_data geo;

	verify_fn = verify_geometry_cmd;

	spdk_nvme_ocssd_ctrlr_cmd_geometry(&ctrlr, expected_geometry_ns, &geo,
					   sizeof(geo), NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_spdk_nvme_ctrlr_is_ocssd_supported(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_ns ns = {};
	bool rc;

	RB_INIT(&ctrlr.ns);
	ns.id = 1;
	RB_INSERT(nvme_ns_tree, &ctrlr.ns, &ns);

	ns.nsdata.vendor_specific[0] = 1;
	ctrlr.quirks |= NVME_QUIRK_OCSSD;
	ctrlr.cdata.vid = SPDK_PCI_VID_CNEXLABS;
	ctrlr.cdata.nn = 1;

	rc = spdk_nvme_ctrlr_is_ocssd_supported(&ctrlr);
	CU_ASSERT(rc == true);

	/* Clear quirks`s ocssd flag. */
	ctrlr.quirks = 0;

	rc = spdk_nvme_ctrlr_is_ocssd_supported(&ctrlr);
	CU_ASSERT(rc == false);

	/* NS count is 0. */
	ctrlr.cdata.nn = 0;

	rc = spdk_nvme_ctrlr_is_ocssd_supported(&ctrlr);
	CU_ASSERT(rc == false);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_ctrlr_cmd", NULL, NULL);

	CU_ADD_TEST(suite, test_geometry_cmd);
	CU_ADD_TEST(suite, test_spdk_nvme_ctrlr_is_ocssd_supported);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
