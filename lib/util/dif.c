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

/*
 * TODO: Format type that the data integrity field is transferred as the
 * last eight bytes of metadata.
 */

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

static void
_copy_iovs_to_buf(void *buf, size_t buf_len, struct iovec *iovs, int iovcnt)
{
	int i;
	size_t len;

	for (i = 0; i < iovcnt; i++) {
		len = spdk_min(iovs[i].iov_len, buf_len);
		memcpy(buf, iovs[i].iov_base, len);
		buf += len;
		buf_len -= len;
	}
}

static void
_copy_buf_to_iovs(struct iovec *iovs, int iovcnt, void *buf, uint32_t buf_len)
{
	int i;
	size_t len;

	for (i = 0; i < iovcnt; i++) {
		len = spdk_min(iovs[i].iov_len, buf_len);
		memcpy(iovs[i].iov_base, buf, len);
		buf += len;
		buf_len -= len;
	}
}

static void
_t10dif_generate(void *_dif, void *data_buf, uint32_t data_block_size,
		 uint32_t dif_flags, uint32_t ref_tag, uint16_t app_tag)
{
	struct spdk_t10dif *dif = _dif;
	uint16_t guard;

	if (dif_flags & SPDK_T10DIF_GUARD_CHECK) {
		guard = spdk_crc16_t10dif(data_buf, data_block_size);
		to_be16(&dif->guard, guard);
	}

	if (dif_flags & SPDK_T10DIF_APPTAG_CHECK) {
		to_be16(&dif->app_tag, app_tag);
	}

	if (dif_flags & SPDK_T10DIF_REFTAG_CHECK) {
		to_be32(&dif->ref_tag, ref_tag);
	}
}

static void
t10dif_generate(struct iovec *iovs, int iovcnt,
		uint32_t data_block_size, uint32_t block_size,
		uint32_t dif_flags, uint32_t ref_tag, uint16_t app_tag)
{
	uint32_t iov_offset;
	int iovpos;
	void *buf;

	iovpos = 0;
	iov_offset = 0;

	while (iovpos < iovcnt) {
		buf = iovs[iovpos].iov_base + iov_offset;
		_t10dif_generate(buf + data_block_size, buf, data_block_size,
				 dif_flags, ref_tag, app_tag);

		ref_tag++;
		iov_offset += block_size;
		if (iov_offset == iovs[iovpos].iov_len) {
			iovpos++;
			iov_offset = 0;
		}
	}
}

static void
t10dif_generate_split(struct iovec *iovs, int iovcnt,
		      uint32_t data_block_size, uint32_t block_size,
		      uint32_t dif_flags, uint32_t ref_tag, uint16_t app_tag)
{
	uint32_t payload_offset, offset_blocks, offset_in_block, iov_offset;
	uint32_t buf_len, _ref_tag;
	void *buf;
	int iovpos;
	uint8_t contig_buf[4096], contig_dif[8] = {0};

	payload_offset = 0;
	iovpos = 0;
	iov_offset = 0;

	while (iovpos < iovcnt) {
		offset_blocks = payload_offset / block_size;
		offset_in_block = payload_offset % block_size;

		_ref_tag = ref_tag + offset_blocks;

		buf = iovs[iovpos].iov_base + iov_offset;
		buf_len = iovs[iovpos].iov_len - iov_offset;

		if (offset_in_block < data_block_size) {
			buf_len = spdk_min(buf_len, data_block_size - offset_in_block);
			if (dif_flags & SPDK_T10DIF_GUARD_CHECK) {
				memcpy(&contig_buf[offset_in_block], buf, buf_len);
			}
			if (offset_in_block + buf_len == data_block_size) {
				_t10dif_generate(contig_dif, contig_buf, data_block_size,
						 dif_flags, _ref_tag, app_tag);
			}
		} else if (offset_in_block < data_block_size + 8) {
			buf_len = spdk_min(buf_len, data_block_size + 8 - offset_in_block);
			memcpy(buf, &contig_dif[offset_in_block - data_block_size], buf_len);
		} else {
			buf_len = spdk_min(buf_len, block_size - offset_in_block);
		}

		iov_offset += buf_len;
		if (iov_offset == iovs[iovpos].iov_len) {
			iovpos++;
			iov_offset = 0;
		}
		payload_offset += buf_len;
	}
}

void
spdk_t10dif_generate(struct iovec *iovs, int iovcnt,
		     uint32_t data_block_size, uint32_t metadata_size,
		     uint32_t dif_flags, uint32_t ref_tag, uint16_t app_tag)
{
	uint32_t block_size;

	if (metadata_size == 0) {
		return;
	}

	block_size = data_block_size + metadata_size;

	if (_are_iovs_bytes_multiple(iovs, iovcnt, block_size)) {
		t10dif_generate(iovs, iovcnt, data_block_size, block_size,
				dif_flags, ref_tag, app_tag);
	} else {
		t10dif_generate_split(iovs, iovcnt, data_block_size, block_size,
				      dif_flags, ref_tag, app_tag);
	}
}

static int
_t10dif_verify(void *_dif, void *data_buf, uint32_t data_block_size, uint32_t dif_flags,
	       uint32_t ref_tag, uint16_t apptag_mask, uint16_t app_tag)
{
	struct spdk_t10dif *dif = _dif;
	uint16_t guard, _guard, _app_tag;
	uint32_t _ref_tag;

	if (dif->app_tag == 0xFFFF) {
		return 0;
	}

	if (dif_flags & SPDK_T10DIF_GUARD_CHECK) {
		guard = spdk_crc16_t10dif(data_buf, data_block_size);
		_guard = from_be16(&dif->guard);
		if (_guard != guard) {
			SPDK_ERRLOG("Failed to compare Guard: LBA=%" PRIu32 "," \
				    "  Expected=%x, Actual=%x\n",
				    ref_tag, _guard, guard);
			return -1;
		}
	}

	if (dif_flags & SPDK_T10DIF_APPTAG_CHECK) {
		_app_tag = from_be16(&dif->app_tag);
		if ((_app_tag & ~apptag_mask) != app_tag) {
			SPDK_ERRLOG("Failed to compare App Tag: LBA=%" PRIu32 "," \
				    "  Expected=%x, Actual=%x\n",
				    ref_tag, app_tag, (_app_tag & ~apptag_mask));
			return -1;
		}
	}

	if (dif_flags & SPDK_T10DIF_REFTAG_CHECK) {
		_ref_tag = from_be32(&dif->ref_tag);
		if (_ref_tag != ref_tag) {
			SPDK_ERRLOG("Failed to compare Ref Tag: LBA=%" PRIu32 "," \
				    " Expected=%x, Actual=%x\n",
				    ref_tag, ref_tag, _ref_tag);
			return -1;
		}
	}

	return 0;
}

static int
t10dif_verify(struct iovec *iovs, int iovcnt,
	      uint32_t data_block_size, uint32_t block_size,
	      uint32_t dif_flags, uint32_t ref_tag,
	      uint16_t apptag_mask, uint16_t app_tag)
{
	uint32_t iov_offset;
	int iovpos, rc;
	void *buf;

	iovpos = 0;
	iov_offset = 0;

	while (iovpos < iovcnt) {
		buf = iovs[iovpos].iov_base + iov_offset;

		rc = _t10dif_verify(buf + data_block_size, buf, data_block_size,
				    dif_flags, ref_tag, apptag_mask, app_tag);
		if (rc != 0) {
			return rc;
		}

		ref_tag++;
		iov_offset += block_size;
		if (iov_offset == iovs[iovpos].iov_len) {
			iovpos++;
			iov_offset = 0;
		}
	}

	return 0;
}

static int
t10dif_verify_split(struct iovec *iovs, int iovcnt,
		    uint32_t data_block_size, uint32_t block_size,
		    uint32_t dif_flags, uint32_t ref_tag,
		    uint16_t apptag_mask, uint16_t app_tag)
{
	uint32_t payload_offset, offset_blocks, offset_in_block, iov_offset, buf_len;
	uint32_t _ref_tag;
	void *buf;
	int iovpos, rc;
	uint8_t contig_buf[4096], contig_dif[8] = {0};

	payload_offset = 0;
	iovpos = 0;
	iov_offset = 0;

	while (iovpos < iovcnt) {
		offset_blocks = payload_offset / block_size;
		offset_in_block = payload_offset % block_size;

		_ref_tag = ref_tag + offset_blocks;

		buf = iovs[iovpos].iov_base + iov_offset;
		buf_len = iovs[iovpos].iov_len - iov_offset;

		if (offset_in_block < data_block_size) {
			buf_len = spdk_min(buf_len, data_block_size - offset_in_block);
			if (dif_flags & SPDK_T10DIF_GUARD_CHECK) {
				memcpy(&contig_buf[offset_in_block], buf, buf_len);
			}
		} else if (offset_in_block < data_block_size + 8) {
			buf_len = spdk_min(buf_len, data_block_size + 8 - offset_in_block);
			memcpy(&contig_dif[offset_in_block - data_block_size], buf, buf_len);
			if (offset_in_block + buf_len == data_block_size + 8) {
				rc = _t10dif_verify(contig_dif, contig_buf, data_block_size,
						    dif_flags, _ref_tag, apptag_mask, app_tag);
				if (rc != 0) {
					return rc;
				}
			}
		} else {
			buf_len = spdk_min(buf_len, block_size - offset_in_block);
		}

		iov_offset += buf_len;
		if (iov_offset == iovs[iovpos].iov_len) {
			iovpos++;
			iov_offset = 0;
		}
	}

	return 0;
}

int
spdk_t10dif_verify(struct iovec *iovs, int iovcnt,
		   uint32_t data_block_size, uint32_t metadata_size,
		   uint32_t dif_flags, uint32_t ref_tag,
		   uint16_t apptag_mask, uint16_t app_tag)
{
	uint32_t block_size;

	if (metadata_size == 0) {
		return 0;
	}

	block_size = data_block_size + metadata_size;

	if (_are_iovs_bytes_multiple(iovs, iovcnt, block_size)) {
		return t10dif_verify(iovs, iovcnt, data_block_size, block_size,
				     dif_flags, ref_tag, apptag_mask, app_tag);
	} else {
		return t10dif_verify_split(iovs, iovcnt, data_block_size, block_size,
					   dif_flags, ref_tag, apptag_mask, app_tag);
	}
}

static void
t10dif_generate_copy(void *bounce_buf, struct iovec *iovs, int iovcnt,
		     uint32_t data_block_size, uint32_t block_size,
		     uint32_t dif_flags, uint32_t ref_tag, uint16_t app_tag)
{
	uint32_t iov_offset;
	int iovpos;
	void *buf;

	iovpos = 0;
	iov_offset = 0;

	while (iovpos < iovcnt) {
		/* copy data block */
		buf = iovs[iovpos].iov_base + iov_offset;
		memcpy(bounce_buf, buf, data_block_size);

		/* generate and append T10 DIF */
		_t10dif_generate(bounce_buf + data_block_size, bounce_buf,
				 data_block_size, dif_flags, ref_tag, app_tag);

		ref_tag++;
		iov_offset += data_block_size;
		if (iov_offset == iovs[iovpos].iov_len) {
			iovpos++;
			iov_offset = 0;
		}
		bounce_buf += block_size;
	}
}


static void
t10dif_generate_copy_split(void *bounce_buf, struct iovec *iovs, int iovcnt,
			   uint32_t data_block_size, uint32_t block_size,
			   uint32_t dif_flags, uint32_t ref_tag, uint16_t app_tag)
{
	uint32_t payload_offset, iov_offset, buf_len;
	uint32_t offset_blocks, offset_in_block, _ref_tag;
	int iovpos;
	void *buf;

	payload_offset = 0;
	iovpos = 0;
	iov_offset = 0;

	while (iovpos < iovcnt) {
		offset_blocks = payload_offset / data_block_size;
		offset_in_block = payload_offset % data_block_size;

		_ref_tag = ref_tag + offset_blocks;

		/* copy data block */
		buf = iovs[iovpos].iov_base + iov_offset;
		buf_len = spdk_min(iovs[iovpos].iov_len - iov_offset,
				   data_block_size - offset_in_block);

		memcpy(bounce_buf + offset_in_block, buf, buf_len);

		if (offset_in_block + buf_len == data_block_size) {
			/* generate and append T10 DIF */
			_t10dif_generate(bounce_buf + data_block_size, bounce_buf,
					 data_block_size, dif_flags, _ref_tag, app_tag);
			bounce_buf += block_size;
		}

		iov_offset += buf_len;
		if (iov_offset == iovs[iovpos].iov_len) {
			iovpos++;
			iov_offset = 0;
		}
		payload_offset += buf_len;
	}
}

int
spdk_t10dif_generate_copy(void *bounce_buf, uint32_t bounce_buf_len, struct iovec *iovs,
			  int iovcnt, uint32_t data_block_size, uint32_t metadata_size,
			  uint32_t dif_flags, uint32_t ref_tag, uint16_t app_tag)
{
	uint32_t required_len, block_size;

	block_size = data_block_size + metadata_size;
	required_len =  _get_iovs_len(iovs, iovcnt) * block_size / data_block_size;
	if (bounce_buf_len < required_len) {
		return -1;
	}

	if (metadata_size == 0) {
		_copy_iovs_to_buf(bounce_buf, bounce_buf_len, iovs, iovcnt);
		return 0;
	}

	if (_are_iovs_bytes_multiple(iovs, iovcnt, data_block_size)) {
		t10dif_generate_copy(bounce_buf, iovs, iovcnt, data_block_size,
				     block_size, dif_flags, ref_tag, app_tag);
	} else {
		t10dif_generate_copy_split(bounce_buf, iovs, iovcnt, data_block_size,
					   block_size, dif_flags, ref_tag, app_tag);
	}

	return 0;
}

static int
t10dif_verify_copy(struct iovec *iovs, int iovcnt, void *bounce_buf,
		   uint32_t data_block_size, uint32_t block_size, uint32_t dif_flags,
		   uint32_t ref_tag, uint16_t apptag_mask, uint16_t app_tag)
{
	uint32_t iov_offset;
	int iovpos, rc;

	iovpos = 0;
	iov_offset = 0;

	while (iovpos < iovcnt) {
		rc = _t10dif_verify(bounce_buf + data_block_size, bounce_buf, data_block_size,
				    dif_flags, ref_tag, apptag_mask, app_tag);
		if (rc != 0) {
			return rc;
		}

		memcpy(iovs[iovpos].iov_base + iov_offset, bounce_buf, data_block_size);

		ref_tag++;
		iov_offset += data_block_size;
		if (iov_offset == iovs[iovpos].iov_len) {
			iovpos++;
			iov_offset = 0;
		}
		bounce_buf += block_size;
	}

	return 0;
}

static int
t10dif_verify_copy_split(struct iovec *iovs, int iovcnt, void *bounce_buf,
			 uint32_t data_block_size, uint32_t block_size, uint32_t dif_flags,
			 uint32_t ref_tag, uint16_t apptag_mask, uint16_t app_tag)
{
	uint32_t payload_offset, offset_blocks, offset_in_block, iov_offset, buf_len;
	uint32_t _ref_tag;
	void *buf;
	int iovpos, rc;

	payload_offset = 0;
	iovpos = 0;
	iov_offset = 0;

	while (iovpos < iovcnt) {
		offset_blocks = payload_offset / data_block_size;
		offset_in_block = payload_offset % data_block_size;

		_ref_tag = ref_tag + offset_blocks;

		if (offset_in_block == 0) {
			rc = _t10dif_verify(bounce_buf + data_block_size, bounce_buf,
					    data_block_size, dif_flags,
					    _ref_tag, apptag_mask, app_tag);
			if (rc != 0) {
				return rc;
			}
		}

		buf = iovs[iovpos].iov_base + iov_offset;
		buf_len = spdk_min(iovs[iovpos].iov_len - iov_offset,
				   data_block_size - offset_in_block);

		memcpy(buf, bounce_buf + offset_in_block, buf_len);

		if (offset_in_block + buf_len == data_block_size) {
			bounce_buf += block_size;
		}

		iov_offset += buf_len;
		if (iov_offset == iovs[iovpos].iov_len) {
			iovpos++;
			iov_offset = 0;
		}
		payload_offset += buf_len;
	}

	return 0;
}

int
spdk_t10dif_verify_copy(struct iovec *iovs, int iovcnt, void *bounce_buf,
			uint32_t bounce_buf_len, uint32_t data_block_size,
			uint32_t metadata_size, uint32_t dif_flags,
			uint32_t ref_tag, uint16_t apptag_mask, uint16_t app_tag)
{
	uint32_t required_len, block_size;

	block_size = data_block_size + metadata_size;
	required_len =  _get_iovs_len(iovs, iovcnt) * block_size / data_block_size;

	if (bounce_buf_len < required_len) {
		return -1;
	}

	if (metadata_size == 0) {
		_copy_buf_to_iovs(iovs, iovcnt, bounce_buf, bounce_buf_len);
		return 0;
	}

	if (_are_iovs_bytes_multiple(iovs, iovcnt, data_block_size)) {
		return t10dif_verify_copy(iovs, iovcnt, bounce_buf,
					  data_block_size, block_size, dif_flags,
					  ref_tag, apptag_mask, app_tag);
	} else {
		return t10dif_verify_copy_split(iovs, iovcnt, bounce_buf,
						data_block_size, block_size, dif_flags,
						ref_tag, apptag_mask, app_tag);
	}
}
