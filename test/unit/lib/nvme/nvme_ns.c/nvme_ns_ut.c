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

#include "nvme/nvme_ns.c"

#include "common/lib/test_env.c"

SPDK_LOG_REGISTER_COMPONENT("nvme", SPDK_LOG_NVME)

DEFINE_STUB(spdk_nvme_wait_for_completion_robust_lock, int,
	    (struct spdk_nvme_qpair *qpair,
	     struct nvme_completion_poll_status *status,
	     pthread_mutex_t *robust_mutex), 0);

int
nvme_ctrlr_cmd_identify(struct spdk_nvme_ctrlr *ctrlr, uint8_t cns, uint16_t cntid, uint32_t nsid,
			void *payload, size_t payload_size,
			spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return -1;
}

void
nvme_completion_poll_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
}

int32_t
spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	return -1;
}

static void
test_nvme_ns_construct(void)
{
	struct spdk_nvme_ns ns = {};
	uint32_t id = 1;
	struct spdk_nvme_ctrlr ctrlr = {};

	nvme_ns_construct(&ns, id, &ctrlr);
	CU_ASSERT(ns.id == 1);
}

static void
test_nvme_ns_uuid(void)
{
	struct spdk_nvme_ns ns = {};
	const struct spdk_uuid *uuid;
	struct spdk_uuid expected_uuid;

	memset(&expected_uuid, 0xA5, sizeof(expected_uuid));

	/* Empty list - no UUID should be found */
	memset(ns.id_desc_list, 0, sizeof(ns.id_desc_list));
	uuid = spdk_nvme_ns_get_uuid(&ns);
	CU_ASSERT(uuid == NULL);

	/* NGUID only (no UUID in list) */
	memset(ns.id_desc_list, 0, sizeof(ns.id_desc_list));
	ns.id_desc_list[0] = 0x02; /* NIDT == NGUID */
	ns.id_desc_list[1] = 0x10; /* NIDL */
	memset(&ns.id_desc_list[4], 0xCC, 0x10);
	uuid = spdk_nvme_ns_get_uuid(&ns);
	CU_ASSERT(uuid == NULL);

	/* Just UUID in the list */
	memset(ns.id_desc_list, 0, sizeof(ns.id_desc_list));
	ns.id_desc_list[0] = 0x03; /* NIDT == UUID */
	ns.id_desc_list[1] = 0x10; /* NIDL */
	memcpy(&ns.id_desc_list[4], &expected_uuid, sizeof(expected_uuid));
	uuid = spdk_nvme_ns_get_uuid(&ns);
	SPDK_CU_ASSERT_FATAL(uuid != NULL);
	CU_ASSERT(memcmp(uuid, &expected_uuid, sizeof(*uuid)) == 0);

	/* UUID followed by NGUID */
	memset(ns.id_desc_list, 0, sizeof(ns.id_desc_list));
	ns.id_desc_list[0] = 0x03; /* NIDT == UUID */
	ns.id_desc_list[1] = 0x10; /* NIDL */
	memcpy(&ns.id_desc_list[4], &expected_uuid, sizeof(expected_uuid));
	ns.id_desc_list[20] = 0x02; /* NIDT == NGUID */
	ns.id_desc_list[21] = 0x10; /* NIDL */
	memset(&ns.id_desc_list[24], 0xCC, 0x10);
	uuid = spdk_nvme_ns_get_uuid(&ns);
	SPDK_CU_ASSERT_FATAL(uuid != NULL);
	CU_ASSERT(memcmp(uuid, &expected_uuid, sizeof(*uuid)) == 0);

	/* NGUID followed by UUID */
	memset(ns.id_desc_list, 0, sizeof(ns.id_desc_list));
	ns.id_desc_list[0] = 0x02; /* NIDT == NGUID */
	ns.id_desc_list[1] = 0x10; /* NIDL */
	memset(&ns.id_desc_list[4], 0xCC, 0x10);
	ns.id_desc_list[20] = 0x03; /* NIDT = UUID */
	ns.id_desc_list[21] = 0x10; /* NIDL */
	memcpy(&ns.id_desc_list[24], &expected_uuid, sizeof(expected_uuid));
	uuid = spdk_nvme_ns_get_uuid(&ns);
	SPDK_CU_ASSERT_FATAL(uuid != NULL);
	CU_ASSERT(memcmp(uuid, &expected_uuid, sizeof(*uuid)) == 0);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme", NULL, NULL);

	CU_ADD_TEST(suite, test_nvme_ns_construct);
	CU_ADD_TEST(suite, test_nvme_ns_uuid);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
