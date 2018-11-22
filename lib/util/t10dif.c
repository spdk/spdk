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

#include "spdk/t10dif.h"

#include "spdk/crc16.h"
#include "spdk/endian.h"
#include "spdk/iovec.h"
#include "spdk/log.h"
#include "spdk/util.h"

/*
 * TODO: Format type that the data integrity field is transferred as the
 * last eight bytes of metadata.
 */

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
	struct spdk_iovec_iter iter;
	void *buf;

	spdk_iovec_iter_init(iter, iovs, iovcnt);

	spdk_iovec_iter_cont(iter) {
		buf = iter.iov->iov_base + iter.iov_offset;
		_t10dif_generate(buf + data_block_size, buf, data_block_size,
				 dif_flags, ref_tag, app_tag);
		ref_tag++;
		spdk_iovec_iter_advance(iter, block_size);
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

	if (spdk_iovec_has_granularity(iovs, iovcnt, block_size)) {
		t10dif_generate(iovs, iovcnt, data_block_size, block_size,
				dif_flags, ref_tag, app_tag);
	} else {
		assert(false);
	}
}

static int
_t10dif_verify(void *_dif, void *data_buf, uint32_t data_block_size,
	       uint32_t dif_flags, uint32_t ref_tag,
	       uint16_t apptag_mask, uint16_t app_tag)
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
	struct spdk_iovec_iter iter;
	int rc;
	void *buf;

	spdk_iovec_iter_init(iter, iovs, iovcnt);

	spdk_iovec_iter_cont(iter) {
		buf = iter.iov->iov_base + iter.iov_offset;
		rc = _t10dif_verify(buf + data_block_size, buf, data_block_size,
				    dif_flags, ref_tag, apptag_mask, app_tag);
		if (rc != 0) {
			return rc;
		}
		ref_tag++;
		spdk_iovec_iter_advance(iter, block_size);
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

	if (spdk_iovec_has_granularity(iovs, iovcnt, block_size)) {
		return t10dif_verify(iovs, iovcnt, data_block_size, block_size,
				     dif_flags, ref_tag, apptag_mask, app_tag);
	} else {
		return -1;
	}
}
