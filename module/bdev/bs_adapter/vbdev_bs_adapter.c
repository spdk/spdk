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

#include "vbdev_bs_adapter.h"

#include "spdk/config.h"
#include "spdk/nvme.h"

#include "spdk_internal/log.h"

static int adapter_init(void);
static int adapter_get_ctx_size(void);
static void adapter_finish(void);
static int adapter_config_json(struct spdk_json_write_ctx *w);
static void adapter_examine(struct spdk_bdev *bdev);
static void
_adapter_complete_read_modify(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

static struct spdk_bdev_module bdev_adapter_if = {
	.name = "bdev_adapter",
	.module_init = adapter_init,
	.module_fini = adapter_finish,
	.config_text = NULL,
	.config_json = adapter_config_json,
	.examine_config = adapter_examine,
	.get_ctx_size = adapter_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(adapter_block, &bdev_adapter_if)

/* List of adapter vbdev names and their base bdevs via configuration file.
 * Used so we can parse the conf once at init and use this list in examine().
 */
struct bdev_adapter_config {
	char					*vbdev_name;
	char					*bdev_name;
	TAILQ_ENTRY(bdev_adapter_config)	link;
};
static TAILQ_HEAD(, bdev_adapter_config) g_bdev_configs = TAILQ_HEAD_INITIALIZER(g_bdev_configs);

/* List of adapter vbdevs and associated info for each. */
struct bdev_adapter {
	struct spdk_bdev		bdev;    /* the block zoned bdev */
	struct spdk_bdev_desc		*base_desc; /* its descriptor we get from open */
	uint64_t			block_size_scaling;
	bool				busy;
	TAILQ_ENTRY(bdev_adapter)	link;
};
static TAILQ_HEAD(, bdev_adapter) g_bdev_nodes = TAILQ_HEAD_INITIALIZER(g_bdev_nodes);

struct adapter_io_channel {
	struct spdk_io_channel	*base_ch; /* IO channel of base device */
};

enum write_state {
	WRITE_STATE_SLBA_READ,
	WRITE_STATE_SLBA_WRITE,
	WRITE_STATE_MIDLBA_WRITE,
	WRITE_STATE_ELBA_READ,
	WRITE_STATE_ELBA_WRITE,
};

struct adapter_io {
	/* bdev IO was issued to */
	struct bdev_adapter *bdev_adapter;
	union {
		/* Indicates whether the buffer was reallocated and needs to be copied for reads */
		bool copy_buffer;
		struct {
			/* Indicates whether the write needed a read-modify-write for first sector */
			bool slba_unaligned;
			uint64_t slba;
			uint64_t slba_len;
			/* Indicates whether the write needed a read-modify-write for last sector */
			bool elba_unaligned;
			uint64_t elba;
			uint64_t elba_len;
			/* Indicates whether the write has any aligned 4k sector buffer */
			bool midlba_exists;
			uint64_t midlba;
			uint64_t midlba_len;
			enum write_state state;
		} write;
	};
};

static int
adapter_init(void)
{
	return 0;
}

static void
adapter_remove_config(struct bdev_adapter_config *name)
{
	TAILQ_REMOVE(&g_bdev_configs, name, link);
	free(name->bdev_name);
	free(name->vbdev_name);
	free(name);
}

static void
adapter_finish(void)
{
	struct bdev_adapter_config *name;

	while ((name = TAILQ_FIRST(&g_bdev_configs))) {
		adapter_remove_config(name);
	}
}

static int
adapter_get_ctx_size(void)
{
	return sizeof(struct adapter_io);
}

static int
adapter_config_json(struct spdk_json_write_ctx *w)
{
	struct bdev_adapter *bdev_node;
	struct spdk_bdev *base_bdev = NULL;

	TAILQ_FOREACH(bdev_node, &g_bdev_nodes, link) {
		base_bdev = spdk_bdev_desc_get_bdev(bdev_node->base_desc);
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "bdev_bs_adapter_create");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "base_bdev", spdk_bdev_get_name(base_bdev));
		spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&bdev_node->bdev));
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}

	return 0;
}

/* Callback for unregistering the IO device. */
static void
_device_unregister_cb(void *io_device)
{
	struct bdev_adapter *bdev_node  = io_device;

	free(bdev_node->bdev.name);
	free(bdev_node);
}

static int
adapter_destruct(void *ctx)
{
	struct bdev_adapter *bdev_node = (struct bdev_adapter *)ctx;

	TAILQ_REMOVE(&g_bdev_nodes, bdev_node, link);

	/* Unclaim the underlying bdev. */
	spdk_bdev_module_release_bdev(spdk_bdev_desc_get_bdev(bdev_node->base_desc));

	/* Close the underlying bdev. */
	spdk_bdev_close(bdev_node->base_desc);

	/* Unregister the io_device. */
	spdk_io_device_unregister(bdev_node, _device_unregister_cb);

	return 0;
}

static void
_copy_to_iovs(void *buf, struct spdk_bdev_io *destination_io)
{
	size_t buf_len = destination_io->u.bdev.num_blocks * 512;
	struct iovec *iovs = destination_io->u.bdev.iovs;
	int iovcnt = destination_io->u.bdev.iovcnt;
	size_t len;
	int i;

	for (i = 0; i < iovcnt; i++) {
		len = spdk_min(iovs[i].iov_len, buf_len);
		memcpy(iovs[i].iov_base, buf, len);
		buf += len;
		buf_len -= len;
	}
}

static void
_adapter_complete_read(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	struct adapter_io *io_ctx = (struct adapter_io *)orig_io->driver_ctx;
	struct bdev_adapter *bdev_node = SPDK_CONTAINEROF(orig_io->bdev, struct bdev_adapter, bdev);
	uint64_t lba = orig_io->u.bdev.offset_blocks;
	uint64_t start_buf_offset = (lba % bdev_node->block_size_scaling) * 512;
	void *buf = ((uint8_t *)bdev_io->u.bdev.iovs[0].iov_base) + start_buf_offset;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;

	if (io_ctx->copy_buffer) {
		assert(bdev_io->u.bdev.iovcnt == 1);
		_copy_to_iovs(buf, orig_io);
	}

	/* Complete the original IO and then free the one that we created here
	 * as a result of issuing an IO via submit_reqeust.
	 */
	spdk_bdev_io_complete(orig_io, status);
	spdk_bdev_free_io(bdev_io);
}

static int
adapter_read(struct bdev_adapter *bdev_node, struct adapter_io_channel *ch,
	     struct spdk_bdev_io *bdev_io)
{
	struct adapter_io *io_ctx = (struct adapter_io *)bdev_io->driver_ctx;
	uint64_t len = bdev_io->u.bdev.num_blocks;
	uint64_t lba = bdev_io->u.bdev.offset_blocks;
	uint64_t scaling = bdev_node->block_size_scaling;
	uint64_t physical_lba = lba / scaling;
	uint64_t physical_len = 0;
	uint64_t aligned_len = len;
	bool slba_unaligned = false, elba_unaligned = false;
	void *iovs = bdev_io->u.bdev.iovs;
	uint64_t iovcnt = bdev_io->u.bdev.iovcnt;
	int rc = 0;

	if (lba > bdev_node->bdev.blockcnt || (lba + len) > bdev_node->bdev.blockcnt) {
		SPDK_ERRLOG("Read exceeds device capacity (lba 0x%lx, len 0x%lx)\n", lba, len);
		return -EINVAL;
	}

	io_ctx->copy_buffer = false;
	/* Check if the first block starts on unaligned 4k offset - if it does recalculate physical
	 * starting LBA and extend the read length by 1 sector.
	 */
	if (lba % scaling) {
		slba_unaligned = true;
		physical_len++;
		if ((scaling - lba % scaling) > aligned_len) {
			aligned_len = 0;
		} else {
			aligned_len -= (scaling - lba % scaling);
		}
	}

	/* Check if the last block ends on unaligned 4k offset - if it does extend read length by one
	 * sector, but only if the request crossed at least one 4k sector boundary
	 */
	if ((lba + len) % scaling) {
		if (((lba + len) / scaling) != physical_lba || !slba_unaligned) {
			elba_unaligned = true;
			physical_len++;
			aligned_len -= ((lba + len) % scaling);
		}
	}
	physical_len += (aligned_len / scaling);

	if (slba_unaligned || elba_unaligned) {
		iovs = NULL;
		iovcnt = 0;
		io_ctx->copy_buffer = true;
	}
	rc = spdk_bdev_readv_blocks(bdev_node->base_desc, ch->base_ch, iovs,
				    iovcnt, physical_lba,
				    physical_len, _adapter_complete_read,
				    bdev_io);

	return rc;
}

static void
_adapter_complete_write(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

static void
_copy_from_iovs(void *buf, size_t buf_len, struct spdk_bdev_io *source_io, uint64_t offset)
{
	struct iovec *iovs = source_io->u.bdev.iovs;
	uint64_t total_offset = 0, src_iov_offset = 0, shift;
	int iovcnt = source_io->u.bdev.iovcnt;
	int src_index = 0;
	void *src_buf;
	size_t len;

	while (true) {
		shift = spdk_min(source_io->u.bdev.iovs[src_index].iov_len - src_iov_offset, offset - total_offset);
		total_offset += shift;
		src_iov_offset += shift;

		if (shift == 0) {
			break;
		}
		if (src_iov_offset == source_io->u.bdev.iovs[src_index].iov_len) {
			src_iov_offset = 0;
			src_index++;
		}
		if (total_offset == offset) {
			break;
		}
	}

	for (; src_index < iovcnt && buf_len; src_index++) {
		len = spdk_min(iovs[src_index].iov_len - src_iov_offset, buf_len);
		src_buf = ((uint8_t *)source_io->u.bdev.iovs[src_index].iov_base) + src_iov_offset;
		memcpy(buf, src_buf, len);
		buf += len;
		buf_len -= len;
		src_iov_offset = 0;
	}
}

static void
_adapter_complete_zcopy_start(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	struct adapter_io *io_ctx = (struct adapter_io *)orig_io->driver_ctx;
	struct bdev_adapter *bdev_node = SPDK_CONTAINEROF(orig_io->bdev, struct bdev_adapter, bdev);
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	bool complete_request = false;
	void *buf;
	int rc;

	if (!success) {
		goto complete_request;
	}

	buf = bdev_io->u.bdev.iovs[0].iov_base;
	_copy_from_iovs(buf, bdev_io->u.bdev.iovcnt * bdev_node->block_size_scaling * 512, orig_io,
			io_ctx->write.slba_len * 512);

	rc = spdk_bdev_zcopy_end(bdev_io, true, _adapter_complete_write, orig_io);
	if (rc) {
		complete_request = true;
		status = SPDK_BDEV_IO_STATUS_FAILED;
	}
complete_request:
	if (complete_request) {
		if (io_ctx->write.slba_unaligned || io_ctx->write.elba_unaligned) {
			/* Increased QD unlock */
			__atomic_store_n(&bdev_node->busy, false, __ATOMIC_SEQ_CST);
		}
		spdk_bdev_io_complete(orig_io, status);
		spdk_bdev_free_io(bdev_io);
	}
}

static void
_adapter_complete_write(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	struct adapter_io *io_ctx = (struct adapter_io *)orig_io->driver_ctx;
	struct bdev_adapter *bdev_node = SPDK_CONTAINEROF(orig_io->bdev, struct bdev_adapter, bdev);
	struct spdk_io_channel *ioch = spdk_bdev_get_io_channel(bdev_node->base_desc);
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	bool complete_request = false;
	int rc = 0;

	if (!success) {
		goto complete_request;
	}

	switch (io_ctx->write.state) {
	case WRITE_STATE_SLBA_WRITE:
		/* Increased QD unlock */
		if (io_ctx->write.midlba_exists) {
			io_ctx->write.state = WRITE_STATE_MIDLBA_WRITE;
			rc = spdk_bdev_zcopy_start(bdev_node->base_desc, ioch,
						   io_ctx->write.midlba, io_ctx->write.midlba_len,
						   false, _adapter_complete_zcopy_start, orig_io);
		} else if (io_ctx->write.elba_unaligned) {
			io_ctx->write.state = WRITE_STATE_ELBA_READ;
			/* Increased QD lock */
			rc = spdk_bdev_zcopy_start(bdev_node->base_desc, ioch, io_ctx->write.elba,
						   1, true, _adapter_complete_read_modify,
						   orig_io);
		} else {
			complete_request = true;
		}
		break;
	case WRITE_STATE_MIDLBA_WRITE:
		if (!io_ctx->write.elba_unaligned) {
			complete_request = true;
			break;
		}
		/* Increased QD lock */
		io_ctx->write.state = WRITE_STATE_ELBA_READ;
		rc = spdk_bdev_zcopy_start(bdev_node->base_desc, ioch, io_ctx->write.elba,
					   1, true, _adapter_complete_read_modify,
					   orig_io);
		break;
	case WRITE_STATE_ELBA_WRITE:
		/* Increased QD unlock */
		complete_request = true;
		break;
	default:
		SPDK_ERRLOG("Incorrect request state (%x)\n", io_ctx->write.state);
		status = SPDK_BDEV_IO_STATUS_FAILED;
		complete_request = true;
	}

	if (rc) {
		complete_request = true;
		status = SPDK_BDEV_IO_STATUS_FAILED;
	}
	/* Complete the original IO and then free the one that we created here
	 * as a result of issuing an IO via submit_reqeust.
	 */
complete_request:
	if (complete_request) {
		if (io_ctx->write.slba_unaligned || io_ctx->write.elba_unaligned) {
			/* Increased QD unlock */
			__atomic_store_n(&bdev_node->busy, false, __ATOMIC_SEQ_CST);
		}
		spdk_bdev_io_complete(orig_io, status);
	}
	spdk_bdev_free_io(bdev_io);
}

static void
_adapter_complete_read_modify(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	struct adapter_io *io_ctx = (struct adapter_io *)orig_io->driver_ctx;
	struct bdev_adapter *bdev_node = SPDK_CONTAINEROF(orig_io->bdev, struct bdev_adapter, bdev);
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	uint64_t lba = orig_io->u.bdev.offset_blocks;
	uint64_t len = orig_io->u.bdev.num_blocks;
	uint64_t start_buf_offset = (lba % bdev_node->block_size_scaling) * 512;
	uint64_t scaling = bdev_node->block_size_scaling;
	void *buf = NULL;
	int rc = 0;

	if (!success) {
		goto complete_request;
	}

	switch (io_ctx->write.state) {
	case WRITE_STATE_SLBA_READ:
		io_ctx->write.state = WRITE_STATE_SLBA_WRITE;
		buf = ((uint8_t *)bdev_io->u.bdev.iovs[0].iov_base) + start_buf_offset;
		_copy_from_iovs(buf, 512 * bdev_node->block_size_scaling - start_buf_offset, orig_io, 0);
		rc = spdk_bdev_zcopy_end(bdev_io, true, _adapter_complete_write, orig_io);
		break;
	case WRITE_STATE_ELBA_READ:
		io_ctx->write.state = WRITE_STATE_ELBA_WRITE;
		buf = ((uint8_t *)bdev_io->u.bdev.iovs[0].iov_base);
		_copy_from_iovs(buf, ((lba + len) % scaling) * 512, orig_io,
				io_ctx->write.slba_len * 512 + io_ctx->write.midlba_len * 512 * scaling);
		rc = spdk_bdev_zcopy_end(bdev_io, true, _adapter_complete_write, orig_io);
		break;
	default:
		SPDK_ERRLOG("Incorrect request state (%x)\n", io_ctx->write.state);
		status = SPDK_BDEV_IO_STATUS_FAILED;
		success = false;
	}
	if (rc) {
		success = false;
		status = SPDK_BDEV_IO_STATUS_FAILED;
	}

complete_request:
	if (!success) {
		/* Increased QD unlock */
		__atomic_store_n(&bdev_node->busy, false, __ATOMIC_SEQ_CST);
		spdk_bdev_io_complete(orig_io, status);
	}
}

static int
adapter_write(struct bdev_adapter *bdev_node, struct adapter_io_channel *ch,
	      struct spdk_bdev_io *bdev_io)
{
	struct adapter_io *io_ctx = (struct adapter_io *)bdev_io->driver_ctx;
	uint64_t scaling = bdev_node->block_size_scaling;
	uint64_t len = bdev_io->u.bdev.num_blocks;
	uint64_t lba = bdev_io->u.bdev.offset_blocks;
	uint64_t physical_lba = lba / scaling;
	uint64_t aligned_len = len;
	bool slba_unaligned = false;
	void *iovs = bdev_io->u.bdev.iovs;
	uint64_t iovcnt = bdev_io->u.bdev.iovcnt;
	int rc = 0;

	if (lba > bdev_node->bdev.blockcnt || (lba + len) > bdev_node->bdev.blockcnt) {
		SPDK_ERRLOG("Write exceeds device capacity (lba 0x%lx, len 0x%lx)\n", lba, len);
		return -EINVAL;
	}

	memset(io_ctx, 0, sizeof(struct adapter_io));
	/* Check state and skip some parts of this code */
	/* Check if the first block starts on unaligned 4k offset - if it does recalculate physical
	 * starting LBA
	 */
	if (lba % scaling || (aligned_len / scaling) == 0) {
		slba_unaligned = true;
		io_ctx->write.slba_unaligned = true;
		io_ctx->write.slba_len = scaling - lba % scaling;
		if ((aligned_len / scaling) == 0) {
			aligned_len = 0;
		} else {
			aligned_len -= (scaling - lba % scaling);
		}
	}

	/* Check if the last block ends on unaligned 4k offset - if it does extend request by one
	 * sector, but only if the request crossed at least one 4k sector boundary
	 */
	if ((lba + len) % scaling) {
		if (((lba + len) / scaling) != physical_lba || !slba_unaligned) {
			io_ctx->write.elba_unaligned = true;
			io_ctx->write.elba = (lba + len) / scaling;
			if (aligned_len) {
				aligned_len -= ((lba + len) % scaling);
			}
		}
	}

	/* Check if there's at least one aligned 4k part of the write */
	if (aligned_len / scaling) {
		io_ctx->write.midlba_exists = true;
		io_ctx->write.midlba = physical_lba + (slba_unaligned ? 1 : 0);
		io_ctx->write.midlba_len = aligned_len / scaling;
	}

	/* TODO allow for increased QD of unaligned requests */
	if (slba_unaligned || io_ctx->write.elba_unaligned) {
		if (__atomic_exchange_n(&bdev_node->busy, true, __ATOMIC_SEQ_CST)) {
			return -ENOMEM;
		}
	}
	if (!slba_unaligned) {
		io_ctx->write.state = WRITE_STATE_MIDLBA_WRITE;
		rc = spdk_bdev_writev_blocks(bdev_node->base_desc, ch->base_ch, iovs,
					     iovcnt, physical_lba,
					     aligned_len / scaling, _adapter_complete_write,
					     bdev_io);
	} else {
		io_ctx->write.state = WRITE_STATE_SLBA_READ;
		/* Increased QD lock */
		rc = spdk_bdev_zcopy_start(bdev_node->base_desc, ch->base_ch, physical_lba,
					   1, true, _adapter_complete_read_modify,
					   bdev_io);
	}

	return rc;
}

static void
bdev_io_get_buf_cb(struct spdk_io_channel *ioch, struct spdk_bdev_io *bdev_io, bool success)
{
	struct bdev_adapter *bdev_node = SPDK_CONTAINEROF(bdev_io->bdev, struct bdev_adapter, bdev);
	struct adapter_io_channel *dev_ch = spdk_io_channel_get_ctx(ioch);
	int rc;

	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		return;
	}

	rc = adapter_read(bdev_node, dev_ch, bdev_io);
	if (rc != 0) {
		if (rc == -ENOMEM) {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		} else {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

static void
adapter_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct bdev_adapter *bdev_node = SPDK_CONTAINEROF(bdev_io->bdev, struct bdev_adapter, bdev);
	struct adapter_io_channel *dev_ch = spdk_io_channel_get_ctx(ch);
	int rc = 0;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_io_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		rc = adapter_write(bdev_node, dev_ch, bdev_io);
		break;
	default:
		SPDK_ERRLOG("vbdev_adapter: unknown I/O type %u\n", bdev_io->type);
		rc = -ENOTSUP;
		break;
	}

	if (rc != 0) {
		if (rc == -ENOMEM) {
			SPDK_WARNLOG("ENOMEM, start to queue io for vbdev.\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		} else {
			SPDK_ERRLOG("ERROR on bdev_io submission!\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

static bool
adapter_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
		return true;
	default:
		return false;
	}
}

static struct spdk_io_channel *
adapter_get_io_channel(void *ctx)
{
	struct bdev_adapter *bdev_node = (struct bdev_adapter *)ctx;

	return spdk_get_io_channel(bdev_node);
}

static int
adapter_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct bdev_adapter *bdev_node = (struct bdev_adapter *)ctx;
	struct spdk_bdev *base_bdev = spdk_bdev_desc_get_bdev(bdev_node->base_desc);

	spdk_json_write_name(w, "adapter");
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&bdev_node->bdev));
	spdk_json_write_named_string(w, "base_bdev", spdk_bdev_get_name(base_bdev));
	spdk_json_write_object_end(w);

	return 0;
}

/* When we register our vbdev this is how we specify our entry points. */
static const struct spdk_bdev_fn_table adapter_fn_table = {
	.destruct		= adapter_destruct,
	.submit_request		= adapter_submit_request,
	.io_type_supported	= adapter_io_type_supported,
	.get_io_channel		= adapter_get_io_channel,
	.dump_info_json		= adapter_dump_info_json,
};

static void
adapter_base_bdev_hotremove_cb(void *ctx)
{
	struct bdev_adapter *bdev_node, *tmp;
	struct spdk_bdev *bdev_find = ctx;

	TAILQ_FOREACH_SAFE(bdev_node, &g_bdev_nodes, link, tmp) {
		if (bdev_find == spdk_bdev_desc_get_bdev(bdev_node->base_desc)) {
			spdk_bdev_unregister(&bdev_node->bdev, NULL, NULL);
		}
	}
}

static int
_adapter_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct adapter_io_channel *bdev_ch = ctx_buf;
	struct bdev_adapter *bdev_node = io_device;

	bdev_ch->base_ch = spdk_bdev_get_io_channel(bdev_node->base_desc);
	if (!bdev_ch->base_ch) {
		return -ENOMEM;
	}

	return 0;
}

static void
_adapter_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct adapter_io_channel *bdev_ch = ctx_buf;

	spdk_put_io_channel(bdev_ch->base_ch);
}

static int
adapter_insert_name(const char *bdev_name, const char *vbdev_name)
{
	struct bdev_adapter_config *config;

	TAILQ_FOREACH(config, &g_bdev_configs, link) {
		if (strcmp(vbdev_name, config->vbdev_name) == 0) {
			SPDK_ERRLOG("adapter bdev %s already exists\n", vbdev_name);
			return -EEXIST;
		}
		if (strcmp(bdev_name, config->bdev_name) == 0) {
			SPDK_ERRLOG("base bdev %s already claimed\n", bdev_name);
			return -EEXIST;
		}
	}

	config = calloc(1, sizeof(*config));
	if (!config) {
		SPDK_ERRLOG("could not allocate bdev config\n");
		return -ENOMEM;
	}

	config->bdev_name = strdup(bdev_name);
	if (!config->bdev_name) {
		SPDK_ERRLOG("could not allocate config->bdev_name\n");
		free(config);
		return -ENOMEM;
	}

	config->vbdev_name = strdup(vbdev_name);
	if (!config->vbdev_name) {
		SPDK_ERRLOG("could not allocate config->vbdev_name\n");
		free(config->bdev_name);
		free(config);
		return -ENOMEM;
	}


	TAILQ_INSERT_TAIL(&g_bdev_configs, config, link);

	return 0;
}

static int
adapter_register(struct spdk_bdev *base_bdev)
{
	struct bdev_adapter_config *name, *tmp;
	struct bdev_adapter *bdev_node;
	int rc = 0;

	/* Check our list of names from config versus this bdev and if
	 * there's a match, create the bdev_node & bdev accordingly.
	 */
	TAILQ_FOREACH_SAFE(name, &g_bdev_configs, link, tmp) {
		if (strcmp(name->bdev_name, base_bdev->name) != 0) {
			continue;
		}

		if (base_bdev->blocklen == 512) {
			SPDK_ERRLOG("Base bdev %s already has 512B sector size\n", base_bdev->name);
			rc = -EINVAL;
			goto free_config;
		}

		if (spdk_bdev_is_zoned(base_bdev)) {
			SPDK_ERRLOG("Base bdev %s can't be zoned\n", base_bdev->name);
			rc = -EINVAL;
			goto free_config;
		}

		bdev_node = calloc(1, sizeof(struct bdev_adapter));
		if (!bdev_node) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate bdev_node\n");
			goto free_config;
		}

		/* The base bdev that we're attaching to. */
		bdev_node->bdev.name = strdup(name->vbdev_name);
		if (!bdev_node->bdev.name) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate bdev_node name\n");
			goto strdup_failed;
		}
		bdev_node->bdev.product_name = "adapter";
		bdev_node->block_size_scaling = (base_bdev->blocklen / 512);
		if (bdev_node->block_size_scaling <= 1) {
			rc = -EINVAL;
			goto scaling_failed;
		}

		/* Copy some properties from the underlying base bdev. */
		bdev_node->bdev.write_cache = base_bdev->write_cache;
		bdev_node->bdev.required_alignment = base_bdev->required_alignment;
		bdev_node->bdev.optimal_io_boundary = base_bdev->optimal_io_boundary;
		bdev_node->bdev.blocklen = 512;
		bdev_node->bdev.blockcnt = base_bdev->blockcnt * bdev_node->block_size_scaling;

		bdev_node->bdev.md_interleave = base_bdev->md_interleave;
		bdev_node->bdev.md_len = 0;
		bdev_node->bdev.dif_type = base_bdev->dif_type;
		bdev_node->bdev.dif_is_head_of_md = base_bdev->dif_is_head_of_md;
		bdev_node->bdev.dif_check_flags = base_bdev->dif_check_flags;

		bdev_node->bdev.ctxt = bdev_node;
		bdev_node->bdev.fn_table = &adapter_fn_table;
		bdev_node->bdev.module = &bdev_adapter_if;

		TAILQ_INSERT_TAIL(&g_bdev_nodes, bdev_node, link);

		spdk_io_device_register(bdev_node, _adapter_ch_create_cb, _adapter_ch_destroy_cb,
					sizeof(struct adapter_io_channel),
					name->vbdev_name);

		rc = spdk_bdev_open(base_bdev, true, adapter_base_bdev_hotremove_cb,
				    base_bdev, &bdev_node->base_desc);
		if (rc) {
			SPDK_ERRLOG("could not open bdev %s\n", spdk_bdev_get_name(base_bdev));
			goto open_failed;
		}

		rc = spdk_bdev_module_claim_bdev(base_bdev, bdev_node->base_desc, bdev_node->bdev.module);
		if (rc) {
			SPDK_ERRLOG("could not claim bdev %s\n", spdk_bdev_get_name(base_bdev));
			goto claim_failed;
		}

		rc = spdk_bdev_register(&bdev_node->bdev);
		if (rc) {
			SPDK_ERRLOG("could not register adapter bdev\n");
			goto register_failed;
		}
	}

	return rc;
register_failed:
	spdk_bdev_module_release_bdev(&bdev_node->bdev);
claim_failed:
	spdk_bdev_close(bdev_node->base_desc);
open_failed:
	TAILQ_REMOVE(&g_bdev_nodes, bdev_node, link);
	spdk_io_device_unregister(bdev_node, NULL);
scaling_failed:
	free(bdev_node->bdev.name);
strdup_failed:
	free(bdev_node);
free_config:
	adapter_remove_config(name);
	return rc;
}

int
spdk_vbdev_bs_adapter_create(const char *bdev_name, const char *vbdev_name)
{
	struct spdk_bdev *bdev = NULL;
	int rc = 0;

	/* Insert the bdev into our global name list even if it doesn't exist yet,
	 * it may show up soon...
	 */
	rc = adapter_insert_name(bdev_name, vbdev_name);
	if (rc) {
		return rc;
	}

	bdev = spdk_bdev_get_by_name(bdev_name);
	if (!bdev) {
		/* This is not an error, even though the bdev is not present at this time it may
		 * still show up later.
		 */
		return 0;
	}

	return adapter_register(bdev);
}

void
spdk_vbdev_bs_adapter_delete(const char *name, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct bdev_adapter_config *name_node;
	struct spdk_bdev *bdev = NULL;

	bdev = spdk_bdev_get_by_name(name);
	if (!bdev || bdev->module != &bdev_adapter_if) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	TAILQ_FOREACH(name_node, &g_bdev_configs, link) {
		if (strcmp(name_node->vbdev_name, bdev->name) == 0) {
			adapter_remove_config(name_node);
			break;
		}
	}

	spdk_bdev_unregister(bdev, cb_fn, cb_arg);
}

static void
adapter_examine(struct spdk_bdev *bdev)
{
	adapter_register(bdev);

	spdk_bdev_module_examine_done(&bdev_adapter_if);
}

SPDK_LOG_REGISTER_COMPONENT("vbdev_bs_adapter", SPDK_LOG_VBDEV_BS_ADAPTER)
