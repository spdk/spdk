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

#include "json/json_parse.c"

static uint8_t g_buf[1000];
static void *g_end;
static struct spdk_json_val g_vals[100];
static int g_cur_val;

/* Fill buf with raw data */
#define BUF_SETUP(in) \
	memset(g_buf, 0, sizeof(g_buf)); \
	if (sizeof(in) > 1) { \
		memcpy(g_buf, in, sizeof(in) - 1); \
	} \
	g_end = NULL

/*
 * Do two checks - first pass NULL for values to ensure the count is correct,
 *  then pass g_vals to get the actual values.
 */
#define PARSE_PASS_FLAGS(in, num_vals, trailing, flags) \
	BUF_SETUP(in); \
	CU_ASSERT(spdk_json_parse(g_buf, sizeof(in) - 1, NULL, 0, &g_end, flags) == num_vals); \
	memset(g_vals, 0, sizeof(g_vals)); \
	CU_ASSERT(spdk_json_parse(g_buf, sizeof(in) - 1, g_vals, sizeof(g_vals), &g_end, flags | SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE) == num_vals); \
	CU_ASSERT(g_end == g_buf + sizeof(in) - sizeof(trailing)); \
	CU_ASSERT(memcmp(g_end, trailing, sizeof(trailing) - 1) == 0); \
	g_cur_val = 0

#define PARSE_PASS(in, num_vals, trailing) \
	PARSE_PASS_FLAGS(in, num_vals, trailing, 0)

#define PARSE_FAIL_FLAGS(in, retval, flags) \
	BUF_SETUP(in); \
	CU_ASSERT(spdk_json_parse(g_buf, sizeof(in) - 1, NULL, 0, &g_end, flags) == retval)

#define PARSE_FAIL(in, retval) \
	PARSE_FAIL_FLAGS(in, retval, 0)

#define VAL_STRING_MATCH(str, var_type) \
	CU_ASSERT(g_vals[g_cur_val].type == var_type); \
	CU_ASSERT(g_vals[g_cur_val].len == sizeof(str) - 1); \
	if (g_vals[g_cur_val].len == sizeof(str) - 1 && sizeof(str) > 1) { \
		CU_ASSERT(memcmp(g_vals[g_cur_val].start, str, g_vals[g_cur_val].len) == 0); \
	} \
	g_cur_val++

#define VAL_STRING(str) VAL_STRING_MATCH(str, SPDK_JSON_VAL_STRING)
#define VAL_NAME(str) VAL_STRING_MATCH(str, SPDK_JSON_VAL_NAME)
#define VAL_NUMBER(num) VAL_STRING_MATCH(num, SPDK_JSON_VAL_NUMBER)

#define VAL_LITERAL(str, val_type) \
	CU_ASSERT(g_vals[g_cur_val].type == val_type); \
	CU_ASSERT(g_vals[g_cur_val].len == strlen(str)); \
	if (g_vals[g_cur_val].len == strlen(str)) { \
		CU_ASSERT(memcmp(g_vals[g_cur_val].start, str, g_vals[g_cur_val].len) == 0); \
	} \
	g_cur_val++

#define VAL_TRUE() VAL_LITERAL("true", SPDK_JSON_VAL_TRUE)
#define VAL_FALSE() VAL_LITERAL("false", SPDK_JSON_VAL_FALSE)
#define VAL_NULL() VAL_LITERAL("null", SPDK_JSON_VAL_NULL)

#define VAL_ARRAY_BEGIN(count) \
	CU_ASSERT(g_vals[g_cur_val].type == SPDK_JSON_VAL_ARRAY_BEGIN); \
	CU_ASSERT(g_vals[g_cur_val].len == count); \
	g_cur_val++

#define VAL_ARRAY_END() \
	CU_ASSERT(g_vals[g_cur_val].type == SPDK_JSON_VAL_ARRAY_END); \
	g_cur_val++

#define VAL_OBJECT_BEGIN(count) \
	CU_ASSERT(g_vals[g_cur_val].type == SPDK_JSON_VAL_OBJECT_BEGIN); \
	CU_ASSERT(g_vals[g_cur_val].len == count); \
	g_cur_val++

#define VAL_OBJECT_END() \
	CU_ASSERT(g_vals[g_cur_val].type == SPDK_JSON_VAL_OBJECT_END); \
	g_cur_val++

/* Simplified macros for string-only testing */
#define STR_PASS(in, out) \
	PARSE_PASS("\"" in "\"", 1, ""); \
	VAL_STRING(out)

#define STR_FAIL(in, retval) \
	PARSE_FAIL("\"" in "\"", retval)

/* Simplified macros for number-only testing (no whitespace allowed) */
#define NUM_PASS(in) \
	PARSE_PASS(in, 1, ""); \
	VAL_NUMBER(in)

#define NUM_FAIL(in, retval) \
	PARSE_FAIL(in, retval)

static void
test_parse_literal(void)
{
	PARSE_PASS("true", 1, "");
	VAL_TRUE();

	PARSE_PASS("  true  ", 1, "");
	VAL_TRUE();

	PARSE_PASS("false", 1, "");
	VAL_FALSE();

	PARSE_PASS("null", 1, "");
	VAL_NULL();

	PARSE_PASS("trueaaa", 1, "aaa");
	VAL_TRUE();

	PARSE_PASS("truefalse", 1, "false");
	VAL_TRUE();

	PARSE_PASS("true false", 1, "false");
	VAL_TRUE();

	PARSE_PASS("true,false", 1, ",false");
	VAL_TRUE();

	PARSE_PASS("true,", 1, ",");
	VAL_TRUE();

	PARSE_FAIL("True", SPDK_JSON_PARSE_INVALID);
	PARSE_FAIL("abcdef", SPDK_JSON_PARSE_INVALID);

	PARSE_FAIL("t", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("tru", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("f", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("fals", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("n", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("nul", SPDK_JSON_PARSE_INCOMPLETE);

	PARSE_FAIL("taaaaa", SPDK_JSON_PARSE_INVALID);
	PARSE_FAIL("faaaaa", SPDK_JSON_PARSE_INVALID);
	PARSE_FAIL("naaaaa", SPDK_JSON_PARSE_INVALID);
}

static void
test_parse_string_simple(void)
{
	PARSE_PASS("\"\"", 1, "");
	VAL_STRING("");

	PARSE_PASS("\"hello world\"", 1, "");
	VAL_STRING("hello world");

	PARSE_PASS("     \"hello world\"     ", 1, "");
	VAL_STRING("hello world");

	/* Unterminated string */
	PARSE_FAIL("\"hello world", SPDK_JSON_PARSE_INCOMPLETE);

	/* Trailing comma */
	PARSE_PASS("\"hello world\",", 1, ",");
	VAL_STRING("hello world");
}

static void
test_parse_string_control_chars(void)
{
	/* U+0000 through U+001F must be escaped */
	STR_FAIL("\x00", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x01", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x02", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x03", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x04", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x05", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x06", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x07", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x08", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x09", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x0A", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x0B", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x0C", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x0D", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x0E", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x0F", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x10", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x11", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x12", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x13", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x14", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x15", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x16", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x17", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x18", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x19", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x1A", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x1B", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x1C", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x1D", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x1E", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\x1F", SPDK_JSON_PARSE_INVALID);
	STR_PASS(" ", " "); /* \x20 (first valid unescaped char) */

	/* Test control chars in the middle of a string */
	STR_FAIL("abc\ndef", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("abc\tdef", SPDK_JSON_PARSE_INVALID);
}

static void
test_parse_string_utf8(void)
{
	/* Valid one-, two-, three-, and four-byte sequences */
	STR_PASS("\x41", "A");
	STR_PASS("\xC3\xB6", "\xC3\xB6");
	STR_PASS("\xE2\x88\x9A", "\xE2\x88\x9A");
	STR_PASS("\xF0\xA0\x9C\x8E", "\xF0\xA0\x9C\x8E");

	/* Examples from RFC 3629 */
	STR_PASS("\x41\xE2\x89\xA2\xCE\x91\x2E", "\x41\xE2\x89\xA2\xCE\x91\x2E");
	STR_PASS("\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4", "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4");
	STR_PASS("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
	STR_PASS("\xEF\xBB\xBF\xF0\xA3\x8E\xB4", "\xEF\xBB\xBF\xF0\xA3\x8E\xB4");

	/* Edge cases */
	STR_PASS("\x7F", "\x7F");
	STR_FAIL("\x80", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xC1", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xC2", SPDK_JSON_PARSE_INVALID);
	STR_PASS("\xC2\x80", "\xC2\x80");
	STR_PASS("\xC2\xBF", "\xC2\xBF");
	STR_PASS("\xDF\x80", "\xDF\x80");
	STR_PASS("\xDF\xBF", "\xDF\xBF");
	STR_FAIL("\xDF", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xE0\x80", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xE0\x1F", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xE0\x1F\x80", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xE0", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xE0\xA0", SPDK_JSON_PARSE_INVALID);
	STR_PASS("\xE0\xA0\x80", "\xE0\xA0\x80");
	STR_PASS("\xE0\xA0\xBF", "\xE0\xA0\xBF");
	STR_FAIL("\xE0\xA0\xC0", SPDK_JSON_PARSE_INVALID);
	STR_PASS("\xE0\xBF\x80", "\xE0\xBF\x80");
	STR_PASS("\xE0\xBF\xBF", "\xE0\xBF\xBF");
	STR_FAIL("\xE0\xC0\x80", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xE1", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xE1\x80", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xE1\x7F\x80", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xE1\x80\x7F", SPDK_JSON_PARSE_INVALID);
	STR_PASS("\xE1\x80\x80", "\xE1\x80\x80");
	STR_PASS("\xE1\x80\xBF", "\xE1\x80\xBF");
	STR_PASS("\xE1\xBF\x80", "\xE1\xBF\x80");
	STR_PASS("\xE1\xBF\xBF", "\xE1\xBF\xBF");
	STR_FAIL("\xE1\xC0\x80", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xE1\x80\xC0", SPDK_JSON_PARSE_INVALID);
	STR_PASS("\xEF\x80\x80", "\xEF\x80\x80");
	STR_PASS("\xEF\xBF\xBF", "\xEF\xBF\xBF");
	STR_FAIL("\xF0", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xF0\x90", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xF0\x90\x80", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xF0\x80\x80\x80", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xF0\x8F\x80\x80", SPDK_JSON_PARSE_INVALID);
	STR_PASS("\xF0\x90\x80\x80", "\xF0\x90\x80\x80");
	STR_PASS("\xF0\x90\x80\xBF", "\xF0\x90\x80\xBF");
	STR_PASS("\xF0\x90\xBF\x80", "\xF0\x90\xBF\x80");
	STR_PASS("\xF0\xBF\x80\x80", "\xF0\xBF\x80\x80");
	STR_FAIL("\xF0\xC0\x80\x80", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xF1", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xF1\x80", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xF1\x80\x80", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xF1\x80\x80\x7F", SPDK_JSON_PARSE_INVALID);
	STR_PASS("\xF1\x80\x80\x80", "\xF1\x80\x80\x80");
	STR_PASS("\xF1\x80\x80\xBF", "\xF1\x80\x80\xBF");
	STR_PASS("\xF1\x80\xBF\x80", "\xF1\x80\xBF\x80");
	STR_PASS("\xF1\xBF\x80\x80", "\xF1\xBF\x80\x80");
	STR_PASS("\xF3\x80\x80\x80", "\xF3\x80\x80\x80");
	STR_FAIL("\xF3\xC0\x80\x80", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xF3\x80\xC0\x80", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xF3\x80\x80\xC0", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xF4", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xF4\x80", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xF4\x80\x80", SPDK_JSON_PARSE_INVALID);
	STR_PASS("\xF4\x80\x80\x80", "\xF4\x80\x80\x80");
	STR_PASS("\xF4\x8F\x80\x80", "\xF4\x8F\x80\x80");
	STR_PASS("\xF4\x8F\xBF\xBF", "\xF4\x8F\xBF\xBF");
	STR_FAIL("\xF4\x90\x80\x80", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xF5", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xF5\x80", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xF5\x80\x80", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xF5\x80\x80\x80", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\xF5\x80\x80\x80\x80", SPDK_JSON_PARSE_INVALID);

	/* Overlong encodings */
	STR_FAIL("\xC0\x80", SPDK_JSON_PARSE_INVALID);

	/* Surrogate pairs */
	STR_FAIL("\xED\xA0\x80", SPDK_JSON_PARSE_INVALID); /* U+D800 First high surrogate */
	STR_FAIL("\xED\xAF\xBF", SPDK_JSON_PARSE_INVALID); /* U+DBFF Last high surrogate */
	STR_FAIL("\xED\xB0\x80", SPDK_JSON_PARSE_INVALID); /* U+DC00 First low surrogate */
	STR_FAIL("\xED\xBF\xBF", SPDK_JSON_PARSE_INVALID); /* U+DFFF Last low surrogate */
	STR_FAIL("\xED\xA1\x8C\xED\xBE\xB4",
		 SPDK_JSON_PARSE_INVALID); /* U+233B4 (invalid surrogate pair encoding) */
}

static void
test_parse_string_escapes_twochar(void)
{
	STR_PASS("\\\"", "\"");
	STR_PASS("\\\\", "\\");
	STR_PASS("\\/", "/");
	STR_PASS("\\b", "\b");
	STR_PASS("\\f", "\f");
	STR_PASS("\\n", "\n");
	STR_PASS("\\r", "\r");
	STR_PASS("\\t", "\t");

	STR_PASS("abc\\tdef", "abc\tdef");
	STR_PASS("abc\\\"def", "abc\"def");

	/* Backslash at end of string (will be treated as escaped quote) */
	STR_FAIL("\\", SPDK_JSON_PARSE_INCOMPLETE);
	STR_FAIL("abc\\", SPDK_JSON_PARSE_INCOMPLETE);

	/* Invalid C-like escapes */
	STR_FAIL("\\a", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\\v", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\\'", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\\?", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\\0", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\\x00", SPDK_JSON_PARSE_INVALID);

	/* Other invalid escapes */
	STR_FAIL("\\B", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\\z", SPDK_JSON_PARSE_INVALID);
}

static void
test_parse_string_escapes_unicode(void)
{
	STR_PASS("\\u0000", "\0");
	STR_PASS("\\u0001", "\1");
	STR_PASS("\\u0041", "A");
	STR_PASS("\\uAAAA", "\xEA\xAA\xAA");
	STR_PASS("\\uaaaa", "\xEA\xAA\xAA");
	STR_PASS("\\uAaAa", "\xEA\xAA\xAA");

	STR_FAIL("\\u", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\\u0", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\\u00", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\\u000", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\\u000g", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\\U", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\\U0000", SPDK_JSON_PARSE_INVALID);

	PARSE_FAIL("\"\\u", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("\"\\u0", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("\"\\u00", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("\"\\u000", SPDK_JSON_PARSE_INCOMPLETE);

	/* Surrogate pair */
	STR_PASS("\\uD834\\uDD1E", "\xF0\x9D\x84\x9E");

	/* Low surrogate without high */
	STR_FAIL("\\uDC00", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\\uDC00\\uDC00", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\\uDC00abcdef", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\\uDEAD", SPDK_JSON_PARSE_INVALID);
	PARSE_FAIL("\"\\uD834", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("\"\\uD834\\", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("\"\\uD834\\u", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("\"\\uD834\\uD", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("\"\\uD834\\uDD1", SPDK_JSON_PARSE_INCOMPLETE);

	/* High surrogate without low */
	STR_FAIL("\\uD800", SPDK_JSON_PARSE_INVALID);
	STR_FAIL("\\uD800abcdef", SPDK_JSON_PARSE_INVALID);

	/* High surrogate followed by high surrogate */
	STR_FAIL("\\uD800\\uD800", SPDK_JSON_PARSE_INVALID);
}

static void
test_parse_number(void)
{
	NUM_PASS("0");
	NUM_PASS("1");
	NUM_PASS("100");
	NUM_PASS("-1");
	NUM_PASS("-0");
	NUM_PASS("3.0");
	NUM_PASS("3.00");
	NUM_PASS("3.001");
	NUM_PASS("3.14159");
	NUM_PASS("3.141592653589793238462643383279");
	NUM_PASS("1e400");
	NUM_PASS("1E400");
	NUM_PASS("0e10");
	NUM_PASS("0e0");
	NUM_PASS("-0e0");
	NUM_PASS("-0e+0");
	NUM_PASS("-0e-0");
	NUM_PASS("1e+400");
	NUM_PASS("1e-400");
	NUM_PASS("6.022e23");
	NUM_PASS("-1.234e+56");
	NUM_PASS("1.23e+56");
	NUM_PASS("-1.23e-56");
	NUM_PASS("1.23e-56");
	NUM_PASS("1e04");

	/* Trailing garbage */
	PARSE_PASS("0A", 1, "A");
	VAL_NUMBER("0");

	PARSE_PASS("0,", 1, ",");
	VAL_NUMBER("0");

	PARSE_PASS("0true", 1, "true");
	VAL_NUMBER("0");

	PARSE_PASS("00", 1, "0");
	VAL_NUMBER("0");
	PARSE_FAIL("[00", SPDK_JSON_PARSE_INVALID);

	PARSE_PASS("007", 1, "07");
	VAL_NUMBER("0");
	PARSE_FAIL("[007]", SPDK_JSON_PARSE_INVALID);

	PARSE_PASS("345.678.1", 1, ".1");
	VAL_NUMBER("345.678");
	PARSE_FAIL("[345.678.1]", SPDK_JSON_PARSE_INVALID);

	PARSE_PASS("3.2e-4+5", 1, "+5");
	VAL_NUMBER("3.2e-4");
	PARSE_FAIL("[3.2e-4+5]", SPDK_JSON_PARSE_INVALID);

	PARSE_PASS("3.4.5", 1, ".5");
	VAL_NUMBER("3.4");
	PARSE_FAIL("[3.4.5]", SPDK_JSON_PARSE_INVALID);

	NUM_FAIL("345.", SPDK_JSON_PARSE_INCOMPLETE);
	NUM_FAIL("+1", SPDK_JSON_PARSE_INVALID);
	NUM_FAIL("--1", SPDK_JSON_PARSE_INVALID);
	NUM_FAIL("3.", SPDK_JSON_PARSE_INCOMPLETE);
	NUM_FAIL("3.+4", SPDK_JSON_PARSE_INVALID);
	NUM_FAIL("3.2e+-4", SPDK_JSON_PARSE_INVALID);
	NUM_FAIL("3.2e-+4", SPDK_JSON_PARSE_INVALID);
	NUM_FAIL("3e+", SPDK_JSON_PARSE_INCOMPLETE);
	NUM_FAIL("3e-", SPDK_JSON_PARSE_INCOMPLETE);
	NUM_FAIL("3.e4", SPDK_JSON_PARSE_INVALID);
	NUM_FAIL("3.2eX", SPDK_JSON_PARSE_INVALID);
	NUM_FAIL("-", SPDK_JSON_PARSE_INCOMPLETE);
	NUM_FAIL("NaN", SPDK_JSON_PARSE_INVALID);
	NUM_FAIL(".123", SPDK_JSON_PARSE_INVALID);
}

static void
test_parse_array(void)
{
	char buffer[SPDK_JSON_MAX_NESTING_DEPTH + 2] = {0};

	PARSE_PASS("[]", 2, "");
	VAL_ARRAY_BEGIN(0);
	VAL_ARRAY_END();

	PARSE_PASS("[true]", 3, "");
	VAL_ARRAY_BEGIN(1);
	VAL_TRUE();
	VAL_ARRAY_END();

	PARSE_PASS("[true, false]", 4, "");
	VAL_ARRAY_BEGIN(2);
	VAL_TRUE();
	VAL_FALSE();
	VAL_ARRAY_END();

	PARSE_PASS("[\"hello\"]", 3, "");
	VAL_ARRAY_BEGIN(1);
	VAL_STRING("hello");
	VAL_ARRAY_END();

	PARSE_PASS("[[]]", 4, "");
	VAL_ARRAY_BEGIN(2);
	VAL_ARRAY_BEGIN(0);
	VAL_ARRAY_END();
	VAL_ARRAY_END();

	PARSE_PASS("[\"hello\", \"world\"]", 4, "");
	VAL_ARRAY_BEGIN(2);
	VAL_STRING("hello");
	VAL_STRING("world");
	VAL_ARRAY_END();

	PARSE_PASS("[],", 2, ",");
	VAL_ARRAY_BEGIN(0);
	VAL_ARRAY_END();

	PARSE_FAIL("]", SPDK_JSON_PARSE_INVALID);
	PARSE_FAIL("[", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("[true", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("[\"hello", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("[\"hello\"", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("[true,]", SPDK_JSON_PARSE_INVALID);
	PARSE_FAIL("[,]", SPDK_JSON_PARSE_INVALID);
	PARSE_FAIL("[,true]", SPDK_JSON_PARSE_INVALID);
	PARSE_FAIL("[true}", SPDK_JSON_PARSE_INVALID);
	PARSE_FAIL("[true,,true]", SPDK_JSON_PARSE_INVALID);

	/* Nested arrays exactly up to the allowed nesting depth */
	memset(buffer, '[', SPDK_JSON_MAX_NESTING_DEPTH);
	buffer[SPDK_JSON_MAX_NESTING_DEPTH] = ' ';
	PARSE_FAIL(buffer, SPDK_JSON_PARSE_INCOMPLETE);

	/* Nested arrays exceeding the maximum allowed nesting depth for this implementation */
	buffer[SPDK_JSON_MAX_NESTING_DEPTH] = '[';
	PARSE_FAIL(buffer, SPDK_JSON_PARSE_MAX_DEPTH_EXCEEDED);
}

static void
test_parse_object(void)
{
	PARSE_PASS("{}", 2, "");
	VAL_OBJECT_BEGIN(0);
	VAL_OBJECT_END();

	PARSE_PASS("{\"a\": true}", 4, "");
	VAL_OBJECT_BEGIN(2);
	VAL_NAME("a");
	VAL_TRUE();
	VAL_OBJECT_END();

	PARSE_PASS("{\"abc\": \"def\"}", 4, "");
	VAL_OBJECT_BEGIN(2);
	VAL_NAME("abc");
	VAL_STRING("def");
	VAL_OBJECT_END();

	PARSE_PASS("{\"a\": true, \"b\": false}", 6, "");
	VAL_OBJECT_BEGIN(4);
	VAL_NAME("a");
	VAL_TRUE();
	VAL_NAME("b");
	VAL_FALSE();
	VAL_OBJECT_END();

	PARSE_PASS("{\"a\": { \"b\": true } }", 7, "");
	VAL_OBJECT_BEGIN(5);
	VAL_NAME("a");
	VAL_OBJECT_BEGIN(2);
	VAL_NAME("b");
	VAL_TRUE();
	VAL_OBJECT_END();
	VAL_OBJECT_END();

	PARSE_PASS("{\"{test\": 0}", 4, "");
	VAL_OBJECT_BEGIN(2);
	VAL_NAME("{test");
	VAL_NUMBER("0");
	VAL_OBJECT_END();

	PARSE_PASS("{\"test}\": 1}", 4, "");
	VAL_OBJECT_BEGIN(2);
	VAL_NAME("test}");
	VAL_NUMBER("1");
	VAL_OBJECT_END();

	PARSE_PASS("{\"\\\"\": 2}", 4, "");
	VAL_OBJECT_BEGIN(2);
	VAL_NAME("\"");
	VAL_NUMBER("2");
	VAL_OBJECT_END();

	PARSE_PASS("{\"a\":true},", 4, ",");
	VAL_OBJECT_BEGIN(2);
	VAL_NAME("a");
	VAL_TRUE();
	VAL_OBJECT_END();

	/* Object end without object begin (trailing garbage) */
	PARSE_PASS("true}", 1, "}");
	VAL_TRUE();

	PARSE_PASS("0}", 1, "}");
	VAL_NUMBER("0");

	PARSE_PASS("\"a\"}", 1, "}");
	VAL_STRING("a");

	PARSE_FAIL("}", SPDK_JSON_PARSE_INVALID);
	PARSE_FAIL("{", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("{\"a", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("{\"a\"", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("{\"a\":", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("{\"a\":true", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("{\"a\":true,", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("{\"a\":true]", SPDK_JSON_PARSE_INVALID);
	PARSE_FAIL("{\"a\":true,}", SPDK_JSON_PARSE_INVALID);
	PARSE_FAIL("{\"a\":true,\"}", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("{\"a\":true,\"b}", SPDK_JSON_PARSE_INCOMPLETE);
	PARSE_FAIL("{\"a\":true,\"b\"}", SPDK_JSON_PARSE_INVALID);
	PARSE_FAIL("{\"a\":true,\"b\":}", SPDK_JSON_PARSE_INVALID);
	PARSE_FAIL("{\"a\":true,\"b\",}", SPDK_JSON_PARSE_INVALID);
	PARSE_FAIL("{\"a\",}", SPDK_JSON_PARSE_INVALID);
	PARSE_FAIL("{,\"a\": true}", SPDK_JSON_PARSE_INVALID);
	PARSE_FAIL("{a:true}", SPDK_JSON_PARSE_INVALID);
	PARSE_FAIL("{'a':true}", SPDK_JSON_PARSE_INVALID);
}

static void
test_parse_nesting(void)
{
	PARSE_PASS("[[[[[[[[]]]]]]]]", 16, "");

	PARSE_PASS("{\"a\": [0, 1, 2]}", 8, "");
	VAL_OBJECT_BEGIN(6);
	VAL_NAME("a");
	VAL_ARRAY_BEGIN(3);
	VAL_NUMBER("0");
	VAL_NUMBER("1");
	VAL_NUMBER("2");
	VAL_ARRAY_END();
	VAL_OBJECT_END();

	PARSE_PASS("{\"a\": [0, 1, 2], \"b\": 3 }", 10, "");
	VAL_OBJECT_BEGIN(8);
	VAL_NAME("a");
	VAL_ARRAY_BEGIN(3);
	VAL_NUMBER("0");
	VAL_NUMBER("1");
	VAL_NUMBER("2");
	VAL_ARRAY_END();
	VAL_NAME("b");
	VAL_NUMBER("3");
	VAL_OBJECT_END();

	PARSE_PASS("[0, 1, {\"a\": 3}, 4, 5]", 10, "");
	VAL_ARRAY_BEGIN(8);
	VAL_NUMBER("0");
	VAL_NUMBER("1");
	VAL_OBJECT_BEGIN(2);
	VAL_NAME("a");
	VAL_NUMBER("3");
	VAL_OBJECT_END();
	VAL_NUMBER("4");
	VAL_NUMBER("5");
	VAL_ARRAY_END();

	PARSE_PASS("\t[  { \"a\": {\"b\": [ {\"c\": 1}, 2 ],\n\"d\": 3}, \"e\" : 4}, 5 ] ", 20, "");
	VAL_ARRAY_BEGIN(18);
	VAL_OBJECT_BEGIN(15);
	VAL_NAME("a");
	VAL_OBJECT_BEGIN(10);
	VAL_NAME("b");
	VAL_ARRAY_BEGIN(5);
	VAL_OBJECT_BEGIN(2);
	VAL_NAME("c");
	VAL_NUMBER("1");
	VAL_OBJECT_END();
	VAL_NUMBER("2");
	VAL_ARRAY_END();
	VAL_NAME("d");
	VAL_NUMBER("3");
	VAL_OBJECT_END();
	VAL_NAME("e");
	VAL_NUMBER("4");
	VAL_OBJECT_END();
	VAL_NUMBER("5");
	VAL_ARRAY_END();

	/* Examples from RFC 7159 */
	PARSE_PASS(
		"{\n"
		"  \"Image\": {\n"
		"    \"Width\":  800,\n"
		"    \"Height\": 600,\n"
		"    \"Title\":  \"View from 15th Floor\",\n"
		"    \"Thumbnail\": {\n"
		"        \"Url\":    \"http://www.example.com/image/481989943\",\n"
		"        \"Height\": 125,\n"
		"        \"Width\":  100\n"
		"    },\n"
		"    \"Animated\" : false,\n"
		"    \"IDs\": [116, 943, 234, 38793]\n"
		"  }\n"
		"}\n",
		29, "");

	VAL_OBJECT_BEGIN(27);
	VAL_NAME("Image");
	VAL_OBJECT_BEGIN(24);
	VAL_NAME("Width");
	VAL_NUMBER("800");
	VAL_NAME("Height");
	VAL_NUMBER("600");
	VAL_NAME("Title");
	VAL_STRING("View from 15th Floor");
	VAL_NAME("Thumbnail");
	VAL_OBJECT_BEGIN(6);
	VAL_NAME("Url");
	VAL_STRING("http://www.example.com/image/481989943");
	VAL_NAME("Height");
	VAL_NUMBER("125");
	VAL_NAME("Width");
	VAL_NUMBER("100");
	VAL_OBJECT_END();
	VAL_NAME("Animated");
	VAL_FALSE();
	VAL_NAME("IDs");
	VAL_ARRAY_BEGIN(4);
	VAL_NUMBER("116");
	VAL_NUMBER("943");
	VAL_NUMBER("234");
	VAL_NUMBER("38793");
	VAL_ARRAY_END();
	VAL_OBJECT_END();
	VAL_OBJECT_END();

	PARSE_PASS(
		"[\n"
		"  {\n"
		"    \"precision\": \"zip\",\n"
		"    \"Latitude\":  37.7668,\n"
		"    \"Longitude\": -122.3959,\n"
		"    \"Address\":   \"\",\n"
		"    \"City\":      \"SAN FRANCISCO\",\n"
		"    \"State\":     \"CA\",\n"
		"    \"Zip\":       \"94107\",\n"
		"    \"Country\":   \"US\"\n"
		"  },\n"
		"  {\n"
		"    \"precision\": \"zip\",\n"
		"    \"Latitude\":  37.371991,\n"
		"    \"Longitude\": -122.026020,\n"
		"    \"Address\":   \"\",\n"
		"    \"City\":      \"SUNNYVALE\",\n"
		"    \"State\":     \"CA\",\n"
		"    \"Zip\":       \"94085\",\n"
		"    \"Country\":   \"US\"\n"
		"  }\n"
		"]",
		38, "");

	VAL_ARRAY_BEGIN(36);
	VAL_OBJECT_BEGIN(16);
	VAL_NAME("precision");
	VAL_STRING("zip");
	VAL_NAME("Latitude");
	VAL_NUMBER("37.7668");
	VAL_NAME("Longitude");
	VAL_NUMBER("-122.3959");
	VAL_NAME("Address");
	VAL_STRING("");
	VAL_NAME("City");
	VAL_STRING("SAN FRANCISCO");
	VAL_NAME("State");
	VAL_STRING("CA");
	VAL_NAME("Zip");
	VAL_STRING("94107");
	VAL_NAME("Country");
	VAL_STRING("US");
	VAL_OBJECT_END();
	VAL_OBJECT_BEGIN(16);
	VAL_NAME("precision");
	VAL_STRING("zip");
	VAL_NAME("Latitude");
	VAL_NUMBER("37.371991");
	VAL_NAME("Longitude");
	VAL_NUMBER("-122.026020");
	VAL_NAME("Address");
	VAL_STRING("");
	VAL_NAME("City");
	VAL_STRING("SUNNYVALE");
	VAL_NAME("State");
	VAL_STRING("CA");
	VAL_NAME("Zip");
	VAL_STRING("94085");
	VAL_NAME("Country");
	VAL_STRING("US");
	VAL_OBJECT_END();
	VAL_ARRAY_END();

	/* Trailing garbage */
	PARSE_PASS("{\"a\": [0, 1, 2]}]", 8, "]");
	VAL_OBJECT_BEGIN(6);
	VAL_NAME("a");
	VAL_ARRAY_BEGIN(3);
	VAL_NUMBER("0");
	VAL_NUMBER("1");
	VAL_NUMBER("2");
	VAL_ARRAY_END();
	VAL_OBJECT_END();

	PARSE_PASS("{\"a\": [0, 1, 2]}}", 8, "}");
	PARSE_PASS("{\"a\": [0, 1, 2]}]", 8, "]");
	VAL_OBJECT_BEGIN(6);
	VAL_NAME("a");
	VAL_ARRAY_BEGIN(3);
	VAL_NUMBER("0");
	VAL_NUMBER("1");
	VAL_NUMBER("2");
	VAL_ARRAY_END();
	VAL_OBJECT_END();

	PARSE_FAIL("{\"a\": [0, 1, 2}]", SPDK_JSON_PARSE_INVALID);
	PARSE_FAIL("{\"a\": [0, 1, 2]", SPDK_JSON_PARSE_INCOMPLETE);
}


static void
test_parse_comment(void)
{
	/* Comments are not allowed by the JSON RFC */
	PARSE_PASS("[0]", 3, "");
	PARSE_FAIL("/* test */[0]", SPDK_JSON_PARSE_INVALID);
	PARSE_FAIL("[/* test */0]", SPDK_JSON_PARSE_INVALID);
	PARSE_FAIL("[0/* test */]", SPDK_JSON_PARSE_INVALID);

	/*
	 * This is allowed since the parser stops once it reads a complete JSON object.
	 * The next parse call would fail (see tests above) when parsing the comment.
	 */
	PARSE_PASS("[0]/* test */", 3, "/* test */");

	/*
	 * Test with non-standard comments enabled.
	 */
	PARSE_PASS_FLAGS("/* test */[0]", 3, "", SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
	VAL_ARRAY_BEGIN(1);
	VAL_NUMBER("0");
	VAL_ARRAY_END();

	PARSE_PASS_FLAGS("[/* test */0]", 3, "", SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
	VAL_ARRAY_BEGIN(1);
	VAL_NUMBER("0");
	VAL_ARRAY_END();

	PARSE_PASS_FLAGS("[0/* test */]", 3, "", SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
	VAL_ARRAY_BEGIN(1);
	VAL_NUMBER("0");
	VAL_ARRAY_END();

	PARSE_FAIL_FLAGS("/* test */", SPDK_JSON_PARSE_INCOMPLETE, SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
	PARSE_FAIL_FLAGS("[/* test */", SPDK_JSON_PARSE_INCOMPLETE, SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
	PARSE_FAIL_FLAGS("[0/* test */", SPDK_JSON_PARSE_INCOMPLETE, SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);

	/*
	 * Single-line comments
	 */
	PARSE_PASS_FLAGS("// test\n0", 1, "", SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
	VAL_NUMBER("0");

	PARSE_PASS_FLAGS("// test\r\n0", 1, "", SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
	VAL_NUMBER("0");

	PARSE_PASS_FLAGS("// [0] test\n0", 1, "", SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
	VAL_NUMBER("0");

	PARSE_FAIL_FLAGS("//", SPDK_JSON_PARSE_INCOMPLETE, SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
	PARSE_FAIL_FLAGS("// test", SPDK_JSON_PARSE_INCOMPLETE, SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
	PARSE_FAIL_FLAGS("//\n", SPDK_JSON_PARSE_INCOMPLETE, SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);

	/* Invalid character following slash */
	PARSE_FAIL_FLAGS("[0/x", SPDK_JSON_PARSE_INVALID, SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);

	/* Single slash at end of buffer */
	PARSE_FAIL_FLAGS("[0/", SPDK_JSON_PARSE_INCOMPLETE, SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("json", NULL, NULL);

	CU_ADD_TEST(suite, test_parse_literal);
	CU_ADD_TEST(suite, test_parse_string_simple);
	CU_ADD_TEST(suite, test_parse_string_control_chars);
	CU_ADD_TEST(suite, test_parse_string_utf8);
	CU_ADD_TEST(suite, test_parse_string_escapes_twochar);
	CU_ADD_TEST(suite, test_parse_string_escapes_unicode);
	CU_ADD_TEST(suite, test_parse_number);
	CU_ADD_TEST(suite, test_parse_array);
	CU_ADD_TEST(suite, test_parse_object);
	CU_ADD_TEST(suite, test_parse_nesting);
	CU_ADD_TEST(suite, test_parse_comment);

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
