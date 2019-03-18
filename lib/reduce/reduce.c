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

#include "spdk/stdinc.h"

#include "spdk/reduce.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/bit_array.h"
#include "spdk/util.h"
#include "spdk_internal/log.h"

#include "libpmem.h"

/* Always round up the size of the PM region to the nearest cacheline. */
#define REDUCE_PM_SIZE_ALIGNMENT	64

/* Offset into the backing device where the persistent memory file's path is stored. */
#define REDUCE_BACKING_DEV_PATH_OFFSET	4096

#define REDUCE_EMPTY_MAP_ENTRY	-1ULL

#define REDUCE_NUM_VOL_REQUESTS	256

/* Structure written to offset 0 of both the pm file and the backing device. */
struct spdk_reduce_vol_superblock {
	uint8_t				signature[8];
	struct spdk_reduce_vol_params	params;
	uint8_t				reserved[4048];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_reduce_vol_superblock) == 4096, "size incorrect");

#define SPDK_REDUCE_SIGNATURE "SPDKREDU"
/* null terminator counts one */
SPDK_STATIC_ASSERT(sizeof(SPDK_REDUCE_SIGNATURE) - 1 ==
		   sizeof(((struct spdk_reduce_vol_superblock *)0)->signature), "size incorrect");

#define REDUCE_PATH_MAX 4096

/**
 * Describes a persistent memory file used to hold metadata associated with a
 *  compressed volume.
 */
struct spdk_reduce_pm_file {
	char			path[REDUCE_PATH_MAX];
	void			*pm_buf;
	int			pm_is_pmem;
	uint64_t		size;
};

#define REDUCE_IO_READV		1
#define REDUCE_IO_WRITEV	2

struct spdk_reduce_vol_request {
	/**
	 *  Scratch buffer used for read/modify/write operations on
	 *  I/Os less than a full chunk size, and as the intermediate
	 *  buffer for compress/decompress operations.
	 */
	uint8_t					*buf;
	struct iovec				*buf_iov;
	struct iovec				*iov;
	struct spdk_reduce_vol			*vol;
	int					type;
	int					reduce_errno;
	int					iovcnt;
	int					num_backing_ops;
	uint64_t				offset;
	uint64_t				logical_map_index;
	uint64_t				length;
	uint64_t				chunk_map_index;
	uint64_t				*chunk;
	spdk_reduce_vol_op_complete		cb_fn;
	void					*cb_arg;
	TAILQ_ENTRY(spdk_reduce_vol_request)	tailq;
	struct spdk_reduce_vol_cb_args		backing_cb_args;
};

struct spdk_reduce_vol {
	struct spdk_reduce_vol_params		params;
	uint32_t				backing_io_units_per_chunk;
	uint32_t				backing_lba_per_io_unit;
	uint32_t				logical_blocks_per_chunk;
	struct spdk_reduce_pm_file		pm_file;
	struct spdk_reduce_backing_dev		*backing_dev;
	struct spdk_reduce_vol_superblock	*backing_super;
	struct spdk_reduce_vol_superblock	*pm_super;
	uint64_t				*pm_logical_map;
	uint64_t				*pm_chunk_maps;

	struct spdk_bit_array			*allocated_chunk_maps;
	struct spdk_bit_array			*allocated_backing_io_units;

	struct spdk_reduce_vol_request		*request_mem;
	TAILQ_HEAD(, spdk_reduce_vol_request)	free_requests;
	TAILQ_HEAD(, spdk_reduce_vol_request)	executing_requests;
	TAILQ_HEAD(, spdk_reduce_vol_request)	queued_requests;

	/* Single contiguous buffer used for all request buffers for this volume. */
	uint8_t					*reqbufspace;
	struct iovec				*buf_iov_mem;
};

static void _start_readv_request(struct spdk_reduce_vol_request *req);
static void _start_writev_request(struct spdk_reduce_vol_request *req);

/*
 * Allocate extra metadata chunks and corresponding backing io units to account for
 *  outstanding IO in worst case scenario where logical map is completely allocated
 *  and no data can be compressed.  We need extra chunks in this case to handle
 *  in-flight writes since reduce never writes data in place.
 */
#define REDUCE_NUM_EXTRA_CHUNKS 128

static void
_reduce_persist(struct spdk_reduce_vol *vol, const void *addr, size_t len)
{
	if (vol->pm_file.pm_is_pmem) {
		pmem_persist(addr, len);
	} else {
		pmem_msync(addr, len);
	}
}

static uint64_t
_get_pm_logical_map_size(uint64_t vol_size, uint64_t chunk_size)
{
	uint64_t chunks_in_logical_map, logical_map_size;

	chunks_in_logical_map = vol_size / chunk_size;
	logical_map_size = chunks_in_logical_map * sizeof(uint64_t);

	/* Round up to next cacheline. */
	return spdk_divide_round_up(logical_map_size, REDUCE_PM_SIZE_ALIGNMENT) *
	       REDUCE_PM_SIZE_ALIGNMENT;
}

static uint64_t
_get_total_chunks(uint64_t vol_size, uint64_t chunk_size)
{
	uint64_t num_chunks;

	num_chunks = vol_size / chunk_size;
	num_chunks += REDUCE_NUM_EXTRA_CHUNKS;

	return num_chunks;
}

static uint64_t
_get_pm_total_chunks_size(uint64_t vol_size, uint64_t chunk_size, uint64_t backing_io_unit_size)
{
	uint64_t io_units_per_chunk, num_chunks, total_chunks_size;

	num_chunks = _get_total_chunks(vol_size, chunk_size);
	io_units_per_chunk = chunk_size / backing_io_unit_size;
	total_chunks_size = num_chunks * io_units_per_chunk * sizeof(uint64_t);

	return spdk_divide_round_up(total_chunks_size, REDUCE_PM_SIZE_ALIGNMENT) *
	       REDUCE_PM_SIZE_ALIGNMENT;
}

static uint64_t *
_reduce_vol_get_chunk_map(struct spdk_reduce_vol *vol, uint64_t chunk_map_index)
{
	assert(chunk_map_index < _get_total_chunks(vol->params.vol_size, vol->params.chunk_size));

	return vol->pm_chunk_maps + (chunk_map_index * vol->backing_io_units_per_chunk);
}

static int
_validate_vol_params(struct spdk_reduce_vol_params *params)
{
	if (params->vol_size > 0) {
		/**
		 * User does not pass in the vol size - it gets calculated by libreduce from
		 *  values in this structure plus the size of the backing device.
		 */
		return -EINVAL;
	}

	if (params->chunk_size == 0 || params->backing_io_unit_size == 0 ||
	    params->logical_block_size == 0) {
		return -EINVAL;
	}

	/* Chunk size must be an even multiple of the backing io unit size. */
	if ((params->chunk_size % params->backing_io_unit_size) != 0) {
		return -EINVAL;
	}

	/* Chunk size must be an even multiple of the logical block size. */
	if ((params->chunk_size % params->logical_block_size) != 0) {
		return -1;
	}

	return 0;
}

static uint64_t
_get_vol_size(uint64_t chunk_size, uint64_t backing_dev_size)
{
	uint64_t num_chunks;

	num_chunks = backing_dev_size / chunk_size;
	if (num_chunks <= REDUCE_NUM_EXTRA_CHUNKS) {
		return 0;
	}

	num_chunks -= REDUCE_NUM_EXTRA_CHUNKS;
	return num_chunks * chunk_size;
}

static uint64_t
_get_pm_file_size(struct spdk_reduce_vol_params *params)
{
	uint64_t total_pm_size;

	total_pm_size = sizeof(struct spdk_reduce_vol_superblock);
	total_pm_size += _get_pm_logical_map_size(params->vol_size, params->chunk_size);
	total_pm_size += _get_pm_total_chunks_size(params->vol_size, params->chunk_size,
			 params->backing_io_unit_size);
	return total_pm_size;
}

const struct spdk_uuid *
spdk_reduce_vol_get_uuid(struct spdk_reduce_vol *vol)
{
	return &vol->params.uuid;
}

static void
_initialize_vol_pm_pointers(struct spdk_reduce_vol *vol)
{
	/* Superblock is at the beginning of the pm file. */
	vol->pm_super = (struct spdk_reduce_vol_superblock *)vol->pm_file.pm_buf;

	/* Logical map immediately follows the super block. */
	vol->pm_logical_map = (uint64_t *)(vol->pm_super + 1);

	/* Chunks maps follow the logical map. */
	vol->pm_chunk_maps = vol->pm_logical_map + (vol->params.vol_size / vol->params.chunk_size);
}

/* We need 2 iovs during load - one for the superblock, another for the path */
#define LOAD_IOV_COUNT	2

struct reduce_init_load_ctx {
	struct spdk_reduce_vol			*vol;
	struct spdk_reduce_vol_cb_args		backing_cb_args;
	spdk_reduce_vol_op_with_handle_complete	cb_fn;
	void					*cb_arg;
	struct iovec				iov[LOAD_IOV_COUNT];
	void					*path;
};

static int
_allocate_vol_requests(struct spdk_reduce_vol *vol)
{
	struct spdk_reduce_vol_request *req;
	int i;

	vol->reqbufspace = spdk_dma_malloc(REDUCE_NUM_VOL_REQUESTS * vol->params.chunk_size, 64, NULL);
	if (vol->reqbufspace == NULL) {
		return -ENOMEM;
	}

	vol->request_mem = calloc(REDUCE_NUM_VOL_REQUESTS, sizeof(*req));
	if (vol->request_mem == NULL) {
		spdk_dma_free(vol->reqbufspace);
		vol->reqbufspace = NULL;
		return -ENOMEM;
	}

	vol->buf_iov_mem = calloc(REDUCE_NUM_VOL_REQUESTS,
				  sizeof(struct iovec) * vol->backing_io_units_per_chunk);
	if (vol->buf_iov_mem == NULL) {
		free(vol->request_mem);
		spdk_dma_free(vol->reqbufspace);
		vol->request_mem = NULL;
		vol->reqbufspace = NULL;
		return -ENOMEM;
	}

	for (i = 0; i < REDUCE_NUM_VOL_REQUESTS; i++) {
		req = &vol->request_mem[i];
		TAILQ_INSERT_HEAD(&vol->free_requests, req, tailq);
		req->buf_iov = &vol->buf_iov_mem[i * vol->backing_io_units_per_chunk];
		req->buf = vol->reqbufspace + i * vol->params.chunk_size;
	}

	return 0;
}

static void
_init_load_cleanup(struct spdk_reduce_vol *vol, struct reduce_init_load_ctx *ctx)
{
	if (ctx != NULL) {
		spdk_dma_free(ctx->path);
		free(ctx);
	}

	if (vol != NULL) {
		pmem_unmap(vol->pm_file.pm_buf, vol->pm_file.size);
		spdk_dma_free(vol->backing_super);
		spdk_bit_array_free(&vol->allocated_chunk_maps);
		spdk_bit_array_free(&vol->allocated_backing_io_units);
		free(vol->request_mem);
		free(vol->buf_iov_mem);
		spdk_dma_free(vol->reqbufspace);
		free(vol);
	}
}

static void
_init_write_super_cpl(void *cb_arg, int reduce_errno)
{
	struct reduce_init_load_ctx *init_ctx = cb_arg;
	int rc;

	rc = _allocate_vol_requests(init_ctx->vol);
	if (rc != 0) {
		init_ctx->cb_fn(init_ctx->cb_arg, NULL, rc);
		_init_load_cleanup(init_ctx->vol, init_ctx);
		return;
	}

	init_ctx->cb_fn(init_ctx->cb_arg, init_ctx->vol, reduce_errno);
	/* Only clean up the ctx - the vol has been passed to the application
	 *  for use now that initialization was successful.
	 */
	_init_load_cleanup(NULL, init_ctx);
}

static void
_init_write_path_cpl(void *cb_arg, int reduce_errno)
{
	struct reduce_init_load_ctx *init_ctx = cb_arg;
	struct spdk_reduce_vol *vol = init_ctx->vol;

	init_ctx->iov[0].iov_base = vol->backing_super;
	init_ctx->iov[0].iov_len = sizeof(*vol->backing_super);
	init_ctx->backing_cb_args.cb_fn = _init_write_super_cpl;
	init_ctx->backing_cb_args.cb_arg = init_ctx;
	vol->backing_dev->writev(vol->backing_dev, init_ctx->iov, 1,
				 0, sizeof(*vol->backing_super) / vol->backing_dev->blocklen,
				 &init_ctx->backing_cb_args);
}

static int
_allocate_bit_arrays(struct spdk_reduce_vol *vol)
{
	uint64_t total_chunks, total_backing_io_units;
	uint32_t i, num_metadata_io_units;

	total_chunks = _get_total_chunks(vol->params.vol_size, vol->params.chunk_size);
	vol->allocated_chunk_maps = spdk_bit_array_create(total_chunks);
	total_backing_io_units = total_chunks * (vol->params.chunk_size / vol->params.backing_io_unit_size);
	vol->allocated_backing_io_units = spdk_bit_array_create(total_backing_io_units);

	if (vol->allocated_chunk_maps == NULL || vol->allocated_backing_io_units == NULL) {
		return -ENOMEM;
	}

	/* Set backing io unit bits associated with metadata. */
	num_metadata_io_units = (sizeof(*vol->backing_super) + REDUCE_PATH_MAX) /
				vol->backing_dev->blocklen;
	for (i = 0; i < num_metadata_io_units; i++) {
		spdk_bit_array_set(vol->allocated_backing_io_units, i);
	}

	return 0;
}

void
spdk_reduce_vol_init(struct spdk_reduce_vol_params *params,
		     struct spdk_reduce_backing_dev *backing_dev,
		     const char *pm_file_dir,
		     spdk_reduce_vol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_reduce_vol *vol;
	struct reduce_init_load_ctx *init_ctx;
	uint64_t backing_dev_size;
	size_t mapped_len;
	int dir_len, max_dir_len, rc;

	/* We need to append a path separator and the UUID to the supplied
	 * path.
	 */
	max_dir_len = REDUCE_PATH_MAX - SPDK_UUID_STRING_LEN - 1;
	dir_len = strnlen(pm_file_dir, max_dir_len);
	/* Strip trailing slash if the user provided one - we will add it back
	 * later when appending the filename.
	 */
	if (pm_file_dir[dir_len - 1] == '/') {
		dir_len--;
	}
	if (dir_len == max_dir_len) {
		SPDK_ERRLOG("pm_file_dir (%s) too long\n", pm_file_dir);
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	rc = _validate_vol_params(params);
	if (rc != 0) {
		SPDK_ERRLOG("invalid vol params\n");
		cb_fn(cb_arg, NULL, rc);
		return;
	}

	backing_dev_size = backing_dev->blockcnt * backing_dev->blocklen;
	params->vol_size = _get_vol_size(params->chunk_size, backing_dev_size);
	if (params->vol_size == 0) {
		SPDK_ERRLOG("backing device is too small\n");
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	if (backing_dev->readv == NULL || backing_dev->writev == NULL ||
	    backing_dev->unmap == NULL) {
		SPDK_ERRLOG("backing_dev function pointer not specified\n");
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	vol = calloc(1, sizeof(*vol));
	if (vol == NULL) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	TAILQ_INIT(&vol->free_requests);
	TAILQ_INIT(&vol->executing_requests);
	TAILQ_INIT(&vol->queued_requests);

	vol->backing_super = spdk_dma_zmalloc(sizeof(*vol->backing_super), 0, NULL);
	if (vol->backing_super == NULL) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		_init_load_cleanup(vol, NULL);
		return;
	}

	init_ctx = calloc(1, sizeof(*init_ctx));
	if (init_ctx == NULL) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		_init_load_cleanup(vol, NULL);
		return;
	}

	init_ctx->path = spdk_dma_zmalloc(REDUCE_PATH_MAX, 0, NULL);
	if (init_ctx->path == NULL) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		_init_load_cleanup(vol, init_ctx);
		return;
	}

	if (spdk_mem_all_zero(&params->uuid, sizeof(params->uuid))) {
		spdk_uuid_generate(&params->uuid);
	}

	memcpy(vol->pm_file.path, pm_file_dir, dir_len);
	vol->pm_file.path[dir_len] = '/';
	spdk_uuid_fmt_lower(&vol->pm_file.path[dir_len + 1], SPDK_UUID_STRING_LEN,
			    &params->uuid);
	vol->pm_file.size = _get_pm_file_size(params);
	vol->pm_file.pm_buf = pmem_map_file(vol->pm_file.path, vol->pm_file.size,
					    PMEM_FILE_CREATE | PMEM_FILE_EXCL, 0600,
					    &mapped_len, &vol->pm_file.pm_is_pmem);
	if (vol->pm_file.pm_buf == NULL) {
		SPDK_ERRLOG("could not pmem_map_file(%s): %s\n",
			    vol->pm_file.path, strerror(errno));
		cb_fn(cb_arg, NULL, -errno);
		_init_load_cleanup(vol, init_ctx);
		return;
	}

	if (vol->pm_file.size != mapped_len) {
		SPDK_ERRLOG("could not map entire pmem file (size=%" PRIu64 " mapped=%" PRIu64 ")\n",
			    vol->pm_file.size, mapped_len);
		cb_fn(cb_arg, NULL, -ENOMEM);
		_init_load_cleanup(vol, init_ctx);
		return;
	}

	vol->backing_io_units_per_chunk = params->chunk_size / params->backing_io_unit_size;
	vol->logical_blocks_per_chunk = params->chunk_size / params->logical_block_size;
	vol->backing_lba_per_io_unit = params->backing_io_unit_size / backing_dev->blocklen;
	memcpy(&vol->params, params, sizeof(*params));

	vol->backing_dev = backing_dev;

	rc = _allocate_bit_arrays(vol);
	if (rc != 0) {
		cb_fn(cb_arg, NULL, rc);
		_init_load_cleanup(vol, init_ctx);
		return;
	}

	memcpy(vol->backing_super->signature, SPDK_REDUCE_SIGNATURE,
	       sizeof(vol->backing_super->signature));
	memcpy(&vol->backing_super->params, params, sizeof(*params));

	_initialize_vol_pm_pointers(vol);

	memcpy(vol->pm_super, vol->backing_super, sizeof(*vol->backing_super));
	/* Writing 0xFF's is equivalent of filling it all with SPDK_EMPTY_MAP_ENTRY.
	 * Note that this writes 0xFF to not just the logical map but the chunk maps as well.
	 */
	memset(vol->pm_logical_map, 0xFF, vol->pm_file.size - sizeof(*vol->backing_super));
	_reduce_persist(vol, vol->pm_file.pm_buf, vol->pm_file.size);

	init_ctx->vol = vol;
	init_ctx->cb_fn = cb_fn;
	init_ctx->cb_arg = cb_arg;

	memcpy(init_ctx->path, vol->pm_file.path, REDUCE_PATH_MAX);
	init_ctx->iov[0].iov_base = init_ctx->path;
	init_ctx->iov[0].iov_len = REDUCE_PATH_MAX;
	init_ctx->backing_cb_args.cb_fn = _init_write_path_cpl;
	init_ctx->backing_cb_args.cb_arg = init_ctx;
	/* Write path to offset 4K on backing device - just after where the super
	 *  block will be written.  We wait until this is committed before writing the
	 *  super block to guarantee we don't get the super block written without the
	 *  the path if the system crashed in the middle of a write operation.
	 */
	vol->backing_dev->writev(vol->backing_dev, init_ctx->iov, 1,
				 REDUCE_BACKING_DEV_PATH_OFFSET / vol->backing_dev->blocklen,
				 REDUCE_PATH_MAX / vol->backing_dev->blocklen,
				 &init_ctx->backing_cb_args);
}

static void
_load_read_super_and_path_cpl(void *cb_arg, int reduce_errno)
{
	struct reduce_init_load_ctx *load_ctx = cb_arg;
	struct spdk_reduce_vol *vol = load_ctx->vol;
	uint64_t backing_dev_size;
	uint64_t i, num_chunks, logical_map_index;
	uint64_t *chunk;
	size_t mapped_len;
	uint32_t j;
	int rc;

	if (memcmp(vol->backing_super->signature,
		   SPDK_REDUCE_SIGNATURE,
		   sizeof(vol->backing_super->signature)) != 0) {
		/* This backing device isn't a libreduce backing device. */
		rc = -EILSEQ;
		goto error;
	}

	memcpy(&vol->params, &vol->backing_super->params, sizeof(vol->params));
	vol->backing_io_units_per_chunk = vol->params.chunk_size / vol->params.backing_io_unit_size;
	vol->logical_blocks_per_chunk = vol->params.chunk_size / vol->params.logical_block_size;
	vol->backing_lba_per_io_unit = vol->params.backing_io_unit_size / vol->backing_dev->blocklen;

	rc = _allocate_bit_arrays(vol);
	if (rc != 0) {
		goto error;
	}

	backing_dev_size = vol->backing_dev->blockcnt * vol->backing_dev->blocklen;
	if (_get_vol_size(vol->params.chunk_size, backing_dev_size) < vol->params.vol_size) {
		SPDK_ERRLOG("backing device size %" PRIi64 " smaller than expected\n",
			    backing_dev_size);
		rc = -EILSEQ;
		goto error;
	}

	memcpy(vol->pm_file.path, load_ctx->path, sizeof(vol->pm_file.path));
	vol->pm_file.size = _get_pm_file_size(&vol->params);
	vol->pm_file.pm_buf = pmem_map_file(vol->pm_file.path, 0, 0, 0, &mapped_len,
					    &vol->pm_file.pm_is_pmem);
	if (vol->pm_file.pm_buf == NULL) {
		SPDK_ERRLOG("could not pmem_map_file(%s): %s\n", vol->pm_file.path, strerror(errno));
		rc = -errno;
		goto error;
	}

	if (vol->pm_file.size != mapped_len) {
		SPDK_ERRLOG("could not map entire pmem file (size=%" PRIu64 " mapped=%" PRIu64 ")\n",
			    vol->pm_file.size, mapped_len);
		rc = -ENOMEM;
		goto error;
	}

	rc = _allocate_vol_requests(vol);
	if (rc != 0) {
		goto error;
	}

	_initialize_vol_pm_pointers(vol);

	num_chunks = vol->params.vol_size / vol->params.chunk_size;
	for (i = 0; i < num_chunks; i++) {
		logical_map_index = vol->pm_logical_map[i];
		if (logical_map_index == REDUCE_EMPTY_MAP_ENTRY) {
			continue;
		}
		spdk_bit_array_set(vol->allocated_chunk_maps, logical_map_index);
		chunk = _reduce_vol_get_chunk_map(vol, logical_map_index);
		for (j = 0; j < vol->backing_io_units_per_chunk; j++) {
			if (chunk[j] != REDUCE_EMPTY_MAP_ENTRY) {
				spdk_bit_array_set(vol->allocated_backing_io_units, chunk[j]);
			}
		}
	}

	load_ctx->cb_fn(load_ctx->cb_arg, vol, 0);
	/* Only clean up the ctx - the vol has been passed to the application
	 *  for use now that volume load was successful.
	 */
	_init_load_cleanup(NULL, load_ctx);
	return;

error:
	load_ctx->cb_fn(load_ctx->cb_arg, NULL, rc);
	_init_load_cleanup(vol, load_ctx);
}

void
spdk_reduce_vol_load(struct spdk_reduce_backing_dev *backing_dev,
		     spdk_reduce_vol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_reduce_vol *vol;
	struct reduce_init_load_ctx *load_ctx;

	if (backing_dev->readv == NULL || backing_dev->writev == NULL ||
	    backing_dev->unmap == NULL) {
		SPDK_ERRLOG("backing_dev function pointer not specified\n");
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	vol = calloc(1, sizeof(*vol));
	if (vol == NULL) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	TAILQ_INIT(&vol->free_requests);
	TAILQ_INIT(&vol->executing_requests);
	TAILQ_INIT(&vol->queued_requests);

	vol->backing_super = spdk_dma_zmalloc(sizeof(*vol->backing_super), 64, NULL);
	if (vol->backing_super == NULL) {
		_init_load_cleanup(vol, NULL);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	vol->backing_dev = backing_dev;

	load_ctx = calloc(1, sizeof(*load_ctx));
	if (load_ctx == NULL) {
		_init_load_cleanup(vol, NULL);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	load_ctx->path = spdk_dma_zmalloc(REDUCE_PATH_MAX, 64, NULL);
	if (load_ctx->path == NULL) {
		_init_load_cleanup(vol, load_ctx);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	load_ctx->vol = vol;
	load_ctx->cb_fn = cb_fn;
	load_ctx->cb_arg = cb_arg;

	load_ctx->iov[0].iov_base = vol->backing_super;
	load_ctx->iov[0].iov_len = sizeof(*vol->backing_super);
	load_ctx->iov[1].iov_base = load_ctx->path;
	load_ctx->iov[1].iov_len = REDUCE_PATH_MAX;
	load_ctx->backing_cb_args.cb_fn = _load_read_super_and_path_cpl;
	load_ctx->backing_cb_args.cb_arg = load_ctx;
	vol->backing_dev->readv(vol->backing_dev, load_ctx->iov, LOAD_IOV_COUNT, 0,
				(sizeof(*vol->backing_super) + REDUCE_PATH_MAX) /
				vol->backing_dev->blocklen,
				&load_ctx->backing_cb_args);
}

void
spdk_reduce_vol_unload(struct spdk_reduce_vol *vol,
		       spdk_reduce_vol_op_complete cb_fn, void *cb_arg)
{
	if (vol == NULL) {
		/* This indicates a programming error. */
		assert(false);
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	_init_load_cleanup(vol, NULL);
	cb_fn(cb_arg, 0);
}

struct reduce_destroy_ctx {
	spdk_reduce_vol_op_complete		cb_fn;
	void					*cb_arg;
	struct spdk_reduce_vol			*vol;
	struct spdk_reduce_vol_superblock	*super;
	struct iovec				iov;
	struct spdk_reduce_vol_cb_args		backing_cb_args;
	int					reduce_errno;
	char					pm_path[REDUCE_PATH_MAX];
};

static void
destroy_unload_cpl(void *cb_arg, int reduce_errno)
{
	struct reduce_destroy_ctx *destroy_ctx = cb_arg;

	if (destroy_ctx->reduce_errno == 0) {
		if (unlink(destroy_ctx->pm_path)) {
			SPDK_ERRLOG("%s could not be unlinked: %s\n",
				    destroy_ctx->pm_path, strerror(errno));
		}
	}

	/* Even if the unload somehow failed, we still pass the destroy_ctx
	 * reduce_errno since that indicates whether or not the volume was
	 * actually destroyed.
	 */
	destroy_ctx->cb_fn(destroy_ctx->cb_arg, destroy_ctx->reduce_errno);
	spdk_dma_free(destroy_ctx->super);
	free(destroy_ctx);
}

static void
_destroy_zero_super_cpl(void *cb_arg, int reduce_errno)
{
	struct reduce_destroy_ctx *destroy_ctx = cb_arg;
	struct spdk_reduce_vol *vol = destroy_ctx->vol;

	destroy_ctx->reduce_errno = reduce_errno;
	spdk_reduce_vol_unload(vol, destroy_unload_cpl, destroy_ctx);
}

static void
destroy_load_cb(void *cb_arg, struct spdk_reduce_vol *vol, int reduce_errno)
{
	struct reduce_destroy_ctx *destroy_ctx = cb_arg;

	if (reduce_errno != 0) {
		destroy_ctx->cb_fn(destroy_ctx->cb_arg, reduce_errno);
		spdk_dma_free(destroy_ctx->super);
		free(destroy_ctx);
		return;
	}

	destroy_ctx->vol = vol;
	memcpy(destroy_ctx->pm_path, vol->pm_file.path, sizeof(destroy_ctx->pm_path));
	destroy_ctx->iov.iov_base = destroy_ctx->super;
	destroy_ctx->iov.iov_len = sizeof(*destroy_ctx->super);
	destroy_ctx->backing_cb_args.cb_fn = _destroy_zero_super_cpl;
	destroy_ctx->backing_cb_args.cb_arg = destroy_ctx;
	vol->backing_dev->writev(vol->backing_dev, &destroy_ctx->iov, 1, 0,
				 sizeof(*destroy_ctx->super) / vol->backing_dev->blocklen,
				 &destroy_ctx->backing_cb_args);
}

void
spdk_reduce_vol_destroy(struct spdk_reduce_backing_dev *backing_dev,
			spdk_reduce_vol_op_complete cb_fn, void *cb_arg)
{
	struct reduce_destroy_ctx *destroy_ctx;

	destroy_ctx = calloc(1, sizeof(*destroy_ctx));
	if (destroy_ctx == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	destroy_ctx->super = spdk_dma_zmalloc(sizeof(*destroy_ctx->super), 64, NULL);
	if (destroy_ctx->super == NULL) {
		free(destroy_ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	destroy_ctx->cb_fn = cb_fn;
	destroy_ctx->cb_arg = cb_arg;
	spdk_reduce_vol_load(backing_dev, destroy_load_cb, destroy_ctx);
}

static bool
_request_spans_chunk_boundary(struct spdk_reduce_vol *vol, uint64_t offset, uint64_t length)
{
	uint64_t start_chunk, end_chunk;

	start_chunk = offset / vol->logical_blocks_per_chunk;
	end_chunk = (offset + length - 1) / vol->logical_blocks_per_chunk;

	return (start_chunk != end_chunk);
}

typedef void (*reduce_request_fn)(void *_req, int reduce_errno);

static void
_reduce_vol_complete_req(struct spdk_reduce_vol_request *req, int reduce_errno)
{
	struct spdk_reduce_vol_request *next_req;
	struct spdk_reduce_vol *vol = req->vol;

	req->cb_fn(req->cb_arg, reduce_errno);
	TAILQ_FOREACH(next_req, &vol->queued_requests, tailq) {
		if (next_req->logical_map_index == req->logical_map_index) {
			TAILQ_REMOVE(&vol->queued_requests, next_req, tailq);
			break;
		}
	}
	TAILQ_REMOVE(&vol->executing_requests, req, tailq);
	TAILQ_INSERT_HEAD(&vol->free_requests, req, tailq);
	if (next_req == NULL) {
		return;
	}

	if (next_req->type == REDUCE_IO_READV) {
		_start_readv_request(next_req);
	} else {
		assert(next_req->type == REDUCE_IO_WRITEV);
		_start_writev_request(next_req);
	}
}

static void
_write_complete_req(void *_req, int reduce_errno)
{
	struct spdk_reduce_vol_request *req = _req;
	struct spdk_reduce_vol *vol = req->vol;
	uint64_t old_chunk_map_index;
	uint64_t *old_chunk;
	uint32_t i;

	if (reduce_errno != 0) {
		req->reduce_errno = reduce_errno;
	}

	assert(req->num_backing_ops > 0);
	if (--req->num_backing_ops > 0) {
		return;
	}

	if (req->reduce_errno != 0) {
		_reduce_vol_complete_req(req, req->reduce_errno);
		return;
	}

	old_chunk_map_index = vol->pm_logical_map[req->logical_map_index];
	if (old_chunk_map_index != REDUCE_EMPTY_MAP_ENTRY) {
		old_chunk = _reduce_vol_get_chunk_map(vol, old_chunk_map_index);
		for (i = 0; i < vol->backing_io_units_per_chunk; i++) {
			if (old_chunk[i] == REDUCE_EMPTY_MAP_ENTRY) {
				break;
			}
			assert(spdk_bit_array_get(vol->allocated_backing_io_units, old_chunk[i]) == true);
			spdk_bit_array_clear(vol->allocated_backing_io_units, old_chunk[i]);
			old_chunk[i] = REDUCE_EMPTY_MAP_ENTRY;
		}
		spdk_bit_array_clear(vol->allocated_chunk_maps, old_chunk_map_index);
	}

	/*
	 * We don't need to persist the clearing of the old chunk map here.  The old chunk map
	 * becomes invalid after we update the logical map, since the old chunk map will no
	 * longer have a reference to it in the logical map.
	 */

	/* Persist the new chunk map.  This must be persisted before we update the logical map. */
	_reduce_persist(vol, req->chunk, sizeof(uint64_t) * vol->backing_io_units_per_chunk);

	vol->pm_logical_map[req->logical_map_index] = req->chunk_map_index;

	_reduce_persist(vol, &vol->pm_logical_map[req->logical_map_index], sizeof(uint64_t));

	_reduce_vol_complete_req(req, 0);
}

static void
_issue_backing_ops(struct spdk_reduce_vol_request *req, struct spdk_reduce_vol *vol,
		   reduce_request_fn next_fn, bool is_write)
{
	uint32_t i;

	req->num_backing_ops = vol->backing_io_units_per_chunk;
	req->backing_cb_args.cb_fn = next_fn;
	req->backing_cb_args.cb_arg = req;
	for (i = 0; i < vol->backing_io_units_per_chunk; i++) {
		req->buf_iov[i].iov_base = req->buf + i * vol->params.backing_io_unit_size;
		req->buf_iov[i].iov_len = vol->params.backing_io_unit_size;
		if (is_write) {
			vol->backing_dev->writev(vol->backing_dev, &req->buf_iov[i], 1,
						 req->chunk[i] * vol->backing_lba_per_io_unit,
						 vol->backing_lba_per_io_unit, &req->backing_cb_args);
		} else {
			vol->backing_dev->readv(vol->backing_dev, &req->buf_iov[i], 1,
						req->chunk[i] * vol->backing_lba_per_io_unit,
						vol->backing_lba_per_io_unit, &req->backing_cb_args);
		}
	}
}

static void
_reduce_vol_write_chunk(struct spdk_reduce_vol_request *req, reduce_request_fn next_fn)
{
	struct spdk_reduce_vol *vol = req->vol;
	uint32_t i;

	req->chunk_map_index = spdk_bit_array_find_first_clear(vol->allocated_chunk_maps, 0);

	/* TODO: fail if no chunk map found - but really this should not happen if we
	 * size the number of requests similarly to number of extra chunk maps
	 */
	assert(req->chunk_map_index != UINT32_MAX);
	spdk_bit_array_set(vol->allocated_chunk_maps, req->chunk_map_index);

	req->chunk = _reduce_vol_get_chunk_map(vol, req->chunk_map_index);

	for (i = 0; i < vol->backing_io_units_per_chunk; i++) {
		req->chunk[i] = spdk_bit_array_find_first_clear(vol->allocated_backing_io_units, 0);
		/* TODO: fail if no backing block found - but really this should also not
		 * happen (see comment above).
		 */
		assert(req->chunk[i] != UINT32_MAX);
		spdk_bit_array_set(vol->allocated_backing_io_units, req->chunk[i]);
	}

	_issue_backing_ops(req, vol, next_fn, true /* write */);
}

static void
_write_read_done(void *_req, int reduce_errno)
{
	struct spdk_reduce_vol_request *req = _req;
	uint64_t chunk_offset;
	uint8_t *buf;
	int i;

	if (reduce_errno != 0) {
		req->reduce_errno = reduce_errno;
	}

	assert(req->num_backing_ops > 0);
	if (--req->num_backing_ops > 0) {
		return;
	}

	if (req->reduce_errno != 0) {
		_reduce_vol_complete_req(req, req->reduce_errno);
		return;
	}

	chunk_offset = req->offset % req->vol->logical_blocks_per_chunk;
	buf = req->buf + chunk_offset * req->vol->params.logical_block_size;
	for (i = 0; i < req->iovcnt; i++) {
		memcpy(buf, req->iov[i].iov_base, req->iov[i].iov_len);
		buf += req->iov[i].iov_len;
	}

	_reduce_vol_write_chunk(req, _write_complete_req);
}

static void
_read_read_done(void *_req, int reduce_errno)
{
	struct spdk_reduce_vol_request *req = _req;
	uint64_t chunk_offset;
	uint8_t *buf;
	int i;

	if (reduce_errno != 0) {
		req->reduce_errno = reduce_errno;
	}

	assert(req->num_backing_ops > 0);
	if (--req->num_backing_ops > 0) {
		return;
	}

	if (req->reduce_errno != 0) {
		_reduce_vol_complete_req(req, req->reduce_errno);
		return;
	}

	chunk_offset = req->offset % req->vol->logical_blocks_per_chunk;
	buf = req->buf + chunk_offset * req->vol->params.logical_block_size;
	for (i = 0; i < req->iovcnt; i++) {
		memcpy(req->iov[i].iov_base, buf, req->iov[i].iov_len);
		buf += req->iov[i].iov_len;
	}
	_reduce_vol_complete_req(req, 0);
}

static void
_reduce_vol_read_chunk(struct spdk_reduce_vol_request *req, reduce_request_fn next_fn)
{
	struct spdk_reduce_vol *vol = req->vol;

	req->chunk_map_index = vol->pm_logical_map[req->logical_map_index];
	assert(req->chunk_map_index != UINT32_MAX);

	req->chunk = _reduce_vol_get_chunk_map(vol, req->chunk_map_index);
	_issue_backing_ops(req, vol, next_fn, false /* read */);
}

static bool
_iov_array_is_valid(struct spdk_reduce_vol *vol, struct iovec *iov, int iovcnt,
		    uint64_t length)
{
	uint64_t size = 0;
	int i;

	for (i = 0; i < iovcnt; i++) {
		size += iov[i].iov_len;
	}

	return size == (length * vol->params.logical_block_size);
}

static bool
_check_overlap(struct spdk_reduce_vol *vol, uint64_t logical_map_index)
{
	struct spdk_reduce_vol_request *req;

	TAILQ_FOREACH(req, &vol->executing_requests, tailq) {
		if (logical_map_index == req->logical_map_index) {
			return true;
		}
	}

	return false;
}

static void
_start_readv_request(struct spdk_reduce_vol_request *req)
{
	TAILQ_INSERT_TAIL(&req->vol->executing_requests, req, tailq);
	_reduce_vol_read_chunk(req, _read_read_done);
}

void
spdk_reduce_vol_readv(struct spdk_reduce_vol *vol,
		      struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
		      spdk_reduce_vol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_reduce_vol_request *req;
	uint64_t logical_map_index;
	bool overlapped;
	int i;

	if (length == 0) {
		cb_fn(cb_arg, 0);
		return;
	}

	if (_request_spans_chunk_boundary(vol, offset, length)) {
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	if (!_iov_array_is_valid(vol, iov, iovcnt, length)) {
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	logical_map_index = offset / vol->logical_blocks_per_chunk;
	overlapped = _check_overlap(vol, logical_map_index);

	if (!overlapped && vol->pm_logical_map[logical_map_index] == REDUCE_EMPTY_MAP_ENTRY) {
		/*
		 * This chunk hasn't been allocated.  So treat the data as all
		 * zeroes for this chunk - do the memset and immediately complete
		 * the operation.
		 */
		for (i = 0; i < iovcnt; i++) {
			memset(iov[i].iov_base, 0, iov[i].iov_len);
		}
		cb_fn(cb_arg, 0);
		return;
	}

	req = TAILQ_FIRST(&vol->free_requests);
	if (req == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	TAILQ_REMOVE(&vol->free_requests, req, tailq);
	req->type = REDUCE_IO_READV;
	req->vol = vol;
	req->iov = iov;
	req->iovcnt = iovcnt;
	req->offset = offset;
	req->logical_map_index = logical_map_index;
	req->length = length;
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	if (!overlapped) {
		_start_readv_request(req);
	} else {
		TAILQ_INSERT_TAIL(&vol->queued_requests, req, tailq);
	}
}

static void
_start_writev_request(struct spdk_reduce_vol_request *req)
{
	struct spdk_reduce_vol *vol = req->vol;
	uint64_t chunk_offset;
	uint32_t lbsize, lb_per_chunk;
	int i;
	uint8_t *buf;

	TAILQ_INSERT_TAIL(&req->vol->executing_requests, req, tailq);
	if (vol->pm_logical_map[req->logical_map_index] != REDUCE_EMPTY_MAP_ENTRY) {
		/* Read old chunk, then overwrite with data from this write operation.
		 * TODO: bypass reading old chunk if this write operation overwrites
		 * the entire chunk.
		 */
		_reduce_vol_read_chunk(req, _write_read_done);
		return;
	}

	buf = req->buf;
	lbsize = vol->params.logical_block_size;
	lb_per_chunk = vol->logical_blocks_per_chunk;
	/* Note: we must zero out parts of req->buf not specified by this write operation. */
	chunk_offset = req->offset % lb_per_chunk;
	if (chunk_offset != 0) {
		memset(buf, 0, chunk_offset * lbsize);
		buf += chunk_offset * lbsize;
	}
	for (i = 0; i < req->iovcnt; i++) {
		memcpy(buf, req->iov[i].iov_base, req->iov[i].iov_len);
		buf += req->iov[i].iov_len;
	}
	chunk_offset += req->length;
	if (chunk_offset != lb_per_chunk) {
		memset(buf, 0, (lb_per_chunk - chunk_offset) * lbsize);
	}
	_reduce_vol_write_chunk(req, _write_complete_req);
}

void
spdk_reduce_vol_writev(struct spdk_reduce_vol *vol,
		       struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
		       spdk_reduce_vol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_reduce_vol_request *req;
	uint64_t logical_map_index;
	bool overlapped;

	if (length == 0) {
		cb_fn(cb_arg, 0);
		return;
	}

	if (_request_spans_chunk_boundary(vol, offset, length)) {
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	if (!_iov_array_is_valid(vol, iov, iovcnt, length)) {
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	logical_map_index = offset / vol->logical_blocks_per_chunk;
	overlapped = _check_overlap(vol, logical_map_index);

	req = TAILQ_FIRST(&vol->free_requests);
	if (req == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	TAILQ_REMOVE(&vol->free_requests, req, tailq);
	req->type = REDUCE_IO_WRITEV;
	req->vol = vol;
	req->iov = iov;
	req->iovcnt = iovcnt;
	req->offset = offset;
	req->logical_map_index = offset / vol->logical_blocks_per_chunk;
	req->length = length;
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	if (!overlapped) {
		_start_writev_request(req);
	} else {
		TAILQ_INSERT_TAIL(&vol->queued_requests, req, tailq);
	}
}

SPDK_LOG_REGISTER_COMPONENT("reduce", SPDK_LOG_REDUCE)
