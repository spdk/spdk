/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/pipe.h"
#include "spdk/util.h"
#include "spdk/queue.h"
#include "spdk/log.h"

struct spdk_pipe_buf {
	SLIST_ENTRY(spdk_pipe_buf)	link;
	uint32_t			sz;
};

struct spdk_pipe_group {
	SLIST_HEAD(, spdk_pipe_buf) bufs;
};

struct spdk_pipe {
	uint8_t	*buf;
	uint32_t sz;

	uint32_t write;
	uint32_t read;
	bool full;

	struct spdk_pipe_group *group;
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

void *
spdk_pipe_destroy(struct spdk_pipe *pipe)
{
	void *buf;

	if (pipe == NULL) {
		return NULL;
	}

	if (pipe->group) {
		spdk_pipe_group_remove(pipe->group, pipe);
	}

	buf = pipe->buf;
	free(pipe);
	return buf;
}

static void
pipe_alloc_buf_from_group(struct spdk_pipe *pipe)
{
	struct spdk_pipe_buf *buf;
	struct spdk_pipe_group *group;

	assert(pipe->group != NULL);
	group = pipe->group;

	/* We have to pick a buffer that's the correct size. It's almost always
	 * the first one. */
	buf = SLIST_FIRST(&group->bufs);
	while (buf != NULL) {
		if (buf->sz == pipe->sz) {
			/* TODO: Could track the previous and do an SLIST_REMOVE_AFTER */
			SLIST_REMOVE(&pipe->group->bufs, buf, spdk_pipe_buf, link);
			pipe->buf = (void *)buf;
			return;
		}
		buf = SLIST_NEXT(buf, link);
	}
	/* Should never get here. */
	assert(false);
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

	if (pipe->buf == NULL) {
		pipe_alloc_buf_from_group(pipe);
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

	if (read == write) {
		/* The pipe is empty. To re-use the same memory more frequently, jump
		 * both pointers back to the beginning of the pipe. */
		read = 0;
		pipe->write = 0;

		/* Additionally, release the buffer to the shared pool */
		if (pipe->group) {
			struct spdk_pipe_buf *buf = (struct spdk_pipe_buf *)pipe->buf;
			buf->sz = pipe->sz;
			SLIST_INSERT_HEAD(&pipe->group->bufs, buf, link);
			pipe->buf = NULL;
		}
	}

	pipe->read = read;

	return 0;
}

struct spdk_pipe_group *
spdk_pipe_group_create(void)
{
	struct spdk_pipe_group *group;

	group = calloc(1, sizeof(*group));
	if (!group) {
		return NULL;
	}

	SLIST_INIT(&group->bufs);

	return group;
}

void
spdk_pipe_group_destroy(struct spdk_pipe_group *group)
{
	if (!SLIST_EMPTY(&group->bufs)) {
		SPDK_ERRLOG("Destroying a pipe group that still has buffers!\n");
		assert(false);
	}

	free(group);
}

int
spdk_pipe_group_add(struct spdk_pipe_group *group, struct spdk_pipe *pipe)
{
	struct spdk_pipe_buf *buf;

	assert(pipe->group == NULL);

	pipe->group = group;
	if (pipe->read != pipe->write || pipe->full) {
		/* Pipe currently has valid data, so keep the buffer attached
		 * to the pipe for now.  We can move it to the group's SLIST
		 * later when it gets emptied.
		 */
		return 0;
	}

	buf = (struct spdk_pipe_buf *)pipe->buf;
	buf->sz = pipe->sz;
	SLIST_INSERT_HEAD(&group->bufs, buf, link);
	pipe->buf = NULL;
	return 0;
}

int
spdk_pipe_group_remove(struct spdk_pipe_group *group, struct spdk_pipe *pipe)
{
	assert(pipe->group == group);

	if (pipe->buf == NULL) {
		/* Associate a buffer with the pipe before returning. */
		pipe_alloc_buf_from_group(pipe);
		assert(pipe->buf != NULL);
	}

	pipe->group = NULL;
	return 0;
}
