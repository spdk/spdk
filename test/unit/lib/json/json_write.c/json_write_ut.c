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

#include "json/json_write.c"
#include "json/json_parse.c"

#include "spdk/util.h"

static uint8_t g_buf[1000];
static uint8_t *g_write_pos;

static int
write_cb(void *cb_ctx, const void *data, size_t size)
{
	size_t buf_free = g_buf + sizeof(g_buf) - g_write_pos;

	if (size > buf_free) {
		return -1;
	}

	memcpy(g_write_pos, data, size);
	g_write_pos += size;

	return 0;
}

#define BEGIN() \
	memset(g_buf, 0, sizeof(g_buf)); \
	g_write_pos = g_buf; \
	w = spdk_json_write_begin(write_cb, NULL, 0); \
	SPDK_CU_ASSERT_FATAL(w != NULL)

#define END(json) \
	CU_ASSERT(spdk_json_write_end(w) == 0); \
	CU_ASSERT(g_write_pos - g_buf == sizeof(json) - 1); \
	CU_ASSERT(memcmp(json, g_buf, sizeof(json) - 1) == 0)

#define END_SIZE(val, size) \
	CU_ASSERT(spdk_json_write_end(w) == 0); \
	CU_ASSERT(g_write_pos - g_buf == size); \
	CU_ASSERT(memcmp(val, g_buf, size) == 0)

#define END_NOCMP() \
	CU_ASSERT(spdk_json_write_end(w) == 0)

#define END_FAIL() \
	CU_ASSERT(spdk_json_write_end(w) < 0)

#define VAL_STRING(str) \
	CU_ASSERT(spdk_json_write_string_raw(w, str, sizeof(str) - 1) == 0)

#define VAL_STRING_FAIL(str) \
	CU_ASSERT(spdk_json_write_string_raw(w, str, sizeof(str) - 1) < 0)

#define STR_PASS(in, out) \
	BEGIN(); VAL_STRING(in); END("\"" out "\"")

#define STR_FAIL(in) \
	BEGIN(); VAL_STRING_FAIL(in); END_FAIL()

#define VAL_STRING_UTF16LE(str) \
	CU_ASSERT(spdk_json_write_string_utf16le_raw(w, (const uint16_t *)str, sizeof(str) / sizeof(uint16_t) - 1) == 0)

#define VAL_STRING_UTF16LE_FAIL(str) \
	CU_ASSERT(spdk_json_write_string_utf16le_raw(w, (const uint16_t *)str, sizeof(str) / sizeof(uint16_t) - 1) < 0)

#define STR_UTF16LE_PASS(in, out) \
	BEGIN(); VAL_STRING_UTF16LE(in); END("\"" out "\"")

#define STR_UTF16LE_FAIL(in) \
	BEGIN(); VAL_STRING_UTF16LE_FAIL(in); END_FAIL()

#define VAL_NAME(name) \
	CU_ASSERT(spdk_json_write_name_raw(w, name, sizeof(name) - 1) == 0)

#define VAL_NULL() CU_ASSERT(spdk_json_write_null(w) == 0)
#define VAL_TRUE() CU_ASSERT(spdk_json_write_bool(w, true) == 0)
#define VAL_FALSE() CU_ASSERT(spdk_json_write_bool(w, false) == 0)

#define VAL_INT32(i) CU_ASSERT(spdk_json_write_int32(w, i) == 0);
#define VAL_UINT32(u) CU_ASSERT(spdk_json_write_uint32(w, u) == 0);

#define VAL_INT64(i) CU_ASSERT(spdk_json_write_int64(w, i) == 0);
#define VAL_UINT64(u) CU_ASSERT(spdk_json_write_uint64(w, u) == 0);

#define VAL_UINT128(low, high) \
	CU_ASSERT(spdk_json_write_uint128(w, low, high) == 0);
#define VAL_NAME_UINT128(name, low, high) \
	CU_ASSERT(spdk_json_write_named_uint128(w, name, low, high) == 0);

#define VAL_ARRAY_BEGIN() CU_ASSERT(spdk_json_write_array_begin(w) == 0)
#define VAL_ARRAY_END() CU_ASSERT(spdk_json_write_array_end(w) == 0)

#define VAL_OBJECT_BEGIN() CU_ASSERT(spdk_json_write_object_begin(w) == 0)
#define VAL_OBJECT_END() CU_ASSERT(spdk_json_write_object_end(w) == 0)

#define VAL(v) CU_ASSERT(spdk_json_write_val(w, v) == 0)

static void
test_write_literal(void)
{
	struct spdk_json_write_ctx *w;

	BEGIN();
	VAL_NULL();
	END("null");

	BEGIN();
	VAL_TRUE();
	END("true");

	BEGIN();
	VAL_FALSE();
	END("false");
}

static void
test_write_string_simple(void)
{
	struct spdk_json_write_ctx *w;

	STR_PASS("hello world", "hello world");
	STR_PASS(" ", " ");
	STR_PASS("~", "~");
}

static void
test_write_string_escapes(void)
{
	struct spdk_json_write_ctx *w;

	/* Two-character escapes */
	STR_PASS("\b", "\\b");
	STR_PASS("\f", "\\f");
	STR_PASS("\n", "\\n");
	STR_PASS("\r", "\\r");
	STR_PASS("\t", "\\t");
	STR_PASS("\"", "\\\"");
	STR_PASS("\\", "\\\\");

	/* JSON defines an escape for forward slash, but it is optional */
	STR_PASS("/", "/");

	STR_PASS("hello\nworld", "hello\\nworld");

	STR_PASS("\x00", "\\u0000");
	STR_PASS("\x01", "\\u0001");
	STR_PASS("\x02", "\\u0002");

	STR_PASS("\xC3\xB6", "\\u00F6");
	STR_PASS("\xE2\x88\x9A", "\\u221A");
	STR_PASS("\xEA\xAA\xAA", "\\uAAAA");

	/* Surrogate pairs */
	STR_PASS("\xF0\x9D\x84\x9E", "\\uD834\\uDD1E");
	STR_PASS("\xF0\xA0\x9C\x8E", "\\uD841\\uDF0E");

	/* Examples from RFC 3629 */
	STR_PASS("\x41\xE2\x89\xA2\xCE\x91\x2E", "A\\u2262\\u0391.");
	STR_PASS("\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4", "\\uD55C\\uAD6D\\uC5B4");
	STR_PASS("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "\\u65E5\\u672C\\u8A9E");
	STR_PASS("\xEF\xBB\xBF\xF0\xA3\x8E\xB4", "\\uFEFF\\uD84C\\uDFB4");

	/* UTF-8 edge cases */
	STR_PASS("\x7F", "\\u007F");
	STR_FAIL("\x80");
	STR_FAIL("\xC1");
	STR_FAIL("\xC2");
	STR_PASS("\xC2\x80", "\\u0080");
	STR_PASS("\xC2\xBF", "\\u00BF");
	STR_PASS("\xDF\x80", "\\u07C0");
	STR_PASS("\xDF\xBF", "\\u07FF");
	STR_FAIL("\xDF");
	STR_FAIL("\xE0\x80");
	STR_FAIL("\xE0\x1F");
	STR_FAIL("\xE0\x1F\x80");
	STR_FAIL("\xE0");
	STR_FAIL("\xE0\xA0");
	STR_PASS("\xE0\xA0\x80", "\\u0800");
	STR_PASS("\xE0\xA0\xBF", "\\u083F");
	STR_FAIL("\xE0\xA0\xC0");
	STR_PASS("\xE0\xBF\x80", "\\u0FC0");
	STR_PASS("\xE0\xBF\xBF", "\\u0FFF");
	STR_FAIL("\xE0\xC0\x80");
	STR_FAIL("\xE1");
	STR_FAIL("\xE1\x80");
	STR_FAIL("\xE1\x7F\x80");
	STR_FAIL("\xE1\x80\x7F");
	STR_PASS("\xE1\x80\x80", "\\u1000");
	STR_PASS("\xE1\x80\xBF", "\\u103F");
	STR_PASS("\xE1\xBF\x80", "\\u1FC0");
	STR_PASS("\xE1\xBF\xBF", "\\u1FFF");
	STR_FAIL("\xE1\xC0\x80");
	STR_FAIL("\xE1\x80\xC0");
	STR_PASS("\xEF\x80\x80", "\\uF000");
	STR_PASS("\xEF\xBF\xBF", "\\uFFFF");
	STR_FAIL("\xF0");
	STR_FAIL("\xF0\x90");
	STR_FAIL("\xF0\x90\x80");
	STR_FAIL("\xF0\x80\x80\x80");
	STR_FAIL("\xF0\x8F\x80\x80");
	STR_PASS("\xF0\x90\x80\x80", "\\uD800\\uDC00");
	STR_PASS("\xF0\x90\x80\xBF", "\\uD800\\uDC3F");
	STR_PASS("\xF0\x90\xBF\x80", "\\uD803\\uDFC0");
	STR_PASS("\xF0\xBF\x80\x80", "\\uD8BC\\uDC00");
	STR_FAIL("\xF0\xC0\x80\x80");
	STR_FAIL("\xF1");
	STR_FAIL("\xF1\x80");
	STR_FAIL("\xF1\x80\x80");
	STR_FAIL("\xF1\x80\x80\x7F");
	STR_PASS("\xF1\x80\x80\x80", "\\uD8C0\\uDC00");
	STR_PASS("\xF1\x80\x80\xBF", "\\uD8C0\\uDC3F");
	STR_PASS("\xF1\x80\xBF\x80", "\\uD8C3\\uDFC0");
	STR_PASS("\xF1\xBF\x80\x80", "\\uD9BC\\uDC00");
	STR_PASS("\xF3\x80\x80\x80", "\\uDAC0\\uDC00");
	STR_FAIL("\xF3\xC0\x80\x80");
	STR_FAIL("\xF3\x80\xC0\x80");
	STR_FAIL("\xF3\x80\x80\xC0");
	STR_FAIL("\xF4");
	STR_FAIL("\xF4\x80");
	STR_FAIL("\xF4\x80\x80");
	STR_PASS("\xF4\x80\x80\x80", "\\uDBC0\\uDC00");
	STR_PASS("\xF4\x8F\x80\x80", "\\uDBFC\\uDC00");
	STR_PASS("\xF4\x8F\xBF\xBF", "\\uDBFF\\uDFFF");
	STR_FAIL("\xF4\x90\x80\x80");
	STR_FAIL("\xF5");
	STR_FAIL("\xF5\x80");
	STR_FAIL("\xF5\x80\x80");
	STR_FAIL("\xF5\x80\x80\x80");
	STR_FAIL("\xF5\x80\x80\x80\x80");

	/* Overlong encodings */
	STR_FAIL("\xC0\x80");

	/* Surrogate pairs */
	STR_FAIL("\xED\xA0\x80"); /* U+D800 First high surrogate */
	STR_FAIL("\xED\xAF\xBF"); /* U+DBFF Last high surrogate */
	STR_FAIL("\xED\xB0\x80"); /* U+DC00 First low surrogate */
	STR_FAIL("\xED\xBF\xBF"); /* U+DFFF Last low surrogate */
	STR_FAIL("\xED\xA1\x8C\xED\xBE\xB4"); /* U+233B4 (invalid surrogate pair encoding) */
}

static void
test_write_string_utf16le(void)
{
	struct spdk_json_write_ctx *w;

	/* All characters in BMP */
	STR_UTF16LE_PASS(((uint8_t[]) {
		'H', 0, 'e', 0, 'l', 0, 'l', 0, 'o', 0, 0x15, 0xFE, 0, 0
	}), "Hello\\uFE15");

	/* Surrogate pair */
	STR_UTF16LE_PASS(((uint8_t[]) {
		'H', 0, 'i', 0,  0x34, 0xD8, 0x1E, 0xDD, '!', 0, 0, 0
	}), "Hi\\uD834\\uDD1E!");

	/* Valid high surrogate, but no low surrogate */
	STR_UTF16LE_FAIL(((uint8_t[]) {
		0x00, 0xD8, 0, 0 /* U+D800 */
	}));

	/* Invalid leading low surrogate */
	STR_UTF16LE_FAIL(((uint8_t[]) {
		0x00, 0xDC, 0x00, 0xDC, 0, 0 /* U+DC00 U+DC00 */
	}));

	/* Valid high surrogate followed by another high surrogate (invalid) */
	STR_UTF16LE_FAIL(((uint8_t[]) {
		0x00, 0xD8, 0x00, 0xD8, 0, 0 /* U+D800 U+D800 */
	}));
}

static void
test_write_number_int32(void)
{
	struct spdk_json_write_ctx *w;

	BEGIN();
	VAL_INT32(0);
	END("0");

	BEGIN();
	VAL_INT32(1);
	END("1");

	BEGIN();
	VAL_INT32(123);
	END("123");

	BEGIN();
	VAL_INT32(-123);
	END("-123");

	BEGIN();
	VAL_INT32(2147483647);
	END("2147483647");

	BEGIN();
	VAL_INT32(-2147483648);
	END("-2147483648");
}

static void
test_write_number_uint32(void)
{
	struct spdk_json_write_ctx *w;

	BEGIN();
	VAL_UINT32(0);
	END("0");

	BEGIN();
	VAL_UINT32(1);
	END("1");

	BEGIN();
	VAL_UINT32(123);
	END("123");

	BEGIN();
	VAL_UINT32(2147483647);
	END("2147483647");

	BEGIN();
	VAL_UINT32(4294967295);
	END("4294967295");
}

static int
test_generate_string_uint128(char *buf, int buf_size, uint64_t low, uint64_t high)
{
	char tmp_buf[256] = {0};
	unsigned __int128 total;
	uint64_t seg;
	int count = 0;

	memset(buf, 0, buf_size);
	total = ((unsigned __int128)high << 64) + (unsigned __int128)low;
	while (total) {
		/* Use the different calculation to get the 128bits decimal value in UT */
		seg = total % 1000000000000000;
		total = total / 1000000000000000;
		if (total) {
			snprintf(tmp_buf, buf_size, "%015" PRIu64 "%s", seg, buf);
		} else {
			snprintf(tmp_buf, buf_size, "%" PRIu64 "%s", seg, buf);
		}

		count = snprintf(buf, buf_size, "%s", tmp_buf);
	}

	return count;
}

static int
test_generate_string_name_uint128(char *name, char *buf, int buf_size, uint64_t low, uint64_t high)
{
	char tmp_buf[256] = {0};
	int count = test_generate_string_uint128(buf, buf_size, low, high);

	memcpy(tmp_buf, buf, buf_size);
	count = snprintf(buf, 256, "\"%s\":%s", name, tmp_buf);

	return count;
}

static void
test_write_number_uint128(void)
{
	struct spdk_json_write_ctx *w;
	char buf[256] = {0};
	int used_count = 0;

	BEGIN();
	VAL_UINT128(0, 0);
	END("0");

	BEGIN();
	VAL_UINT128(1, 0);
	used_count = test_generate_string_uint128(buf, sizeof(buf), 1, 0);
	END_SIZE(buf, used_count);

	BEGIN();
	VAL_UINT128(123, 0);
	used_count = test_generate_string_uint128(buf, sizeof(buf), 123, 0);
	END_SIZE(buf, used_count);

	BEGIN();
	VAL_UINT128(2147483647, 0);
	used_count = test_generate_string_uint128(buf, sizeof(buf), 2147483647, 0);
	END_SIZE(buf, used_count);

	BEGIN();
	VAL_UINT128(0, 1);
	used_count = test_generate_string_uint128(buf, sizeof(buf), 0, 1);
	END_SIZE(buf, used_count);

	BEGIN();
	VAL_UINT128(4294967295, 1);
	used_count = test_generate_string_uint128(buf, sizeof(buf), 4294967295, 1);
	END_SIZE(buf, used_count);

	BEGIN();
	VAL_UINT128(2147483647, 4294967295);
	used_count = test_generate_string_uint128(buf, sizeof(buf), 2147483647, 4294967295);
	END_SIZE(buf, used_count);

	BEGIN();
	VAL_UINT128(4294967295, 4294967295);
	used_count = test_generate_string_uint128(buf, sizeof(buf), 4294967295, 4294967295);
	END_SIZE(buf, used_count);
}

static void
test_write_string_number_uint128(void)
{
	struct spdk_json_write_ctx *w;
	char buf[256] = {0};
	int used_count = 0;

	BEGIN();
	VAL_NAME_UINT128("case1", 0, 0);
	END("\"case1\":0");

	BEGIN();
	VAL_NAME_UINT128("case2", 1, 0);
	used_count = test_generate_string_name_uint128("case2", buf, sizeof(buf), 1, 0);
	END_SIZE(buf, used_count);

	BEGIN();
	VAL_NAME_UINT128("case3", 123, 0);
	used_count = test_generate_string_name_uint128("case3", buf, sizeof(buf), 123, 0);
	END_SIZE(buf, used_count);

	BEGIN();
	VAL_NAME_UINT128("case4", 2147483647, 0);
	used_count = test_generate_string_name_uint128("case4", buf, sizeof(buf), 2147483647, 0);
	END_SIZE(buf, used_count);

	BEGIN();
	VAL_NAME_UINT128("case5", 0, 1);
	used_count = test_generate_string_name_uint128("case5", buf, sizeof(buf), 0, 1);
	END_SIZE(buf, used_count);

	BEGIN();
	VAL_NAME_UINT128("case6", 4294967295, 1);
	used_count = test_generate_string_name_uint128("case6", buf, sizeof(buf), 4294967295, 1);
	END_SIZE(buf, used_count);

	BEGIN();
	VAL_NAME_UINT128("case7", 2147483647, 4294967295);
	used_count = test_generate_string_name_uint128("case7", buf, sizeof(buf), 2147483647, 4294967295);
	END_SIZE(buf, used_count);

	BEGIN();
	VAL_NAME_UINT128("case8", 4294967295, 4294967295);
	used_count = test_generate_string_name_uint128("case8", buf, sizeof(buf), 4294967295, 4294967295);
	END_SIZE(buf, used_count);
}

static void
test_write_number_int64(void)
{
	struct spdk_json_write_ctx *w;

	BEGIN();
	VAL_INT64(0);
	END("0");

	BEGIN();
	VAL_INT64(1);
	END("1");

	BEGIN();
	VAL_INT64(123);
	END("123");

	BEGIN();
	VAL_INT64(-123);
	END("-123");

	BEGIN();
	VAL_INT64(INT64_MAX);
	END("9223372036854775807");

	BEGIN();
	VAL_INT64(INT64_MIN);
	END("-9223372036854775808");
}

static void
test_write_number_uint64(void)
{
	struct spdk_json_write_ctx *w;

	BEGIN();
	VAL_UINT64(0);
	END("0");

	BEGIN();
	VAL_UINT64(1);
	END("1");

	BEGIN();
	VAL_UINT64(123);
	END("123");

	BEGIN();
	VAL_UINT64(INT64_MAX);
	END("9223372036854775807");

	BEGIN();
	VAL_UINT64(UINT64_MAX);
	END("18446744073709551615");
}

static void
test_write_array(void)
{
	struct spdk_json_write_ctx *w;

	BEGIN();
	VAL_ARRAY_BEGIN();
	VAL_ARRAY_END();
	END("[]");

	BEGIN();
	VAL_ARRAY_BEGIN();
	VAL_INT32(0);
	VAL_ARRAY_END();
	END("[0]");

	BEGIN();
	VAL_ARRAY_BEGIN();
	VAL_INT32(0);
	VAL_INT32(1);
	VAL_ARRAY_END();
	END("[0,1]");

	BEGIN();
	VAL_ARRAY_BEGIN();
	VAL_INT32(0);
	VAL_INT32(1);
	VAL_INT32(2);
	VAL_ARRAY_END();
	END("[0,1,2]");

	BEGIN();
	VAL_ARRAY_BEGIN();
	VAL_STRING("a");
	VAL_ARRAY_END();
	END("[\"a\"]");

	BEGIN();
	VAL_ARRAY_BEGIN();
	VAL_STRING("a");
	VAL_STRING("b");
	VAL_ARRAY_END();
	END("[\"a\",\"b\"]");

	BEGIN();
	VAL_ARRAY_BEGIN();
	VAL_STRING("a");
	VAL_STRING("b");
	VAL_STRING("c");
	VAL_ARRAY_END();
	END("[\"a\",\"b\",\"c\"]");

	BEGIN();
	VAL_ARRAY_BEGIN();
	VAL_TRUE();
	VAL_ARRAY_END();
	END("[true]");

	BEGIN();
	VAL_ARRAY_BEGIN();
	VAL_TRUE();
	VAL_FALSE();
	VAL_ARRAY_END();
	END("[true,false]");

	BEGIN();
	VAL_ARRAY_BEGIN();
	VAL_TRUE();
	VAL_FALSE();
	VAL_TRUE();
	VAL_ARRAY_END();
	END("[true,false,true]");
}

static void
test_write_object(void)
{
	struct spdk_json_write_ctx *w;

	BEGIN();
	VAL_OBJECT_BEGIN();
	VAL_OBJECT_END();
	END("{}");

	BEGIN();
	VAL_OBJECT_BEGIN();
	VAL_NAME("a");
	VAL_INT32(0);
	VAL_OBJECT_END();
	END("{\"a\":0}");

	BEGIN();
	VAL_OBJECT_BEGIN();
	VAL_NAME("a");
	VAL_INT32(0);
	VAL_NAME("b");
	VAL_INT32(1);
	VAL_OBJECT_END();
	END("{\"a\":0,\"b\":1}");

	BEGIN();
	VAL_OBJECT_BEGIN();
	VAL_NAME("a");
	VAL_INT32(0);
	VAL_NAME("b");
	VAL_INT32(1);
	VAL_NAME("c");
	VAL_INT32(2);
	VAL_OBJECT_END();
	END("{\"a\":0,\"b\":1,\"c\":2}");
}

static void
test_write_nesting(void)
{
	struct spdk_json_write_ctx *w;

	BEGIN();
	VAL_ARRAY_BEGIN();
	VAL_ARRAY_BEGIN();
	VAL_ARRAY_END();
	VAL_ARRAY_END();
	END("[[]]");

	BEGIN();
	VAL_ARRAY_BEGIN();
	VAL_ARRAY_BEGIN();
	VAL_ARRAY_BEGIN();
	VAL_ARRAY_END();
	VAL_ARRAY_END();
	VAL_ARRAY_END();
	END("[[[]]]");

	BEGIN();
	VAL_ARRAY_BEGIN();
	VAL_INT32(0);
	VAL_ARRAY_BEGIN();
	VAL_ARRAY_END();
	VAL_ARRAY_END();
	END("[0,[]]");

	BEGIN();
	VAL_ARRAY_BEGIN();
	VAL_ARRAY_BEGIN();
	VAL_ARRAY_END();
	VAL_INT32(0);
	VAL_ARRAY_END();
	END("[[],0]");

	BEGIN();
	VAL_ARRAY_BEGIN();
	VAL_INT32(0);
	VAL_ARRAY_BEGIN();
	VAL_INT32(1);
	VAL_ARRAY_END();
	VAL_INT32(2);
	VAL_ARRAY_END();
	END("[0,[1],2]");

	BEGIN();
	VAL_ARRAY_BEGIN();
	VAL_INT32(0);
	VAL_INT32(1);
	VAL_ARRAY_BEGIN();
	VAL_INT32(2);
	VAL_INT32(3);
	VAL_ARRAY_END();
	VAL_INT32(4);
	VAL_INT32(5);
	VAL_ARRAY_END();
	END("[0,1,[2,3],4,5]");

	BEGIN();
	VAL_OBJECT_BEGIN();
	VAL_NAME("a");
	VAL_OBJECT_BEGIN();
	VAL_OBJECT_END();
	VAL_OBJECT_END();
	END("{\"a\":{}}");

	BEGIN();
	VAL_OBJECT_BEGIN();
	VAL_NAME("a");
	VAL_OBJECT_BEGIN();
	VAL_NAME("b");
	VAL_INT32(0);
	VAL_OBJECT_END();
	VAL_OBJECT_END();
	END("{\"a\":{\"b\":0}}");

	BEGIN();
	VAL_OBJECT_BEGIN();
	VAL_NAME("a");
	VAL_ARRAY_BEGIN();
	VAL_INT32(0);
	VAL_ARRAY_END();
	VAL_OBJECT_END();
	END("{\"a\":[0]}");

	BEGIN();
	VAL_ARRAY_BEGIN();
	VAL_OBJECT_BEGIN();
	VAL_NAME("a");
	VAL_INT32(0);
	VAL_OBJECT_END();
	VAL_ARRAY_END();
	END("[{\"a\":0}]");

	BEGIN();
	VAL_ARRAY_BEGIN();
	VAL_OBJECT_BEGIN();
	VAL_NAME("a");
	VAL_OBJECT_BEGIN();
	VAL_NAME("b");
	VAL_ARRAY_BEGIN();
	VAL_OBJECT_BEGIN();
	VAL_NAME("c");
	VAL_INT32(1);
	VAL_OBJECT_END();
	VAL_INT32(2);
	VAL_ARRAY_END();
	VAL_NAME("d");
	VAL_INT32(3);
	VAL_OBJECT_END();
	VAL_NAME("e");
	VAL_INT32(4);
	VAL_OBJECT_END();
	VAL_INT32(5);
	VAL_ARRAY_END();
	END("[{\"a\":{\"b\":[{\"c\":1},2],\"d\":3},\"e\":4},5]");

	/* Examples from RFC 7159 */
	BEGIN();
	VAL_OBJECT_BEGIN();
	VAL_NAME("Image");
	VAL_OBJECT_BEGIN();
	VAL_NAME("Width");
	VAL_INT32(800);
	VAL_NAME("Height");
	VAL_INT32(600);
	VAL_NAME("Title");
	VAL_STRING("View from 15th Floor");
	VAL_NAME("Thumbnail");
	VAL_OBJECT_BEGIN();
	VAL_NAME("Url");
	VAL_STRING("http://www.example.com/image/481989943");
	VAL_NAME("Height");
	VAL_INT32(125);
	VAL_NAME("Width");
	VAL_INT32(100);
	VAL_OBJECT_END();
	VAL_NAME("Animated");
	VAL_FALSE();
	VAL_NAME("IDs");
	VAL_ARRAY_BEGIN();
	VAL_INT32(116);
	VAL_INT32(943);
	VAL_INT32(234);
	VAL_INT32(38793);
	VAL_ARRAY_END();
	VAL_OBJECT_END();
	VAL_OBJECT_END();
	END(
		"{\"Image\":"
		"{"
		"\"Width\":800,"
		"\"Height\":600,"
		"\"Title\":\"View from 15th Floor\","
		"\"Thumbnail\":{"
		"\"Url\":\"http://www.example.com/image/481989943\","
		"\"Height\":125,"
		"\"Width\":100"
		"},"
		"\"Animated\":false,"
		"\"IDs\":[116,943,234,38793]"
		"}"
		"}");
}

/* Round-trip parse and write test */
static void
test_write_val(void)
{
	struct spdk_json_write_ctx *w;
	struct spdk_json_val values[100];
	char src[] = "{\"a\":[1,2,3],\"b\":{\"c\":\"d\"},\"e\":true,\"f\":false,\"g\":null}";

	CU_ASSERT(spdk_json_parse(src, strlen(src), values, SPDK_COUNTOF(values), NULL,
				  SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE) == 19);

	BEGIN();
	VAL(values);
	END("{\"a\":[1,2,3],\"b\":{\"c\":\"d\"},\"e\":true,\"f\":false,\"g\":null}");
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("json", NULL, NULL);

	CU_ADD_TEST(suite, test_write_literal);
	CU_ADD_TEST(suite, test_write_string_simple);
	CU_ADD_TEST(suite, test_write_string_escapes);
	CU_ADD_TEST(suite, test_write_string_utf16le);
	CU_ADD_TEST(suite, test_write_number_int32);
	CU_ADD_TEST(suite, test_write_number_uint32);
	CU_ADD_TEST(suite, test_write_number_uint128);
	CU_ADD_TEST(suite, test_write_string_number_uint128);
	CU_ADD_TEST(suite, test_write_number_int64);
	CU_ADD_TEST(suite, test_write_number_uint64);
	CU_ADD_TEST(suite, test_write_array);
	CU_ADD_TEST(suite, test_write_object);
	CU_ADD_TEST(suite, test_write_nesting);
	CU_ADD_TEST(suite, test_write_val);

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
