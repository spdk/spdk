/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES
 *   All rights reserved.
 */

/** \file
 * Acceleration Framework
 */

#ifndef SPDK_ACCEL_H
#define SPDK_ACCEL_H

#include "spdk/stdinc.h"
#include "spdk/dma.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Data Encryption Key identifier */
struct spdk_accel_crypto_key;

/* Flags for accel operations */
#define ACCEL_FLAG_PERSISTENT (1 << 0)

struct spdk_accel_crypto_key_create_param {
	char *cipher;	/**< Cipher to be used for crypto operations */
	char *hex_key;	/**< Hexlified key */
	char *hex_key2;	/**< Hexlified key2 */
	char *key_name;	/**< Key name */
};

enum accel_opcode {
	ACCEL_OPC_COPY			= 0,
	ACCEL_OPC_FILL			= 1,
	ACCEL_OPC_DUALCAST		= 2,
	ACCEL_OPC_COMPARE		= 3,
	ACCEL_OPC_CRC32C		= 4,
	ACCEL_OPC_COPY_CRC32C		= 5,
	ACCEL_OPC_COMPRESS		= 6,
	ACCEL_OPC_DECOMPRESS		= 7,
	ACCEL_OPC_ENCRYPT		= 8,
	ACCEL_OPC_DECRYPT		= 9,
	ACCEL_OPC_LAST			= 10,
};

/**
 * Acceleration operation callback.
 *
 * \param cb_arg Callback argument specified in the spdk_accel_submit* call.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_accel_completion_cb)(void *cb_arg, int status);

/**
 * Acceleration framework finish callback.
 *
 * \param cb_arg Callback argument.
 */
typedef void (*spdk_accel_fini_cb)(void *cb_arg);

/**
 * Initialize the acceleration framework.
 *
 * \return 0 on success.
 */
int spdk_accel_initialize(void);

/**
 * Close the acceleration framework.
 *
 * \param cb_fn Called when the close operation completes.
 * \param cb_arg Argument passed to the callback function.
 */
void spdk_accel_finish(spdk_accel_fini_cb cb_fn, void *cb_arg);

/**
 * Get an I/O channel for the acceleration framework.
 *
 * This I/O channel is used to submit requests.
 *
 * \return a pointer to the I/O channel on success, or NULL on failure.
 */
struct spdk_io_channel *spdk_accel_get_io_channel(void);

/**
 * Create a crypto key with given parameters. Accel module copies content of \b param structure
 *
 * \param param Key parameters
 * \return 0 on success, negated errno on error
 */
int spdk_accel_crypto_key_create(const struct spdk_accel_crypto_key_create_param *param);

/**
 * Destroy a crypto key
 *
 * \param key Key to destroy
 * \return 0 on success, negated errno on error
 */
int spdk_accel_crypto_key_destroy(struct spdk_accel_crypto_key *key);

/**
 * Find a crypto key structure by name
 * \param name Key name
 * \return Crypto key structure or NULL
 */
struct spdk_accel_crypto_key *spdk_accel_crypto_key_get(const char *name);

/**
 * Submit a copy request.
 *
 * \param ch I/O channel associated with this call.
 * \param dst Destination to copy to.
 * \param src Source to copy from.
 * \param nbytes Length in bytes to copy.
 * \param flags Accel framework flags for operations.
 * \param cb_fn Called when this copy operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_copy(struct spdk_io_channel *ch, void *dst, void *src, uint64_t nbytes,
			   int flags, spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a dual cast copy request.
 *
 * \param ch I/O channel associated with this call.
 * \param dst1 First destination to copy to (must be 4K aligned).
 * \param dst2 Second destination to copy to (must be 4K aligned).
 * \param src Source to copy from.
 * \param nbytes Length in bytes to copy.
 * \param flags Accel framework flags for operations.
 * \param cb_fn Called when this copy operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_dualcast(struct spdk_io_channel *ch, void *dst1, void *dst2, void *src,
			       uint64_t nbytes, int flags, spdk_accel_completion_cb cb_fn, void *cb_arg);

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
 * \param flags Accel framework flags for operations.
 * \param cb_fn Called when this fill operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_fill(struct spdk_io_channel *ch, void *dst, uint8_t fill, uint64_t nbytes,
			   int flags, spdk_accel_completion_cb cb_fn, void *cb_arg);

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
 * \param flags Accel framework flags for operations.
 * \param cb_fn Called when this CRC-32C operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_copy_crc32c(struct spdk_io_channel *ch, void *dst, void *src,
				  uint32_t *crc_dst, uint32_t seed, uint64_t nbytes, int flags,
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
 * \param flags Accel framework flags for operations.
 * \param cb_fn Called when this CRC-32C operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_copy_crc32cv(struct spdk_io_channel *ch, void *dst, struct iovec *src_iovs,
				   uint32_t iovcnt, uint32_t *crc_dst, uint32_t seed,
				   int flags, spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Build and submit a memory compress request.
 *
 * This function will build the compress descriptor and submit it.
 *
 * \param ch I/O channel associated with this call
 * \param dst Destination to write the data to.
 * \param nbytes Length in bytes.
 * \param src_iovs The io vector array which stores the src data and len.
 * \param src_iovcnt The size of the src io vectors.
 * \param output_size The size of the compressed data (may be NULL if not desired)
 * \param flags Flags, optional flags that can vary per operation.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 * the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_compress(struct spdk_io_channel *ch, void *dst,
			       uint64_t nbytes, struct iovec *src_iovs,
			       size_t src_iovcnt, uint32_t *output_size, int flags,
			       spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Build and submit a memory decompress request.
 *
 * This function will build the decompress descriptor and submit it.
 *
 * \param ch I/O channel associated with this call
 * \param dst_iovs The io vector array which stores the dst data and len.
 * \param dst_iovcnt The size of the dst io vectors.
 * \param src_iovs The io vector array which stores the src data and len.
 * \param src_iovcnt The size of the src io vectors.
 * \param output_size The size of the compressed data (may be NULL if not desired)
 * \param flags Flags, optional flags that can vary per operation.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 * the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_decompress(struct spdk_io_channel *ch, struct iovec *dst_iovs,
				 size_t dst_iovcnt, struct iovec *src_iovs,
				 size_t src_iovcnt, uint32_t *output_size, int flags,
				 spdk_accel_completion_cb cb_fn, void *cb_arg);

/** Object grouping multiple accel operations to be executed at the same point in time */
struct spdk_accel_sequence;

/**
 * Completion callback of a single operation within a sequence.  After it's executed, the sequence
 * object might be freed, so users should not touch it.
 */
typedef void (*spdk_accel_step_cb)(void *cb_arg);

/**
 * Append a copy operation to a sequence.  Copy operation in a sequence is special, as it is not
 * guaranteed that the data will be actually copied.  If it's possible, it will only change
 * source / destination buffers of some of the operations in a sequence.
 *
 * \param seq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param dst_iovs Destination I/O vector array.
 * \param dst_iovcnt Size of the `dst_iovs` array.
 * \param dst_domain Memory domain to which the destination buffers belong.
 * \param dst_domain_ctx Destination buffer domain context.
 * \param src_iovs Source I/O vector array.
 * \param src_iovcnt Size of the `src_iovs` array.
 * \param src_domain Memory domain to which the source buffers belong.
 * \param src_domain_ctx Source buffer domain context.
 * \param flags Accel operation flags.
 * \param cb_fn Callback to be executed once this operation is completed.
 * \param cb_arg Argument to be passed to `cb_fn`.
 *
 * \return 0 if operation was successfully added to the sequence, negative errno otherwise.
 */
int spdk_accel_append_copy(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
			   struct iovec *dst_iovs, uint32_t dst_iovcnt,
			   struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
			   struct iovec *src_iovs, uint32_t src_iovcnt,
			   struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			   int flags, spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append a fill operation to a sequence.
 *
 * \param seq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param buf Data buffer.
 * \param len Length of the data buffer.
 * \param domain Memory domain to which the data buffer belongs.
 * \param domain_ctx Buffer domain context.
 * \param pattern Pattern to fill the buffer with.
 * \param flags Accel operation flags.
 * \param cb_fn Callback to be executed once this operation is completed.
 * \param cb_arg Argument to be passed to `cb_fn`.
 *
 * \return 0 if operation was successfully added to the sequence, negative errno otherwise.
 */
int spdk_accel_append_fill(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
			   void *buf, uint64_t len,
			   struct spdk_memory_domain *domain, void *domain_ctx, uint8_t pattern,
			   int flags, spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append a decompress operation to a sequence.
 *
 * \param seq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param dst_iovs Destination I/O vector array.
 * \param dst_iovcnt Size of the `dst_iovs` array.
 * \param dst_domain Memory domain to which the destination buffers belong.
 * \param dst_domain_ctx Destination buffer domain context.
 * \param src_iovs Source I/O vector array.
 * \param src_iovcnt Size of the `src_iovs` array.
 * \param src_domain Memory domain to which the source buffers belong.
 * \param src_domain_ctx Source buffer domain context.
 * \param flags Accel operation flags.
 * \param cb_fn Callback to be executed once this operation is completed.
 * \param cb_arg Argument to be passed to `cb_fn`.
 *
 * \return 0 if operation was successfully added to the sequence, negative errno otherwise.
 */
int spdk_accel_append_decompress(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
				 struct iovec *dst_iovs, size_t dst_iovcnt,
				 struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
				 struct iovec *src_iovs, size_t src_iovcnt,
				 struct spdk_memory_domain *src_domain, void *src_domain_ctx,
				 int flags, spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append an encrypt operation to a sequence.
 *
 * `nbytes` must be multiple of `block_size`.  `iv` is used to encrypt the first logical block of
 * size `block_size`.  If `src_iovs` describes more than one logical block then `iv` will be
 * incremented for each next logical block.  Data Encryption Key identifier should be created before
 * calling this function using methods specific to the accel module being used.
 *
 * \param seq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param key Data Encryption Key identifier
 * \param dst_iovs Destination I/O vector array.
 * \param dst_iovcnt Size of the `dst_iovs` array.
 * \param dst_domain Memory domain to which the destination buffers belong.
 * \param dst_domain_ctx Destination buffer domain context.
 * \param src_iovs Source I/O vector array.
 * \param src_iovcnt Size of the `src_iovs` array.
 * \param src_domain Memory domain to which the source buffers belong.
 * \param src_domain_ctx Source buffer domain context.
 * \param iv Initialization vector (tweak) used for encryption
 * \param block_size Logical block size, if src contains more than 1 logical block, subsequent
 *        logical blocks will be encrypted with incremented `iv`.
 * \param flags Accel operation flags.
 * \param cb_fn Callback to be executed once this operation is completed.
 * \param cb_arg Argument to be passed to `cb_fn`.
 *
 * \return 0 if operation was successfully added to the sequence, negative errno otherwise.
 */
int spdk_accel_append_encrypt(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
			      struct spdk_accel_crypto_key *key,
			      struct iovec *dst_iovs, uint32_t dst_iovcnt,
			      struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
			      struct iovec *src_iovs, uint32_t src_iovcnt,
			      struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			      uint64_t iv, uint32_t block_size, int flags,
			      spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append a decrypt operation to a sequence.
 *
 * `nbytes` must be multiple of `block_size`. `iv` is used to decrypt the first logical block of
 * size `block_size`. If `src_iovs` describes more than one logical block then `iv` will be
 * incremented for each next logical block.  Data Encryption Key identifier should be created before
 * calling this function using methods specific to the accel module being used.
 *
 * \param seq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param key Data Encryption Key identifier
 * \param dst_iovs Destination I/O vector array.
 * \param dst_iovcnt Size of the `dst_iovs` array.
 * \param dst_domain Memory domain to which the destination buffers belong.
 * \param dst_domain_ctx Destination buffer domain context.
 * \param src_iovs Source I/O vector array.
 * \param src_iovcnt Size of the `src_iovs` array.
 * \param src_domain Memory domain to which the source buffers belong.
 * \param src_domain_ctx Source buffer domain context.
 * \param iv Initialization vector (tweak) used for decryption. Should be the same as `iv` used for
 *        encryption of a data block.
 * \param block_size Logical block size, if src contains more than 1 logical block, subsequent
 *        logical blocks will be decrypted with incremented `iv`.
 * \param flags Accel operation flags.
 * \param cb_fn Callback to be executed once this operation is completed.
 * \param cb_arg Argument to be passed to `cb_fn`.
 *
 * \return 0 if operation was successfully added to the sequence, negative errno otherwise.
 */
int spdk_accel_append_decrypt(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
			      struct spdk_accel_crypto_key *key,
			      struct iovec *dst_iovs, uint32_t dst_iovcnt,
			      struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
			      struct iovec *src_iovs, uint32_t src_iovcnt,
			      struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			      uint64_t iv, uint32_t block_size, int flags,
			      spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Finish a sequence and execute all its operations. After the completion callback is executed, the
 * sequence object is automatically freed.
 *
 * \param seq Sequence to finish.
 * \param cb_fn Completion callback to be executed once all operations are executed.
 * \param cb_arg Argument to be passed to `cb_fn`.
 *
 * \return 0 on success, negative errno otherwise.
 */
int spdk_accel_sequence_finish(struct spdk_accel_sequence *seq,
			       spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Reverse a sequence, so that the last operation becomes the first and vice versa.
 *
 * \param seq Sequence to reverse.
 */
void spdk_accel_sequence_reverse(struct spdk_accel_sequence *seq);

/**
 * Abort a sequence.  This will execute the completion callbacks of all operations that were added
 * to the sequence and will then free the sequence object.
 *
 * \param seq Sequence to abort.
 */
void spdk_accel_sequence_abort(struct spdk_accel_sequence *seq);

/**
 * Allocate a buffer from accel domain.  These buffers can be only used with operations appended to
 * a sequence.  The actual data buffer won't be allocated immediately, but only when it's necessary
 * to execute a given operation.  In some cases, this might even mean that a data buffer won't be
 * allocated at all, if a sequence can be executed without it.
 *
 * A buffer can only be a part of one sequence, but it can be used by multiple operations within
 * that sequence.
 *
 * \param ch I/O channel.
 * \param len Length of the buffer to allocate.
 * \param buf Pointer to the allocated buffer.
 * \param domain Memory domain in which the buffer is allocated.
 * \param domain_ctx Memory domain context related to the allocated buffer.
 *
 * \return 0 if a buffer was successfully allocated, negative errno otherwise.
 */
int spdk_accel_get_buf(struct spdk_io_channel *ch, uint64_t len, void **buf,
		       struct spdk_memory_domain **domain, void **domain_ctx);

/**
 * Release a buffer allocated via `spdk_accel_get_buf()`.
 *
 * \param ch I/O channel.
 * \param buf Buffer allocated via `spdk_accel_get_buf()`.
 * \param domain Memory domain in which the buffer is allocated.
 * \param domain_ctx Memory domain context related to the allocated buffer.
 */
void spdk_accel_put_buf(struct spdk_io_channel *ch, void *buf,
			struct spdk_memory_domain *domain, void *domain_ctx);

/**
 * Build and submit a data encryption request.
 *
 * This function will build the encryption request and submit it. \b nbytes must be multiple of \b block_size.
 * \b iv is used to encrypt the first logical block of size \b block_size. If \b src_iovs describes more than
 * one logical block then \b iv will be incremented for each next logical block.
 * Data Encryption Key identifier should be created before calling this function using methods specific to the accel
 * module being used.
 *
 * \param ch I/O channel associated with this call
 * \param key Data Encryption Key identifier
 * \param dst_iovs The io vector array which stores the dst data and len.
 * \param dst_iovcnt The size of the destination io vectors.
 * \param src_iovs The io vector array which stores the src data and len.
 * \param src_iovcnt The size of the source io vectors.
 * \param iv Initialization vector (tweak) used for encryption
 * \param block_size Logical block size, if src contains more than 1 logical block, subsequent logical blocks will be
 * encrypted with incremented \b iv
 * \param flags Accel framework flags for operations.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_encrypt(struct spdk_io_channel *ch, struct spdk_accel_crypto_key *key,
			      struct iovec *dst_iovs, uint32_t dst_iovcnt,
			      struct iovec *src_iovs, uint32_t src_iovcnt,
			      uint64_t iv, uint32_t block_size, int flags,
			      spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Build and submit a data decryption request.
 *
 * This function will build the decryption request and submit it. \b nbytes must be multiple of \b block_size.
 * \b iv is used to decrypt the first logical block of size \b block_size. If \b src_iovs describes more than
 * one logical block then \b iv will be incremented for each next logical block.
 * Data Encryption Key identifier should be created before calling this function using methods specific to the accel
 * module being used.
 *
 * \param ch I/O channel associated with this call
 * \param key Data Encryption Key identifier
 * \param dst_iovs The io vector array which stores the dst data and len.
 * \param dst_iovcnt The size of the destination io vectors.
 * \param src_iovs The io vector array which stores the src data and len.
 * \param src_iovcnt The size of the source io vectors.
 * \param iv Initialization vector (tweak) used for decryption. Should be the same as \b iv used for encryption of a
 * data block
 * \param block_size Logical block size, if src contains more than 1 logical block, subsequent logical blocks will be
 * decrypted with incremented \b iv
 * \param flags Accel framework flags for operations.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_decrypt(struct spdk_io_channel *ch, struct spdk_accel_crypto_key *key,
			      struct iovec *dst_iovs, uint32_t dst_iovcnt,
			      struct iovec *src_iovs, uint32_t src_iovcnt,
			      uint64_t iv, uint32_t block_size, int flags,
			      spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Return the name of the module assigned to a specific opcode.
 *
 * \param opcode Accel Framework Opcode enum value. Valid codes can be retrieved using
 * `accel_get_opc_assignments` or `spdk_accel_get_opc_name`.
 * \param module_name Pointer to update with module name.
 *
 * \return 0 if a valid module name was provided. -EINVAL for invalid opcode
 *  or -ENOENT no module was found at this time for the provided opcode.
 */
int spdk_accel_get_opc_module_name(enum accel_opcode opcode, const char **module_name);

/**
 * Override the assignment of an opcode to an module.
 *
 * \param opcode Accel Framework Opcode enum value. Valid codes can be retrieved using
 * `accel_get_opc_assignments` or `spdk_accel_get_opc_name`.
 * \param name Name of the module to assign. Valid module names may be retrieved
 * with `spdk_accel_get_opc_module_name`
 *
 * \return 0 if a valid opcode name was provided. -EINVAL for invalid opcode
 *  or if the framework has started (cannot change modules after startup)
 */
int spdk_accel_assign_opc(enum accel_opcode opcode, const char *name);

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
