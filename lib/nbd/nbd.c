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
#include "nbd_internal.h"
#include "spdk/bdev.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/io_channel.h"

#include "spdk_internal/log.h"

struct nbd_io {
	enum spdk_bdev_io_type	type;
	int			ref;
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
	int			dev_fd;
	char			*nbd_path;
	int			kernel_sp_fd;
	int			spdk_sp_fd;
	struct nbd_io		io;
	struct spdk_poller	*nbd_poller;
	uint32_t		buf_align;

	TAILQ_ENTRY(spdk_nbd_disk)	tailq;
};

struct spdk_nbd_disk_globals {
	TAILQ_HEAD(, spdk_nbd_disk)	disk_head;
};

static struct spdk_nbd_disk_globals g_spdk_nbd;

int
spdk_nbd_init(void)
{
	TAILQ_INIT(&g_spdk_nbd.disk_head);

	return 0;
}

void
spdk_nbd_fini(void)
{
	struct spdk_nbd_disk *nbd_idx, *nbd_tmp;

	/*
	 * Stop running spdk_nbd_disk.
	 * Here, nbd removing are unnecessary, but _SAFE variant
	 * is needed, since internal spdk_nbd_disk_unregister will
	 * remove nbd from TAILQ.
	 */
	TAILQ_FOREACH_SAFE(nbd_idx, &g_spdk_nbd.disk_head, tailq, nbd_tmp) {
		spdk_nbd_stop(nbd_idx);
	}
}

static int
spdk_nbd_disk_register(struct spdk_nbd_disk *nbd)
{
	if (spdk_nbd_disk_find_by_nbd_path(nbd->nbd_path)) {
		SPDK_NOTICELOG("%s is already exported\n", nbd->nbd_path);
		return -1;
	}

	TAILQ_INSERT_TAIL(&g_spdk_nbd.disk_head, nbd, tailq);

	return 0;
}

static void
spdk_nbd_disk_unregister(struct spdk_nbd_disk *nbd)
{
	struct spdk_nbd_disk *nbd_idx, *nbd_tmp;

	/*
	 * nbd disk may be stopped before registered.
	 * check whether it was registered.
	 */
	TAILQ_FOREACH_SAFE(nbd_idx, &g_spdk_nbd.disk_head, tailq, nbd_tmp) {
		if (nbd == nbd_idx) {
			TAILQ_REMOVE(&g_spdk_nbd.disk_head, nbd_idx, tailq);
			break;
		}
	}
}

struct spdk_nbd_disk *
spdk_nbd_disk_find_by_nbd_path(const char *nbd_path)
{
	struct spdk_nbd_disk *nbd;

	/*
	 * check whether nbd has already been registered by nbd path.
	 */
	TAILQ_FOREACH(nbd, &g_spdk_nbd.disk_head, tailq) {
		if (!strcmp(nbd->nbd_path, nbd_path)) {
			return nbd;
		}
	}

	return NULL;
}

struct spdk_nbd_disk *spdk_nbd_disk_first(void)
{
	return TAILQ_FIRST(&g_spdk_nbd.disk_head);
}

struct spdk_nbd_disk *spdk_nbd_disk_next(struct spdk_nbd_disk *prev)
{
	return TAILQ_NEXT(prev, tailq);
}

const char *
spdk_nbd_disk_get_nbd_path(struct spdk_nbd_disk *nbd)
{
	return nbd->nbd_path;
}

const char *
spdk_nbd_disk_get_bdev_name(struct spdk_nbd_disk *nbd)
{
	return spdk_bdev_get_name(nbd->bdev);
}

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
nbd_disconnect(struct spdk_nbd_disk *nbd)
{
	/*
	 * nbd soft-disconnection to terminate transmission phase.
	 * After receiving this ioctl command, nbd kernel module will send
	 * a NBD_CMD_DISC type io to nbd server in order to inform server.
	 */
	ioctl(nbd->dev_fd, NBD_DISCONNECT);
}

static void
_nbd_stop(struct spdk_nbd_disk *nbd)
{
	if (nbd->ch) {
		spdk_put_io_channel(nbd->ch);
	}

	if (nbd->bdev_desc) {
		spdk_bdev_close(nbd->bdev_desc);
	}

	if (nbd->dev_fd >= 0) {
		ioctl(nbd->dev_fd, NBD_CLEAR_QUE);
		ioctl(nbd->dev_fd, NBD_CLEAR_SOCK);
		close(nbd->dev_fd);
	}

	if (nbd->nbd_path) {
		free(nbd->nbd_path);
	}

	if (nbd->spdk_sp_fd >= 0) {
		close(nbd->spdk_sp_fd);
	}

	if (nbd->kernel_sp_fd >= 0) {
		close(nbd->kernel_sp_fd);
	}

	if (nbd->nbd_poller) {
		spdk_poller_unregister(&nbd->nbd_poller);
	}

	spdk_nbd_disk_unregister(nbd);

	free(nbd);
}

void
spdk_nbd_stop(struct spdk_nbd_disk *nbd)
{
	if (nbd == NULL) {
		return;
	}

	nbd->io.ref--;
	if (nbd->io.ref == 0) {
		_nbd_stop(nbd);
	}
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

	io->ref--;
	if (io->ref == 0) {
		_nbd_stop(SPDK_CONTAINEROF(io, struct spdk_nbd_disk, io));
	}
}

static void
nbd_submit_bdev_io(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
		   struct spdk_io_channel *ch, struct nbd_io *io)
{
	int rc;

	io->ref++;

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

	if (io->payload_size) {
		io->payload = spdk_dma_malloc(io->payload_size, nbd->buf_align, NULL);
		if (io->payload == NULL) {
			SPDK_ERRLOG("could not allocate io->payload of size %d\n", io->payload_size);
			return -ENOMEM;
		}
	} else {
		io->payload = NULL;
	}

	if (from_be32(&io->req.magic) != NBD_REQUEST_MAGIC) {
		SPDK_ERRLOG("invalid request magic\n");
		return -EINVAL;
	}

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

/**
 * Poll an NBD instance.
 *
 * \return 0 on success or negated errno values on error (e.g. connection closed).
 */
static int
_spdk_nbd_poll(struct spdk_nbd_disk *nbd)
{
	struct nbd_io	*io = &nbd->io;
	int		fd = nbd->spdk_sp_fd;
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
spdk_nbd_poll(void *arg)
{
	struct spdk_nbd_disk *nbd = arg;
	int rc;

	rc = _spdk_nbd_poll(nbd);
	if (rc < 0) {
		SPDK_INFOLOG(SPDK_LOG_NBD, "spdk_nbd_poll() returned %s (%d); closing connection\n",
			     spdk_strerror(-rc), rc);
		spdk_nbd_stop(nbd);
	}
}

static void *
nbd_start_kernel(void *arg)
{
	int dev_fd = (int)(intptr_t)arg;

	spdk_unaffinitize_thread();

	/* This will block in the kernel until we close the spdk_sp_fd. */
	ioctl(dev_fd, NBD_DO_IT);

	pthread_exit(NULL);
}

static void
spdk_nbd_bdev_hot_remove(void *remove_ctx)
{
	struct spdk_nbd_disk *nbd = remove_ctx;

	spdk_nbd_stop(nbd);
}

struct spdk_nbd_disk *
spdk_nbd_start(const char *bdev_name, const char *nbd_path)
{
	struct spdk_nbd_disk	*nbd;
	struct spdk_bdev	*bdev;
	pthread_t		tid;
	int			rc;
	int			sp[2];
	int			flag;

	bdev = spdk_bdev_get_by_name(bdev_name);
	if (bdev == NULL) {
		SPDK_ERRLOG("no bdev %s exists\n", bdev_name);
		return NULL;
	}

	nbd = calloc(1, sizeof(*nbd));
	if (nbd == NULL) {
		return NULL;
	}
	nbd->dev_fd = -1;
	nbd->spdk_sp_fd = -1;
	nbd->kernel_sp_fd = -1;

	rc = spdk_bdev_open(bdev, true, spdk_nbd_bdev_hot_remove, nbd, &nbd->bdev_desc);
	if (rc != 0) {
		SPDK_ERRLOG("could not open bdev %s, error=%d\n", spdk_bdev_get_name(bdev), rc);
		goto err;
	}

	nbd->io.ref = 1;
	nbd->bdev = bdev;

	nbd->ch = spdk_bdev_get_io_channel(nbd->bdev_desc);
	nbd->buf_align = spdk_max(spdk_bdev_get_buf_align(bdev), 64);

	rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
	if (rc != 0) {
		SPDK_ERRLOG("socketpair failed\n");
		goto err;
	}

	nbd->spdk_sp_fd = sp[0];
	nbd->kernel_sp_fd = sp[1];
	nbd->nbd_path = strdup(nbd_path);
	if (!nbd->nbd_path) {
		SPDK_ERRLOG("strdup allocation failure\n");
		goto err;
	}
	/* Add nbd_disk to the end of disk list */
	rc = spdk_nbd_disk_register(nbd);
	if (rc != 0) {
		goto err;
	}

	nbd->dev_fd = open(nbd_path, O_RDWR);
	if (nbd->dev_fd == -1) {
		SPDK_ERRLOG("open(\"%s\") failed: %s\n", nbd_path, spdk_strerror(errno));
		goto err;
	}

	rc = ioctl(nbd->dev_fd, NBD_SET_BLKSIZE, spdk_bdev_get_block_size(bdev));
	if (rc == -1) {
		SPDK_ERRLOG("ioctl(NBD_SET_BLKSIZE) failed: %s\n", spdk_strerror(errno));
		goto err;
	}

	rc = ioctl(nbd->dev_fd, NBD_SET_SIZE_BLOCKS, spdk_bdev_get_num_blocks(bdev));
	if (rc == -1) {
		SPDK_ERRLOG("ioctl(NBD_SET_SIZE_BLOCKS) failed: %s\n", spdk_strerror(errno));
		goto err;
	}

	rc = ioctl(nbd->dev_fd, NBD_CLEAR_SOCK);
	if (rc == -1) {
		SPDK_ERRLOG("ioctl(NBD_CLEAR_SOCK) failed: %s\n", spdk_strerror(errno));
		goto err;
	}

	SPDK_INFOLOG(SPDK_LOG_NBD, "Enabling kernel access to bdev %s via %s\n",
		     spdk_bdev_get_name(bdev), nbd_path);

	rc = ioctl(nbd->dev_fd, NBD_SET_SOCK, nbd->kernel_sp_fd);
	if (rc == -1) {
		SPDK_ERRLOG("ioctl(NBD_SET_SOCK) failed: %s\n", spdk_strerror(errno));
		goto err;
	}

#ifdef NBD_FLAG_SEND_TRIM
	rc = ioctl(nbd->dev_fd, NBD_SET_FLAGS, NBD_FLAG_SEND_TRIM);
	if (rc == -1) {
		SPDK_ERRLOG("ioctl(NBD_SET_FLAGS) failed: %s\n", spdk_strerror(errno));
		goto err;
	}
#endif

	rc = pthread_create(&tid, NULL, nbd_start_kernel, (void *)(intptr_t)nbd->dev_fd);
	if (rc != 0) {
		SPDK_ERRLOG("could not create thread: %s\n", spdk_strerror(rc));
		goto err;
	}

	rc = pthread_detach(tid);
	if (rc != 0) {
		SPDK_ERRLOG("could not detach thread for nbd kernel: %s\n", spdk_strerror(rc));
		goto err;
	}

	flag = fcntl(nbd->spdk_sp_fd, F_GETFL);
	if (fcntl(nbd->spdk_sp_fd, F_SETFL, flag | O_NONBLOCK) < 0) {
		SPDK_ERRLOG("fcntl can't set nonblocking mode for socket, fd: %d (%s)\n",
			    nbd->spdk_sp_fd, spdk_strerror(errno));
		goto err;
	}

	to_be32(&nbd->io.resp.magic, NBD_REPLY_MAGIC);
	nbd->io.req_in_progress = true;

	nbd->nbd_poller = spdk_poller_register(spdk_nbd_poll, nbd, 0);

	return nbd;

err:
	spdk_nbd_stop(nbd);

	return NULL;
}

SPDK_LOG_REGISTER_COMPONENT("nbd", SPDK_LOG_NBD)
