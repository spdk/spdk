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

#include "blobstore.h"
#include "request.h"

#include "spdk/thread.h"
#include "spdk/queue.h"

#include "spdk/log.h"

void
bs_call_cpl(struct spdk_bs_cpl *cpl, int bserrno)
{
	switch (cpl->type) {
	case SPDK_BS_CPL_TYPE_BS_BASIC:
		cpl->u.bs_basic.cb_fn(cpl->u.bs_basic.cb_arg,
				      bserrno);
		break;
	case SPDK_BS_CPL_TYPE_BS_HANDLE:
		cpl->u.bs_handle.cb_fn(cpl->u.bs_handle.cb_arg,
				       bserrno == 0 ? cpl->u.bs_handle.bs : NULL,
				       bserrno);
		break;
	case SPDK_BS_CPL_TYPE_BLOB_BASIC:
		cpl->u.blob_basic.cb_fn(cpl->u.blob_basic.cb_arg,
					bserrno);
		break;
	case SPDK_BS_CPL_TYPE_BLOBID:
		cpl->u.blobid.cb_fn(cpl->u.blobid.cb_arg,
				    bserrno == 0 ? cpl->u.blobid.blobid : SPDK_BLOBID_INVALID,
				    bserrno);
		break;
	case SPDK_BS_CPL_TYPE_BLOB_HANDLE:
		cpl->u.blob_handle.cb_fn(cpl->u.blob_handle.cb_arg,
					 bserrno == 0 ? cpl->u.blob_handle.blob : NULL,
					 bserrno);
		break;
	case SPDK_BS_CPL_TYPE_NESTED_SEQUENCE:
		cpl->u.nested_seq.cb_fn(cpl->u.nested_seq.cb_arg,
					cpl->u.nested_seq.parent,
					bserrno);
		break;
	case SPDK_BS_CPL_TYPE_NONE:
		/* this completion's callback is handled elsewhere */
		break;
	}
}

static void
bs_request_set_complete(struct spdk_bs_request_set *set)
{
	struct spdk_bs_cpl cpl = set->cpl;
	int bserrno = set->bserrno;

	TAILQ_INSERT_TAIL(&set->channel->reqs, set, link);

	bs_call_cpl(&cpl, bserrno);
}

static void
bs_sequence_completion(struct spdk_io_channel *channel, void *cb_arg, int bserrno)
{
	struct spdk_bs_request_set *set = cb_arg;

	set->bserrno = bserrno;
	set->u.sequence.cb_fn((spdk_bs_sequence_t *)set, set->u.sequence.cb_arg, bserrno);
}

spdk_bs_sequence_t *
bs_sequence_start(struct spdk_io_channel *_channel,
		  struct spdk_bs_cpl *cpl)
{
	struct spdk_bs_channel		*channel;
	struct spdk_bs_request_set	*set;

	channel = spdk_io_channel_get_ctx(_channel);
	assert(channel != NULL);
	set = TAILQ_FIRST(&channel->reqs);
	if (!set) {
		return NULL;
	}
	TAILQ_REMOVE(&channel->reqs, set, link);

	set->cpl = *cpl;
	set->bserrno = 0;
	set->channel = channel;

	set->cb_args.cb_fn = bs_sequence_completion;
	set->cb_args.cb_arg = set;
	set->cb_args.channel = channel->dev_channel;

	return (spdk_bs_sequence_t *)set;
}

void
bs_sequence_read_bs_dev(spdk_bs_sequence_t *seq, struct spdk_bs_dev *bs_dev,
			void *payload, uint64_t lba, uint32_t lba_count,
			spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_bs_request_set      *set = (struct spdk_bs_request_set *)seq;
	struct spdk_bs_channel       *channel = set->channel;

	SPDK_DEBUGLOG(blob_rw, "Reading %" PRIu32 " blocks from LBA %" PRIu64 "\n", lba_count,
		      lba);

	set->u.sequence.cb_fn = cb_fn;
	set->u.sequence.cb_arg = cb_arg;

	bs_dev->read(bs_dev, spdk_io_channel_from_ctx(channel), payload, lba, lba_count, &set->cb_args);
}

void
bs_sequence_read_dev(spdk_bs_sequence_t *seq, void *payload,
		     uint64_t lba, uint32_t lba_count,
		     spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_bs_request_set      *set = (struct spdk_bs_request_set *)seq;
	struct spdk_bs_channel       *channel = set->channel;

	SPDK_DEBUGLOG(blob_rw, "Reading %" PRIu32 " blocks from LBA %" PRIu64 "\n", lba_count,
		      lba);

	set->u.sequence.cb_fn = cb_fn;
	set->u.sequence.cb_arg = cb_arg;

	channel->dev->read(channel->dev, channel->dev_channel, payload, lba, lba_count, &set->cb_args);
}

void
bs_sequence_write_dev(spdk_bs_sequence_t *seq, void *payload,
		      uint64_t lba, uint32_t lba_count,
		      spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_bs_request_set      *set = (struct spdk_bs_request_set *)seq;
	struct spdk_bs_channel       *channel = set->channel;

	SPDK_DEBUGLOG(blob_rw, "Writing %" PRIu32 " blocks from LBA %" PRIu64 "\n", lba_count,
		      lba);

	set->u.sequence.cb_fn = cb_fn;
	set->u.sequence.cb_arg = cb_arg;

	channel->dev->write(channel->dev, channel->dev_channel, payload, lba, lba_count,
			    &set->cb_args);
}

void
bs_sequence_readv_bs_dev(spdk_bs_sequence_t *seq, struct spdk_bs_dev *bs_dev,
			 struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count,
			 spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_bs_request_set      *set = (struct spdk_bs_request_set *)seq;
	struct spdk_bs_channel       *channel = set->channel;

	SPDK_DEBUGLOG(blob_rw, "Reading %" PRIu32 " blocks from LBA %" PRIu64 "\n", lba_count,
		      lba);

	set->u.sequence.cb_fn = cb_fn;
	set->u.sequence.cb_arg = cb_arg;

	bs_dev->readv(bs_dev, spdk_io_channel_from_ctx(channel), iov, iovcnt, lba, lba_count,
		      &set->cb_args);
}

void
bs_sequence_readv_dev(spdk_bs_sequence_t *seq, struct iovec *iov, int iovcnt,
		      uint64_t lba, uint32_t lba_count, spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_bs_request_set      *set = (struct spdk_bs_request_set *)seq;
	struct spdk_bs_channel       *channel = set->channel;

	SPDK_DEBUGLOG(blob_rw, "Reading %" PRIu32 " blocks from LBA %" PRIu64 "\n", lba_count,
		      lba);

	set->u.sequence.cb_fn = cb_fn;
	set->u.sequence.cb_arg = cb_arg;
	channel->dev->readv(channel->dev, channel->dev_channel, iov, iovcnt, lba, lba_count,
			    &set->cb_args);
}

void
bs_sequence_writev_dev(spdk_bs_sequence_t *seq, struct iovec *iov, int iovcnt,
		       uint64_t lba, uint32_t lba_count,
		       spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_bs_request_set      *set = (struct spdk_bs_request_set *)seq;
	struct spdk_bs_channel       *channel = set->channel;

	SPDK_DEBUGLOG(blob_rw, "Writing %" PRIu32 " blocks from LBA %" PRIu64 "\n", lba_count,
		      lba);

	set->u.sequence.cb_fn = cb_fn;
	set->u.sequence.cb_arg = cb_arg;

	channel->dev->writev(channel->dev, channel->dev_channel, iov, iovcnt, lba, lba_count,
			     &set->cb_args);
}

void
bs_sequence_write_zeroes_dev(spdk_bs_sequence_t *seq,
			     uint64_t lba, uint64_t lba_count,
			     spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_bs_request_set      *set = (struct spdk_bs_request_set *)seq;
	struct spdk_bs_channel       *channel = set->channel;

	SPDK_DEBUGLOG(blob_rw, "writing zeroes to %" PRIu64 " blocks at LBA %" PRIu64 "\n",
		      lba_count, lba);

	set->u.sequence.cb_fn = cb_fn;
	set->u.sequence.cb_arg = cb_arg;

	channel->dev->write_zeroes(channel->dev, channel->dev_channel, lba, lba_count,
				   &set->cb_args);
}

void
bs_sequence_finish(spdk_bs_sequence_t *seq, int bserrno)
{
	if (bserrno != 0) {
		seq->bserrno = bserrno;
	}
	bs_request_set_complete((struct spdk_bs_request_set *)seq);
}

void
bs_user_op_sequence_finish(void *cb_arg, int bserrno)
{
	spdk_bs_sequence_t *seq = cb_arg;

	bs_sequence_finish(seq, bserrno);
}

static void
bs_batch_completion(struct spdk_io_channel *_channel,
		    void *cb_arg, int bserrno)
{
	struct spdk_bs_request_set	*set = cb_arg;

	set->u.batch.outstanding_ops--;
	if (bserrno != 0) {
		set->bserrno = bserrno;
	}

	if (set->u.batch.outstanding_ops == 0 && set->u.batch.batch_closed) {
		if (set->u.batch.cb_fn) {
			set->cb_args.cb_fn = bs_sequence_completion;
			set->u.batch.cb_fn((spdk_bs_sequence_t *)set, set->u.batch.cb_arg, bserrno);
		} else {
			bs_request_set_complete(set);
		}
	}
}

spdk_bs_batch_t *
bs_batch_open(struct spdk_io_channel *_channel,
	      struct spdk_bs_cpl *cpl)
{
	struct spdk_bs_channel		*channel;
	struct spdk_bs_request_set	*set;

	channel = spdk_io_channel_get_ctx(_channel);
	assert(channel != NULL);
	set = TAILQ_FIRST(&channel->reqs);
	if (!set) {
		return NULL;
	}
	TAILQ_REMOVE(&channel->reqs, set, link);

	set->cpl = *cpl;
	set->bserrno = 0;
	set->channel = channel;

	set->u.batch.cb_fn = NULL;
	set->u.batch.cb_arg = NULL;
	set->u.batch.outstanding_ops = 0;
	set->u.batch.batch_closed = 0;

	set->cb_args.cb_fn = bs_batch_completion;
	set->cb_args.cb_arg = set;
	set->cb_args.channel = channel->dev_channel;

	return (spdk_bs_batch_t *)set;
}

void
bs_batch_read_bs_dev(spdk_bs_batch_t *batch, struct spdk_bs_dev *bs_dev,
		     void *payload, uint64_t lba, uint32_t lba_count)
{
	struct spdk_bs_request_set	*set = (struct spdk_bs_request_set *)batch;
	struct spdk_bs_channel		*channel = set->channel;

	SPDK_DEBUGLOG(blob_rw, "Reading %" PRIu32 " blocks from LBA %" PRIu64 "\n", lba_count,
		      lba);

	set->u.batch.outstanding_ops++;
	bs_dev->read(bs_dev, spdk_io_channel_from_ctx(channel), payload, lba, lba_count, &set->cb_args);
}

void
bs_batch_read_dev(spdk_bs_batch_t *batch, void *payload,
		  uint64_t lba, uint32_t lba_count)
{
	struct spdk_bs_request_set	*set = (struct spdk_bs_request_set *)batch;
	struct spdk_bs_channel		*channel = set->channel;

	SPDK_DEBUGLOG(blob_rw, "Reading %" PRIu32 " blocks from LBA %" PRIu64 "\n", lba_count,
		      lba);

	set->u.batch.outstanding_ops++;
	channel->dev->read(channel->dev, channel->dev_channel, payload, lba, lba_count, &set->cb_args);
}

void
bs_batch_write_dev(spdk_bs_batch_t *batch, void *payload,
		   uint64_t lba, uint32_t lba_count)
{
	struct spdk_bs_request_set	*set = (struct spdk_bs_request_set *)batch;
	struct spdk_bs_channel		*channel = set->channel;

	SPDK_DEBUGLOG(blob_rw, "Writing %" PRIu32 " blocks to LBA %" PRIu64 "\n", lba_count, lba);

	set->u.batch.outstanding_ops++;
	channel->dev->write(channel->dev, channel->dev_channel, payload, lba, lba_count,
			    &set->cb_args);
}

void
bs_batch_unmap_dev(spdk_bs_batch_t *batch,
		   uint64_t lba, uint64_t lba_count)
{
	struct spdk_bs_request_set	*set = (struct spdk_bs_request_set *)batch;
	struct spdk_bs_channel		*channel = set->channel;

	SPDK_DEBUGLOG(blob_rw, "Unmapping %" PRIu64 " blocks at LBA %" PRIu64 "\n", lba_count,
		      lba);

	set->u.batch.outstanding_ops++;
	channel->dev->unmap(channel->dev, channel->dev_channel, lba, lba_count,
			    &set->cb_args);
}

void
bs_batch_write_zeroes_dev(spdk_bs_batch_t *batch,
			  uint64_t lba, uint64_t lba_count)
{
	struct spdk_bs_request_set	*set = (struct spdk_bs_request_set *)batch;
	struct spdk_bs_channel		*channel = set->channel;

	SPDK_DEBUGLOG(blob_rw, "Zeroing %" PRIu64 " blocks at LBA %" PRIu64 "\n", lba_count, lba);

	set->u.batch.outstanding_ops++;
	channel->dev->write_zeroes(channel->dev, channel->dev_channel, lba, lba_count,
				   &set->cb_args);
}

void
bs_batch_close(spdk_bs_batch_t *batch)
{
	struct spdk_bs_request_set	*set = (struct spdk_bs_request_set *)batch;

	set->u.batch.batch_closed = 1;

	if (set->u.batch.outstanding_ops == 0) {
		if (set->u.batch.cb_fn) {
			set->cb_args.cb_fn = bs_sequence_completion;
			set->u.batch.cb_fn((spdk_bs_sequence_t *)set, set->u.batch.cb_arg, set->bserrno);
		} else {
			bs_request_set_complete(set);
		}
	}
}

spdk_bs_batch_t *
bs_sequence_to_batch(spdk_bs_sequence_t *seq, spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_bs_request_set *set = (struct spdk_bs_request_set *)seq;

	set->u.batch.cb_fn = cb_fn;
	set->u.batch.cb_arg = cb_arg;
	set->u.batch.outstanding_ops = 0;
	set->u.batch.batch_closed = 0;

	set->cb_args.cb_fn = bs_batch_completion;

	return set;
}

spdk_bs_user_op_t *
bs_user_op_alloc(struct spdk_io_channel *_channel, struct spdk_bs_cpl *cpl,
		 enum spdk_blob_op_type op_type, struct spdk_blob *blob,
		 void *payload, int iovcnt, uint64_t offset, uint64_t length)
{
	struct spdk_bs_channel		*channel;
	struct spdk_bs_request_set	*set;
	struct spdk_bs_user_op_args	*args;

	channel = spdk_io_channel_get_ctx(_channel);
	assert(channel != NULL);
	set = TAILQ_FIRST(&channel->reqs);
	if (!set) {
		return NULL;
	}
	TAILQ_REMOVE(&channel->reqs, set, link);

	set->cpl = *cpl;
	set->channel = channel;

	args = &set->u.user_op;

	args->type = op_type;
	args->iovcnt = iovcnt;
	args->blob = blob;
	args->offset = offset;
	args->length = length;
	args->payload = payload;

	return (spdk_bs_user_op_t *)set;
}

void
bs_user_op_execute(spdk_bs_user_op_t *op)
{
	struct spdk_bs_request_set	*set;
	struct spdk_bs_user_op_args	*args;
	struct spdk_io_channel		*ch;

	set = (struct spdk_bs_request_set *)op;
	args = &set->u.user_op;
	ch = spdk_io_channel_from_ctx(set->channel);

	switch (args->type) {
	case SPDK_BLOB_READ:
		spdk_blob_io_read(args->blob, ch, args->payload, args->offset, args->length,
				  set->cpl.u.blob_basic.cb_fn, set->cpl.u.blob_basic.cb_arg);
		break;
	case SPDK_BLOB_WRITE:
		spdk_blob_io_write(args->blob, ch, args->payload, args->offset, args->length,
				   set->cpl.u.blob_basic.cb_fn, set->cpl.u.blob_basic.cb_arg);
		break;
	case SPDK_BLOB_UNMAP:
		spdk_blob_io_unmap(args->blob, ch, args->offset, args->length,
				   set->cpl.u.blob_basic.cb_fn, set->cpl.u.blob_basic.cb_arg);
		break;
	case SPDK_BLOB_WRITE_ZEROES:
		spdk_blob_io_write_zeroes(args->blob, ch, args->offset, args->length,
					  set->cpl.u.blob_basic.cb_fn, set->cpl.u.blob_basic.cb_arg);
		break;
	case SPDK_BLOB_READV:
		spdk_blob_io_readv(args->blob, ch, args->payload, args->iovcnt,
				   args->offset, args->length,
				   set->cpl.u.blob_basic.cb_fn, set->cpl.u.blob_basic.cb_arg);
		break;
	case SPDK_BLOB_WRITEV:
		spdk_blob_io_writev(args->blob, ch, args->payload, args->iovcnt,
				    args->offset, args->length,
				    set->cpl.u.blob_basic.cb_fn, set->cpl.u.blob_basic.cb_arg);
		break;
	}
	TAILQ_INSERT_TAIL(&set->channel->reqs, set, link);
}

void
bs_user_op_abort(spdk_bs_user_op_t *op)
{
	struct spdk_bs_request_set	*set;

	set = (struct spdk_bs_request_set *)op;

	set->cpl.u.blob_basic.cb_fn(set->cpl.u.blob_basic.cb_arg, -EIO);
	TAILQ_INSERT_TAIL(&set->channel->reqs, set, link);
}

void
bs_sequence_to_batch_completion(void *cb_arg, int bserrno)
{
	struct spdk_bs_request_set *set = (struct spdk_bs_request_set *)cb_arg;

	set->u.batch.outstanding_ops--;

	if (set->u.batch.outstanding_ops == 0 && set->u.batch.batch_closed) {
		if (set->cb_args.cb_fn) {
			set->cb_args.cb_fn(set->cb_args.channel, set->cb_args.cb_arg, bserrno);
		}
	}
}

SPDK_LOG_REGISTER_COMPONENT(blob_rw)
