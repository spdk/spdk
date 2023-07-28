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

#include "spdk/log.h"

static inline uint8_t
raid5_parity_strip_index(struct raid_bdev *raid_bdev, uint64_t stripe_index) {
	return raid_bdev->num_base_bdevs - 1 - stripe_index % raid_bdev->num_base_bdevs;
}

static inline struct iovec *
raid5_get_buffer(size_t iovlen) {
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
raid5_free_buffer(struct iovec *buffer) {
	free(buffer->iov_base);
	free(buffer);
}

static void
raid5_fill_buffer_with_zeroes(struct iovec *buffer) {
	uint64_t *b8 = buffer->iov_base;
	char *b = buffer->iov_base;
	size_t len8 = buffer->iov_len / 8;
	size_t len = buffer->iov_len;

	for (size_t i=0; i < len8; ++i) {
		b8[i] = 0;
	}

	len8 *= 8;
	for (size_t i = len8; i < len; ++i) {
		b[i] = 0;
	}
}

static void
raid5_xor_buffers(struct iovec *xor_res, struct iovec *buffer) {
	uint64_t *xb8 = xor_res->iov_base;
	uint64_t *b8 = buffer->iov_base;
	char *xb = xor_res->iov_base;
	char *b = buffer->iov_base;
	size_t len8 = xor_res->iov_len / 8;
	size_t len = xor_res->iov_len;

	for (size_t i=0; i < len8; ++i) {
		xb8[i] ^= b8[i];
	}

	len8 *= 8;
	for (size_t i = len8; i < len; ++i) {
		xb[i] ^= b[i];
	}
}

static void
raid5_copy_iovec(struct iovec *dst, struct iovec *src) {
	uint64_t *db8 = dst->iov_base;
	uint64_t *sb8 = src->iov_base;
	char *db = dst->iov_base;
	char *sb = src->iov_base;
	size_t len8 = dst->iov_len / 8;
	size_t len = dst->iov_len;

	for (size_t i=0; i < len8; ++i) {
		db8[i] = sb8[i];
	}

	len8 *= 8;
	for (size_t i = len8; i < len; ++i) {
		db[i] = sb[i];
	}
}

static void
raid5_bdev_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	raid_bdev_io_complete_part(raid_io, 1, success ?
				   SPDK_BDEV_IO_STATUS_SUCCESS :
				   SPDK_BDEV_IO_STATUS_FAILED);
}

static void raid5_submit_rw_request(struct raid_bdev_io *raid_io);

static void
_raid5_submit_rw_request(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;

	raid5_submit_rw_request(raid_io);
}

static int
raid5_submit_read_request(struct raid_bdev_io *raid_io) {
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
		return -EINVAL;
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
		// reading only one strip case
		raid_io->base_bdev_io_remaining = 1;

		ret = spdk_bdev_readv_blocks_ext(base_info->desc, base_ch,
						 bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
						 offset_blocks, num_blocks, raid5_bdev_io_completion,
						 raid_io, &io_opts);

		if (ret == -ENOMEM) {
			SPDK_ERRLOG("ENOMEM on reading request in RAID5\n");
			assert(false);

			raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
					base_ch, _raid5_submit_rw_request);
			return 0;
		}

		return ret;
	} else {
		// reading stripe case
		if (raid_io->base_bdev_io_submitted == 0) {
			raid_io->base_bdev_io_remaining = raid_bdev->num_base_bdevs;
		}

		struct iovec *buffer = raid5_get_buffer(num_blocks * block_size_b);
		if (buffer == NULL) {
			return -ENOMEM;
		}

		struct iovec *xor_res = raid5_get_buffer(num_blocks * block_size_b);
		if (xor_res == NULL) {
			raid5_free_buffer(buffer);
			return -ENOMEM;
		}

		raid5_fill_buffer_with_zeroes(xor_res);

		uint8_t num_base_bdevs = raid_bdev->num_base_bdevs;

		for (uint8_t idx = 0; idx < num_base_bdevs; ++idx) {
			base_info = &raid_bdev->base_bdev_info[idx];
			base_ch = raid_ch->base_channel[idx];
			if (base_ch == NULL) {
				if (idx == req_bdev_idx) {
					continue;
				} else {
					SPDK_ERRLOG("2 broken strips\n");
					raid5_free_buffer(buffer);
					raid5_free_buffer(xor_res);
					return -EIO;	
				}
			}

			ret = spdk_bdev_readv_blocks_ext(base_info->desc, base_ch,
						 buffer, 1,
						 offset_blocks, num_blocks, raid5_bdev_io_completion,
						 raid_io, &io_opts);

			if (ret != 0) {
				if (ret == -ENOMEM) {
					raid5_free_buffer(buffer);
					raid5_free_buffer(xor_res);
					SPDK_ERRLOG("ENOMEM on read request in RAID5\n");
					assert(false);
					raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
							base_ch, _raid5_submit_rw_request);
				}

				raid5_free_buffer(buffer);
				raid5_free_buffer(xor_res);
				return ret;
			}
			
			raid5_xor_buffers(xor_res, buffer);

			raid_io->base_bdev_io_submitted++;
		}

		// copying result to request iovec
		raid5_copy_iovec(bdev_io->u.bdev.iovs, xor_res);

		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_SUCCESS);

		raid5_free_buffer(buffer);
		raid5_free_buffer(xor_res);

		return 0;
	}
}

static int
raid5_submit_write_request(struct raid_bdev_io *raid_io) {
	struct spdk_bdev_io		*bdev_io = spdk_bdev_io_from_ctx(raid_io);
	struct spdk_bdev_ext_io_opts	io_opts = {};
	struct raid_bdev_io_channel	*raid_ch = raid_io->raid_ch;
	struct raid_bdev		*raid_bdev = raid_io->raid_bdev;
	uint64_t			block_size_b = (raid_bdev->strip_size_kb / raid_bdev->strip_size) * (uint64_t)1024;
	uint8_t				num_base_bdevs = raid_bdev->num_base_bdevs;
	uint8_t				broken_bdev_idx = num_base_bdevs;
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
		return -EINVAL;
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
	parity_strip_idx = raid5_parity_strip_index(raid_bdev, stripe_index);

	// calculating of broken strip idx
	for (uint8_t idx = 0; idx < num_base_bdevs; ++idx) {
		if (raid_ch->base_channel[idx] == NULL) {
			if (broken_bdev_idx == num_base_bdevs) {
				broken_bdev_idx = idx;
			} else {
				SPDK_ERRLOG("2 broken strips\n");
				return -EIO;
			}
		}
	}

	if (broken_bdev_idx == parity_strip_idx) {
		raid_io->base_bdev_io_remaining = 1;

		base_info = &raid_bdev->base_bdev_info[req_bdev_idx];
		base_ch = raid_ch->base_channel[req_bdev_idx];

		ret = spdk_bdev_writev_blocks_ext(base_info->desc, base_ch,
						  bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
						  offset_blocks, num_blocks, raid5_bdev_io_completion,
						  raid_io, &io_opts);
		
		if (ret == -ENOMEM) {
			SPDK_ERRLOG("ENOMEM on write request in RAID5\n");
			assert(false);

			raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
					base_ch, _raid5_submit_rw_request);
			return 0;
		}
		return ret;
	} else {
		struct iovec *buffer = raid5_get_buffer(num_blocks * block_size_b);
		if (buffer == NULL) {
			return -ENOMEM;
		}

		struct iovec *xor_res = raid5_get_buffer(num_blocks * block_size_b);
		if (xor_res == NULL) {
			raid5_free_buffer(buffer);
			return -ENOMEM;
		}

		if (broken_bdev_idx != req_bdev_idx && broken_bdev_idx != num_base_bdevs) {
			raid_io->base_bdev_io_remaining = 4;
		
			// reading

			base_info = &raid_bdev->base_bdev_info[parity_strip_idx];
			base_ch = raid_ch->base_channel[parity_strip_idx];
			ret = spdk_bdev_readv_blocks_ext(base_info->desc, base_ch,
						 xor_res, 1,
						 offset_blocks, num_blocks, raid5_bdev_io_completion,
						 raid_io, &io_opts);
			if (ret != 0) {
				if (ret == -ENOMEM) {
					raid5_free_buffer(buffer);
					raid5_free_buffer(xor_res);
					SPDK_ERRLOG("ENOMEM on write request in RAID5\n");
					assert(false);
					raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
							base_ch, _raid5_submit_rw_request);
				}
				raid5_free_buffer(buffer);
				raid5_free_buffer(xor_res);
				return ret;
			}
			raid_io->base_bdev_io_submitted++;

			base_info = &raid_bdev->base_bdev_info[req_bdev_idx];
			base_ch = raid_ch->base_channel[req_bdev_idx];
			ret = spdk_bdev_readv_blocks_ext(base_info->desc, base_ch,
						 buffer, 1,
						 offset_blocks, num_blocks, raid5_bdev_io_completion,
						 raid_io, &io_opts);
			if (ret != 0) {
				if (ret == -ENOMEM) {
					raid5_free_buffer(buffer);
					raid5_free_buffer(xor_res);
					SPDK_ERRLOG("ENOMEM on write request in RAID5\n");
					assert(false);
					raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
							base_ch, _raid5_submit_rw_request);
				}
				raid5_free_buffer(buffer);
				raid5_free_buffer(xor_res);
				return ret;
			}
			raid_io->base_bdev_io_submitted++;

			// new parity calculation

			raid5_xor_buffers(xor_res, buffer);
			raid5_xor_buffers(xor_res, &bdev_io->u.bdev.iovs[0]);

			// writing

			ret = spdk_bdev_writev_blocks_ext(base_info->desc, base_ch,
						 bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
						 offset_blocks, num_blocks, raid5_bdev_io_completion,
						 raid_io, &io_opts);
			if (ret != 0) {
				if (ret == -ENOMEM) {
					raid5_free_buffer(buffer);
					raid5_free_buffer(xor_res);
					SPDK_ERRLOG("ENOMEM on write request in RAID5\n");
					assert(false);
					raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
							base_ch, _raid5_submit_rw_request);
				}
				raid5_free_buffer(buffer);
				raid5_free_buffer(xor_res);
				return ret;
			}
			raid_io->base_bdev_io_submitted++;

			base_info = &raid_bdev->base_bdev_info[parity_strip_idx];
			base_ch = raid_ch->base_channel[parity_strip_idx];
			ret = spdk_bdev_writev_blocks_ext(base_info->desc, base_ch,
						 xor_res, 1,
						 offset_blocks, num_blocks, raid5_bdev_io_completion,
						 raid_io, &io_opts);
			if (ret != 0) {
				if (ret == -ENOMEM) {
					raid5_free_buffer(buffer);
					raid5_free_buffer(xor_res);
					SPDK_ERRLOG("ENOMEM on write request in RAID5\n");
					assert(false);
					raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
							base_ch, _raid5_submit_rw_request);
				}
				raid5_free_buffer(buffer);
				raid5_free_buffer(xor_res);
				return ret;
			}
			raid_io->base_bdev_io_submitted++;
		} else {
			if (broken_bdev_idx == req_bdev_idx) {
				if (raid_io->base_bdev_io_submitted == 0) {
					raid_io->base_bdev_io_remaining = raid_bdev->num_base_bdevs - 1;
				}
				raid5_fill_buffer_with_zeroes(xor_res);
				
				for (uint8_t idx = 0; idx < num_base_bdevs; ++idx) {
					base_info = &raid_bdev->base_bdev_info[idx];
					base_ch = raid_ch->base_channel[idx];
					if (idx == parity_strip_idx || idx == req_bdev_idx) {
						continue;
					}

					ret = spdk_bdev_readv_blocks_ext(base_info->desc, base_ch,
								buffer, 1,
								offset_blocks, num_blocks, raid5_bdev_io_completion,
								raid_io, &io_opts);

					if (ret != 0) {
						if (ret == -ENOMEM) {
							raid5_free_buffer(buffer);
							raid5_free_buffer(xor_res);
							SPDK_ERRLOG("ENOMEM on write request in RAID5\n");
							assert(false);
							raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
									base_ch, _raid5_submit_rw_request);
						}

						raid5_free_buffer(buffer);
						raid5_free_buffer(xor_res);
						return ret;
					}
					
					raid5_xor_buffers(xor_res, buffer);

					raid_io->base_bdev_io_submitted++;
				}

				raid5_xor_buffers(xor_res, &bdev_io->u.bdev.iovs[0]);

				base_info = &raid_bdev->base_bdev_info[parity_strip_idx];
				base_ch = raid_ch->base_channel[parity_strip_idx];
				ret = spdk_bdev_writev_blocks_ext(base_info->desc, base_ch,
							xor_res, 1,
							offset_blocks, num_blocks, raid5_bdev_io_completion,
							raid_io, &io_opts);
				if (ret != 0) {
					if (ret == -ENOMEM) {
						raid5_free_buffer(buffer);
						raid5_free_buffer(xor_res);
						SPDK_ERRLOG("ENOMEM on write request in RAID5\n");
						assert(false);
						raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
								base_ch, _raid5_submit_rw_request);
					}
					raid5_free_buffer(buffer);
					raid5_free_buffer(xor_res);
					return ret;
				}
			} else {
				if (raid_io->base_bdev_io_submitted == 0) {
					raid_io->base_bdev_io_remaining = raid_bdev->num_base_bdevs;
				}
				raid5_fill_buffer_with_zeroes(xor_res);
				
				for (uint8_t idx = 0; idx < num_base_bdevs; ++idx) {
					base_info = &raid_bdev->base_bdev_info[idx];
					base_ch = raid_ch->base_channel[idx];
					if (idx == parity_strip_idx || idx == req_bdev_idx) {
						continue;
					}

					ret = spdk_bdev_readv_blocks_ext(base_info->desc, base_ch,
								buffer, 1,
								offset_blocks, num_blocks, raid5_bdev_io_completion,
								raid_io, &io_opts);

					if (ret != 0) {
						if (ret == -ENOMEM) {
							raid5_free_buffer(buffer);
							raid5_free_buffer(xor_res);
							SPDK_ERRLOG("ENOMEM on write request in RAID5\n");
							assert(false);
							raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
									base_ch, _raid5_submit_rw_request);
						}

						raid5_free_buffer(buffer);
						raid5_free_buffer(xor_res);
						return ret;
					}
					
					raid5_xor_buffers(xor_res, buffer);

					raid_io->base_bdev_io_submitted++;
				}

				raid5_xor_buffers(xor_res, &bdev_io->u.bdev.iovs[0]);

				base_info = &raid_bdev->base_bdev_info[req_bdev_idx];
				base_ch = raid_ch->base_channel[req_bdev_idx];
				ret = spdk_bdev_writev_blocks_ext(base_info->desc, base_ch,
							bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
							offset_blocks, num_blocks, raid5_bdev_io_completion,
							raid_io, &io_opts);
				if (ret != 0) {
					if (ret == -ENOMEM) {
						raid5_free_buffer(buffer);
						raid5_free_buffer(xor_res);
						SPDK_ERRLOG("ENOMEM on write request in RAID5\n");
						assert(false);
						raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
								base_ch, _raid5_submit_rw_request);
					}
					raid5_free_buffer(buffer);
					raid5_free_buffer(xor_res);
					return ret;
				}

				base_info = &raid_bdev->base_bdev_info[parity_strip_idx];
				base_ch = raid_ch->base_channel[parity_strip_idx];
				ret = spdk_bdev_writev_blocks_ext(base_info->desc, base_ch,
							xor_res, 1,
							offset_blocks, num_blocks, raid5_bdev_io_completion,
							raid_io, &io_opts);
				if (ret != 0) {
					if (ret == -ENOMEM) {
						raid5_free_buffer(buffer);
						raid5_free_buffer(xor_res);
						SPDK_ERRLOG("ENOMEM on write request in RAID5\n");
						assert(false);
						raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
								base_ch, _raid5_submit_rw_request);
					}
					raid5_free_buffer(buffer);
					raid5_free_buffer(xor_res);
					return ret;
				}
			}
		}
		raid5_free_buffer(buffer);
		raid5_free_buffer(xor_res);
		
		return 0;
	}
}

static void
raid5_submit_rw_request(struct raid_bdev_io *raid_io)
{
	struct spdk_bdev_io		*bdev_io = spdk_bdev_io_from_ctx(raid_io);
	int				ret = 0;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		ret = raid5_submit_read_request(raid_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		ret = raid5_submit_write_request(raid_io);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	
	if (ret != 0) {
		SPDK_ERRLOG("bdev io submit error, it should not happen\n");
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
