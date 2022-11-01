/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/ftl.h"
#include "ftl/ftl_core.h"

#include "ftl/mngt/ftl_mngt_bdev.c"

struct base_bdev_geometry {
	size_t write_unit_size;
	size_t zone_size;
	size_t optimal_open_zones;
	size_t blockcnt;
};

extern struct base_bdev_geometry g_geo;

struct spdk_ftl_dev *test_init_ftl_dev(const struct base_bdev_geometry *geo);
struct ftl_band *test_init_ftl_band(struct spdk_ftl_dev *dev, size_t id, size_t zone_size);
void test_free_ftl_dev(struct spdk_ftl_dev *dev);
void test_free_ftl_band(struct ftl_band *band);
uint64_t test_offset_from_addr(ftl_addr addr, struct ftl_band *band);

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
void *
ftl_mempool_get(struct ftl_mempool *mpool)
{
	return spdk_mempool_get((struct spdk_mempool *)mpool);
}

void
ftl_mempool_put(struct ftl_mempool *mpool, void *element)
{
	spdk_mempool_put((struct spdk_mempool *)mpool, element);
}

ftl_df_obj_id
ftl_mempool_get_df_obj_id(struct ftl_mempool *mpool, void *df_obj_ptr)
{
	return (ftl_df_obj_id)df_obj_ptr;
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

	dev->layout.base.total_blocks = UINT64_MAX;

	for (size_t i = 0; i < dev->num_bands; i++) {
		struct ftl_band_md *md;
		int ret;

		ret = posix_memalign((void **)&md, FTL_BLOCK_SIZE, sizeof(*md));
		SPDK_CU_ASSERT_FATAL(ret == 0);
		memset(md, 0, sizeof(*md));
		dev->bands[i].md = md;
	}

	dev->p2l_pool = (struct ftl_mempool *)spdk_mempool_create("ftl_ut", 2, 0x210200,
			SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
			SPDK_ENV_SOCKET_ID_ANY);
	SPDK_CU_ASSERT_FATAL(dev->p2l_pool != NULL);

	TAILQ_INIT(&dev->free_bands);
	TAILQ_INIT(&dev->shut_bands);

	/* Cache frequently used values */
	dev->num_blocks_in_band = ftl_calculate_num_blocks_in_band(dev->base_bdev_desc);
	dev->is_zoned = spdk_bdev_is_zoned(spdk_bdev_desc_get_bdev(dev->base_bdev_desc));

	return dev;
}

struct ftl_band *
test_init_ftl_band(struct spdk_ftl_dev *dev, size_t id, size_t zone_size)
{
	struct ftl_band *band;

	SPDK_CU_ASSERT_FATAL(dev != NULL);
	SPDK_CU_ASSERT_FATAL(id < dev->num_bands);

	band = &dev->bands[id];
	band->dev = dev;
	band->id = id;

	band->md->state = FTL_BAND_STATE_CLOSED;
	band->md->df_p2l_map = FTL_DF_OBJ_ID_INVALID;
	TAILQ_INSERT_HEAD(&dev->shut_bands, band, queue_entry);

	band->p2l_map.valid = (struct ftl_bitmap *)spdk_bit_array_create(ftl_get_num_blocks_in_band(dev));
	SPDK_CU_ASSERT_FATAL(band->p2l_map.valid != NULL);

	band->start_addr = zone_size * id;

	return band;
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
	spdk_mempool_free((struct spdk_mempool *)dev->p2l_pool);
	for (size_t i = 0; i < dev->num_bands; i++) {
		free(dev->bands[i].md);
	}
	free(dev->bands);
	free(dev);
}

void
test_free_ftl_band(struct ftl_band *band)
{
	SPDK_CU_ASSERT_FATAL(band != NULL);
	spdk_bit_array_free((struct spdk_bit_array **)&band->p2l_map.valid);
	spdk_dma_free(band->p2l_map.band_dma_md);

	band->p2l_map.band_dma_md = NULL;
}

uint64_t
test_offset_from_addr(ftl_addr addr, struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;

	CU_ASSERT_EQUAL(ftl_addr_get_band(dev, addr), band->id);

	return addr - band->id * ftl_get_num_blocks_in_band(dev);
}
