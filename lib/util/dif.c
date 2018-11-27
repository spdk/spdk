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

static uint32_t
_dif_get_guard_interval(uint32_t block_size, uint32_t md_size, bool dif_start)
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
_dif_generate(void *_dif, void *data_buf, uint32_t guard_interval,
	      uint32_t dif_flags, uint32_t ref_tag, uint16_t app_tag)
{
	struct spdk_dif *dif = _dif;
	uint16_t guard;

	if (dif_flags & SPDK_DIF_GUARD_CHECK) {
		/* Compute CRC over the logical block data. */
		guard = spdk_crc16_t10dif(data_buf, guard_interval);
		to_be16(&dif->guard, guard);
	}

	if (dif_flags & SPDK_DIF_APPTAG_CHECK) {
		to_be16(&dif->app_tag, app_tag);
	}

	if (dif_flags & SPDK_DIF_REFTAG_CHECK) {
		to_be32(&dif->ref_tag, ref_tag);
	}
}

static int
dif_generate(struct iovec *iovs, int iovcnt, uint32_t block_size,
	     uint32_t guard_interval, enum spdk_dif_type dif_type,
	     uint32_t dif_flags, uint32_t init_ref_tag, uint16_t app_tag)
{
	uint32_t iov_offset, ref_tag;
	int iovpos;
	void *buf;

	iov_offset = 0;
	iovpos = 0;
	ref_tag = init_ref_tag;

	while (iovpos < iovcnt) {
		buf = iovs[iovpos].iov_base + iov_offset;

		_dif_generate(buf + guard_interval, buf, guard_interval,
			      dif_flags, ref_tag, app_tag);

		/* For type 1 and 2, the reference tag is incremented for each
		 * subsequent logical block. For type 3, the reference tag is
		 * remains the same as the initial reference tag.
		 */
		if (dif_type != SPDK_DIF_TYPE3) {
			ref_tag++;
		}

		iov_offset += block_size;
		if (iov_offset == iovs[iovpos].iov_len) {
			iovpos++;
			iov_offset = 0;
		}
	}

	return 0;
}

static int
dif_generate_split(struct iovec *iovs, int iovcnt, uint32_t block_size,
		   uint32_t guard_interval, enum spdk_dif_type dif_type,
		   uint32_t dif_flags, uint32_t init_ref_tag, uint16_t app_tag)
{
	uint32_t payload_offset, offset_blocks, offset_in_block;
	uint32_t iov_offset, buf_len, ref_tag;
	int iovpos;
	void *buf;
	uint8_t *contig_buf, contig_dif[sizeof(struct spdk_dif)] = {0};

	contig_buf = calloc(1, guard_interval);
	if (!contig_buf) {
		SPDK_ERRLOG("calloc() failed for contiguous data block buffer.\n");
		return -ENOMEM;
	}

	payload_offset = 0;
	iov_offset = 0;
	iovpos = 0;

	while (iovpos < iovcnt) {
		offset_blocks = payload_offset / block_size;
		offset_in_block = payload_offset % block_size;

		/* For type 1 and 2, the reference tag is incremented for each
		 * subsequent logical block. For type 3, the reference tag is
		 * remains the same as the initial reference tag.
		 */
		if (dif_type != SPDK_DIF_TYPE3) {
			ref_tag = init_ref_tag + offset_blocks;
		} else {
			ref_tag = init_ref_tag;
		}

		buf = iovs[iovpos].iov_base + iov_offset;
		buf_len = iovs[iovpos].iov_len - iov_offset;

		if (offset_in_block < guard_interval) {
			/* Copy the split logical block data to the temporary
			 * contiguous buffer to compute CRC.
			 */
			buf_len = spdk_min(guard_interval - offset_in_block, buf_len);
			if (dif_flags & SPDK_DIF_GUARD_CHECK) {
				memcpy(&contig_buf[offset_in_block], buf, buf_len);
			}

			if (offset_in_block + buf_len == guard_interval) {
				/* If a whole logical block data is parsed, generate DIF and
				 * save it to the temporary contiguous buffer.
				 */
				_dif_generate(contig_dif, contig_buf, guard_interval,
					      dif_flags, ref_tag, app_tag);
			}

		} else if (offset_in_block < guard_interval + sizeof(struct spdk_dif)) {
			/* Copy generated DIF to the split DIF field. */
			buf_len = spdk_min(guard_interval + sizeof(struct spdk_dif) - offset_in_block,
					   buf_len);
			memcpy(buf, &contig_dif[offset_in_block - guard_interval], buf_len);

		} else {
			/* Skip metadata field after DIF field when metadata size is more
			 * than 8 bytes.
			 */
			buf_len = spdk_min(block_size - offset_in_block, buf_len);
		}

		payload_offset += buf_len;
		iov_offset += buf_len;
		if (iov_offset == iovs[iovpos].iov_len) {
			iovpos++;
			iov_offset = 0;
		}
	}

	free(contig_buf);
	return 0;
}

int
spdk_dif_generate(struct iovec *iovs, int iovcnt, uint32_t block_size,
		  uint32_t md_size, bool dif_start, enum spdk_dif_type dif_type,
		  uint32_t dif_flags, uint32_t init_ref_tag, uint16_t app_tag)
{
	uint32_t guard_interval;

	if (md_size == 0) {
		return -EINVAL;
	}

	switch (dif_type) {
	case SPDK_DIF_TYPE1:
	case SPDK_DIF_TYPE2:
		break;
	case SPDK_DIF_TYPE3:
		if (dif_flags & SPDK_DIF_REFTAG_CHECK) {
			SPDK_ERRLOG("Reference Tag should not be checked for Type 3\n");
			return -EINVAL;
		}
		break;
	default:
		SPDK_ERRLOG("Unknown DIF Type: %d\n", dif_type);
		return -EINVAL;
	}

	guard_interval = _dif_get_guard_interval(block_size, md_size, dif_start);

	if (_are_iovs_bytes_multiple(iovs, iovcnt, block_size)) {
		return dif_generate(iovs, iovcnt, block_size, guard_interval,
				    dif_type, dif_flags, init_ref_tag, app_tag);
	} else {
		return dif_generate_split(iovs, iovcnt, block_size, guard_interval,
					  dif_type, dif_flags, init_ref_tag, app_tag);
	}
}

static int
_dif_verify(void *_dif, void *data_buf, uint32_t guard_interval,
	    enum spdk_dif_type dif_type, uint32_t dif_flags,
	    uint32_t ref_tag, uint16_t apptag_mask, uint16_t app_tag)
{
	struct spdk_dif *dif = _dif;
	uint16_t guard, _guard, _app_tag;
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
		guard = spdk_crc16_t10dif(data_buf, guard_interval);
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
			 * if Type 2 is used, */
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
dif_verify(struct iovec *iovs, int iovcnt, uint32_t block_size,
	   uint32_t guard_interval, enum spdk_dif_type dif_type,
	   uint32_t dif_flags, uint32_t init_ref_tag,
	   uint16_t apptag_mask, uint16_t app_tag)
{
	uint32_t iov_offset, ref_tag;
	int iovpos, rc;
	void *buf;

	ref_tag = init_ref_tag;
	iov_offset = 0;
	iovpos = 0;

	while (iovpos < iovcnt) {
		buf = iovs[iovpos].iov_base + iov_offset;

		rc = _dif_verify(buf + guard_interval, buf, guard_interval,
				 dif_type, dif_flags, ref_tag, apptag_mask, app_tag);
		if (rc != 0) {
			return rc;
		}

		/* For type 1 and 2, the reference tag is incremented for each
		 * subsequent logical block. For type 3, the reference tag is
		 * remains the same as the initial reference tag.
		 */
		if (dif_type != SPDK_DIF_TYPE3) {
			ref_tag++;
		}

		iov_offset += block_size;
		if (iov_offset == iovs[iovpos].iov_len) {
			iovpos++;
			iov_offset = 0;
		}
	}

	return 0;
}

static int
dif_verify_split(struct iovec *iovs, int iovcnt, uint32_t block_size,
		 uint32_t guard_interval, enum spdk_dif_type dif_type,
		 uint32_t dif_flags, uint32_t init_ref_tag,
		 uint16_t apptag_mask, uint16_t app_tag)
{
	uint32_t payload_offset, offset_blocks, offset_in_block;
	uint32_t iov_offset, buf_len, ref_tag;
	int iovpos, rc;
	void *buf;
	uint8_t *contig_buf, contig_dif[8] = {0};

	contig_buf = calloc(1, guard_interval);
	if (!contig_buf) {
		SPDK_ERRLOG("calloc() failed for contiguous data block buffer\n");
		return -ENOMEM;
	}

	payload_offset = 0;
	iov_offset = 0;
	iovpos = 0;

	while (iovpos < iovcnt) {
		offset_blocks = payload_offset / block_size;
		offset_in_block = payload_offset % block_size;

		/* For type 1 and 2, the reference tag is incremented for each
		 * subsequent logical block. For type 3, the reference tag is
		 * remains the same as the initial reference tag.
		 */
		if (dif_type != SPDK_DIF_TYPE3) {
			ref_tag = init_ref_tag + offset_blocks;
		} else {
			ref_tag = init_ref_tag;
		}

		buf = iovs[iovpos].iov_base + iov_offset;
		buf_len = iovs[iovpos].iov_len - iov_offset;

		if (offset_in_block < guard_interval) {
			/* Copy the split logical block data to the temporary
			 * contiguous buffer to compute CRC.
			 */
			buf_len = spdk_min(guard_interval - offset_in_block, buf_len);
			if (dif_flags & SPDK_DIF_GUARD_CHECK) {
				memcpy(&contig_buf[offset_in_block], buf, buf_len);
			}
		} else if (offset_in_block < guard_interval + sizeof(struct spdk_dif)) {
			/* Copy the split DIF field to the temporary contiguous buffer */
			buf_len = spdk_min(guard_interval + sizeof(struct spdk_dif) - offset_in_block,
					   buf_len);
			memcpy(&contig_dif[offset_in_block - guard_interval], buf, buf_len);
			if (offset_in_block + buf_len == guard_interval + sizeof(struct spdk_dif)) {
				/* If a whole DIF field is parsed, compare the parsed DIF field
				 * and computed or passed values.
				 */
				rc = _dif_verify(contig_dif, contig_buf, guard_interval,
						 dif_type, dif_flags, ref_tag, apptag_mask, app_tag);
				if (rc != 0) {
					free(contig_buf);
					return rc;
				}
			}
		} else {
			/* Skip metadata field after DIF field when metadata size is more
			 * than 8 bytes.
			 */
			buf_len = spdk_min(buf_len, block_size - offset_in_block);
		}

		payload_offset += buf_len;
		iov_offset += buf_len;
		if (iov_offset == iovs[iovpos].iov_len) {
			iovpos++;
			iov_offset = 0;
		}
	}

	free(contig_buf);
	return 0;
}

int
spdk_dif_verify(struct iovec *iovs, int iovcnt, uint32_t block_size,
		uint32_t md_size, bool dif_start, enum spdk_dif_type dif_type,
		uint32_t dif_flags, uint32_t init_ref_tag,
		uint16_t apptag_mask, uint16_t app_tag)
{
	uint32_t guard_interval;

	if (md_size == 0) {
		SPDK_ERRLOG("Metadata size is 0\n");
		return -EINVAL;
	}

	switch (dif_type) {
	case SPDK_DIF_TYPE1:
	case SPDK_DIF_TYPE2:
	case SPDK_DIF_TYPE3:
		break;
	default:
		SPDK_ERRLOG("Unknown DIF Type: %d\n", dif_type);
		return -EINVAL;
	}

	guard_interval = _dif_get_guard_interval(block_size, md_size, dif_start);

	if (_are_iovs_bytes_multiple(iovs, iovcnt, block_size)) {
		return dif_verify(iovs, iovcnt, block_size, guard_interval, dif_type,
				  dif_flags, init_ref_tag, apptag_mask, app_tag);
	} else {
		return dif_verify_split(iovs, iovcnt, block_size, guard_interval,
					dif_type, dif_flags, init_ref_tag,
					apptag_mask, app_tag);
	}
}
