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
#include "spdk/uuid.h"
#include "spdk/thread.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/likely.h"
#include "spdk/queue.h"
#include "spdk/ftl.h"
#include "spdk/bdev.h"
#include "spdk/bdev_zone.h"

#include "ftl_addr.h"
#include "ftl_io.h"
#include "ftl_trace.h"

#ifdef SPDK_CONFIG_PMDK
#include "libpmem.h"
#endif /* SPDK_CONFIG_PMDK */

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

struct ftl_batch {
	/* Queue of write buffer entries, can reach up to xfer_size entries */
	TAILQ_HEAD(, ftl_wbuf_entry)		entries;
	/* Number of entries in the queue above */
	uint32_t				num_entries;
	/* Index within spdk_ftl_dev.batch_array */
	uint32_t				index;
	struct iovec				*iov;
	void					*metadata;
	TAILQ_ENTRY(ftl_batch)			tailq;
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
	/* Indicates the device is about to start stopping - use to handle multiple stop request */
	bool					halt_started;

	/* Underlying device */
	struct spdk_bdev_desc			*base_bdev_desc;

	/* Non-volatile write buffer cache */
	struct ftl_nv_cache			nv_cache;

	/* LBA map memory pool */
	struct spdk_mempool			*lba_pool;

	/* LBA map requests pool */
	struct spdk_mempool			*lba_request_pool;

	/* Media management events pool */
	struct spdk_mempool			*media_events_pool;

	/* Statistics */
	struct ftl_stats			stats;

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
	/* Size of pages mmapped for l2p, valid only for mapping on persistent memory */
	size_t					l2p_pmem_len;

	/* Address size */
	size_t					addr_len;

	/* Flush list */
	LIST_HEAD(, ftl_flush)			flush_list;
	/* List of band flush requests */
	LIST_HEAD(, ftl_band_flush)		band_flush_list;

	/* Device specific md buffer */
	struct ftl_global_md			global_md;

	/* Metadata size */
	size_t					md_size;
	void					*md_buf;

	/* Transfer unit size */
	size_t					xfer_size;

	/* Current user write limit */
	int					limit;

	/* Inflight IO operations */
	uint32_t				num_inflight;

	/* Manages data relocation */
	struct ftl_reloc			*reloc;

	/* Thread on which the poller is running */
	struct spdk_thread			*core_thread;
	/* IO channel */
	struct spdk_io_channel			*ioch;
	/* Poller */
	struct spdk_poller			*core_poller;

	/* IO channel array provides means for retrieving write buffer entries
	 * from their address stored in L2P.  The address is divided into two
	 * parts - IO channel offset poining at specific IO channel (within this
	 * array) and entry offset pointing at specific entry within that IO
	 * channel.
	 */
	struct ftl_io_channel			**ioch_array;
	TAILQ_HEAD(, ftl_io_channel)		ioch_queue;
	uint64_t				num_io_channels;
	/* Value required to shift address of a write buffer entry to retrieve
	 * the IO channel it's part of.  The other part of the address describes
	 * the offset of an entry within the IO channel's entry array.
	 */
	uint64_t				ioch_shift;

	/* Write buffer batches */
#define FTL_BATCH_COUNT 4096
	struct ftl_batch			batch_array[FTL_BATCH_COUNT];
	/* Iovec buffer used by batches */
	struct iovec				*iov_buf;
	/* Batch currently being filled  */
	struct ftl_batch			*current_batch;
	/* Full and ready to be sent batches. A batch is put on this queue in
	 * case it's already filled, but cannot be sent.
	 */
	TAILQ_HEAD(, ftl_batch)			pending_batches;
	TAILQ_HEAD(, ftl_batch)			free_batches;

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

struct ftl_media_event {
	/* Owner */
	struct spdk_ftl_dev			*dev;
	/* Media event */
	struct spdk_bdev_media_event		event;
};

typedef void (*ftl_restore_fn)(struct ftl_restore *, int, void *cb_arg);

void	ftl_apply_limits(struct spdk_ftl_dev *dev);
void	ftl_io_read(struct ftl_io *io);
void	ftl_io_write(struct ftl_io *io);
int	ftl_flush_wbuf(struct spdk_ftl_dev *dev, spdk_ftl_fn cb_fn, void *cb_arg);
int	ftl_current_limit(const struct spdk_ftl_dev *dev);
int	ftl_invalidate_addr(struct spdk_ftl_dev *dev, struct ftl_addr addr);
int	ftl_task_core(void *ctx);
int	ftl_task_read(void *ctx);
void	ftl_process_anm_event(struct ftl_anm_event *event);
size_t	ftl_tail_md_num_blocks(const struct spdk_ftl_dev *dev);
size_t	ftl_tail_md_hdr_num_blocks(void);
size_t	ftl_vld_map_num_blocks(const struct spdk_ftl_dev *dev);
size_t	ftl_lba_map_num_blocks(const struct spdk_ftl_dev *dev);
size_t	ftl_head_md_num_blocks(const struct spdk_ftl_dev *dev);
int	ftl_restore_md(struct spdk_ftl_dev *dev, ftl_restore_fn cb, void *cb_arg);
int	ftl_restore_device(struct ftl_restore *restore, ftl_restore_fn cb, void *cb_arg);
void	ftl_restore_nv_cache(struct ftl_restore *restore, ftl_restore_fn cb, void *cb_arg);
int	ftl_band_set_direct_access(struct ftl_band *band, bool access);
bool	ftl_addr_is_written(struct ftl_band *band, struct ftl_addr addr);
int	ftl_flush_active_bands(struct spdk_ftl_dev *dev, spdk_ftl_fn cb_fn, void *cb_arg);
int	ftl_nv_cache_write_header(struct ftl_nv_cache *nv_cache, bool shutdown,
				  spdk_bdev_io_completion_cb cb_fn, void *cb_arg);
int	ftl_nv_cache_scrub(struct ftl_nv_cache *nv_cache, spdk_bdev_io_completion_cb cb_fn,
			   void *cb_arg);
void	ftl_get_media_events(struct spdk_ftl_dev *dev);
int	ftl_io_channel_poll(void *arg);
void	ftl_evict_cache_entry(struct spdk_ftl_dev *dev, struct ftl_wbuf_entry *entry);
struct spdk_io_channel *ftl_get_io_channel(const struct spdk_ftl_dev *dev);
struct ftl_io_channel *ftl_io_channel_get_ctx(struct spdk_io_channel *ioch);


#define ftl_to_addr(address) \
	(struct ftl_addr) { .offset = (uint64_t)(address) }

#define ftl_to_addr_packed(address) \
	(struct ftl_addr) { .pack.offset = (uint32_t)(address) }

static inline struct spdk_thread *
ftl_get_core_thread(const struct spdk_ftl_dev *dev)
{
	return dev->core_thread;
}

static inline size_t
ftl_get_num_bands(const struct spdk_ftl_dev *dev)
{
	return dev->num_bands;
}

static inline size_t
ftl_get_num_punits(const struct spdk_ftl_dev *dev)
{
	return spdk_bdev_get_optimal_open_zones(spdk_bdev_desc_get_bdev(dev->base_bdev_desc));
}

static inline size_t
ftl_get_num_zones(const struct spdk_ftl_dev *dev)
{
	return ftl_get_num_bands(dev) * ftl_get_num_punits(dev);
}

static inline size_t
ftl_get_num_blocks_in_zone(const struct spdk_ftl_dev *dev)
{
	return spdk_bdev_get_zone_size(spdk_bdev_desc_get_bdev(dev->base_bdev_desc));
}

static inline uint64_t
ftl_get_num_blocks_in_band(const struct spdk_ftl_dev *dev)
{
	return ftl_get_num_punits(dev) * ftl_get_num_blocks_in_zone(dev);
}

static inline uint64_t
ftl_addr_get_zone_slba(const struct spdk_ftl_dev *dev, struct ftl_addr addr)
{
	return addr.offset -= (addr.offset % ftl_get_num_blocks_in_zone(dev));
}

static inline uint64_t
ftl_addr_get_band(const struct spdk_ftl_dev *dev, struct ftl_addr addr)
{
	return addr.offset / ftl_get_num_blocks_in_band(dev);
}

static inline uint64_t
ftl_addr_get_punit(const struct spdk_ftl_dev *dev, struct ftl_addr addr)
{
	return (addr.offset / ftl_get_num_blocks_in_zone(dev)) % ftl_get_num_punits(dev);
}

static inline uint64_t
ftl_addr_get_zone_offset(const struct spdk_ftl_dev *dev, struct ftl_addr addr)
{
	return addr.offset % ftl_get_num_blocks_in_zone(dev);
}

static inline size_t
ftl_vld_map_size(const struct spdk_ftl_dev *dev)
{
	return (size_t)spdk_divide_round_up(ftl_get_num_blocks_in_band(dev), CHAR_BIT);
}

static inline int
ftl_addr_packed(const struct spdk_ftl_dev *dev)
{
	return dev->addr_len < 32;
}

static inline void
ftl_l2p_lba_persist(const struct spdk_ftl_dev *dev, uint64_t lba)
{
#ifdef SPDK_CONFIG_PMDK
	size_t ftl_addr_size = ftl_addr_packed(dev) ? 4 : 8;
	pmem_persist((char *)dev->l2p + (lba * ftl_addr_size), ftl_addr_size);
#else /* SPDK_CONFIG_PMDK */
	SPDK_ERRLOG("Libpmem not available, cannot flush l2p to pmem\n");
	assert(0);
#endif /* SPDK_CONFIG_PMDK */
}

static inline int
ftl_addr_invalid(struct ftl_addr addr)
{
	return addr.offset == ftl_to_addr(FTL_ADDR_INVALID).offset;
}

static inline int
ftl_addr_cached(struct ftl_addr addr)
{
	return !ftl_addr_invalid(addr) && addr.cached;
}

static inline struct ftl_addr
ftl_addr_to_packed(const struct spdk_ftl_dev *dev, struct ftl_addr addr)
{
	struct ftl_addr p = {};

	if (ftl_addr_invalid(addr)) {
		p = ftl_to_addr_packed(FTL_ADDR_INVALID);
	} else if (ftl_addr_cached(addr)) {
		p.pack.cached = 1;
		p.pack.cache_offset = (uint32_t) addr.cache_offset;
	} else {
		p.pack.offset = (uint32_t) addr.offset;
	}

	return p;
}

static inline struct ftl_addr
ftl_addr_from_packed(const struct spdk_ftl_dev *dev, struct ftl_addr p)
{
	struct ftl_addr addr = {};

	if (p.pack.offset == (uint32_t)FTL_ADDR_INVALID) {
		addr = ftl_to_addr(FTL_ADDR_INVALID);
	} else if (p.pack.cached) {
		addr.cached = 1;
		addr.cache_offset = p.pack.cache_offset;
	} else {
		addr = p;
	}

	return addr;
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

#define ftl_addr_cmp(p1, p2) \
	((p1).offset == (p2).offset)

static inline void
ftl_l2p_set(struct spdk_ftl_dev *dev, uint64_t lba, struct ftl_addr addr)
{
	assert(dev->num_lbas > lba);

	if (ftl_addr_packed(dev)) {
		_ftl_l2p_set32(dev->l2p, lba, ftl_addr_to_packed(dev, addr).offset);
	} else {
		_ftl_l2p_set64(dev->l2p, lba, addr.offset);
	}

	if (dev->l2p_pmem_len != 0) {
		ftl_l2p_lba_persist(dev, lba);
	}
}

static inline struct ftl_addr
ftl_l2p_get(struct spdk_ftl_dev *dev, uint64_t lba)
{
	assert(dev->num_lbas > lba);

	if (ftl_addr_packed(dev)) {
		return ftl_addr_from_packed(dev, ftl_to_addr_packed(
						    _ftl_l2p_get32(dev->l2p, lba)));
	} else {
		return ftl_to_addr(_ftl_l2p_get64(dev->l2p, lba));
	}
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

static inline bool
ftl_is_append_supported(const struct spdk_ftl_dev *dev)
{
	return dev->conf.use_append;
}

#endif /* FTL_CORE_H */
