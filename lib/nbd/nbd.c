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

/*
 * Each type of nbd_io has 2 parts: request and response.
 * But for read and write io types, there is a third part -- payload
 * to carry on data receiving or sending.
 */
enum nbd_io_state_t {
	NBD_IO_RECV_REQ = 0,
	NBD_IO_RECV_PAYLOAD,
	NBD_IO_PROCESSING,
	NBD_IO_SEND_RESP,
	NBD_IO_SEND_PAYLOAD,
	NBD_IO_STATE_COUNT,
};

struct nbd_io {
	struct spdk_nbd_disk	*nbd;
	enum nbd_io_state_t	state;

	enum spdk_bdev_io_type	type;
	int			ref;
	void			*payload;

	/* NOTE: for TRIM, this represents number of bytes to trim. */
	uint32_t		payload_size;

	struct nbd_request	req;
	struct nbd_reply	resp;

	/*
	 * Tracks current progress on reading/writing a request,
	 * response, or payload from the nbd socket.
	 */
	uint32_t		offset;
};

/*
 * Functions used by poller to process nbd io.
 * @return
 *   = 0  success
 *   < 0  error
 */
typedef int (*nbd_poller_func_t)(struct nbd_io *io);

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
	struct spdk_nbd_disk *nbd = io->nbd;

	if (success) {
		io->resp.error = 0;
	} else {
		to_be32(&io->resp.error, EIO);
	}

	if (bdev_io != NULL) {
		spdk_bdev_free_io(bdev_io);
	}


	io->state = NBD_IO_SEND_RESP;

	io->ref--;
	if (io->ref == 0) {
		_nbd_stop(nbd);
	}
}

static int
nbd_submit_bdev_io(struct spdk_nbd_disk *nbd, struct nbd_io *io)
{
	struct spdk_bdev_desc *desc = nbd->bdev_desc;
	struct spdk_io_channel *ch = nbd->ch;
	int rc = 0;

	io->ref++;

	switch (from_be32(&io->req.type)) {
	case NBD_CMD_READ:
		io->type = SPDK_BDEV_IO_TYPE_READ;
		rc = spdk_bdev_read(desc, ch, io->payload, from_be64(&io->req.from),
				    io->payload_size, nbd_io_done, io);
		break;
	case NBD_CMD_WRITE:
		io->type = SPDK_BDEV_IO_TYPE_WRITE;
		rc = spdk_bdev_write(desc, ch, io->payload, from_be64(&io->req.from),
				     io->payload_size, nbd_io_done, io);
		break;
#ifdef NBD_FLAG_SEND_FLUSH
	case NBD_CMD_FLUSH:
		io->type = SPDK_BDEV_IO_TYPE_FLUSH;
		rc = spdk_bdev_flush(desc, ch, 0,
				     spdk_bdev_get_num_blocks(nbd->bdev) * spdk_bdev_get_block_size(nbd->bdev),
				     nbd_io_done, io);
		break;
#endif
#ifdef NBD_FLAG_SEND_TRIM
	case NBD_CMD_TRIM:
		io->type = SPDK_BDEV_IO_TYPE_UNMAP;
		rc = spdk_bdev_unmap(desc, ch, from_be64(&io->req.from),
				     io->payload_size, nbd_io_done, io);
		break;
#endif
	case NBD_CMD_DISC:
		io->ref--;
		return -ECONNRESET;
	default:
		rc = -1;
	}

	if (rc < 0) {
		nbd_io_done(NULL, false, io);
	}

	return 0;
}

static int
nbd_io_recv_req(struct nbd_io *io)
{
	struct spdk_nbd_disk *nbd = io->nbd;
	int ret;

	ret = read_from_socket(nbd->spdk_sp_fd, (char *)&io->req + io->offset,
			       sizeof(io->req) - io->offset);
	if (ret <= 0) {
		return ret;
	}

	io->offset += ret;
	ret = 0;

	if (io->offset == sizeof(io->req)) {
		io->offset = 0;

		/* req magic check */
		if (from_be32(&io->req.magic) != NBD_REQUEST_MAGIC) {
			SPDK_ERRLOG("invalid request magic\n");
			return -EINVAL;
		}

		/* io payload allocate */
		io->payload_size = from_be32(&io->req.len);

		if (io->payload_size) {
			io->payload = spdk_dma_malloc(io->payload_size, nbd->buf_align, NULL);
			if (io->payload == NULL) {
				SPDK_ERRLOG("could not allocate io->payload of size %d\n", io->payload_size);
				return -ENOMEM;
			}
		} else {
			io->payload = NULL;
		}

		/* next IO step */
		switch (from_be32(&io->req.type)) {
		case NBD_CMD_WRITE:
			io->state = NBD_IO_RECV_PAYLOAD;
			break;
		default:
			io->state = NBD_IO_PROCESSING;
			ret = nbd_submit_bdev_io(nbd, io);
		}
	}

	return ret;
}

static int
nbd_io_recv_payload(struct nbd_io *io)
{
	struct spdk_nbd_disk *nbd = io->nbd;
	int ret;

	ret = read_from_socket(nbd->spdk_sp_fd, io->payload + io->offset, io->payload_size - io->offset);
	if (ret <= 0) {
		return ret;
	}

	io->offset += ret;
	ret = 0;

	if (io->offset == io->payload_size) {
		io->offset = 0;

		io->state = NBD_IO_PROCESSING;
		ret = nbd_submit_bdev_io(nbd, io);
	}

	return ret;
}

static int
nbd_io_processing(struct nbd_io *io)
{
	return 0;
}

static int
nbd_io_send_resp(struct nbd_io *io)
{
	struct spdk_nbd_disk *nbd = io->nbd;
	int ret;

	/* resp error is set in io_done */
	to_be32(&io->resp.magic, NBD_REPLY_MAGIC);
	memcpy(&io->resp.handle, &io->req.handle, sizeof(io->resp.handle));

	ret = write_to_socket(nbd->spdk_sp_fd, (char *)&io->resp + io->offset,
			      sizeof(io->resp) - io->offset);
	if (ret <= 0) {
		return ret;
	}

	io->offset += ret;
	ret = 0;

	if (io->offset == sizeof(io->resp)) {
		io->offset = 0;

		/* next IO step */
		switch (from_be32(&io->req.type)) {
		case NBD_CMD_READ:
			io->state = NBD_IO_SEND_PAYLOAD;
			break;
		default:
			io->state = NBD_IO_RECV_REQ;
			if (io->payload) {
				spdk_dma_free(io->payload);
			}
		}
	}

	return ret;

}
static int
nbd_io_send_payload(struct nbd_io *io)
{
	struct spdk_nbd_disk *nbd = io->nbd;
	int ret;

	ret = write_to_socket(nbd->spdk_sp_fd, io->payload + io->offset, io->payload_size - io->offset);
	if (ret <= 0) {
		return ret;
	}

	io->offset += ret;
	ret = 0;

	if (io->offset == io->payload_size) {
		io->offset = 0;
		io->state = NBD_IO_RECV_REQ;
		if (io->payload) {
			spdk_dma_free(io->payload);
		}
	}

	return ret;
}

const nbd_poller_func_t nbd_io_poll_func[NBD_IO_STATE_COUNT] = {
	nbd_io_recv_req,
	nbd_io_recv_payload,
	nbd_io_processing,
	nbd_io_send_resp,
	nbd_io_send_payload,
};

/**
 * Poll an NBD instance.
 *
 * \return 0 on success or negated errno values on error (e.g. connection closed).
 */
static inline int
_spdk_nbd_poll(struct spdk_nbd_disk *nbd)
{
	struct nbd_io	*io = &nbd->io;

	return nbd_io_poll_func[io->state](io);
}

static void
spdk_nbd_poll(void *arg)
{
	struct spdk_nbd_disk *nbd = arg;
	char buf[64];
	int rc;

	rc = _spdk_nbd_poll(nbd);
	if (rc < 0) {
		spdk_strerror_r(-rc, buf, sizeof(buf));
		SPDK_INFOLOG(SPDK_LOG_NBD, "spdk_nbd_poll() returned %s (%d); closing connection\n",
			     buf, rc);
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
	ioctl(dev_fd, NBD_CLEAR_QUE);
	ioctl(dev_fd, NBD_CLEAR_SOCK);
	pthread_exit(NULL);
}

struct spdk_nbd_disk *
spdk_nbd_start(const char *bdev_name, const char *nbd_path)
{
	struct spdk_nbd_disk	*nbd;
	struct spdk_bdev	*bdev;
	pthread_t		tid;
	int			rc;
	int			sp[2];
	char			buf[64];
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

	nbd->io.nbd = nbd;
	nbd->dev_fd = -1;
	nbd->spdk_sp_fd = -1;
	nbd->kernel_sp_fd = -1;

	rc = spdk_bdev_open(bdev, true, NULL, NULL, &nbd->bdev_desc);
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
		spdk_strerror_r(errno, buf, sizeof(buf));
		SPDK_ERRLOG("open(\"%s\") failed: %s\n", nbd_path, buf);
		goto err;
	}

	rc = ioctl(nbd->dev_fd, NBD_SET_BLKSIZE, spdk_bdev_get_block_size(bdev));
	if (rc == -1) {
		spdk_strerror_r(errno, buf, sizeof(buf));
		SPDK_ERRLOG("ioctl(NBD_SET_BLKSIZE) failed: %s\n", buf);
		goto err;
	}

	rc = ioctl(nbd->dev_fd, NBD_SET_SIZE_BLOCKS, spdk_bdev_get_num_blocks(bdev));
	if (rc == -1) {
		spdk_strerror_r(errno, buf, sizeof(buf));
		SPDK_ERRLOG("ioctl(NBD_SET_SIZE_BLOCKS) failed: %s\n", buf);
		goto err;
	}

	rc = ioctl(nbd->dev_fd, NBD_CLEAR_SOCK);
	if (rc == -1) {
		spdk_strerror_r(errno, buf, sizeof(buf));
		SPDK_ERRLOG("ioctl(NBD_CLEAR_SOCK) failed: %s\n", buf);
		goto err;
	}

	SPDK_INFOLOG(SPDK_LOG_NBD, "Enabling kernel access to bdev %s via %s\n",
		     spdk_bdev_get_name(bdev), nbd_path);

	rc = ioctl(nbd->dev_fd, NBD_SET_SOCK, nbd->kernel_sp_fd);
	if (rc == -1) {
		spdk_strerror_r(errno, buf, sizeof(buf));
		SPDK_ERRLOG("ioctl(NBD_SET_SOCK) failed: %s\n", buf);
		goto err;
	}

#ifdef NBD_FLAG_SEND_TRIM
	rc = ioctl(nbd->dev_fd, NBD_SET_FLAGS, NBD_FLAG_SEND_TRIM);
	if (rc == -1) {
		spdk_strerror_r(errno, buf, sizeof(buf));
		SPDK_ERRLOG("ioctl(NBD_SET_FLAGS) failed: %s\n", buf);
		goto err;
	}
#endif

	rc = pthread_create(&tid, NULL, nbd_start_kernel, (void *)(intptr_t)nbd->dev_fd);
	if (rc != 0) {
		spdk_strerror_r(rc, buf, sizeof(buf));
		SPDK_ERRLOG("could not create thread: %s\n", buf);
		goto err;
	}

	rc = pthread_detach(tid);
	if (rc != 0) {
		spdk_strerror_r(rc, buf, sizeof(buf));
		SPDK_ERRLOG("could not detach thread for nbd kernel: %s\n", buf);
		goto err;
	}

	flag = fcntl(nbd->spdk_sp_fd, F_GETFL);
	if (fcntl(nbd->spdk_sp_fd, F_SETFL, flag | O_NONBLOCK) < 0) {
		spdk_strerror_r(errno, buf, sizeof(buf));
		SPDK_ERRLOG("fcntl can't set nonblocking mode for socket, fd: %d (%s)\n", nbd->spdk_sp_fd, buf);
		goto err;
	}

	nbd->nbd_poller = spdk_poller_register(spdk_nbd_poll, nbd, 0);

	return nbd;

err:
	spdk_nbd_stop(nbd);

	return NULL;
}

SPDK_LOG_REGISTER_COMPONENT("nbd", SPDK_LOG_NBD)
