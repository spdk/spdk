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
#include "spdk/accel_engine.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/queue.h"
#include "spdk/util.h"

#include "spdk_internal/thread.h"
#include "spdk_internal/event.h"

#include "config-host.h"
#include "fio.h"
#include "optgroup.h"

/* FreeBSD is missing CLOCK_MONOTONIC_RAW,
 * so alternative is provided. */
#ifndef CLOCK_MONOTONIC_RAW /* Defined in glibc bits/time.h */
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

struct spdk_fio_options {
	void *pad;
	char *conf;
	char *json_conf;
	unsigned mem_mb;
	bool mem_single_seg;
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

	TAILQ_HEAD(, spdk_fio_target)	targets;
	bool				failed; /* true if the thread failed to initialize */

	struct io_u		**iocq;		/* io completion queue */
	unsigned int		iocq_count;	/* number of iocq entries filled by last getevents */
	unsigned int		iocq_size;	/* number of iocq entries allocated */

	TAILQ_ENTRY(spdk_fio_thread)	link;
};

static bool g_spdk_env_initialized = false;
static const char *g_json_config_file = NULL;

static int spdk_fio_init(struct thread_data *td);
static void spdk_fio_cleanup(struct thread_data *td);
static size_t spdk_fio_poll_thread(struct spdk_fio_thread *fio_thread);

static pthread_t g_init_thread_id = 0;
static pthread_mutex_t g_init_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_init_cond;
static bool g_poll_loop = true;
static TAILQ_HEAD(, spdk_fio_thread) g_threads = TAILQ_HEAD_INITIALIZER(g_threads);

/* Default polling timeout (ns) */
#define SPDK_FIO_POLLING_TIMEOUT 1000000000ULL

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

	fio_thread->thread = spdk_thread_create("fio_thread", NULL);
	if (!fio_thread->thread) {
		free(fio_thread);
		SPDK_ERRLOG("failed to allocate thread\n");
		return -1;
	}
	spdk_set_thread(fio_thread->thread);

	fio_thread->iocq_size = td->o.iodepth;
	fio_thread->iocq = calloc(fio_thread->iocq_size, sizeof(struct io_u *));
	assert(fio_thread->iocq != NULL);

	TAILQ_INIT(&fio_thread->targets);

	return 0;
}

static void
spdk_fio_bdev_close_targets(void *arg)
{
	struct spdk_fio_thread *fio_thread = arg;
	struct spdk_fio_target *target, *tmp;

	TAILQ_FOREACH_SAFE(target, &fio_thread->targets, link, tmp) {
		TAILQ_REMOVE(&fio_thread->targets, target, link);
		spdk_put_io_channel(target->ch);
		spdk_bdev_close(target->desc);
		free(target);
	}
}

static void
spdk_fio_cleanup_thread(struct spdk_fio_thread *fio_thread)
{
	spdk_thread_send_msg(fio_thread->thread, spdk_fio_bdev_close_targets, fio_thread);

	pthread_mutex_lock(&g_init_mtx);
	TAILQ_INSERT_TAIL(&g_threads, fio_thread, link);
	pthread_mutex_unlock(&g_init_mtx);
}

static void
spdk_fio_calc_timeout(struct spdk_fio_thread *fio_thread, struct timespec *ts)
{
	uint64_t timeout, now;

	if (spdk_thread_has_active_pollers(fio_thread->thread)) {
		return;
	}

	timeout = spdk_thread_next_poller_expiration(fio_thread->thread);
	now = spdk_get_ticks();

	if (timeout == 0) {
		timeout = now + (SPDK_FIO_POLLING_TIMEOUT * spdk_get_ticks_hz()) / SPDK_SEC_TO_NSEC;
	}

	if (timeout > now) {
		timeout = ((timeout - now) * SPDK_SEC_TO_NSEC) / spdk_get_ticks_hz() +
			  ts->tv_sec * SPDK_SEC_TO_NSEC + ts->tv_nsec;

		ts->tv_sec  = timeout / SPDK_SEC_TO_NSEC;
		ts->tv_nsec = timeout % SPDK_SEC_TO_NSEC;
	}
}

static void
spdk_fio_bdev_init_done(int rc, void *cb_arg)
{
	*(bool *)cb_arg = true;
}

static void
spdk_fio_bdev_init_start(void *arg)
{
	bool *done = arg;

	spdk_app_json_config_load(g_json_config_file, SPDK_DEFAULT_RPC_ADDR,
				  spdk_fio_bdev_init_done, done, true);
}

static void
spdk_fio_bdev_fini_done(void *cb_arg)
{
	*(bool *)cb_arg = true;
}

static void
spdk_fio_bdev_fini_start(void *arg)
{
	bool *done = arg;

	spdk_subsystem_fini(spdk_fio_bdev_fini_done, done);
}

static void *
spdk_init_thread_poll(void *arg)
{
	struct spdk_fio_options		*eo = arg;
	struct spdk_fio_thread		*fio_thread;
	struct spdk_fio_thread		*thread, *tmp;
	struct spdk_env_opts		opts;
	bool				done;
	int				rc;
	struct timespec			ts;
	struct thread_data		td = {};

	/* Create a dummy thread data for use on the initialization thread. */
	td.o.iodepth = 32;
	td.eo = eo;

	/* Parse the SPDK configuration file */
	eo = arg;

	if (eo->conf && eo->json_conf) {
		SPDK_ERRLOG("Cannot provide two types of configuration files\n");
		rc = EINVAL;
		goto err_exit;
	} else if (eo->conf && strlen(eo->conf)) {
		g_json_config_file = eo->conf;
	} else if (eo->json_conf && strlen(eo->json_conf)) {
		g_json_config_file = eo->json_conf;
	} else {
		SPDK_ERRLOG("No configuration file provided\n");
		rc = EINVAL;
		goto err_exit;
	}

	/* Initialize the environment library */
	spdk_env_opts_init(&opts);
	opts.name = "fio";

	if (eo->mem_mb) {
		opts.mem_size = eo->mem_mb;
	}
	opts.hugepage_single_segments = eo->mem_single_seg;

	if (spdk_env_init(&opts) < 0) {
		SPDK_ERRLOG("Unable to initialize SPDK env\n");
		rc = EINVAL;
		goto err_exit;
	}
	spdk_unaffinitize_thread();

	spdk_thread_lib_init(NULL, 0);

	/* Create an SPDK thread temporarily */
	rc = spdk_fio_init_thread(&td);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to create initialization thread\n");
		goto err_exit;
	}

	fio_thread = td.io_ops_data;

	/* Initialize the bdev layer */
	done = false;
	spdk_thread_send_msg(fio_thread->thread, spdk_fio_bdev_init_start, &done);

	do {
		spdk_fio_poll_thread(fio_thread);
	} while (!done);

	/*
	 * Continue polling until there are no more events.
	 * This handles any final events posted by pollers.
	 */
	while (spdk_fio_poll_thread(fio_thread) > 0) {};

	/* Set condition variable */
	pthread_mutex_lock(&g_init_mtx);
	pthread_cond_signal(&g_init_cond);

	pthread_mutex_unlock(&g_init_mtx);

	while (g_poll_loop) {
		spdk_fio_poll_thread(fio_thread);

		pthread_mutex_lock(&g_init_mtx);
		if (!TAILQ_EMPTY(&g_threads)) {
			TAILQ_FOREACH_SAFE(thread, &g_threads, link, tmp) {
				spdk_fio_poll_thread(thread);
			}

			/* If there are exiting threads to poll, don't sleep. */
			pthread_mutex_unlock(&g_init_mtx);
			continue;
		}

		/* Figure out how long to sleep. */
		clock_gettime(CLOCK_MONOTONIC, &ts);
		spdk_fio_calc_timeout(fio_thread, &ts);

		rc = pthread_cond_timedwait(&g_init_cond, &g_init_mtx, &ts);
		pthread_mutex_unlock(&g_init_mtx);

		if (rc != ETIMEDOUT) {
			break;
		}


	}

	spdk_fio_cleanup_thread(fio_thread);

	/* Finalize the bdev layer */
	done = false;
	spdk_thread_send_msg(fio_thread->thread, spdk_fio_bdev_fini_start, &done);

	do {
		spdk_fio_poll_thread(fio_thread);

		TAILQ_FOREACH_SAFE(thread, &g_threads, link, tmp) {
			spdk_fio_poll_thread(thread);
		}
	} while (!done);

	/* Now exit all the threads */
	TAILQ_FOREACH(thread, &g_threads, link) {
		spdk_set_thread(thread->thread);
		spdk_thread_exit(thread->thread);
		spdk_set_thread(NULL);
	}

	/* And wait for them to gracefully exit */
	while (!TAILQ_EMPTY(&g_threads)) {
		TAILQ_FOREACH_SAFE(thread, &g_threads, link, tmp) {
			if (spdk_thread_is_exited(thread->thread)) {
				TAILQ_REMOVE(&g_threads, thread, link);
				spdk_thread_destroy(thread->thread);
				free(thread->iocq);
				free(thread);
			} else {
				spdk_thread_poll(thread->thread, 0, 0);
			}
		}
	}

	pthread_exit(NULL);

err_exit:
	exit(rc);
	return NULL;
}

static int
spdk_fio_init_env(struct thread_data *td)
{
	pthread_condattr_t attr;
	int rc = -1;

	if (pthread_condattr_init(&attr)) {
		SPDK_ERRLOG("Unable to initialize condition variable\n");
		return -1;
	}

	if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)) {
		SPDK_ERRLOG("Unable to initialize condition variable\n");
		goto out;
	}

	if (pthread_cond_init(&g_init_cond, &attr)) {
		SPDK_ERRLOG("Unable to initialize condition variable\n");
		goto out;
	}

	/*
	 * Spawn a thread to handle initialization operations and to poll things
	 * like the admin queues periodically.
	 */
	rc = pthread_create(&g_init_thread_id, NULL, &spdk_init_thread_poll, td->eo);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to spawn thread to poll admin queue. It won't be polled.\n");
	}

	/* Wait for background thread to advance past the initialization */
	pthread_mutex_lock(&g_init_mtx);
	pthread_cond_wait(&g_init_cond, &g_init_mtx);
	pthread_mutex_unlock(&g_init_mtx);
out:
	pthread_condattr_destroy(&attr);
	return rc;
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

	/* we might be running in a daemonized FIO instance where standard
	 * input and output were closed and fds 0, 1, and 2 are reused
	 * for something important by FIO. We can't ensure we won't print
	 * anything (and so will our dependencies, e.g. DPDK), so abort early.
	 * (is_backend is an fio global variable)
	 */
	if (is_backend) {
		char buf[1024];
		snprintf(buf, sizeof(buf),
			 "SPDK FIO plugin won't work with daemonized FIO server.");
		fio_server_text_output(FIO_LOG_ERR, buf, sizeof(buf));
		return -1;
	}

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

	if (td->o.nr_files == 1 && strcmp(td->files[0]->file_name, "*") == 0) {
		struct spdk_bdev *bdev;

		/* add all available bdevs as fio targets */
		for (bdev = spdk_bdev_first_leaf(); bdev; bdev = spdk_bdev_next_leaf(bdev)) {
			add_file(td, spdk_bdev_get_name(bdev), 0, 1);
		}
	}

	for_each_file(td, f, i) {
		struct spdk_bdev *bdev;

		if (strcmp(f->file_name, "*") == 0) {
			continue;
		}

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

static void
fio_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		  void *event_ctx)
{
	SPDK_WARNLOG("Unsupported bdev event: type %d\n", type);
}

static void
spdk_fio_bdev_open(void *arg)
{
	struct thread_data *td = arg;
	struct spdk_fio_thread *fio_thread;
	unsigned int i;
	struct fio_file *f;
	int rc;

	fio_thread = td->io_ops_data;

	for_each_file(td, f, i) {
		struct spdk_fio_target *target;

		if (strcmp(f->file_name, "*") == 0) {
			continue;
		}

		target = calloc(1, sizeof(*target));
		if (!target) {
			SPDK_ERRLOG("Unable to allocate memory for I/O target.\n");
			fio_thread->failed = true;
			return;
		}

		rc = spdk_bdev_open_ext(f->file_name, true, fio_bdev_event_cb, NULL,
					&target->desc);
		if (rc) {
			SPDK_ERRLOG("Unable to open bdev %s\n", f->file_name);
			free(target);
			fio_thread->failed = true;
			return;
		}

		target->bdev = spdk_bdev_desc_get_bdev(target->desc);

		target->ch = spdk_bdev_get_io_channel(target->desc);
		if (!target->ch) {
			SPDK_ERRLOG("Unable to get I/O channel for bdev.\n");
			spdk_bdev_close(target->desc);
			free(target);
			fio_thread->failed = true;
			return;
		}

		f->engine_data = target;

		TAILQ_INSERT_TAIL(&fio_thread->targets, target, link);
	}
}

/* Called for each thread, on that thread, shortly after the thread
 * starts.
 */
static int
spdk_fio_init(struct thread_data *td)
{
	struct spdk_fio_thread *fio_thread;

	spdk_fio_init_thread(td);

	fio_thread = td->io_ops_data;
	fio_thread->failed = false;

	spdk_thread_send_msg(fio_thread->thread, spdk_fio_bdev_open, td);

	while (spdk_fio_poll_thread(fio_thread) > 0) {}

	if (fio_thread->failed) {
		return -1;
	}

	return 0;
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

	io_u->engine_data = NULL;

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
	return spdk_thread_poll(fio_thread->thread, 0, 0);
}

static int
spdk_fio_getevents(struct thread_data *td, unsigned int min,
		   unsigned int max, const struct timespec *t)
{
	struct spdk_fio_thread *fio_thread = td->io_ops_data;
	struct timespec t0, t1;
	uint64_t timeout = 0;

	if (t) {
		timeout = t->tv_sec * SPDK_SEC_TO_NSEC + t->tv_nsec;
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
			uint64_t elapse = ((t1.tv_sec - t0.tv_sec) * SPDK_SEC_TO_NSEC)
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
		.help		= "A SPDK JSON configuration file",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name           = "spdk_json_conf",
		.lname          = "SPDK JSON configuration file",
		.type           = FIO_OPT_STR_STORE,
		.off1           = offsetof(struct spdk_fio_options, json_conf),
		.help           = "A SPDK JSON configuration file",
		.category       = FIO_OPT_C_ENGINE,
		.group          = FIO_OPT_G_INVALID,
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
	/* .prep		= unused, */
	.queue			= spdk_fio_queue,
	/* .commit		= unused, */
	.getevents		= spdk_fio_getevents,
	.event			= spdk_fio_event,
	/* .errdetails		= unused, */
	/* .cancel		= unused, */
	.cleanup		= spdk_fio_cleanup,
	.open_file		= spdk_fio_open,
	.close_file		= spdk_fio_close,
	.invalidate		= spdk_fio_invalidate,
	/* .unlink_file		= unused, */
	/* .get_file_size	= unused, */
	/* .terminate		= unused, */
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
spdk_fio_finish_env(void)
{
	pthread_mutex_lock(&g_init_mtx);
	g_poll_loop = false;
	pthread_cond_signal(&g_init_cond);
	pthread_mutex_unlock(&g_init_mtx);
	pthread_join(g_init_thread_id, NULL);

	spdk_thread_lib_fini();
}

static void fio_exit spdk_fio_unregister(void)
{
	if (g_spdk_env_initialized) {
		spdk_fio_finish_env();
		g_spdk_env_initialized = false;
	}
	unregister_ioengine(&ioengine);
}
