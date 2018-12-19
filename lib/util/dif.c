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

#include "spdk/dif.h"
#include "spdk/crc16.h"
#include "spdk/endian.h"
#include "spdk/log.h"
#include "spdk/util.h"

/* Context to iterate a iovec array. */
struct _iov_iter {
	/* Current iovec in the iteration */
	struct iovec *iov;

	/* Remaining count of iovecs in the iteration. */
	int iovcnt;

	/* Current offset in the iovec */
	uint32_t iov_offset;
};

static inline void
_iov_iter_init(struct _iov_iter *i, struct iovec *iovs, int iovcnt)
{
	i->iov = iovs;
	i->iovcnt = iovcnt;
	i->iov_offset = 0;
}

static inline bool
_iov_iter_cont(struct _iov_iter *i)
{
	return i->iovcnt != 0;
}

static inline void
_iov_iter_advance(struct _iov_iter *i, uint32_t step)
{
	i->iov_offset += step;
	if (i->iov_offset == i->iov->iov_len) {
		i->iov++;
		i->iovcnt--;
		i->iov_offset = 0;
	}
}

static inline void
_iov_iter_get_buf(struct _iov_iter *i, void **_buf, uint32_t *_buf_len)
{
	if (_buf != NULL) {
		*_buf = i->iov->iov_base + i->iov_offset;
	}
	if (_buf_len != NULL) {
		*_buf_len = i->iov->iov_len - i->iov_offset;
	}
}

static bool
_are_iovs_bytes_multiple(struct iovec *iovs, int iovcnt, uint32_t bytes)
{
	int i;

	for (i = 0; i < iovcnt; i++) {
		if (iovs[i].iov_len % bytes) {
			return false;
		}
	}

	return true;
}

static bool
_are_iovs_valid(struct iovec *iovs, int iovcnt, uint32_t bytes)
{
	uint64_t total = 0;
	int i;

	for (i = 0; i < iovcnt; i++) {
		total += iovs[i].iov_len;
	}

	return total >= bytes;
}

static bool
_dif_type_is_valid(enum spdk_dif_type dif_type, uint32_t dif_flags)
{
	switch (dif_type) {
	case SPDK_DIF_TYPE1:
	case SPDK_DIF_TYPE2:
		break;
	case SPDK_DIF_TYPE3:
		if (dif_flags & SPDK_DIF_REFTAG_CHECK) {
			SPDK_ERRLOG("Reference Tag should not be checked for Type 3\n");
			return false;
		}
		break;
	default:
		SPDK_ERRLOG("Unknown DIF Type: %d\n", dif_type);
		return false;
	}

	return true;
}

static uint32_t
_get_dif_guard_interval(uint32_t block_size, uint32_t md_size, bool dif_loc)
{
	if (dif_loc) {
		/* For metadata formats with more than 8 bytes, if the DIF is
		 * contained in the last 8 bytes of metadata, then the CRC
		 * covers all metadata up to but excluding these last 8 bytes.
		 */
		return block_size - sizeof(struct spdk_dif);
	} else {
		/* For metadata formats with more than 8 bytes, if the DIF is
		 * contained in the first 8 bytes of metadata, then the CRC
		 * does not cover any metadata.
		 */
		return block_size - md_size;
	}
}

static void
_dif_generate(void *_dif, uint32_t dif_flags,
	      uint16_t guard, uint32_t ref_tag, uint16_t app_tag)
{
	struct spdk_dif *dif = _dif;

	if (dif_flags & SPDK_DIF_GUARD_CHECK) {
		to_be16(&dif->guard, guard);
	}

	if (dif_flags & SPDK_DIF_APPTAG_CHECK) {
		to_be16(&dif->app_tag, app_tag);
	}

	if (dif_flags & SPDK_DIF_REFTAG_CHECK) {
		to_be32(&dif->ref_tag, ref_tag);
	}
}

static void
dif_generate(struct iovec *iovs, int iovcnt,
	     uint32_t block_size, uint32_t guard_interval, uint32_t num_blocks,
	     enum spdk_dif_type dif_type, uint32_t dif_flags,
	     uint32_t init_ref_tag, uint16_t app_tag)
{
	struct _iov_iter iter;
	uint32_t offset_blocks, ref_tag;
	void *buf;
	uint16_t guard = 0;

	offset_blocks = 0;
	_iov_iter_init(&iter, iovs, iovcnt);

	while (offset_blocks < num_blocks && _iov_iter_cont(&iter)) {
		/* For type 1 and 2, the reference tag is incremented for each
		 * subsequent logical block. For type 3, the reference tag
		 * remains the same as the initial reference tag.
		 */
		if (dif_type != SPDK_DIF_TYPE3) {
			ref_tag = init_ref_tag + offset_blocks;
		} else {
			ref_tag = init_ref_tag;
		}

		_iov_iter_get_buf(&iter, &buf, NULL);

		if (dif_flags & SPDK_DIF_GUARD_CHECK) {
			guard = spdk_crc16_t10dif(0, buf, guard_interval);
		}

		_dif_generate(buf + guard_interval, dif_flags, guard, ref_tag,
			      app_tag);

		_iov_iter_advance(&iter, block_size);
		offset_blocks++;
	}
}

static void
dif_generate_split(struct iovec *iovs, int iovcnt,
		   uint32_t block_size, uint32_t guard_interval, uint32_t num_blocks,
		   enum spdk_dif_type dif_type, uint32_t dif_flags,
		   uint32_t init_ref_tag, uint16_t app_tag)
{
	struct _iov_iter iter;
	uint32_t offset_blocks, offset_in_block, offset_in_dif;
	uint32_t buf_len, ref_tag;
	void *buf;
	uint16_t guard;
	struct spdk_dif dif = {};

	offset_blocks = 0;
	_iov_iter_init(&iter, iovs, iovcnt);

	while (offset_blocks < num_blocks && _iov_iter_cont(&iter)) {
		/* For type 1 and 2, the reference tag is incremented for each
		 * subsequent logical block. For type 3, the reference tag
		 * remains the same as the initial reference tag.
		 */
		if (dif_type != SPDK_DIF_TYPE3) {
			ref_tag = init_ref_tag + offset_blocks;
		} else {
			ref_tag = init_ref_tag;
		}

		guard = 0;
		offset_in_block = 0;

		while (offset_in_block < block_size && _iov_iter_cont(&iter)) {
			_iov_iter_get_buf(&iter, &buf, &buf_len);

			if (offset_in_block < guard_interval) {
				buf_len = spdk_min(buf_len, guard_interval - offset_in_block);

				if (dif_flags & SPDK_DIF_GUARD_CHECK) {
					/* Compute CRC over split logical block data. */
					guard = spdk_crc16_t10dif(guard, buf, buf_len);
				}

				if (offset_in_block + buf_len == guard_interval) {
					/* If a whole logical block data is parsed, generate DIF
					 * and save it to the temporary DIF area.
					 */
					_dif_generate(&dif, dif_flags, guard, ref_tag, app_tag);
				}
			} else if (offset_in_block < guard_interval + sizeof(struct spdk_dif)) {
				/* Copy generated DIF to the split DIF field. */
				offset_in_dif = offset_in_block - guard_interval;
				buf_len = spdk_min(buf_len, sizeof(struct spdk_dif) - offset_in_dif);

				memcpy(buf, ((uint8_t *)&dif) + offset_in_dif, buf_len);
			} else {
				/* Skip metadata field after DIF field. */
				buf_len = spdk_min(buf_len, block_size - offset_in_block);
			}

			_iov_iter_advance(&iter, buf_len);
			offset_in_block += buf_len;
		}
		offset_blocks++;
	}
}

int
spdk_dif_generate(struct iovec *iovs, int iovcnt,
		  uint32_t block_size, uint32_t md_size, uint32_t num_blocks,
		  bool dif_loc, enum spdk_dif_type dif_type, uint32_t dif_flags,
		  uint32_t init_ref_tag, uint16_t app_tag)
{
	uint32_t guard_interval;

	if (md_size == 0) {
		return -EINVAL;
	}

	if (!_are_iovs_valid(iovs, iovcnt, block_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		return -EINVAL;
	}

	if (!_dif_type_is_valid(dif_type, dif_flags)) {
		SPDK_ERRLOG("DIF type is invalid.\n");
		return -EINVAL;
	}

	guard_interval = _get_dif_guard_interval(block_size, md_size, dif_loc);

	if (_are_iovs_bytes_multiple(iovs, iovcnt, block_size)) {
		dif_generate(iovs, iovcnt, block_size, guard_interval, num_blocks,
			     dif_type, dif_flags, init_ref_tag, app_tag);
	} else {
		dif_generate_split(iovs, iovcnt, block_size, guard_interval, num_blocks,
				   dif_type, dif_flags, init_ref_tag, app_tag);
	}

	return 0;
}

static int
_dif_verify(void *_dif, enum spdk_dif_type dif_type, uint32_t dif_flags,
	    uint16_t guard, uint32_t ref_tag, uint16_t apptag_mask, uint16_t app_tag)
{
	struct spdk_dif *dif = _dif;
	uint16_t _guard;
	uint16_t _app_tag;
	uint32_t _ref_tag;

	switch (dif_type) {
	case SPDK_DIF_TYPE1:
	case SPDK_DIF_TYPE2:
		/* If Type 1 or 2 is used, then all DIF checks are disabled when
		 * the Application Tag is 0xFFFF.
		 */
		if (dif->app_tag == 0xFFFF) {
			return 0;
		}
		break;
	case SPDK_DIF_TYPE3:
		/* If Type 3 is used, then all DIF checks are disabled when the
		 * Application Tag is 0xFFFF and the Reference Tag is 0xFFFFFFFF.
		 */
		if (dif->app_tag == 0xFFFF && dif->ref_tag == 0xFFFFFFFF) {
			return 0;
		}
	}

	if (dif_flags & SPDK_DIF_GUARD_CHECK) {
		/* Compare the DIF Guard field to the CRC computed over the logical
		 * block data.
		 */
		_guard = from_be16(&dif->guard);
		if (_guard != guard) {
			SPDK_ERRLOG("Failed to compare Guard: LBA=%" PRIu32 "," \
				    "  Expected=%x, Actual=%x\n",
				    ref_tag, _guard, guard);
			return -1;
		}
	}

	if (dif_flags & SPDK_DIF_APPTAG_CHECK) {
		/* Compare unmasked bits in the DIF Application Tag field to the
		 * passed Application Tag.
		 */
		_app_tag = from_be16(&dif->app_tag);
		if ((_app_tag & apptag_mask) != app_tag) {
			SPDK_ERRLOG("Failed to compare App Tag: LBA=%" PRIu32 "," \
				    "  Expected=%x, Actual=%x\n",
				    ref_tag, app_tag, (_app_tag & apptag_mask));
			return -1;
		}
	}

	if (dif_flags & SPDK_DIF_REFTAG_CHECK) {
		switch (dif_type) {
		case SPDK_DIF_TYPE1:
		case SPDK_DIF_TYPE2:
			/* Compare the DIF Reference Tag field to the passed Reference Tag.
			 * The passed Reference Tag will be the least significant 4 bytes
			 * of the LBA when Type 1 is used, and application specific value
			 * if Type 2 is used,
			 */
			_ref_tag = from_be32(&dif->ref_tag);
			if (_ref_tag != ref_tag) {
				SPDK_ERRLOG("Failed to compare Ref Tag: LBA=%" PRIu32 "," \
					    " Expected=%x, Actual=%x\n",
					    ref_tag, ref_tag, _ref_tag);
				return -1;
			}
			break;
		case SPDK_DIF_TYPE3:
			/* For Type 3, computed Reference Tag remains unchanged.
			 * Hence ignore the Reference Tag field.
			 */
			break;
		}
	}

	return 0;
}

static int
dif_verify(struct iovec *iovs, int iovcnt,
	   uint32_t block_size, uint32_t guard_interval, uint32_t num_blocks,
	   enum spdk_dif_type dif_type, uint32_t dif_flags, uint32_t init_ref_tag,
	   uint16_t apptag_mask, uint16_t app_tag)
{
	struct _iov_iter iter;
	uint32_t offset_blocks, ref_tag;
	int rc;
	void *buf;
	uint16_t guard = 0;

	offset_blocks = 0;
	_iov_iter_init(&iter, iovs, iovcnt);

	while (offset_blocks < num_blocks && _iov_iter_cont(&iter)) {
		/* For type 1 and 2, the reference tag is incremented for each
		 * subsequent logical block. For type 3, the reference tag
		 * remains the same as the initial reference tag.
		 */
		if (dif_type != SPDK_DIF_TYPE3) {
			ref_tag = init_ref_tag + offset_blocks;
		} else {
			ref_tag = init_ref_tag;
		}

		_iov_iter_get_buf(&iter, &buf, NULL);

		if (dif_flags & SPDK_DIF_GUARD_CHECK) {
			guard = spdk_crc16_t10dif(0, buf, guard_interval);
		}

		rc = _dif_verify(buf + guard_interval, dif_type, dif_flags,
				 guard, ref_tag, apptag_mask, app_tag);
		if (rc != 0) {
			return rc;
		}

		_iov_iter_advance(&iter, block_size);
		offset_blocks++;
	}

	return 0;
}

static int
dif_verify_split(struct iovec *iovs, int iovcnt,
		 uint32_t block_size, uint32_t guard_interval, uint32_t num_blocks,
		 enum spdk_dif_type dif_type, uint32_t dif_flags,
		 uint32_t init_ref_tag, uint16_t apptag_mask, uint16_t app_tag)
{
	struct _iov_iter iter;
	uint32_t offset_blocks, offset_in_block, offset_in_dif;
	uint32_t buf_len, ref_tag = 0;
	int rc;
	void *buf;
	uint16_t guard;
	struct spdk_dif dif = {};

	offset_blocks = 0;
	_iov_iter_init(&iter, iovs, iovcnt);

	while (offset_blocks < num_blocks && _iov_iter_cont(&iter)) {
		/* For type 1 and 2, the reference tag is incremented for each
		 * subsequent logical block. For type 3, the reference tag
		 * remains the same as the initial reference tag.
		 */
		if (dif_type != SPDK_DIF_TYPE3) {
			ref_tag = init_ref_tag + offset_blocks;
		} else {
			ref_tag = init_ref_tag;
		}

		guard = 0;
		offset_in_block = 0;

		while (offset_in_block < block_size && _iov_iter_cont(&iter)) {
			_iov_iter_get_buf(&iter, &buf, &buf_len);

			if (offset_in_block < guard_interval) {
				buf_len = spdk_min(buf_len, guard_interval - offset_in_block);

				if (dif_flags & SPDK_DIF_GUARD_CHECK) {
					/* Compute CRC over split logical block data. */
					guard = spdk_crc16_t10dif(guard, buf, buf_len);
				}
			} else if (offset_in_block < guard_interval + sizeof(struct spdk_dif)) {
				/* Copy the split DIF field to the temporary DIF buffer. */
				offset_in_dif = offset_in_block - guard_interval;
				buf_len = spdk_min(buf_len, sizeof(struct spdk_dif) - offset_in_dif);

				memcpy((uint8_t *)&dif + offset_in_dif, buf, buf_len);
			} else {
				/* Skip metadata field after DIF field. */
				buf_len = spdk_min(buf_len, block_size - offset_in_block);
			}

			_iov_iter_advance(&iter, buf_len);
			offset_in_block += buf_len;
		}

		rc = _dif_verify(&dif, dif_type, dif_flags, guard, ref_tag, apptag_mask, app_tag);
		if (rc != 0) {
			return 0;
		}

		offset_blocks++;
	}

	return 0;
}

int
spdk_dif_verify(struct iovec *iovs, int iovcnt,
		uint32_t block_size, uint32_t md_size, uint32_t num_blocks,
		bool dif_loc, enum spdk_dif_type dif_type, uint32_t dif_flags,
		uint32_t init_ref_tag, uint16_t apptag_mask, uint16_t app_tag)
{
	uint32_t guard_interval;

	if (md_size == 0) {
		return -EINVAL;
	}

	if (!_are_iovs_valid(iovs, iovcnt, block_size * num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		return -EINVAL;
	}

	if (!_dif_type_is_valid(dif_type, dif_flags)) {
		SPDK_ERRLOG("DIF type is invalid.\n");
		return -EINVAL;
	}

	guard_interval = _get_dif_guard_interval(block_size, md_size, dif_loc);

	if (_are_iovs_bytes_multiple(iovs, iovcnt, block_size)) {
		return dif_verify(iovs, iovcnt, block_size, guard_interval, num_blocks,
				  dif_type, dif_flags, init_ref_tag, apptag_mask, app_tag);
	} else {
		return dif_verify_split(iovs, iovcnt, block_size, guard_interval, num_blocks,
					dif_type, dif_flags, init_ref_tag, apptag_mask, app_tag);
	}
}
