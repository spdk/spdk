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
	uint8_t mem[10];

	pipe = spdk_pipe_create(mem, sizeof(mem));
	SPDK_CU_ASSERT_FATAL(pipe != NULL);

	spdk_pipe_destroy(pipe);
}

static void
test_write_get_buffer(void)
{
	struct spdk_pipe *pipe;
	uint8_t mem[10];
	struct iovec iovs[2];
	int rc;

	pipe = spdk_pipe_create(mem, sizeof(mem));
	SPDK_CU_ASSERT_FATAL(pipe != NULL);

	/* Get some available memory. */
	rc = spdk_pipe_writer_get_buffer(pipe, 5, iovs);
	CU_ASSERT(rc == 5);
	CU_ASSERT(iovs[0].iov_base == mem);
	CU_ASSERT(iovs[0].iov_len == 5);
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
	rc = spdk_pipe_writer_get_buffer(pipe, 9, iovs);
	CU_ASSERT(rc == 9);
	CU_ASSERT(iovs[0].iov_base == mem);
	CU_ASSERT(iovs[0].iov_len == 9);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 0);
	CU_ASSERT(pipe->read == 0);

	memset(iovs, 0, sizeof(iovs));

	/* Get the full size of the data buffer backing the pipe, which isn't allowed */
	rc = spdk_pipe_writer_get_buffer(pipe, 10, iovs);
	CU_ASSERT(rc == 9);
	CU_ASSERT(iovs[0].iov_base == mem);
	CU_ASSERT(iovs[0].iov_len == 9);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 0);
	CU_ASSERT(pipe->read == 0);

	memset(iovs, 0, sizeof(iovs));

	/* Advance the write pointer 7 bytes in. */
	pipe->write = 7;

	/* Get all of the available memory. */
	rc = spdk_pipe_writer_get_buffer(pipe, 2, iovs);
	CU_ASSERT(rc == 2);
	CU_ASSERT(iovs[0].iov_base == (mem + 7));
	CU_ASSERT(iovs[0].iov_len == 2);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 7);
	CU_ASSERT(pipe->read == 0);

	memset(iovs, 0, sizeof(iovs));

	/* Get more than the available memory */
	rc = spdk_pipe_writer_get_buffer(pipe, 3, iovs);
	CU_ASSERT(rc == 2);
	CU_ASSERT(iovs[0].iov_base == (mem + 7));
	CU_ASSERT(iovs[0].iov_len == 2);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 7);
	CU_ASSERT(pipe->read == 0);

	memset(iovs, 0, sizeof(iovs));

	/* Advance the read pointer 3 bytes in. */
	pipe->read = 3;

	/* Get all of the available memory. */
	rc = spdk_pipe_writer_get_buffer(pipe, 5, iovs);
	CU_ASSERT(rc == 5);
	CU_ASSERT(iovs[0].iov_base == (mem + 7));
	CU_ASSERT(iovs[0].iov_len == 3);
	CU_ASSERT(iovs[1].iov_base == mem);
	CU_ASSERT(iovs[1].iov_len == 2);
	CU_ASSERT(pipe->write == 7);
	CU_ASSERT(pipe->read == 3);

	memset(iovs, 0, sizeof(iovs));

	/* Get more than the available memory */
	rc = spdk_pipe_writer_get_buffer(pipe, 6, iovs);
	CU_ASSERT(rc == 5);
	CU_ASSERT(iovs[0].iov_base == (mem + 7));
	CU_ASSERT(iovs[0].iov_len == 3);
	CU_ASSERT(iovs[1].iov_base == mem);
	CU_ASSERT(iovs[1].iov_len == 2);
	CU_ASSERT(pipe->write == 7);
	CU_ASSERT(pipe->read == 3);

	memset(iovs, 0, sizeof(iovs));

	/* Advance the read pointer past the write pointer */
	pipe->read = 9;

	/* Get all of the available memory. */
	rc = spdk_pipe_writer_get_buffer(pipe, 1, iovs);
	CU_ASSERT(rc == 1);
	CU_ASSERT(iovs[0].iov_base == (mem + 7));
	CU_ASSERT(iovs[0].iov_len == 1);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 7);
	CU_ASSERT(pipe->read == 9);

	memset(iovs, 0, sizeof(iovs));

	/* Get more than the available memory */
	rc = spdk_pipe_writer_get_buffer(pipe, 2, iovs);
	CU_ASSERT(rc == 1);
	CU_ASSERT(iovs[0].iov_base == (mem + 7));
	CU_ASSERT(iovs[0].iov_len == 1);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 7);
	CU_ASSERT(pipe->read == 9);

	memset(iovs, 0, sizeof(iovs));

	/* Fill the pipe */
	pipe->write = 8;

	/* Get data while the pipe is full */
	rc = spdk_pipe_writer_get_buffer(pipe, 1, iovs);
	CU_ASSERT(rc == 0);
	CU_ASSERT(iovs[0].iov_base == NULL);
	CU_ASSERT(iovs[0].iov_len == 0);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 8);
	CU_ASSERT(pipe->read == 9);

	spdk_pipe_destroy(pipe);
}

static void
test_write_advance(void)
{
	struct spdk_pipe *pipe;
	uint8_t mem[10];
	int rc;

	pipe = spdk_pipe_create(mem, sizeof(mem));
	SPDK_CU_ASSERT_FATAL(pipe != NULL);

	/* Advance half way through the pipe */
	rc = spdk_pipe_writer_advance(pipe, 5);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pipe->write == 5);
	CU_ASSERT(pipe->read == 0);

	pipe->write = 0;

	/* Advance to the end of the pipe */
	rc = spdk_pipe_writer_advance(pipe, 9);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pipe->write == 9);
	CU_ASSERT(pipe->read == 0);

	pipe->write = 0;

	/* Advance beyond the end */
	rc = spdk_pipe_writer_advance(pipe, 10);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(pipe->write == 0);
	CU_ASSERT(pipe->read == 0);

	/* Move the read pointer forward */
	pipe->write = 0;
	pipe->read = 5;

	/* Advance to the end of the pipe */
	rc = spdk_pipe_writer_advance(pipe, 4);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pipe->write == 4);
	CU_ASSERT(pipe->read == 5);

	pipe->write = 0;
	pipe->read = 5;

	/* Advance beyond the end */
	rc = spdk_pipe_writer_advance(pipe, 5);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(pipe->write == 0);
	CU_ASSERT(pipe->read == 5);

	/* Test wrap around */
	pipe->write = 7;
	pipe->read = 3;

	/* Advance to the end of the pipe */
	rc = spdk_pipe_writer_advance(pipe, 5);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pipe->write == 2);
	CU_ASSERT(pipe->read == 3);

	pipe->write = 7;
	pipe->read = 3;

	/* Advance beyond the end */
	rc = spdk_pipe_writer_advance(pipe, 6);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(pipe->write == 7);
	CU_ASSERT(pipe->read == 3);

	spdk_pipe_destroy(pipe);
}

static void
test_read_get_buffer(void)
{
	struct spdk_pipe *pipe;
	uint8_t mem[10];
	struct iovec iovs[2];
	int rc;

	pipe = spdk_pipe_create(mem, sizeof(mem));
	SPDK_CU_ASSERT_FATAL(pipe != NULL);

	/* Set the write pointer to the end, making all data available. */
	pipe->write = 9;

	/* Get half the available memory. */
	rc = spdk_pipe_reader_get_buffer(pipe, 5, iovs);
	CU_ASSERT(rc == 5);
	CU_ASSERT(iovs[0].iov_base == mem);
	CU_ASSERT(iovs[0].iov_len == 5);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 9);
	CU_ASSERT(pipe->read == 0);

	memset(iovs, 0, sizeof(iovs));

	/* Get 0 bytes. */
	rc = spdk_pipe_reader_get_buffer(pipe, 0, iovs);
	CU_ASSERT(rc == 0);
	CU_ASSERT(iovs[0].iov_base == NULL);
	CU_ASSERT(iovs[0].iov_len == 0);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 9);
	CU_ASSERT(pipe->read == 0);

	memset(iovs, 0, sizeof(iovs));

	/* Get all available memory */
	rc = spdk_pipe_reader_get_buffer(pipe, 9, iovs);
	CU_ASSERT(rc == 9);
	CU_ASSERT(iovs[0].iov_base == mem);
	CU_ASSERT(iovs[0].iov_len == 9);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 9);
	CU_ASSERT(pipe->read == 0);

	memset(iovs, 0, sizeof(iovs));

	/* Get more bytes than exist */
	rc = spdk_pipe_reader_get_buffer(pipe, 10, iovs);
	CU_ASSERT(rc == 9);
	CU_ASSERT(iovs[0].iov_base == mem);
	CU_ASSERT(iovs[0].iov_len == 9);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 9);
	CU_ASSERT(pipe->read == 0);

	memset(iovs, 0, sizeof(iovs));

	/* Advance the read pointer 5 bytes in. */
	pipe->read = 5;
	pipe->write = 0;

	/* Get all of the available memory. */
	rc = spdk_pipe_reader_get_buffer(pipe, 5, iovs);
	CU_ASSERT(rc == 5);
	CU_ASSERT(iovs[0].iov_base == (mem + 5));
	CU_ASSERT(iovs[0].iov_len == 5);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 0);
	CU_ASSERT(pipe->read == 5);

	memset(iovs, 0, sizeof(iovs));

	/* Get more than the available memory */
	rc = spdk_pipe_reader_get_buffer(pipe, 6, iovs);
	CU_ASSERT(rc == 5);
	CU_ASSERT(iovs[0].iov_base == (mem + 5));
	CU_ASSERT(iovs[0].iov_len == 5);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 0);
	CU_ASSERT(pipe->read == 5);

	memset(iovs, 0, sizeof(iovs));

	/* Invert the write and read pointers */
	pipe->read = 7;
	pipe->write = 3;

	/* Get all of the available memory. */
	rc = spdk_pipe_reader_get_buffer(pipe, 6, iovs);
	CU_ASSERT(rc == 6);
	CU_ASSERT(iovs[0].iov_base == (mem + 7));
	CU_ASSERT(iovs[0].iov_len == 3);
	CU_ASSERT(iovs[1].iov_base == mem);
	CU_ASSERT(iovs[1].iov_len == 3);
	CU_ASSERT(pipe->write == 3);
	CU_ASSERT(pipe->read == 7);

	memset(iovs, 0, sizeof(iovs));

	/* Get more than the available memory */
	rc = spdk_pipe_reader_get_buffer(pipe, 7, iovs);
	CU_ASSERT(rc == 6);
	CU_ASSERT(iovs[0].iov_base == (mem + 7));
	CU_ASSERT(iovs[0].iov_len == 3);
	CU_ASSERT(iovs[1].iov_base == mem);
	CU_ASSERT(iovs[1].iov_len == 3);
	CU_ASSERT(pipe->write == 3);
	CU_ASSERT(pipe->read == 7);

	memset(iovs, 0, sizeof(iovs));

	/* Empty the pipe */
	pipe->read = 8;
	pipe->write = 8;

	/* Get data while the pipe is empty */
	rc = spdk_pipe_reader_get_buffer(pipe, 1, iovs);
	CU_ASSERT(rc == 0);
	CU_ASSERT(iovs[0].iov_base == NULL);
	CU_ASSERT(iovs[0].iov_len == 0);
	CU_ASSERT(iovs[1].iov_base == NULL);
	CU_ASSERT(iovs[1].iov_len == 0);
	CU_ASSERT(pipe->write == 8);
	CU_ASSERT(pipe->read == 8);

	spdk_pipe_destroy(pipe);
}

static void
test_read_advance(void)
{
	struct spdk_pipe *pipe;
	uint8_t mem[10];
	int rc;

	pipe = spdk_pipe_create(mem, sizeof(mem));
	SPDK_CU_ASSERT_FATAL(pipe != NULL);

	pipe->read = 0;
	pipe->write = 9;

	/* Advance half way through the pipe */
	rc = spdk_pipe_reader_advance(pipe, 5);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pipe->read == 5);
	CU_ASSERT(pipe->write == 9);

	pipe->read = 0;
	pipe->write = 9;

	/* Advance to the end of the pipe */
	rc = spdk_pipe_reader_advance(pipe, 9);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pipe->read == 9);
	CU_ASSERT(pipe->write == 9);

	pipe->read = 0;
	pipe->write = 9;

	/* Advance beyond the end */
	rc = spdk_pipe_reader_advance(pipe, 10);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(pipe->read == 0);
	CU_ASSERT(pipe->write == 9);

	/* Move the write pointer forward */
	pipe->read = 0;
	pipe->write = 5;

	/* Advance to the end of the pipe */
	rc = spdk_pipe_reader_advance(pipe, 5);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pipe->write == 5);
	CU_ASSERT(pipe->read == 5);

	pipe->read = 0;
	pipe->write = 5;

	/* Advance beyond the end */
	rc = spdk_pipe_reader_advance(pipe, 6);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(pipe->read == 0);
	CU_ASSERT(pipe->write == 5);

	/* Test wrap around */
	pipe->read = 7;
	pipe->write = 3;

	/* Advance to the end of the pipe */
	rc = spdk_pipe_reader_advance(pipe, 6);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pipe->read == 3);
	CU_ASSERT(pipe->write == 3);

	pipe->read = 7;
	pipe->write = 3;

	/* Advance beyond the end */
	rc = spdk_pipe_writer_advance(pipe, 7);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(pipe->read == 7);
	CU_ASSERT(pipe->write == 3);

	spdk_pipe_destroy(pipe);
}

static void
test_data(void)
{
	struct spdk_pipe *pipe;
	uint8_t mem[10];
	struct iovec iovs[2];
	uint8_t *data;
	int rc;
	size_t i;

	memset(mem, 0, sizeof(mem));
	memset(iovs, 0, sizeof(iovs));

	pipe = spdk_pipe_create(mem, sizeof(mem));
	SPDK_CU_ASSERT_FATAL(pipe != NULL);

	/* Place 1 byte in the pipe */
	rc = spdk_pipe_writer_get_buffer(pipe, 1, iovs);
	CU_ASSERT(rc == 1);
	CU_ASSERT(iovs[0].iov_base != NULL);
	CU_ASSERT(iovs[0].iov_len == 1);

	memset(iovs[0].iov_base, 'A', 1);

	rc = spdk_pipe_writer_advance(pipe, 1);
	CU_ASSERT(rc == 0);

	CU_ASSERT(mem[0] == 'A');
	CU_ASSERT(mem[1] == 0);
	CU_ASSERT(mem[2] == 0);
	CU_ASSERT(mem[3] == 0);
	CU_ASSERT(mem[4] == 0);
	CU_ASSERT(mem[5] == 0);
	CU_ASSERT(mem[6] == 0);
	CU_ASSERT(mem[7] == 0);
	CU_ASSERT(mem[8] == 0);
	CU_ASSERT(mem[9] == 0);

	memset(iovs, 0, sizeof(iovs));

	/* Get 1 byte from the pipe */
	CU_ASSERT(spdk_pipe_reader_bytes_available(pipe) == 1);
	rc = spdk_pipe_reader_get_buffer(pipe, 10, iovs);
	CU_ASSERT(rc == 1);

	data = iovs[0].iov_base;
	CU_ASSERT(*data = 'A');

	spdk_pipe_reader_advance(pipe, 1);

	/* Put 9 more bytes in the pipe, so every byte has
	 * been written */
	rc = spdk_pipe_writer_get_buffer(pipe, 9, iovs);
	CU_ASSERT(rc == 9);
	CU_ASSERT(iovs[0].iov_len == 9);
	CU_ASSERT(iovs[1].iov_len == 0);

	memset(iovs[0].iov_base, 'B', iovs[0].iov_len);

	rc = spdk_pipe_writer_advance(pipe, 9);
	CU_ASSERT(rc == 0);

	CU_ASSERT(mem[0] == 'A');
	CU_ASSERT(mem[1] == 'B');
	CU_ASSERT(mem[2] == 'B');
	CU_ASSERT(mem[3] == 'B');
	CU_ASSERT(mem[4] == 'B');
	CU_ASSERT(mem[5] == 'B');
	CU_ASSERT(mem[6] == 'B');
	CU_ASSERT(mem[7] == 'B');
	CU_ASSERT(mem[8] == 'B');
	CU_ASSERT(mem[9] == 'B');

	memset(iovs, 0, sizeof(iovs));

	/* Get 7 bytes of the previously written 9. */
	CU_ASSERT(spdk_pipe_reader_bytes_available(pipe) == 9);
	rc = spdk_pipe_reader_get_buffer(pipe, 7, iovs);
	CU_ASSERT(rc == 7);

	CU_ASSERT(iovs[0].iov_len == 7);
	data = iovs[0].iov_base;
	for (i = 0; i < iovs[0].iov_len; i++) {
		CU_ASSERT(data[i] == 'B');
	}

	spdk_pipe_reader_advance(pipe, 7);

	memset(iovs, 0, sizeof(iovs));

	/* Put 1 more byte in the pipe, overwriting the original 'A' */
	rc = spdk_pipe_writer_get_buffer(pipe, 1, iovs);
	CU_ASSERT(rc == 1);
	CU_ASSERT(iovs[0].iov_len == 1);
	CU_ASSERT(iovs[1].iov_len == 0);

	memset(iovs[0].iov_base, 'C', iovs[0].iov_len);

	rc = spdk_pipe_writer_advance(pipe, 1);
	CU_ASSERT(rc == 0);

	CU_ASSERT(mem[0] == 'C');
	CU_ASSERT(mem[1] == 'B');
	CU_ASSERT(mem[2] == 'B');
	CU_ASSERT(mem[3] == 'B');
	CU_ASSERT(mem[4] == 'B');
	CU_ASSERT(mem[5] == 'B');
	CU_ASSERT(mem[6] == 'B');
	CU_ASSERT(mem[7] == 'B');
	CU_ASSERT(mem[8] == 'B');
	CU_ASSERT(mem[9] == 'B');

	memset(iovs, 0, sizeof(iovs));

	/* Get all of the data out of the pipe */
	CU_ASSERT(spdk_pipe_reader_bytes_available(pipe) == 3);
	rc = spdk_pipe_reader_get_buffer(pipe, 3, iovs);
	CU_ASSERT(rc == 3);
	CU_ASSERT(iovs[0].iov_len == 2);
	CU_ASSERT(iovs[1].iov_len == 1);

	data = iovs[0].iov_base;
	CU_ASSERT(data[0] == 'B');
	CU_ASSERT(data[1] == 'B');
	data = iovs[1].iov_base;
	CU_ASSERT(data[0] == 'C');

	spdk_pipe_reader_advance(pipe, 3);

	spdk_pipe_destroy(pipe);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("pipe", NULL, NULL);

	CU_ADD_TEST(suite, test_create_destroy);
	CU_ADD_TEST(suite, test_write_get_buffer);
	CU_ADD_TEST(suite, test_write_advance);
	CU_ADD_TEST(suite, test_read_get_buffer);
	CU_ADD_TEST(suite, test_read_advance);
	CU_ADD_TEST(suite, test_data);

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
