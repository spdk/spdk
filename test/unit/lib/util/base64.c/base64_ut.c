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

#include "util/base64.c"

char text_A[] = "FZB3";
uint8_t raw_A[] = {0x15, 0x90, 0x77};
char text_B[] = "AbC/1+c=";
char text_urlsafe_B[] = "AbC_1-c=";
uint8_t raw_B[] = {0x01, 0xB0, 0xBF, 0xD7, 0xE7};
char text_C[] = "AbC/1+cC";
char text_urlsafe_C[] = "AbC_1-cC";
uint8_t raw_C[] = {0x01, 0xB0, 0xBF, 0xD7, 0xE7, 0x02};
char text_D[] = "AbC/1w==";
char text_urlsafe_D[] = "AbC_1w==";
uint8_t raw_D[] = {0x01, 0xB0, 0xBF, 0xD7};
char text_E[] = "AbC12===";
char text_F[] = "AbCd112";
char text_G[] = "AbCd12";
char text_H[] = "AbC12";
char text_I[] = "AQu/1+cCCBUnOBFWv+HzoL3BOVUBItP2mRDdqhnxAtIT4hD1wbQ30Ylm8R+7khPS";//64 bytes
char text_urlsafe_I[] =
	"AQu_1-cCCBUnOBFWv-HzoL3BOVUBItP2mRDdqhnxAtIT4hD1wbQ30Ylm8R-7khPS";//64 bytes
uint8_t raw_I[] = {0x01, 0x0B, 0xBF, 0xD7, 0xE7, 0x02, 0x08, 0x15, 0x27, 0x38, 0x11, 0x56, 0xBF, 0xE1, 0xF3, 0xA0,
		   0xBD, 0xC1, 0x39, 0x55, 0x01, 0x22, 0xD3, 0xF6, 0x99, 0x10, 0xDD, 0xAA, 0x19, 0xF1, 0x02, 0xD2,
		   0x13, 0xE2, 0x10, 0xF5, 0xC1, 0xB4, 0x37, 0xD1, 0x89, 0x66, 0xF1, 0x1F, 0xBB, 0x92, 0x13, 0xD2
		  };
char text_J[] =
	"AQu/1+cCCBUnOBFWv+HzoL3BOVUBItP2mRDdqhnxAtIT4hD1wbQ30Ylm8R+7khPSvcE5VecCCBUZ8QLiEPVm8b3BOVUBItP2GfEC4hD1ZvE5VQEi0/aJZvEfu5LiEPUTvcE5VQEi0/YZEQ==";
char text_urlsafe_J[] =
	"AQu_1-cCCBUnOBFWv-HzoL3BOVUBItP2mRDdqhnxAtIT4hD1wbQ30Ylm8R-7khPSvcE5VecCCBUZ8QLiEPVm8b3BOVUBItP2GfEC4hD1ZvE5VQEi0_aJZvEfu5LiEPUTvcE5VQEi0_YZEQ==";
uint8_t raw_J[] = {0x01, 0x0B, 0xBF, 0xD7, 0xE7, 0x02, 0x08, 0x15, 0x27, 0x38, 0x11, 0x56, 0xBF, 0xE1, 0xF3, 0xA0,
		   0xBD, 0xC1, 0x39, 0x55, 0x01, 0x22, 0xD3, 0xF6, 0x99, 0x10, 0xDD, 0xAA, 0x19, 0xF1, 0x02, 0xD2,
		   0x13, 0xE2, 0x10, 0xF5, 0xC1, 0xB4, 0x37, 0xD1, 0x89, 0x66, 0xF1, 0x1F, 0xBB, 0x92, 0x13, 0xD2,
		   0xBD, 0xC1, 0x39, 0x55, 0xE7, 0x02, 0x08, 0x15, 0x19, 0xF1, 0x02, 0xE2, 0x10, 0xF5, 0x66, 0xF1,
		   0xBD, 0xC1, 0x39, 0x55, 0x01, 0x22, 0xD3, 0xF6, 0x19, 0xF1, 0x02, 0xE2, 0x10, 0xF5, 0x66, 0xF1,
		   0x39, 0x55, 0x01, 0x22, 0xD3, 0xF6, 0x89, 0x66, 0xF1, 0x1F, 0xBB, 0x92, 0xE2, 0x10, 0xF5, 0x13,
		   0xBD, 0xC1, 0x39, 0x55, 0x01, 0x22, 0xD3, 0xF6, 0x19, 0x11
		  };

static void
test_base64_get_encoded_strlen(void)
{
	uint32_t raw_lens[4] = {8, 9, 10, 11};
	uint32_t text_strlens[4] = {12, 12, 16, 16};
	uint32_t text_strlen;
	int i;

	for (i = 0; i < 4; i++) {
		text_strlen = spdk_base64_get_encoded_strlen(raw_lens[i]);
		CU_ASSERT_EQUAL(text_strlen, text_strlens[i]);
	}
}

static void
test_base64_get_decoded_len(void)
{
	uint32_t text_strlens[4] = {8, 10, 11, 12};
	uint32_t raw_lens[4] = {6, 7, 8, 9};
	uint32_t bin_len;
	int i;

	for (i = 0; i < 4; i++) {
		bin_len = spdk_base64_get_decoded_len(text_strlens[i]);
		CU_ASSERT_EQUAL(bin_len, raw_lens[i]);
	}
}

static void
test_base64_encode(void)
{
	char text[200];
	int ret;

	ret = spdk_base64_encode(text, raw_A, sizeof(raw_A));
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT(strcmp(text, text_A) == 0);
	CU_ASSERT_EQUAL(strlen(text), strlen(text_A));

	ret = spdk_base64_encode(text, raw_B, sizeof(raw_B));
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT(strcmp(text, text_B) == 0);
	CU_ASSERT_EQUAL(strlen(text), strlen(text_B));

	ret = spdk_base64_encode(text, raw_C, sizeof(raw_C));
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT(strcmp(text, text_C) == 0);

	ret = spdk_base64_encode(text, raw_D, sizeof(raw_D));
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT(strcmp(text, text_D) == 0);

	ret = spdk_base64_encode(text, raw_I, sizeof(raw_I));
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT(strcmp(text, text_I) == 0);

	ret = spdk_base64_encode(text, raw_J, sizeof(raw_J));
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT(strcmp(text, text_J) == 0);

	ret = spdk_base64_encode(NULL, raw_A, sizeof(raw_A));
	CU_ASSERT_EQUAL(ret, -EINVAL);
	ret = spdk_base64_encode(text, NULL, sizeof(raw_A));
	CU_ASSERT_EQUAL(ret, -EINVAL);
	ret = spdk_base64_encode(text, raw_A, 0);
	CU_ASSERT_EQUAL(ret, -EINVAL);
}

static void
test_base64_decode(void)
{
	char raw_buf[200];
	void *raw = (void *)raw_buf;
	size_t raw_len;
	int ret;

	/* len */
	ret = spdk_base64_decode(NULL, &raw_len, text_A);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_A));

	/* decode */
	ret = spdk_base64_decode(raw, &raw_len, text_A);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_A));
	CU_ASSERT(memcmp(raw, raw_A, sizeof(raw_A)) == 0);

	/* len */
	ret = spdk_base64_decode(NULL, &raw_len, text_B);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_B));

	/* decode */
	ret = spdk_base64_decode(raw, &raw_len, text_B);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_B));
	CU_ASSERT(memcmp(raw, raw_B, sizeof(raw_B)) == 0);

	/* len */
	ret = spdk_base64_decode(NULL, &raw_len, text_C);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_C));

	/* decode */
	ret = spdk_base64_decode(raw, &raw_len, text_C);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_C));
	CU_ASSERT(memcmp(raw, raw_C, sizeof(raw_C)) == 0);

	/* len */
	ret = spdk_base64_decode(NULL, &raw_len, text_D);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_D));

	/* decode */
	ret = spdk_base64_decode(raw, &raw_len, text_D);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_D));
	CU_ASSERT(memcmp(raw, raw_D, sizeof(raw_D)) == 0);

	/* len */
	ret = spdk_base64_decode(NULL, &raw_len, text_I);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_I));

	/* decode */
	ret = spdk_base64_decode(raw, &raw_len, text_I);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_I));
	CU_ASSERT(memcmp(raw, raw_I, sizeof(raw_I)) == 0);

	/* len */
	ret = spdk_base64_decode(NULL, &raw_len, text_J);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_J));

	/* decode */
	ret = spdk_base64_decode(raw, &raw_len, text_J);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_J));
	CU_ASSERT(memcmp(raw, raw_J, sizeof(raw_J)) == 0);

	ret = spdk_base64_decode(raw, &raw_len, text_E);
	CU_ASSERT_EQUAL(ret, -EINVAL);
	ret = spdk_base64_decode(raw, &raw_len, text_F);
	CU_ASSERT_EQUAL(ret, -EINVAL);
	ret = spdk_base64_decode(raw, &raw_len, text_G);
	CU_ASSERT_EQUAL(ret, -EINVAL);
	ret = spdk_base64_decode(raw, &raw_len, text_H);
	CU_ASSERT_EQUAL(ret, -EINVAL);
	ret = spdk_base64_decode(raw, &raw_len, NULL);
	CU_ASSERT_EQUAL(ret, -EINVAL);
}

static void
test_base64_urlsafe_encode(void)
{
	char text[200];
	int ret;

	ret = spdk_base64_urlsafe_encode(text, raw_A, sizeof(raw_A));
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT(strcmp(text, text_A) == 0);
	CU_ASSERT_EQUAL(strlen(text), strlen(text_A));

	ret = spdk_base64_urlsafe_encode(text, raw_B, sizeof(raw_B));
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT(strcmp(text, text_urlsafe_B) == 0);
	CU_ASSERT_EQUAL(strlen(text), strlen(text_urlsafe_B));

	ret = spdk_base64_urlsafe_encode(text, raw_C, sizeof(raw_C));
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT(strcmp(text, text_urlsafe_C) == 0);

	ret = spdk_base64_urlsafe_encode(text, raw_D, sizeof(raw_D));
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT(strcmp(text, text_urlsafe_D) == 0);

	ret = spdk_base64_urlsafe_encode(text, raw_I, sizeof(raw_I));
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT(strcmp(text, text_urlsafe_I) == 0);

	ret = spdk_base64_urlsafe_encode(text, raw_J, sizeof(raw_J));
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT(strcmp(text, text_urlsafe_J) == 0);

	ret = spdk_base64_urlsafe_encode(NULL, raw_A, sizeof(raw_A));
	CU_ASSERT_EQUAL(ret, -EINVAL);
	ret = spdk_base64_urlsafe_encode(text, NULL, sizeof(raw_A));
	CU_ASSERT_EQUAL(ret, -EINVAL);
	ret = spdk_base64_urlsafe_encode(text, raw_A, 0);
	CU_ASSERT_EQUAL(ret, -EINVAL);
}

static void
test_base64_urlsafe_decode(void)
{
	char raw_buf[200];
	void *raw = (void *)raw_buf;
	size_t raw_len = 0;
	int ret;

	/* len */
	ret = spdk_base64_urlsafe_decode(NULL, &raw_len, text_A);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_A));

	/* decode */
	ret = spdk_base64_urlsafe_decode(raw, &raw_len, text_A);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_A));
	CU_ASSERT(memcmp(raw, raw_A, sizeof(raw_A)) == 0);

	/* len */
	ret = spdk_base64_urlsafe_decode(NULL, &raw_len, text_urlsafe_B);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_B));

	/* decode */
	ret = spdk_base64_urlsafe_decode(raw, &raw_len, text_urlsafe_B);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_B));
	CU_ASSERT(memcmp(raw, raw_B, sizeof(raw_B)) == 0);

	/* len */
	ret = spdk_base64_urlsafe_decode(NULL, &raw_len, text_urlsafe_C);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_C));

	/* decode */
	ret = spdk_base64_urlsafe_decode(raw, &raw_len, text_urlsafe_C);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_C));
	CU_ASSERT(memcmp(raw, raw_C, sizeof(raw_C)) == 0);

	/* len */
	ret = spdk_base64_urlsafe_decode(NULL, &raw_len, text_urlsafe_D);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_D));

	/* decode */
	ret = spdk_base64_urlsafe_decode(raw, &raw_len, text_urlsafe_D);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_D));
	CU_ASSERT(memcmp(raw, raw_D, sizeof(raw_D)) == 0);

	/* len */
	ret = spdk_base64_urlsafe_decode(NULL, &raw_len, text_urlsafe_I);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_I));

	/* decode */
	ret = spdk_base64_urlsafe_decode(raw, &raw_len, text_urlsafe_I);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_I));
	CU_ASSERT(memcmp(raw, raw_I, sizeof(raw_I)) == 0);

	/* len */
	ret = spdk_base64_urlsafe_decode(NULL, &raw_len, text_urlsafe_J);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_J));

	/* decode */
	ret = spdk_base64_urlsafe_decode(raw, &raw_len, text_urlsafe_J);
	CU_ASSERT_EQUAL(ret, 0);
	CU_ASSERT_EQUAL(raw_len, sizeof(raw_J));
	CU_ASSERT(memcmp(raw, raw_J, sizeof(raw_J)) == 0);

	ret = spdk_base64_urlsafe_decode(raw, &raw_len, text_E);
	CU_ASSERT_EQUAL(ret, -EINVAL);
	ret = spdk_base64_urlsafe_decode(raw, &raw_len, text_F);
	CU_ASSERT_EQUAL(ret, -EINVAL);
	ret = spdk_base64_urlsafe_decode(raw, &raw_len, text_G);
	CU_ASSERT_EQUAL(ret, -EINVAL);
	ret = spdk_base64_urlsafe_decode(raw, &raw_len, text_H);
	CU_ASSERT_EQUAL(ret, -EINVAL);
	ret = spdk_base64_urlsafe_decode(raw, &raw_len, NULL);
	CU_ASSERT_EQUAL(ret, -EINVAL);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("base64", NULL, NULL);

	CU_ADD_TEST(suite, test_base64_get_encoded_strlen);
	CU_ADD_TEST(suite, test_base64_get_decoded_len);
	CU_ADD_TEST(suite, test_base64_encode);
	CU_ADD_TEST(suite, test_base64_decode);
	CU_ADD_TEST(suite, test_base64_urlsafe_encode);
	CU_ADD_TEST(suite, test_base64_urlsafe_decode);

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
