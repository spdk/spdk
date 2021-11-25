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
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/thread.h"

#include "spdk/queue.h"

#define GET_IO_LOOP_COUNT		16
#define NBD_START_BUSY_WAITING_MS	1000
#define NBD_STOP_BUSY_WAITING_MS	10000
#define NBD_BUSY_POLLING_INTERVAL_US	20000
#define NBD_IO_TIMEOUT_S		60

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

struct spdk_nbd_disk {
	struct spdk_bdev	*bdev;
	struct spdk_bdev_desc	*bdev_desc;
	struct spdk_io_channel	*ch;
	int			dev_fd;
	char			*nbd_path;
	int			kernel_sp_fd;
	int			spdk_sp_fd;
	struct spdk_poller	*nbd_poller;
	struct spdk_interrupt	*intr;
	bool			interrupt_mode;
	uint32_t		buf_align;

	struct spdk_poller	*retry_poller;
	int			retry_count;
	/* Synchronize nbd_start_kernel pthread and nbd_stop */
	bool			has_nbd_pthread;

	struct nbd_io		*io_in_recv;
	TAILQ_HEAD(, nbd_io)	received_io_list;
	TAILQ_HEAD(, nbd_io)	executed_io_list;
	TAILQ_HEAD(, nbd_io)	processing_io_list;

	bool			is_started;
	bool			is_closing;
	/* count of nbd_io in spdk_nbd_disk */
	int			io_count;

	TAILQ_ENTRY(spdk_nbd_disk)	tailq;
};

struct spdk_nbd_disk_globals {
	TAILQ_HEAD(, spdk_nbd_disk)	disk_head;
};

static struct spdk_nbd_disk_globals g_spdk_nbd;
static spdk_nbd_fini_cb g_fini_cb_fn;
static void *g_fini_cb_arg;

static void _nbd_fini(void *arg1);

static int
nbd_submit_bdev_io(struct spdk_nbd_disk *nbd, struct nbd_io *io);
static int
nbd_io_recv_internal(struct spdk_nbd_disk *nbd);

int
spdk_nbd_init(void)
{
	TAILQ_INIT(&g_spdk_nbd.disk_head);

	return 0;
}

static void
_nbd_fini(void *arg1)
{
	struct spdk_nbd_disk *nbd, *nbd_tmp;

	TAILQ_FOREACH_SAFE(nbd, &g_spdk_nbd.disk_head, tailq, nbd_tmp) {
		if (!nbd->is_closing) {
			spdk_nbd_stop(nbd);
		}
	}

	/* Check if all nbds closed */
	if (!TAILQ_FIRST(&g_spdk_nbd.disk_head)) {
		g_fini_cb_fn(g_fini_cb_arg);
	} else {
		spdk_thread_send_msg(spdk_get_thread(),
				     _nbd_fini, NULL);
	}
}

void
spdk_nbd_fini(spdk_nbd_fini_cb cb_fn, void *cb_arg)
{
	g_fini_cb_fn = cb_fn;
	g_fini_cb_arg = cb_arg;

	_nbd_fini(NULL);
}

static int
nbd_disk_register(struct spdk_nbd_disk *nbd)
{
	/* Make sure nbd_path is not used in this SPDK app */
	if (nbd_disk_find_by_nbd_path(nbd->nbd_path)) {
		SPDK_NOTICELOG("%s is already exported\n", nbd->nbd_path);
		return -EBUSY;
	}

	TAILQ_INSERT_TAIL(&g_spdk_nbd.disk_head, nbd, tailq);

	return 0;
}

static void
nbd_disk_unregister(struct spdk_nbd_disk *nbd)
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
nbd_disk_find_by_nbd_path(const char *nbd_path)
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

struct spdk_nbd_disk *nbd_disk_first(void)
{
	return TAILQ_FIRST(&g_spdk_nbd.disk_head);
}

struct spdk_nbd_disk *nbd_disk_next(struct spdk_nbd_disk *prev)
{
	return TAILQ_NEXT(prev, tailq);
}

const char *
nbd_disk_get_nbd_path(struct spdk_nbd_disk *nbd)
{
	return nbd->nbd_path;
}

const char *
nbd_disk_get_bdev_name(struct spdk_nbd_disk *nbd)
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

		spdk_json_write_named_string(w, "method", "nbd_start_disk");

		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "nbd_device",  nbd_disk_get_nbd_path(nbd));
		spdk_json_write_named_string(w, "bdev_name", nbd_disk_get_bdev_name(nbd));
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
nbd_get_io(struct spdk_nbd_disk *nbd)
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
nbd_put_io(struct spdk_nbd_disk *nbd, struct nbd_io *io)
{
	if (io->payload) {
		spdk_free(io->payload);
	}
	free(io);

	nbd->io_count--;
}

/*
 * Check whether received nbd_io are all executed,
 * and put back executed nbd_io instead of transmitting them
 *
 * \return 1 there is still some nbd_io under executing
 *         0 all nbd_io gotten are freed.
 */
static int
nbd_cleanup_io(struct spdk_nbd_disk *nbd)
{
	/* Try to read the remaining nbd commands in the socket */
	while (nbd_io_recv_internal(nbd) > 0);

	/* free io_in_recv */
	if (nbd->io_in_recv != NULL) {
		nbd_put_io(nbd, nbd->io_in_recv);
		nbd->io_in_recv = NULL;
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

static int
_nbd_stop(void *arg)
{
	struct spdk_nbd_disk *nbd = arg;

	if (nbd->nbd_poller) {
		spdk_poller_unregister(&nbd->nbd_poller);
	}

	if (nbd->intr) {
		spdk_interrupt_unregister(&nbd->intr);
	}

	if (nbd->spdk_sp_fd >= 0) {
		close(nbd->spdk_sp_fd);
		nbd->spdk_sp_fd = -1;
	}

	if (nbd->kernel_sp_fd >= 0) {
		close(nbd->kernel_sp_fd);
		nbd->kernel_sp_fd = -1;
	}

	/* Continue the stop procedure after the exit of nbd_start_kernel pthread */
	if (nbd->has_nbd_pthread) {
		if (nbd->retry_poller == NULL) {
			nbd->retry_count = NBD_STOP_BUSY_WAITING_MS * 1000ULL / NBD_BUSY_POLLING_INTERVAL_US;
			nbd->retry_poller = SPDK_POLLER_REGISTER(_nbd_stop, nbd,
					    NBD_BUSY_POLLING_INTERVAL_US);
			return SPDK_POLLER_BUSY;
		}

		if (nbd->retry_count-- > 0) {
			return SPDK_POLLER_BUSY;
		}

		SPDK_ERRLOG("Failed to wait for returning of NBD_DO_IT ioctl.\n");
	}

	if (nbd->retry_poller) {
		spdk_poller_unregister(&nbd->retry_poller);
	}

	if (nbd->dev_fd >= 0) {
		/* Clear nbd device only if it is occupied by SPDK app */
		if (nbd->nbd_path && nbd_disk_find_by_nbd_path(nbd->nbd_path)) {
			ioctl(nbd->dev_fd, NBD_CLEAR_QUE);
			ioctl(nbd->dev_fd, NBD_CLEAR_SOCK);
		}
		close(nbd->dev_fd);
	}

	if (nbd->nbd_path) {
		free(nbd->nbd_path);
	}

	if (nbd->ch) {
		spdk_put_io_channel(nbd->ch);
		nbd->ch = NULL;
	}

	if (nbd->bdev_desc) {
		spdk_bdev_close(nbd->bdev_desc);
		nbd->bdev_desc = NULL;
	}

	nbd_disk_unregister(nbd);

	free(nbd);

	return 0;
}

int
spdk_nbd_stop(struct spdk_nbd_disk *nbd)
{
	int rc = 0;

	if (nbd == NULL) {
		return rc;
	}

	nbd->is_closing = true;

	/* if nbd is not started, it will continue to call nbd stop later */
	if (!nbd->is_started) {
		return 1;
	}

	/*
	 * Stop action should be called only after all nbd_io are executed.
	 */

	rc = nbd_cleanup_io(nbd);
	if (!rc) {
		_nbd_stop(nbd);
	}

	return rc;
}

static int64_t
nbd_socket_rw(int fd, void *buf, size_t length, bool read_op)
{
	ssize_t rc;

	if (read_op) {
		rc = read(fd, buf, length);
	} else {
		rc = write(fd, buf, length);
	}

	if (rc == 0) {
		return -EIO;
	} else if (rc == -1) {
		if (errno != EAGAIN) {
			return -errno;
		}
		return 0;
	} else {
		return rc;
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

	/* When there begins to have executed_io, enable socket writable notice in order to
	 * get it processed in nbd_io_xmit
	 */
	if (nbd->interrupt_mode && TAILQ_EMPTY(&nbd->executed_io_list)) {
		spdk_interrupt_set_event_types(nbd->intr, SPDK_INTERRUPT_EVENT_IN | SPDK_INTERRUPT_EVENT_OUT);
	}

	TAILQ_REMOVE(&nbd->processing_io_list, io, tailq);
	TAILQ_INSERT_TAIL(&nbd->executed_io_list, io, tailq);

	if (bdev_io != NULL) {
		spdk_bdev_free_io(bdev_io);
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
		SPDK_INFOLOG(nbd, "nbd: io resubmit for dev %s , io_type %d, returned %d.\n",
			     nbd_disk_get_bdev_name(nbd), from_be32(&io->req.type), rc);
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
	default:
		rc = -1;
	}

	if (rc < 0) {
		if (rc == -ENOMEM) {
			SPDK_INFOLOG(nbd, "No memory, start to queue io.\n");
			nbd_queue_io(io);
		} else {
			SPDK_ERRLOG("nbd io failed in nbd_queue_io, rc=%d.\n", rc);
			nbd_io_done(NULL, false, io);
		}
	}

	return 0;
}

static int
nbd_io_exec(struct spdk_nbd_disk *nbd)
{
	struct nbd_io *io, *io_tmp;
	int io_count = 0;
	int ret = 0;

	if (!TAILQ_EMPTY(&nbd->received_io_list)) {
		TAILQ_FOREACH_SAFE(io, &nbd->received_io_list, tailq, io_tmp) {
			TAILQ_REMOVE(&nbd->received_io_list, io, tailq);
			TAILQ_INSERT_TAIL(&nbd->processing_io_list, io, tailq);
			ret = nbd_submit_bdev_io(nbd, io);
			if (ret < 0) {
				return ret;
			}

			io_count++;
		}
	}

	return io_count;
}

static int
nbd_io_recv_internal(struct spdk_nbd_disk *nbd)
{
	struct nbd_io *io;
	int ret = 0;
	int received = 0;

	if (nbd->io_in_recv == NULL) {
		nbd->io_in_recv = nbd_get_io(nbd);
		if (!nbd->io_in_recv) {
			return -ENOMEM;
		}
	}

	io = nbd->io_in_recv;

	if (io->state == NBD_IO_RECV_REQ) {
		ret = nbd_socket_rw(nbd->spdk_sp_fd, (char *)&io->req + io->offset,
				    sizeof(io->req) - io->offset, true);
		if (ret < 0) {
			nbd_put_io(nbd, io);
			nbd->io_in_recv = NULL;
			return ret;
		}

		io->offset += ret;
		received = ret;

		/* request is fully received */
		if (io->offset == sizeof(io->req)) {
			io->offset = 0;

			/* req magic check */
			if (from_be32(&io->req.magic) != NBD_REQUEST_MAGIC) {
				SPDK_ERRLOG("invalid request magic\n");
				nbd_put_io(nbd, io);
				nbd->io_in_recv = NULL;
				return -EINVAL;
			}

			if (from_be32(&io->req.type) == NBD_CMD_DISC) {
				nbd->is_closing = true;
				nbd->io_in_recv = NULL;
				if (nbd->interrupt_mode && TAILQ_EMPTY(&nbd->executed_io_list)) {
					spdk_interrupt_set_event_types(nbd->intr, SPDK_INTERRUPT_EVENT_IN | SPDK_INTERRUPT_EVENT_OUT);
				}
				nbd_put_io(nbd, io);
				/* After receiving NBD_CMD_DISC, nbd will not receive any new commands */
				return received;
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
				io->payload = spdk_malloc(io->payload_size, nbd->buf_align, NULL,
							  SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
				if (io->payload == NULL) {
					SPDK_ERRLOG("could not allocate io->payload of size %d\n", io->payload_size);
					nbd_put_io(nbd, io);
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
				if (spdk_likely((!nbd->is_closing) && nbd->is_started)) {
					TAILQ_INSERT_TAIL(&nbd->received_io_list, io, tailq);
				} else {
					TAILQ_INSERT_TAIL(&nbd->processing_io_list, io, tailq);
					nbd_io_done(NULL, false, io);
				}
				nbd->io_in_recv = NULL;
			}
		}
	}

	if (io->state == NBD_IO_RECV_PAYLOAD) {
		ret = nbd_socket_rw(nbd->spdk_sp_fd, io->payload + io->offset, io->payload_size - io->offset, true);
		if (ret < 0) {
			nbd_put_io(nbd, io);
			nbd->io_in_recv = NULL;
			return ret;
		}

		io->offset += ret;
		received += ret;

		/* request payload is fully received */
		if (io->offset == io->payload_size) {
			io->offset = 0;
			io->state = NBD_IO_XMIT_RESP;
			if (spdk_likely((!nbd->is_closing) && nbd->is_started)) {
				TAILQ_INSERT_TAIL(&nbd->received_io_list, io, tailq);
			} else {
				TAILQ_INSERT_TAIL(&nbd->processing_io_list, io, tailq);
				nbd_io_done(NULL, false, io);
			}
			nbd->io_in_recv = NULL;
		}

	}

	return received;
}

static int
nbd_io_recv(struct spdk_nbd_disk *nbd)
{
	int i, rc, ret = 0;

	/*
	 * nbd server should not accept request after closing command
	 */
	if (nbd->is_closing) {
		return 0;
	}

	for (i = 0; i < GET_IO_LOOP_COUNT; i++) {
		rc = nbd_io_recv_internal(nbd);
		if (rc < 0) {
			return rc;
		}
		ret += rc;
		if (nbd->is_closing) {
			break;
		}
	}

	return ret;
}

static int
nbd_io_xmit_internal(struct spdk_nbd_disk *nbd)
{
	struct nbd_io *io;
	int ret = 0;
	int sent = 0;

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
		ret = nbd_socket_rw(nbd->spdk_sp_fd, (char *)&io->resp + io->offset,
				    sizeof(io->resp) - io->offset, false);
		if (ret <= 0) {
			goto reinsert;
		}

		io->offset += ret;
		sent = ret;

		/* response is fully transmitted */
		if (io->offset == sizeof(io->resp)) {
			io->offset = 0;

			/* transmit payload only when NBD_CMD_READ with no resp error */
			if (from_be32(&io->req.type) != NBD_CMD_READ || io->resp.error != 0) {
				nbd_put_io(nbd, io);
				return 0;
			} else {
				io->state = NBD_IO_XMIT_PAYLOAD;
			}
		}
	}

	if (io->state == NBD_IO_XMIT_PAYLOAD) {
		ret = nbd_socket_rw(nbd->spdk_sp_fd, io->payload + io->offset, io->payload_size - io->offset,
				    false);
		if (ret <= 0) {
			goto reinsert;
		}

		io->offset += ret;
		sent += ret;

		/* read payload is fully transmitted */
		if (io->offset == io->payload_size) {
			nbd_put_io(nbd, io);
			return sent;
		}
	}

reinsert:
	TAILQ_INSERT_HEAD(&nbd->executed_io_list, io, tailq);
	return ret < 0 ? ret : sent;
}

static int
nbd_io_xmit(struct spdk_nbd_disk *nbd)
{
	int ret = 0;
	int rc;

	while (!TAILQ_EMPTY(&nbd->executed_io_list)) {
		rc = nbd_io_xmit_internal(nbd);
		if (rc < 0) {
			return rc;
		}

		ret += rc;
	}

	/* When there begins to have no executed_io, disable socket writable notice */
	if (nbd->interrupt_mode) {
		spdk_interrupt_set_event_types(nbd->intr, SPDK_INTERRUPT_EVENT_IN);
	}

	return ret;
}

/**
 * Poll an NBD instance.
 *
 * \return 0 on success or negated errno values on error (e.g. connection closed).
 */
static int
_nbd_poll(struct spdk_nbd_disk *nbd)
{
	int received, sent, executed;

	/* transmit executed io first */
	sent = nbd_io_xmit(nbd);
	if (sent < 0) {
		return sent;
	}

	received = nbd_io_recv(nbd);
	if (received < 0) {
		return received;
	}

	executed = nbd_io_exec(nbd);
	if (executed < 0) {
		return executed;
	}

	return sent + received + executed;
}

static int
nbd_poll(void *arg)
{
	struct spdk_nbd_disk *nbd = arg;
	int rc;

	rc = _nbd_poll(nbd);
	if (rc < 0) {
		SPDK_INFOLOG(nbd, "nbd_poll() returned %s (%d); closing connection\n",
			     spdk_strerror(-rc), rc);
		_nbd_stop(nbd);
		return SPDK_POLLER_IDLE;
	}
	if (nbd->is_closing) {
		spdk_nbd_stop(nbd);
	}

	return SPDK_POLLER_BUSY;
}

static void *
nbd_start_kernel(void *arg)
{
	struct spdk_nbd_disk *nbd = arg;

	spdk_unaffinitize_thread();

	/* This will block in the kernel until we close the spdk_sp_fd. */
	ioctl(nbd->dev_fd, NBD_DO_IT);

	nbd->has_nbd_pthread = false;

	pthread_exit(NULL);
}

static void
nbd_bdev_hot_remove(struct spdk_nbd_disk *nbd)
{
	struct nbd_io *io, *io_tmp;

	nbd->is_closing = true;
	nbd_cleanup_io(nbd);

	if (!TAILQ_EMPTY(&nbd->received_io_list)) {
		TAILQ_FOREACH_SAFE(io, &nbd->received_io_list, tailq, io_tmp) {
			TAILQ_REMOVE(&nbd->received_io_list, io, tailq);
			TAILQ_INSERT_TAIL(&nbd->processing_io_list, io, tailq);
		}
	}
	if (!TAILQ_EMPTY(&nbd->processing_io_list)) {
		TAILQ_FOREACH_SAFE(io, &nbd->processing_io_list, tailq, io_tmp) {
			nbd_io_done(NULL, false, io);
		}
	}
}

static void
nbd_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		  void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		nbd_bdev_hot_remove(event_ctx);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

struct spdk_nbd_start_ctx {
	struct spdk_nbd_disk	*nbd;
	spdk_nbd_start_cb	cb_fn;
	void			*cb_arg;
};

static void
nbd_poller_set_interrupt_mode(struct spdk_poller *poller, void *cb_arg, bool interrupt_mode)
{
	struct spdk_nbd_disk *nbd = cb_arg;

	nbd->interrupt_mode = interrupt_mode;
}

static void
nbd_start_complete(struct spdk_nbd_start_ctx *ctx)
{
	int		rc;
	pthread_t	tid;
	unsigned long	nbd_flags = 0;

	rc = ioctl(ctx->nbd->dev_fd, NBD_SET_BLKSIZE, spdk_bdev_get_block_size(ctx->nbd->bdev));
	if (rc == -1) {
		SPDK_ERRLOG("ioctl(NBD_SET_BLKSIZE) failed: %s\n", spdk_strerror(errno));
		rc = -errno;
		goto err;
	}

	rc = ioctl(ctx->nbd->dev_fd, NBD_SET_SIZE_BLOCKS, spdk_bdev_get_num_blocks(ctx->nbd->bdev));
	if (rc == -1) {
		SPDK_ERRLOG("ioctl(NBD_SET_SIZE_BLOCKS) failed: %s\n", spdk_strerror(errno));
		rc = -errno;
		goto err;
	}

#ifdef NBD_SET_TIMEOUT
	rc = ioctl(ctx->nbd->dev_fd, NBD_SET_TIMEOUT, NBD_IO_TIMEOUT_S);
	if (rc == -1) {
		SPDK_ERRLOG("ioctl(NBD_SET_TIMEOUT) failed: %s\n", spdk_strerror(errno));
		rc = -errno;
		goto err;
	}
#else
	SPDK_NOTICELOG("ioctl(NBD_SET_TIMEOUT) is not supported.\n");
#endif

#ifdef NBD_FLAG_SEND_FLUSH
	nbd_flags |= NBD_FLAG_SEND_FLUSH;
#endif
#ifdef NBD_FLAG_SEND_TRIM
	nbd_flags |= NBD_FLAG_SEND_TRIM;
#endif
	if (nbd_flags) {
		rc = ioctl(ctx->nbd->dev_fd, NBD_SET_FLAGS, nbd_flags);
		if (rc == -1) {
			SPDK_ERRLOG("ioctl(NBD_SET_FLAGS, 0x%lx) failed: %s\n", nbd_flags, spdk_strerror(errno));
			rc = -errno;
			goto err;
		}
	}

	ctx->nbd->has_nbd_pthread = true;
	rc = pthread_create(&tid, NULL, nbd_start_kernel, ctx->nbd);
	if (rc != 0) {
		ctx->nbd->has_nbd_pthread = false;
		SPDK_ERRLOG("could not create thread: %s\n", spdk_strerror(rc));
		rc = -rc;
		goto err;
	}

	rc = pthread_detach(tid);
	if (rc != 0) {
		SPDK_ERRLOG("could not detach thread for nbd kernel: %s\n", spdk_strerror(rc));
		rc = -rc;
		goto err;
	}

	if (spdk_interrupt_mode_is_enabled()) {
		ctx->nbd->intr = SPDK_INTERRUPT_REGISTER(ctx->nbd->spdk_sp_fd, nbd_poll, ctx->nbd);
	}

	ctx->nbd->nbd_poller = SPDK_POLLER_REGISTER(nbd_poll, ctx->nbd, 0);
	spdk_poller_register_interrupt(ctx->nbd->nbd_poller, nbd_poller_set_interrupt_mode, ctx->nbd);

	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->cb_arg, ctx->nbd, 0);
	}

	/* nbd will possibly receive stop command while initing */
	ctx->nbd->is_started = true;

	free(ctx);
	return;

err:
	_nbd_stop(ctx->nbd);
	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->cb_arg, NULL, rc);
	}
	free(ctx);
}

static int
nbd_enable_kernel(void *arg)
{
	struct spdk_nbd_start_ctx *ctx = arg;
	int rc;

	/* Declare device setup by this process */
	rc = ioctl(ctx->nbd->dev_fd, NBD_SET_SOCK, ctx->nbd->kernel_sp_fd);

	if (rc) {
		if (errno == EBUSY) {
			if (ctx->nbd->retry_poller == NULL) {
				ctx->nbd->retry_count = NBD_START_BUSY_WAITING_MS * 1000ULL / NBD_BUSY_POLLING_INTERVAL_US;
				ctx->nbd->retry_poller = SPDK_POLLER_REGISTER(nbd_enable_kernel, ctx,
							 NBD_BUSY_POLLING_INTERVAL_US);
				return SPDK_POLLER_BUSY;
			} else if (ctx->nbd->retry_count-- > 0) {
				/* Repeatedly unregister and register retry poller to avoid scan-build error */
				spdk_poller_unregister(&ctx->nbd->retry_poller);
				ctx->nbd->retry_poller = SPDK_POLLER_REGISTER(nbd_enable_kernel, ctx,
							 NBD_BUSY_POLLING_INTERVAL_US);
				return SPDK_POLLER_BUSY;
			}
		}

		SPDK_ERRLOG("ioctl(NBD_SET_SOCK) failed: %s\n", spdk_strerror(errno));
		if (ctx->nbd->retry_poller) {
			spdk_poller_unregister(&ctx->nbd->retry_poller);
		}

		_nbd_stop(ctx->nbd);

		if (ctx->cb_fn) {
			ctx->cb_fn(ctx->cb_arg, NULL, -errno);
		}

		free(ctx);
		return SPDK_POLLER_BUSY;
	}

	if (ctx->nbd->retry_poller) {
		spdk_poller_unregister(&ctx->nbd->retry_poller);
	}

	nbd_start_complete(ctx);

	return SPDK_POLLER_BUSY;
}

void
spdk_nbd_start(const char *bdev_name, const char *nbd_path,
	       spdk_nbd_start_cb cb_fn, void *cb_arg)
{
	struct spdk_nbd_start_ctx	*ctx = NULL;
	struct spdk_nbd_disk		*nbd = NULL;
	struct spdk_bdev		*bdev;
	int				rc;
	int				sp[2];

	nbd = calloc(1, sizeof(*nbd));
	if (nbd == NULL) {
		rc = -ENOMEM;
		goto err;
	}

	nbd->dev_fd = -1;
	nbd->spdk_sp_fd = -1;
	nbd->kernel_sp_fd = -1;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		rc = -ENOMEM;
		goto err;
	}

	ctx->nbd = nbd;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	rc = spdk_bdev_open_ext(bdev_name, true, nbd_bdev_event_cb, nbd, &nbd->bdev_desc);
	if (rc != 0) {
		SPDK_ERRLOG("could not open bdev %s, error=%d\n", bdev_name, rc);
		goto err;
	}

	bdev = spdk_bdev_desc_get_bdev(nbd->bdev_desc);
	nbd->bdev = bdev;

	nbd->ch = spdk_bdev_get_io_channel(nbd->bdev_desc);
	nbd->buf_align = spdk_max(spdk_bdev_get_buf_align(bdev), 64);

	rc = socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sp);
	if (rc != 0) {
		SPDK_ERRLOG("socketpair failed\n");
		rc = -errno;
		goto err;
	}

	nbd->spdk_sp_fd = sp[0];
	nbd->kernel_sp_fd = sp[1];
	nbd->nbd_path = strdup(nbd_path);
	if (!nbd->nbd_path) {
		SPDK_ERRLOG("strdup allocation failure\n");
		rc = -ENOMEM;
		goto err;
	}

	TAILQ_INIT(&nbd->received_io_list);
	TAILQ_INIT(&nbd->executed_io_list);
	TAILQ_INIT(&nbd->processing_io_list);

	/* Add nbd_disk to the end of disk list */
	rc = nbd_disk_register(ctx->nbd);
	if (rc != 0) {
		goto err;
	}

	nbd->dev_fd = open(nbd_path, O_RDWR | O_DIRECT);
	if (nbd->dev_fd == -1) {
		SPDK_ERRLOG("open(\"%s\") failed: %s\n", nbd_path, spdk_strerror(errno));
		rc = -errno;
		goto err;
	}

	SPDK_INFOLOG(nbd, "Enabling kernel access to bdev %s via %s\n",
		     bdev_name, nbd_path);

	nbd_enable_kernel(ctx);
	return;

err:
	free(ctx);
	if (nbd) {
		_nbd_stop(nbd);
	}

	if (cb_fn) {
		cb_fn(cb_arg, NULL, rc);
	}
}

const char *
spdk_nbd_get_path(struct spdk_nbd_disk *nbd)
{
	return nbd->nbd_path;
}

SPDK_LOG_REGISTER_COMPONENT(nbd)
