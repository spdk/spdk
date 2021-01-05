#include "bdev_aio.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/bdev_module.h"

#include <libaio.h>

#include "bdev_aio_task.h"
#include "bdev_aio_sync.h"

#define MAX_QUEUE_LEN 1024

struct queue
{
	void *message[MAX_QUEUE_LEN];
	size_t used;
};

static struct queue g_queue;

static void
_init_queue(void)
{
	g_queue.used = 0;
}

static int
_enqueue(void *message)
{
	if (g_queue.used < MAX_QUEUE_LEN)
	{
		g_queue.message[g_queue.used++] = message;

		return 0;
	}

	return -1;
}

static size_t
_dequeue(void *message[], size_t n)
{
	size_t i;

	if (g_queue.used > n)
	{
		for (i = 0; i < n; i++)
		{
			message[i] = g_queue.message[i];
		}

		g_queue.used -= n;

		for (i = 0; i < g_queue.used; i++)
		{
			g_queue.message[i] = g_queue.message[n + i];
		}

		return n;
	}

	if (g_queue.used > 0)
	{
		n = g_queue.used;
		g_queue.used = 0;

		for (i = 0; i < n; i++)
		{
			message[i] = g_queue.message[i];
		}

		return n;
	}

	return 0;
}

static pthread_mutex_t g_mutex;

static pthread_cond_t g_cond;

void
aio_complete(struct spdk_bdev_io *bdev_io, int status)
{
	spdk_bdev_io_complete(bdev_io, (status == 0) ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED);
}

static void
aio_call_complete_fn(void *arg)
{
	struct aio_request_ctx *ctx = arg;

	aio_complete(spdk_bdev_io_from_ctx(ctx->aio_task), ctx->status);

	free(arg);
}

static void
aio_call_request_fn(void *arg)
{
	struct aio_request_ctx *ctx = arg;

	ctx->fn(ctx);

	spdk_set_thread(ctx->thread);

	spdk_thread_send_msg(ctx->thread, aio_call_complete_fn, ctx);

	spdk_set_thread(NULL);
}

int
aio_remote_request(aio_request_fn fn, void *arg)
{
	struct aio_request_ctx *ctx = arg;
	int status;

	ctx->fn = fn;

	pthread_mutex_lock(&g_mutex);

	status = _enqueue(ctx);

	pthread_mutex_unlock(&g_mutex);

	pthread_cond_signal(&g_cond);

	return status;
}

void
aio_local_request(aio_request_fn fn, void *arg)
{
	struct aio_request_ctx *ctx = arg;

	ctx->fn = fn;

	ctx->fn(ctx);

	aio_call_complete_fn(ctx);
}

struct aio_request_ctx *
create_aio_request_ctx(struct spdk_bdev_io *bdev_io)
{
	struct aio_request_ctx *ctx;

	ctx = calloc(1, sizeof(struct aio_request_ctx));

	if (ctx != NULL)
	{
		ctx->fdisk = bdev_io->bdev->ctxt,
		ctx->aio_task = (struct bdev_aio_task *)bdev_io->driver_ctx;
		ctx->thread = spdk_get_thread();
	}

	return ctx;
}

#define BATCH_SIZE 64

static pthread_t g_blocking_worker_thread;

static bool g_exit;

static void *
blocking_worker(void *arg)
{
	void *message[BATCH_SIZE];
	size_t count;
	size_t i;

	pthread_mutex_lock(&g_mutex);

	for (;;)
	{
		for (;;)
		{
			count = _dequeue(message, BATCH_SIZE);

			if (count == 0)
			{
				break;
			}

			pthread_mutex_unlock(&g_mutex);

			for (i = 0; i < count; i++)
			{
				aio_call_request_fn(message[i]);
			}

			pthread_mutex_lock(&g_mutex);
		}

		if (g_exit)
		{
			break;
		}

		pthread_cond_wait(&g_cond, &g_mutex);
	}

	pthread_mutex_unlock(&g_mutex);

	return NULL;
}

void
aio_sync_init(void)
{
	_init_queue();

	g_exit = false;

	pthread_mutex_init(&g_mutex, NULL);

	pthread_cond_init(&g_cond, NULL);

	pthread_create(&g_blocking_worker_thread, NULL, blocking_worker, NULL);
}

void
aio_sync_fini(void)
{
	g_exit = true;

	pthread_cond_signal(&g_cond);

	pthread_join(g_blocking_worker_thread, NULL);

	pthread_cond_destroy(&g_cond);

	pthread_mutex_destroy(&g_mutex);
}
