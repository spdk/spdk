/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "bdev_raid.h"

#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "lib/thread/thread_internal.h"
#include "spdk/ht.h"
#include "spdk/tree.h"

#include "spdk/log.h"

struct raid5_io_buffer {
	struct raid_bdev_io *raid_io;

	struct iovec *buffer;
};

struct raid5_write_request_buffer {
	struct raid5_io_buffer *wr_xor_buff;
	
	struct iovec *buffer;
};

static inline uint8_t
raid5_parity_strip_index(struct raid_bdev *raid_bdev, uint64_t stripe_index)
{
	return raid_bdev->num_base_bdevs - 1 - stripe_index % raid_bdev->num_base_bdevs;
}

static inline struct iovec *
raid5_get_buffer(size_t iovlen)
{
	struct iovec *buffer;

	buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		return NULL;
	}

	buffer->iov_len = iovlen;
	buffer->iov_base = calloc(buffer->iov_len, sizeof(char));
	if (buffer->iov_base == NULL) {
		free(buffer);
		return NULL;
	}

	return buffer;
}

static inline void
raid5_free_buffer(struct iovec *buffer)
{
	free(buffer->iov_base);
	free(buffer);
}

static inline struct raid5_io_buffer *
raid5_get_io_buffer(struct raid_bdev_io *raid_io, size_t data_len)
{
	struct raid5_io_buffer *io_buffer;

	io_buffer = calloc(1, sizeof(struct raid5_io_buffer));
	if (io_buffer == NULL) {
		return NULL;
	}

	io_buffer->buffer = raid5_get_buffer(data_len);
	if (io_buffer->buffer == NULL) {
		free(io_buffer);
		return NULL;
	}

	io_buffer->raid_io = raid_io;
	return io_buffer;
}

static inline void
raid5_free_io_buffer(struct raid5_io_buffer *io_buffer)
{
	raid5_free_buffer(io_buffer->buffer);
	free(io_buffer);
}

static inline struct raid5_write_request_buffer *
raid5_get_write_request_buffer(struct raid5_io_buffer *wr_xor_buff, size_t data_len)
{
	struct raid5_write_request_buffer *wr_buffer;

	wr_buffer = calloc(1, sizeof(struct raid5_write_request_buffer));
	if (wr_buffer == NULL) {
		return NULL;
	}

	wr_buffer->buffer = raid5_get_buffer(data_len);
	if (wr_buffer->buffer == NULL) {
		free(wr_buffer);
		return NULL;
	}

	wr_buffer->wr_xor_buff = wr_xor_buff;
	return wr_buffer;
}

static inline void
raid5_free_write_request_buffer(struct raid5_write_request_buffer *wr_buffer)
{
	raid5_free_buffer(wr_buffer->buffer);
	free(wr_buffer);
}

static inline void
raid5_xor_buffers(struct iovec *xor_res, struct iovec *buffer)
{
	uint64_t *xb8 = xor_res->iov_base;
	uint64_t *b8 = buffer->iov_base;
	size_t len8 = xor_res->iov_len / 8;

	for (size_t i=0; i < len8; ++i) {
		xb8[i] ^= b8[i];
	}
}

static inline void
raid5_xor_iovs_with_buffer(struct iovec *iovs, int iovcnt, struct iovec *buffer)
{
	uint64_t *xb8;
	uint64_t *b8 = buffer->iov_base;
	size_t b8i = 0;
	size_t len8;

	for (int iovidx = 0; iovidx < iovcnt; ++iovidx) {
		xb8 = iovs[iovidx].iov_base;
		len8 = iovs[iovidx].iov_len / 8;
		for (size_t i = 0; i < len8; ++i, ++b8i) {
			xb8[i] ^= b8[b8i];
		}
	}
}

static inline void
raid5_xor_buffer_with_iovs(struct iovec *buffer, struct iovec *iovs, int iovcnt)
{
	uint64_t *xb8 = buffer->iov_base;
	uint64_t *b8;
	size_t xb8i = 0;
	size_t len8;

	for (int iovidx = 0; iovidx < iovcnt; ++iovidx) {
		b8 = iovs[iovidx].iov_base;
		len8 = iovs[iovidx].iov_len / 8;
		for (size_t i = 0; i < len8; ++i, ++xb8i) {
			xb8[xb8i] ^= b8[i];
		}
	}
}

static inline void
raid5_fill_iovs_with_zeroes(struct iovec *iovs, int iovcnt)
{
	uint64_t *b8;
	size_t len8;

	for (int iovidx = 0; iovidx < iovcnt; ++iovidx) {
		b8 = iovs[iovidx].iov_base;
		len8 = iovs[iovidx].iov_len / 8;
		for (size_t i = 0; i < len8; ++i) {
			b8[i] = 0;
		}
	}
}

void
raid5_queue_io_wait(struct raid_bdev_io *raid_io, struct spdk_bdev *bdev,
		struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn, void *cb_arg)
{
	raid_io->waitq_entry.bdev = bdev;
	raid_io->waitq_entry.cb_fn = cb_fn;
	raid_io->waitq_entry.cb_arg = cb_arg;
	spdk_bdev_queue_io_wait(bdev, ch, &raid_io->waitq_entry);
}

static void
raid5_bdev_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	raid_bdev_io_complete(raid_io, success ?
				   SPDK_BDEV_IO_STATUS_SUCCESS :
				   SPDK_BDEV_IO_STATUS_FAILED);
}

static void
raid5_read_request_complete_part(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid5_io_buffer *io_buffer = cb_arg;
	struct spdk_bdev_io		*rbdev_io = spdk_bdev_io_from_ctx(io_buffer->raid_io);

	spdk_bdev_free_io(bdev_io);

	assert(io_buffer->raid_io->base_bdev_io_remaining > 0);
	io_buffer->raid_io->base_bdev_io_remaining--;

	if (!success) {
		io_buffer->raid_io->base_bdev_io_status = SPDK_BDEV_IO_STATUS_FAILED;
	} else {
		raid5_xor_iovs_with_buffer(rbdev_io->u.bdev.iovs, rbdev_io->u.bdev.iovcnt,
				io_buffer->buffer);
	}

	if (io_buffer->raid_io->base_bdev_io_remaining == 0) {
		raid_bdev_io_complete(io_buffer->raid_io,
				io_buffer->raid_io->base_bdev_io_status);
	}

	raid5_free_io_buffer(io_buffer);
}

static void raid5_submit_write_request_writing(struct raid5_io_buffer *io_buffer);

static void
raid5_write_request_reading_complete_part (struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid5_write_request_buffer *wr_buffer = cb_arg;
	struct spdk_bdev_io		*rbdev_io = spdk_bdev_io_from_ctx(wr_buffer->wr_xor_buff->raid_io);

	spdk_bdev_free_io(bdev_io);

	assert(wr_buffer->wr_xor_buff->raid_io->base_bdev_io_remaining > 0);
	wr_buffer->wr_xor_buff->raid_io->base_bdev_io_remaining--;

	if (!success) {
		wr_buffer->wr_xor_buff->raid_io->base_bdev_io_status = SPDK_BDEV_IO_STATUS_FAILED;
	} else {
		raid5_xor_buffers(wr_buffer->wr_xor_buff->buffer, wr_buffer->buffer);
	}

	if (wr_buffer->wr_xor_buff->raid_io->base_bdev_io_remaining == 0) {
		if (wr_buffer->wr_xor_buff->raid_io->base_bdev_io_status == SPDK_BDEV_IO_STATUS_SUCCESS) {
			raid5_xor_buffer_with_iovs(wr_buffer->wr_xor_buff->buffer,
					rbdev_io->u.bdev.iovs, rbdev_io->u.bdev.iovcnt);
			wr_buffer->wr_xor_buff->raid_io->base_bdev_io_submitted = 1;
			wr_buffer->wr_xor_buff->raid_io->base_bdev_io_remaining = 1;
			raid5_submit_write_request_writing(wr_buffer->wr_xor_buff);
		} else {
			raid_bdev_io_complete(wr_buffer->wr_xor_buff->raid_io,
					wr_buffer->wr_xor_buff->raid_io->base_bdev_io_status);
			raid5_free_io_buffer(wr_buffer->wr_xor_buff);
		}
	}

	raid5_free_write_request_buffer(wr_buffer);
}

static void
raid5_write_request_reading_with_writing_req_strip_complete_part(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid5_write_request_buffer *wr_buffer = cb_arg;
	struct spdk_bdev_io		*rbdev_io = spdk_bdev_io_from_ctx(wr_buffer->wr_xor_buff->raid_io);
	
	spdk_bdev_free_io(bdev_io);

	assert(wr_buffer->wr_xor_buff->raid_io->base_bdev_io_remaining > 0);
	wr_buffer->wr_xor_buff->raid_io->base_bdev_io_remaining--;

	if (!success) {
		wr_buffer->wr_xor_buff->raid_io->base_bdev_io_status = SPDK_BDEV_IO_STATUS_FAILED;
	} else {
		raid5_xor_buffers(wr_buffer->wr_xor_buff->buffer, wr_buffer->buffer);
	}
	
	if (wr_buffer->wr_xor_buff->raid_io->base_bdev_io_remaining == 0) {
		if (wr_buffer->wr_xor_buff->raid_io->base_bdev_io_status == SPDK_BDEV_IO_STATUS_SUCCESS) {
			raid5_xor_buffer_with_iovs(wr_buffer->wr_xor_buff->buffer,
					rbdev_io->u.bdev.iovs, rbdev_io->u.bdev.iovcnt);
			wr_buffer->wr_xor_buff->raid_io->base_bdev_io_submitted = 0;
			wr_buffer->wr_xor_buff->raid_io->base_bdev_io_remaining = 2;
			raid5_submit_write_request_writing(wr_buffer->wr_xor_buff);
		} else {
			raid_bdev_io_complete(wr_buffer->wr_xor_buff->raid_io,
					wr_buffer->wr_xor_buff->raid_io->base_bdev_io_status);
			raid5_free_io_buffer(wr_buffer->wr_xor_buff);
		}
	}

	raid5_free_write_request_buffer(wr_buffer);
}

static void
raid5_write_request_writing_complete_part(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid5_io_buffer *io_buffer = cb_arg;
	
	spdk_bdev_free_io(bdev_io);

	assert(io_buffer->raid_io->base_bdev_io_remaining > 0);
	io_buffer->raid_io->base_bdev_io_remaining--;

	if (!success) {
		io_buffer->raid_io->base_bdev_io_status = SPDK_BDEV_IO_STATUS_FAILED;
	}

	if (io_buffer->raid_io->base_bdev_io_remaining == 0) {
		raid_bdev_io_complete(io_buffer->raid_io,
				io_buffer->raid_io->base_bdev_io_status);
		raid5_free_io_buffer(io_buffer);	
	}
}

static void raid5_submit_rw_request(struct raid_bdev_io *raid_io);

static void
_raid5_submit_rw_request(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;

	raid5_submit_rw_request(raid_io);
}

static void
raid5_submit_read_request(struct raid_bdev_io *raid_io)
{
	struct spdk_bdev_io		*bdev_io = spdk_bdev_io_from_ctx(raid_io);
	struct spdk_bdev_ext_io_opts	io_opts = {};
	struct raid_bdev_io_channel	*raid_ch = raid_io->raid_ch;
	struct raid_bdev		*raid_bdev = raid_io->raid_bdev;
	uint64_t			block_size_b = (raid_bdev->strip_size_kb / raid_bdev->strip_size) * (uint64_t)1024;
	uint64_t			stripe_index;
	uint64_t			parity_strip_idx;
	uint64_t			req_bdev_idx;
	uint32_t			offset_in_strip;
	uint64_t			offset_blocks;
	uint64_t			num_blocks;
	int				ret = 0;
	uint64_t			start_strip_idx;
	uint64_t			end_strip_idx;
	struct raid_base_bdev_info	*base_info;
	struct spdk_io_channel		*base_ch;

	start_strip_idx = bdev_io->u.bdev.offset_blocks >> raid_bdev->strip_size_shift;
	end_strip_idx = (bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks - 1) >>
		    raid_bdev->strip_size_shift;
	if (start_strip_idx != end_strip_idx) {
		SPDK_ERRLOG("I/O spans strip boundary!\n");
		assert(false);
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	assert(raid_ch != NULL);
	assert(raid_ch->base_channel);

	io_opts.size = sizeof(io_opts);
	io_opts.memory_domain = bdev_io->u.bdev.memory_domain;
	io_opts.memory_domain_ctx = bdev_io->u.bdev.memory_domain_ctx;
	io_opts.metadata = bdev_io->u.bdev.md_buf;

	stripe_index = start_strip_idx / (raid_bdev->num_base_bdevs - 1);
	parity_strip_idx = raid5_parity_strip_index(raid_bdev, stripe_index);
	offset_in_strip = bdev_io->u.bdev.offset_blocks % (raid_bdev->strip_size);

	req_bdev_idx = start_strip_idx % (raid_bdev->num_base_bdevs - 1);
	if (req_bdev_idx >= parity_strip_idx) {
		++req_bdev_idx;
	}
	offset_blocks = (stripe_index << raid_bdev->strip_size_shift) + offset_in_strip;
	num_blocks = bdev_io->u.bdev.num_blocks;

	base_info = &raid_bdev->base_bdev_info[req_bdev_idx];
	base_ch = raid_ch->base_channel[req_bdev_idx];

	if (base_ch != NULL) {
		// case: reading only one strip

		ret = spdk_bdev_readv_blocks_ext(base_info->desc, base_ch,
						 bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
						 offset_blocks, num_blocks, raid5_bdev_io_completion,
						 raid_io, &io_opts);

		if (ret == -ENOMEM) {
			raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
					base_ch, _raid5_submit_rw_request);
		} else if (ret != 0) {
			SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
			assert(false);
			raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	} else {
		// case: broken request strip

		uint8_t start_idx;

		if (raid_io->base_bdev_io_submitted == 0) {
			raid_io->base_bdev_io_remaining = raid_bdev->num_base_bdevs - 1;
			raid5_fill_iovs_with_zeroes(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt);
		}

		start_idx = raid_io->base_bdev_io_submitted;
		if (req_bdev_idx <= start_idx) {
			start_idx++;
		}

		for (uint8_t idx = start_idx; idx < raid_bdev->num_base_bdevs; ++idx) {
			struct raid5_io_buffer *io_buffer;

			base_info = &raid_bdev->base_bdev_info[idx];
			base_ch = raid_ch->base_channel[idx];
			if (base_ch == NULL) {
				if (idx == req_bdev_idx) {
					continue;
				} else {
					SPDK_ERRLOG("2 broken strips\n");
					assert(false);
					raid_io->base_bdev_io_status = SPDK_BDEV_IO_STATUS_FAILED;
					raid_io->base_bdev_io_remaining = raid_io->base_bdev_io_remaining + raid_io->base_bdev_io_submitted -
															(raid_bdev->num_base_bdevs - 1);
					if (raid_io->base_bdev_io_remaining == 0) {
						raid_bdev_io_complete(raid_io, raid_io->base_bdev_io_status);
					}
					return;
				}
			}

			io_buffer = raid5_get_io_buffer(raid_io, num_blocks * block_size_b);
			if (io_buffer == NULL) {
				raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
					base_ch, _raid5_submit_rw_request);
				return;
			}
			
			ret = spdk_bdev_readv_blocks_ext(base_info->desc, base_ch,
						 io_buffer->buffer, 1,
						 offset_blocks, num_blocks, raid5_read_request_complete_part,
						 io_buffer, &io_opts);

			if (ret != 0) {
				raid5_free_io_buffer(io_buffer);
				if (ret == -ENOMEM) {
					raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
							base_ch, _raid5_submit_rw_request);
				} else {
					SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
					assert(false);
					raid_io->base_bdev_io_status = SPDK_BDEV_IO_STATUS_FAILED;
					raid_io->base_bdev_io_remaining = raid_io->base_bdev_io_remaining + raid_io->base_bdev_io_submitted -
															(raid_bdev->num_base_bdevs - 1);
					if (raid_io->base_bdev_io_remaining == 0) {
						raid_bdev_io_complete(raid_io, raid_io->base_bdev_io_status);
					}
				}
				return;
			}			 

			raid_io->base_bdev_io_submitted++;
		}
	}
}

static void raid5_submit_write_request_reading(struct raid5_io_buffer *wr_xor_buff);

static void
_raid5_submit_write_request_reading(void *_wr_xor_buff)
{
	struct raid5_io_buffer *wr_xor_buff = _wr_xor_buff;

	raid5_submit_write_request_reading(wr_xor_buff);
}

static void
raid5_submit_write_request_reading(struct raid5_io_buffer *wr_xor_buff)
{
	struct raid_bdev_io *raid_io = wr_xor_buff->raid_io;
	struct spdk_bdev_io		*bdev_io = spdk_bdev_io_from_ctx(raid_io);
	struct spdk_bdev_ext_io_opts	io_opts = {};
	struct raid_bdev_io_channel	*raid_ch = raid_io->raid_ch;
	struct raid_bdev		*raid_bdev = raid_io->raid_bdev;
	uint64_t			block_size_b = (raid_bdev->strip_size_kb / raid_bdev->strip_size) * (uint64_t)1024;
	uint8_t				broken_bdev_idx = raid_bdev->num_base_bdevs;
	uint64_t			stripe_index;
	uint64_t			parity_strip_idx;
	uint64_t			req_bdev_idx;
	uint32_t			offset_in_strip;
	uint64_t			offset_blocks;
	uint64_t			num_blocks;
	int				ret = 0;
	uint64_t			start_strip_idx;
	struct raid_base_bdev_info	*base_info;
	struct spdk_io_channel		*base_ch;
	struct raid5_write_request_buffer *wr_buffer;

	start_strip_idx = bdev_io->u.bdev.offset_blocks >> raid_bdev->strip_size_shift;

	io_opts.size = sizeof(io_opts);
	io_opts.memory_domain = bdev_io->u.bdev.memory_domain;
	io_opts.memory_domain_ctx = bdev_io->u.bdev.memory_domain_ctx;
	io_opts.metadata = bdev_io->u.bdev.md_buf;

	stripe_index = start_strip_idx / (raid_bdev->num_base_bdevs - 1);
	parity_strip_idx = raid5_parity_strip_index(raid_bdev, stripe_index);
	offset_in_strip = bdev_io->u.bdev.offset_blocks % (raid_bdev->strip_size);

	req_bdev_idx = start_strip_idx % (raid_bdev->num_base_bdevs - 1);
	if (req_bdev_idx >= parity_strip_idx) {
		++req_bdev_idx;
	}
	offset_blocks = (stripe_index << raid_bdev->strip_size_shift) + offset_in_strip;
	num_blocks = bdev_io->u.bdev.num_blocks;
	
	// calculating of broken strip idx
	for (uint8_t idx = 0; idx < raid_bdev->num_base_bdevs; ++idx) {
		if (raid_ch->base_channel[idx] == NULL) {
			if (broken_bdev_idx == raid_bdev->num_base_bdevs) {
				broken_bdev_idx = idx;
			} else {
				SPDK_ERRLOG("2 broken strips\n");
				assert(false);
				raid_io->base_bdev_io_status = SPDK_BDEV_IO_STATUS_FAILED;
				if (raid_io->base_bdev_io_submitted == 0) {
					raid_bdev_io_complete(raid_io, raid_io->base_bdev_io_status);
				}
				return;
			}
		}
	}

	if (broken_bdev_idx != req_bdev_idx && broken_bdev_idx != raid_bdev->num_base_bdevs) {
		// case: broken strip isn't request strip or parity strip

		if (raid_io->base_bdev_io_submitted == 0) {
			raid_io->base_bdev_io_remaining = 2;
		}
		
		switch (raid_io->base_bdev_io_submitted) {
			case 0:
				base_info = &raid_bdev->base_bdev_info[parity_strip_idx];
				base_ch = raid_ch->base_channel[parity_strip_idx];
				
				wr_buffer = raid5_get_write_request_buffer(wr_xor_buff, num_blocks * block_size_b);
				if (wr_buffer == NULL) {
					raid5_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
							base_ch, _raid5_submit_write_request_reading, wr_xor_buff);
					return;
				}

				ret = spdk_bdev_readv_blocks_ext(base_info->desc, base_ch,
							wr_buffer->buffer, 1,
							offset_blocks, num_blocks, raid5_write_request_reading_with_writing_req_strip_complete_part,
							wr_buffer, &io_opts);
				
				if (ret != 0) {
					raid5_free_write_request_buffer(wr_buffer);
					if (ret == -ENOMEM) {
						raid5_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
								base_ch, _raid5_submit_write_request_reading, wr_xor_buff);
					} else {
						SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
						assert(false);
						raid5_free_io_buffer(wr_xor_buff);
						raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
					}
					return;
				}
				raid_io->base_bdev_io_submitted++;
			case 1:
				base_info = &raid_bdev->base_bdev_info[req_bdev_idx];
				base_ch = raid_ch->base_channel[req_bdev_idx];
				
				wr_buffer = raid5_get_write_request_buffer(wr_xor_buff, num_blocks * block_size_b);
				if (wr_buffer == NULL) {
					raid5_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
							base_ch, _raid5_submit_write_request_reading, wr_xor_buff);
					return;
				}

				ret = spdk_bdev_readv_blocks_ext(base_info->desc, base_ch,
							wr_buffer->buffer, 1,
							offset_blocks, num_blocks, raid5_write_request_reading_with_writing_req_strip_complete_part,
							wr_buffer, &io_opts);
							
				if (ret != 0) {
					raid5_free_write_request_buffer(wr_buffer);
					if (ret == -ENOMEM) {
						raid5_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
								base_ch, _raid5_submit_write_request_reading, wr_xor_buff);
					} else {
						SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
						assert(false);
						raid_io->base_bdev_io_status = SPDK_BDEV_IO_STATUS_FAILED;
						raid_io->base_bdev_io_remaining = raid_io->base_bdev_io_remaining + raid_io->base_bdev_io_submitted - 2;
						if (raid_io->base_bdev_io_remaining == 0) {
							raid5_free_io_buffer(wr_xor_buff);
							raid_bdev_io_complete(raid_io, raid_io->base_bdev_io_status);
						}
					}
					return;
				}
				raid_io->base_bdev_io_submitted++;
		}
	} else {
		// cases with reading stripe

		uint8_t start_idx;
		spdk_bdev_io_completion_cb cb;
		
		if (broken_bdev_idx == req_bdev_idx) {
			// case: broken request strip
			cb = raid5_write_request_reading_complete_part;
		} else {
			// case: without broken strip
			cb = raid5_write_request_reading_with_writing_req_strip_complete_part;
		}

		if (raid_io->base_bdev_io_submitted == 0) {
			raid_io->base_bdev_io_remaining = raid_bdev->num_base_bdevs - 2;
		}

		start_idx = raid_io->base_bdev_io_submitted;
		if (req_bdev_idx <= start_idx || parity_strip_idx <= start_idx) {
			start_idx++;
			if (req_bdev_idx <= start_idx && parity_strip_idx <= start_idx)  {
				start_idx++;
			}
		}

		for (uint8_t idx = start_idx; idx < raid_bdev->num_base_bdevs; ++idx) {
			if (idx == req_bdev_idx || idx == parity_strip_idx) {
				continue;
			}

			base_info = &raid_bdev->base_bdev_info[idx];
			base_ch = raid_ch->base_channel[idx];

			wr_buffer = raid5_get_write_request_buffer(wr_xor_buff, num_blocks * block_size_b);
			if (wr_buffer == NULL) {
				raid5_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
						base_ch, _raid5_submit_write_request_reading, wr_xor_buff);
				return;
			}

			ret = spdk_bdev_readv_blocks_ext(base_info->desc, base_ch,
						wr_buffer->buffer, 1,
						offset_blocks, num_blocks, cb,
						wr_buffer, &io_opts);

			if (ret != 0) {
				raid5_free_write_request_buffer(wr_buffer);
				if (ret == -ENOMEM) {
					raid5_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
							base_ch, _raid5_submit_write_request_reading, wr_xor_buff);
				} else {
					SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
					assert(false);
					raid_io->base_bdev_io_status = SPDK_BDEV_IO_STATUS_FAILED;
					raid_io->base_bdev_io_remaining = raid_io->base_bdev_io_remaining + raid_io->base_bdev_io_submitted -
															(raid_bdev->num_base_bdevs - 2);
					if (raid_io->base_bdev_io_remaining == 0) {
						raid5_free_io_buffer(wr_xor_buff);
						raid_bdev_io_complete(raid_io, raid_io->base_bdev_io_status);
					}
				}
				return;
			}
			raid_io->base_bdev_io_submitted++;
		}
	}
}

static void
_raid5_submit_write_request_writing(void *_io_buffer)
{
	struct raid5_io_buffer *io_buffer = _io_buffer;

	raid5_submit_write_request_writing(io_buffer);
}

static void
raid5_submit_write_request_writing(struct raid5_io_buffer *io_buffer)
{
	struct raid_bdev_io *raid_io = io_buffer->raid_io;
	struct spdk_bdev_io		*bdev_io = spdk_bdev_io_from_ctx(raid_io);
	struct spdk_bdev_ext_io_opts	io_opts = {};
	struct raid_bdev_io_channel	*raid_ch = raid_io->raid_ch;
	struct raid_bdev		*raid_bdev = raid_io->raid_bdev;
	uint64_t			stripe_index;
	uint64_t			parity_strip_idx;
	uint64_t			req_bdev_idx;
	uint32_t			offset_in_strip;
	uint64_t			offset_blocks;
	uint64_t			num_blocks;
	int				ret = 0;
	uint64_t			start_strip_idx;
	struct raid_base_bdev_info	*base_info;
	struct spdk_io_channel		*base_ch;

	start_strip_idx = bdev_io->u.bdev.offset_blocks >> raid_bdev->strip_size_shift;

	io_opts.size = sizeof(io_opts);
	io_opts.memory_domain = bdev_io->u.bdev.memory_domain;
	io_opts.memory_domain_ctx = bdev_io->u.bdev.memory_domain_ctx;
	io_opts.metadata = bdev_io->u.bdev.md_buf;

	stripe_index = start_strip_idx / (raid_bdev->num_base_bdevs - 1);
	parity_strip_idx = raid5_parity_strip_index(raid_bdev, stripe_index);
	offset_in_strip = bdev_io->u.bdev.offset_blocks % (raid_bdev->strip_size);

	req_bdev_idx = start_strip_idx % (raid_bdev->num_base_bdevs - 1);
	if (req_bdev_idx >= parity_strip_idx) {
		++req_bdev_idx;
	}
	offset_blocks = (stripe_index << raid_bdev->strip_size_shift) + offset_in_strip;
	num_blocks = bdev_io->u.bdev.num_blocks;

	switch (raid_io->base_bdev_io_submitted) {
		case 0:
			// writing request strip

			base_info = &raid_bdev->base_bdev_info[req_bdev_idx];
			base_ch = raid_ch->base_channel[req_bdev_idx];

			ret = spdk_bdev_writev_blocks_ext(base_info->desc, base_ch,
						bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
						offset_blocks, num_blocks, raid5_write_request_writing_complete_part,
						io_buffer, &io_opts);
						
			if (ret != 0) {
				if (ret == -ENOMEM) {
					raid5_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
							base_ch, _raid5_submit_write_request_writing, io_buffer);
				} else {
					SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
					assert(false);
					raid5_free_io_buffer(io_buffer);
					raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
				}
				return;
			}

			raid_io->base_bdev_io_submitted++;
		case 1:
			// writing parity strip
			
			base_info = &raid_bdev->base_bdev_info[parity_strip_idx];
			base_ch = raid_ch->base_channel[parity_strip_idx];

			ret = spdk_bdev_writev_blocks_ext(base_info->desc, base_ch,
						io_buffer->buffer, 1,
						offset_blocks, num_blocks, raid5_write_request_writing_complete_part,
						io_buffer, &io_opts);
			
			if (ret != 0) {
				if (ret == -ENOMEM) {
					raid5_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
							base_ch, _raid5_submit_write_request_writing, io_buffer);
				} else {
					SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
					assert(false);
					raid_io->base_bdev_io_status = SPDK_BDEV_IO_STATUS_FAILED;
					raid_io->base_bdev_io_remaining = raid_io->base_bdev_io_remaining + raid_io->base_bdev_io_submitted - 2;
					if (raid_io->base_bdev_io_remaining == 0) {
						raid5_free_io_buffer(io_buffer);
						raid_bdev_io_complete(raid_io, raid_io->base_bdev_io_status);
					}
				}
				return;
			}

			raid_io->base_bdev_io_submitted++;
	}
}

//-----------------------------our workspace-----------------------------

#define MAX_HT_STRING_LEN 35

struct raid5_write_request {
    uint32_t addr;
    RB_ENTRY(raid5_write_request) link;
	struct raid5_io_buffer *io_buffer;
};

static int
addr_cmp(struct raid5_write_request *c1, struct raid5_write_request *c2) {
    return (c1->addr < c2->addr ? -1 : c1->addr > c2->addr);
}

struct raid5_request_tree {
    RB_HEAD(raid5_addr_tree, raid5_write_request) tree;
    uint64_t size;
};

RB_GENERATE_STATIC(raid5_addr_tree, raid5_write_request, link, addr_cmp);

struct raid5_requests_ht {
	ht* raid5_request_table;
} raid5_ht;

static void
clear_tree(struct raid5_request_tree* tree)
{
	struct raid5_write_request *current_request;

    RB_FOREACH(current_request, raid5_addr_tree, &tree->tree)
    {
        RB_REMOVE(raid5_addr_tree, &tree->tree, current_request);
        free(current_request);
        SPDK_DEBUGLOG(bdev_malloc, "DELETING ONE REQUEST\n");
    }
    tree->size = 0;
	SPDK_ERRLOG("TREE IS CLEARED\n");
}

void
raid5_catching_requests(struct raid5_io_buffer *io_buffer)
{
	struct raid5_write_request *write_request = malloc(sizeof *write_request);
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(io_buffer->raid_io);	
	struct raid_bdev *raid_bdev = io_buffer->raid_io->raid_bdev;
	struct raid5_request_tree* stripe_tree;
	char stripe_key[MAX_HT_STRING_LEN];
	uint64_t stripe_index;
	uint64_t start_strip_idx;
	uint8_t max_tree_size = raid_bdev->num_base_bdevs - 1;

	start_strip_idx = bdev_io->u.bdev.offset_blocks >> raid_bdev->strip_size_shift;
	write_request->addr = bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen;
	write_request->io_buffer = io_buffer;
	stripe_index = start_strip_idx / (raid_bdev->num_base_bdevs - 1);
	snprintf(stripe_key, sizeof stripe_key, "%lu", stripe_index);
	stripe_tree = ht_get(raid5_ht.raid5_request_table, stripe_key);
	
	SPDK_ERRLOG("max_tree_size test %d\n", max_tree_size);
	SPDK_ERRLOG("STRIPE_INDEX TEST %lu\n", stripe_index);
	SPDK_ERRLOG("SNPRINTF TEST. STRIPE KEY IS %s\n", stripe_key);

	if (stripe_tree == NULL) {
		stripe_tree = malloc(sizeof *stripe_tree);
		stripe_tree->size = 0;
		RB_INIT(&stripe_tree->tree);
		ht_set(raid5_ht.raid5_request_table, stripe_key, stripe_tree);
		SPDK_ERRLOG("ADDED TO HASHTABLE 1\n");
	}

	SPDK_ERRLOG("THE SIZE OF THE TREE IS%d\n", stripe_tree->size);
	SPDK_ERRLOG("POINTER OF STRIPE TREE %p. THE SIZE IS %lu\n", stripe_tree, sizeof(*stripe_tree));
	RB_INSERT(raid5_addr_tree, &stripe_tree->tree, write_request);
	SPDK_ERRLOG("ADDED TO TREE\n");
	stripe_tree->size++;
	if (stripe_tree->size == max_tree_size) clear_tree(stripe_tree);
	SPDK_ERRLOG("\n\n -------//--------\n\n");
}

//-----------------------------our workspace-----------------------------

static void
raid5_submit_write_request(struct raid_bdev_io *raid_io)
{
	struct spdk_bdev_io		*bdev_io = spdk_bdev_io_from_ctx(raid_io);
	struct spdk_bdev_ext_io_opts	io_opts = {};
	struct raid_bdev_io_channel	*raid_ch = raid_io->raid_ch;
	struct raid_bdev		*raid_bdev = raid_io->raid_bdev;
	uint64_t			block_size_b = (raid_bdev->strip_size_kb / raid_bdev->strip_size) * (uint64_t)1024;
	uint8_t				broken_bdev_idx = raid_bdev->num_base_bdevs;
	uint64_t			stripe_index;
	uint64_t			parity_strip_idx;
	uint64_t			req_bdev_idx;
	uint32_t			offset_in_strip;
	uint64_t			offset_blocks;
	uint64_t			num_blocks;
	int				ret = 0;
	uint64_t			start_strip_idx;
	uint64_t			end_strip_idx;
	struct raid_base_bdev_info	*base_info;
	struct spdk_io_channel		*base_ch;

	start_strip_idx = bdev_io->u.bdev.offset_blocks >> raid_bdev->strip_size_shift;
	end_strip_idx = (bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks - 1) >>
		    raid_bdev->strip_size_shift;
	if (start_strip_idx != end_strip_idx) {
		SPDK_ERRLOG("I/O spans strip boundary!\n");
		assert(false);
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	assert(raid_ch != NULL);
	assert(raid_ch->base_channel);

	io_opts.size = sizeof(io_opts);
	io_opts.memory_domain = bdev_io->u.bdev.memory_domain;
	io_opts.memory_domain_ctx = bdev_io->u.bdev.memory_domain_ctx;
	io_opts.metadata = bdev_io->u.bdev.md_buf;

	stripe_index = start_strip_idx / (raid_bdev->num_base_bdevs - 1);
	parity_strip_idx = raid5_parity_strip_index(raid_bdev, stripe_index);
	offset_in_strip = bdev_io->u.bdev.offset_blocks % (raid_bdev->strip_size);

	req_bdev_idx = start_strip_idx % (raid_bdev->num_base_bdevs - 1);
	if (req_bdev_idx >= parity_strip_idx) {
		++req_bdev_idx;
	}
	offset_blocks = (stripe_index << raid_bdev->strip_size_shift) + offset_in_strip;
	num_blocks = bdev_io->u.bdev.num_blocks;

	// calculating of broken strip idx
	for (uint8_t idx = 0; idx < raid_bdev->num_base_bdevs; ++idx) {
		if (raid_ch->base_channel[idx] == NULL) {
			if (broken_bdev_idx == raid_bdev->num_base_bdevs) {
				broken_bdev_idx = idx;
			} else {
				SPDK_ERRLOG("2 broken strips\n");
				assert(false);
				raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
		}
	}

	if (broken_bdev_idx == parity_strip_idx) {
		// case: broken parity strip

		base_info = &raid_bdev->base_bdev_info[req_bdev_idx];
		base_ch = raid_ch->base_channel[req_bdev_idx];

		ret = spdk_bdev_writev_blocks_ext(base_info->desc, base_ch,
						  bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
						  offset_blocks, num_blocks, raid5_bdev_io_completion,
						  raid_io, &io_opts);
		
		if (ret == -ENOMEM) {
			raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
					base_ch, _raid5_submit_rw_request);
		} else if (ret != 0) {
			SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
			assert(false);
			raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	} else {
		// cases with parity recalculating

		struct raid5_io_buffer *io_buffer;

		base_info = &raid_bdev->base_bdev_info[parity_strip_idx];
		base_ch = raid_ch->base_channel[parity_strip_idx];

		io_buffer = raid5_get_io_buffer(raid_io, num_blocks * block_size_b);
		if (io_buffer == NULL) {
			raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
					base_ch, _raid5_submit_rw_request);
			return;
		}
		//We're catching requests here
		raid5_catching_requests(io_buffer);

		raid5_submit_write_request_reading(io_buffer);
	}
}

static void
raid5_submit_rw_request(struct raid_bdev_io *raid_io)
{
	struct spdk_bdev_io		*bdev_io = spdk_bdev_io_from_ctx(raid_io);

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		raid5_submit_read_request(raid_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		raid5_submit_write_request(raid_io);
		break;
	default:
		SPDK_ERRLOG("Invalid request type");
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		assert(false);
	}
}

static uint64_t
raid5_calculate_blockcnt(struct raid_bdev *raid_bdev)
{
	uint64_t min_blockcnt = UINT64_MAX;
	struct raid_base_bdev_info *base_info;
	uint64_t total_stripes;
	uint64_t stripe_blockcnt;

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		min_blockcnt = spdk_min(min_blockcnt, spdk_bdev_desc_get_bdev(base_info->desc)->blockcnt);
	}

	total_stripes = min_blockcnt / raid_bdev->strip_size;
	stripe_blockcnt = raid_bdev->strip_size * (raid_bdev->num_base_bdevs - 1);

	SPDK_DEBUGLOG(bdev_raid5, "min blockcount %" PRIu64 ",  numbasedev %u, strip size shift %u\n",
		      min_blockcnt, raid_bdev->num_base_bdevs, raid_bdev->strip_size_shift);

	return total_stripes * stripe_blockcnt;
}

static int
raid5_start(struct raid_bdev *raid_bdev)
{
	raid_bdev->bdev.blockcnt = raid5_calculate_blockcnt(raid_bdev);
	raid_bdev->bdev.optimal_io_boundary = raid_bdev->strip_size;
	raid_bdev->bdev.split_on_optimal_io_boundary = true;
	raid_bdev->min_base_bdevs_operational = raid_bdev->num_base_bdevs - 1;
	raid5_ht.raid5_request_table = ht_create();

	return 0;
}

static void
raid5_resize(struct raid_bdev *raid_bdev)
{
	uint64_t blockcnt;
	int rc;

	blockcnt = raid5_calculate_blockcnt(raid_bdev);

	if (blockcnt == raid_bdev->bdev.blockcnt) {
		return;
	}

	SPDK_NOTICELOG("raid5 '%s': min blockcount was changed from %" PRIu64 " to %" PRIu64 "\n",
		       raid_bdev->bdev.name,
		       raid_bdev->bdev.blockcnt,
		       blockcnt);

	rc = spdk_bdev_notify_blockcnt_change(&raid_bdev->bdev, blockcnt);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to notify blockcount change\n");
	}
}

static struct raid_bdev_module g_raid5_module = {
	.level = RAID5,
	.base_bdevs_min = 3,
	.memory_domains_supported = true,
	.start = raid5_start,
	.submit_rw_request = raid5_submit_rw_request,
	.resize = raid5_resize
};
RAID_MODULE_REGISTER(&g_raid5_module)

SPDK_LOG_REGISTER_COMPONENT(bdev_raid5)
