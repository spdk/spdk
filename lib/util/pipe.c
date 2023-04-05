/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/pipe.h"
#include "spdk/util.h"

struct spdk_pipe {
	uint8_t	*buf;
	uint32_t sz;

	uint32_t write;
	uint32_t read;
	bool full;
};

struct spdk_pipe *
spdk_pipe_create(void *buf, uint32_t sz)
{
	struct spdk_pipe *pipe;

	pipe = calloc(1, sizeof(*pipe));
	if (pipe == NULL) {
		return NULL;
	}

	pipe->buf = buf;
	pipe->sz = sz;

	return pipe;
}

void
spdk_pipe_destroy(struct spdk_pipe *pipe)
{
	free(pipe);
}

int
spdk_pipe_writer_get_buffer(struct spdk_pipe *pipe, uint32_t requested_sz, struct iovec *iovs)
{
	uint32_t sz;
	uint32_t read;
	uint32_t write;

	read = pipe->read;
	write = pipe->write;

	if (pipe->full || requested_sz == 0) {
		iovs[0].iov_base = NULL;
		iovs[0].iov_len = 0;
		return 0;
	}

	if (read <= write) {
		sz = spdk_min(requested_sz, pipe->sz - write);

		iovs[0].iov_base = pipe->buf + write;
		iovs[0].iov_len = sz;

		requested_sz -= sz;

		if (requested_sz > 0) {
			sz = spdk_min(requested_sz, read);

			iovs[1].iov_base = (sz == 0) ? NULL : pipe->buf;
			iovs[1].iov_len = sz;
		} else {
			iovs[1].iov_base = NULL;
			iovs[1].iov_len = 0;
		}
	} else {
		sz = spdk_min(requested_sz, read - write);

		iovs[0].iov_base = pipe->buf + write;
		iovs[0].iov_len = sz;
		iovs[1].iov_base = NULL;
		iovs[1].iov_len = 0;
	}

	return iovs[0].iov_len + iovs[1].iov_len;
}

int
spdk_pipe_writer_advance(struct spdk_pipe *pipe, uint32_t requested_sz)
{
	uint32_t sz;
	uint32_t read;
	uint32_t write;

	read = pipe->read;
	write = pipe->write;

	if (requested_sz > pipe->sz || pipe->full) {
		return -EINVAL;
	}

	if (read <= write) {
		if (requested_sz > (pipe->sz - write) + read) {
			return -EINVAL;
		}

		sz = spdk_min(requested_sz, pipe->sz - write);

		write += sz;
		if (write == pipe->sz) {
			write = 0;
		}
		requested_sz -= sz;

		if (requested_sz > 0) {
			write = requested_sz;
		}
	} else {
		if (requested_sz > (read - write)) {
			return -EINVAL;
		}

		write += requested_sz;
	}

	if (read == write) {
		pipe->full = true;
	}
	pipe->write = write;

	return 0;
}

uint32_t
spdk_pipe_reader_bytes_available(struct spdk_pipe *pipe)
{
	uint32_t read;
	uint32_t write;

	read = pipe->read;
	write = pipe->write;

	if (read == write && !pipe->full) {
		return 0;
	} else if (read < write) {
		return write - read;
	} else {
		return (pipe->sz - read) + write;
	}
}

int
spdk_pipe_reader_get_buffer(struct spdk_pipe *pipe, uint32_t requested_sz, struct iovec *iovs)
{
	uint32_t sz;
	uint32_t read;
	uint32_t write;

	read = pipe->read;
	write = pipe->write;

	if ((read == write && !pipe->full) || requested_sz == 0) {
		iovs[0].iov_base = NULL;
		iovs[0].iov_len = 0;
		iovs[1].iov_base = NULL;
		iovs[1].iov_len = 0;
	} else if (read < write) {
		sz = spdk_min(requested_sz, write - read);

		iovs[0].iov_base = pipe->buf + read;
		iovs[0].iov_len = sz;
		iovs[1].iov_base = NULL;
		iovs[1].iov_len = 0;
	} else {
		sz = spdk_min(requested_sz, pipe->sz - read);

		iovs[0].iov_base = pipe->buf + read;
		iovs[0].iov_len = sz;

		requested_sz -= sz;

		if (requested_sz > 0) {
			sz = spdk_min(requested_sz, write);
			iovs[1].iov_base = (sz == 0) ? NULL : pipe->buf;
			iovs[1].iov_len = sz;
		} else {
			iovs[1].iov_base = NULL;
			iovs[1].iov_len = 0;
		}
	}

	return iovs[0].iov_len + iovs[1].iov_len;
}

int
spdk_pipe_reader_advance(struct spdk_pipe *pipe, uint32_t requested_sz)
{
	uint32_t sz;
	uint32_t read;
	uint32_t write;

	read = pipe->read;
	write = pipe->write;

	if (requested_sz == 0) {
		return 0;
	}

	if (read < write) {
		if (requested_sz > (write - read)) {
			return -EINVAL;
		}

		read += requested_sz;
	} else {
		sz = spdk_min(requested_sz, pipe->sz - read);

		read += sz;
		if (read == pipe->sz) {
			read = 0;
		}
		requested_sz -= sz;

		if (requested_sz > 0) {
			if (requested_sz > write) {
				return -EINVAL;
			}

			read = requested_sz;
		}
	}

	/* We know we advanced at least one byte, so the pipe isn't full. */
	pipe->full = false;
	pipe->read = read;

	return 0;
}
