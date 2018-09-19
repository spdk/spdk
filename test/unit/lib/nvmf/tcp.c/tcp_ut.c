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
#include "spdk_internal/mock.h"

#include "common/lib/test_env.c"
#include "nvmf/ctrlr.c"
#include "nvmf/tcp.c"

#define UT_IPV4_ADDR "192.168.0.1"
#define UT_PORT "4420"
#define UT_NVMF_ADRFAM_INVALID 0xf
#define UT_MAX_QUEUE_DEPTH 128

SPDK_LOG_REGISTER_COMPONENT("nvmf", SPDK_LOG_NVMF)

struct spdk_trace_histories *g_trace_histories;

struct spdk_bdev {
	int ut_mock;
	uint64_t blockcnt;
};

int
spdk_nvmf_request_complete(struct spdk_nvmf_request *req)
{
	return 0;
}

struct spdk_nvmf_subsystem *
spdk_nvmf_tgt_find_subsystem(struct spdk_nvmf_tgt *tgt, const char *subnqn)
{
	return NULL;
}

struct spdk_nvmf_ctrlr *
spdk_nvmf_subsystem_get_ctrlr(struct spdk_nvmf_subsystem *subsystem, uint16_t cntlid)
{
	return NULL;
}
int
spdk_nvmf_subsystem_add_ctrlr(struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ctrlr *ctrlr)
{
	return 0;
}

void
spdk_trace_register_object(uint8_t type, char id_prefix)
{
}

void
spdk_trace_register_description(const char *name, const char *short_name,
				uint16_t tpoint_id, uint8_t owner_type,
				uint8_t object_type, uint8_t new_object,
				uint8_t arg1_is_ptr, const char *arg1_name)
{
}

int
spdk_nvme_transport_id_compare(const struct spdk_nvme_transport_id *trid1,
			       const struct spdk_nvme_transport_id *trid2)
{
	return 0;
}

void
_spdk_trace_record(uint64_t tsc, uint16_t tpoint_id, uint16_t poller_id,
		   uint32_t size, uint64_t object_id, uint64_t arg1)
{
}

int
spdk_nvmf_qpair_disconnect(struct spdk_nvmf_qpair *qpair, nvmf_qpair_disconnect_cb cb_fn, void *ctx)
{
	return 0;
}

void
spdk_nvmf_subsystem_remove_ctrlr(struct spdk_nvmf_subsystem *subsystem,
				 struct spdk_nvmf_ctrlr *ctrlr)
{
}

void
spdk_nvmf_get_discovery_log_page(struct spdk_nvmf_tgt *tgt, void *buffer,
				 uint64_t offset, uint32_t length)
{
}

struct spdk_nvmf_ns *
spdk_nvmf_subsystem_get_first_ns(struct spdk_nvmf_subsystem *subsystem)
{
	abort();
	return NULL;
}

struct spdk_nvmf_ns *
spdk_nvmf_subsystem_get_next_ns(struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ns *prev_ns)
{
	abort();
	return NULL;
}

int
spdk_nvmf_bdev_ctrlr_identify_ns(struct spdk_nvmf_ns *ns, struct spdk_nvme_ns_data *nsdata)
{
	uint64_t num_blocks;

	SPDK_CU_ASSERT_FATAL(ns->bdev != NULL);
	num_blocks = ns->bdev->blockcnt;
	nsdata->nsze = num_blocks;
	nsdata->ncap = num_blocks;
	nsdata->nuse = num_blocks;
	nsdata->nlbaf = 0;
	nsdata->flbas.format = 0;
	nsdata->lbaf[0].lbads = spdk_u32log2(512);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

const char *
spdk_nvmf_subsystem_get_sn(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->sn;
}

bool
spdk_nvmf_ctrlr_dsm_supported(struct spdk_nvmf_ctrlr *ctrlr)
{
	return false;
}

bool
spdk_nvmf_ctrlr_write_zeroes_supported(struct spdk_nvmf_ctrlr *ctrlr)
{
	return false;
}

int
spdk_nvmf_request_free(struct spdk_nvmf_request *req)
{
	return 0;
}

bool
spdk_nvmf_subsystem_host_allowed(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{
	return true;
}

int
spdk_nvmf_transport_qpair_set_sqsize(struct spdk_nvmf_qpair *qpair)
{
	return 0;
}

void
spdk_trace_add_register_fn(struct spdk_trace_register_fn *reg_fn)
{
}

void
spdk_nvmf_request_exec(struct spdk_nvmf_request *req)
{
}

static void
test_nvmf_tcp_qpair_is_idle(void)
{
	struct spdk_nvmf_tcp_qpair tqpair;

	memset(&tqpair, 0, sizeof(tqpair));

	/* case 1 */
	tqpair.max_queue_depth = 0;
	tqpair.state_cntr[TCP_REQUEST_STATE_FREE] = 0;
	CU_ASSERT(spdk_nvmf_tcp_qpair_is_idle(&tqpair.qpair) == true);

	/* case 2 */
	tqpair.max_queue_depth = UT_MAX_QUEUE_DEPTH;
	tqpair.state_cntr[TCP_REQUEST_STATE_FREE] = 0;
	CU_ASSERT(spdk_nvmf_tcp_qpair_is_idle(&tqpair.qpair) == false);

	/* case 3 */
	tqpair.state_cntr[TCP_REQUEST_STATE_FREE] = 1;
	CU_ASSERT(spdk_nvmf_tcp_qpair_is_idle(&tqpair.qpair) == false);

	/* case 4 */
	tqpair.state_cntr[TCP_REQUEST_STATE_FREE] = UT_MAX_QUEUE_DEPTH;
	CU_ASSERT(spdk_nvmf_tcp_qpair_is_idle(&tqpair.qpair) == true);
}

static void
test_nvmf_tcp_qpair_get_local_trid(void)
{
	struct spdk_nvmf_tcp_qpair tqpair;
	struct sockaddr_in saddr_in;
	struct spdk_nvme_transport_id trid;

	/* case 1 */
	memset(&tqpair, 0, sizeof(tqpair));
	memset(&saddr_in, 0, sizeof(saddr_in));
	saddr_in.sin_family = AF_INET;
	saddr_in.sin_port = htons(atoi(UT_PORT));
	saddr_in.sin_addr.s_addr = inet_addr(UT_IPV4_ADDR);
	memcpy(tqpair.target_addr, &saddr_in, sizeof(saddr_in));
	/* expect success */
	CU_ASSERT(spdk_nvmf_tcp_qpair_get_local_trid(&tqpair.qpair, &trid) == 0);
	CU_ASSERT(trid.adrfam == SPDK_NVMF_ADRFAM_IPV4);
	CU_ASSERT(strcmp(trid.traddr, UT_IPV4_ADDR) == 0);

	/* case 2 */
	memset(&tqpair, 0, sizeof(tqpair));
	memset(&saddr_in, 0, sizeof(saddr_in));
	saddr_in.sin_family = UT_NVMF_ADRFAM_INVALID;
	saddr_in.sin_port = htons(atoi(UT_PORT));
	saddr_in.sin_addr.s_addr = inet_addr(UT_IPV4_ADDR);
	memcpy(tqpair.target_addr, &saddr_in, sizeof(saddr_in));
	/* expect failure */
	CU_ASSERT(spdk_nvmf_tcp_qpair_get_local_trid(&tqpair.qpair, &trid) == -1);
	memset(&tqpair, 0, sizeof(tqpair));

	/* case 3 */
	memset(&saddr_in, 0, sizeof(saddr_in));
	saddr_in.sin_family = AF_INET;
	saddr_in.sin_port = htons(atoi(UT_PORT));
	saddr_in.sin_addr.s_addr = inet_addr(UT_IPV4_ADDR);
	memcpy(tqpair.initiator_addr, &saddr_in, sizeof(saddr_in));
	/* expect failure */
	CU_ASSERT(spdk_nvmf_tcp_qpair_get_local_trid(&tqpair.qpair, &trid) == -1);
}

static void
test_nvmf_tcp_qpair_get_peer_trid(void)
{
	struct spdk_nvmf_tcp_qpair tqpair;
	struct sockaddr_in saddr_in;
	struct spdk_nvme_transport_id trid;

	/* case 1 */
	memset(&tqpair, 0, sizeof(tqpair));
	memset(&saddr_in, 0, sizeof(saddr_in));
	saddr_in.sin_family = AF_INET;
	saddr_in.sin_port = htons(atoi(UT_PORT));
	saddr_in.sin_addr.s_addr = inet_addr(UT_IPV4_ADDR);
	memcpy(tqpair.initiator_addr, &saddr_in, sizeof(saddr_in));
	/* expect success */
	CU_ASSERT(spdk_nvmf_tcp_qpair_get_peer_trid(&tqpair.qpair, &trid) == 0);
	CU_ASSERT(trid.adrfam == SPDK_NVMF_ADRFAM_IPV4);
	CU_ASSERT(strcmp(trid.traddr, UT_IPV4_ADDR) == 0);

	/* case 2 */
	memset(&tqpair, 0, sizeof(tqpair));
	memset(&saddr_in, 0, sizeof(saddr_in));
	saddr_in.sin_family = UT_NVMF_ADRFAM_INVALID;
	saddr_in.sin_port = htons(atoi(UT_PORT));
	saddr_in.sin_addr.s_addr = inet_addr(UT_IPV4_ADDR);
	memcpy(tqpair.initiator_addr, &saddr_in, sizeof(saddr_in));
	/* expect failure */
	CU_ASSERT(spdk_nvmf_tcp_qpair_get_peer_trid(&tqpair.qpair, &trid) == -1);
	memset(&tqpair, 0, sizeof(tqpair));

	/* case 3 */
	memset(&saddr_in, 0, sizeof(saddr_in));
	saddr_in.sin_family = AF_INET;
	saddr_in.sin_port = htons(atoi(UT_PORT));
	saddr_in.sin_addr.s_addr = inet_addr(UT_IPV4_ADDR);
	memcpy(tqpair.target_addr, &saddr_in, sizeof(saddr_in));
	/* expect failure */
	CU_ASSERT(spdk_nvmf_tcp_qpair_get_peer_trid(&tqpair.qpair, &trid) == -1);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvmf", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "nvmf_tcp_qpair_get_peer_trid", test_nvmf_tcp_qpair_get_peer_trid) == NULL ||
		CU_add_test(suite, "nvmf_tcp_qpair_get_local_trid", test_nvmf_tcp_qpair_get_local_trid) == NULL ||
		CU_add_test(suite, "nvmf_tcp_qpair_is_idle", test_nvmf_tcp_qpair_is_idle) == NULL
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
