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

static void
_write_t10dif(void *_dif, uint32_t dif_flags, uint16_t guard, uint32_t ref_tag,
	      uint16_t app_tag)
{
	struct spdk_t10dif *dif = _dif;

	if (dif_flags & SPDK_T10DIF_GUARD_CHECK) {
		to_be16(&dif->guard, guard);
	}

	if (dif_flags & SPDK_T10DIF_APPTAG_CHECK) {
		to_be16(&dif->app_tag, app_tag);
	}

	if (dif_flags & SPDK_T10DIF_REFTAG_CHECK) {
		to_be32(&dif->ref_tag, ref_tag);
	}
}


static int
_t10dif_generate(struct iovec *iovs, int iovcnt,
		 uint32_t start_blocks, uint32_t num_blocks,
		 uint32_t data_block_size, uint32_t block_size,
		 uint32_t dif_flags, uint16_t app_tag)
{
	uint16_t guard;
	uint32_t offset_blocks, iov_offset, ref_tag;
	int iovpos;

	offset_blocks = 0;
	iovpos = 0;
	iov_offset = 0;

	while (offset_blocks < num_blocks && iovpos < iovcnt) {
		guard = spdk_crc16_t10dif(iovs[iovpos].iov_base + iov_offset,
					  data_block_size);
		ref_tag = start_blocks + offset_blocks;

		_write_t10dif(iovs[iovpos].iov_base + iov_offset + data_block_size,
			      dif_flags, guard, ref_tag, app_tag);

		iov_offset += block_size;
		if (iov_offset == iovs[iovpos].iov_len) {
			iovpos++;
			iov_offset = 0;
		}
		offset_blocks++;
	}

	if (offset_blocks == num_blocks) {
		return 0;
	} else {
		return -1;
	}
}

static int
_t10dif_generate_split(struct iovec *iovs, int iovcnt,
		       uint32_t start_blocks, uint32_t num_blocks,
		       uint32_t data_block_size, uint32_t block_size,
		       uint32_t dif_flags, uint16_t app_tag)
{
	uint32_t payload_size, payload_offset, iov_offset, buf_len;
	uint32_t offset_blocks, offset_in_block, ref_tag;
	void *buf;
	uint8_t contig_buf[4096], contig_dif[8];
	uint16_t guard;
	int iovpos;

	payload_size = num_blocks * block_size;
	payload_offset = 0;
	iovpos = 0;
	iov_offset = 0;

	while (payload_offset < payload_size && iovpos < iovcnt) {
		offset_blocks = payload_offset / block_size;
		offset_in_block = payload_offset % block_size;

		ref_tag = start_blocks + offset_blocks;

		buf = iovs[iovpos].iov_base + iov_offset;
		buf_len = spdk_min(payload_size - payload_offset,
				   iovs[iovpos].iov_len - iov_offset);

		if (offset_in_block < data_block_size) {
			buf_len = spdk_min(buf_len, data_block_size - offset_in_block);
			memcpy(&contig_buf[offset_in_block], buf, buf_len);

			if (offset_in_block + buf_len == data_block_size) {
				guard = spdk_crc16_t10dif(contig_buf, data_block_size);
				_write_t10dif(contig_dif, dif_flags, guard, ref_tag, app_tag);
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

	if (payload_offset == payload_size) {
		return 0;
	} else {
		return -1;
	}
}

int
spdk_t10dif_generate(struct iovec *iovs, int iovcnt,
		     uint32_t start_blocks, uint32_t num_blocks,
		     uint32_t data_block_size, uint32_t metadata_size,
		     uint32_t dif_flags, uint16_t app_tag)
{
	uint32_t block_size;

	if (metadata_size == 0) {
		return 0;
	}

	block_size = data_block_size + metadata_size;

	if (_are_iovs_bytes_multiple(iovs, iovcnt, block_size)) {
		return _t10dif_generate(iovs, iovcnt, start_blocks, num_blocks,
					data_block_size, block_size, dif_flags, app_tag);
	} else {
		return _t10dif_generate_split(iovs, iovcnt, start_blocks, num_blocks,
					      data_block_size, block_size, dif_flags, app_tag);
	}
}

static bool
_verify_t10dif(void *_dif, uint32_t dif_flags, uint16_t guard, uint32_t ref_tag,
	       uint16_t apptag_mask, uint16_t app_tag)
{
	struct spdk_t10dif *dif = _dif;
	uint16_t _guard, _app_tag;
	uint32_t _ref_tag;

	if (dif->app_tag == 0xFFFF) {
		return true;
	}

	if (dif_flags & SPDK_T10DIF_GUARD_CHECK) {
		_guard = from_be16(&dif->guard);
		if (_guard != guard) {
			SPDK_ERRLOG("Failed to compare Guard: LBA=%" PRIu32 ", Expected=%u, Actual=%u\n",
				    ref_tag, _guard, guard);
			return false;
		}
	}

	if (dif_flags & SPDK_T10DIF_APPTAG_CHECK) {
		_app_tag = from_be16(&dif->app_tag);
		if ((_app_tag & ~apptag_mask) != app_tag) {
			SPDK_ERRLOG("Failed to compare App Tag: LBA=%" PRIu32 ", Expected=%u, Actual=%u\n",
				    ref_tag, app_tag, (_app_tag & ~apptag_mask));
			return false;
		}
	}

	if (dif_flags & SPDK_T10DIF_REFTAG_CHECK) {
		_ref_tag = from_be32(&dif->ref_tag);
		if (_ref_tag != ref_tag) {
			SPDK_ERRLOG("Failed to compare Ref Tag: LBA=%" PRIu32 ", Expected=%u, Actual=%u\n",
				    ref_tag, ref_tag, _ref_tag);
			return false;
		}
	}

	return true;
}

static int
_t10dif_verify(struct iovec *iovs, int iovcnt,
	       uint32_t start_blocks, uint32_t num_blocks,
	       uint32_t data_block_size, uint32_t block_size,
	       uint32_t dif_flags, uint16_t apptag_mask, uint16_t app_tag)
{
	uint16_t guard;
	uint32_t offset_blocks, iov_offset, ref_tag;
	int iovpos;

	offset_blocks = 0;
	iovpos = 0;
	iov_offset = 0;

	while (offset_blocks < num_blocks && iovpos < iovcnt) {
		guard = spdk_crc16_t10dif(iovs[iovpos].iov_base + iov_offset,
					  data_block_size);
		ref_tag = start_blocks + offset_blocks;

		if (!_verify_t10dif(iovs[iovpos].iov_base + iov_offset + data_block_size,
				    dif_flags, guard, ref_tag, apptag_mask, app_tag)) {
			return -1;
		}

		iov_offset += block_size;
		if (iov_offset == iovs[iovpos].iov_len) {
			iovpos++;
			iov_offset = 0;
		}
		offset_blocks++;
	}

	if (offset_blocks == num_blocks) {
		return 0;
	} else {
		return -1;
	}
}

static int
_t10dif_verify_split(struct iovec *iovs, int iovcnt,
		     uint32_t start_blocks, uint32_t num_blocks,
		     uint32_t data_block_size, uint32_t block_size,
		     uint32_t dif_flags, uint16_t apptag_mask, uint16_t app_tag)
{
	uint32_t payload_size, payload_offset, iov_offset, buf_len;
	uint32_t offset_blocks, offset_in_block, ref_tag;
	void *buf;
	uint8_t contig_buf[4096], contig_dif[8];
	uint16_t guard = 0;
	int iovpos;

	payload_size = num_blocks * block_size;
	payload_offset = 0;
	iovpos = 0;
	iov_offset = 0;

	while (payload_offset < payload_size && iovpos < iovcnt) {
		offset_blocks = payload_offset / block_size;
		offset_in_block = payload_offset % block_size;

		ref_tag = start_blocks + offset_blocks;

		buf = iovs[iovpos].iov_base + iov_offset;
		buf_len = iovs[iovpos].iov_len - iov_offset;

		if (offset_in_block < data_block_size) {
			buf_len = spdk_min(buf_len, data_block_size - offset_in_block);
			memcpy(&contig_buf[offset_in_block], buf, buf_len);
			if (offset_in_block + buf_len == data_block_size) {
				guard = spdk_crc16_t10dif(contig_buf, data_block_size);
			}
		} else if (offset_in_block < data_block_size + 8) {
			buf_len = spdk_min(buf_len, data_block_size + 8 - offset_in_block);
			memcpy(&contig_dif[offset_in_block - data_block_size], buf, buf_len);
			if (offset_in_block + buf_len == data_block_size + 8) {
				if (!_verify_t10dif(contig_dif, dif_flags, guard, ref_tag,
						    apptag_mask, app_tag)) {
					return -1;
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
		payload_offset += buf_len;
	}

	if (payload_offset == payload_size) {
		return 0;
	} else {
		return -1;
	}
}

int
spdk_t10dif_verify(struct iovec *iovs, int iovcnt,
		   uint32_t start_blocks, uint32_t num_blocks,
		   uint32_t data_block_size, uint32_t metadata_size,
		   uint32_t dif_flags, uint16_t apptag_mask, uint16_t app_tag)
{
	uint32_t block_size;

	if (metadata_size == 0) {
		return 0;
	}

	block_size = data_block_size + metadata_size;

	if (_are_iovs_bytes_multiple(iovs, iovcnt, block_size)) {
		return _t10dif_verify(iovs, iovcnt, start_blocks, num_blocks,
				      data_block_size, block_size,
				      dif_flags, apptag_mask, app_tag);
	} else {
		return _t10dif_verify_split(iovs, iovcnt, start_blocks, num_blocks,
					    data_block_size, block_size,
					    dif_flags, apptag_mask, app_tag);
	}
}
