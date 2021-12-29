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
#include "spdk/bdev_zone.h"
#include "spdk/accel_engine.h"
#include "spdk/env.h"
#include "spdk/init.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/queue.h"
#include "spdk/util.h"

#include "spdk_internal/event.h"

#include "config-host.h"
#include "fio.h"
#include "optgroup.h"

#ifdef for_each_rw_ddir
#define FIO_HAS_ZBD (FIO_IOOPS_VERSION >= 26)
#else
#define FIO_HAS_ZBD (0)
#endif

/* FreeBSD is missing CLOCK_MONOTONIC_RAW,
 * so alternative is provided. */
#ifndef CLOCK_MONOTONIC_RAW /* Defined in glibc bits/time.h */
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

struct spdk_fio_options {
	void *pad;
	char *conf;
	char *json_conf;
	char *log_flags;
	unsigned mem_mb;
	int mem_single_seg;
	int initial_zone_reset;
	int zone_append;
};

struct spdk_fio_request {
	struct io_u		*io;
	struct thread_data	*td;
};

struct spdk_fio_target {
	struct spdk_bdev	*bdev;
	struct spdk_bdev_desc	*desc;
	struct spdk_io_channel	*ch;
	bool zone_append_enabled;

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

struct spdk_fio_zone_cb_arg {
	struct spdk_fio_target *target;
	struct spdk_bdev_zone_info *spdk_zones;
	int completed;
	uint64_t offset_blocks;
	struct zbd_zone *fio_zones;
	unsigned int nr_zones;
};

static bool g_spdk_env_initialized = false;
static const char *g_json_config_file = NULL;

static int spdk_fio_init(struct thread_data *td);
static void spdk_fio_cleanup(struct thread_data *td);
static size_t spdk_fio_poll_thread(struct spdk_fio_thread *fio_thread);
static int spdk_fio_handle_options(struct thread_data *td, struct fio_file *f,
				   struct spdk_bdev *bdev);
static int spdk_fio_handle_options_per_target(struct thread_data *td, struct fio_file *f);

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

	spdk_subsystem_init_from_json_config(g_json_config_file, SPDK_DEFAULT_RPC_ADDR,
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

	if (eo->log_flags) {
		char *tok = strtok(eo->log_flags, ",");
		do {
			rc = spdk_log_set_flag(tok);
			if (rc < 0) {
				SPDK_ERRLOG("unknown spdk log flag %s\n", tok);
				rc = EINVAL;
				goto err_exit;
			}
		} while ((tok = strtok(NULL, ",")) != NULL);
#ifdef DEBUG
		spdk_log_set_print_level(SPDK_LOG_DEBUG);
#endif
	}

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

static bool
fio_redirected_to_dev_null(void)
{
	char path[PATH_MAX] = "";
	ssize_t ret;

	ret = readlink("/proc/self/fd/1", path, sizeof(path));

	if (ret == -1 || strcmp(path, "/dev/null") != 0) {
		return false;
	}

	ret = readlink("/proc/self/fd/2", path, sizeof(path));

	if (ret == -1 || strcmp(path, "/dev/null") != 0) {
		return false;
	}

	return true;
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

	/*
	 * If we're running in a daemonized FIO instance, it's possible
	 * fd 1/2 were re-used for something important by FIO. Newer fio
	 * versions are careful to redirect those to /dev/null, but if we're
	 * not, we'll abort early, so we don't accidentally write messages to
	 * an important file, etc.
	 */
	if (is_backend && !fio_redirected_to_dev_null()) {
		char buf[1024];
		snprintf(buf, sizeof(buf),
			 "SPDK FIO plugin is in daemon mode, but stdout/stderr "
			 "aren't redirected to /dev/null. Aborting.");
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
		int rc;

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
		f->filetype = FIO_TYPE_BLOCK;
		fio_file_set_size_known(f);

		rc = spdk_fio_handle_options(td, f, bdev);
		if (rc) {
			return rc;
		}
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

		rc = spdk_fio_handle_options_per_target(td, f);
		if (rc) {
			SPDK_ERRLOG("Failed to handle options for: %s\n", f->file_name);
			f->engine_data = NULL;
			spdk_put_io_channel(target->ch);
			spdk_bdev_close(target->desc);
			free(target);
			fio_thread->failed = true;
			return;
		}

		TAILQ_INSERT_TAIL(&fio_thread->targets, target, link);
	}
}

/* Called for each thread, on that thread, shortly after the thread
 * starts.
 *
 * Also called by spdk_fio_report_zones(), since we need an I/O channel
 * in order to get the zone report. (fio calls the .report_zones callback
 * before it calls the .init callback.)
 * Therefore, if fio was run with --zonemode=zbd, the thread will already
 * be initialized by the time that fio calls the .init callback.
 */
static int
spdk_fio_init(struct thread_data *td)
{
	struct spdk_fio_thread *fio_thread;
	int rc;

	/* If thread has already been initialized, do nothing. */
	if (td->io_ops_data) {
		return 0;
	}

	rc = spdk_fio_init_thread(td);
	if (rc) {
		return rc;
	}

	fio_thread = td->io_ops_data;
	assert(fio_thread);
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

static uint64_t
spdk_fio_zone_bytes_to_blocks(struct spdk_bdev *bdev, uint64_t offset_bytes, uint64_t *zone_start,
			      uint64_t num_bytes, uint64_t *num_blocks)
{
	uint32_t block_size = spdk_bdev_get_block_size(bdev);
	*zone_start = spdk_bdev_get_zone_id(bdev, offset_bytes / block_size);
	*num_blocks = num_bytes / block_size;
	return (offset_bytes % block_size) | (num_bytes % block_size);
}

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
		if (!target->zone_append_enabled) {
			rc = spdk_bdev_write(target->desc, target->ch,
					     io_u->buf, io_u->offset, io_u->xfer_buflen,
					     spdk_fio_completion_cb, fio_req);
		} else {
			uint64_t zone_start, num_blocks;
			if (spdk_fio_zone_bytes_to_blocks(target->bdev, io_u->offset, &zone_start,
							  io_u->xfer_buflen, &num_blocks) != 0) {
				rc = -EINVAL;
				break;
			}
			rc = spdk_bdev_zone_append(target->desc, target->ch, io_u->buf,
						   zone_start, num_blocks, spdk_fio_completion_cb,
						   fio_req);
		}
		break;
	case DDIR_TRIM:
		rc = spdk_bdev_unmap(target->desc, target->ch,
				     io_u->offset, io_u->xfer_buflen,
				     spdk_fio_completion_cb, fio_req);
		break;
	case DDIR_SYNC:
		rc = spdk_bdev_flush(target->desc, target->ch,
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

#if FIO_HAS_ZBD
static int
spdk_fio_get_zoned_model(struct thread_data *td, struct fio_file *f, enum zbd_zoned_model *model)
{
	struct spdk_bdev *bdev;

	if (f->filetype != FIO_TYPE_BLOCK) {
		SPDK_ERRLOG("Unsupported filetype: %d\n", f->filetype);
		return -EINVAL;
	}

	bdev = spdk_bdev_get_by_name(f->file_name);
	if (!bdev) {
		SPDK_ERRLOG("Cannot get zoned model, no bdev with name: %s\n", f->file_name);
		return -ENODEV;
	}

	if (spdk_bdev_is_zoned(bdev)) {
		*model = ZBD_HOST_MANAGED;
	} else {
		*model = ZBD_NONE;
	}

	return 0;
}

static void
spdk_fio_bdev_get_zone_info_done(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct spdk_fio_zone_cb_arg *cb_arg = arg;
	unsigned int i;
	int handled_zones = 0;

	if (!success) {
		spdk_bdev_free_io(bdev_io);
		cb_arg->completed = -EIO;
		return;
	}

	for (i = 0; i < cb_arg->nr_zones; i++) {
		struct spdk_bdev_zone_info *zone_src = &cb_arg->spdk_zones[handled_zones];
		struct zbd_zone *zone_dest = &cb_arg->fio_zones[handled_zones];
		uint32_t block_size = spdk_bdev_get_block_size(cb_arg->target->bdev);

		zone_dest->type = ZBD_ZONE_TYPE_SWR;
		zone_dest->len = spdk_bdev_get_zone_size(cb_arg->target->bdev) * block_size;
		zone_dest->capacity = zone_src->capacity * block_size;
		zone_dest->start = zone_src->zone_id * block_size;
		zone_dest->wp = zone_src->write_pointer * block_size;

		switch (zone_src->state) {
		case SPDK_BDEV_ZONE_STATE_EMPTY:
			zone_dest->cond = ZBD_ZONE_COND_EMPTY;
			break;
		case SPDK_BDEV_ZONE_STATE_IMP_OPEN:
			zone_dest->cond = ZBD_ZONE_COND_IMP_OPEN;
			break;
		case SPDK_BDEV_ZONE_STATE_EXP_OPEN:
			zone_dest->cond = ZBD_ZONE_COND_EXP_OPEN;
			break;
		case SPDK_BDEV_ZONE_STATE_FULL:
			zone_dest->cond = ZBD_ZONE_COND_FULL;
			break;
		case SPDK_BDEV_ZONE_STATE_CLOSED:
			zone_dest->cond = ZBD_ZONE_COND_CLOSED;
			break;
		case SPDK_BDEV_ZONE_STATE_READ_ONLY:
			zone_dest->cond = ZBD_ZONE_COND_READONLY;
			break;
		case SPDK_BDEV_ZONE_STATE_OFFLINE:
			zone_dest->cond = ZBD_ZONE_COND_OFFLINE;
			break;
		default:
			spdk_bdev_free_io(bdev_io);
			cb_arg->completed = -EIO;
			return;
		}
		handled_zones++;
	}

	spdk_bdev_free_io(bdev_io);
	cb_arg->completed = handled_zones;
}

static void
spdk_fio_bdev_get_zone_info(void *arg)
{
	struct spdk_fio_zone_cb_arg *cb_arg = arg;
	struct spdk_fio_target *target = cb_arg->target;
	int rc;

	rc = spdk_bdev_get_zone_info(target->desc, target->ch, cb_arg->offset_blocks,
				     cb_arg->nr_zones, cb_arg->spdk_zones,
				     spdk_fio_bdev_get_zone_info_done, cb_arg);
	if (rc < 0) {
		cb_arg->completed = rc;
	}
}

static int
spdk_fio_report_zones(struct thread_data *td, struct fio_file *f, uint64_t offset,
		      struct zbd_zone *zones, unsigned int nr_zones)
{
	struct spdk_fio_target *target;
	struct spdk_fio_thread *fio_thread;
	struct spdk_fio_zone_cb_arg cb_arg;
	uint32_t block_size;
	int rc;

	if (nr_zones == 0) {
		return 0;
	}

	/* spdk_fio_report_zones() is only called before the bdev I/O channels have been created.
	 * Since we need an I/O channel for report_zones(), call spdk_fio_init() to initialize
	 * the thread early.
	 * spdk_fio_report_zones() might be called several times by fio, if e.g. the zone report
	 * for all zones does not fit in the buffer that fio has allocated for the zone report.
	 * It is safe to call spdk_fio_init(), even if the thread has already been initialized.
	 */
	rc = spdk_fio_init(td);
	if (rc) {
		return rc;
	}
	fio_thread = td->io_ops_data;
	target = f->engine_data;

	assert(fio_thread);
	assert(target);

	block_size = spdk_bdev_get_block_size(target->bdev);

	cb_arg.target = target;
	cb_arg.completed = 0;
	cb_arg.offset_blocks = offset / block_size;
	cb_arg.fio_zones = zones;
	cb_arg.nr_zones = spdk_min(nr_zones, spdk_bdev_get_num_zones(target->bdev));

	cb_arg.spdk_zones = calloc(1, sizeof(*cb_arg.spdk_zones) * cb_arg.nr_zones);
	if (!cb_arg.spdk_zones) {
		SPDK_ERRLOG("Could not allocate memory for zone report!\n");
		rc = -ENOMEM;
		goto cleanup_thread;
	}

	spdk_thread_send_msg(fio_thread->thread, spdk_fio_bdev_get_zone_info, &cb_arg);
	do {
		spdk_fio_poll_thread(fio_thread);
	} while (!cb_arg.completed);

	/* Free cb_arg.spdk_zones. The report in fio format is stored in cb_arg.fio_zones/zones. */
	free(cb_arg.spdk_zones);

	rc = cb_arg.completed;
	if (rc < 0) {
		SPDK_ERRLOG("Failed to get zone info: %d\n", rc);
		goto cleanup_thread;
	}

	/* Return the amount of zones successfully copied. */
	return rc;

cleanup_thread:
	spdk_fio_cleanup(td);

	return rc;
}

static void
spdk_fio_bdev_zone_reset_done(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct spdk_fio_zone_cb_arg *cb_arg = arg;

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		cb_arg->completed = -EIO;
	} else {
		cb_arg->completed = 1;
	}
}

static void
spdk_fio_bdev_zone_reset(void *arg)
{
	struct spdk_fio_zone_cb_arg *cb_arg = arg;
	struct spdk_fio_target *target = cb_arg->target;
	int rc;

	rc = spdk_bdev_zone_management(target->desc, target->ch, cb_arg->offset_blocks,
				       SPDK_BDEV_ZONE_RESET,
				       spdk_fio_bdev_zone_reset_done, cb_arg);
	if (rc < 0) {
		cb_arg->completed = rc;
	}
}

static int
spdk_fio_reset_zones(struct spdk_fio_thread *fio_thread, struct spdk_fio_target *target,
		     uint64_t offset, uint64_t length)
{
	uint64_t zone_size_bytes;
	uint32_t block_size;
	int rc;

	assert(fio_thread);
	assert(target);

	block_size = spdk_bdev_get_block_size(target->bdev);
	zone_size_bytes = spdk_bdev_get_zone_size(target->bdev) * block_size;

	for (uint64_t cur = offset; cur < offset + length; cur += zone_size_bytes) {
		struct spdk_fio_zone_cb_arg cb_arg = {
			.target = target,
			.completed = 0,
			.offset_blocks = cur / block_size,
		};

		spdk_thread_send_msg(fio_thread->thread, spdk_fio_bdev_zone_reset, &cb_arg);
		do {
			spdk_fio_poll_thread(fio_thread);
		} while (!cb_arg.completed);

		rc = cb_arg.completed;
		if (rc < 0) {
			SPDK_ERRLOG("Failed to reset zone: %d\n", rc);
			return rc;
		}
	}

	return 0;
}

static int
spdk_fio_reset_wp(struct thread_data *td, struct fio_file *f, uint64_t offset, uint64_t length)
{
	return spdk_fio_reset_zones(td->io_ops_data, f->engine_data, offset, length);
}
#endif

#if FIO_IOOPS_VERSION >= 30
static int spdk_fio_get_max_open_zones(struct thread_data *td, struct fio_file *f,
				       unsigned int *max_open_zones)
{
	struct spdk_bdev *bdev;

	bdev = spdk_bdev_get_by_name(f->file_name);
	if (!bdev) {
		SPDK_ERRLOG("Cannot get max open zones, no bdev with name: %s\n", f->file_name);
		return -ENODEV;
	}

	*max_open_zones = spdk_bdev_get_max_open_zones(bdev);

	return 0;
}
#endif

static int
spdk_fio_handle_options(struct thread_data *td, struct fio_file *f, struct spdk_bdev *bdev)
{
	struct spdk_fio_options *fio_options = td->eo;

	if (fio_options->initial_zone_reset && spdk_bdev_is_zoned(bdev)) {
#if FIO_HAS_ZBD
		int rc = spdk_fio_init(td);
		if (rc) {
			return rc;
		}
		rc = spdk_fio_reset_zones(td->io_ops_data, f->engine_data, 0, f->real_file_size);
		if (rc) {
			spdk_fio_cleanup(td);
			return rc;
		}
#else
		SPDK_ERRLOG("fio version is too old to support zoned block devices\n");
#endif
	}

	return 0;
}

static int
spdk_fio_handle_options_per_target(struct thread_data *td, struct fio_file *f)
{
	struct spdk_fio_target *target = f->engine_data;
	struct spdk_fio_options *fio_options = td->eo;

	if (fio_options->zone_append && spdk_bdev_is_zoned(target->bdev)) {
		if (spdk_bdev_io_type_supported(target->bdev, SPDK_BDEV_IO_TYPE_ZONE_APPEND)) {
			SPDK_DEBUGLOG(fio_bdev, "Using zone appends instead of writes on: '%s'\n",
				      f->file_name);
			target->zone_append_enabled = true;
		} else {
			SPDK_WARNLOG("Falling back to writes on: '%s' - bdev lacks zone append cmd\n",
				     f->file_name);
		}
	}

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
		.def            = "0",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name           = "log_flags",
		.lname          = "log flags",
		.type           = FIO_OPT_STR_STORE,
		.off1           = offsetof(struct spdk_fio_options, log_flags),
		.help           = "SPDK log flags to enable",
		.category       = FIO_OPT_C_ENGINE,
		.group          = FIO_OPT_G_INVALID,
	},
	{
		.name		= "initial_zone_reset",
		.lname		= "Reset Zones on initialization",
		.type		= FIO_OPT_INT,
		.off1		= offsetof(struct spdk_fio_options, initial_zone_reset),
		.def		= "0",
		.help		= "Reset Zones on initialization (0=disable, 1=Reset All Zones)",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "zone_append",
		.lname		= "Use zone append instead of write",
		.type		= FIO_OPT_INT,
		.off1		= offsetof(struct spdk_fio_options, zone_append),
		.def		= "0",
		.help		= "Use zone append instead of write (1=zone append, 0=write)",
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
#if FIO_HAS_ZBD
	.get_zoned_model	= spdk_fio_get_zoned_model,
	.report_zones		= spdk_fio_report_zones,
	.reset_wp		= spdk_fio_reset_wp,
#endif
#if FIO_IOOPS_VERSION >= 30
	.get_max_open_zones	= spdk_fio_get_max_open_zones,
#endif
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
	spdk_env_fini();
}

static void fio_exit spdk_fio_unregister(void)
{
	if (g_spdk_env_initialized) {
		spdk_fio_finish_env();
		g_spdk_env_initialized = false;
	}
	unregister_ioengine(&ioengine);
}

SPDK_LOG_REGISTER_COMPONENT(fio_bdev)
