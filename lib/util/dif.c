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

static bool
_are_iovs_block_multiple(struct iovec *iovs, int iovcnt,
			 uint32_t block_size)
{
	int i;

	for (i = 0; i < iovcnt; i++) {
		if (iovs[i].iov_len % block_size) {
			return false;
		}
	}

	return true;
}

static bool
_are_iovs_valid(struct iovec *iovs, int iovcnt, uint32_t block_size,
		uint32_t num_blocks)
{
	uint64_t total = 0;
	int i;

	for (i = 0; i < iovcnt; i++) {
		total += iovs[i].iov_len;
	}

	return total >= (block_size * num_blocks);
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
_get_dif_guard_interval(uint32_t block_size, uint32_t md_size, bool dif_start)
{
	if (dif_start) {
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
dif_generate(struct iovec *iovs,
	     uint32_t block_size, uint32_t guard_interval,
	     uint32_t dif_flags, uint32_t ref_tag, uint16_t app_tag,
	     int *iovpos, uint32_t *iov_offset)
{
	uint16_t guard = 0;
	void *buf;

	buf = iovs[*iovpos].iov_base + *iov_offset;

	if (dif_flags & SPDK_DIF_GUARD_CHECK) {
		guard = spdk_crc16_t10dif(guard, buf, guard_interval);
	}

	_dif_generate(buf + guard_interval, dif_flags, guard, ref_tag, app_tag);

	*iov_offset += block_size;
	if (*iov_offset == iovs[*iovpos].iov_len) {
		(*iovpos)++;
		*iov_offset = 0;
	}
}

static void
dif_generate_split(struct iovec *iovs, int iovcnt,
		   uint32_t block_size, uint32_t guard_interval,
		   uint32_t dif_flags, uint32_t ref_tag, uint16_t app_tag,
		   int *iovpos, uint32_t *iov_offset)
{
	uint32_t offset_in_block = 0, offset_in_dif, buf_len;
	uint16_t guard = 0 ;
	void *buf;
	struct spdk_dif dif = {};

	while (offset_in_block < block_size && *iovpos < iovcnt) {
		buf = iovs[*iovpos].iov_base + *iov_offset;
		buf_len = iovs[*iovpos].iov_len - *iov_offset;

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

		*iov_offset += buf_len;
		if (*iov_offset == iovs[*iovpos].iov_len) {
			(*iovpos)++;
			*iov_offset = 0;
		}
		offset_in_block += buf_len;
	}
}

int
spdk_dif_generate(struct iovec *iovs, int iovcnt,
		  uint32_t block_size, uint32_t md_size, uint32_t num_blocks,
		  bool dif_start, enum spdk_dif_type dif_type, uint32_t dif_flags,
		  uint32_t init_ref_tag, uint16_t app_tag)
{
	uint32_t offset_blocks = 0, iov_offset = 0;
	uint32_t guard_interval, ref_tag;
	int iovpos = 0;
	bool contig;

	if (md_size == 0) {
		return -EINVAL;
	}

	if (!_are_iovs_valid(iovs, iovcnt, block_size, num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		return -EINVAL;
	}

	if (!_dif_type_is_valid(dif_type, dif_flags)) {
		SPDK_ERRLOG("DIF type is invalid.\n");
		return -EINVAL;
	}

	ref_tag = init_ref_tag;
	guard_interval = _get_dif_guard_interval(block_size, md_size, dif_start);
	contig = _are_iovs_block_multiple(iovs, iovcnt, block_size);

	while (offset_blocks < num_blocks && iovpos < iovcnt) {
		if (contig) {
			dif_generate(iovs, block_size, guard_interval,
				     dif_flags, ref_tag, app_tag,
				     &iovpos, &iov_offset);
		} else {
			dif_generate_split(iovs, iovcnt, block_size, guard_interval,
					   dif_flags, ref_tag, app_tag,
					   &iovpos, &iov_offset);
		}

		/* Reference tag check is not enabled for type 3, and just
		 * incrementing ref_tag per block is OK.
		 */
		ref_tag++;

		offset_blocks++;
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
dif_verify(struct iovec *iovs,
	   uint32_t block_size, uint32_t guard_interval,
	   enum spdk_dif_type dif_type, uint32_t dif_flags,
	   uint32_t ref_tag, uint16_t apptag_mask, uint16_t app_tag,
	   int *iovpos, uint32_t *iov_offset)
{
	uint16_t guard = 0;
	void *buf;
	int rc;

	buf = iovs[*iovpos].iov_base + *iov_offset;

	if (dif_flags & SPDK_DIF_GUARD_CHECK) {
		guard = spdk_crc16_t10dif(0, buf, guard_interval);
	}

	rc = _dif_verify(buf + guard_interval, dif_type, dif_flags,
			 guard, ref_tag, apptag_mask, app_tag);
	if (rc != 0) {
		return rc;
	}

	*iov_offset += block_size;
	if (*iov_offset == iovs[*iovpos].iov_len) {
		(*iovpos)++;
		*iov_offset = 0;
	}

	return 0;
}

static int
dif_verify_split(struct iovec *iovs, int iovcnt,
		 uint32_t block_size, uint32_t guard_interval,
		 enum spdk_dif_type dif_type, uint32_t dif_flags,
		 uint32_t ref_tag, uint16_t apptag_mask, uint16_t app_tag,
		 int *iovpos, uint32_t *iov_offset)
{
	uint32_t offset_in_block = 0, offset_in_dif, buf_len;
	uint16_t guard = 0;
	void *buf;
	struct spdk_dif dif = {};

	while (offset_in_block < block_size && *iovpos < iovcnt) {
		buf = iovs[*iovpos].iov_base + *iov_offset;
		buf_len = iovs[*iovpos].iov_len - *iov_offset;

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

		*iov_offset += buf_len;
		if (*iov_offset == iovs[*iovpos].iov_len) {
			(*iovpos)++;
			*iov_offset = 0;
		}
		offset_in_block += buf_len;
	}

	/* If a whole block is parsed, compare the parsed DIF field and computed
	 * or passed values.
	 */
	return _dif_verify(&dif, dif_type, dif_flags,
			   guard, ref_tag, apptag_mask, app_tag);
}

int
spdk_dif_verify(struct iovec *iovs, int iovcnt,
		uint32_t block_size, uint32_t md_size, uint32_t num_blocks,
		bool dif_start, enum spdk_dif_type dif_type, uint32_t dif_flags,
		uint32_t init_ref_tag, uint16_t apptag_mask, uint16_t app_tag)
{
	uint32_t offset_blocks = 0, iov_offset = 0, guard_interval, ref_tag;
	int iovpos = 0, rc;
	bool contig;

	if (md_size == 0) {
		return -EINVAL;
	}

	if (!_are_iovs_valid(iovs, iovcnt, block_size, num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		return -EINVAL;
	}

	if (!_dif_type_is_valid(dif_type, dif_flags)) {
		SPDK_ERRLOG("DIF type is invalid.\n");
		return -EINVAL;
	}

	ref_tag = init_ref_tag;
	guard_interval = _get_dif_guard_interval(block_size, md_size, dif_start);
	contig = _are_iovs_block_multiple(iovs, iovcnt, block_size);

	while (offset_blocks < num_blocks && iovpos < iovcnt) {
		if (contig) {
			rc = dif_verify(iovs, block_size, guard_interval,
					dif_type, dif_flags,
					ref_tag, apptag_mask, app_tag,
					&iovpos, &iov_offset);
		} else {
			rc = dif_verify_split(iovs, iovcnt, block_size, guard_interval,
					      dif_type, dif_flags,
					      ref_tag, apptag_mask, app_tag,
					      &iovpos, &iov_offset);
		}

		if (rc != 0) {
			return rc;
		}

		/* Reference tag check is not enabled for type 3, and just
		 * incrementing ref_tag per block is OK.
		 */
		ref_tag++;

		offset_blocks++;
	}

	return 0;
}

static void
dif_generate_copy(struct iovec *iovs, struct iovec *bounce_iov,
		  uint32_t block_size, uint32_t data_block_size, uint32_t guard_interval,
		  uint32_t dif_flags, uint32_t ref_tag, uint16_t app_tag,
		  int *iovpos, uint32_t *iov_offset, uint32_t bounce_iov_offset)
{
	uint16_t guard = 0;
	void *src, *dst;

	src = iovs[*iovpos].iov_base + *iov_offset;
	dst = bounce_iov->iov_base + bounce_iov_offset;

	if (dif_flags & SPDK_DIF_GUARD_CHECK) {
		guard = spdk_crc16_t10dif_copy(guard, dst, src, data_block_size);
		guard = spdk_crc16_t10dif(guard, dst + data_block_size,
					  guard_interval - data_block_size);
	} else {
		memcpy(dst, src, data_block_size);
	}

	_dif_generate(dst + guard_interval, dif_flags, guard, ref_tag, app_tag);

	*iov_offset += data_block_size;
	if (*iov_offset == iovs[*iovpos].iov_len) {
		(*iovpos)++;
		*iov_offset = 0;
	}
}

static void
dif_generate_copy_split(struct iovec *iovs, int iovcnt, struct iovec *bounce_iov,
			uint32_t block_size, uint32_t data_block_size, uint32_t guard_interval,
			uint32_t dif_flags, uint32_t ref_tag, uint16_t app_tag,
			int *iovpos, uint32_t *iov_offset, uint32_t bounce_iov_offset)
{
	uint32_t offset_in_block = 0, src_len;
	uint16_t guard = 0;
	void *src, *dst;

	dst = bounce_iov->iov_base + bounce_iov_offset;

	if (dif_flags & SPDK_DIF_GUARD_CHECK) {
		guard = spdk_crc16_t10dif(guard, dst + data_block_size,
					  guard_interval - data_block_size);
	}

	while (offset_in_block < data_block_size && *iovpos < iovcnt) {
		/* Compute CRC over split logical block data and copy
		 * data to bounce buffer.
		 */
		src = iovs[*iovpos].iov_base + *iov_offset;
		src_len = spdk_min(iovs[*iovpos].iov_len - *iov_offset,
				   data_block_size - offset_in_block);

		if (dif_flags & SPDK_DIF_GUARD_CHECK) {
			guard = spdk_crc16_t10dif_copy(guard, dst + offset_in_block,
						       src, src_len);
		} else {
			memcpy(dst + offset_in_block, src, src_len);
		}

		*iov_offset += src_len;
		if (*iov_offset == iovs[*iovpos].iov_len) {
			(*iovpos)++;
			*iov_offset = 0;
		}
		offset_in_block += src_len;
	}

	assert(offset_in_block == data_block_size);

	_dif_generate(dst + guard_interval, dif_flags, guard, ref_tag, app_tag);
}

int
spdk_dif_generate_copy(struct iovec *iovs, int iovcnt, struct iovec *bounce_iov,
		       uint32_t block_size, uint32_t md_size, uint32_t num_blocks,
		       bool dif_start, enum spdk_dif_type dif_type, uint32_t dif_flags,
		       uint32_t init_ref_tag, uint16_t app_tag)
{
	uint32_t offset_blocks = 0, iov_offset = 0, bounce_iov_offset = 0;
	uint32_t data_block_size, guard_interval, ref_tag;
	int iovpos = 0;
	bool contig;

	if (md_size == 0) {
		return -EINVAL;
	}

	data_block_size = block_size - md_size;

	if (!_are_iovs_valid(iovs, iovcnt, data_block_size, num_blocks) ||
	    !_are_iovs_valid(bounce_iov, 1, block_size, num_blocks)) {
		SPDK_ERRLOG("Size of iovec arrays are not valid.\n");
		return -EINVAL;
	}

	if (!_dif_type_is_valid(dif_type, dif_flags)) {
		SPDK_ERRLOG("DIF type is invalid\n");
		return -EINVAL;
	}

	ref_tag = init_ref_tag;
	guard_interval = _get_dif_guard_interval(block_size, md_size, dif_start);
	contig = _are_iovs_block_multiple(iovs, iovcnt, block_size);

	while (offset_blocks < num_blocks && iovpos < iovcnt &&
	       bounce_iov_offset < bounce_iov->iov_len) {
		if (contig) {
			dif_generate_copy(iovs, bounce_iov,
					  block_size, data_block_size, guard_interval,
					  dif_flags, ref_tag, app_tag,
					  &iovpos, &iov_offset, bounce_iov_offset);
		} else {
			dif_generate_copy_split(iovs, iovcnt, bounce_iov,
						block_size, data_block_size, guard_interval,
						dif_flags, ref_tag, app_tag,
						&iovpos, &iov_offset, bounce_iov_offset);
		}

		/* Reference tag check is not enabled for type 3, and just incrementing
		 * ref_tag per block is OK.
		 */
		ref_tag++;

		offset_blocks++;
		bounce_iov_offset += block_size;
	}

	return 0;
}

static int
dif_verify_copy(struct iovec *iovs, struct iovec *bounce_iov,
		uint32_t block_size, uint32_t data_block_size, uint32_t guard_interval,
		enum spdk_dif_type dif_type, uint32_t dif_flags,
		uint32_t ref_tag, uint16_t apptag_mask, uint16_t app_tag,
		int *iovpos, uint32_t *iov_offset, uint32_t bounce_iov_offset)
{
	uint16_t guard = 0;
	void *src, *dst;
	int rc;

	dst = iovs[*iovpos].iov_base + *iov_offset;
	src = bounce_iov->iov_base + bounce_iov_offset;

	if (dif_flags & SPDK_DIF_GUARD_CHECK) {
		guard = spdk_crc16_t10dif_copy(guard, dst, src, data_block_size);
		guard = spdk_crc16_t10dif(guard, src + data_block_size,
					  guard_interval - data_block_size);
	} else {
		memcpy(dst, src, data_block_size);
	}

	rc = _dif_verify(src + guard_interval, dif_type, dif_flags,
			 guard, ref_tag, apptag_mask, app_tag);
	if (rc != 0) {
		return rc;
	}

	*iov_offset += data_block_size;
	if (*iov_offset == iovs[*iovpos].iov_len) {
		(*iovpos)++;
		*iov_offset = 0;
	}

	return 0;
}

static int
dif_verify_copy_split(struct iovec *iovs, int iovcnt, struct iovec *bounce_iov,
		      uint32_t block_size, uint32_t data_block_size, uint32_t guard_interval,
		      enum spdk_dif_type dif_type, uint32_t dif_flags,
		      uint32_t ref_tag, uint16_t apptag_mask, uint16_t app_tag,
		      int *iovpos, uint32_t *iov_offset, uint32_t bounce_iov_offset)
{
	uint32_t offset_in_block = 0, dst_len;
	uint16_t guard = 0;
	void *src, *dst;

	src = bounce_iov->iov_base + bounce_iov_offset;

	if (dif_flags & SPDK_DIF_GUARD_CHECK) {
		guard = spdk_crc16_t10dif(guard, src + data_block_size,
					  guard_interval - data_block_size);
	}

	while (offset_in_block < data_block_size && *iovpos < iovcnt) {
		/* Compute CRC over logical block data in bounce buffer and
		 * copy data to original split buffers.
		 */
		dst = iovs[*iovpos].iov_base + *iov_offset;
		dst_len = spdk_min(iovs[*iovpos].iov_len - *iov_offset,
				   data_block_size - offset_in_block);

		if (dif_flags & SPDK_DIF_GUARD_CHECK) {
			guard = spdk_crc16_t10dif_copy(guard, dst,
						       src + offset_in_block, dst_len);
		} else {
			memcpy(dst, src + offset_in_block, dst_len);
		}

		*iov_offset += dst_len;
		if (*iov_offset == iovs[*iovpos].iov_len) {
			(*iovpos)++;
			*iov_offset = 0;
		}
		offset_in_block += dst_len;
	}

	assert(offset_in_block == data_block_size);

	return _dif_verify(src + guard_interval, dif_type, dif_flags,
			   guard, ref_tag, apptag_mask, app_tag);
}

int
spdk_dif_verify_copy(struct iovec *iovs, int iovcnt, struct iovec *bounce_iov,
		     uint32_t block_size, uint32_t md_size, uint32_t num_blocks,
		     bool dif_start, enum spdk_dif_type dif_type, uint32_t dif_flags,
		     uint32_t init_ref_tag, uint16_t apptag_mask, uint16_t app_tag)
{
	uint32_t offset_blocks = 0, iov_offset = 0, bounce_iov_offset = 0;
	uint32_t data_block_size, guard_interval, ref_tag;
	int iovpos = 0, rc;
	bool contig;

	if (md_size == 0) {
		return -EINVAL;
	}

	data_block_size = block_size - md_size;

	if (!_are_iovs_valid(iovs, iovcnt, data_block_size, num_blocks) ||
	    !_are_iovs_valid(bounce_iov, 1, block_size, num_blocks)) {
		SPDK_ERRLOG("Size of iovec arrays are not valid\n");
		return -EINVAL;
	}

	if (!_dif_type_is_valid(dif_type, dif_flags)) {
		SPDK_ERRLOG("DIF type is invalid.\n");
		return -EINVAL;
	}

	ref_tag = init_ref_tag;
	guard_interval = _get_dif_guard_interval(block_size, md_size, dif_start);
	contig = _are_iovs_block_multiple(iovs, iovcnt, block_size);

	while (offset_blocks < num_blocks && iovpos < iovcnt &&
	       bounce_iov_offset < bounce_iov->iov_len) {
		if (contig) {
			rc = dif_verify_copy(iovs, bounce_iov,
					     block_size, data_block_size, guard_interval,
					     dif_type, dif_flags,
					     ref_tag, apptag_mask, app_tag,
					     &iovpos, &iov_offset, bounce_iov_offset);
		} else {
			rc = dif_verify_copy_split(iovs, iovcnt, bounce_iov,
						   block_size, data_block_size, guard_interval,
						   dif_type, dif_flags,
						   ref_tag, apptag_mask, app_tag,
						   &iovpos, &iov_offset, bounce_iov_offset);
		}

		if (rc != 0) {
			return rc;
		}

		/* Reference tag check is not enabled for type 3, and just incrementing
		 * ref_tag per block is OK.
		 */
		ref_tag++;

		offset_blocks++;
		bounce_iov_offset += block_size;
	}

	return 0;
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
_dif_inject_error(struct iovec *iovs, int iovcnt,
		  uint32_t block_size, uint32_t num_blocks,
		  uint32_t inject_offset_blocks,
		  uint32_t inject_offset_bytes,
		  uint32_t inject_offset_bits)
{
	uint32_t iov_offset, offset_in_block, buf_len;
	int iovpos;
	void *buf;

	iov_offset = block_size * inject_offset_blocks;

	for (iovpos = 0; iovpos < iovcnt; iovpos++) {
		if (iov_offset < iovs[iovpos].iov_len) {
			break;
		}
		iov_offset -= iovs[iovpos].iov_len;
	}

	offset_in_block = 0;

	while (offset_in_block < block_size && iovpos < iovcnt) {
		buf = iovs[iovpos].iov_base + iov_offset;
		buf_len = iovs[iovpos].iov_len - iov_offset;

		if (inject_offset_bytes >= offset_in_block &&
		    inject_offset_bytes < offset_in_block + buf_len) {
			buf += inject_offset_bytes - offset_in_block;
			_bit_flip(buf, inject_offset_bits);
			return 0;
		}

		iov_offset += buf_len;
		if (iov_offset == iovs[iovpos].iov_len) {
			iovpos++;
			iov_offset = 0;
		}
		offset_in_block += buf_len;
	}

	return -1;
}

static int
dif_inject_error(struct iovec *iovs, int iovcnt,
		 uint32_t block_size, uint32_t num_blocks,
		 uint32_t start_inject_bytes, uint32_t inject_range_bytes)
{
	uint32_t inject_offset_blocks, inject_offset_bytes, inject_offset_bits;
	uint32_t offset_blocks;

	srand(time(0));

	inject_offset_blocks = rand() % num_blocks;
	inject_offset_bytes = start_inject_bytes + (rand() % inject_range_bytes);
	inject_offset_bits = rand() % sizeof(uint8_t);

	for (offset_blocks = 0; offset_blocks < num_blocks; offset_blocks++) {
		if (offset_blocks == inject_offset_blocks) {
			return _dif_inject_error(iovs, iovcnt, block_size, num_blocks,
						 inject_offset_blocks,
						 inject_offset_bytes,
						 inject_offset_bits);
		}
	}

	return -1;
}

#define _member_size(type, member)	sizeof(((type *)0)->member)

int
spdk_dif_inject_error(struct iovec *iovs, int iovcnt,
		      uint32_t block_size, uint32_t md_size, uint32_t num_blocks,
		      bool dif_start, uint32_t inject_flags)
{
	uint32_t guard_interval;
	int rc;

	if (md_size == 0) {
		return -EINVAL;
	}

	if (!_are_iovs_valid(iovs, iovcnt, block_size, num_blocks)) {
		SPDK_ERRLOG("Size of iovec array is not valid.\n");
		return -EINVAL;
	}

	guard_interval = _get_dif_guard_interval(block_size, md_size, dif_start);

	if (inject_flags & SPDK_DIF_REFTAG_ERROR) {
		rc = dif_inject_error(iovs, iovcnt, block_size, num_blocks,
				      guard_interval + offsetof(struct spdk_dif, ref_tag),
				      _member_size(struct spdk_dif, ref_tag));
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to Reference Tag.\n");
			return rc;
		}
	}

	if (inject_flags & SPDK_DIF_APPTAG_ERROR) {
		rc = dif_inject_error(iovs, iovcnt, block_size, num_blocks,
				      guard_interval + offsetof(struct spdk_dif, app_tag),
				      _member_size(struct spdk_dif, app_tag));
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to Application Tag.\n");
			return rc;
		}
	}
	if (inject_flags & SPDK_DIF_GUARD_ERROR) {
		rc = dif_inject_error(iovs, iovcnt, block_size, num_blocks,
				      guard_interval,
				      _member_size(struct spdk_dif, guard));
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to Guard.\n");
			return rc;
		}
	}

	if (inject_flags & SPDK_DIF_DATA_ERROR) {
		rc = dif_inject_error(iovs, iovcnt, block_size, num_blocks,
				      0,
				      guard_interval);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to data block.\n");
			return rc;
		}
	}

	return 0;
}
