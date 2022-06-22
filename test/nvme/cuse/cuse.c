
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
#include "nvme/nvme_cuse.c"

DEFINE_STUB(nvme_io_msg_send, int, (struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				    spdk_nvme_io_msg_fn fn, void *arg), 0);

DEFINE_STUB(spdk_nvme_ctrlr_alloc_cmb_io_buffer, void *, (struct spdk_nvme_ctrlr *ctrlr,
		size_t size), NULL);

DEFINE_STUB(spdk_nvme_ctrlr_cmd_admin_raw, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_cmd *cmd, void *buf, uint32_t len,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_ctrlr_cmd_io_raw_with_md, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair, struct spdk_nvme_cmd *cmd, void *buf, uint32_t len, void *md_buf,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_ctrlr_get_num_ns, uint32_t, (struct spdk_nvme_ctrlr *ctrlr), 128);

static uint32_t g_active_num_ns = 4;
static uint32_t g_active_nsid_min = 1;

bool
spdk_nvme_ctrlr_is_active_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	return nsid >= g_active_nsid_min && nsid < g_active_num_ns + g_active_nsid_min;
}

uint32_t
spdk_nvme_ctrlr_get_first_active_ns(struct spdk_nvme_ctrlr *ctrlr)
{
	return g_active_nsid_min;
}

uint32_t
spdk_nvme_ctrlr_get_next_active_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	nsid = nsid + 1;

	if (spdk_nvme_ctrlr_is_active_ns(ctrlr, nsid)) {
		return nsid;
	}

	return 0;
}

DEFINE_STUB(spdk_nvme_ctrlr_reset, int, (struct spdk_nvme_ctrlr *ctrlr), 0);

DEFINE_STUB(spdk_nvme_ctrlr_reset_subsystem, int, (struct spdk_nvme_ctrlr *ctrlr), 0);

DEFINE_STUB(spdk_nvme_ns_cmd_read_with_md, int, (struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		void *payload, void *metadata,
		uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag), 0);

DEFINE_STUB(spdk_nvme_ns_cmd_write_with_md, int, (struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		void *payload, void *metadata,
		uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag), 0);

DEFINE_STUB(spdk_nvme_ns_get_num_sectors, uint64_t, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_get_sector_size, uint32_t, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_get_md_size, uint32_t, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB_V(spdk_unaffinitize_thread, (void));

DEFINE_STUB(spdk_nvme_ctrlr_get_ns, struct spdk_nvme_ns *, (struct spdk_nvme_ctrlr *ctrlr,
		uint32_t nsid), NULL);

DEFINE_STUB(nvme_io_msg_ctrlr_register, int,
	    (struct spdk_nvme_ctrlr *ctrlr, struct nvme_io_msg_producer *io_msg_producer), 0);

DEFINE_STUB_V(nvme_io_msg_ctrlr_unregister,
	      (struct spdk_nvme_ctrlr *ctrlr, struct nvme_io_msg_producer *io_msg_producer));

DEFINE_STUB_V(nvme_ctrlr_update_namespaces, (struct spdk_nvme_ctrlr *ctrlr));

static bool
wait_for_file(char *filename, bool exists)
{
	int i;

	for (i = 0; i < 1000; i++) {
		if ((access(filename, F_OK) != -1) ^ (!exists)) {
			return true;
		}
		usleep(100);
	}
	return false;
}

static void
verify_devices(struct spdk_nvme_ctrlr *ctrlr)
{
	char ctrlr_name[256];
	size_t ctrlr_name_size;
	char ctrlr_dev[256];
	char ns_dev[256 + 1 + 10]; /* sizeof ctrl_dev + 'n' + string size of UINT32_MAX */
	uint32_t nsid, num_ns;
	int rv;

	ctrlr_name_size = sizeof(ctrlr_name);
	rv = spdk_nvme_cuse_get_ctrlr_name(ctrlr, ctrlr_name, &ctrlr_name_size);
	SPDK_CU_ASSERT_FATAL(rv == 0);

	rv = snprintf(ctrlr_dev, sizeof(ctrlr_dev), "/dev/%s", ctrlr_name);
	CU_ASSERT(rv > 0);
	CU_ASSERT(wait_for_file(ctrlr_dev, true));

	num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);

	for (nsid = 1; nsid <= num_ns; nsid++) {
		snprintf(ns_dev, sizeof(ns_dev), "%sn%" PRIu32, ctrlr_dev, nsid);
		if (spdk_nvme_ctrlr_is_active_ns(ctrlr, nsid)) {
			CU_ASSERT(wait_for_file(ns_dev, true));
		} else {
			CU_ASSERT(wait_for_file(ns_dev, false));
		}
	}

	/* Next one should never exist */
	snprintf(ns_dev, sizeof(ns_dev), "%sn%" PRIu32, ctrlr_dev, nsid);
	CU_ASSERT(wait_for_file(ns_dev, false));
}

static void
test_cuse_update(void)
{
	int rc;
	struct spdk_nvme_ctrlr	ctrlr = {};

	rc = nvme_cuse_start(&ctrlr);
	CU_ASSERT(rc == 0);

	g_active_num_ns = 4;
	g_active_nsid_min = 1;
	nvme_cuse_update(&ctrlr);
	verify_devices(&ctrlr);

	g_active_num_ns = 0;
	nvme_cuse_update(&ctrlr);
	verify_devices(&ctrlr);

	g_active_num_ns = 4;
	g_active_nsid_min = spdk_nvme_ctrlr_get_num_ns(&ctrlr) - g_active_num_ns;
	nvme_cuse_update(&ctrlr);
	verify_devices(&ctrlr);

	g_active_num_ns = 2;
	g_active_nsid_min = 2;
	nvme_cuse_update(&ctrlr);
	verify_devices(&ctrlr);

	g_active_num_ns = 10;
	g_active_nsid_min = 5;
	nvme_cuse_update(&ctrlr);
	verify_devices(&ctrlr);

	g_active_num_ns = 5;
	g_active_nsid_min = 3;
	nvme_cuse_update(&ctrlr);
	verify_devices(&ctrlr);

	g_active_num_ns = 6;
	g_active_nsid_min = 1;
	nvme_cuse_update(&ctrlr);
	verify_devices(&ctrlr);

	nvme_cuse_stop(&ctrlr);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();
	suite = CU_add_suite("nvme_cuse", NULL, NULL);
	CU_ADD_TEST(suite, test_cuse_update);
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
