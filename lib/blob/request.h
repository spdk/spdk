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

#ifndef SPDK_BS_REQUEST_H
#define SPDK_BS_REQUEST_H

#include "spdk/stdinc.h"

#include "spdk/blob.h"

enum spdk_bs_cpl_type {
	SPDK_BS_CPL_TYPE_NONE,
	SPDK_BS_CPL_TYPE_BS_BASIC,
	SPDK_BS_CPL_TYPE_BS_HANDLE,
	SPDK_BS_CPL_TYPE_BLOB_BASIC,
	SPDK_BS_CPL_TYPE_BLOBID,
	SPDK_BS_CPL_TYPE_BLOB_HANDLE,
	SPDK_BS_CPL_TYPE_NESTED_SEQUENCE,
};

enum spdk_blob_op_type;

struct spdk_bs_request_set;

/* Use a sequence to submit a set of requests serially */
typedef struct spdk_bs_request_set spdk_bs_sequence_t;

/* Use a batch to submit a set of requests in parallel */
typedef struct spdk_bs_request_set spdk_bs_batch_t;

/* Use a user_op to queue a user operation for later execution */
typedef struct spdk_bs_request_set spdk_bs_user_op_t;

typedef void (*spdk_bs_nested_seq_complete)(void *cb_arg, spdk_bs_sequence_t *parent, int bserrno);

struct spdk_bs_cpl {
	enum spdk_bs_cpl_type type;
	union {
		struct {
			spdk_bs_op_complete     cb_fn;
			void                    *cb_arg;
		} bs_basic;

		struct {
			spdk_bs_op_with_handle_complete cb_fn;
			void                            *cb_arg;
			struct spdk_blob_store          *bs;
		} bs_handle;

		struct {
			spdk_blob_op_complete   cb_fn;
			void                    *cb_arg;
		} blob_basic;

		struct {
			spdk_blob_op_with_id_complete   cb_fn;
			void                            *cb_arg;
			spdk_blob_id                     blobid;
		} blobid;

		struct {
			spdk_blob_op_with_handle_complete       cb_fn;
			void                                    *cb_arg;
			struct spdk_blob                        *blob;
		} blob_handle;

		struct {
			spdk_bs_nested_seq_complete	cb_fn;
			void				*cb_arg;
			spdk_bs_sequence_t		*parent;
		} nested_seq;
	} u;
};

typedef void (*spdk_bs_sequence_cpl)(spdk_bs_sequence_t *sequence,
				     void *cb_arg, int bserrno);

/* A generic request set. Can be a sequence, batch or a user_op. */
struct spdk_bs_request_set {
	struct spdk_bs_cpl      cpl;

	int                     bserrno;

	struct spdk_bs_channel		*channel;

	struct spdk_bs_dev_cb_args	cb_args;

	union {
		struct {
			spdk_bs_sequence_cpl    cb_fn;
			void                    *cb_arg;
		} sequence;

		struct {
			uint32_t		outstanding_ops;
			uint32_t		batch_closed;
			spdk_bs_sequence_cpl	cb_fn;
			void			*cb_arg;
		} batch;

		struct spdk_bs_user_op_args {
			int			type;
			int			iovcnt;
			struct spdk_blob	*blob;
			uint64_t		offset;
			uint64_t		length;
			spdk_blob_op_complete	cb_fn;
			void			*cb_arg;
			void			*payload; /* cast to iov for readv/writev */
		} user_op;
	} u;

	TAILQ_ENTRY(spdk_bs_request_set) link;
};

void bs_call_cpl(struct spdk_bs_cpl *cpl, int bserrno);

spdk_bs_sequence_t *bs_sequence_start(struct spdk_io_channel *channel,
				      struct spdk_bs_cpl *cpl);

void bs_sequence_read_bs_dev(spdk_bs_sequence_t *seq, struct spdk_bs_dev *bs_dev,
			     void *payload, uint64_t lba, uint32_t lba_count,
			     spdk_bs_sequence_cpl cb_fn, void *cb_arg);

void bs_sequence_read_dev(spdk_bs_sequence_t *seq, void *payload,
			  uint64_t lba, uint32_t lba_count,
			  spdk_bs_sequence_cpl cb_fn, void *cb_arg);

void bs_sequence_write_dev(spdk_bs_sequence_t *seq, void *payload,
			   uint64_t lba, uint32_t lba_count,
			   spdk_bs_sequence_cpl cb_fn, void *cb_arg);

void bs_sequence_readv_bs_dev(spdk_bs_batch_t *batch, struct spdk_bs_dev *bs_dev,
			      struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count,
			      spdk_bs_sequence_cpl cb_fn, void *cb_arg);

void bs_sequence_readv_dev(spdk_bs_batch_t *batch, struct iovec *iov, int iovcnt,
			   uint64_t lba, uint32_t lba_count,
			   spdk_bs_sequence_cpl cb_fn, void *cb_arg);

void bs_sequence_writev_dev(spdk_bs_batch_t *batch, struct iovec *iov, int iovcnt,
			    uint64_t lba, uint32_t lba_count,
			    spdk_bs_sequence_cpl cb_fn, void *cb_arg);

void bs_sequence_write_zeroes_dev(spdk_bs_sequence_t *seq,
				  uint64_t lba, uint64_t lba_count,
				  spdk_bs_sequence_cpl cb_fn, void *cb_arg);

void bs_sequence_finish(spdk_bs_sequence_t *seq, int bserrno);

void bs_user_op_sequence_finish(void *cb_arg, int bserrno);

spdk_bs_batch_t *bs_batch_open(struct spdk_io_channel *channel,
			       struct spdk_bs_cpl *cpl);

void bs_batch_read_bs_dev(spdk_bs_batch_t *batch, struct spdk_bs_dev *bs_dev,
			  void *payload, uint64_t lba, uint32_t lba_count);

void bs_batch_read_dev(spdk_bs_batch_t *batch, void *payload,
		       uint64_t lba, uint32_t lba_count);

void bs_batch_write_dev(spdk_bs_batch_t *batch, void *payload,
			uint64_t lba, uint32_t lba_count);

void bs_batch_unmap_dev(spdk_bs_batch_t *batch,
			uint64_t lba, uint64_t lba_count);

void bs_batch_write_zeroes_dev(spdk_bs_batch_t *batch,
			       uint64_t lba, uint64_t lba_count);

void bs_batch_close(spdk_bs_batch_t *batch);

spdk_bs_batch_t *bs_sequence_to_batch(spdk_bs_sequence_t *seq,
				      spdk_bs_sequence_cpl cb_fn,
				      void *cb_arg);

spdk_bs_user_op_t *bs_user_op_alloc(struct spdk_io_channel *channel, struct spdk_bs_cpl *cpl,
				    enum spdk_blob_op_type op_type, struct spdk_blob *blob,
				    void *payload, int iovcnt, uint64_t offset, uint64_t length);

void bs_user_op_execute(spdk_bs_user_op_t *op);

void bs_user_op_abort(spdk_bs_user_op_t *op);

void bs_sequence_to_batch_completion(void *cb_arg, int bserrno);

#endif
