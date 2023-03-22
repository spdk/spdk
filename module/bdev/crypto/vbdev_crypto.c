/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "vbdev_crypto.h"

#include "spdk_internal/assert.h"
#include "spdk/thread.h"
#include "spdk/bdev_module.h"
#include "spdk/likely.h"

/* Limit the max IO size by some reasonable value. Since in write operation we use aux buffer,
 * let's set the limit to the bdev bounce aux buffer size */
#define CRYPTO_MAX_IO SPDK_BDEV_LARGE_BUF_MAX_SIZE

struct bdev_names {
	struct vbdev_crypto_opts	*opts;
	TAILQ_ENTRY(bdev_names)		link;
};

/* List of crypto_bdev names and their base bdevs via configuration file. */
static TAILQ_HEAD(, bdev_names) g_bdev_names = TAILQ_HEAD_INITIALIZER(g_bdev_names);

struct vbdev_crypto {
	struct spdk_bdev		*base_bdev;		/* the thing we're attaching to */
	struct spdk_bdev_desc		*base_desc;		/* its descriptor we get from open */
	struct spdk_bdev		crypto_bdev;		/* the crypto virtual bdev */
	struct vbdev_crypto_opts	*opts;			/* crypto options such as names and DEK */
	TAILQ_ENTRY(vbdev_crypto)	link;
	struct spdk_thread		*thread;		/* thread where base device is opened */
};

/* List of virtual bdevs and associated info for each. We keep the device friendly name here even
 * though its also in the device struct because we use it early on.
 */
static TAILQ_HEAD(, vbdev_crypto) g_vbdev_crypto = TAILQ_HEAD_INITIALIZER(g_vbdev_crypto);

/* The crypto vbdev channel struct. It is allocated and freed on my behalf by the io channel code.
 * We store things in here that are needed on per thread basis like the base_channel for this thread.
 */
struct crypto_io_channel {
	struct spdk_io_channel		*base_ch;	/* IO channel of base device */
	struct spdk_io_channel		*accel_channel;	/* Accel engine channel used for crypto ops */
	struct spdk_accel_crypto_key	*crypto_key;
	TAILQ_HEAD(, spdk_bdev_io)	in_accel_fw;	/* request submitted to accel fw */
	struct spdk_io_channel_iter	*reset_iter;	/* used with for_each_channel in reset */
};

enum crypto_io_resubmit_state {
	CRYPTO_IO_NEW,		/* Resubmit IO from the scratch */
	CRYPTO_IO_READ_DONE,	/* Need to decrypt */
	CRYPTO_IO_ENCRYPT_DONE,	/* Need to write */
};

/* This is the crypto per IO context that the bdev layer allocates for us opaquely and attaches to
 * each IO for us.
 */
struct crypto_bdev_io {
	struct crypto_io_channel *crypto_ch;		/* need to store for crypto completion handling */
	struct vbdev_crypto *crypto_bdev;		/* the crypto node struct associated with this IO */
	struct spdk_bdev_io *read_io;			/* the read IO we issued */
	/* Used for the single contiguous buffer that serves as the crypto destination target for writes */
	uint64_t aux_num_blocks;			/* num of blocks for the contiguous buffer */
	uint64_t aux_offset_blocks;			/* block offset on media */
	void *aux_buf_raw;				/* raw buffer that the bdev layer gave us for write buffer */
	struct iovec aux_buf_iov;			/* iov representing aligned contig write buffer */

	/* for bdev_io_wait */
	struct spdk_bdev_io_wait_entry bdev_io_wait;
	enum crypto_io_resubmit_state resubmit_state;
};

static void vbdev_crypto_queue_io(struct spdk_bdev_io *bdev_io,
				  enum crypto_io_resubmit_state state);
static void _complete_internal_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void _complete_internal_read(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void _complete_internal_write(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void vbdev_crypto_examine(struct spdk_bdev *bdev);
static int vbdev_crypto_claim(const char *bdev_name);
static void vbdev_crypto_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);

/* Following an encrypt or decrypt we need to then either write the encrypted data or finish
 * the read on decrypted data. Do that here.
 */
static void
_crypto_operation_complete(void *ref, int status)
{
	struct spdk_bdev_io *bdev_io = ref;
	struct vbdev_crypto *crypto_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_crypto,
					   crypto_bdev);
	struct crypto_bdev_io *crypto_io = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	struct crypto_io_channel *crypto_ch = crypto_io->crypto_ch;
	struct spdk_bdev_io *free_me = crypto_io->read_io;
	int rc = 0;

	if (status || crypto_ch->reset_iter) {
		/* If we're completing this with an outstanding reset we need to fail it */
		rc = -EINVAL;
	}

	TAILQ_REMOVE(&crypto_ch->in_accel_fw, bdev_io, module_link);

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		/* Complete the original IO and then free the one that we created
		 * as a result of issuing an IO via submit_request.
		 */
		if (!rc) {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		} else {
			SPDK_ERRLOG("Issue with decryption on bdev_io %p\n", bdev_io);
		}
		spdk_bdev_free_io(free_me);

	} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		if (!rc) {
			/* Write the encrypted data. */
			rc = spdk_bdev_writev_blocks(crypto_bdev->base_desc, crypto_ch->base_ch,
						     &crypto_io->aux_buf_iov, 1, crypto_io->aux_offset_blocks,
						     crypto_io->aux_num_blocks, _complete_internal_write,
						     bdev_io);
			if (rc == -ENOMEM) {
				vbdev_crypto_queue_io(bdev_io, CRYPTO_IO_ENCRYPT_DONE);
				goto check_reset;
			}
		} else {
			SPDK_ERRLOG("Issue with encryption on bdev_io %p\n", bdev_io);
		}
	} else {
		SPDK_ERRLOG("Unknown bdev type %u on crypto operation completion\n", bdev_io->type);
		rc = -EINVAL;
	}

	if (rc) {
		if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
			spdk_bdev_io_put_aux_buf(bdev_io, crypto_io->aux_buf_raw);
		}
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}

check_reset:
	/* If the channel iter is not NULL, we need to wait
	 * until the pending list is empty, then we can move on to the
	 * next channel.
	 */
	if (crypto_ch->reset_iter && TAILQ_EMPTY(&crypto_ch->in_accel_fw)) {
		SPDK_NOTICELOG("Channel %p has been quiesced.\n", crypto_ch);
		spdk_for_each_channel_continue(crypto_ch->reset_iter, 0);
		crypto_ch->reset_iter = NULL;
	}
}

/* We're either encrypting on the way down or decrypting on the way back. */
static int
_crypto_operation(struct spdk_bdev_io *bdev_io, bool encrypt, void *aux_buf)
{
	struct crypto_bdev_io *crypto_io = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	struct crypto_io_channel *crypto_ch = crypto_io->crypto_ch;
	uint32_t crypto_len = crypto_io->crypto_bdev->crypto_bdev.blocklen;
	uint64_t total_length;
	uint64_t alignment;
	int rc;

	/* For encryption, we need to prepare a single contiguous buffer as the encryption
	 * destination, we'll then pass that along for the write after encryption is done.
	 * This is done to avoiding encrypting the provided write buffer which may be
	 * undesirable in some use cases.
	 */
	if (encrypt) {
		total_length = bdev_io->u.bdev.num_blocks * crypto_len;
		alignment = spdk_bdev_get_buf_align(&crypto_io->crypto_bdev->crypto_bdev);
		crypto_io->aux_buf_iov.iov_len = total_length;
		crypto_io->aux_buf_raw = aux_buf;
		crypto_io->aux_buf_iov.iov_base  = (void *)(((uintptr_t)aux_buf + (alignment - 1)) & ~
						   (alignment - 1));
		crypto_io->aux_offset_blocks = bdev_io->u.bdev.offset_blocks;
		crypto_io->aux_num_blocks = bdev_io->u.bdev.num_blocks;

		rc = spdk_accel_submit_encrypt(crypto_ch->accel_channel, crypto_ch->crypto_key,
					       &crypto_io->aux_buf_iov, 1,
					       bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					       bdev_io->u.bdev.offset_blocks, crypto_len, 0,
					       _crypto_operation_complete, bdev_io);
	} else {
		rc = spdk_accel_submit_decrypt(crypto_ch->accel_channel, crypto_ch->crypto_key,
					       bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.iovs,
					       bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks,
					       crypto_len, 0,
					       _crypto_operation_complete, bdev_io);
	}

	if (!rc) {
		TAILQ_INSERT_TAIL(&crypto_ch->in_accel_fw, bdev_io, module_link);
	}

	return rc;
}

/* This function is called after all channels have been quiesced following
 * a bdev reset.
 */
static void
_ch_quiesce_done(struct spdk_io_channel_iter *i, int status)
{
	struct crypto_bdev_io *crypto_io = spdk_io_channel_iter_get_ctx(i);
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(crypto_io);

	assert(TAILQ_EMPTY(&crypto_io->crypto_ch->in_accel_fw));

	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
}

static void
_ch_quiesce(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct crypto_io_channel *crypto_ch = spdk_io_channel_get_ctx(ch);

	if (TAILQ_EMPTY(&crypto_ch->in_accel_fw)) {
		spdk_for_each_channel_continue(i, 0);
	} else {
		/* In accel completion callback we will see the non-NULL iter and handle the quiesce */
		crypto_ch->reset_iter = i;
	}
}

/* Completion callback for IO that were issued from this bdev other than read/write.
 * They have their own for readability.
 */
static void
_complete_internal_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_RESET) {
		struct crypto_bdev_io *orig_ctx = (struct crypto_bdev_io *)orig_io->driver_ctx;

		spdk_bdev_free_io(bdev_io);

		spdk_for_each_channel(orig_ctx->crypto_bdev,
				      _ch_quiesce,
				      orig_ctx,
				      _ch_quiesce_done);
		return;
	}

	spdk_bdev_io_complete(orig_io, status);
	spdk_bdev_free_io(bdev_io);
}

/* Completion callback for writes that were issued from this bdev. */
static void
_complete_internal_write(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	struct crypto_bdev_io *orig_ctx = (struct crypto_bdev_io *)orig_io->driver_ctx;

	spdk_bdev_io_put_aux_buf(orig_io, orig_ctx->aux_buf_raw);

	spdk_bdev_io_complete(orig_io, status);
	spdk_bdev_free_io(bdev_io);
}

/* Completion callback for reads that were issued from this bdev. */
static void
_complete_internal_read(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	struct crypto_bdev_io *orig_ctx = (struct crypto_bdev_io *)orig_io->driver_ctx;
	int rc;

	if (success) {
		/* Save off this bdev_io so it can be freed after decryption. */
		orig_ctx->read_io = bdev_io;
		rc = _crypto_operation(orig_io, false, NULL);
		if (!rc) {
			return;
		} else {
			if (rc == -ENOMEM) {
				SPDK_DEBUGLOG(vbdev_crypto, "No memory, queue the IO.\n");
				spdk_bdev_io_complete(orig_io, SPDK_BDEV_IO_STATUS_NOMEM);
				spdk_bdev_free_io(bdev_io);
				return;
			} else {
				SPDK_ERRLOG("Failed to decrypt, rc %d\n", rc);
			}
		}
	} else {
		SPDK_ERRLOG("Failed to read prior to decrypting!\n");
	}

	spdk_bdev_io_complete(orig_io, SPDK_BDEV_IO_STATUS_FAILED);
	spdk_bdev_free_io(bdev_io);
}

static void
vbdev_crypto_resubmit_io(void *arg)
{
	struct spdk_bdev_io *bdev_io = (struct spdk_bdev_io *)arg;
	struct crypto_bdev_io *crypto_io = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	struct spdk_io_channel *ch;

	switch (crypto_io->resubmit_state) {
	case CRYPTO_IO_NEW:
		assert(crypto_io->crypto_ch);
		ch = spdk_io_channel_from_ctx(crypto_io->crypto_ch);
		vbdev_crypto_submit_request(ch, bdev_io);
		break;
	case CRYPTO_IO_ENCRYPT_DONE:
		_crypto_operation_complete(bdev_io, 0);
		break;
	case CRYPTO_IO_READ_DONE:
		_complete_internal_read(crypto_io->read_io, true, bdev_io);
		break;
	default:
		SPDK_UNREACHABLE();
	}
}

static void
vbdev_crypto_queue_io(struct spdk_bdev_io *bdev_io, enum crypto_io_resubmit_state state)
{
	struct crypto_bdev_io *crypto_io = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	int rc;

	crypto_io->bdev_io_wait.bdev = bdev_io->bdev;
	crypto_io->bdev_io_wait.cb_fn = vbdev_crypto_resubmit_io;
	crypto_io->bdev_io_wait.cb_arg = bdev_io;
	crypto_io->resubmit_state = state;

	rc = spdk_bdev_queue_io_wait(bdev_io->bdev, crypto_io->crypto_ch->base_ch,
				     &crypto_io->bdev_io_wait);
	if (rc != 0) {
		SPDK_ERRLOG("Queue io failed in vbdev_crypto_queue_io, rc=%d.\n", rc);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/* Callback for getting a buf from the bdev pool in the event that the caller passed
 * in NULL, we need to own the buffer so it doesn't get freed by another vbdev module
 * beneath us before we're done with it.
 */
static void
crypto_read_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		       bool success)
{
	struct vbdev_crypto *crypto_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_crypto,
					   crypto_bdev);
	struct crypto_io_channel *crypto_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	rc = spdk_bdev_readv_blocks(crypto_bdev->base_desc, crypto_ch->base_ch, bdev_io->u.bdev.iovs,
				    bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks,
				    bdev_io->u.bdev.num_blocks, _complete_internal_read,
				    bdev_io);
	if (rc != 0) {
		if (rc == -ENOMEM) {
			SPDK_DEBUGLOG(vbdev_crypto, "No memory, queue the IO.\n");
			vbdev_crypto_queue_io(bdev_io, CRYPTO_IO_NEW);
		} else {
			SPDK_ERRLOG("Failed to submit bdev_io!\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

/* For encryption we don't want to encrypt the data in place as the host isn't
 * expecting us to mangle its data buffers so we need to encrypt into the bdev
 * aux buffer, then we can use that as the source for the disk data transfer.
 */
static void
crypto_write_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
			void *aux_buf)
{
	int rc;

	if (spdk_unlikely(!aux_buf)) {
		SPDK_ERRLOG("Failed to get aux buffer!\n");
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	rc = _crypto_operation(bdev_io, true, aux_buf);
	if (rc != 0) {
		spdk_bdev_io_put_aux_buf(bdev_io, aux_buf);
		if (rc == -ENOMEM) {
			SPDK_DEBUGLOG(vbdev_crypto, "No memory, queue the IO.\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		} else {
			SPDK_ERRLOG("Failed to submit crypto operation!\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

/* Called when someone submits IO to this crypto vbdev. For IO's not relevant to crypto,
 * we're simply passing it on here via SPDK IO calls which in turn allocate another bdev IO
 * and call our cpl callback provided below along with the original bdev_io so that we can
 * complete it once this IO completes. For crypto operations, we'll either encrypt it first
 * (writes) then call back into bdev to submit it or we'll submit a read and then catch it
 * on the way back for decryption.
 */
static void
vbdev_crypto_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_crypto *crypto_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_crypto,
					   crypto_bdev);
	struct crypto_io_channel *crypto_ch = spdk_io_channel_get_ctx(ch);
	struct crypto_bdev_io *crypto_io = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	int rc = 0;

	memset(crypto_io, 0, sizeof(struct crypto_bdev_io));
	crypto_io->crypto_bdev = crypto_bdev;
	crypto_io->crypto_ch = crypto_ch;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, crypto_read_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		/* Tell the bdev layer that we need an aux buf in addition to the data
		 * buf already associated with the bdev.
		 */
		spdk_bdev_io_get_aux_buf(bdev_io, crypto_write_get_buf_cb);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = spdk_bdev_unmap_blocks(crypto_bdev->base_desc, crypto_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _complete_internal_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = spdk_bdev_flush_blocks(crypto_bdev->base_desc, crypto_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _complete_internal_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		rc = spdk_bdev_reset(crypto_bdev->base_desc, crypto_ch->base_ch,
				     _complete_internal_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	default:
		SPDK_ERRLOG("crypto: unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (rc != 0) {
		if (rc == -ENOMEM) {
			SPDK_DEBUGLOG(vbdev_crypto, "No memory, queue the IO.\n");
			vbdev_crypto_queue_io(bdev_io, CRYPTO_IO_NEW);
		} else {
			SPDK_ERRLOG("Failed to submit bdev_io!\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

/* We'll just call the base bdev and let it answer except for WZ command which
 * we always say we don't support so that the bdev layer will actually send us
 * real writes that we can encrypt.
 */
static bool
vbdev_crypto_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct vbdev_crypto *crypto_bdev = (struct vbdev_crypto *)ctx;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_FLUSH:
		return spdk_bdev_io_type_supported(crypto_bdev->base_bdev, io_type);
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	/* Force the bdev layer to issue actual writes of zeroes so we can
	 * encrypt them as regular writes.
	 */
	default:
		return false;
	}
}

/* Callback for unregistering the IO device. */
static void
_device_unregister_cb(void *io_device)
{
	struct vbdev_crypto *crypto_bdev = io_device;

	/* Done with this crypto_bdev. */
	crypto_bdev->opts = NULL;

	spdk_bdev_destruct_done(&crypto_bdev->crypto_bdev, 0);
	free(crypto_bdev->crypto_bdev.name);
	free(crypto_bdev);
}

/* Wrapper for the bdev close operation. */
static void
_vbdev_crypto_destruct(void *ctx)
{
	struct spdk_bdev_desc *desc = ctx;

	spdk_bdev_close(desc);
}

/* Called after we've unregistered following a hot remove callback.
 * Our finish entry point will be called next.
 */
static int
vbdev_crypto_destruct(void *ctx)
{
	struct vbdev_crypto *crypto_bdev = (struct vbdev_crypto *)ctx;

	/* Remove this device from the internal list */
	TAILQ_REMOVE(&g_vbdev_crypto, crypto_bdev, link);

	/* Unclaim the underlying bdev. */
	spdk_bdev_module_release_bdev(crypto_bdev->base_bdev);

	/* Close the underlying bdev on its same opened thread. */
	if (crypto_bdev->thread && crypto_bdev->thread != spdk_get_thread()) {
		spdk_thread_send_msg(crypto_bdev->thread, _vbdev_crypto_destruct, crypto_bdev->base_desc);
	} else {
		spdk_bdev_close(crypto_bdev->base_desc);
	}

	/* Unregister the io_device. */
	spdk_io_device_unregister(crypto_bdev, _device_unregister_cb);

	return 1;
}

/* We supplied this as an entry point for upper layers who want to communicate to this
 * bdev.  This is how they get a channel. We are passed the same context we provided when
 * we created our crypto vbdev in examine() which, for this bdev, is the address of one of
 * our context nodes. From here we'll ask the SPDK channel code to fill out our channel
 * struct and we'll keep it in our crypto node.
 */
static struct spdk_io_channel *
vbdev_crypto_get_io_channel(void *ctx)
{
	struct vbdev_crypto *crypto_bdev = (struct vbdev_crypto *)ctx;

	/* The IO channel code will allocate a channel for us which consists of
	 * the SPDK channel structure plus the size of our crypto_io_channel struct
	 * that we passed in when we registered our IO device. It will then call
	 * our channel create callback to populate any elements that we need to
	 * update.
	 */
	return spdk_get_io_channel(crypto_bdev);
}

/* This is the output for bdev_get_bdevs() for this vbdev */
static int
vbdev_crypto_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct vbdev_crypto *crypto_bdev = (struct vbdev_crypto *)ctx;

	spdk_json_write_name(w, "crypto");
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(crypto_bdev->base_bdev));
	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&crypto_bdev->crypto_bdev));
	spdk_json_write_named_string(w, "key_name", crypto_bdev->opts->key->param.key_name);
	spdk_json_write_object_end(w);

	return 0;
}

static int
vbdev_crypto_config_json(struct spdk_json_write_ctx *w)
{
	struct vbdev_crypto *crypto_bdev;

	TAILQ_FOREACH(crypto_bdev, &g_vbdev_crypto, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "bdev_crypto_create");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(crypto_bdev->base_bdev));
		spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&crypto_bdev->crypto_bdev));
		spdk_json_write_named_string(w, "key_name", crypto_bdev->opts->key->param.key_name);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
	return 0;
}

/* We provide this callback for the SPDK channel code to create a channel using
 * the channel struct we provided in our module get_io_channel() entry point. Here
 * we get and save off an underlying base channel of the device below us so that
 * we can communicate with the base bdev on a per channel basis. We also register the
 * poller used to complete crypto operations from the device.
 */
static int
crypto_bdev_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct crypto_io_channel *crypto_ch = ctx_buf;
	struct vbdev_crypto *crypto_bdev = io_device;

	crypto_ch->base_ch = spdk_bdev_get_io_channel(crypto_bdev->base_desc);
	crypto_ch->accel_channel = spdk_accel_get_io_channel();
	crypto_ch->crypto_key = crypto_bdev->opts->key;

	/* We use this queue to track outstanding IO in our layer. */
	TAILQ_INIT(&crypto_ch->in_accel_fw);

	return 0;
}

/* We provide this callback for the SPDK channel code to destroy a channel
 * created with our create callback. We just need to undo anything we did
 * when we created.
 */
static void
crypto_bdev_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct crypto_io_channel *crypto_ch = ctx_buf;

	spdk_put_io_channel(crypto_ch->base_ch);
	spdk_put_io_channel(crypto_ch->accel_channel);
}

/* Create the association from the bdev and vbdev name and insert
 * on the global list. */
static int
vbdev_crypto_insert_name(struct vbdev_crypto_opts *opts, struct bdev_names **out)
{
	struct bdev_names *name;

	assert(opts);
	assert(out);

	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(opts->vbdev_name, name->opts->vbdev_name) == 0) {
			SPDK_ERRLOG("Crypto bdev %s already exists\n", opts->vbdev_name);
			return -EEXIST;
		}
	}

	name = calloc(1, sizeof(struct bdev_names));
	if (!name) {
		SPDK_ERRLOG("Failed to allocate memory for bdev_names.\n");
		return -ENOMEM;
	}

	name->opts = opts;
	TAILQ_INSERT_TAIL(&g_bdev_names, name, link);
	*out = name;

	return 0;
}

void
free_crypto_opts(struct vbdev_crypto_opts *opts)
{
	free(opts->bdev_name);
	free(opts->vbdev_name);
	free(opts);
}

static void
vbdev_crypto_delete_name(struct bdev_names *name)
{
	TAILQ_REMOVE(&g_bdev_names, name, link);
	if (name->opts) {
		if (name->opts->key_owner && name->opts->key) {
			spdk_accel_crypto_key_destroy(name->opts->key);
		}
		free_crypto_opts(name->opts);
		name->opts = NULL;
	}
	free(name);
}

/* RPC entry point for crypto creation. */
int
create_crypto_disk(struct vbdev_crypto_opts *opts)
{
	struct bdev_names *name = NULL;
	int rc;

	rc = vbdev_crypto_insert_name(opts, &name);
	if (rc) {
		return rc;
	}

	rc = vbdev_crypto_claim(opts->bdev_name);
	if (rc == -ENODEV) {
		SPDK_NOTICELOG("vbdev creation deferred pending base bdev arrival\n");
		rc = 0;
	}

	if (rc) {
		assert(name != NULL);
		/* In case of error we let the caller function to deallocate @opts
		 * since it is its responsibility. Setting name->opts = NULL let's
		 * vbdev_crypto_delete_name() know it does not have to do anything
		 * about @opts.
		 */
		name->opts = NULL;
		vbdev_crypto_delete_name(name);
	}
	return rc;
}

/* Called at driver init time, parses config file to prepare for examine calls,
 * also fully initializes the crypto drivers.
 */
static int
vbdev_crypto_init(void)
{
	return 0;
}

/* Called when the entire module is being torn down. */
static void
vbdev_crypto_finish(void)
{
	struct bdev_names *name;

	while ((name = TAILQ_FIRST(&g_bdev_names))) {
		vbdev_crypto_delete_name(name);
	}
}

/* During init we'll be asked how much memory we'd like passed to us
 * in bev_io structures as context. Here's where we specify how
 * much context we want per IO.
 */
static int
vbdev_crypto_get_ctx_size(void)
{
	return sizeof(struct crypto_bdev_io);
}

static void
vbdev_crypto_base_bdev_hotremove_cb(struct spdk_bdev *bdev_find)
{
	struct vbdev_crypto *crypto_bdev, *tmp;

	TAILQ_FOREACH_SAFE(crypto_bdev, &g_vbdev_crypto, link, tmp) {
		if (bdev_find == crypto_bdev->base_bdev) {
			spdk_bdev_unregister(&crypto_bdev->crypto_bdev, NULL, NULL);
		}
	}
}

/* Called when the underlying base bdev triggers asynchronous event such as bdev removal. */
static void
vbdev_crypto_base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
				void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		vbdev_crypto_base_bdev_hotremove_cb(bdev);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

/* When we register our bdev this is how we specify our entry points. */
static const struct spdk_bdev_fn_table vbdev_crypto_fn_table = {
	.destruct		= vbdev_crypto_destruct,
	.submit_request		= vbdev_crypto_submit_request,
	.io_type_supported	= vbdev_crypto_io_type_supported,
	.get_io_channel		= vbdev_crypto_get_io_channel,
	.dump_info_json		= vbdev_crypto_dump_info_json,
};

static struct spdk_bdev_module crypto_if = {
	.name = "crypto",
	.module_init = vbdev_crypto_init,
	.get_ctx_size = vbdev_crypto_get_ctx_size,
	.examine_config = vbdev_crypto_examine,
	.module_fini = vbdev_crypto_finish,
	.config_json = vbdev_crypto_config_json
};

SPDK_BDEV_MODULE_REGISTER(crypto, &crypto_if)

static int
vbdev_crypto_claim(const char *bdev_name)
{
	struct bdev_names *name;
	struct vbdev_crypto *vbdev;
	struct spdk_bdev *bdev;
	int rc = 0;

	/* Check our list of names from config versus this bdev and if
	 * there's a match, create the crypto_bdev & bdev accordingly.
	 */
	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(name->opts->bdev_name, bdev_name) != 0) {
			continue;
		}
		SPDK_DEBUGLOG(vbdev_crypto, "Match on %s\n", bdev_name);

		vbdev = calloc(1, sizeof(struct vbdev_crypto));
		if (!vbdev) {
			SPDK_ERRLOG("Failed to allocate memory for crypto_bdev.\n");
			return -ENOMEM;
		}
		vbdev->crypto_bdev.product_name = "crypto";

		vbdev->crypto_bdev.name = strdup(name->opts->vbdev_name);
		if (!vbdev->crypto_bdev.name) {
			SPDK_ERRLOG("Failed to allocate memory for crypto_bdev name.\n");
			rc = -ENOMEM;
			goto error_bdev_name;
		}

		rc = spdk_bdev_open_ext(bdev_name, true, vbdev_crypto_base_bdev_event_cb,
					NULL, &vbdev->base_desc);
		if (rc) {
			if (rc != -ENODEV) {
				SPDK_ERRLOG("Failed to open bdev %s: error %d\n", bdev_name, rc);
			}
			goto error_open;
		}

		bdev = spdk_bdev_desc_get_bdev(vbdev->base_desc);
		vbdev->base_bdev = bdev;

		vbdev->crypto_bdev.write_cache = bdev->write_cache;
		if (bdev->optimal_io_boundary > 0) {
			vbdev->crypto_bdev.optimal_io_boundary =
				spdk_min((CRYPTO_MAX_IO / bdev->blocklen), bdev->optimal_io_boundary);
		} else {
			vbdev->crypto_bdev.optimal_io_boundary = (CRYPTO_MAX_IO / bdev->blocklen);
		}
		vbdev->crypto_bdev.split_on_optimal_io_boundary = true;
		if (bdev->required_alignment > 0) {
			vbdev->crypto_bdev.required_alignment = bdev->required_alignment;
		} else {
			/* Some accel modules may not support SGL input or output, if this module works with physical
			 * addresses, unaligned buffer may cross huge page boundary which leads to scattered payload.
			 * To avoid such cases, set required_alignment to the block size */
			vbdev->crypto_bdev.required_alignment = spdk_u32log2(bdev->blocklen);
		}
		vbdev->crypto_bdev.blocklen = bdev->blocklen;
		vbdev->crypto_bdev.blockcnt = bdev->blockcnt;

		/* This is the context that is passed to us when the bdev
		 * layer calls in so we'll save our crypto_bdev node here.
		 */
		vbdev->crypto_bdev.ctxt = vbdev;
		vbdev->crypto_bdev.fn_table = &vbdev_crypto_fn_table;
		vbdev->crypto_bdev.module = &crypto_if;

		/* Assign crypto opts from the name. The pointer is valid up to the point
		 * the module is unloaded and all names removed from the list. */
		vbdev->opts = name->opts;

		TAILQ_INSERT_TAIL(&g_vbdev_crypto, vbdev, link);

		spdk_io_device_register(vbdev, crypto_bdev_ch_create_cb, crypto_bdev_ch_destroy_cb,
					sizeof(struct crypto_io_channel), vbdev->crypto_bdev.name);

		/* Save the thread where the base device is opened */
		vbdev->thread = spdk_get_thread();

		rc = spdk_bdev_module_claim_bdev(bdev, vbdev->base_desc, vbdev->crypto_bdev.module);
		if (rc) {
			SPDK_ERRLOG("Failed to claim bdev %s\n", spdk_bdev_get_name(bdev));
			goto error_claim;
		}

		rc = spdk_bdev_register(&vbdev->crypto_bdev);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to register vbdev: error %d\n", rc);
			rc = -EINVAL;
			goto error_bdev_register;
		}
		SPDK_DEBUGLOG(vbdev_crypto, "Registered io_device and virtual bdev for: %s\n",
			      vbdev->opts->vbdev_name);
		break;
	}

	return rc;

	/* Error cleanup paths. */
error_bdev_register:
	spdk_bdev_module_release_bdev(vbdev->base_bdev);
error_claim:
	TAILQ_REMOVE(&g_vbdev_crypto, vbdev, link);
	spdk_io_device_unregister(vbdev, NULL);
	spdk_bdev_close(vbdev->base_desc);
error_open:
	free(vbdev->crypto_bdev.name);
error_bdev_name:
	free(vbdev);

	return rc;
}

struct crypto_delete_disk_ctx {
	spdk_delete_crypto_complete cb_fn;
	void *cb_arg;
	char *bdev_name;
};

static void
delete_crypto_disk_bdev_name(void *ctx, int rc)
{
	struct bdev_names *name;
	struct crypto_delete_disk_ctx *disk_ctx = ctx;

	/* Remove the association (vbdev, bdev) from g_bdev_names. This is required so that the
	 * vbdev does not get re-created if the same bdev is constructed at some other time,
	 * unless the underlying bdev was hot-removed. */
	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(name->opts->vbdev_name, disk_ctx->bdev_name) == 0) {
			vbdev_crypto_delete_name(name);
			break;
		}
	}

	disk_ctx->cb_fn(disk_ctx->cb_arg, rc);

	free(disk_ctx->bdev_name);
	free(disk_ctx);
}

/* RPC entry for deleting a crypto vbdev. */
void
delete_crypto_disk(const char *bdev_name, spdk_delete_crypto_complete cb_fn,
		   void *cb_arg)
{
	int rc;
	struct crypto_delete_disk_ctx *ctx;

	ctx = calloc(1, sizeof(struct crypto_delete_disk_ctx));
	if (!ctx) {
		SPDK_ERRLOG("Failed to allocate delete crypto disk ctx\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->bdev_name = strdup(bdev_name);
	if (!ctx->bdev_name) {
		SPDK_ERRLOG("Failed to copy bdev_name\n");
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	ctx->cb_arg = cb_arg;
	ctx->cb_fn = cb_fn;
	/* Some cleanup happens in the destruct callback. */
	rc = spdk_bdev_unregister_by_name(bdev_name, &crypto_if, delete_crypto_disk_bdev_name, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("Encountered an error during bdev unregistration\n");
		cb_fn(cb_arg, rc);
		free(ctx->bdev_name);
		free(ctx);
	}
}

/* Because we specified this function in our crypto bdev function table when we
 * registered our crypto bdev, we'll get this call anytime a new bdev shows up.
 * Here we need to decide if we care about it and if so what to do. We
 * parsed the config file at init so we check the new bdev against the list
 * we built up at that time and if the user configured us to attach to this
 * bdev, here's where we do it.
 */
static void
vbdev_crypto_examine(struct spdk_bdev *bdev)
{
	vbdev_crypto_claim(spdk_bdev_get_name(bdev));
	spdk_bdev_module_examine_done(&crypto_if);
}

SPDK_LOG_REGISTER_COMPONENT(vbdev_crypto)
