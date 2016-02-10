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

#include "nvme/nvme_ns_cmd.c"

char outbuf[OUTBUF_SIZE];

struct nvme_request *g_request = NULL;

uint64_t nvme_vtophys(void *buf)
{
	return (uintptr_t)buf;
}

int
nvme_ctrlr_construct(struct spdk_nvme_ctrlr *ctrlr, void *devhandle)
{
	return 0;
}

void
nvme_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
}

int
nvme_ctrlr_start(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

uint32_t
spdk_nvme_ns_get_sector_size(struct spdk_nvme_ns *ns)
{
	return ns->sector_size;
}

uint32_t
spdk_nvme_ns_get_max_io_xfer_size(struct spdk_nvme_ns *ns)
{
	return ns->ctrlr->max_xfer_size;
}

void
nvme_ctrlr_submit_io_request(struct spdk_nvme_ctrlr *ctrlr,
			     struct nvme_request *req)
{
	g_request = req;
}

static void
prepare_for_test(struct spdk_nvme_ns *ns, struct spdk_nvme_ctrlr *ctrlr,
		 uint32_t sector_size, uint32_t max_xfer_size,
		 uint32_t stripe_size)
{
	ctrlr->max_xfer_size = max_xfer_size;
	memset(ns, 0, sizeof(*ns));
	ns->ctrlr = ctrlr;
	ns->sector_size = sector_size;
	ns->stripe_size = stripe_size;
	ns->sectors_per_max_io = spdk_nvme_ns_get_max_io_xfer_size(ns) / ns->sector_size;
	ns->sectors_per_stripe = ns->stripe_size / ns->sector_size;

	g_request = NULL;
}

static void
nvme_cmd_interpret_rw(const struct spdk_nvme_cmd *cmd,
		      uint64_t *lba, uint32_t *num_blocks)
{
	*lba = *(const uint64_t *)&cmd->cdw10;
	*num_blocks = (cmd->cdw12 & 0xFFFFu) + 1;
}

static void
split_test(void)
{
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	void			*payload;
	uint64_t		lba, cmd_lba;
	uint32_t		lba_count, cmd_lba_count;
	int			rc;

	prepare_for_test(&ns, &ctrlr, 512, 128 * 1024, 0);
	payload = malloc(512);
	lba = 0;
	lba_count = 1;

	rc = spdk_nvme_ns_cmd_read(&ns, payload, lba, lba_count, NULL, NULL, 0);

	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);

	CU_ASSERT(g_request->num_children == 0);
	nvme_cmd_interpret_rw(&g_request->cmd, &cmd_lba, &cmd_lba_count);
	CU_ASSERT(cmd_lba == lba);
	CU_ASSERT(cmd_lba_count == lba_count);

	free(payload);
	nvme_free_request(g_request);
}

static void
split_test2(void)
{
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct nvme_request	*child;
	void			*payload;
	uint64_t		lba, cmd_lba;
	uint32_t		lba_count, cmd_lba_count;
	int			rc;

	/*
	 * Controller has max xfer of 128 KB (256 blocks).
	 * Submit an I/O of 256 KB starting at LBA 0, which should be split
	 * on the max I/O boundary into two I/Os of 128 KB.
	 */

	prepare_for_test(&ns, &ctrlr, 512, 128 * 1024, 0);
	payload = malloc(256 * 1024);
	lba = 0;
	lba_count = (256 * 1024) / 512;

	rc = spdk_nvme_ns_cmd_read(&ns, payload, lba, lba_count, NULL, NULL, 0);

	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);

	CU_ASSERT(g_request->num_children == 2);

	child = TAILQ_FIRST(&g_request->children);
	TAILQ_REMOVE(&g_request->children, child, child_tailq);
	nvme_cmd_interpret_rw(&child->cmd, &cmd_lba, &cmd_lba_count);
	CU_ASSERT(child->num_children == 0);
	CU_ASSERT(child->payload_size == 128 * 1024);
	CU_ASSERT(cmd_lba == 0);
	CU_ASSERT(cmd_lba_count == 256); /* 256 * 512 byte blocks = 128 KB */
	nvme_free_request(child);

	child = TAILQ_FIRST(&g_request->children);
	TAILQ_REMOVE(&g_request->children, child, child_tailq);
	nvme_cmd_interpret_rw(&child->cmd, &cmd_lba, &cmd_lba_count);
	CU_ASSERT(child->num_children == 0);
	CU_ASSERT(child->payload_size == 128 * 1024);
	CU_ASSERT(cmd_lba == 256);
	CU_ASSERT(cmd_lba_count == 256);
	nvme_free_request(child);

	CU_ASSERT(TAILQ_EMPTY(&g_request->children));

	free(payload);
	nvme_free_request(g_request);
}

static void
split_test3(void)
{
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct nvme_request	*child;
	void			*payload;
	uint64_t		lba, cmd_lba;
	uint32_t		lba_count, cmd_lba_count;
	int			rc;

	/*
	 * Controller has max xfer of 128 KB (256 blocks).
	 * Submit an I/O of 256 KB starting at LBA 10, which should be split
	 * into two I/Os:
	 *  1) LBA = 10, count = 256 blocks
	 *  2) LBA = 266, count = 256 blocks
	 */

	prepare_for_test(&ns, &ctrlr, 512, 128 * 1024, 0);
	payload = malloc(256 * 1024);
	lba = 10; /* Start at an LBA that isn't aligned to the stripe size */
	lba_count = (256 * 1024) / 512;

	rc = spdk_nvme_ns_cmd_read(&ns, payload, lba, lba_count, NULL, NULL, 0);

	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);

	SPDK_CU_ASSERT_FATAL(g_request->num_children == 2);

	child = TAILQ_FIRST(&g_request->children);
	TAILQ_REMOVE(&g_request->children, child, child_tailq);
	nvme_cmd_interpret_rw(&child->cmd, &cmd_lba, &cmd_lba_count);
	CU_ASSERT(child->num_children == 0);
	CU_ASSERT(child->payload_size == 128 * 1024);
	CU_ASSERT(cmd_lba == 10);
	CU_ASSERT(cmd_lba_count == 256);
	nvme_free_request(child);

	child = TAILQ_FIRST(&g_request->children);
	TAILQ_REMOVE(&g_request->children, child, child_tailq);
	nvme_cmd_interpret_rw(&child->cmd, &cmd_lba, &cmd_lba_count);
	CU_ASSERT(child->num_children == 0);
	CU_ASSERT(child->payload_size == 128 * 1024);
	CU_ASSERT(cmd_lba == 266);
	CU_ASSERT(cmd_lba_count == 256);
	nvme_free_request(child);

	CU_ASSERT(TAILQ_EMPTY(&g_request->children));

	free(payload);
	nvme_free_request(g_request);
}

static void
split_test4(void)
{
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct nvme_request	*child;
	void			*payload;
	uint64_t		lba, cmd_lba;
	uint32_t		lba_count, cmd_lba_count;
	int			rc;

	/*
	 * Controller has max xfer of 128 KB (256 blocks) and a stripe size of 128 KB.
	 * (Same as split_test3 except with driver-assisted striping enabled.)
	 * Submit an I/O of 256 KB starting at LBA 10, which should be split
	 * into three I/Os:
	 *  1) LBA = 10, count = 246 blocks (less than max I/O size to align to stripe size)
	 *  2) LBA = 256, count = 256 blocks (aligned to stripe size and max I/O size)
	 *  3) LBA = 512, count = 10 blocks (finish off the remaining I/O size)
	 */

	prepare_for_test(&ns, &ctrlr, 512, 128 * 1024, 128 * 1024);
	payload = malloc(256 * 1024);
	lba = 10; /* Start at an LBA that isn't aligned to the stripe size */
	lba_count = (256 * 1024) / 512;

	rc = spdk_nvme_ns_cmd_read(&ns, payload, lba, lba_count, NULL, NULL,
				   SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS);

	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);

	SPDK_CU_ASSERT_FATAL(g_request->num_children == 3);

	child = TAILQ_FIRST(&g_request->children);
	TAILQ_REMOVE(&g_request->children, child, child_tailq);
	nvme_cmd_interpret_rw(&child->cmd, &cmd_lba, &cmd_lba_count);
	CU_ASSERT(child->num_children == 0);
	CU_ASSERT(child->payload_size == (256 - 10) * 512);
	CU_ASSERT(cmd_lba == 10);
	CU_ASSERT(cmd_lba_count == 256 - 10);
	CU_ASSERT((child->cmd.cdw12 & SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS) != 0);
	CU_ASSERT((child->cmd.cdw12 & SPDK_NVME_IO_FLAGS_LIMITED_RETRY) == 0);
	nvme_free_request(child);

	child = TAILQ_FIRST(&g_request->children);
	TAILQ_REMOVE(&g_request->children, child, child_tailq);
	nvme_cmd_interpret_rw(&child->cmd, &cmd_lba, &cmd_lba_count);
	CU_ASSERT(child->num_children == 0);
	CU_ASSERT(child->payload_size == 128 * 1024);
	CU_ASSERT(cmd_lba == 256);
	CU_ASSERT(cmd_lba_count == 256);
	CU_ASSERT((child->cmd.cdw12 & SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS) != 0);
	CU_ASSERT((child->cmd.cdw12 & SPDK_NVME_IO_FLAGS_LIMITED_RETRY) == 0);
	nvme_free_request(child);

	child = TAILQ_FIRST(&g_request->children);
	TAILQ_REMOVE(&g_request->children, child, child_tailq);
	nvme_cmd_interpret_rw(&child->cmd, &cmd_lba, &cmd_lba_count);
	CU_ASSERT(child->num_children == 0);
	CU_ASSERT(child->payload_size == 10 * 512);
	CU_ASSERT(cmd_lba == 512);
	CU_ASSERT(cmd_lba_count == 10);
	CU_ASSERT((child->cmd.cdw12 & SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS) != 0);
	CU_ASSERT((child->cmd.cdw12 & SPDK_NVME_IO_FLAGS_LIMITED_RETRY) == 0);
	nvme_free_request(child);

	CU_ASSERT(TAILQ_EMPTY(&g_request->children));

	free(payload);
	nvme_free_request(g_request);
}

static void
test_nvme_ns_cmd_flush(void)
{
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	spdk_nvme_cmd_cb	cb_fn = NULL;
	void			*cb_arg = NULL;

	prepare_for_test(&ns, &ctrlr, 512, 128 * 1024, 0);

	spdk_nvme_ns_cmd_flush(&ns, cb_fn, cb_arg);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_FLUSH);
	CU_ASSERT(g_request->cmd.nsid == ns.id);

	nvme_free_request(g_request);
}

static void
test_nvme_ns_cmd_write_zeroes(void)
{
	struct spdk_nvme_ns	ns = { 0 };
	struct spdk_nvme_ctrlr	ctrlr = { 0 };
	spdk_nvme_cmd_cb	cb_fn = NULL;
	void			*cb_arg = NULL;
	uint64_t		cmd_lba;
	uint32_t		cmd_lba_count;

	prepare_for_test(&ns, &ctrlr, 512, 128 * 1024, 0);

	spdk_nvme_ns_cmd_write_zeroes(&ns, 0, 2, cb_fn, cb_arg, 0);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_WRITE_ZEROES);
	CU_ASSERT(g_request->cmd.nsid == ns.id);
	nvme_cmd_interpret_rw(&g_request->cmd, &cmd_lba, &cmd_lba_count);
	CU_ASSERT_EQUAL(cmd_lba, 0);
	CU_ASSERT_EQUAL(cmd_lba_count, 2);

	nvme_free_request(g_request);
}

static void
test_nvme_ns_cmd_deallocate(void)
{
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	spdk_nvme_cmd_cb	cb_fn = NULL;
	void			*cb_arg = NULL;
	uint16_t		num_ranges = 1;
	void			*payload = NULL;
	int			rc = 0;

	prepare_for_test(&ns, &ctrlr, 512, 128 * 1024, 0);
	payload = malloc(num_ranges * sizeof(struct spdk_nvme_dsm_range));

	spdk_nvme_ns_cmd_deallocate(&ns, payload, num_ranges, cb_fn, cb_arg);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_DATASET_MANAGEMENT);
	CU_ASSERT(g_request->cmd.nsid == ns.id);
	CU_ASSERT(g_request->cmd.cdw10 == num_ranges - 1u);
	CU_ASSERT(g_request->cmd.cdw11 == SPDK_NVME_DSM_ATTR_DEALLOCATE);
	free(payload);
	nvme_free_request(g_request);

	num_ranges = 256;
	payload = malloc(num_ranges * sizeof(struct spdk_nvme_dsm_range));
	spdk_nvme_ns_cmd_deallocate(&ns, payload, num_ranges, cb_fn, cb_arg);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_DATASET_MANAGEMENT);
	CU_ASSERT(g_request->cmd.nsid == ns.id);
	CU_ASSERT(g_request->cmd.cdw10 == num_ranges - 1u);
	CU_ASSERT(g_request->cmd.cdw11 == SPDK_NVME_DSM_ATTR_DEALLOCATE);
	free(payload);
	nvme_free_request(g_request);

	payload = NULL;
	num_ranges = 0;
	rc = spdk_nvme_ns_cmd_deallocate(&ns, payload, num_ranges, cb_fn, cb_arg);
	CU_ASSERT(rc != 0);
}

static void
test_io_flags(void)
{
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	void			*payload;
	uint64_t		lba;
	uint32_t		lba_count;
	int			rc;

	prepare_for_test(&ns, &ctrlr, 512, 128 * 1024, 128 * 1024);
	payload = malloc(256 * 1024);
	lba = 0;
	lba_count = (4 * 1024) / 512;

	rc = spdk_nvme_ns_cmd_read(&ns, payload, lba, lba_count, NULL, NULL,
				   SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS);
	CU_ASSERT(rc == 0);
	CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT((g_request->cmd.cdw12 & SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS) != 0);
	CU_ASSERT((g_request->cmd.cdw12 & SPDK_NVME_IO_FLAGS_LIMITED_RETRY) == 0);
	nvme_free_request(g_request);

	rc = spdk_nvme_ns_cmd_read(&ns, payload, lba, lba_count, NULL, NULL,
				   SPDK_NVME_IO_FLAGS_LIMITED_RETRY);
	CU_ASSERT(rc == 0);
	CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT((g_request->cmd.cdw12 & SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS) == 0);
	CU_ASSERT((g_request->cmd.cdw12 & SPDK_NVME_IO_FLAGS_LIMITED_RETRY) != 0);
	nvme_free_request(g_request);

	free(payload);

}

static void
test_nvme_ns_cmd_reservation_register(void)
{
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_reservation_register_data *payload;
	bool			ignore_key = 1;
	spdk_nvme_cmd_cb	cb_fn = NULL;
	void			*cb_arg = NULL;
	int			rc = 0;
	uint32_t		tmp_cdw10;

	prepare_for_test(&ns, &ctrlr, 512, 128 * 1024, 0);
	payload = malloc(sizeof(struct spdk_nvme_reservation_register_data));

	rc = spdk_nvme_ns_cmd_reservation_register(&ns, payload, ignore_key,
			SPDK_NVME_RESERVE_REGISTER_KEY,
			SPDK_NVME_RESERVE_PTPL_NO_CHANGES,
			cb_fn, cb_arg);

	CU_ASSERT(rc == 0);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_RESERVATION_REGISTER);
	CU_ASSERT(g_request->cmd.nsid == ns.id);

	tmp_cdw10 = SPDK_NVME_RESERVE_REGISTER_KEY;
	tmp_cdw10 |= ignore_key ? 1 << 3 : 0;
	tmp_cdw10 |= (uint32_t)SPDK_NVME_RESERVE_PTPL_NO_CHANGES << 30;

	CU_ASSERT(g_request->cmd.cdw10 == tmp_cdw10);

	nvme_free_request(g_request);
	free(payload);
}

static void
test_nvme_ns_cmd_reservation_release(void)
{
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_reservation_key_data *payload;
	bool			ignore_key = 1;
	spdk_nvme_cmd_cb	cb_fn = NULL;
	void			*cb_arg = NULL;
	int			rc = 0;
	uint32_t		tmp_cdw10;

	prepare_for_test(&ns, &ctrlr, 512, 128 * 1024, 0);
	payload = malloc(sizeof(struct spdk_nvme_reservation_key_data));

	rc = spdk_nvme_ns_cmd_reservation_release(&ns, payload, ignore_key,
			SPDK_NVME_RESERVE_RELEASE,
			SPDK_NVME_RESERVE_WRITE_EXCLUSIVE,
			cb_fn, cb_arg);

	CU_ASSERT(rc == 0);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_RESERVATION_RELEASE);
	CU_ASSERT(g_request->cmd.nsid == ns.id);

	tmp_cdw10 = SPDK_NVME_RESERVE_RELEASE;
	tmp_cdw10 |= ignore_key ? 1 << 3 : 0;
	tmp_cdw10 |= (uint32_t)SPDK_NVME_RESERVE_WRITE_EXCLUSIVE << 8;

	CU_ASSERT(g_request->cmd.cdw10 == tmp_cdw10);

	nvme_free_request(g_request);
	free(payload);
}

static void
test_nvme_ns_cmd_reservation_acquire(void)
{
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_reservation_acquire_data *payload;
	bool			ignore_key = 1;
	spdk_nvme_cmd_cb	cb_fn = NULL;
	void			*cb_arg = NULL;
	int			rc = 0;
	uint32_t		tmp_cdw10;

	prepare_for_test(&ns, &ctrlr, 512, 128 * 1024, 0);
	payload = malloc(sizeof(struct spdk_nvme_reservation_acquire_data));

	rc = spdk_nvme_ns_cmd_reservation_acquire(&ns, payload, ignore_key,
			SPDK_NVME_RESERVE_ACQUIRE,
			SPDK_NVME_RESERVE_WRITE_EXCLUSIVE,
			cb_fn, cb_arg);

	CU_ASSERT(rc == 0);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_RESERVATION_ACQUIRE);
	CU_ASSERT(g_request->cmd.nsid == ns.id);

	tmp_cdw10 = SPDK_NVME_RESERVE_ACQUIRE;
	tmp_cdw10 |= ignore_key ? 1 << 3 : 0;
	tmp_cdw10 |= (uint32_t)SPDK_NVME_RESERVE_WRITE_EXCLUSIVE << 8;

	CU_ASSERT(g_request->cmd.cdw10 == tmp_cdw10);

	nvme_free_request(g_request);
	free(payload);
}

static void
test_nvme_ns_cmd_reservation_report(void)
{
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_reservation_status_data *payload;
	spdk_nvme_cmd_cb	cb_fn = NULL;
	void			*cb_arg = NULL;
	int			rc = 0;

	prepare_for_test(&ns, &ctrlr, 512, 128 * 1024, 0);
	payload = malloc(sizeof(struct spdk_nvme_reservation_status_data));

	rc = spdk_nvme_ns_cmd_reservation_report(&ns, payload, 0x1000,
			cb_fn, cb_arg);

	CU_ASSERT(rc == 0);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_RESERVATION_REPORT);
	CU_ASSERT(g_request->cmd.nsid == ns.id);

	CU_ASSERT(g_request->cmd.cdw10 == (0x1000 / 4));

	nvme_free_request(g_request);
	free(payload);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvme_ns_cmd", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "split_test", split_test) == NULL
		|| CU_add_test(suite, "split_test2", split_test2) == NULL
		|| CU_add_test(suite, "split_test3", split_test3) == NULL
		|| CU_add_test(suite, "split_test4", split_test4) == NULL
		|| CU_add_test(suite, "nvme_ns_cmd_flush", test_nvme_ns_cmd_flush) == NULL
		|| CU_add_test(suite, "nvme_ns_cmd_deallocate", test_nvme_ns_cmd_deallocate) == NULL
		|| CU_add_test(suite, "io_flags", test_io_flags) == NULL
		|| CU_add_test(suite, "nvme_ns_cmd_write_zeroes", test_nvme_ns_cmd_write_zeroes) == NULL
		|| CU_add_test(suite, "nvme_ns_cmd_reservation_register",
			       test_nvme_ns_cmd_reservation_register) == NULL
		|| CU_add_test(suite, "nvme_ns_cmd_reservation_release",
			       test_nvme_ns_cmd_reservation_release) == NULL
		|| CU_add_test(suite, "nvme_ns_cmd_reservation_acquire",
			       test_nvme_ns_cmd_reservation_acquire) == NULL
		|| CU_add_test(suite, "nvme_ns_cmd_reservation_report", test_nvme_ns_cmd_reservation_report) == NULL
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
