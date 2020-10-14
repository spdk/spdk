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

#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/fd.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/util.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/iscsi_spec.h"

#include "spdk/log.h"
#include "spdk/bdev_module.h"

#include "iscsi/iscsi.h"
#include "iscsi/scsi-lowlevel.h"

#include "bdev_iscsi.h"

struct bdev_iscsi_lun;

#define BDEV_ISCSI_CONNECTION_POLL_US 500 /* 0.5 ms */
#define BDEV_ISCSI_NO_MASTER_CH_POLL_US 10000 /* 10ms */

#define DEFAULT_INITIATOR_NAME "iqn.2016-06.io.spdk:init"

static int bdev_iscsi_initialize(void);
static TAILQ_HEAD(, bdev_iscsi_conn_req) g_iscsi_conn_req = TAILQ_HEAD_INITIALIZER(
			g_iscsi_conn_req);
static struct spdk_poller *g_conn_poller = NULL;

struct bdev_iscsi_io {
	struct spdk_thread *submit_td;
	enum spdk_bdev_io_status status;
	int scsi_status;
	enum spdk_scsi_sense sk;
	uint8_t asc;
	uint8_t ascq;
};

struct bdev_iscsi_lun {
	struct spdk_bdev		bdev;
	struct iscsi_context		*context;
	char				*initiator_iqn;
	int				lun_id;
	char				*url;
	pthread_mutex_t			mutex;
	uint32_t			ch_count;
	struct spdk_thread		*master_td;
	struct spdk_poller		*no_master_ch_poller;
	struct spdk_thread		*no_master_ch_poller_td;
	bool				unmap_supported;
	struct spdk_poller		*poller;
};

struct bdev_iscsi_io_channel {
	struct bdev_iscsi_lun	*lun;
};

struct bdev_iscsi_conn_req {
	char					*url;
	char					*bdev_name;
	char					*initiator_iqn;
	struct iscsi_context			*context;
	spdk_bdev_iscsi_create_cb		create_cb;
	void					*create_cb_arg;
	bool					unmap_supported;
	int					lun;
	int					status;
	TAILQ_ENTRY(bdev_iscsi_conn_req)	link;
};

static void
complete_conn_req(struct bdev_iscsi_conn_req *req, struct spdk_bdev *bdev,
		  int status)
{
	TAILQ_REMOVE(&g_iscsi_conn_req, req, link);
	req->create_cb(req->create_cb_arg, bdev, status);

	/*
	 * we are still running in the context of iscsi_service()
	 * so do not tear down its data structures here
	 */
	req->status = status;
}

static int
bdev_iscsi_get_ctx_size(void)
{
	return sizeof(struct bdev_iscsi_io);
}

static void
_iscsi_free_lun(void *arg)
{
	struct bdev_iscsi_lun *lun = arg;

	assert(lun != NULL);
	iscsi_destroy_context(lun->context);
	pthread_mutex_destroy(&lun->mutex);
	free(lun->bdev.name);
	free(lun->url);
	free(lun->initiator_iqn);

	spdk_bdev_destruct_done(&lun->bdev, 0);
	free(lun);
}

static void
_bdev_iscsi_conn_req_free(struct bdev_iscsi_conn_req *req)
{
	free(req->initiator_iqn);
	free(req->bdev_name);
	free(req->url);
	/* destroy will call iscsi_disconnect() implicitly if connected */
	iscsi_destroy_context(req->context);
	free(req);
}

static void
bdev_iscsi_finish(void)
{
	struct bdev_iscsi_conn_req *req, *tmp;

	/* clear out pending connection requests here. We cannot
	 * simply set the state to a non SCSI_STATUS_GOOD state as
	 * the connection poller wont run anymore
	 */
	TAILQ_FOREACH_SAFE(req, &g_iscsi_conn_req, link, tmp) {
		_bdev_iscsi_conn_req_free(req);
	}

	if (g_conn_poller) {
		spdk_poller_unregister(&g_conn_poller);
	}
}

static struct spdk_bdev_module g_iscsi_bdev_module = {
	.name		= "iscsi",
	.module_init	= bdev_iscsi_initialize,
	.module_fini	= bdev_iscsi_finish,
	.get_ctx_size	= bdev_iscsi_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(iscsi, &g_iscsi_bdev_module);

static void
_bdev_iscsi_io_complete(void *_iscsi_io)
{
	struct bdev_iscsi_io *iscsi_io = _iscsi_io;

	if (iscsi_io->status == SPDK_BDEV_IO_STATUS_SUCCESS) {
		spdk_bdev_io_complete_scsi_status(spdk_bdev_io_from_ctx(iscsi_io), iscsi_io->scsi_status,
						  iscsi_io->sk, iscsi_io->asc, iscsi_io->ascq);
	} else {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(iscsi_io), iscsi_io->status);
	}
}

static void
bdev_iscsi_io_complete(struct bdev_iscsi_io *iscsi_io, enum spdk_bdev_io_status status)
{
	iscsi_io->status = status;
	if (iscsi_io->submit_td != NULL) {
		spdk_thread_send_msg(iscsi_io->submit_td, _bdev_iscsi_io_complete, iscsi_io);
	} else {
		_bdev_iscsi_io_complete(iscsi_io);
	}
}

/* Common call back function for read/write/flush command */
static void
bdev_iscsi_command_cb(struct iscsi_context *context, int status, void *_task, void *_iscsi_io)
{
	struct scsi_task *task = _task;
	struct bdev_iscsi_io *iscsi_io = _iscsi_io;

	iscsi_io->scsi_status = status;
	iscsi_io->sk = (uint8_t)task->sense.key;
	iscsi_io->asc = (task->sense.ascq >> 8) & 0xFF;
	iscsi_io->ascq = task->sense.ascq & 0xFF;

	scsi_free_scsi_task(task);
	bdev_iscsi_io_complete(iscsi_io, SPDK_BDEV_IO_STATUS_SUCCESS);
}

static void
bdev_iscsi_readv(struct bdev_iscsi_lun *lun, struct bdev_iscsi_io *iscsi_io,
		 struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t lba)
{
	struct scsi_task *task;

	SPDK_DEBUGLOG(iscsi_init, "read %d iovs size %lu to lba: %#lx\n",
		      iovcnt, nbytes, lba);

	task = iscsi_read16_task(lun->context, lun->lun_id, lba, nbytes, lun->bdev.blocklen, 0, 0, 0, 0, 0,
				 bdev_iscsi_command_cb, iscsi_io);
	if (task == NULL) {
		SPDK_ERRLOG("failed to get read16_task\n");
		bdev_iscsi_io_complete(iscsi_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

#if defined(LIBISCSI_FEATURE_IOVECTOR)
	scsi_task_set_iov_in(task, (struct scsi_iovec *)iov, iovcnt);
#else
	int i;
	for (i = 0; i < iovcnt; i++) {
		scsi_task_add_data_in_buffer(task, iov[i].iov_len, iov[i].iov_base);
	}
#endif
}

static void
bdev_iscsi_writev(struct bdev_iscsi_lun *lun, struct bdev_iscsi_io *iscsi_io,
		  struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t lba)
{
	struct scsi_task *task;

	SPDK_DEBUGLOG(iscsi_init, "write %d iovs size %lu to lba: %#lx\n",
		      iovcnt, nbytes, lba);

	task = iscsi_write16_task(lun->context, lun->lun_id, lba, NULL, nbytes, lun->bdev.blocklen, 0, 0, 0,
				  0, 0,
				  bdev_iscsi_command_cb, iscsi_io);
	if (task == NULL) {
		SPDK_ERRLOG("failed to get write16_task\n");
		bdev_iscsi_io_complete(iscsi_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

#if defined(LIBISCSI_FEATURE_IOVECTOR)
	scsi_task_set_iov_out(task, (struct scsi_iovec *)iov, iovcnt);
#else
	int i;
	for (i = 0; i < iovcnt; i++) {
		scsi_task_add_data_in_buffer(task, iov[i].iov_len, iov[i].iov_base);
	}
#endif
}

static void
bdev_iscsi_destruct_cb(void *ctx)
{
	struct bdev_iscsi_lun *lun = ctx;

	spdk_poller_unregister(&lun->no_master_ch_poller);
	spdk_io_device_unregister(lun, _iscsi_free_lun);
}

static int
bdev_iscsi_destruct(void *ctx)
{
	struct bdev_iscsi_lun *lun = ctx;

	assert(lun->no_master_ch_poller_td);
	spdk_thread_send_msg(lun->no_master_ch_poller_td, bdev_iscsi_destruct_cb, lun);
	return 1;
}

static void
bdev_iscsi_flush(struct bdev_iscsi_lun *lun, struct bdev_iscsi_io *iscsi_io, uint32_t num_blocks,
		 int immed, uint64_t lba)
{
	struct scsi_task *task;

	task = iscsi_synchronizecache16_task(lun->context, lun->lun_id, lba,
					     num_blocks, 0, immed, bdev_iscsi_command_cb, iscsi_io);
	if (task == NULL) {
		SPDK_ERRLOG("failed to get sync16_task\n");
		bdev_iscsi_io_complete(iscsi_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
}

static void
bdev_iscsi_unmap(struct bdev_iscsi_lun *lun, struct bdev_iscsi_io *iscsi_io,
		 uint64_t lba, uint64_t num_blocks)
{
	struct scsi_task *task;
	struct unmap_list list[1];

	list[0].lba = lba;
	list[0].num = num_blocks;
	task = iscsi_unmap_task(lun->context, 0, 0, 0, list, 1,
				bdev_iscsi_command_cb, iscsi_io);
	if (task == NULL) {
		SPDK_ERRLOG("failed to get unmap_task\n");
		bdev_iscsi_io_complete(iscsi_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
}

static void
bdev_iscsi_reset_cb(struct iscsi_context *context __attribute__((unused)), int status,
		    void *command_data, void *private_data)
{
	uint32_t tmf_response;
	struct bdev_iscsi_io *iscsi_io = private_data;

	tmf_response = *(uint32_t *)command_data;
	if (tmf_response == ISCSI_TASK_FUNC_RESP_COMPLETE) {
		bdev_iscsi_io_complete(iscsi_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		bdev_iscsi_io_complete(iscsi_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
_bdev_iscsi_reset(void *_bdev_io)
{
	int rc;
	struct spdk_bdev_io *bdev_io = _bdev_io;
	struct bdev_iscsi_lun *lun = (struct bdev_iscsi_lun *)bdev_io->bdev->ctxt;
	struct bdev_iscsi_io *iscsi_io = (struct bdev_iscsi_io *)bdev_io->driver_ctx;
	struct iscsi_context *context = lun->context;

	rc = iscsi_task_mgmt_lun_reset_async(context, lun->lun_id,
					     bdev_iscsi_reset_cb, iscsi_io);
	if (rc != 0) {
		SPDK_ERRLOG("failed to do iscsi reset\n");
		bdev_iscsi_io_complete(iscsi_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
}

static void
bdev_iscsi_reset(struct spdk_bdev_io *bdev_io)
{
	struct bdev_iscsi_lun *lun = (struct bdev_iscsi_lun *)bdev_io->bdev->ctxt;
	spdk_thread_send_msg(lun->master_td, _bdev_iscsi_reset, bdev_io);
}

static int
bdev_iscsi_poll_lun(void *_lun)
{
	struct bdev_iscsi_lun *lun = _lun;
	struct pollfd pfd = {};

	pfd.fd = iscsi_get_fd(lun->context);
	pfd.events = iscsi_which_events(lun->context);

	if (poll(&pfd, 1, 0) < 0) {
		SPDK_ERRLOG("poll failed\n");
		return SPDK_POLLER_IDLE;
	}

	if (pfd.revents != 0) {
		if (iscsi_service(lun->context, pfd.revents) < 0) {
			SPDK_ERRLOG("iscsi_service failed: %s\n", iscsi_get_error(lun->context));
		}

		return SPDK_POLLER_BUSY;
	}

	return SPDK_POLLER_IDLE;
}

static int
bdev_iscsi_no_master_ch_poll(void *arg)
{
	struct bdev_iscsi_lun *lun = arg;
	enum spdk_thread_poller_rc rc = SPDK_POLLER_IDLE;

	if (pthread_mutex_trylock(&lun->mutex)) {
		/* Don't care about the error code here. */
		return SPDK_POLLER_IDLE;
	}

	if (lun->ch_count == 0) {
		rc = bdev_iscsi_poll_lun(arg);
	}

	pthread_mutex_unlock(&lun->mutex);
	return rc;
}

static void
bdev_iscsi_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		      bool success)
{
	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	bdev_iscsi_readv((struct bdev_iscsi_lun *)bdev_io->bdev->ctxt,
			 (struct bdev_iscsi_io *)bdev_io->driver_ctx,
			 bdev_io->u.bdev.iovs,
			 bdev_io->u.bdev.iovcnt,
			 bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
			 bdev_io->u.bdev.offset_blocks);
}

static void _bdev_iscsi_submit_request(void *_bdev_io)
{
	struct spdk_bdev_io *bdev_io = _bdev_io;
	struct bdev_iscsi_io *iscsi_io = (struct bdev_iscsi_io *)bdev_io->driver_ctx;
	struct bdev_iscsi_lun *lun = (struct bdev_iscsi_lun *)bdev_io->bdev->ctxt;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_iscsi_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;

	case SPDK_BDEV_IO_TYPE_WRITE:
		bdev_iscsi_writev(lun, iscsi_io,
				  bdev_io->u.bdev.iovs,
				  bdev_io->u.bdev.iovcnt,
				  bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
				  bdev_io->u.bdev.offset_blocks);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		bdev_iscsi_flush(lun, iscsi_io,
				 bdev_io->u.bdev.num_blocks,
				 ISCSI_IMMEDIATE_DATA_NO,
				 bdev_io->u.bdev.offset_blocks);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		bdev_iscsi_reset(bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		bdev_iscsi_unmap(lun, iscsi_io,
				 bdev_io->u.bdev.offset_blocks,
				 bdev_io->u.bdev.num_blocks);
		break;
	default:
		bdev_iscsi_io_complete(iscsi_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}

static void bdev_iscsi_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct spdk_thread *submit_td = spdk_io_channel_get_thread(_ch);
	struct bdev_iscsi_io *iscsi_io = (struct bdev_iscsi_io *)bdev_io->driver_ctx;
	struct bdev_iscsi_lun *lun = (struct bdev_iscsi_lun *)bdev_io->bdev->ctxt;

	if (lun->master_td != submit_td) {
		iscsi_io->submit_td = submit_td;
		spdk_thread_send_msg(lun->master_td, _bdev_iscsi_submit_request, bdev_io);
		return;
	} else {
		iscsi_io->submit_td = NULL;
	}

	_bdev_iscsi_submit_request(bdev_io);
}

static bool
bdev_iscsi_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct bdev_iscsi_lun *lun = ctx;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
		return true;

	case SPDK_BDEV_IO_TYPE_UNMAP:
		return lun->unmap_supported;
	default:
		return false;
	}
}

static int
bdev_iscsi_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_iscsi_io_channel *ch = ctx_buf;
	struct bdev_iscsi_lun *lun = io_device;

	pthread_mutex_lock(&lun->mutex);
	if (lun->ch_count == 0) {
		assert(lun->master_td == NULL);
		lun->master_td = spdk_get_thread();
		lun->poller = SPDK_POLLER_REGISTER(bdev_iscsi_poll_lun, lun, 0);
		ch->lun = lun;
	}
	lun->ch_count++;
	pthread_mutex_unlock(&lun->mutex);

	return 0;
}

static void
_iscsi_destroy_cb(void *ctx)
{
	struct bdev_iscsi_lun *lun = ctx;

	pthread_mutex_lock(&lun->mutex);

	assert(lun->master_td == spdk_get_thread());
	assert(lun->ch_count > 0);

	lun->ch_count--;
	if (lun->ch_count > 0) {
		pthread_mutex_unlock(&lun->mutex);
		return;
	}

	lun->master_td = NULL;
	spdk_poller_unregister(&lun->poller);

	pthread_mutex_unlock(&lun->mutex);
}

static void
bdev_iscsi_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_iscsi_lun *lun = io_device;
	struct spdk_thread *thread;

	pthread_mutex_lock(&lun->mutex);
	lun->ch_count--;
	if (lun->ch_count == 0) {
		assert(lun->master_td != NULL);

		if (lun->master_td != spdk_get_thread()) {
			/* The final channel was destroyed on a different thread
			 * than where the first channel was created. Pass a message
			 * to the master thread to unregister the poller. */
			lun->ch_count++;
			thread = lun->master_td;
			pthread_mutex_unlock(&lun->mutex);
			spdk_thread_send_msg(thread, _iscsi_destroy_cb, lun);
			return;
		}

		lun->master_td = NULL;
		spdk_poller_unregister(&lun->poller);
	}
	pthread_mutex_unlock(&lun->mutex);
}

static struct spdk_io_channel *
bdev_iscsi_get_io_channel(void *ctx)
{
	struct bdev_iscsi_lun *lun = ctx;

	return spdk_get_io_channel(lun);
}

static int
bdev_iscsi_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct bdev_iscsi_lun *lun = ctx;

	spdk_json_write_named_object_begin(w, "iscsi");
	spdk_json_write_named_string(w, "initiator_name", lun->initiator_iqn);
	spdk_json_write_named_string(w, "url", lun->url);
	spdk_json_write_object_end(w);

	return 0;
}

static void
bdev_iscsi_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct bdev_iscsi_lun *lun = bdev->ctxt;

	pthread_mutex_lock(&lun->mutex);
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_iscsi_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_string(w, "initiator_iqn", lun->initiator_iqn);
	spdk_json_write_named_string(w, "url", lun->url);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
	pthread_mutex_unlock(&lun->mutex);
}

static const struct spdk_bdev_fn_table iscsi_fn_table = {
	.destruct		= bdev_iscsi_destruct,
	.submit_request		= bdev_iscsi_submit_request,
	.io_type_supported	= bdev_iscsi_io_type_supported,
	.get_io_channel		= bdev_iscsi_get_io_channel,
	.dump_info_json		= bdev_iscsi_dump_info_json,
	.write_config_json	= bdev_iscsi_write_config_json,
};

static int
create_iscsi_lun(struct iscsi_context *context, int lun_id, char *url, char *initiator_iqn,
		 char *name,
		 uint64_t num_blocks, uint32_t block_size, struct spdk_bdev **bdev, bool unmap_supported)
{
	struct bdev_iscsi_lun *lun;
	int rc;

	lun = calloc(sizeof(*lun), 1);
	if (!lun) {
		SPDK_ERRLOG("Unable to allocate enough memory for iscsi backend\n");
		return -ENOMEM;
	}

	lun->context = context;
	lun->lun_id = lun_id;
	lun->url = url;
	lun->initiator_iqn = initiator_iqn;

	pthread_mutex_init(&lun->mutex, NULL);

	lun->bdev.name = name;
	lun->bdev.product_name = "iSCSI LUN";
	lun->bdev.module = &g_iscsi_bdev_module;
	lun->bdev.blocklen = block_size;
	lun->bdev.blockcnt = num_blocks;
	lun->bdev.ctxt = lun;
	lun->unmap_supported = unmap_supported;

	lun->bdev.fn_table = &iscsi_fn_table;

	spdk_io_device_register(lun, bdev_iscsi_create_cb, bdev_iscsi_destroy_cb,
				sizeof(struct bdev_iscsi_io_channel),
				name);
	rc = spdk_bdev_register(&lun->bdev);
	if (rc) {
		spdk_io_device_unregister(lun, NULL);
		pthread_mutex_destroy(&lun->mutex);
		free(lun);
		return rc;
	}

	lun->no_master_ch_poller_td = spdk_get_thread();
	lun->no_master_ch_poller = SPDK_POLLER_REGISTER(bdev_iscsi_no_master_ch_poll, lun,
				   BDEV_ISCSI_NO_MASTER_CH_POLL_US);

	*bdev = &lun->bdev;
	return 0;
}

static void
iscsi_readcapacity16_cb(struct iscsi_context *iscsi, int status,
			void *command_data, void *private_data)
{
	struct bdev_iscsi_conn_req *req = private_data;
	struct scsi_readcapacity16 *readcap16;
	struct spdk_bdev *bdev = NULL;
	struct scsi_task *task = command_data;

	if (status != SPDK_SCSI_STATUS_GOOD) {
		SPDK_ERRLOG("iSCSI error: %s\n", iscsi_get_error(iscsi));
		goto ret;
	}

	readcap16 = scsi_datain_unmarshall(task);
	if (!readcap16) {
		status = -ENOMEM;
		goto ret;
	}

	status = create_iscsi_lun(req->context, req->lun, req->url, req->initiator_iqn, req->bdev_name,
				  readcap16->returned_lba + 1, readcap16->block_length, &bdev, req->unmap_supported);
	if (status) {
		SPDK_ERRLOG("Unable to create iscsi bdev: %s (%d)\n", spdk_strerror(-status), status);
	}

ret:
	scsi_free_scsi_task(task);
	complete_conn_req(req, bdev, status);
}

static void
bdev_iscsi_inquiry_cb(struct iscsi_context *context, int status, void *_task, void *private_data)
{
	struct scsi_task *task = _task;
	struct scsi_inquiry_logical_block_provisioning *lbp_inq = NULL;
	struct bdev_iscsi_conn_req *req = private_data;

	if (status == SPDK_SCSI_STATUS_GOOD) {
		lbp_inq = scsi_datain_unmarshall(task);
		if (lbp_inq != NULL && lbp_inq->lbpu) {
			req->unmap_supported = true;
		}
	}

	task = iscsi_readcapacity16_task(context, req->lun, iscsi_readcapacity16_cb, req);
	if (task) {
		return;
	}

	SPDK_ERRLOG("iSCSI error: %s\n", iscsi_get_error(req->context));
	complete_conn_req(req, NULL, status);
}

static void
iscsi_connect_cb(struct iscsi_context *iscsi, int status,
		 void *command_data, void *private_data)
{
	struct bdev_iscsi_conn_req *req = private_data;
	struct scsi_task *task;

	if (status != SPDK_SCSI_STATUS_GOOD) {
		goto ret;
	}

	task = iscsi_inquiry_task(iscsi, req->lun, 1,
				  SCSI_INQUIRY_PAGECODE_LOGICAL_BLOCK_PROVISIONING,
				  255, bdev_iscsi_inquiry_cb, req);
	if (task) {
		return;
	}

ret:
	SPDK_ERRLOG("iSCSI error: %s\n", iscsi_get_error(req->context));
	complete_conn_req(req, NULL, status);
}

static int
iscsi_bdev_conn_poll(void *arg)
{
	struct bdev_iscsi_conn_req *req, *tmp;
	struct pollfd pfd;
	struct iscsi_context *context;

	if (TAILQ_EMPTY(&g_iscsi_conn_req)) {
		return SPDK_POLLER_IDLE;
	}

	TAILQ_FOREACH_SAFE(req, &g_iscsi_conn_req, link, tmp) {
		context = req->context;
		pfd.fd = iscsi_get_fd(context);
		pfd.events = iscsi_which_events(context);
		pfd.revents = 0;
		if (poll(&pfd, 1, 0) < 0) {
			SPDK_ERRLOG("poll failed\n");
			return SPDK_POLLER_BUSY;
		}

		if (pfd.revents != 0) {
			if (iscsi_service(context, pfd.revents) < 0) {
				SPDK_ERRLOG("iscsi_service failed: %s\n", iscsi_get_error(context));
			}
		}

		if (req->status == 0) {
			/*
			 * The request completed successfully.
			 */
			free(req);
		} else if (req->status > 0) {
			/*
			 * An error has occurred during connecting.  This req has already
			 * been removed from the g_iscsi_conn_req list, but we needed to
			 * wait until iscsi_service unwound before we could free the req.
			 */
			_bdev_iscsi_conn_req_free(req);
		}
	}
	return SPDK_POLLER_BUSY;
}

int
create_iscsi_disk(const char *bdev_name, const char *url, const char *initiator_iqn,
		  spdk_bdev_iscsi_create_cb cb_fn, void *cb_arg)
{
	struct bdev_iscsi_conn_req *req;
	struct iscsi_url *iscsi_url = NULL;
	int rc;

	if (!bdev_name || !url || !initiator_iqn || strlen(initiator_iqn) == 0 || !cb_fn) {
		return -EINVAL;
	}

	req = calloc(1, sizeof(struct bdev_iscsi_conn_req));
	if (!req) {
		SPDK_ERRLOG("Cannot allocate pointer of struct bdev_iscsi_conn_req\n");
		return -ENOMEM;
	}

	req->status = SCSI_STATUS_GOOD;
	req->bdev_name = strdup(bdev_name);
	req->url = strdup(url);
	req->initiator_iqn = strdup(initiator_iqn);
	req->context = iscsi_create_context(initiator_iqn);
	if (!req->bdev_name || !req->url || !req->initiator_iqn || !req->context) {
		SPDK_ERRLOG("Out of memory\n");
		rc = -ENOMEM;
		goto err;
	}

	req->create_cb = cb_fn;
	req->create_cb_arg = cb_arg;

	iscsi_url = iscsi_parse_full_url(req->context, url);
	if (iscsi_url == NULL) {
		SPDK_ERRLOG("could not parse URL: %s\n", iscsi_get_error(req->context));
		rc = -EINVAL;
		goto err;
	}

	req->lun = iscsi_url->lun;
	rc = iscsi_set_session_type(req->context, ISCSI_SESSION_NORMAL);
	rc = rc ? rc : iscsi_set_header_digest(req->context, ISCSI_HEADER_DIGEST_NONE);
	rc = rc ? rc : iscsi_set_targetname(req->context, iscsi_url->target);
	rc = rc ? rc : iscsi_full_connect_async(req->context, iscsi_url->portal, iscsi_url->lun,
						iscsi_connect_cb, req);
	if (rc == 0 && iscsi_url->user[0] != '\0') {
		rc = iscsi_set_initiator_username_pwd(req->context, iscsi_url->user, iscsi_url->passwd);
	}

	if (rc < 0) {
		SPDK_ERRLOG("Failed to connect provided URL=%s: %s\n", url, iscsi_get_error(req->context));
		goto err;
	}

	iscsi_destroy_url(iscsi_url);
	req->status = -1;
	TAILQ_INSERT_TAIL(&g_iscsi_conn_req, req, link);
	if (!g_conn_poller) {
		g_conn_poller = SPDK_POLLER_REGISTER(iscsi_bdev_conn_poll, NULL, BDEV_ISCSI_CONNECTION_POLL_US);
	}

	return 0;

err:
	/* iscsi_destroy_url() is not NULL-proof */
	if (iscsi_url) {
		iscsi_destroy_url(iscsi_url);
	}

	if (req->context) {
		iscsi_destroy_context(req->context);
	}

	free(req->initiator_iqn);
	free(req->bdev_name);
	free(req->url);
	free(req);
	return rc;
}

void
delete_iscsi_disk(struct spdk_bdev *bdev, spdk_delete_iscsi_complete cb_fn, void *cb_arg)
{
	if (!bdev || bdev->module != &g_iscsi_bdev_module) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	spdk_bdev_unregister(bdev, cb_fn, cb_arg);
}

static int
bdev_iscsi_initialize(void)
{
	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(iscsi_init)
