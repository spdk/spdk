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
#include "spdk/thread.h"
#include "spdk/bdev_module.h"
#include "spdk/util.h"

#include "common/lib/ut_multithread.c"

#include "bdev/nvme/bdev_nvme.c"
#include "bdev/nvme/common.c"

#include "unit/lib/json_mock.c"

DEFINE_STUB(spdk_nvme_connect_async, struct spdk_nvme_probe_ctx *,
	    (const struct spdk_nvme_transport_id *trid,
	     const struct spdk_nvme_ctrlr_opts *opts,
	     spdk_nvme_attach_cb attach_cb), NULL);

DEFINE_STUB(spdk_nvme_probe_async, struct spdk_nvme_probe_ctx *,
	    (const struct spdk_nvme_transport_id *trid, void *cb_ctx,
	     spdk_nvme_probe_cb probe_cb, spdk_nvme_attach_cb attach_cb,
	     spdk_nvme_remove_cb remove_cb), NULL);

DEFINE_STUB(spdk_nvme_probe_poll_async, int, (struct spdk_nvme_probe_ctx *probe_ctx), 0);

DEFINE_STUB(spdk_nvme_detach, int, (struct spdk_nvme_ctrlr *ctrlr), 0);

DEFINE_STUB(spdk_nvme_transport_id_compare, int, (const struct spdk_nvme_transport_id *trid1,
		const struct spdk_nvme_transport_id *trid2), -1);

DEFINE_STUB_V(spdk_nvme_trid_populate_transport, (struct spdk_nvme_transport_id *trid,
		enum spdk_nvme_transport_type trtype));

DEFINE_STUB(spdk_nvme_transport_id_trtype_str, const char *, (enum spdk_nvme_transport_type trtype),
	    NULL);

DEFINE_STUB(spdk_nvme_transport_id_adrfam_str, const char *, (enum spdk_nvmf_adrfam adrfam), NULL);

DEFINE_STUB_V(spdk_nvme_ctrlr_get_default_ctrlr_opts, (struct spdk_nvme_ctrlr_opts *opts,
		size_t opts_size));

DEFINE_STUB(spdk_nvme_ctrlr_set_trid, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_transport_id *trid), 0);

DEFINE_STUB_V(spdk_nvme_ctrlr_set_remove_cb, (struct spdk_nvme_ctrlr *ctrlr,
		spdk_nvme_remove_cb remove_cb, void *remove_ctx));

DEFINE_STUB(spdk_nvme_ctrlr_reset, int, (struct spdk_nvme_ctrlr *ctrlr), 0);

DEFINE_STUB_V(spdk_nvme_ctrlr_fail, (struct spdk_nvme_ctrlr *ctrlr));

DEFINE_STUB(spdk_nvme_ctrlr_process_admin_completions, int32_t,
	    (struct spdk_nvme_ctrlr *ctrlr), 0);

DEFINE_STUB(spdk_nvme_ctrlr_get_data, const struct spdk_nvme_ctrlr_data *,
	    (struct spdk_nvme_ctrlr *ctrlr), NULL);

DEFINE_STUB(spdk_nvme_ctrlr_get_flags, uint64_t, (struct spdk_nvme_ctrlr *ctrlr), 0);

DEFINE_STUB(spdk_nvme_ctrlr_connect_io_qpair, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair), 0);

DEFINE_STUB(spdk_nvme_ctrlr_alloc_io_qpair, struct spdk_nvme_qpair *,
	    (struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_io_qpair_opts *user_opts,
	     size_t opts_size), NULL);

DEFINE_STUB(spdk_nvme_ctrlr_reconnect_io_qpair, int, (struct spdk_nvme_qpair *qpair), 0);

DEFINE_STUB(spdk_nvme_ctrlr_free_io_qpair, int, (struct spdk_nvme_qpair *qpair), 0);

DEFINE_STUB_V(spdk_nvme_ctrlr_get_default_io_qpair_opts, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_io_qpair_opts *opts, size_t opts_size));

DEFINE_STUB(spdk_nvme_ctrlr_get_ns, struct spdk_nvme_ns *, (struct spdk_nvme_ctrlr *ctrlr,
		uint32_t nsid), NULL);

DEFINE_STUB(spdk_nvme_ctrlr_is_active_ns, bool, (struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid),
	    false);

DEFINE_STUB(spdk_nvme_ctrlr_get_max_xfer_size, uint32_t,
	    (const struct spdk_nvme_ctrlr *ctrlr), 0);

DEFINE_STUB(spdk_nvme_ctrlr_get_transport_id, const struct spdk_nvme_transport_id *,
	    (struct spdk_nvme_ctrlr *ctrlr), NULL);

DEFINE_STUB_V(spdk_nvme_ctrlr_register_aer_callback, (struct spdk_nvme_ctrlr *ctrlr,
		spdk_nvme_aer_cb aer_cb_fn, void *aer_cb_arg));

DEFINE_STUB_V(spdk_nvme_ctrlr_register_timeout_callback, (struct spdk_nvme_ctrlr *ctrlr,
		uint64_t timeout_us, spdk_nvme_timeout_cb cb_fn, void *cb_arg));

DEFINE_STUB(spdk_nvme_ctrlr_is_ocssd_supported, bool, (struct spdk_nvme_ctrlr *ctrlr), false);

DEFINE_STUB(spdk_nvme_ctrlr_cmd_admin_raw, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_cmd *cmd, void *buf, uint32_t len,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_ctrlr_cmd_abort, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair, uint16_t cid, spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_ctrlr_cmd_abort_ext, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair, void *cmd_cb_arg, spdk_nvme_cmd_cb cb_fn,
		void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_ctrlr_cmd_io_raw, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair, struct spdk_nvme_cmd *cmd, void *buf,
		uint32_t len, spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_ctrlr_cmd_io_raw_with_md, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair, struct spdk_nvme_cmd *cmd, void *buf,
		uint32_t len, void *md_buf, spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_ns_get_id, uint32_t, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_get_ctrlr, struct spdk_nvme_ctrlr *, (struct spdk_nvme_ns *ns), NULL);

DEFINE_STUB(spdk_nvme_ns_get_max_io_xfer_size, uint32_t, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_get_extended_sector_size, uint32_t, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_get_num_sectors, uint64_t, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_get_pi_type, enum spdk_nvme_pi_type, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_supports_compare, bool, (struct spdk_nvme_ns *ns), false);

DEFINE_STUB(spdk_nvme_ns_get_md_size, uint32_t, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_get_data, const struct spdk_nvme_ns_data *,
	    (struct spdk_nvme_ns *ns), NULL);

DEFINE_STUB(spdk_nvme_ns_get_dealloc_logical_block_read_value,
	    enum spdk_nvme_dealloc_logical_block_read_value, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_get_optimal_io_boundary, uint32_t, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_get_uuid, const struct spdk_uuid *,
	    (const struct spdk_nvme_ns *ns), NULL);

DEFINE_STUB(spdk_nvme_poll_group_create, struct spdk_nvme_poll_group *,
	    (void *ctx), (void *)0x1);

DEFINE_STUB(spdk_nvme_ns_cmd_read_with_md, int, (struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair, void *buffer, void *metadata,
		uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag), 0);

DEFINE_STUB(spdk_nvme_ns_cmd_write_with_md, int, (struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair, void *buffer, void *metadata,
		uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag), 0);

DEFINE_STUB(spdk_nvme_ns_cmd_readv_with_md, int, (struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair, uint64_t lba, uint32_t lba_count,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
		spdk_nvme_req_reset_sgl_cb reset_sgl_fn, spdk_nvme_req_next_sge_cb next_sge_fn,
		void *metadata, uint16_t apptag_mask, uint16_t apptag), 0);

DEFINE_STUB(spdk_nvme_ns_cmd_writev_with_md, int, (struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair, uint64_t lba, uint32_t lba_count,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
		spdk_nvme_req_reset_sgl_cb reset_sgl_fn, spdk_nvme_req_next_sge_cb next_sge_fn,
		void *metadata, uint16_t apptag_mask, uint16_t apptag), 0);

DEFINE_STUB(spdk_nvme_ns_cmd_comparev_with_md, int, (struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair, uint64_t lba, uint32_t lba_count,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
		spdk_nvme_req_reset_sgl_cb reset_sgl_fn, spdk_nvme_req_next_sge_cb next_sge_fn,
		void *metadata, uint16_t apptag_mask, uint16_t apptag), 0);

DEFINE_STUB(spdk_nvme_ns_cmd_dataset_management, int, (struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair, uint32_t type,
		const struct spdk_nvme_dsm_range *ranges, uint16_t num_ranges,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_cuse_get_ns_name, int, (struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
		char *name, size_t *size), 0);

DEFINE_STUB(spdk_nvme_poll_group_add, int, (struct spdk_nvme_poll_group *group,
		struct spdk_nvme_qpair *qpair), 0);

DEFINE_STUB(spdk_nvme_poll_group_remove, int, (struct spdk_nvme_poll_group *group,
		struct spdk_nvme_qpair *qpair), 0);

DEFINE_STUB(spdk_nvme_poll_group_process_completions, int64_t,
	    (struct spdk_nvme_poll_group *group, uint32_t completions_per_qpair,
	     spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb), 0);

DEFINE_STUB(spdk_nvme_poll_group_destroy, int, (struct spdk_nvme_poll_group *group), 0);

DEFINE_STUB(spdk_bdev_register, int, (struct spdk_bdev *bdev), 0);

DEFINE_STUB_V(spdk_bdev_unregister, (struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn,
				     void *cb_arg));

DEFINE_STUB_V(spdk_bdev_module_finish_done, (void));

DEFINE_STUB_V(spdk_bdev_io_get_buf, (struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb,
				     uint64_t len));

DEFINE_STUB_V(spdk_bdev_io_complete, (struct spdk_bdev_io *bdev_io,
				      enum spdk_bdev_io_status status));

DEFINE_STUB_V(spdk_bdev_io_complete_nvme_status, (struct spdk_bdev_io *bdev_io,
		uint32_t cdw0, int sct, int sc));

DEFINE_STUB(spdk_bdev_io_get_io_channel, struct spdk_io_channel *,
	    (struct spdk_bdev_io *bdev_io), (void *)0x1);

DEFINE_STUB(spdk_bdev_notify_blockcnt_change, int, (struct spdk_bdev *bdev, uint64_t size), 0);

DEFINE_STUB_V(spdk_bdev_module_list_add, (struct spdk_bdev_module *bdev_module));

DEFINE_STUB(spdk_opal_dev_construct, struct spdk_opal_dev *, (struct spdk_nvme_ctrlr *ctrlr), NULL);

DEFINE_STUB_V(spdk_opal_dev_destruct, (struct spdk_opal_dev *dev));

DEFINE_STUB_V(bdev_ocssd_populate_namespace, (struct nvme_bdev_ctrlr *nvme_bdev_ctrlr,
		struct nvme_bdev_ns *nvme_ns, struct nvme_async_probe_ctx *ctx));

DEFINE_STUB_V(bdev_ocssd_depopulate_namespace, (struct nvme_bdev_ns *nvme_ns));

DEFINE_STUB_V(bdev_ocssd_namespace_config_json, (struct spdk_json_write_ctx *w,
		struct nvme_bdev_ns *nvme_ns));

DEFINE_STUB(bdev_ocssd_create_io_channel, int, (struct nvme_io_channel *ioch), 0);

DEFINE_STUB_V(bdev_ocssd_destroy_io_channel, (struct nvme_io_channel *ioch));

DEFINE_STUB(bdev_ocssd_init_ctrlr, int, (struct nvme_bdev_ctrlr *nvme_bdev_ctrlr), 0);

DEFINE_STUB_V(bdev_ocssd_fini_ctrlr, (struct nvme_bdev_ctrlr *nvme_bdev_ctrlr));

DEFINE_STUB_V(bdev_ocssd_handle_chunk_notification, (struct nvme_bdev_ctrlr *nvme_bdev_ctrlr));

struct spdk_nvme_ctrlr {
	uint32_t			num_ns;
};

static void
ut_init_trid(struct spdk_nvme_transport_id *trid)
{
	trid->trtype = SPDK_NVME_TRANSPORT_TCP;
	snprintf(trid->subnqn, SPDK_NVMF_NQN_MAX_LEN, "%s", "nqn.2016-06.io.spdk:cnode1");
	snprintf(trid->traddr, SPDK_NVMF_TRADDR_MAX_LEN, "%s", "192.168.100.8");
	snprintf(trid->trsvcid, SPDK_NVMF_TRSVCID_MAX_LEN, "%s", "4420");
}

uint32_t
spdk_nvme_ctrlr_get_num_ns(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->num_ns;
}

union spdk_nvme_csts_register
	spdk_nvme_ctrlr_get_regs_csts(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_csts_register csts;

	csts.raw = 0;

	return csts;
}

union spdk_nvme_vs_register
	spdk_nvme_ctrlr_get_regs_vs(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_vs_register vs;

	vs.raw = 0;

	return vs;
}

static void
test_create_ctrlr(void)
{
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	int rc;

	ut_init_trid(&trid);

	rc = nvme_bdev_ctrlr_create(&ctrlr, "nvme0", &trid, 0);
	CU_ASSERT(rc == 0);

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") != NULL);

	rc = bdev_nvme_delete("nvme0");
	CU_ASSERT(rc == 0);

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") != NULL);

	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);
}

int
main(int argc, const char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme", NULL, NULL);

	CU_ADD_TEST(suite, test_create_ctrlr);

	CU_basic_set_mode(CU_BRM_VERBOSE);

	allocate_threads(3);
	set_thread(0);
	bdev_nvme_library_init();

	CU_basic_run_tests();

	set_thread(0);
	bdev_nvme_library_fini();
	free_threads();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
