/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) croit GmbH.
 *   All rights reserved.
 */

#include <sys/queue.h>

#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/json.h"
#include "spdk/thread.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/stdinc.h"
#include "spdk/log.h"

#include <daos.h>
#include <daos_event.h>
#include <daos_fs.h>
#include <daos_types.h>
#include <daos_pool.h>
#include <daos_cont.h>
#include <daos_errno.h>

#include "bdev_daos.h"

#define BDEV_DAOS_IOVECS_MAX 32

struct bdev_daos_task {
	daos_event_t ev;
	struct spdk_thread *submit_td;
	struct spdk_bdev_io *bdev_io;

	enum spdk_bdev_io_status status;

	uint64_t offset;

	/* DAOS version of iovec and scatter/gather */
	daos_size_t read_size;
	d_iov_t diovs[BDEV_DAOS_IOVECS_MAX];
	d_sg_list_t sgl;
};

struct bdev_daos {
	struct spdk_bdev disk;
	daos_oclass_id_t oclass;

	char pool_name[DAOS_PROP_MAX_LABEL_BUF_LEN];
	char cont_name[DAOS_PROP_MAX_LABEL_BUF_LEN];

	struct bdev_daos_task *reset_task;
	struct spdk_poller    *reset_retry_timer;
};

struct bdev_daos_io_channel {
	struct bdev_daos *disk;
	struct spdk_poller *poller;

	daos_handle_t pool;
	daos_handle_t cont;

	dfs_t *dfs;
	dfs_obj_t *obj;
	daos_handle_t queue;
};

static uint32_t g_bdev_daos_init_count = 0;
static pthread_mutex_t g_bdev_daos_init_mutex = PTHREAD_MUTEX_INITIALIZER;

static int bdev_daos_initialize(void);

static int bdev_get_daos_engine(void);
static int bdev_daos_put_engine(void);

static int
bdev_daos_get_ctx_size(void)
{
	return sizeof(struct bdev_daos_task);
}

static struct spdk_bdev_module daos_if = {
	.name = "daos",
	.module_init = bdev_daos_initialize,
	.get_ctx_size = bdev_daos_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(daos, &daos_if)

static void
bdev_daos_free(struct bdev_daos *bdev_daos)
{
	if (!bdev_daos) {
		return;
	}

	free(bdev_daos->disk.name);
	free(bdev_daos);
}

static void
bdev_daos_destruct_cb(void *io_device)
{
	int rc;
	struct bdev_daos *daos = io_device;

	assert(daos != NULL);

	bdev_daos_free(daos);

	rc = bdev_daos_put_engine();
	if (rc) {
		SPDK_ERRLOG("could not de-initialize DAOS engine: " DF_RC "\n", DP_RC(rc));
	}
}

static int
bdev_daos_destruct(void *ctx)
{
	struct bdev_daos *daos = ctx;

	SPDK_NOTICELOG("%s: destroying bdev_daos device\n", daos->disk.name);

	spdk_io_device_unregister(daos, bdev_daos_destruct_cb);

	return 0;
}

static void
_bdev_daos_io_complete(void *bdev_daos_task)
{
	struct bdev_daos_task *task = bdev_daos_task;

	SPDK_DEBUGLOG(bdev_daos, "completed IO at %#lx with status %s\n", task->offset,
		      task->status == SPDK_BDEV_IO_STATUS_SUCCESS ? "SUCCESS" : "FAILURE");

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task), task->status);
}

static void
bdev_daos_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	struct bdev_daos_task *task = (struct bdev_daos_task *)bdev_io->driver_ctx;
	struct spdk_thread *current_thread = spdk_get_thread();

	assert(task->submit_td != NULL);

	task->status = status;
	if (task->submit_td != current_thread) {
		spdk_thread_send_msg(task->submit_td, _bdev_daos_io_complete, task);
	} else {
		_bdev_daos_io_complete(task);
	}
}

static int64_t
bdev_daos_writev(struct bdev_daos *daos, struct bdev_daos_io_channel *ch,
		 struct bdev_daos_task *task,
		 struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	int rc;

	SPDK_DEBUGLOG(bdev_daos, "write %d iovs size %lu to off: %#lx\n",
		      iovcnt, nbytes, offset);

	assert(ch != NULL);
	assert(daos != NULL);
	assert(task != NULL);
	assert(iov != NULL);

	if (iovcnt > BDEV_DAOS_IOVECS_MAX) {
		SPDK_ERRLOG("iovs number [%d] exceeds max allowed limit [%d]\n", iovcnt,
			    BDEV_DAOS_IOVECS_MAX);
		return -E2BIG;
	}

	if ((rc = daos_event_init(&task->ev, ch->queue, NULL))) {
		SPDK_ERRLOG("%s: could not initialize async event: " DF_RC "\n",
			    daos->disk.name, DP_RC(rc));
		return -EINVAL;
	}

	for (int i = 0; i < iovcnt; i++, iov++) {
		d_iov_set(&(task->diovs[i]), iov->iov_base, iov->iov_len);
	}

	task->sgl.sg_nr = iovcnt;
	task->sgl.sg_nr_out = 0;
	task->sgl.sg_iovs = task->diovs;
	task->offset = offset;

	if ((rc = dfs_write(ch->dfs, ch->obj, &task->sgl, offset, &task->ev))) {
		SPDK_ERRLOG("%s: could not start async write: " DF_RC "\n",
			    daos->disk.name, DP_RC(rc));
		daos_event_fini(&task->ev);
		return -EINVAL;
	}

	return nbytes;
}

static int64_t
bdev_daos_readv(struct bdev_daos *daos, struct bdev_daos_io_channel *ch,
		struct bdev_daos_task *task,
		struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	int rc;

	SPDK_DEBUGLOG(bdev_daos, "read %d iovs size %lu to off: %#lx\n",
		      iovcnt, nbytes, offset);

	assert(ch != NULL);
	assert(daos != NULL);
	assert(task != NULL);
	assert(iov != NULL);

	if (iovcnt > BDEV_DAOS_IOVECS_MAX) {
		SPDK_ERRLOG("iovs number [%d] exceeds max allowed limit [%d]\n", iovcnt,
			    BDEV_DAOS_IOVECS_MAX);
		return -E2BIG;
	}

	if ((rc = daos_event_init(&task->ev, ch->queue, NULL))) {
		SPDK_ERRLOG("%s: could not initialize async event: " DF_RC "\n",
			    daos->disk.name, DP_RC(rc));
		return -EINVAL;
	}

	for (int i = 0; i < iovcnt; i++, iov++) {
		d_iov_set(&(task->diovs[i]), iov->iov_base, iov->iov_len);
	}

	task->sgl.sg_nr = iovcnt;
	task->sgl.sg_nr_out = 0;
	task->sgl.sg_iovs = task->diovs;
	task->offset = offset;

	if ((rc = dfs_read(ch->dfs, ch->obj, &task->sgl, offset, &task->read_size, &task->ev))) {
		SPDK_ERRLOG("%s: could not start async read: " DF_RC "\n",
			    daos->disk.name, DP_RC(rc));
		daos_event_fini(&task->ev);
		return -EINVAL;
	}

	return nbytes;
}

static void
bdev_daos_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		     bool success)
{
	int64_t rc;
	struct bdev_daos_io_channel *dch = spdk_io_channel_get_ctx(ch);

	if (!success) {
		bdev_daos_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	rc = bdev_daos_readv((struct bdev_daos *)bdev_io->bdev->ctxt,
			     dch,
			     (struct bdev_daos_task *)bdev_io->driver_ctx,
			     bdev_io->u.bdev.iovs,
			     bdev_io->u.bdev.iovcnt,
			     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
			     bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen);

	if (rc < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
}

static void
_bdev_daos_get_io_inflight(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct bdev_daos_io_channel *dch = spdk_io_channel_get_ctx(ch);
	int io_inflight = daos_eq_query(dch->queue, DAOS_EQR_WAITING, 0, NULL);

	if (io_inflight > 0) {
		spdk_for_each_channel_continue(i, -1);
		return;
	}

	spdk_for_each_channel_continue(i, 0);
}

static int bdev_daos_reset_retry_timer(void *arg);

static void
_bdev_daos_get_io_inflight_done(struct spdk_io_channel_iter *i, int status)
{
	struct bdev_daos *daos = spdk_io_channel_iter_get_ctx(i);

	if (status == -1) {
		daos->reset_retry_timer = SPDK_POLLER_REGISTER(bdev_daos_reset_retry_timer, daos, 1000);
		return;
	}

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(daos->reset_task), SPDK_BDEV_IO_STATUS_SUCCESS);
}

static int
bdev_daos_reset_retry_timer(void *arg)
{
	struct bdev_daos *daos = arg;

	if (daos->reset_retry_timer) {
		spdk_poller_unregister(&daos->reset_retry_timer);
	}

	spdk_for_each_channel(daos,
			      _bdev_daos_get_io_inflight,
			      daos,
			      _bdev_daos_get_io_inflight_done);

	return SPDK_POLLER_BUSY;
}

static void
bdev_daos_reset(struct bdev_daos *daos, struct bdev_daos_task *task)
{
	assert(daos != NULL);
	assert(task != NULL);

	daos->reset_task = task;
	bdev_daos_reset_retry_timer(daos);
}


static int64_t
bdev_daos_unmap(struct bdev_daos_io_channel *ch, uint64_t nbytes,
		uint64_t offset)
{
	SPDK_DEBUGLOG(bdev_daos, "unmap at %#lx with size %#lx\n", offset, nbytes);
	return dfs_punch(ch->dfs, ch->obj, offset, nbytes);
}

static void
_bdev_daos_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct bdev_daos_io_channel *dch = spdk_io_channel_get_ctx(ch);

	int64_t rc;
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_daos_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;

	case SPDK_BDEV_IO_TYPE_WRITE:
		rc = bdev_daos_writev((struct bdev_daos *)bdev_io->bdev->ctxt,
				      dch,
				      (struct bdev_daos_task *)bdev_io->driver_ctx,
				      bdev_io->u.bdev.iovs,
				      bdev_io->u.bdev.iovcnt,
				      bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
				      bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen);
		if (rc < 0) {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
		break;

	case SPDK_BDEV_IO_TYPE_RESET:
		/* Can't cancel in-flight requests, but can wait for their completions */
		bdev_daos_reset((struct bdev_daos *)bdev_io->bdev->ctxt,
				(struct bdev_daos_task *)bdev_io->driver_ctx);
		break;

	case SPDK_BDEV_IO_TYPE_FLUSH:
		/* NOOP because DAOS requests land on PMEM and writes are persistent upon completion */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		break;

	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = bdev_daos_unmap(dch,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
				     bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen);
		if (!rc) {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		} else {
			SPDK_DEBUGLOG(bdev_daos, "%s: could not unmap: " DF_RC "\n",
				      dch->disk->disk.name, DP_RC((int)rc));
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}

		break;

	default:
		SPDK_ERRLOG("Wrong io type\n");
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}

static void
bdev_daos_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct bdev_daos_task *task = (struct bdev_daos_task *)bdev_io->driver_ctx;
	struct spdk_thread *submit_td = spdk_io_channel_get_thread(ch);

	assert(task != NULL);

	task->submit_td = submit_td;
	task->bdev_io = bdev_io;

	_bdev_daos_submit_request(ch, bdev_io);
}

#define POLLING_EVENTS_NUM 64

static int
bdev_daos_channel_poll(void *arg)
{
	daos_event_t *evp[POLLING_EVENTS_NUM];
	struct bdev_daos_io_channel *ch = arg;

	assert(ch != NULL);
	assert(ch->disk != NULL);

	int rc = daos_eq_poll(ch->queue, 0, DAOS_EQ_NOWAIT,
			      POLLING_EVENTS_NUM, evp);

	if (rc < 0) {
		SPDK_DEBUGLOG(bdev_daos, "%s: could not poll daos event queue: " DF_RC "\n",
			      ch->disk->disk.name, DP_RC(rc));
		/*
		 * TODO: There are cases when this is self healing, e.g.
		 * brief network issues, DAOS agent restarting etc.
		 * However, if the issue persists over some time better would be
		 * to remove a bdev or the whole controller
		 */
		return SPDK_POLLER_BUSY;
	}

	for (int i = 0; i < rc; ++i) {
		struct bdev_daos_task *task = container_of(evp[i], struct bdev_daos_task, ev);
		enum spdk_bdev_io_status status = SPDK_BDEV_IO_STATUS_SUCCESS;

		assert(task != NULL);

		if (task->ev.ev_error != DER_SUCCESS) {
			status = SPDK_BDEV_IO_STATUS_FAILED;
		}

		daos_event_fini(&task->ev);
		bdev_daos_io_complete(task->bdev_io, status);
	}

	return rc > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static bool
bdev_daos_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		return true;

	default:
		return false;
	}
}

static struct spdk_io_channel *
bdev_daos_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(ctx);
}

static void
bdev_daos_write_json_config(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	char uuid_str[SPDK_UUID_STRING_LEN];
	struct bdev_daos *daos = bdev->ctxt;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_daos_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_string(w, "pool", daos->pool_name);
	spdk_json_write_named_string(w, "cont", daos->cont_name);
	spdk_json_write_named_uint64(w, "num_blocks", bdev->blockcnt);
	spdk_json_write_named_uint32(w, "block_size", bdev->blocklen);
	spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &bdev->uuid);
	spdk_json_write_named_string(w, "uuid", uuid_str);

	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table daos_fn_table = {
	.destruct		= bdev_daos_destruct,
	.submit_request		= bdev_daos_submit_request,
	.io_type_supported	= bdev_daos_io_type_supported,
	.get_io_channel		= bdev_daos_get_io_channel,
	.write_config_json	= bdev_daos_write_json_config,
};

static void *
_bdev_daos_io_channel_create_cb(void *ctx)
{
	int rc = 0 ;
	struct bdev_daos_io_channel *ch = ctx;
	struct bdev_daos *daos = ch->disk;

	daos_pool_info_t pinfo;
	daos_cont_info_t cinfo;

	int fd_oflag = O_CREAT | O_RDWR;
	mode_t mode = S_IFREG | S_IRWXU | S_IRWXG | S_IRWXO;

	rc = bdev_get_daos_engine();
	if (rc) {
		SPDK_ERRLOG("could not initialize DAOS engine: " DF_RC "\n", DP_RC(rc));
		return NULL;
	}

	SPDK_DEBUGLOG(bdev_daos, "connecting to daos pool '%s'\n", daos->pool_name);
	if ((rc = daos_pool_connect(daos->pool_name, NULL, DAOS_PC_RW, &ch->pool, &pinfo, NULL))) {
		SPDK_ERRLOG("%s: could not connect to daos pool: " DF_RC "\n",
			    daos->disk.name, DP_RC(rc));
		return NULL;
	}
	SPDK_DEBUGLOG(bdev_daos, "connecting to daos container '%s'\n", daos->cont_name);
	if ((rc = daos_cont_open(ch->pool, daos->cont_name, DAOS_COO_RW, &ch->cont, &cinfo, NULL))) {
		SPDK_ERRLOG("%s: could not open daos container: " DF_RC "\n",
			    daos->disk.name, DP_RC(rc));
		goto cleanup_pool;
	}
	SPDK_DEBUGLOG(bdev_daos, "mounting daos dfs\n");
	if ((rc = dfs_mount(ch->pool, ch->cont, O_RDWR, &ch->dfs))) {
		SPDK_ERRLOG("%s: could not mount daos dfs: " DF_RC "\n",
			    daos->disk.name, DP_RC(rc));
		goto cleanup_cont;
	}
	SPDK_DEBUGLOG(bdev_daos, "opening dfs object\n");
	if ((rc = dfs_open(ch->dfs, NULL, daos->disk.name, mode, fd_oflag, daos->oclass,
			   0, NULL, &ch->obj))) {
		SPDK_ERRLOG("%s: could not open dfs object: " DF_RC "\n",
			    daos->disk.name, DP_RC(rc));
		goto cleanup_mount;
	}
	if ((rc = daos_eq_create(&ch->queue))) {
		SPDK_ERRLOG("%s: could not create daos event queue: " DF_RC "\n",
			    daos->disk.name, DP_RC(rc));
		goto cleanup_obj;
	}

	return ctx;

cleanup_obj:
	dfs_release(ch->obj);
cleanup_mount:
	dfs_umount(ch->dfs);
cleanup_cont:
	daos_cont_close(ch->cont, NULL);
cleanup_pool:
	daos_pool_disconnect(ch->pool, NULL);

	return NULL;
}

static int
bdev_daos_io_channel_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_daos_io_channel *ch = ctx_buf;

	ch->disk = io_device;

	if (spdk_call_unaffinitized(_bdev_daos_io_channel_create_cb, ch) == NULL) {
		return -EINVAL;
	}

	SPDK_DEBUGLOG(bdev_daos, "%s: starting daos event queue poller\n",
		      ch->disk->disk.name);

	ch->poller = SPDK_POLLER_REGISTER(bdev_daos_channel_poll, ch, 0);

	return 0;
}

static void
bdev_daos_io_channel_destroy_cb(void *io_device, void *ctx_buf)
{
	int rc;
	struct bdev_daos_io_channel *ch = ctx_buf;

	SPDK_DEBUGLOG(bdev_daos, "stopping daos event queue poller\n");

	spdk_poller_unregister(&ch->poller);

	if ((rc = daos_eq_destroy(ch->queue, DAOS_EQ_DESTROY_FORCE))) {
		SPDK_ERRLOG("could not destroy daos event queue: " DF_RC "\n", DP_RC(rc));
	}
	if ((rc = dfs_release(ch->obj))) {
		SPDK_ERRLOG("could not release dfs object: " DF_RC "\n", DP_RC(rc));
	}
	if ((rc = dfs_umount(ch->dfs))) {
		SPDK_ERRLOG("could not unmount dfs: " DF_RC "\n", DP_RC(rc));
	}
	if ((rc = daos_cont_close(ch->cont, NULL))) {
		SPDK_ERRLOG("could not close container: " DF_RC "\n", DP_RC(rc));
	}
	if ((rc = daos_pool_disconnect(ch->pool, NULL))) {
		SPDK_ERRLOG("could not disconnect from pool: " DF_RC "\n", DP_RC(rc));
	}
	rc = bdev_daos_put_engine();
	if (rc) {
		SPDK_ERRLOG("could not de-initialize DAOS engine: " DF_RC "\n", DP_RC(rc));
	}
}

int
create_bdev_daos(struct spdk_bdev **bdev,
		 const char *name, const struct spdk_uuid *uuid,
		 const char *pool, const char *cont, const char *oclass,
		 uint64_t num_blocks, uint32_t block_size)
{
	int rc;
	size_t len;
	struct bdev_daos *daos;
	struct bdev_daos_io_channel ch = {};

	SPDK_NOTICELOG("%s: creating bdev_daos disk on '%s:%s'\n", name, pool, cont);

	if (num_blocks == 0) {
		SPDK_ERRLOG("Disk num_blocks must be greater than 0");
		return -EINVAL;
	}

	if (block_size % 512) {
		SPDK_ERRLOG("block size must be 512 bytes aligned\n");
		return -EINVAL;
	}

	if (!name) {
		SPDK_ERRLOG("device name cannot be empty\n");
		return -EINVAL;
	}

	if (!pool) {
		SPDK_ERRLOG("daos pool cannot be empty\n");
		return -EINVAL;
	}
	if (!cont) {
		SPDK_ERRLOG("daos cont cannot be empty\n");
		return -EINVAL;
	}

	daos = calloc(1, sizeof(*daos));
	if (!daos) {
		SPDK_ERRLOG("calloc() failed\n");
		return -ENOMEM;
	}

	if (!oclass) {
		oclass = "SX"; /* Max throughput by default */
	}
	daos->oclass = daos_oclass_name2id(oclass);
	if (daos->oclass == OC_UNKNOWN) {
		SPDK_ERRLOG("could not parse daos oclass: '%s'\n", oclass);
		free(daos);
		return -EINVAL;
	}

	len = strlen(pool);
	if (len > DAOS_PROP_LABEL_MAX_LEN) {
		SPDK_ERRLOG("daos pool name is too long\n");
		free(daos);
		return -EINVAL;
	}
	memcpy(daos->pool_name, pool, len);

	len = strlen(cont);
	if (len > DAOS_PROP_LABEL_MAX_LEN) {
		SPDK_ERRLOG("daos cont name is too long\n");
		free(daos);
		return -EINVAL;
	}
	memcpy(daos->cont_name, cont, len);

	daos->disk.name = strdup(name);
	daos->disk.product_name = "DAOS bdev";

	daos->disk.write_cache = 0;
	daos->disk.blocklen = block_size;
	daos->disk.blockcnt = num_blocks;

	if (uuid) {
		daos->disk.uuid = *uuid;
	} else {
		spdk_uuid_generate(&daos->disk.uuid);
	}

	daos->disk.ctxt = daos;
	daos->disk.fn_table = &daos_fn_table;
	daos->disk.module = &daos_if;

	rc = bdev_get_daos_engine();
	if (rc) {
		SPDK_ERRLOG("could not initialize DAOS engine: " DF_RC "\n", DP_RC(rc));
		bdev_daos_free(daos);
		return rc;
	}

	/* We try to connect to the DAOS container during channel creation, so simulate
	 * creating a channel here, so that we can return a failure when the DAOS bdev
	 * is created, instead of finding it out later when the first channel is created
	 * and leaving unusable bdev registered.
	 */
	rc = bdev_daos_io_channel_create_cb(daos, &ch);
	if (rc) {
		SPDK_ERRLOG("'%s' could not initialize io-channel: %s", name, strerror(-rc));
		bdev_daos_free(daos);
		return rc;
	}
	bdev_daos_io_channel_destroy_cb(daos, &ch);

	spdk_io_device_register(daos, bdev_daos_io_channel_create_cb,
				bdev_daos_io_channel_destroy_cb,
				sizeof(struct bdev_daos_io_channel),
				daos->disk.name);


	rc = spdk_bdev_register(&daos->disk);
	if (rc) {
		spdk_io_device_unregister(daos, NULL);
		bdev_daos_free(daos);
		return rc;
	}

	*bdev = &(daos->disk);

	return rc;
}

static void
dummy_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
}

int
bdev_daos_resize(const char *name, const uint64_t new_size_in_mb)
{
	int rc = 0;
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	struct spdk_io_channel *ch;
	struct bdev_daos_io_channel *dch;
	uint64_t new_size_in_byte;
	uint64_t current_size_in_mb;

	rc = spdk_bdev_open_ext(name, false, dummy_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		return rc;
	}

	bdev = spdk_bdev_desc_get_bdev(desc);
	if (bdev->module != &daos_if) {
		rc = -EINVAL;
		goto exit;
	}

	current_size_in_mb = bdev->blocklen * bdev->blockcnt / (1024 * 1024);
	if (current_size_in_mb > new_size_in_mb) {
		SPDK_ERRLOG("The new bdev size must be larger than current bdev size.\n");
		rc = -EINVAL;
		goto exit;
	}

	ch = bdev_daos_get_io_channel(bdev);
	dch = spdk_io_channel_get_ctx(ch);
	new_size_in_byte = new_size_in_mb * 1024 * 1024;

	rc = dfs_punch(dch->dfs, dch->obj, new_size_in_byte, DFS_MAX_FSIZE);
	spdk_put_io_channel(ch);
	if (rc != 0) {
		SPDK_ERRLOG("failed to resize daos bdev: " DF_RC "\n", DP_RC(rc));
		rc = -EINTR;
		goto exit;
	}

	SPDK_NOTICELOG("DAOS bdev device is resized: bdev name %s, old block count %" PRIu64
		       ", new block count %"
		       PRIu64 "\n",
		       bdev->name,
		       bdev->blockcnt,
		       new_size_in_byte / bdev->blocklen);
	rc = spdk_bdev_notify_blockcnt_change(bdev, new_size_in_byte / bdev->blocklen);
	if (rc != 0) {
		SPDK_ERRLOG("failed to notify block cnt change.\n");
	}

exit:
	spdk_bdev_close(desc);
	return rc;
}

void
delete_bdev_daos(struct spdk_bdev *bdev, spdk_delete_daos_complete cb_fn, void *cb_arg)
{
	if (!bdev || bdev->module != &daos_if) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	spdk_bdev_unregister(bdev, cb_fn, cb_arg);
}

static int
bdev_get_daos_engine(void)
{
	int rc = 0;

	pthread_mutex_lock(&g_bdev_daos_init_mutex);
	if (g_bdev_daos_init_count++ > 0) {
		pthread_mutex_unlock(&g_bdev_daos_init_mutex);
		return 0;
	}
	SPDK_DEBUGLOG(bdev_daos, "initializing DAOS engine\n");

	rc = daos_init();
	pthread_mutex_unlock(&g_bdev_daos_init_mutex);

	if (rc != -DER_ALREADY && rc) {
		return rc;
	}
	return 0;
}

static int
bdev_daos_put_engine(void)
{
	int rc = 0;

	pthread_mutex_lock(&g_bdev_daos_init_mutex);
	if (--g_bdev_daos_init_count > 0) {
		pthread_mutex_unlock(&g_bdev_daos_init_mutex);
		return 0;
	}
	SPDK_DEBUGLOG(bdev_daos, "de-initializing DAOS engine\n");

	rc = daos_fini();
	pthread_mutex_unlock(&g_bdev_daos_init_mutex);

	return rc;
}

static int
bdev_daos_initialize(void)
{
	/* DAOS engine and client initialization happens
	   during the first bdev creation */
	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(bdev_daos)
