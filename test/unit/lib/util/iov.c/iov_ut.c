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

#include "util/iov.c"

static int
_check_val(void *buf, size_t len, uint8_t val)
{
	size_t i;
	uint8_t *data = buf;

	for (i = 0; i < len; i++) {
		if (data[i] != val) {
			return -1;
		}
	}

	return 0;
}

static void
test_single_iov(void)
{
	struct iovec siov[1];
	struct iovec diov[1];
	uint8_t sdata[64];
	uint8_t ddata[64];
	ssize_t rc;

	/* Simplest cases- 1 element in each iovec. */

	/* Same size. */
	memset(sdata, 1, sizeof(sdata));
	memset(ddata, 0, sizeof(ddata));
	siov[0].iov_base = sdata;
	siov[0].iov_len = sizeof(sdata);
	diov[0].iov_base = ddata;
	diov[0].iov_len = sizeof(ddata);

	rc = spdk_iovcpy(siov, 1, diov, 1);
	CU_ASSERT(rc == sizeof(sdata));
	CU_ASSERT(_check_val(ddata, 64, 1) == 0);

	/* Source smaller than dest */
	memset(sdata, 1, sizeof(sdata));
	memset(ddata, 0, sizeof(ddata));
	siov[0].iov_base = sdata;
	siov[0].iov_len = 48;
	diov[0].iov_base = ddata;
	diov[0].iov_len = sizeof(ddata);

	rc = spdk_iovcpy(siov, 1, diov, 1);
	CU_ASSERT(rc == 48);
	CU_ASSERT(_check_val(ddata, 48, 1) == 0);
	CU_ASSERT(_check_val(&ddata[48], 16, 0) == 0);

	/* Dest smaller than source */
	memset(sdata, 1, sizeof(sdata));
	memset(ddata, 0, sizeof(ddata));
	siov[0].iov_base = sdata;
	siov[0].iov_len = sizeof(sdata);
	diov[0].iov_base = ddata;
	diov[0].iov_len = 48;

	rc = spdk_iovcpy(siov, 1, diov, 1);
	CU_ASSERT(rc == 48);
	CU_ASSERT(_check_val(ddata, 48, 1) == 0);
	CU_ASSERT(_check_val(&ddata[48], 16, 0) == 0);
}

static void
test_simple_iov(void)
{
	struct iovec siov[4];
	struct iovec diov[4];
	uint8_t sdata[64];
	uint8_t ddata[64];
	ssize_t rc;
	int i;

	/* Simple cases with 4 iov elements */

	/* Same size. */
	memset(sdata, 1, sizeof(sdata));
	memset(ddata, 0, sizeof(ddata));
	for (i = 0; i < 4; i++) {
		siov[i].iov_base = sdata + (16 * i);
		siov[i].iov_len = 16;
		diov[i].iov_base = ddata + (16 * i);
		diov[i].iov_len = 16;
	}

	rc = spdk_iovcpy(siov, 4, diov, 4);
	CU_ASSERT(rc == sizeof(sdata));
	CU_ASSERT(_check_val(ddata, 64, 1) == 0);

	/* Source smaller than dest */
	memset(sdata, 1, sizeof(sdata));
	memset(ddata, 0, sizeof(ddata));
	for (i = 0; i < 4; i++) {
		siov[i].iov_base = sdata + (8 * i);
		siov[i].iov_len = 8;
		diov[i].iov_base = ddata + (16 * i);
		diov[i].iov_len = 16;
	}

	rc = spdk_iovcpy(siov, 4, diov, 4);
	CU_ASSERT(rc == 32);
	CU_ASSERT(_check_val(ddata, 32, 1) == 0);
	CU_ASSERT(_check_val(&ddata[32], 32, 0) == 0);

	/* Dest smaller than source */
	memset(sdata, 1, sizeof(sdata));
	memset(ddata, 0, sizeof(ddata));
	for (i = 0; i < 4; i++) {
		siov[i].iov_base = sdata + (16 * i);
		siov[i].iov_len = 16;
		diov[i].iov_base = ddata + (8 * i);
		diov[i].iov_len = 8;
	}

	rc = spdk_iovcpy(siov, 4, diov, 4);
	CU_ASSERT(rc == 32);
	CU_ASSERT(_check_val(ddata, 32, 1) == 0);
	CU_ASSERT(_check_val(&ddata[32], 32, 0) == 0);
}

static void
test_complex_iov(void)
{
	struct iovec siov[4];
	struct iovec diov[4];
	uint8_t sdata[64];
	uint8_t ddata[64];
	ssize_t rc;
	int i;

	/* More source elements */
	memset(sdata, 1, sizeof(sdata));
	memset(ddata, 0, sizeof(ddata));
	for (i = 0; i < 4; i++) {
		siov[i].iov_base = sdata + (16 * i);
		siov[i].iov_len = 16;
	}
	diov[0].iov_base = ddata;
	diov[0].iov_len = sizeof(ddata);

	rc = spdk_iovcpy(siov, 4, diov, 1);
	CU_ASSERT(rc == sizeof(sdata));
	CU_ASSERT(_check_val(ddata, 64, 1) == 0);

	/* More dest elements */
	memset(sdata, 1, sizeof(sdata));
	memset(ddata, 0, sizeof(ddata));
	for (i = 0; i < 4; i++) {
		diov[i].iov_base = ddata + (16 * i);
		diov[i].iov_len = 16;
	}
	siov[0].iov_base = sdata;
	siov[0].iov_len = sizeof(sdata);

	rc = spdk_iovcpy(siov, 1, diov, 4);
	CU_ASSERT(rc == sizeof(sdata));
	CU_ASSERT(_check_val(ddata, 64, 1) == 0);

	/* Build one by hand that's really terrible */
	memset(sdata, 1, sizeof(sdata));
	memset(ddata, 0, sizeof(ddata));
	siov[0].iov_base = sdata;
	siov[0].iov_len = 1;
	siov[1].iov_base = siov[0].iov_base + siov[0].iov_len;
	siov[1].iov_len = 13;
	siov[2].iov_base = siov[1].iov_base + siov[1].iov_len;
	siov[2].iov_len = 6;
	siov[3].iov_base = siov[2].iov_base + siov[2].iov_len;
	siov[3].iov_len = 44;

	diov[0].iov_base = ddata;
	diov[0].iov_len = 31;
	diov[1].iov_base = diov[0].iov_base + diov[0].iov_len;
	diov[1].iov_len = 9;
	diov[2].iov_base = diov[1].iov_base + diov[1].iov_len;
	diov[2].iov_len = 1;
	diov[3].iov_base = diov[2].iov_base + diov[2].iov_len;
	diov[3].iov_len = 23;

	rc = spdk_iovcpy(siov, 4, diov, 4);
	CU_ASSERT(rc == 64);
	CU_ASSERT(_check_val(ddata, 64, 1) == 0);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("iov", NULL, NULL);

	CU_ADD_TEST(suite, test_single_iov);
	CU_ADD_TEST(suite, test_simple_iov);
	CU_ADD_TEST(suite, test_complex_iov);

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
