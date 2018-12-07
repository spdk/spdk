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

#define DATA_PATTERN	0xAB

static void
_data_pattern_generate(struct iovec *iovs, int iovcnt,
		       uint32_t block_size, uint32_t md_size)
{
	struct _iov_iter iter;
	uint32_t offset_in_block;
	uint32_t buf_len;
	void *buf;

	_iov_iter_init(&iter, iovs, iovcnt);

	while (_iov_iter_cont(&iter)) {
		offset_in_block = _iov_iter_get_payload_offset(&iter) % block_size;

		_iov_iter_get_buf(&iter, &buf, &buf_len);

		if (offset_in_block < block_size - md_size) {
			buf_len = spdk_min(block_size - md_size - offset_in_block,
					   buf_len);
			memset(buf, DATA_PATTERN, buf_len);
		} else {
			buf_len = spdk_min(block_size - offset_in_block, buf_len);
			memset(buf, 0, buf_len);
		}

		_iov_iter_advance(&iter, buf_len);
	}
}

static int
_data_pattern_verify(struct iovec *iovs, int iovcnt,
		     uint32_t block_size, uint32_t md_size)
{
	struct _iov_iter iter;
	uint32_t offset_in_block;
	uint32_t buf_len, i;
	uint8_t *buf;

	_iov_iter_init(&iter, iovs, iovcnt);

	while (_iov_iter_cont(&iter)) {
		offset_in_block = _iov_iter_get_payload_offset(&iter) % block_size;

		_iov_iter_get_buf(&iter, (void **)&buf, &buf_len);

		if (offset_in_block < block_size - md_size) {
			buf_len = spdk_min(block_size - md_size - offset_in_block,
					   buf_len);
			for (i = 0; i < buf_len; i++) {
				if (buf[i] != DATA_PATTERN) {
					return -1;
				}
			}
		} else {
			buf_len = spdk_min(block_size - offset_in_block, buf_len);
		}

		_iov_iter_advance(&iter, buf_len);
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
dif_generate_and_verify_test(void)
{
	struct iovec iov;
	uint32_t dif_flags;
	int rc;

	dif_flags = SPDK_DIF_GUARD_CHECK | SPDK_DIF_APPTAG_CHECK | SPDK_DIF_REFTAG_CHECK;

	_iov_alloc_buf(&iov, 4096 + 128);

	_data_pattern_generate(&iov, 1, 4096 + 128, 128);

	_dif_generate(iov.iov_base + 4096, iov.iov_base, 4096, dif_flags, 22, 0x22);

	rc = _dif_verify(iov.iov_base + 4096, iov.iov_base, 4096, 1, dif_flags,
			 22, 0xFFFF, 0x22);
	CU_ASSERT(rc == 0);

	rc = _data_pattern_verify(&iov, 1, 4096 + 128, 128);
	CU_ASSERT(rc == 0);

	_iov_free_buf(&iov);
}

static void
sec_512_md_0_error_test(void)
{
	struct iovec iov = {0};
	int rc;

	rc = spdk_dif_generate(&iov, 1, 512, 0, true, 1, 0, 0, 0);
	CU_ASSERT(rc != 0);

	rc = spdk_dif_verify(&iov, 1, 512, 0, true, 1, 0, 0, 0, 0);
	CU_ASSERT(rc != 0);
}

static void
dif_generate_and_verify(struct iovec *iovs, int iovcnt,
			uint32_t block_size, uint32_t md_size,
			bool dif_start, uint32_t dif_type, uint32_t dif_flags,
			uint32_t ref_tag, uint16_t apptag_mask, uint16_t app_tag)
{
	int rc;

	_data_pattern_generate(iovs, iovcnt, block_size, md_size);

	rc = spdk_dif_generate(iovs, iovcnt, block_size, md_size, dif_start,
			       dif_type, dif_flags, ref_tag, app_tag);
	CU_ASSERT(rc == 0);

	rc = spdk_dif_verify(iovs, iovcnt, block_size, md_size,
			     dif_start, dif_type, dif_flags,
			     ref_tag, apptag_mask, app_tag);
	CU_ASSERT(rc == 0);

	rc = _data_pattern_verify(iovs, iovcnt, block_size, md_size);
	CU_ASSERT(rc == 0);
}

static void
sec_512_md_8_prchk_0_single_iov_test(void)
{
	struct iovec iov;

	_iov_alloc_buf(&iov, (512 + 8) * 4);

	dif_generate_and_verify(&iov, 1, 512 + 8, 8, true, 1, 0, 0, 0, 0);

	_iov_free_buf(&iov);
}

static void
sec_512_md_8_prchk_0_1_2_4_multi_iovs_test(void)
{
	struct iovec iovs[4];
	int i;

	for (i = 0; i < 4; i++) {
		_iov_alloc_buf(&iovs[i], (512 + 8) * (i + 1));
	}

	dif_generate_and_verify(iovs, 4, 512 + 8, 8, true, 1, 0, 22, 0xFFFF, 0x22);

	dif_generate_and_verify(iovs, 4, 512 + 8, 8, true, 1, SPDK_DIF_GUARD_CHECK,
				22, 0xFFFF, 0x22);

	dif_generate_and_verify(iovs, 4, 512 + 8, 8, true, 1, SPDK_DIF_APPTAG_CHECK,
				22, 0xFFFF, 0x22);

	dif_generate_and_verify(iovs, 4, 512 + 8, 8, true, 1, SPDK_DIF_REFTAG_CHECK,
				22, 0xFFFF, 0x22);

	for (i = 0; i < 4; i++) {
		_iov_free_buf(&iovs[i]);
	}
}

static void
sec_4096_md_128_prchk_7_multi_iovs_test(void)
{
	struct iovec iovs[4];
	int i;
	uint32_t dif_flags;

	dif_flags = SPDK_DIF_GUARD_CHECK | SPDK_DIF_APPTAG_CHECK | SPDK_DIF_REFTAG_CHECK;

	for (i = 0; i < 4; i++) {
		_iov_alloc_buf(&iovs[i], (4096 + 128) * (i + 1));
	}

	dif_generate_and_verify(iovs, 4, 4096 + 128, 128, true, 1, dif_flags, 22, 0xFFFF, 0x22);

	for (i = 0; i < 4; i++) {
		_iov_free_buf(&iovs[i]);
	}
}

static void
sec_512_md_8_prchk_7_multi_iovs_split_data_and_md_test(void)
{
	struct iovec iovs[2];
	uint32_t dif_flags;

	dif_flags = SPDK_DIF_GUARD_CHECK | SPDK_DIF_APPTAG_CHECK | SPDK_DIF_REFTAG_CHECK;

	_iov_alloc_buf(&iovs[0], 512);
	_iov_alloc_buf(&iovs[1], 8);

	dif_generate_and_verify(iovs, 2, 512 + 8, 8, true, 1, dif_flags, 22, 0xFFFF, 0x22);

	_iov_free_buf(&iovs[0]);
	_iov_free_buf(&iovs[1]);
}

static void
sec_512_md_8_prchk_7_multi_iovs_split_data_test(void)
{
	struct iovec iovs[2];
	uint32_t dif_flags;

	dif_flags = SPDK_DIF_GUARD_CHECK | SPDK_DIF_APPTAG_CHECK | SPDK_DIF_REFTAG_CHECK;

	_iov_alloc_buf(&iovs[0], 256);
	_iov_alloc_buf(&iovs[1], 264);

	dif_generate_and_verify(iovs, 2, 512 + 8, 8, true, 1, dif_flags, 22, 0xFFFF, 0x22);

	_iov_free_buf(&iovs[0]);
	_iov_free_buf(&iovs[1]);
}

static void
sec_512_md_8_prchk_7_multi_iovs_split_guard_test(void)
{
	struct iovec iovs[2];
	uint32_t dif_flags;

	dif_flags = SPDK_DIF_GUARD_CHECK | SPDK_DIF_APPTAG_CHECK | SPDK_DIF_REFTAG_CHECK;

	_iov_alloc_buf(&iovs[0], 513);
	_iov_alloc_buf(&iovs[1], 7);

	dif_generate_and_verify(iovs, 2, 512 + 8, 8, true, 1, dif_flags, 22, 0xFFFF, 0x22);

	_iov_free_buf(&iovs[0]);
	_iov_free_buf(&iovs[1]);
}

static void
sec_512_md_8_prchk_7_multi_iovs_split_apptag_test(void)
{
	struct iovec iovs[2];
	uint32_t dif_flags;

	dif_flags = SPDK_DIF_GUARD_CHECK | SPDK_DIF_APPTAG_CHECK | SPDK_DIF_REFTAG_CHECK;

	_iov_alloc_buf(&iovs[0], 515);
	_iov_alloc_buf(&iovs[1], 5);

	dif_generate_and_verify(iovs, 2, 512 + 8, 8, true, 1, dif_flags, 22, 0xFFFF, 0x22);

	_iov_free_buf(&iovs[0]);
	_iov_free_buf(&iovs[1]);
}

static void
sec_512_md_8_prchk_7_multi_iovs_split_reftag_test(void)
{
	struct iovec iovs[2];
	uint32_t dif_flags;

	dif_flags = SPDK_DIF_GUARD_CHECK | SPDK_DIF_APPTAG_CHECK | SPDK_DIF_REFTAG_CHECK;

	_iov_alloc_buf(&iovs[0], 518);
	_iov_alloc_buf(&iovs[1], 2);

	dif_generate_and_verify(iovs, 2, 512 + 8, 8, true, 1, dif_flags, 22, 0xFFFF, 0x22);

	_iov_free_buf(&iovs[0]);
	_iov_free_buf(&iovs[1]);
}

static void
sec_512_md_8_prchk_7_multi_iovs_complex_splits_test(void)
{
	struct iovec iovs[9];
	uint32_t dif_flags;
	int i;

	dif_flags = SPDK_DIF_GUARD_CHECK | SPDK_DIF_APPTAG_CHECK | SPDK_DIF_REFTAG_CHECK;

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

	dif_generate_and_verify(iovs, 9, 512 + 8, 8, true, 1, dif_flags, 22, 0xFFFF, 0x22);

	for (i = 0; i < 9; i++) {
		_iov_free_buf(&iovs[i]);
	}
}

static void
sec_4096_md_128_prchk_7_multi_iovs_complex_splits_test(void)
{
	struct iovec iovs[11];
	uint32_t dif_flags;
	int i;

	dif_flags = SPDK_DIF_GUARD_CHECK | SPDK_DIF_APPTAG_CHECK | SPDK_DIF_REFTAG_CHECK;

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

	dif_generate_and_verify(iovs, 11, 4096 + 128, 128, true, 1, dif_flags, 22, 0xFFFF, 0x22);

	for (i = 0; i < 11; i++) {
		_iov_free_buf(&iovs[i]);
	}
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("dif", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "dif_generate_and_verify_test", dif_generate_and_verify_test) == NULL ||
		CU_add_test(suite, "sec_512_md_0_error_test", sec_512_md_0_error_test) == NULL ||
		CU_add_test(suite, "sec_512_md_8_prchk_0_single_iov_test",
			    sec_512_md_8_prchk_0_single_iov_test) == NULL ||
		CU_add_test(suite, "sec_512_md_8_prchk_0_1_2_4_multi_iovs_test",
			    sec_512_md_8_prchk_0_1_2_4_multi_iovs_test) == NULL ||
		CU_add_test(suite, "sec_4096_md_128_prchk_7_multi_iovs_test",
			    sec_4096_md_128_prchk_7_multi_iovs_test) == NULL ||
		CU_add_test(suite, "sec_512_md_8_prchk_7_multi_iovs_split_data_and_md_test",
			    sec_512_md_8_prchk_7_multi_iovs_split_data_and_md_test) == NULL ||
		CU_add_test(suite, "sec_512_md_8_prchk_7_multi_iovs_split_data_test",
			    sec_512_md_8_prchk_7_multi_iovs_split_data_test) == NULL ||
		CU_add_test(suite, "sec_512_md_8_prchk_7_multi_iovs_split_guard_test",
			    sec_512_md_8_prchk_7_multi_iovs_split_guard_test) == NULL ||
		CU_add_test(suite, "sec_512_md_8_prchk_7_multi_iovs_split_apptag_test",
			    sec_512_md_8_prchk_7_multi_iovs_split_apptag_test) == NULL ||
		CU_add_test(suite, "sec_512_md_8_prchk_7_multi_iovs_split_reftag_test",
			    sec_512_md_8_prchk_7_multi_iovs_split_reftag_test) == NULL ||
		CU_add_test(suite, "sec_512_md_8_prchk_7_multi_iovs_complex_splits_test",
			    sec_512_md_8_prchk_7_multi_iovs_complex_splits_test) == NULL ||
		CU_add_test(suite, "sec_4096_md_128_prchk_7_multi_iovs_complex_splits_test",
			    sec_4096_md_128_prchk_7_multi_iovs_complex_splits_test) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
