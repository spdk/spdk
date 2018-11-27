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

#include "util/t10dif.c"

#define DATA_PATTERN	0xAB

static void
_data_pattern_generate(struct iovec *iovs, int iovcnt,
		       uint32_t data_block_size, uint32_t metadata_size)
{
	uint32_t block_size, payload_offset, offset_in_block;
	uint32_t iov_offset, buf_len;
	int iovpos;
	void *buf;

	block_size = data_block_size + metadata_size;
	payload_offset = 0;
	iovpos = 0;
	iov_offset = 0;

	while (iovpos < iovcnt) {
		offset_in_block = payload_offset % block_size;

		buf = iovs[iovpos].iov_base + iov_offset;
		buf_len = iovs[iovpos].iov_len - iov_offset;

		if (offset_in_block < data_block_size) {
			buf_len = spdk_min(buf_len, data_block_size - offset_in_block);
			memset(buf, DATA_PATTERN, buf_len);
		} else {
			buf_len = spdk_min(buf_len, block_size - offset_in_block);
			memset(buf, 0, buf_len);
		}

		payload_offset += buf_len;
		iov_offset += buf_len;
		if (iov_offset == iovs[iovpos].iov_len) {
			iovpos++;
			iov_offset = 0;
		}
	}
}

static int
_data_pattern_verify(struct iovec *iovs, int iovcnt,
		     uint32_t data_block_size, uint32_t metadata_size)
{
	uint32_t block_size, payload_offset, offset_in_block;
	uint32_t iov_offset, buf_len, i;
	int iovpos;
	uint8_t *buf;

	block_size = data_block_size + metadata_size;
	payload_offset = 0;
	iovpos = 0;
	iov_offset = 0;

	while (iovpos < iovcnt) {
		offset_in_block = payload_offset % block_size;

		buf = (uint8_t *)(iovs[iovpos].iov_base + iov_offset);
		buf_len = iovs[iovpos].iov_len - iov_offset;

		if (offset_in_block < data_block_size) {
			buf_len = spdk_min(buf_len, data_block_size - offset_in_block);
			for (i = 0; i < buf_len; i++) {
				if (buf[i] != DATA_PATTERN) {
					return -1;
				}
			}
		} else {
			buf_len = spdk_min(buf_len, block_size - offset_in_block);
		}

		payload_offset += buf_len;
		iov_offset += buf_len;
		if (iov_offset == iovs[iovpos].iov_len) {
			iovpos++;
			iov_offset = 0;
		}
	}

	return 0;
}

static void
t10dif_generate_and_verify(void)
{
	struct iovec iov;
	uint32_t dif_flags;
	int rc;

	dif_flags = SPDK_T10DIF_GUARD_CHECK | SPDK_T10DIF_APPTAG_CHECK | SPDK_T10DIF_REFTAG_CHECK;

	iov.iov_base = calloc(1, 4096 + 128);
	iov.iov_len = 4096 + 128;
	SPDK_CU_ASSERT_FATAL(iov.iov_base != NULL);

	_data_pattern_generate(&iov, 1, 4096, 128);

	_t10dif_generate(iov.iov_base + 4096, iov.iov_base, 4096, dif_flags, 22, 0x22);

	rc = _t10dif_verify(iov.iov_base + 4096, iov.iov_base, 4096, dif_flags, 22, 0, 0x22);
	CU_ASSERT(rc == 0);

	rc = _data_pattern_verify(&iov, 1, 4096, 128);
	CU_ASSERT(rc == 0);

	free(iov.iov_base);
}

static void
sec_512_md_0_error(void)
{
	struct iovec iov = {0};
	int rc;

	rc = spdk_t10dif_generate(&iov, 1, 512, 0, 0, 0, 0);
	CU_ASSERT(rc != 0);

	rc = spdk_t10dif_verify(&iov, 1, 512, 0, 0, 0, 0, 0);
	CU_ASSERT(rc != 0);
}

static void
sec_512_md_8_prchk_0_single_iov(void)
{
	struct iovec iov;
	int rc;

	iov.iov_base = calloc(1, (512 + 8) * 4);
	iov.iov_len = (512 + 8) * 4;
	SPDK_CU_ASSERT_FATAL(iov.iov_base != NULL);

	_data_pattern_generate(&iov, 1, 512, 8);

	rc = spdk_t10dif_generate(&iov, 1, 512, 8, 0, 0, 0);
	CU_ASSERT(rc == 0);

	rc = spdk_t10dif_verify(&iov, 1, 512, 8, 0, 0, 0, 0);
	CU_ASSERT(rc == 0);

	rc = _data_pattern_verify(&iov, 1, 512, 8);
	CU_ASSERT(rc == 0);

	free(iov.iov_base);
}

static void
sec_512_md_8_prchk_0_multi_iovs(void)
{
	struct iovec iovs[4];
	int i, rc;

	for (i = 0; i < 4; i++) {
		iovs[i].iov_base = calloc(1, (512 + 8) * (i + 1));
		iovs[i].iov_len = (512 + 8) * (i + 1);
		SPDK_CU_ASSERT_FATAL(iovs[i].iov_base != NULL);
	}

	_data_pattern_generate(iovs, 4, 512, 8);

	rc = spdk_t10dif_generate(iovs, 4, 512, 8, 0, 0, 0);
	CU_ASSERT(rc == 0);

	rc = spdk_t10dif_verify(iovs, 4, 512, 8, 0, 0, 0, 0);
	CU_ASSERT(rc == 0);

	rc = _data_pattern_verify(iovs, 4, 512, 8);
	CU_ASSERT(rc == 0);

	for (i = 0; i < 4; i++) {
		free(iovs[i].iov_base);
	}
}

static void
sec_512_md_8_prchk_1_multi_iovs(void)
{
	struct iovec iovs[4];
	int i, rc;
	uint32_t dif_flags;

	dif_flags = SPDK_T10DIF_GUARD_CHECK;

	for (i = 0; i < 4; i++) {
		iovs[i].iov_base = calloc(1, (512 + 8) * (i + 1));
		iovs[i].iov_len = (512 + 8) * (i + 1);
		SPDK_CU_ASSERT_FATAL(iovs[i].iov_base != NULL);
	}

	_data_pattern_generate(iovs, 4, 512, 8);

	spdk_t10dif_generate(iovs, 4, 512, 8, dif_flags, 22, 0x22);
	CU_ASSERT(rc == 0);

	rc = spdk_t10dif_verify(iovs, 4, 512, 8, dif_flags, 22, 0, 0x22);
	CU_ASSERT(rc == 0);

	rc = _data_pattern_verify(iovs, 4, 512, 8);
	CU_ASSERT(rc == 0);

	for (i = 0; i < 4; i++) {
		free(iovs[i].iov_base);
	}
}

static void
sec_512_md_8_prchk_2_multi_iovs(void)
{
	struct iovec iovs[4];
	int i, rc;
	uint32_t dif_flags;

	dif_flags = SPDK_T10DIF_APPTAG_CHECK;

	for (i = 0; i < 4; i++) {
		iovs[i].iov_base = calloc(1, (512 + 8) * (i + 1));
		iovs[i].iov_len = (512 + 8) * (i + 1);
		SPDK_CU_ASSERT_FATAL(iovs[i].iov_base != NULL);
	}

	_data_pattern_generate(iovs, 4, 512, 8);

	rc = spdk_t10dif_generate(iovs, 4, 512, 8, dif_flags, 22, 0x22);
	CU_ASSERT(rc == 0);

	rc = spdk_t10dif_verify(iovs, 4, 512, 8, dif_flags, 22, 0, 0x22);
	CU_ASSERT(rc == 0);

	rc = _data_pattern_verify(iovs, 4, 512, 8);
	CU_ASSERT(rc == 0);

	for (i = 0; i < 4; i++) {
		free(iovs[i].iov_base);
	}
}

static void
sec_512_md_8_prchk_4_multi_iovs(void)
{
	struct iovec iovs[4];
	int i, rc;
	uint32_t dif_flags;

	dif_flags = SPDK_T10DIF_REFTAG_CHECK;

	for (i = 0; i < 4; i++) {
		iovs[i].iov_base = calloc(1, (512 + 8) * (i + 1));
		iovs[i].iov_len = (512 + 8) * (i + 1);
		SPDK_CU_ASSERT_FATAL(iovs[i].iov_base != NULL);
	}

	_data_pattern_generate(iovs, 4, 512, 8);

	rc = spdk_t10dif_generate(iovs, 4, 512, 8, dif_flags, 22, 0x22);
	CU_ASSERT(rc == 0);

	rc = spdk_t10dif_verify(iovs, 4, 512, 8, dif_flags, 22, 0, 0x22);
	CU_ASSERT(rc == 0);

	rc = _data_pattern_verify(iovs, 4, 512, 8);
	CU_ASSERT(rc == 0);

	for (i = 0; i < 4; i++) {
		free(iovs[i].iov_base);
	}
}

static void
sec_4096_md_128_prchk_7_multi_iovs(void)
{
	struct iovec iovs[4];
	uint32_t dif_flags;
	int i, rc;

	dif_flags = SPDK_T10DIF_GUARD_CHECK | SPDK_T10DIF_APPTAG_CHECK | SPDK_T10DIF_REFTAG_CHECK;

	for (i = 0; i < 4; i++) {
		iovs[i].iov_base = calloc(1, (4096 + 128) * (i + 1));
		iovs[i].iov_len = (4096 + 128) * (i + 1);
		SPDK_CU_ASSERT_FATAL(iovs[i].iov_base != NULL);
	}

	_data_pattern_generate(iovs, 4, 4096, 128);

	rc = spdk_t10dif_generate(iovs, 4, 4096, 128, dif_flags, 22, 0x22);
	CU_ASSERT(rc == 0);

	rc = spdk_t10dif_verify(iovs, 4, 4096, 128, dif_flags, 22, 0, 0x22);
	CU_ASSERT(rc == 0);

	rc = _data_pattern_verify(iovs, 4, 4096, 128);
	CU_ASSERT(rc == 0);

	for (i = 0; i < 4; i++) {
		free(iovs[i].iov_base);
	}
}

static void
sec_512_md_8_prchk_7_multi_iovs_split_data_and_md(void)
{
	struct iovec iovs[2];
	uint32_t dif_flags;
	int rc;

	dif_flags = SPDK_T10DIF_GUARD_CHECK | SPDK_T10DIF_APPTAG_CHECK | SPDK_T10DIF_REFTAG_CHECK;

	iovs[0].iov_base = calloc(1, 512);
	iovs[0].iov_len = 512;
	SPDK_CU_ASSERT_FATAL(iovs[0].iov_base != NULL);

	iovs[1].iov_base = calloc(1, 8);
	iovs[1].iov_len = 8;
	SPDK_CU_ASSERT_FATAL(iovs[1].iov_base != NULL);

	_data_pattern_generate(iovs, 2, 512, 8);

	rc = spdk_t10dif_generate(iovs, 2, 512, 8, dif_flags, 22, 0x22);
	CU_ASSERT(rc == 0);

	rc = spdk_t10dif_verify(iovs, 2, 512, 8, dif_flags, 22, 0, 0x22);
	CU_ASSERT(rc == 0);

	rc = _data_pattern_verify(iovs, 2, 512, 8);
	CU_ASSERT(rc == 0);

	free(iovs[0].iov_base);
	free(iovs[1].iov_base);
}

static void
sec_512_md_8_prchk_7_multi_iovs_split_data(void)
{
	struct iovec iovs[2];
	uint32_t dif_flags;
	int rc;

	dif_flags = SPDK_T10DIF_GUARD_CHECK | SPDK_T10DIF_APPTAG_CHECK | SPDK_T10DIF_REFTAG_CHECK;

	iovs[0].iov_base = calloc(1, 256);
	iovs[0].iov_len = 256;
	SPDK_CU_ASSERT_FATAL(iovs[0].iov_base != NULL);

	iovs[1].iov_base = calloc(1, 264);
	iovs[1].iov_len = 264;
	SPDK_CU_ASSERT_FATAL(iovs[1].iov_base != NULL);

	_data_pattern_generate(iovs, 2, 512, 8);

	rc = spdk_t10dif_generate(iovs, 2, 512, 8, dif_flags, 22, 0x22);
	CU_ASSERT(rc == 0);

	rc = spdk_t10dif_verify(iovs, 2, 512, 8, dif_flags, 22, 0, 0x22);
	CU_ASSERT(rc == 0);

	rc = _data_pattern_verify(iovs, 2, 512, 8);
	CU_ASSERT(rc == 0);

	free(iovs[0].iov_base);
	free(iovs[1].iov_base);
}

static void
sec_512_md_8_prchk_7_multi_iovs_split_guard(void)
{
	struct iovec iovs[2];
	uint32_t dif_flags;
	int rc;

	dif_flags = SPDK_T10DIF_GUARD_CHECK | SPDK_T10DIF_APPTAG_CHECK | SPDK_T10DIF_REFTAG_CHECK;

	iovs[0].iov_base = calloc(1, 513);
	iovs[0].iov_len = 513;
	SPDK_CU_ASSERT_FATAL(iovs[0].iov_base != NULL);

	iovs[1].iov_base = calloc(1, 7);
	iovs[1].iov_len = 7;
	SPDK_CU_ASSERT_FATAL(iovs[1].iov_base != NULL);

	_data_pattern_generate(iovs, 2, 512, 8);

	rc = spdk_t10dif_generate(iovs, 2, 512, 8, dif_flags, 22, 0x22);
	CU_ASSERT(rc == 0);

	rc = spdk_t10dif_verify(iovs, 2, 512, 8, dif_flags, 22, 0, 0x22);
	CU_ASSERT(rc == 0);

	rc = _data_pattern_verify(iovs, 2, 512, 8);
	CU_ASSERT(rc == 0);

	free(iovs[0].iov_base);
	free(iovs[1].iov_base);
}

static void
sec_512_md_8_prchk_7_multi_iovs_split_apptag(void)
{
	struct iovec iovs[2];
	uint32_t dif_flags;
	int rc;

	dif_flags = SPDK_T10DIF_GUARD_CHECK | SPDK_T10DIF_APPTAG_CHECK | SPDK_T10DIF_REFTAG_CHECK;

	iovs[0].iov_base = calloc(1, 515);
	iovs[0].iov_len = 515;
	SPDK_CU_ASSERT_FATAL(iovs[0].iov_base != NULL);

	iovs[1].iov_base = calloc(1, 5);
	iovs[1].iov_len = 5;
	SPDK_CU_ASSERT_FATAL(iovs[1].iov_base != NULL);

	_data_pattern_generate(iovs, 2, 512, 8);

	rc = spdk_t10dif_generate(iovs, 2, 512, 8, dif_flags, 22, 0x22);
	CU_ASSERT(rc == 0);

	rc = spdk_t10dif_verify(iovs, 2, 512, 8, dif_flags, 22, 0, 0x22);
	CU_ASSERT(rc == 0);

	rc = _data_pattern_verify(iovs, 2, 512, 8);
	CU_ASSERT(rc == 0);

	free(iovs[0].iov_base);
	free(iovs[1].iov_base);
}

static void
sec_512_md_8_prchk_7_multi_iovs_split_reftag(void)
{
	struct iovec iovs[2];
	uint32_t dif_flags;
	int rc;

	dif_flags = SPDK_T10DIF_GUARD_CHECK | SPDK_T10DIF_APPTAG_CHECK | SPDK_T10DIF_REFTAG_CHECK;

	iovs[0].iov_base = calloc(1, 512);
	iovs[0].iov_len = 512;
	SPDK_CU_ASSERT_FATAL(iovs[0].iov_base != NULL);

	iovs[1].iov_base = calloc(1, 8);
	iovs[1].iov_len = 8;
	SPDK_CU_ASSERT_FATAL(iovs[1].iov_base != NULL);

	_data_pattern_generate(iovs, 2, 512, 8);

	rc = spdk_t10dif_generate(iovs, 2, 512, 8, dif_flags, 22, 0x22);
	CU_ASSERT(rc == 0);

	rc = spdk_t10dif_verify(iovs, 2, 512, 8, dif_flags, 22, 0, 0x22);
	CU_ASSERT(rc == 0);

	rc = _data_pattern_verify(iovs, 2, 512, 8);
	CU_ASSERT(rc == 0);

	free(iovs[0].iov_base);
	free(iovs[1].iov_base);
}

static void
sec_512_md_8_prchk_7_multi_iovs_complex_splits(void)
{
	struct iovec iovs[9];
	uint32_t dif_flags;
	int i, rc;

	dif_flags = SPDK_T10DIF_GUARD_CHECK | SPDK_T10DIF_APPTAG_CHECK | SPDK_T10DIF_REFTAG_CHECK;

	/* data[0][255:0] */
	iovs[0].iov_base = calloc(1, 256);
	iovs[0].iov_len = 256;
	SPDK_CU_ASSERT_FATAL(iovs[0].iov_base != NULL);

	/* data[0][511:256], guard[0][0] */
	iovs[1].iov_base = calloc(1, 256 + 1);
	iovs[1].iov_len = 256 + 1;
	SPDK_CU_ASSERT_FATAL(iovs[1].iov_base != NULL);

	/* guard[0][1], apptag[0][0] */
	iovs[2].iov_base = calloc(1, 1 + 1);
	iovs[2].iov_len = 1 + 1;
	SPDK_CU_ASSERT_FATAL(iovs[2].iov_base != NULL);

	/* apptag[0][1], reftag[0][0] */
	iovs[3].iov_base = calloc(1, 1 + 1);
	iovs[3].iov_len = 1 + 1;
	SPDK_CU_ASSERT_FATAL(iovs[3].iov_base != NULL);

	/* reftag[0][3:1], data[1][255:0] */
	iovs[4].iov_base = calloc(1, 3 + 256);
	iovs[4].iov_len = 3 + 256;
	SPDK_CU_ASSERT_FATAL(iovs[4].iov_base != NULL);

	/* data[1][511:256], guard[1][0] */
	iovs[5].iov_base = calloc(1, 256 + 1);
	iovs[5].iov_len = 256 + 1;
	SPDK_CU_ASSERT_FATAL(iovs[5].iov_base != NULL);

	/* guard[1][1], apptag[1][0] */
	iovs[6].iov_base = calloc(1, 1 + 1);
	iovs[6].iov_len = 1 + 1;
	SPDK_CU_ASSERT_FATAL(iovs[6].iov_base != NULL);

	/* apptag[1][1], reftag[1][0] */
	iovs[7].iov_base = calloc(1, 1 + 1);
	iovs[7].iov_len = 1 + 1;
	SPDK_CU_ASSERT_FATAL(iovs[7].iov_base != NULL);

	/* reftag[1][3:1] */
	iovs[8].iov_base = calloc(1, 3);
	iovs[8].iov_len = 3;
	SPDK_CU_ASSERT_FATAL(iovs[8].iov_base != NULL);

	_data_pattern_generate(iovs, 9, 512, 8);

	rc = spdk_t10dif_generate(iovs, 9, 512, 8, dif_flags, 22, 0x22);
	CU_ASSERT(rc == 0);

	rc = spdk_t10dif_verify(iovs, 9, 512, 8, dif_flags, 22, 0, 0x22);
	CU_ASSERT(rc == 0);

	rc = _data_pattern_verify(iovs, 9, 512, 8);
	CU_ASSERT(rc == 0);

	for (i = 0; i < 9; i++) {
		free(iovs[i].iov_base);
	}
}

static void
sec_4096_md_128_prchk_7_multi_iovs_complex_splits(void)
{
	struct iovec iovs[11];
	uint32_t dif_flags;
	int i, rc;

	dif_flags = SPDK_T10DIF_GUARD_CHECK | SPDK_T10DIF_APPTAG_CHECK | SPDK_T10DIF_REFTAG_CHECK;

	/* data[0][1000:0] */
	iovs[0].iov_base = calloc(1, 1000);
	iovs[0].iov_len = 1000;
	SPDK_CU_ASSERT_FATAL(iovs[0].iov_base != NULL);

	/* data[0][3095:1000], guard[0][0] */
	iovs[1].iov_base = calloc(1, 3096 + 1);
	iovs[1].iov_len = 3096 + 1;
	SPDK_CU_ASSERT_FATAL(iovs[1].iov_base != NULL);

	/* guard[0][1], apptag[0][0] */
	iovs[2].iov_base = calloc(1, 1 + 1);
	iovs[2].iov_len = 1 + 1;
	SPDK_CU_ASSERT_FATAL(iovs[2].iov_base != NULL);

	/* apptag[0][1], reftag[0][0] */
	iovs[3].iov_base = calloc(1, 1 + 1);
	iovs[3].iov_len = 1 + 1;
	SPDK_CU_ASSERT_FATAL(iovs[3].iov_base != NULL);

	/* reftag[0][3:1], ignore[0][59:0] */
	iovs[4].iov_base = calloc(1, 3 + 60);
	iovs[4].iov_len = 3 + 60;
	SPDK_CU_ASSERT_FATAL(iovs[4].iov_base != NULL);

	/* ignore[119:60], data[1][3050:0] */
	iovs[5].iov_base = calloc(1, 60 + 3051);
	iovs[5].iov_len = 60 + 3051;
	SPDK_CU_ASSERT_FATAL(iovs[5].iov_base != NULL);

	/* data[1][4095:3050], guard[1][0] */
	iovs[6].iov_base = calloc(1, 1045 + 1);
	iovs[6].iov_len = 1045 + 1;
	SPDK_CU_ASSERT_FATAL(iovs[6].iov_base != NULL);

	/* guard[1][1], apptag[1][0] */
	iovs[7].iov_base = calloc(1, 1 + 1);
	iovs[7].iov_len = 1 + 1;
	SPDK_CU_ASSERT_FATAL(iovs[7].iov_base != NULL);

	/* apptag[1][1], reftag[1][0] */
	iovs[8].iov_base = calloc(1, 1 + 1);
	iovs[8].iov_len = 1 + 1;
	SPDK_CU_ASSERT_FATAL(iovs[8].iov_base != NULL);

	/* reftag[1][3:1], ignore[1][9:0] */
	iovs[9].iov_base = calloc(1, 3 + 10);
	iovs[9].iov_len = 3 + 10;
	SPDK_CU_ASSERT_FATAL(iovs[9].iov_base != NULL);

	/* ignore[1][127:9] */
	iovs[10].iov_base = calloc(1, 118);
	iovs[10].iov_len = 118;
	SPDK_CU_ASSERT_FATAL(iovs[10].iov_base != NULL);

	_data_pattern_generate(iovs, 11, 4096, 128);

	rc = spdk_t10dif_generate(iovs, 11, 4096, 128, dif_flags, 22, 0x22);
	CU_ASSERT(rc == 0);

	rc = spdk_t10dif_verify(iovs, 11, 4096, 128, dif_flags, 22, 0, 0x22);
	CU_ASSERT(rc == 0);

	rc = _data_pattern_verify(iovs, 11, 4096, 128);
	CU_ASSERT(rc == 0);

	for (i = 0; i < 11; i++) {
		free(iovs[i].iov_base);
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
		CU_add_test(suite, "t10dif_generate_and_verify", t10dif_generate_and_verify) == NULL ||
		CU_add_test(suite, "sec_512_md_0_error", sec_512_md_0_error) == NULL ||
		CU_add_test(suite, "sec_512_md_8_prchk_0_single_iov", sec_512_md_8_prchk_0_single_iov) == NULL ||
		CU_add_test(suite, "sec_512_md_8_prchk_0_multi_iovs", sec_512_md_8_prchk_0_multi_iovs) == NULL ||
		CU_add_test(suite, "sec_512_md_8_prchk_1_multi_iovs", sec_512_md_8_prchk_1_multi_iovs) == NULL ||
		CU_add_test(suite, "sec_512_md_8_prchk_2_multi_iovs", sec_512_md_8_prchk_2_multi_iovs) == NULL ||
		CU_add_test(suite, "sec_512_md_8_prchk_4_multi_iovs", sec_512_md_8_prchk_4_multi_iovs) == NULL ||
		CU_add_test(suite, "sec_4096_md_128_prchk_7_multi_iovs",
			    sec_4096_md_128_prchk_7_multi_iovs) == NULL ||
		CU_add_test(suite, "sec_512_md_8_prchk_7_multi_iovs_split_data_and_md",
			    sec_512_md_8_prchk_7_multi_iovs_split_data_and_md) == NULL ||
		CU_add_test(suite, "sec_512_md_8_prchk_7_multi_iovs_split_data",
			    sec_512_md_8_prchk_7_multi_iovs_split_data) == NULL ||
		CU_add_test(suite, "sec_512_md_8_prchk_7_multi_iovs_split_guard",
			    sec_512_md_8_prchk_7_multi_iovs_split_guard) == NULL ||
		CU_add_test(suite, "sec_512_md_8_prchk_7_multi_iovs_split_apptag",
			    sec_512_md_8_prchk_7_multi_iovs_split_apptag) == NULL ||
		CU_add_test(suite, "sec_512_md_8_prchk_7_multi_iovs_split_reftag",
			    sec_512_md_8_prchk_7_multi_iovs_split_reftag) == NULL ||
		CU_add_test(suite, "sec_512_md_8_prchk_7_multi_iovs_complex_splits",
			    sec_512_md_8_prchk_7_multi_iovs_complex_splits) == NULL ||
		CU_add_test(suite, "sec_4096_md_128_prchk_7_multi_iovs_complex_splits",
			    sec_4096_md_128_prchk_7_multi_iovs_complex_splits) == NULL
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
