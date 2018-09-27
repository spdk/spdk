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
#include "spdk/copy_engine.h"
#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/queue.h"

#include "config-host.h"
#include "fio.h"
#include "optgroup.h"

struct spdk_fio_options {
	void *pad;
	char *conf;
	unsigned mem_mb;
	bool mem_single_seg;
};

/* Used to pass messages between fio threads */
struct spdk_fio_msg {
	spdk_thread_fn	cb_fn;
	void		*cb_arg;
};

/* A polling function */
struct spdk_fio_poller {
	spdk_poller_fn		cb_fn;
	void			*cb_arg;
	uint64_t		period_microseconds;

	TAILQ_ENTRY(spdk_fio_poller)	link;
};

struct spdk_fio_request {
	struct io_u		*io;
	struct thread_data	*td;
};

struct spdk_fio_target {
	struct spdk_bdev	*bdev;
	struct spdk_bdev_desc	*desc;
	struct spdk_io_channel	*ch;

	TAILQ_ENTRY(spdk_fio_target) link;
};

struct spdk_fio_thread {
	struct thread_data		*td; /* fio thread context */
	struct spdk_thread		*thread; /* spdk thread context */
	struct spdk_ring		*ring; /* ring for passing messages to this thread */
	TAILQ_HEAD(, spdk_fio_poller)	pollers; /* List of registered pollers on this thread */

	TAILQ_HEAD(, spdk_fio_target)	targets;

	struct io_u		**iocq;		// io completion queue
	unsigned int		iocq_count;	// number of iocq entries filled by last getevents
	unsigned int		iocq_size;	// number of iocq entries allocated
};

static struct spdk_fio_thread *g_init_thread = NULL;
static pthread_t g_init_thread_id = 0;
static bool g_spdk_env_initialized = false;

static int spdk_fio_init(struct thread_data *td);
static void spdk_fio_cleanup(struct thread_data *td);
static size_t spdk_fio_poll_thread(struct spdk_fio_thread *fio_thread);

static void
spdk_fio_send_msg(spdk_thread_fn fn, void *ctx, void *thread_ctx)
{
	struct spdk_fio_thread *thread = thread_ctx;
	struct spdk_fio_msg *msg;
	size_t count;

	msg = calloc(1, sizeof(*msg));
	assert(msg != NULL);

	msg->cb_fn = fn;
	msg->cb_arg = ctx;

	count = spdk_ring_enqueue(thread->ring, (void **)&msg, 1);
	if (count != 1) {
		SPDK_ERRLOG("Unable to send message to thread %p. rc: %lu\n", thread, count);
	}
}

static void
spdk_fio_bdev_init_done(void *cb_arg, int rc)
{
	*(bool *)cb_arg = true;
}

static struct spdk_poller *
spdk_fio_start_poller(void *thread_ctx,
		      spdk_poller_fn fn,
		      void *arg,
		      uint64_t period_microseconds)
{
	struct spdk_fio_thread *fio_thread = thread_ctx;
	struct spdk_fio_poller *fio_poller;

	fio_poller = calloc(1, sizeof(*fio_poller));
	if (!fio_poller) {
		SPDK_ERRLOG("Unable to allocate poller\n");
		return NULL;
	}

	fio_poller->cb_fn = fn;
	fio_poller->cb_arg = arg;
	fio_poller->period_microseconds = period_microseconds;

	TAILQ_INSERT_TAIL(&fio_thread->pollers, fio_poller, link);

	return (struct spdk_poller *)fio_poller;
}

static void
spdk_fio_stop_poller(struct spdk_poller *poller, void *thread_ctx)
{
	struct spdk_fio_poller *fio_poller;
	struct spdk_fio_thread *fio_thread = thread_ctx;

	fio_poller = (struct spdk_fio_poller *)poller;

	TAILQ_REMOVE(&fio_thread->pollers, fio_poller, link);

	free(fio_poller);
}

static int
spdk_fio_init_thread(struct thread_data *td)
{
	struct spdk_fio_thread *fio_thread;

	fio_thread = calloc(1, sizeof(*fio_thread));
	if (!fio_thread) {
		SPDK_ERRLOG("failed to allocate thread local context\n");
		return -1;
	}

	fio_thread->td = td;
	td->io_ops_data = fio_thread;

	fio_thread->ring = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 4096, SPDK_ENV_SOCKET_ID_ANY);
	if (!fio_thread->ring) {
		SPDK_ERRLOG("failed to allocate ring\n");
		free(fio_thread);
		return -1;
	}

	fio_thread->thread = spdk_allocate_thread(spdk_fio_send_msg,
			     spdk_fio_start_poller,
			     spdk_fio_stop_poller,
			     fio_thread,
			     "fio_thread");
	if (!fio_thread->thread) {
		spdk_ring_free(fio_thread->ring);
		free(fio_thread);
		SPDK_ERRLOG("failed to allocate thread\n");
		return -1;
	}

	TAILQ_INIT(&fio_thread->pollers);

	fio_thread->iocq_size = td->o.iodepth;
	fio_thread->iocq = calloc(fio_thread->iocq_size, sizeof(struct io_u *));
	assert(fio_thread->iocq != NULL);

	TAILQ_INIT(&fio_thread->targets);

	return 0;
}

static void *
spdk_init_thread_poll(void *arg)
{
	struct spdk_fio_thread *thread = arg;
	int oldstate;
	int rc;

	/* Loop until the thread is cancelled */
	while (true) {
		rc = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);
		if (rc != 0) {
			SPDK_ERRLOG("Unable to set cancel state disabled on g_init_thread (%d): %s\n",
				    rc, spdk_strerror(rc));
		}

		spdk_fio_poll_thread(thread);

		rc = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);
		if (rc != 0) {
			SPDK_ERRLOG("Unable to set cancel state enabled on g_init_thread (%d): %s\n",
				    rc, spdk_strerror(rc));
		}

		/* This is a pthread cancellation point and cannot be removed. */
		sleep(1);
	}

	return NULL;
}

static int
spdk_fio_init_env(struct thread_data *td)
{
	struct spdk_fio_thread		*fio_thread;
	struct spdk_fio_options		*eo;
	bool				done = false;
	int				rc;
	struct spdk_conf		*config;
	struct spdk_env_opts		opts;
	size_t				count;

	/* Parse the SPDK configuration file */
	eo = td->eo;
	if (!eo->conf || !strlen(eo->conf)) {
		SPDK_ERRLOG("No configuration file provided\n");
		return -1;
	}

	config = spdk_conf_allocate();
	if (!config) {
		SPDK_ERRLOG("Unable to allocate configuration file\n");
		return -1;
	}

	rc = spdk_conf_read(config, eo->conf);
	if (rc != 0) {
		SPDK_ERRLOG("Invalid configuration file format\n");
		spdk_conf_free(config);
		return -1;
	}
	if (spdk_conf_first_section(config) == NULL) {
		SPDK_ERRLOG("Invalid configuration file format\n");
		spdk_conf_free(config);
		return -1;
	}
	spdk_conf_set_as_default(config);

	/* Initialize the environment library */
	spdk_env_opts_init(&opts);
	opts.name = "fio";

	if (eo->mem_mb) {
		opts.mem_size = eo->mem_mb;
	}
	opts.hugepage_single_segments = eo->mem_single_seg;

	if (spdk_env_init(&opts) < 0) {
		SPDK_ERRLOG("Unable to initialize SPDK env\n");
		spdk_conf_free(config);
		return -1;
	}
	spdk_unaffinitize_thread();

	/* Create an SPDK thread temporarily */
	rc = spdk_fio_init_thread(td);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to create initialization thread\n");
		return -1;
	}

	g_init_thread = fio_thread = td->io_ops_data;

	/* Initialize the copy engine */
	spdk_copy_engine_initialize();

	/* Initialize the bdev layer */
	spdk_bdev_initialize(spdk_fio_bdev_init_done, &done);

	/* First, poll until initialization is done. */
	do {
		spdk_fio_poll_thread(fio_thread);
	} while (!done);

	/*
	 * Continue polling until there are no more events.
	 * This handles any final events posted by pollers.
	 */
	do {
		count = spdk_fio_poll_thread(fio_thread);
	} while (count > 0);

	/*
	 * Spawn a thread to continue polling this thread
	 * occasionally.
	 */

	rc = pthread_create(&g_init_thread_id, NULL, &spdk_init_thread_poll, fio_thread);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to spawn thread to poll admin queue. It won't be polled.\n");
	}

	return 0;
}

/* Called for each thread to fill in the 'real_file_size' member for
 * each file associated with this thread. This is called prior to
 * the init operation (spdk_fio_init()) below. This call will occur
 * on the initial start up thread if 'create_serialize' is true, or
 * on the thread actually associated with 'thread_data' if 'create_serialize'
 * is false.
 */
static int
spdk_fio_setup(struct thread_data *td)
{
	unsigned int i;
	struct fio_file *f;

	if (!td->o.use_thread) {
		SPDK_ERRLOG("must set thread=1 when using spdk plugin\n");
		return -1;
	}

	if (!g_spdk_env_initialized) {
		if (spdk_fio_init_env(td)) {
			SPDK_ERRLOG("failed to initialize\n");
			return -1;
		}

		g_spdk_env_initialized = true;
	}

	for_each_file(td, f, i) {
		struct spdk_bdev *bdev;

		bdev = spdk_bdev_get_by_name(f->file_name);
		if (!bdev) {
			SPDK_ERRLOG("Unable to find bdev with name %s\n", f->file_name);
			return -1;
		}

		f->real_file_size = spdk_bdev_get_num_blocks(bdev) *
				    spdk_bdev_get_block_size(bdev);

	}

	return 0;
}

/* Called for each thread, on that thread, shortly after the thread
 * starts.
 */
static int
spdk_fio_init(struct thread_data *td)
{
	struct spdk_fio_thread *fio_thread;
	unsigned int i;
	struct fio_file *f;
	int rc;

	spdk_fio_init_thread(td);

	fio_thread = td->io_ops_data;

	for_each_file(td, f, i) {
		struct spdk_fio_target *target;

		target = calloc(1, sizeof(*target));
		if (!target) {
			SPDK_ERRLOG("Unable to allocate memory for I/O target.\n");
			return -1;
		}

		target->bdev = spdk_bdev_get_by_name(f->file_name);
		if (!target->bdev) {
			SPDK_ERRLOG("Unable to find bdev with name %s\n", f->file_name);
			free(target);
			return -1;
		}

		rc = spdk_bdev_open(target->bdev, true, NULL, NULL, &target->desc);
		if (rc) {
			SPDK_ERRLOG("Unable to open bdev %s\n", f->file_name);
			free(target);
			return -1;
		}

		target->ch = spdk_bdev_get_io_channel(target->desc);
		if (!target->ch) {
			SPDK_ERRLOG("Unable to get I/O channel for bdev.\n");
			spdk_bdev_close(target->desc);
			free(target);
			return -1;
		}

		f->engine_data = target;

		TAILQ_INSERT_TAIL(&fio_thread->targets, target, link);
	}

	return 0;
}

static void
spdk_fio_cleanup_thread(struct spdk_fio_thread *fio_thread)
{
	struct spdk_fio_target *target, *tmp;

	TAILQ_FOREACH_SAFE(target, &fio_thread->targets, link, tmp) {
		TAILQ_REMOVE(&fio_thread->targets, target, link);
		spdk_put_io_channel(target->ch);
		spdk_bdev_close(target->desc);
		free(target);
	}

	while (spdk_fio_poll_thread(fio_thread) > 0) {}

	spdk_free_thread();
	spdk_ring_free(fio_thread->ring);
	free(fio_thread->iocq);
	free(fio_thread);
}

static void
spdk_fio_cleanup(struct thread_data *td)
{
	struct spdk_fio_thread *fio_thread = td->io_ops_data;

	spdk_fio_cleanup_thread(fio_thread);
	td->io_ops_data = NULL;
}

static int
spdk_fio_open(struct thread_data *td, struct fio_file *f)
{

	return 0;
}

static int
spdk_fio_close(struct thread_data *td, struct fio_file *f)
{
	return 0;
}

static int
spdk_fio_iomem_alloc(struct thread_data *td, size_t total_mem)
{
	td->orig_buffer = spdk_dma_zmalloc(total_mem, 0x1000, NULL);
	return td->orig_buffer == NULL;
}

static void
spdk_fio_iomem_free(struct thread_data *td)
{
	spdk_dma_free(td->orig_buffer);
}

static int
spdk_fio_io_u_init(struct thread_data *td, struct io_u *io_u)
{
	struct spdk_fio_request	*fio_req;

	fio_req = calloc(1, sizeof(*fio_req));
	if (fio_req == NULL) {
		return 1;
	}
	fio_req->io = io_u;
	fio_req->td = td;

	io_u->engine_data = fio_req;

	return 0;
}

static void
spdk_fio_io_u_free(struct thread_data *td, struct io_u *io_u)
{
	struct spdk_fio_request *fio_req = io_u->engine_data;

	if (fio_req) {
		assert(fio_req->io == io_u);
		free(fio_req);
		io_u->engine_data = NULL;
	}
}

static void
spdk_fio_completion_cb(struct spdk_bdev_io *bdev_io,
		       bool success,
		       void *cb_arg)
{
	struct spdk_fio_request		*fio_req = cb_arg;
	struct thread_data		*td = fio_req->td;
	struct spdk_fio_thread		*fio_thread = td->io_ops_data;

	assert(fio_thread->iocq_count < fio_thread->iocq_size);
	fio_req->io->error = success ? 0 : EIO;
	fio_thread->iocq[fio_thread->iocq_count++] = fio_req->io;

	spdk_bdev_free_io(bdev_io);
}

#if FIO_IOOPS_VERSION >= 24
typedef enum fio_q_status fio_q_status_t;
#else
typedef int fio_q_status_t;
#endif

static fio_q_status_t
spdk_fio_queue(struct thread_data *td, struct io_u *io_u)
{
	int rc = 1;
	struct spdk_fio_request	*fio_req = io_u->engine_data;
	struct spdk_fio_target *target = io_u->file->engine_data;

	assert(fio_req->td == td);

	if (!target) {
		SPDK_ERRLOG("Unable to look up correct I/O target.\n");
		fio_req->io->error = ENODEV;
		return FIO_Q_COMPLETED;
	}

	switch (io_u->ddir) {
	case DDIR_READ:
		rc = spdk_bdev_read(target->desc, target->ch,
				    io_u->buf, io_u->offset, io_u->xfer_buflen,
				    spdk_fio_completion_cb, fio_req);
		break;
	case DDIR_WRITE:
		rc = spdk_bdev_write(target->desc, target->ch,
				     io_u->buf, io_u->offset, io_u->xfer_buflen,
				     spdk_fio_completion_cb, fio_req);
		break;
	case DDIR_TRIM:
		rc = spdk_bdev_unmap(target->desc, target->ch,
				     io_u->offset, io_u->xfer_buflen,
				     spdk_fio_completion_cb, fio_req);
		break;
	default:
		assert(false);
		break;
	}

	if (rc == -ENOMEM) {
		return FIO_Q_BUSY;
	}

	if (rc != 0) {
		fio_req->io->error = abs(rc);
		return FIO_Q_COMPLETED;
	}

	return FIO_Q_QUEUED;
}

static struct io_u *
spdk_fio_event(struct thread_data *td, int event)
{
	struct spdk_fio_thread *fio_thread = td->io_ops_data;

	assert(event >= 0);
	assert((unsigned)event < fio_thread->iocq_count);
	return fio_thread->iocq[event];
}

static size_t
spdk_fio_poll_thread(struct spdk_fio_thread *fio_thread)
{
	struct spdk_fio_msg *msg;
	struct spdk_fio_poller *p, *tmp;
	size_t count;

	/* Process new events */
	count = spdk_ring_dequeue(fio_thread->ring, (void **)&msg, 1);
	if (count > 0) {
		msg->cb_fn(msg->cb_arg);
		free(msg);
	}

	/* Call all pollers */
	TAILQ_FOREACH_SAFE(p, &fio_thread->pollers, link, tmp) {
		p->cb_fn(p->cb_arg);
	}

	return count;
}

static int
spdk_fio_getevents(struct thread_data *td, unsigned int min,
		   unsigned int max, const struct timespec *t)
{
	struct spdk_fio_thread *fio_thread = td->io_ops_data;
	struct timespec t0, t1;
	uint64_t timeout = 0;

	if (t) {
		timeout = t->tv_sec * 1000000000L + t->tv_nsec;
		clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
	}

	fio_thread->iocq_count = 0;

	for (;;) {
		spdk_fio_poll_thread(fio_thread);

		if (fio_thread->iocq_count >= min) {
			return fio_thread->iocq_count;
		}

		if (t) {
			clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
			uint64_t elapse = ((t1.tv_sec - t0.tv_sec) * 1000000000L)
					  + t1.tv_nsec - t0.tv_nsec;
			if (elapse > timeout) {
				break;
			}
		}
	}

	return fio_thread->iocq_count;
}

static int
spdk_fio_invalidate(struct thread_data *td, struct fio_file *f)
{
	/* TODO: This should probably send a flush to the device, but for now just return successful. */
	return 0;
}

static struct fio_option options[] = {
	{
		.name		= "spdk_conf",
		.lname		= "SPDK configuration file",
		.type		= FIO_OPT_STR_STORE,
		.off1		= offsetof(struct spdk_fio_options, conf),
		.help		= "A SPDK configuration file",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "spdk_mem",
		.lname		= "SPDK memory in MB",
		.type		= FIO_OPT_INT,
		.off1		= offsetof(struct spdk_fio_options, mem_mb),
		.help		= "Amount of memory in MB to allocate for SPDK",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "spdk_single_seg",
		.lname		= "SPDK switch to create just a single hugetlbfs file",
		.type		= FIO_OPT_BOOL,
		.off1		= offsetof(struct spdk_fio_options, mem_single_seg),
		.help		= "If set to 1, SPDK will use just a single hugetlbfs file",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= NULL,
	},
};

/* FIO imports this structure using dlsym */
struct ioengine_ops ioengine = {
	.name			= "spdk_bdev",
	.version		= FIO_IOOPS_VERSION,
	.flags			= FIO_RAWIO | FIO_NOEXTEND | FIO_NODISKUTIL | FIO_MEMALIGN,
	.setup			= spdk_fio_setup,
	.init			= spdk_fio_init,
	//.prep			= unused,
	.queue			= spdk_fio_queue,
	//.commit		= unused,
	.getevents		= spdk_fio_getevents,
	.event			= spdk_fio_event,
	//.errdetails		= unused,
	//.cancel		= unused,
	.cleanup		= spdk_fio_cleanup,
	.open_file		= spdk_fio_open,
	.close_file		= spdk_fio_close,
	.invalidate		= spdk_fio_invalidate,
	//.unlink_file		= unused,
	//.get_file_size	= unused,
	//.terminate		= unused,
	.iomem_alloc		= spdk_fio_iomem_alloc,
	.iomem_free		= spdk_fio_iomem_free,
	.io_u_init		= spdk_fio_io_u_init,
	.io_u_free		= spdk_fio_io_u_free,
	.option_struct_size	= sizeof(struct spdk_fio_options),
	.options		= options,
};

static void fio_init spdk_fio_register(void)
{
	register_ioengine(&ioengine);
}

static void
spdk_fio_module_finish_done(void *cb_arg)
{
	*(bool *)cb_arg = true;
}

static void
spdk_fio_finish_env(void)
{
	struct spdk_fio_thread		*fio_thread;
	bool				done = false;
	size_t				count;

	/* the same thread that called spdk_fio_init_env */
	fio_thread = g_init_thread;

	if (pthread_cancel(g_init_thread_id) == 0) {
		pthread_join(g_init_thread_id, NULL);
	}

	spdk_bdev_finish(spdk_fio_module_finish_done, &done);

	do {
		spdk_fio_poll_thread(fio_thread);
	} while (!done);

	do {
		count = spdk_fio_poll_thread(fio_thread);
	} while (count > 0);

	done = false;
	spdk_copy_engine_finish(spdk_fio_module_finish_done, &done);

	do {
		spdk_fio_poll_thread(fio_thread);
	} while (!done);

	do {
		count = spdk_fio_poll_thread(fio_thread);
	} while (count > 0);

	spdk_fio_cleanup_thread(fio_thread);
}

static void fio_exit spdk_fio_unregister(void)
{
	if (g_spdk_env_initialized) {
		spdk_fio_finish_env();
		g_spdk_env_initialized = false;
	}
	unregister_ioengine(&ioengine);
}
