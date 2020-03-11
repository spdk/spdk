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

#include "spdk/stdinc.h"

#include "spdk_cunit.h"

#include "util/dif.c"

#define DATA_PATTERN(offset)	((uint8_t)(0xAB + (offset)))
#define GUARD_SEED		0xCD

static int
ut_data_pattern_generate(struct iovec *iovs, int iovcnt,
			 uint32_t block_size, uint32_t md_size, uint32_t num_blocks)
{
	struct _dif_sgl sgl;
	uint32_t offset_blocks, offset_in_block, buf_len, data_offset, i;
	uint8_t *buf;

	_dif_sgl_init(&sgl, iovs, iovcnt);

	if (!_dif_sgl_is_valid(&sgl, block_size * num_blocks)) {
		return -1;
	}

	offset_blocks = 0;
	data_offset = 0;

	while (offset_blocks < num_blocks) {
		offset_in_block = 0;
		while (offset_in_block < block_size) {
			_dif_sgl_get_buf(&sgl, (void *)&buf, &buf_len);
			if (offset_in_block < block_size - md_size) {
				buf_len = spdk_min(buf_len,
						   block_size - md_size - offset_in_block);
				for (i = 0; i < buf_len; i++) {
					buf[i] = DATA_PATTERN(data_offset + i);
				}
				data_offset += buf_len;
			} else {
				buf_len = spdk_min(buf_len, block_size - offset_in_block);
				memset(buf, 0, buf_len);
			}
			_dif_sgl_advance(&sgl, buf_len);
			offset_in_block += buf_len;
		}
		offset_blocks++;
	}

	return 0;
}

static int
ut_data_pattern_verify(struct iovec *iovs, int iovcnt,
		       uint32_t block_size, uint32_t md_size, uint32_t num_blocks)
{
	struct _dif_sgl sgl;
	uint32_t offset_blocks, offset_in_block, buf_len, data_offset, i;
	uint8_t *buf;

	_dif_sgl_init(&sgl, iovs, iovcnt);

	if (!_dif_sgl_is_valid(&sgl, block_size * num_blocks)) {
		return -1;
	}

	offset_blocks = 0;
	data_offset = 0;

	while (offset_blocks < num_blocks) {
		offset_in_block = 0;
		while (offset_in_block < block_size) {
			_dif_sgl_get_buf(&sgl, (void *)&buf, &buf_len);

			if (offset_in_block < block_size - md_size) {
				buf_len = spdk_min(buf_len,
						   block_size - md_size - offset_in_block);
				for (i = 0; i < buf_len; i++) {
					if (buf[i] != DATA_PATTERN(data_offset + i)) {
						return -1;
					}
				}
				data_offset += buf_len;
			} else {
				buf_len = spdk_min(buf_len, block_size - offset_in_block);
			}
			_dif_sgl_advance(&sgl, buf_len);
			offset_in_block += buf_len;
		}
		offset_blocks++;
	}

	return 0;
}

static void
_iov_alloc_buf(struct iovec *iov, uint32_t len)
{
	iov->iov_base = calloc(1, len);
	iov->iov_len = len;
	SPDK_CU_ASSERT_FATAL(iov->iov_base != NULL);
}

static void
_iov_free_buf(struct iovec *iov)
{
	free(iov->iov_base);
}

static void
_iov_set_buf(struct iovec *iov, uint8_t *buf, uint32_t buf_len)
{
	iov->iov_base = buf;
	iov->iov_len = buf_len;
}

static bool
_iov_check(struct iovec *iov, void *iov_base, uint32_t iov_len)
{
	return (iov->iov_base == iov_base && iov->iov_len == iov_len);
}

static void
_dif_generate_and_verify(struct iovec *iov,
			 uint32_t block_size, uint32_t md_size, bool dif_loc,
			 enum spdk_dif_type dif_type, uint32_t dif_flags,
			 uint32_t ref_tag, uint32_t e_ref_tag,
			 uint16_t app_tag, uint16_t apptag_mask, uint16_t e_app_tag,
			 bool expect_pass)
{
	struct spdk_dif_ctx ctx = {};
	uint32_t guard_interval;
	uint16_t guard = 0;
	int rc;

	rc = ut_data_pattern_generate(iov, 1, block_size, md_size, 1);
	CU_ASSERT(rc == 0);

	guard_interval = _get_guard_interval(block_size, md_size, dif_loc, true);

	ctx.dif_type = dif_type;
	ctx.dif_flags = dif_flags;
	ctx.init_ref_tag = ref_tag;
	ctx.app_tag = app_tag;

	if (dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		guard = spdk_crc16_t10dif(0, iov->iov_base, guard_interval);
	}

	_dif_generate(iov->iov_base + guard_interval, guard, 0, &ctx);

	ctx.init_ref_tag = e_ref_tag;
	ctx.apptag_mask = apptag_mask;
	ctx.app_tag = e_app_tag;

	rc = _dif_verify(iov->iov_base + guard_interval, guard, 0, &ctx, NULL);
	CU_ASSERT((expect_pass && rc == 0) || (!expect_pass && rc != 0));

	rc = ut_data_pattern_verify(iov, 1, block_size, md_size, 1);
	CU_ASSERT(rc == 0);
}

static void
dif_generate_and_verify_test(void)
{
	struct iovec iov;
	uint32_t dif_flags;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	_iov_alloc_buf(&iov, 4096 + 128);

	/* Positive cases */

	/* The case that DIF is contained in the first 8 bytes of metadata. */
	_dif_generate_and_verify(&iov,
				 4096 + 128, 128, true,
				 SPDK_DIF_TYPE1, dif_flags,
				 22, 22,
				 0x22, 0xFFFF, 0x22,
				 true);

	/* The case that DIF is contained in the last 8 bytes of metadata. */
	_dif_generate_and_verify(&iov,
				 4096 + 128, 128, false,
				 SPDK_DIF_TYPE1, dif_flags,
				 22, 22,
				 0x22, 0xFFFF, 0x22,
				 true);

	/* Negative cases */

	/* Reference tag doesn't match. */
	_dif_generate_and_verify(&iov,
				 4096 + 128, 128, false,
				 SPDK_DIF_TYPE1, dif_flags,
				 22, 23,
				 0x22, 0xFFFF, 0x22,
				 false);

	/* Application tag doesn't match. */
	_dif_generate_and_verify(&iov,
				 4096 + 128, 128, false,
				 SPDK_DIF_TYPE1, dif_flags,
				 22, 22,
				 0x22, 0xFFFF, 0x23,
				 false);

	_iov_free_buf(&iov);
}

static void
dif_disable_check_test(void)
{
	struct iovec iov;
	uint32_t dif_flags;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	_iov_alloc_buf(&iov, 4096 + 128);

	/* The case that DIF check is disabled when the Application Tag is 0xFFFF for
	 * Type 1. DIF check is disabled and pass is expected.
	 */
	_dif_generate_and_verify(&iov,
				 4096 + 128, 128, false,
				 SPDK_DIF_TYPE1, dif_flags,
				 22, 22,
				 0xFFFF, 0xFFFF, 0x22,
				 true);

	/* The case that DIF check is not disabled when the Application Tag is 0xFFFF but
	 * the Reference Tag is not 0xFFFFFFFF for Type 3. DIF check is not disabled and
	 * fail is expected.
	 */
	_dif_generate_and_verify(&iov,
				 4096 + 128, 128, false,
				 SPDK_DIF_TYPE3, dif_flags,
				 22, 22,
				 0xFFFF, 0xFFFF, 0x22,
				 false);

	/* The case that DIF check is disabled when the Application Tag is 0xFFFF and
	 * the Reference Tag is 0xFFFFFFFF for Type 3. DIF check is disabled and
	 * pass is expected.
	 */
	_dif_generate_and_verify(&iov,
				 4096 + 128, 128, false,
				 SPDK_DIF_TYPE3, dif_flags,
				 0xFFFFFFFF, 22,
				 0xFFFF, 0xFFFF, 0x22,
				 true);

	_iov_free_buf(&iov);
}

static void
dif_sec_512_md_0_error_test(void)
{
	struct spdk_dif_ctx ctx = {};
	int rc;

	/* Metadata size is 0. */
	rc = spdk_dif_ctx_init(&ctx, 512, 0, true, false, SPDK_DIF_TYPE1, 0, 0, 0, 0, 0, 0);
	CU_ASSERT(rc != 0);
}

static void
dif_guard_seed_test(void)
{
	struct iovec iov;
	struct spdk_dif_ctx ctx = {};
	struct spdk_dif_error err_blk = {};
	struct spdk_dif *dif;
	uint16_t guard;
	int rc;

	_iov_alloc_buf(&iov, 512 + 8);

	memset(iov.iov_base, 0, 512 + 8);

	dif = (struct spdk_dif *)(iov.iov_base + 512);

	rc = spdk_dif_ctx_init(&ctx, 512 + 8, 8, true, false, SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK, 0, 0, 0, 0, 0);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_generate(&iov, 1, 1, &ctx);
	CU_ASSERT(rc == 0);

	/* Guard should be zero if the block is all zero and seed is not added. */
	guard = from_be16(&dif->guard);
	CU_ASSERT(guard == 0);

	rc = spdk_dif_verify(&iov, 1, 1, &ctx, &err_blk);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_ctx_init(&ctx, 512 + 8, 8, true, false, SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK, 0, 0, 0, 0, GUARD_SEED);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_generate(&iov, 1, 1, &ctx);
	CU_ASSERT(rc == 0);

	/* Guard should not be zero if the block is all zero but seed is added. */
	guard = from_be16(&dif->guard);
	CU_ASSERT(guard != 0);

	rc = spdk_dif_verify(&iov, 1, 1, &ctx, &err_blk);
	CU_ASSERT(rc == 0);

	_iov_free_buf(&iov);
}

static void
dif_generate_and_verify(struct iovec *iovs, int iovcnt,
			uint32_t block_size, uint32_t md_size, uint32_t num_blocks,
			bool dif_loc, enum spdk_dif_type dif_type, uint32_t dif_flags,
			uint32_t init_ref_tag, uint16_t apptag_mask, uint16_t app_tag)
{
	struct spdk_dif_ctx ctx = {};
	int rc;

	rc = ut_data_pattern_generate(iovs, iovcnt, block_size, md_size, num_blocks);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_ctx_init(&ctx, block_size, md_size, true, dif_loc, dif_type, dif_flags,
			       init_ref_tag, apptag_mask, app_tag, 0, GUARD_SEED);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_generate(iovs, iovcnt, num_blocks, &ctx);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_verify(iovs, iovcnt, num_blocks, &ctx, NULL);
	CU_ASSERT(rc == 0);

	rc = ut_data_pattern_verify(iovs, iovcnt, block_size, md_size, num_blocks);
	CU_ASSERT(rc == 0);
}

static void
dif_disable_sec_512_md_8_single_iov_test(void)
{
	struct iovec iov;

	_iov_alloc_buf(&iov, 512 + 8);

	dif_generate_and_verify(&iov, 1, 512 + 8, 8, 1, false, SPDK_DIF_DISABLE, 0, 0, 0, 0);

	_iov_free_buf(&iov);
}

static void
dif_sec_512_md_8_prchk_0_single_iov_test(void)
{
	struct iovec iov;

	_iov_alloc_buf(&iov, 512 + 8);

	dif_generate_and_verify(&iov, 1, 512 + 8, 8, 1, false, SPDK_DIF_TYPE1, 0, 0, 0, 0);

	_iov_free_buf(&iov);
}

static void
dif_sec_512_md_8_prchk_0_1_2_4_multi_iovs_test(void)
{
	struct iovec iovs[4];
	int i, num_blocks;

	num_blocks = 0;

	for (i = 0; i < 4; i++) {
		_iov_alloc_buf(&iovs[i], (512 + 8) * (i + 1));
		num_blocks += i + 1;
	}

	dif_generate_and_verify(iovs, 4, 512 + 8, 8, num_blocks, false, SPDK_DIF_TYPE1,
				0, 22, 0xFFFF, 0x22);

	dif_generate_and_verify(iovs, 4, 512 + 8, 8, num_blocks, false, SPDK_DIF_TYPE1,
				SPDK_DIF_FLAGS_GUARD_CHECK, 22, 0xFFFF, 0x22);

	dif_generate_and_verify(iovs, 4, 512 + 8, 8, num_blocks, false, SPDK_DIF_TYPE1,
				SPDK_DIF_FLAGS_APPTAG_CHECK, 22, 0xFFFF, 0x22);

	dif_generate_and_verify(iovs, 4, 512 + 8, 8, num_blocks, false, SPDK_DIF_TYPE1,
				SPDK_DIF_FLAGS_REFTAG_CHECK, 22, 0xFFFF, 0x22);

	for (i = 0; i < 4; i++) {
		_iov_free_buf(&iovs[i]);
	}
}

static void
dif_sec_4096_md_128_prchk_7_multi_iovs_test(void)
{
	struct iovec iovs[4];
	int i, num_blocks;
	uint32_t dif_flags;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	num_blocks = 0;

	for (i = 0; i < 4; i++) {
		_iov_alloc_buf(&iovs[i], (4096 + 128) * (i + 1));
		num_blocks += i + 1;
	}

	dif_generate_and_verify(iovs, 4, 4096 + 128, 128, num_blocks, false, SPDK_DIF_TYPE1,
				dif_flags, 22, 0xFFFF, 0x22);

	dif_generate_and_verify(iovs, 4, 4096 + 128, 128, num_blocks, true, SPDK_DIF_TYPE1,
				dif_flags, 22, 0xFFFF, 0x22);

	for (i = 0; i < 4; i++) {
		_iov_free_buf(&iovs[i]);
	}
}

static void
dif_sec_512_md_8_prchk_7_multi_iovs_split_data_and_md_test(void)
{
	struct iovec iovs[2];
	uint32_t dif_flags;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	_iov_alloc_buf(&iovs[0], 512);
	_iov_alloc_buf(&iovs[1], 8);

	dif_generate_and_verify(iovs, 2, 512 + 8, 8, 1, false, SPDK_DIF_TYPE1,
				dif_flags, 22, 0xFFFF, 0x22);

	_iov_free_buf(&iovs[0]);
	_iov_free_buf(&iovs[1]);
}

static void
dif_sec_512_md_8_prchk_7_multi_iovs_split_data_test(void)
{
	struct iovec iovs[2];
	uint32_t dif_flags;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	_iov_alloc_buf(&iovs[0], 256);
	_iov_alloc_buf(&iovs[1], 264);

	dif_generate_and_verify(iovs, 2, 512 + 8, 8, 1, false, SPDK_DIF_TYPE1,
				dif_flags, 22, 0xFFFF, 0x22);

	_iov_free_buf(&iovs[0]);
	_iov_free_buf(&iovs[1]);
}

static void
dif_sec_512_md_8_prchk_7_multi_iovs_split_guard_test(void)
{
	struct iovec iovs[2];
	uint32_t dif_flags;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	_iov_alloc_buf(&iovs[0], 513);
	_iov_alloc_buf(&iovs[1], 7);

	dif_generate_and_verify(iovs, 2, 512 + 8, 8, 1, false, SPDK_DIF_TYPE1,
				dif_flags, 22, 0xFFFF, 0x22);

	_iov_free_buf(&iovs[0]);
	_iov_free_buf(&iovs[1]);
}

static void
dif_sec_512_md_8_prchk_7_multi_iovs_split_apptag_test(void)
{
	struct iovec iovs[2];
	uint32_t dif_flags;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	_iov_alloc_buf(&iovs[0], 515);
	_iov_alloc_buf(&iovs[1], 5);

	dif_generate_and_verify(iovs, 2, 512 + 8, 8, 1, false, SPDK_DIF_TYPE1,
				dif_flags, 22, 0xFFFF, 0x22);

	_iov_free_buf(&iovs[0]);
	_iov_free_buf(&iovs[1]);
}

static void
dif_sec_512_md_8_prchk_7_multi_iovs_split_reftag_test(void)
{
	struct iovec iovs[2];
	uint32_t dif_flags;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	_iov_alloc_buf(&iovs[0], 518);
	_iov_alloc_buf(&iovs[1], 2);

	dif_generate_and_verify(iovs, 2, 512 + 8, 8, 1, false, SPDK_DIF_TYPE1,
				dif_flags, 22, 0xFFFF, 0x22);

	_iov_free_buf(&iovs[0]);
	_iov_free_buf(&iovs[1]);
}

static void
dif_sec_512_md_8_prchk_7_multi_iovs_complex_splits_test(void)
{
	struct iovec iovs[9];
	uint32_t dif_flags;
	int i;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	/* data[0][255:0] */
	_iov_alloc_buf(&iovs[0], 256);

	/* data[0][511:256], guard[0][0] */
	_iov_alloc_buf(&iovs[1], 256 + 1);

	/* guard[0][1], apptag[0][0] */
	_iov_alloc_buf(&iovs[2], 1 + 1);

	/* apptag[0][1], reftag[0][0] */
	_iov_alloc_buf(&iovs[3], 1 + 1);

	/* reftag[0][3:1], data[1][255:0] */
	_iov_alloc_buf(&iovs[4], 3 + 256);

	/* data[1][511:256], guard[1][0] */
	_iov_alloc_buf(&iovs[5], 256 + 1);

	/* guard[1][1], apptag[1][0] */
	_iov_alloc_buf(&iovs[6], 1 + 1);

	/* apptag[1][1], reftag[1][0] */
	_iov_alloc_buf(&iovs[7], 1 + 1);

	/* reftag[1][3:1] */
	_iov_alloc_buf(&iovs[8], 3);

	dif_generate_and_verify(iovs, 9, 512 + 8, 8, 2, false, SPDK_DIF_TYPE1, dif_flags,
				22, 0xFFFF, 0x22);

	for (i = 0; i < 9; i++) {
		_iov_free_buf(&iovs[i]);
	}
}

static void
dif_sec_4096_md_128_prchk_7_multi_iovs_complex_splits_test(void)
{
	struct iovec iovs[11];
	uint32_t dif_flags;
	int i;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	/* data[0][1000:0] */
	_iov_alloc_buf(&iovs[0], 1000);

	/* data[0][3095:1000], guard[0][0] */
	_iov_alloc_buf(&iovs[1], 3096 + 1);

	/* guard[0][1], apptag[0][0] */
	_iov_alloc_buf(&iovs[2], 1 + 1);

	/* apptag[0][1], reftag[0][0] */
	_iov_alloc_buf(&iovs[3], 1 + 1);

	/* reftag[0][3:1], ignore[0][59:0] */
	_iov_alloc_buf(&iovs[4], 3 + 60);

	/* ignore[119:60], data[1][3050:0] */
	_iov_alloc_buf(&iovs[5], 60 + 3051);

	/* data[1][4095:3050], guard[1][0] */
	_iov_alloc_buf(&iovs[6], 1045 + 1);

	/* guard[1][1], apptag[1][0] */
	_iov_alloc_buf(&iovs[7], 1 + 1);

	/* apptag[1][1], reftag[1][0] */
	_iov_alloc_buf(&iovs[8], 1 + 1);

	/* reftag[1][3:1], ignore[1][9:0] */
	_iov_alloc_buf(&iovs[9], 3 + 10);

	/* ignore[1][127:9] */
	_iov_alloc_buf(&iovs[10], 118);

	dif_generate_and_verify(iovs, 11, 4096 + 128, 128, 2, false, SPDK_DIF_TYPE1, dif_flags,
				22, 0xFFFF, 0x22);
	dif_generate_and_verify(iovs, 11, 4096 + 128, 128, 2, true, SPDK_DIF_TYPE1, dif_flags,
				22, 0xFFFF, 0x22);

	for (i = 0; i < 11; i++) {
		_iov_free_buf(&iovs[i]);
	}
}

static void
_dif_inject_error_and_verify(struct iovec *iovs, int iovcnt,
			     uint32_t block_size, uint32_t md_size, uint32_t num_blocks,
			     uint32_t inject_flags, bool dif_loc)
{
	struct spdk_dif_ctx ctx = {};
	struct spdk_dif_error err_blk = {};
	uint32_t inject_offset = 0, dif_flags;
	int rc;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	rc = ut_data_pattern_generate(iovs, iovcnt, block_size, md_size, num_blocks);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_ctx_init(&ctx, block_size, md_size, true, dif_loc,
			       SPDK_DIF_TYPE1, dif_flags, 88, 0xFFFF, 0x88, 0, GUARD_SEED);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_generate(iovs, iovcnt, num_blocks, &ctx);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_inject_error(iovs, iovcnt, num_blocks, &ctx, inject_flags, &inject_offset);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_verify(iovs, iovcnt, num_blocks, &ctx, &err_blk);
	CU_ASSERT(rc != 0);
	if (inject_flags == SPDK_DIF_DATA_ERROR) {
		CU_ASSERT(SPDK_DIF_GUARD_ERROR == err_blk.err_type);
	} else {
		CU_ASSERT(inject_flags == err_blk.err_type);
	}
	CU_ASSERT(inject_offset == err_blk.err_offset);

	rc = ut_data_pattern_verify(iovs, iovcnt, block_size, md_size, num_blocks);
	CU_ASSERT((rc == 0 && (inject_flags != SPDK_DIF_DATA_ERROR)) ||
		  (rc != 0 && (inject_flags == SPDK_DIF_DATA_ERROR)));
}

static void
dif_inject_error_and_verify(struct iovec *iovs, int iovcnt,
			    uint32_t block_size, uint32_t md_size, uint32_t num_blocks,
			    uint32_t inject_flags)
{
	/* The case that DIF is contained in the first 8 bytes of metadata. */
	_dif_inject_error_and_verify(iovs, iovcnt, block_size, md_size, num_blocks,
				     inject_flags, true);

	/* The case that DIF is contained in the last 8 bytes of metadata. */
	_dif_inject_error_and_verify(iovs, iovcnt, block_size, md_size, num_blocks,
				     inject_flags, false);
}

static void
dif_sec_4096_md_128_inject_1_2_4_8_multi_iovs_test(void)
{
	struct iovec iovs[4];
	int i, num_blocks;

	num_blocks = 0;

	for (i = 0; i < 4; i++) {
		_iov_alloc_buf(&iovs[i], (4096 + 128) * (i + 1));
		num_blocks += i + 1;
	}

	dif_inject_error_and_verify(iovs, 4, 4096 + 128, 128, num_blocks, SPDK_DIF_GUARD_ERROR);
	dif_inject_error_and_verify(iovs, 4, 4096 + 128, 128, num_blocks, SPDK_DIF_APPTAG_ERROR);
	dif_inject_error_and_verify(iovs, 4, 4096 + 128, 128, num_blocks, SPDK_DIF_REFTAG_ERROR);
	dif_inject_error_and_verify(iovs, 4, 4096 + 128, 128, num_blocks, SPDK_DIF_DATA_ERROR);

	for (i = 0; i < 4; i++) {
		_iov_free_buf(&iovs[i]);
	}
}

static void
dif_sec_4096_md_128_inject_1_2_4_8_multi_iovs_split_data_and_md_test(void)
{
	struct iovec iovs[2];

	_iov_alloc_buf(&iovs[0], 4096);
	_iov_alloc_buf(&iovs[1], 128);

	dif_inject_error_and_verify(iovs, 2, 4096 + 128, 128, 1, SPDK_DIF_GUARD_ERROR);
	dif_inject_error_and_verify(iovs, 2, 4096 + 128, 128, 1, SPDK_DIF_APPTAG_ERROR);
	dif_inject_error_and_verify(iovs, 2, 4096 + 128, 128, 1, SPDK_DIF_REFTAG_ERROR);
	dif_inject_error_and_verify(iovs, 2, 4096 + 128, 128, 1, SPDK_DIF_DATA_ERROR);

	_iov_free_buf(&iovs[0]);
	_iov_free_buf(&iovs[1]);
}

static void
dif_sec_4096_md_128_inject_1_2_4_8_multi_iovs_split_data_test(void)
{
	struct iovec iovs[2];

	_iov_alloc_buf(&iovs[0], 2048);
	_iov_alloc_buf(&iovs[1], 2048 + 128);

	dif_inject_error_and_verify(iovs, 2, 4096 + 128, 128, 1, SPDK_DIF_GUARD_ERROR);
	dif_inject_error_and_verify(iovs, 2, 4096 + 128, 128, 1, SPDK_DIF_APPTAG_ERROR);
	dif_inject_error_and_verify(iovs, 2, 4096 + 128, 128, 1, SPDK_DIF_REFTAG_ERROR);
	dif_inject_error_and_verify(iovs, 2, 4096 + 128, 128, 1, SPDK_DIF_DATA_ERROR);

	_iov_free_buf(&iovs[0]);
	_iov_free_buf(&iovs[1]);
}

static void
dif_sec_4096_md_128_inject_1_2_4_8_multi_iovs_split_guard_test(void)
{
	struct iovec iovs[2];

	_iov_alloc_buf(&iovs[0], 4096 + 1);
	_iov_alloc_buf(&iovs[1], 127);

	dif_inject_error_and_verify(iovs, 2, 4096 + 128, 128, 1, SPDK_DIF_GUARD_ERROR);
	dif_inject_error_and_verify(iovs, 2, 4096 + 128, 128, 1, SPDK_DIF_APPTAG_ERROR);
	dif_inject_error_and_verify(iovs, 2, 4096 + 128, 128, 1, SPDK_DIF_REFTAG_ERROR);
	dif_inject_error_and_verify(iovs, 2, 4096 + 128, 128, 1, SPDK_DIF_DATA_ERROR);

	_iov_free_buf(&iovs[0]);
	_iov_free_buf(&iovs[1]);
}

static void
dif_sec_4096_md_128_inject_1_2_4_8_multi_iovs_split_apptag_test(void)
{
	struct iovec iovs[2];

	_iov_alloc_buf(&iovs[0], 4096 + 3);
	_iov_alloc_buf(&iovs[1], 125);

	dif_inject_error_and_verify(iovs, 2, 4096 + 128, 128, 1, SPDK_DIF_GUARD_ERROR);
	dif_inject_error_and_verify(iovs, 2, 4096 + 128, 128, 1, SPDK_DIF_APPTAG_ERROR);
	dif_inject_error_and_verify(iovs, 2, 4096 + 128, 128, 1, SPDK_DIF_REFTAG_ERROR);
	dif_inject_error_and_verify(iovs, 2, 4096 + 128, 128, 1, SPDK_DIF_DATA_ERROR);

	_iov_free_buf(&iovs[0]);
	_iov_free_buf(&iovs[1]);
}

static void
dif_sec_4096_md_128_inject_1_2_4_8_multi_iovs_split_reftag_test(void)
{
	struct iovec iovs[2];

	_iov_alloc_buf(&iovs[0], 4096 + 6);
	_iov_alloc_buf(&iovs[1], 122);

	dif_inject_error_and_verify(iovs, 2, 4096 + 128, 128, 1, SPDK_DIF_GUARD_ERROR);
	dif_inject_error_and_verify(iovs, 2, 4096 + 128, 128, 1, SPDK_DIF_APPTAG_ERROR);
	dif_inject_error_and_verify(iovs, 2, 4096 + 128, 128, 1, SPDK_DIF_REFTAG_ERROR);
	dif_inject_error_and_verify(iovs, 2, 4096 + 128, 128, 1, SPDK_DIF_DATA_ERROR);

	_iov_free_buf(&iovs[0]);
	_iov_free_buf(&iovs[1]);
}

static void
dif_copy_gen_and_verify(struct iovec *iovs, int iovcnt, struct iovec *bounce_iov,
			uint32_t block_size, uint32_t md_size, uint32_t num_blocks,
			bool dif_loc, enum spdk_dif_type dif_type, uint32_t dif_flags,
			uint32_t init_ref_tag, uint16_t apptag_mask, uint16_t app_tag)
{
	struct spdk_dif_ctx ctx = {};
	int rc;

	rc = ut_data_pattern_generate(iovs, iovcnt, block_size - md_size, 0, num_blocks);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_ctx_init(&ctx, block_size, md_size, true, dif_loc, dif_type, dif_flags,
			       init_ref_tag, apptag_mask, app_tag, 0, GUARD_SEED);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_generate_copy(iovs, iovcnt, bounce_iov, num_blocks, &ctx);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_verify_copy(iovs, iovcnt, bounce_iov, num_blocks, &ctx, NULL);
	CU_ASSERT(rc == 0);

	rc = ut_data_pattern_verify(iovs, iovcnt, block_size - md_size, 0, num_blocks);
	CU_ASSERT(rc == 0);
}

static void
dif_copy_sec_512_md_8_prchk_0_single_iov(void)
{
	struct iovec iov, bounce_iov;

	_iov_alloc_buf(&iov, 512 * 4);
	_iov_alloc_buf(&bounce_iov, (512 + 8) * 4);

	dif_copy_gen_and_verify(&iov, 1, &bounce_iov, 512 + 8, 8, 4,
				false, SPDK_DIF_TYPE1, 0, 0, 0, 0);
	dif_copy_gen_and_verify(&iov, 1, &bounce_iov, 512 + 8, 8, 4,
				true, SPDK_DIF_TYPE1, 0, 0, 0, 0);

	_iov_free_buf(&iov);
	_iov_free_buf(&bounce_iov);
}

static void
dif_copy_sec_512_md_8_prchk_0_1_2_4_multi_iovs(void)
{
	struct iovec iovs[4], bounce_iov;
	int i, num_blocks;

	num_blocks = 0;

	for (i = 0; i < 4; i++) {
		_iov_alloc_buf(&iovs[i], 512 * (i + 1));
		num_blocks += i + 1;
	}

	_iov_alloc_buf(&bounce_iov, (512 + 8) * num_blocks);

	dif_copy_gen_and_verify(iovs, 4, &bounce_iov, 512 + 8, 8, num_blocks,
				false, SPDK_DIF_TYPE1, 0, 22, 0xFFFF, 0x22);

	dif_copy_gen_and_verify(iovs, 4, &bounce_iov, 512 + 8, 8, num_blocks,
				false, SPDK_DIF_TYPE1, SPDK_DIF_FLAGS_GUARD_CHECK, 22, 0xFFFF, 0x22);

	dif_copy_gen_and_verify(iovs, 4, &bounce_iov, 512 + 8, 8, num_blocks,
				false, SPDK_DIF_TYPE1, SPDK_DIF_FLAGS_APPTAG_CHECK, 22, 0xFFFF, 0x22);

	dif_copy_gen_and_verify(iovs, 4, &bounce_iov, 512 + 8, 8, num_blocks,
				false, SPDK_DIF_TYPE1, SPDK_DIF_FLAGS_REFTAG_CHECK, 22, 0xFFFF, 0x22);

	for (i = 0; i < 4; i++) {
		_iov_free_buf(&iovs[i]);
	}
	_iov_free_buf(&bounce_iov);
}

static void
dif_copy_sec_4096_md_128_prchk_7_multi_iovs(void)
{
	struct iovec iovs[4], bounce_iov;
	uint32_t dif_flags;
	int i, num_blocks;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	num_blocks = 0;

	for (i = 0; i < 4; i++) {
		_iov_alloc_buf(&iovs[i], 4096 * (i + 1));
		num_blocks += i + 1;
	}

	_iov_alloc_buf(&bounce_iov, (4096 + 128) * num_blocks);

	dif_copy_gen_and_verify(iovs, 4, &bounce_iov, 4096 + 128, 128, num_blocks,
				false, SPDK_DIF_TYPE1, dif_flags, 22, 0xFFFF, 0x22);
	dif_copy_gen_and_verify(iovs, 4, &bounce_iov, 4096 + 128, 128, num_blocks,
				true, SPDK_DIF_TYPE1, dif_flags, 22, 0xFFFF, 0x22);

	for (i = 0; i < 4; i++) {
		_iov_free_buf(&iovs[i]);
	}
	_iov_free_buf(&bounce_iov);
}

static void
dif_copy_sec_512_md_8_prchk_7_multi_iovs_split_data(void)
{
	struct iovec iovs[2], bounce_iov;
	uint32_t dif_flags;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	_iov_alloc_buf(&iovs[0], 256);
	_iov_alloc_buf(&iovs[1], 256);

	_iov_alloc_buf(&bounce_iov, 512 + 8);

	dif_copy_gen_and_verify(iovs, 2, &bounce_iov, 512 + 8, 8, 1,
				false, SPDK_DIF_TYPE1, dif_flags, 22, 0xFFFF, 0x22);

	_iov_free_buf(&iovs[0]);
	_iov_free_buf(&iovs[1]);
	_iov_free_buf(&bounce_iov);
}

static void
dif_copy_sec_512_md_8_prchk_7_multi_iovs_complex_splits(void)
{
	struct iovec iovs[6], bounce_iov;
	uint32_t dif_flags;
	int i;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	/* data[0][255:0] */
	_iov_alloc_buf(&iovs[0], 256);

	/* data[0][511:256], data[1][255:0] */
	_iov_alloc_buf(&iovs[1], 256 + 256);

	/* data[1][382:256] */
	_iov_alloc_buf(&iovs[2], 128);

	/* data[1][383] */
	_iov_alloc_buf(&iovs[3], 1);

	/* data[1][510:384] */
	_iov_alloc_buf(&iovs[4], 126);

	/* data[1][511], data[2][511:0], data[3][511:0] */
	_iov_alloc_buf(&iovs[5], 1 + 512 * 2);

	_iov_alloc_buf(&bounce_iov, (512 + 8) * 4);

	dif_copy_gen_and_verify(iovs, 6, &bounce_iov, 512 + 8, 8, 4,
				true, SPDK_DIF_TYPE1, dif_flags, 22, 0xFFFF, 0x22);

	for (i = 0; i < 6; i++) {
		_iov_free_buf(&iovs[i]);
	}
	_iov_free_buf(&bounce_iov);
}

static void
_dif_copy_inject_error_and_verify(struct iovec *iovs, int iovcnt, struct iovec *bounce_iov,
				  uint32_t block_size, uint32_t md_size, uint32_t num_blocks,
				  uint32_t inject_flags, bool dif_loc)
{
	struct spdk_dif_ctx ctx = {};
	struct spdk_dif_error err_blk = {};
	uint32_t inject_offset = 0, dif_flags;
	int rc;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	rc = ut_data_pattern_generate(iovs, iovcnt, block_size - md_size, 0, num_blocks);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_ctx_init(&ctx, block_size, md_size, true, dif_loc, SPDK_DIF_TYPE1, dif_flags,
			       88, 0xFFFF, 0x88, 0, GUARD_SEED);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dif_generate_copy(iovs, iovcnt, bounce_iov, num_blocks, &ctx);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_inject_error(bounce_iov, 1, num_blocks, &ctx, inject_flags, &inject_offset);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_verify_copy(iovs, iovcnt, bounce_iov, num_blocks, &ctx, &err_blk);
	CU_ASSERT(rc != 0);
	if (inject_flags == SPDK_DIF_DATA_ERROR) {
		CU_ASSERT(SPDK_DIF_GUARD_ERROR == err_blk.err_type);
	} else {
		CU_ASSERT(inject_flags == err_blk.err_type);
	}
	CU_ASSERT(inject_offset == err_blk.err_offset);
}

static void
dif_copy_inject_error_and_verify(struct iovec *iovs, int iovcnt, struct iovec *bounce_iov,
				 uint32_t block_size, uint32_t md_size, uint32_t num_blocks,
				 uint32_t inject_flags)
{
	/* The case that DIF is contained in the first 8 bytes of metadata. */
	_dif_copy_inject_error_and_verify(iovs, iovcnt, bounce_iov,
					  block_size, md_size, num_blocks,
					  inject_flags, true);

	/* The case that DIF is contained in the last 8 bytes of metadata. */
	_dif_copy_inject_error_and_verify(iovs, iovcnt, bounce_iov,
					  block_size, md_size, num_blocks,
					  inject_flags, false);
}

static void
dif_copy_sec_4096_md_128_inject_1_2_4_8_multi_iovs_test(void)
{
	struct iovec iovs[4], bounce_iov;
	int i, num_blocks;

	num_blocks = 0;

	for (i = 0; i < 4; i++) {
		_iov_alloc_buf(&iovs[i], 4096 * (i + 1));
		num_blocks += i + 1;
	}

	_iov_alloc_buf(&bounce_iov, (4096 + 128) * num_blocks);

	dif_copy_inject_error_and_verify(iovs, 4, &bounce_iov, 4096 + 128, 128,
					 num_blocks, SPDK_DIF_GUARD_ERROR);

	dif_copy_inject_error_and_verify(iovs, 4, &bounce_iov, 4096 + 128, 128,
					 num_blocks, SPDK_DIF_APPTAG_ERROR);

	dif_copy_inject_error_and_verify(iovs, 4, &bounce_iov, 4096 + 128, 128,
					 num_blocks, SPDK_DIF_REFTAG_ERROR);

	dif_copy_inject_error_and_verify(iovs, 4, &bounce_iov, 4096 + 128, 128,
					 num_blocks, SPDK_DIF_DATA_ERROR);

	for (i = 0; i < 4; i++) {
		_iov_free_buf(&iovs[i]);
	}
	_iov_free_buf(&bounce_iov);
}

static void
dif_copy_sec_4096_md_128_inject_1_2_4_8_multi_iovs_split_test(void)
{
	struct iovec iovs[4], bounce_iov;
	int i;

	_iov_alloc_buf(&iovs[0], 2048);
	_iov_alloc_buf(&iovs[1], 2048);
	_iov_alloc_buf(&iovs[2], 1);
	_iov_alloc_buf(&iovs[3], 4095);

	_iov_alloc_buf(&bounce_iov, (4096 + 128) * 2);

	dif_copy_inject_error_and_verify(iovs, 4, &bounce_iov, 4096 + 128, 128,
					 2, SPDK_DIF_GUARD_ERROR);

	dif_copy_inject_error_and_verify(iovs, 4, &bounce_iov, 4096 + 128, 128,
					 2, SPDK_DIF_APPTAG_ERROR);

	dif_copy_inject_error_and_verify(iovs, 4, &bounce_iov, 4096 + 128, 128,
					 2, SPDK_DIF_REFTAG_ERROR);

	dif_copy_inject_error_and_verify(iovs, 4, &bounce_iov, 4096 + 128, 128,
					 2, SPDK_DIF_DATA_ERROR);

	for (i = 0; i < 4; i++) {
		_iov_free_buf(&iovs[i]);
	}
	_iov_free_buf(&bounce_iov);
}

static void
dix_sec_512_md_0_error(void)
{
	struct spdk_dif_ctx ctx;
	int rc;

	rc = spdk_dif_ctx_init(&ctx, 512, 0, false, false, SPDK_DIF_TYPE1, 0, 0, 0, 0, 0, 0);
	CU_ASSERT(rc != 0);
}

static void
dix_generate_and_verify(struct iovec *iovs, int iovcnt, struct iovec *md_iov,
			uint32_t block_size, uint32_t md_size, uint32_t num_blocks,
			bool dif_loc, enum spdk_dif_type dif_type, uint32_t dif_flags,
			uint32_t init_ref_tag, uint16_t apptag_mask, uint16_t app_tag)
{
	struct spdk_dif_ctx ctx;
	int rc;

	rc = ut_data_pattern_generate(iovs, iovcnt, block_size, 0, num_blocks);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_ctx_init(&ctx, block_size, md_size, false, dif_loc, dif_type, dif_flags,
			       init_ref_tag, apptag_mask, app_tag, 0, GUARD_SEED);
	CU_ASSERT(rc == 0);

	rc = spdk_dix_generate(iovs, iovcnt, md_iov, num_blocks, &ctx);
	CU_ASSERT(rc == 0);

	rc = spdk_dix_verify(iovs, iovcnt, md_iov, num_blocks, &ctx, NULL);
	CU_ASSERT(rc == 0);

	rc = ut_data_pattern_verify(iovs, iovcnt, block_size, 0, num_blocks);
	CU_ASSERT(rc == 0);
}

static void
dix_sec_512_md_8_prchk_0_single_iov(void)
{
	struct iovec iov, md_iov;

	_iov_alloc_buf(&iov, 512 * 4);
	_iov_alloc_buf(&md_iov, 8 * 4);

	dix_generate_and_verify(&iov, 1, &md_iov, 512, 8, 4, false, SPDK_DIF_TYPE1, 0, 0, 0, 0);
	dix_generate_and_verify(&iov, 1, &md_iov, 512, 8, 4, true, SPDK_DIF_TYPE1, 0, 0, 0, 0);

	_iov_free_buf(&iov);
	_iov_free_buf(&md_iov);
}

static void
dix_sec_512_md_8_prchk_0_1_2_4_multi_iovs(void)
{
	struct iovec iovs[4], md_iov;
	int i, num_blocks;

	num_blocks = 0;

	for (i = 0; i < 4; i++) {
		_iov_alloc_buf(&iovs[i], 512 * (i + 1));
		num_blocks += i + 1;
	}
	_iov_alloc_buf(&md_iov, 8 * num_blocks);

	dix_generate_and_verify(iovs, 4, &md_iov, 512, 8, num_blocks, false, SPDK_DIF_TYPE1,
				0, 22, 0xFFFF, 0x22);

	dix_generate_and_verify(iovs, 4, &md_iov, 512, 8, num_blocks, false, SPDK_DIF_TYPE1,
				SPDK_DIF_FLAGS_GUARD_CHECK, 22, 0xFFFF, 0x22);

	dix_generate_and_verify(iovs, 4, &md_iov, 512, 8, num_blocks, false, SPDK_DIF_TYPE1,
				SPDK_DIF_FLAGS_APPTAG_CHECK, 22, 0xFFFF, 0x22);

	dix_generate_and_verify(iovs, 4, &md_iov, 512, 8, num_blocks, false, SPDK_DIF_TYPE1,
				SPDK_DIF_FLAGS_REFTAG_CHECK, 22, 0xFFFF, 0x22);

	for (i = 0; i < 4; i++) {
		_iov_free_buf(&iovs[i]);
	}
	_iov_free_buf(&md_iov);
}

static void
dix_sec_4096_md_128_prchk_7_multi_iovs(void)
{
	struct iovec iovs[4], md_iov;
	uint32_t dif_flags;
	int i, num_blocks;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	num_blocks = 0;

	for (i = 0; i < 4; i++) {
		_iov_alloc_buf(&iovs[i], 4096 * (i + 1));
		num_blocks += i + 1;
	}
	_iov_alloc_buf(&md_iov, 128 * num_blocks);

	dix_generate_and_verify(iovs, 4, &md_iov, 4096, 128, num_blocks, false, SPDK_DIF_TYPE1,
				dif_flags, 22, 0xFFFF, 0x22);
	dix_generate_and_verify(iovs, 4, &md_iov, 4096, 128, num_blocks, true, SPDK_DIF_TYPE1,
				dif_flags, 22, 0xFFFF, 0x22);

	for (i = 0; i < 4; i++) {
		_iov_free_buf(&iovs[i]);
	}
	_iov_free_buf(&md_iov);
}

static void
dix_sec_512_md_8_prchk_7_multi_iovs_split_data(void)
{
	struct iovec iovs[2], md_iov;
	uint32_t dif_flags;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	_iov_alloc_buf(&iovs[0], 256);
	_iov_alloc_buf(&iovs[1], 256);
	_iov_alloc_buf(&md_iov, 8);

	dix_generate_and_verify(iovs, 2, &md_iov, 512, 8, 1, false, SPDK_DIF_TYPE1,
				dif_flags, 22, 0xFFFF, 0x22);

	_iov_free_buf(&iovs[0]);
	_iov_free_buf(&iovs[1]);
	_iov_free_buf(&md_iov);
}

static void
dix_sec_512_md_8_prchk_7_multi_iovs_complex_splits(void)
{
	struct iovec iovs[6], md_iov;
	uint32_t dif_flags;
	int i;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	/* data[0][255:0] */
	_iov_alloc_buf(&iovs[0], 256);

	/* data[0][511:256], data[1][255:0] */
	_iov_alloc_buf(&iovs[1], 256 + 256);

	/* data[1][382:256] */
	_iov_alloc_buf(&iovs[2], 128);

	/* data[1][383] */
	_iov_alloc_buf(&iovs[3], 1);

	/* data[1][510:384] */
	_iov_alloc_buf(&iovs[4], 126);

	/* data[1][511], data[2][511:0], data[3][511:0] */
	_iov_alloc_buf(&iovs[5], 1 + 512 * 2);

	_iov_alloc_buf(&md_iov, 8 * 4);

	dix_generate_and_verify(iovs, 6, &md_iov, 512, 8, 4, false, SPDK_DIF_TYPE1,
				dif_flags, 22, 0xFFFF, 0x22);

	for (i = 0; i < 6; i++) {
		_iov_free_buf(&iovs[i]);
	}
	_iov_free_buf(&md_iov);
}

static void
_dix_inject_error_and_verify(struct iovec *iovs, int iovcnt, struct iovec *md_iov,
			     uint32_t block_size, uint32_t md_size, uint32_t num_blocks,
			     uint32_t inject_flags, bool dif_loc)
{
	struct spdk_dif_ctx ctx = {};
	struct spdk_dif_error err_blk = {};
	uint32_t inject_offset = 0, dif_flags;
	int rc;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	rc = ut_data_pattern_generate(iovs, iovcnt, block_size, 0, num_blocks);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_ctx_init(&ctx, block_size, md_size, false, dif_loc, SPDK_DIF_TYPE1, dif_flags,
			       88, 0xFFFF, 0x88, 0, GUARD_SEED);
	CU_ASSERT(rc == 0);

	rc = spdk_dix_generate(iovs, iovcnt, md_iov, num_blocks, &ctx);
	CU_ASSERT(rc == 0);

	rc = spdk_dix_inject_error(iovs, iovcnt, md_iov, num_blocks, &ctx, inject_flags, &inject_offset);
	CU_ASSERT(rc == 0);

	rc = spdk_dix_verify(iovs, iovcnt, md_iov, num_blocks, &ctx, &err_blk);
	CU_ASSERT(rc != 0);

	if (inject_flags == SPDK_DIF_DATA_ERROR) {
		CU_ASSERT(SPDK_DIF_GUARD_ERROR == err_blk.err_type);
	} else {
		CU_ASSERT(inject_flags == err_blk.err_type);
	}
	CU_ASSERT(inject_offset == err_blk.err_offset);
}

static void
dix_inject_error_and_verify(struct iovec *iovs, int iovcnt, struct iovec *md_iov,
			    uint32_t block_size, uint32_t md_size, uint32_t num_blocks,
			    uint32_t inject_flags)
{
	/* The case that DIF is contained in the first 8 bytes of metadata. */
	_dix_inject_error_and_verify(iovs, iovcnt, md_iov, block_size, md_size, num_blocks,
				     inject_flags, true);

	/* The case that DIF is contained in the last 8 bytes of metadata. */
	_dix_inject_error_and_verify(iovs, iovcnt, md_iov, block_size, md_size, num_blocks,
				     inject_flags, false);
}

static void
dix_sec_4096_md_128_inject_1_2_4_8_multi_iovs_test(void)
{
	struct iovec iovs[4], md_iov;
	int i, num_blocks;

	num_blocks = 0;

	for (i = 0; i < 4; i++) {
		_iov_alloc_buf(&iovs[i], 4096 * (i + 1));
		num_blocks += i + 1;
	}

	_iov_alloc_buf(&md_iov, 128 * num_blocks);

	dix_inject_error_and_verify(iovs, 4, &md_iov, 4096, 128, num_blocks, SPDK_DIF_GUARD_ERROR);
	dix_inject_error_and_verify(iovs, 4, &md_iov, 4096, 128, num_blocks, SPDK_DIF_APPTAG_ERROR);
	dix_inject_error_and_verify(iovs, 4, &md_iov, 4096, 128, num_blocks, SPDK_DIF_REFTAG_ERROR);
	dix_inject_error_and_verify(iovs, 4, &md_iov, 4096, 128, num_blocks, SPDK_DIF_DATA_ERROR);

	for (i = 0; i < 4; i++) {
		_iov_free_buf(&iovs[i]);
	}
	_iov_free_buf(&md_iov);
}

static void
dix_sec_4096_md_128_inject_1_2_4_8_multi_iovs_split_test(void)
{
	struct iovec iovs[4], md_iov;
	int i;

	_iov_alloc_buf(&iovs[0], 2048);
	_iov_alloc_buf(&iovs[1], 2048);
	_iov_alloc_buf(&iovs[2], 1);
	_iov_alloc_buf(&iovs[3], 4095);

	_iov_alloc_buf(&md_iov, 128 * 2);

	dix_inject_error_and_verify(iovs, 4, &md_iov, 4096, 128, 2, SPDK_DIF_GUARD_ERROR);
	dix_inject_error_and_verify(iovs, 4, &md_iov, 4096, 128, 2, SPDK_DIF_APPTAG_ERROR);
	dix_inject_error_and_verify(iovs, 4, &md_iov, 4096, 128, 2, SPDK_DIF_REFTAG_ERROR);
	dix_inject_error_and_verify(iovs, 4, &md_iov, 4096, 128, 2, SPDK_DIF_DATA_ERROR);

	for (i = 0; i < 4; i++) {
		_iov_free_buf(&iovs[i]);
	}
	_iov_free_buf(&md_iov);
}

static int
ut_readv(uint32_t read_base, uint32_t read_len, struct iovec *iovs, int iovcnt)
{
	int i;
	uint32_t j, offset;
	uint8_t *buf;

	offset = 0;
	for (i = 0; i < iovcnt; i++) {
		buf = iovs[i].iov_base;
		for (j = 0; j < iovs[i].iov_len; j++, offset++) {
			if (offset >= read_len) {
				return offset;
			}
			buf[j] = DATA_PATTERN(read_base + offset);
		}
	}

	return offset;
}

static void
set_md_interleave_iovs_test(void)
{
	struct spdk_dif_ctx ctx = {};
	struct spdk_dif_error err_blk = {};
	struct iovec iov1, iov2, dif_iovs[4] = {};
	uint32_t dif_check_flags, data_len, read_len, data_offset, mapped_len = 0;
	uint8_t *buf1, *buf2;
	int rc;

	dif_check_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
			  SPDK_DIF_FLAGS_REFTAG_CHECK;

	rc = spdk_dif_ctx_init(&ctx, 4096 + 128, 128, true, false, SPDK_DIF_TYPE1,
			       dif_check_flags, 22, 0xFFFF, 0x22, 0, GUARD_SEED);
	CU_ASSERT(rc == 0);

	/* The first data buffer:
	 * - Create iovec array to Leave a space for metadata for each block
	 * - Split vectored read and so creating iovec array is done before every vectored read.
	 */
	buf1 = calloc(1, (4096 + 128) * 4);
	SPDK_CU_ASSERT_FATAL(buf1 != NULL);
	_iov_set_buf(&iov1, buf1, (4096 + 128) * 4);

	data_offset = 0;
	data_len = 4096 * 4;

	/* 1st read */
	rc = spdk_dif_set_md_interleave_iovs(dif_iovs, 4, &iov1, 1,
					     data_offset, data_len, &mapped_len, &ctx);
	CU_ASSERT(rc == 4);
	CU_ASSERT(mapped_len == 4096 * 4);
	CU_ASSERT(_iov_check(&dif_iovs[0], buf1, 4096) == true);
	CU_ASSERT(_iov_check(&dif_iovs[1], buf1 + 4096 + 128, 4096) == true);
	CU_ASSERT(_iov_check(&dif_iovs[2], buf1 + (4096 + 128) * 2, 4096) == true);
	CU_ASSERT(_iov_check(&dif_iovs[3], buf1 + (4096 + 128) * 3, 4096) == true);

	read_len = ut_readv(data_offset, 1024, dif_iovs, 4);
	CU_ASSERT(read_len == 1024);

	rc = spdk_dif_generate_stream(&iov1, 1, data_offset, read_len, &ctx);
	CU_ASSERT(rc == 0);

	data_offset += read_len;
	data_len -= read_len;

	/* 2nd read */
	rc = spdk_dif_set_md_interleave_iovs(dif_iovs, 4, &iov1, 1,
					     data_offset, data_len, &mapped_len, &ctx);
	CU_ASSERT(rc == 4);
	CU_ASSERT(mapped_len == 3072 + 4096 * 3);
	CU_ASSERT(_iov_check(&dif_iovs[0], buf1 + 1024, 3072) == true);
	CU_ASSERT(_iov_check(&dif_iovs[1], buf1 + 4096 + 128, 4096) == true);
	CU_ASSERT(_iov_check(&dif_iovs[2], buf1 + (4096 + 128) * 2, 4096) == true);
	CU_ASSERT(_iov_check(&dif_iovs[3], buf1 + (4096 + 128) * 3, 4096) == true);

	read_len = ut_readv(data_offset, 3071, dif_iovs, 4);
	CU_ASSERT(read_len == 3071);

	rc = spdk_dif_generate_stream(&iov1, 1, data_offset, read_len, &ctx);
	CU_ASSERT(rc == 0);

	data_offset += read_len;
	data_len -= read_len;

	/* 3rd read */
	rc = spdk_dif_set_md_interleave_iovs(dif_iovs, 4, &iov1, 1,
					     data_offset, data_len, &mapped_len, &ctx);
	CU_ASSERT(rc == 4);
	CU_ASSERT(mapped_len == 1 + 4096 * 3);
	CU_ASSERT(_iov_check(&dif_iovs[0], buf1 + 4095, 1) == true);
	CU_ASSERT(_iov_check(&dif_iovs[1], buf1 + 4096 + 128, 4096) == true);
	CU_ASSERT(_iov_check(&dif_iovs[2], buf1 + (4096 + 128) * 2, 4096) == true);
	CU_ASSERT(_iov_check(&dif_iovs[3], buf1 + (4096 + 128) * 3, 4096) == true);

	read_len = ut_readv(data_offset, 1 + 4096 * 2 + 512, dif_iovs, 4);
	CU_ASSERT(read_len == 1 + 4096 * 2 + 512);

	rc = spdk_dif_generate_stream(&iov1, 1, data_offset, read_len, &ctx);
	CU_ASSERT(rc == 0);

	data_offset += read_len;
	data_len -= read_len;

	/* 4th read */
	rc = spdk_dif_set_md_interleave_iovs(dif_iovs, 4, &iov1, 1,
					     data_offset, data_len, &mapped_len, &ctx);
	CU_ASSERT(rc == 1);
	CU_ASSERT(mapped_len == 3584);
	CU_ASSERT(_iov_check(&dif_iovs[0], buf1 + (4096 + 128) * 3 + 512, 3584) == true);

	read_len = ut_readv(data_offset, 3584, dif_iovs, 1);
	CU_ASSERT(read_len == 3584);

	rc = spdk_dif_generate_stream(&iov1, 1, data_offset, read_len, &ctx);
	CU_ASSERT(rc == 0);

	data_offset += read_len;
	CU_ASSERT(data_offset == 4096 * 4);
	data_len -= read_len;
	CU_ASSERT(data_len == 0);

	/* The second data buffer:
	 * - Set data pattern with a space for metadata for each block.
	 */
	buf2 = calloc(1, (4096 + 128) * 4);
	SPDK_CU_ASSERT_FATAL(buf2 != NULL);
	_iov_set_buf(&iov2, buf2, (4096 + 128) * 4);

	rc = ut_data_pattern_generate(&iov2, 1, 4096 + 128, 128, 4);
	CU_ASSERT(rc == 0);
	rc = spdk_dif_generate(&iov2, 1, 4, &ctx);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_verify(&iov1, 1, 4, &ctx, &err_blk);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_verify(&iov2, 1, 4, &ctx, &err_blk);
	CU_ASSERT(rc == 0);

	/* Compare the first and the second data buffer by byte. */
	rc = memcmp(buf1, buf2, (4096 + 128) * 4);
	CU_ASSERT(rc == 0);

	free(buf1);
	free(buf2);
}

static void
set_md_interleave_iovs_split_test(void)
{
	struct spdk_dif_ctx ctx = {};
	struct spdk_dif_error err_blk = {};
	struct iovec iovs1[7], iovs2[7], dif_iovs[8] = {};
	uint32_t dif_check_flags, data_len, read_len, data_offset, mapped_len = 0;
	int rc, i;

	dif_check_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
			  SPDK_DIF_FLAGS_REFTAG_CHECK;

	rc = spdk_dif_ctx_init(&ctx, 512 + 8, 8, true, false, SPDK_DIF_TYPE1,
			       dif_check_flags, 22, 0xFFFF, 0x22, 0, GUARD_SEED);
	CU_ASSERT(rc == 0);

	/* The first SGL data buffer:
	 * - Create iovec array to leave a space for metadata for each block
	 * - Split vectored read and so creating iovec array is done before every vectored read.
	 */
	_iov_alloc_buf(&iovs1[0], 512 + 8 + 128);
	_iov_alloc_buf(&iovs1[1], 128);
	_iov_alloc_buf(&iovs1[2], 256 + 8);
	_iov_alloc_buf(&iovs1[3], 100);
	_iov_alloc_buf(&iovs1[4], 412 + 5);
	_iov_alloc_buf(&iovs1[5], 3 + 300);
	_iov_alloc_buf(&iovs1[6], 212 + 8);

	data_offset = 0;
	data_len = 512 * 4;

	/* 1st read */
	rc = spdk_dif_set_md_interleave_iovs(dif_iovs, 8, iovs1, 7,
					     data_offset, data_len, &mapped_len, &ctx);
	CU_ASSERT(rc == 8);
	CU_ASSERT(mapped_len == 512 * 4);
	CU_ASSERT(_iov_check(&dif_iovs[0], iovs1[0].iov_base, 512) == true);
	CU_ASSERT(_iov_check(&dif_iovs[1], iovs1[0].iov_base + 512 + 8, 128) == true);
	CU_ASSERT(_iov_check(&dif_iovs[2], iovs1[1].iov_base, 128) == true);
	CU_ASSERT(_iov_check(&dif_iovs[3], iovs1[2].iov_base, 256) == true);
	CU_ASSERT(_iov_check(&dif_iovs[4], iovs1[3].iov_base, 100) == true);
	CU_ASSERT(_iov_check(&dif_iovs[5], iovs1[4].iov_base, 412) == true);
	CU_ASSERT(_iov_check(&dif_iovs[6], iovs1[5].iov_base + 3, 300) == true);
	CU_ASSERT(_iov_check(&dif_iovs[7], iovs1[6].iov_base, 212) == true);

	read_len = ut_readv(data_offset, 128, dif_iovs, 8);
	CU_ASSERT(read_len == 128);

	rc = spdk_dif_generate_stream(iovs1, 7, data_offset, read_len, &ctx);
	CU_ASSERT(rc == 0);

	data_offset += read_len;
	data_len -= read_len;

	/* 2nd read */
	rc = spdk_dif_set_md_interleave_iovs(dif_iovs, 8, iovs1, 7,
					     data_offset, data_len, &mapped_len, &ctx);
	CU_ASSERT(rc == 8);
	CU_ASSERT(mapped_len == 384 + 512 * 3);
	CU_ASSERT(_iov_check(&dif_iovs[0], iovs1[0].iov_base + 128, 384) == true);
	CU_ASSERT(_iov_check(&dif_iovs[1], iovs1[0].iov_base + 512 + 8, 128) == true);
	CU_ASSERT(_iov_check(&dif_iovs[2], iovs1[1].iov_base, 128) == true);
	CU_ASSERT(_iov_check(&dif_iovs[3], iovs1[2].iov_base, 256) == true);
	CU_ASSERT(_iov_check(&dif_iovs[4], iovs1[3].iov_base, 100) == true);
	CU_ASSERT(_iov_check(&dif_iovs[5], iovs1[4].iov_base, 412) == true);
	CU_ASSERT(_iov_check(&dif_iovs[6], iovs1[5].iov_base + 3, 300) == true);
	CU_ASSERT(_iov_check(&dif_iovs[7], iovs1[6].iov_base, 212) == true);

	read_len = ut_readv(data_offset, 383, dif_iovs, 8);
	CU_ASSERT(read_len == 383);

	rc = spdk_dif_generate_stream(iovs1, 7, data_offset, read_len, &ctx);
	CU_ASSERT(rc == 0);

	data_offset += read_len;
	data_len -= read_len;

	/* 3rd read */
	rc = spdk_dif_set_md_interleave_iovs(dif_iovs, 8, iovs1, 7,
					     data_offset, data_len, &mapped_len, &ctx);
	CU_ASSERT(rc == 8);
	CU_ASSERT(mapped_len == 1 + 512 * 3);
	CU_ASSERT(_iov_check(&dif_iovs[0], iovs1[0].iov_base + 511, 1) == true);
	CU_ASSERT(_iov_check(&dif_iovs[1], iovs1[0].iov_base + 512 + 8, 128) == true);
	CU_ASSERT(_iov_check(&dif_iovs[2], iovs1[1].iov_base, 128) == true);
	CU_ASSERT(_iov_check(&dif_iovs[3], iovs1[2].iov_base, 256) == true);
	CU_ASSERT(_iov_check(&dif_iovs[4], iovs1[3].iov_base, 100) == true);
	CU_ASSERT(_iov_check(&dif_iovs[5], iovs1[4].iov_base, 412) == true);
	CU_ASSERT(_iov_check(&dif_iovs[6], iovs1[5].iov_base + 3, 300) == true);
	CU_ASSERT(_iov_check(&dif_iovs[7], iovs1[6].iov_base, 212) == true);

	read_len = ut_readv(data_offset, 1 + 512 * 2 + 128, dif_iovs, 8);
	CU_ASSERT(read_len == 1 + 512 * 2 + 128);

	rc = spdk_dif_generate_stream(iovs1, 7, data_offset, read_len, &ctx);
	CU_ASSERT(rc == 0);

	data_offset += read_len;
	data_len -= read_len;

	/* 4th read */
	rc = spdk_dif_set_md_interleave_iovs(dif_iovs, 8, iovs1, 7,
					     data_offset, data_len, &mapped_len, &ctx);
	CU_ASSERT(rc == 2);
	CU_ASSERT(mapped_len == 384);
	CU_ASSERT(_iov_check(&dif_iovs[0], iovs1[5].iov_base + 3 + 128, 172) == true);
	CU_ASSERT(_iov_check(&dif_iovs[1], iovs1[6].iov_base, 212) == true);

	read_len = ut_readv(data_offset, 384, dif_iovs, 8);
	CU_ASSERT(read_len == 384);

	rc = spdk_dif_generate_stream(iovs1, 7, data_offset, read_len, &ctx);
	CU_ASSERT(rc == 0);

	data_offset += read_len;
	CU_ASSERT(data_offset == 512 * 4);
	data_len -= read_len;
	CU_ASSERT(data_len == 0);

	/* The second SGL data buffer:
	 * - Set data pattern with a space for metadata for each block.
	 */
	_iov_alloc_buf(&iovs2[0], 512 + 8 + 128);
	_iov_alloc_buf(&iovs2[1], 128);
	_iov_alloc_buf(&iovs2[2], 256 + 8);
	_iov_alloc_buf(&iovs2[3], 100);
	_iov_alloc_buf(&iovs2[4], 412 + 5);
	_iov_alloc_buf(&iovs2[5], 3 + 300);
	_iov_alloc_buf(&iovs2[6], 212 + 8);

	rc = ut_data_pattern_generate(iovs2, 7, 512 + 8, 8, 4);
	CU_ASSERT(rc == 0);
	rc = spdk_dif_generate(iovs2, 7, 4, &ctx);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_verify(iovs1, 7, 4, &ctx, &err_blk);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_verify(iovs2, 7, 4, &ctx, &err_blk);
	CU_ASSERT(rc == 0);

	/* Compare the first and the second SGL data buffer by byte. */
	for (i = 0; i < 7; i++) {
		rc = memcmp(iovs1[i].iov_base, iovs2[i].iov_base,
			    iovs1[i].iov_len);
		CU_ASSERT(rc == 0);
	}

	for (i = 0; i < 7; i++) {
		_iov_free_buf(&iovs1[i]);
		_iov_free_buf(&iovs2[i]);
	}
}

static void
dif_generate_stream_test(void)
{
	struct iovec iov;
	struct spdk_dif_ctx ctx;
	struct spdk_dif_error err_blk;
	uint32_t dif_flags;
	int rc;

	_iov_alloc_buf(&iov, (512 + 8) * 5);

	rc = ut_data_pattern_generate(&iov, 1, 512 + 8, 8, 5);
	CU_ASSERT(rc == 0);

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	rc = spdk_dif_ctx_init(&ctx, 512 + 8, 8, true, false, SPDK_DIF_TYPE1, dif_flags,
			       22, 0xFFFF, 0x22, 0, GUARD_SEED);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_generate_stream(&iov, 1, 0, 511, &ctx);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_generate_stream(&iov, 1, 511, 1, &ctx);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_generate_stream(&iov, 1, 512, 256, &ctx);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_generate_stream(&iov, 1, 768, 512, &ctx);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_generate_stream(&iov, 1, 1280, 1024, &ctx);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_generate_stream(&iov, 1, 2304, 256, &ctx);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_generate_stream(&iov, 1, 2560, 512, &ctx);
	CU_ASSERT(rc == -ERANGE);

	rc = spdk_dif_verify(&iov, 1, 5, &ctx, &err_blk);
	CU_ASSERT(rc == 0);

	rc = ut_data_pattern_verify(&iov, 1, 512 + 8, 8, 5);
	CU_ASSERT(rc == 0);

	_iov_free_buf(&iov);
}

static void
set_md_interleave_iovs_alignment_test(void)
{
	struct iovec iovs[3], dif_iovs[5] = {};
	uint32_t mapped_len = 0;
	int rc;
	struct spdk_dif_ctx ctx;

	rc = spdk_dif_ctx_init(&ctx, 512 + 8, 8, true, false, SPDK_DIF_TYPE1,
			       0, 0, 0, 0, 0, 0);
	CU_ASSERT(rc == 0);

	/* The case that buffer size is smaller than necessary. */
	_iov_set_buf(&iovs[0], (uint8_t *)0xDEADBEEF, 1024);
	_iov_set_buf(&iovs[1], (uint8_t *)0xFEEDBEEF, 1024);
	_iov_set_buf(&iovs[2], (uint8_t *)0xC0FFEE, 24);

	rc = spdk_dif_set_md_interleave_iovs(dif_iovs, 5, iovs, 3, 0, 2048, &mapped_len, &ctx);
	CU_ASSERT(rc == -ERANGE);

	/* The folllowing are the normal cases. */
	_iov_set_buf(&iovs[2], (uint8_t *)0xC0FFEE, 32);

	/* data length is less than a data block size. */
	rc = spdk_dif_set_md_interleave_iovs(dif_iovs, 5, iovs, 3, 0, 500, &mapped_len, &ctx);
	CU_ASSERT(rc == 1);
	CU_ASSERT(mapped_len == 500);
	CU_ASSERT(_iov_check(&dif_iovs[0], (void *)0xDEADBEEF, 500) == true);

	/* Pass enough number of iovecs */
	rc = spdk_dif_set_md_interleave_iovs(dif_iovs, 5, iovs, 3, 500, 1000, &mapped_len, &ctx);
	CU_ASSERT(rc == 4);
	CU_ASSERT(mapped_len == 1000);
	CU_ASSERT(_iov_check(&dif_iovs[0], (void *)(0xDEADBEEF + 500), 12) == true);
	CU_ASSERT(_iov_check(&dif_iovs[1], (void *)(0xDEADBEEF + 520), 504) == true);
	CU_ASSERT(_iov_check(&dif_iovs[2], (void *)0xFEEDBEEF, 8) == true);
	CU_ASSERT(_iov_check(&dif_iovs[3], (void *)(0xFEEDBEEF + 16), 476) == true);

	/* Pass iovecs smaller than necessary */
	rc = spdk_dif_set_md_interleave_iovs(dif_iovs, 3, iovs, 3, 500, 1000, &mapped_len, &ctx);
	CU_ASSERT(rc == 3);
	CU_ASSERT(mapped_len == 524);
	CU_ASSERT(_iov_check(&dif_iovs[0], (void *)(0xDEADBEEF + 500), 12) == true);
	CU_ASSERT(_iov_check(&dif_iovs[1], (void *)(0xDEADBEEF + 520), 504) == true);
	CU_ASSERT(_iov_check(&dif_iovs[2], (void *)0xFEEDBEEF, 8) == true);

	rc = spdk_dif_set_md_interleave_iovs(dif_iovs, 5, iovs, 3, 1500, 500, &mapped_len, &ctx);
	CU_ASSERT(rc == 2);
	CU_ASSERT(mapped_len == 500);
	CU_ASSERT(_iov_check(&dif_iovs[0], (void *)(0xFEEDBEEF + 492), 36) == true);
	CU_ASSERT(_iov_check(&dif_iovs[1], (void *)(0xFEEDBEEF + 536), 464) == true);

	rc = spdk_dif_set_md_interleave_iovs(dif_iovs, 5, iovs, 3, 2000, 48, &mapped_len, &ctx);
	CU_ASSERT(rc == 2);
	CU_ASSERT(mapped_len == 48);
	CU_ASSERT(_iov_check(&dif_iovs[0], (void *)0xFEEDBEEF + 1000, 24) == true);
	CU_ASSERT(_iov_check(&dif_iovs[1], (void *)0xC0FFEE, 24) ==  true);
}

static void
_dif_generate_split_test(void)
{
	struct spdk_dif_ctx ctx = {};
	struct iovec iov;
	uint8_t *buf1, *buf2;
	struct _dif_sgl sgl;
	uint16_t guard = 0, prev_guard;
	uint32_t dif_flags;
	int rc;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	rc = spdk_dif_ctx_init(&ctx, 4096 + 128, 128, true, false, SPDK_DIF_TYPE1,
			       dif_flags, 0, 0, 0, 0, GUARD_SEED);
	CU_ASSERT(rc == 0);

	buf1 = calloc(1, 4096 + 128);
	SPDK_CU_ASSERT_FATAL(buf1 != NULL);
	_iov_set_buf(&iov, buf1, 4096 + 128);

	rc = ut_data_pattern_generate(&iov, 1, 4096 + 128, 128, 1);
	CU_ASSERT(rc == 0);

	_dif_sgl_init(&sgl, &iov, 1);

	guard = GUARD_SEED;
	prev_guard = GUARD_SEED;

	guard = _dif_generate_split(&sgl, 0, 1000, guard, 0, &ctx);
	CU_ASSERT(sgl.iov_offset == 1000);
	CU_ASSERT(guard == spdk_crc16_t10dif(prev_guard, buf1, 1000));

	prev_guard = guard;

	guard = _dif_generate_split(&sgl, 1000, 3000, guard, 0, &ctx);
	CU_ASSERT(sgl.iov_offset == 4000);
	CU_ASSERT(guard == spdk_crc16_t10dif(prev_guard, buf1 + 1000, 3000));

	guard = _dif_generate_split(&sgl, 4000, 96 + 128, guard, 0, &ctx);
	CU_ASSERT(guard == GUARD_SEED);
	CU_ASSERT(sgl.iov_offset == 0);
	CU_ASSERT(sgl.iovcnt == 0);

	rc = ut_data_pattern_verify(&iov, 1, 4096 + 128, 128, 1);
	CU_ASSERT(rc == 0);

	_dif_sgl_init(&sgl, &iov, 1);

	rc = dif_verify(&sgl, 1, &ctx, NULL);
	CU_ASSERT(rc == 0);

	buf2 = calloc(1, 4096 + 128);
	SPDK_CU_ASSERT_FATAL(buf2 != NULL);
	_iov_set_buf(&iov, buf2, 4096 + 128);

	rc = ut_data_pattern_generate(&iov, 1, 4096 + 128, 128, 1);
	CU_ASSERT(rc == 0);

	_dif_sgl_init(&sgl, &iov, 1);

	dif_generate(&sgl, 1, &ctx);

	rc = ut_data_pattern_verify(&iov, 1, 4096 + 128, 128, 1);
	CU_ASSERT(rc == 0);

	_dif_sgl_init(&sgl, &iov, 1);

	rc = dif_verify(&sgl, 1, &ctx, NULL);
	CU_ASSERT(rc == 0);

	rc = memcmp(buf1, buf2, 4096 + 128);
	CU_ASSERT(rc == 0);

	free(buf1);
	free(buf2);
}

static void
set_md_interleave_iovs_multi_segments_test(void)
{
	struct spdk_dif_ctx ctx = {};
	struct spdk_dif_error err_blk = {};
	struct iovec iov1 = {}, iov2 = {}, dif_iovs[4] = {};
	uint32_t dif_check_flags, data_len, read_len, data_offset, read_offset, mapped_len = 0;
	uint8_t *buf1, *buf2;
	int rc;

	dif_check_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
			  SPDK_DIF_FLAGS_REFTAG_CHECK;

	rc = spdk_dif_ctx_init(&ctx, 4096 + 128, 128, true, false, SPDK_DIF_TYPE1,
			       dif_check_flags, 22, 0xFFFF, 0x22, 0, GUARD_SEED);
	CU_ASSERT(rc == 0);

	/* The first data buffer:
	 * - Data buffer is split into multi data segments
	 * - For each data segment,
	 *  - Create iovec array to Leave a space for metadata for each block
	 *  - Split vectored read and so creating iovec array is done before every vectored read.
	 */
	buf1 = calloc(1, (4096 + 128) * 4);
	SPDK_CU_ASSERT_FATAL(buf1 != NULL);
	_iov_set_buf(&iov1, buf1, (4096 + 128) * 4);

	/* 1st data segment */
	data_offset = 0;
	data_len = 1024;

	spdk_dif_ctx_set_data_offset(&ctx, data_offset);

	read_offset = 0;

	/* 1st read in 1st data segment */
	rc = spdk_dif_set_md_interleave_iovs(dif_iovs, 4, &iov1, 1,
					     read_offset, data_len - read_offset,
					     &mapped_len, &ctx);
	CU_ASSERT(rc == 1);
	CU_ASSERT(mapped_len == 1024);
	CU_ASSERT(_iov_check(&dif_iovs[0], buf1, 1024) == true);

	read_len = ut_readv(data_offset + read_offset, 1024, dif_iovs, 4);
	CU_ASSERT(read_len == 1024);

	rc = spdk_dif_generate_stream(&iov1, 1, read_offset, read_len, &ctx);
	CU_ASSERT(rc == 0);

	read_offset += read_len;
	CU_ASSERT(read_offset == data_len);

	/* 2nd data segment */
	data_offset += data_len;
	data_len = 3072 + 4096 * 2 + 512;

	spdk_dif_ctx_set_data_offset(&ctx, data_offset);
	_iov_set_buf(&iov1, buf1 + 1024, 3072 + 128 + (4096 + 128) * 3 + 512);

	read_offset = 0;

	/* 1st read in 2nd data segment */
	rc = spdk_dif_set_md_interleave_iovs(dif_iovs, 4, &iov1, 1,
					     read_offset, data_len - read_offset,
					     &mapped_len, &ctx);
	CU_ASSERT(rc == 4);
	CU_ASSERT(mapped_len == 3072 + 4096 * 2 + 512);
	CU_ASSERT(_iov_check(&dif_iovs[0], buf1 + 1024, 3072) == true);
	CU_ASSERT(_iov_check(&dif_iovs[1], buf1 + 4096 + 128, 4096) == true);
	CU_ASSERT(_iov_check(&dif_iovs[2], buf1 + (4096 + 128) * 2, 4096) == true);
	CU_ASSERT(_iov_check(&dif_iovs[3], buf1 + (4096 + 128) * 3, 512) == true);

	read_len = ut_readv(data_offset + read_offset, 3071, dif_iovs, 4);
	CU_ASSERT(read_len == 3071);

	rc = spdk_dif_generate_stream(&iov1, 1, read_offset, read_len, &ctx);
	CU_ASSERT(rc == 0);

	read_offset += read_len;

	/* 2nd read in 2nd data segment */
	rc = spdk_dif_set_md_interleave_iovs(dif_iovs, 4, &iov1, 1,
					     read_offset, data_len - read_offset,
					     &mapped_len, &ctx);
	CU_ASSERT(rc == 4);
	CU_ASSERT(mapped_len == 1 + 4096 * 2 + 512);
	CU_ASSERT(_iov_check(&dif_iovs[0], buf1 + 4095, 1) == true);
	CU_ASSERT(_iov_check(&dif_iovs[1], buf1 + 4096 + 128, 4096) == true);
	CU_ASSERT(_iov_check(&dif_iovs[2], buf1 + (4096 + 128) * 2, 4096) == true);
	CU_ASSERT(_iov_check(&dif_iovs[3], buf1 + (4096 + 128) * 3, 512) == true);

	read_len = ut_readv(data_offset + read_offset, 1 + 4096 * 2 + 512, dif_iovs, 4);
	CU_ASSERT(read_len == 1 + 4096 * 2 + 512);

	rc = spdk_dif_generate_stream(&iov1, 1, read_offset, read_len, &ctx);
	CU_ASSERT(rc == 0);

	read_offset += read_len;
	CU_ASSERT(read_offset == data_len);

	/* 3rd data segment */
	data_offset += data_len;
	data_len = 3584;

	spdk_dif_ctx_set_data_offset(&ctx, data_offset);
	_iov_set_buf(&iov1, buf1 + (4096 + 128) * 3 + 512, 3584 + 128);

	read_offset = 0;

	/* 1st read in 3rd data segment */
	rc = spdk_dif_set_md_interleave_iovs(dif_iovs, 4, &iov1, 1,
					     read_offset, data_len - read_offset,
					     &mapped_len, &ctx);
	CU_ASSERT(rc == 1);
	CU_ASSERT(mapped_len == 3584);
	CU_ASSERT(_iov_check(&dif_iovs[0], buf1 + (4096 + 128) * 3 + 512, 3584) == true);

	read_len = ut_readv(data_offset + read_offset, 3584, dif_iovs, 1);
	CU_ASSERT(read_len == 3584);

	rc = spdk_dif_generate_stream(&iov1, 1, read_offset, read_len, &ctx);
	CU_ASSERT(rc == 0);

	read_offset += read_len;
	CU_ASSERT(read_offset == data_len);
	data_offset += data_len;
	CU_ASSERT(data_offset == 4096 * 4);

	spdk_dif_ctx_set_data_offset(&ctx, 0);
	_iov_set_buf(&iov1, buf1, (4096 + 128) * 4);

	/* The second data buffer:
	 * - Set data pattern with a space for metadata for each block.
	 */
	buf2 = calloc(1, (4096 + 128) * 4);
	SPDK_CU_ASSERT_FATAL(buf2 != NULL);
	_iov_set_buf(&iov2, buf2, (4096 + 128) * 4);

	rc = ut_data_pattern_generate(&iov2, 1, 4096 + 128, 128, 4);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_generate(&iov2, 1, 4, &ctx);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_verify(&iov1, 1, 4, &ctx, &err_blk);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_verify(&iov2, 1, 4, &ctx, &err_blk);
	CU_ASSERT(rc == 0);

	/* Compare the first and the second data buffer by byte. */
	rc = memcmp(buf1, buf2, (4096 + 128) * 4);
	CU_ASSERT(rc == 0);

	free(buf1);
	free(buf2);
}

static void
_dif_verify_split_test(void)
{
	struct spdk_dif_ctx ctx = {};
	struct spdk_dif_error err_blk = {};
	struct iovec iov;
	uint8_t *buf;
	struct _dif_sgl sgl;
	uint16_t guard = 0, prev_guard = 0;
	uint32_t dif_flags;
	int rc;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	rc = spdk_dif_ctx_init(&ctx, 4096 + 128, 128, true, false, SPDK_DIF_TYPE1,
			       dif_flags, 0, 0, 0, 0, GUARD_SEED);
	CU_ASSERT(rc == 0);

	buf = calloc(1, 4096 + 128);
	SPDK_CU_ASSERT_FATAL(buf != NULL);
	_iov_set_buf(&iov, buf, 4096 + 128);

	rc = ut_data_pattern_generate(&iov, 1, 4096 + 128, 128, 1);
	CU_ASSERT(rc == 0);

	_dif_sgl_init(&sgl, &iov, 1);

	dif_generate(&sgl, 1, &ctx);

	_dif_sgl_init(&sgl, &iov, 1);

	guard = GUARD_SEED;
	prev_guard = GUARD_SEED;

	rc = _dif_verify_split(&sgl, 0, 1000, &guard, 0, &ctx, &err_blk);
	CU_ASSERT(rc == 0);
	CU_ASSERT(guard == spdk_crc16_t10dif(prev_guard, buf, 1000));
	CU_ASSERT(sgl.iov_offset == 1000);

	prev_guard = guard;

	rc = _dif_verify_split(&sgl, 1000, 3000, &guard, 0, &ctx, &err_blk);
	CU_ASSERT(rc == 0);
	CU_ASSERT(guard == spdk_crc16_t10dif(prev_guard, buf + 1000, 3000));
	CU_ASSERT(sgl.iov_offset == 4000);

	rc = _dif_verify_split(&sgl, 4000, 96 + 128, &guard, 0, &ctx, &err_blk);
	CU_ASSERT(rc == 0);
	CU_ASSERT(guard == GUARD_SEED);
	CU_ASSERT(sgl.iov_offset == 0);
	CU_ASSERT(sgl.iovcnt == 0);

	_dif_sgl_init(&sgl, &iov, 1);

	rc = dif_verify(&sgl, 1, &ctx, &err_blk);
	CU_ASSERT(rc == 0);

	rc = ut_data_pattern_verify(&iov, 1, 4096 + 128, 128, 1);
	CU_ASSERT(rc == 0);

	free(buf);
}

static void
dif_verify_stream_multi_segments_test(void)
{
	struct spdk_dif_ctx ctx = {};
	struct spdk_dif_error err_blk = {};
	struct iovec iov = {};
	uint8_t *buf;
	uint32_t dif_flags;
	int rc;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	rc = spdk_dif_ctx_init(&ctx, 4096 + 128, 128, true, false, SPDK_DIF_TYPE1,
			       dif_flags, 22, 0xFFFF, 0x22, 0, GUARD_SEED);
	CU_ASSERT(rc == 0);

	buf = calloc(1, (4096 + 128) * 4);
	SPDK_CU_ASSERT_FATAL(buf != NULL);
	_iov_set_buf(&iov, buf, (4096 + 128) * 4);

	rc = ut_data_pattern_generate(&iov, 1, 4096 + 128, 128, 4);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_generate(&iov, 1, 4, &ctx);
	CU_ASSERT(rc == 0);

	/* 1st data segment */
	_iov_set_buf(&iov, buf, 1024);
	spdk_dif_ctx_set_data_offset(&ctx, 0);

	rc = spdk_dif_verify_stream(&iov, 1, 0, 1024, &ctx, &err_blk);
	CU_ASSERT(rc == 0);

	/* 2nd data segment */
	_iov_set_buf(&iov, buf + 1024, (3072 + 128) + (4096 + 128) * 2 + 512);
	spdk_dif_ctx_set_data_offset(&ctx, 1024);

	rc = spdk_dif_verify_stream(&iov, 1, 0, 3072 + 4096 * 2 + 512, &ctx, &err_blk);
	CU_ASSERT(rc == 0);

	/* 3rd data segment */
	_iov_set_buf(&iov, buf + (4096 + 128) * 3 + 512, 3584 + 128);
	spdk_dif_ctx_set_data_offset(&ctx, 4096 * 3);

	rc = spdk_dif_verify_stream(&iov, 1, 0, 3584, &ctx, &err_blk);
	CU_ASSERT(rc == 0);

	/* verify all data segments once */
	_iov_set_buf(&iov, buf, (4096 + 128) * 4);
	spdk_dif_ctx_set_data_offset(&ctx, 0);

	rc = spdk_dif_verify(&iov, 1, 4, &ctx, &err_blk);
	CU_ASSERT(rc == 0);

	rc = ut_data_pattern_verify(&iov, 1, 4096 + 128, 128, 4);
	CU_ASSERT(rc == 0);

	free(buf);
}

#define UT_CRC32C_XOR	0xffffffffUL

static void
update_crc32c_test(void)
{
	struct spdk_dif_ctx ctx = {};
	struct iovec iovs[7];
	uint32_t crc32c1, crc32c2, crc32c3, crc32c4;
	uint32_t dif_flags;
	int i, rc;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	rc = spdk_dif_ctx_init(&ctx, 512 + 8, 8, true, false, SPDK_DIF_TYPE1,
			       dif_flags, 0, 0, 0, 0, 0);
	CU_ASSERT(rc == 0);

	/* data[0][255:0] */
	_iov_alloc_buf(&iovs[0], 256);

	/* data[0][511:256], md[0][0] */
	_iov_alloc_buf(&iovs[1], 256 + 1);

	/* md[0][4:1] */
	_iov_alloc_buf(&iovs[2], 4);

	/* md[0][7:5], data[1][122:0] */
	_iov_alloc_buf(&iovs[3], 3 + 123);

	/* data[1][511:123], md[1][5:0] */
	_iov_alloc_buf(&iovs[4], 389 + 6);

	/* md[1][7:6], data[2][511:0], md[2][7:0], data[3][431:0] */
	_iov_alloc_buf(&iovs[5], 2 + 512 + 8 + 432);

	/* data[3][511:432], md[3][7:0] */
	_iov_alloc_buf(&iovs[6], 80 + 8);

	rc = ut_data_pattern_generate(iovs, 7, 512 + 8, 8, 4);
	CU_ASSERT(rc == 0);

	crc32c1 = UT_CRC32C_XOR;

	rc = spdk_dif_update_crc32c(iovs, 7, 4, &crc32c1, &ctx);
	CU_ASSERT(rc == 0);

	/* Test if DIF doesn't affect CRC for split case. */
	rc = spdk_dif_generate(iovs, 7, 4, &ctx);
	CU_ASSERT(rc == 0);

	crc32c2 = UT_CRC32C_XOR;

	rc = spdk_dif_update_crc32c(iovs, 7, 4, &crc32c2, &ctx);
	CU_ASSERT(rc == 0);

	CU_ASSERT(crc32c1 == crc32c2);

	for (i = 0; i < 7; i++) {
		_iov_free_buf(&iovs[i]);
	}

	/* Test if CRC is same regardless of splitting. */
	for (i = 0; i < 4; i++) {
		_iov_alloc_buf(&iovs[i], 512 + 8);
	}

	rc = ut_data_pattern_generate(iovs, 4, 512 + 8, 8, 4);
	CU_ASSERT(rc == 0);

	crc32c3 = UT_CRC32C_XOR;

	rc = spdk_dif_update_crc32c(iovs, 4, 4, &crc32c3, &ctx);
	CU_ASSERT(rc == 0);

	CU_ASSERT(crc32c1 == crc32c3);

	/* Test if DIF doesn't affect CRC for non-split case. */
	rc = spdk_dif_generate(iovs, 4, 4, &ctx);
	CU_ASSERT(rc == 0);

	crc32c4 = UT_CRC32C_XOR;

	rc = spdk_dif_update_crc32c(iovs, 4, 4, &crc32c4, &ctx);
	CU_ASSERT(rc == 0);

	CU_ASSERT(crc32c1 == crc32c4);

	for (i = 0; i < 4; i++) {
		_iov_free_buf(&iovs[i]);
	}
}

static void
_dif_update_crc32c_split_test(void)
{
	struct spdk_dif_ctx ctx = {};
	struct iovec iov;
	uint8_t *buf;
	struct _dif_sgl sgl;
	uint32_t dif_flags, crc32c, prev_crc32c;
	int rc;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	rc = spdk_dif_ctx_init(&ctx, 4096 + 128, 128, true, false, SPDK_DIF_TYPE1,
			       dif_flags, 0, 0, 0, 0, GUARD_SEED);
	CU_ASSERT(rc == 0);

	buf = calloc(1, 4096 + 128);
	SPDK_CU_ASSERT_FATAL(buf != NULL);
	_iov_set_buf(&iov, buf, 4096 + 128);

	rc = ut_data_pattern_generate(&iov, 1, 4096 + 128, 128, 1);
	CU_ASSERT(rc == 0);

	_dif_sgl_init(&sgl, &iov, 1);

	dif_generate(&sgl, 1, &ctx);

	_dif_sgl_init(&sgl, &iov, 1);

	crc32c = _dif_update_crc32c_split(&sgl, 0, 1000, UT_CRC32C_XOR, &ctx);
	CU_ASSERT(crc32c == spdk_crc32c_update(buf, 1000, UT_CRC32C_XOR));

	prev_crc32c = crc32c;

	crc32c = _dif_update_crc32c_split(&sgl, 1000, 3000, prev_crc32c, &ctx);
	CU_ASSERT(crc32c == spdk_crc32c_update(buf + 1000, 3000, prev_crc32c));

	prev_crc32c = crc32c;

	crc32c = _dif_update_crc32c_split(&sgl, 4000, 96 + 128, prev_crc32c, &ctx);
	CU_ASSERT(crc32c == spdk_crc32c_update(buf + 4000, 96, prev_crc32c));

	CU_ASSERT(crc32c == spdk_crc32c_update(buf, 4096, UT_CRC32C_XOR));

	free(buf);
}

static void
dif_update_crc32c_stream_multi_segments_test(void)
{
	struct spdk_dif_ctx ctx = {};
	struct iovec iov = {};
	uint8_t *buf;
	uint32_t dif_flags, crc32c1, crc32c2;
	int rc;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	rc = spdk_dif_ctx_init(&ctx, 4096 + 128, 128, true, false, SPDK_DIF_TYPE1,
			       dif_flags, 22, 0xFFFF, 0x22, 0, GUARD_SEED);
	CU_ASSERT(rc == 0);

	buf = calloc(1, (4096 + 128) * 4);
	SPDK_CU_ASSERT_FATAL(buf != NULL);
	_iov_set_buf(&iov, buf, (4096 + 128) * 4);

	rc = ut_data_pattern_generate(&iov, 1, 4096 + 128, 128, 4);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_generate(&iov, 1, 4, &ctx);
	CU_ASSERT(rc == 0);

	crc32c1 = UT_CRC32C_XOR;
	crc32c2 = UT_CRC32C_XOR;

	/* 1st data segment */
	_iov_set_buf(&iov, buf, 1024);
	spdk_dif_ctx_set_data_offset(&ctx, 0);

	rc = spdk_dif_update_crc32c_stream(&iov, 1, 0, 1024, &crc32c1, &ctx);
	CU_ASSERT(rc == 0);

	/* 2nd data segment */
	_iov_set_buf(&iov, buf + 1024, (3072 + 128) + (4096 + 128) * 2 + 512);
	spdk_dif_ctx_set_data_offset(&ctx, 1024);

	rc = spdk_dif_update_crc32c_stream(&iov, 1, 0, 3072 + 4096 * 2 + 512, &crc32c1, &ctx);
	CU_ASSERT(rc == 0);

	/* 3rd data segment */
	_iov_set_buf(&iov, buf + (4096 + 128) * 3 + 512, 3584 + 128);
	spdk_dif_ctx_set_data_offset(&ctx, 4096 * 3);

	rc = spdk_dif_update_crc32c_stream(&iov, 1, 0, 3584, &crc32c1, &ctx);
	CU_ASSERT(rc == 0);

	/* Update CRC32C for all data segments once */
	_iov_set_buf(&iov, buf, (4096 + 128) * 4);
	spdk_dif_ctx_set_data_offset(&ctx, 0);

	rc = spdk_dif_update_crc32c(&iov, 1, 4, &crc32c2, &ctx);
	CU_ASSERT(rc == 0);

	CU_ASSERT(crc32c1 == crc32c2);

	free(buf);
}

static void
get_range_with_md_test(void)
{
	struct spdk_dif_ctx ctx = {};
	uint32_t buf_offset, buf_len;
	int rc;

	rc = spdk_dif_ctx_init(&ctx, 4096 + 128, 128, true, false, 0, 0, 0, 0, 0, 0, 0);
	CU_ASSERT(rc == 0);

	spdk_dif_get_range_with_md(0, 2048, &buf_offset, &buf_len, &ctx);
	CU_ASSERT(buf_offset == 0);
	CU_ASSERT(buf_len == 2048);

	spdk_dif_get_range_with_md(2048, 4096, &buf_offset, &buf_len, &ctx);
	CU_ASSERT(buf_offset == 2048);
	CU_ASSERT(buf_len == 4096 + 128);

	spdk_dif_get_range_with_md(4096, 10240, &buf_offset, &buf_len, &ctx);
	CU_ASSERT(buf_offset == 4096 + 128);
	CU_ASSERT(buf_len == 10240 + 256);

	spdk_dif_get_range_with_md(10240, 2048, &buf_offset, &buf_len, &ctx);
	CU_ASSERT(buf_offset == 10240 + 256);
	CU_ASSERT(buf_len == 2048 + 128);

	buf_len = spdk_dif_get_length_with_md(6144, &ctx);
	CU_ASSERT(buf_len == 6144 + 128);
}

static void
dif_generate_remap_and_verify(struct iovec *iovs, int iovcnt,
			      uint32_t block_size, uint32_t md_size, uint32_t num_blocks,
			      bool dif_loc, enum spdk_dif_type dif_type, uint32_t dif_flags,
			      uint32_t init_ref_tag, uint32_t remapped_init_ref_tag,
			      uint16_t apptag_mask, uint16_t app_tag)
{
	struct spdk_dif_ctx ctx = {};
	int rc;

	rc = ut_data_pattern_generate(iovs, iovcnt, block_size, md_size, num_blocks);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_ctx_init(&ctx, block_size, md_size, true, dif_loc, dif_type, dif_flags,
			       init_ref_tag, apptag_mask, app_tag, 0, GUARD_SEED);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_generate(iovs, iovcnt, num_blocks, &ctx);
	CU_ASSERT(rc == 0);

	spdk_dif_ctx_set_remapped_init_ref_tag(&ctx, remapped_init_ref_tag);

	rc = spdk_dif_remap_ref_tag(iovs, iovcnt, num_blocks, &ctx, NULL);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_ctx_init(&ctx, block_size, md_size, true, dif_loc, dif_type, dif_flags,
			       remapped_init_ref_tag, apptag_mask, app_tag, 0, GUARD_SEED);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_verify(iovs, iovcnt, num_blocks, &ctx, NULL);
	CU_ASSERT(rc == 0);

	rc = ut_data_pattern_verify(iovs, iovcnt, block_size, md_size, num_blocks);
	CU_ASSERT(rc == 0);
}

static void
dif_sec_4096_md_128_prchk_7_multi_iovs_remap_test(void)
{
	struct iovec iovs[4];
	int i, num_blocks;
	uint32_t dif_flags;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	num_blocks = 0;

	for (i = 0; i < 4; i++) {
		_iov_alloc_buf(&iovs[i], (512 + 8) * (i + 1));
		num_blocks += i + 1;
	}

	dif_generate_remap_and_verify(iovs, 4, 512 + 8, 8, num_blocks, false, SPDK_DIF_TYPE1,
				      dif_flags, 22, 99, 0xFFFF, 0x22);

	dif_generate_remap_and_verify(iovs, 4, 512 + 8, 8, num_blocks, true, SPDK_DIF_TYPE1,
				      dif_flags, 22, 99, 0xFFFF, 0x22);

	for (i = 0; i < 4; i++) {
		_iov_free_buf(&iovs[i]);
	}
}

static void
dif_sec_4096_md_128_prchk_7_multi_iovs_complex_splits_remap_test(void)
{
	struct iovec iovs[11];
	uint32_t dif_flags;
	int i;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	/* data[0][1000:0] */
	_iov_alloc_buf(&iovs[0], 1000);

	/* data[0][3095:1000], guard[0][0] */
	_iov_alloc_buf(&iovs[1], 3096 + 1);

	/* guard[0][1], apptag[0][0] */
	_iov_alloc_buf(&iovs[2], 1 + 1);

	/* apptag[0][1], reftag[0][0] */
	_iov_alloc_buf(&iovs[3], 1 + 1);

	/* reftag[0][3:1], ignore[0][59:0] */
	_iov_alloc_buf(&iovs[4], 3 + 60);

	/* ignore[119:60], data[1][3050:0] */
	_iov_alloc_buf(&iovs[5], 60 + 3051);

	/* data[1][4095:3050], guard[1][0] */
	_iov_alloc_buf(&iovs[6], 1045 + 1);

	/* guard[1][1], apptag[1][0] */
	_iov_alloc_buf(&iovs[7], 1 + 1);

	/* apptag[1][1], reftag[1][0] */
	_iov_alloc_buf(&iovs[8], 1 + 1);

	/* reftag[1][3:1], ignore[1][9:0] */
	_iov_alloc_buf(&iovs[9], 3 + 10);

	/* ignore[1][127:9] */
	_iov_alloc_buf(&iovs[10], 118);

	dif_generate_remap_and_verify(iovs, 11, 4096 + 128, 128, 2, false, SPDK_DIF_TYPE1, dif_flags,
				      22, 99, 0xFFFF, 0x22);
	dif_generate_remap_and_verify(iovs, 11, 4096 + 128, 128, 2, true, SPDK_DIF_TYPE1, dif_flags,
				      22, 99, 0xFFFF, 0x22);

	for (i = 0; i < 11; i++) {
		_iov_free_buf(&iovs[i]);
	}
}

static void
dix_generate_remap_and_verify(struct iovec *iovs, int iovcnt, struct iovec *md_iov,
			      uint32_t block_size, uint32_t md_size, uint32_t num_blocks,
			      bool dif_loc, enum spdk_dif_type dif_type, uint32_t dif_flags,
			      uint32_t init_ref_tag, uint32_t remapped_init_ref_tag,
			      uint16_t apptag_mask, uint16_t app_tag)
{
	struct spdk_dif_ctx ctx;
	int rc;

	rc = ut_data_pattern_generate(iovs, iovcnt, block_size, 0, num_blocks);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_ctx_init(&ctx, block_size, md_size, false, dif_loc, dif_type, dif_flags,
			       init_ref_tag, apptag_mask, app_tag, 0, GUARD_SEED);
	CU_ASSERT(rc == 0);

	rc = spdk_dix_generate(iovs, iovcnt, md_iov, num_blocks, &ctx);
	CU_ASSERT(rc == 0);

	spdk_dif_ctx_set_remapped_init_ref_tag(&ctx, remapped_init_ref_tag);

	rc = spdk_dix_remap_ref_tag(md_iov, num_blocks, &ctx, NULL);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_ctx_init(&ctx, block_size, md_size, false, dif_loc, dif_type, dif_flags,
			       remapped_init_ref_tag, apptag_mask, app_tag, 0, GUARD_SEED);
	CU_ASSERT(rc == 0);

	rc = spdk_dix_verify(iovs, iovcnt, md_iov, num_blocks, &ctx, NULL);
	CU_ASSERT(rc == 0);

	rc = ut_data_pattern_verify(iovs, iovcnt, block_size, 0, num_blocks);
	CU_ASSERT(rc == 0);
}

static void
dix_sec_4096_md_128_prchk_7_multi_iovs_remap(void)
{
	struct iovec iovs[4], md_iov;
	uint32_t dif_flags;
	int i, num_blocks;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	num_blocks = 0;

	for (i = 0; i < 4; i++) {
		_iov_alloc_buf(&iovs[i], 4096 * (i + 1));
		num_blocks += i + 1;
	}
	_iov_alloc_buf(&md_iov, 128 * num_blocks);

	dix_generate_remap_and_verify(iovs, 4, &md_iov, 4096, 128, num_blocks, false, SPDK_DIF_TYPE1,
				      dif_flags, 22, 99, 0xFFFF, 0x22);
	dix_generate_remap_and_verify(iovs, 4, &md_iov, 4096, 128, num_blocks, true, SPDK_DIF_TYPE1,
				      dif_flags, 22, 99, 0xFFFF, 0x22);

	for (i = 0; i < 4; i++) {
		_iov_free_buf(&iovs[i]);
	}
	_iov_free_buf(&md_iov);
}

static void
dix_sec_512_md_8_prchk_7_multi_iovs_complex_splits_remap(void)
{
	struct iovec iovs[6], md_iov;
	uint32_t dif_flags;
	int i;

	dif_flags = SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
		    SPDK_DIF_FLAGS_REFTAG_CHECK;

	/* data[0][255:0] */
	_iov_alloc_buf(&iovs[0], 256);

	/* data[0][511:256], data[1][255:0] */
	_iov_alloc_buf(&iovs[1], 256 + 256);

	/* data[1][382:256] */
	_iov_alloc_buf(&iovs[2], 128);

	/* data[1][383] */
	_iov_alloc_buf(&iovs[3], 1);

	/* data[1][510:384] */
	_iov_alloc_buf(&iovs[4], 126);

	/* data[1][511], data[2][511:0], data[3][511:0] */
	_iov_alloc_buf(&iovs[5], 1 + 512 * 2);

	_iov_alloc_buf(&md_iov, 8 * 4);

	dix_generate_remap_and_verify(iovs, 6, &md_iov, 512, 8, 4, false, SPDK_DIF_TYPE1,
				      dif_flags, 22, 99, 0xFFFF, 0x22);

	for (i = 0; i < 6; i++) {
		_iov_free_buf(&iovs[i]);
	}
	_iov_free_buf(&md_iov);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("dif", NULL, NULL);

	CU_ADD_TEST(suite, dif_generate_and_verify_test);
	CU_ADD_TEST(suite, dif_disable_check_test);
	CU_ADD_TEST(suite, dif_sec_512_md_0_error_test);
	CU_ADD_TEST(suite, dif_guard_seed_test);
	CU_ADD_TEST(suite, dif_disable_sec_512_md_8_single_iov_test);
	CU_ADD_TEST(suite, dif_sec_512_md_8_prchk_0_single_iov_test);
	CU_ADD_TEST(suite, dif_sec_512_md_8_prchk_0_1_2_4_multi_iovs_test);
	CU_ADD_TEST(suite, dif_sec_4096_md_128_prchk_7_multi_iovs_test);
	CU_ADD_TEST(suite, dif_sec_512_md_8_prchk_7_multi_iovs_split_data_and_md_test);
	CU_ADD_TEST(suite, dif_sec_512_md_8_prchk_7_multi_iovs_split_data_test);
	CU_ADD_TEST(suite, dif_sec_512_md_8_prchk_7_multi_iovs_split_guard_test);
	CU_ADD_TEST(suite, dif_sec_512_md_8_prchk_7_multi_iovs_split_apptag_test);
	CU_ADD_TEST(suite, dif_sec_512_md_8_prchk_7_multi_iovs_split_reftag_test);
	CU_ADD_TEST(suite, dif_sec_512_md_8_prchk_7_multi_iovs_complex_splits_test);
	CU_ADD_TEST(suite, dif_sec_4096_md_128_prchk_7_multi_iovs_complex_splits_test);
	CU_ADD_TEST(suite, dif_sec_4096_md_128_inject_1_2_4_8_multi_iovs_test);
	CU_ADD_TEST(suite, dif_sec_4096_md_128_inject_1_2_4_8_multi_iovs_split_data_and_md_test);
	CU_ADD_TEST(suite, dif_sec_4096_md_128_inject_1_2_4_8_multi_iovs_split_data_test);
	CU_ADD_TEST(suite, dif_sec_4096_md_128_inject_1_2_4_8_multi_iovs_split_guard_test);
	CU_ADD_TEST(suite, dif_sec_4096_md_128_inject_1_2_4_8_multi_iovs_split_apptag_test);
	CU_ADD_TEST(suite, dif_sec_4096_md_128_inject_1_2_4_8_multi_iovs_split_reftag_test);
	CU_ADD_TEST(suite, dif_copy_sec_512_md_8_prchk_0_single_iov);
	CU_ADD_TEST(suite, dif_copy_sec_512_md_8_prchk_0_1_2_4_multi_iovs);
	CU_ADD_TEST(suite, dif_copy_sec_4096_md_128_prchk_7_multi_iovs);
	CU_ADD_TEST(suite, dif_copy_sec_512_md_8_prchk_7_multi_iovs_split_data);
	CU_ADD_TEST(suite, dif_copy_sec_512_md_8_prchk_7_multi_iovs_complex_splits);
	CU_ADD_TEST(suite, dif_copy_sec_4096_md_128_inject_1_2_4_8_multi_iovs_test);
	CU_ADD_TEST(suite, dif_copy_sec_4096_md_128_inject_1_2_4_8_multi_iovs_split_test);
	CU_ADD_TEST(suite, dix_sec_512_md_0_error);
	CU_ADD_TEST(suite, dix_sec_512_md_8_prchk_0_single_iov);
	CU_ADD_TEST(suite, dix_sec_512_md_8_prchk_0_1_2_4_multi_iovs);
	CU_ADD_TEST(suite, dix_sec_4096_md_128_prchk_7_multi_iovs);
	CU_ADD_TEST(suite, dix_sec_512_md_8_prchk_7_multi_iovs_split_data);
	CU_ADD_TEST(suite, dix_sec_512_md_8_prchk_7_multi_iovs_complex_splits);
	CU_ADD_TEST(suite, dix_sec_4096_md_128_inject_1_2_4_8_multi_iovs_test);
	CU_ADD_TEST(suite, dix_sec_4096_md_128_inject_1_2_4_8_multi_iovs_split_test);
	CU_ADD_TEST(suite, set_md_interleave_iovs_test);
	CU_ADD_TEST(suite, set_md_interleave_iovs_split_test);
	CU_ADD_TEST(suite, dif_generate_stream_test);
	CU_ADD_TEST(suite, set_md_interleave_iovs_alignment_test);
	CU_ADD_TEST(suite, _dif_generate_split_test);
	CU_ADD_TEST(suite, set_md_interleave_iovs_multi_segments_test);
	CU_ADD_TEST(suite, _dif_verify_split_test);
	CU_ADD_TEST(suite, dif_verify_stream_multi_segments_test);
	CU_ADD_TEST(suite, update_crc32c_test);
	CU_ADD_TEST(suite, _dif_update_crc32c_split_test);
	CU_ADD_TEST(suite, dif_update_crc32c_stream_multi_segments_test);
	CU_ADD_TEST(suite, get_range_with_md_test);
	CU_ADD_TEST(suite, dif_sec_4096_md_128_prchk_7_multi_iovs_remap_test);
	CU_ADD_TEST(suite, dif_sec_4096_md_128_prchk_7_multi_iovs_complex_splits_remap_test);
	CU_ADD_TEST(suite, dix_sec_4096_md_128_prchk_7_multi_iovs_remap);
	CU_ADD_TEST(suite, dix_sec_512_md_8_prchk_7_multi_iovs_complex_splits_remap);

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
