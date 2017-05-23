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

#include "json_util.c"

#define NUM_SETUP(x) \
	snprintf(buf, sizeof(buf), "%s", x); \
	v.type = SPDK_JSON_VAL_NUMBER; \
	v.start = buf; \
	v.len = sizeof(x) - 1

#define NUM_INT32_PASS(s, i) \
	NUM_SETUP(s); \
	CU_ASSERT(spdk_json_number_to_int32(&v, &i32) == 0); \
	CU_ASSERT(i32 == i)

#define NUM_INT32_FAIL(s) \
	NUM_SETUP(s); \
	CU_ASSERT(spdk_json_number_to_int32(&v, &i32) != 0)

static void
test_strequal(void)
{
	struct spdk_json_val v;

	v.type = SPDK_JSON_VAL_STRING;
	v.start = "test";
	v.len = sizeof("test") - 1;
	CU_ASSERT(spdk_json_strequal(&v, "test") == true);
	CU_ASSERT(spdk_json_strequal(&v, "TEST") == false);
	CU_ASSERT(spdk_json_strequal(&v, "hello") == false);
	CU_ASSERT(spdk_json_strequal(&v, "t") == false);

	v.type = SPDK_JSON_VAL_NAME;
	CU_ASSERT(spdk_json_strequal(&v, "test") == true);

	v.type = SPDK_JSON_VAL_NUMBER;
	CU_ASSERT(spdk_json_strequal(&v, "test") == false);

	v.type = SPDK_JSON_VAL_STRING;
	v.start = "test\0hello";
	v.len = sizeof("test\0hello") - 1;
	CU_ASSERT(spdk_json_strequal(&v, "test") == false);
}

static void
test_num_to_int32(void)
{
	struct spdk_json_val v;
	char buf[100];
	int32_t i32 = 0;

	NUM_SETUP("1234");
	CU_ASSERT(spdk_json_number_to_int32(&v, &i32) == 0);
	CU_ASSERT(i32 == 1234);


	NUM_INT32_PASS("0", 0);
	NUM_INT32_PASS("1234", 1234);
	NUM_INT32_PASS("-1234", -1234);
	NUM_INT32_PASS("1234.00000", 1234);
	NUM_INT32_PASS("1.2e1", 12);
	NUM_INT32_PASS("12340e-1", 1234);
	NUM_INT32_PASS("-0", 0);

	NUM_INT32_FAIL("1.2");
	NUM_INT32_FAIL("1.2E0");
	NUM_INT32_FAIL("1.234e1");
	NUM_INT32_FAIL("12341e-1");
}

static void
test_decode_bool(void)
{
	struct spdk_json_val v;
	bool b;

	/* valid bool (true) */
	v.type = SPDK_JSON_VAL_TRUE;
	b = false;
	CU_ASSERT(spdk_json_decode_bool(&v, &b) == 0);
	CU_ASSERT(b == true);

	/* valid bool (false) */
	v.type = SPDK_JSON_VAL_FALSE;
	b = true;
	CU_ASSERT(spdk_json_decode_bool(&v, &b) == 0);
	CU_ASSERT(b == false);

	/* incorrect type */
	v.type = SPDK_JSON_VAL_NULL;
	CU_ASSERT(spdk_json_decode_bool(&v, &b) != 0);
}

static void
test_decode_int32(void)
{
	struct spdk_json_val v;
	int32_t i;

	/* correct type and valid value */
	v.type = SPDK_JSON_VAL_NUMBER;
	v.start = "33";
	v.len = 2;
	i = 0;
	CU_ASSERT(spdk_json_decode_int32(&v, &i) == 0);
	CU_ASSERT(i == 33)

	/* correct type and invalid value (float) */
	v.start = "32.45";
	v.len = 5;
	i = 0;
	CU_ASSERT(spdk_json_decode_int32(&v, &i) != 0);

	/* incorrect type */
	v.type = SPDK_JSON_VAL_STRING;
	v.start = "String";
	v.len = 6;
	i = 0;
	CU_ASSERT(spdk_json_decode_int32(&v, &i) != 0);

	/* incorrect type */
	v.type = SPDK_JSON_VAL_TRUE;
	CU_ASSERT(spdk_json_decode_int32(&v, &i) != 0);

	/* edge case (integer max) */
	v.type = SPDK_JSON_VAL_NUMBER;
	v.start = "2147483647";
	v.len = 10;
	i = 0;
	CU_ASSERT(spdk_json_decode_int32(&v, &i) == 0);
	CU_ASSERT(i == 2147483647);

	/* invalid value (overflow) */
	v.start = "2147483648";
	i = 0;
	CU_ASSERT(spdk_json_decode_int32(&v, &i) != 0);

	/* edge case (integer min) */
	v.type = SPDK_JSON_VAL_NUMBER;
	v.start = "-2147483648";
	v.len = 11;
	i = 0;
	CU_ASSERT(spdk_json_decode_int32(&v, &i) == 0);
	CU_ASSERT(i == -2147483648);

	/* invalid value (overflow) */
	v.start = "-2147483649";
	CU_ASSERT(spdk_json_decode_int32(&v, &i) != 0);

	/* valid exponent */
	v.start = "4e3";
	v.len = 3;
	i = 0;
	CU_ASSERT(spdk_json_decode_int32(&v, &i) == 0);
	CU_ASSERT(i == 4000);

	/* invalid negative exponent */
	v.start = "-400e-4";
	v.len = 7;
	i = 0;
	CU_ASSERT(spdk_json_decode_int32(&v, &i) != 0);

	/* invalid negative exponent */
	v.start = "400e-4";
	v.len = 6;
	i = 0;
	CU_ASSERT(spdk_json_decode_int32(&v, &i) != 0);

	/* valid negative exponent */
	v.start = "-400e-2";
	v.len = 7;
	i = 0;
	CU_ASSERT(spdk_json_decode_int32(&v, &i) == 0);
	CU_ASSERT(i == -4)

	/* invalid exponent (overflow) */
	v.start = "-2e32";
	v.len = 5;
	i = 0;
	CU_ASSERT(spdk_json_decode_int32(&v, &i) != 0);

	/* valid exponent with decimal */
	v.start = "2.13e2";
	v.len = 6;
	i = 0;
	CU_ASSERT(spdk_json_decode_int32(&v, &i) == 0);
	CU_ASSERT(i == 213)

	/* invalid exponent with decimal */
	v.start = "2.134e2";
	v.len = 7;
	i = 0;
	CU_ASSERT(spdk_json_decode_int32(&v, &i) != 0);
}

static void
test_decode_uint32(void)
{
	struct spdk_json_val v;
	uint32_t i;

	/* incorrect type */
	v.type = SPDK_JSON_VAL_STRING;
	v.start = "String";
	v.len = 6;
	CU_ASSERT(spdk_json_decode_uint32(&v, &i) != 0);

	/* invalid value (float) */
	v.type = SPDK_JSON_VAL_NUMBER;
	v.start = "123.45";
	v.len = 6;
	CU_ASSERT(spdk_json_decode_uint32(&v, &i) != 0);

	/* edge case (0) */
	v.start = "0";
	v.len = 1;
	i = 456;
	CU_ASSERT(spdk_json_decode_uint32(&v, &i) == 0);
	CU_ASSERT(i == 0);

	/* invalid value (negative) */
	v.start = "-1";
	v.len = 2;
	CU_ASSERT(spdk_json_decode_uint32(&v, &i) != 0);

	/* edge case (maximum) */
	v.start = "4294967295";
	v.len = 10;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint32(&v, &i) == 0);
	CU_ASSERT(i == 4294967295);

	/* invalid value (overflow) */
	v.start = "4294967296";
	v.len = 10;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint32(&v, &i) != 0);

	/* valid exponent */
	v.start = "42E2";
	v.len = 4;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint32(&v, &i) == 0);
	CU_ASSERT(i == 4200);

	/* invalid exponent (overflow) */
	v.start = "42e32";
	v.len = 5;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint32(&v, &i) != 0);

	/* invalid exponent (decimal) */
	v.start = "42.323E2";
	v.len = 8;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint32(&v, &i) != 0);

	/* valid exponent with decimal */
	v.start = "42.32E2";
	v.len = 7;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint32(&v, &i) == 0);
	CU_ASSERT(i == 4232);

	/* invalid negative exponent */
	v.start = "400e-4";
	v.len = 6;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint32(&v, &i) != 0);

	/* invalid negative exponent */
	v.start = "-400e-2";
	v.len = 7;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint32(&v, &i) != 0);

	/* valid negative exponent */
	v.start = "400e-2";
	v.len = 6;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint32(&v, &i) == 0);
	CU_ASSERT(i == 4)
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("json", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "strequal", test_strequal) == NULL ||
		CU_add_test(suite, "num_to_int32", test_num_to_int32) == NULL ||
		CU_add_test(suite, "decode_bool", test_decode_bool) == NULL ||
		CU_add_test(suite, "decode_int32", test_decode_int32) == NULL ||
		CU_add_test(suite, "decode_uint32", test_decode_uint32) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
