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

#ifndef FTL_BAND_H
#define FTL_BAND_H

#include "spdk/stdinc.h"
#include "spdk/bit_array.h"
#include "spdk/queue.h"

#include "ftl_ppa.h"

struct spdk_ftl_dev;
struct ftl_cb;

enum ftl_chunk_state {
	FTL_CHUNK_STATE_FREE,
	FTL_CHUNK_STATE_OPEN,
	FTL_CHUNK_STATE_CLOSED,
	FTL_CHUNK_STATE_BAD,
	FTL_CHUNK_STATE_VACANT,
};

struct ftl_chunk {
	/* Block state */
	enum ftl_chunk_state			state;

	/* Indicates that there is inflight write */
	bool					busy;

	/* First PPA */
	struct ftl_ppa				start_ppa;

	/* Pointer to parallel unit */
	struct ftl_punit			*punit;

	/* Position in band's chunk_buf */
	uint32_t				pos;

	CIRCLEQ_ENTRY(ftl_chunk)		circleq;
};

enum ftl_md_status {
	FTL_MD_SUCCESS,
	/* Metadata read failure */
	FTL_MD_IO_FAILURE,
	/* Invalid version */
	FTL_MD_INVALID_VER,
	/* UUID doesn't match */
	FTL_MD_NO_MD,
	/* UUID and version matches but CRC doesn't */
	FTL_MD_INVALID_CRC,
	/* Vld or lba map size doesn't match */
	FTL_MD_INVALID_SIZE
};

struct ftl_md {
	/* Sequence number */
	uint64_t				seq;

	/* Number of defrag cycles */
	uint64_t				wr_cnt;

	/* LBA/vld map lock */
	pthread_spinlock_t			lock;

	/* Number of valid LBAs */
	size_t					num_vld;

	/* LBA map's reference count */
	size_t					ref_cnt;

	/* Bitmap of valid LBAs */
	struct spdk_bit_array			*vld_map;

	/* LBA map (only valid for open/relocating bands) */
	uint64_t				*lba_map;

	/* Metadata DMA buffer (only valid for open/relocating bands) */
	void					*dma_buf;
};

enum ftl_band_state {
	FTL_BAND_STATE_FREE,
	FTL_BAND_STATE_PREP,
	FTL_BAND_STATE_OPENING,
	FTL_BAND_STATE_OPEN,
	FTL_BAND_STATE_FULL,
	FTL_BAND_STATE_CLOSING,
	FTL_BAND_STATE_CLOSED,
	FTL_BAND_STATE_MAX
};

struct ftl_band {
	/* Device this band belongs to */
	struct spdk_ftl_dev			*dev;

	/* Number of operational chunks */
	size_t					num_chunks;

	/* Array of chunks */
	struct ftl_chunk			*chunk_buf;

	/* List of operational chunks */
	CIRCLEQ_HEAD(, ftl_chunk)		chunks;

	/* Band's metadata */
	struct ftl_md				md;

	/* Band's state */
	enum ftl_band_state			state;

	/* Band's index */
	unsigned int				id;

	/* Latest merit calculation */
	double					merit;

	/* High defrag priority - means that the metadata should be copied and */
	/* the band should be defragged immediately */
	int					high_prio;

	/* End metadata start ppa */
	struct ftl_ppa				tail_md_ppa;

	/* Free/shut bands' lists */
	LIST_ENTRY(ftl_band)			list_entry;

	/* High priority queue link */
	STAILQ_ENTRY(ftl_band)			prio_stailq;
};

uint64_t	ftl_band_lbkoff_from_ppa(struct ftl_band *band, struct ftl_ppa ppa);
struct ftl_ppa ftl_band_ppa_from_lbkoff(struct ftl_band *band, uint64_t lbkoff);
void		ftl_band_set_state(struct ftl_band *band, enum ftl_band_state state);
size_t		ftl_band_age(const struct ftl_band *band);
void		ftl_band_acquire_md(struct ftl_band *band);
int		ftl_band_alloc_md(struct ftl_band *band);
void		ftl_band_release_md(struct ftl_band *band);
struct ftl_ppa ftl_band_next_xfer_ppa(struct ftl_band *band, struct ftl_ppa ppa,
				      size_t num_lbks);
struct ftl_ppa ftl_band_next_ppa(struct ftl_band *band, struct ftl_ppa ppa,
				 size_t offset);
size_t		ftl_band_num_usable_lbks(const struct ftl_band *band);
size_t		ftl_band_user_lbks(const struct ftl_band *band);
void		ftl_band_set_addr(struct ftl_band *band, uint64_t lba,
				  struct ftl_ppa ppa);
struct ftl_band *ftl_band_from_ppa(struct spdk_ftl_dev *dev, struct ftl_ppa ppa);
struct ftl_chunk *ftl_band_chunk_from_ppa(struct ftl_band *band, struct ftl_ppa);
void		ftl_band_md_clear(struct ftl_md *md);
int		ftl_band_read_tail_md(struct ftl_band *band, struct ftl_md *md,
				      void *data, struct ftl_ppa,
				      const struct ftl_cb *cb);
int		ftl_band_read_head_md(struct ftl_band *band, struct ftl_md *md,
				      void *data, const struct ftl_cb *cb);
int		ftl_band_read_lba_map(struct ftl_band *band, struct ftl_md *md,
				      void *data, const struct ftl_cb *cb);
int		ftl_band_write_tail_md(struct ftl_band *band, void *data, spdk_ftl_fn cb);
int		ftl_band_write_head_md(struct ftl_band *band, void *data, spdk_ftl_fn cb);
struct ftl_ppa ftl_band_tail_md_ppa(struct ftl_band *band);
struct ftl_ppa ftl_band_head_md_ppa(struct ftl_band *band);
void		ftl_band_write_failed(struct ftl_band *band);
void		ftl_band_clear_md(struct ftl_band *band);
int		ftl_band_full(struct ftl_band *band, size_t offset);
int		ftl_band_erase(struct ftl_band *band);
int		ftl_band_write_prep(struct ftl_band *band);
struct ftl_chunk *ftl_band_next_operational_chunk(struct ftl_band *band,
		struct ftl_chunk *chunk);

static inline int
ftl_band_empty(const struct ftl_band *band)
{
	return band->md.num_vld == 0;
}

static inline struct ftl_chunk *
ftl_band_next_chunk(struct ftl_band *band, struct ftl_chunk *chunk)
{
	assert(chunk->state != FTL_CHUNK_STATE_BAD);
	return CIRCLEQ_LOOP_NEXT(&band->chunks, chunk, circleq);
}

static inline void
ftl_band_set_next_state(struct ftl_band *band)
{
	ftl_band_set_state(band, (band->state + 1) % FTL_BAND_STATE_MAX);
}

static inline int
ftl_band_state_changing(struct ftl_band *band)
{
	return band->state == FTL_BAND_STATE_OPENING ||
	       band->state == FTL_BAND_STATE_CLOSING;
}

static inline int
ftl_band_lbkoff_valid(struct ftl_band *band, size_t lbkoff)
{
	struct ftl_md *md = &band->md;

	pthread_spin_lock(&md->lock);
	if (spdk_bit_array_get(md->vld_map, lbkoff)) {
		pthread_spin_unlock(&md->lock);
		return 1;
	}

	pthread_spin_unlock(&md->lock);
	return 0;
}

static inline int
ftl_band_chunk_is_last(struct ftl_band *band, struct ftl_chunk *chunk)
{
	return chunk == CIRCLEQ_LAST(&band->chunks);
}

static inline int
ftl_band_chunk_is_first(struct ftl_band *band, struct ftl_chunk *chunk)
{
	return chunk == CIRCLEQ_FIRST(&band->chunks);
}

static inline int
ftl_chunk_is_writable(const struct ftl_chunk *chunk)
{
	return (chunk->state == FTL_CHUNK_STATE_OPEN ||
		chunk->state == FTL_CHUNK_STATE_FREE) &&
	       !chunk->busy;
}

#endif /* FTL_BAND_H */
