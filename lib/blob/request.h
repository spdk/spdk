/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
			void					*esnap_ctx;
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

	/*
	 * The blobstore's channel, obtained by blobstore consumers via
	 * spdk_bs_alloc_io_channel(). Used for IO to the blobstore.
	 */
	struct spdk_bs_channel		*channel;
	/*
	 * The channel used by the blobstore to perform IO on back_bs_dev. Unless the blob
	 * is an esnap clone, back_channel == spdk_io_channel_get_ctx(set->channel).
	 */
	struct spdk_io_channel		*back_channel;

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
	/* Pointer to ext_io_opts passed by the user */
	struct spdk_blob_ext_io_opts *ext_io_opts;
	TAILQ_ENTRY(spdk_bs_request_set) link;
};

void bs_call_cpl(struct spdk_bs_cpl *cpl, int bserrno);

spdk_bs_sequence_t *bs_sequence_start_bs(struct spdk_io_channel *channel,
		struct spdk_bs_cpl *cpl);

spdk_bs_sequence_t *bs_sequence_start_blob(struct spdk_io_channel *channel,
		struct spdk_bs_cpl *cpl, struct spdk_blob *blob);

spdk_bs_sequence_t *bs_sequence_start_esnap(struct spdk_io_channel *channel,
		struct spdk_bs_cpl *cpl, struct spdk_blob *blob);

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

void bs_sequence_copy_dev(spdk_bs_sequence_t *seq,
			  uint64_t dst_lba, uint64_t src_lba, uint64_t lba_count,
			  spdk_bs_sequence_cpl cb_fn, void *cb_arg);

void bs_sequence_finish(spdk_bs_sequence_t *seq, int bserrno);

void bs_user_op_sequence_finish(void *cb_arg, int bserrno);

spdk_bs_batch_t *bs_batch_open(struct spdk_io_channel *channel,
			       struct spdk_bs_cpl *cpl, struct spdk_blob *blob);

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

void bs_user_op_abort(spdk_bs_user_op_t *op, int bserrno);

#endif
