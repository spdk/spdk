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
	bool has_prefix;

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

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("string", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test_parse_ip_addr", test_parse_ip_addr) == NULL ||
		CU_add_test(suite, "test_str_chomp", test_str_chomp) == NULL ||
		CU_add_test(suite, "test_parse_capacity", test_parse_capacity) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
