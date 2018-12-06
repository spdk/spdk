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
#include "spdk_internal/log.h"

#include "libpmem.h"

/* Always round up the size of the PM region to the nearest cacheline. */
#define REDUCE_PM_SIZE_ALIGNMENT	64

#define SPDK_REDUCE_SIGNATURE "SPDKREDU"

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

struct spdk_reduce_vol_request {
	uint8_t					*buf;
	TAILQ_ENTRY(spdk_reduce_vol_request)	tailq;
};

struct spdk_reduce_vol {
	struct spdk_reduce_vol_params		params;
	uint32_t				backing_io_units_per_chunk;
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
	TAILQ_HEAD(, spdk_reduce_vol_request)	requests;
	uint8_t					*bufspace;
};

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

static inline uint64_t
divide_round_up(uint64_t num, uint64_t divisor)
{
	return (num + divisor - 1) / divisor;
}

static uint64_t
_get_pm_logical_map_size(uint64_t vol_size, uint64_t chunk_size)
{
	uint64_t chunks_in_logical_map, logical_map_size;

	chunks_in_logical_map = vol_size / chunk_size;
	logical_map_size = chunks_in_logical_map * sizeof(uint64_t);

	/* Round up to next cacheline. */
	return divide_round_up(logical_map_size, REDUCE_PM_SIZE_ALIGNMENT) * REDUCE_PM_SIZE_ALIGNMENT;
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

	return divide_round_up(total_chunks_size, REDUCE_PM_SIZE_ALIGNMENT) * REDUCE_PM_SIZE_ALIGNMENT;
}

static int
_validate_vol_params(struct spdk_reduce_vol_params *params)
{
	if (params->vol_size == 0 || params->chunk_size == 0 ||
	    params->backing_io_unit_size == 0 || params->logical_block_size == 0) {
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

	/* Volume size must be an even multiple of the chunk size. */
	if ((params->vol_size % params->chunk_size) != 0) {
		return -EINVAL;
	}

	return 0;
}

int64_t
spdk_reduce_get_pm_file_size(struct spdk_reduce_vol_params *params)
{
	uint64_t total_pm_size;
	int rc;

	rc = _validate_vol_params(params);
	if (rc != 0) {
		return rc;
	}

	total_pm_size = sizeof(struct spdk_reduce_vol_superblock);
	total_pm_size += _get_pm_logical_map_size(params->vol_size, params->chunk_size);
	total_pm_size += _get_pm_total_chunks_size(params->vol_size, params->chunk_size,
			 params->backing_io_unit_size);
	return total_pm_size;
}

int64_t
spdk_reduce_get_backing_device_size(struct spdk_reduce_vol_params *params)
{
	uint64_t total_backing_size, num_chunks;
	int rc;

	rc = _validate_vol_params(params);
	if (rc != 0) {
		return rc;
	}

	num_chunks = _get_total_chunks(params->vol_size, params->chunk_size);
	total_backing_size = num_chunks * params->chunk_size;
	total_backing_size += sizeof(struct spdk_reduce_vol_superblock);

	return total_backing_size;
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

	vol->bufspace = spdk_dma_malloc(REDUCE_NUM_VOL_REQUESTS * vol->params.chunk_size, 64, NULL);
	if (vol->bufspace == NULL) {
		return -ENOMEM;
	}

	vol->request_mem = calloc(REDUCE_NUM_VOL_REQUESTS, sizeof(*req));
	if (vol->request_mem == NULL) {
		free(vol->bufspace);
		return -ENOMEM;
	}

	for (i = 0; i < REDUCE_NUM_VOL_REQUESTS; i++) {
		req = &vol->request_mem[i];
		TAILQ_INSERT_HEAD(&vol->requests, req, tailq);
		req->buf = vol->bufspace + i * vol->params.chunk_size;
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
		spdk_dma_free(vol->backing_super);
		spdk_bit_array_free(&vol->allocated_chunk_maps);
		spdk_bit_array_free(&vol->allocated_backing_io_units);
		free(vol->request_mem);
		spdk_dma_free(vol->bufspace);
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

	total_chunks = _get_total_chunks(vol->params.vol_size, vol->params.chunk_size);
	vol->allocated_chunk_maps = spdk_bit_array_create(total_chunks);
	total_backing_io_units = total_chunks * (vol->params.chunk_size / vol->params.backing_io_unit_size);
	vol->allocated_backing_io_units = spdk_bit_array_create(total_backing_io_units);

	if (vol->allocated_chunk_maps == NULL || vol->allocated_backing_io_units == NULL) {
		return -ENOMEM;
	}

	/* Set backing io unit bits associated with metadata. */
	spdk_bit_array_set(vol->allocated_backing_io_units, 0);
	spdk_bit_array_set(vol->allocated_backing_io_units, 1);

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
	int64_t size, size_needed;
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

	size_needed = spdk_reduce_get_backing_device_size(params);
	size = backing_dev->blockcnt * backing_dev->blocklen;
	if (size_needed > size) {
		SPDK_ERRLOG("backing device size %" PRIi64 " but %" PRIi64 " needed\n",
			    size, size_needed);
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	if (size_needed > size) {
		SPDK_ERRLOG("pm file size %" PRIi64 " but %" PRIi64 " needed\n",
			    size, size_needed);
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	if (backing_dev->close == NULL || backing_dev->readv == NULL ||
	    backing_dev->writev == NULL || backing_dev->unmap == NULL) {
		SPDK_ERRLOG("backing_dev function pointer not specified\n");
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	vol = calloc(1, sizeof(*vol));
	if (vol == NULL) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

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
	vol->pm_file.size = spdk_reduce_get_pm_file_size(params);
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
	memcpy(&vol->params, params, sizeof(*params));

	rc = _allocate_bit_arrays(vol);
	if (rc != 0) {
		cb_fn(cb_arg, NULL, rc);
		_init_load_cleanup(vol, init_ctx);
		return;
	}

	vol->backing_dev = backing_dev;

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
	int64_t size, size_needed;
	size_t mapped_len;
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

	rc = _allocate_bit_arrays(vol);
	if (rc != 0) {
		goto error;
	}

	size_needed = spdk_reduce_get_backing_device_size(&vol->params);
	size = vol->backing_dev->blockcnt * vol->backing_dev->blocklen;
	if (size_needed > size) {
		SPDK_ERRLOG("backing device size %" PRIi64 " but %" PRIi64 " expected\n",
			    size, size_needed);
		rc = -EILSEQ;
		goto error;
	}

	memcpy(vol->pm_file.path, load_ctx->path, sizeof(vol->pm_file.path));
	vol->pm_file.size = spdk_reduce_get_pm_file_size(&vol->params);
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

	if (backing_dev->close == NULL || backing_dev->readv == NULL ||
	    backing_dev->writev == NULL || backing_dev->unmap == NULL) {
		SPDK_ERRLOG("backing_dev function pointer not specified\n");
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	vol = calloc(1, sizeof(*vol));
	if (vol == NULL) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

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

	pmem_unmap(vol->pm_file.pm_buf, vol->pm_file.size);

	vol->backing_dev->close(vol->backing_dev);

	_init_load_cleanup(vol, NULL);
	cb_fn(cb_arg, 0);
}

SPDK_LOG_REGISTER_COMPONENT("reduce", SPDK_LOG_REDUCE)
