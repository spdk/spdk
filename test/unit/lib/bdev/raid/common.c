/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk_cunit.h"
#include "spdk/stdinc.h"
#include "bdev/raid/bdev_raid.h"

struct spdk_bdev_desc {
	struct spdk_bdev *bdev;
};

struct raid_params {
	uint8_t num_base_bdevs;
	uint64_t base_bdev_blockcnt;
	uint32_t base_bdev_blocklen;
	uint32_t strip_size;
	uint32_t md_len;
};

struct raid_params *g_params;
size_t g_params_count;
size_t g_params_size;

#define ARRAY_FOR_EACH(a, e) \
	for (e = a; e < a + SPDK_COUNTOF(a); e++)

#define RAID_PARAMS_FOR_EACH(p) \
	for (p = g_params; p < g_params + g_params_count; p++)

static int
raid_test_params_alloc(size_t count)
{
	assert(g_params == NULL);

	g_params_size = count;
	g_params_count = 0;
	g_params = calloc(count, sizeof(*g_params));

	return g_params ? 0 : -ENOMEM;
}

static void
raid_test_params_free(void)
{
	g_params_count = 0;
	g_params_size = 0;
	free(g_params);
}

static void
raid_test_params_add(struct raid_params *params)
{
	assert(g_params_count < g_params_size);

	memcpy(g_params + g_params_count, params, sizeof(*params));
	g_params_count++;
}

static struct raid_bdev *
raid_test_create_raid_bdev(struct raid_params *params, struct raid_bdev_module *module)
{
	struct raid_bdev *raid_bdev;
	struct raid_base_bdev_info *base_info;

	raid_bdev = calloc(1, sizeof(*raid_bdev));
	SPDK_CU_ASSERT_FATAL(raid_bdev != NULL);

	raid_bdev->module = module;
	raid_bdev->level = module->level;
	raid_bdev->num_base_bdevs = params->num_base_bdevs;

	switch (raid_bdev->module->base_bdevs_constraint.type) {
	case CONSTRAINT_MAX_BASE_BDEVS_REMOVED:
		raid_bdev->min_base_bdevs_operational = raid_bdev->num_base_bdevs -
							raid_bdev->module->base_bdevs_constraint.value;
		break;
	case CONSTRAINT_MIN_BASE_BDEVS_OPERATIONAL:
		raid_bdev->min_base_bdevs_operational = raid_bdev->module->base_bdevs_constraint.value;
		break;
	case CONSTRAINT_UNSET:
		raid_bdev->min_base_bdevs_operational = raid_bdev->num_base_bdevs;
		break;
	default:
		CU_FAIL_FATAL("unsupported raid constraint type");
	};

	raid_bdev->base_bdev_info = calloc(raid_bdev->num_base_bdevs,
					   sizeof(struct raid_base_bdev_info));
	SPDK_CU_ASSERT_FATAL(raid_bdev->base_bdev_info != NULL);

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		struct spdk_bdev *bdev;
		struct spdk_bdev_desc *desc;

		bdev = calloc(1, sizeof(*bdev));
		SPDK_CU_ASSERT_FATAL(bdev != NULL);
		bdev->blockcnt = params->base_bdev_blockcnt;
		bdev->blocklen = params->base_bdev_blocklen;

		desc = calloc(1, sizeof(*desc));
		SPDK_CU_ASSERT_FATAL(desc != NULL);
		desc->bdev = bdev;

		base_info->bdev = bdev;
		base_info->desc = desc;
	}

	raid_bdev->strip_size = params->strip_size;
	raid_bdev->strip_size_kb = params->strip_size * params->base_bdev_blocklen / 1024;
	raid_bdev->strip_size_shift = spdk_u32log2(raid_bdev->strip_size);
	raid_bdev->blocklen_shift = spdk_u32log2(params->base_bdev_blocklen);
	raid_bdev->bdev.blocklen = params->base_bdev_blocklen;
	raid_bdev->bdev.md_len = params->md_len;

	return raid_bdev;
}

static void
raid_test_delete_raid_bdev(struct raid_bdev *raid_bdev)
{
	struct raid_base_bdev_info *base_info;

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		free(base_info->bdev);
		free(base_info->desc);
	}
	free(raid_bdev->base_bdev_info);
	free(raid_bdev);
}
