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

#include "spdk/likely.h"

#include "ftl_writer.h"
#include "ftl_band.h"

void ftl_writer_init(struct spdk_ftl_dev *dev, struct ftl_writer *writer,
		     uint64_t limit, enum ftl_band_type type)
{
	memset(writer, 0, sizeof(*writer));
	writer->dev = dev;
	TAILQ_INIT(&writer->rq_queue);
	TAILQ_INIT(&writer->full_bands);
	writer->limit = limit;
	writer->halt = true;
	writer->writer_type = type;
}

static bool
can_write(struct ftl_writer *writer)
{
	if (spdk_unlikely(writer->halt)) {
		return false;
	}

	return writer->band->md->state == FTL_BAND_STATE_OPEN;
}

void
ftl_writer_band_state_change(struct ftl_band *band)
{
	struct ftl_writer *writer = band->owner.priv;

	switch (band->md->state) {
	case FTL_BAND_STATE_FULL:
		assert(writer->band == band);
		TAILQ_INSERT_TAIL(&writer->full_bands, band, queue_entry);
		writer->band = NULL;
		break;

	case FTL_BAND_STATE_CLOSED:
		assert(writer->band_num > 0);
		writer->band_num--;
		ftl_band_clear_owner(band, ftl_writer_band_state_change, writer);
		break;

	default:
		break;
	}
}

static void
close_full_bands(struct ftl_writer *writer)
{
	struct ftl_band *band, *next;

	TAILQ_FOREACH_SAFE(band, &writer->full_bands, queue_entry, next) {
		if (band->queue_depth) {
			continue;
		}

		TAILQ_REMOVE(&writer->full_bands, band, queue_entry);
		ftl_band_close(band);
	}
}

static bool
is_active(struct ftl_writer *writer)
{
	if (writer->dev->limit < writer->limit) {
		return false;
	}

	return true;
}

static struct ftl_band *
get_band(struct ftl_writer *writer)
{
	if (spdk_unlikely(!writer->band)) {
		if (!is_active(writer)) {
			return NULL;
		}

		if (spdk_unlikely(NULL != writer->next_band)) {
			if (FTL_BAND_STATE_OPEN == writer->next_band->md->state) {
				writer->band = writer->next_band;
				writer->next_band = NULL;

				return writer->band;
			} else {
				assert(FTL_BAND_STATE_OPEN == writer->next_band->md->state);
				ftl_abort();
			}
		}

		writer->band = ftl_band_get_next_free(writer->dev);
		if (writer->band) {
			writer->band_num++;
			ftl_band_set_owner(writer->band,
					   ftl_writer_band_state_change, writer);

			if (ftl_band_write_prep(writer->band)) {
				/*
				 * This error might happen due to allocation failure. However number
				 * of open bands is controlled and it should have enough resources
				 * to do it. So here is better to perform a crash and recover from
				 * shared memory to bring back stable state.
				 *  */
				ftl_abort();
			}
		} else {
			return NULL;
		}
	}

	if (spdk_likely(writer->band->md->state == FTL_BAND_STATE_OPEN)) {
		return writer->band;
	} else {
		if (spdk_unlikely(writer->band->md->state == FTL_BAND_STATE_PREP)) {
			struct ftl_zone *zone = CIRCLEQ_FIRST(&writer->band->zones);
			if (ftl_zone_is_writable(writer->dev, zone)) {
				ftl_band_open(writer->band, writer->writer_type);
			}
		}
		return NULL;
	}
}

void ftl_writer_run(struct ftl_writer *writer)
{
	struct ftl_band *band;

	if (spdk_unlikely(!TAILQ_EMPTY(&writer->full_bands))) {
		close_full_bands(writer);
	}

	if (!TAILQ_EMPTY(&writer->rq_queue)) {
		band = get_band(writer);
		if (spdk_unlikely(!band)) {
			return;
		}

		if (!can_write(writer)) {
			return;
		}

		/* Finally we can write to band */
		struct ftl_rq *rq = TAILQ_FIRST(&writer->rq_queue);
		TAILQ_REMOVE(&writer->rq_queue, rq, qentry);
		ftl_band_rq_write(writer->band, rq);
	}
}

bool
ftl_writer_is_halted(struct ftl_writer *writer)
{
	if (spdk_unlikely(!TAILQ_EMPTY(&writer->full_bands))) {
		return false;
	}

	if (writer->band) {
		if (writer->band->md->state != FTL_BAND_STATE_OPEN) {
			return false;
		}

		if (writer->band->queue_depth) {
			return false;
		}
	}

	return writer->halt;
}

uint64_t ftl_writer_free_block(struct ftl_writer *writer)
{
	uint64_t free_blocks = 0;

	if (writer->band) {
		free_blocks += ftl_band_user_blocks_left(writer->band,
				writer->band->md->iter.offset);
	}

	if (writer->next_band) {
		free_blocks += ftl_band_user_blocks_left(writer->next_band,
				writer->next_band->md->iter.offset);
	}

	return free_blocks;
}
