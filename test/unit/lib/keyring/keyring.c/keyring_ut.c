/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation.  All rights reserved.
 */
#include "spdk_internal/cunit.h"
#include "spdk_internal/mock.h"
#include "spdk/util.h"

#include "keyring/keyring.c"

#include "unit/lib/json_mock.c"

struct ut_key {
#define UT_KEY_SIZE 64
	char buf[UT_KEY_SIZE];
	int len;
};

struct ut_key_opts {
	char *buf;
	int len;
};

static int g_add_status;

static int
ut_keyring_add_key(struct spdk_key *key, void *ctx)
{
	struct ut_key_opts *opts = ctx;
	struct ut_key *utkey = spdk_key_get_ctx(key);

	if (g_add_status) {
		return g_add_status;
	}

	SPDK_CU_ASSERT_FATAL(opts != NULL);

	/* Use spdk_json_val's start/len to pass a buffer with the key */
	memcpy(utkey->buf, opts->buf, opts->len);
	utkey->len = opts->len;

	return 0;
}

static bool g_remove_called;

static void
ut_keyring_remove_key(struct spdk_key *key)
{
	struct ut_key *utkey = spdk_key_get_ctx(key);

	memset(utkey->buf, 0, utkey->len);
	g_remove_called = true;
}

static int
ut_keyring_get_key(struct spdk_key *key, void *buf, int len)
{
	struct ut_key *utkey = spdk_key_get_ctx(key);

	memcpy(buf, utkey->buf, utkey->len);

	return utkey->len;
}

static size_t
ut_keyring_get_ctx_size(void)
{
	return sizeof(struct ut_key);
}

static struct spdk_keyring_module g_module = {
	.name = "ut",
	.add_key = ut_keyring_add_key,
	.remove_key = ut_keyring_remove_key,
	.get_key = ut_keyring_get_key,
	.get_ctx_size = ut_keyring_get_ctx_size,
};

SPDK_KEYRING_REGISTER_MODULE(ut, &g_module);

static void
test_keyring_add_remove(void)
{
	struct spdk_key_opts opts = {};
	struct spdk_key *key;
	char keybuf[UT_KEY_SIZE] = {}, rcvbuf[UT_KEY_SIZE] = {};
	struct ut_key_opts uopts = { .buf = keybuf, .len = UT_KEY_SIZE };
	struct spdk_keyring_module module2 = { .name = "ut2" };
	int rc;

	/* Add a key */
	memset(keybuf, 0xa5, UT_KEY_SIZE);
	opts.size = SPDK_SIZEOF(&opts, ctx);
	opts.name = "key0";
	opts.module = &g_module;
	opts.ctx = &uopts;
	rc = spdk_keyring_add_key(&opts);
	CU_ASSERT_EQUAL(rc, 0);

	/* Get a reference to that key */
	key = spdk_keyring_get_key("key0");
	CU_ASSERT_PTR_NOT_NULL(key);

	/* Get its keying material */
	rc = spdk_key_get_key(key, rcvbuf, UT_KEY_SIZE);
	CU_ASSERT_EQUAL(rc, UT_KEY_SIZE)
	CU_ASSERT_EQUAL(memcmp(rcvbuf, keybuf, UT_KEY_SIZE), 0);

	/* Remove it and try to get another reference */
	spdk_keyring_remove_key("key0", &g_module);
	CU_ASSERT_PTR_NULL(spdk_keyring_get_key("key0"));

	/* Now that the key has been remove spdk_key_get_key() should result in an -ENOKEY error */
	rc = spdk_key_get_key(key, rcvbuf, UT_KEY_SIZE);
	CU_ASSERT_EQUAL(rc, -ENOKEY);

	/* Finally, release the reference */
	spdk_keyring_put_key(key);

	/* Explicitly specify global keyring */
	rc = spdk_keyring_add_key(&opts);
	CU_ASSERT_EQUAL(rc, 0);
	key = spdk_keyring_get_key(":key0");
	CU_ASSERT_PTR_NOT_NULL(key);

	memset(rcvbuf, 0, UT_KEY_SIZE);
	rc = spdk_key_get_key(key, rcvbuf, UT_KEY_SIZE);
	CU_ASSERT_EQUAL(rc, UT_KEY_SIZE)
	CU_ASSERT_EQUAL(memcmp(rcvbuf, keybuf, UT_KEY_SIZE), 0);

	spdk_keyring_put_key(key);

	/* Remove the key without explicitly specifying global keyring */
	spdk_keyring_remove_key("key0", &g_module);
	CU_ASSERT_PTR_NULL(spdk_keyring_get_key("key0"));
	CU_ASSERT_PTR_NULL(spdk_keyring_get_key(":key0"));

	/* Try to create a key with the same name twice */
	opts.size = SPDK_SIZEOF(&opts, ctx);
	opts.name = "key0";
	opts.module = &g_module;
	opts.ctx = &uopts;
	rc = spdk_keyring_add_key(&opts);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_keyring_add_key(&opts);
	CU_ASSERT_EQUAL(rc, -EEXIST);

	/* Explicitly specify global keyring */
	opts.name = ":key0";
	rc = spdk_keyring_add_key(&opts);
	CU_ASSERT_EQUAL(rc, -EEXIST);

	/* Try to remove a key owned by a different module */
	spdk_keyring_remove_key("key0", &module2);
	CU_ASSERT_PTR_NOT_NULL(spdk_keyring_get_key("key0"));
	CU_ASSERT_PTR_NOT_NULL(spdk_keyring_get_key(":key0"));

	spdk_keyring_remove_key(":key0", &g_module);
	CU_ASSERT_PTR_NULL(spdk_keyring_get_key("key0"));
	CU_ASSERT_PTR_NULL(spdk_keyring_get_key(":key0"));
	CU_ASSERT(g_remove_called);
	g_remove_called = false;

	/* Remove an already removed key */
	spdk_keyring_remove_key("key0", &g_module);
	spdk_keyring_remove_key(":key0", &g_module);
	CU_ASSERT(!g_remove_called);

	/* Check that an error from module's add_key() results in failure */
	g_add_status = -EIO;
	opts.size = SPDK_SIZEOF(&opts, ctx);
	opts.name = "key0";
	opts.module = &g_module;
	opts.ctx = &uopts;
	rc = spdk_keyring_add_key(&opts);
	CU_ASSERT_EQUAL(rc, -EIO);
	CU_ASSERT_PTR_NULL(spdk_keyring_get_key("key0"));
	g_add_status = 0;
}

static void
test_keyring_get_put(void)
{
	struct spdk_key_opts opts = {};
	struct spdk_key *key, *tmp;
	char keybuf[UT_KEY_SIZE] = {};
	struct ut_key_opts uopts = { .buf = keybuf, .len = UT_KEY_SIZE };
	int rc, i;

	opts.size = SPDK_SIZEOF(&opts, ctx);
	opts.name = "key0";
	opts.module = &g_module;
	opts.ctx = &uopts;
	rc = spdk_keyring_add_key(&opts);
	CU_ASSERT_EQUAL(rc, 0);

	/* Get multiple references to the same key */
	key = spdk_keyring_get_key("key0");
	CU_ASSERT_PTR_NOT_NULL(key);
#define UT_KEY_REFS 8
	for (i = 0; i < UT_KEY_REFS; ++i) {
		tmp = spdk_keyring_get_key("key0");
		CU_ASSERT_PTR_EQUAL(key, tmp);
	}

	/* Remove the key and verify (relying on the address sanitizer to catch any use-after-free
	 * errors) that the reference is still valid
	 */
	spdk_keyring_remove_key("key0", &g_module);
	CU_ASSERT_EQUAL(strcmp(spdk_key_get_name(key), "key0"), 0);

	/* Release all but one reference and verify that it's still valid (again, relying on the
	 * address sanitizer)
	 */
	for (i = 0; i < UT_KEY_REFS; ++i) {
		spdk_keyring_put_key(key);
		CU_ASSERT_EQUAL(strcmp(spdk_key_get_name(key), "key0"), 0);
	}

	/* Release the last reference - this should also free the key */
	spdk_keyring_put_key(key);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite;
	unsigned int num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("keyring", NULL, NULL);
	CU_ADD_TEST(suite, test_keyring_add_remove);
	CU_ADD_TEST(suite, test_keyring_get_put);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	return num_failures;
}
