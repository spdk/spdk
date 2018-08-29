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

#include "json/json_util.c"

/* For spdk_json_parse() */
#include "json/json_parse.c"

#define NUM_SETUP(x) \
	snprintf(buf, sizeof(buf), "%s", x); \
	v.type = SPDK_JSON_VAL_NUMBER; \
	v.start = buf; \
	v.len = sizeof(x) - 1

#define NUM_UINT16_PASS(s, i) \
	NUM_SETUP(s); \
	CU_ASSERT(spdk_json_number_to_uint16(&v, &u16) == 0); \
	CU_ASSERT(u16 == i)

#define NUM_UINT16_FAIL(s) \
	NUM_SETUP(s); \
	CU_ASSERT(spdk_json_number_to_uint16(&v, &u16) != 0)

#define NUM_INT32_PASS(s, i) \
	NUM_SETUP(s); \
	CU_ASSERT(spdk_json_number_to_int32(&v, &i32) == 0); \
	CU_ASSERT(i32 == i)

#define NUM_INT32_FAIL(s) \
	NUM_SETUP(s); \
	CU_ASSERT(spdk_json_number_to_int32(&v, &i32) != 0)

#define NUM_UINT64_PASS(s, i) \
	NUM_SETUP(s); \
	CU_ASSERT(spdk_json_number_to_uint64(&v, &u64) == 0); \
	CU_ASSERT(u64 == i)

#define NUM_UINT64_FAIL(s) \
	NUM_SETUP(s); \
	CU_ASSERT(spdk_json_number_to_uint64(&v, &u64) != 0)

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
test_num_to_uint16(void)
{
	struct spdk_json_val v;
	char buf[100];
	uint16_t u16 = 0;

	NUM_SETUP("1234");
	CU_ASSERT(spdk_json_number_to_uint16(&v, &u16) == 0);
	CU_ASSERT(u16 == 1234);

	NUM_UINT16_PASS("0", 0);
	NUM_UINT16_PASS("1234", 1234);
	NUM_UINT16_PASS("1234.00000", 1234);
	NUM_UINT16_PASS("1.2e1", 12);
	NUM_UINT16_PASS("12340e-1", 1234);

	NUM_UINT16_FAIL("1.2");
	NUM_UINT16_FAIL("-1234");
	NUM_UINT16_FAIL("1.2E0");
	NUM_UINT16_FAIL("1.234e1");
	NUM_UINT16_FAIL("12341e-1");
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
test_num_to_uint64(void)
{
	struct spdk_json_val v;
	char buf[100];
	uint64_t u64 = 0;

	NUM_SETUP("1234");
	CU_ASSERT(spdk_json_number_to_uint64(&v, &u64) == 0);
	CU_ASSERT(u64 == 1234);


	NUM_UINT64_PASS("0", 0);
	NUM_UINT64_PASS("1234", 1234);
	NUM_UINT64_PASS("1234.00000", 1234);
	NUM_UINT64_PASS("1.2e1", 12);
	NUM_UINT64_PASS("12340e-1", 1234);
	NUM_UINT64_PASS("123456780e-1", 12345678);

	NUM_UINT64_FAIL("1.2");
	NUM_UINT64_FAIL("-1234");
	NUM_UINT64_FAIL("1.2E0");
	NUM_UINT64_FAIL("1.234e1");
	NUM_UINT64_FAIL("12341e-1");
	NUM_UINT64_FAIL("123456781e-1");
}

static void
test_decode_object(void)
{
	struct my_object {
		char *my_name;
		uint32_t my_int;
		bool my_bool;
	};
	struct spdk_json_val object[] = {
		{"", 6, SPDK_JSON_VAL_OBJECT_BEGIN},
		{"first", 5, SPDK_JSON_VAL_NAME},
		{"HELLO", 5, SPDK_JSON_VAL_STRING},
		{"second", 6, SPDK_JSON_VAL_NAME},
		{"234", 3, SPDK_JSON_VAL_NUMBER},
		{"third", 5, SPDK_JSON_VAL_NAME},
		{"", 1, SPDK_JSON_VAL_TRUE},
		{"", 0, SPDK_JSON_VAL_OBJECT_END},
	};

	struct spdk_json_object_decoder decoders[] = {
		{"first", offsetof(struct my_object, my_name), spdk_json_decode_string, false},
		{"second", offsetof(struct my_object, my_int), spdk_json_decode_uint32, false},
		{"third", offsetof(struct my_object, my_bool), spdk_json_decode_bool, false},
		{"fourth", offsetof(struct my_object, my_bool), spdk_json_decode_bool, true},
	};
	struct my_object output = {
		.my_name = NULL,
		.my_int = 0,
		.my_bool = false,
	};
	uint32_t answer = 234;
	char *answer_str = "HELLO";
	bool answer_bool = true;

	/* Passing Test: object containing simple types */
	CU_ASSERT(spdk_json_decode_object(object, decoders, 4, &output) == 0);
	SPDK_CU_ASSERT_FATAL(output.my_name != NULL);
	CU_ASSERT(memcmp(output.my_name, answer_str, 6) == 0);
	CU_ASSERT(output.my_int == answer);
	CU_ASSERT(output.my_bool == answer_bool);

	/* Failing Test: member with no matching decoder */
	/* i.e. I remove the matching decoder from the boolean argument */
	CU_ASSERT(spdk_json_decode_object(object, decoders, 2, &output) != 0);

	/* Failing Test: non-optional decoder with no corresponding member */

	decoders[3].optional = false;
	CU_ASSERT(spdk_json_decode_object(object, decoders, 4, &output) != 0);

	/* return to base state */
	decoders[3].optional = true;

	/* Failing Test: duplicated names for json values */
	object[3].start = "first";
	object[3].len = 5;
	CU_ASSERT(spdk_json_decode_object(object, decoders, 3, &output) != 0);

	/* return to base state */
	object[3].start = "second";
	object[3].len = 6;

	/* Failing Test: invalid value for decoder */
	object[2].start = "HELO";
	CU_ASSERT(spdk_json_decode_object(object, decoders, 3, &output) != 0);

	/* return to base state */
	object[2].start = "HELLO";

	/* Failing Test: not an object */
	object[0].type = SPDK_JSON_VAL_ARRAY_BEGIN;
	CU_ASSERT(spdk_json_decode_object(object, decoders, 3, &output) != 0);

	free(output.my_name);
}

static void
test_decode_array(void)
{
	struct spdk_json_val values[4];
	uint32_t my_int[2] = {0, 0};
	char *my_string[2] = {NULL, NULL};
	size_t out_size;

	/* passing integer test */
	values[0].type = SPDK_JSON_VAL_ARRAY_BEGIN;
	values[0].len = 2;
	values[1].type = SPDK_JSON_VAL_NUMBER;
	values[1].len = 4;
	values[1].start = "1234";
	values[2].type = SPDK_JSON_VAL_NUMBER;
	values[2].len = 4;
	values[2].start = "5678";
	values[3].type = SPDK_JSON_VAL_ARRAY_END;
	CU_ASSERT(spdk_json_decode_array(values, spdk_json_decode_uint32, my_int, 2, &out_size,
					 sizeof(uint32_t)) == 0);
	CU_ASSERT(my_int[0] == 1234);
	CU_ASSERT(my_int[1] == 5678);
	CU_ASSERT(out_size == 2);

	/* array length exceeds max */
	values[0].len = 3;
	CU_ASSERT(spdk_json_decode_array(values, spdk_json_decode_uint32, my_int, 2, &out_size,
					 sizeof(uint32_t)) != 0);

	/* mixed types */
	values[0].len = 2;
	values[2].type = SPDK_JSON_VAL_STRING;
	values[2].len = 5;
	values[2].start = "HELLO";
	CU_ASSERT(spdk_json_decode_array(values, spdk_json_decode_uint32, my_int, 2, &out_size,
					 sizeof(uint32_t)) != 0);

	/* no array start */
	values[0].type = SPDK_JSON_VAL_NUMBER;
	values[2].type = SPDK_JSON_VAL_NUMBER;
	values[2].len = 4;
	values[2].start = "5678";
	CU_ASSERT(spdk_json_decode_array(values, spdk_json_decode_uint32, my_int, 2, &out_size,
					 sizeof(uint32_t)) != 0);

	/* mismatched array type and parser */
	values[0].type = SPDK_JSON_VAL_ARRAY_BEGIN;
	values[1].type = SPDK_JSON_VAL_STRING;
	values[1].len = 5;
	values[1].start = "HELLO";
	values[2].type = SPDK_JSON_VAL_STRING;
	values[2].len = 5;
	values[2].start = "WORLD";
	CU_ASSERT(spdk_json_decode_array(values, spdk_json_decode_uint32, my_int, 2, &out_size,
					 sizeof(uint32_t)) != 0);

	/* passing String example */
	CU_ASSERT(spdk_json_decode_array(values, spdk_json_decode_string, my_string, 2, &out_size,
					 sizeof(char *)) == 0);
	SPDK_CU_ASSERT_FATAL(my_string[0] != NULL);
	SPDK_CU_ASSERT_FATAL(my_string[1] != NULL);
	CU_ASSERT(memcmp(my_string[0], "HELLO", 6) == 0);
	CU_ASSERT(memcmp(my_string[1], "WORLD", 6) == 0);
	CU_ASSERT(out_size == 2);

	free(my_string[0]);
	free(my_string[1]);
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
test_decode_uint16(void)
{
	struct spdk_json_val v;
	uint32_t i;

	/* incorrect type */
	v.type = SPDK_JSON_VAL_STRING;
	v.start = "Strin";
	v.len = 5;
	CU_ASSERT(spdk_json_decode_uint16(&v, &i) != 0);

	/* invalid value (float) */
	v.type = SPDK_JSON_VAL_NUMBER;
	v.start = "123.4";
	v.len = 5;
	CU_ASSERT(spdk_json_decode_uint16(&v, &i) != 0);

	/* edge case (0) */
	v.start = "0";
	v.len = 1;
	i = 456;
	CU_ASSERT(spdk_json_decode_uint16(&v, &i) == 0);
	CU_ASSERT(i == 0);

	/* invalid value (negative) */
	v.start = "-1";
	v.len = 2;
	CU_ASSERT(spdk_json_decode_uint16(&v, &i) != 0);

	/* edge case (maximum) */
	v.start = "65535";
	v.len = 5;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint16(&v, &i) == 0);
	CU_ASSERT(i == 65535);

	/* invalid value (overflow) */
	v.start = "65536";
	v.len = 5;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint16(&v, &i) != 0);

	/* valid exponent */
	v.start = "66E2";
	v.len = 4;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint16(&v, &i) == 0);
	CU_ASSERT(i == 6600);

	/* invalid exponent (overflow) */
	v.start = "66E3";
	v.len = 4;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint16(&v, &i) != 0);

	/* invalid exponent (decimal) */
	v.start = "65.535E2";
	v.len = 7;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint16(&v, &i) != 0);

	/* valid exponent with decimal */
	v.start = "65.53E2";
	v.len = 7;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint16(&v, &i) == 0);
	CU_ASSERT(i == 6553);

	/* invalid negative exponent */
	v.start = "40e-2";
	v.len = 5;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint16(&v, &i) != 0);

	/* invalid negative exponent */
	v.start = "-40e-1";
	v.len = 6;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint16(&v, &i) != 0);

	/* valid negative exponent */
	v.start = "40e-1";
	v.len = 5;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint16(&v, &i) == 0);
	CU_ASSERT(i == 4);
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
	CU_ASSERT(i == 4);

	/* valid negative exponent */
	v.start = "10e-1";
	v.len = 5;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint32(&v, &i) == 0);
	CU_ASSERT(i == 1)
}

static void
test_decode_uint64(void)
{
	struct spdk_json_val v;
	uint64_t i;

	/* incorrect type */
	v.type = SPDK_JSON_VAL_STRING;
	v.start = "String";
	v.len = 6;
	CU_ASSERT(spdk_json_decode_uint64(&v, &i) != 0);

	/* invalid value (float) */
	v.type = SPDK_JSON_VAL_NUMBER;
	v.start = "123.45";
	v.len = 6;
	CU_ASSERT(spdk_json_decode_uint64(&v, &i) != 0);

	/* edge case (0) */
	v.start = "0";
	v.len = 1;
	i = 456;
	CU_ASSERT(spdk_json_decode_uint64(&v, &i) == 0);
	CU_ASSERT(i == 0);

	/* invalid value (negative) */
	v.start = "-1";
	v.len = 2;
	CU_ASSERT(spdk_json_decode_uint64(&v, &i) != 0);

	/* edge case (maximum) */
	v.start = "18446744073709551615";
	v.len = 20;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint64(&v, &i) == 0);
	CU_ASSERT(i == 18446744073709551615U);

	/* invalid value (overflow) */
	v.start = "18446744073709551616";
	v.len = 20;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint64(&v, &i) != 0);

	/* valid exponent */
	v.start = "42E2";
	v.len = 4;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint64(&v, &i) == 0);
	CU_ASSERT(i == 4200);

	/* invalid exponent (overflow) */
	v.start = "42e64";
	v.len = 5;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint64(&v, &i) != 0);

	/* invalid exponent (decimal) */
	v.start = "42.323E2";
	v.len = 8;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint64(&v, &i) != 0);

	/* valid exponent with decimal */
	v.start = "42.32E2";
	v.len = 7;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint64(&v, &i) == 0);
	CU_ASSERT(i == 4232);

	/* invalid negative exponent */
	v.start = "400e-4";
	v.len = 6;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint64(&v, &i) != 0);

	/* invalid negative exponent */
	v.start = "-400e-2";
	v.len = 7;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint64(&v, &i) != 0);

	/* valid negative exponent */
	v.start = "400e-2";
	v.len = 6;
	i = 0;
	CU_ASSERT(spdk_json_decode_uint64(&v, &i) == 0);
	CU_ASSERT(i == 4)
}

static void
test_decode_string(void)
{
	struct spdk_json_val v;
	char *value = NULL;

	/* Passing Test: Standard */
	v.type = SPDK_JSON_VAL_STRING;
	v.start = "HELLO";
	v.len = 5;
	CU_ASSERT(spdk_json_decode_string(&v, &value) == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(memcmp(value, v.start, 6) == 0);

	/* Edge Test: Empty String */
	v.start = "";
	v.len = 0;
	CU_ASSERT(spdk_json_decode_string(&v, &value) == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(memcmp(value, v.start, 1) == 0);

	/*
	 * Failing Test: Null Terminator In String
	 * It is valid for a json string to contain \u0000 and the parser will accept it.
	 * However, a null terminated C string cannot contain '\0' and should be rejected
	 * if that character is found before the end of the string.
	 */
	v.start = "HELO";
	v.len = 5;
	CU_ASSERT(spdk_json_decode_string(&v, &value) != 0);

	/* Failing Test: Wrong Type */
	v.start = "45673";
	v.type = SPDK_JSON_VAL_NUMBER;
	CU_ASSERT(spdk_json_decode_string(&v, &value) != 0);

	/* Passing Test: Special Characters */
	v.type = SPDK_JSON_VAL_STRING;
	v.start = "HE\bLL\tO\\WORLD";
	v.len = 13;
	CU_ASSERT(spdk_json_decode_string(&v, &value) == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(memcmp(value, v.start, 14) == 0);

	free(value);
}

char ut_json_text[] =
	"{"
	"	\"string\": \"Some string data\","
	"	\"object\": { "
	"		\"another_string\": \"Yet anoter string data\","
	"		\"array name with space\": [1, [], {} ]"
	"	},"
	"	\"array\": [ \"Text\", 2, {} ]"
	"}"
	;

static void
test_find(void)
{
	struct spdk_json_val *values, *key, *val, *key2, *val2;
	ssize_t values_cnt;
	ssize_t rc;

	values_cnt = spdk_json_parse(ut_json_text, strlen(ut_json_text), NULL, 0, NULL, 0);
	SPDK_CU_ASSERT_FATAL(values_cnt > 0);

	values = calloc(values_cnt, sizeof(struct spdk_json_val));
	SPDK_CU_ASSERT_FATAL(values != NULL);

	rc = spdk_json_parse(ut_json_text, strlen(ut_json_text), values, values_cnt, NULL, 0);
	SPDK_CU_ASSERT_FATAL(values_cnt == rc);

	key = val = NULL;
	rc = spdk_json_find(values, "string", &key, &val, SPDK_JSON_VAL_STRING);
	CU_ASSERT(rc == 0);

	CU_ASSERT(key != NULL && spdk_json_strequal(key, "string") == true);
	CU_ASSERT(val != NULL && spdk_json_strequal(val, "Some string data") == true)

	key = val = NULL;
	rc = spdk_json_find(values, "object", &key, &val, SPDK_JSON_VAL_OBJECT_BEGIN);
	CU_ASSERT(rc == 0);

	CU_ASSERT(key != NULL && spdk_json_strequal(key, "object") == true);

	/* Find key in "object" by passing SPDK_JSON_VAL_ANY to match any type */
	key2 = val2 = NULL;
	rc = spdk_json_find(val, "array name with space", &key2, &val2, SPDK_JSON_VAL_ANY);
	CU_ASSERT(rc == 0);
	CU_ASSERT(key2 != NULL && spdk_json_strequal(key2, "array name with space") == true);
	CU_ASSERT(val2 != NULL && val2->type == SPDK_JSON_VAL_ARRAY_BEGIN);

	/* Find the "array" key in "object" by passing SPDK_JSON_VAL_ARRAY_BEGIN to match only array */
	key2 = val2 = NULL;
	rc = spdk_json_find(val, "array name with space", &key2, &val2, SPDK_JSON_VAL_ARRAY_BEGIN);
	CU_ASSERT(rc == 0);
	CU_ASSERT(key2 != NULL && spdk_json_strequal(key2, "array name with space") == true);
	CU_ASSERT(val2 != NULL && val2->type == SPDK_JSON_VAL_ARRAY_BEGIN);

	/* Negative test - key doesn't exist */
	key2 = val2 = NULL;
	rc = spdk_json_find(val, "this_key_does_not_exist", &key2, &val2, SPDK_JSON_VAL_ANY);
	CU_ASSERT(rc == -ENOENT);

	/* Negative test - key type doesn't match */
	key2 = val2 = NULL;
	rc = spdk_json_find(val, "another_string", &key2, &val2, SPDK_JSON_VAL_ARRAY_BEGIN);
	CU_ASSERT(rc == -EDOM);

	free(values);
}

static void
test_iterating(void)
{
	struct spdk_json_val *values;
	struct spdk_json_val *string_key;
	struct spdk_json_val *object_key, *object_val;
	struct spdk_json_val *array_key, *array_val;
	struct spdk_json_val *another_string_key;
	struct spdk_json_val *array_name_with_space_key, *array_name_with_space_val;
	struct spdk_json_val *it;
	ssize_t values_cnt;
	ssize_t rc;

	values_cnt = spdk_json_parse(ut_json_text, strlen(ut_json_text), NULL, 0, NULL, 0);
	SPDK_CU_ASSERT_FATAL(values_cnt > 0);

	values = calloc(values_cnt, sizeof(struct spdk_json_val));
	SPDK_CU_ASSERT_FATAL(values != NULL);

	rc = spdk_json_parse(ut_json_text, strlen(ut_json_text), values, values_cnt, NULL, 0);
	SPDK_CU_ASSERT_FATAL(values_cnt == rc);

	/* Iterate over object keys. JSON spec doesn't guarantee order of keys in object but
	 * SPDK implementation implicitly does.
	 */
	string_key = spdk_json_object_first(values);
	CU_ASSERT(spdk_json_strequal(string_key, "string") == true);

	object_key = spdk_json_next(string_key);
	object_val = spdk_json_value(object_key);
	CU_ASSERT(spdk_json_strequal(object_key, "object") == true);

	array_key = spdk_json_next(object_key);
	array_val = spdk_json_value(array_key);
	CU_ASSERT(spdk_json_strequal(array_key, "array") == true);

	/* NULL '}' */
	CU_ASSERT(spdk_json_next(array_key) == NULL);

	/* Iterate over subobjects */
	another_string_key = spdk_json_object_first(object_val);
	CU_ASSERT(spdk_json_strequal(another_string_key, "another_string") == true);

	array_name_with_space_key = spdk_json_next(another_string_key);
	array_name_with_space_val = spdk_json_value(array_name_with_space_key);
	CU_ASSERT(spdk_json_strequal(array_name_with_space_key, "array name with space") == true);

	CU_ASSERT(spdk_json_next(array_name_with_space_key) == NULL);

	/* Iterate over array in subobject */
	it = spdk_json_array_first(array_name_with_space_val);
	SPDK_CU_ASSERT_FATAL(it != NULL);
	CU_ASSERT(it->type == SPDK_JSON_VAL_NUMBER);

	it = spdk_json_next(it);
	SPDK_CU_ASSERT_FATAL(it != NULL);
	CU_ASSERT(it->type == SPDK_JSON_VAL_ARRAY_BEGIN);

	it = spdk_json_next(it);
	SPDK_CU_ASSERT_FATAL(it != NULL);
	CU_ASSERT(it->type == SPDK_JSON_VAL_OBJECT_BEGIN);

	it = spdk_json_next(it);
	CU_ASSERT(it == NULL);

	/* Iterate over array in root object */
	it = spdk_json_array_first(array_val);
	SPDK_CU_ASSERT_FATAL(it != NULL);
	CU_ASSERT(it->type == SPDK_JSON_VAL_STRING);

	it = spdk_json_next(it);
	SPDK_CU_ASSERT_FATAL(it != NULL);
	CU_ASSERT(it->type == SPDK_JSON_VAL_NUMBER);

	it = spdk_json_next(it);
	SPDK_CU_ASSERT_FATAL(it != NULL);
	CU_ASSERT(it->type == SPDK_JSON_VAL_OBJECT_BEGIN);

	/* Array end */
	it = spdk_json_next(it);
	CU_ASSERT(it == NULL);

	free(values);
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
		CU_add_test(suite, "num_to_uint16", test_num_to_uint16) == NULL ||
		CU_add_test(suite, "num_to_int32", test_num_to_int32) == NULL ||
		CU_add_test(suite, "num_to_uint64", test_num_to_uint64) == NULL ||
		CU_add_test(suite, "decode_object", test_decode_object) == NULL ||
		CU_add_test(suite, "decode_array", test_decode_array) == NULL ||
		CU_add_test(suite, "decode_bool", test_decode_bool) == NULL ||
		CU_add_test(suite, "decode_uint16", test_decode_uint16) == NULL ||
		CU_add_test(suite, "decode_int32", test_decode_int32) == NULL ||
		CU_add_test(suite, "decode_uint32", test_decode_uint32) == NULL ||
		CU_add_test(suite, "decode_uint64", test_decode_uint64) == NULL ||
		CU_add_test(suite, "decode_string", test_decode_string) == NULL ||
		CU_add_test(suite, "find_object", test_find) == NULL ||
		CU_add_test(suite, "iterating", test_iterating) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
