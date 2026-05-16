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
#include "spdk/dif.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPDK_ACCEL_AES_XTS_128_KEY_SIZE 16
#define SPDK_ACCEL_AES_XTS_256_KEY_SIZE 32

enum spdk_accel_comp_algo {
	SPDK_ACCEL_COMP_ALGO_DEFLATE = 0,
	SPDK_ACCEL_COMP_ALGO_LZ4
};

/** Data Encryption Key identifier */
struct spdk_accel_crypto_key;

struct spdk_accel_crypto_key_create_param {
	char *cipher;	/**< Cipher to be used for crypto operations */
	char *hex_key;	/**< Hexlified key */
	char *hex_key2;	/**< Hexlified key2 */
	char *tweak_mode;	/**< Tweak mode */
	char *key_name;	/**< Key name */
};

enum spdk_accel_opcode {
	SPDK_ACCEL_OPC_COPY			= 0,
	SPDK_ACCEL_OPC_FILL			= 1,
	SPDK_ACCEL_OPC_DUALCAST			= 2,
	SPDK_ACCEL_OPC_COMPARE			= 3,
	SPDK_ACCEL_OPC_CRC32C			= 4,
	SPDK_ACCEL_OPC_COPY_CRC32C		= 5,
	SPDK_ACCEL_OPC_COMPRESS			= 6,
	SPDK_ACCEL_OPC_DECOMPRESS		= 7,
	SPDK_ACCEL_OPC_ENCRYPT			= 8,
	SPDK_ACCEL_OPC_DECRYPT			= 9,
	SPDK_ACCEL_OPC_XOR			= 10,
	SPDK_ACCEL_OPC_DIF_VERIFY		= 11,
	SPDK_ACCEL_OPC_DIF_VERIFY_COPY		= 12,
	SPDK_ACCEL_OPC_DIF_GENERATE		= 13,
	SPDK_ACCEL_OPC_DIF_GENERATE_COPY	= 14,
	SPDK_ACCEL_OPC_DIX_GENERATE		= 15,
	SPDK_ACCEL_OPC_DIX_VERIFY		= 16,
	SPDK_ACCEL_OPC_LAST			= 17,
};

enum spdk_accel_cipher {
	SPDK_ACCEL_CIPHER_AES_CBC,
	SPDK_ACCEL_CIPHER_AES_XTS,
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
				   uint32_t iovcnt, uint32_t *crc_dst, uint32_t seed,
				   spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Build and submit a memory compress request using the deflate algorithm.
 *
 * This function will build the compress descriptor and submit it.
 *
 * \param ch I/O channel associated with this call
 * \param dst Destination to write the data to.
 * \param nbytes Length in bytes.
 * \param src_iovs The io vector array which stores the src data and len.
 * \param src_iovcnt The size of the src io vectors.
 * \param output_size The size of the compressed data (may be NULL if not desired)
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 * the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_compress(struct spdk_io_channel *ch, void *dst,
			       uint64_t nbytes, struct iovec *src_iovs,
			       size_t src_iovcnt, uint32_t *output_size,
			       spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Build and submit a memory decompress request using the deflate algorithm.
 *
 * This function will build the decompress descriptor and submit it.
 *
 * \param ch I/O channel associated with this call
 * \param dst_iovs The io vector array which stores the dst data and len.
 * \param dst_iovcnt The size of the dst io vectors.
 * \param src_iovs The io vector array which stores the src data and len.
 * \param src_iovcnt The size of the src io vectors.
 * \param output_size The size of the compressed data (may be NULL if not desired)
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 * the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_decompress(struct spdk_io_channel *ch, struct iovec *dst_iovs,
				 size_t dst_iovcnt, struct iovec *src_iovs,
				 size_t src_iovcnt, uint32_t *output_size,
				 spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Build and submit a memory compress request using the specified algorithm.
 *
 * This function will build the compress descriptor and submit it.
 *
 * \param ch I/O channel associated with this call
 * \param dst Destination to write the data to.
 * \param nbytes Length in bytes.
 * \param src_iovs The io vector array which stores the src data and len.
 * \param src_iovcnt The size of the src io vectors.
 * \param comp_algo The compression algorithm, enum spdk_accel_comp_algo value.
 * \param comp_level The compression algorithm level.
 * \param output_size The size of the compressed data (may be NULL if not desired)
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 * the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_compress_ext(struct spdk_io_channel *ch, void *dst, uint64_t nbytes,
				   struct iovec *src_iovs, size_t src_iovcnt,
				   enum spdk_accel_comp_algo comp_algo, uint32_t comp_level,
				   uint32_t *output_size, spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Build and submit a memory decompress request using the specified algorithm.
 *
 * This function will build the decompress descriptor and submit it.
 *
 * \param ch I/O channel associated with this call
 * \param dst_iovs The io vector array which stores the dst data and len.
 * \param dst_iovcnt The size of the dst io vectors.
 * \param src_iovs The io vector array which stores the src data and len.
 * \param src_iovcnt The size of the src io vectors.
 * \param decomp_algo The decompression algorithm, enum spdk_accel_comp_algo value.
 * \param output_size The size of the compressed data (may be NULL if not desired)
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 * the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_decompress_ext(struct spdk_io_channel *ch, struct iovec *dst_iovs,
				     size_t dst_iovcnt, struct iovec *src_iovs, size_t src_iovcnt,
				     enum spdk_accel_comp_algo decomp_algo, uint32_t *output_size,
				     spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Gets the level range of the specified algorithm.
 *
 * \param comp_algo The compression algorithm.
 * \param min_level The lowest level supported by the compression algorithm.
 * \param max_level The highest level supported by the compression algorithm.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_get_compress_level_range(enum spdk_accel_comp_algo comp_algo,
					uint32_t *min_level, uint32_t *max_level);

/**
 * Submit an xor request.
 *
 * \param ch I/O channel associated with this call.
 * \param dst Destination to write the data to.
 * \param sources Array of source buffers.
 * \param nsrcs Number of source buffers in the array.
 * \param nbytes Length in bytes.
 * \param cb_fn Called when this copy operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_xor(struct spdk_io_channel *ch, void *dst, void **sources, uint32_t nsrcs,
			  uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Build and submit a data encryption request.
 *
 * This function will build the encryption request and submit it. \b nbytes must be multiple of \b
 * block_size.  \b iv is used to encrypt the first logical block of size \b block_size. If \b
 * src_iovs describes more than one logical block then \b iv will be incremented for each next
 * logical block.  Data Encryption Key identifier should be created before calling this function
 * using methods specific to the accel module being used.
 *
 * \param ch I/O channel associated with this call
 * \param key Data Encryption Key identifier
 * \param dst_iovs The io vector array which stores the dst data and len.
 * \param dst_iovcnt The size of the destination io vectors.
 * \param src_iovs The io vector array which stores the src data and len.
 * \param src_iovcnt The size of the source io vectors.
 * \param iv Initialization vector (tweak) used for encryption
 * \param block_size Logical block size, if src contains more than 1 logical block, subsequent
 *        logical blocks will be encrypted with incremented \b iv
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in the completion
 *        callback.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_encrypt(struct spdk_io_channel *ch, struct spdk_accel_crypto_key *key,
			      struct iovec *dst_iovs, uint32_t dst_iovcnt,
			      struct iovec *src_iovs, uint32_t src_iovcnt,
			      uint64_t iv, uint32_t block_size,
			      spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Build and submit a data decryption request.
 *
 * This function will build the decryption request and submit it. \b nbytes must be multiple of \b
 * block_size.  \b iv is used to decrypt the first logical block of size \b block_size. If \b
 * src_iovs describes more than one logical block then \b iv will be incremented for each next
 * logical block.  Data Encryption Key identifier should be created before calling this function
 * using methods specific to the accel module being used.
 *
 * \param ch I/O channel associated with this call
 * \param key Data Encryption Key identifier
 * \param dst_iovs The io vector array which stores the dst data and len.
 * \param dst_iovcnt The size of the destination io vectors.
 * \param src_iovs The io vector array which stores the src data and len.
 * \param src_iovcnt The size of the source io vectors.
 * \param iv Initialization vector (tweak) used for decryption. Should be the same as \b iv used for
 *        encryption of a data block
 * \param block_size Logical block size, if src contains more than 1 logical block, subsequent
 *        logical blocks will be decrypted with incremented \b iv
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in the completion
 *        callback.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_decrypt(struct spdk_io_channel *ch, struct spdk_accel_crypto_key *key,
			      struct iovec *dst_iovs, uint32_t dst_iovcnt,
			      struct iovec *src_iovs, uint32_t src_iovcnt,
			      uint64_t iv, uint32_t block_size,
			      spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a Data Integrity Field (DIF) verify request.
 *
 * This operation computes the DIF on the data and compares it against the DIF contained
 * in the metadata.
 *
 * \param ch I/O channel associated with this call.
 * \param iovs The io vector array. The total allocated memory size needs to be at least:
 *             num_blocks * block_size (including metadata)
 * \param iovcnt The size of the io vectors array.
 * \param num_blocks Number of data blocks to check.
 * \param ctx DIF context. Contains the DIF configuration values, including the reference
 *            Application Tag value and initial value of the Reference Tag to check
 *            Note: the user must ensure the validity of this pointer throughout the entire operation
 *            because it is not validated along the processing path.
 * \param err DIF error detailed information.
 *            Note: the user must ensure the validity of this pointer throughout the entire operation
 *            because it is not validated along the processing path.
 * \param cb_fn Called when this operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_dif_verify(struct spdk_io_channel *ch,
				 struct iovec *iovs, size_t iovcnt, uint32_t num_blocks,
				 const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err,
				 spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a Data Integrity Field (DIF) copy and verify request.
 *
 * This operation copies memory from the source to the destination address and removes
 * the DIF data with its verification according to the flags provided in the context.
 *
 * \param ch I/O channel associated with this call.
 * \param dst_iovs The destination I/O vector array. The total allocated memory size needs
 *		  to be at least: num_blocks * data_block_size.
 * \param dst_iovcnt The size of the destination I/O vectors array.
 * \param src_iovs The source I/O vector array. The total allocated memory size needs
 *		  to be at least: num_blocks * block_size (including metadata)
 * \param src_iovcnt The size of the source I/O vectors array.
 * \param num_blocks Number of data blocks to process.
 * \param ctx DIF context. Contains the DIF configuration values, including the reference
 *            Application Tag value and initial value of the Reference Tag to insert.
 * \param err DIF error detailed information.
 *            Note: the user must ensure the validity of this pointer throughout the entire operation
 *            because it is not validated along the processing path.
 * \param cb_fn Called when this operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_dif_verify_copy(struct spdk_io_channel *ch,
				      struct iovec *dst_iovs, size_t dst_iovcnt,
				      struct iovec *src_iovs, size_t src_iovcnt, uint32_t num_blocks,
				      const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err,
				      spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a Data Integrity Field (DIF) generate request.
 *
 * This operation compute the DIF on the source data and inserting the DIF in place into
 * the source data.
 *
 * \param ch I/O channel associated with this call.
 * \param iovs The io vector array. The total allocated memory size needs to be at least:
 *             num_blocks * block_size (including metadata)
 * \param iovcnt The size of the io vectors array.
 * \param num_blocks Number of data blocks.
 * \param ctx DIF context. Contains the DIF configuration values, including the reference
 *            Application Tag value and initial value of the Reference Tag to insert
 * \param cb_fn Called when this operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_dif_generate(struct spdk_io_channel *ch,
				   struct iovec *iovs, size_t iovcnt, uint32_t num_blocks,
				   const struct spdk_dif_ctx *ctx,
				   spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a Data Integrity Field (DIF) copy and generate request.
 *
 * This operation copies memory from the source to the destination address,
 * while computing the DIF on the source data and inserting the DIF into
 * the output data.
 *
 * \param ch I/O channel associated with this call.
 * \param dst_iovs The destination io vector array. The total allocated memory size needs
 *		  to be at least: num_blocks * block_size (provided to spdk_dif_ctx_init())
 * \param dst_iovcnt The size of the destination io vectors array.
 * \param src_iovs The source io vector array. The total allocated memory size needs
 *		  to be at least: num_blocks * data_block_size.
 * \param src_iovcnt The size of the source io vectors array.
 * \param num_blocks Number of data blocks to process.
 * \param ctx DIF context. Contains the DIF configuration values, including the reference
 *            Application Tag value and initial value of the Reference Tag to insert
 * \param cb_fn Called when this operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_dif_generate_copy(struct spdk_io_channel *ch, struct iovec *dst_iovs,
					size_t dst_iovcnt, struct iovec *src_iovs, size_t src_iovcnt,
					uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
					spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a Data Integrity Extension (DIX) generate operation.
 *
 * This operation computes Protection Information (DIX) and inserts it into metadata buffer.
 *
 * \param ch I/O channel associated with this call.
 * \param iovs The source io vector array. The total allocated memory size needs to be at least:
 *	       num_blocks * block_size_no_md
 * \param iovcnt The size of the source io vectors array.
 * \param md_iov The metadata vector array. The total allocated memory size needs to be at least:
 *		  num_blocks * md_size (8B or 16B, depending on the PI format)
 * \param num_blocks Number of data blocks to process.
 * \param ctx DIX context. Contains the DIX configuration values, including the reference
 *	      Application Tag value and initial value of the Reference Tag to insert
 * \param cb_fn Called when this operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_dix_generate(struct spdk_io_channel *ch, struct iovec *iovs,
				   size_t iovcnt, struct iovec *md_iov, uint32_t num_blocks,
				   const struct spdk_dif_ctx *ctx, spdk_accel_completion_cb cb_fn,
				   void *cb_arg);

/**
 * Submit a Data Integrity Extension (DIX) verify request.
 *
 * This operation computes the Protection Information (DIX) on the data and compares it against
 * the DIX contained in the metadata.
 *
 * \param ch I/O channel associated with this call.
 * \param iovs The io vector array. The total allocated memory size needs to be at least:
 *             num_blocks * block_size
 * \param iovcnt The size of the io vectors array.
 * \param md_iov The metadata vector array. The total allocated memory size needs to be at least:
 *		 num_blocks * md_size (8B or 16B, depending on the PI format)
 * \param num_blocks Number of data blocks to check.
 * \param ctx DIX context. Contains the DIF configuration values, including the reference
 *            Application Tag value and initial value of the Reference Tag to check
 *            Note: the user must ensure the validity of this pointer throughout the entire
 *	      operation because it is not validated along the processing path.
 * \param err DIX error detailed information.
 *            Note: the user must ensure the validity of this pointer throughout the entire
 *	      operation because it is not validated along the processing path.
 * \param cb_fn Called when this operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_dix_verify(struct spdk_io_channel *ch, struct iovec *iovs,
				 size_t iovcnt,  struct iovec *md_iov, uint32_t num_blocks,
				 const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err,
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
			   spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append a compare operation to a sequence. This operation compares data from two
 * source buffers to verify they are identical. If a mismatch is found, the operation
 * will typically complete with a failure status.
 *
 * \param pseq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param src1_iovs First source I/O vector array.
 * \param src1_iovcnt Size of the `src1_iovs` array.
 * \param src1_domain Memory domain to which the first source buffers belong.
 * \param src1_domain_ctx First source buffer domain context.
 * \param src2_iovs Second source I/O vector array.
 * \param src2_iovcnt Size of the `src2_iovs` array.
 * \param src2_domain Memory domain to which the second source buffers belong.
 * \param src2_domain_ctx Second source buffer domain context.
 * \param cb_fn Callback to be executed once this operation is completed.
 * \param cb_arg Argument to be passed to `cb_fn`.
 *
 * \return 0 if operation was successfully added to the sequence, negative errno otherwise.
 */
int spdk_accel_append_compare(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
			      struct iovec *src1_iovs, uint32_t src1_iovcnt,
			      struct spdk_memory_domain *src1_domain, void *src1_domain_ctx,
			      struct iovec *src2_iovs, uint32_t src2_iovcnt,
			      struct spdk_memory_domain *src2_domain, void *src2_domain_ctx,
			      spdk_accel_step_cb cb_fn, void *cb_arg);

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
 * \param cb_fn Callback to be executed once this operation is completed.
 * \param cb_arg Argument to be passed to `cb_fn`.
 *
 * \return 0 if operation was successfully added to the sequence, negative errno otherwise.
 */
int spdk_accel_append_fill(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
			   void *buf, uint64_t len,
			   struct spdk_memory_domain *domain, void *domain_ctx, uint8_t pattern,
			   spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append a decompression operation using the specified algorithm to a sequence.
 *
 * \param pseq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param dst_iovs Destination I/O vector array.
 * \param dst_iovcnt Size of the `dst_iovs` array.
 * \param dst_domain Memory domain to which the destination buffers belong.
 * \param dst_domain_ctx Destination buffer domain context.
 * \param src_iovs Source I/O vector array.
 * \param src_iovcnt Size of the `src_iovs` array.
 * \param src_domain Memory domain to which the source buffers belong.
 * \param src_domain_ctx Source buffer domain context.
 * \param decomp_algo The decompression algorithm, enum spdk_accel_comp_algo value.
 * \param cb_fn Callback to be executed once this operation is completed.
 * \param cb_arg Argument to be passed to `cb_fn`.
 *
 * \return 0 if operation was successfully added to the sequence, negative errno otherwise.
 */
int spdk_accel_append_decompress_ext(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
				     struct iovec *dst_iovs, size_t dst_iovcnt,
				     struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
				     struct iovec *src_iovs, size_t src_iovcnt,
				     struct spdk_memory_domain *src_domain, void *src_domain_ctx,
				     enum spdk_accel_comp_algo decomp_algo,
				     spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append a decompression operation using the deflate algorithm to a sequence.
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
				 spdk_accel_step_cb cb_fn, void *cb_arg);

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
			      uint64_t iv, uint32_t block_size,
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
			      uint64_t iv, uint32_t block_size,
			      spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append a crc32c operation to a sequence.
 *
 * \param seq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param dst Destination to write the calculated value.
 * \param iovs Source I/O vector array.
 * \param iovcnt Size of the `iovs` array.
 * \param domain Memory domain to which the source buffers belong.
 * \param domain_ctx Source buffer domain context.
 * \param seed Initial value.
 * \param cb_fn Callback to be executed once this operation is completed.
 * \param cb_arg Argument to be passed to `cb_fn`.
 *
 * \return 0 if operation was successfully added to the sequence, negative errno otherwise.
 */
int spdk_accel_append_crc32c(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
			     uint32_t *dst, struct iovec *iovs, uint32_t iovcnt,
			     struct spdk_memory_domain *domain, void *domain_ctx,
			     uint32_t seed, spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append a Data Integrity Field (DIF) verify operation to a sequence.
 *
 * This operation computes the DIF on the data and compares it against the DIF contained
 * in the metadata.
 *
 * \param seq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param iovs The io vector array. The total allocated memory size needs to be at least:
 *             num_blocks * block_size (including metadata)
 * \param iovcnt The size of the io vectors array.
 * \param domain Memory domain to which the data buffers belong.
 * \param domain_ctx Data buffer domain context.
 * \param num_blocks Number of data blocks to check.
 * \param ctx DIF context. Contains the DIF configuration values, including the reference
 *            Application Tag value and initial value of the Reference Tag to check
 *            Note: the user must ensure the validity of this pointer throughout the entire operation
 *            because it is not validated along the processing path.
 * \param err DIF error detailed information.
 *            Note: the user must ensure the validity of this pointer throughout the entire operation
 *            because it is not validated along the processing path.
 * \param cb_fn Callback to be executed once this operation is completed.
 * \param cb_arg Argument to be passed to `cb_fn`.
 *
 * \return 0 if operation was successfully added to the sequence, negative errno on failure.
 */
int spdk_accel_append_dif_verify(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
				 struct iovec *iovs, size_t iovcnt,
				 struct spdk_memory_domain *domain, void *domain_ctx,
				 uint32_t num_blocks,
				 const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err,
				 spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append a Data Integrity Field (DIF) copy and verify operation to a sequence.
 *
 * This operation copies memory from the source to the destination address and removes
 * the DIF data with its verification according to the flags provided in the context.
 *
 * \param seq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param dst_iovs The destination I/O vector array. The total allocated memory size needs
 *                to be at least: num_blocks * data_block_size
 * \param dst_iovcnt The size of the destination I/O vectors array.
 * \param dst_domain Memory domain to which the destination buffers belong.
 * \param dst_domain_ctx Destination buffer domain context.
 * \param src_iovs The source I/O vector array. The total allocated memory size needs
 *                to be at least: num_blocks * block_size (including metadata)
 * \param src_iovcnt The size of the source I/O vectors array.
 * \param src_domain Memory domain to which the source buffers belong.
 * \param src_domain_ctx Source buffer domain context.
 * \param num_blocks Number of data blocks to process.
 * \param ctx DIF context. Contains the DIF configuration values, including the reference
 *            Application Tag value and initial value of the Reference Tag to insert.
 * \param err DIF error detailed information.
 *            Note: the user must ensure the validity of this pointer throughout the entire operation
 *            because it is not validated along the processing path.
 * \param cb_fn Called when this operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_append_dif_verify_copy(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
				      struct iovec *dst_iovs, size_t dst_iovcnt,
				      struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
				      struct iovec *src_iovs, size_t src_iovcnt,
				      struct spdk_memory_domain *src_domain, void *src_domain_ctx,
				      uint32_t num_blocks,
				      const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err,
				      spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append a Data Integrity Field (DIF) generate operation to a sequence.
 *
 * This operation compute the DIF on the source data and inserting the DIF in place into
 * the source data.
 *
 * \param seq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel associated with this call.
 * \param iovs The io vector array. The total allocated memory size needs to be at least:
 *             num_blocks * block_size (including metadata)
 * \param iovcnt The size of the io vectors array.
 * \param domain Memory domain to which the data buffers belong.
 * \param domain_ctx Data buffer domain context.
 * \param num_blocks Number of data blocks.
 * \param ctx DIF context. Contains the DIF configuration values, including the reference
 *            Application Tag value and initial value of the Reference Tag to insert
 * \param cb_fn Called when this operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_append_dif_generate(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
				   struct iovec *iovs, size_t iovcnt,
				   struct spdk_memory_domain *domain, void *domain_ctx,
				   uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
				   spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Submit a Data Integrity Field (DIF) copy and generate request.
 *
 * This operation copies memory from the source to the destination address,
 * while computing the DIF on the source data and inserting the DIF into
 * the output data.
 *
 * \param seq Sequence object.  If NULL, a new sequence object will be created.
 * \param ch I/O channel associated with this call.
 * \param dst_iovs The destination io vector array. The total allocated memory size needs
 *                to be at least: num_blocks * block_size (provided to spdk_dif_ctx_init())
 * \param dst_iovcnt The size of the destination io vectors array.
 * \param dst_domain Memory domain to which the destination buffers belong.
 * \param dst_domain_ctx Destination buffer domain context.
 * \param src_iovs The source io vector array. The total allocated memory size needs
 *                to be at least: num_blocks * data_block_size.
 * \param src_iovcnt The size of the source io vectors array.
 * \param src_domain Memory domain to which the source buffers belong.
 * \param src_domain_ctx Source buffer domain context.
 * \param num_blocks Number of data blocks to process.
 * \param ctx DIF context. Contains the DIF configuration values, including the reference
 *            Application Tag value and initial value of the Reference Tag to insert
 * \param cb_fn Called when this operation completes.
 * \param cb_arg Callback argument.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_append_dif_generate_copy(struct spdk_accel_sequence **seq,
					struct spdk_io_channel *ch,
					struct iovec *dst_iovs, size_t dst_iovcnt,
					struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
					struct iovec *src_iovs, size_t src_iovcnt,
					struct spdk_memory_domain *src_domain, void *src_domain_ctx,
					uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
					spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append DIX generate operation to a sequence.
 *
 * \param seq Sequence object. If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param iovs Source I/O vector array. The total allocated memory size needs to be at least:
 *	num_blocks * block_size_no_md.
 * \param iovcnt Size of the source I/O vectors' array.
 * \param domain Memory domain to which the source buffers belong.
 * \param domain_ctx Source buffer domain context.
 * \param md_iov Metadata iovec. The total allocated memory size needs to be at least:
 *	num_blocks * md_size (8B or 16B, depending on the PI format).
 * \param md_domain Memory domain to which the metadata buffers belongs.
 * \param md_domain_ctx Metadata buffer domain context.
 * \param num_blocks Number of data blocks to process.
 * \param ctx DIX context. Contains the DIX configuration values, including the reference
 *	Application Tag value and initial value of the Reference Tag to insert.
 * \param cb_fn Called when this operation completes.
 * \param cb_arg Callback argument.
 *
 * \returns 0 on success, negative errno on failure.
 */
int spdk_accel_append_dix_generate(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
				   struct iovec *iovs, size_t iovcnt,
				   struct spdk_memory_domain *domain, void *domain_ctx,
				   struct iovec *md_iov, struct spdk_memory_domain *md_domain,
				   void *md_domain_ctx, uint32_t num_blocks,
				   const struct spdk_dif_ctx *ctx,
				   spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Append DIX verify operation to a sequence.
 *
 * \param seq Sequence object. If NULL, a new sequence object will be created.
 * \param ch I/O channel.
 * \param iovs Source I/O vector array. The total allocated memory size needs to be at least:
 *	num_blocks * block_size_no_md.
 * \param iovcnt Size of the source I/O vectors' array.
 * \param domain Memory domain to which the source buffers belong.
 * \param domain_ctx Source buffer domain context.
 * \param md_iov Metadata iovec. The total allocated memory size needs to be at least:
 *	num_blocks * md_size (8B or 16B, depending on the PI format).
 * \param md_domain Memory domain to which the metadata buffers belongs.
 * \param md_domain_ctx Metadata buffer domain context.
 * \param num_blocks Number of data blocks to process.
 * \param ctx DIX context. Contains the DIX configuration values, including the reference
 *	Application Tag value and initial value of the Reference Tag to insert.
 * \param err DIX error detailed information.
 *	Note: the user must ensure the validity of this pointer throughout the entire
 *	operation because it is not validated along the processing path.
 * \param cb_fn Called when this operation completes.
 * \param cb_arg Callback argument.
 *
 * \returns 0 on success, negative errno on failure.
 */
int spdk_accel_append_dix_verify(struct spdk_accel_sequence **seq, struct spdk_io_channel *ch,
				 struct iovec *iovs, size_t iovcnt,
				 struct spdk_memory_domain *domain, void *domain_ctx,
				 struct iovec *md_iov, struct spdk_memory_domain *md_domain,
				 void *md_domain_ctx, uint32_t num_blocks,
				 const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err,
				 spdk_accel_step_cb cb_fn, void *cb_arg);

/**
 * Finish a sequence and execute all its operations. After the completion callback is executed, the
 * sequence object is automatically freed.
 *
 * \param seq Sequence to finish.
 * \param cb_fn Completion callback to be executed once all operations are executed.
 * \param cb_arg Argument to be passed to `cb_fn`.
 */
void spdk_accel_sequence_finish(struct spdk_accel_sequence *seq,
				spdk_accel_completion_cb cb_fn, void *cb_arg);

/**
 * Reverse a sequence, so that the last operation becomes the first and vice versa.
 *
 * \param seq Sequence to reverse.
 */
void spdk_accel_sequence_reverse(struct spdk_accel_sequence *seq);

/**
 * Abort a sequence.  This will execute the completion callbacks of all operations that were added
 * to the sequence and will then free the sequence object.  This function can only be used before a
 * sequence is executed, i.e. before calling `spdk_accel_sequence_finish()`.
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
 * Return the name of the module assigned to a specific opcode.
 *
 * \param opcode Accel Framework Opcode enum value. Valid codes can be retrieved using
 * `accel_get_opc_assignments` or `spdk_accel_get_opcode_name`.
 * \param module_name Pointer to update with module name.
 *
 * \return 0 if a valid module name was provided. -EINVAL for invalid opcode
 *  or -ENOENT no module was found at this time for the provided opcode.
 */
int spdk_accel_get_opc_module_name(enum spdk_accel_opcode opcode, const char **module_name);

/**
 * Override the assignment of an opcode to an module.
 *
 * \param opcode Accel Framework Opcode enum value. Valid codes can be retrieved using
 * `accel_get_opc_assignments` or `spdk_accel_get_opcode_name`.
 * \param name Name of the module to assign. Valid module names may be retrieved
 * with `spdk_accel_get_opc_module_name`
 *
 * \return 0 if a valid opcode name was provided. -EINVAL for invalid opcode
 *  or if the framework has started (cannot change modules after startup)
 */
int spdk_accel_assign_opc(enum spdk_accel_opcode opcode, const char *name);

struct spdk_json_write_ctx;

/**
 * Write Acceleration subsystem configuration into provided JSON context.
 *
 * \param w JSON write context
 */
void spdk_accel_write_config_json(struct spdk_json_write_ctx *w);

/**
 * Select platform driver to execute operation chains.
 *
 * \param name Name of the driver.  If NULL or empty string, this function will clear the driver
 * that was previously assigned.
 *
 * \return 0 on success, negetive errno otherwise.
 */
int spdk_accel_set_driver(const char *name);

/**
 * Get platform driver name.
 *
 * \return Name of the driver as a null-terminated string or NULL if driver not set.
 */
const char *spdk_accel_get_driver_name(void);

/**
 * Retrieves accel memory domain.
 *
 * \return Accel memory domain.
 */
struct spdk_memory_domain *spdk_accel_get_memory_domain(void);

struct spdk_accel_opts {
	/**
	 * The size of spdk_accel_opts according to the caller of this library is used for ABI
	 * compatibility.  The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t		opts_size;

	/** Size of the small iobuf cache */
	uint32_t	small_cache_size;
	/** Size of the large iobuf cache */
	uint32_t	large_cache_size;
	/** Maximum number of tasks per IO channel */
	uint32_t	task_count;
	/** Maximum number of sequences per IO channel */
	uint32_t	sequence_count;
	/** Maximum number of accel buffers per IO channel */
	uint32_t	buf_count;

} __attribute__((packed));

/**
 * Set the options for the accel framework.
 *
 * \param opts Accel options.
 *
 * \return 0 on success, negative errno otherwise.
 */
int spdk_accel_set_opts(const struct spdk_accel_opts *opts);

/**
 * Get the options for the accel framework.
 *
 * \param opts Output parameter for options.
 * \param opts_size sizeof(*opts)
 */
void spdk_accel_get_opts(struct spdk_accel_opts *opts, size_t opts_size);

struct spdk_accel_opcode_stats {
	/** Number of executed operations */
	uint64_t	executed;
	/** Number of failed operations */
	uint64_t	failed;
	/** Number of processed bytes */
	uint64_t	num_bytes;
} __attribute__((packed));

/**
 * Retrieve opcode statistics for a given IO channel.
 *
 * \param ch I/O channel.
 * \param opcode Operation to retrieve statistics.
 * \param stats Per-channel statistics.
 * \param size Size of the `stats` structure.
 */
void spdk_accel_get_opcode_stats(struct spdk_io_channel *ch, enum spdk_accel_opcode opcode,
				 struct spdk_accel_opcode_stats *stats, size_t size);

/**
 * Context for the `spdk_accel_get_buf_align()` function.  Depending on the operation, some of the
 * fields might be unused.
 */
struct spdk_accel_operation_exec_ctx {
	/** Size of this structure in bytes */
	size_t size;
	/** Block size in bytes, required for encrypt and decrypt */
	uint32_t block_size;
};

/**
 * Get minimum buffer alignment to execute a given operation.  It accounts for constraints of a
 * module assigned to execute a given operation and the driver (if set). The alignment is returned
 * as a power of 2.  The value of 0 means that the buffers don't need to be aligned.
 *
 * \param opcode Opcode.
 * \param ctx Context in which the operation will be executed.
 *
 * \return Minimum alignment.
 */
uint8_t spdk_accel_get_buf_align(enum spdk_accel_opcode opcode,
				 const struct spdk_accel_operation_exec_ctx *ctx);

/**
 * Return memory domains used by specific opcode.
 *
 * The returned memory domains depend on the accel module which is assigned to handle the \b opcode
 *
 * \param opcode Accel Framework Opcode enum value.
 * \param domains Pointer to an array of memory domains to be filled by this function. The user should allocate big enough
  * array to keep all memory domains.
 * \param array_size size of \b domains array
 * \return the number of entries in \b domains array or negated errno. If returned value is bigger than \b array_size passed by the user
  * then the user should increase the size of \b domains array and call this function again. There is no guarantees that
  * the content of \b domains array is valid in that case.
 */
int spdk_accel_get_opc_memory_domains(enum spdk_accel_opcode opcode,
				      struct spdk_memory_domain **domains, int array_size);

/**
 * Return the name of an operation based on the opcode.
 *
 * \param opcode Opcode.
 *
 * \return Name of the operation.
 */
const char *spdk_accel_get_opcode_name(enum spdk_accel_opcode opcode);

#ifdef __cplusplus
}
#endif

#endif
