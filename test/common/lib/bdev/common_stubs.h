/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk_internal/mock.h"

DEFINE_STUB(spdk_notify_send, uint64_t, (const char *type, const char *ctx), 0);
DEFINE_STUB(spdk_notify_type_register, struct spdk_notify_type *, (const char *type), NULL);
DEFINE_STUB(spdk_memory_domain_get_dma_device_id, const char *, (struct spdk_memory_domain *domain),
	    "test_domain");
DEFINE_STUB(spdk_memory_domain_get_dma_device_type, enum spdk_dma_device_type,
	    (struct spdk_memory_domain *domain), 0);
DEFINE_STUB_V(spdk_accel_sequence_finish,
	      (struct spdk_accel_sequence *seq, spdk_accel_completion_cb cb_fn, void *cb_arg));
DEFINE_STUB_V(spdk_accel_sequence_abort, (struct spdk_accel_sequence *seq));
DEFINE_STUB_V(spdk_accel_sequence_reverse, (struct spdk_accel_sequence *seq));
DEFINE_STUB(spdk_accel_append_copy, int,
	    (struct spdk_accel_sequence **seq, struct spdk_io_channel *ch, struct iovec *dst_iovs,
	     uint32_t dst_iovcnt, struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
	     struct iovec *src_iovs, uint32_t src_iovcnt, struct spdk_memory_domain *src_domain,
	     void *src_domain_ctx, spdk_accel_step_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(spdk_accel_append_dif_verify_copy, int,
	    (struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
	     struct iovec *dst_iovs, size_t dst_iovcnt,
	     struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
	     struct iovec *src_iovs, size_t src_iovcnt,
	     struct spdk_memory_domain *src_domain, void *src_domain_ctx,
	     uint32_t num_blocks,
	     const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err,
	     spdk_accel_step_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(spdk_accel_append_dif_generate_copy, int,
	    (struct spdk_accel_sequence **seq,
	     struct spdk_io_channel *ch,
	     struct iovec *dst_iovs, size_t dst_iovcnt,
	     struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
	     struct iovec *src_iovs, size_t src_iovcnt,
	     struct spdk_memory_domain *src_domain, void *src_domain_ctx,
	     uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
	     spdk_accel_step_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(spdk_accel_get_memory_domain, struct spdk_memory_domain *, (void), NULL);
DEFINE_STUB(spdk_accel_get_buf, int, (struct spdk_io_channel *ch, uint64_t len, void **buf,
				      struct spdk_memory_domain **domain, void **domain_ctx), 0);
DEFINE_STUB_V(spdk_accel_put_buf, (struct spdk_io_channel *ch, void *buf,
				   struct spdk_memory_domain *domain, void *domain_ctx));
