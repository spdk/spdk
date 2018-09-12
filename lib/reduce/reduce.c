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
#include "spdk_internal/log.h"

/* Structure written to offset 0 of both the pm file and the backing device. */
struct spdk_reduce_vol_superblock {
	struct spdk_reduce_vol_params	params;
	uint8_t				reserved[4080];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_reduce_vol_superblock) == 4096, "size incorrect");

struct spdk_reduce_vol {
};

/*
 * Allocate extra metadata chunks and corresponding backing io units to account for
 *  outstanding I/O in worst case scenario where logical map is completely allocated
 *  and no data can be compressed.  We need extra chunks in this case to handle
 *  in-flight writes since reduce never writes data in place.
 */
#define SBZIP_EXTRA_CHUNKS 128

static inline uint64_t
divide_round_up(uint64_t num, uint64_t divisor)
{
	return (num + divisor - 1) / divisor;
}

static uint64_t
_reduce_pm_logical_map_size(uint64_t vol_size, uint64_t chunk_size)
{
	uint64_t chunks_in_logical_map, logical_map_size;

	chunks_in_logical_map = vol_size / chunk_size;
	logical_map_size = chunks_in_logical_map * sizeof(uint64_t);

	/* Round up to next cacheline. */
	return divide_round_up(logical_map_size, 64) * 64;
}

static uint64_t
_reduce_total_chunks(uint64_t vol_size, uint64_t chunk_size)
{
	uint64_t num_chunks;

	num_chunks = vol_size / chunk_size;
	num_chunks += SBZIP_EXTRA_CHUNKS;

	return num_chunks;
}

static uint64_t
_reduce_pm_total_chunks_size(uint64_t vol_size, uint64_t chunk_size, uint64_t backing_io_unit_size)
{
	uint64_t io_units_per_chunk, num_chunks, total_chunks_size;

	num_chunks = _reduce_total_chunks(vol_size, chunk_size);
	io_units_per_chunk = chunk_size / backing_io_unit_size;
	total_chunks_size = num_chunks * io_units_per_chunk * sizeof(uint64_t);

	/* Round up to next cacheline. */
	return divide_round_up(total_chunks_size, 64) * 64;
}

static int
_validate_vol_params(struct spdk_reduce_vol_params *params)
{
	if (params->vol_size == 0 || params->chunk_size == 0 || params->backing_io_unit_size == 0) {
		return -EINVAL;
	}

	/* Chunk size must be an even multiple of the backing io unit size. */
	if ((params->chunk_size % params->backing_io_unit_size) != 0) {
		return -EINVAL;
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

	if (_validate_vol_params(params) != 0) {
		return -1;
	}

	total_pm_size = sizeof(struct spdk_reduce_vol_superblock);
	total_pm_size += _reduce_pm_logical_map_size(params->vol_size, params->chunk_size);
	total_pm_size += _reduce_pm_total_chunks_size(params->vol_size, params->chunk_size,
			 params->backing_io_unit_size);
	return total_pm_size;
}

int64_t
spdk_reduce_get_backing_device_size(struct spdk_reduce_vol_params *params)
{
	uint64_t total_backing_size, num_chunks;

	if (_validate_vol_params(params) != 0) {
		return -1;
	}

	num_chunks = _reduce_total_chunks(params->vol_size, params->chunk_size);
	total_backing_size = num_chunks * params->chunk_size;
	total_backing_size += sizeof(struct spdk_reduce_vol_superblock);

	return total_backing_size;
}

SPDK_LOG_REGISTER_COMPONENT("reduce", SPDK_LOG_REDUCE)
