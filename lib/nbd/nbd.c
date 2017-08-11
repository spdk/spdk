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
#include "spdk/string.h"

#include <linux/nbd.h>

#include "spdk/nbd.h"
#include "spdk/bdev.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/io_channel.h"

struct nbd_io {
	enum spdk_bdev_io_type	type;
	void			*payload;

	/* NOTE: for TRIM, this represents number of bytes to trim. */
	uint32_t		payload_size;

	bool			payload_in_progress;

	struct nbd_request	req;
	bool			req_in_progress;

	struct nbd_reply	resp;
	bool			resp_in_progress;

	/*
	 * Tracks current progress on reading/writing a request,
	 * response, or payload from the nbd socket.
	 */
	uint32_t		offset;
};

struct spdk_nbd_disk {
	struct spdk_bdev	*bdev;
	struct spdk_bdev_desc	*bdev_desc;
	struct spdk_io_channel	*ch;
	int			fd;
	struct nbd_io		io;
	uint32_t		buf_align;
};

static bool
is_read(enum spdk_bdev_io_type io_type)
{
	if (io_type == SPDK_BDEV_IO_TYPE_READ) {
		return true;
	} else {
		return false;
	}
}

static bool
is_write(enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		return true;
	default:
		return false;
	}
}

void
spdk_nbd_stop(struct spdk_nbd_disk *nbd)
{
	if (nbd == NULL) {
		return;
	}

	if (nbd->ch) {
		spdk_put_io_channel(nbd->ch);
	}

	if (nbd->bdev_desc) {
		spdk_bdev_close(nbd->bdev_desc);
	}

	if (nbd->fd >= 0) {
		close(nbd->fd);
	}

	free(nbd);
}

static int64_t
read_from_socket(int fd, void *buf, size_t length)
{
	ssize_t bytes_read;

	bytes_read = read(fd, buf, length);
	if (bytes_read == 0) {
		return -EIO;
	} else if (bytes_read == -1) {
		if (errno != EAGAIN) {
			return -errno;
		}
		return 0;
	} else {
		return bytes_read;
	}
}

static int64_t
write_to_socket(int fd, void *buf, size_t length)
{
	ssize_t bytes_written;

	bytes_written = write(fd, buf, length);
	if (bytes_written == 0) {
		return -EIO;
	} else if (bytes_written == -1) {
		if (errno != EAGAIN) {
			return -errno;
		}
		return 0;
	} else {
		return bytes_written;
	}
}

static void
nbd_io_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct nbd_io	*io = cb_arg;

	if (success) {
		io->resp.error = 0;
	} else {
		to_be32(&io->resp.error, EIO);
	}
	io->resp_in_progress = true;
	if (bdev_io != NULL) {
		spdk_bdev_free_io(bdev_io);
	}
}

static void
nbd_submit_bdev_io(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
		   struct spdk_io_channel *ch, struct nbd_io *io)
{
	int rc;

	switch (io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		rc = spdk_bdev_read(desc, ch, io->payload, from_be64(&io->req.from),
				    io->payload_size, nbd_io_done, io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		rc = spdk_bdev_write(desc, ch, io->payload, from_be64(&io->req.from),
				     io->payload_size, nbd_io_done, io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = spdk_bdev_unmap(desc, ch, from_be64(&io->req.from),
				     io->payload_size, nbd_io_done, io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = spdk_bdev_flush(desc, ch, 0, spdk_bdev_get_num_blocks(bdev) * spdk_bdev_get_block_size(bdev),
				     nbd_io_done, io);
		break;
	default:
		rc = -1;
		break;
	}

	if (rc == -1) {
		nbd_io_done(NULL, false, io);
	}
}

static int
process_request(struct spdk_nbd_disk *nbd)
{
	struct nbd_io	*io = &nbd->io;

	memcpy(&io->resp.handle, &io->req.handle, sizeof(io->resp.handle));
	io->resp.error = 0;
	io->offset = 0;

	io->payload_size = from_be32(&io->req.len);
	spdk_dma_free(io->payload);
	io->payload = spdk_dma_malloc(io->payload_size, nbd->buf_align, NULL);
	if (io->payload == NULL) {
		SPDK_ERRLOG("could not allocate io->payload of size %d\n", io->payload_size);
		return -ENOMEM;
	}

	assert(from_be32(&io->req.magic) == NBD_REQUEST_MAGIC);

	switch (from_be32(&io->req.type)) {
	case NBD_CMD_READ:
		io->type = SPDK_BDEV_IO_TYPE_READ;
		nbd_submit_bdev_io(nbd->bdev, nbd->bdev_desc, nbd->ch, io);
		break;
	case NBD_CMD_WRITE:
		io->type = SPDK_BDEV_IO_TYPE_WRITE;
		io->payload_in_progress = true;
		break;
	case NBD_CMD_DISC:
		return -ECONNRESET;
#ifdef NBD_FLAG_SEND_FLUSH
	case NBD_CMD_FLUSH:
		io->type = SPDK_BDEV_IO_TYPE_FLUSH;
		nbd_submit_bdev_io(nbd->bdev, nbd->bdev_desc, nbd->ch, io);
		break;
#endif
#ifdef NBD_FLAG_SEND_TRIM
	case NBD_CMD_TRIM:
		io->type = SPDK_BDEV_IO_TYPE_UNMAP;
		nbd_submit_bdev_io(nbd->bdev, nbd->bdev_desc, nbd->ch, io);
		break;
#endif
	}

	return 0;
}

int
spdk_nbd_poll(struct spdk_nbd_disk *nbd)
{
	struct nbd_io	*io = &nbd->io;
	int		fd = nbd->fd;
	int64_t		ret;
	int		rc;

	if (io->req_in_progress) {
		ret = read_from_socket(fd, (char *)&io->req + io->offset, sizeof(io->req) - io->offset);
		if (ret <= 0) {
			return ret;
		}
		io->offset += ret;
		if (io->offset == sizeof(io->req)) {
			io->req_in_progress = false;
			rc = process_request(nbd);
			if (rc != 0) {
				return rc;
			}
		}
	}

	if (io->payload_in_progress && is_write(io->type)) {
		ret = read_from_socket(fd, io->payload + io->offset, io->payload_size - io->offset);
		if (ret <= 0) {
			return ret;
		}
		io->offset += ret;
		if (io->offset == io->payload_size) {
			io->payload_in_progress = false;
			nbd_submit_bdev_io(nbd->bdev, nbd->bdev_desc, nbd->ch, io);
			io->offset = 0;
		}
	}

	if (io->resp_in_progress) {
		ret = write_to_socket(fd, (char *)&io->resp + io->offset, sizeof(io->resp) - io->offset);
		if (ret <= 0) {
			return ret;
		}
		io->offset += ret;
		if (io->offset == sizeof(io->resp)) {
			io->resp_in_progress = false;
			if (is_read(io->type)) {
				io->payload_in_progress = true;
			} else {
				io->req_in_progress = true;
			}
			io->offset = 0;
		}
	}

	if (io->payload_in_progress && is_read(io->type)) {
		ret = write_to_socket(fd, io->payload + io->offset, io->payload_size - io->offset);
		if (ret <= 0) {
			return ret;
		}
		io->offset += ret;
		if (io->offset == io->payload_size) {
			io->payload_in_progress = false;
			io->req_in_progress = true;
			io->offset = 0;
		}
	}

	return 0;
}

static void
nbd_start_kernel(int nbd_fd, int *sp)
{
	int rc;
	char buf[64];

	close(sp[0]);

	rc = ioctl(nbd_fd, NBD_SET_SOCK, sp[1]);
	if (rc == -1) {
		spdk_strerror_r(errno, buf, sizeof(buf));
		SPDK_ERRLOG("ioctl(NBD_SET_SOCK) failed: %s\n", buf);
		exit(-1);
	}

#ifdef NBD_FLAG_SEND_TRIM
	rc = ioctl(nbd_fd, NBD_SET_FLAGS, NBD_FLAG_SEND_TRIM);
	if (rc == -1) {
		spdk_strerror_r(errno, buf, sizeof(buf));
		SPDK_ERRLOG("ioctl(NBD_SET_FLAGS) failed: %s\n", buf);
		exit(-1);
	}
#endif

	/* This will block in the kernel until the client disconnects. */
	ioctl(nbd_fd, NBD_DO_IT);

	ioctl(nbd_fd, NBD_CLEAR_QUE);
	ioctl(nbd_fd, NBD_CLEAR_SOCK);

	exit(0);
}

struct spdk_nbd_disk *
spdk_nbd_start(struct spdk_bdev *bdev, const char *nbd_path)
{
	struct spdk_nbd_disk	*nbd;
	int			rc;
	int			sp[2] = { -1, -1 }, nbd_fd = -1;
	char buf[64];

	nbd = calloc(1, sizeof(*nbd));
	if (nbd == NULL) {
		return NULL;
	}
	nbd->fd = -1;

	rc = spdk_bdev_open(bdev, true, NULL, NULL, &nbd->bdev_desc);
	if (rc != 0) {
		SPDK_ERRLOG("could not open bdev %s, error=%d\n", spdk_bdev_get_name(bdev), rc);
		goto err;
	}

	nbd->bdev = bdev;
	nbd->ch = spdk_bdev_get_io_channel(nbd->bdev_desc);
	nbd->buf_align = spdk_max(spdk_bdev_get_buf_align(bdev), 64);

	rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
	if (rc != 0) {
		SPDK_ERRLOG("socketpair failed\n");
		goto err;
	}

	nbd_fd = open(nbd_path, O_RDWR);
	if (nbd_fd == -1) {
		spdk_strerror_r(errno, buf, sizeof(buf));
		SPDK_ERRLOG("open(\"%s\") failed: %s\n", nbd_path, buf);
		goto err;
	}

	rc = ioctl(nbd_fd, NBD_SET_BLKSIZE, spdk_bdev_get_block_size(bdev));
	if (rc == -1) {
		spdk_strerror_r(errno, buf, sizeof(buf));
		SPDK_ERRLOG("ioctl(NBD_SET_BLKSIZE) failed: %s\n", buf);
		goto err;
	}

	rc = ioctl(nbd_fd, NBD_SET_SIZE_BLOCKS, spdk_bdev_get_num_blocks(bdev));
	if (rc == -1) {
		spdk_strerror_r(errno, buf, sizeof(buf));
		SPDK_ERRLOG("ioctl(NBD_SET_SIZE_BLOCKS) failed: %s\n", buf);
		goto err;
	}

	rc = ioctl(nbd_fd, NBD_CLEAR_SOCK);
	if (rc == -1) {
		spdk_strerror_r(errno, buf, sizeof(buf));
		SPDK_ERRLOG("ioctl(NBD_CLEAR_SOCK) failed: %s\n", buf);
		goto err;
	}

	printf("Enabling kernel access to bdev %s via %s\n", spdk_bdev_get_name(bdev), nbd_path);

	rc = fork();

	switch (rc) {
	case 0:
		nbd_start_kernel(nbd_fd, sp);
		break;
	case -1:
		spdk_strerror_r(errno, buf, sizeof(buf));
		SPDK_ERRLOG("could not fork: %s\n", buf);
		goto err;
	default:
		close(nbd_fd);
		break;
	}

	close(sp[1]);

	nbd->fd = sp[0];
	fcntl(nbd->fd, F_SETFL, O_NONBLOCK);

	to_be32(&nbd->io.resp.magic, NBD_REPLY_MAGIC);
	nbd->io.req_in_progress = true;

	return nbd;

err:
	if (sp[0] >= 0) {
		close(sp[0]);
	}

	if (sp[1] >= 0) {
		close(sp[1]);
	}

	if (nbd_fd >= 0) {
		close(nbd_fd);
	}

	spdk_nbd_stop(nbd);

	return NULL;
}
