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

#include "nvmf/ctrlr_bdev.c"


SPDK_LOG_REGISTER_COMPONENT("nvmf", SPDK_LOG_NVMF)

int
spdk_nvmf_request_complete(struct spdk_nvmf_request *req)
{
	return -1;
}

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return "test";
}

uint32_t
spdk_bdev_get_block_size(const struct spdk_bdev *bdev)
{
	abort();
	return 0;
}

uint64_t
spdk_bdev_get_num_blocks(const struct spdk_bdev *bdev)
{
	abort();
	return 0;
}

uint32_t
spdk_bdev_get_optimal_io_boundary(const struct spdk_bdev *bdev)
{
	abort();
	return 0;
}

struct spdk_io_channel *
spdk_bdev_get_io_channel(struct spdk_bdev_desc *desc)
{
	return NULL;
}

int
spdk_bdev_flush_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return 0;
}

int
spdk_bdev_unmap_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return 0;
}

bool
spdk_bdev_io_type_supported(struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type)
{
	return false;
}

int
spdk_bdev_queue_io_wait(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
			struct spdk_bdev_io_wait_entry *entry)
{
	return 0;
}

int
spdk_bdev_write_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, void *buf,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return 0;
}

int
spdk_bdev_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			struct iovec *iov, int iovcnt,
			uint64_t offset_blocks, uint64_t num_blocks,
			spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return 0;
}

int
spdk_bdev_read_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, void *buf,
		      uint64_t offset_blocks, uint64_t num_blocks,
		      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return 0;
}

int spdk_bdev_readv_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   struct iovec *iov, int iovcnt,
			   uint64_t offset_blocks, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return 0;
}

int
spdk_bdev_write_zeroes_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			      uint64_t offset_blocks, uint64_t num_blocks,
			      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return 0;
}

int
spdk_bdev_nvme_io_passthru(struct spdk_bdev_desc *desc,
			   struct spdk_io_channel *ch,
			   const struct spdk_nvme_cmd *cmd,
			   void *buf, size_t nbytes,
			   spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return 0;
}

void spdk_bdev_free_io(struct spdk_bdev_io *bdev_io)
{
}

const char *spdk_nvmf_subsystem_get_nqn(struct spdk_nvmf_subsystem *subsystem)
{
	return NULL;
}

struct spdk_nvmf_ns *
spdk_nvmf_subsystem_get_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	abort();
	return NULL;
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

void spdk_bdev_io_get_nvme_status(const struct spdk_bdev_io *bdev_io, int *sct, int *sc)
{
}

/*
 * Reservation Unit Test Configuration
 *       --------             --------    --------
 *      | Host A |           | Host B |  | Host C |
 *       --------             --------    --------
 *      /        \               |           |
 *  --------   --------       -------     -------
 * |Ctrlr1_A| |Ctrlr2_A|     |Ctrlr_B|   |Ctrlr_C|
 *  --------   --------       -------     -------
 *    \           \              /           /
 *     \           \            /           /
 *      \           \          /           /
 *      --------------------------------------
 *     |            NAMESPACE 1               |
 *      --------------------------------------
 */

static struct spdk_nvmf_subsystem g_subsystem;
static struct spdk_nvmf_ctrlr g_ctrlr1_A, g_ctrlr2_A, g_ctrlr_B, g_ctrlr_C;
static struct spdk_nvmf_ns g_ns;

static void
ut_reservation_init(void)
{
	TAILQ_INIT(&g_subsystem.reg_head);
	pthread_mutex_init(&g_subsystem.reservation_lock, NULL);
	g_subsystem.regctl = 0;
	g_subsystem.gen = 0;

	/* Host A has two controllers */
	spdk_uuid_generate(&g_ctrlr1_A.hostid);
	g_ctrlr1_A.subsys = &g_subsystem;
	spdk_uuid_copy(&g_ctrlr2_A.hostid, &g_ctrlr1_A.hostid);
	g_ctrlr2_A.subsys = &g_subsystem;

	/* Host B has 1 controller */
	spdk_uuid_generate(&g_ctrlr_B.hostid);
	g_ctrlr_B.subsys = &g_subsystem;

	/* Host C has 1 controller */
	spdk_uuid_generate(&g_ctrlr_C.hostid);
	g_ctrlr_C.subsys = &g_subsystem;

	g_ns.subsystem = &g_subsystem;
}

static void
ut_reservation_deinit(void)
{
	struct spdk_nvmf_registrant *reg, *tmp;

	pthread_mutex_destroy(&g_subsystem.reservation_lock);
	TAILQ_FOREACH_SAFE(reg, &g_subsystem.reg_head, link, tmp) {
		TAILQ_REMOVE(&g_subsystem.reg_head, reg, link);
		free(reg);
	}
	g_ns.rtype = 0;
	g_ns.crkey = 0;
	g_ns.holder = NULL;
}

static struct spdk_nvmf_request *
ut_reservation_build_req(uint32_t length)
{
	struct spdk_nvmf_request *req;

	req = calloc(1, sizeof(*req));
	assert(req != NULL);

	req->data = calloc(1, length);
	assert(req->data != NULL);
	req->length = length;

	req->cmd = (union nvmf_h2c_msg *)calloc(1, sizeof(union nvmf_h2c_msg));
	assert(req->cmd != NULL);

	req->rsp = (union nvmf_c2h_msg *)calloc(1, sizeof(union nvmf_c2h_msg));
	assert(req->rsp != NULL);

	return req;
}

static void
ut_reservation_free_req(struct spdk_nvmf_request *req)
{
	free(req->cmd);
	free(req->rsp);
	free(req->data);
	free(req);
}

static void
ut_reservation_build_register_request(struct spdk_nvmf_request *req,
				      uint8_t rrega, uint8_t iekey,
				      uint8_t cptpl, uint64_t crkey,
				      uint64_t nrkey)
{
	uint32_t cdw10;
	struct spdk_nvme_reservation_register_data key;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	cdw10 = ((cptpl << 30) | (iekey << 3) | rrega);
	key.crkey = crkey;
	key.nrkey = nrkey;
	cmd->cdw10 = cdw10;
	memcpy(req->data, &key, sizeof(key));
}

static void
ut_reservation_build_acquire_request(struct spdk_nvmf_request *req,
				     uint8_t racqa, uint8_t iekey,
				     uint8_t rtype, uint64_t crkey,
				     uint64_t prkey)
{
	uint32_t cdw10;
	struct spdk_nvme_reservation_acquire_data key;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	cdw10 = ((rtype << 8) | (iekey << 3) | racqa);
	key.crkey = crkey;
	key.prkey = prkey;
	cmd->cdw10 = cdw10;
	memcpy(req->data, &key, sizeof(key));
}

static void
ut_reservation_build_release_request(struct spdk_nvmf_request *req,
				     uint8_t rrela, uint8_t iekey,
				     uint8_t rtype, uint64_t crkey)
{
	uint32_t cdw10;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	cdw10 = ((rtype << 8) | (iekey << 3) | rrela);
	cmd->cdw10 = cdw10;
	memcpy(req->data, &crkey, sizeof(crkey));
}

/*
 * Construct four registrants for other test cases.
 *
 * g_ctrlr1_A register with key 0xa1.
 * g_ctrlr2_A register with key 0xa1.
 * g_ctrlr_B register with key 0xb1.
 * g_ctrlr_C register with key 0xc1.
 * */
static void
ut_reservation_build_registrants(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;
	struct spdk_nvmf_registrant *reg;
	uint32_t gen;

	req = ut_reservation_build_req(16);
	rsp = &req->rsp->nvme_cpl;
	SPDK_CU_ASSERT_FATAL(req != NULL);
	gen = g_subsystem.gen;

	/* TEST CASE: g_ctrlr1_A register with a new key */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REGISTER_KEY,
					      0, 0, 0, 0xa1);
	nvmf_ns_reservation_register(req, &g_ctrlr1_A, &g_ns);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr1_A);
	SPDK_CU_ASSERT_FATAL(reg->rkey == 0xa1);
	SPDK_CU_ASSERT_FATAL(g_subsystem.gen == gen + 1);

	/* TEST CASE: g_ctrlr2_A register with a new key, because it has same
	 * Host Identifier with g_ctrlr1_A, so the register key should same.
	 */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REGISTER_KEY,
					      0, 0, 0, 0xa2);
	nvmf_ns_reservation_register(req, &g_ctrlr2_A, &g_ns);
	/* Reservation conflict for other key than 0xa1 */
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_RESERVATION_CONFLICT);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr2_A);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REGISTER_KEY,
					      0, 0, 0, 0xa1);
	nvmf_ns_reservation_register(req, &g_ctrlr2_A, &g_ns);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr2_A);
	SPDK_CU_ASSERT_FATAL(reg->rkey == 0xa1);
	SPDK_CU_ASSERT_FATAL(g_subsystem.gen == gen + 2);

	/* g_ctrlr_B register with a new key */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REGISTER_KEY,
					      0, 0, 0, 0xb1);
	nvmf_ns_reservation_register(req, &g_ctrlr_B, &g_ns);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr_B);
	SPDK_CU_ASSERT_FATAL(reg->rkey == 0xb1);
	SPDK_CU_ASSERT_FATAL(g_subsystem.gen == gen + 3);

	/* g_ctrlr_C register with a new key */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REGISTER_KEY,
					      0, 0, 0, 0xc1);
	nvmf_ns_reservation_register(req, &g_ctrlr_C, &g_ns);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr_C);
	SPDK_CU_ASSERT_FATAL(reg->rkey == 0xc1);
	SPDK_CU_ASSERT_FATAL(g_subsystem.gen == gen + 4);

	ut_reservation_free_req(req);
}

static void
test_reservation_register(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;
	struct spdk_nvmf_registrant *reg;
	uint32_t gen;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	rsp = &req->rsp->nvme_cpl;
	SPDK_CU_ASSERT_FATAL(req != NULL);

	ut_reservation_build_registrants();

	/* TEST CASE: Replace g_ctrlr1_A with a new key */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REPLACE_KEY,
					      0, 0, 0xa1, 0xa11);
	nvmf_ns_reservation_register(req, &g_ctrlr1_A, &g_ns);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr1_A);
	SPDK_CU_ASSERT_FATAL(reg->rkey == 0xa11);

	/* TEST CASE: Host A with g_ctrlr1_A get reservation with
	 * type SPDK_NVME_RESERVE_WRITE_EXCLUSIVE
	 */
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE, 0xa11, 0x0);
	gen = g_subsystem.gen;
	nvmf_ns_reservation_acquire(req, &g_ctrlr1_A, &g_ns);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr1_A);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE);
	SPDK_CU_ASSERT_FATAL(g_ns.crkey == 0xa11);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == reg);
	SPDK_CU_ASSERT_FATAL(g_subsystem.gen == gen);

	/* TEST CASE: g_ctrlr_C unregister with IEKEY enabled */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_UNREGISTER_KEY,
					      1, 0, 0, 0);
	nvmf_ns_reservation_register(req, &g_ctrlr_C, &g_ns);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr_C);
	SPDK_CU_ASSERT_FATAL(reg == NULL);

	/* TEST CASE: g_ctrlr_B unregister with correct key */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_UNREGISTER_KEY,
					      0, 0, 0xb1, 0);
	nvmf_ns_reservation_register(req, &g_ctrlr_B, &g_ns);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr_B);
	SPDK_CU_ASSERT_FATAL(reg == NULL);

	/* TEST CASE: g_ctrlr1_A unregister with correct key,
	 * reservation should be removed as well.
	 */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_UNREGISTER_KEY,
					      0, 0, 0xa11, 0);
	nvmf_ns_reservation_register(req, &g_ctrlr1_A, &g_ns);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr1_A);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == 0);
	SPDK_CU_ASSERT_FATAL(g_ns.crkey == 0);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == NULL);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static void
test_reservation_acquire_preempt_1(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;
	struct spdk_nvmf_registrant *reg;
	uint32_t gen;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	rsp = &req->rsp->nvme_cpl;
	SPDK_CU_ASSERT_FATAL(req != NULL);

	ut_reservation_build_registrants();

	gen = g_subsystem.gen;
	/* ACQUIRE: Host A with g_ctrlr1_A acquire reservation with
	 * type SPDK_NVME_RESERVE_WRITE_EXCLUSIVE.
	 */
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY, 0xa1, 0x0);
	nvmf_ns_reservation_acquire(req, &g_ctrlr1_A, &g_ns);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr1_A);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY);
	SPDK_CU_ASSERT_FATAL(g_ns.crkey == 0xa1);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == reg);
	SPDK_CU_ASSERT_FATAL(g_subsystem.gen == gen);

	/* TEST CASE: g_ctrlr1_A holds the reservation, g_ctrlr_B preempt g_ctrl1_A,
	 * g_ctrl1_A registrant is unregistred, and g_ctrlr2_A is still there.
	 */
	gen = g_subsystem.gen;
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_PREEMPT, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS, 0xb1, 0xa1);
	nvmf_ns_reservation_acquire(req, &g_ctrlr_B, &g_ns);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr1_A);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr2_A);
	SPDK_CU_ASSERT_FATAL(reg != NULL);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr_B);
	SPDK_CU_ASSERT_FATAL(reg != NULL);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == reg);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr_C);
	SPDK_CU_ASSERT_FATAL(reg != NULL);
	SPDK_CU_ASSERT_FATAL(g_subsystem.regctl == 3);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS);
	SPDK_CU_ASSERT_FATAL(g_subsystem.gen > gen);

	/* TEST CASE: g_ctrlr_B holds the reservation, g_ctrlr_C preempt g_ctrlr_B
	 * with valid key and PRKEY set to 0, all registrants other the host that issued
	 * the command are unregistered.
	 */
	gen = g_subsystem.gen;
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_PREEMPT, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS, 0xc1, 0x0);
	nvmf_ns_reservation_acquire(req, &g_ctrlr_C, &g_ns);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr2_A);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr_B);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr_C);
	SPDK_CU_ASSERT_FATAL(reg != NULL);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == reg);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS);
	SPDK_CU_ASSERT_FATAL(g_subsystem.gen > gen);
	SPDK_CU_ASSERT_FATAL(g_subsystem.regctl == 1);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static void
test_reservation_release(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;
	struct spdk_nvmf_registrant *reg;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	rsp = &req->rsp->nvme_cpl;
	SPDK_CU_ASSERT_FATAL(req != NULL);

	ut_reservation_build_registrants();

	/* ACQUIRE: Host A with g_ctrlr1_A get reservation with
	 * type SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS
	 */
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS, 0xa1, 0x0);
	nvmf_ns_reservation_acquire(req, &g_ctrlr1_A, &g_ns);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr1_A);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == reg);

	/* Test Case: Host B release the reservation */
	ut_reservation_build_release_request(req, SPDK_NVME_RESERVE_RELEASE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS, 0xb1);
	nvmf_ns_reservation_release(req, &g_ctrlr_B, &g_ns);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == 0);
	SPDK_CU_ASSERT_FATAL(g_ns.crkey == 0);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == NULL);

	/* Test Case: Host C clear the registrants */
	ut_reservation_build_release_request(req, SPDK_NVME_RESERVE_CLEAR, 0,
					     0, 0xc1);
	nvmf_ns_reservation_release(req, &g_ctrlr_C, &g_ns);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr1_A);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr2_A);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr_B);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr_C);
	SPDK_CU_ASSERT_FATAL(reg == NULL);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static void
test_reservation_write_exclusive(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cmd *cmd;
	struct spdk_nvme_cpl *rsp;
	struct spdk_nvmf_registrant *reg;
	int rc;

	ut_reservation_init();

	req = ut_reservation_build_req(4096);
	cmd = &req->cmd->nvme_cmd;
	rsp = &req->rsp->nvme_cpl;
	SPDK_CU_ASSERT_FATAL(req != NULL);

	ut_reservation_build_registrants();

	/* ACQUIRE: Host A with g_ctrlr1_A get reservation with
	 * type SPDK_NVME_RESERVE_WRITE_EXCLUSIVE
	 */
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE, 0xa1, 0x0);
	nvmf_ns_reservation_acquire(req, &g_ctrlr1_A, &g_ns);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr1_A);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == reg);

	/* Test Case: Issue a Read command from Host A and Host B */
	cmd->opc = SPDK_NVME_OPC_READ;
	rc = spdk_nvmf_reservation_filter(req, &g_ctrlr1_A, &g_ns);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	rc = spdk_nvmf_reservation_filter(req, &g_ctrlr_B, &g_ns);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Test Case: Issue a DSM Write command from Host A and Host B */
	cmd->opc = SPDK_NVME_OPC_DATASET_MANAGEMENT;
	rc = spdk_nvmf_reservation_filter(req, &g_ctrlr1_A, &g_ns);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	rc = spdk_nvmf_reservation_filter(req, &g_ctrlr_B, &g_ns);
	SPDK_CU_ASSERT_FATAL(rc < 0);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_RESERVATION_CONFLICT);

	/* Unregister Host C */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_UNREGISTER_KEY,
					      1, 0, 0, 0);
	nvmf_ns_reservation_register(req, &g_ctrlr_C, &g_ns);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr_C);
	SPDK_CU_ASSERT_FATAL(reg == NULL);

	/* Test Case: Read and Write commands from non-registrant Host C */
	cmd->opc = SPDK_NVME_OPC_READ;
	rc = spdk_nvmf_reservation_filter(req, &g_ctrlr_C, &g_ns);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	cmd->opc = SPDK_NVME_OPC_WRITE;
	rc = spdk_nvmf_reservation_filter(req, &g_ctrlr_C, &g_ns);
	SPDK_CU_ASSERT_FATAL(rc < 0);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_RESERVATION_CONFLICT);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static void
test_reservation_exclusive_access(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cmd *cmd;
	struct spdk_nvme_cpl *rsp;
	struct spdk_nvmf_registrant *reg;
	int rc;

	ut_reservation_init();

	req = ut_reservation_build_req(4096);
	cmd = &req->cmd->nvme_cmd;
	rsp = &req->rsp->nvme_cpl;
	SPDK_CU_ASSERT_FATAL(req != NULL);

	ut_reservation_build_registrants();

	/* ACQUIRE: Host A with g_ctrlr1_A get reservation with
	 * type SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS
	 */
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS, 0xa1, 0x0);
	nvmf_ns_reservation_acquire(req, &g_ctrlr1_A, &g_ns);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ctrlr_get_registrant(&g_subsystem, &g_ctrlr1_A);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == reg);

	/* Test Case: Issue a Read command from Host B */
	cmd->opc = SPDK_NVME_OPC_READ;
	rc = spdk_nvmf_reservation_filter(req, &g_ctrlr_B, &g_ns);
	SPDK_CU_ASSERT_FATAL(rc < 0);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_RESERVATION_CONFLICT);

	/* Test Case: Issue a Reservation Release command from a valid Registrant */
	cmd->opc = SPDK_NVME_OPC_RESERVATION_RELEASE;
	rc = spdk_nvmf_reservation_filter(req, &g_ctrlr_B, &g_ns);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static void
test_get_rw_params(void)
{
	struct spdk_nvme_cmd cmd = {0};
	uint64_t lba;
	uint64_t count;

	lba = 0;
	count = 0;
	to_le64(&cmd.cdw10, 0x1234567890ABCDEF);
	to_le32(&cmd.cdw12, 0x9875 | SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS);
	nvmf_bdev_ctrlr_get_rw_params(&cmd, &lba, &count);
	CU_ASSERT(lba == 0x1234567890ABCDEF);
	CU_ASSERT(count == 0x9875 + 1); /* NOTE: this field is 0's based, hence the +1 */
}

static void
test_lba_in_range(void)
{
	/* Trivial cases (no overflow) */
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(1000, 0, 1) == true);
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(1000, 0, 1000) == true);
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(1000, 0, 1001) == false);
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(1000, 1, 999) == true);
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(1000, 1, 1000) == false);
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(1000, 999, 1) == true);
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(1000, 1000, 1) == false);
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(1000, 1001, 1) == false);

	/* Overflow edge cases */
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(UINT64_MAX, 0, UINT64_MAX) == true);
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(UINT64_MAX, 1, UINT64_MAX) == false)
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(UINT64_MAX, UINT64_MAX - 1, 1) == true);
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(UINT64_MAX, UINT64_MAX, 1) == false);
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
		CU_add_test(suite, "get_rw_params", test_get_rw_params) == NULL ||
		CU_add_test(suite, "lba_in_range", test_lba_in_range) == NULL ||
		CU_add_test(suite, "reservation_register", test_reservation_register) == NULL ||
		CU_add_test(suite, "reservation_acquire_preempt_1", test_reservation_acquire_preempt_1) == NULL ||
		CU_add_test(suite, "reservation_release", test_reservation_release) == NULL ||
		CU_add_test(suite, "reservation_write_exclusive", test_reservation_write_exclusive) == NULL ||
		CU_add_test(suite, "reservation_exclusive_access", test_reservation_exclusive_access) == NULL
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
