/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 Intel Corporation.  All rights reserved.
 */

#include "spdk_internal/cunit.h"

#include "nvme/nvme_auth.c"

#include "common/lib/test_env.c"

SPDK_LOG_REGISTER_COMPONENT(nvme)

pid_t g_spdk_nvme_pid;

DEFINE_STUB(nvme_qpair_state_string, const char *, (enum nvme_qpair_state state), "UNKNOWN");
DEFINE_STUB_V(nvme_completion_poll_cb, (void *arg, const struct spdk_nvme_cpl *cpl));
DEFINE_STUB(nvme_qpair_submit_request, int, (struct spdk_nvme_qpair *qpair,
		struct nvme_request *req), 0);
DEFINE_STUB(nvme_wait_for_completion_poll, int, (struct spdk_nvme_qpair *qpair,
		struct nvme_completion_poll_status *status), 0);
DEFINE_STUB(spdk_key_get_name, const char *, (struct spdk_key *key), NULL);
DEFINE_STUB(spdk_key_get_key, int, (struct spdk_key *key, void *buf, int len), 0);
DEFINE_STUB(spdk_key_dup, struct spdk_key *, (struct spdk_key *key), NULL);
DEFINE_STUB_V(spdk_keyring_put_key, (struct spdk_key *key));
DEFINE_STUB_V(nvme_fabric_qpair_poll_cleanup, (struct spdk_nvme_qpair *qpair));
DEFINE_STUB_V(nvme_fabric_qpair_auth_cleanup, (struct spdk_nvme_qpair *qpair, int status));

static struct spdk_nvme_ctrlr g_ctrlr;
static struct spdk_nvme_qpair g_qpair;

static void
init_qpair(void)
{
	memset(&g_ctrlr, 0, sizeof(g_ctrlr));
	memset(&g_qpair, 0, sizeof(g_qpair));
	g_qpair.ctrlr = &g_ctrlr;
}

static void
test_auth_digest_allowed(void)
{
	init_qpair();

	/* Allow only SHA256 */
	g_ctrlr.opts.dhchap_digests = SPDK_BIT(SPDK_NVMF_DHCHAP_HASH_SHA256);
	CU_ASSERT(nvme_auth_digest_allowed(&g_qpair, SPDK_NVMF_DHCHAP_HASH_SHA256) == true);
	CU_ASSERT(nvme_auth_digest_allowed(&g_qpair, SPDK_NVMF_DHCHAP_HASH_SHA384) == false);
	CU_ASSERT(nvme_auth_digest_allowed(&g_qpair, SPDK_NVMF_DHCHAP_HASH_SHA512) == false);

	/* Allow SHA384 and SHA512 */
	g_ctrlr.opts.dhchap_digests = SPDK_BIT(SPDK_NVMF_DHCHAP_HASH_SHA384) |
				      SPDK_BIT(SPDK_NVMF_DHCHAP_HASH_SHA512);
	CU_ASSERT(nvme_auth_digest_allowed(&g_qpair, SPDK_NVMF_DHCHAP_HASH_SHA256) == false);
	CU_ASSERT(nvme_auth_digest_allowed(&g_qpair, SPDK_NVMF_DHCHAP_HASH_SHA384) == true);
	CU_ASSERT(nvme_auth_digest_allowed(&g_qpair, SPDK_NVMF_DHCHAP_HASH_SHA512) == true);

	/* No digests allowed */
	g_ctrlr.opts.dhchap_digests = 0;
	CU_ASSERT(nvme_auth_digest_allowed(&g_qpair, SPDK_NVMF_DHCHAP_HASH_SHA256) == false);
}

static void
test_auth_dhgroup_allowed(void)
{
	init_qpair();

	/* Allow only null dhgroup */
	g_ctrlr.opts.dhchap_dhgroups = SPDK_BIT(SPDK_NVMF_DHCHAP_DHGROUP_NULL);
	CU_ASSERT(nvme_auth_dhgroup_allowed(&g_qpair, SPDK_NVMF_DHCHAP_DHGROUP_NULL) == true);
	CU_ASSERT(nvme_auth_dhgroup_allowed(&g_qpair, SPDK_NVMF_DHCHAP_DHGROUP_2048) == false);
	CU_ASSERT(nvme_auth_dhgroup_allowed(&g_qpair, SPDK_NVMF_DHCHAP_DHGROUP_3072) == false);
	CU_ASSERT(nvme_auth_dhgroup_allowed(&g_qpair, SPDK_NVMF_DHCHAP_DHGROUP_4096) == false);
	CU_ASSERT(nvme_auth_dhgroup_allowed(&g_qpair, SPDK_NVMF_DHCHAP_DHGROUP_6144) == false);
	CU_ASSERT(nvme_auth_dhgroup_allowed(&g_qpair, SPDK_NVMF_DHCHAP_DHGROUP_8192) == false);

	/* Allow ffdhe2048 and ffdhe8192 */
	g_ctrlr.opts.dhchap_dhgroups = SPDK_BIT(SPDK_NVMF_DHCHAP_DHGROUP_2048) |
				       SPDK_BIT(SPDK_NVMF_DHCHAP_DHGROUP_8192);
	CU_ASSERT(nvme_auth_dhgroup_allowed(&g_qpair, SPDK_NVMF_DHCHAP_DHGROUP_NULL) == false);
	CU_ASSERT(nvme_auth_dhgroup_allowed(&g_qpair, SPDK_NVMF_DHCHAP_DHGROUP_2048) == true);
	CU_ASSERT(nvme_auth_dhgroup_allowed(&g_qpair, SPDK_NVMF_DHCHAP_DHGROUP_3072) == false);
	CU_ASSERT(nvme_auth_dhgroup_allowed(&g_qpair, SPDK_NVMF_DHCHAP_DHGROUP_4096) == false);
	CU_ASSERT(nvme_auth_dhgroup_allowed(&g_qpair, SPDK_NVMF_DHCHAP_DHGROUP_6144) == false);
	CU_ASSERT(nvme_auth_dhgroup_allowed(&g_qpair, SPDK_NVMF_DHCHAP_DHGROUP_8192) == true);

	/* No dhgroups allowed */
	g_ctrlr.opts.dhchap_dhgroups = 0;
	CU_ASSERT(nvme_auth_dhgroup_allowed(&g_qpair, SPDK_NVMF_DHCHAP_DHGROUP_NULL) == false);
	CU_ASSERT(nvme_auth_dhgroup_allowed(&g_qpair, SPDK_NVMF_DHCHAP_DHGROUP_2048) == false);
	CU_ASSERT(nvme_auth_dhgroup_allowed(&g_qpair, SPDK_NVMF_DHCHAP_DHGROUP_3072) == false);
	CU_ASSERT(nvme_auth_dhgroup_allowed(&g_qpair, SPDK_NVMF_DHCHAP_DHGROUP_4096) == false);
	CU_ASSERT(nvme_auth_dhgroup_allowed(&g_qpair, SPDK_NVMF_DHCHAP_DHGROUP_6144) == false);
	CU_ASSERT(nvme_auth_dhgroup_allowed(&g_qpair, SPDK_NVMF_DHCHAP_DHGROUP_8192) == false);
}

static void
test_auth_set_failure(void)
{
	init_qpair();

	/* Set failure without failure2 -> goes to DONE */
	nvme_auth_set_failure(&g_qpair, -EIO, false);
	CU_ASSERT(g_qpair.auth.status == -EIO);
	CU_ASSERT(g_qpair.auth.state == NVME_QPAIR_AUTH_STATE_DONE);

	/* Status should not be overwritten once set */
	nvme_auth_set_failure(&g_qpair, -EINVAL, false);
	CU_ASSERT(g_qpair.auth.status == -EIO);

	/* Reset and test failure2 -> goes to AWAIT_FAILURE2 */
	init_qpair();
	nvme_auth_set_failure(&g_qpair, -EACCES, true);
	CU_ASSERT(g_qpair.auth.status == -EACCES);
	CU_ASSERT(g_qpair.auth.state == NVME_QPAIR_AUTH_STATE_AWAIT_FAILURE2);
}

static void
test_dhchap_dhkey_free(void)
{
	struct spdk_nvme_dhchap_dhkey *key = NULL;

	/* NULL pointer should not crash */
	spdk_nvme_dhchap_dhkey_free(NULL);

	/* Pointer to NULL should not crash and remain NULL */
	spdk_nvme_dhchap_dhkey_free(&key);
	CU_ASSERT(key == NULL);
}

static void
test_dhchap_get_digest_id(void)
{
	/* Valid digests */
	CU_ASSERT(spdk_nvme_dhchap_get_digest_id("sha256") == SPDK_NVMF_DHCHAP_HASH_SHA256);
	CU_ASSERT(spdk_nvme_dhchap_get_digest_id("sha384") == SPDK_NVMF_DHCHAP_HASH_SHA384);
	CU_ASSERT(spdk_nvme_dhchap_get_digest_id("sha512") == SPDK_NVMF_DHCHAP_HASH_SHA512);

	/* Invalid digests */
	CU_ASSERT(spdk_nvme_dhchap_get_digest_id("md5") == -EINVAL);
	CU_ASSERT(spdk_nvme_dhchap_get_digest_id("") == -EINVAL);
	CU_ASSERT(spdk_nvme_dhchap_get_digest_id("SHA256") == -EINVAL);
}

static void
test_dhchap_get_digest_name(void)
{
	/* Valid ids */
	CU_ASSERT_STRING_EQUAL(spdk_nvme_dhchap_get_digest_name(SPDK_NVMF_DHCHAP_HASH_SHA256),
			       "sha256");
	CU_ASSERT_STRING_EQUAL(spdk_nvme_dhchap_get_digest_name(SPDK_NVMF_DHCHAP_HASH_SHA384),
			       "sha384");
	CU_ASSERT_STRING_EQUAL(spdk_nvme_dhchap_get_digest_name(SPDK_NVMF_DHCHAP_HASH_SHA512),
			       "sha512");

	/* Invalid ids */
	CU_ASSERT(spdk_nvme_dhchap_get_digest_name(SPDK_NVMF_DHCHAP_HASH_NONE) == NULL);
	CU_ASSERT(spdk_nvme_dhchap_get_digest_name(0xff) == NULL);
}

static void
test_dhchap_get_digest_length(void)
{
	CU_ASSERT(spdk_nvme_dhchap_get_digest_length(SPDK_NVMF_DHCHAP_HASH_SHA256) == 32);
	CU_ASSERT(spdk_nvme_dhchap_get_digest_length(SPDK_NVMF_DHCHAP_HASH_SHA384) == 48);
	CU_ASSERT(spdk_nvme_dhchap_get_digest_length(SPDK_NVMF_DHCHAP_HASH_SHA512) == 64);

	/* Invalid digest */
	CU_ASSERT(spdk_nvme_dhchap_get_digest_length(SPDK_NVMF_DHCHAP_HASH_NONE) == 0);
	CU_ASSERT(spdk_nvme_dhchap_get_digest_length(0xff) == 0);
}

static void
test_dhchap_get_dhgroup_id(void)
{
	/* Valid dhgroups */
	CU_ASSERT(spdk_nvme_dhchap_get_dhgroup_id("null") == SPDK_NVMF_DHCHAP_DHGROUP_NULL);
	CU_ASSERT(spdk_nvme_dhchap_get_dhgroup_id("ffdhe2048") == SPDK_NVMF_DHCHAP_DHGROUP_2048);
	CU_ASSERT(spdk_nvme_dhchap_get_dhgroup_id("ffdhe3072") == SPDK_NVMF_DHCHAP_DHGROUP_3072);
	CU_ASSERT(spdk_nvme_dhchap_get_dhgroup_id("ffdhe4096") == SPDK_NVMF_DHCHAP_DHGROUP_4096);
	CU_ASSERT(spdk_nvme_dhchap_get_dhgroup_id("ffdhe6144") == SPDK_NVMF_DHCHAP_DHGROUP_6144);
	CU_ASSERT(spdk_nvme_dhchap_get_dhgroup_id("ffdhe8192") == SPDK_NVMF_DHCHAP_DHGROUP_8192);

	/* Invalid dhgroups */
	CU_ASSERT(spdk_nvme_dhchap_get_dhgroup_id("invalid") == -EINVAL);
	CU_ASSERT(spdk_nvme_dhchap_get_dhgroup_id("") == -EINVAL);
	CU_ASSERT(spdk_nvme_dhchap_get_dhgroup_id("NULL") == -EINVAL);
}

static void
test_dhchap_get_dhgroup_name(void)
{
	/* Valid ids */
	CU_ASSERT_STRING_EQUAL(spdk_nvme_dhchap_get_dhgroup_name(SPDK_NVMF_DHCHAP_DHGROUP_NULL),
			       "null");
	CU_ASSERT_STRING_EQUAL(spdk_nvme_dhchap_get_dhgroup_name(SPDK_NVMF_DHCHAP_DHGROUP_2048),
			       "ffdhe2048");
	CU_ASSERT_STRING_EQUAL(spdk_nvme_dhchap_get_dhgroup_name(SPDK_NVMF_DHCHAP_DHGROUP_3072),
			       "ffdhe3072");
	CU_ASSERT_STRING_EQUAL(spdk_nvme_dhchap_get_dhgroup_name(SPDK_NVMF_DHCHAP_DHGROUP_4096),
			       "ffdhe4096");
	CU_ASSERT_STRING_EQUAL(spdk_nvme_dhchap_get_dhgroup_name(SPDK_NVMF_DHCHAP_DHGROUP_6144),
			       "ffdhe6144");
	CU_ASSERT_STRING_EQUAL(spdk_nvme_dhchap_get_dhgroup_name(SPDK_NVMF_DHCHAP_DHGROUP_8192),
			       "ffdhe8192");

	/* Invalid ids */
	CU_ASSERT(spdk_nvme_dhchap_get_dhgroup_name(0xff) == NULL);
}

static void
test_dhchap_digest_roundtrip(void)
{
	int id;
	const char *name;

	/* Verify all digests can be looked up by name and back */
	name = spdk_nvme_dhchap_get_digest_name(SPDK_NVMF_DHCHAP_HASH_SHA256);
	SPDK_CU_ASSERT_FATAL(name != NULL);
	id = spdk_nvme_dhchap_get_digest_id(name);
	CU_ASSERT(id == SPDK_NVMF_DHCHAP_HASH_SHA256);

	name = spdk_nvme_dhchap_get_digest_name(SPDK_NVMF_DHCHAP_HASH_SHA384);
	SPDK_CU_ASSERT_FATAL(name != NULL);
	id = spdk_nvme_dhchap_get_digest_id(name);
	CU_ASSERT(id == SPDK_NVMF_DHCHAP_HASH_SHA384);

	name = spdk_nvme_dhchap_get_digest_name(SPDK_NVMF_DHCHAP_HASH_SHA512);
	SPDK_CU_ASSERT_FATAL(name != NULL);
	id = spdk_nvme_dhchap_get_digest_id(name);
	CU_ASSERT(id == SPDK_NVMF_DHCHAP_HASH_SHA512);
}

static void
test_dhchap_dhgroup_roundtrip(void)
{
	int id;
	const char *name;

	/* Verify all dhgroups can be looked up by name and back */
	name = spdk_nvme_dhchap_get_dhgroup_name(SPDK_NVMF_DHCHAP_DHGROUP_NULL);
	SPDK_CU_ASSERT_FATAL(name != NULL);
	id = spdk_nvme_dhchap_get_dhgroup_id(name);
	CU_ASSERT(id == SPDK_NVMF_DHCHAP_DHGROUP_NULL);

	name = spdk_nvme_dhchap_get_dhgroup_name(SPDK_NVMF_DHCHAP_DHGROUP_2048);
	SPDK_CU_ASSERT_FATAL(name != NULL);
	id = spdk_nvme_dhchap_get_dhgroup_id(name);
	CU_ASSERT(id == SPDK_NVMF_DHCHAP_DHGROUP_2048);

	name = spdk_nvme_dhchap_get_dhgroup_name(SPDK_NVMF_DHCHAP_DHGROUP_8192);
	SPDK_CU_ASSERT_FATAL(name != NULL);
	id = spdk_nvme_dhchap_get_dhgroup_id(name);
	CU_ASSERT(id == SPDK_NVMF_DHCHAP_DHGROUP_8192);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("nvme_auth", NULL, NULL);

	CU_ADD_TEST(suite, test_dhchap_get_digest_id);
	CU_ADD_TEST(suite, test_dhchap_get_digest_name);
	CU_ADD_TEST(suite, test_dhchap_get_digest_length);
	CU_ADD_TEST(suite, test_dhchap_get_dhgroup_id);
	CU_ADD_TEST(suite, test_dhchap_get_dhgroup_name);
	CU_ADD_TEST(suite, test_dhchap_digest_roundtrip);
	CU_ADD_TEST(suite, test_dhchap_dhgroup_roundtrip);
	CU_ADD_TEST(suite, test_auth_digest_allowed);
	CU_ADD_TEST(suite, test_auth_dhgroup_allowed);
	CU_ADD_TEST(suite, test_auth_set_failure);
	CU_ADD_TEST(suite, test_dhchap_dhkey_free);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
