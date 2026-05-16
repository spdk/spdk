/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   Copyright (c) 2022, 2023 NVIDIA CORPORATION & AFFILIATES
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/accel_module.h"
#include "accel_internal.h"

#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/crc32.h"
#include "spdk/util.h"
#include "spdk/xor.h"
#include "spdk/dif.h"

#ifdef SPDK_CONFIG_HAVE_LZ4
#include <lz4.h>
#endif

#ifdef SPDK_CONFIG_ISAL
#include "spdk/isa-l.h"
#ifdef SPDK_CONFIG_ISAL_CRYPTO
#include "spdk/isa-l-crypto.h"
#endif
#endif

/* Per the AES-XTS spec, the size of data unit cannot be bigger than 2^20 blocks, 128b each block */
#define ACCEL_AES_XTS_MAX_BLOCK_SIZE (1 << 24)

#ifdef SPDK_CONFIG_ISAL
#define COMP_DEFLATE_MIN_LEVEL ISAL_DEF_MIN_LEVEL
#define COMP_DEFLATE_MAX_LEVEL ISAL_DEF_MAX_LEVEL
#else
#define COMP_DEFLATE_MIN_LEVEL 0
#define COMP_DEFLATE_MAX_LEVEL 0
#endif

#define COMP_DEFLATE_LEVEL_NUM (COMP_DEFLATE_MAX_LEVEL + 1)

struct comp_deflate_level_buf {
	uint32_t size;
	uint8_t  *buf;
};

struct sw_accel_io_channel {
	/* for ISAL */
#ifdef SPDK_CONFIG_ISAL
	struct isal_zstream		stream;
	struct inflate_state		state;
	/* The array index corresponds to the algorithm level */
	struct comp_deflate_level_buf   deflate_level_bufs[COMP_DEFLATE_LEVEL_NUM];
	uint8_t                         level_buf_mem[ISAL_DEF_LVL0_DEFAULT + ISAL_DEF_LVL1_DEFAULT +
					      ISAL_DEF_LVL2_DEFAULT + ISAL_DEF_LVL3_DEFAULT];
#endif
#ifdef SPDK_CONFIG_HAVE_LZ4
	/* for lz4 */
	LZ4_stream_t                    *lz4_stream;
	LZ4_streamDecode_t              *lz4_stream_decode;
#endif
	struct spdk_poller		*completion_poller;
	STAILQ_HEAD(, spdk_accel_task)	tasks_to_complete;
};

typedef int (*sw_accel_crypto_op)(const uint8_t *k2, const uint8_t *k1,
				  const uint8_t *initial_tweak, const uint64_t len_bytes,
				  const void *in, void *out);

struct sw_accel_crypto_key_data {
	sw_accel_crypto_op encrypt;
	sw_accel_crypto_op decrypt;
};

static struct spdk_accel_module_if g_sw_module;

static void sw_accel_crypto_key_deinit(struct spdk_accel_crypto_key *_key);
static int sw_accel_crypto_key_init(struct spdk_accel_crypto_key *key);
static bool sw_accel_crypto_supports_tweak_mode(enum spdk_accel_crypto_tweak_mode tweak_mode);
static bool sw_accel_crypto_supports_cipher(enum spdk_accel_cipher cipher, size_t key_size);

/* Post SW completions to a list; processed by ->completion_poller. */
inline static void
_add_to_comp_list(struct sw_accel_io_channel *sw_ch, struct spdk_accel_task *accel_task, int status)
{
	accel_task->status = status;
	STAILQ_INSERT_TAIL(&sw_ch->tasks_to_complete, accel_task, link);
}

static bool
sw_accel_supports_opcode(enum spdk_accel_opcode opc)
{
	switch (opc) {
	case SPDK_ACCEL_OPC_COPY:
	case SPDK_ACCEL_OPC_FILL:
	case SPDK_ACCEL_OPC_DUALCAST:
	case SPDK_ACCEL_OPC_COMPARE:
	case SPDK_ACCEL_OPC_CRC32C:
	case SPDK_ACCEL_OPC_COPY_CRC32C:
	case SPDK_ACCEL_OPC_COMPRESS:
	case SPDK_ACCEL_OPC_DECOMPRESS:
	case SPDK_ACCEL_OPC_ENCRYPT:
	case SPDK_ACCEL_OPC_DECRYPT:
	case SPDK_ACCEL_OPC_XOR:
	case SPDK_ACCEL_OPC_DIF_VERIFY:
	case SPDK_ACCEL_OPC_DIF_GENERATE:
	case SPDK_ACCEL_OPC_DIF_GENERATE_COPY:
	case SPDK_ACCEL_OPC_DIF_VERIFY_COPY:
	case SPDK_ACCEL_OPC_DIX_GENERATE:
	case SPDK_ACCEL_OPC_DIX_VERIFY:
		return true;
	default:
		return false;
	}
}

static int
_sw_accel_dualcast_iovs(struct iovec *dst_iovs, uint32_t dst_iovcnt,
			struct iovec *dst2_iovs, uint32_t dst2_iovcnt,
			struct iovec *src_iovs, uint32_t src_iovcnt)
{
	if (spdk_unlikely(dst_iovcnt != 1 || dst2_iovcnt != 1 || src_iovcnt != 1)) {
		return -EINVAL;
	}

	if (spdk_unlikely(dst_iovs[0].iov_len != src_iovs[0].iov_len ||
			  dst_iovs[0].iov_len != dst2_iovs[0].iov_len)) {
		return -EINVAL;
	}

	memcpy(dst_iovs[0].iov_base, src_iovs[0].iov_base, dst_iovs[0].iov_len);
	memcpy(dst2_iovs[0].iov_base, src_iovs[0].iov_base, dst_iovs[0].iov_len);

	return 0;
}

static void
_sw_accel_copy_iovs(struct iovec *dst_iovs, uint32_t dst_iovcnt,
		    struct iovec *src_iovs, uint32_t src_iovcnt)
{
	struct spdk_ioviter iter;
	void *src, *dst;
	size_t len;

	for (len = spdk_ioviter_first(&iter, src_iovs, src_iovcnt,
				      dst_iovs, dst_iovcnt, &src, &dst);
	     len != 0;
	     len = spdk_ioviter_next(&iter, &src, &dst)) {
		memcpy(dst, src, len);
	}
}

static int
_sw_accel_compare(struct iovec *src_iovs, uint32_t src_iovcnt,
		  struct iovec *src2_iovs, uint32_t src2_iovcnt)
{
	struct spdk_ioviter iter;
	void *src1, *src2;
	size_t len = spdk_ioviter_first(&iter, src_iovs, src_iovcnt, src2_iovs,
					src2_iovcnt, &src1, &src2);

	while (len > 0) {
		if (memcmp(src1, src2, len) != 0) {
			return -EILSEQ;
		}
		len = spdk_ioviter_next(&iter, &src1, &src2);
	}
	return 0;
}

static int
_sw_accel_fill(struct iovec *iovs, uint32_t iovcnt, uint8_t fill)
{
	void *dst;
	size_t nbytes;

	if (spdk_unlikely(iovcnt != 1)) {
		return -EINVAL;
	}

	dst = iovs[0].iov_base;
	nbytes = iovs[0].iov_len;

	memset(dst, fill, nbytes);

	return 0;
}

static void
_sw_accel_crc32cv(uint32_t *crc_dst, struct iovec *iov, uint32_t iovcnt, uint32_t seed)
{
	*crc_dst = spdk_crc32c_iov_update(iov, iovcnt, ~seed);
}

static int
_sw_accel_compress_lz4(struct sw_accel_io_channel *sw_ch, struct spdk_accel_task *accel_task)
{
#ifdef SPDK_CONFIG_HAVE_LZ4
	LZ4_stream_t *stream = sw_ch->lz4_stream;
	struct iovec *siov = accel_task->s.iovs;
	struct iovec *diov = accel_task->d.iovs;
	size_t dst_segoffset = 0;
	int32_t comp_size = 0;
	uint32_t output_size = 0;
	uint32_t i, d = 0;
	int rc = 0;

	LZ4_resetStream(stream);
	for (i = 0; i < accel_task->s.iovcnt; i++) {
		if ((diov[d].iov_len - dst_segoffset) == 0) {
			if (++d < accel_task->d.iovcnt) {
				dst_segoffset = 0;
			} else {
				SPDK_ERRLOG("Not enough destination buffer provided.\n");
				rc = -ENOMEM;
				break;
			}
		}

		comp_size = LZ4_compress_fast_continue(stream, siov[i].iov_base, diov[d].iov_base + dst_segoffset,
						       siov[i].iov_len, diov[d].iov_len - dst_segoffset,
						       accel_task->comp.level);
		if (comp_size <= 0) {
			SPDK_ERRLOG("LZ4_compress_fast_continue was incorrectly executed.\n");
			rc = -EIO;
			break;
		}

		dst_segoffset += comp_size;
		output_size += comp_size;
	}

	/* Get our total output size */
	if (accel_task->output_size != NULL) {
		*accel_task->output_size = output_size;
	}

	return rc;
#else
	SPDK_ERRLOG("LZ4 library is required to use software compression.\n");
	return -EINVAL;
#endif
}

static int
_sw_accel_decompress_lz4(struct sw_accel_io_channel *sw_ch, struct spdk_accel_task *accel_task)
{
#ifdef SPDK_CONFIG_HAVE_LZ4
	LZ4_streamDecode_t *stream = sw_ch->lz4_stream_decode;
	struct iovec *siov = accel_task->s.iovs;
	struct iovec *diov = accel_task->d.iovs;
	size_t dst_segoffset = 0;
	int32_t decomp_size = 0;
	uint32_t output_size = 0;
	uint32_t i, d = 0;
	int rc = 0;

	LZ4_setStreamDecode(stream, NULL, 0);
	for (i = 0; i < accel_task->s.iovcnt; ++i) {
		if ((diov[d].iov_len - dst_segoffset) == 0) {
			if (++d < accel_task->d.iovcnt) {
				dst_segoffset = 0;
			} else {
				SPDK_ERRLOG("Not enough destination buffer provided.\n");
				rc = -ENOMEM;
				break;
			}
		}
		decomp_size = LZ4_decompress_safe_continue(stream, siov[i].iov_base,
				diov[d].iov_base + dst_segoffset, siov[i].iov_len,
				diov[d].iov_len - dst_segoffset);
		if (decomp_size < 0) {
			SPDK_ERRLOG("LZ4_compress_fast_continue was incorrectly executed.\n");
			rc = -EIO;
			break;
		}
		dst_segoffset += decomp_size;
		output_size += decomp_size;
	}

	/* Get our total output size */
	if (accel_task->output_size != NULL) {
		*accel_task->output_size = output_size;
	}

	return rc;
#else
	SPDK_ERRLOG("LZ4 library is required to use software compression.\n");
	return -EINVAL;
#endif
}

static int
_sw_accel_compress_deflate(struct sw_accel_io_channel *sw_ch, struct spdk_accel_task *accel_task)
{
#ifdef SPDK_CONFIG_ISAL
	size_t last_seglen = accel_task->s.iovs[accel_task->s.iovcnt - 1].iov_len;
	struct iovec *siov = accel_task->s.iovs;
	struct iovec *diov = accel_task->d.iovs;
	size_t remaining;
	uint32_t i, s = 0, d = 0;
	int rc = 0;

	if (accel_task->comp.level > COMP_DEFLATE_MAX_LEVEL) {
		SPDK_ERRLOG("isal_deflate doesn't support this algorithm level(%u)\n", accel_task->comp.level);
		return -EINVAL;
	}

	remaining = 0;
	for (i = 0; i < accel_task->s.iovcnt; ++i) {
		remaining += accel_task->s.iovs[i].iov_len;
	}

	isal_deflate_reset(&sw_ch->stream);
	sw_ch->stream.end_of_stream = 0;
	sw_ch->stream.next_out = diov[d].iov_base;
	sw_ch->stream.avail_out = diov[d].iov_len;
	sw_ch->stream.next_in = siov[s].iov_base;
	sw_ch->stream.avail_in = siov[s].iov_len;
	sw_ch->stream.level = accel_task->comp.level;
	sw_ch->stream.level_buf = sw_ch->deflate_level_bufs[accel_task->comp.level].buf;
	sw_ch->stream.level_buf_size = sw_ch->deflate_level_bufs[accel_task->comp.level].size;

	do {
		/* if isal has exhausted the current dst iovec, move to the next
		 * one if there is one */
		if (sw_ch->stream.avail_out == 0) {
			if (++d < accel_task->d.iovcnt) {
				sw_ch->stream.next_out = diov[d].iov_base;
				sw_ch->stream.avail_out = diov[d].iov_len;
				assert(sw_ch->stream.avail_out > 0);
			} else {
				/* we have no avail_out but also no more iovecs left so this is
				* the case where either the output buffer was a perfect fit
				* or not enough was provided.  Check the ISAL state to determine
				* which. */
				if (sw_ch->stream.internal_state.state != ZSTATE_END) {
					SPDK_ERRLOG("Not enough destination buffer provided.\n");
					rc = -ENOMEM;
				}
				break;
			}
		}

		/* if isal has exhausted the current src iovec, move to the next
		 * one if there is one */
		if (sw_ch->stream.avail_in == 0 && ((s + 1) < accel_task->s.iovcnt)) {
			s++;
			sw_ch->stream.next_in = siov[s].iov_base;
			sw_ch->stream.avail_in = siov[s].iov_len;
			assert(sw_ch->stream.avail_in > 0);
		}

		if (remaining <= last_seglen) {
			/* Need to set end of stream on last block */
			sw_ch->stream.end_of_stream = 1;
		}

		rc = isal_deflate(&sw_ch->stream);
		if (rc) {
			SPDK_ERRLOG("isal_deflate returned error %d.\n", rc);
		}

		if (remaining > 0) {
			assert(siov[s].iov_len > sw_ch->stream.avail_in);
			remaining -= (siov[s].iov_len - sw_ch->stream.avail_in);
		}

	} while (remaining > 0 || sw_ch->stream.avail_out == 0);
	assert(sw_ch->stream.avail_in  == 0);

	/* Get our total output size */
	if (accel_task->output_size != NULL) {
		assert(sw_ch->stream.total_out > 0);
		*accel_task->output_size = sw_ch->stream.total_out;
	}

	return rc;
#else
	SPDK_ERRLOG("ISAL option is required to use software compression.\n");
	return -EINVAL;
#endif
}

static int
_sw_accel_decompress_deflate(struct sw_accel_io_channel *sw_ch, struct spdk_accel_task *accel_task)
{
#ifdef SPDK_CONFIG_ISAL
	struct iovec *siov = accel_task->s.iovs;
	struct iovec *diov = accel_task->d.iovs;
	uint32_t s = 0, d = 0;
	int rc = 0;

	isal_inflate_reset(&sw_ch->state);
	sw_ch->state.next_out = diov[d].iov_base;
	sw_ch->state.avail_out = diov[d].iov_len;
	sw_ch->state.next_in = siov[s].iov_base;
	sw_ch->state.avail_in = siov[s].iov_len;

	do {
		/* if isal has exhausted the current dst iovec, move to the next
		 * one if there is one */
		if (sw_ch->state.avail_out == 0 && ((d + 1) < accel_task->d.iovcnt)) {
			d++;
			sw_ch->state.next_out = diov[d].iov_base;
			sw_ch->state.avail_out = diov[d].iov_len;
			assert(sw_ch->state.avail_out > 0);
		}

		/* if isal has exhausted the current src iovec, move to the next
		 * one if there is one */
		if (sw_ch->state.avail_in == 0 && ((s + 1) < accel_task->s.iovcnt)) {
			s++;
			sw_ch->state.next_in = siov[s].iov_base;
			sw_ch->state.avail_in = siov[s].iov_len;
			assert(sw_ch->state.avail_in > 0);
		}

		rc = isal_inflate(&sw_ch->state);
		if (rc) {
			SPDK_ERRLOG("isal_inflate returned error %d.\n", rc);
		}

	} while (sw_ch->state.block_state < ISAL_BLOCK_FINISH);
	assert(sw_ch->state.avail_in == 0);

	/* Get our total output size */
	if (accel_task->output_size != NULL) {
		assert(sw_ch->state.total_out > 0);
		*accel_task->output_size = sw_ch->state.total_out;
	}

	return rc;
#else
	SPDK_ERRLOG("ISAL option is required to use software decompression.\n");
	return -EINVAL;
#endif
}

static int
_sw_accel_compress(struct sw_accel_io_channel *sw_ch, struct spdk_accel_task *accel_task)
{
	switch (accel_task->comp.algo) {
	case SPDK_ACCEL_COMP_ALGO_DEFLATE:
		return _sw_accel_compress_deflate(sw_ch, accel_task);
	case SPDK_ACCEL_COMP_ALGO_LZ4:
		return _sw_accel_compress_lz4(sw_ch, accel_task);
	default:
		assert(0);
		return -EINVAL;
	}
}

static int
_sw_accel_decompress(struct sw_accel_io_channel *sw_ch, struct spdk_accel_task *accel_task)
{
	switch (accel_task->comp.algo) {
	case SPDK_ACCEL_COMP_ALGO_DEFLATE:
		return _sw_accel_decompress_deflate(sw_ch, accel_task);
	case SPDK_ACCEL_COMP_ALGO_LZ4:
		return _sw_accel_decompress_lz4(sw_ch, accel_task);
	default:
		assert(0);
		return -EINVAL;
	}
}

static int
_sw_accel_crypto_operation(struct spdk_accel_task *accel_task, struct spdk_accel_crypto_key *key,
			   sw_accel_crypto_op op)
{
#ifdef SPDK_CONFIG_ISAL_CRYPTO
	uint64_t iv[2];
	size_t remaining_len, dst_len;
	uint64_t src_offset = 0, dst_offset = 0;
	uint32_t src_iovpos = 0, dst_iovpos = 0, src_iovcnt, dst_iovcnt;
	uint32_t i, block_size, crypto_len, crypto_accum_len = 0;
	struct iovec *src_iov, *dst_iov;
	uint8_t *src, *dst;
	int rc;

	/* iv is 128 bits, since we are using logical block address (64 bits) as iv, fill first 8 bytes with zeroes */
	iv[0] = 0;
	iv[1] = accel_task->iv;
	src_iov = accel_task->s.iovs;
	src_iovcnt = accel_task->s.iovcnt;
	if (accel_task->d.iovcnt) {
		dst_iov = accel_task->d.iovs;
		dst_iovcnt = accel_task->d.iovcnt;
	} else {
		/* inplace operation */
		dst_iov = accel_task->s.iovs;
		dst_iovcnt = accel_task->s.iovcnt;
	}
	block_size = accel_task->block_size;

	if (!src_iovcnt || !dst_iovcnt || !block_size || !op) {
		SPDK_ERRLOG("src_iovcnt %d, dst_iovcnt %d, block_size %d, op %p\n", src_iovcnt, dst_iovcnt,
			    block_size, op);
		return -EINVAL;
	}

	remaining_len = 0;
	for (i = 0; i < src_iovcnt; i++) {
		remaining_len += src_iov[i].iov_len;
	}
	dst_len = 0;
	for (i = 0; i < dst_iovcnt; i++) {
		dst_len += dst_iov[i].iov_len;
	}

	if (spdk_unlikely(remaining_len != dst_len || !remaining_len)) {
		return -ERANGE;
	}
	if (spdk_unlikely(remaining_len % accel_task->block_size != 0)) {
		return -EINVAL;
	}

	while (remaining_len) {
		crypto_len = spdk_min(block_size - crypto_accum_len, src_iov->iov_len - src_offset);
		crypto_len = spdk_min(crypto_len, dst_iov->iov_len - dst_offset);
		src = (uint8_t *)src_iov->iov_base + src_offset;
		dst = (uint8_t *)dst_iov->iov_base + dst_offset;

		rc = op((uint8_t *)key->key2, (uint8_t *)key->key, (uint8_t *)iv, crypto_len, src, dst);
		if (rc != ISAL_CRYPTO_ERR_NONE) {
			break;
		}

		src_offset += crypto_len;
		dst_offset += crypto_len;
		crypto_accum_len += crypto_len;
		remaining_len -= crypto_len;

		if (crypto_accum_len == block_size) {
			/* we can process part of logical block. Once the whole block is processed, increment iv */
			crypto_accum_len = 0;
			iv[1]++;
		}
		if (src_offset == src_iov->iov_len) {
			src_iov++;
			src_iovpos++;
			src_offset = 0;
		}
		if (src_iovpos == src_iovcnt) {
			break;
		}
		if (dst_offset == dst_iov->iov_len) {
			dst_iov++;
			dst_iovpos++;
			dst_offset = 0;
		}
		if (dst_iovpos == dst_iovcnt) {
			break;
		}
	}

	if (remaining_len) {
		SPDK_ERRLOG("remaining len %zu\n", remaining_len);
		return -EINVAL;
	}

	return 0;
#else
	return -ENOTSUP;
#endif
}

static int
_sw_accel_encrypt(struct sw_accel_io_channel *sw_ch, struct spdk_accel_task *accel_task)
{
	struct spdk_accel_crypto_key *key;
	struct sw_accel_crypto_key_data *key_data;

	key = accel_task->crypto_key;
	if (spdk_unlikely(key->module_if != &g_sw_module || !key->priv)) {
		return -EINVAL;
	}
	if (spdk_unlikely(accel_task->block_size > ACCEL_AES_XTS_MAX_BLOCK_SIZE)) {
		SPDK_WARNLOG("Max block size for AES_XTS is limited to %u, current size %u\n",
			     ACCEL_AES_XTS_MAX_BLOCK_SIZE, accel_task->block_size);
		return -ERANGE;
	}
	key_data = key->priv;
	return _sw_accel_crypto_operation(accel_task, key, key_data->encrypt);
}

static int
_sw_accel_decrypt(struct sw_accel_io_channel *sw_ch, struct spdk_accel_task *accel_task)
{
	struct spdk_accel_crypto_key *key;
	struct sw_accel_crypto_key_data *key_data;

	key = accel_task->crypto_key;
	if (spdk_unlikely(key->module_if != &g_sw_module || !key->priv)) {
		return -EINVAL;
	}
	if (spdk_unlikely(accel_task->block_size > ACCEL_AES_XTS_MAX_BLOCK_SIZE)) {
		SPDK_WARNLOG("Max block size for AES_XTS is limited to %u, current size %u\n",
			     ACCEL_AES_XTS_MAX_BLOCK_SIZE, accel_task->block_size);
		return -ERANGE;
	}
	key_data = key->priv;
	return _sw_accel_crypto_operation(accel_task, key, key_data->decrypt);
}

static int
_sw_accel_xor(struct sw_accel_io_channel *sw_ch, struct spdk_accel_task *accel_task)
{
	return spdk_xor_gen(accel_task->d.iovs[0].iov_base,
			    accel_task->nsrcs.srcs,
			    accel_task->nsrcs.cnt,
			    accel_task->d.iovs[0].iov_len);
}

static int
_sw_accel_dif_verify(struct sw_accel_io_channel *sw_ch, struct spdk_accel_task *accel_task)
{
	return spdk_dif_verify(accel_task->s.iovs,
			       accel_task->s.iovcnt,
			       accel_task->dif.num_blocks,
			       accel_task->dif.ctx,
			       accel_task->dif.err);
}

static int
_sw_accel_dif_verify_copy(struct sw_accel_io_channel *sw_ch, struct spdk_accel_task *accel_task)
{
	return spdk_dif_verify_copy(accel_task->d.iovs,
				    accel_task->d.iovcnt,
				    accel_task->s.iovs,
				    accel_task->s.iovcnt,
				    accel_task->dif.num_blocks,
				    accel_task->dif.ctx,
				    accel_task->dif.err);
}

static int
_sw_accel_dif_generate(struct sw_accel_io_channel *sw_ch, struct spdk_accel_task *accel_task)
{
	return spdk_dif_generate(accel_task->s.iovs,
				 accel_task->s.iovcnt,
				 accel_task->dif.num_blocks,
				 accel_task->dif.ctx);
}

static int
_sw_accel_dif_generate_copy(struct sw_accel_io_channel *sw_ch, struct spdk_accel_task *accel_task)
{
	return spdk_dif_generate_copy(accel_task->s.iovs,
				      accel_task->s.iovcnt,
				      accel_task->d.iovs,
				      accel_task->d.iovcnt,
				      accel_task->dif.num_blocks,
				      accel_task->dif.ctx);
}

static int
_sw_accel_dix_generate(struct sw_accel_io_channel *sw_ch, struct spdk_accel_task *accel_task)
{
	return spdk_dix_generate(accel_task->s.iovs,
				 accel_task->s.iovcnt,
				 accel_task->d.iovs,
				 accel_task->dif.num_blocks,
				 accel_task->dif.ctx);
}

static int
_sw_accel_dix_verify(struct sw_accel_io_channel *sw_ch, struct spdk_accel_task *accel_task)
{
	return spdk_dix_verify(accel_task->s.iovs,
			       accel_task->s.iovcnt,
			       accel_task->d.iovs,
			       accel_task->dif.num_blocks,
			       accel_task->dif.ctx,
			       accel_task->dif.err);
}

static int
accel_comp_poll(void *arg)
{
	struct sw_accel_io_channel	*sw_ch = arg;
	STAILQ_HEAD(, spdk_accel_task)	tasks_to_complete;
	struct spdk_accel_task		*accel_task;

	if (STAILQ_EMPTY(&sw_ch->tasks_to_complete)) {
		return SPDK_POLLER_IDLE;
	}

	STAILQ_INIT(&tasks_to_complete);
	STAILQ_SWAP(&tasks_to_complete, &sw_ch->tasks_to_complete, spdk_accel_task);

	while ((accel_task = STAILQ_FIRST(&tasks_to_complete))) {
		STAILQ_REMOVE_HEAD(&tasks_to_complete, link);
		spdk_accel_task_complete(accel_task, accel_task->status);
	}

	return SPDK_POLLER_BUSY;
}

static int
sw_accel_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task *accel_task)
{
	struct sw_accel_io_channel *sw_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *tmp;
	int rc = 0;

	/*
	 * Lazily initialize our completion poller. We don't want to complete
	 * them inline as they'll likely submit another.
	 */
	if (spdk_unlikely(sw_ch->completion_poller == NULL)) {
		sw_ch->completion_poller = SPDK_POLLER_REGISTER(accel_comp_poll, sw_ch, 0);
	}

	do {
		switch (accel_task->op_code) {
		case SPDK_ACCEL_OPC_COPY:
			_sw_accel_copy_iovs(accel_task->d.iovs, accel_task->d.iovcnt,
					    accel_task->s.iovs, accel_task->s.iovcnt);
			break;
		case SPDK_ACCEL_OPC_FILL:
			rc = _sw_accel_fill(accel_task->d.iovs, accel_task->d.iovcnt,
					    accel_task->fill_pattern);
			break;
		case SPDK_ACCEL_OPC_DUALCAST:
			rc = _sw_accel_dualcast_iovs(accel_task->d.iovs, accel_task->d.iovcnt,
						     accel_task->d2.iovs, accel_task->d2.iovcnt,
						     accel_task->s.iovs, accel_task->s.iovcnt);
			break;
		case SPDK_ACCEL_OPC_COMPARE:
			rc = _sw_accel_compare(accel_task->s.iovs, accel_task->s.iovcnt,
					       accel_task->s2.iovs, accel_task->s2.iovcnt);
			break;
		case SPDK_ACCEL_OPC_CRC32C:
			_sw_accel_crc32cv(accel_task->crc_dst, accel_task->s.iovs, accel_task->s.iovcnt, accel_task->seed);
			break;
		case SPDK_ACCEL_OPC_COPY_CRC32C:
			_sw_accel_copy_iovs(accel_task->d.iovs, accel_task->d.iovcnt,
					    accel_task->s.iovs, accel_task->s.iovcnt);
			_sw_accel_crc32cv(accel_task->crc_dst, accel_task->s.iovs,
					  accel_task->s.iovcnt, accel_task->seed);
			break;
		case SPDK_ACCEL_OPC_COMPRESS:
			rc = _sw_accel_compress(sw_ch, accel_task);
			break;
		case SPDK_ACCEL_OPC_DECOMPRESS:
			rc = _sw_accel_decompress(sw_ch, accel_task);
			break;
		case SPDK_ACCEL_OPC_XOR:
			rc = _sw_accel_xor(sw_ch, accel_task);
			break;
		case SPDK_ACCEL_OPC_ENCRYPT:
			rc = _sw_accel_encrypt(sw_ch, accel_task);
			break;
		case SPDK_ACCEL_OPC_DECRYPT:
			rc = _sw_accel_decrypt(sw_ch, accel_task);
			break;
		case SPDK_ACCEL_OPC_DIF_VERIFY:
			rc = _sw_accel_dif_verify(sw_ch, accel_task);
			break;
		case SPDK_ACCEL_OPC_DIF_VERIFY_COPY:
			rc = _sw_accel_dif_verify_copy(sw_ch, accel_task);
			break;
		case SPDK_ACCEL_OPC_DIF_GENERATE:
			rc = _sw_accel_dif_generate(sw_ch, accel_task);
			break;
		case SPDK_ACCEL_OPC_DIF_GENERATE_COPY:
			rc = _sw_accel_dif_generate_copy(sw_ch, accel_task);
			break;
		case SPDK_ACCEL_OPC_DIX_GENERATE:
			rc = _sw_accel_dix_generate(sw_ch, accel_task);
			break;
		case SPDK_ACCEL_OPC_DIX_VERIFY:
			rc = _sw_accel_dix_verify(sw_ch, accel_task);
			break;
		default:
			assert(false);
			break;
		}

		tmp = STAILQ_NEXT(accel_task, link);

		_add_to_comp_list(sw_ch, accel_task, rc);

		accel_task = tmp;
	} while (accel_task);

	return 0;
}

static int
sw_accel_create_cb(void *io_device, void *ctx_buf)
{
	struct sw_accel_io_channel *sw_ch = ctx_buf;
#ifdef SPDK_CONFIG_ISAL
	struct comp_deflate_level_buf *deflate_level_bufs;
	int i;
#endif

	STAILQ_INIT(&sw_ch->tasks_to_complete);
	sw_ch->completion_poller = NULL;

#ifdef SPDK_CONFIG_HAVE_LZ4
	sw_ch->lz4_stream = LZ4_createStream();
	if (sw_ch->lz4_stream == NULL) {
		SPDK_ERRLOG("Failed to create the lz4 stream for compression\n");
		return -ENOMEM;
	}
	sw_ch->lz4_stream_decode = LZ4_createStreamDecode();
	if (sw_ch->lz4_stream_decode == NULL) {
		SPDK_ERRLOG("Failed to create the lz4 stream for decompression\n");
		LZ4_freeStream(sw_ch->lz4_stream);
		return -ENOMEM;
	}
#endif
#ifdef SPDK_CONFIG_ISAL
	sw_ch->deflate_level_bufs[0].buf = sw_ch->level_buf_mem;
	deflate_level_bufs = sw_ch->deflate_level_bufs;
	deflate_level_bufs[0].size = ISAL_DEF_LVL0_DEFAULT;
	for (i = 1; i < COMP_DEFLATE_LEVEL_NUM; i++) {
		deflate_level_bufs[i].buf = deflate_level_bufs[i - 1].buf +
					    deflate_level_bufs[i - 1].size;
		switch (i) {
		case 1:
			deflate_level_bufs[i].size = ISAL_DEF_LVL1_DEFAULT;
			break;
		case 2:
			deflate_level_bufs[i].size = ISAL_DEF_LVL2_DEFAULT;
			break;
		case 3:
			deflate_level_bufs[i].size = ISAL_DEF_LVL3_DEFAULT;
			break;
		default:
			assert(false);
		}
	}

	isal_deflate_init(&sw_ch->stream);
	sw_ch->stream.flush = NO_FLUSH;
	isal_inflate_init(&sw_ch->state);
#endif

	return 0;
}

static void
sw_accel_destroy_cb(void *io_device, void *ctx_buf)
{
	struct sw_accel_io_channel *sw_ch = ctx_buf;

#ifdef SPDK_CONFIG_HAVE_LZ4
	LZ4_freeStream(sw_ch->lz4_stream);
	LZ4_freeStreamDecode(sw_ch->lz4_stream_decode);
#endif
	spdk_poller_unregister(&sw_ch->completion_poller);
}

static struct spdk_io_channel *
sw_accel_get_io_channel(void)
{
	return spdk_get_io_channel(&g_sw_module);
}

static size_t
sw_accel_module_get_ctx_size(void)
{
	return sizeof(struct spdk_accel_task);
}

static int
sw_accel_module_init(void)
{
	spdk_io_device_register(&g_sw_module, sw_accel_create_cb, sw_accel_destroy_cb,
				sizeof(struct sw_accel_io_channel), "sw_accel_module");

	return 0;
}

static void
sw_accel_module_fini(void *ctxt)
{
	spdk_io_device_unregister(&g_sw_module, NULL);
	spdk_accel_module_finish();
}

static int
sw_accel_create_aes_xts(struct spdk_accel_crypto_key *key)
{
#ifdef SPDK_CONFIG_ISAL_CRYPTO
	struct sw_accel_crypto_key_data *key_data;

	key_data = calloc(1, sizeof(*key_data));
	if (!key_data) {
		return -ENOMEM;
	}

	switch (key->key_size) {
	case SPDK_ACCEL_AES_XTS_128_KEY_SIZE:
		key_data->encrypt = isal_aes_xts_enc_128;
		key_data->decrypt = isal_aes_xts_dec_128;
		break;
	case SPDK_ACCEL_AES_XTS_256_KEY_SIZE:
		key_data->encrypt = isal_aes_xts_enc_256;
		key_data->decrypt = isal_aes_xts_dec_256;
		break;
	default:
		assert(0);
		free(key_data);
		return -EINVAL;
	}

	key->priv = key_data;

	return 0;
#else
	return -ENOTSUP;
#endif
}

static int
sw_accel_crypto_key_init(struct spdk_accel_crypto_key *key)
{
	return sw_accel_create_aes_xts(key);
}

static void
sw_accel_crypto_key_deinit(struct spdk_accel_crypto_key *key)
{
	if (!key || key->module_if != &g_sw_module || !key->priv) {
		return;
	}

	free(key->priv);
}

static bool
sw_accel_crypto_supports_tweak_mode(enum spdk_accel_crypto_tweak_mode tweak_mode)
{
	return tweak_mode == SPDK_ACCEL_CRYPTO_TWEAK_MODE_SIMPLE_LBA;
}

static bool
sw_accel_crypto_supports_cipher(enum spdk_accel_cipher cipher, size_t key_size)
{
	switch (cipher) {
	case SPDK_ACCEL_CIPHER_AES_XTS:
		return key_size == SPDK_ACCEL_AES_XTS_128_KEY_SIZE || key_size == SPDK_ACCEL_AES_XTS_256_KEY_SIZE;
	default:
		return false;
	}
}

static bool
sw_accel_compress_supports_algo(enum spdk_accel_comp_algo algo)
{
	switch (algo) {
	case SPDK_ACCEL_COMP_ALGO_DEFLATE:
#ifdef SPDK_CONFIG_HAVE_LZ4
	case SPDK_ACCEL_COMP_ALGO_LZ4:
#endif
		return true;
	default:
		return false;
	}
}

static int
sw_accel_get_compress_level_range(enum spdk_accel_comp_algo algo,
				  uint32_t *min_level, uint32_t *max_level)
{
	switch (algo) {
	case SPDK_ACCEL_COMP_ALGO_DEFLATE:
#ifdef SPDK_CONFIG_ISAL
		*min_level = COMP_DEFLATE_MIN_LEVEL;
		*max_level = COMP_DEFLATE_MAX_LEVEL;
		return 0;
#else
		SPDK_ERRLOG("ISAL option is required to use software compression.\n");
		return -EINVAL;
#endif
	case SPDK_ACCEL_COMP_ALGO_LZ4:
#ifdef SPDK_CONFIG_HAVE_LZ4
		*min_level = 1;
		*max_level = 65537;
		return 0;
#else
		SPDK_ERRLOG("LZ4 library is required to use software compression.\n");
		return -EINVAL;
#endif
	default:
		return -EINVAL;
	}
}

static int
sw_accel_get_operation_info(enum spdk_accel_opcode opcode,
			    const struct spdk_accel_operation_exec_ctx *ctx,
			    struct spdk_accel_opcode_info *info)
{
	info->required_alignment = 0;

	return 0;
}

static struct spdk_accel_module_if g_sw_module = {
	.module_init			= sw_accel_module_init,
	.module_fini			= sw_accel_module_fini,
	.write_config_json		= NULL,
	.get_ctx_size			= sw_accel_module_get_ctx_size,
	.name				= "software",
	.priority			= SPDK_ACCEL_SW_PRIORITY,
	.supports_opcode		= sw_accel_supports_opcode,
	.get_io_channel			= sw_accel_get_io_channel,
	.submit_tasks			= sw_accel_submit_tasks,
	.crypto_key_init		= sw_accel_crypto_key_init,
	.crypto_key_deinit		= sw_accel_crypto_key_deinit,
	.crypto_supports_tweak_mode	= sw_accel_crypto_supports_tweak_mode,
	.crypto_supports_cipher		= sw_accel_crypto_supports_cipher,
	.compress_supports_algo         = sw_accel_compress_supports_algo,
	.get_compress_level_range       = sw_accel_get_compress_level_range,
	.get_operation_info		= sw_accel_get_operation_info,
};

SPDK_ACCEL_MODULE_REGISTER(sw, &g_sw_module)
