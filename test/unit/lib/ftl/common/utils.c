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

#include "spdk/ftl.h"
#include "ftl/ftl_core.h"

struct base_bdev_geometry {
	size_t write_unit_size;
	size_t zone_size;
	size_t optimal_open_zones;
	size_t blockcnt;
};

extern struct base_bdev_geometry g_geo;

struct spdk_ftl_dev *test_init_ftl_dev(const struct base_bdev_geometry *geo);
void test_free_ftl_dev(struct spdk_ftl_dev *dev);

DEFINE_STUB(spdk_bdev_desc_get_bdev, struct spdk_bdev *, (struct spdk_bdev_desc *desc), NULL);

uint64_t
spdk_bdev_get_zone_size(const struct spdk_bdev *bdev)
{
	return g_geo.zone_size;
}

uint32_t
spdk_bdev_get_optimal_open_zones(const struct spdk_bdev *bdev)
{
	return g_geo.optimal_open_zones;
}

DEFINE_RETURN_MOCK(ftl_mempool_get, void *);
void *ftl_mempool_get(struct ftl_mempool *mpool)
{
	return spdk_mempool_get((struct spdk_mempool *)mpool);
}

void ftl_mempool_put(struct ftl_mempool *mpool, void *element)
{
	spdk_mempool_put((struct spdk_mempool *)mpool, element);
}

struct spdk_ftl_dev *
test_init_ftl_dev(const struct base_bdev_geometry *geo)
{
	struct spdk_ftl_dev *dev;

	dev = calloc(1, sizeof(*dev));
	SPDK_CU_ASSERT_FATAL(dev != NULL);

	dev->xfer_size = geo->write_unit_size;
	dev->core_thread = spdk_thread_create("unit_test_thread", NULL);
	spdk_set_thread(dev->core_thread);
	dev->ioch = calloc(1, SPDK_IO_CHANNEL_STRUCT_SIZE
			   + sizeof(struct ftl_io_channel *));
	dev->num_bands = geo->blockcnt / (geo->zone_size * geo->optimal_open_zones);
	dev->bands = calloc(dev->num_bands, sizeof(*dev->bands));
	SPDK_CU_ASSERT_FATAL(dev->bands != NULL);

	dev->layout.btm.total_blocks = UINT64_MAX;

	for (size_t i = 0; i < dev->num_bands; i++) {
		struct ftl_band_md *md;
		int ret;

		ret = posix_memalign((void **)&md, FTL_BLOCK_SIZE, sizeof(*md));
		SPDK_CU_ASSERT_FATAL(ret == 0);
		memset(md, 0, sizeof(*md));
		dev->bands[i].md = md;
	}

	TAILQ_INIT(&dev->free_bands);
	TAILQ_INIT(&dev->shut_bands);

	/* Cache frequently used values */
	dev->num_blocks_in_band = ftl_calculate_num_blocks_in_band(dev->base_bdev_desc);
	dev->num_punits = ftl_calculate_num_punits(dev->base_bdev_desc);
	dev->num_blocks_in_zone = ftl_calculate_num_blocks_in_zone(dev->base_bdev_desc);
	dev->is_zoned = spdk_bdev_is_zoned(spdk_bdev_desc_get_bdev(dev->base_bdev_desc));

	return dev;
}

void
test_free_ftl_dev(struct spdk_ftl_dev *dev)
{
	struct spdk_thread *thread;

	SPDK_CU_ASSERT_FATAL(dev != NULL);
	free(dev->ioch);

	thread = dev->core_thread;

	spdk_set_thread(thread);
	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}
	spdk_thread_destroy(thread);
	for (size_t i = 0; i < dev->num_bands; i++) {
		free(dev->bands[i].md);
	}
	free(dev->bands);
	free(dev);
}
