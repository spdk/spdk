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
#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/fd.h"
#include "spdk/io_channel.h"
#include "spdk/json.h"
#include "spdk/util.h"
#include "spdk/rpc.h"
#include "spdk/string.h"

#include "spdk_internal/log.h"
#include "spdk_internal/bdev.h"

#include "iscsi/iscsi.h"
#include "iscsi/scsi-lowlevel.h"

struct bdev_iscsi_lun;

#define DEFAULT_INITIATOR_NAME "iqn.2016-06.io.spdk:init"

static int bdev_iscsi_initialize(void);
static TAILQ_HEAD(, bdev_iscsi_lun) g_iscsi_lun_head = TAILQ_HEAD_INITIALIZER(g_iscsi_lun_head);
static TAILQ_HEAD(, bdev_iscsi_conn_req) g_iscsi_conn_req = TAILQ_HEAD_INITIALIZER(
			g_iscsi_conn_req);
static struct spdk_poller *g_conn_poller = NULL;
static bool g_module_is_initialized;

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
	struct iscsi_url		*url;
	pthread_mutex_t			mutex;
	uint32_t			ch_count;
	struct bdev_iscsi_io_channel	*master_ch;
	struct spdk_thread		*master_td;
	TAILQ_ENTRY(bdev_iscsi_lun)	link;
};

struct bdev_iscsi_io_channel {
	struct spdk_poller	*poller;
	struct bdev_iscsi_lun	*lun;
};

struct bdev_iscsi_conn_req {
	struct iscsi_url			*url;
	char					*bdev_name;
	struct iscsi_context			*context;
	TAILQ_ENTRY(bdev_iscsi_conn_req)	link;
};

static int
bdev_iscsi_get_ctx_size(void)
{
	return sizeof(struct bdev_iscsi_io);
}

static void
bdev_iscsi_remove_conn_req(struct bdev_iscsi_conn_req *req)
{
	TAILQ_REMOVE(&g_iscsi_conn_req, req, link);
	free(req);
}

static void
bdev_iscsi_finish_done(void)
{
	struct bdev_iscsi_conn_req *req, *tmp;

	TAILQ_FOREACH_SAFE(req, &g_iscsi_conn_req, link, tmp) {
		bdev_iscsi_remove_conn_req(req);
	}

	if (g_conn_poller) {
		spdk_poller_unregister(&g_conn_poller);
	}

}

static void iscsi_free_lun(struct bdev_iscsi_lun *lun)
{
	assert(lun != NULL);
	free(lun->bdev.name);
	free(lun);
}

static void
bdev_iscsi_lun_cleanup(struct bdev_iscsi_lun *lun)
{
	TAILQ_REMOVE(&g_iscsi_lun_head, lun, link);
	iscsi_destroy_context(lun->context);
	iscsi_destroy_url(lun->url);
	iscsi_free_lun(lun);
	if (TAILQ_EMPTY(&g_iscsi_lun_head)) {
		bdev_iscsi_finish_done();
		spdk_bdev_module_finish_done();
	}
}

static void
iscsi_logout_cb(struct iscsi_context *iscsi, int status,
		void *command_data, void *private_data)
{
	struct bdev_iscsi_lun *lun = private_data;

	if (status != SPDK_SCSI_STATUS_GOOD) {
		SPDK_ERRLOG("Failed to logout from lun=%p\n", lun);
	}

	bdev_iscsi_lun_cleanup(lun);
}

static void
bdev_iscsi_finish(void)
{
	struct bdev_iscsi_lun *lun, *tmp;

	if (TAILQ_EMPTY(&g_iscsi_lun_head)) {
		bdev_iscsi_finish_done();
		spdk_bdev_module_finish_done();
		return;
	}

	TAILQ_FOREACH_SAFE(lun, &g_iscsi_lun_head, link, tmp) {
		if (iscsi_logout_async(lun->context, iscsi_logout_cb, lun) != 0) {
			bdev_iscsi_lun_cleanup(lun);
		}
	}
}

static struct spdk_bdev_module g_iscsi_bdev_module = {
	.name		= "iscsi",
	.module_init	= bdev_iscsi_initialize,
	.module_fini	= bdev_iscsi_finish,
	.get_ctx_size	= bdev_iscsi_get_ctx_size,
	.async_init	= true,
	.async_fini	= true,
};

SPDK_BDEV_MODULE_REGISTER(&g_iscsi_bdev_module);

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

static void
bdev_iscsi_rw_cb(struct iscsi_context *context, int status, void *_task, void *_iscsi_io)
{
	struct scsi_task *task = _task;
	struct bdev_iscsi_io *iscsi_io = _iscsi_io;

	iscsi_io->scsi_status = task->status;
	iscsi_io->sk = task->sense.key;
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

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI_INIT, "read %d iovs size %lu to lba: %#lx\n",
		      iovcnt, nbytes, lba);

	task = iscsi_read16_task(lun->context, 0, lba, nbytes, lun->bdev.blocklen, 0, 0, 0, 0, 0,
				 bdev_iscsi_rw_cb, iscsi_io);
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

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI_INIT, "write %d iovs size %lu to lba: %#lx\n",
		      iovcnt, nbytes, lba);

	task = iscsi_write16_task(lun->context, 0, lba, NULL, nbytes, lun->bdev.blocklen, 0, 0, 0, 0, 0,
				  bdev_iscsi_rw_cb, iscsi_io);
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

static int
bdev_iscsi_destruct(void *ctx)
{
	struct bdev_iscsi_lun *lun = ctx;
	int rc = 0;

	TAILQ_REMOVE(&g_iscsi_lun_head, lun, link);
	return rc;
}

static int
bdev_iscsi_poll(void *arg)
{
	struct bdev_iscsi_io_channel *ch = arg;
	struct bdev_iscsi_lun *lun = ch->lun;
	struct pollfd pfd = {};

	pfd.fd = iscsi_get_fd(lun->context);
	pfd.events = iscsi_which_events(lun->context);

	if (poll(&pfd, 1, 0) < 0) {
		SPDK_ERRLOG("poll failed\n");
		return -1;
	}

	if (pfd.revents != 0) {
		if (iscsi_service(lun->context, pfd.revents) < 0) {
			SPDK_ERRLOG("iscsi_service failed: %s\n", iscsi_get_error(lun->context));
		}
	}

	return 0;
}

static void bdev_iscsi_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
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
	}

	_bdev_iscsi_submit_request(bdev_io);
}

static bool
bdev_iscsi_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return true;

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
		assert(lun->master_ch == NULL);
		assert(lun->master_td == NULL);
		lun->master_ch = ch;
		lun->master_td = spdk_get_thread();
		ch->poller = spdk_poller_register(bdev_iscsi_poll, ch, 0);
		ch->lun = lun;
	}
	lun->ch_count++;
	pthread_mutex_unlock(&lun->mutex);

	return 0;
}

static void
bdev_iscsi_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_iscsi_io_channel *io_channel = ctx_buf;
	struct bdev_iscsi_lun *lun = io_device;

	pthread_mutex_lock(&lun->mutex);
	lun->ch_count--;
	if (lun->ch_count == 0) {
		assert(lun->master_ch != NULL);
		assert(lun->master_td != NULL);
		lun->master_ch = NULL;
		lun->master_td = NULL;
		spdk_poller_unregister(&io_channel->poller);
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

	spdk_json_write_name(w, "iscsi");
	spdk_json_write_object_begin(w);
	spdk_json_write_name(w, "initiator_name");
	spdk_json_write_string(w, lun->initiator_iqn);
	spdk_json_write_name(w, "target");
	spdk_json_write_string(w, lun->url->target);
	spdk_json_write_object_end(w);

	return 0;
}

static const struct spdk_bdev_fn_table iscsi_fn_table = {
	.destruct		= bdev_iscsi_destruct,
	.submit_request		= bdev_iscsi_submit_request,
	.io_type_supported	= bdev_iscsi_io_type_supported,
	.get_io_channel		= bdev_iscsi_get_io_channel,
	.dump_info_json		= bdev_iscsi_dump_info_json,
};

static struct spdk_bdev *
create_iscsi_lun(struct iscsi_context *context, struct iscsi_url *url,
		 const char *name, uint64_t num_blocks, uint32_t block_size)
{
	struct bdev_iscsi_lun *lun;
	int rc;

	lun = calloc(sizeof(*lun), 1);
	if (!lun) {
		SPDK_ERRLOG("Unable to allocate enough memory for iscsi backend\n");
		return NULL;
	}

	lun->context = context;
	lun->url = url;

	pthread_mutex_init(&lun->mutex, NULL);

	lun->bdev.name = strdup(name);
	if (!lun->bdev.name) {
		goto error_return;
	}
	lun->bdev.product_name = "iSCSI LUN";
	lun->bdev.module = &g_iscsi_bdev_module;
	lun->bdev.blocklen = block_size;
	lun->bdev.blockcnt = num_blocks;
	lun->bdev.ctxt = lun;

	lun->bdev.fn_table = &iscsi_fn_table;

	spdk_io_device_register(lun, bdev_iscsi_create_cb, bdev_iscsi_destroy_cb,
				sizeof(struct bdev_iscsi_io_channel));
	rc = spdk_bdev_register(&lun->bdev);
	if (rc) {
		spdk_io_device_unregister(lun, NULL);
		goto error_return;
	}

	TAILQ_INSERT_TAIL(&g_iscsi_lun_head, lun, link);
	return &lun->bdev;

error_return:
	iscsi_free_lun(lun);
	return NULL;
}

static struct bdev_iscsi_conn_req *
bdev_iscsi_allocate_conn_req(struct iscsi_context *context, char *bdev_name, struct iscsi_url *url)
{
	struct bdev_iscsi_conn_req *req;

	req = calloc(1, sizeof(struct bdev_iscsi_conn_req));
	if (!req) {
		SPDK_ERRLOG("Cannot allocate pointer of struct bdev_iscsi_conn_req\n");
		return NULL;
	}

	req->bdev_name = bdev_name;
	req->url = url;
	req->context = context;

	return req;
}

static void
_bdev_iscsi_set_module_init(void)
{
	spdk_bdev_module_init_done(&g_iscsi_bdev_module);
	g_module_is_initialized = true;
}

static void
bdev_iscsi_set_module_init(void)
{
	if (!g_module_is_initialized && TAILQ_EMPTY(&g_iscsi_conn_req)) {
		_bdev_iscsi_set_module_init();
	}
}

static void
iscsi_readcapacity16_cb(struct iscsi_context *iscsi, int status,
			void *command_data, void *private_data)
{
	struct bdev_iscsi_conn_req *req = private_data;
	struct scsi_readcapacity16 *readcap16;
	struct spdk_bdev *bdev;
	struct scsi_task *task = command_data;

	if (status != SPDK_SCSI_STATUS_GOOD) {
		goto ret;
	}

	readcap16 = scsi_datain_unmarshall(task);
	bdev = create_iscsi_lun(req->context, req->url, req->bdev_name,
				readcap16->returned_lba + 1, readcap16->block_length);
	if (!bdev) {
		SPDK_ERRLOG("Unable to create iscsi bdev\n");
	}

ret:
	scsi_free_scsi_task(task);
	bdev_iscsi_remove_conn_req(req);
	bdev_iscsi_set_module_init();
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

	task = iscsi_readcapacity16_task(iscsi, 0, iscsi_readcapacity16_cb, req);
	if (task) {
		return;
	}
ret:
	bdev_iscsi_remove_conn_req(req);
	bdev_iscsi_set_module_init();
}

static int
iscsi_bdev_conn_poll(void *arg)
{
	struct bdev_iscsi_conn_req *req, *tmp;
	struct pollfd pfd;

	TAILQ_FOREACH_SAFE(req, &g_iscsi_conn_req, link, tmp) {
		pfd.fd = iscsi_get_fd(req->context);
		pfd.events = iscsi_which_events(req->context);
		pfd.revents = 0;
		if (poll(&pfd, 1, 0) < 0) {
			SPDK_ERRLOG("poll failed\n");
			return -1;
		}

		if (pfd.revents != 0) {
			if (iscsi_service(req->context, pfd.revents) < 0) {
				SPDK_ERRLOG("iscsi_service failed: %s\n", iscsi_get_error(req->context));
			}
		}
	}

	return 0;
}

static int
bdev_iscsi_initialize(void)
{
	struct spdk_conf_section *sp;
	struct iscsi_context *context;
	struct iscsi_url *url;
	char *val, *bdev_name, *initiator_iqn;
	struct bdev_iscsi_conn_req *req;
	int i, rc;

	sp = spdk_conf_find_section(NULL, "iSCSI_Initiator");
	if (sp == NULL) {
		_bdev_iscsi_set_module_init();
		return 0;
	}

	initiator_iqn = spdk_conf_section_get_val(sp, "initiator_name");
	if (!initiator_iqn) {
		initiator_iqn = DEFAULT_INITIATOR_NAME;
	}

	i = 0;
	while (true) {
		val = spdk_conf_section_get_nmval(sp, "URL", i, 0);
		if (val == NULL) {
			break;
		}

		bdev_name = spdk_conf_section_get_nmval(sp, "URL", i, 1);
		if (bdev_name == NULL) {
			SPDK_ERRLOG("no bdev name specified for URL %s\n", val);
			break;
		}

		context = iscsi_create_context(initiator_iqn);
		if (context == NULL) {
			SPDK_ERRLOG("could not create iscsi context\n");
			break;
		}

		url = iscsi_parse_full_url(context, val);
		if (url == NULL) {
			SPDK_ERRLOG("could not parse URL\n");
			break;
		}

		req = bdev_iscsi_allocate_conn_req(context, bdev_name, url);
		if (!req) {
			_bdev_iscsi_set_module_init();
			return -1;
		}

		iscsi_set_session_type(context, ISCSI_SESSION_NORMAL);
		iscsi_set_header_digest(context, ISCSI_HEADER_DIGEST_NONE);
		rc = iscsi_full_connect_async(context, url->portal, url->lun, iscsi_connect_cb, req);
		if (rc < 0) {
			free(req);
			SPDK_ERRLOG("Failed to connect provided URL=%s, will ignore it\n", val);
			continue;
		}

		TAILQ_INSERT_TAIL(&g_iscsi_conn_req, req, link);
		i++;
	}

	if (!TAILQ_EMPTY(&g_iscsi_conn_req)) {
		g_conn_poller = spdk_poller_register(iscsi_bdev_conn_poll, NULL, 0);
	}
	return 0;
}

SPDK_LOG_REGISTER_COMPONENT("iscsi_init", SPDK_LOG_ISCSI_INIT)
