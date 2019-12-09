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

#ifndef FTL_CORE_H
#define FTL_CORE_H

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/nvme_ocssd.h"
#include "spdk/uuid.h"
#include "spdk/thread.h"
#include "spdk/util.h"
#include "spdk_internal/log.h"
#include "spdk/likely.h"
#include "spdk/queue.h"
#include "spdk/ftl.h"
#include "spdk/bdev.h"

#include "ftl_ppa.h"
#include "ftl_io.h"
#include "ftl_trace.h"

struct spdk_ftl_dev;
struct ftl_band;
struct ftl_zone;
struct ftl_io;
struct ftl_restore;
struct ftl_wptr;
struct ftl_flush;
struct ftl_reloc;
struct ftl_anm_event;
struct ftl_band_flush;

struct ftl_stats {
	/* Number of writes scheduled directly by the user */
	uint64_t				write_user;

	/* Total number of writes */
	uint64_t				write_total;

	/* Traces */
	struct ftl_trace			trace;

	/* Number of limits applied */
	uint64_t				limits[SPDK_FTL_LIMIT_MAX];
};

struct ftl_punit {
	struct spdk_ftl_dev			*dev;

	struct ftl_ppa				start_ppa;
};

struct ftl_thread {
	/* Owner */
	struct spdk_ftl_dev			*dev;
	/* I/O queue pair */
	struct spdk_nvme_qpair			*qpair;

	/* Thread on which the poller is running */
	struct spdk_thread			*thread;

	/* IO channel */
	struct spdk_io_channel			*ioch;
	/* Poller */
	struct spdk_poller			*poller;
	/* Poller's function */
	spdk_poller_fn				poller_fn;
	/* Poller's frequency */
	uint64_t				period_us;
};

struct ftl_global_md {
	/* Device instance */
	struct spdk_uuid			uuid;
	/* Size of the l2p table */
	uint64_t				num_lbas;
};

struct ftl_nv_cache {
	/* Write buffer cache bdev */
	struct spdk_bdev_desc			*bdev_desc;
	/* Write pointer */
	uint64_t				current_addr;
	/* Number of available blocks left */
	uint64_t				num_available;
	/* Maximum number of blocks */
	uint64_t				num_data_blocks;
	/*
	 * Phase of the current cycle of writes. Each time whole cache area is filled, the phase is
	 * advanced. Current phase is saved in every IO's metadata, as well as in the header saved
	 * in the first sector. By looking at the phase of each block, it's possible to find the
	 * oldest block and replay the order of the writes when recovering the data from the cache.
	 */
	unsigned int				phase;
	/* Indicates that the data can be written to the cache */
	bool					ready;
	/* Metadata pool */
	struct spdk_mempool			*md_pool;
	/* DMA buffer for writing the header */
	void					*dma_buf;
	/* Cache lock */
	pthread_spinlock_t			lock;
};

struct ftl_init_context {
	/* User's callback */
	spdk_ftl_init_fn			cb_fn;
	/* Callback's argument */
	void					*cb_arg;
	/* Thread to call the callback on */
	struct spdk_thread			*thread;
	/* Poller to check if the device has been destroyed/initialized */
	struct spdk_poller			*poller;
};

struct spdk_ftl_dev {
	/* Device instance */
	struct spdk_uuid			uuid;
	/* Device name */
	char					*name;
	/* Configuration */
	struct spdk_ftl_conf			conf;

	/* Indicates the device is fully initialized */
	int					initialized;
	/* Indicates the device is about to be stopped */
	int					halt;

	/* Status to return for halt completion callback */
	int					halt_complete_status;
	/* Initializaton context */
	struct ftl_init_context			init_ctx;
	/* Destruction context */
	struct ftl_init_context			fini_ctx;

	/* NVMe controller */
	struct spdk_nvme_ctrlr			*ctrlr;
	/* NVMe namespace */
	struct spdk_nvme_ns			*ns;
	/* NVMe transport ID */
	struct spdk_nvme_transport_id		trid;

	/* Non-volatile write buffer cache */
	struct ftl_nv_cache			nv_cache;

	/* LBA map memory pool */
	struct spdk_mempool			*lba_pool;

	/* LBA map requests pool */
	struct spdk_mempool			*lba_request_pool;

	/* Statistics */
	struct ftl_stats			stats;

	/* Parallel unit range */
	struct spdk_ftl_punit_range		range;
	/* Array of parallel units */
	struct ftl_punit			*punits;

	/* Current sequence number */
	uint64_t				seq;

	/* Array of bands */
	struct ftl_band				*bands;
	/* Number of operational bands */
	size_t					num_bands;
	/* Next write band */
	struct ftl_band				*next_band;
	/* Free band list */
	LIST_HEAD(, ftl_band)			free_bands;
	/* Closed bands list */
	LIST_HEAD(, ftl_band)			shut_bands;
	/* Number of free bands */
	size_t					num_free;

	/* List of write pointers */
	LIST_HEAD(, ftl_wptr)			wptr_list;

	/* Logical -> physical table */
	void					*l2p;
	/* Size of the l2p table */
	uint64_t				num_lbas;

	/* PPA format */
	struct ftl_ppa_fmt			ppaf;
	/* PPA address size */
	size_t					ppa_len;
	/* Device's geometry */
	struct spdk_ocssd_geometry_data		geo;

	/* Flush list */
	LIST_HEAD(, ftl_flush)			flush_list;
	/* List of band flush requests */
	LIST_HEAD(, ftl_band_flush)		band_flush_list;

	/* Device specific md buffer */
	struct ftl_global_md			global_md;

	/* Metadata size */
	size_t					md_size;

	/* Transfer unit size */
	size_t					xfer_size;
	/* Ring write buffer */
	struct ftl_rwb				*rwb;

	/* Current user write limit */
	int					limit;

	/* Inflight IO operations */
	uint32_t				num_inflight;
	/* Queue of IO awaiting retry */
	TAILQ_HEAD(, ftl_io)			retry_queue;

	/* Manages data relocation */
	struct ftl_reloc			*reloc;

	/* Threads */
	struct ftl_thread			core_thread;
	struct ftl_thread			read_thread;

	/* Devices' list */
	STAILQ_ENTRY(spdk_ftl_dev)		stailq;
};

struct ftl_nv_cache_header {
	/* Version of the header */
	uint32_t				version;
	/* UUID of the FTL device */
	struct spdk_uuid			uuid;
	/* Size of the non-volatile cache (in blocks) */
	uint64_t				size;
	/* Contains the next address to be written after clean shutdown, invalid LBA otherwise */
	uint64_t				current_addr;
	/* Current phase */
	uint8_t					phase;
	/* Checksum of the header, needs to be last element */
	uint32_t				checksum;
} __attribute__((packed));

typedef void (*ftl_restore_fn)(struct spdk_ftl_dev *, struct ftl_restore *, int);

void	ftl_apply_limits(struct spdk_ftl_dev *dev);
void	ftl_io_read(struct ftl_io *io);
void	ftl_io_write(struct ftl_io *io);
int	ftl_io_erase(struct ftl_io *io);
int	ftl_flush_rwb(struct spdk_ftl_dev *dev, spdk_ftl_fn cb_fn, void *cb_arg);
int	ftl_current_limit(const struct spdk_ftl_dev *dev);
int	ftl_invalidate_addr(struct spdk_ftl_dev *dev, struct ftl_ppa ppa);
int	ftl_task_core(void *ctx);
int	ftl_task_read(void *ctx);
void	ftl_process_anm_event(struct ftl_anm_event *event);
size_t	ftl_tail_md_num_lbks(const struct spdk_ftl_dev *dev);
size_t	ftl_tail_md_hdr_num_lbks(void);
size_t	ftl_vld_map_num_lbks(const struct spdk_ftl_dev *dev);
size_t	ftl_lba_map_num_lbks(const struct spdk_ftl_dev *dev);
size_t	ftl_head_md_num_lbks(const struct spdk_ftl_dev *dev);
int	ftl_restore_md(struct spdk_ftl_dev *dev, ftl_restore_fn cb);
int	ftl_restore_device(struct ftl_restore *restore, ftl_restore_fn cb);
void	ftl_restore_nv_cache(struct ftl_restore *restore, ftl_restore_fn cb);
int	ftl_band_set_direct_access(struct ftl_band *band, bool access);
int	ftl_retrieve_chunk_info(struct spdk_ftl_dev *dev, struct ftl_ppa ppa,
				struct spdk_ocssd_chunk_information_entry *info,
				unsigned int num_entries);
bool	ftl_ppa_is_written(struct ftl_band *band, struct ftl_ppa ppa);
int	ftl_flush_active_bands(struct spdk_ftl_dev *dev, spdk_ftl_fn cb_fn, void *cb_arg);
int	ftl_nv_cache_write_header(struct ftl_nv_cache *nv_cache, bool shutdown,
				  spdk_bdev_io_completion_cb cb_fn, void *cb_arg);
int	ftl_nv_cache_scrub(struct ftl_nv_cache *nv_cache, spdk_bdev_io_completion_cb cb_fn,
			   void *cb_arg);

struct spdk_io_channel *
ftl_get_io_channel(const struct spdk_ftl_dev *dev);

#define ftl_to_ppa(addr) \
	(struct ftl_ppa) { .ppa = (uint64_t)(addr) }

#define ftl_to_ppa_packed(addr) \
	(struct ftl_ppa) { .pack.ppa = (uint32_t)(addr) }

static inline struct spdk_thread *
ftl_get_core_thread(const struct spdk_ftl_dev *dev)
{
	return dev->core_thread.thread;
}

static inline struct spdk_nvme_qpair *
ftl_get_write_qpair(const struct spdk_ftl_dev *dev)
{
	return dev->core_thread.qpair;
}

static inline struct spdk_thread *
ftl_get_read_thread(const struct spdk_ftl_dev *dev)
{
	return dev->read_thread.thread;
}

static inline struct spdk_nvme_qpair *
ftl_get_read_qpair(const struct spdk_ftl_dev *dev)
{
	return dev->read_thread.qpair;
}

static inline int
ftl_ppa_packed(const struct spdk_ftl_dev *dev)
{
	return dev->ppa_len < 32;
}

static inline int
ftl_ppa_invalid(struct ftl_ppa ppa)
{
	return ppa.ppa == ftl_to_ppa(FTL_PPA_INVALID).ppa;
}

static inline int
ftl_ppa_cached(struct ftl_ppa ppa)
{
	return !ftl_ppa_invalid(ppa) && ppa.cached;
}

static inline uint64_t
ftl_ppa_addr_pack(const struct spdk_ftl_dev *dev, struct ftl_ppa ppa)
{
	uint64_t lbk, chk, pu, grp;

	lbk = ppa.lbk;
	chk = ppa.chk;
	pu = ppa.pu;
	grp = ppa.grp;

	return (lbk << dev->ppaf.lbk_offset) |
	       (chk << dev->ppaf.chk_offset) |
	       (pu  << dev->ppaf.pu_offset) |
	       (grp << dev->ppaf.grp_offset);
}

static inline struct ftl_ppa
ftl_ppa_addr_unpack(const struct spdk_ftl_dev *dev, uint64_t ppa)
{
	struct ftl_ppa res = {};

	res.lbk = (ppa >> dev->ppaf.lbk_offset) & dev->ppaf.lbk_mask;
	res.chk = (ppa >> dev->ppaf.chk_offset) & dev->ppaf.chk_mask;
	res.pu  = (ppa >> dev->ppaf.pu_offset)  & dev->ppaf.pu_mask;
	res.grp = (ppa >> dev->ppaf.grp_offset) & dev->ppaf.grp_mask;

	return res;
}

static inline struct ftl_ppa
ftl_ppa_to_packed(const struct spdk_ftl_dev *dev, struct ftl_ppa ppa)
{
	struct ftl_ppa p = {};

	if (ftl_ppa_invalid(ppa)) {
		p = ftl_to_ppa_packed(FTL_PPA_INVALID);
	} else if (ftl_ppa_cached(ppa)) {
		p.pack.cached = 1;
		p.pack.offset = (uint32_t) ppa.offset;
	} else {
		p.pack.ppa = (uint32_t) ftl_ppa_addr_pack(dev, ppa);
	}

	return p;
}

static inline struct ftl_ppa
ftl_ppa_from_packed(const struct spdk_ftl_dev *dev, struct ftl_ppa p)
{
	struct ftl_ppa ppa = {};

	if (p.pack.ppa == (uint32_t)FTL_PPA_INVALID) {
		ppa = ftl_to_ppa(FTL_PPA_INVALID);
	} else if (p.pack.cached) {
		ppa.cached = 1;
		ppa.offset = p.pack.offset;
	} else {
		ppa = ftl_ppa_addr_unpack(dev, p.pack.ppa);
	}

	return ppa;
}

static inline unsigned int
ftl_ppa_flatten_punit(const struct spdk_ftl_dev *dev, struct ftl_ppa ppa)
{
	return ppa.pu * dev->geo.num_grp + ppa.grp - dev->range.begin;
}

static inline int
ftl_ppa_in_range(const struct spdk_ftl_dev *dev, struct ftl_ppa ppa)
{
	unsigned int punit = ftl_ppa_flatten_punit(dev, ppa) + dev->range.begin;

	if (punit >= dev->range.begin && punit <= dev->range.end) {
		return 1;
	}

	return 0;
}

#define _ftl_l2p_set(l2p, off, val, bits) \
	__atomic_store_n(((uint##bits##_t *)(l2p)) + (off), val, __ATOMIC_SEQ_CST)

#define _ftl_l2p_set32(l2p, off, val) \
	_ftl_l2p_set(l2p, off, val, 32)

#define _ftl_l2p_set64(l2p, off, val) \
	_ftl_l2p_set(l2p, off, val, 64)

#define _ftl_l2p_get(l2p, off, bits) \
	__atomic_load_n(((uint##bits##_t *)(l2p)) + (off), __ATOMIC_SEQ_CST)

#define _ftl_l2p_get32(l2p, off) \
	_ftl_l2p_get(l2p, off, 32)

#define _ftl_l2p_get64(l2p, off) \
	_ftl_l2p_get(l2p, off, 64)

#define ftl_ppa_cmp(p1, p2) \
	((p1).ppa == (p2).ppa)

static inline void
ftl_l2p_set(struct spdk_ftl_dev *dev, uint64_t lba, struct ftl_ppa ppa)
{
	assert(dev->num_lbas > lba);

	if (ftl_ppa_packed(dev)) {
		_ftl_l2p_set32(dev->l2p, lba, ftl_ppa_to_packed(dev, ppa).ppa);
	} else {
		_ftl_l2p_set64(dev->l2p, lba, ppa.ppa);
	}
}

static inline struct ftl_ppa
ftl_l2p_get(struct spdk_ftl_dev *dev, uint64_t lba)
{
	assert(dev->num_lbas > lba);

	if (ftl_ppa_packed(dev)) {
		return ftl_ppa_from_packed(dev, ftl_to_ppa_packed(
						   _ftl_l2p_get32(dev->l2p, lba)));
	} else {
		return ftl_to_ppa(_ftl_l2p_get64(dev->l2p, lba));
	}
}
static inline size_t
ftl_dev_num_bands(const struct spdk_ftl_dev *dev)
{
	return dev->geo.num_chk;
}

static inline size_t
ftl_dev_lbks_in_zone(const struct spdk_ftl_dev *dev)
{
	return dev->geo.clba;
}

static inline size_t
ftl_dev_num_punits(const struct spdk_ftl_dev *dev)
{
	return dev->range.end - dev->range.begin + 1;
}

static inline uint64_t
ftl_num_band_lbks(const struct spdk_ftl_dev *dev)
{
	return ftl_dev_num_punits(dev) * ftl_dev_lbks_in_zone(dev);
}

static inline size_t
ftl_vld_map_size(const struct spdk_ftl_dev *dev)
{
	return (size_t)spdk_divide_round_up(ftl_num_band_lbks(dev), CHAR_BIT);
}

static inline bool
ftl_dev_has_nv_cache(const struct spdk_ftl_dev *dev)
{
	return dev->nv_cache.bdev_desc != NULL;
}

#define FTL_NV_CACHE_HEADER_VERSION	(1)
#define FTL_NV_CACHE_DATA_OFFSET	(1)
#define FTL_NV_CACHE_PHASE_OFFSET	(62)
#define FTL_NV_CACHE_PHASE_COUNT	(4)
#define FTL_NV_CACHE_PHASE_MASK		(3ULL << FTL_NV_CACHE_PHASE_OFFSET)
#define FTL_NV_CACHE_LBA_INVALID	(FTL_LBA_INVALID & ~FTL_NV_CACHE_PHASE_MASK)

static inline bool
ftl_nv_cache_phase_is_valid(unsigned int phase)
{
	return phase > 0 && phase <= 3;
}

static inline unsigned int
ftl_nv_cache_next_phase(unsigned int current)
{
	static const unsigned int phases[] = { 0, 2, 3, 1 };
	assert(ftl_nv_cache_phase_is_valid(current));
	return phases[current];
}

static inline unsigned int
ftl_nv_cache_prev_phase(unsigned int current)
{
	static const unsigned int phases[] = { 0, 3, 1, 2 };
	assert(ftl_nv_cache_phase_is_valid(current));
	return phases[current];
}

static inline uint64_t
ftl_nv_cache_pack_lba(uint64_t lba, unsigned int phase)
{
	assert(ftl_nv_cache_phase_is_valid(phase));
	return (lba & ~FTL_NV_CACHE_PHASE_MASK) | ((uint64_t)phase << FTL_NV_CACHE_PHASE_OFFSET);
}

static inline void
ftl_nv_cache_unpack_lba(uint64_t in_lba, uint64_t *out_lba, unsigned int *phase)
{
	*out_lba = in_lba & ~FTL_NV_CACHE_PHASE_MASK;
	*phase = (in_lba & FTL_NV_CACHE_PHASE_MASK) >> FTL_NV_CACHE_PHASE_OFFSET;

	/* If the phase is invalid the block wasn't written yet, so treat the LBA as invalid too */
	if (!ftl_nv_cache_phase_is_valid(*phase) || *out_lba == FTL_NV_CACHE_LBA_INVALID) {
		*out_lba = FTL_LBA_INVALID;
	}
}

#endif /* FTL_CORE_H */
