/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2025 StarWind Software, Inc.
 *   All rights reserved.
 */

#ifndef SPDK_ACCEL_CUDA_KERN_H
#define SPDK_ACCEL_CUDA_KERN_H

#ifdef __cplusplus
extern "C" {
#endif

#define CUDA_CACHE_LINE_SIZE	128

#define CUDA_XOR_MAX_SOURCES	16

int
accel_cuda_xor_start(
	void *output,
	void **inputs,
	int num_inputs,
	size_t length,
	char *status,
	cudaStream_t stream);

int accel_cuda_copy_start(
	struct iovec *src_iovs,
	uint32_t src_iov_cnt,
	struct iovec *dst_iovs,
	uint32_t dst_iov_cnt,
	char *status,
	cudaStream_t stream);

int accel_cuda_fill_start(
	struct iovec *dst_iovs,
	uint32_t dst_iov_cnt,
	uint64_t fill_pattern,
	char *status,
	cudaStream_t stream);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_ACCEL_CUDA_KERN_H */
