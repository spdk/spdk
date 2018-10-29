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

#ifndef OCSSD_CORE_H
#define OCSSD_CORE_H

#include <spdk/stdinc.h>
#include <spdk/nvme.h>
#include <spdk/nvme_ocssd.h>
#include <spdk/uuid.h>
#include <spdk/thread.h>
#include <spdk_internal/log.h>
#include <stdatomic.h>
#include <sys/queue.h>
#include <spdk/ocssd.h>
#include "ocssd_ppa.h"
#include "ocssd_nvme.h"
#include "ocssd_utils.h"
#include "ocssd_trace.h"

struct ocssd_dev;
struct ocssd_band;
struct ocssd_chunk;
struct ocssd_io;
struct ocssd_restore;
struct ocssd_reloc;
struct ocssd_wptr;
struct ocssd_flush;

struct ocssd_stats {
	/* Number of writes scheduled directly by the user */
	uint64_t				write_user;

	/* Total number of writes */
	uint64_t				write_total;

	/* Traces */
	struct ocssd_trace			*trace;

	/* Number of limits applied */
	uint64_t				limits[OCSSD_LIMIT_MAX];
};

struct ocssd_punit {
	struct ocssd_dev			*dev;

	struct ocssd_ppa			start_ppa;
};

enum ocssd_thread_id {
	OCSSD_THREAD_ID_CORE,
	OCSSD_THREAD_ID_READ,
	OCSSD_THREAD_ID_MAX,
};

struct ocssd_io_thread {
	/* Owner */
	struct ocssd_dev			*dev;

	/* Thread descriptor */
	struct ocssd_thread			*thread;

	/* I/O pair */
	struct ocssd_nvme_qpair			*qpair;
};

struct ocssd_global_md {
	/* Device instance */
	struct spdk_uuid			uuid;

	/* Size of the l2p table */
	uint64_t				l2p_len;
};

struct ocssd_dev {
	/* Device instance */
	struct spdk_uuid			uuid;
	/* Device name */
	char					*name;
	/* Configuration */
	struct ocssd_conf			conf;

	/* NVMe controller */
	struct ocssd_nvme_ctrlr			*ctrlr;

	/* LBA map memory pool */
	struct spdk_mempool			*lba_pool;

	/* Statistics */
	struct ocssd_stats			stats;

	/* Parallel unit range */
	struct ocssd_punit_range		range;
	/* Array of parallel units */
	struct ocssd_punit			*punits;

	/* Current sequence number */
	uint64_t				seq;

	/* Array of bands */
	struct ocssd_band			*bands;
	/* Band being curently defraged */
	struct ocssd_band			*df_band;
	/* Number of operational bands */
	size_t					num_bands;
	/* Next write band */
	struct ocssd_band			*next_band;
	/* Free band list */
	LIST_HEAD(, ocssd_band)			free_bands;
	/* Closed bands list */
	LIST_HEAD(, ocssd_band)			shut_bands;
	/* Number of free bands */
	size_t					num_free;

	/* List of write pointers */
	LIST_HEAD(, ocssd_wptr)			wptr_list;

	/* Logical -> physical table */
	void					*l2p;
	/* Size of the l2p table */
	uint64_t				l2p_len;

	/* PPA format */
	struct ocssd_ppa_fmt			ppaf;
	/* PPA address size */
	size_t					ppa_len;
	/* Device's geometry */
	struct spdk_ocssd_geometry_data		geo;

	/* Flush list */
	LIST_HEAD(, ocssd_flush)		flush_list;

	/* Device specific md buffer */
	struct ocssd_global_md			global_md;

	/* Metadata size */
	size_t					md_size;

	/* Transfer unit size */
	size_t					xfer_size;
	/* Ring write buffer */
	struct ocssd_rwb			*rwb;

	/* Current user write limit */
	int					limit;

	/* Inflight io operations */
	atomic_ulong				num_inflight;

	/* Manages data relocation */
	struct ocssd_reloc			*reloc;

	/* Array of io threads */
	struct ocssd_io_thread			thread[OCSSD_THREAD_ID_MAX];

	/* Devices' list */
	STAILQ_ENTRY(ocssd_dev)			stailq;
};

void	ocssd_apply_limits(struct ocssd_dev *dev);
void	ocssd_core_thread(void *ctx);
int	ocssd_io_read(struct ocssd_io *io);
int	ocssd_io_write(struct ocssd_io *io);
int	ocssd_io_erase(struct ocssd_io *io);
int	ocssd_io_flush(struct ocssd_io *io);
int	ocssd_current_limit(const struct ocssd_dev *dev);
int	ocssd_invalidate_addr(struct ocssd_dev *dev, struct ocssd_ppa ppa);
void	ocssd_core_thread(void *ctx);
void	ocssd_read_thread(void *ctx);
size_t	ocssd_tail_md_num_lbks(const struct ocssd_dev *dev);
size_t	ocssd_tail_md_hdr_num_lbks(const struct ocssd_dev *dev);
size_t	ocssd_vld_map_num_lbks(const struct ocssd_dev *dev);
size_t	ocssd_lba_map_num_lbks(const struct ocssd_dev *dev);
size_t	ocssd_head_md_num_lbks(const struct ocssd_dev *dev);
struct ocssd_restore *ocssd_restore_init(struct ocssd_dev *dev);
int	ocssd_restore_check_device(struct ocssd_dev *dev,
				   struct ocssd_restore *restore);
int	ocssd_restore_state(struct ocssd_dev *dev, struct ocssd_restore *restore);
void	ocssd_restore_free(struct ocssd_restore *restore);

#define ocssd_to_ppa(addr) \
	(struct ocssd_ppa) { .ppa = (uint64_t)(addr) }

#define ocssd_to_ppa_packed(addr) \
	(struct ocssd_ppa) { .pack.ppa = (uint32_t)(addr) }

static inline struct ocssd_thread *
ocssd_get_core_thread(const struct ocssd_dev *dev)
{
	return dev->thread[OCSSD_THREAD_ID_CORE].thread;
}

static inline struct ocssd_nvme_qpair *
ocssd_get_write_qpair(const struct ocssd_dev *dev)
{
	return dev->thread[OCSSD_THREAD_ID_CORE].qpair;
}

static inline struct ocssd_thread *
ocssd_get_read_thread(const struct ocssd_dev *dev)
{
	return dev->thread[OCSSD_THREAD_ID_READ].thread;
}

static inline struct ocssd_nvme_qpair *
ocssd_get_read_qpair(const struct ocssd_dev *dev)
{
	return dev->thread[OCSSD_THREAD_ID_READ].qpair;
}

static inline int
ocssd_ppa_packed(const struct ocssd_dev *dev)
{
	return dev->ppa_len < 32;
}

static inline int
ocssd_ppa_invalid(struct ocssd_ppa ppa)
{
	return ppa.ppa == ocssd_to_ppa(OCSSD_PPA_INVALID).ppa;
}

static inline int
ocssd_ppa_cached(struct ocssd_ppa ppa)
{
	return !ocssd_ppa_invalid(ppa) && ppa.cached;
}

static inline uint64_t
ocssd_ppa_addr_pack(const struct ocssd_dev *dev, struct ocssd_ppa ppa)
{
	return (ppa.lbk << dev->ppaf.lbk_offset) |
	       (ppa.chk << dev->ppaf.chk_offset) |
	       (ppa.pu  << dev->ppaf.pu_offset) |
	       (ppa.grp << dev->ppaf.grp_offset);
}

static inline struct ocssd_ppa
ocssd_ppa_addr_unpack(const struct ocssd_dev *dev, uint64_t ppa)
{
	struct ocssd_ppa res = {};

	res.lbk = (ppa >> dev->ppaf.lbk_offset) & dev->ppaf.lbk_mask;
	res.chk = (ppa >> dev->ppaf.chk_offset) & dev->ppaf.chk_mask;
	res.pu  = (ppa >> dev->ppaf.pu_offset)  & dev->ppaf.pu_mask;
	res.grp = (ppa >> dev->ppaf.grp_offset) & dev->ppaf.grp_mask;

	return res;
}

static inline struct ocssd_ppa
ocssd_ppa_to_packed(const struct ocssd_dev *dev, struct ocssd_ppa ppa)
{
	struct ocssd_ppa p = {};

	if (ocssd_ppa_invalid(ppa)) {
		p = ocssd_to_ppa_packed(OCSSD_PPA_INVALID);
	} else if (ocssd_ppa_cached(ppa)) {
		p.pack.cached = 1;
		p.pack.offset = (uint32_t) ppa.offset;
	} else {
		p.pack.ppa = (uint32_t) ocssd_ppa_addr_pack(dev, ppa);
	}

	return p;
}

static inline struct ocssd_ppa
ocssd_ppa_from_packed(const struct ocssd_dev *dev, struct ocssd_ppa p)
{
	struct ocssd_ppa ppa = {};

	if (p.pack.ppa == (uint32_t)OCSSD_PPA_INVALID) {
		ppa = ocssd_to_ppa(OCSSD_PPA_INVALID);
	} else if (p.pack.cached) {
		ppa.cached = 1;
		ppa.offset = p.pack.offset;
	} else {
		ppa = ocssd_ppa_addr_unpack(dev, p.pack.ppa);
	}

	return ppa;
}

static inline unsigned int
ocssd_ppa_flatten_punit(const struct ocssd_dev *dev, struct ocssd_ppa ppa)
{
	return ppa.pu * dev->geo.num_grp + ppa.grp - dev->range.begin;
}

static inline int
ocssd_ppa_in_range(const struct ocssd_dev *dev, struct ocssd_ppa ppa)
{
	unsigned int punit = ocssd_ppa_flatten_punit(dev, ppa);

	if (punit >= dev->range.begin && punit <= dev->range.end) {
		return 1;
	}

	return 0;
}

static inline int
ocssd_lba_invalid(uint64_t lba)
{
	return lba == OCSSD_LBA_INVALID;
}

#define _ocssd_l2p_set(l2p, off, val, bits) \
	atomic_store((_Atomic(uint##bits##_t) *)(l2p) + (off), val)

#define _ocssd_l2p_set32(l2p, off, val) \
	_ocssd_l2p_set(l2p, off, val, 32)

#define _ocssd_l2p_set64(l2p, off, val) \
	_ocssd_l2p_set(l2p, off, val, 64)

#define _ocssd_l2p_get(l2p, off, bits) \
	atomic_load((_Atomic(uint##bits##_t) *)(l2p) + (off))

#define _ocssd_l2p_get32(l2p, off) \
	_ocssd_l2p_get(l2p, off, 32)

#define _ocssd_l2p_get64(l2p, off) \
	_ocssd_l2p_get(l2p, off, 64)

#define ocssd_ppa_cmp(p1, p2) \
	((p1).ppa == (p2).ppa)

static inline void
ocssd_l2p_set(struct ocssd_dev *dev, uint64_t lba, struct ocssd_ppa ppa)
{
	assert(dev->l2p_len > lba);

	if (ocssd_ppa_packed(dev)) {
		_ocssd_l2p_set32(dev->l2p, lba, ocssd_ppa_to_packed(dev, ppa).ppa);
	} else {
		_ocssd_l2p_set64(dev->l2p, lba, ppa.ppa);
	}
}

static inline struct ocssd_ppa
ocssd_l2p_get(struct ocssd_dev *dev, uint64_t lba)
{
	assert(dev->l2p_len > lba);

	if (ocssd_ppa_packed(dev)) {
		return ocssd_ppa_from_packed(dev, ocssd_to_ppa_packed(
						     _ocssd_l2p_get32(dev->l2p, lba)));
	} else {
		return ocssd_to_ppa(_ocssd_l2p_get64(dev->l2p, lba));
	}
}
static inline size_t
ocssd_dev_num_bands(const struct ocssd_dev *dev)
{
	return dev->geo.num_chk;
}

static inline size_t
ocssd_dev_lbks_in_chunk(const struct ocssd_dev *dev)
{
	return dev->geo.clba;
}

static inline size_t
ocssd_dev_num_punits(const struct ocssd_dev *dev)
{
	return dev->range.end - dev->range.begin + 1;
}

static inline uint64_t
ocssd_num_band_lbks(const struct ocssd_dev *dev)
{
	return ocssd_dev_num_punits(dev) * ocssd_dev_lbks_in_chunk(dev);
}

static inline size_t
ocssd_vld_map_size(const struct ocssd_dev *dev)
{
	return ocssd_div_up(ocssd_num_band_lbks(dev), CHAR_BIT);
}

static inline struct ocssd_trace *
ocssd_dev_trace(struct ocssd_dev *dev)
{
	return dev->stats.trace;
}

#endif /* OCSSD_CORE_H */
