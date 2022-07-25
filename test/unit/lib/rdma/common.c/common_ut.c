/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_cunit.h"
#include "spdk_internal/mock.h"
#include "common/lib/test_env.c"
#include "rdma/common.c"

DEFINE_STUB(spdk_mem_map_alloc, struct spdk_mem_map *, (uint64_t default_translation,
		const struct spdk_mem_map_ops *ops, void *cb_ctx), NULL);
DEFINE_STUB_V(spdk_mem_map_free, (struct spdk_mem_map **pmap));
DEFINE_STUB(spdk_mem_map_set_translation, int, (struct spdk_mem_map *map, uint64_t vaddr,
		uint64_t size, uint64_t translation), 0);
DEFINE_STUB(spdk_mem_map_clear_translation, int, (struct spdk_mem_map *map, uint64_t vaddr,
		uint64_t size), 0);
DEFINE_STUB(spdk_mem_map_translate, uint64_t, (const struct spdk_mem_map *map, uint64_t vaddr,
		uint64_t *size), 0);

struct ut_rdma_device {
	struct ibv_context		*context;
	bool				removed;
	TAILQ_ENTRY(ut_rdma_device)	tailq;
};

static TAILQ_HEAD(, ut_rdma_device) g_ut_dev_list = TAILQ_HEAD_INITIALIZER(g_ut_dev_list);

struct ibv_context **
rdma_get_devices(int *num_devices)
{
	struct ibv_context **ctx_list;
	struct ut_rdma_device *ut_dev;
	int num_ut_devs = 0;
	int i = 0;

	TAILQ_FOREACH(ut_dev, &g_ut_dev_list, tailq) {
		if (!ut_dev->removed) {
			num_ut_devs++;
		}
	}

	ctx_list = malloc(sizeof(*ctx_list) * (num_ut_devs + 1));
	SPDK_CU_ASSERT_FATAL(ctx_list);

	TAILQ_FOREACH(ut_dev, &g_ut_dev_list, tailq) {
		if (!ut_dev->removed) {
			ctx_list[i++] = ut_dev->context;
		}
	}
	ctx_list[i] = NULL;

	if (num_devices) {
		*num_devices = num_ut_devs;
	}

	return ctx_list;
}

void
rdma_free_devices(struct ibv_context **list)
{
	free(list);
}

struct ibv_pd *
ibv_alloc_pd(struct ibv_context *context)
{
	struct ibv_pd *pd;
	struct ut_rdma_device *ut_dev;

	TAILQ_FOREACH(ut_dev, &g_ut_dev_list, tailq) {
		if (ut_dev->context == context && !ut_dev->removed) {
			break;
		}
	}

	if (!ut_dev) {
		return NULL;
	}

	pd = calloc(1, sizeof(*pd));
	SPDK_CU_ASSERT_FATAL(pd);

	pd->context = context;

	return pd;
}

int
ibv_dealloc_pd(struct ibv_pd *pd)
{
	free(pd);

	return 0;
}

static struct ut_rdma_device *
ut_rdma_add_dev(struct ibv_context *context)
{
	struct ut_rdma_device *ut_dev;

	ut_dev = calloc(1, sizeof(*ut_dev));
	if (!ut_dev) {
		return NULL;
	}

	ut_dev->context = context;
	TAILQ_INSERT_TAIL(&g_ut_dev_list, ut_dev, tailq);

	return ut_dev;
}

static void
ut_rdma_remove_dev(struct ut_rdma_device *ut_dev)
{
	TAILQ_REMOVE(&g_ut_dev_list, ut_dev, tailq);
	free(ut_dev);
}

static struct spdk_rdma_device *
_rdma_get_dev(struct ibv_context *context)
{
	struct spdk_rdma_device *dev;

	TAILQ_FOREACH(dev, &g_dev_list, tailq) {
		if (dev->context == context) {
			break;
		}
	}

	return dev;
}

static void
test_spdk_rdma_pd(void)
{
	struct ut_rdma_device *ut_dev0, *ut_dev1, *ut_dev2;
	struct ibv_pd *pd1, *pd1_1, *pd2;

	ut_dev0 = ut_rdma_add_dev((struct ibv_context *)0xface);
	SPDK_CU_ASSERT_FATAL(ut_dev0 != NULL);

	ut_dev1 = ut_rdma_add_dev((struct ibv_context *)0xc0ffee);
	SPDK_CU_ASSERT_FATAL(ut_dev1 != NULL);

	ut_dev2 = ut_rdma_add_dev((struct ibv_context *)0xf00d);
	SPDK_CU_ASSERT_FATAL(ut_dev2 != NULL);

	/* There are ut_dev0 and ut_dev1. */
	ut_dev2->removed = true;

	/* Call spdk_rdma_get_pd() to non-existent ut_dev2. */
	pd2 = spdk_rdma_get_pd(ut_dev2->context);

	/* Then, spdk_rdma_get_pd() should return NULL and g_dev_list should have dev0 and dev1. */
	CU_ASSERT(pd2 == NULL);
	CU_ASSERT(_rdma_get_dev(ut_dev0->context) != NULL);
	CU_ASSERT(_rdma_get_dev(ut_dev1->context) != NULL);
	CU_ASSERT(_rdma_get_dev(ut_dev2->context) == NULL);

	/* Remove ut_dev0 and add ut_dev2. */
	ut_dev0->removed = true;
	ut_dev2->removed = false;

	/* Call spdk_rdma_get_pd() to ut_dev1. */
	pd1 = spdk_rdma_get_pd(ut_dev1->context);

	/* Then, spdk_rdma_get_pd() should return pd1 and g_dev_list should have dev1 and dev2. */
	CU_ASSERT(pd1 != NULL);
	CU_ASSERT(_rdma_get_dev(ut_dev0->context) == NULL);
	CU_ASSERT(_rdma_get_dev(ut_dev1->context) != NULL);
	CU_ASSERT(_rdma_get_dev(ut_dev2->context) != NULL);

	/* Remove ut_dev1. */
	ut_dev1->removed = true;

	/* Call spdk_rdma_get_pd() again to ut_dev1 which does not exist anymore. */
	pd1_1 = spdk_rdma_get_pd(ut_dev1->context);

	/* Then, spdk_rdma_get_pd() should return NULL and g_dev_list should still have dev1. */
	CU_ASSERT(pd1_1 == NULL);
	CU_ASSERT(_rdma_get_dev(ut_dev0->context) == NULL);
	CU_ASSERT(_rdma_get_dev(ut_dev1->context) != NULL);
	CU_ASSERT(_rdma_get_dev(ut_dev2->context) != NULL);

	/* Call spdk_rdma_put_pd() to pd1. */
	spdk_rdma_put_pd(pd1);

	/* Then, dev1 should be removed from g_dev_list. */
	CU_ASSERT(_rdma_get_dev(ut_dev0->context) == NULL);
	CU_ASSERT(_rdma_get_dev(ut_dev1->context) == NULL);
	CU_ASSERT(_rdma_get_dev(ut_dev2->context) != NULL);

	/* Call spdk_rdma_get_pd() to ut_dev2. */
	pd2 = spdk_rdma_get_pd(ut_dev2->context);

	/* spdk_rdma_get_pd() should succeed and g_dev_list should still have dev2
	 * even after spdk_rdma_put_pd() is called to pd2.
	 */
	CU_ASSERT(pd2 != NULL);

	spdk_rdma_put_pd(pd2);

	CU_ASSERT(_rdma_get_dev(ut_dev0->context) == NULL);
	CU_ASSERT(_rdma_get_dev(ut_dev1->context) == NULL);
	CU_ASSERT(_rdma_get_dev(ut_dev2->context) != NULL);

	_rdma_fini();

	ut_rdma_remove_dev(ut_dev0);
	ut_rdma_remove_dev(ut_dev1);
	ut_rdma_remove_dev(ut_dev2);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("rdma_common", NULL, NULL);
	CU_ADD_TEST(suite, test_spdk_rdma_pd);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
