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
#include "thread/thread_internal.h"

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
uint64_t test_offset_from_addr(struct ftl_addr addr, struct ftl_band *band);

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

struct spdk_ftl_dev *
test_init_ftl_dev(const struct base_bdev_geometry *geo)
{
	struct spdk_ftl_dev *dev;

	dev = calloc(1, sizeof(*dev));
	SPDK_CU_ASSERT_FATAL(dev != NULL);

	dev->xfer_size = geo->write_unit_size;
	dev->core_thread = spdk_thread_create("unit_test_thread", NULL);
	spdk_set_thread(dev->core_thread);
	dev->ioch = calloc(1, sizeof(*dev->ioch)
			   + sizeof(struct ftl_io_channel *));
	dev->num_bands = geo->blockcnt / (geo->zone_size * geo->optimal_open_zones);
	dev->bands = calloc(dev->num_bands, sizeof(*dev->bands));
	SPDK_CU_ASSERT_FATAL(dev->bands != NULL);

	dev->lba_pool = spdk_mempool_create("ftl_ut", 2, 0x18000,
					    SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
					    SPDK_ENV_SOCKET_ID_ANY);
	SPDK_CU_ASSERT_FATAL(dev->lba_pool != NULL);

	LIST_INIT(&dev->free_bands);
	LIST_INIT(&dev->shut_bands);

	return dev;
}

struct ftl_band *
test_init_ftl_band(struct spdk_ftl_dev *dev, size_t id, size_t zone_size)
{
	struct ftl_band *band;
	struct ftl_zone *zone;

	SPDK_CU_ASSERT_FATAL(dev != NULL);
	SPDK_CU_ASSERT_FATAL(id < dev->num_bands);

	band = &dev->bands[id];
	band->dev = dev;
	band->id = id;

	band->state = FTL_BAND_STATE_CLOSED;
	LIST_INSERT_HEAD(&dev->shut_bands, band, list_entry);
	CIRCLEQ_INIT(&band->zones);

	band->lba_map.vld = spdk_bit_array_create(ftl_get_num_blocks_in_band(dev));
	SPDK_CU_ASSERT_FATAL(band->lba_map.vld != NULL);

	band->zone_buf = calloc(ftl_get_num_punits(dev), sizeof(*band->zone_buf));
	SPDK_CU_ASSERT_FATAL(band->zone_buf != NULL);

	band->reloc_bitmap = spdk_bit_array_create(ftl_get_num_bands(dev));
	SPDK_CU_ASSERT_FATAL(band->reloc_bitmap != NULL);

	for (size_t i = 0; i < ftl_get_num_punits(dev); ++i) {
		zone = &band->zone_buf[i];
		zone->info.state = SPDK_BDEV_ZONE_STATE_FULL;
		zone->info.zone_id = zone_size * (id * ftl_get_num_punits(dev) + i);
		CIRCLEQ_INSERT_TAIL(&band->zones, zone, circleq);
		band->num_zones++;
	}

	pthread_spin_init(&band->lba_map.lock, PTHREAD_PROCESS_PRIVATE);
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
	spdk_mempool_free(dev->lba_pool);
	free(dev->bands);
	free(dev);
}

void
test_free_ftl_band(struct ftl_band *band)
{
	SPDK_CU_ASSERT_FATAL(band != NULL);
	spdk_bit_array_free(&band->lba_map.vld);
	spdk_bit_array_free(&band->reloc_bitmap);
	free(band->zone_buf);
	spdk_dma_free(band->lba_map.dma_buf);
}

uint64_t
test_offset_from_addr(struct ftl_addr addr, struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;

	CU_ASSERT_EQUAL(ftl_addr_get_band(dev, addr), band->id);

	return addr.offset - band->id * ftl_get_num_blocks_in_band(dev);
}
