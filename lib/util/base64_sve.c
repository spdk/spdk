/*-
 *   BSD LICENSE
 *
 *   Copyright(c) ARM Limited. 2021 All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions are
 *   met:
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
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 *   IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 *   TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 *   PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 *   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __aarch64__
#error Unsupported hardware
#endif

#include "spdk/stdinc.h"
#include <arm_sve.h>

static int
table_lookup_8vec(svuint8_t tbl_vec0, svuint8_t tbl_vec1, svuint8_t tbl_vec2, svuint8_t tbl_vec3,
		  svuint8_t tbl_vec4, svuint8_t tbl_vec5, svuint8_t tbl_vec6, svuint8_t tbl_vec7,
		  svuint8_t indices, svuint8_t *output, svbool_t p8_in, uint64_t vl)
{
	svuint8_t res2, res3, res4, res5, res6, res7;

	/*
	 * In base64 decode table, the first 32 elements are invalid value,
	 * so skip tbl_vec0 and tbl_vec1
	 */
	indices = svsub_n_u8_z(p8_in, indices, 2 * vl);
	res2 = svtbl_u8(tbl_vec2, indices);
	indices = svsub_n_u8_z(p8_in, indices, vl);
	res3 = svtbl_u8(tbl_vec3, indices);
	indices = svsub_n_u8_z(p8_in, indices, vl);
	res4 = svtbl_u8(tbl_vec4, indices);
	indices = svsub_n_u8_z(p8_in, indices, vl);
	res5 = svtbl_u8(tbl_vec5, indices);
	indices = svsub_n_u8_z(p8_in, indices, vl);
	res6 = svtbl_u8(tbl_vec6, indices);
	indices = svsub_n_u8_z(p8_in, indices, vl);
	res7 = svtbl_u8(tbl_vec7, indices);

	*output = svdup_n_u8(0);
	*output = svadd_u8_z(p8_in, res2, *output);
	*output = svadd_u8_z(p8_in, res3, *output);
	*output = svadd_u8_z(p8_in, res4, *output);
	*output = svadd_u8_z(p8_in, res5, *output);
	*output = svadd_u8_z(p8_in, res6, *output);
	*output = svadd_u8_z(p8_in, res7, *output);

	if (svcntp_b8(p8_in, svcmpeq_n_u8(p8_in, *output, 255))) {
		return -1;
	}

	return 0;
}

static int
table_lookup_4vec(svuint8_t tbl_vec0, svuint8_t tbl_vec1, svuint8_t tbl_vec2, svuint8_t tbl_vec3,
		  svuint8_t indices, svuint8_t *output, svbool_t p8_in, uint64_t vl)
{
	svuint8_t res0, res1, res2, res3;

	res0 = svtbl_u8(tbl_vec0, indices);
	indices = svsub_n_u8_z(p8_in, indices, vl);
	res1 = svtbl_u8(tbl_vec1, indices);
	indices = svsub_n_u8_z(p8_in, indices, vl);
	res2 = svtbl_u8(tbl_vec2, indices);
	indices = svsub_n_u8_z(p8_in, indices, vl);
	res3 = svtbl_u8(tbl_vec3, indices);

	*output = svdup_n_u8(0);

	*output = svadd_u8_z(p8_in, res0, *output);
	*output = svadd_u8_z(p8_in, res1, *output);
	*output = svadd_u8_z(p8_in, res2, *output);
	*output = svadd_u8_z(p8_in, res3, *output);

	if (svcntp_b8(p8_in, svcmpeq_n_u8(p8_in, *output, 255))) {
		return -1;
	}

	return 0;
}

static int
table_lookup_3vec(svuint8_t tbl_vec0, svuint8_t tbl_vec1, svuint8_t tbl_vec2, svuint8_t indices,
		  svuint8_t *output, svbool_t p8_in, uint64_t vl)
{
	svuint8_t res0, res1, res2;

	res0 = svtbl_u8(tbl_vec0, indices);
	indices = svsub_n_u8_z(p8_in, indices, vl);
	res1 = svtbl_u8(tbl_vec1, indices);
	indices = svsub_n_u8_z(p8_in, indices, vl);
	res2 = svtbl_u8(tbl_vec2, indices);

	*output = svdup_n_u8(0);

	*output = svadd_u8_z(p8_in, res0, *output);
	*output = svadd_u8_z(p8_in, res1, *output);
	*output = svadd_u8_z(p8_in, res2, *output);

	if (svcntp_b8(p8_in, svcmpeq_n_u8(p8_in, *output, 255))) {
		return -1;
	}

	return 0;
}

static int
table_lookup_2vec(svuint8_t tbl_vec0, svuint8_t tbl_vec1, svuint8_t indices, svuint8_t *output,
		  svbool_t p8_in, uint64_t vl)
{
	svuint8_t res0, res1;

	res0 = svtbl_u8(tbl_vec0, indices);
	indices = svsub_n_u8_z(p8_in, indices, vl);
	res1 = svtbl_u8(tbl_vec1, indices);

	*output = svdup_n_u8(0);

	*output = svadd_u8_z(p8_in, res0, *output);
	*output = svadd_u8_z(p8_in, res1, *output);

	if (svcntp_b8(p8_in, svcmpeq_n_u8(p8_in, *output, 255))) {
		return -1;
	}

	return 0;
}

static inline void
convert_6bits_to_8bits(svbool_t pred, uint8_t *src, svuint8_t *temp0, svuint8_t *temp1,
		       svuint8_t *temp2, svuint8_t *temp3)
{
	svuint8_t str0, str1, str2;
	svuint8x3_t ld_enc_input;

	ld_enc_input = svld3_u8(pred, src);

	str0 = svget3_u8(ld_enc_input, 0);
	str1 = svget3_u8(ld_enc_input, 1);
	str2 = svget3_u8(ld_enc_input, 2);


	*temp0 = svlsr_n_u8_z(pred, str0, 2);
	*temp1 = svand_u8_z(pred, svorr_u8_z(pred, svlsr_n_u8_z(pred, str1, 4), svlsl_n_u8_z(pred, str0,
					     4)),
			    svdup_u8(0x3F));
	*temp2 = svand_u8_z(pred, svorr_u8_z(pred, svlsr_n_u8_z(pred, str2, 6), svlsl_n_u8_z(pred, str1,
					     2)),
			    svdup_u8(0x3F));
	*temp3 = svand_u8_z(pred, str2, svdup_u8(0x3F));
}

static inline void
convert_8bits_to_6bits(svbool_t pred, svuint8_t temp0, svuint8_t temp1, svuint8_t temp2,
		       svuint8_t temp3, svuint8_t *output0, svuint8_t *output1, svuint8_t *output2)
{
	*output0 = svorr_u8_z(pred, svlsl_n_u8_z(pred, temp0, 2), svlsr_n_u8_z(pred, temp1, 4));
	*output1 = svorr_u8_z(pred, svlsl_n_u8_z(pred, temp1, 4), svlsr_n_u8_z(pred, temp2, 2));
	*output2 = svorr_u8_z(pred, svlsl_n_u8_z(pred, temp2, 6), temp3);
}

static void
base64_encode_sve(char **dst, const char *enc_table, const void **src, size_t *src_len)
{
	uint64_t vl = svcntb();
	svuint8_t temp0, temp1, temp2, temp3;
	svuint8_t output0, output1, output2, output3;
	svuint8_t tbl_enc0, tbl_enc1, tbl_enc2, tbl_enc3;
	svuint8x4_t st_enc_output;
	svbool_t p8_all = svptrue_b8();
	svbool_t pred;
	uint64_t i = 0;
	uint64_t pred_count = 0;
	uint64_t N = (*src_len / 3) * 3;

	if (vl == 16) {

		tbl_enc0 = svld1_u8(p8_all, (uint8_t *)enc_table + 0);
		tbl_enc1 = svld1_u8(p8_all, (uint8_t *)enc_table + 16);
		tbl_enc2 = svld1_u8(p8_all, (uint8_t *)enc_table + 32);
		tbl_enc3 = svld1_u8(p8_all, (uint8_t *)enc_table + 48);

		while (i < N) {
			pred = svwhilelt_b8(i / 3, N / 3);

			convert_6bits_to_8bits(pred, (uint8_t *)*src, &temp0, &temp1, &temp2, &temp3);

			table_lookup_4vec(tbl_enc0, tbl_enc1, tbl_enc2, tbl_enc3, temp0, &output0, pred, vl);
			table_lookup_4vec(tbl_enc0, tbl_enc1, tbl_enc2, tbl_enc3, temp1, &output1, pred, vl);
			table_lookup_4vec(tbl_enc0, tbl_enc1, tbl_enc2, tbl_enc3, temp2, &output2, pred, vl);
			table_lookup_4vec(tbl_enc0, tbl_enc1, tbl_enc2, tbl_enc3, temp3, &output3, pred, vl);

			st_enc_output = svcreate4_u8(output0, output1, output2, output3);
			svst4_u8(pred, (uint8_t *)*dst, st_enc_output);

			pred_count = svcntp_b8(pred, pred);
			*src += pred_count * 3;
			*dst += pred_count * 4;
			*src_len -= pred_count * 3;
			i += pred_count * 3;

		}
	} else if (vl == 32 || vl == 48) {

		tbl_enc0 = svld1_u8(p8_all, (uint8_t *)enc_table + 0);
		pred = svwhilelt_b8(vl, (uint64_t)64);
		tbl_enc1 = svld1_u8(pred, (uint8_t *)enc_table + vl);

		while (i < N) {
			pred = svwhilelt_b8(i / 3, N / 3);

			convert_6bits_to_8bits(pred, (uint8_t *)*src, &temp0, &temp1, &temp2, &temp3);

			table_lookup_2vec(tbl_enc0, tbl_enc1, temp0, &output0, pred, vl);
			table_lookup_2vec(tbl_enc0, tbl_enc1, temp1, &output1, pred, vl);
			table_lookup_2vec(tbl_enc0, tbl_enc1, temp2, &output2, pred, vl);
			table_lookup_2vec(tbl_enc0, tbl_enc1, temp3, &output3, pred, vl);

			st_enc_output = svcreate4_u8(output0, output1, output2, output3);
			svst4_u8(pred, (uint8_t *)*dst, st_enc_output);

			pred_count = svcntp_b8(pred, pred);
			*src += pred_count * 3;
			*dst += pred_count * 4;
			*src_len -= pred_count * 3;
			i += pred_count * 3;

		}
	} else if (vl >= 64) {

		pred = svwhilelt_b8((uint64_t)0, (uint64_t)64);
		tbl_enc0 = svld1_u8(pred, (uint8_t *)enc_table);

		while (i < N) {
			pred = svwhilelt_b8(i / 3, N / 3);

			convert_6bits_to_8bits(pred, (uint8_t *)*src, &temp0, &temp1, &temp2, &temp3);

			output0 = svtbl_u8(tbl_enc0, temp0);
			output1 = svtbl_u8(tbl_enc0, temp1);
			output2 = svtbl_u8(tbl_enc0, temp2);
			output3 = svtbl_u8(tbl_enc0, temp3);

			st_enc_output = svcreate4_u8(output0, output1, output2, output3);
			svst4_u8(pred, (uint8_t *)*dst, st_enc_output);

			pred_count = svcntp_b8(pred, pred);
			*src += pred_count * 3;
			*dst += pred_count * 4;
			*src_len -= pred_count * 3;
			i += pred_count * 3;

		}
	}
}

static void
base64_decode_sve(void **dst, const uint8_t *dec_table, const uint8_t **src, size_t *src_len)
{
	uint64_t vl = svcntb();
	svuint8_t str0, str1, str2, str3;
	svuint8_t temp0, temp1, temp2, temp3;
	svuint8_t output0, output1, output2;
	svuint8_t tbl_dec0, tbl_dec1, tbl_dec2, tbl_dec3, tbl_dec4, tbl_dec5, tbl_dec6, tbl_dec7;
	svuint8x3_t st_dec_output;
	svbool_t p8_all = svptrue_b8();
	svbool_t pred;
	uint64_t i = 0;
	uint64_t pred_count = 0;
	uint64_t N = (*src_len / 4) * 4;
	svuint8x4_t ld_dec_input;

	if (vl == 16) {
		tbl_dec0 = svld1_u8(p8_all, (uint8_t *)dec_table + 0);
		tbl_dec1 = svld1_u8(p8_all, (uint8_t *)dec_table + 16);
		tbl_dec2 = svld1_u8(p8_all, (uint8_t *)dec_table + 32);
		tbl_dec3 = svld1_u8(p8_all, (uint8_t *)dec_table + 48);
		tbl_dec4 = svld1_u8(p8_all, (uint8_t *)dec_table + 64);
		tbl_dec5 = svld1_u8(p8_all, (uint8_t *)dec_table + 80);
		tbl_dec6 = svld1_u8(p8_all, (uint8_t *)dec_table + 96);
		tbl_dec7 = svld1_u8(p8_all, (uint8_t *)dec_table + 112);

		while (i < N) {
			pred = svwhilelt_b8(i / 4, N / 4);

			ld_dec_input = svld4_u8(pred, *src);

			str0 = svget4_u8(ld_dec_input, 0);
			str1 = svget4_u8(ld_dec_input, 1);
			str2 = svget4_u8(ld_dec_input, 2);
			str3 = svget4_u8(ld_dec_input, 3);

			if (svcntp_b8(pred, svcmpge_n_u8(pred, str0, 128))) { return; }
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str1, 128))) { return; }
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str2, 128))) { return; }
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str3, 128))) { return; }

			if (table_lookup_8vec(tbl_dec0, tbl_dec1, tbl_dec2, tbl_dec3, tbl_dec4, tbl_dec5, tbl_dec6,
					      tbl_dec7, str0, &temp0, pred, vl)) { return; }
			if (table_lookup_8vec(tbl_dec0, tbl_dec1, tbl_dec2, tbl_dec3, tbl_dec4, tbl_dec5, tbl_dec6,
					      tbl_dec7, str1, &temp1, pred, vl)) { return; }
			if (table_lookup_8vec(tbl_dec0, tbl_dec1, tbl_dec2, tbl_dec3, tbl_dec4, tbl_dec5, tbl_dec6,
					      tbl_dec7, str2, &temp2, pred, vl)) { return; }
			if (table_lookup_8vec(tbl_dec0, tbl_dec1, tbl_dec2, tbl_dec3, tbl_dec4, tbl_dec5, tbl_dec6,
					      tbl_dec7, str3, &temp3, pred, vl)) { return; }

			convert_8bits_to_6bits(pred, temp0, temp1, temp2, temp3, &output0, &output1, &output2);

			st_dec_output = svcreate3_u8(output0, output1, output2);
			svst3_u8(pred, (uint8_t *)*dst, st_dec_output);

			pred_count = svcntp_b8(pred, pred);
			*src += pred_count * 4;
			*dst += pred_count * 3;
			*src_len -= pred_count * 4;
			i += pred_count * 4;

		}
	} else if (vl == 32) {
		tbl_dec0 = svld1_u8(p8_all, (uint8_t *)dec_table + 0);
		tbl_dec1 = svld1_u8(p8_all, (uint8_t *)dec_table + vl);
		tbl_dec2 = svld1_u8(p8_all, (uint8_t *)dec_table + vl * 2);
		tbl_dec3 = svld1_u8(p8_all, (uint8_t *)dec_table + vl * 3);

		while (i < N) {
			pred = svwhilelt_b8(i / 4, N / 4);

			ld_dec_input = svld4_u8(pred, *src);

			str0 = svget4_u8(ld_dec_input, 0);
			str1 = svget4_u8(ld_dec_input, 1);
			str2 = svget4_u8(ld_dec_input, 2);
			str3 = svget4_u8(ld_dec_input, 3);

			if (svcntp_b8(pred, svcmpge_n_u8(pred, str0, 128))) { return; }
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str1, 128))) { return; }
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str2, 128))) { return; }
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str3, 128))) { return; }

			if (table_lookup_4vec(tbl_dec0, tbl_dec1, tbl_dec2, tbl_dec3, str0, &temp0, pred, vl)) { return; }
			if (table_lookup_4vec(tbl_dec0, tbl_dec1, tbl_dec2, tbl_dec3, str1, &temp1, pred, vl)) { return; }
			if (table_lookup_4vec(tbl_dec0, tbl_dec1, tbl_dec2, tbl_dec3, str2, &temp2, pred, vl)) { return; }
			if (table_lookup_4vec(tbl_dec0, tbl_dec1, tbl_dec2, tbl_dec3, str3, &temp3, pred, vl)) { return; }

			convert_8bits_to_6bits(pred, temp0, temp1, temp2, temp3, &output0, &output1, &output2);

			st_dec_output = svcreate3_u8(output0, output1, output2);
			svst3_u8(pred, (uint8_t *)*dst, st_dec_output);

			pred_count = svcntp_b8(pred, pred);
			*src += pred_count * 4;
			*dst += pred_count * 3;
			*src_len -= pred_count * 4;
			i += pred_count * 4;

		}

	} else if (vl == 48) {
		tbl_dec0 = svld1_u8(p8_all, (uint8_t *)dec_table + 0);
		tbl_dec1 = svld1_u8(p8_all, (uint8_t *)dec_table + vl);
		pred = svwhilelt_b8(vl * 2, (uint64_t)128);
		tbl_dec2 = svld1_u8(pred, (uint8_t *)dec_table + 2 * vl);

		while (i < N) {
			pred = svwhilelt_b8(i / 4, N / 4);

			ld_dec_input = svld4_u8(pred, *src);

			str0 = svget4_u8(ld_dec_input, 0);
			str1 = svget4_u8(ld_dec_input, 1);
			str2 = svget4_u8(ld_dec_input, 2);
			str3 = svget4_u8(ld_dec_input, 3);

			if (svcntp_b8(pred, svcmpge_n_u8(pred, str0, 128))) { return; }
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str1, 128))) { return; }
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str2, 128))) { return; }
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str3, 128))) { return; }

			if (table_lookup_3vec(tbl_dec0, tbl_dec1, tbl_dec2, str0, &temp0, pred, vl)) { return; }
			if (table_lookup_3vec(tbl_dec0, tbl_dec1, tbl_dec2, str1, &temp1, pred, vl)) { return; }
			if (table_lookup_3vec(tbl_dec0, tbl_dec1, tbl_dec2, str2, &temp2, pred, vl)) { return; }
			if (table_lookup_3vec(tbl_dec0, tbl_dec1, tbl_dec2, str3, &temp3, pred, vl)) { return; }

			convert_8bits_to_6bits(pred, temp0, temp1, temp2, temp3, &output0, &output1, &output2);

			st_dec_output = svcreate3_u8(output0, output1, output2);
			svst3_u8(pred, (uint8_t *)*dst, st_dec_output);

			pred_count = svcntp_b8(pred, pred);
			*src += pred_count * 4;
			*dst += pred_count * 3;
			*src_len -= pred_count * 4;
			i += pred_count * 4;

		}
	} else if (vl == 64 || vl == 80 || vl == 96 || vl == 112) {
		tbl_dec0 = svld1_u8(p8_all, (uint8_t *)dec_table + 0);
		pred = svwhilelt_b8(vl, (uint64_t)128);
		tbl_dec1 = svld1_u8(pred, (uint8_t *)dec_table + vl);

		while (i < N) {
			pred = svwhilelt_b8(i / 4, N / 4);

			ld_dec_input = svld4_u8(pred, *src);

			str0 = svget4_u8(ld_dec_input, 0);
			str1 = svget4_u8(ld_dec_input, 1);
			str2 = svget4_u8(ld_dec_input, 2);
			str3 = svget4_u8(ld_dec_input, 3);

			if (svcntp_b8(pred, svcmpge_n_u8(pred, str0, 128))) { return; }
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str1, 128))) { return; }
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str2, 128))) { return; }
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str3, 128))) { return; }

			if (table_lookup_2vec(tbl_dec0, tbl_dec1, str0, &temp0, pred, vl)) { return; }
			if (table_lookup_2vec(tbl_dec0, tbl_dec1, str1, &temp1, pred, vl)) { return; }
			if (table_lookup_2vec(tbl_dec0, tbl_dec1, str2, &temp2, pred, vl)) { return; }
			if (table_lookup_2vec(tbl_dec0, tbl_dec1, str3, &temp3, pred, vl)) { return; }

			convert_8bits_to_6bits(pred, temp0, temp1, temp2, temp3, &output0, &output1, &output2);

			st_dec_output = svcreate3_u8(output0, output1, output2);
			svst3_u8(pred, (uint8_t *)*dst, st_dec_output);

			pred_count = svcntp_b8(pred, pred);
			*src += pred_count * 4;
			*dst += pred_count * 3;
			*src_len -= pred_count * 4;
			i += pred_count * 4;

		}
	} else if (vl >= 128) {
		pred = svwhilelt_b8((uint64_t)0, (uint64_t)128);
		tbl_dec0 = svld1_u8(pred, (uint8_t *)dec_table + 0);

		while (i < N) {
			pred = svwhilelt_b8(i / 4, N / 4);

			ld_dec_input = svld4_u8(pred, *src);

			str0 = svget4_u8(ld_dec_input, 0);
			str1 = svget4_u8(ld_dec_input, 1);
			str2 = svget4_u8(ld_dec_input, 2);
			str3 = svget4_u8(ld_dec_input, 3);

			if (svcntp_b8(pred, svcmpge_n_u8(pred, str0, 128))) { return; }
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str1, 128))) { return; }
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str2, 128))) { return; }
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str3, 128))) { return; }

			temp0 = svtbl_u8(tbl_dec0, str0);
			temp1 = svtbl_u8(tbl_dec0, str1);
			temp2 = svtbl_u8(tbl_dec0, str2);
			temp3 = svtbl_u8(tbl_dec0, str3);

			if (svcntp_b8(pred, svcmpeq_n_u8(pred, temp0, 255))) { return; }
			if (svcntp_b8(pred, svcmpeq_n_u8(pred, temp1, 255))) { return; }
			if (svcntp_b8(pred, svcmpeq_n_u8(pred, temp2, 255))) { return; }
			if (svcntp_b8(pred, svcmpeq_n_u8(pred, temp3, 255))) { return; }

			convert_8bits_to_6bits(pred, temp0, temp1, temp2, temp3, &output0, &output1, &output2);

			st_dec_output = svcreate3_u8(output0, output1, output2);
			svst3_u8(pred, (uint8_t *)*dst, st_dec_output);

			pred_count = svcntp_b8(pred, pred);
			*src += pred_count * 4;
			*dst += pred_count * 3;
			*src_len -= pred_count * 4;
			i += pred_count * 4;

		}
	}
}
