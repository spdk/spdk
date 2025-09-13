/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2025 StarWind Software, Inc.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/likely.h"
#include <cuda_runtime_api.h>
#include "accel_cuda_kern.h"

__global__ void
xor_kernel(
	uint32_t *output,
	const uint32_t **inputs,
	const uint32_t num_inputs,
	const uint32_t count,
	const uint32_t step)
{
	extern __shared__ const uint32_t *ins[];

	if (threadIdx.x < num_inputs) {
		ins[threadIdx.x] = inputs[threadIdx.x];
	}
	__syncthreads();

	for (uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x; idx < count; idx += step) {
		uint32_t out = ins[0][idx] ^ ins[1][idx];

		if (num_inputs > 2) {
			for (uint32_t i = 2; i < num_inputs; i++) {
				out ^= ins[i][idx];
			}
		}
		output[idx] = out;
	}
}

static int
use_host_ptrs()
{
	static int _use_host_ptrs = -1;
	if (spdk_unlikely(_use_host_ptrs < 0)) {
		cudaDeviceGetAttribute(&_use_host_ptrs, cudaDevAttrCanUseHostPointerForRegisteredMem, 0);
	}
	return _use_host_ptrs;
}

static int
get_max_tpb()
{
	static int max_tpb = 0;
	if (spdk_unlikely(max_tpb == 0)) {
		cudaDeviceGetAttribute(&max_tpb, cudaDevAttrMaxThreadsPerBlock, 0);
	}
	return max_tpb;
}

static int
get_num_mps()
{
	static int num_mps = 0;
	if (spdk_unlikely(num_mps == 0)) {
		cudaDeviceGetAttribute(&num_mps, cudaDevAttrMultiProcessorCount, 0);
	}
	return num_mps;
}

static int
_accel_cuda_queue_status_return(char *status, cudaStream_t stream)
{
	cudaError_t err;
	char *dstatus = status;

	if (!use_host_ptrs()) {
		err = cudaHostGetDevicePointer(&dstatus, status, 0);
		if (err != cudaSuccess) {
			SPDK_ERRLOG("status %p not registered (err %d)!\n", status, err);
			return -EINVAL;
		}
	}
	err = cudaMemcpyAsync(dstatus, dstatus + 1, sizeof(char), cudaMemcpyHostToHost, stream);
	if (err != cudaSuccess) {
		SPDK_ERRLOG("cudaMemcpyAsync failed for dptr %p (err %d)!\n", dstatus, err);
		return -EINVAL;
	}
	return 0;
}

extern "C" int
accel_cuda_xor_start(
	void *output,
	void **inputs,
	int num_inputs,
	size_t length,
	char *status,
	cudaStream_t stream)
{
	uint32_t count = (uint32_t)(length / sizeof(uint32_t));
	uint32_t *doutput = (uint32_t *)output;
	uint32_t **dinputs = (uint32_t **)inputs;
	cudaError_t err;
	int tpb, num_blocks, grid_size, step;
	int max_tpb = get_max_tpb();
	static int blocks_per_mp = 0;

	if (num_inputs < 2 || num_inputs > CUDA_XOR_MAX_SOURCES) {
		SPDK_ERRLOG("accel_cuda_xor_prepare supports 2-%d inputs (we got %d)!\n",
			CUDA_XOR_MAX_SOURCES, num_inputs);
		return -EINVAL;
	}

	if (!use_host_ptrs()) {
		/* get device ptrs for the buffers and chunks */
		err = cudaHostGetDevicePointer(&doutput, output, 0);
		if (err != cudaSuccess) {
			SPDK_ERRLOG("output %p not registered!\n", output);
			return -EINVAL;
		}
		err = cudaHostGetDevicePointer(&dinputs, inputs, 0);
		if (err != cudaSuccess) {
			SPDK_ERRLOG("inputs %p not registered!\n", inputs);
			return -EINVAL;
		}
		for (int i = 0; i < num_inputs; i++) {
			err = cudaHostGetDevicePointer(&inputs[i], inputs[i], 0);
			if (err != cudaSuccess) {
				SPDK_ERRLOG("inputs[%d] ptr %p not registered!\n", i, inputs[i]);
				return -EINVAL;
			}
		}
	}

	tpb = max_tpb / 4;

	if (spdk_unlikely(blocks_per_mp == 0)) {
		cudaOccupancyMaxActiveBlocksPerMultiprocessor(&blocks_per_mp, xor_kernel, tpb, 0);
	}

	num_blocks = (count + tpb - 1) / tpb;
	grid_size = blocks_per_mp * get_num_mps();
	grid_size = min(num_blocks, grid_size);
	step = grid_size * tpb;

	SPDK_DEBUGLOG(accel_cuda, "grid %u, tpb %u, count %u, steps %u, step %u\n",
		grid_size, tpb, count, (count + step - 1) / step, step);

	/* launch our cuda kernel */
	xor_kernel<<<grid_size, tpb, num_inputs * sizeof(void*), stream>>>(
		doutput, (const uint32_t **)dinputs, num_inputs, count, step);

	return _accel_cuda_queue_status_return(status, stream);
}

extern "C" int
accel_cuda_copy_start(
	struct iovec *src_iovs,
	uint32_t src_iov_cnt,
	struct iovec *dst_iovs,
	uint32_t dst_iov_cnt,
	char *status,
	cudaStream_t stream)
{
	cudaError_t err;
	struct spdk_ioviter iter;
	void *src, *dst;
	size_t len;

	len = spdk_ioviter_first(&iter, src_iovs, src_iov_cnt, dst_iovs, dst_iov_cnt, &src, &dst);

	for (; len != 0; len = spdk_ioviter_next(&iter, &src, &dst)) {
		void *ddst = dst;
		void *dsrc = src;

		if (!use_host_ptrs()) {
			err = cudaHostGetDevicePointer(&dsrc, src, 0);
			if (err != cudaSuccess) {
				SPDK_ERRLOG("src %p not registered!\n", src);
				return -EINVAL;
			}
			err = cudaHostGetDevicePointer(&ddst, dst, 0);
			if (err != cudaSuccess)	{
				SPDK_ERRLOG("dst %p not registered!\n", dst);
				return -EINVAL;
			}
		}

		err = cudaMemcpyAsync(ddst, dsrc, len, cudaMemcpyHostToHost, stream);
		if (err != cudaSuccess)	{
			SPDK_ERRLOG("cudaMemcpyAsync failed for ddst %p, dsrc %p, len %lu (err %d)!\n",
						ddst, dsrc, len, err);
			return -EINVAL;
		}
	}

	return _accel_cuda_queue_status_return(status, stream);
}

extern "C" int
accel_cuda_fill_start(
	struct iovec *dst_iovs,
	uint32_t dst_iov_cnt,
	uint64_t fill_pattern,
	char *status,
	cudaStream_t stream)
{
	cudaError_t err;
	uint32_t i;

	for (i = 0; i < dst_iov_cnt; i++) {
		struct iovec *iov = &dst_iovs[i];
		void *dptr = iov->iov_base;

		if (!use_host_ptrs()) {
			err = cudaHostGetDevicePointer(&dptr, iov->iov_base, 0);
			if (err != cudaSuccess) {
				SPDK_ERRLOG("dst %p not registered!\n", iov->iov_base);
				return -EINVAL;
			}
		}

		err = cudaMemsetAsync(dptr, fill_pattern, iov->iov_len, stream);
		if (err != cudaSuccess)	{
			SPDK_ERRLOG("cudaMemsetAsync failed for dptr %p (err %d)!\n", dptr, err);
			return -EINVAL;
		}
	}

	return _accel_cuda_queue_status_return(status, stream);
}
