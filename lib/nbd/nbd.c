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
#include <rte_mempool.h>

#include <linux/nbd.h>

#include "spdk/nbd.h"
#include "spdk/bdev.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/io_channel.h"
#include "spdk/queue.h"

struct nbd_io;
/*
 * Functions used by poller to process nbd IO.
 * Socket error is recorded in nbd structure.
 * @return
 * 	 >= 0  byte count of data recv/send
 *   < 0   error
 */
typedef int (*nbd_poller_func_t)(struct nbd_io *io);
static int nbd_io_poller_default(struct nbd_io *io);
static int nbd_io_recv_req(struct nbd_io *io);
static int nbd_io_recv_payload(struct nbd_io *io);
static int nbd_io_send_resp(struct nbd_io *io);
static int nbd_io_send_payload(struct nbd_io *io);

static int
process_request(struct spdk_nbd_disk *nbd, struct nbd_io *io);

struct nbd_io {
	enum spdk_bdev_io_type	type;
	struct spdk_nbd_disk *nbd;

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

	/*
	 * Tracks IO in which progress (reading/writing a request,
	 * or payload from/to the nbd socket.
	 */
	nbd_poller_func_t poller_func;
	TAILQ_ENTRY(nbd_io)	tailq;
};

struct spdk_nbd_disk {
	struct spdk_bdev	*bdev;
	struct spdk_bdev_desc	*bdev_desc;
	struct spdk_io_channel	*ch;
	int			dev_fd;
	int			kernel_sp_fd;
	int			spdk_sp_fd;
	/*
	 * indicate nbd disk is stopping
	 */
	bool leave;

	struct nbd_io		*cur_rio;
	struct nbd_io		*cur_sio;
	TAILQ_HEAD(, nbd_io) recv_io_list;
	TAILQ_HEAD(, nbd_io) send_io_list;
	struct rte_mempool *io_pool;

	uint32_t		buf_align;
};

#define DEFAULT_IO_POOL_SIZE 128

static int
spdk_nbd_initialize_io_pool(struct spdk_nbd_disk *nbd)
{
	/* create nbd io pool */
	nbd->io_pool = rte_mempool_create("NBD_IO_Pool",
					  DEFAULT_IO_POOL_SIZE,
					  sizeof(struct nbd_io),
					  0, 0,
					  NULL, NULL, NULL, NULL,
					  SOCKET_ID_ANY, 0);
	if (!nbd->io_pool) {
		SPDK_ERRLOG("create io pool failed\n");
		return -1;
	}

	return 0;
}

static struct spdk_nbd_disk *spdk_nbd_create_construct(void)
{
	struct spdk_nbd_disk *nbd = NULL;

	nbd = calloc(1, sizeof(*nbd));
	if (nbd == NULL) {
		return NULL;
	}

	if (spdk_nbd_initialize_io_pool(nbd)) {
		free(nbd);
		return NULL;
	}

	TAILQ_INIT(&nbd->recv_io_list);
	TAILQ_INIT(&nbd->send_io_list);
	nbd->cur_rio = NULL;
	nbd->cur_sio = NULL;

	nbd->dev_fd = -1;
	nbd->spdk_sp_fd = -1;
	nbd->kernel_sp_fd = -1;
	nbd->leave = false;

	return nbd;
}

static struct nbd_io *
nbd_io_idle_obtain(struct spdk_nbd_disk *nbd)
{
	struct nbd_io *rio = NULL;

	rte_mempool_get(nbd->io_pool, (void **)&rio);

	if (rio) {
		rio->poller_func = nbd_io_recv_req;
		rio->payload = NULL;
		rio->offset = 0;
		rio->nbd = nbd;
	}

	return rio;
}

static struct nbd_io *
nbd_io_done_obtain(struct spdk_nbd_disk *nbd)
{
	struct nbd_io *sio = NULL;

	if (!TAILQ_EMPTY(&nbd->send_io_list)) {
		sio = TAILQ_FIRST(&nbd->send_io_list);
		TAILQ_REMOVE(&nbd->send_io_list, sio, tailq);

		sio->poller_func = nbd_io_send_resp;
	}

	return sio;
}

static int
nbd_io_putback(struct nbd_io *io)
{
	struct spdk_nbd_disk *nbd = io->nbd;

	if (io->payload)
		spdk_dma_free(io->payload);

	io->poller_func = nbd_io_poller_default;
	rte_mempool_put(nbd->io_pool, (void *)io);

	return 0;
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

	if (nbd->spdk_sp_fd >= 0) {
		close(nbd->spdk_sp_fd);
	}

	if (nbd->kernel_sp_fd >= 0) {
		close(nbd->kernel_sp_fd);
	}

	rte_mempool_free(nbd->io_pool);
	free(nbd);
}

void
spdk_nbd_stop(struct spdk_nbd_disk *nbd)
{
	struct nbd_io *io, *io_tmp;

	if (nbd == NULL) {
		return;
	}

	nbd->leave = true;
	/*
	 * Free nbd io in send io list, cur_rio and cur_sio
	 */
	if (nbd->cur_rio) {
		nbd_io_putback(nbd->cur_rio);
	}

	if (nbd->cur_sio) {
		nbd_io_putback(nbd->cur_sio);
	}

	TAILQ_FOREACH_SAFE(io, &nbd->send_io_list, tailq, io_tmp) {
		nbd_io_putback(io);
	}

	if (TAILQ_EMPTY(&nbd->recv_io_list)) {
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

static int
nbd_io_poller_default(struct nbd_io *io)
{
	struct spdk_nbd_disk *nbd = io->nbd;

	SPDK_ERRLOG("Invalid NBD IO stage for poller\n");

	return -EIO;
}

static int
nbd_io_recv_req(struct nbd_io *io)
{
	struct spdk_nbd_disk *nbd = io->nbd;
	int		ret;

	ret = read_from_socket(nbd->spdk_sp_fd, (char *)&io->req + io->offset,
			       sizeof(io->req) - io->offset);
	if (ret <= 0) {
		return ret;
	}

	io->offset += ret;
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
		}

		/* next IO step */
		switch (from_be32(&io->req.type)) {
		case NBD_CMD_WRITE:
			io->poller_func = nbd_io_recv_payload;
			break;
		default:
			io->poller_func = nbd_io_poller_default;
			/* transfer io into recv list */
			TAILQ_INSERT_TAIL(&nbd->recv_io_list, io, tailq);
			ret = process_request(nbd, io);
			if (ret != 0) {
				return ret;
			}
			break;
		}
	}

	return ret;
}

static int
nbd_io_recv_payload(struct nbd_io *io)
{
	struct spdk_nbd_disk *nbd = io->nbd;
	int		ret;

	ret = read_from_socket(nbd->spdk_sp_fd, io->payload + io->offset, io->payload_size - io->offset);
	if (ret <= 0) {
		return ret;
	}

	io->offset += ret;
	if (io->offset == io->payload_size) {
		io->offset = 0;

		io->poller_func = nbd_io_poller_default;
		/* transfer io into recv list */
		TAILQ_INSERT_TAIL(&nbd->recv_io_list, io, tailq);
		ret = process_request(nbd, io);
		if (ret != 0) {
			return ret;
		}
	}

	return ret;
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
	if (io->offset == sizeof(io->resp)) {
		io->offset = 0;

		/* next IO step */
		switch (from_be32(&io->req.type)) {
		case NBD_CMD_READ:
			io->poller_func = nbd_io_send_payload;
			break;
		default:
			nbd_io_putback(io);
			break;
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
	if (io->offset == io->payload_size) {
		io->offset = 0;

		nbd_io_putback(io);
	}

	return ret;
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

	if (!nbd->leave) {
		/* transfer IO from recv list to send list */
		TAILQ_REMOVE(&nbd->recv_io_list, io, tailq);
		TAILQ_INSERT_TAIL(&nbd->send_io_list, io, tailq);
	} else {
		/* transfer IO from recv list back to io-pool */
		TAILQ_REMOVE(&nbd->recv_io_list, io, tailq);
		nbd_io_putback(io);
		if (TAILQ_EMPTY(&nbd->recv_io_list)) {
			_nbd_stop(nbd);
		}
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
process_request(struct spdk_nbd_disk *nbd, struct nbd_io *io)
{
	switch (from_be32(&io->req.type)) {
	case NBD_CMD_DISC:
		return -ECONNRESET;
	case NBD_CMD_READ:
		io->type = SPDK_BDEV_IO_TYPE_READ;
		break;
	case NBD_CMD_WRITE:
		io->type = SPDK_BDEV_IO_TYPE_WRITE;
		break;
#ifdef NBD_FLAG_SEND_FLUSH
	case NBD_CMD_FLUSH:
		io->type = SPDK_BDEV_IO_TYPE_FLUSH;
		break;
#endif
#ifdef NBD_FLAG_SEND_TRIM
	case NBD_CMD_TRIM:
		io->type = SPDK_BDEV_IO_TYPE_UNMAP;
		break;
#endif
	default:
		return -EIO;
	}

	nbd_submit_bdev_io(nbd->bdev, nbd->bdev_desc, nbd->ch, io);

	return 0;
}

int
spdk_nbd_poll(struct spdk_nbd_disk *nbd)
{
	struct nbd_io	*io;
	int		rc;

	/* socket read progress */
	if (!nbd->cur_rio) {
		nbd->cur_rio = nbd_io_idle_obtain(nbd);
	}

	if (nbd->cur_rio) {
		io = nbd->cur_rio;
		rc = io->poller_func(io);
		if (rc < 0) {
			return rc;
		}

		if (io->poller_func == nbd_io_poller_default)
			nbd->cur_rio = NULL;
	}

	/* socket write progress */
	if (!nbd->cur_sio) {
		nbd->cur_sio = nbd_io_done_obtain(nbd);
	}

	if (nbd->cur_sio) {
		io = nbd->cur_sio;
		rc = io->poller_func(io);
		if (rc < 0) {
			return rc;
		}

		if (io->poller_func == nbd_io_poller_default)
			nbd->cur_sio = NULL;
	}

	return 0;
}

static void *
nbd_start_kernel(void *arg)
{
	struct spdk_nbd_disk *nbd = arg;
	int rc;
	char buf[64];

	spdk_unaffinitize_thread();

	rc = ioctl(nbd->dev_fd, NBD_SET_SOCK, nbd->kernel_sp_fd);
	if (rc == -1) {
		spdk_strerror_r(errno, buf, sizeof(buf));
		SPDK_ERRLOG("ioctl(NBD_SET_SOCK) failed: %s\n", buf);
		pthread_exit(NULL);
	}

#ifdef NBD_FLAG_SEND_TRIM
	rc = ioctl(nbd->dev_fd, NBD_SET_FLAGS, NBD_FLAG_SEND_TRIM);
	if (rc == -1) {
		spdk_strerror_r(errno, buf, sizeof(buf));
		SPDK_ERRLOG("ioctl(NBD_SET_FLAGS) failed: %s\n", buf);
		pthread_exit(NULL);
	}
#endif

	/* This will block in the kernel until we close the spdk_sp_fd. */
	ioctl(nbd->dev_fd, NBD_DO_IT);
	SPDK_ERRLOG("client thread exit\n");
	ioctl(nbd->dev_fd, NBD_CLEAR_QUE);
	ioctl(nbd->dev_fd, NBD_CLEAR_SOCK);
	pthread_exit(NULL);
}

struct spdk_nbd_disk *
spdk_nbd_start(struct spdk_bdev *bdev, const char *nbd_path)
{
	struct spdk_nbd_disk	*nbd;
	pthread_t		tid;
	int			rc;
	int			sp[2];
	char			buf[64];

	nbd = spdk_nbd_create_construct();

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

	nbd->spdk_sp_fd = sp[0];
	nbd->kernel_sp_fd = sp[1];
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

	printf("Enabling kernel access to bdev %s via %s\n", spdk_bdev_get_name(bdev), nbd_path);

	rc = pthread_create(&tid, NULL, &nbd_start_kernel, nbd);
	if (rc != 0) {
		spdk_strerror_r(rc, buf, sizeof(buf));
		SPDK_ERRLOG("could not create thread: %s\n", buf);
		goto err;
	}

	fcntl(nbd->spdk_sp_fd, F_SETFL, O_NONBLOCK);

	return nbd;

err:
	spdk_nbd_stop(nbd);

	return NULL;
}
