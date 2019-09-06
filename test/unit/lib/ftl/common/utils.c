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

#include "spdk_internal/thread.h"

#include "spdk/ftl.h"
#include "ftl/ftl_core.h"

struct spdk_ftl_dev *test_init_ftl_dev(const struct spdk_ocssd_geometry_data *geo,
				       const struct spdk_ftl_punit_range *range);
struct ftl_band *test_init_ftl_band(struct spdk_ftl_dev *dev, size_t id);
void test_free_ftl_dev(struct spdk_ftl_dev *dev);
void test_free_ftl_band(struct ftl_band *band);
uint64_t test_offset_from_addr(struct ftl_addr addr, struct ftl_band *band);

struct spdk_ftl_dev *
test_init_ftl_dev(const struct spdk_ocssd_geometry_data *geo,
		  const struct spdk_ftl_punit_range *range)
{
	struct spdk_ftl_dev *dev;
	unsigned int punit;

	dev = calloc(1, sizeof(*dev));
	SPDK_CU_ASSERT_FATAL(dev != NULL);

	dev->xfer_size = geo->ws_opt;
	dev->geo = *geo;
	dev->range = *range;
	dev->core_thread.thread = spdk_thread_create("unit_test_thread", NULL);
	spdk_set_thread(dev->core_thread.thread);

	dev->bands = calloc(geo->num_chk, sizeof(*dev->bands));
	SPDK_CU_ASSERT_FATAL(dev->bands != NULL);

	dev->punits = calloc(ftl_dev_num_punits(dev), sizeof(*dev->punits));
	SPDK_CU_ASSERT_FATAL(dev->punits != NULL);

	dev->lba_pool = spdk_mempool_create("ftl_ut", 2, 0x18000,
					    SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
					    SPDK_ENV_SOCKET_ID_ANY);
	SPDK_CU_ASSERT_FATAL(dev->lba_pool != NULL);

	for (size_t i = 0; i < ftl_dev_num_punits(dev); ++i) {
		punit = range->begin + i;
		dev->punits[i].dev = dev;
		dev->punits[i].start_addr.pu = punit;
	}

	LIST_INIT(&dev->free_bands);
	LIST_INIT(&dev->shut_bands);

	return dev;
}

struct ftl_band *
test_init_ftl_band(struct spdk_ftl_dev *dev, size_t id)
{
	struct ftl_band *band;
	struct ftl_zone *zone;

	SPDK_CU_ASSERT_FATAL(dev != NULL);
	SPDK_CU_ASSERT_FATAL(id < dev->geo.num_chk);

	band = &dev->bands[id];
	band->dev = dev;
	band->id = id;

	band->state = FTL_BAND_STATE_CLOSED;
	LIST_INSERT_HEAD(&dev->shut_bands, band, list_entry);
	CIRCLEQ_INIT(&band->zones);

	band->lba_map.vld = spdk_bit_array_create(ftl_num_band_lbks(dev));
	SPDK_CU_ASSERT_FATAL(band->lba_map.vld != NULL);

	band->zone_buf = calloc(ftl_dev_num_punits(dev), sizeof(*band->zone_buf));
	SPDK_CU_ASSERT_FATAL(band->zone_buf != NULL);

	band->reloc_bitmap = spdk_bit_array_create(ftl_dev_num_bands(dev));
	SPDK_CU_ASSERT_FATAL(band->reloc_bitmap != NULL);

	for (size_t i = 0; i < ftl_dev_num_punits(dev); ++i) {
		zone = &band->zone_buf[i];
		zone->pos = i;
		zone->state = SPDK_BDEV_ZONE_STATE_CLOSED;
		zone->punit = &dev->punits[i];
		zone->start_addr = dev->punits[i].start_addr;
		zone->start_addr.zone_id = band->id;
		CIRCLEQ_INSERT_TAIL(&band->zones, zone, circleq);
		band->num_zones++;
	}

	pthread_spin_init(&band->lba_map.lock, PTHREAD_PROCESS_PRIVATE);
	return band;
}

void
test_free_ftl_dev(struct spdk_ftl_dev *dev)
{
	SPDK_CU_ASSERT_FATAL(dev != NULL);
	spdk_set_thread(dev->core_thread.thread);
	spdk_thread_exit(dev->core_thread.thread);
	spdk_thread_destroy(dev->core_thread.thread);
	spdk_mempool_free(dev->lba_pool);
	free(dev->punits);
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
	unsigned int punit;

	/* TODO: ftl_addr_flatten_punit should return uint32_t */
	punit = ftl_addr_flatten_punit(dev, addr);
	CU_ASSERT_EQUAL(addr.zone_id, band->id);

	return punit * ftl_dev_lbks_in_zone(dev) + addr.offset;
}
