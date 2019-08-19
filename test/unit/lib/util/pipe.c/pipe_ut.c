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

#include "util/pipe.c"
#include "common/lib/test_env.c"

static void
test_create_destroy(void)
{
	struct spdk_pipe *pipe;
	uint8_t mem[64];

	pipe = spdk_pipe_create(mem, sizeof(mem));
	SPDK_CU_ASSERT_FATAL(pipe != NULL);

	spdk_pipe_destroy(pipe);
}

static void
test_write_get_buffer(void)
{
	struct spdk_pipe *pipe;
	uint8_t mem[64];
	struct iovec iovs[2];
	int rc;

	pipe = spdk_pipe_create(mem, sizeof(mem));
	SPDK_CU_ASSERT_FATAL(pipe != NULL);

	/* Get half the available memory. */
	rc = spdk_pipe_writer_get_buffer(pipe, 32, iovs);
	CU_ASSERT(rc == 32);
	CU_ASSERT(iovs[0].iov_base == mem);
	CU_ASSERT(iovs[0].iov_len == 32);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 0);
	CU_ASSERT(pipe->read == 0);

	memset(iovs, 0, sizeof(iovs));

	/* Get 0 bytes. */
	rc = spdk_pipe_writer_get_buffer(pipe, 0, iovs);
	CU_ASSERT(rc == 0);
	CU_ASSERT(iovs[0].iov_base == NULL);
	CU_ASSERT(iovs[0].iov_len == 0);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 0);
	CU_ASSERT(pipe->read == 0);

	memset(iovs, 0, sizeof(iovs));

	/* Get all available memory */
	rc = spdk_pipe_writer_get_buffer(pipe, 64, iovs);
	CU_ASSERT(rc == 64);
	CU_ASSERT(iovs[0].iov_base == mem);
	CU_ASSERT(iovs[0].iov_len == 64);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 0);
	CU_ASSERT(pipe->read == 0);

	memset(iovs, 0, sizeof(iovs));

	/* Get more bytes than exist */
	rc = spdk_pipe_writer_get_buffer(pipe, 65, iovs);
	CU_ASSERT(rc == 64);
	CU_ASSERT(iovs[0].iov_base == mem);
	CU_ASSERT(iovs[0].iov_len == 64);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 0);
	CU_ASSERT(pipe->read == 0);

	memset(iovs, 0, sizeof(iovs));

	/* Advance the write pointer 40 bytes in. */
	pipe->write = 40;

	/* Get all of the available memory. */
	rc = spdk_pipe_writer_get_buffer(pipe, 24, iovs);
	CU_ASSERT(rc == 24);
	CU_ASSERT(iovs[0].iov_base == (mem + 40));
	CU_ASSERT(iovs[0].iov_len == 24);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 40);
	CU_ASSERT(pipe->read == 0);

	memset(iovs, 0, sizeof(iovs));

	/* Get more than the available memory */
	rc = spdk_pipe_writer_get_buffer(pipe, 25, iovs);
	CU_ASSERT(rc == 24);
	CU_ASSERT(iovs[0].iov_base == (mem + 40));
	CU_ASSERT(iovs[0].iov_len == 24);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 40);
	CU_ASSERT(pipe->read == 0);

	memset(iovs, 0, sizeof(iovs));

	/* Advance the read pointer 10 bytes in. */
	pipe->read = 10;

	/* Get all of the available memory. */
	rc = spdk_pipe_writer_get_buffer(pipe, 34, iovs);
	CU_ASSERT(rc == 34);
	CU_ASSERT(iovs[0].iov_base == (mem + 40));
	CU_ASSERT(iovs[0].iov_len == 24);
	CU_ASSERT(iovs[1].iov_base == mem);
	CU_ASSERT(iovs[1].iov_len == 10);
	CU_ASSERT(pipe->write == 40);
	CU_ASSERT(pipe->read == 10);

	memset(iovs, 0, sizeof(iovs));

	/* Get more than the available memory */
	rc = spdk_pipe_writer_get_buffer(pipe, 35, iovs);
	CU_ASSERT(rc == 34);
	CU_ASSERT(iovs[0].iov_base == (mem + 40));
	CU_ASSERT(iovs[0].iov_len == 24);
	CU_ASSERT(iovs[1].iov_base == mem);
	CU_ASSERT(iovs[1].iov_len == 10);
	CU_ASSERT(pipe->write == 40);
	CU_ASSERT(pipe->read == 10);

	memset(iovs, 0, sizeof(iovs));

	/* Advance the read pointer past the write pointer */
	pipe->read = 50;

	/* Get all of the available memory. */
	rc = spdk_pipe_writer_get_buffer(pipe, 10, iovs);
	CU_ASSERT(rc == 10);
	CU_ASSERT(iovs[0].iov_base == (mem + 40));
	CU_ASSERT(iovs[0].iov_len == 10);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 40);
	CU_ASSERT(pipe->read == 50);

	memset(iovs, 0, sizeof(iovs));

	/* Get more than the available memory */
	rc = spdk_pipe_writer_get_buffer(pipe, 11, iovs);
	CU_ASSERT(rc == 10);
	CU_ASSERT(iovs[0].iov_base == (mem + 40));
	CU_ASSERT(iovs[0].iov_len == 10);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 40);
	CU_ASSERT(pipe->read == 50);

	memset(iovs, 0, sizeof(iovs));

	spdk_pipe_destroy(pipe);
}

static void
test_write_advance(void)
{
	struct spdk_pipe *pipe;
	uint8_t mem[64];
	int rc;

	pipe = spdk_pipe_create(mem, sizeof(mem));
	SPDK_CU_ASSERT_FATAL(pipe != NULL);

	/* Advance half way through the pipe */
	rc = spdk_pipe_writer_advance(pipe, 32);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pipe->write == 32);

	pipe->write = 0;

	/* Advance to the end of the pipe */
	rc = spdk_pipe_writer_advance(pipe, 64);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pipe->write == 64);

	pipe->write = 0;

	/* Advance beyond the end */
	rc = spdk_pipe_writer_advance(pipe, 65);
	CU_ASSERT(rc == -EINVAL);

	/* Move the write pointer forward */
	pipe->write = 30;

	/* Advance to the end of the pipe */
	rc = spdk_pipe_writer_advance(pipe, 34);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pipe->write == 64);

	pipe->write = 30;

	/* Advance beyond the end */
	rc = spdk_pipe_writer_advance(pipe, 35);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(pipe->write == 30);

	/* Invert the write and read pointers to test wrap around */
	pipe->write = 30;
	pipe->read = 10;

	/* Advance to the end of the pipe */
	rc = spdk_pipe_writer_advance(pipe, 44);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pipe->write == 10);

	pipe->write = 30;
	pipe->read = 10;

	/* Advance beyond the end */
	rc = spdk_pipe_writer_advance(pipe, 45);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(pipe->write == 30);

	spdk_pipe_destroy(pipe);
}

static void
test_read_get_buffer(void)
{
	struct spdk_pipe *pipe;
	uint8_t mem[64];
	struct iovec iovs[2];
	int rc;

	pipe = spdk_pipe_create(mem, sizeof(mem));
	SPDK_CU_ASSERT_FATAL(pipe != NULL);

	/* Set the write pointer to the end, making all data available. */
	pipe->write = 64;

	/* Get half the available memory. */
	rc = spdk_pipe_reader_get_buffer(pipe, 32, iovs);
	CU_ASSERT(rc == 32);
	CU_ASSERT(iovs[0].iov_base == mem);
	CU_ASSERT(iovs[0].iov_len == 32);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 64);
	CU_ASSERT(pipe->read == 0);

	memset(iovs, 0, sizeof(iovs));

	/* Get 0 bytes. */
	rc = spdk_pipe_reader_get_buffer(pipe, 0, iovs);
	CU_ASSERT(rc == 0);
	CU_ASSERT(iovs[0].iov_base == NULL);
	CU_ASSERT(iovs[0].iov_len == 0);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 64);
	CU_ASSERT(pipe->read == 0);

	memset(iovs, 0, sizeof(iovs));

	/* Get all available memory */
	rc = spdk_pipe_reader_get_buffer(pipe, 64, iovs);
	CU_ASSERT(rc == 64);
	CU_ASSERT(iovs[0].iov_base == mem);
	CU_ASSERT(iovs[0].iov_len == 64);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 64);
	CU_ASSERT(pipe->read == 0);

	memset(iovs, 0, sizeof(iovs));

	/* Get more bytes than exist */
	rc = spdk_pipe_reader_get_buffer(pipe, 65, iovs);
	CU_ASSERT(rc == 64);
	CU_ASSERT(iovs[0].iov_base == mem);
	CU_ASSERT(iovs[0].iov_len == 64);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 64);
	CU_ASSERT(pipe->read == 0);

	memset(iovs, 0, sizeof(iovs));

	/* Advance the read pointer 40 bytes in. */
	pipe->read = 40;

	/* Get all of the available memory. */
	rc = spdk_pipe_reader_get_buffer(pipe, 24, iovs);
	CU_ASSERT(rc == 24);
	CU_ASSERT(iovs[0].iov_base == (mem + 40));
	CU_ASSERT(iovs[0].iov_len == 24);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 64);
	CU_ASSERT(pipe->read == 40);

	memset(iovs, 0, sizeof(iovs));

	/* Get more than the available memory */
	rc = spdk_pipe_reader_get_buffer(pipe, 25, iovs);
	CU_ASSERT(rc == 24);
	CU_ASSERT(iovs[0].iov_base == (mem + 40));
	CU_ASSERT(iovs[0].iov_len == 24);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 64);
	CU_ASSERT(pipe->read == 40);

	memset(iovs, 0, sizeof(iovs));

	/* Advance the write pointer 10 more bytes */
	pipe->write = 10;

	/* Get all of the available memory. */
	rc = spdk_pipe_reader_get_buffer(pipe, 34, iovs);
	CU_ASSERT(rc == 34);
	CU_ASSERT(iovs[0].iov_base == (mem + 40));
	CU_ASSERT(iovs[0].iov_len == 24);
	CU_ASSERT(iovs[1].iov_base == mem);
	CU_ASSERT(iovs[1].iov_len == 10);
	CU_ASSERT(pipe->write == 10);
	CU_ASSERT(pipe->read == 40);

	memset(iovs, 0, sizeof(iovs));

	/* Get more than the available memory */
	rc = spdk_pipe_reader_get_buffer(pipe, 35, iovs);
	CU_ASSERT(rc == 34);
	CU_ASSERT(iovs[0].iov_base == (mem + 40));
	CU_ASSERT(iovs[0].iov_len == 24);
	CU_ASSERT(iovs[1].iov_base == mem);
	CU_ASSERT(iovs[1].iov_len == 10);
	CU_ASSERT(pipe->write == 10);
	CU_ASSERT(pipe->read == 40);

	memset(iovs, 0, sizeof(iovs));

	/* Advance the write pointer past the read pointer */
	pipe->write = 50;

	/* Get all of the available memory. */
	rc = spdk_pipe_reader_get_buffer(pipe, 10, iovs);
	CU_ASSERT(rc == 10);
	CU_ASSERT(iovs[0].iov_base == (mem + 40));
	CU_ASSERT(iovs[0].iov_len == 10);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 50);
	CU_ASSERT(pipe->read == 40);

	memset(iovs, 0, sizeof(iovs));

	/* Get more than the available memory */
	rc = spdk_pipe_reader_get_buffer(pipe, 11, iovs);
	CU_ASSERT(rc == 10);
	CU_ASSERT(iovs[0].iov_base == (mem + 40));
	CU_ASSERT(iovs[0].iov_len == 10);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 50);
	CU_ASSERT(pipe->read == 40);

	memset(iovs, 0, sizeof(iovs));

	spdk_pipe_destroy(pipe);
}

static void
test_read_advance(void)
{
	struct spdk_pipe *pipe;
	uint8_t mem[64];
	int rc;

	pipe = spdk_pipe_create(mem, sizeof(mem));
	SPDK_CU_ASSERT_FATAL(pipe != NULL);

	/* Set the write pointer to the end, making all data available. */
	pipe->write = 64;

	/* Advance half way through the pipe */
	rc = spdk_pipe_reader_advance(pipe, 32);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pipe->read == 32);

	pipe->read = 0;

	/* Advance to the end of the pipe */
	rc = spdk_pipe_reader_advance(pipe, 64);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pipe->read == 64);

	pipe->read = 0;

	/* Advance beyond the end */
	rc = spdk_pipe_reader_advance(pipe, 65);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(pipe->read == 0);

	/* Move the read pointer forward */
	pipe->read = 30;

	/* Advance to the end of the pipe */
	rc = spdk_pipe_reader_advance(pipe, 34);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pipe->read == 64);

	pipe->read = 30;

	/* Advance beyond the end */
	rc = spdk_pipe_reader_advance(pipe, 35);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(pipe->read == 30);

	/* Invert the write and read pointers to test wrap around */
	pipe->write = 10;
	pipe->read = 30;

	/* Advance to the end of the pipe */
	rc = spdk_pipe_reader_advance(pipe, 44);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pipe->read == 10);

	pipe->write = 10;
	pipe->read = 30;

	/* Advance beyond the end */
	rc = spdk_pipe_reader_advance(pipe, 45);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(pipe->read == 30);

	spdk_pipe_destroy(pipe);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("pipe", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test_create_destroy", test_create_destroy) == NULL ||
		CU_add_test(suite, "test_write_get_buffer", test_write_get_buffer) == NULL ||
		CU_add_test(suite, "test_write_advance", test_write_advance) == NULL ||
		CU_add_test(suite, "test_read_get_buffer", test_read_get_buffer) == NULL ||
		CU_add_test(suite, "test_read_advance", test_read_advance) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
