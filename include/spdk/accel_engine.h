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

/** \file
 * Acceleration engine abstraction layer
 */

#ifndef SPDK_ACCEL_ENGINE_H
#define SPDK_ACCEL_ENGINE_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

enum accel_capability {
	ACCEL_COPY		= 1 << 0,
	ACCEL_FILL		= 1 << 1,
	ACCEL_DUALCAST		= 1 << 2,
	ACCEL_COMPARE		= 1 << 3,
	ACCEL_CRC32C		= 1 << 4,
	ACCEL_DIF		= 1 << 5,
	ACCEL_COPY_CRC32C	= 1 << 6,
};

/**
 * Acceleration operation callback.
 *
 * \param ref 'accel_task' passed to the corresponding spdk_accel_submit* call.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_accel_completion_cb)(void *ref, int status);

/**
 * Acceleration engine finish callback.
 *
 * \param cb_arg Callback argument.
 */
typedef void (*spdk_accel_fini_cb)(void *cb_arg);

/**
 * Initialize the acceleration engine.
 *
 * \return 0 on success.
 */
int spdk_accel_engine_initialize(void);

/**
 * Close the acceleration engine.
 *
 * \param cb_fn Called when the close operation completes.
 * \param cb_arg Argument passed to the callback function.
 */
void spdk_accel_engine_finish(spdk_accel_fini_cb cb_fn, void *cb_arg);

/**
 * Close the acceleration engine module and perform any necessary cleanup.
 */
void spdk_accel_engine_module_finish(void);

/**
 * Get the I/O channel registered on the acceleration engine.
 *
 * This I/O channel is used to submit copy request.
 *
 * \return a pointer to the I/O channel on success, or NULL on failure.
 */
struct spdk_io_channel *spdk_accel_engine_get_io_channel(void);

/**
 * Retrieve accel engine HW acceleration capabilities.
 *
 * \param ch I/O channel associated with this call.
 *
 * \return bitmap of HW acceleration capabilities defined by enum accel_capability.
 */
uint64_t spdk_accel_get_capabilities(struct spdk_io_channel *ch);

/**
 * Submit a copy request.
 *
 * \param ch I/O channel associated with this call.
 * \param dst Destination to copy to.
 * \param src Source to copy from.
 * \param nbytes Length in bytes to copy.
 * \param cb_fn Called when this copy operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_copy(struct spdk_io_channel *ch, void *dst, void *src, uint64_t nbytes,
			   spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a dual cast copy request.
 *
 * \param ch I/O channel associated with this call.
 * \param dst1 First destination to copy to (must be 4K aligned).
 * \param dst2 Second destination to copy to (must be 4K aligned).
 * \param src Source to copy from.
 * \param nbytes Length in bytes to copy.
 * \param cb_fn Called when this copy operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_dualcast(struct spdk_io_channel *ch, void *dst1, void *dst2, void *src,
			       uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a compare request.
 *
 * \param ch I/O channel associated with this call.
 * \param src1 First location to perform compare on.
 * \param src2 Second location to perform compare on.
 * \param nbytes Length in bytes to compare.
 * \param cb_fn Called when this compare operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, any other value means there was a miscompare.
 */
int spdk_accel_submit_compare(struct spdk_io_channel *ch, void *src1, void *src2, uint64_t nbytes,
			      spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a fill request.
 *
 * This operation will fill the destination buffer with the specified value.
 *
 * \param ch I/O channel associated with this call.
 * \param dst Destination to fill.
 * \param fill Constant byte to fill to the destination.
 * \param nbytes Length in bytes to fill.
 * \param cb_fn Called when this fill operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_fill(struct spdk_io_channel *ch, void *dst, uint8_t fill, uint64_t nbytes,
			   spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a CRC-32C calculation request.
 *
 * This operation will calculate the 4 byte CRC32-C for the given data.
 *
 * \param ch I/O channel associated with this call.
 * \param crc_dst Destination to write the CRC-32C to.
 * \param src The source address for the data.
 * \param seed Four byte seed value.
 * \param nbytes Length in bytes.
 * \param cb_fn Called when this CRC-32C operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_crc32c(struct spdk_io_channel *ch, uint32_t *crc_dst, void *src,
			     uint32_t seed,
			     uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a chained CRC-32C calculation request.
 *
 * This operation will calculate the 4 byte CRC32-C for the given data.
 *
 * \param ch I/O channel associated with this call.
 * \param crc_dst Destination to write the CRC-32C to.
 * \param iovs The io vector array which stores the src data and len.
 * \param iovcnt The size of the iov.
 * \param seed Four byte seed value.
 * \param cb_fn Called when this CRC-32C operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_crc32cv(struct spdk_io_channel *ch, uint32_t *crc_dst, struct iovec *iovs,
			      uint32_t iovcnt, uint32_t seed, spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a copy with CRC-32C calculation request.
 *
 * This operation will copy data and calculate the 4 byte CRC32-C for the given data.
 *
 * \param ch I/O channel associated with this call.
 * \param dst Destination to write the data to.
 * \param src The source address for the data.
 * \param crc_dst Destination to write the CRC-32C to.
 * \param seed Four byte seed value.
 * \param nbytes Length in bytes.
 * \param cb_fn Called when this CRC-32C operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_copy_crc32c(struct spdk_io_channel *ch, void *dst, void *src,
				  uint32_t *crc_dst, uint32_t seed, uint64_t nbytes,
				  spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a chained copy + CRC-32C calculation request.
 *
 * This operation will calculate the 4 byte CRC32-C for the given data.
 *
 * \param ch I/O channel associated with this call.
 * \param dst Destination to write the data to.
 * \param src_iovs The io vector array which stores the src data and len.
 * \param iovcnt The size of the io vectors.
 * \param crc_dst Destination to write the CRC-32C to.
 * \param seed Four byte seed value.
 * \param cb_fn Called when this CRC-32C operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_copy_crc32cv(struct spdk_io_channel *ch, void *dst, struct iovec *src_iovs,
				   uint32_t iovcnt, uint32_t *crc_dst, uint32_t seed, spdk_accel_completion_cb cb_fn, void *cb_arg);


struct spdk_json_write_ctx;

/**
 * Write Acceleration subsystem configuration into provided JSON context.
 *
 * \param w JSON write context
 */
void spdk_accel_write_config_json(struct spdk_json_write_ctx *w);

#ifdef __cplusplus
}
#endif

#endif
