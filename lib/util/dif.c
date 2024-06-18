/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/dif.h"
#include "spdk/crc16.h"
#include "spdk/crc32.h"
#include "spdk/crc64.h"
#include "spdk/endian.h"
#include "spdk/log.h"
#include "spdk/util.h"

#define REFTAG_MASK_16 0x00000000FFFFFFFF
#define REFTAG_MASK_32 0xFFFFFFFFFFFFFFFF
#define REFTAG_MASK_64 0x0000FFFFFFFFFFFF

/* The variable size Storage Tag and Reference Tag is not supported yet,
 * so the maximum size of the Reference Tag is assumed.
 */
struct spdk_dif {
	union {
		struct {
			uint16_t guard;
			uint16_t app_tag;
			uint32_t stor_ref_space;
		} g16;
		struct {
			uint32_t guard;
			uint16_t app_tag;
			uint16_t stor_ref_space_p1;
			uint64_t stor_ref_space_p2;
		} g32;
		struct {
			uint64_t guard;
			uint16_t app_tag;
			uint16_t stor_ref_space_p1;
			uint32_t stor_ref_space_p2;
		} g64;
	};
};
SPDK_STATIC_ASSERT(SPDK_SIZEOF_MEMBER(struct spdk_dif, g16) == 8, "Incorrect size");
SPDK_STATIC_ASSERT(SPDK_SIZEOF_MEMBER(struct spdk_dif, g32) == 16, "Incorrect size");
SPDK_STATIC_ASSERT(SPDK_SIZEOF_MEMBER(struct spdk_dif, g64) == 16, "Incorrect size");

/* Context to iterate or create a iovec array.
 * Each sgl is either iterated or created at a time.
 */
struct _dif_sgl {
	/* Current iovec in the iteration or creation */
	struct iovec *iov;

	/* Remaining count of iovecs in the iteration or creation. */
	int iovcnt;

	/* Current offset in the iovec */
	uint32_t iov_offset;

	/* Size of the created iovec array in bytes */
	uint32_t total_size;
};

static inline void
_dif_sgl_init(struct _dif_sgl *s, struct iovec *iovs, int iovcnt)
{
	s->iov = iovs;
	s->iovcnt = iovcnt;
	s->iov_offset = 0;
	s->total_size = 0;
}

static void
_dif_sgl_advance(struct _dif_sgl *s, uint32_t step)
{
	s->iov_offset += step;
	while (s->iovcnt != 0) {
		if (s->iov_offset < s->iov->iov_len) {
			break;
		}

		s->iov_offset -= s->iov->iov_len;
		s->iov++;
		s->iovcnt--;
	}
}

static inline void
_dif_sgl_get_buf(struct _dif_sgl *s, uint8_t **_buf, uint32_t *_buf_len)
{
	if (_buf != NULL) {
		*_buf = (uint8_t *)s->iov->iov_base + s->iov_offset;
	}
	if (_buf_len != NULL) {
		*_buf_len = s->iov->iov_len - s->iov_offset;
	}
}

static inline bool
_dif_sgl_append(struct _dif_sgl *s, uint8_t *data, uint32_t data_len)
{
	assert(s->iovcnt > 0);
	s->iov->iov_base = data;
	s->iov->iov_len = data_len;
	s->total_size += data_len;
	s->iov++;
	s->iovcnt--;

	if (s->iovcnt > 0) {
		return true;
	} else {
		return false;
	}
}

static inline bool
_dif_sgl_append_split(struct _dif_sgl *dst, struct _dif_sgl *src, uint32_t data_len)
{
	uint8_t *buf;
	uint32_t buf_len;

	while (data_len != 0) {
		_dif_sgl_get_buf(src, &buf, &buf_len);
		buf_len = spdk_min(buf_len, data_len);

		if (!_dif_sgl_append(dst, buf, buf_len)) {
			return false;
		}

		_dif_sgl_advance(src, buf_len);
		data_len -= buf_len;
	}

	return true;
}

/* This function must be used before starting iteration. */
static bool
_dif_sgl_is_bytes_multiple(struct _dif_sgl *s, uint32_t bytes)
{
	int i;

	for (i = 0; i < s->iovcnt; i++) {
		if (s->iov[i].iov_len % bytes) {
			return false;
		}
	}

	return true;
}

static bool
_dif_sgl_is_valid_block_aligned(struct _dif_sgl *s, uint32_t num_blocks, uint32_t block_size)
{
	uint32_t count = 0;
	int i;

	for (i = 0; i < s->iovcnt; i++) {
		if (s->iov[i].iov_len % block_size) {
			return false;
		}
		count += s->iov[i].iov_len / block_size;
	}

	return count >= num_blocks;
}

/* This function must be used before starting iteration. */
static bool
_dif_sgl_is_valid(struct _dif_sgl *s, uint32_t bytes)
{
	uint64_t total = 0;
	int i;

	for (i = 0; i < s->iovcnt; i++) {
		total += s->iov[i].iov_len;
	}

	return total >= bytes;
}

static void
_dif_sgl_copy(struct _dif_sgl *to, struct _dif_sgl *from)
{
	memcpy(to, from, sizeof(struct _dif_sgl));
}

static bool
_dif_is_disabled(enum spdk_dif_type dif_type)
{
	if (dif_type == SPDK_DIF_DISABLE) {
		return true;
	} else {
		return false;
	}
}

static inline size_t
_dif_size(enum spdk_dif_pi_format dif_pi_format)
{
	uint8_t size;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g16);
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g32);
	} else {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g64);
	}

	return size;
}

static uint32_t
_get_guard_interval(uint32_t block_size, uint32_t md_size, bool dif_loc, bool md_interleave,
		    size_t dif_size)
{
	if (!dif_loc) {
		/* For metadata formats with more than 8/16 bytes (depending on
		 * the PI format), if the DIF is contained in the last 8/16 bytes
		 * of metadata, then the CRC covers all metadata up to but excluding
		 * these last 8/16 bytes.
		 */
		if (md_interleave) {
			return block_size - dif_size;
		} else {
			return md_size - dif_size;
		}
	} else {
		/* For metadata formats with more than 8/16 bytes (depending on
		 * the PI format), if the DIF is contained in the first 8/16 bytes
		 * of metadata, then the CRC does not cover any metadata.
		 */
		if (md_interleave) {
			return block_size - md_size;
		} else {
			return 0;
		}
	}
}

static inline uint8_t
_dif_guard_size(enum spdk_dif_pi_format dif_pi_format)
{
	uint8_t size;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g16.guard);
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g32.guard);
	} else {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g64.guard);
	}

	return size;
}

static inline void
_dif_set_guard(struct spdk_dif *dif, uint64_t guard, enum spdk_dif_pi_format dif_pi_format)
{
	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		to_be16(&(dif->g16.guard), (uint16_t)guard);
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		to_be32(&(dif->g32.guard), (uint32_t)guard);
	} else {
		to_be64(&(dif->g64.guard), guard);
	}
}

static inline uint64_t
_dif_get_guard(struct spdk_dif *dif, enum spdk_dif_pi_format dif_pi_format)
{
	uint64_t guard;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		guard = (uint64_t)from_be16(&(dif->g16.guard));
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		guard = (uint64_t)from_be32(&(dif->g32.guard));
	} else {
		guard = from_be64(&(dif->g64.guard));
	}

	return guard;
}

static inline uint64_t
_dif_generate_guard(uint64_t guard_seed, void *buf, size_t buf_len,
		    enum spdk_dif_pi_format dif_pi_format)
{
	uint64_t guard;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		guard = (uint64_t)spdk_crc16_t10dif((uint16_t)guard_seed, buf, buf_len);
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		guard = (uint64_t)spdk_crc32c_nvme(buf, buf_len, guard_seed);
	} else {
		guard = spdk_crc64_nvme(buf, buf_len, guard_seed);
	}

	return guard;
}

static inline uint64_t
_dif_generate_guard_copy(uint64_t guard_seed, void *dst, void *src, size_t buf_len,
			 enum spdk_dif_pi_format dif_pi_format)
{
	uint64_t guard;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		guard = (uint64_t)spdk_crc16_t10dif_copy((uint16_t)guard_seed, dst, src, buf_len);
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		memcpy(dst, src, buf_len);
		guard = (uint64_t)spdk_crc32c_nvme(src, buf_len, guard_seed);
	} else {
		memcpy(dst, src, buf_len);
		guard = spdk_crc64_nvme(src, buf_len, guard_seed);
	}

	return guard;
}

static inline uint8_t
_dif_apptag_offset(enum spdk_dif_pi_format dif_pi_format)
{
	return _dif_guard_size(dif_pi_format);
}

static inline uint8_t
_dif_apptag_size(void)
{
	return SPDK_SIZEOF_MEMBER(struct spdk_dif, g16.app_tag);
}

static inline void
_dif_set_apptag(struct spdk_dif *dif, uint16_t app_tag, enum spdk_dif_pi_format dif_pi_format)
{
	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		to_be16(&(dif->g16.app_tag), app_tag);
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		to_be16(&(dif->g32.app_tag), app_tag);
	} else {
		to_be16(&(dif->g64.app_tag), app_tag);
	}
}

static inline uint16_t
_dif_get_apptag(struct spdk_dif *dif, enum spdk_dif_pi_format dif_pi_format)
{
	uint16_t app_tag;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		app_tag = from_be16(&(dif->g16.app_tag));
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		app_tag = from_be16(&(dif->g32.app_tag));
	} else {
		app_tag = from_be16(&(dif->g64.app_tag));
	}

	return app_tag;
}

static inline bool
_dif_apptag_ignore(struct spdk_dif *dif, enum spdk_dif_pi_format dif_pi_format)
{
	return _dif_get_apptag(dif, dif_pi_format) == SPDK_DIF_APPTAG_IGNORE;
}

static inline uint8_t
_dif_reftag_offset(enum spdk_dif_pi_format dif_pi_format)
{
	uint8_t offset;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		offset = _dif_apptag_offset(dif_pi_format) + _dif_apptag_size();
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		offset = _dif_apptag_offset(dif_pi_format) + _dif_apptag_size()
			 + SPDK_SIZEOF_MEMBER(struct spdk_dif, g32.stor_ref_space_p1);
	} else {
		offset = _dif_apptag_offset(dif_pi_format) + _dif_apptag_size();
	}

	return offset;
}

static inline uint8_t
_dif_reftag_size(enum spdk_dif_pi_format dif_pi_format)
{
	uint8_t size;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g16.stor_ref_space);
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g32.stor_ref_space_p2);
	} else {
		size = SPDK_SIZEOF_MEMBER(struct spdk_dif, g64.stor_ref_space_p1) +
		       SPDK_SIZEOF_MEMBER(struct spdk_dif, g64.stor_ref_space_p2);
	}

	return size;
}

static inline void
_dif_set_reftag(struct spdk_dif *dif, uint64_t ref_tag, enum spdk_dif_pi_format dif_pi_format)
{
	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		to_be32(&(dif->g16.stor_ref_space), (uint32_t)ref_tag);
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		to_be64(&(dif->g32.stor_ref_space_p2), ref_tag);
	} else {
		to_be16(&(dif->g64.stor_ref_space_p1), (uint16_t)(ref_tag >> 32));
		to_be32(&(dif->g64.stor_ref_space_p2), (uint32_t)ref_tag);
	}
}

static inline uint64_t
_dif_get_reftag(struct spdk_dif *dif, enum spdk_dif_pi_format dif_pi_format)
{
	uint64_t ref_tag;

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		ref_tag = (uint64_t)from_be32(&(dif->g16.stor_ref_space));
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		ref_tag = from_be64(&(dif->g32.stor_ref_space_p2));
	} else {
		ref_tag = (uint64_t)from_be16(&(dif->g64.stor_ref_space_p1));
		ref_tag <<= 32;
		ref_tag |= (uint64_t)from_be32(&(dif->g64.stor_ref_space_p2));
	}

	return ref_tag;
}

static inline bool
_dif_reftag_match(struct spdk_dif *dif, uint64_t ref_tag,
		  enum spdk_dif_pi_format dif_pi_format)
{
	uint64_t _ref_tag;
	bool match;

	_ref_tag = _dif_get_reftag(dif, dif_pi_format);

	if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
		match = (_ref_tag == (ref_tag & REFTAG_MASK_16));
	} else if (dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
		match = (_ref_tag == ref_tag);
	} else {
		match = (_ref_tag == (ref_tag & REFTAG_MASK_64));
	}

	return match;
}

static inline bool
_dif_reftag_ignore(struct spdk_dif *dif, enum spdk_dif_pi_format dif_pi_format)
{
	return _dif_reftag_match(dif, REFTAG_MASK_32, dif_pi_format);
}

static bool
_dif_ignore(struct spdk_dif *dif, const struct spdk_dif_ctx *ctx)
{
	switch (ctx->dif_type) {
	case SPDK_DIF_TYPE1:
	case SPDK_DIF_TYPE2:
		/* If Type 1 or 2 is used, then all DIF checks are disabled when
		 * the Application Tag is 0xFFFF.
		 */
		if (_dif_apptag_ignore(dif, ctx->dif_pi_format)) {
			return true;
		}
		break;
	case SPDK_DIF_TYPE3:
		/* If Type 3 is used, then all DIF checks are disabled when the
		 * Application Tag is 0xFFFF and the Reference Tag is 0xFFFFFFFF
		 * or 0xFFFFFFFFFFFFFFFF depending on the PI format.
		 */

		if (_dif_apptag_ignore(dif, ctx->dif_pi_format) &&
		    _dif_reftag_ignore(dif, ctx->dif_pi_format)) {
			return true;
		}
		break;
	default:
		break;
	}

	return false;
}

int
spdk_dif_ctx_init(struct spdk_dif_ctx *ctx, uint32_t block_size, uint32_t md_size,
		  bool md_interleave, bool dif_loc, enum spdk_dif_type dif_type, uint32_t dif_flags,
		  uint32_t init_ref_tag, uint16_t apptag_mask, uint16_t app_tag,
		  uint32_t data_offset, uint64_t guard_seed, struct spdk_dif_ctx_init_ext_opts *opts)
{
	uint32_t data_block_size;
	enum spdk_dif_pi_format dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	if (opts != NULL) {
		if (opts->dif_pi_format != SPDK_DIF_PI_FORMAT_16 &&
		    opts->dif_pi_format != SPDK_DIF_PI_FORMAT_32 &&
		    opts->dif_pi_format != SPDK_DIF_PI_FORMAT_64) {
			SPDK_ERRLOG("No valid DIF PI format provided.\n");
			return -EINVAL;
		}

		dif_pi_format = opts->dif_pi_format;
	}

	if (md_size < _dif_size(dif_pi_format)) {
		SPDK_ERRLOG("Metadata size is smaller than DIF size.\n");
		return -EINVAL;
	}

	if (md_interleave) {
		if (block_size < md_size) {
			SPDK_ERRLOG("Block size is smaller than DIF size.\n");
			return -EINVAL;
		}
		data_block_size = block_size - md_size;
	} else {
		if (dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
			if (block_size == 0 || (block_size % 512) != 0) {
				SPDK_ERRLOG("Zero block size is not allowed and should be a multiple of 512B\n");
				return -EINVAL;
			}
		} else {
			if (block_size == 0 || (block_size % 4096) != 0) {
				SPDK_ERRLOG("Zero block size is not allowed and should be a multiple of 4kB\n");
				return -EINVAL;
			}
		}

		data_block_size = block_size;
	}

	ctx->block_size = block_size;
	ctx->md_size = md_size;
	ctx->md_interleave = md_interleave;
	ctx->dif_pi_format = dif_pi_format;
	ctx->guard_interval = _get_guard_interval(block_size, md_size, dif_loc, md_interleave,
			      _dif_size(ctx->dif_pi_format));
	ctx->dif_type = dif_type;
	ctx->dif_flags = dif_flags;
	ctx->init_ref_tag = init_ref_tag;
	ctx->apptag_mask = apptag_mask;
	ctx->app_tag = app_tag;
	ctx->data_offset = data_offset;
	ctx->ref_tag_offset = data_offset / data_block_size;
	ctx->last_guard = guard_seed;
	ctx->guard_seed = guard_seed;
	ctx->remapped_init_ref_tag = 0;

	return 0;
}

void
spdk_dif_ctx_set_data_offset(struct spdk_dif_ctx *ctx, uint32_t data_offset)
{
	uint32_t data_block_size;

	if (ctx->md_interleave) {
		data_block_size = ctx->block_size - ctx->md_size;
	} else {
		data_block_size = ctx->block_size;
	}

	ctx->data_offset = data_offset;
	ctx->ref_tag_offset = data_offset / data_block_size;
}

void
spdk_dif_ctx_set_remapped_init_ref_tag(struct spdk_dif_ctx *ctx,
				       uint32_t remapped_init_ref_tag)
{
	ctx->remapped_init_ref_tag = remapped_init_ref_tag;
}

static void
_dif_generate(void *_dif, uint64_t guard, uint32_t offset_blocks,
	      const struct spdk_dif_ctx *ctx)
{
	struct spdk_dif *dif = _dif;
	uint64_t ref_tag;

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		_dif_set_guard(dif, guard, ctx->dif_pi_format);
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_APPTAG_CHECK) {
		_dif_set_apptag(dif, ctx->app_tag, ctx->dif_pi_format);
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_REFTAG_CHECK) {
		/* For type 1 and 2, the reference tag is incremented for each
		 * subsequent logical block. For type 3, the reference tag
		 * remains the same as the initial reference tag.
		 */
		if (ctx->dif_type != SPDK_DIF_TYPE3) {
			ref_tag = ctx->init_ref_tag + ctx->ref_tag_offset + offset_blocks;
		} else {
			ref_tag = ctx->init_ref_tag + ctx->ref_tag_offset;
		}

		/* Overwrite reference tag if initialization reference tag is SPDK_DIF_REFTAG_IGNORE */
		if (ctx->init_ref_tag == SPDK_DIF_REFTAG_IGNORE) {
			if (ctx->dif_pi_format == SPDK_DIF_PI_FORMAT_16) {
				ref_tag = REFTAG_MASK_16;
			} else if (ctx->dif_pi_format == SPDK_DIF_PI_FORMAT_32) {
				ref_tag = REFTAG_MASK_32;
			} else {
				ref_tag = REFTAG_MASK_64;
			}
		}

		_dif_set_reftag(dif, ref_tag, ctx->dif_pi_format);
	}
}

static void
dif_generate(struct _dif_sgl *sgl, uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks = 0;
	uint8_t *buf;
	uint64_t guard = 0;

	while (offset_blocks < num_blocks) {
		_dif_sgl_get_buf(sgl, &buf, NULL);

		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			guard = _dif_generate_guard(ctx->guard_seed, buf, ctx->guard_interval, ctx->dif_pi_format);
		}

		_dif_generate(buf + ctx->guard_interval, guard, offset_blocks, ctx);

		_dif_sgl_advance(sgl, ctx->block_size);
		offset_blocks++;
	}
}

static uint64_t
_dif_generate_split(struct _dif_sgl *sgl, uint32_t offset_in_block, uint32_t data_len,
		    uint64_t guard, uint32_t offset_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_in_dif, buf_len;
	uint8_t *buf;
	struct spdk_dif dif = {};

	assert(offset_in_block < ctx->guard_interval);
	assert(offset_in_block + data_len < ctx->guard_interval ||
	       offset_in_block + data_len == ctx->block_size);

	/* Compute CRC over split logical block data. */
	while (data_len != 0 && offset_in_block < ctx->guard_interval) {
		_dif_sgl_get_buf(sgl, &buf, &buf_len);
		buf_len = spdk_min(buf_len, data_len);
		buf_len = spdk_min(buf_len, ctx->guard_interval - offset_in_block);

		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			guard = _dif_generate_guard(guard, buf, buf_len, ctx->dif_pi_format);
		}

		_dif_sgl_advance(sgl, buf_len);
		offset_in_block += buf_len;
		data_len -= buf_len;
	}

	if (offset_in_block < ctx->guard_interval) {
		return guard;
	}

	/* If a whole logical block data is parsed, generate DIF
	 * and save it to the temporary DIF area.
	 */
	_dif_generate(&dif, guard, offset_blocks, ctx);

	/* Copy generated DIF field to the split DIF field, and then
	 * skip metadata field after DIF field (if any).
	 */
	while (offset_in_block < ctx->block_size) {
		_dif_sgl_get_buf(sgl, &buf, &buf_len);

		if (offset_in_block < ctx->guard_interval + _dif_size(ctx->dif_pi_format)) {
			offset_in_dif = offset_in_block - ctx->guard_interval;
			buf_len = spdk_min(buf_len, _dif_size(ctx->dif_pi_format) - offset_in_dif);

			memcpy(buf, ((uint8_t *)&dif) + offset_in_dif, buf_len);
		} else {
			buf_len = spdk_min(buf_len, ctx->block_size - offset_in_block);
		}

		_dif_sgl_advance(sgl, buf_len);
		offset_in_block += buf_len;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->guard_seed;
	}

	return guard;
}

static void
dif_generate_split(struct _dif_sgl *sgl, uint32_t num_blocks,
		   const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;
	uint64_t guard = 0;

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->guard_seed;
	}

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		_dif_generate_split(sgl, 0, ctx->block_size, guard, offset_blocks, ctx);
	}
}

int
spdk_dif_generate(struct iovec *iovs, int iovcnt, uint32_t num_blocks,
		  const struct spdk_dif_ctx *ctx)
{
	struct _dif_sgl sgl;

	_dif_sgl_init(&sgl, iovs, iovcnt);

	if (!_dif_sgl_is_valid(&sgl, ctx->block_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		return 0;
	}

	if (_dif_sgl_is_bytes_multiple(&sgl, ctx->block_size)) {
		dif_generate(&sgl, num_blocks, ctx);
	} else {
		dif_generate_split(&sgl, num_blocks, ctx);
	}

	return 0;
}

static void
_dif_error_set(struct spdk_dif_error *err_blk, uint8_t err_type,
	       uint64_t expected, uint64_t actual, uint32_t err_offset)
{
	if (err_blk) {
		err_blk->err_type = err_type;
		err_blk->expected = expected;
		err_blk->actual = actual;
		err_blk->err_offset = err_offset;
	}
}

static bool
_dif_reftag_check(struct spdk_dif *dif, const struct spdk_dif_ctx *ctx,
		  uint64_t expected_reftag, uint32_t offset_blocks, struct spdk_dif_error *err_blk)
{
	uint64_t reftag;

	if (ctx->dif_flags & SPDK_DIF_FLAGS_REFTAG_CHECK) {
		switch (ctx->dif_type) {
		case SPDK_DIF_TYPE1:
		case SPDK_DIF_TYPE2:
			/* Compare the DIF Reference Tag field to the passed Reference Tag.
			 * The passed Reference Tag will be the least significant 4 bytes
			 * or 8 bytes (depending on the PI format)
			 * of the LBA when Type 1 is used, and application specific value
			 * if Type 2 is used.
			 */
			if (!_dif_reftag_match(dif, expected_reftag, ctx->dif_pi_format)) {
				reftag = _dif_get_reftag(dif, ctx->dif_pi_format);
				_dif_error_set(err_blk, SPDK_DIF_REFTAG_ERROR, expected_reftag,
					       reftag, offset_blocks);
				SPDK_ERRLOG("Failed to compare Ref Tag: LBA=%" PRIu64 "," \
					    " Expected=%lx, Actual=%lx\n",
					    expected_reftag, expected_reftag, reftag);
				return false;
			}
			break;
		case SPDK_DIF_TYPE3:
			/* For Type 3, computed Reference Tag remains unchanged.
			 * Hence ignore the Reference Tag field.
			 */
			break;
		default:
			break;
		}
	}

	return true;
}

static int
_dif_verify(void *_dif, uint64_t guard, uint32_t offset_blocks,
	    const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk)
{
	struct spdk_dif *dif = _dif;
	uint64_t _guard;
	uint16_t _app_tag;
	uint64_t ref_tag;

	if (_dif_ignore(dif, ctx)) {
		return 0;
	}

	/* For type 1 and 2, the reference tag is incremented for each
	 * subsequent logical block. For type 3, the reference tag
	 * remains the same as the initial reference tag.
	 */
	if (ctx->dif_type != SPDK_DIF_TYPE3) {
		ref_tag = ctx->init_ref_tag + ctx->ref_tag_offset + offset_blocks;
	} else {
		ref_tag = ctx->init_ref_tag + ctx->ref_tag_offset;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		/* Compare the DIF Guard field to the CRC computed over the logical
		 * block data.
		 */
		_guard = _dif_get_guard(dif, ctx->dif_pi_format);
		if (_guard != guard) {
			_dif_error_set(err_blk, SPDK_DIF_GUARD_ERROR, _guard, guard,
				       offset_blocks);
			SPDK_ERRLOG("Failed to compare Guard: LBA=%" PRIu64 "," \
				    "  Expected=%lx, Actual=%lx\n",
				    ref_tag, _guard, guard);
			return -1;
		}
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_APPTAG_CHECK) {
		/* Compare unmasked bits in the DIF Application Tag field to the
		 * passed Application Tag.
		 */
		_app_tag = _dif_get_apptag(dif, ctx->dif_pi_format);
		if ((_app_tag & ctx->apptag_mask) != (ctx->app_tag & ctx->apptag_mask)) {
			_dif_error_set(err_blk, SPDK_DIF_APPTAG_ERROR, ctx->app_tag,
				       (_app_tag & ctx->apptag_mask), offset_blocks);
			SPDK_ERRLOG("Failed to compare App Tag: LBA=%" PRIu64 "," \
				    "  Expected=%x, Actual=%x\n",
				    ref_tag, ctx->app_tag, (_app_tag & ctx->apptag_mask));
			return -1;
		}
	}

	if (!_dif_reftag_check(dif, ctx, ref_tag, offset_blocks, err_blk)) {
		return -1;
	}

	return 0;
}

static int
dif_verify(struct _dif_sgl *sgl, uint32_t num_blocks,
	   const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk)
{
	uint32_t offset_blocks = 0;
	int rc;
	uint8_t *buf;
	uint64_t guard = 0;

	while (offset_blocks < num_blocks) {
		_dif_sgl_get_buf(sgl, &buf, NULL);

		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			guard = _dif_generate_guard(ctx->guard_seed, buf, ctx->guard_interval, ctx->dif_pi_format);
		}

		rc = _dif_verify(buf + ctx->guard_interval, guard, offset_blocks, ctx, err_blk);
		if (rc != 0) {
			return rc;
		}

		_dif_sgl_advance(sgl, ctx->block_size);
		offset_blocks++;
	}

	return 0;
}

static int
_dif_verify_split(struct _dif_sgl *sgl, uint32_t offset_in_block, uint32_t data_len,
		  uint64_t *_guard, uint32_t offset_blocks,
		  const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk)
{
	uint32_t offset_in_dif, buf_len;
	uint8_t *buf;
	uint64_t guard;
	struct spdk_dif dif = {};
	int rc;

	assert(_guard != NULL);
	assert(offset_in_block < ctx->guard_interval);
	assert(offset_in_block + data_len < ctx->guard_interval ||
	       offset_in_block + data_len == ctx->block_size);

	guard = *_guard;

	/* Compute CRC over split logical block data. */
	while (data_len != 0 && offset_in_block < ctx->guard_interval) {
		_dif_sgl_get_buf(sgl, &buf, &buf_len);
		buf_len = spdk_min(buf_len, data_len);
		buf_len = spdk_min(buf_len, ctx->guard_interval - offset_in_block);

		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			guard = _dif_generate_guard(guard, buf, buf_len, ctx->dif_pi_format);
		}

		_dif_sgl_advance(sgl, buf_len);
		offset_in_block += buf_len;
		data_len -= buf_len;
	}

	if (offset_in_block < ctx->guard_interval) {
		*_guard = guard;
		return 0;
	}

	/* Copy the split DIF field to the temporary DIF buffer, and then
	 * skip metadata field after DIF field (if any). */
	while (offset_in_block < ctx->block_size) {
		_dif_sgl_get_buf(sgl, &buf, &buf_len);

		if (offset_in_block < ctx->guard_interval + _dif_size(ctx->dif_pi_format)) {
			offset_in_dif = offset_in_block - ctx->guard_interval;
			buf_len = spdk_min(buf_len, _dif_size(ctx->dif_pi_format) - offset_in_dif);

			memcpy((uint8_t *)&dif + offset_in_dif, buf, buf_len);
		} else {
			buf_len = spdk_min(buf_len, ctx->block_size - offset_in_block);
		}
		_dif_sgl_advance(sgl, buf_len);
		offset_in_block += buf_len;
	}

	rc = _dif_verify(&dif, guard, offset_blocks, ctx, err_blk);
	if (rc != 0) {
		return rc;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->guard_seed;
	}

	*_guard = guard;
	return 0;
}

static int
dif_verify_split(struct _dif_sgl *sgl, uint32_t num_blocks,
		 const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk)
{
	uint32_t offset_blocks;
	uint64_t guard = 0;
	int rc;

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->guard_seed;
	}

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		rc = _dif_verify_split(sgl, 0, ctx->block_size, &guard, offset_blocks,
				       ctx, err_blk);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

int
spdk_dif_verify(struct iovec *iovs, int iovcnt, uint32_t num_blocks,
		const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk)
{
	struct _dif_sgl sgl;

	_dif_sgl_init(&sgl, iovs, iovcnt);

	if (!_dif_sgl_is_valid(&sgl, ctx->block_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		return 0;
	}

	if (_dif_sgl_is_bytes_multiple(&sgl, ctx->block_size)) {
		return dif_verify(&sgl, num_blocks, ctx, err_blk);
	} else {
		return dif_verify_split(&sgl, num_blocks, ctx, err_blk);
	}
}

static uint32_t
dif_update_crc32c(struct _dif_sgl *sgl, uint32_t num_blocks,
		  uint32_t crc32c,  const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;
	uint8_t *buf;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		_dif_sgl_get_buf(sgl, &buf, NULL);

		crc32c = spdk_crc32c_update(buf, ctx->block_size - ctx->md_size, crc32c);

		_dif_sgl_advance(sgl, ctx->block_size);
	}

	return crc32c;
}

static uint32_t
_dif_update_crc32c_split(struct _dif_sgl *sgl, uint32_t offset_in_block, uint32_t data_len,
			 uint32_t crc32c, const struct spdk_dif_ctx *ctx)
{
	uint32_t data_block_size, buf_len;
	uint8_t *buf;

	data_block_size = ctx->block_size - ctx->md_size;

	assert(offset_in_block + data_len <= ctx->block_size);

	while (data_len != 0) {
		_dif_sgl_get_buf(sgl, &buf, &buf_len);
		buf_len = spdk_min(buf_len, data_len);

		if (offset_in_block < data_block_size) {
			buf_len = spdk_min(buf_len, data_block_size - offset_in_block);
			crc32c = spdk_crc32c_update(buf, buf_len, crc32c);
		}

		_dif_sgl_advance(sgl, buf_len);
		offset_in_block += buf_len;
		data_len -= buf_len;
	}

	return crc32c;
}

static uint32_t
dif_update_crc32c_split(struct _dif_sgl *sgl, uint32_t num_blocks,
			uint32_t crc32c, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		crc32c = _dif_update_crc32c_split(sgl, 0, ctx->block_size, crc32c, ctx);
	}

	return crc32c;
}

int
spdk_dif_update_crc32c(struct iovec *iovs, int iovcnt, uint32_t num_blocks,
		       uint32_t *_crc32c, const struct spdk_dif_ctx *ctx)
{
	struct _dif_sgl sgl;

	if (_crc32c == NULL) {
		return -EINVAL;
	}

	_dif_sgl_init(&sgl, iovs, iovcnt);

	if (!_dif_sgl_is_valid(&sgl, ctx->block_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		return -EINVAL;
	}

	if (_dif_sgl_is_bytes_multiple(&sgl, ctx->block_size)) {
		*_crc32c = dif_update_crc32c(&sgl, num_blocks, *_crc32c, ctx);
	} else {
		*_crc32c = dif_update_crc32c_split(&sgl, num_blocks, *_crc32c, ctx);
	}

	return 0;
}

static void
dif_generate_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		  uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks = 0, data_block_size;
	uint8_t *src, *dst;
	uint64_t guard;

	data_block_size = ctx->block_size - ctx->md_size;

	while (offset_blocks < num_blocks) {
		_dif_sgl_get_buf(src_sgl, &src, NULL);
		_dif_sgl_get_buf(dst_sgl, &dst, NULL);

		guard = 0;
		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			guard = _dif_generate_guard_copy(ctx->guard_seed, dst, src, data_block_size,
							 ctx->dif_pi_format);
			guard = _dif_generate_guard(guard, dst + data_block_size,
						    ctx->guard_interval - data_block_size, ctx->dif_pi_format);
		} else {
			memcpy(dst, src, data_block_size);
		}

		_dif_generate(dst + ctx->guard_interval, guard, offset_blocks, ctx);

		_dif_sgl_advance(src_sgl, data_block_size);
		_dif_sgl_advance(dst_sgl, ctx->block_size);
		offset_blocks++;
	}
}

static void
_dif_generate_copy_split(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
			 uint32_t offset_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_in_block, src_len, data_block_size;
	uint8_t *src, *dst;
	uint64_t guard = 0;

	_dif_sgl_get_buf(dst_sgl, &dst, NULL);

	data_block_size = ctx->block_size - ctx->md_size;

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->guard_seed;
	}
	offset_in_block = 0;

	while (offset_in_block < data_block_size) {
		/* Compute CRC over split logical block data and copy
		 * data to bounce buffer.
		 */
		_dif_sgl_get_buf(src_sgl, &src, &src_len);
		src_len = spdk_min(src_len, data_block_size - offset_in_block);

		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			guard = _dif_generate_guard_copy(guard, dst + offset_in_block,
							 src, src_len, ctx->dif_pi_format);
		} else {
			memcpy(dst + offset_in_block, src, src_len);
		}

		_dif_sgl_advance(src_sgl, src_len);
		offset_in_block += src_len;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = _dif_generate_guard(guard, dst + data_block_size,
					    ctx->guard_interval - data_block_size, ctx->dif_pi_format);
	}

	_dif_sgl_advance(dst_sgl, ctx->block_size);

	_dif_generate(dst + ctx->guard_interval, guard, offset_blocks, ctx);
}

static void
dif_generate_copy_split(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
			uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		_dif_generate_copy_split(src_sgl, dst_sgl, offset_blocks, ctx);
	}
}

int
spdk_dif_generate_copy(struct iovec *iovs, int iovcnt, struct iovec *bounce_iovs,
		       int bounce_iovcnt, uint32_t num_blocks,
		       const struct spdk_dif_ctx *ctx)
{
	struct _dif_sgl src_sgl, dst_sgl;
	uint32_t data_block_size;

	_dif_sgl_init(&src_sgl, iovs, iovcnt);
	_dif_sgl_init(&dst_sgl, bounce_iovs, bounce_iovcnt);

	data_block_size = ctx->block_size - ctx->md_size;

	if (!_dif_sgl_is_valid(&src_sgl, data_block_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec arrays are not valid.\n");
		return -EINVAL;
	}

	if (!_dif_sgl_is_valid_block_aligned(&dst_sgl, num_blocks, ctx->block_size)) {
		SPDK_ERRLOG("Size of bounce_iovs arrays are not valid or misaligned with block_size.\n");
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		return 0;
	}

	if (_dif_sgl_is_bytes_multiple(&src_sgl, data_block_size)) {
		dif_generate_copy(&src_sgl, &dst_sgl, num_blocks, ctx);
	} else {
		dif_generate_copy_split(&src_sgl, &dst_sgl, num_blocks, ctx);
	}

	return 0;
}

static int
dif_verify_copy(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
		struct spdk_dif_error *err_blk)
{
	uint32_t offset_blocks = 0, data_block_size;
	uint8_t *src, *dst;
	int rc;
	uint64_t guard;

	data_block_size = ctx->block_size - ctx->md_size;

	while (offset_blocks < num_blocks) {
		_dif_sgl_get_buf(src_sgl, &src, NULL);
		_dif_sgl_get_buf(dst_sgl, &dst, NULL);

		guard = 0;
		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			guard = _dif_generate_guard_copy(ctx->guard_seed, dst, src, data_block_size,
							 ctx->dif_pi_format);
			guard = _dif_generate_guard(guard, src + data_block_size,
						    ctx->guard_interval - data_block_size, ctx->dif_pi_format);
		} else {
			memcpy(dst, src, data_block_size);
		}

		rc = _dif_verify(src + ctx->guard_interval, guard, offset_blocks, ctx, err_blk);
		if (rc != 0) {
			return rc;
		}

		_dif_sgl_advance(src_sgl, ctx->block_size);
		_dif_sgl_advance(dst_sgl, data_block_size);
		offset_blocks++;
	}

	return 0;
}

static int
_dif_verify_copy_split(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		       uint32_t offset_blocks, const struct spdk_dif_ctx *ctx,
		       struct spdk_dif_error *err_blk)
{
	uint32_t offset_in_block, dst_len, data_block_size;
	uint8_t *src, *dst;
	uint64_t guard = 0;

	_dif_sgl_get_buf(src_sgl, &src, NULL);

	data_block_size = ctx->block_size - ctx->md_size;

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->guard_seed;
	}
	offset_in_block = 0;

	while (offset_in_block < data_block_size) {
		/* Compute CRC over split logical block data and copy
		 * data to bounce buffer.
		 */
		_dif_sgl_get_buf(dst_sgl, &dst, &dst_len);
		dst_len = spdk_min(dst_len, data_block_size - offset_in_block);

		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			guard = _dif_generate_guard_copy(guard, dst, src + offset_in_block,
							 dst_len, ctx->dif_pi_format);
		} else {
			memcpy(dst, src + offset_in_block, dst_len);
		}

		_dif_sgl_advance(dst_sgl, dst_len);
		offset_in_block += dst_len;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = _dif_generate_guard(guard, src + data_block_size,
					    ctx->guard_interval - data_block_size, ctx->dif_pi_format);
	}

	_dif_sgl_advance(src_sgl, ctx->block_size);

	return _dif_verify(src + ctx->guard_interval, guard, offset_blocks, ctx, err_blk);
}

static int
dif_verify_copy_split(struct _dif_sgl *src_sgl, struct _dif_sgl *dst_sgl,
		      uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
		      struct spdk_dif_error *err_blk)
{
	uint32_t offset_blocks;
	int rc;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		rc = _dif_verify_copy_split(src_sgl, dst_sgl, offset_blocks, ctx, err_blk);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

int
spdk_dif_verify_copy(struct iovec *iovs, int iovcnt, struct iovec *bounce_iovs,
		     int bounce_iovcnt, uint32_t num_blocks,
		     const struct spdk_dif_ctx *ctx,
		     struct spdk_dif_error *err_blk)
{
	struct _dif_sgl src_sgl, dst_sgl;
	uint32_t data_block_size;

	_dif_sgl_init(&src_sgl, bounce_iovs, bounce_iovcnt);
	_dif_sgl_init(&dst_sgl, iovs, iovcnt);

	data_block_size = ctx->block_size - ctx->md_size;

	if (!_dif_sgl_is_valid(&dst_sgl, data_block_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec arrays are not valid\n");
		return -EINVAL;
	}

	if (!_dif_sgl_is_valid_block_aligned(&src_sgl, num_blocks, ctx->block_size)) {
		SPDK_ERRLOG("Size of bounce_iovs arrays are not valid or misaligned with block_size.\n");
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		return 0;
	}

	if (_dif_sgl_is_bytes_multiple(&dst_sgl, data_block_size)) {
		return dif_verify_copy(&src_sgl, &dst_sgl, num_blocks, ctx, err_blk);
	} else {
		return dif_verify_copy_split(&src_sgl, &dst_sgl, num_blocks, ctx, err_blk);
	}
}

static void
_bit_flip(uint8_t *buf, uint32_t flip_bit)
{
	uint8_t byte;

	byte = *buf;
	byte ^= 1 << flip_bit;
	*buf = byte;
}

static int
_dif_inject_error(struct _dif_sgl *sgl,
		  uint32_t block_size, uint32_t num_blocks,
		  uint32_t inject_offset_blocks,
		  uint32_t inject_offset_bytes,
		  uint32_t inject_offset_bits)
{
	uint32_t offset_in_block, buf_len;
	uint8_t *buf;

	_dif_sgl_advance(sgl, block_size * inject_offset_blocks);

	offset_in_block = 0;

	while (offset_in_block < block_size) {
		_dif_sgl_get_buf(sgl, &buf, &buf_len);
		buf_len = spdk_min(buf_len, block_size - offset_in_block);

		if (inject_offset_bytes >= offset_in_block &&
		    inject_offset_bytes < offset_in_block + buf_len) {
			buf += inject_offset_bytes - offset_in_block;
			_bit_flip(buf, inject_offset_bits);
			return 0;
		}

		_dif_sgl_advance(sgl, buf_len);
		offset_in_block += buf_len;
	}

	return -1;
}

static int
dif_inject_error(struct _dif_sgl *sgl, uint32_t block_size, uint32_t num_blocks,
		 uint32_t start_inject_bytes, uint32_t inject_range_bytes,
		 uint32_t *inject_offset)
{
	uint32_t inject_offset_blocks, inject_offset_bytes, inject_offset_bits;
	uint32_t offset_blocks;
	int rc;

	srand(time(0));

	inject_offset_blocks = rand() % num_blocks;
	inject_offset_bytes = start_inject_bytes + (rand() % inject_range_bytes);
	inject_offset_bits = rand() % 8;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		if (offset_blocks == inject_offset_blocks) {
			rc = _dif_inject_error(sgl, block_size, num_blocks,
					       inject_offset_blocks,
					       inject_offset_bytes,
					       inject_offset_bits);
			if (rc == 0) {
				*inject_offset = inject_offset_blocks;
			}
			return rc;
		}
	}

	return -1;
}

int
spdk_dif_inject_error(struct iovec *iovs, int iovcnt, uint32_t num_blocks,
		      const struct spdk_dif_ctx *ctx, uint32_t inject_flags,
		      uint32_t *inject_offset)
{
	struct _dif_sgl sgl;
	int rc;

	_dif_sgl_init(&sgl, iovs, iovcnt);

	if (!_dif_sgl_is_valid(&sgl, ctx->block_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		return -EINVAL;
	}

	if (inject_flags & SPDK_DIF_REFTAG_ERROR) {
		rc = dif_inject_error(&sgl, ctx->block_size, num_blocks,
				      ctx->guard_interval + _dif_reftag_offset(ctx->dif_pi_format),
				      _dif_reftag_size(ctx->dif_pi_format),
				      inject_offset);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to Reference Tag.\n");
			return rc;
		}
	}

	if (inject_flags & SPDK_DIF_APPTAG_ERROR) {
		rc = dif_inject_error(&sgl, ctx->block_size, num_blocks,
				      ctx->guard_interval + _dif_apptag_offset(ctx->dif_pi_format),
				      _dif_apptag_size(),
				      inject_offset);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to Application Tag.\n");
			return rc;
		}
	}
	if (inject_flags & SPDK_DIF_GUARD_ERROR) {
		rc = dif_inject_error(&sgl, ctx->block_size, num_blocks,
				      ctx->guard_interval,
				      _dif_guard_size(ctx->dif_pi_format),
				      inject_offset);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to Guard.\n");
			return rc;
		}
	}

	if (inject_flags & SPDK_DIF_DATA_ERROR) {
		/* If the DIF information is contained within the last 8/16 bytes of
		 * metadata (depending on the PI format), then the CRC covers all metadata
		 * bytes up to but excluding the last 8/16 bytes. But error injection does not
		 * cover these metadata because classification is not determined yet.
		 *
		 * Note: Error injection to data block is expected to be detected as
		 * guard error.
		 */
		rc = dif_inject_error(&sgl, ctx->block_size, num_blocks,
				      0,
				      ctx->block_size - ctx->md_size,
				      inject_offset);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to data block.\n");
			return rc;
		}
	}

	return 0;
}

static void
dix_generate(struct _dif_sgl *data_sgl, struct _dif_sgl *md_sgl,
	     uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks = 0;
	uint8_t *data_buf, *md_buf;
	uint64_t guard;

	while (offset_blocks < num_blocks) {
		_dif_sgl_get_buf(data_sgl, &data_buf, NULL);
		_dif_sgl_get_buf(md_sgl, &md_buf, NULL);

		guard = 0;
		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			guard = _dif_generate_guard(ctx->guard_seed, data_buf, ctx->block_size,
						    ctx->dif_pi_format);
			guard = _dif_generate_guard(guard, md_buf, ctx->guard_interval,
						    ctx->dif_pi_format);
		}

		_dif_generate(md_buf + ctx->guard_interval, guard, offset_blocks, ctx);

		_dif_sgl_advance(data_sgl, ctx->block_size);
		_dif_sgl_advance(md_sgl, ctx->md_size);
		offset_blocks++;
	}
}

static void
_dix_generate_split(struct _dif_sgl *data_sgl, struct _dif_sgl *md_sgl,
		    uint32_t offset_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_in_block, data_buf_len;
	uint8_t *data_buf, *md_buf;
	uint64_t guard = 0;

	_dif_sgl_get_buf(md_sgl, &md_buf, NULL);

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->guard_seed;
	}
	offset_in_block = 0;

	while (offset_in_block < ctx->block_size) {
		_dif_sgl_get_buf(data_sgl, &data_buf, &data_buf_len);
		data_buf_len = spdk_min(data_buf_len, ctx->block_size - offset_in_block);

		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			guard = _dif_generate_guard(guard, data_buf, data_buf_len,
						    ctx->dif_pi_format);
		}

		_dif_sgl_advance(data_sgl, data_buf_len);
		offset_in_block += data_buf_len;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = _dif_generate_guard(guard, md_buf, ctx->guard_interval,
					    ctx->dif_pi_format);
	}

	_dif_sgl_advance(md_sgl, ctx->md_size);

	_dif_generate(md_buf + ctx->guard_interval, guard, offset_blocks, ctx);
}

static void
dix_generate_split(struct _dif_sgl *data_sgl, struct _dif_sgl *md_sgl,
		   uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	uint32_t offset_blocks;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		_dix_generate_split(data_sgl, md_sgl, offset_blocks, ctx);
	}
}

int
spdk_dix_generate(struct iovec *iovs, int iovcnt, struct iovec *md_iov,
		  uint32_t num_blocks, const struct spdk_dif_ctx *ctx)
{
	struct _dif_sgl data_sgl, md_sgl;

	_dif_sgl_init(&data_sgl, iovs, iovcnt);
	_dif_sgl_init(&md_sgl, md_iov, 1);

	if (!_dif_sgl_is_valid(&data_sgl, ctx->block_size * num_blocks) ||
	    !_dif_sgl_is_valid(&md_sgl, ctx->md_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		return 0;
	}

	if (_dif_sgl_is_bytes_multiple(&data_sgl, ctx->block_size)) {
		dix_generate(&data_sgl, &md_sgl, num_blocks, ctx);
	} else {
		dix_generate_split(&data_sgl, &md_sgl, num_blocks, ctx);
	}

	return 0;
}

static int
dix_verify(struct _dif_sgl *data_sgl, struct _dif_sgl *md_sgl,
	   uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
	   struct spdk_dif_error *err_blk)
{
	uint32_t offset_blocks = 0;
	uint8_t *data_buf, *md_buf;
	uint64_t guard;
	int rc;

	while (offset_blocks < num_blocks) {
		_dif_sgl_get_buf(data_sgl, &data_buf, NULL);
		_dif_sgl_get_buf(md_sgl, &md_buf, NULL);

		guard = 0;
		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			guard = _dif_generate_guard(ctx->guard_seed, data_buf, ctx->block_size,
						    ctx->dif_pi_format);
			guard = _dif_generate_guard(guard, md_buf, ctx->guard_interval,
						    ctx->dif_pi_format);
		}

		rc = _dif_verify(md_buf + ctx->guard_interval, guard, offset_blocks, ctx, err_blk);
		if (rc != 0) {
			return rc;
		}

		_dif_sgl_advance(data_sgl, ctx->block_size);
		_dif_sgl_advance(md_sgl, ctx->md_size);
		offset_blocks++;
	}

	return 0;
}

static int
_dix_verify_split(struct _dif_sgl *data_sgl, struct _dif_sgl *md_sgl,
		  uint32_t offset_blocks, const struct spdk_dif_ctx *ctx,
		  struct spdk_dif_error *err_blk)
{
	uint32_t offset_in_block, data_buf_len;
	uint8_t *data_buf, *md_buf;
	uint64_t guard = 0;

	_dif_sgl_get_buf(md_sgl, &md_buf, NULL);

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->guard_seed;
	}
	offset_in_block = 0;

	while (offset_in_block < ctx->block_size) {
		_dif_sgl_get_buf(data_sgl, &data_buf, &data_buf_len);
		data_buf_len = spdk_min(data_buf_len, ctx->block_size - offset_in_block);

		if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
			guard = _dif_generate_guard(guard, data_buf, data_buf_len,
						    ctx->dif_pi_format);
		}

		_dif_sgl_advance(data_sgl, data_buf_len);
		offset_in_block += data_buf_len;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = _dif_generate_guard(guard, md_buf, ctx->guard_interval,
					    ctx->dif_pi_format);
	}

	_dif_sgl_advance(md_sgl, ctx->md_size);

	return _dif_verify(md_buf + ctx->guard_interval, guard, offset_blocks, ctx, err_blk);
}

static int
dix_verify_split(struct _dif_sgl *data_sgl, struct _dif_sgl *md_sgl,
		 uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
		 struct spdk_dif_error *err_blk)
{
	uint32_t offset_blocks;
	int rc;

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		rc = _dix_verify_split(data_sgl, md_sgl, offset_blocks, ctx, err_blk);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

int
spdk_dix_verify(struct iovec *iovs, int iovcnt, struct iovec *md_iov,
		uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
		struct spdk_dif_error *err_blk)
{
	struct _dif_sgl data_sgl, md_sgl;

	if (md_iov->iov_base == NULL) {
		SPDK_ERRLOG("Metadata buffer is NULL.\n");
		return -EINVAL;
	}

	_dif_sgl_init(&data_sgl, iovs, iovcnt);
	_dif_sgl_init(&md_sgl, md_iov, 1);

	if (!_dif_sgl_is_valid(&data_sgl, ctx->block_size * num_blocks) ||
	    !_dif_sgl_is_valid(&md_sgl, ctx->md_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		return 0;
	}

	if (_dif_sgl_is_bytes_multiple(&data_sgl, ctx->block_size)) {
		return dix_verify(&data_sgl, &md_sgl, num_blocks, ctx, err_blk);
	} else {
		return dix_verify_split(&data_sgl, &md_sgl, num_blocks, ctx, err_blk);
	}
}

int
spdk_dix_inject_error(struct iovec *iovs, int iovcnt, struct iovec *md_iov,
		      uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
		      uint32_t inject_flags, uint32_t *inject_offset)
{
	struct _dif_sgl data_sgl, md_sgl;
	int rc;

	_dif_sgl_init(&data_sgl, iovs, iovcnt);
	_dif_sgl_init(&md_sgl, md_iov, 1);

	if (!_dif_sgl_is_valid(&data_sgl, ctx->block_size * num_blocks) ||
	    !_dif_sgl_is_valid(&md_sgl, ctx->md_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		return -EINVAL;
	}

	if (inject_flags & SPDK_DIF_REFTAG_ERROR) {
		rc = dif_inject_error(&md_sgl, ctx->md_size, num_blocks,
				      ctx->guard_interval + _dif_reftag_offset(ctx->dif_pi_format),
				      _dif_reftag_size(ctx->dif_pi_format),
				      inject_offset);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to Reference Tag.\n");
			return rc;
		}
	}

	if (inject_flags & SPDK_DIF_APPTAG_ERROR) {
		rc = dif_inject_error(&md_sgl, ctx->md_size, num_blocks,
				      ctx->guard_interval + _dif_apptag_offset(ctx->dif_pi_format),
				      _dif_apptag_size(),
				      inject_offset);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to Application Tag.\n");
			return rc;
		}
	}

	if (inject_flags & SPDK_DIF_GUARD_ERROR) {
		rc = dif_inject_error(&md_sgl, ctx->md_size, num_blocks,
				      ctx->guard_interval,
				      _dif_guard_size(ctx->dif_pi_format),
				      inject_offset);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to Guard.\n");
			return rc;
		}
	}

	if (inject_flags & SPDK_DIF_DATA_ERROR) {
		/* Note: Error injection to data block is expected to be detected
		 * as guard error.
		 */
		rc = dif_inject_error(&data_sgl, ctx->block_size, num_blocks,
				      0,
				      ctx->block_size,
				      inject_offset);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to Guard.\n");
			return rc;
		}
	}

	return 0;
}

static uint32_t
_to_next_boundary(uint32_t offset, uint32_t boundary)
{
	return boundary - (offset % boundary);
}

static uint32_t
_to_size_with_md(uint32_t size, uint32_t data_block_size, uint32_t block_size)
{
	return (size / data_block_size) * block_size + (size % data_block_size);
}

int
spdk_dif_set_md_interleave_iovs(struct iovec *iovs, int iovcnt,
				struct iovec *buf_iovs, int buf_iovcnt,
				uint32_t data_offset, uint32_t data_len,
				uint32_t *_mapped_len,
				const struct spdk_dif_ctx *ctx)
{
	uint32_t data_block_size, data_unalign, buf_len, buf_offset, len;
	struct _dif_sgl dif_sgl;
	struct _dif_sgl buf_sgl;

	if (iovs == NULL || iovcnt == 0 || buf_iovs == NULL || buf_iovcnt == 0) {
		return -EINVAL;
	}

	data_block_size = ctx->block_size - ctx->md_size;

	data_unalign = ctx->data_offset % data_block_size;

	buf_len = _to_size_with_md(data_unalign + data_offset + data_len, data_block_size,
				   ctx->block_size);
	buf_len -= data_unalign;

	_dif_sgl_init(&dif_sgl, iovs, iovcnt);
	_dif_sgl_init(&buf_sgl, buf_iovs, buf_iovcnt);

	if (!_dif_sgl_is_valid(&buf_sgl, buf_len)) {
		SPDK_ERRLOG("Buffer overflow will occur.\n");
		return -ERANGE;
	}

	buf_offset = _to_size_with_md(data_unalign + data_offset, data_block_size, ctx->block_size);
	buf_offset -= data_unalign;

	_dif_sgl_advance(&buf_sgl, buf_offset);

	while (data_len != 0) {
		len = spdk_min(data_len, _to_next_boundary(ctx->data_offset + data_offset, data_block_size));
		if (!_dif_sgl_append_split(&dif_sgl, &buf_sgl, len)) {
			break;
		}
		_dif_sgl_advance(&buf_sgl, ctx->md_size);
		data_offset += len;
		data_len -= len;
	}

	if (_mapped_len != NULL) {
		*_mapped_len = dif_sgl.total_size;
	}

	return iovcnt - dif_sgl.iovcnt;
}

static int
_dif_sgl_setup_stream(struct _dif_sgl *sgl, uint32_t *_buf_offset, uint32_t *_buf_len,
		      uint32_t data_offset, uint32_t data_len,
		      const struct spdk_dif_ctx *ctx)
{
	uint32_t data_block_size, data_unalign, buf_len, buf_offset;

	data_block_size = ctx->block_size - ctx->md_size;

	data_unalign = ctx->data_offset % data_block_size;

	/* If the last data block is complete, DIF of the data block is
	 * inserted or verified in this turn.
	 */
	buf_len = _to_size_with_md(data_unalign + data_offset + data_len, data_block_size,
				   ctx->block_size);
	buf_len -= data_unalign;

	if (!_dif_sgl_is_valid(sgl, buf_len)) {
		return -ERANGE;
	}

	buf_offset = _to_size_with_md(data_unalign + data_offset, data_block_size, ctx->block_size);
	buf_offset -= data_unalign;

	_dif_sgl_advance(sgl, buf_offset);
	buf_len -= buf_offset;

	buf_offset += data_unalign;

	*_buf_offset = buf_offset;
	*_buf_len = buf_len;

	return 0;
}

int
spdk_dif_generate_stream(struct iovec *iovs, int iovcnt,
			 uint32_t data_offset, uint32_t data_len,
			 struct spdk_dif_ctx *ctx)
{
	uint32_t buf_len = 0, buf_offset = 0;
	uint32_t len, offset_in_block, offset_blocks;
	uint64_t guard = 0;
	struct _dif_sgl sgl;
	int rc;

	if (iovs == NULL || iovcnt == 0) {
		return -EINVAL;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->last_guard;
	}

	_dif_sgl_init(&sgl, iovs, iovcnt);

	rc = _dif_sgl_setup_stream(&sgl, &buf_offset, &buf_len, data_offset, data_len, ctx);
	if (rc != 0) {
		return rc;
	}

	while (buf_len != 0) {
		len = spdk_min(buf_len, _to_next_boundary(buf_offset, ctx->block_size));
		offset_in_block = buf_offset % ctx->block_size;
		offset_blocks = buf_offset / ctx->block_size;

		guard = _dif_generate_split(&sgl, offset_in_block, len, guard, offset_blocks, ctx);

		buf_len -= len;
		buf_offset += len;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		ctx->last_guard = guard;
	}

	return 0;
}

int
spdk_dif_verify_stream(struct iovec *iovs, int iovcnt,
		       uint32_t data_offset, uint32_t data_len,
		       struct spdk_dif_ctx *ctx,
		       struct spdk_dif_error *err_blk)
{
	uint32_t buf_len = 0, buf_offset = 0;
	uint32_t len, offset_in_block, offset_blocks;
	uint64_t guard = 0;
	struct _dif_sgl sgl;
	int rc = 0;

	if (iovs == NULL || iovcnt == 0) {
		return -EINVAL;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = ctx->last_guard;
	}

	_dif_sgl_init(&sgl, iovs, iovcnt);

	rc = _dif_sgl_setup_stream(&sgl, &buf_offset, &buf_len, data_offset, data_len, ctx);
	if (rc != 0) {
		return rc;
	}

	while (buf_len != 0) {
		len = spdk_min(buf_len, _to_next_boundary(buf_offset, ctx->block_size));
		offset_in_block = buf_offset % ctx->block_size;
		offset_blocks = buf_offset / ctx->block_size;

		rc = _dif_verify_split(&sgl, offset_in_block, len, &guard, offset_blocks,
				       ctx, err_blk);
		if (rc != 0) {
			goto error;
		}

		buf_len -= len;
		buf_offset += len;
	}

	if (ctx->dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		ctx->last_guard = guard;
	}
error:
	return rc;
}

int
spdk_dif_update_crc32c_stream(struct iovec *iovs, int iovcnt,
			      uint32_t data_offset, uint32_t data_len,
			      uint32_t *_crc32c, const struct spdk_dif_ctx *ctx)
{
	uint32_t buf_len = 0, buf_offset = 0, len, offset_in_block;
	uint32_t crc32c;
	struct _dif_sgl sgl;
	int rc;

	if (iovs == NULL || iovcnt == 0) {
		return -EINVAL;
	}

	crc32c = *_crc32c;
	_dif_sgl_init(&sgl, iovs, iovcnt);

	rc = _dif_sgl_setup_stream(&sgl, &buf_offset, &buf_len, data_offset, data_len, ctx);
	if (rc != 0) {
		return rc;
	}

	while (buf_len != 0) {
		len = spdk_min(buf_len, _to_next_boundary(buf_offset, ctx->block_size));
		offset_in_block = buf_offset % ctx->block_size;

		crc32c = _dif_update_crc32c_split(&sgl, offset_in_block, len, crc32c, ctx);

		buf_len -= len;
		buf_offset += len;
	}

	*_crc32c = crc32c;

	return 0;
}

void
spdk_dif_get_range_with_md(uint32_t data_offset, uint32_t data_len,
			   uint32_t *_buf_offset, uint32_t *_buf_len,
			   const struct spdk_dif_ctx *ctx)
{
	uint32_t data_block_size, data_unalign, buf_offset, buf_len;

	if (!ctx->md_interleave) {
		buf_offset = data_offset;
		buf_len = data_len;
	} else {
		data_block_size = ctx->block_size - ctx->md_size;

		data_unalign = data_offset % data_block_size;

		buf_offset = _to_size_with_md(data_offset, data_block_size, ctx->block_size);
		buf_len = _to_size_with_md(data_unalign + data_len, data_block_size, ctx->block_size) -
			  data_unalign;
	}

	if (_buf_offset != NULL) {
		*_buf_offset = buf_offset;
	}

	if (_buf_len != NULL) {
		*_buf_len = buf_len;
	}
}

uint32_t
spdk_dif_get_length_with_md(uint32_t data_len, const struct spdk_dif_ctx *ctx)
{
	uint32_t data_block_size;

	if (!ctx->md_interleave) {
		return data_len;
	} else {
		data_block_size = ctx->block_size - ctx->md_size;

		return _to_size_with_md(data_len, data_block_size, ctx->block_size);
	}
}

static int
_dif_remap_ref_tag(struct _dif_sgl *sgl, uint32_t offset_blocks,
		   const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk,
		   bool check_ref_tag)
{
	uint32_t offset, buf_len;
	uint64_t expected = 0, remapped;
	uint8_t *buf;
	struct _dif_sgl tmp_sgl;
	struct spdk_dif dif;

	/* Fast forward to DIF field. */
	_dif_sgl_advance(sgl, ctx->guard_interval);
	_dif_sgl_copy(&tmp_sgl, sgl);

	/* Copy the split DIF field to the temporary DIF buffer */
	offset = 0;
	while (offset < _dif_size(ctx->dif_pi_format)) {
		_dif_sgl_get_buf(sgl, &buf, &buf_len);
		buf_len = spdk_min(buf_len, _dif_size(ctx->dif_pi_format) - offset);

		memcpy((uint8_t *)&dif + offset, buf, buf_len);

		_dif_sgl_advance(sgl, buf_len);
		offset += buf_len;
	}

	if (_dif_ignore(&dif, ctx)) {
		goto end;
	}

	/* For type 1 and 2, the Reference Tag is incremented for each
	 * subsequent logical block. For type 3, the Reference Tag
	 * remains the same as the initial Reference Tag.
	 */
	if (ctx->dif_type != SPDK_DIF_TYPE3) {
		expected = ctx->init_ref_tag + ctx->ref_tag_offset + offset_blocks;
		remapped = ctx->remapped_init_ref_tag + ctx->ref_tag_offset + offset_blocks;
	} else {
		remapped = ctx->remapped_init_ref_tag;
	}

	/* Verify the stored Reference Tag. */
	if (check_ref_tag && !_dif_reftag_check(&dif, ctx, expected, offset_blocks, err_blk)) {
		return -1;
	}

	/* Update the stored Reference Tag to the remapped one. */
	_dif_set_reftag(&dif, remapped, ctx->dif_pi_format);

	offset = 0;
	while (offset < _dif_size(ctx->dif_pi_format)) {
		_dif_sgl_get_buf(&tmp_sgl, &buf, &buf_len);
		buf_len = spdk_min(buf_len, _dif_size(ctx->dif_pi_format) - offset);

		memcpy(buf, (uint8_t *)&dif + offset, buf_len);

		_dif_sgl_advance(&tmp_sgl, buf_len);
		offset += buf_len;
	}

end:
	_dif_sgl_advance(sgl, ctx->block_size - ctx->guard_interval - _dif_size(ctx->dif_pi_format));

	return 0;
}

int
spdk_dif_remap_ref_tag(struct iovec *iovs, int iovcnt, uint32_t num_blocks,
		       const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk,
		       bool check_ref_tag)
{
	struct _dif_sgl sgl;
	uint32_t offset_blocks;
	int rc;

	_dif_sgl_init(&sgl, iovs, iovcnt);

	if (!_dif_sgl_is_valid(&sgl, ctx->block_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		return 0;
	}

	if (!(ctx->dif_flags & SPDK_DIF_FLAGS_REFTAG_CHECK)) {
		return 0;
	}

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		rc = _dif_remap_ref_tag(&sgl, offset_blocks, ctx, err_blk, check_ref_tag);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

static int
_dix_remap_ref_tag(struct _dif_sgl *md_sgl, uint32_t offset_blocks,
		   const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err_blk,
		   bool check_ref_tag)
{
	uint64_t expected = 0, remapped;
	uint8_t *md_buf;
	struct spdk_dif *dif;

	_dif_sgl_get_buf(md_sgl, &md_buf, NULL);

	dif = (struct spdk_dif *)(md_buf + ctx->guard_interval);

	if (_dif_ignore(dif, ctx)) {
		goto end;
	}

	/* For type 1 and 2, the Reference Tag is incremented for each
	 * subsequent logical block. For type 3, the Reference Tag
	 * remains the same as the initialReference Tag.
	 */
	if (ctx->dif_type != SPDK_DIF_TYPE3) {
		expected = ctx->init_ref_tag + ctx->ref_tag_offset + offset_blocks;
		remapped = ctx->remapped_init_ref_tag + ctx->ref_tag_offset + offset_blocks;
	} else {
		remapped = ctx->remapped_init_ref_tag;
	}

	/* Verify the stored Reference Tag. */
	if (check_ref_tag && !_dif_reftag_check(dif, ctx, expected, offset_blocks, err_blk)) {
		return -1;
	}

	/* Update the stored Reference Tag to the remapped one. */
	_dif_set_reftag(dif, remapped, ctx->dif_pi_format);

end:
	_dif_sgl_advance(md_sgl, ctx->md_size);

	return 0;
}

int
spdk_dix_remap_ref_tag(struct iovec *md_iov, uint32_t num_blocks,
		       const struct spdk_dif_ctx *ctx,
		       struct spdk_dif_error *err_blk,
		       bool check_ref_tag)
{
	struct _dif_sgl md_sgl;
	uint32_t offset_blocks;
	int rc;

	_dif_sgl_init(&md_sgl, md_iov, 1);

	if (!_dif_sgl_is_valid(&md_sgl, ctx->md_size * num_blocks)) {
		SPDK_ERRLOG("Size of metadata iovec array is not valid.\n");
		return -EINVAL;
	}

	if (_dif_is_disabled(ctx->dif_type)) {
		return 0;
	}

	if (!(ctx->dif_flags & SPDK_DIF_FLAGS_REFTAG_CHECK)) {
		return 0;
	}

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		rc = _dix_remap_ref_tag(&md_sgl, offset_blocks, ctx, err_blk, check_ref_tag);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}
