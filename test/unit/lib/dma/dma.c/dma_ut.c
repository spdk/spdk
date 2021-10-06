/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
 *     * Neither the name of Nvidia Corporation nor the names of its
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
#include "common/lib/test_env.c"
#include "unit/lib/json_mock.c"
#include "dma/dma.c"

static bool g_memory_domain_pull_called;
static bool g_memory_domain_push_called;
static bool g_memory_domain_translate_called;
static int g_memory_domain_cb_rc = 123;

static void
test_memory_domain_data_cpl_cb(void *ctx, int rc)
{
}

static int test_memory_domain_pull_data_cb(struct spdk_memory_domain *src_device,
		void *src_device_ctx, struct iovec *src_iov, uint32_t src_iovcnt, struct iovec *dst_iov,
		uint32_t dst_iovcnt, spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg)
{
	g_memory_domain_pull_called = true;

	return g_memory_domain_cb_rc;
}

static int test_memory_domain_push_data_cb(struct spdk_memory_domain *dst_domain,
		void *dst_domain_ctx,
		struct iovec *dst_iov, uint32_t dst_iovcnt, struct iovec *src_iov, uint32_t src_iovcnt,
		spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg)
{
	g_memory_domain_push_called = true;

	return g_memory_domain_cb_rc;
}

static int
test_memory_domain_translate_memory_cb(struct spdk_memory_domain *src_device, void *src_device_ctx,
				       struct spdk_memory_domain *dst_device, struct spdk_memory_domain_translation_ctx *dst_device_ctx,
				       void *addr, size_t len, struct spdk_memory_domain_translation_result *result)
{
	g_memory_domain_translate_called = true;

	return g_memory_domain_cb_rc;
}

static void
test_dma(void)
{
	void *test_ibv_pd = (void *)0xdeadbeaf;
	struct iovec src_iov = {}, dst_iov = {};
	struct spdk_memory_domain *domain = NULL, *domain_2 = NULL, *domain_3 = NULL;
	struct spdk_memory_domain_rdma_ctx rdma_ctx = { .ibv_pd = test_ibv_pd };
	struct spdk_memory_domain_ctx memory_domain_ctx = { .user_ctx = &rdma_ctx };
	struct spdk_memory_domain_ctx *stored_memory_domain_ctx;
	struct spdk_memory_domain_translation_result translation_result;
	const char *id;
	int rc;

	/* Create memory domain. No device ptr, expect fail */
	rc = spdk_memory_domain_create(NULL, SPDK_DMA_DEVICE_TYPE_RDMA, &memory_domain_ctx, "test");
	CU_ASSERT(rc != 0);

	/* Create memory domain. ctx with zero size, expect fail */
	memory_domain_ctx.size = 0;
	rc = spdk_memory_domain_create(&domain, SPDK_DMA_DEVICE_TYPE_RDMA, &memory_domain_ctx, "test");
	CU_ASSERT(rc != 0);

	/* Create memory domain. expect pass */
	memory_domain_ctx.size = sizeof(memory_domain_ctx);
	rc = spdk_memory_domain_create(&domain, SPDK_DMA_DEVICE_TYPE_RDMA, &memory_domain_ctx, "test");
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(domain != NULL);

	/* Get context. Expect pass */
	stored_memory_domain_ctx = spdk_memory_domain_get_context(domain);
	SPDK_CU_ASSERT_FATAL(stored_memory_domain_ctx != NULL);
	CU_ASSERT(stored_memory_domain_ctx->user_ctx == &rdma_ctx);
	CU_ASSERT(((struct spdk_memory_domain_rdma_ctx *)stored_memory_domain_ctx->user_ctx)->ibv_pd ==
		  rdma_ctx.ibv_pd);

	/* Get DMA device type. Expect pass */
	CU_ASSERT(spdk_memory_domain_get_dma_device_type(domain) == SPDK_DMA_DEVICE_TYPE_RDMA);

	/* Get DMA id. Expect pass */
	id = spdk_memory_domain_get_dma_device_id(domain);
	CU_ASSERT((!strcmp(id, domain->id)));

	/* pull data, callback is NULL. Expect fail */
	g_memory_domain_pull_called = false;
	rc = spdk_memory_domain_pull_data(domain, NULL, &src_iov, 1, &dst_iov, 1,
					  test_memory_domain_data_cpl_cb, NULL);
	CU_ASSERT(rc == -ENOTSUP);
	CU_ASSERT(g_memory_domain_pull_called == false);

	/* Set pull callback */
	spdk_memory_domain_set_pull(domain, test_memory_domain_pull_data_cb);

	/* pull data. Expect pass */
	rc = spdk_memory_domain_pull_data(domain, NULL, &src_iov, 1, &dst_iov, 1,
					  test_memory_domain_data_cpl_cb, NULL);
	CU_ASSERT(rc == g_memory_domain_cb_rc);
	CU_ASSERT(g_memory_domain_pull_called == true);

	/* push data, callback is NULL. Expect fail */
	g_memory_domain_push_called = false;
	rc = spdk_memory_domain_push_data(domain, NULL, &dst_iov, 1, &src_iov, 1,
					  test_memory_domain_data_cpl_cb, NULL);
	CU_ASSERT(rc == -ENOTSUP);
	CU_ASSERT(g_memory_domain_push_called == false);

	/* Set push callback */
	spdk_memory_domain_set_push(domain, test_memory_domain_push_data_cb);

	/* push data. Expect pass */
	rc = spdk_memory_domain_push_data(domain, NULL, &dst_iov, 1, &src_iov, 1,
					  test_memory_domain_data_cpl_cb, NULL);
	CU_ASSERT(rc == g_memory_domain_cb_rc);
	CU_ASSERT(g_memory_domain_push_called == true);

	/* Translate data, callback is NULL. Expect fail */
	g_memory_domain_translate_called = false;
	rc = spdk_memory_domain_translate_data(domain, NULL, domain, NULL, (void *)0xfeeddbeef, 0x1000,
					       &translation_result);
	CU_ASSERT(rc == -ENOTSUP);
	CU_ASSERT(g_memory_domain_translate_called == false);

	/* Set translate callback */
	spdk_memory_domain_set_translation(domain, test_memory_domain_translate_memory_cb);

	/* Translate data. Expect pass */
	g_memory_domain_translate_called = false;
	rc = spdk_memory_domain_translate_data(domain, NULL, domain, NULL, (void *)0xfeeddbeef, 0x1000,
					       &translation_result);
	CU_ASSERT(rc == g_memory_domain_cb_rc);
	CU_ASSERT(g_memory_domain_translate_called == true);

	/* Set translation callback to NULL. Expect pass */
	spdk_memory_domain_set_translation(domain, NULL);
	CU_ASSERT(domain->translate_cb == NULL);

	/* Set translation callback. Expect pass */
	spdk_memory_domain_set_translation(domain, test_memory_domain_translate_memory_cb);
	CU_ASSERT(domain->translate_cb == test_memory_domain_translate_memory_cb);

	/* Set pull callback to NULL. Expect pass */
	spdk_memory_domain_set_pull(domain, NULL);
	CU_ASSERT(domain->pull_cb == NULL);

	/* Set translation_callback. Expect pass */
	spdk_memory_domain_set_pull(domain, test_memory_domain_pull_data_cb);
	CU_ASSERT(domain->pull_cb == test_memory_domain_pull_data_cb);

	/* Create 2nd and 3rd memory domains with equal id to test enumeration */
	rc = spdk_memory_domain_create(&domain_2, SPDK_DMA_DEVICE_TYPE_RDMA, &memory_domain_ctx, "test_2");
	CU_ASSERT(rc == 0);

	rc = spdk_memory_domain_create(&domain_3, SPDK_DMA_DEVICE_TYPE_RDMA, &memory_domain_ctx, "test_2");
	CU_ASSERT(rc == 0);

	CU_ASSERT(spdk_memory_domain_get_first("test") == domain);
	CU_ASSERT(spdk_memory_domain_get_next(domain, "test") == NULL);
	CU_ASSERT(spdk_memory_domain_get_first("test_2") == domain_2);
	CU_ASSERT(spdk_memory_domain_get_next(domain_2, "test_2") == domain_3);
	CU_ASSERT(spdk_memory_domain_get_next(domain_3, "test_2") == NULL);

	CU_ASSERT(spdk_memory_domain_get_first(NULL) == domain);
	CU_ASSERT(spdk_memory_domain_get_next(domain, NULL) == domain_2);
	CU_ASSERT(spdk_memory_domain_get_next(domain_2, NULL) == domain_3);
	CU_ASSERT(spdk_memory_domain_get_next(domain_3, NULL) == NULL);

	/* Remove 2nd device, repeat iteration */
	spdk_memory_domain_destroy(domain_2);
	CU_ASSERT(spdk_memory_domain_get_first(NULL) == domain);
	CU_ASSERT(spdk_memory_domain_get_next(domain, NULL) == domain_3);
	CU_ASSERT(spdk_memory_domain_get_next(domain_3, NULL) == NULL);

	/* Remove 3rd device, repeat iteration */
	spdk_memory_domain_destroy(domain_3);
	CU_ASSERT(spdk_memory_domain_get_first(NULL) == domain);
	CU_ASSERT(spdk_memory_domain_get_next(domain, NULL) == NULL);
	CU_ASSERT(spdk_memory_domain_get_first("test_2") == NULL);

	/* Destroy memory domain, domain == NULL */
	spdk_memory_domain_destroy(NULL);
	CU_ASSERT(spdk_memory_domain_get_first(NULL) == domain);

	/* Destroy memory domain */
	spdk_memory_domain_destroy(domain);
	CU_ASSERT(spdk_memory_domain_get_first(NULL) == NULL);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("dma_suite", NULL, NULL);
	CU_ADD_TEST(suite, test_dma);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
