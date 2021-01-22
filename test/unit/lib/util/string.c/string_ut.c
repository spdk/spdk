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

#include "util/string.c"

static void
test_parse_ip_addr(void)
{
	int rc;
	char *host;
	char *port;
	char ip[255];

	/* IPv4 */
	snprintf(ip, 255, "%s", "192.168.0.1");
	rc = spdk_parse_ip_addr(ip, &host, &port);
	CU_ASSERT_EQUAL(rc, 0);
	SPDK_CU_ASSERT_FATAL(host != NULL);
	CU_ASSERT(strcmp(host, "192.168.0.1") == 0);
	CU_ASSERT_EQUAL(strlen(host), 11);
	CU_ASSERT_EQUAL(port, NULL);

	/* IPv4 with port */
	snprintf(ip, 255, "%s", "123.456.789.0:5520");
	rc = spdk_parse_ip_addr(ip, &host, &port);
	CU_ASSERT_EQUAL(rc, 0);
	SPDK_CU_ASSERT_FATAL(host != NULL);
	CU_ASSERT(strcmp(host, "123.456.789.0") == 0);
	CU_ASSERT_EQUAL(strlen(host), 13);
	SPDK_CU_ASSERT_FATAL(port != NULL);
	CU_ASSERT(strcmp(port, "5520") == 0);
	CU_ASSERT_EQUAL(strlen(port), 4);

	/* IPv6 */
	snprintf(ip, 255, "%s", "[2001:db8:85a3:8d3:1319:8a2e:370:7348]");
	rc = spdk_parse_ip_addr(ip, &host, &port);
	CU_ASSERT_EQUAL(rc, 0);
	SPDK_CU_ASSERT_FATAL(host != NULL);
	CU_ASSERT(strcmp(host, "2001:db8:85a3:8d3:1319:8a2e:370:7348") == 0);
	CU_ASSERT_EQUAL(strlen(host), 36);
	CU_ASSERT_EQUAL(port, NULL);

	/* IPv6 with port */
	snprintf(ip, 255, "%s", "[2001:db8:85a3:8d3:1319:8a2e:370:7348]:443");
	rc = spdk_parse_ip_addr(ip, &host, &port);
	CU_ASSERT_EQUAL(rc, 0);
	SPDK_CU_ASSERT_FATAL(host != NULL);
	CU_ASSERT(strcmp(host, "2001:db8:85a3:8d3:1319:8a2e:370:7348") == 0);
	CU_ASSERT_EQUAL(strlen(host), 36);
	SPDK_CU_ASSERT_FATAL(port != NULL);
	CU_ASSERT(strcmp(port, "443") == 0);
	CU_ASSERT_EQUAL(strlen(port), 3);

	/* IPv6 dangling colon */
	snprintf(ip, 255, "%s", "[2001:db8:85a3:8d3:1319:8a2e:370:7348]:");
	rc = spdk_parse_ip_addr(ip, &host, &port);
	CU_ASSERT_EQUAL(rc, 0);
	SPDK_CU_ASSERT_FATAL(host != NULL);
	CU_ASSERT(strcmp(host, "2001:db8:85a3:8d3:1319:8a2e:370:7348") == 0);
	CU_ASSERT_EQUAL(strlen(host), 36);
	CU_ASSERT_EQUAL(port, NULL);
}

static void
test_str_chomp(void)
{
	char s[1024];

	/* One \n newline */
	snprintf(s, sizeof(s), "%s", "hello world\n");
	CU_ASSERT(spdk_str_chomp(s) == 1);
	CU_ASSERT(strcmp(s, "hello world") == 0);

	/* One \r\n newline */
	snprintf(s, sizeof(s), "%s", "hello world\r\n");
	CU_ASSERT(spdk_str_chomp(s) == 2);
	CU_ASSERT(strcmp(s, "hello world") == 0);

	/* No newlines */
	snprintf(s, sizeof(s), "%s", "hello world");
	CU_ASSERT(spdk_str_chomp(s) == 0);
	CU_ASSERT(strcmp(s, "hello world") == 0);

	/* Two newlines */
	snprintf(s, sizeof(s), "%s", "hello world\n\n");
	CU_ASSERT(spdk_str_chomp(s) == 2);
	CU_ASSERT(strcmp(s, "hello world") == 0);

	/* Empty string */
	snprintf(s, sizeof(s), "%s", "");
	CU_ASSERT(spdk_str_chomp(s) == 0);
	CU_ASSERT(strcmp(s, "") == 0);

	/* One-character string with only \n */
	snprintf(s, sizeof(s), "%s", "\n");
	CU_ASSERT(spdk_str_chomp(s) == 1);
	CU_ASSERT(strcmp(s, "") == 0);

	/* One-character string without a newline */
	snprintf(s, sizeof(s), "%s", "a");
	CU_ASSERT(spdk_str_chomp(s) == 0);
	CU_ASSERT(strcmp(s, "a") == 0);
}

static void
test_parse_capacity(void)
{
	char str[128];
	uint64_t cap;
	int rc;
	bool has_prefix = true;

	rc = spdk_parse_capacity("472", &cap, &has_prefix);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cap == 472);
	CU_ASSERT(has_prefix == false);

	snprintf(str, sizeof(str), "%"PRIu64, UINT64_MAX);
	rc = spdk_parse_capacity(str, &cap, &has_prefix);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cap == UINT64_MAX);
	CU_ASSERT(has_prefix == false);

	rc = spdk_parse_capacity("12k", &cap, &has_prefix);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cap == 12 * 1024);
	CU_ASSERT(has_prefix == true);

	rc = spdk_parse_capacity("12K", &cap, &has_prefix);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cap == 12 * 1024);
	CU_ASSERT(has_prefix == true);

	rc = spdk_parse_capacity("12KB", &cap, &has_prefix);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cap == 12 * 1024);
	CU_ASSERT(has_prefix == true);

	rc = spdk_parse_capacity("100M", &cap, &has_prefix);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cap == 100 * 1024 * 1024);
	CU_ASSERT(has_prefix == true);

	rc = spdk_parse_capacity("128M", &cap, &has_prefix);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cap == 128 * 1024 * 1024);
	CU_ASSERT(has_prefix == true);

	rc = spdk_parse_capacity("4G", &cap, &has_prefix);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cap == 4ULL * 1024 * 1024 * 1024);
	CU_ASSERT(has_prefix == true);

	rc = spdk_parse_capacity("100M 512k", &cap, &has_prefix);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cap == 100ULL * 1024 * 1024);

	rc = spdk_parse_capacity("12k8K", &cap, &has_prefix);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cap == 12 * 1024);
	CU_ASSERT(has_prefix == true);

	/* Non-number */
	rc = spdk_parse_capacity("G", &cap, &has_prefix);
	CU_ASSERT(rc != 0);

	rc = spdk_parse_capacity("darsto", &cap, &has_prefix);
	CU_ASSERT(rc != 0);
}

static void
test_sprintf_append_realloc(void)
{
	char *str1, *str2, *str3, *str4;

	/* Test basic functionality. */
	str1 = spdk_sprintf_alloc("hello world\ngood morning\n" \
				  "good afternoon\ngood evening\n");
	SPDK_CU_ASSERT_FATAL(str1 != NULL);

	str2 = spdk_sprintf_append_realloc(NULL, "hello world\n");
	SPDK_CU_ASSERT_FATAL(str2);

	str2 = spdk_sprintf_append_realloc(str2, "good morning\n");
	SPDK_CU_ASSERT_FATAL(str2);

	str2 = spdk_sprintf_append_realloc(str2, "good afternoon\n");
	SPDK_CU_ASSERT_FATAL(str2);

	str2 = spdk_sprintf_append_realloc(str2, "good evening\n");
	SPDK_CU_ASSERT_FATAL(str2);

	CU_ASSERT(strcmp(str1, str2) == 0);

	free(str1);
	free(str2);

	/* Test doubling buffer size. */
	str3 = spdk_sprintf_append_realloc(NULL, "aaaaaaaaaa\n");
	str3 = spdk_sprintf_append_realloc(str3, "bbbbbbbbbb\n");
	str3 = spdk_sprintf_append_realloc(str3, "cccccccccc\n");

	str4 = malloc(33 + 1);
	memset(&str4[0], 'a', 10);
	str4[10] = '\n';
	memset(&str4[11], 'b', 10);
	str4[21] = '\n';
	memset(&str4[22], 'c', 10);
	str4[32] = '\n';
	str4[33] = 0;

	CU_ASSERT(strcmp(str3, str4) == 0);

	free(str3);
	free(str4);
}

static void
generate_string(char *str, size_t len, int64_t limit, int adjust)
{
	/* There isn't a portable way of handling the arithmetic, so */
	/* perform the calculation in two parts to avoid overflow */
	int64_t hi = (limit / 10) + (adjust / 10);
	int64_t lo = (limit % 10) + (adjust % 10);

	/* limit is large and adjust is small, so hi part will be */
	/* non-zero even if there is a carry, but check it */
	CU_ASSERT(hi < -1 || hi > 1);

	/* Correct a difference in sign */
	if ((hi < 0) != (lo < 0) && lo != 0) {
		lo += (hi < 0) ? -10 : 10;
		hi += (hi < 0) ? 1 : -1;
	}

	snprintf(str, len, "%" PRId64 "%01" PRId64, hi + (lo / 10),
		 (lo < 0) ? (-lo % 10) : (lo % 10));
}

static void
test_strtol(void)
{
	long int val;
	char str[256];

	const char *val1 = "no_digits";
	/* digits + chars */
	const char *val8 = "10_is_ten";
	/* chars + digits */
	const char *val9 = "ten_is_10";
	/* all zeroes */
	const char *val10 = "00000000";
	/* leading minus sign, but not negative */
	const char *val11 = "-0";

	val = spdk_strtol(val1, 10);
	CU_ASSERT(val == -EINVAL);

	/* LONG_MIN - 1 */
	generate_string(str, sizeof(str), LONG_MIN, -1);
	val = spdk_strtol(str, 10);
	CU_ASSERT(val == -ERANGE);

	/* LONG_MIN */
	generate_string(str, sizeof(str), LONG_MIN, 0);
	val = spdk_strtol(str, 10);
	CU_ASSERT(val == -ERANGE);

	/* LONG_MIN + 1 */
	generate_string(str, sizeof(str), LONG_MIN, +1);
	val = spdk_strtol(str, 10);
	CU_ASSERT(val == -ERANGE);

	/* LONG_MAX - 1 */
	generate_string(str, sizeof(str), LONG_MAX, -1);
	val = spdk_strtol(str, 10);
	CU_ASSERT(val == LONG_MAX - 1);

	/* LONG_MAX */
	generate_string(str, sizeof(str), LONG_MAX, 0);
	val = spdk_strtol(str, 10);
	CU_ASSERT(val == LONG_MAX);

	/* LONG_MAX + 1 */
	generate_string(str, sizeof(str), LONG_MAX, +1);
	val = spdk_strtol(str, 10);
	CU_ASSERT(val == -ERANGE);

	val = spdk_strtol(val8, 10);
	CU_ASSERT(val == -EINVAL);

	val = spdk_strtol(val9, 10);
	CU_ASSERT(val == -EINVAL);

	val = spdk_strtol(val10, 10);
	CU_ASSERT(val == 0);

	/* Invalid base */
	val = spdk_strtol(val10, 1);
	CU_ASSERT(val == -EINVAL);

	val = spdk_strtol(val11, 10);
	CU_ASSERT(val == 0);
}

static void
test_strtoll(void)
{
	long long int val;
	char str[256];

	const char *val1 = "no_digits";
	/* digits + chars */
	const char *val8 = "10_is_ten";
	/* chars + digits */
	const char *val9 = "ten_is_10";
	/* all zeroes */
	const char *val10 = "00000000";
	/* leading minus sign, but not negative */
	const char *val11 = "-0";

	val = spdk_strtoll(val1, 10);
	CU_ASSERT(val == -EINVAL);

	/* LLONG_MIN - 1 */
	generate_string(str, sizeof(str), LLONG_MIN, -1);
	val = spdk_strtoll(str, 10);
	CU_ASSERT(val == -ERANGE);

	/* LLONG_MIN */
	generate_string(str, sizeof(str), LLONG_MIN, 0);
	val = spdk_strtoll(str, 10);
	CU_ASSERT(val == -ERANGE);

	/* LLONG_MIN + 1 */
	generate_string(str, sizeof(str), LLONG_MIN, +1);
	val = spdk_strtoll(str, 10);
	CU_ASSERT(val == -ERANGE);

	/* LLONG_MAX - 1 */
	generate_string(str, sizeof(str), LLONG_MAX, -1);
	val = spdk_strtoll(str, 10);
	CU_ASSERT(val == LLONG_MAX - 1);

	/* LLONG_MAX */
	generate_string(str, sizeof(str), LLONG_MAX, 0);
	val = spdk_strtoll(str, 10);
	CU_ASSERT(val == LLONG_MAX);

	/* LLONG_MAX + 1 */
	generate_string(str, sizeof(str), LLONG_MAX, +1);
	val = spdk_strtoll(str, 10);
	CU_ASSERT(val == -ERANGE);

	val = spdk_strtoll(val8, 10);
	CU_ASSERT(val == -EINVAL);

	val = spdk_strtoll(val9, 10);
	CU_ASSERT(val == -EINVAL);

	val = spdk_strtoll(val10, 10);
	CU_ASSERT(val == 0);

	/* Invalid base */
	val = spdk_strtoll(val10, 1);
	CU_ASSERT(val == -EINVAL);

	val = spdk_strtoll(val11, 10);
	CU_ASSERT(val == 0);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("string", NULL, NULL);

	CU_ADD_TEST(suite, test_parse_ip_addr);
	CU_ADD_TEST(suite, test_str_chomp);
	CU_ADD_TEST(suite, test_parse_capacity);
	CU_ADD_TEST(suite, test_sprintf_append_realloc);
	CU_ADD_TEST(suite, test_strtol);
	CU_ADD_TEST(suite, test_strtoll);

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
