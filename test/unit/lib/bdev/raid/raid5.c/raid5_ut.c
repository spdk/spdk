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
 *   A PARTICULAR PURPOSE AiRE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
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
#include "spdk/env.h"
#include "spdk_internal/mock.h"

#include "bdev/raid/raid5.c"

DEFINE_STUB_V(raid_bdev_module_list_add, (struct raid_bdev_module *raid_module));
DEFINE_STUB_V(raid_bdev_io_complete, (struct raid_bdev_io *raid_io,
				      enum spdk_bdev_io_status status));

struct raid5_params {
	uint8_t num_base_bdevs;
	uint64_t base_bdev_blockcnt;
	uint32_t base_bdev_blocklen;
	uint32_t strip_size;
};

static struct raid5_params *g_params;
static size_t g_params_count;

#define ARRAY_FOR_EACH(a, e) \
	for (e = a; e < a + SPDK_COUNTOF(a); e++)

#define RAID5_PARAMS_FOR_EACH(p) \
	for (p = g_params; p < g_params + g_params_count; p++)

static int
test_setup(void)
{
	uint8_t num_base_bdevs_values[] = { 3, 4, 5 };
	uint64_t base_bdev_blockcnt_values[] = { 1, 1024, 1024 * 1024 };
	uint32_t base_bdev_blocklen_values[] = { 512, 4096 };
	uint32_t strip_size_kb_values[] = { 1, 4, 128 };
	uint8_t *num_base_bdevs;
	uint64_t *base_bdev_blockcnt;
	uint32_t *base_bdev_blocklen;
	uint32_t *strip_size_kb;
	struct raid5_params *params;

	g_params_count = SPDK_COUNTOF(num_base_bdevs_values) *
			 SPDK_COUNTOF(base_bdev_blockcnt_values) *
			 SPDK_COUNTOF(base_bdev_blocklen_values) *
			 SPDK_COUNTOF(strip_size_kb_values);
	g_params = calloc(g_params_count, sizeof(*g_params));
	if (!g_params) {
		return -ENOMEM;
	}

	params = g_params;

	ARRAY_FOR_EACH(num_base_bdevs_values, num_base_bdevs) {
		ARRAY_FOR_EACH(base_bdev_blockcnt_values, base_bdev_blockcnt) {
			ARRAY_FOR_EACH(base_bdev_blocklen_values, base_bdev_blocklen) {
				ARRAY_FOR_EACH(strip_size_kb_values, strip_size_kb) {
					params->num_base_bdevs = *num_base_bdevs;
					params->base_bdev_blockcnt = *base_bdev_blockcnt;
					params->base_bdev_blocklen = *base_bdev_blocklen;
					params->strip_size = *strip_size_kb * 1024 / *base_bdev_blocklen;
					if (params->strip_size == 0 ||
					    params->strip_size > *base_bdev_blockcnt) {
						g_params_count--;
						continue;
					}
					params++;
				}
			}
		}
	}

	return 0;
}

static int
test_cleanup(void)
{
	free(g_params);
	return 0;
}

static struct raid_bdev *
create_raid_bdev(struct raid5_params *params)
{
	struct raid_bdev *raid_bdev;
	struct raid_base_bdev_info *base_info;

	raid_bdev = calloc(1, sizeof(*raid_bdev));
	SPDK_CU_ASSERT_FATAL(raid_bdev != NULL);

	raid_bdev->module = &g_raid5_module;
	raid_bdev->num_base_bdevs = params->num_base_bdevs;
	raid_bdev->base_bdev_info = calloc(raid_bdev->num_base_bdevs,
					   sizeof(struct raid_base_bdev_info));
	SPDK_CU_ASSERT_FATAL(raid_bdev->base_bdev_info != NULL);

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		base_info->bdev = calloc(1, sizeof(*base_info->bdev));
		SPDK_CU_ASSERT_FATAL(base_info->bdev != NULL);

		base_info->bdev->blockcnt = params->base_bdev_blockcnt;
		base_info->bdev->blocklen = params->base_bdev_blocklen;
	}

	raid_bdev->strip_size = params->strip_size;
	raid_bdev->strip_size_shift = spdk_u32log2(raid_bdev->strip_size);
	raid_bdev->bdev.blocklen = params->base_bdev_blocklen;

	return raid_bdev;
}

static void
delete_raid_bdev(struct raid_bdev *raid_bdev)
{
	struct raid_base_bdev_info *base_info;

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		free(base_info->bdev);
	}
	free(raid_bdev->base_bdev_info);
	free(raid_bdev);
}

static struct raid5_info *
create_raid5(struct raid5_params *params)
{
	struct raid_bdev *raid_bdev = create_raid_bdev(params);

	SPDK_CU_ASSERT_FATAL(raid5_start(raid_bdev) == 0);

	return raid_bdev->module_private;
}

static void
delete_raid5(struct raid5_info *r5info)
{
	struct raid_bdev *raid_bdev = r5info->raid_bdev;

	raid5_stop(raid_bdev);

	delete_raid_bdev(raid_bdev);
}

static void
test_raid5_start(void)
{
	struct raid5_params *params;

	RAID5_PARAMS_FOR_EACH(params) {
		struct raid5_info *r5info;

		r5info = create_raid5(params);

		CU_ASSERT_EQUAL(r5info->stripe_blocks, params->strip_size * (params->num_base_bdevs - 1));
		CU_ASSERT_EQUAL(r5info->total_stripes, params->base_bdev_blockcnt / params->strip_size);
		CU_ASSERT_EQUAL(r5info->raid_bdev->bdev.blockcnt,
				(params->base_bdev_blockcnt - params->base_bdev_blockcnt % params->strip_size) *
				(params->num_base_bdevs - 1));
		CU_ASSERT_EQUAL(r5info->raid_bdev->bdev.optimal_io_boundary, r5info->stripe_blocks);

		delete_raid5(r5info);
	}
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("raid5", test_setup, test_cleanup);
	CU_ADD_TEST(suite, test_raid5_start);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
