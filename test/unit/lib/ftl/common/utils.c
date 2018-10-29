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

#include <spdk/ftl.h>

static struct spdk_ftl_dev *
test_init_ftl_dev(const struct spdk_ocssd_geometry_data *geo,
		  const struct spdk_ftl_punit_range *range)
{
	struct spdk_ftl_dev *dev;
	unsigned int punit;

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		return NULL;
	}

	dev->xfer_size = geo->ws_opt;
	dev->geo = *geo;
	dev->range = *range;

	dev->bands = calloc(geo->num_chk, sizeof(*dev->bands));
	if (!dev->bands) {
		goto free_dev;
	}

	dev->punits = calloc(ftl_dev_num_punits(dev), sizeof(*dev->punits));
	if (!dev->punits) {
		goto free_bands;
	}

	for (size_t i = 0; i < ftl_dev_num_punits(dev); ++i) {
		punit = range->begin + i;
		dev->punits[i].dev = dev;
		dev->punits[i].start_ppa.grp = punit % geo->num_grp;
		dev->punits[i].start_ppa.pu = punit / geo->num_grp;
	}

	return dev;
free_bands:
	free(dev->bands);
free_dev:
	free(dev);
	return NULL;
}

static struct ftl_band *
test_init_ftl_band(struct spdk_ftl_dev *dev, size_t id)
{
	struct ftl_band *band;
	struct ftl_chunk *chunk;

	if (!dev) {
		return NULL;
	}

	if (id >= dev->geo.num_chk) {
		return NULL;
	}

	band = &dev->bands[id];
	band->dev = dev;
	band->id = id;
	CIRCLEQ_INIT(&band->chunks);

	band->md.vld_map = spdk_bit_array_create(ftl_num_band_lbks(dev));
	if (!band->md.vld_map) {
		goto error;
	}

	band->chunk_buf = calloc(ftl_dev_num_punits(dev), sizeof(*band->chunk_buf));
	if (!band->chunk_buf) {
		goto error;
	}

	for (size_t i = 0; i < ftl_dev_num_punits(dev); ++i) {
		chunk = &band->chunk_buf[i];
		chunk->pos = i;
		chunk->state = FTL_CHUNK_STATE_CLOSED;
		chunk->punit = &dev->punits[i];
		chunk->start_ppa = dev->punits[i].start_ppa;
		chunk->start_ppa.chk = band->id;
		CIRCLEQ_INSERT_TAIL(&band->chunks, chunk, circleq);
		band->num_chunks++;
	}

	pthread_spin_init(&band->md.lock, PTHREAD_PROCESS_PRIVATE);
	return band;
error:
	spdk_bit_array_free(&band->md.vld_map);
	free(band->chunk_buf);
	return NULL;
}

static void
test_free_ftl_dev(struct spdk_ftl_dev *dev)
{
	if (!dev) {
		return;
	}

	free(dev->punits);
	free(dev->bands);
	free(dev);
}

static void
test_free_ftl_band(struct ftl_band *band)
{
	if (!band) {
		return;
	}

	spdk_bit_array_free(&band->md.vld_map);
	free(band->chunk_buf);
	free(band->md.lba_map);
}
