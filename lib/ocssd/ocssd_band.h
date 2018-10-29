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

#ifndef OCSSD_BAND_H
#define OCSSD_BAND_H

#include <spdk/stdinc.h>
#include <sys/queue.h>
#include "ocssd_ppa.h"
#include "ocssd_utils.h"

struct ocssd_dev;
struct ocssd_cb;

enum ocssd_chunk_state {
	OCSSD_CHUNK_STATE_FREE,
	OCSSD_CHUNK_STATE_OPEN,
	OCSSD_CHUNK_STATE_CLOSED,
	OCSSD_CHUNK_STATE_BAD,
	OCSSD_CHUNK_STATE_VACANT,
};

struct ocssd_chunk {
	/* Block state */
	enum ocssd_chunk_state			state;

	/* First PPA */
	struct ocssd_ppa			start_ppa;

	/* Pointer to parallel unit */
	struct ocssd_punit			*punit;

	/* Position in band's chunk_buf */
	unsigned int				pos;

	CIRCLEQ_ENTRY(ocssd_chunk)		circleq;
};

enum ocssd_md_status {
	OCSSD_MD_SUCCESS,
	/* Metadata read failure */
	OCSSD_MD_IO_FAILURE,
	/* Invalid version */
	OCSSD_MD_INVALID_VER,
	/* UUID doesn't match */
	OCSSD_MD_NO_MD,
	/* UUID and version matches but CRC doesn't */
	OCSSD_MD_INVALID_CRC,
	/* Vld or lba map size doesn't match */
	OCSSD_MD_INVALID_SIZE
};

struct ocssd_md {
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
	void					*vld_map;

	/* LBA map (only valid for open bands) */
	uint64_t				*lba_map;
};

enum ocssd_band_state {
	OCSSD_BAND_STATE_FREE,
	OCSSD_BAND_STATE_PREP,
	OCSSD_BAND_STATE_OPENING,
	OCSSD_BAND_STATE_OPEN,
	OCSSD_BAND_STATE_FULL,
	OCSSD_BAND_STATE_CLOSING,
	OCSSD_BAND_STATE_CLOSED,
	OCSSD_BAND_STATE_MAX
};

struct ocssd_band {
	/* Device this band belongs to */
	struct ocssd_dev			*dev;

	/* Number of operational chunks */
	size_t					num_chunks;

	/* Array of chunks */
	struct ocssd_chunk			*chunk_buf;

	/* List of operational chunks */
	CIRCLEQ_HEAD(, ocssd_chunk)		chunks;

	/* Band's metadata */
	struct ocssd_md				md;

	/* Band's state */
	enum ocssd_band_state			state;

	/* Band's index */
	unsigned int				id;

	/* Latest merit calculation */
	double					merit;

	/* High defrag priority - means that the metadata should be copied and */
	/* the band should be defragged immediately */
	int					high_prio;

	/* End metadata start ppa */
	struct ocssd_ppa			tail_md_ppa;

	/* Free/shut bands' lists */
	LIST_ENTRY(ocssd_band)			list_entry;

	/* High priority queue link */
	STAILQ_ENTRY(ocssd_band)		prio_stailq;
};

uint64_t	ocssd_band_lbkoff_from_ppa(struct ocssd_band *band, struct ocssd_ppa ppa);
struct ocssd_ppa ocssd_band_ppa_from_lbkoff(struct ocssd_band *band, uint64_t lbkoff);
void		ocssd_band_set_state(struct ocssd_band *band, enum ocssd_band_state state);
size_t		ocssd_band_age(const struct ocssd_band *band);
void		ocssd_band_acquire_md(struct ocssd_band *band);
int		ocssd_band_alloc_md(struct ocssd_band *band);
void		ocssd_band_release_md(struct ocssd_band *band);
struct ocssd_ppa ocssd_band_next_xfer_ppa(struct ocssd_band *band, struct ocssd_ppa ppa,
		size_t num_lbks);
struct ocssd_ppa ocssd_band_next_ppa(struct ocssd_band *band, struct ocssd_ppa ppa,
				     size_t offset);
size_t		ocssd_band_num_usable_lbks(const struct ocssd_band *band);
size_t		ocssd_band_user_lbks(const struct ocssd_band *band);
void		ocssd_band_set_addr(struct ocssd_band *band, uint64_t lba,
				    struct ocssd_ppa ppa);
struct ocssd_band *ocssd_band_from_ppa(struct ocssd_dev *dev, struct ocssd_ppa ppa);
struct ocssd_chunk *ocssd_band_chunk_from_ppa(struct ocssd_band *band, struct ocssd_ppa);
void		ocssd_band_md_clear(struct ocssd_md *md);
int		ocssd_band_read_tail_md(struct ocssd_band *band, struct ocssd_md *md,
					void *data, struct ocssd_ppa,
					const struct ocssd_cb *cb);
int		ocssd_band_read_head_md(struct ocssd_band *band, struct ocssd_md *md,
					void *data, const struct ocssd_cb *cb);
int		ocssd_band_read_lba_map(struct ocssd_band *band, struct ocssd_md *md,
					void *data, const struct ocssd_cb *cb);
int		ocssd_band_write_tail_md(struct ocssd_band *band, void *data, ocssd_fn cb);
int		ocssd_band_write_head_md(struct ocssd_band *band, void *data, ocssd_fn cb);
struct ocssd_ppa ocssd_band_tail_md_ppa(struct ocssd_band *band);
struct ocssd_ppa ocssd_band_head_md_ppa(struct ocssd_band *band);
void		ocssd_band_write_failed(struct ocssd_band *band);
void		ocssd_band_clear_md(struct ocssd_band *band);
int		ocssd_band_full(struct ocssd_band *band, size_t offset);
int		ocssd_band_erase(struct ocssd_band *band);
int		ocssd_band_write_prep(struct ocssd_band *band);
struct ocssd_chunk *ocssd_band_next_operational_chunk(struct ocssd_band *band,
		struct ocssd_chunk *chunk);

static inline int
ocssd_band_empty(const struct ocssd_band *band)
{
	return band->md.num_vld == 0;
}

static inline int
ocssd_chunk_is_bad(struct ocssd_chunk *chunk)
{
	return chunk->state == OCSSD_CHUNK_STATE_BAD;
}

static inline struct ocssd_chunk *
ocssd_band_next_chunk(struct ocssd_band *band, struct ocssd_chunk *chunk)
{
	assert(!ocssd_chunk_is_bad(chunk));
	return CIRCLEQ_LOOP_NEXT(&band->chunks, chunk, circleq);
}

static inline void
ocssd_band_set_next_state(struct ocssd_band *band)
{
	ocssd_band_set_state(band, (band->state + 1) % OCSSD_BAND_STATE_MAX);
}

static inline int
ocssd_band_check_state(struct ocssd_band *band, enum ocssd_band_state state)
{
	return band->state == state;
}

static inline int
ocssd_band_state_changing(struct ocssd_band *band)
{
	return ocssd_band_check_state(band, OCSSD_BAND_STATE_OPENING) ||
	       ocssd_band_check_state(band, OCSSD_BAND_STATE_CLOSING);
}

static inline int
ocssd_band_lbkoff_valid(struct ocssd_band *band, size_t lbkoff)
{
	struct ocssd_md *md = &band->md;

	pthread_spin_lock(&md->lock);
	if (ocssd_get_bit(lbkoff, md->vld_map)) {
		pthread_spin_unlock(&md->lock);
		return 1;
	}

	pthread_spin_unlock(&md->lock);
	return 0;
}

static inline void
ocssd_band_lock(struct ocssd_band *band)
{
	pthread_spin_lock(&band->md.lock);
}

static inline void
ocssd_band_unlock(struct ocssd_band *band)
{
	pthread_spin_unlock(&band->md.lock);
}

static inline int
ocssd_band_chunk_is_last(struct ocssd_band *band, struct ocssd_chunk *chunk)
{
	return chunk == CIRCLEQ_LAST(&band->chunks);
}

static inline int
ocssd_band_has_chunks(struct ocssd_band *band)
{
	return (band->num_chunks > 0);
}

static inline int
ocssd_band_chunk_is_first(struct ocssd_band *band, struct ocssd_chunk *chunk)
{
	return chunk == CIRCLEQ_FIRST(&band->chunks);
}

static inline int
ocssd_chunk_is_writable(const struct ocssd_chunk *chunk)
{
	return chunk->state == OCSSD_CHUNK_STATE_OPEN || chunk->state == OCSSD_CHUNK_STATE_FREE;
}

static inline void
ocssd_chunk_set_state(struct ocssd_chunk *chunk, enum ocssd_chunk_state state)
{
	chunk->state = state;
}

#endif /* OCSSD_BAND_H */
