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

/* Context to iterate a scatter gather list. */
struct _iov_iter {
	/* Current iovec in the iteration */
	struct iovec *iov;

	/* Remaining count of iovecs in the iteration. */
	int iovcnt;

	/* Current offset in the iovec */
	uint32_t iov_offset;

	/* Current offset in virtually contiguous payload */
	uint32_t payload_offset;
};

static inline void
_iov_iter_init(struct _iov_iter *i, struct iovec *iovs, int iovcnt)
{
	i->iov = iovs;
	i->iovcnt = iovcnt;
	i->iov_offset = 0;
	i->payload_offset = 0;
}

static inline bool
_iov_iter_cont(struct _iov_iter *i)
{
	return i->iovcnt != 0;
}

static inline void
_iov_iter_advance(struct _iov_iter *i, uint32_t step)
{
	i->payload_offset += step;
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

static inline uint32_t
_iov_iter_get_payload_offset(struct _iov_iter *i)
{
	return i->payload_offset;
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

static uint32_t
_get_iovs_len(struct iovec *iovs, int iovcnt)
{
	int i;
	uint32_t len = 0;

	for (i = 0; i < iovcnt; i++) {
		len += iovs[i].iov_len;
	}

	return len;
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
	struct _iov_iter iter;
	uint32_t ref_tag;
	void *buf;

	_iov_iter_init(&iter, iovs, iovcnt);
	ref_tag = init_ref_tag;

	while (_iov_iter_cont(&iter)) {
		_iov_iter_get_buf(&iter, &buf, NULL);

		_dif_generate(buf + guard_interval, buf, guard_interval,
			      dif_flags, ref_tag, app_tag);

		/* For type 1 and 2, the reference tag is incremented for each
		 * subsequent logical block. For type 3, the reference tag is
		 * remains the same as the initial reference tag.
		 */
		if (dif_type != SPDK_DIF_TYPE3) {
			ref_tag++;
		}

		_iov_iter_advance(&iter, block_size);
	}

	return 0;
}

static int
dif_generate_split(struct iovec *iovs, int iovcnt, uint32_t block_size,
		   uint32_t guard_interval, enum spdk_dif_type dif_type,
		   uint32_t dif_flags, uint32_t init_ref_tag, uint16_t app_tag)
{
	struct _iov_iter iter;
	uint32_t offset_blocks, offset_in_block;
	uint32_t buf_len, ref_tag;
	void *buf;
	uint8_t *contig_buf, contig_dif[sizeof(struct spdk_dif)] = {0};

	contig_buf = calloc(1, guard_interval);
	if (!contig_buf) {
		SPDK_ERRLOG("calloc() failed for contiguous data block buffer.\n");
		return -ENOMEM;
	}

	_iov_iter_init(&iter, iovs, iovcnt);

	while (_iov_iter_cont(&iter)) {
		offset_blocks = _iov_iter_get_payload_offset(&iter) / block_size;
		offset_in_block = _iov_iter_get_payload_offset(&iter) % block_size;

		/* For type 1 and 2, the reference tag is incremented for each
		 * subsequent logical block. For type 3, the reference tag is
		 * remains the same as the initial reference tag.
		 */
		if (dif_type != SPDK_DIF_TYPE3) {
			ref_tag = init_ref_tag + offset_blocks;
		} else {
			ref_tag = init_ref_tag;
		}

		_iov_iter_get_buf(&iter, &buf, &buf_len);

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

		_iov_iter_advance(&iter, buf_len);
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
	struct _iov_iter iter;
	uint32_t ref_tag;
	int rc;
	void *buf;

	_iov_iter_init(&iter, iovs, iovcnt);
	ref_tag = init_ref_tag;

	while (_iov_iter_cont(&iter)) {
		_iov_iter_get_buf(&iter, &buf, NULL);

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

		_iov_iter_advance(&iter, block_size);
	}

	return 0;
}

static int
dif_verify_split(struct iovec *iovs, int iovcnt, uint32_t block_size,
		 uint32_t guard_interval, enum spdk_dif_type dif_type,
		 uint32_t dif_flags, uint32_t init_ref_tag,
		 uint16_t apptag_mask, uint16_t app_tag)
{
	struct _iov_iter iter;
	uint32_t offset_blocks, offset_in_block;
	uint32_t buf_len, ref_tag;
	int rc;
	void *buf;
	uint8_t *contig_buf, contig_dif[8] = {0};

	contig_buf = calloc(1, guard_interval);
	if (!contig_buf) {
		SPDK_ERRLOG("calloc() failed for contiguous data block buffer\n");
		return -ENOMEM;
	}

	_iov_iter_init(&iter, iovs, iovcnt);

	while (_iov_iter_cont(&iter)) {
		offset_blocks = _iov_iter_get_payload_offset(&iter) / block_size;
		offset_in_block = _iov_iter_get_payload_offset(&iter) % block_size;

		/* For type 1 and 2, the reference tag is incremented for each
		 * subsequent logical block. For type 3, the reference tag is
		 * remains the same as the initial reference tag.
		 */
		if (dif_type != SPDK_DIF_TYPE3) {
			ref_tag = init_ref_tag + offset_blocks;
		} else {
			ref_tag = init_ref_tag;
		}

		_iov_iter_get_buf(&iter, &buf, &buf_len);

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

		_iov_iter_advance(&iter, buf_len);
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

static int
dif_generate_copy(void *bounce_buf, struct iovec *iovs, int iovcnt,
		  uint32_t block_size, uint32_t data_block_size,
		  uint32_t guard_interval, enum spdk_dif_type dif_type,
		  uint32_t dif_flags, uint32_t init_ref_tag, uint16_t app_tag)
{
	struct _iov_iter iter;
	uint32_t ref_tag;
	void *buf;

	_iov_iter_init(&iter, iovs, iovcnt);
	ref_tag = init_ref_tag;

	while (_iov_iter_cont(&iter)) {
		/* Copy data block to bounce buffer. */
		_iov_iter_get_buf(&iter, &buf, NULL);
		memcpy(bounce_buf, buf, data_block_size);

		/* Generate and append DIF. */
		_dif_generate(bounce_buf + guard_interval, bounce_buf,
			      guard_interval, dif_flags, ref_tag, app_tag);

		/* For type 1 and 2, the reference tag is incremented for each
		 * subsequent logical block. For type 3, the reference tag is
		 * remains the same as the initial reference tag.
		 */
		if (dif_type != 3) {
			ref_tag++;
		}

		_iov_iter_advance(&iter, data_block_size);
		bounce_buf += block_size;
	}

	return 0;
}

static int
dif_generate_copy_split(void *bounce_buf, struct iovec *iovs, int iovcnt,
			uint32_t block_size, uint32_t data_block_size,
			uint32_t guard_interval, enum spdk_dif_type dif_type,
			uint32_t dif_flags, uint32_t init_ref_tag, uint16_t app_tag)
{
	struct _iov_iter iter;
	uint32_t offset_blocks, offset_in_block;
	uint32_t buf_len, ref_tag;
	void *buf;

	_iov_iter_init(&iter, iovs, iovcnt);

	while (_iov_iter_cont(&iter)) {
		offset_blocks = _iov_iter_get_payload_offset(&iter) / data_block_size;
		offset_in_block = _iov_iter_get_payload_offset(&iter) % data_block_size;

		/* For type 1 and 2, the reference tag is incremented for each
		 * subsequent logical block. For type 3, the reference tag is
		 * remains the same as the initial reference tag.
		 */
		if (dif_type != 3) {
			ref_tag = init_ref_tag + offset_blocks;
		} else {
			ref_tag = init_ref_tag;
		}

		_iov_iter_get_buf(&iter, &buf, &buf_len);
		buf_len = spdk_min(data_block_size - offset_in_block, buf_len);

		/* Copy split data block to contiguous bounce buffer. */
		memcpy(bounce_buf + offset_in_block, buf, buf_len);

		if (offset_in_block + buf_len == data_block_size) {
			/* If a whole block data is parsed, generate and append DIF. */
			_dif_generate(bounce_buf + guard_interval, bounce_buf,
				      guard_interval, dif_flags, ref_tag, app_tag);
			bounce_buf += block_size;
		}

		_iov_iter_advance(&iter, buf_len);
	}

	return 0;
}

int
spdk_dif_generate_copy(void *bounce_buf, struct iovec *iovs, int iovcnt,
		       uint32_t block_size, uint32_t md_size,
		       bool dif_start, enum spdk_dif_type dif_type,
		       uint32_t dif_flags, uint32_t init_ref_tag, uint16_t app_tag)
{
	uint32_t data_block_size, guard_interval;

	if (md_size == 0) {
		SPDK_ERRLOG("Metadata size is 0\n");
		return -EINVAL;
	}

	data_block_size = block_size - md_size;
	guard_interval = _dif_get_guard_interval(block_size, md_size, dif_start);

	if (_are_iovs_bytes_multiple(iovs, iovcnt, data_block_size)) {
		return dif_generate_copy(bounce_buf, iovs, iovcnt, block_size,
					 data_block_size, guard_interval, dif_type,
					 dif_flags, init_ref_tag, app_tag);
	} else {
		return dif_generate_copy_split(bounce_buf, iovs, iovcnt, block_size,
					       data_block_size, guard_interval, dif_type,
					       dif_flags, init_ref_tag, app_tag);
	}
}

static int
dif_verify_copy(struct iovec *iovs, int iovcnt, void *bounce_buf,
		uint32_t block_size, uint32_t data_block_size,
		uint32_t guard_interval, enum spdk_dif_type dif_type,
		uint32_t dif_flags, uint32_t init_ref_tag,
		uint16_t apptag_mask, uint16_t app_tag)
{
	struct _iov_iter iter;
	uint32_t ref_tag;
	int rc;
	void *buf;

	_iov_iter_init(&iter, iovs, iovcnt);
	ref_tag = init_ref_tag;

	while (_iov_iter_cont(&iter)) {
		rc = _dif_verify(bounce_buf + guard_interval, bounce_buf, guard_interval,
				 dif_type, dif_flags, ref_tag, apptag_mask, app_tag);
		if (rc != 0) {
			return rc;
		}

		_iov_iter_get_buf(&iter, &buf, NULL);
		memcpy(buf, bounce_buf, data_block_size);

		/* For type 1 and 2, the reference tag is incremented for each
		 * subsequent logical block. For type 3, the reference tag is
		 * remains the same as the initial reference tag.
		 */
		if (dif_type != 3) {
			ref_tag++;
		}

		_iov_iter_advance(&iter, data_block_size);
		bounce_buf += block_size;
	}

	return 0;
}

static int
dif_verify_copy_split(struct iovec *iovs, int iovcnt, void *bounce_buf,
		      uint32_t block_size, uint32_t data_block_size,
		      uint32_t guard_interval, enum spdk_dif_type dif_type,
		      uint32_t dif_flags, uint32_t init_ref_tag,
		      uint16_t apptag_mask, uint16_t app_tag)
{
	struct _iov_iter iter;
	uint32_t offset_blocks, offset_in_block;
	uint32_t buf_len, ref_tag;
	int rc;
	void *buf;

	_iov_iter_init(&iter, iovs, iovcnt);

	while (_iov_iter_cont(&iter)) {
		offset_blocks = _iov_iter_get_payload_offset(&iter) / data_block_size;
		offset_in_block = _iov_iter_get_payload_offset(&iter) % data_block_size;

		/* For type 1 and 2, the reference tag is incremented for each
		 * subsequent logical block. For type 3, the reference tag is
		 * remains the same as the initial reference tag.
		 */
		if (dif_type != 3) {
			ref_tag = init_ref_tag + offset_blocks;
		} else {
			ref_tag = init_ref_tag;
		}

		if (offset_in_block == 0) {
			rc = _dif_verify(bounce_buf + guard_interval, bounce_buf,
					 guard_interval, dif_type, dif_flags,
					 ref_tag, apptag_mask, app_tag);
			if (rc != 0) {
				return rc;
			}
		}

		_iov_iter_get_buf(&iter, &buf, &buf_len);
		buf_len = spdk_min(data_block_size - offset_in_block, buf_len);

		memcpy(buf, bounce_buf + offset_in_block, buf_len);

		if (offset_in_block + buf_len == data_block_size) {
			bounce_buf += block_size;
		}

		_iov_iter_advance(&iter, buf_len);
	}

	return 0;
}

int
spdk_dif_verify_copy(struct iovec *iovs, int iovcnt, void *bounce_buf,
		     uint32_t block_size, uint32_t md_size, bool dif_start,
		     enum spdk_dif_type dif_type, uint32_t dif_flags,
		     uint32_t init_ref_tag, uint16_t apptag_mask, uint16_t app_tag)
{
	uint32_t data_block_size, guard_interval;

	if (md_size == 0) {
		SPDK_ERRLOG("Metadata size is 0\n");
		return -EINVAL;
	}

	data_block_size = block_size - md_size;
	guard_interval = _dif_get_guard_interval(block_size, md_size, dif_start);

	if (_are_iovs_bytes_multiple(iovs, iovcnt, data_block_size)) {
		return dif_verify_copy(iovs, iovcnt, bounce_buf, block_size,
				       data_block_size, guard_interval, dif_type,
				       dif_flags, init_ref_tag, apptag_mask, app_tag);
	} else {
		return dif_verify_copy_split(iovs, iovcnt, bounce_buf, block_size,
					     data_block_size, guard_interval, dif_type,
					     dif_flags, init_ref_tag, apptag_mask, app_tag);
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
_dif_inject_error(struct iovec *iovs, int iovcnt, uint32_t block_size,
		  uint32_t inject_offset_blocks, uint32_t inject_offset_bytes,
		  uint32_t inject_offset_bits)
{
	struct _iov_iter iter;
	uint32_t offset_blocks;
	uint8_t *buf;

	_iov_iter_init(&iter, iovs, iovcnt);

	while (_iov_iter_cont(&iter)) {
		offset_blocks = _iov_iter_get_payload_offset(&iter) / block_size;

		_iov_iter_get_buf(&iter, (void **)&buf, NULL);

		if (inject_offset_blocks == offset_blocks) {
			buf += inject_offset_bytes;
			_bit_flip(buf, inject_offset_bits);
			return 0;
		}

		_iov_iter_advance(&iter, block_size);
	}

	return -1;
}

static int
_dif_inject_error_split(struct iovec *iovs, int iovcnt, uint32_t block_size,
			uint32_t inject_offset_blocks, uint32_t inject_offset_bytes,
			uint32_t inject_offset_bits)
{
	struct _iov_iter iter;
	uint32_t offset_blocks, offset_in_block, buf_len;
	uint8_t *buf;

	_iov_iter_init(&iter, iovs, iovcnt);

	while (_iov_iter_cont(&iter)) {
		offset_blocks = _iov_iter_get_payload_offset(&iter) / block_size;
		offset_in_block = _iov_iter_get_payload_offset(&iter) % block_size;

		_iov_iter_get_buf(&iter, (void **)&buf, &buf_len);

		if (inject_offset_blocks == offset_blocks &&
		    inject_offset_bytes >= offset_in_block &&
		    inject_offset_bytes < offset_in_block + buf_len) {
			buf += inject_offset_bytes - offset_in_block;
			_bit_flip(buf, inject_offset_bits);
			return 0;
		}

		_iov_iter_advance(&iter, buf_len);
	}

	return -1;
}

static int
dif_inject_error(struct iovec *iovs, int iovcnt, uint32_t block_size,
		 uint32_t start_inject_bytes, uint32_t num_inject_bytes)
{
	uint32_t num_blocks;
	uint32_t inject_offset_blocks, inject_offset_bytes, inject_offset_bits;

	num_blocks = _get_iovs_len(iovs, iovcnt) / block_size;

	srand(time(0));

	inject_offset_blocks = rand() % num_blocks;
	inject_offset_bytes = start_inject_bytes + (rand() % num_inject_bytes);
	inject_offset_bits = rand() % sizeof(uint8_t);

	if (_are_iovs_bytes_multiple(iovs, iovcnt, block_size)) {
		return _dif_inject_error(iovs, iovcnt, block_size, inject_offset_blocks,
					 inject_offset_bytes, inject_offset_bits);
	} else {
		return _dif_inject_error_split(iovs, iovcnt, block_size,
					       inject_offset_blocks, inject_offset_bytes,
					       inject_offset_bits);
	}
}

#define _member_size(type, member)	sizeof(((type *)0)->member)

int
spdk_dif_inject_error(struct iovec *iovs, int iovcnt,
		      uint32_t block_size, uint32_t md_size, bool dif_start,
		      uint32_t inject_flags)
{
	uint32_t guard_interval;
	int rc;

	guard_interval = _dif_get_guard_interval(block_size, md_size, dif_start);

	if (inject_flags & SPDK_DIF_GUARD_ERROR) {
		rc = dif_inject_error(iovs, iovcnt, block_size, guard_interval,
				      _member_size(struct spdk_dif, guard));
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to Guard\n");
			return rc;
		}
	}

	if (inject_flags & SPDK_DIF_APPTAG_ERROR) {
		rc = dif_inject_error(iovs, iovcnt, block_size,
				      guard_interval + offsetof(struct spdk_dif, app_tag),
				      _member_size(struct spdk_dif, app_tag));
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to App Tag\n");
			return rc;
		}
	}

	if (inject_flags & SPDK_DIF_REFTAG_ERROR) {
		rc = dif_inject_error(iovs, iovcnt, block_size,
				      guard_interval + offsetof(struct spdk_dif, ref_tag),
				      _member_size(struct spdk_dif, ref_tag));
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to Ref Tag\n");
			return rc;
		}
	}

	if (inject_flags & SPDK_DIF_DATA_ERROR) {
		rc = dif_inject_error(iovs, iovcnt, block_size, 0, guard_interval);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to inject error to data block\n");
			return rc;
		}
	}

	return 0;
}
