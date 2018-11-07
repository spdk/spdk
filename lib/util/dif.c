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

static void
setup_dif(void *_dif, uint32_t dif_chk_flags, uint16_t guard, uint32_t ref_tag,
	  uint16_t app_tag)
{
	struct spdk_t10_dif *dif = _dif;

	if (dif_chk_flags & SPDK_DIF_GUARD_CHECK) {
		to_be16(&dif->guard, guard);
	}

	if (dif_chk_flags & SPDK_DIF_APPTAG_CHECK) {
		to_be16(&dif->app_tag, app_tag);
	}

	if (dif_chk_flags & SPDK_DIF_REFTAG_CHECK) {
		to_be32(&dif->ref_tag, ref_tag);
	}
}

static void
prepare_and_update_dif(void *buf, uint32_t ref_tag, uint32_t data_block_size,
		       uint32_t dif_chk_flags, uint16_t app_tag)
{
	uint16_t guard;

	guard = spdk_crc16_t10dif(buf, data_block_size);
	setup_dif(buf + data_block_size, dif_chk_flags, guard, ref_tag, app_tag);
}

static uint32_t
prepare_dif(void *buf, uint32_t buf_len, uint8_t *bounce_buf, uint8_t *bounce_dif,
	    uint32_t offset_in_block, uint32_t ref_tag,
	    uint32_t data_block_size, uint32_t dif_chk_flags, uint16_t app_tag)
{
	uint32_t len;
	uint16_t guard;

	len = spdk_min(buf_len, data_block_size - offset_in_block);
	memcpy(&bounce_buf[offset_in_block], buf, len);

	if (offset_in_block + len == data_block_size) {
		guard = spdk_crc16_t10dif(bounce_buf, data_block_size);
		setup_dif(bounce_dif, dif_chk_flags, guard, ref_tag, app_tag);
	}

	return len;
}

static uint32_t
update_dif(void *buf, uint32_t buf_len, uint8_t *bounce_dif, uint32_t offset_in_block,
	   uint32_t data_block_size)
{
	uint32_t len;

	len = spdk_min(buf_len, data_block_size + 8 - offset_in_block);
	memcpy(buf, &bounce_dif[offset_in_block - data_block_size], len);

	return len;
}

int
spdk_t10dif_payload_setupv(struct iovec *iovs, int iovcnt,
			   uint32_t start_blocks, uint32_t num_blocks,
			   uint32_t data_block_size, uint32_t metadata_size,
			   uint32_t dif_chk_flags, uint16_t app_tag)
{
	uint32_t payload_size, payload_offset, iov_offset, buf_len, len;
	uint32_t block_size, offset_blocks, offset_in_block, ref_tag;
	int iovpos;
	void *buf;
	uint8_t bounce_buf[4096], bounce_dif[8];

	if (metadata_size == 0) {
		return 0;
	}

	block_size = data_block_size + metadata_size;
	payload_size = num_blocks * block_size;
	payload_offset = 0;
	iovpos = 0;
	iov_offset = 0;

	while (payload_offset < payload_size && iovpos < iovcnt) {
		offset_blocks = payload_offset / block_size;
		offset_in_block = payload_offset % block_size;

		ref_tag = start_blocks + offset_blocks;

		buf = (void *)(iovs[iovpos].iov_base + iov_offset);
		buf_len = spdk_min(payload_size - payload_offset, iovs[iovpos].iov_len - iov_offset);

		if (offset_in_block == 0 && buf_len >= block_size) {
			/* data block and metadata */
			prepare_and_update_dif(buf, ref_tag, data_block_size, dif_chk_flags, app_tag);
			len = block_size;
		} else if (offset_in_block < data_block_size) {
			/* data block */
			len = prepare_dif(buf, buf_len, bounce_buf, bounce_dif, offset_in_block,
					  ref_tag, data_block_size, dif_chk_flags, app_tag);
		} else if (offset_in_block < data_block_size + 8) {
			/* metadata */
			len = update_dif(buf, buf_len, bounce_dif, offset_in_block, data_block_size);
		} else {
			len = spdk_min(block_size - offset_in_block, buf_len);
		}

		iov_offset += len;
		if (iov_offset == iovs[iovpos].iov_len) {
			iovpos++;
			iov_offset = 0;
		}
		payload_offset += len;
	}

	if (payload_offset == payload_size) {
		return 0;
	} else {
		return -1;
	}
}

static bool
check_dif(void *_dif, uint32_t dif_chk_flags, uint16_t guard, uint32_t ref_tag,
	  uint16_t apptag_mask, uint16_t app_tag)
{
	struct spdk_t10_dif *dif = _dif;
	uint16_t _guard, _app_tag;
	uint32_t _ref_tag;

	if (dif->app_tag == 0xFFFF) {
		return true;
	}

	if (dif_chk_flags & SPDK_DIF_GUARD_CHECK) {
		_guard = from_be16(&dif->guard);
		if (_guard != guard) {
			SPDK_ERRLOG("Failed to compare Guard: LBA=%" PRIu32 ", Expected=%u, Actual=%u\n",
				    ref_tag, _guard, guard);
			return false;
		}
	}

	if (dif_chk_flags & SPDK_DIF_APPTAG_CHECK) {
		_app_tag = from_be16(&dif->app_tag);
		if ((_app_tag & ~apptag_mask) != app_tag) {
			SPDK_ERRLOG("Failed to compare App Tag: LBA=%" PRIu32 ", Expected=%u, Actual=%u\n",
				    ref_tag, app_tag, (_app_tag & ~apptag_mask));
			return false;
		}
	}

	if (dif_chk_flags & SPDK_DIF_REFTAG_CHECK) {
		_ref_tag = from_be32(&dif->ref_tag);
		if (_ref_tag != ref_tag) {
			SPDK_ERRLOG("Failed to compare Ref Tag: LBA=%" PRIu32 ", Expected=%u, Actual=%u\n",
				    ref_tag, ref_tag, _ref_tag);
			return false;
		}
	}

	return true;
}

int
spdk_t10dif_payload_checkv(struct iovec *iovs, int iovcnt,
			   uint32_t start_blocks, uint32_t num_blocks,
			   uint32_t data_block_size, uint32_t metadata_size,
			   uint32_t dif_chk_flags, uint16_t apptag_mask, uint16_t app_tag)
{
	uint32_t payload_size, payload_offset, iov_offset, buf_len, len;
	uint32_t block_size, offset_blocks, offset_in_block, ref_tag;
	int iovpos;
	void *buf;
	uint8_t bounce_buf[4096], bounce_dif[8];
	uint16_t guard = 0;

	if (metadata_size == 0) {
		return 0;
	}

	block_size = data_block_size + metadata_size;

	payload_size = num_blocks * block_size;
	payload_offset = 0;
	iovpos = 0;
	iov_offset = 0;

	while (payload_offset < payload_size && iovpos < iovcnt) {
		offset_blocks = payload_offset / block_size;
		offset_in_block = payload_offset % block_size;

		ref_tag = start_blocks + offset_blocks;

		buf = (void *)(iovs[iovpos].iov_base + iov_offset);
		buf_len = spdk_min(payload_size - payload_offset, iovs[iovpos].iov_len - iov_offset);

		if (offset_in_block == 0 && buf_len >= block_size) {
			/* data block and metadata */
			guard = spdk_crc16_t10dif(buf, data_block_size);
			if (!check_dif(buf + data_block_size, dif_chk_flags, guard, ref_tag,
				       apptag_mask, app_tag)) {
				return -1;
			}
			len = block_size;

		} else if (offset_in_block < data_block_size) {
			/* data block */
			len = spdk_min(buf_len, data_block_size - offset_in_block);
			memcpy(&bounce_buf[offset_in_block], buf, len);
			if (offset_in_block + len == data_block_size) {
				guard = spdk_crc16_t10dif(bounce_buf, data_block_size);
			}

		} else if (offset_in_block < data_block_size + 8) {
			/* metadata */
			len = spdk_min(buf_len, data_block_size + 8 - offset_in_block);
			memcpy(&bounce_dif[offset_in_block - data_block_size], buf, len);
			if (offset_in_block + len == data_block_size + 8) {
				if (!check_dif(bounce_dif, dif_chk_flags, guard, ref_tag,
					       apptag_mask, app_tag)) {
					return -1;
				}
			}
		} else {
			len = spdk_min(block_size - offset_in_block, buf_len);
		}

		iov_offset += len;
		if (iov_offset == iovs[iovpos].iov_len) {
			iovpos++;
			iov_offset = 0;
		}
		payload_offset += len;
	}

	if (payload_offset == payload_size) {
		return 0;
	} else {
		return -1;
	}
}
