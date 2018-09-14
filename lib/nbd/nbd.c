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
#include "spdk/thread.h"
#include "spdk/event.h"

#include "spdk_internal/log.h"
#include "spdk/queue.h"

#define GET_IO_LOOP_COUNT	16

enum nbd_io_state_t {
	/* Receiving or ready to receive nbd request header */
	NBD_IO_RECV_REQ = 0,
	/* Receiving write payload */
	NBD_IO_RECV_PAYLOAD,
	/* Transmitting or ready to transmit nbd response header */
	NBD_IO_XMIT_RESP,
	/* Transmitting read payload */
	NBD_IO_XMIT_PAYLOAD,
};

struct nbd_io {
	struct spdk_nbd_disk	*nbd;
	enum nbd_io_state_t	state;

	void			*payload;
	uint32_t		payload_size;

	struct nbd_request	req;
	struct nbd_reply	resp;

	/*
	 * Tracks current progress on reading/writing a request,
	 * response, or payload from the nbd socket.
	 */
	uint32_t		offset;

	/* for bdev io_wait */
	struct spdk_bdev_io_wait_entry bdev_io_wait;

	TAILQ_ENTRY(nbd_io)	tailq;
};

enum nbd_disk_state_t {
	NBD_DISK_STATE_RUNNING = 0,
	/* soft disconnection caused by receiving nbd_cmd_disc */
	NBD_DISK_STATE_SOFTDISC,
	/* hard disconnection caused by mandatory conditions */
	NBD_DISK_STATE_HARDDISC,
};

struct spdk_nbd_disk {
	struct spdk_bdev	*bdev;
	struct spdk_bdev_desc	*bdev_desc;
	struct spdk_io_channel	*ch;
	int			dev_fd;
	char			*nbd_path;
	int			kernel_sp_fd;
	int			spdk_sp_fd;
	struct spdk_poller	*nbd_poller;
	uint32_t		buf_align;

	struct nbd_io		*io_in_recv;
	TAILQ_HEAD(, nbd_io)	received_io_list;
	TAILQ_HEAD(, nbd_io)	executed_io_list;

	enum nbd_disk_state_t	state;
	/* count of nbd_io in spdk_nbd_disk */
	int			io_count;

	TAILQ_ENTRY(spdk_nbd_disk)	tailq;
};

struct spdk_nbd_disk_globals {
	TAILQ_HEAD(, spdk_nbd_disk)	disk_head;
};

static struct spdk_nbd_disk_globals g_spdk_nbd;

static int
nbd_submit_bdev_io(struct spdk_nbd_disk *nbd, struct nbd_io *io);

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
spdk_nbd_write_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_nbd_disk *nbd;

	spdk_json_write_array_begin(w);

	TAILQ_FOREACH(nbd, &g_spdk_nbd.disk_head, tailq) {
		spdk_json_write_object_begin(w);

		spdk_json_write_named_string(w, "method", "start_nbd_disk");

		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "nbd_device",  spdk_nbd_disk_get_nbd_path(nbd));
		spdk_json_write_named_string(w, "bdev_name", spdk_nbd_disk_get_bdev_name(nbd));
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}

	spdk_json_write_array_end(w);
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

static struct nbd_io *
spdk_get_nbd_io(struct spdk_nbd_disk *nbd)
{
	struct nbd_io *io;

	io = calloc(1, sizeof(*io));
	if (!io) {
		return NULL;
	}

	io->nbd = nbd;
	to_be32(&io->resp.magic, NBD_REPLY_MAGIC);

	nbd->io_count++;

	return io;
}

static void
spdk_put_nbd_io(struct spdk_nbd_disk *nbd, struct nbd_io *io)
{
	if (io->payload) {
		spdk_dma_free(io->payload);
	}
	free(io);

	nbd->io_count--;
}

/*
 * Check whether received nbd_io are all transmitted.
 *
 * \return 1 there is still some nbd_io not transmitted.
 *         0 all nbd_io received are transmitted.
 */
static int
spdk_nbd_io_xmit_check(struct spdk_nbd_disk *nbd)
{
	if (nbd->io_count == 0) {
		return 0;
	} else if (nbd->io_count == 1 && nbd->io_in_recv != NULL) {
		return 0;
	}

	return 1;
}

/*
 * Check whether received nbd_io are all executed,
 * and put back executed nbd_io instead of transmitting them
 *
 * \return 1 there is still some nbd_io under executing
 *         0 all nbd_io gotten are freed.
 */
static int
spdk_nbd_cleanup_io(struct spdk_nbd_disk *nbd)
{
	struct nbd_io *io, *io_tmp;

	/* free io_in_recv */
	if (nbd->io_in_recv != NULL) {
		spdk_put_nbd_io(nbd, nbd->io_in_recv);
		nbd->io_in_recv = NULL;
	}

	/* free io in received_io_list */
	if (!TAILQ_EMPTY(&nbd->received_io_list)) {
		TAILQ_FOREACH_SAFE(io, &nbd->received_io_list, tailq, io_tmp) {
			TAILQ_REMOVE(&nbd->received_io_list, io, tailq);
			spdk_put_nbd_io(nbd, io);
		}
	}

	/* free io in executed_io_list */
	if (!TAILQ_EMPTY(&nbd->executed_io_list)) {
		TAILQ_FOREACH_SAFE(io, &nbd->executed_io_list, tailq, io_tmp) {
			TAILQ_REMOVE(&nbd->executed_io_list, io, tailq);
			spdk_put_nbd_io(nbd, io);
		}
	}

	/*
	 * Some nbd_io may be under executing in bdev.
	 * Wait for their done operation.
	 */
	if (nbd->io_count != 0) {
		return 1;
	}

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

	if (nbd->nbd_path) {
		free(nbd->nbd_path);
	}

	if (nbd->spdk_sp_fd >= 0) {
		close(nbd->spdk_sp_fd);
	}

	if (nbd->kernel_sp_fd >= 0) {
		close(nbd->kernel_sp_fd);
	}

	if (nbd->dev_fd >= 0) {
		ioctl(nbd->dev_fd, NBD_CLEAR_QUE);
		ioctl(nbd->dev_fd, NBD_CLEAR_SOCK);
		close(nbd->dev_fd);
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

	nbd->state = NBD_DISK_STATE_HARDDISC;

	/*
	 * Stop action should be called only after all nbd_io are executed.
	 */
	if (!spdk_nbd_cleanup_io(nbd)) {
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

	memcpy(&io->resp.handle, &io->req.handle, sizeof(io->resp.handle));
	TAILQ_INSERT_TAIL(&nbd->executed_io_list, io, tailq);

	if (bdev_io != NULL) {
		spdk_bdev_free_io(bdev_io);
	}

	if (nbd->state == NBD_DISK_STATE_HARDDISC && !spdk_nbd_cleanup_io(nbd)) {
		_nbd_stop(nbd);
	}
}

static void
nbd_resubmit_io(void *arg)
{
	struct nbd_io *io = (struct nbd_io *)arg;
	struct spdk_nbd_disk *nbd = io->nbd;
	int rc = 0;

	rc = nbd_submit_bdev_io(nbd, io);
	if (rc) {
		SPDK_INFOLOG(SPDK_LOG_NBD, "nbd: io resubmit for dev %s , io_type %d, returned %d.\n",
			     spdk_nbd_disk_get_bdev_name(nbd), from_be32(&io->req.type), rc);
	}
}

static void
nbd_queue_io(struct nbd_io *io)
{
	int rc;
	struct spdk_bdev *bdev = io->nbd->bdev;

	io->bdev_io_wait.bdev = bdev;
	io->bdev_io_wait.cb_fn = nbd_resubmit_io;
	io->bdev_io_wait.cb_arg = io;

	rc = spdk_bdev_queue_io_wait(bdev, io->nbd->ch, &io->bdev_io_wait);
	if (rc != 0) {
		SPDK_ERRLOG("Queue io failed in nbd_queue_io, rc=%d.\n", rc);
		nbd_io_done(NULL, false, io);
	}
}

static int
nbd_submit_bdev_io(struct spdk_nbd_disk *nbd, struct nbd_io *io)
{
	struct spdk_bdev_desc *desc = nbd->bdev_desc;
	struct spdk_io_channel *ch = nbd->ch;
	int rc = 0;

	switch (from_be32(&io->req.type)) {
	case NBD_CMD_READ:
		rc = spdk_bdev_read(desc, ch, io->payload, from_be64(&io->req.from),
				    io->payload_size, nbd_io_done, io);
		break;
	case NBD_CMD_WRITE:
		rc = spdk_bdev_write(desc, ch, io->payload, from_be64(&io->req.from),
				     io->payload_size, nbd_io_done, io);
		break;
#ifdef NBD_FLAG_SEND_FLUSH
	case NBD_CMD_FLUSH:
		rc = spdk_bdev_flush(desc, ch, 0,
				     spdk_bdev_get_num_blocks(nbd->bdev) * spdk_bdev_get_block_size(nbd->bdev),
				     nbd_io_done, io);
		break;
#endif
#ifdef NBD_FLAG_SEND_TRIM
	case NBD_CMD_TRIM:
		rc = spdk_bdev_unmap(desc, ch, from_be64(&io->req.from),
				     from_be32(&io->req.len), nbd_io_done, io);
		break;
#endif
	case NBD_CMD_DISC:
		spdk_put_nbd_io(nbd, io);
		nbd->state = NBD_DISK_STATE_SOFTDISC;
		break;
	default:
		rc = -1;
	}

	if (rc < 0) {
		if (rc == -ENOMEM) {
			SPDK_INFOLOG(SPDK_LOG_NBD, "No memory, start to queue io.\n");
			nbd_queue_io(io);
		} else {
			SPDK_ERRLOG("nbd io failed in nbd_queue_io, rc=%d.\n", rc);
			nbd_io_done(NULL, false, io);
		}
	}

	return 0;
}

static int
spdk_nbd_io_exec(struct spdk_nbd_disk *nbd)
{
	struct nbd_io *io, *io_tmp;
	int ret = 0;

	/*
	 * For soft disconnection, nbd server must handle all outstanding
	 * request before closing connection.
	 */
	if (nbd->state == NBD_DISK_STATE_HARDDISC) {
		return 0;
	}

	if (!TAILQ_EMPTY(&nbd->received_io_list)) {
		TAILQ_FOREACH_SAFE(io, &nbd->received_io_list, tailq, io_tmp) {
			TAILQ_REMOVE(&nbd->received_io_list, io, tailq);
			ret = nbd_submit_bdev_io(nbd, io);
			if (ret < 0) {
				break;
			}
		}
	}

	return ret;
}

static int
spdk_nbd_io_recv_internal(struct spdk_nbd_disk *nbd)
{
	struct nbd_io *io;
	int ret = 0;

	if (nbd->io_in_recv == NULL) {
		nbd->io_in_recv = spdk_get_nbd_io(nbd);
		if (!nbd->io_in_recv) {
			return -ENOMEM;
		}
	}

	io = nbd->io_in_recv;

	if (io->state == NBD_IO_RECV_REQ) {
		ret = read_from_socket(nbd->spdk_sp_fd, (char *)&io->req + io->offset,
				       sizeof(io->req) - io->offset);
		if (ret < 0) {
			spdk_put_nbd_io(nbd, io);
			nbd->io_in_recv = NULL;
			return ret;
		}

		io->offset += ret;

		/* request is fully received */
		if (io->offset == sizeof(io->req)) {
			io->offset = 0;

			/* req magic check */
			if (from_be32(&io->req.magic) != NBD_REQUEST_MAGIC) {
				SPDK_ERRLOG("invalid request magic\n");
				spdk_put_nbd_io(nbd, io);
				nbd->io_in_recv = NULL;
				return -EINVAL;
			}

			/* io except read/write should ignore payload */
			if (from_be32(&io->req.type) == NBD_CMD_WRITE ||
			    from_be32(&io->req.type) == NBD_CMD_READ) {
				io->payload_size = from_be32(&io->req.len);
			} else {
				io->payload_size = 0;
			}

			/* io payload allocate */
			if (io->payload_size) {
				io->payload = spdk_dma_malloc(io->payload_size, nbd->buf_align, NULL);
				if (io->payload == NULL) {
					SPDK_ERRLOG("could not allocate io->payload of size %d\n", io->payload_size);
					spdk_put_nbd_io(nbd, io);
					nbd->io_in_recv = NULL;
					return -ENOMEM;
				}
			} else {
				io->payload = NULL;
			}

			/* next io step */
			if (from_be32(&io->req.type) == NBD_CMD_WRITE) {
				io->state = NBD_IO_RECV_PAYLOAD;
			} else {
				io->state = NBD_IO_XMIT_RESP;
				nbd->io_in_recv = NULL;
				TAILQ_INSERT_TAIL(&nbd->received_io_list, io, tailq);
			}
		}
	}

	if (io->state == NBD_IO_RECV_PAYLOAD) {
		ret = read_from_socket(nbd->spdk_sp_fd, io->payload + io->offset, io->payload_size - io->offset);
		if (ret < 0) {
			spdk_put_nbd_io(nbd, io);
			nbd->io_in_recv = NULL;
			return ret;
		}

		io->offset += ret;

		/* request payload is fully received */
		if (io->offset == io->payload_size) {
			io->offset = 0;
			io->state = NBD_IO_XMIT_RESP;
			nbd->io_in_recv = NULL;
			TAILQ_INSERT_TAIL(&nbd->received_io_list, io, tailq);
		}

	}

	return 0;
}

static int
spdk_nbd_io_recv(struct spdk_nbd_disk *nbd)
{
	int i, ret = 0;

	/*
	 * nbd server should not accept request in both soft and hard
	 * disconnect states.
	 */
	if (nbd->state != NBD_DISK_STATE_RUNNING) {
		return 0;
	}

	for (i = 0; i < GET_IO_LOOP_COUNT; i++) {
		ret = spdk_nbd_io_recv_internal(nbd);
		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

static int
spdk_nbd_io_xmit_internal(struct spdk_nbd_disk *nbd)
{
	struct nbd_io *io;
	int ret = 0;

	io = TAILQ_FIRST(&nbd->executed_io_list);
	if (io == NULL) {
		return 0;
	}

	/* Remove IO from list now assuming it will be completed.  It will be inserted
	 *  back to the head if it cannot be completed.  This approach is specifically
	 *  taken to work around a scan-build use-after-free mischaracterization.
	 */
	TAILQ_REMOVE(&nbd->executed_io_list, io, tailq);

	/* resp error and handler are already set in io_done */

	if (io->state == NBD_IO_XMIT_RESP) {
		ret = write_to_socket(nbd->spdk_sp_fd, (char *)&io->resp + io->offset,
				      sizeof(io->resp) - io->offset);
		if (ret <= 0) {
			goto reinsert;
		}

		io->offset += ret;

		/* response is fully transmitted */
		if (io->offset == sizeof(io->resp)) {
			io->offset = 0;

			/* transmit payload only when NBD_CMD_READ with no resp error */
			if (from_be32(&io->req.type) != NBD_CMD_READ || io->resp.error != 0) {
				spdk_put_nbd_io(nbd, io);
				return 0;
			} else {
				io->state = NBD_IO_XMIT_PAYLOAD;
			}
		}
	}

	if (io->state == NBD_IO_XMIT_PAYLOAD) {
		ret = write_to_socket(nbd->spdk_sp_fd, io->payload + io->offset, io->payload_size - io->offset);
		if (ret <= 0) {
			goto reinsert;
		}

		io->offset += ret;

		/* read payload is fully transmitted */
		if (io->offset == io->payload_size) {
			spdk_put_nbd_io(nbd, io);
			return 0;
		}
	}

reinsert:
	TAILQ_INSERT_HEAD(&nbd->executed_io_list, io, tailq);
	return ret;
}

static int
spdk_nbd_io_xmit(struct spdk_nbd_disk *nbd)
{
	int ret = 0;

	/*
	 * For soft disconnection, nbd server must handle all outstanding
	 * request before closing connection.
	 */
	if (nbd->state == NBD_DISK_STATE_HARDDISC) {
		return 0;
	}

	while (!TAILQ_EMPTY(&nbd->executed_io_list)) {
		ret = spdk_nbd_io_xmit_internal(nbd);
		if (ret != 0) {
			return ret;
		}
	}

	/*
	 * For soft disconnection, nbd server can close connection after all
	 * outstanding request are transmitted.
	 */
	if (nbd->state == NBD_DISK_STATE_SOFTDISC && !spdk_nbd_io_xmit_check(nbd)) {
		return -1;
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
	int rc;

	/* transmit executed io first */
	rc = spdk_nbd_io_xmit(nbd);
	if (rc < 0) {
		return rc;
	}

	rc = spdk_nbd_io_recv(nbd);
	if (rc < 0) {
		return rc;
	}

	rc = spdk_nbd_io_exec(nbd);

	return rc;
}

static int
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

	return -1;
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

	TAILQ_INIT(&nbd->received_io_list);
	TAILQ_INIT(&nbd->executed_io_list);

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

	nbd->nbd_poller = spdk_poller_register(spdk_nbd_poll, nbd, 0);

	return nbd;

err:
	spdk_nbd_stop(nbd);

	return NULL;
}

SPDK_LOG_REGISTER_COMPONENT("nbd", SPDK_LOG_NBD)
