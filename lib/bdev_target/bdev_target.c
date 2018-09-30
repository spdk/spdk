
#include "spdk/stdinc.h"

#include "spdk/thread.h"
#include "spdk/assert.h"
#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/event.h"
#include "spdk_internal/log.h"
#include "spdk/queue.h"

#include "spdk/bdev_target.h"


/* spdk env init start */
struct spdk_bt_io_channel {
	uint32_t bdev_core;
	struct spdk_bdev_desc	*desc;
	struct spdk_io_channel	*bdev_spdk_io_channel;

	struct spdk_bdev_target *bt;

//	pthread_spinlock_t	req_lock;
//	struct spdk_bt_request		*req_mem;
//	TAILQ_HEAD(, spdk_bt_request)	reqs;
//	int				nreq_in_use;
};

struct spdk_bt_bdev_channel {
	uint32_t bdev_core;
	struct spdk_io_channel	*bdev_spdk_io_channel;
};

struct spdk_bdev_target {
	const char		*bt_name;
	struct spdk_bdev	*bdev;
	struct spdk_bdev_desc	*desc;

	struct {
		struct spdk_io_channel		*target_io_channel;
		struct spdk_bt_bdev_channel	*bt_bdev_channel;
	} bdev_target;

	struct {
//		uint32_t	max_ops;
	} io_target;
};

#define SPDK_BT_MAX_NUM	16

struct spdk_thread_env_args {
	struct spdk_thread *thd;
	char *bt_names[SPDK_BT_MAX_NUM];
	struct spdk_io_channel *bt_spdk_io_channels[SPDK_BT_MAX_NUM];
};
__thread struct spdk_thread_env_args g_spdk_thread_env_args = {};

static void
_spdk_send_msg(spdk_thread_fn fn, void *ctx, void *thread_ctx)
{
	SPDK_DEBUGLOG(SPDK_LOG_BT, "BT-Backend Threadd send-msg\n");
	fn(ctx);
}

static int
_spdk_env_thread_init(void)
{
	if (g_spdk_thread_env_args.thd != NULL) {
		SPDK_DEBUGLOG(SPDK_LOG_BT, "BT thread already initialized\n");
		return 0;
	}

	g_spdk_thread_env_args.thd = spdk_allocate_thread(_spdk_send_msg, NULL, NULL, NULL, "spdk_bt_thread");
	if (!g_spdk_thread_env_args.thd) {
		SPDK_ERRLOG("Unable to initialize bt thread\n");
		return -1;
	}

	SPDK_DEBUGLOG(SPDK_LOG_BT, "bt thread initialized\n");
	return 0;
}

static struct spdk_io_channel *
_spdk_bt_alloc_io_channel(struct spdk_bdev_target *bt)
{
	struct spdk_io_channel *target_io_channel;

	target_io_channel = spdk_get_io_channel(&bt->io_target);

	SPDK_DEBUGLOG(SPDK_LOG_BT, "bt io channel is allocated\n");
	return target_io_channel;
}

static void
_spdk_bt_free_io_channel(struct spdk_io_channel *channel)
{
//	struct spdk_bt_io_channel *bt_io_channel;
//
//	bt_io_channel = spdk_io_channel_get_ctx(channel);
//	if (bt_io_channel->nreq_in_use > 0) {
//		SPDK_ERRLOG("bt io channel still has %d req in use\n", bt_io_channel->nreq_in_use);
//	}

	spdk_put_io_channel(channel);

	SPDK_DEBUGLOG(SPDK_LOG_BT, "bt io channel is freed\n");
}

static struct spdk_io_channel *
spdk_env_get_io_channel(struct spdk_bdev_target *bt)
{
	int i, rc;
	struct spdk_io_channel *io_channel;
	struct spdk_bt_io_channel *bt_io_channel;

	if (_spdk_env_thread_init()) {
		return NULL;
	}

	/* Check whether channel is allocated */
	for (i = 0; i < SPDK_BT_MAX_NUM; i++) {
		if (g_spdk_thread_env_args.bt_names[i] == NULL) {
			continue;
		}

		rc = strcmp(bt->bt_name, g_spdk_thread_env_args.bt_names[i]);
		if (rc) {
			continue;
		}

		/* Check whether it is a stale channel */
		io_channel = g_spdk_thread_env_args.bt_spdk_io_channels[i];
		bt_io_channel = spdk_io_channel_get_ctx(io_channel);
		if ((uint64_t)bt_io_channel->bt != (uint64_t)bt) {
			/* reallocate io channel for new bt */
			_spdk_bt_free_io_channel(io_channel);

			g_spdk_thread_env_args.bt_spdk_io_channels[i] =
					_spdk_bt_alloc_io_channel(bt);
		}

		return g_spdk_thread_env_args.bt_spdk_io_channels[i];
	}

	/* Assign one slot for bt to record its channel and name */
	for (i = 0; i < SPDK_BT_MAX_NUM; i++) {
		if (g_spdk_thread_env_args.bt_names[i] != NULL) {
			continue;
		}

		g_spdk_thread_env_args.bt_names[i] = strdup(bt->bt_name);
		g_spdk_thread_env_args.bt_spdk_io_channels[i] =
				_spdk_bt_alloc_io_channel(bt);

		return g_spdk_thread_env_args.bt_spdk_io_channels[i];
	}

	SPDK_ERRLOG("Unable to get io channel from spdk env\n");
	return NULL;
}
/* spdk env init end */

static int
_spdk_bt_bdev_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_target	*bt;
	struct spdk_bt_bdev_channel	*channel = ctx_buf;

	bt = SPDK_CONTAINEROF(io_device, struct spdk_bdev_target, bdev_target);
	channel->bdev_spdk_io_channel = spdk_bdev_get_io_channel(bt->desc);
	channel->bdev_core = spdk_env_get_current_core();

	SPDK_DEBUGLOG(SPDK_LOG_BT, "Create bdev channel\n");
	return 0;
}

static void
_spdk_bt_bdev_channel_destroy(void *io_device, void *ctx_buf)
{
	struct spdk_bt_bdev_channel	*channel = ctx_buf;

	if (channel->bdev_spdk_io_channel != NULL) {
		spdk_put_io_channel(channel->bdev_spdk_io_channel);
	}
	SPDK_DEBUGLOG(SPDK_LOG_BT, "Destroy bt bdev channel\n");
}

static int
_spdk_bt_io_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_target	*bt;
	struct spdk_bt_io_channel	*channel = ctx_buf;

	bt = SPDK_CONTAINEROF(io_device, struct spdk_bdev_target, io_target);
	channel->bt = bt;
	channel->desc = bt->desc;
	channel->bdev_core = bt->bdev_target.bt_bdev_channel->bdev_core;
	channel->bdev_spdk_io_channel = bt->bdev_target.bt_bdev_channel->bdev_spdk_io_channel;

//	/* Prepare req pool */
//	uint32_t i;
//	channel->req_mem = calloc(bt->io_target.max_ops, sizeof(struct spdk_bt_request));
//	if (!channel->req_mem) {
//		return -ENOMEM;
//	}
//	TAILQ_INIT(&channel->reqs);
//	for (i = 0; i < bt->io_target.max_ops; i++) {
//		TAILQ_INSERT_TAIL(&channel->reqs, &channel->req_mem[i], link);
//	}
//	pthread_spin_init(&channel->req_lock, 0);

	SPDK_DEBUGLOG(SPDK_LOG_BT, "Create bt io channel\n");
	return 0;
}

static void
_spdk_bt_io_channel_destroy(void *io_device, void *ctx_buf)
{
//	struct spdk_bt_io_channel *channel = ctx_buf;

//	free(channel->req_mem);
	SPDK_DEBUGLOG(SPDK_LOG_BT, "Destroy bt io channel\n");
}

/* spdk_bt_open start */
typedef void (*spdk_bt_open_cb)(void *cb_arg, struct spdk_bdev_target *bt);

struct _spdk_bt_open_internal_args {
	spdk_bt_open_cb cb_fn;
	void *cb_arg;
};

static void
_spdk_bt_open(void *arg1, void *arg2)
{
	char *bdev_name = arg1;
	struct _spdk_bt_open_internal_args *args = arg2;
	struct spdk_bdev *bdev;
	struct spdk_bdev_target *bt = NULL;
	int rc;

	SPDK_DEBUGLOG(SPDK_LOG_BT, "open bdev internal %s\n", bdev_name);
	//TODO: check whether this bdev bt is allocated.

	bdev = spdk_bdev_get_by_name(bdev_name);
	if (!bdev) {
		goto out;
	}

	bt = calloc(1, sizeof(*bt));
	if (bt == NULL) {
		goto out;
	}

	rc = spdk_bdev_open(bdev, true, NULL, NULL, &bt->desc);
	if (rc) {
		free(bt);
		bt = NULL;
		goto out;
	}
	bt->bdev = bdev;

//	//TODO: use spmc mode
//	bt->event_cp_ring = spdk_ring_create(0,512, SPDK_ENV_SOCKET_ID_ANY);
//	if (!bt->event_cp_ring) {
//		free(bt);
//		bt = NULL;
//		goto out;
//	}

	SPDK_DEBUGLOG(SPDK_LOG_BT, "start to register bdev target device\n");
	spdk_io_device_register(&bt->bdev_target, _spdk_bt_bdev_channel_create, _spdk_bt_bdev_channel_destroy,
				sizeof(struct spdk_bt_bdev_channel), "bdev_bdev_io");
	bt->bdev_target.target_io_channel = spdk_get_io_channel(&bt->bdev_target);
	bt->bdev_target.bt_bdev_channel = spdk_io_channel_get_ctx(bt->bdev_target.target_io_channel);

//	bt->io_target.max_ops = 512;
	spdk_io_device_register(&bt->io_target, _spdk_bt_io_channel_create, _spdk_bt_io_channel_destroy,
				sizeof(struct spdk_bt_io_channel), "bdev_user_io");

	bt->bt_name = strdup(bdev_name);

out:
	args->cb_fn(args->cb_arg, bt);
	free(args);
	return;
}

static int
_spdk_bt_open_async(char *bdev_name, spdk_bt_open_cb cb_fn, void *cb_arg)
{
	struct _spdk_bt_open_internal_args *args;
	uint32_t master_core;
	struct spdk_event *event;

	SPDK_DEBUGLOG(SPDK_LOG_BT, "open bdev %s\n", bdev_name);
	args = calloc(1, sizeof(*args));
	if (args == NULL) {
		cb_fn(cb_arg, NULL);
		return -ENOMEM;
	}

	args->cb_fn = cb_fn;
	args->cb_arg = cb_arg;

	master_core = spdk_env_get_first_core();
	event = spdk_event_allocate(master_core, _spdk_bt_open, bdev_name, args);
	spdk_event_call(event);

	return 0;
}

struct _spdk_bt_open_cb_sync_args {
	sem_t *sem;
	struct spdk_bdev_target *bt;
};

static void
_spdk_bt_open_cb_sync(void *cb_arg, struct spdk_bdev_target *bt)
{
	struct _spdk_bt_open_cb_sync_args *args = (struct _spdk_bt_open_cb_sync_args *)cb_arg;

	SPDK_DEBUGLOG(SPDK_LOG_BT, "bt is opened at %p\n", bt);
	args->bt = bt;
	sem_post(args->sem);
}

int
spdk_bt_open(char *bdev_name, struct spdk_bdev_target **bt)
{
	sem_t sem;
	struct _spdk_bt_open_cb_sync_args args = {};

	sem_init(&sem, 0, 0);
	args.sem = &sem;

	_spdk_bt_open_async(bdev_name, _spdk_bt_open_cb_sync, &args);

	sem_wait(&sem);
	*bt = args.bt;

	return 0;
}
/* spdk_bt_open end */

/* spdk_bt_close start */
static void
_spdk_bt_close(void *arg1, void *arg2)
{
	struct spdk_bdev_target *bt = arg1;
	sem_t *sem = arg2;

	spdk_put_io_channel(bt->bdev_target.target_io_channel);

	spdk_io_device_unregister(&bt->bdev_target, NULL);
	spdk_io_device_unregister(&bt->io_target, NULL);

	spdk_bdev_close(bt->desc);
//	spdk_ring_free(bt->event_cp_ring);

	free((void *)bt->bt_name);
	free(bt);
	sem_post(sem);
}

int
spdk_bt_close(struct spdk_bdev_target *bt)
{
	sem_t sem;
	uint32_t master_core;
	struct spdk_event *event;

	sem_init(&sem, 0, 0);
	master_core = spdk_env_get_first_core();
	event = spdk_event_allocate(master_core, _spdk_bt_close, bt, &sem);
	spdk_event_call(event);
	sem_wait(&sem);

	SPDK_DEBUGLOG(SPDK_LOG_BT, "bt is closed\n");
	return 0;
}
/* spdk_bt_close end */

/* transfer func start */
typedef void (*bt_request_fn)(void *);

static void
__call_fn(void *arg1, void *arg2)
{
	bt_request_fn fn;

	fn = (bt_request_fn)arg1;
	fn(arg2);
}

static int
_send_request(uint32_t core, bt_request_fn fn, void *arg)
{
	struct spdk_event *event;

	event = spdk_event_allocate(core, __call_fn, (void *)fn, arg);
	if (event == NULL) {
		SPDK_ERRLOG("Unable to allocate event\n");
		return -1;
	}

	spdk_event_call(event);
	return 0;
}
/* transfer func end */



int
spdk_bdev_aio_ctx_setup(struct spdk_bdev_aio_ctx *ctx, struct spdk_bdev_target *bt)
{
	struct spdk_bt_io_channel *bt_io_channel;
	struct spdk_io_channel *bt_spdk_io_channel;

	SPDK_DEBUGLOG(SPDK_LOG_BT, "BT setup aio ctx\n");
	bt_spdk_io_channel = spdk_env_get_io_channel(bt);
	bt_io_channel = spdk_io_channel_get_ctx(bt_spdk_io_channel);
	if (bt_io_channel == NULL) {
		return -1;
	}

	memset(ctx, 0, sizeof(*ctx));
	TAILQ_INIT(&ctx->submitting_list);
	TAILQ_INIT(&ctx->completed_list);

	ctx->bt = bt;
	ctx->desc = bt_io_channel->desc;
	ctx->bdev_spdk_io_channel = bt_io_channel->bdev_spdk_io_channel;
	ctx->bdev_core = bt_io_channel->bdev_core;

	return 0;
}

/* aio_ctx_get_reqs start */
static void
__aio_ctx_get_reqs(void *_args)
{
	struct spdk_bdev_aio_get_reqs_ctx *get_reqs = _args;
	struct spdk_bdev_aio_ctx *ctx = get_reqs->ctx;
	struct spdk_bdev_aio_req *req;
	int i, nreqs;

	SPDK_DEBUGLOG(SPDK_LOG_BT, "bt internal get requests\n");
	assert(ctx->reqs_completed >= 0);
	assert(ctx->reqs_submitted >= 0);
	assert(ctx->reqs_submitting >= 0);

	/* check whether get_gets notify is already registered */
	if (ctx->get_reqs != NULL) {
		get_reqs->get_reqs_rc = -EBUSY;
		get_reqs->get_reqs_cb(get_reqs->get_reqs_cb_arg);
		return;
	}

	/* whether need to run out all reqs */
	if (get_reqs->all == true) {
		get_reqs->nr_min = ctx->reqs_completed + ctx->reqs_submitted + ctx->reqs_submitting;
	}

	/* register get_reqs if completed reqs are not enough */
	if (ctx->reqs_completed < get_reqs->nr_min) {
		ctx->get_reqs = get_reqs;
		return;
	}

	/* notify get_reqs */
	nreqs = spdk_min(ctx->reqs_completed, get_reqs->nr);
	for (i = 0; i < nreqs; i++) {
		req = TAILQ_FIRST(&ctx->completed_list);
		TAILQ_REMOVE(&ctx->completed_list, req, req_tailq);
		if (get_reqs->reqs) {
			get_reqs->reqs[i] = req;
		}
	}

	get_reqs->get_reqs_cb(get_reqs->get_reqs_cb_arg);
}

static void
_aio_ctx_get_reqs_cb(void *cb_arg)
{
	sem_t *sem = cb_arg;
	sem_post(sem);
}

int
spdk_bdev_aio_ctx_get_reqs(struct spdk_bdev_aio_ctx *ctx,
		int nr_min, int nr, struct spdk_bdev_aio_req *reqs[],
		struct timespec *timeout)
{
	struct spdk_bdev_aio_get_reqs_ctx get_reqs[1] = {0};
	int rc;
	sem_t sem;

	sem_init(&sem, 0, 0);
	get_reqs->get_reqs_cb_arg = &sem;
	get_reqs->get_reqs_cb = _aio_ctx_get_reqs_cb;

	if (nr_min == -1) {
		get_reqs->all = true;
	} else {
		get_reqs->nr_min = nr_min;
	}
	get_reqs->nr = nr;
	get_reqs->reqs = reqs;

	get_reqs->ctx = ctx;

	SPDK_DEBUGLOG(SPDK_LOG_BT, "bt get requests\n");
	rc = _send_request(ctx->bdev_core, __aio_ctx_get_reqs, get_reqs);
	if (rc) {
		SPDK_ERRLOG("Failed to get_reqs (rc is %d)\n", rc);
		return rc;

	}

	sem_wait(&sem);

	return get_reqs->get_reqs_rc;
}
/* aio_ctx_get_reqs end */

int
spdk_bdev_aio_ctx_destroy(struct spdk_bdev_aio_ctx *ctx, bool polling_check)
{
	int rc = 0;

	//TODO: if submit success, then waiting for all reqs completed.
	if (polling_check) {
		rc = spdk_bdev_aio_ctx_get_reqs(ctx,
				-1, INT_MAX, NULL, NULL);
	}

	return rc;
}

/* Internal complete start */
static void
_bdev_aio_ctx_req_complete(void *arg, int bterrno, struct spdk_bdev_ret *nvm_ret)
{
	struct spdk_bdev_aio_req *req = arg;
	struct spdk_bdev_aio_ctx *ctx = req->ctx;
	int nreqs, i;

	SPDK_DEBUGLOG(SPDK_LOG_BT, "bdev target bdev cmd complete req\n");
	assert(ctx->reqs_completed >= 0);
	assert(ctx->reqs_submitted >= 0);
	assert(ctx->reqs_submitting >= 0);

	//TODO: consider series reqs process

	ctx->reqs_submitted--;

	/* the req has its own completion notify callback */
	if (req->user_complete_cb) {
		req->user_complete_cb(req, bterrno, nvm_ret);
		return;
	} else {
		/* record req result */
		req->req_rc = bterrno;
		if (nvm_ret) {
			req->ret = *nvm_ret;
		}
		ctx->reqs_completed++;
		TAILQ_INSERT_TAIL(&ctx->completed_list, req, req_tailq);
	}

	if (ctx->get_reqs == NULL) {
		return;
	}

	SPDK_DEBUGLOG(SPDK_LOG_BT, "bt internal ctx completed req is %d, min_nr %d\n", ctx->reqs_completed, ctx->get_reqs->nr_min);
	/* whether need to run out all reqs */
	if (ctx->get_reqs->all == true) {
		ctx->get_reqs->nr_min = ctx->reqs_completed + ctx->reqs_submitted + ctx->reqs_submitting;
	}

	/* check whether get_gets notify is expected */
	if (ctx->get_reqs->get_reqs_cb == NULL || ctx->reqs_completed < ctx->get_reqs->nr_min) {
		return;
	}

	/* notify get_reqs */
	nreqs = spdk_min(ctx->reqs_completed, ctx->get_reqs->nr);
	for (i = 0; i < nreqs; i++) {
		req = TAILQ_FIRST(&ctx->completed_list);
		TAILQ_REMOVE(&ctx->completed_list, req, req_tailq);
		if (ctx->get_reqs->reqs) {
			ctx->get_reqs->reqs[i] = req;
		}
	}

	ctx->get_reqs->get_reqs_cb(ctx->get_reqs->get_reqs_cb_arg);
	/* unregister get_reqs */
	ctx->get_reqs = NULL;
}

static void
_bt_bdev_complete(struct spdk_bdev_io *bdev_io,
		bool success,
		void *cb_arg)
{
	struct spdk_bdev_ret nvm_ret = {};
	int sct, sc;

	SPDK_DEBUGLOG(SPDK_LOG_BT, "bdev target bdev cmd complete\n");
	spdk_bdev_io_get_nvme_status(bdev_io, &sct, &sc);
	nvm_ret.status = (sct << 8) | sc;

	if (!success) {
		SPDK_NOTICELOG("submit command error: SC %x SCT %x\n", sc, sct);
	}

	_bdev_aio_ctx_req_complete(cb_arg, 0, &nvm_ret);

	spdk_bdev_free_io(bdev_io);
}
/* Internal complete end */

/* aio_ctx_submit start */
static void
__aio_ctx_submit(void *_args)
{
	struct spdk_bdev_aio_ctx *ctx = _args;
	struct spdk_bdev_aio_req *req, *req_tmp;

	SPDK_DEBUGLOG(SPDK_LOG_BT, "BT internal sends out %d requests\n", ctx->reqs_submitting);
	if (!TAILQ_EMPTY(&ctx->submitting_list)) {
		TAILQ_FOREACH_SAFE(req, &ctx->submitting_list, req_tailq, req_tmp) {
			TAILQ_REMOVE(&ctx->submitting_list, req, req_tailq);
			ctx->reqs_submitting--;
			ctx->reqs_submitted++;
			SPDK_DEBUGLOG(SPDK_LOG_BT, "BT internal queue req %p fn %p\n", req, req->queue_req_fn);
			req->queue_req_fn(req);
		}
	}
}

int
spdk_bdev_aio_ctx_submit(struct spdk_bdev_aio_ctx *ctx,
		int nr, struct spdk_bdev_aio_req *reqs[])
{
	struct spdk_bdev_aio_req *req;
	int i, rc;

	for (i = 0; i < nr; i++) {
		SPDK_DEBUGLOG(SPDK_LOG_BT, "BT aio ctx submits request %p\n", reqs[i]);
		reqs[i]->ctx = ctx;
		TAILQ_INSERT_TAIL(&ctx->submitting_list, reqs[i], req_tailq);
		ctx->reqs_submitting++;
	}

	SPDK_DEBUGLOG(SPDK_LOG_BT, "BT sends out %d requests\n", ctx->reqs_submitting);
	rc = _send_request(ctx->bdev_core, __aio_ctx_submit, ctx);
	if (rc) {
		for (i = 0; i < nr; i++) {
			req = TAILQ_LAST(&ctx->submitting_list, req_submitting_list);
			TAILQ_REMOVE(&ctx->submitting_list, req, req_tailq);
			ctx->reqs_submitting--;
		}
	}

	return rc;
}
/* aio_ctx_submit end */

void
spdk_bdev_aio_req_set_cb(struct spdk_bdev_aio_req *req, spdk_bdev_aio_req_complete_cb cb, void *cb_arg)
{
	req->user_complete_cb = cb;
	req->complete_cb_arg = cb_arg;
}

static void
__passthru_from_ot(void *_args)
{
	struct spdk_bdev_aio_req *req = _args;
	struct spdk_bdev_aio_ctx *ctx = req->ctx;
	int rc;

	SPDK_DEBUGLOG(SPDK_LOG_BT, "bdev target process passthru cmd\n");
	if (req->op.passthru.is_admin) {
		rc = spdk_bdev_nvme_admin_passthru(ctx->desc, ctx->bdev_spdk_io_channel,
					req->op.passthru.cmd, req->op.passthru.pin_buf, req->op.passthru.data_len,
					_bt_bdev_complete, req);
	} else {
		rc = spdk_bdev_nvme_io_passthru_md(ctx->desc, ctx->bdev_spdk_io_channel,
					req->op.passthru.cmd, req->op.passthru.pin_buf, req->op.passthru.data_len,
					req->op.passthru.pin_meta, req->op.passthru.md_len,
					_bt_bdev_complete, req);
	}

	SPDK_DEBUGLOG(SPDK_LOG_BT, "bdev target sent out passthru request (rc is %d)\n", rc);
	if (rc) {
		_bdev_aio_ctx_req_complete(req, rc, NULL);
	}
}

void
spdk_bdev_aio_req_prep_admin_passthru(struct spdk_bdev_aio_req *req,
		struct spdk_nvme_cmd *cmd, void *pin_buf, size_t data_len)
{
	memset(req, 0, sizeof(*req));
	req->op.passthru.cmd = cmd;

	req->op.passthru.pin_buf = pin_buf;
	req->op.passthru.data_len = data_len;
	req->op.passthru.is_admin = true;
	req->queue_req_fn = __passthru_from_ot;
	printf("req %p passthru fn %p\n", req, req->queue_req_fn);
	req->user_complete_cb = NULL;
}

void
spdk_bdev_aio_req_prep_io_passthru(struct spdk_bdev_aio_req *req, struct spdk_nvme_cmd *cmd,
		void *pin_buf, size_t data_len, void *pin_meta, size_t md_len)
{
	memset(req, 0, sizeof(*req));
	req->op.passthru.cmd = cmd;

	req->op.passthru.pin_buf = pin_buf;
	req->op.passthru.data_len = data_len;
	req->op.passthru.pin_meta = pin_meta;
	req->op.passthru.md_len = md_len;
	req->op.passthru.is_admin = false;

	req->queue_req_fn = __passthru_from_ot;
	req->user_complete_cb = NULL;
}

/* Sync version API start */
static void
_spdk_bt_io_cb_sync(void *cb_arg, int bterrno, struct spdk_bdev_ret *ret)
{
	struct spdk_bdev_aio_req *req = cb_arg;
	sem_t *sem = req->complete_cb_arg;

	req->req_rc = bterrno;
	if (ret) {
		req->ret = *ret;
	}

	sem_post(sem);
}

int
spdk_bdev_aio_req_admin_passthru_sync(struct spdk_bdev_target *bt, struct spdk_nvme_cmd *cmd,
		void *pin_buf, size_t data_len, struct spdk_bdev_ret *ret)
{
	struct spdk_bdev_aio_ctx ctx[1];
	struct spdk_bdev_aio_req req[1];
	struct spdk_bdev_aio_req *reqs[1];

	int rc;
	sem_t sem;

	SPDK_DEBUGLOG(SPDK_LOG_BT, "BT admin passthru\n");
	sem_init(&sem, 0, 0);
	rc = spdk_bdev_aio_ctx_setup(ctx, bt);

	spdk_bdev_aio_req_prep_admin_passthru(req, cmd, pin_buf, data_len);
	spdk_bdev_aio_req_set_cb(req, _spdk_bt_io_cb_sync, &sem);
	reqs[0] = req;

	rc = spdk_bdev_aio_ctx_submit(ctx, 1, reqs);
	if (rc) {
		SPDK_INFOLOG(SPDK_LOG_BT, "Failed to submit ctx's req. rc = %d\n", rc);
		_spdk_bt_io_cb_sync(&sem, rc, NULL);
	}

	sem_wait(&sem);
	spdk_bdev_aio_ctx_destroy(ctx, false);

	if (req->req_rc == 0 && req->ret.status) {
		req->req_rc = -1;
	}

	if (ret) {
		*ret = req->ret;
	}
	return req->req_rc;
}

int
spdk_bdev_aio_req_io_passthru_sync(struct spdk_bdev_target *bt, struct spdk_nvme_cmd *cmd,
		void *pin_buf, size_t data_len, void *pin_meta, size_t md_len, struct spdk_bdev_ret *ret)
{
	struct spdk_bdev_aio_ctx ctx[1];
	struct spdk_bdev_aio_req req[1];
	struct spdk_bdev_aio_req *reqs[1];
	int rc;
	sem_t sem;

	SPDK_DEBUGLOG(SPDK_LOG_BT, "BT io passthru\n");
	sem_init(&sem, 0, 0);
	rc = spdk_bdev_aio_ctx_setup(ctx, bt);

	spdk_bdev_aio_req_prep_io_passthru(req, cmd, pin_buf, data_len, pin_meta, md_len);
	spdk_bdev_aio_req_set_cb(req, _spdk_bt_io_cb_sync, &sem);
	reqs[0] = req;

	rc = spdk_bdev_aio_ctx_submit(ctx, 1, reqs);
	if (rc) {
		SPDK_INFOLOG(SPDK_LOG_BT, "Failed to submit ctx's req. rc = %d\n", rc);
		_spdk_bt_io_cb_sync(&sem, rc, NULL);
	}

	sem_wait(&sem);
	spdk_bdev_aio_ctx_destroy(ctx, false);

	if (req->req_rc == 0 && req->ret.status) {
		req->req_rc = -1;
	}

	if (ret) {
		*ret = req->ret;
	}
	return req->req_rc;
}
/* Sync version API end */

SPDK_LOG_REGISTER_COMPONENT("bdev_target", SPDK_LOG_BT)
