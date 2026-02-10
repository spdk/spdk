/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk_internal/cunit.h"

#include "nvme/nvme_kv.c"
#include "nvme/nvme_ns_cmd.c"
#include "nvme/nvme.c"

#include "common/lib/test_env.c"
#include "common/lib/nvme/cmd_ut_common.h"

static struct nvme_driver _g_nvme_driver = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
};

static struct nvme_request *g_request = NULL;

/* Single 16-byte master key used by all tests. Shorter key lengths take a prefix. */
static const uint8_t g_master_key[SPDK_NVME_KV_KEY_MAX_LEN] = {
	0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
	0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0xFF,
};

/* Key lengths exercised across all command tests: min, half, boundary, max. */
static const uint8_t g_test_key_lens[] = { 1, 8, 9, 16 };

DEFINE_STUB_V(nvme_io_msg_ctrlr_detach, (struct spdk_nvme_ctrlr *ctrlr));

DEFINE_STUB_V(nvme_ctrlr_destruct_async,
	      (struct spdk_nvme_ctrlr *ctrlr, struct nvme_ctrlr_detach_ctx *ctx));

DEFINE_STUB(nvme_ctrlr_destruct_poll_async,
	    int,
	    (struct spdk_nvme_ctrlr *ctrlr, struct nvme_ctrlr_detach_ctx *ctx),
	    0);

DEFINE_STUB(spdk_nvme_poll_group_process_completions,
	    int64_t,
	    (struct spdk_nvme_poll_group *group, uint32_t completions_per_qpair,
	     spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb),
	    0);

DEFINE_STUB(spdk_nvme_ctrlr_get_regs_csts,
	    union spdk_nvme_csts_register,
	    (struct spdk_nvme_ctrlr *ctrlr),
	    {});

DEFINE_STUB(spdk_pci_event_listen, int, (void), 1);

DEFINE_STUB_V(nvme_ctrlr_fail,
	      (struct spdk_nvme_ctrlr *ctrlr, bool hotremove));

DEFINE_STUB(nvme_transport_ctrlr_destruct,
	    int,
	    (struct spdk_nvme_ctrlr *ctrlr),
	    0);

DEFINE_STUB(nvme_transport_ctrlr_scan_attached,
	    int,
	    (struct spdk_nvme_probe_ctx *probe_ctx),
	    0);

int
nvme_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	g_request = req;

	return 0;
}

void
nvme_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
}

void
nvme_ctrlr_proc_get_ref(struct spdk_nvme_ctrlr *ctrlr)
{
	return;
}

int
nvme_ctrlr_process_init(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

void
nvme_ctrlr_proc_put_ref(struct spdk_nvme_ctrlr *ctrlr)
{
	return;
}

void
spdk_nvme_ctrlr_get_default_ctrlr_opts(struct spdk_nvme_ctrlr_opts *opts, size_t opts_size)
{
	memset(opts, 0, sizeof(*opts));
}

bool
spdk_nvme_transport_available_by_name(const char *transport_name)
{
	return true;
}

struct spdk_nvme_ctrlr *nvme_transport_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
		const struct spdk_nvme_ctrlr_opts *opts,
		void *devhandle)
{
	return NULL;
}

int
nvme_ctrlr_get_ref_count(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

int
nvme_transport_ctrlr_scan(struct spdk_nvme_probe_ctx *probe_ctx,
			  bool direct_connect)
{
	return 0;
}

static void
prepare_for_test(struct spdk_nvme_ns *ns, struct spdk_nvme_ctrlr *ctrlr,
		 struct spdk_nvme_qpair *qpair)
{
	memset(ctrlr, 0, sizeof(*ctrlr));
	ctrlr->max_xfer_size = 0x100000;
	ctrlr->flags = 0;
	ctrlr->min_page_size = 4096;
	ctrlr->page_size = 4096;
	memset(&ctrlr->opts, 0, sizeof(ctrlr->opts));
	memset(ns, 0, sizeof(*ns));
	ns->ctrlr = ctrlr;
	ns->id = 1;

	ut_qpair_init(qpair, ctrlr);
	g_request = NULL;
}

static void
cleanup_after_test(struct spdk_nvme_qpair *qpair)
{
	ut_qpair_cleanup(qpair);
}

/*
 * Verify that a KV command encodes the key into CDW2-3 (low 8 bytes) and
 * CDW14-15 (high 8 bytes) and reports the length in CDW11. Empty keys must
 * leave all four DWORDs zero.
 */
static void
verify_key_encoding(const struct spdk_nvme_cmd *cmd, const void *key, uint8_t key_len)
{
	const uint8_t *key_bytes = (const uint8_t *)key;
	const uint8_t *cdw2_bytes = (const uint8_t *)&cmd->cdw2;
	const uint8_t *cdw14_bytes = (const uint8_t *)&cmd->cdw14;
	uint32_t i;

	CU_ASSERT(cmd->cdw11_bits.kv.kl == key_len);

	if (key_len == 0) {
		CU_ASSERT(cmd->cdw2 == 0);
		CU_ASSERT(cmd->cdw3 == 0);
		CU_ASSERT(cmd->cdw14 == 0);
		CU_ASSERT(cmd->cdw15 == 0);
		return;
	}

	for (i = 0; i < spdk_min(key_len, 8); i++) {
		CU_ASSERT(cdw2_bytes[i] == key_bytes[i]);
	}
	for (i = key_len; i < 8; i++) {
		CU_ASSERT(cdw2_bytes[i] == 0);
	}

	if (key_len > 8) {
		uint32_t remaining_len = key_len - 8;
		for (i = 0; i < remaining_len; i++) {
			CU_ASSERT(cdw14_bytes[i] == key_bytes[8 + i]);
		}
		for (i = remaining_len; i < 8; i++) {
			CU_ASSERT(cdw14_bytes[i] == 0);
		}
	} else {
		CU_ASSERT(cmd->cdw14 == 0);
		CU_ASSERT(cmd->cdw15 == 0);
	}
}

static void
test_nvme_kv_store_key_lengths(void)
{
	struct spdk_nvme_ns ns;
	struct spdk_nvme_ctrlr ctrlr;
	struct spdk_nvme_qpair qpair;
	uint8_t value[256] = {0};
	uint32_t value_len = sizeof(value);
	size_t i;

	prepare_for_test(&ns, &ctrlr, &qpair);

	for (i = 0; i < SPDK_COUNTOF(g_test_key_lens); i++) {
		uint8_t key_len = g_test_key_lens[i];
		int rc;

		rc = spdk_nvme_kv_store(&ns, &qpair, g_master_key, key_len,
					value, value_len, NULL, NULL, 0);
		CU_ASSERT(rc == 0);
		SPDK_CU_ASSERT_FATAL(g_request != NULL);
		CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_KV_STORE);
		CU_ASSERT(g_request->cmd.nsid == ns.id);
		CU_ASSERT(g_request->cmd.cdw10_bits.kv.vsize == value_len);
		CU_ASSERT(g_request->cmd.cdw11_bits.kv.ro == 0);
		verify_key_encoding(&g_request->cmd, g_master_key, key_len);
		CU_ASSERT(g_request->payload.size == value_len);
		ut_user_req_cleanup(&g_request);
	}

	cleanup_after_test(&qpair);
}

static void
test_nvme_kv_store_options(void)
{
	struct spdk_nvme_ns ns;
	struct spdk_nvme_ctrlr ctrlr;
	struct spdk_nvme_qpair qpair;
	uint8_t value[128] = {0};
	uint32_t value_len = sizeof(value);
	const uint32_t opts[] = {
		SPDK_NVME_KV_STORE_OPT_DONT_STORE_IF_KEY_NOT_EXISTS,
		SPDK_NVME_KV_STORE_OPT_DONT_STORE_IF_KEY_EXISTS,
		SPDK_NVME_KV_STORE_OPT_DONT_COMPRESS,
		SPDK_NVME_KV_STORE_OPT_DONT_STORE_IF_KEY_NOT_EXISTS |
		SPDK_NVME_KV_STORE_OPT_DONT_COMPRESS,
	};
	size_t i;

	prepare_for_test(&ns, &ctrlr, &qpair);

	for (i = 0; i < SPDK_COUNTOF(opts); i++) {
		int rc;

		rc = spdk_nvme_kv_store(&ns, &qpair, g_master_key, 4,
					value, value_len, NULL, NULL, opts[i]);
		CU_ASSERT(rc == 0);
		SPDK_CU_ASSERT_FATAL(g_request != NULL);
		CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_KV_STORE);
		CU_ASSERT(g_request->cmd.cdw11_bits.kv.ro == opts[i]);
		ut_user_req_cleanup(&g_request);
	}

	cleanup_after_test(&qpair);
}

static void
test_nvme_kv_store_errors(void)
{
	struct spdk_nvme_ns ns;
	struct spdk_nvme_ctrlr ctrlr;
	struct spdk_nvme_qpair qpair;
	uint8_t value[128] = {0};
	uint32_t value_len = sizeof(value);
	int rc;

	prepare_for_test(&ns, &ctrlr, &qpair);

	rc = spdk_nvme_kv_store(&ns, &qpair, NULL, 4, value, value_len, NULL, NULL, 0);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_request == NULL);

	rc = spdk_nvme_kv_store(&ns, &qpair, g_master_key, 0, value, value_len, NULL, NULL, 0);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_request == NULL);

	rc = spdk_nvme_kv_store(&ns, &qpair, g_master_key, SPDK_NVME_KV_KEY_MAX_LEN + 1,
				value, value_len, NULL, NULL, 0);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_request == NULL);

	rc = spdk_nvme_kv_store(&ns, &qpair, g_master_key, 4, NULL, value_len, NULL, NULL, 0);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_request == NULL);

	cleanup_after_test(&qpair);
}

static void
test_nvme_kv_retrieve(void)
{
	struct spdk_nvme_ns ns;
	struct spdk_nvme_ctrlr ctrlr;
	struct spdk_nvme_qpair qpair;
	uint8_t value[512] = {0};
	uint32_t value_len = sizeof(value);
	const uint32_t opts[] = {
		0,
		SPDK_NVME_KV_RETRIEVE_OPT_RETRIEVE_RAW,
	};
	size_t i;

	prepare_for_test(&ns, &ctrlr, &qpair);

	for (i = 0; i < SPDK_COUNTOF(opts); i++) {
		int rc;

		rc = spdk_nvme_kv_retrieve(&ns, &qpair, g_master_key, 8,
					   value, value_len, NULL, NULL, opts[i]);
		CU_ASSERT(rc == 0);
		SPDK_CU_ASSERT_FATAL(g_request != NULL);
		CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_KV_RETRIEVE);
		CU_ASSERT(g_request->cmd.nsid == ns.id);
		CU_ASSERT(g_request->cmd.cdw10_bits.kv.vsize == value_len);
		CU_ASSERT(g_request->cmd.cdw11_bits.kv.ro == opts[i]);
		verify_key_encoding(&g_request->cmd, g_master_key, 8);
		CU_ASSERT(g_request->payload.size == value_len);
		ut_user_req_cleanup(&g_request);
	}

	cleanup_after_test(&qpair);
}

static void
test_nvme_kv_retrieve_errors(void)
{
	struct spdk_nvme_ns ns;
	struct spdk_nvme_ctrlr ctrlr;
	struct spdk_nvme_qpair qpair;
	uint8_t value[128] = {0};
	uint32_t value_len = sizeof(value);
	int rc;

	prepare_for_test(&ns, &ctrlr, &qpair);

	rc = spdk_nvme_kv_retrieve(&ns, &qpair, NULL, 4, value, value_len, NULL, NULL, 0);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_request == NULL);

	rc = spdk_nvme_kv_retrieve(&ns, &qpair, g_master_key, 0, value, value_len, NULL, NULL, 0);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_request == NULL);

	rc = spdk_nvme_kv_retrieve(&ns, &qpair, g_master_key, SPDK_NVME_KV_KEY_MAX_LEN + 1,
				   value, value_len, NULL, NULL, 0);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_request == NULL);

	rc = spdk_nvme_kv_retrieve(&ns, &qpair, g_master_key, 4, NULL, value_len, NULL, NULL, 0);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_request == NULL);

	cleanup_after_test(&qpair);
}

/*
 * A zero length value is legal. Store creates an empty value and Retrieve
 * queries the value size, so neither transfers data. Both must still report
 * the length the caller asked for in CDW10.
 */
static void
test_nvme_kv_zero_length_value(void)
{
	struct spdk_nvme_ns ns;
	struct spdk_nvme_ctrlr ctrlr;
	struct spdk_nvme_qpair qpair;
	uint8_t value[128] = {0};
	int rc;

	prepare_for_test(&ns, &ctrlr, &qpair);

	rc = spdk_nvme_kv_store(&ns, &qpair, g_master_key, 8, value, 0, NULL, NULL, 0);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_KV_STORE);
	CU_ASSERT(g_request->cmd.cdw10_bits.kv.vsize == 0);
	verify_key_encoding(&g_request->cmd, g_master_key, 8);
	CU_ASSERT(g_request->payload.size == 0);
	ut_req_cleanup(&g_request);

	/* A NULL buffer is fine when nothing is transferred. */
	rc = spdk_nvme_kv_store(&ns, &qpair, g_master_key, 8, NULL, 0, NULL, NULL, 0);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.cdw10_bits.kv.vsize == 0);
	CU_ASSERT(g_request->payload.size == 0);
	ut_req_cleanup(&g_request);

	rc = spdk_nvme_kv_retrieve(&ns, &qpair, g_master_key, 8, NULL, 0, NULL, NULL, 0);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_KV_RETRIEVE);
	CU_ASSERT(g_request->cmd.cdw10_bits.kv.vsize == 0);
	verify_key_encoding(&g_request->cmd, g_master_key, 8);
	CU_ASSERT(g_request->payload.size == 0);
	ut_req_cleanup(&g_request);

	cleanup_after_test(&qpair);
}

static void
test_nvme_kv_delete(void)
{
	struct spdk_nvme_ns ns;
	struct spdk_nvme_ctrlr ctrlr;
	struct spdk_nvme_qpair qpair;
	size_t i;

	prepare_for_test(&ns, &ctrlr, &qpair);

	for (i = 0; i < SPDK_COUNTOF(g_test_key_lens); i++) {
		uint8_t key_len = g_test_key_lens[i];
		int rc;

		rc = spdk_nvme_kv_delete(&ns, &qpair, g_master_key, key_len, NULL, NULL);
		CU_ASSERT(rc == 0);
		SPDK_CU_ASSERT_FATAL(g_request != NULL);
		CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_KV_DELETE);
		CU_ASSERT(g_request->cmd.nsid == ns.id);
		verify_key_encoding(&g_request->cmd, g_master_key, key_len);
		CU_ASSERT(g_request->payload.size == 0);
		ut_req_cleanup(&g_request);
	}

	cleanup_after_test(&qpair);
}

static void
test_nvme_kv_delete_errors(void)
{
	struct spdk_nvme_ns ns;
	struct spdk_nvme_ctrlr ctrlr;
	struct spdk_nvme_qpair qpair;
	int rc;

	prepare_for_test(&ns, &ctrlr, &qpair);

	rc = spdk_nvme_kv_delete(&ns, &qpair, NULL, 4, NULL, NULL);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_request == NULL);

	rc = spdk_nvme_kv_delete(&ns, &qpair, g_master_key, 0, NULL, NULL);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_request == NULL);

	rc = spdk_nvme_kv_delete(&ns, &qpair, g_master_key, SPDK_NVME_KV_KEY_MAX_LEN + 1,
				 NULL, NULL);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_request == NULL);

	cleanup_after_test(&qpair);
}

static void
test_nvme_kv_exist(void)
{
	struct spdk_nvme_ns ns;
	struct spdk_nvme_ctrlr ctrlr;
	struct spdk_nvme_qpair qpair;
	int rc;

	prepare_for_test(&ns, &ctrlr, &qpair);

	rc = spdk_nvme_kv_exist(&ns, &qpair, g_master_key, 8, NULL, NULL);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_KV_EXIST);
	CU_ASSERT(g_request->cmd.nsid == ns.id);
	verify_key_encoding(&g_request->cmd, g_master_key, 8);
	CU_ASSERT(g_request->payload.size == 0);
	ut_req_cleanup(&g_request);

	cleanup_after_test(&qpair);
}

static void
test_nvme_kv_exist_errors(void)
{
	struct spdk_nvme_ns ns;
	struct spdk_nvme_ctrlr ctrlr;
	struct spdk_nvme_qpair qpair;
	int rc;

	prepare_for_test(&ns, &ctrlr, &qpair);

	rc = spdk_nvme_kv_exist(&ns, &qpair, NULL, 4, NULL, NULL);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_request == NULL);

	rc = spdk_nvme_kv_exist(&ns, &qpair, g_master_key, 0, NULL, NULL);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_request == NULL);

	rc = spdk_nvme_kv_exist(&ns, &qpair, g_master_key, SPDK_NVME_KV_KEY_MAX_LEN + 1,
				NULL, NULL);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_request == NULL);

	cleanup_after_test(&qpair);
}

/*
 * Iterate over start_key lengths including 0, which means "list all keys" and
 * is encoded as NULL/0 on the API.
 */
static void
test_nvme_kv_list(void)
{
	struct spdk_nvme_ns ns;
	struct spdk_nvme_ctrlr ctrlr;
	struct spdk_nvme_qpair qpair;
	uint8_t buffer[1024] = {0};
	uint32_t buffer_len = sizeof(buffer);
	const uint8_t key_lens[] = { 0, 1, 8, 16 };
	size_t i;

	prepare_for_test(&ns, &ctrlr, &qpair);

	for (i = 0; i < SPDK_COUNTOF(key_lens); i++) {
		uint8_t key_len = key_lens[i];
		const uint8_t *start_key = key_len == 0 ? NULL : g_master_key;
		int rc;

		rc = spdk_nvme_kv_list(&ns, &qpair, start_key, key_len,
				       buffer, buffer_len, NULL, NULL);
		CU_ASSERT(rc == 0);
		SPDK_CU_ASSERT_FATAL(g_request != NULL);
		CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_KV_LIST);
		CU_ASSERT(g_request->cmd.nsid == ns.id);
		CU_ASSERT(g_request->cmd.cdw10_bits.kv.vsize == buffer_len);
		verify_key_encoding(&g_request->cmd, start_key, key_len);
		CU_ASSERT(g_request->payload.size == buffer_len);
		ut_user_req_cleanup(&g_request);
	}

	cleanup_after_test(&qpair);
}

static void
test_nvme_kv_list_errors(void)
{
	struct spdk_nvme_ns ns;
	struct spdk_nvme_ctrlr ctrlr;
	struct spdk_nvme_qpair qpair;
	uint8_t buffer[1024] = {0};
	uint32_t buffer_len = sizeof(buffer);
	int rc;

	prepare_for_test(&ns, &ctrlr, &qpair);

	rc = spdk_nvme_kv_list(&ns, &qpair, g_master_key, 4, NULL, buffer_len, NULL, NULL);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_request == NULL);

	rc = spdk_nvme_kv_list(&ns, &qpair, g_master_key, 4, buffer, 0, NULL, NULL);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_request == NULL);

	rc = spdk_nvme_kv_list(&ns, &qpair, g_master_key, SPDK_NVME_KV_KEY_MAX_LEN + 1,
			       buffer, buffer_len, NULL, NULL);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_request == NULL);

	cleanup_after_test(&qpair);
}

static void
test_nvme_kv_ns_helpers(void)
{
	struct spdk_nvme_ns ns;
	struct spdk_nvme_kv_ns_data nsdata_kv = {};
	const struct spdk_nvme_kv_ns_data *ret_nsdata;

	memset(&ns, 0, sizeof(ns));

	ret_nsdata = spdk_nvme_kv_ns_get_data(&ns);
	CU_ASSERT(ret_nsdata == NULL);

	nsdata_kv.kvfc.kvfi = 0;
	nsdata_kv.kvf[0].kvkml = 16;
	nsdata_kv.kvf[0].kvvml = 4096;
	nsdata_kv.novg = 1024;
	ns.nsdata_kv = &nsdata_kv;

	ret_nsdata = spdk_nvme_kv_ns_get_data(&ns);
	CU_ASSERT(ret_nsdata == &nsdata_kv);
	CU_ASSERT(ret_nsdata->kvf[0].kvkml == 16);
	CU_ASSERT(ret_nsdata->kvf[0].kvvml == 4096);
	CU_ASSERT(ret_nsdata->novg == 1024);
}

static void
test_nvme_kv_ctrlr_helpers(void)
{
	struct spdk_nvme_ctrlr ctrlr;
	struct spdk_nvme_kv_ctrlr_data cdata_kv = {};
	const struct spdk_nvme_kv_ctrlr_data *ret_cdata;

	memset(&ctrlr, 0, sizeof(ctrlr));

	ret_cdata = spdk_nvme_kv_ctrlr_get_data(&ctrlr);
	CU_ASSERT(ret_cdata == NULL);

	ctrlr.cdata_kv = &cdata_kv;
	ret_cdata = spdk_nvme_kv_ctrlr_get_data(&ctrlr);
	CU_ASSERT(ret_cdata == &cdata_kv);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("nvme_kv", NULL, NULL);

	CU_ADD_TEST(suite, test_nvme_kv_store_key_lengths);
	CU_ADD_TEST(suite, test_nvme_kv_store_options);
	CU_ADD_TEST(suite, test_nvme_kv_store_errors);

	CU_ADD_TEST(suite, test_nvme_kv_retrieve);
	CU_ADD_TEST(suite, test_nvme_kv_retrieve_errors);

	CU_ADD_TEST(suite, test_nvme_kv_zero_length_value);

	CU_ADD_TEST(suite, test_nvme_kv_delete);
	CU_ADD_TEST(suite, test_nvme_kv_delete_errors);

	CU_ADD_TEST(suite, test_nvme_kv_exist);
	CU_ADD_TEST(suite, test_nvme_kv_exist_errors);

	CU_ADD_TEST(suite, test_nvme_kv_list);
	CU_ADD_TEST(suite, test_nvme_kv_list_errors);

	CU_ADD_TEST(suite, test_nvme_kv_ns_helpers);
	CU_ADD_TEST(suite, test_nvme_kv_ctrlr_helpers);

	g_spdk_nvme_driver = &_g_nvme_driver;

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
