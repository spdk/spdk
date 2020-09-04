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

SPDK_LOG_REGISTER_COMPONENT(nvme)

DEFINE_STUB(nvme_wait_for_completion_robust_lock, int,
	    (struct spdk_nvme_qpair *qpair,
	     struct nvme_completion_poll_status *status,
	     pthread_mutex_t *robust_mutex), 0);
DEFINE_STUB(nvme_ctrlr_multi_iocs_enabled, bool, (struct spdk_nvme_ctrlr *ctrlr), true);

int
nvme_ctrlr_cmd_identify(struct spdk_nvme_ctrlr *ctrlr, uint8_t cns, uint16_t cntid, uint32_t nsid,
			uint8_t csi, void *payload, size_t payload_size,
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
	uint32_t id = 1;
	struct spdk_nvme_ns_data nsdata = {};
	struct spdk_nvme_ctrlr ctrlr = { .nsdata = &nsdata };
	const struct spdk_uuid *uuid;
	struct spdk_uuid expected_uuid;

	memset(&expected_uuid, 0xA5, sizeof(expected_uuid));

	/* Empty list - no UUID should be found */
	nvme_ns_construct(&ns, id, &ctrlr);
	uuid = spdk_nvme_ns_get_uuid(&ns);
	CU_ASSERT(uuid == NULL);
	nvme_ns_destruct(&ns);

	/* NGUID only (no UUID in list) */
	nvme_ns_construct(&ns, id, &ctrlr);
	ns.id_desc_list[0] = 0x02; /* NIDT == NGUID */
	ns.id_desc_list[1] = 0x10; /* NIDL */
	memset(&ns.id_desc_list[4], 0xCC, 0x10);
	uuid = spdk_nvme_ns_get_uuid(&ns);
	CU_ASSERT(uuid == NULL);
	nvme_ns_destruct(&ns);

	/* Just UUID in the list */
	nvme_ns_construct(&ns, id, &ctrlr);
	ns.id_desc_list[0] = 0x03; /* NIDT == UUID */
	ns.id_desc_list[1] = 0x10; /* NIDL */
	memcpy(&ns.id_desc_list[4], &expected_uuid, sizeof(expected_uuid));
	uuid = spdk_nvme_ns_get_uuid(&ns);
	SPDK_CU_ASSERT_FATAL(uuid != NULL);
	CU_ASSERT(memcmp(uuid, &expected_uuid, sizeof(*uuid)) == 0);
	nvme_ns_destruct(&ns);

	/* UUID followed by NGUID */
	nvme_ns_construct(&ns, id, &ctrlr);
	ns.id_desc_list[0] = 0x03; /* NIDT == UUID */
	ns.id_desc_list[1] = 0x10; /* NIDL */
	memcpy(&ns.id_desc_list[4], &expected_uuid, sizeof(expected_uuid));
	ns.id_desc_list[20] = 0x02; /* NIDT == NGUID */
	ns.id_desc_list[21] = 0x10; /* NIDL */
	memset(&ns.id_desc_list[24], 0xCC, 0x10);
	uuid = spdk_nvme_ns_get_uuid(&ns);
	SPDK_CU_ASSERT_FATAL(uuid != NULL);
	CU_ASSERT(memcmp(uuid, &expected_uuid, sizeof(*uuid)) == 0);
	nvme_ns_destruct(&ns);

	/* NGUID followed by UUID */
	nvme_ns_construct(&ns, id, &ctrlr);
	ns.id_desc_list[0] = 0x02; /* NIDT == NGUID */
	ns.id_desc_list[1] = 0x10; /* NIDL */
	memset(&ns.id_desc_list[4], 0xCC, 0x10);
	ns.id_desc_list[20] = 0x03; /* NIDT = UUID */
	ns.id_desc_list[21] = 0x10; /* NIDL */
	memcpy(&ns.id_desc_list[24], &expected_uuid, sizeof(expected_uuid));
	uuid = spdk_nvme_ns_get_uuid(&ns);
	SPDK_CU_ASSERT_FATAL(uuid != NULL);
	CU_ASSERT(memcmp(uuid, &expected_uuid, sizeof(*uuid)) == 0);
	nvme_ns_destruct(&ns);
}

static void
test_nvme_ns_csi(void)
{
	struct spdk_nvme_ns ns = {};
	uint32_t id = 1;
	struct spdk_nvme_ns_data nsdata = {};
	struct spdk_nvme_ctrlr ctrlr = { .nsdata = &nsdata };
	enum spdk_nvme_csi csi;

	/* Empty list - SPDK_NVME_CSI_NVM should be returned */
	nvme_ns_construct(&ns, id, &ctrlr);
	csi = nvme_ns_get_csi(&ns);
	CU_ASSERT(csi == SPDK_NVME_CSI_NVM);
	nvme_ns_destruct(&ns);

	/* NVM CSI - SPDK_NVME_CSI_NVM should be returned */
	nvme_ns_construct(&ns, id, &ctrlr);
	ns.id_desc_list[0] = 0x4; /* NIDT == CSI */
	ns.id_desc_list[1] = 0x1; /* NIDL */
	ns.id_desc_list[4] = 0x0; /* SPDK_NVME_CSI_NVM */
	csi = nvme_ns_get_csi(&ns);
	CU_ASSERT(csi == SPDK_NVME_CSI_NVM);
	nvme_ns_destruct(&ns);

	/* NGUID followed by ZNS CSI - SPDK_NVME_CSI_ZNS should be returned */
	nvme_ns_construct(&ns, id, &ctrlr);
	ns.id_desc_list[0] = 0x02; /* NIDT == NGUID */
	ns.id_desc_list[1] = 0x10; /* NIDL */
	memset(&ns.id_desc_list[4], 0xCC, 0x10);
	ns.id_desc_list[20] = 0x4; /* NIDT == CSI */
	ns.id_desc_list[21] = 0x1; /* NIDL */
	ns.id_desc_list[24] = 0x2; /* SPDK_NVME_CSI_ZNS */
	csi = nvme_ns_get_csi(&ns);
	CU_ASSERT(csi == SPDK_NVME_CSI_ZNS);
	nvme_ns_destruct(&ns);

	/* KV CSI followed by NGUID - SPDK_NVME_CSI_KV should be returned */
	nvme_ns_construct(&ns, id, &ctrlr);
	ns.id_desc_list[0] = 0x4; /* NIDT == CSI */
	ns.id_desc_list[1] = 0x1; /* NIDL */
	ns.id_desc_list[4] = 0x1; /* SPDK_NVME_CSI_KV */
	ns.id_desc_list[5] = 0x02; /* NIDT == NGUID */
	ns.id_desc_list[6] = 0x10; /* NIDL */
	memset(&ns.id_desc_list[9], 0xCC, 0x10);
	csi = nvme_ns_get_csi(&ns);
	CU_ASSERT(csi == SPDK_NVME_CSI_KV);
	nvme_ns_destruct(&ns);
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
	CU_ADD_TEST(suite, test_nvme_ns_csi);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
