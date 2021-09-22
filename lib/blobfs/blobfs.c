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

#include "spdk/blobfs.h"
#include "tree.h"

#include "spdk/queue.h"
#include "spdk/thread.h"
#include "spdk/assert.h"
#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/trace.h"

#include "spdk_internal/trace_defs.h"

#define BLOBFS_TRACE(file, str, args...) \
	SPDK_DEBUGLOG(blobfs, "file=%s " str, file->name, ##args)

#define BLOBFS_TRACE_RW(file, str, args...) \
	SPDK_DEBUGLOG(blobfs_rw, "file=%s " str, file->name, ##args)

#define BLOBFS_DEFAULT_CACHE_SIZE (4ULL * 1024 * 1024 * 1024)
#define SPDK_BLOBFS_DEFAULT_OPTS_CLUSTER_SZ (1024 * 1024)

#define SPDK_BLOBFS_SIGNATURE	"BLOBFS"

static uint64_t g_fs_cache_size = BLOBFS_DEFAULT_CACHE_SIZE;
static struct spdk_mempool *g_cache_pool;
static TAILQ_HEAD(, spdk_file) g_caches = TAILQ_HEAD_INITIALIZER(g_caches);
static struct spdk_poller *g_cache_pool_mgmt_poller;
static struct spdk_thread *g_cache_pool_thread;
#define BLOBFS_CACHE_POOL_POLL_PERIOD_IN_US 1000ULL
static int g_fs_count = 0;
static pthread_mutex_t g_cache_init_lock = PTHREAD_MUTEX_INITIALIZER;

SPDK_TRACE_REGISTER_FN(blobfs_trace, "blobfs", TRACE_GROUP_BLOBFS)
{
	struct spdk_trace_tpoint_opts opts[] = {
		{
			"BLOBFS_XATTR_START", TRACE_BLOBFS_XATTR_START,
			OWNER_NONE, OBJECT_NONE, 0,
			{{ "file", SPDK_TRACE_ARG_TYPE_STR, 40 }},
		},
		{
			"BLOBFS_XATTR_END", TRACE_BLOBFS_XATTR_END,
			OWNER_NONE, OBJECT_NONE, 0,
			{{ "file", SPDK_TRACE_ARG_TYPE_STR, 40 }},
		},
		{
			"BLOBFS_OPEN", TRACE_BLOBFS_OPEN,
			OWNER_NONE, OBJECT_NONE, 0,
			{{ "file", SPDK_TRACE_ARG_TYPE_STR, 40 }},
		},
		{
			"BLOBFS_CLOSE", TRACE_BLOBFS_CLOSE,
			OWNER_NONE, OBJECT_NONE, 0,
			{{ "file", SPDK_TRACE_ARG_TYPE_STR, 40 }},
		},
		{
			"BLOBFS_DELETE_START", TRACE_BLOBFS_DELETE_START,
			OWNER_NONE, OBJECT_NONE, 0,
			{{ "file", SPDK_TRACE_ARG_TYPE_STR, 40 }},
		},
		{
			"BLOBFS_DELETE_DONE", TRACE_BLOBFS_DELETE_DONE,
			OWNER_NONE, OBJECT_NONE, 0,
			{{ "file", SPDK_TRACE_ARG_TYPE_STR, 40 }},
		}
	};

	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));
}

void
cache_buffer_free(struct cache_buffer *cache_buffer)
{
	spdk_mempool_put(g_cache_pool, cache_buffer->buf);
	free(cache_buffer);
}

#define CACHE_READAHEAD_THRESHOLD	(128 * 1024)

struct spdk_file {
	struct spdk_filesystem	*fs;
	struct spdk_blob	*blob;
	char			*name;
	uint64_t		length;
	bool                    is_deleted;
	bool			open_for_writing;
	uint64_t		length_flushed;
	uint64_t		length_xattr;
	uint64_t		append_pos;
	uint64_t		seq_byte_count;
	uint64_t		next_seq_offset;
	uint32_t		priority;
	TAILQ_ENTRY(spdk_file)	tailq;
	spdk_blob_id		blobid;
	uint32_t		ref_count;
	pthread_spinlock_t	lock;
	struct cache_buffer	*last;
	struct cache_tree	*tree;
	TAILQ_HEAD(open_requests_head, spdk_fs_request) open_requests;
	TAILQ_HEAD(sync_requests_head, spdk_fs_request) sync_requests;
	TAILQ_ENTRY(spdk_file)	cache_tailq;
};

struct spdk_deleted_file {
	spdk_blob_id	id;
	TAILQ_ENTRY(spdk_deleted_file)	tailq;
};

struct spdk_filesystem {
	struct spdk_blob_store	*bs;
	TAILQ_HEAD(, spdk_file)	files;
	struct spdk_bs_opts	bs_opts;
	struct spdk_bs_dev	*bdev;
	fs_send_request_fn	send_request;

	struct {
		uint32_t		max_ops;
		struct spdk_io_channel	*sync_io_channel;
		struct spdk_fs_channel	*sync_fs_channel;
	} sync_target;

	struct {
		uint32_t		max_ops;
		struct spdk_io_channel	*md_io_channel;
		struct spdk_fs_channel	*md_fs_channel;
	} md_target;

	struct {
		uint32_t		max_ops;
	} io_target;
};

struct spdk_fs_cb_args {
	union {
		spdk_fs_op_with_handle_complete		fs_op_with_handle;
		spdk_fs_op_complete			fs_op;
		spdk_file_op_with_handle_complete	file_op_with_handle;
		spdk_file_op_complete			file_op;
		spdk_file_stat_op_complete		stat_op;
	} fn;
	void *arg;
	sem_t *sem;
	struct spdk_filesystem *fs;
	struct spdk_file *file;
	int rc;
	int *rwerrno;
	struct iovec *iovs;
	uint32_t iovcnt;
	struct iovec iov;
	union {
		struct {
			TAILQ_HEAD(, spdk_deleted_file)	deleted_files;
		} fs_load;
		struct {
			uint64_t	length;
		} truncate;
		struct {
			struct spdk_io_channel	*channel;
			void		*pin_buf;
			int		is_read;
			off_t		offset;
			size_t		length;
			uint64_t	start_lba;
			uint64_t	num_lba;
			uint32_t	blocklen;
		} rw;
		struct {
			const char	*old_name;
			const char	*new_name;
		} rename;
		struct {
			struct cache_buffer	*cache_buffer;
			uint64_t		length;
		} flush;
		struct {
			struct cache_buffer	*cache_buffer;
			uint64_t		length;
			uint64_t		offset;
		} readahead;
		struct {
			/* offset of the file when the sync request was made */
			uint64_t			offset;
			TAILQ_ENTRY(spdk_fs_request)	tailq;
			bool				xattr_in_progress;
			/* length written to the xattr for this file - this should
			 * always be the same as the offset if only one thread is
			 * writing to the file, but could differ if multiple threads
			 * are appending
			 */
			uint64_t			length;
		} sync;
		struct {
			uint32_t			num_clusters;
		} resize;
		struct {
			const char	*name;
			uint32_t	flags;
			TAILQ_ENTRY(spdk_fs_request)	tailq;
		} open;
		struct {
			const char		*name;
			struct spdk_blob	*blob;
		} create;
		struct {
			const char	*name;
		} delete;
		struct {
			const char	*name;
		} stat;
	} op;
};

static void file_free(struct spdk_file *file);
static void fs_io_device_unregister(struct spdk_filesystem *fs);
static void fs_free_io_channels(struct spdk_filesystem *fs);

void
spdk_fs_opts_init(struct spdk_blobfs_opts *opts)
{
	opts->cluster_sz = SPDK_BLOBFS_DEFAULT_OPTS_CLUSTER_SZ;
}

static int _blobfs_cache_pool_reclaim(void *arg);

static bool
blobfs_cache_pool_need_reclaim(void)
{
	size_t count;

	count = spdk_mempool_count(g_cache_pool);
	/* We define a aggressive policy here as the requirements from db_bench are batched, so start the poller
	 *  when the number of available cache buffer is less than 1/5 of total buffers.
	 */
	if (count > (size_t)g_fs_cache_size / CACHE_BUFFER_SIZE / 5) {
		return false;
	}

	return true;
}

static void
__start_cache_pool_mgmt(void *ctx)
{
	assert(g_cache_pool == NULL);

	g_cache_pool = spdk_mempool_create("spdk_fs_cache",
					   g_fs_cache_size / CACHE_BUFFER_SIZE,
					   CACHE_BUFFER_SIZE,
					   SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
					   SPDK_ENV_SOCKET_ID_ANY);
	if (!g_cache_pool) {
		SPDK_ERRLOG("Create mempool failed, you may "
			    "increase the memory and try again\n");
		assert(false);
	}

	assert(g_cache_pool_mgmt_poller == NULL);
	g_cache_pool_mgmt_poller = SPDK_POLLER_REGISTER(_blobfs_cache_pool_reclaim, NULL,
				   BLOBFS_CACHE_POOL_POLL_PERIOD_IN_US);
}

static void
__stop_cache_pool_mgmt(void *ctx)
{
	spdk_poller_unregister(&g_cache_pool_mgmt_poller);

	assert(g_cache_pool != NULL);
	assert(spdk_mempool_count(g_cache_pool) == g_fs_cache_size / CACHE_BUFFER_SIZE);
	spdk_mempool_free(g_cache_pool);
	g_cache_pool = NULL;

	spdk_thread_exit(g_cache_pool_thread);
}

static void
initialize_global_cache(void)
{
	pthread_mutex_lock(&g_cache_init_lock);
	if (g_fs_count == 0) {
		g_cache_pool_thread = spdk_thread_create("cache_pool_mgmt", NULL);
		assert(g_cache_pool_thread != NULL);
		spdk_thread_send_msg(g_cache_pool_thread, __start_cache_pool_mgmt, NULL);
	}
	g_fs_count++;
	pthread_mutex_unlock(&g_cache_init_lock);
}

static void
free_global_cache(void)
{
	pthread_mutex_lock(&g_cache_init_lock);
	g_fs_count--;
	if (g_fs_count == 0) {
		spdk_thread_send_msg(g_cache_pool_thread, __stop_cache_pool_mgmt, NULL);
	}
	pthread_mutex_unlock(&g_cache_init_lock);
}

static uint64_t
__file_get_blob_size(struct spdk_file *file)
{
	uint64_t cluster_sz;

	cluster_sz = file->fs->bs_opts.cluster_sz;
	return cluster_sz * spdk_blob_get_num_clusters(file->blob);
}

struct spdk_fs_request {
	struct spdk_fs_cb_args		args;
	TAILQ_ENTRY(spdk_fs_request)	link;
	struct spdk_fs_channel		*channel;
};

struct spdk_fs_channel {
	struct spdk_fs_request		*req_mem;
	TAILQ_HEAD(, spdk_fs_request)	reqs;
	sem_t				sem;
	struct spdk_filesystem		*fs;
	struct spdk_io_channel		*bs_channel;
	fs_send_request_fn		send_request;
	bool				sync;
	uint32_t			outstanding_reqs;
	pthread_spinlock_t		lock;
};

/* For now, this is effectively an alias. But eventually we'll shift
 * some data members over. */
struct spdk_fs_thread_ctx {
	struct spdk_fs_channel	ch;
};

static struct spdk_fs_request *
alloc_fs_request_with_iov(struct spdk_fs_channel *channel, uint32_t iovcnt)
{
	struct spdk_fs_request *req;
	struct iovec *iovs = NULL;

	if (iovcnt > 1) {
		iovs = calloc(iovcnt, sizeof(struct iovec));
		if (!iovs) {
			return NULL;
		}
	}

	if (channel->sync) {
		pthread_spin_lock(&channel->lock);
	}

	req = TAILQ_FIRST(&channel->reqs);
	if (req) {
		channel->outstanding_reqs++;
		TAILQ_REMOVE(&channel->reqs, req, link);
	}

	if (channel->sync) {
		pthread_spin_unlock(&channel->lock);
	}

	if (req == NULL) {
		SPDK_ERRLOG("Cannot allocate req on spdk_fs_channel =%p\n", channel);
		free(iovs);
		return NULL;
	}
	memset(req, 0, sizeof(*req));
	req->channel = channel;
	if (iovcnt > 1) {
		req->args.iovs = iovs;
	} else {
		req->args.iovs = &req->args.iov;
	}
	req->args.iovcnt = iovcnt;

	return req;
}

static struct spdk_fs_request *
alloc_fs_request(struct spdk_fs_channel *channel)
{
	return alloc_fs_request_with_iov(channel, 0);
}

static void
free_fs_request(struct spdk_fs_request *req)
{
	struct spdk_fs_channel *channel = req->channel;

	if (req->args.iovcnt > 1) {
		free(req->args.iovs);
	}

	if (channel->sync) {
		pthread_spin_lock(&channel->lock);
	}

	TAILQ_INSERT_HEAD(&req->channel->reqs, req, link);
	channel->outstanding_reqs--;

	if (channel->sync) {
		pthread_spin_unlock(&channel->lock);
	}
}

static int
fs_channel_create(struct spdk_filesystem *fs, struct spdk_fs_channel *channel,
		  uint32_t max_ops)
{
	uint32_t i;

	channel->req_mem = calloc(max_ops, sizeof(struct spdk_fs_request));
	if (!channel->req_mem) {
		return -1;
	}

	channel->outstanding_reqs = 0;
	TAILQ_INIT(&channel->reqs);
	sem_init(&channel->sem, 0, 0);

	for (i = 0; i < max_ops; i++) {
		TAILQ_INSERT_TAIL(&channel->reqs, &channel->req_mem[i], link);
	}

	channel->fs = fs;

	return 0;
}

static int
fs_md_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_filesystem		*fs;
	struct spdk_fs_channel		*channel = ctx_buf;

	fs = SPDK_CONTAINEROF(io_device, struct spdk_filesystem, md_target);

	return fs_channel_create(fs, channel, fs->md_target.max_ops);
}

static int
fs_sync_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_filesystem		*fs;
	struct spdk_fs_channel		*channel = ctx_buf;

	fs = SPDK_CONTAINEROF(io_device, struct spdk_filesystem, sync_target);

	return fs_channel_create(fs, channel, fs->sync_target.max_ops);
}

static int
fs_io_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_filesystem		*fs;
	struct spdk_fs_channel		*channel = ctx_buf;

	fs = SPDK_CONTAINEROF(io_device, struct spdk_filesystem, io_target);

	return fs_channel_create(fs, channel, fs->io_target.max_ops);
}

static void
fs_channel_destroy(void *io_device, void *ctx_buf)
{
	struct spdk_fs_channel *channel = ctx_buf;

	if (channel->outstanding_reqs > 0) {
		SPDK_ERRLOG("channel freed with %" PRIu32 " outstanding requests!\n",
			    channel->outstanding_reqs);
	}

	free(channel->req_mem);
	if (channel->bs_channel != NULL) {
		spdk_bs_free_io_channel(channel->bs_channel);
	}
}

static void
__send_request_direct(fs_request_fn fn, void *arg)
{
	fn(arg);
}

static void
common_fs_bs_init(struct spdk_filesystem *fs, struct spdk_blob_store *bs)
{
	fs->bs = bs;
	fs->bs_opts.cluster_sz = spdk_bs_get_cluster_size(bs);
	fs->md_target.md_fs_channel->bs_channel = spdk_bs_alloc_io_channel(fs->bs);
	fs->md_target.md_fs_channel->send_request = __send_request_direct;
	fs->sync_target.sync_fs_channel->bs_channel = spdk_bs_alloc_io_channel(fs->bs);
	fs->sync_target.sync_fs_channel->send_request = __send_request_direct;

	initialize_global_cache();
}

static void
init_cb(void *ctx, struct spdk_blob_store *bs, int bserrno)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;
	struct spdk_filesystem *fs = args->fs;

	if (bserrno == 0) {
		common_fs_bs_init(fs, bs);
	} else {
		free(fs);
		fs = NULL;
	}

	args->fn.fs_op_with_handle(args->arg, fs, bserrno);
	free_fs_request(req);
}

static struct spdk_filesystem *
fs_alloc(struct spdk_bs_dev *dev, fs_send_request_fn send_request_fn)
{
	struct spdk_filesystem *fs;

	fs = calloc(1, sizeof(*fs));
	if (fs == NULL) {
		return NULL;
	}

	fs->bdev = dev;
	fs->send_request = send_request_fn;
	TAILQ_INIT(&fs->files);

	fs->md_target.max_ops = 512;
	spdk_io_device_register(&fs->md_target, fs_md_channel_create, fs_channel_destroy,
				sizeof(struct spdk_fs_channel), "blobfs_md");
	fs->md_target.md_io_channel = spdk_get_io_channel(&fs->md_target);
	fs->md_target.md_fs_channel = spdk_io_channel_get_ctx(fs->md_target.md_io_channel);

	fs->sync_target.max_ops = 512;
	spdk_io_device_register(&fs->sync_target, fs_sync_channel_create, fs_channel_destroy,
				sizeof(struct spdk_fs_channel), "blobfs_sync");
	fs->sync_target.sync_io_channel = spdk_get_io_channel(&fs->sync_target);
	fs->sync_target.sync_fs_channel = spdk_io_channel_get_ctx(fs->sync_target.sync_io_channel);

	fs->io_target.max_ops = 512;
	spdk_io_device_register(&fs->io_target, fs_io_channel_create, fs_channel_destroy,
				sizeof(struct spdk_fs_channel), "blobfs_io");

	return fs;
}

static void
__wake_caller(void *arg, int fserrno)
{
	struct spdk_fs_cb_args *args = arg;

	if ((args->rwerrno != NULL) && (*(args->rwerrno) == 0) && fserrno) {
		*(args->rwerrno) = fserrno;
	}
	args->rc = fserrno;
	sem_post(args->sem);
}

void
spdk_fs_init(struct spdk_bs_dev *dev, struct spdk_blobfs_opts *opt,
	     fs_send_request_fn send_request_fn,
	     spdk_fs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_filesystem *fs;
	struct spdk_fs_request *req;
	struct spdk_fs_cb_args *args;
	struct spdk_bs_opts opts = {};

	fs = fs_alloc(dev, send_request_fn);
	if (fs == NULL) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	req = alloc_fs_request(fs->md_target.md_fs_channel);
	if (req == NULL) {
		fs_free_io_channels(fs);
		fs_io_device_unregister(fs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	args = &req->args;
	args->fn.fs_op_with_handle = cb_fn;
	args->arg = cb_arg;
	args->fs = fs;

	spdk_bs_opts_init(&opts, sizeof(opts));
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), SPDK_BLOBFS_SIGNATURE);
	if (opt) {
		opts.cluster_sz = opt->cluster_sz;
	}
	spdk_bs_init(dev, &opts, init_cb, req);
}

static struct spdk_file *
file_alloc(struct spdk_filesystem *fs)
{
	struct spdk_file *file;

	file = calloc(1, sizeof(*file));
	if (file == NULL) {
		return NULL;
	}

	file->tree = calloc(1, sizeof(*file->tree));
	if (file->tree == NULL) {
		free(file);
		return NULL;
	}

	if (pthread_spin_init(&file->lock, 0)) {
		free(file->tree);
		free(file);
		return NULL;
	}

	file->fs = fs;
	TAILQ_INIT(&file->open_requests);
	TAILQ_INIT(&file->sync_requests);
	TAILQ_INSERT_TAIL(&fs->files, file, tailq);
	file->priority = SPDK_FILE_PRIORITY_LOW;
	return file;
}

static void fs_load_done(void *ctx, int bserrno);

static int
_handle_deleted_files(struct spdk_fs_request *req)
{
	struct spdk_fs_cb_args *args = &req->args;
	struct spdk_filesystem *fs = args->fs;

	if (!TAILQ_EMPTY(&args->op.fs_load.deleted_files)) {
		struct spdk_deleted_file *deleted_file;

		deleted_file = TAILQ_FIRST(&args->op.fs_load.deleted_files);
		TAILQ_REMOVE(&args->op.fs_load.deleted_files, deleted_file, tailq);
		spdk_bs_delete_blob(fs->bs, deleted_file->id, fs_load_done, req);
		free(deleted_file);
		return 0;
	}

	return 1;
}

static void
fs_load_done(void *ctx, int bserrno)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;
	struct spdk_filesystem *fs = args->fs;

	/* The filesystem has been loaded.  Now check if there are any files that
	 *  were marked for deletion before last unload.  Do not complete the
	 *  fs_load callback until all of them have been deleted on disk.
	 */
	if (_handle_deleted_files(req) == 0) {
		/* We found a file that's been marked for deleting but not actually
		 *  deleted yet.  This function will get called again once the delete
		 *  operation is completed.
		 */
		return;
	}

	args->fn.fs_op_with_handle(args->arg, fs, 0);
	free_fs_request(req);

}

static void
iter_cb(void *ctx, struct spdk_blob *blob, int rc)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;
	struct spdk_filesystem *fs = args->fs;
	uint64_t *length;
	const char *name;
	uint32_t *is_deleted;
	size_t value_len;

	if (rc < 0) {
		args->fn.fs_op_with_handle(args->arg, fs, rc);
		free_fs_request(req);
		return;
	}

	rc = spdk_blob_get_xattr_value(blob, "name", (const void **)&name, &value_len);
	if (rc < 0) {
		args->fn.fs_op_with_handle(args->arg, fs, rc);
		free_fs_request(req);
		return;
	}

	rc = spdk_blob_get_xattr_value(blob, "length", (const void **)&length, &value_len);
	if (rc < 0) {
		args->fn.fs_op_with_handle(args->arg, fs, rc);
		free_fs_request(req);
		return;
	}

	assert(value_len == 8);

	/* This file could be deleted last time without close it, then app crashed, so we delete it now */
	rc = spdk_blob_get_xattr_value(blob, "is_deleted", (const void **)&is_deleted, &value_len);
	if (rc < 0) {
		struct spdk_file *f;

		f = file_alloc(fs);
		if (f == NULL) {
			SPDK_ERRLOG("Cannot allocate file to handle deleted file on disk\n");
			args->fn.fs_op_with_handle(args->arg, fs, -ENOMEM);
			free_fs_request(req);
			return;
		}

		f->name = strdup(name);
		f->blobid = spdk_blob_get_id(blob);
		f->length = *length;
		f->length_flushed = *length;
		f->length_xattr = *length;
		f->append_pos = *length;
		SPDK_DEBUGLOG(blobfs, "added file %s length=%ju\n", f->name, f->length);
	} else {
		struct spdk_deleted_file *deleted_file;

		deleted_file = calloc(1, sizeof(*deleted_file));
		if (deleted_file == NULL) {
			args->fn.fs_op_with_handle(args->arg, fs, -ENOMEM);
			free_fs_request(req);
			return;
		}
		deleted_file->id = spdk_blob_get_id(blob);
		TAILQ_INSERT_TAIL(&args->op.fs_load.deleted_files, deleted_file, tailq);
	}
}

static void
load_cb(void *ctx, struct spdk_blob_store *bs, int bserrno)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;
	struct spdk_filesystem *fs = args->fs;
	struct spdk_bs_type bstype;
	static const struct spdk_bs_type blobfs_type = {SPDK_BLOBFS_SIGNATURE};
	static const struct spdk_bs_type zeros;

	if (bserrno != 0) {
		args->fn.fs_op_with_handle(args->arg, NULL, bserrno);
		free_fs_request(req);
		fs_free_io_channels(fs);
		fs_io_device_unregister(fs);
		return;
	}

	bstype = spdk_bs_get_bstype(bs);

	if (!memcmp(&bstype, &zeros, sizeof(bstype))) {
		SPDK_DEBUGLOG(blobfs, "assigning bstype\n");
		spdk_bs_set_bstype(bs, blobfs_type);
	} else if (memcmp(&bstype, &blobfs_type, sizeof(bstype))) {
		SPDK_ERRLOG("not blobfs\n");
		SPDK_LOGDUMP(blobfs, "bstype", &bstype, sizeof(bstype));
		args->fn.fs_op_with_handle(args->arg, NULL, -EINVAL);
		free_fs_request(req);
		fs_free_io_channels(fs);
		fs_io_device_unregister(fs);
		return;
	}

	common_fs_bs_init(fs, bs);
	fs_load_done(req, 0);
}

static void
fs_io_device_unregister(struct spdk_filesystem *fs)
{
	assert(fs != NULL);
	spdk_io_device_unregister(&fs->md_target, NULL);
	spdk_io_device_unregister(&fs->sync_target, NULL);
	spdk_io_device_unregister(&fs->io_target, NULL);
	free(fs);
}

static void
fs_free_io_channels(struct spdk_filesystem *fs)
{
	assert(fs != NULL);
	spdk_fs_free_io_channel(fs->md_target.md_io_channel);
	spdk_fs_free_io_channel(fs->sync_target.sync_io_channel);
}

void
spdk_fs_load(struct spdk_bs_dev *dev, fs_send_request_fn send_request_fn,
	     spdk_fs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_filesystem *fs;
	struct spdk_fs_cb_args *args;
	struct spdk_fs_request *req;
	struct spdk_bs_opts	bs_opts;

	fs = fs_alloc(dev, send_request_fn);
	if (fs == NULL) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	req = alloc_fs_request(fs->md_target.md_fs_channel);
	if (req == NULL) {
		fs_free_io_channels(fs);
		fs_io_device_unregister(fs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	args = &req->args;
	args->fn.fs_op_with_handle = cb_fn;
	args->arg = cb_arg;
	args->fs = fs;
	TAILQ_INIT(&args->op.fs_load.deleted_files);
	spdk_bs_opts_init(&bs_opts, sizeof(bs_opts));
	bs_opts.iter_cb_fn = iter_cb;
	bs_opts.iter_cb_arg = req;
	spdk_bs_load(dev, &bs_opts, load_cb, req);
}

static void
unload_cb(void *ctx, int bserrno)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;
	struct spdk_filesystem *fs = args->fs;
	struct spdk_file *file, *tmp;

	TAILQ_FOREACH_SAFE(file, &fs->files, tailq, tmp) {
		TAILQ_REMOVE(&fs->files, file, tailq);
		file_free(file);
	}

	free_global_cache();

	args->fn.fs_op(args->arg, bserrno);
	free(req);

	fs_io_device_unregister(fs);
}

void
spdk_fs_unload(struct spdk_filesystem *fs, spdk_fs_op_complete cb_fn, void *cb_arg)
{
	struct spdk_fs_request *req;
	struct spdk_fs_cb_args *args;

	/*
	 * We must free the md_channel before unloading the blobstore, so just
	 *  allocate this request from the general heap.
	 */
	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	args = &req->args;
	args->fn.fs_op = cb_fn;
	args->arg = cb_arg;
	args->fs = fs;

	fs_free_io_channels(fs);
	spdk_bs_unload(fs->bs, unload_cb, req);
}

static struct spdk_file *
fs_find_file(struct spdk_filesystem *fs, const char *name)
{
	struct spdk_file *file;

	TAILQ_FOREACH(file, &fs->files, tailq) {
		if (!strncmp(name, file->name, SPDK_FILE_NAME_MAX)) {
			return file;
		}
	}

	return NULL;
}

void
spdk_fs_file_stat_async(struct spdk_filesystem *fs, const char *name,
			spdk_file_stat_op_complete cb_fn, void *cb_arg)
{
	struct spdk_file_stat stat;
	struct spdk_file *f = NULL;

	if (strnlen(name, SPDK_FILE_NAME_MAX + 1) == SPDK_FILE_NAME_MAX + 1) {
		cb_fn(cb_arg, NULL, -ENAMETOOLONG);
		return;
	}

	f = fs_find_file(fs, name);
	if (f != NULL) {
		stat.blobid = f->blobid;
		stat.size = f->append_pos >= f->length ? f->append_pos : f->length;
		cb_fn(cb_arg, &stat, 0);
		return;
	}

	cb_fn(cb_arg, NULL, -ENOENT);
}

static void
__copy_stat(void *arg, struct spdk_file_stat *stat, int fserrno)
{
	struct spdk_fs_request *req = arg;
	struct spdk_fs_cb_args *args = &req->args;

	args->rc = fserrno;
	if (fserrno == 0) {
		memcpy(args->arg, stat, sizeof(*stat));
	}
	sem_post(args->sem);
}

static void
__file_stat(void *arg)
{
	struct spdk_fs_request *req = arg;
	struct spdk_fs_cb_args *args = &req->args;

	spdk_fs_file_stat_async(args->fs, args->op.stat.name,
				args->fn.stat_op, req);
}

int
spdk_fs_file_stat(struct spdk_filesystem *fs, struct spdk_fs_thread_ctx *ctx,
		  const char *name, struct spdk_file_stat *stat)
{
	struct spdk_fs_channel *channel = (struct spdk_fs_channel *)ctx;
	struct spdk_fs_request *req;
	int rc;

	req = alloc_fs_request(channel);
	if (req == NULL) {
		SPDK_ERRLOG("Cannot allocate stat req on file=%s\n", name);
		return -ENOMEM;
	}

	req->args.fs = fs;
	req->args.op.stat.name = name;
	req->args.fn.stat_op = __copy_stat;
	req->args.arg = stat;
	req->args.sem = &channel->sem;
	channel->send_request(__file_stat, req);
	sem_wait(&channel->sem);

	rc = req->args.rc;
	free_fs_request(req);

	return rc;
}

static void
fs_create_blob_close_cb(void *ctx, int bserrno)
{
	int rc;
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;

	rc = args->rc ? args->rc : bserrno;
	args->fn.file_op(args->arg, rc);
	free_fs_request(req);
}

static void
fs_create_blob_resize_cb(void *ctx, int bserrno)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;
	struct spdk_file *f = args->file;
	struct spdk_blob *blob = args->op.create.blob;
	uint64_t length = 0;

	args->rc = bserrno;
	if (bserrno) {
		spdk_blob_close(blob, fs_create_blob_close_cb, args);
		return;
	}

	spdk_blob_set_xattr(blob, "name", f->name, strlen(f->name) + 1);
	spdk_blob_set_xattr(blob, "length", &length, sizeof(length));

	spdk_blob_close(blob, fs_create_blob_close_cb, args);
}

static void
fs_create_blob_open_cb(void *ctx, struct spdk_blob *blob, int bserrno)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;

	if (bserrno) {
		args->fn.file_op(args->arg, bserrno);
		free_fs_request(req);
		return;
	}

	args->op.create.blob = blob;
	spdk_blob_resize(blob, 1, fs_create_blob_resize_cb, req);
}

static void
fs_create_blob_create_cb(void *ctx, spdk_blob_id blobid, int bserrno)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;
	struct spdk_file *f = args->file;

	if (bserrno) {
		args->fn.file_op(args->arg, bserrno);
		free_fs_request(req);
		return;
	}

	f->blobid = blobid;
	spdk_bs_open_blob(f->fs->bs, blobid, fs_create_blob_open_cb, req);
}

void
spdk_fs_create_file_async(struct spdk_filesystem *fs, const char *name,
			  spdk_file_op_complete cb_fn, void *cb_arg)
{
	struct spdk_file *file;
	struct spdk_fs_request *req;
	struct spdk_fs_cb_args *args;

	if (strnlen(name, SPDK_FILE_NAME_MAX + 1) == SPDK_FILE_NAME_MAX + 1) {
		cb_fn(cb_arg, -ENAMETOOLONG);
		return;
	}

	file = fs_find_file(fs, name);
	if (file != NULL) {
		cb_fn(cb_arg, -EEXIST);
		return;
	}

	file = file_alloc(fs);
	if (file == NULL) {
		SPDK_ERRLOG("Cannot allocate new file for creation\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	req = alloc_fs_request(fs->md_target.md_fs_channel);
	if (req == NULL) {
		SPDK_ERRLOG("Cannot allocate create async req for file=%s\n", name);
		TAILQ_REMOVE(&fs->files, file, tailq);
		file_free(file);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	args = &req->args;
	args->file = file;
	args->fn.file_op = cb_fn;
	args->arg = cb_arg;

	file->name = strdup(name);
	if (!file->name) {
		SPDK_ERRLOG("Cannot allocate file->name for file=%s\n", name);
		free_fs_request(req);
		TAILQ_REMOVE(&fs->files, file, tailq);
		file_free(file);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	spdk_bs_create_blob(fs->bs, fs_create_blob_create_cb, args);
}

static void
__fs_create_file_done(void *arg, int fserrno)
{
	struct spdk_fs_request *req = arg;
	struct spdk_fs_cb_args *args = &req->args;

	__wake_caller(args, fserrno);
	SPDK_DEBUGLOG(blobfs, "file=%s\n", args->op.create.name);
}

static void
__fs_create_file(void *arg)
{
	struct spdk_fs_request *req = arg;
	struct spdk_fs_cb_args *args = &req->args;

	SPDK_DEBUGLOG(blobfs, "file=%s\n", args->op.create.name);
	spdk_fs_create_file_async(args->fs, args->op.create.name, __fs_create_file_done, req);
}

int
spdk_fs_create_file(struct spdk_filesystem *fs, struct spdk_fs_thread_ctx *ctx, const char *name)
{
	struct spdk_fs_channel *channel = (struct spdk_fs_channel *)ctx;
	struct spdk_fs_request *req;
	struct spdk_fs_cb_args *args;
	int rc;

	SPDK_DEBUGLOG(blobfs, "file=%s\n", name);

	req = alloc_fs_request(channel);
	if (req == NULL) {
		SPDK_ERRLOG("Cannot allocate req to create file=%s\n", name);
		return -ENOMEM;
	}

	args = &req->args;
	args->fs = fs;
	args->op.create.name = name;
	args->sem = &channel->sem;
	fs->send_request(__fs_create_file, req);
	sem_wait(&channel->sem);
	rc = args->rc;
	free_fs_request(req);

	return rc;
}

static void
fs_open_blob_done(void *ctx, struct spdk_blob *blob, int bserrno)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;
	struct spdk_file *f = args->file;

	f->blob = blob;
	while (!TAILQ_EMPTY(&f->open_requests)) {
		req = TAILQ_FIRST(&f->open_requests);
		args = &req->args;
		TAILQ_REMOVE(&f->open_requests, req, args.op.open.tailq);
		spdk_trace_record(TRACE_BLOBFS_OPEN, 0, 0, 0, f->name);
		args->fn.file_op_with_handle(args->arg, f, bserrno);
		free_fs_request(req);
	}
}

static void
fs_open_blob_create_cb(void *ctx, int bserrno)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;
	struct spdk_file *file = args->file;
	struct spdk_filesystem *fs = args->fs;

	if (file == NULL) {
		/*
		 * This is from an open with CREATE flag - the file
		 *  is now created so look it up in the file list for this
		 *  filesystem.
		 */
		file = fs_find_file(fs, args->op.open.name);
		assert(file != NULL);
		args->file = file;
	}

	file->ref_count++;
	TAILQ_INSERT_TAIL(&file->open_requests, req, args.op.open.tailq);
	if (file->ref_count == 1) {
		assert(file->blob == NULL);
		spdk_bs_open_blob(fs->bs, file->blobid, fs_open_blob_done, req);
	} else if (file->blob != NULL) {
		fs_open_blob_done(req, file->blob, 0);
	} else {
		/*
		 * The blob open for this file is in progress due to a previous
		 *  open request.  When that open completes, it will invoke the
		 *  open callback for this request.
		 */
	}
}

void
spdk_fs_open_file_async(struct spdk_filesystem *fs, const char *name, uint32_t flags,
			spdk_file_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_file *f = NULL;
	struct spdk_fs_request *req;
	struct spdk_fs_cb_args *args;

	if (strnlen(name, SPDK_FILE_NAME_MAX + 1) == SPDK_FILE_NAME_MAX + 1) {
		cb_fn(cb_arg, NULL, -ENAMETOOLONG);
		return;
	}

	f = fs_find_file(fs, name);
	if (f == NULL && !(flags & SPDK_BLOBFS_OPEN_CREATE)) {
		cb_fn(cb_arg, NULL, -ENOENT);
		return;
	}

	if (f != NULL && f->is_deleted == true) {
		cb_fn(cb_arg, NULL, -ENOENT);
		return;
	}

	req = alloc_fs_request(fs->md_target.md_fs_channel);
	if (req == NULL) {
		SPDK_ERRLOG("Cannot allocate async open req for file=%s\n", name);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	args = &req->args;
	args->fn.file_op_with_handle = cb_fn;
	args->arg = cb_arg;
	args->file = f;
	args->fs = fs;
	args->op.open.name = name;

	if (f == NULL) {
		spdk_fs_create_file_async(fs, name, fs_open_blob_create_cb, req);
	} else {
		fs_open_blob_create_cb(req, 0);
	}
}

static void
__fs_open_file_done(void *arg, struct spdk_file *file, int bserrno)
{
	struct spdk_fs_request *req = arg;
	struct spdk_fs_cb_args *args = &req->args;

	args->file = file;
	__wake_caller(args, bserrno);
	SPDK_DEBUGLOG(blobfs, "file=%s\n", args->op.open.name);
}

static void
__fs_open_file(void *arg)
{
	struct spdk_fs_request *req = arg;
	struct spdk_fs_cb_args *args = &req->args;

	SPDK_DEBUGLOG(blobfs, "file=%s\n", args->op.open.name);
	spdk_fs_open_file_async(args->fs, args->op.open.name, args->op.open.flags,
				__fs_open_file_done, req);
}

int
spdk_fs_open_file(struct spdk_filesystem *fs, struct spdk_fs_thread_ctx *ctx,
		  const char *name, uint32_t flags, struct spdk_file **file)
{
	struct spdk_fs_channel *channel = (struct spdk_fs_channel *)ctx;
	struct spdk_fs_request *req;
	struct spdk_fs_cb_args *args;
	int rc;

	SPDK_DEBUGLOG(blobfs, "file=%s\n", name);

	req = alloc_fs_request(channel);
	if (req == NULL) {
		SPDK_ERRLOG("Cannot allocate req for opening file=%s\n", name);
		return -ENOMEM;
	}

	args = &req->args;
	args->fs = fs;
	args->op.open.name = name;
	args->op.open.flags = flags;
	args->sem = &channel->sem;
	fs->send_request(__fs_open_file, req);
	sem_wait(&channel->sem);
	rc = args->rc;
	if (rc == 0) {
		*file = args->file;
	} else {
		*file = NULL;
	}
	free_fs_request(req);

	return rc;
}

static void
fs_rename_blob_close_cb(void *ctx, int bserrno)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;

	args->fn.fs_op(args->arg, bserrno);
	free_fs_request(req);
}

static void
fs_rename_blob_open_cb(void *ctx, struct spdk_blob *blob, int bserrno)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;
	const char *new_name = args->op.rename.new_name;

	spdk_blob_set_xattr(blob, "name", new_name, strlen(new_name) + 1);
	spdk_blob_close(blob, fs_rename_blob_close_cb, req);
}

static void
_fs_md_rename_file(struct spdk_fs_request *req)
{
	struct spdk_fs_cb_args *args = &req->args;
	struct spdk_file *f;

	f = fs_find_file(args->fs, args->op.rename.old_name);
	if (f == NULL) {
		args->fn.fs_op(args->arg, -ENOENT);
		free_fs_request(req);
		return;
	}

	free(f->name);
	f->name = strdup(args->op.rename.new_name);
	args->file = f;
	spdk_bs_open_blob(args->fs->bs, f->blobid, fs_rename_blob_open_cb, req);
}

static void
fs_rename_delete_done(void *arg, int fserrno)
{
	_fs_md_rename_file(arg);
}

void
spdk_fs_rename_file_async(struct spdk_filesystem *fs,
			  const char *old_name, const char *new_name,
			  spdk_file_op_complete cb_fn, void *cb_arg)
{
	struct spdk_file *f;
	struct spdk_fs_request *req;
	struct spdk_fs_cb_args *args;

	SPDK_DEBUGLOG(blobfs, "old=%s new=%s\n", old_name, new_name);
	if (strnlen(new_name, SPDK_FILE_NAME_MAX + 1) == SPDK_FILE_NAME_MAX + 1) {
		cb_fn(cb_arg, -ENAMETOOLONG);
		return;
	}

	req = alloc_fs_request(fs->md_target.md_fs_channel);
	if (req == NULL) {
		SPDK_ERRLOG("Cannot allocate rename async req for renaming file from %s to %s\n", old_name,
			    new_name);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	args = &req->args;
	args->fn.fs_op = cb_fn;
	args->fs = fs;
	args->arg = cb_arg;
	args->op.rename.old_name = old_name;
	args->op.rename.new_name = new_name;

	f = fs_find_file(fs, new_name);
	if (f == NULL) {
		_fs_md_rename_file(req);
		return;
	}

	/*
	 * The rename overwrites an existing file.  So delete the existing file, then
	 *  do the actual rename.
	 */
	spdk_fs_delete_file_async(fs, new_name, fs_rename_delete_done, req);
}

static void
__fs_rename_file_done(void *arg, int fserrno)
{
	struct spdk_fs_request *req = arg;
	struct spdk_fs_cb_args *args = &req->args;

	__wake_caller(args, fserrno);
}

static void
__fs_rename_file(void *arg)
{
	struct spdk_fs_request *req = arg;
	struct spdk_fs_cb_args *args = &req->args;

	spdk_fs_rename_file_async(args->fs, args->op.rename.old_name, args->op.rename.new_name,
				  __fs_rename_file_done, req);
}

int
spdk_fs_rename_file(struct spdk_filesystem *fs, struct spdk_fs_thread_ctx *ctx,
		    const char *old_name, const char *new_name)
{
	struct spdk_fs_channel *channel = (struct spdk_fs_channel *)ctx;
	struct spdk_fs_request *req;
	struct spdk_fs_cb_args *args;
	int rc;

	req = alloc_fs_request(channel);
	if (req == NULL) {
		SPDK_ERRLOG("Cannot allocate rename req for file=%s\n", old_name);
		return -ENOMEM;
	}

	args = &req->args;

	args->fs = fs;
	args->op.rename.old_name = old_name;
	args->op.rename.new_name = new_name;
	args->sem = &channel->sem;
	fs->send_request(__fs_rename_file, req);
	sem_wait(&channel->sem);
	rc = args->rc;
	free_fs_request(req);
	return rc;
}

static void
blob_delete_cb(void *ctx, int bserrno)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;

	args->fn.file_op(args->arg, bserrno);
	free_fs_request(req);
}

void
spdk_fs_delete_file_async(struct spdk_filesystem *fs, const char *name,
			  spdk_file_op_complete cb_fn, void *cb_arg)
{
	struct spdk_file *f;
	spdk_blob_id blobid;
	struct spdk_fs_request *req;
	struct spdk_fs_cb_args *args;

	SPDK_DEBUGLOG(blobfs, "file=%s\n", name);

	if (strnlen(name, SPDK_FILE_NAME_MAX + 1) == SPDK_FILE_NAME_MAX + 1) {
		cb_fn(cb_arg, -ENAMETOOLONG);
		return;
	}

	f = fs_find_file(fs, name);
	if (f == NULL) {
		SPDK_ERRLOG("Cannot find the file=%s to deleted\n", name);
		cb_fn(cb_arg, -ENOENT);
		return;
	}

	req = alloc_fs_request(fs->md_target.md_fs_channel);
	if (req == NULL) {
		SPDK_ERRLOG("Cannot allocate the req for the file=%s to deleted\n", name);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	args = &req->args;
	args->fn.file_op = cb_fn;
	args->arg = cb_arg;

	if (f->ref_count > 0) {
		/* If the ref > 0, we mark the file as deleted and delete it when we close it. */
		f->is_deleted = true;
		spdk_blob_set_xattr(f->blob, "is_deleted", &f->is_deleted, sizeof(bool));
		spdk_blob_sync_md(f->blob, blob_delete_cb, req);
		return;
	}

	blobid = f->blobid;
	TAILQ_REMOVE(&fs->files, f, tailq);

	file_free(f);

	spdk_bs_delete_blob(fs->bs, blobid, blob_delete_cb, req);
}

static void
__fs_delete_file_done(void *arg, int fserrno)
{
	struct spdk_fs_request *req = arg;
	struct spdk_fs_cb_args *args = &req->args;

	spdk_trace_record(TRACE_BLOBFS_DELETE_DONE, 0, 0, 0, args->op.delete.name);
	__wake_caller(args, fserrno);
}

static void
__fs_delete_file(void *arg)
{
	struct spdk_fs_request *req = arg;
	struct spdk_fs_cb_args *args = &req->args;

	spdk_trace_record(TRACE_BLOBFS_DELETE_START, 0, 0, 0, args->op.delete.name);
	spdk_fs_delete_file_async(args->fs, args->op.delete.name, __fs_delete_file_done, req);
}

int
spdk_fs_delete_file(struct spdk_filesystem *fs, struct spdk_fs_thread_ctx *ctx,
		    const char *name)
{
	struct spdk_fs_channel *channel = (struct spdk_fs_channel *)ctx;
	struct spdk_fs_request *req;
	struct spdk_fs_cb_args *args;
	int rc;

	req = alloc_fs_request(channel);
	if (req == NULL) {
		SPDK_DEBUGLOG(blobfs, "Cannot allocate req to delete file=%s\n", name);
		return -ENOMEM;
	}

	args = &req->args;
	args->fs = fs;
	args->op.delete.name = name;
	args->sem = &channel->sem;
	fs->send_request(__fs_delete_file, req);
	sem_wait(&channel->sem);
	rc = args->rc;
	free_fs_request(req);

	return rc;
}

spdk_fs_iter
spdk_fs_iter_first(struct spdk_filesystem *fs)
{
	struct spdk_file *f;

	f = TAILQ_FIRST(&fs->files);
	return f;
}

spdk_fs_iter
spdk_fs_iter_next(spdk_fs_iter iter)
{
	struct spdk_file *f = iter;

	if (f == NULL) {
		return NULL;
	}

	f = TAILQ_NEXT(f, tailq);
	return f;
}

const char *
spdk_file_get_name(struct spdk_file *file)
{
	return file->name;
}

uint64_t
spdk_file_get_length(struct spdk_file *file)
{
	uint64_t length;

	assert(file != NULL);

	length = file->append_pos >= file->length ? file->append_pos : file->length;
	SPDK_DEBUGLOG(blobfs, "file=%s length=0x%jx\n", file->name, length);
	return length;
}

static void
fs_truncate_complete_cb(void *ctx, int bserrno)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;

	args->fn.file_op(args->arg, bserrno);
	free_fs_request(req);
}

static void
fs_truncate_resize_cb(void *ctx, int bserrno)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;
	struct spdk_file *file = args->file;
	uint64_t *length = &args->op.truncate.length;

	if (bserrno) {
		args->fn.file_op(args->arg, bserrno);
		free_fs_request(req);
		return;
	}

	spdk_blob_set_xattr(file->blob, "length", length, sizeof(*length));

	file->length = *length;
	if (file->append_pos > file->length) {
		file->append_pos = file->length;
	}

	spdk_blob_sync_md(file->blob, fs_truncate_complete_cb, req);
}

static uint64_t
__bytes_to_clusters(uint64_t length, uint64_t cluster_sz)
{
	return (length + cluster_sz - 1) / cluster_sz;
}

void
spdk_file_truncate_async(struct spdk_file *file, uint64_t length,
			 spdk_file_op_complete cb_fn, void *cb_arg)
{
	struct spdk_filesystem *fs;
	size_t num_clusters;
	struct spdk_fs_request *req;
	struct spdk_fs_cb_args *args;

	SPDK_DEBUGLOG(blobfs, "file=%s old=0x%jx new=0x%jx\n", file->name, file->length, length);
	if (length == file->length) {
		cb_fn(cb_arg, 0);
		return;
	}

	req = alloc_fs_request(file->fs->md_target.md_fs_channel);
	if (req == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	args = &req->args;
	args->fn.file_op = cb_fn;
	args->arg = cb_arg;
	args->file = file;
	args->op.truncate.length = length;
	fs = file->fs;

	num_clusters = __bytes_to_clusters(length, fs->bs_opts.cluster_sz);

	spdk_blob_resize(file->blob, num_clusters, fs_truncate_resize_cb, req);
}

static void
__truncate(void *arg)
{
	struct spdk_fs_request *req = arg;
	struct spdk_fs_cb_args *args = &req->args;

	spdk_file_truncate_async(args->file, args->op.truncate.length,
				 args->fn.file_op, args);
}

int
spdk_file_truncate(struct spdk_file *file, struct spdk_fs_thread_ctx *ctx,
		   uint64_t length)
{
	struct spdk_fs_channel *channel = (struct spdk_fs_channel *)ctx;
	struct spdk_fs_request *req;
	struct spdk_fs_cb_args *args;
	int rc;

	req = alloc_fs_request(channel);
	if (req == NULL) {
		return -ENOMEM;
	}

	args = &req->args;

	args->file = file;
	args->op.truncate.length = length;
	args->fn.file_op = __wake_caller;
	args->sem = &channel->sem;

	channel->send_request(__truncate, req);
	sem_wait(&channel->sem);
	rc = args->rc;
	free_fs_request(req);

	return rc;
}

static void
__rw_done(void *ctx, int bserrno)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;

	spdk_free(args->op.rw.pin_buf);
	args->fn.file_op(args->arg, bserrno);
	free_fs_request(req);
}

static void
_copy_iovs_to_buf(void *buf, size_t buf_len, struct iovec *iovs, int iovcnt)
{
	int i;
	size_t len;

	for (i = 0; i < iovcnt; i++) {
		len = spdk_min(iovs[i].iov_len, buf_len);
		memcpy(buf, iovs[i].iov_base, len);
		buf += len;
		assert(buf_len >= len);
		buf_len -= len;
	}
}

static void
_copy_buf_to_iovs(struct iovec *iovs, int iovcnt, void *buf, size_t buf_len)
{
	int i;
	size_t len;

	for (i = 0; i < iovcnt; i++) {
		len = spdk_min(iovs[i].iov_len, buf_len);
		memcpy(iovs[i].iov_base, buf, len);
		buf += len;
		assert(buf_len >= len);
		buf_len -= len;
	}
}

static void
__read_done(void *ctx, int bserrno)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;
	void *buf;

	assert(req != NULL);
	buf = (void *)((uintptr_t)args->op.rw.pin_buf + (args->op.rw.offset & (args->op.rw.blocklen - 1)));
	if (args->op.rw.is_read) {
		_copy_buf_to_iovs(args->iovs, args->iovcnt, buf, args->op.rw.length);
		__rw_done(req, 0);
	} else {
		_copy_iovs_to_buf(buf, args->op.rw.length, args->iovs, args->iovcnt);
		spdk_blob_io_write(args->file->blob, args->op.rw.channel,
				   args->op.rw.pin_buf,
				   args->op.rw.start_lba, args->op.rw.num_lba,
				   __rw_done, req);
	}
}

static void
__do_blob_read(void *ctx, int fserrno)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;

	if (fserrno) {
		__rw_done(req, fserrno);
		return;
	}
	spdk_blob_io_read(args->file->blob, args->op.rw.channel,
			  args->op.rw.pin_buf,
			  args->op.rw.start_lba, args->op.rw.num_lba,
			  __read_done, req);
}

static void
__get_page_parameters(struct spdk_file *file, uint64_t offset, uint64_t length,
		      uint64_t *start_lba, uint32_t *lba_size, uint64_t *num_lba)
{
	uint64_t end_lba;

	*lba_size = spdk_bs_get_io_unit_size(file->fs->bs);
	*start_lba = offset / *lba_size;
	end_lba = (offset + length - 1) / *lba_size;
	*num_lba = (end_lba - *start_lba + 1);
}

static bool
__is_lba_aligned(struct spdk_file *file, uint64_t offset, uint64_t length)
{
	uint32_t lba_size = spdk_bs_get_io_unit_size(file->fs->bs);

	if ((offset % lba_size == 0) && (length % lba_size == 0)) {
		return true;
	}

	return false;
}

static void
_fs_request_setup_iovs(struct spdk_fs_request *req, struct iovec *iovs, uint32_t iovcnt)
{
	uint32_t i;

	for (i = 0; i < iovcnt; i++) {
		req->args.iovs[i].iov_base = iovs[i].iov_base;
		req->args.iovs[i].iov_len = iovs[i].iov_len;
	}
}

static void
__readvwritev(struct spdk_file *file, struct spdk_io_channel *_channel,
	      struct iovec *iovs, uint32_t iovcnt, uint64_t offset, uint64_t length,
	      spdk_file_op_complete cb_fn, void *cb_arg, int is_read)
{
	struct spdk_fs_request *req;
	struct spdk_fs_cb_args *args;
	struct spdk_fs_channel *channel = spdk_io_channel_get_ctx(_channel);
	uint64_t start_lba, num_lba, pin_buf_length;
	uint32_t lba_size;

	if (is_read && offset + length > file->length) {
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	req = alloc_fs_request_with_iov(channel, iovcnt);
	if (req == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	__get_page_parameters(file, offset, length, &start_lba, &lba_size, &num_lba);

	args = &req->args;
	args->fn.file_op = cb_fn;
	args->arg = cb_arg;
	args->file = file;
	args->op.rw.channel = channel->bs_channel;
	_fs_request_setup_iovs(req, iovs, iovcnt);
	args->op.rw.is_read = is_read;
	args->op.rw.offset = offset;
	args->op.rw.blocklen = lba_size;

	pin_buf_length = num_lba * lba_size;
	args->op.rw.length = pin_buf_length;
	args->op.rw.pin_buf = spdk_malloc(pin_buf_length, lba_size, NULL,
					  SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (args->op.rw.pin_buf == NULL) {
		SPDK_DEBUGLOG(blobfs, "Failed to allocate buf for: file=%s offset=%jx length=%jx\n",
			      file->name, offset, length);
		free_fs_request(req);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	args->op.rw.start_lba = start_lba;
	args->op.rw.num_lba = num_lba;

	if (!is_read && file->length < offset + length) {
		spdk_file_truncate_async(file, offset + length, __do_blob_read, req);
	} else if (!is_read && __is_lba_aligned(file, offset, length)) {
		_copy_iovs_to_buf(args->op.rw.pin_buf, args->op.rw.length, args->iovs, args->iovcnt);
		spdk_blob_io_write(args->file->blob, args->op.rw.channel,
				   args->op.rw.pin_buf,
				   args->op.rw.start_lba, args->op.rw.num_lba,
				   __rw_done, req);
	} else {
		__do_blob_read(req, 0);
	}
}

static void
__readwrite(struct spdk_file *file, struct spdk_io_channel *channel,
	    void *payload, uint64_t offset, uint64_t length,
	    spdk_file_op_complete cb_fn, void *cb_arg, int is_read)
{
	struct iovec iov;

	iov.iov_base = payload;
	iov.iov_len = (size_t)length;

	__readvwritev(file, channel, &iov, 1, offset, length, cb_fn, cb_arg, is_read);
}

void
spdk_file_write_async(struct spdk_file *file, struct spdk_io_channel *channel,
		      void *payload, uint64_t offset, uint64_t length,
		      spdk_file_op_complete cb_fn, void *cb_arg)
{
	__readwrite(file, channel, payload, offset, length, cb_fn, cb_arg, 0);
}

void
spdk_file_writev_async(struct spdk_file *file, struct spdk_io_channel *channel,
		       struct iovec *iovs, uint32_t iovcnt, uint64_t offset, uint64_t length,
		       spdk_file_op_complete cb_fn, void *cb_arg)
{
	SPDK_DEBUGLOG(blobfs, "file=%s offset=%jx length=%jx\n",
		      file->name, offset, length);

	__readvwritev(file, channel, iovs, iovcnt, offset, length, cb_fn, cb_arg, 0);
}

void
spdk_file_read_async(struct spdk_file *file, struct spdk_io_channel *channel,
		     void *payload, uint64_t offset, uint64_t length,
		     spdk_file_op_complete cb_fn, void *cb_arg)
{
	SPDK_DEBUGLOG(blobfs, "file=%s offset=%jx length=%jx\n",
		      file->name, offset, length);
	__readwrite(file, channel, payload, offset, length, cb_fn, cb_arg, 1);
}

void
spdk_file_readv_async(struct spdk_file *file, struct spdk_io_channel *channel,
		      struct iovec *iovs, uint32_t iovcnt, uint64_t offset, uint64_t length,
		      spdk_file_op_complete cb_fn, void *cb_arg)
{
	SPDK_DEBUGLOG(blobfs, "file=%s offset=%jx length=%jx\n",
		      file->name, offset, length);

	__readvwritev(file, channel, iovs, iovcnt, offset, length, cb_fn, cb_arg, 1);
}

struct spdk_io_channel *
spdk_fs_alloc_io_channel(struct spdk_filesystem *fs)
{
	struct spdk_io_channel *io_channel;
	struct spdk_fs_channel *fs_channel;

	io_channel = spdk_get_io_channel(&fs->io_target);
	fs_channel = spdk_io_channel_get_ctx(io_channel);
	fs_channel->bs_channel = spdk_bs_alloc_io_channel(fs->bs);
	fs_channel->send_request = __send_request_direct;

	return io_channel;
}

void
spdk_fs_free_io_channel(struct spdk_io_channel *channel)
{
	spdk_put_io_channel(channel);
}

struct spdk_fs_thread_ctx *
spdk_fs_alloc_thread_ctx(struct spdk_filesystem *fs)
{
	struct spdk_fs_thread_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return NULL;
	}

	if (pthread_spin_init(&ctx->ch.lock, 0)) {
		free(ctx);
		return NULL;
	}

	fs_channel_create(fs, &ctx->ch, 512);

	ctx->ch.send_request = fs->send_request;
	ctx->ch.sync = 1;

	return ctx;
}


void
spdk_fs_free_thread_ctx(struct spdk_fs_thread_ctx *ctx)
{
	assert(ctx->ch.sync == 1);

	while (true) {
		pthread_spin_lock(&ctx->ch.lock);
		if (ctx->ch.outstanding_reqs == 0) {
			pthread_spin_unlock(&ctx->ch.lock);
			break;
		}
		pthread_spin_unlock(&ctx->ch.lock);
		usleep(1000);
	}

	fs_channel_destroy(NULL, &ctx->ch);
	free(ctx);
}

int
spdk_fs_set_cache_size(uint64_t size_in_mb)
{
	/* setting g_fs_cache_size is only permitted if cache pool
	 * is already freed or hasn't been initialized
	 */
	if (g_cache_pool != NULL) {
		return -EPERM;
	}

	g_fs_cache_size = size_in_mb * 1024 * 1024;

	return 0;
}

uint64_t
spdk_fs_get_cache_size(void)
{
	return g_fs_cache_size / (1024 * 1024);
}

static void __file_flush(void *ctx);

/* Try to free some cache buffers from this file.
 */
static int
reclaim_cache_buffers(struct spdk_file *file)
{
	int rc;

	BLOBFS_TRACE(file, "free=%s\n", file->name);

	/* The function is safe to be called with any threads, while the file
	 * lock maybe locked by other thread for now, so try to get the file
	 * lock here.
	 */
	rc = pthread_spin_trylock(&file->lock);
	if (rc != 0) {
		return -1;
	}

	if (file->tree->present_mask == 0) {
		pthread_spin_unlock(&file->lock);
		return -1;
	}
	tree_free_buffers(file->tree);

	TAILQ_REMOVE(&g_caches, file, cache_tailq);
	/* If not freed, put it in the end of the queue */
	if (file->tree->present_mask != 0) {
		TAILQ_INSERT_TAIL(&g_caches, file, cache_tailq);
	} else {
		file->last = NULL;
	}
	pthread_spin_unlock(&file->lock);

	return 0;
}

static int
_blobfs_cache_pool_reclaim(void *arg)
{
	struct spdk_file *file, *tmp;
	int rc;

	if (!blobfs_cache_pool_need_reclaim()) {
		return SPDK_POLLER_IDLE;
	}

	TAILQ_FOREACH_SAFE(file, &g_caches, cache_tailq, tmp) {
		if (!file->open_for_writing &&
		    file->priority == SPDK_FILE_PRIORITY_LOW) {
			rc = reclaim_cache_buffers(file);
			if (rc < 0) {
				continue;
			}
			if (!blobfs_cache_pool_need_reclaim()) {
				return SPDK_POLLER_BUSY;
			}
			break;
		}
	}

	TAILQ_FOREACH_SAFE(file, &g_caches, cache_tailq, tmp) {
		if (!file->open_for_writing) {
			rc = reclaim_cache_buffers(file);
			if (rc < 0) {
				continue;
			}
			if (!blobfs_cache_pool_need_reclaim()) {
				return SPDK_POLLER_BUSY;
			}
			break;
		}
	}

	TAILQ_FOREACH_SAFE(file, &g_caches, cache_tailq, tmp) {
		rc = reclaim_cache_buffers(file);
		if (rc < 0) {
			continue;
		}
		break;
	}

	return SPDK_POLLER_BUSY;
}

static void
_add_file_to_cache_pool(void *ctx)
{
	struct spdk_file *file = ctx;

	TAILQ_INSERT_TAIL(&g_caches, file, cache_tailq);
}

static void
_remove_file_from_cache_pool(void *ctx)
{
	struct spdk_file *file = ctx;

	TAILQ_REMOVE(&g_caches, file, cache_tailq);
}

static struct cache_buffer *
cache_insert_buffer(struct spdk_file *file, uint64_t offset)
{
	struct cache_buffer *buf;
	int count = 0;
	bool need_update = false;

	buf = calloc(1, sizeof(*buf));
	if (buf == NULL) {
		SPDK_DEBUGLOG(blobfs, "calloc failed\n");
		return NULL;
	}

	do {
		buf->buf = spdk_mempool_get(g_cache_pool);
		if (buf->buf) {
			break;
		}
		if (count++ == 100) {
			SPDK_ERRLOG("Could not allocate cache buffer for file=%p on offset=%jx\n",
				    file, offset);
			free(buf);
			return NULL;
		}
		usleep(BLOBFS_CACHE_POOL_POLL_PERIOD_IN_US);
	} while (true);

	buf->buf_size = CACHE_BUFFER_SIZE;
	buf->offset = offset;

	if (file->tree->present_mask == 0) {
		need_update = true;
	}
	file->tree = tree_insert_buffer(file->tree, buf);

	if (need_update) {
		spdk_thread_send_msg(g_cache_pool_thread, _add_file_to_cache_pool, file);
	}

	return buf;
}

static struct cache_buffer *
cache_append_buffer(struct spdk_file *file)
{
	struct cache_buffer *last;

	assert(file->last == NULL || file->last->bytes_filled == file->last->buf_size);
	assert((file->append_pos % CACHE_BUFFER_SIZE) == 0);

	last = cache_insert_buffer(file, file->append_pos);
	if (last == NULL) {
		SPDK_DEBUGLOG(blobfs, "cache_insert_buffer failed\n");
		return NULL;
	}

	file->last = last;

	return last;
}

static void __check_sync_reqs(struct spdk_file *file);

static void
__file_cache_finish_sync(void *ctx, int bserrno)
{
	struct spdk_file *file;
	struct spdk_fs_request *sync_req = ctx;
	struct spdk_fs_cb_args *sync_args;

	sync_args = &sync_req->args;
	file = sync_args->file;
	pthread_spin_lock(&file->lock);
	file->length_xattr = sync_args->op.sync.length;
	assert(sync_args->op.sync.offset <= file->length_flushed);
	spdk_trace_record(TRACE_BLOBFS_XATTR_END, 0, sync_args->op.sync.offset,
			  0, file->name);
	BLOBFS_TRACE(file, "sync done offset=%jx\n", sync_args->op.sync.offset);
	TAILQ_REMOVE(&file->sync_requests, sync_req, args.op.sync.tailq);
	pthread_spin_unlock(&file->lock);

	sync_args->fn.file_op(sync_args->arg, bserrno);

	free_fs_request(sync_req);
	__check_sync_reqs(file);
}

static void
__check_sync_reqs(struct spdk_file *file)
{
	struct spdk_fs_request *sync_req;

	pthread_spin_lock(&file->lock);

	TAILQ_FOREACH(sync_req, &file->sync_requests, args.op.sync.tailq) {
		if (sync_req->args.op.sync.offset <= file->length_flushed) {
			break;
		}
	}

	if (sync_req != NULL && !sync_req->args.op.sync.xattr_in_progress) {
		BLOBFS_TRACE(file, "set xattr length 0x%jx\n", file->length_flushed);
		sync_req->args.op.sync.xattr_in_progress = true;
		sync_req->args.op.sync.length = file->length_flushed;
		spdk_blob_set_xattr(file->blob, "length", &file->length_flushed,
				    sizeof(file->length_flushed));

		pthread_spin_unlock(&file->lock);
		spdk_trace_record(TRACE_BLOBFS_XATTR_START, 0, file->length_flushed,
				  0, file->name);
		spdk_blob_sync_md(file->blob, __file_cache_finish_sync, sync_req);
	} else {
		pthread_spin_unlock(&file->lock);
	}
}

static void
__file_flush_done(void *ctx, int bserrno)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;
	struct spdk_file *file = args->file;
	struct cache_buffer *next = args->op.flush.cache_buffer;

	BLOBFS_TRACE(file, "length=%jx\n", args->op.flush.length);

	pthread_spin_lock(&file->lock);
	next->in_progress = false;
	next->bytes_flushed += args->op.flush.length;
	file->length_flushed += args->op.flush.length;
	if (file->length_flushed > file->length) {
		file->length = file->length_flushed;
	}
	if (next->bytes_flushed == next->buf_size) {
		BLOBFS_TRACE(file, "write buffer fully flushed 0x%jx\n", file->length_flushed);
		next = tree_find_buffer(file->tree, file->length_flushed);
	}

	/*
	 * Assert that there is no cached data that extends past the end of the underlying
	 *  blob.
	 */
	assert(next == NULL || next->offset < __file_get_blob_size(file) ||
	       next->bytes_filled == 0);

	pthread_spin_unlock(&file->lock);

	__check_sync_reqs(file);

	__file_flush(req);
}

static void
__file_flush(void *ctx)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;
	struct spdk_file *file = args->file;
	struct cache_buffer *next;
	uint64_t offset, length, start_lba, num_lba;
	uint32_t lba_size;

	pthread_spin_lock(&file->lock);
	next = tree_find_buffer(file->tree, file->length_flushed);
	if (next == NULL || next->in_progress ||
	    ((next->bytes_filled < next->buf_size) && TAILQ_EMPTY(&file->sync_requests))) {
		/*
		 * There is either no data to flush, a flush I/O is already in
		 *  progress, or the next buffer is partially filled but there's no
		 *  outstanding request to sync it.
		 * So return immediately - if a flush I/O is in progress we will flush
		 *  more data after that is completed, or a partial buffer will get flushed
		 *  when it is either filled or the file is synced.
		 */
		free_fs_request(req);
		if (next == NULL) {
			/*
			 * For cases where a file's cache was evicted, and then the
			 *  file was later appended, we will write the data directly
			 *  to disk and bypass cache.  So just update length_flushed
			 *  here to reflect that all data was already written to disk.
			 */
			file->length_flushed = file->append_pos;
		}
		pthread_spin_unlock(&file->lock);
		if (next == NULL) {
			/*
			 * There is no data to flush, but we still need to check for any
			 *  outstanding sync requests to make sure metadata gets updated.
			 */
			__check_sync_reqs(file);
		}
		return;
	}

	offset = next->offset + next->bytes_flushed;
	length = next->bytes_filled - next->bytes_flushed;
	if (length == 0) {
		free_fs_request(req);
		pthread_spin_unlock(&file->lock);
		/*
		 * There is no data to flush, but we still need to check for any
		 *  outstanding sync requests to make sure metadata gets updated.
		 */
		__check_sync_reqs(file);
		return;
	}
	args->op.flush.length = length;
	args->op.flush.cache_buffer = next;

	__get_page_parameters(file, offset, length, &start_lba, &lba_size, &num_lba);

	next->in_progress = true;
	BLOBFS_TRACE(file, "offset=0x%jx length=0x%jx page start=0x%jx num=0x%jx\n",
		     offset, length, start_lba, num_lba);
	pthread_spin_unlock(&file->lock);
	spdk_blob_io_write(file->blob, file->fs->sync_target.sync_fs_channel->bs_channel,
			   next->buf + (start_lba * lba_size) - next->offset,
			   start_lba, num_lba, __file_flush_done, req);
}

static void
__file_extend_done(void *arg, int bserrno)
{
	struct spdk_fs_cb_args *args = arg;

	__wake_caller(args, bserrno);
}

static void
__file_extend_resize_cb(void *_args, int bserrno)
{
	struct spdk_fs_cb_args *args = _args;
	struct spdk_file *file = args->file;

	if (bserrno) {
		__wake_caller(args, bserrno);
		return;
	}

	spdk_blob_sync_md(file->blob, __file_extend_done, args);
}

static void
__file_extend_blob(void *_args)
{
	struct spdk_fs_cb_args *args = _args;
	struct spdk_file *file = args->file;

	spdk_blob_resize(file->blob, args->op.resize.num_clusters, __file_extend_resize_cb, args);
}

static void
__rw_from_file_done(void *ctx, int bserrno)
{
	struct spdk_fs_request *req = ctx;

	__wake_caller(&req->args, bserrno);
	free_fs_request(req);
}

static void
__rw_from_file(void *ctx)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;
	struct spdk_file *file = args->file;

	if (args->op.rw.is_read) {
		spdk_file_read_async(file, file->fs->sync_target.sync_io_channel, args->iovs[0].iov_base,
				     args->op.rw.offset, (uint64_t)args->iovs[0].iov_len,
				     __rw_from_file_done, req);
	} else {
		spdk_file_write_async(file, file->fs->sync_target.sync_io_channel, args->iovs[0].iov_base,
				      args->op.rw.offset, (uint64_t)args->iovs[0].iov_len,
				      __rw_from_file_done, req);
	}
}

struct rw_from_file_arg {
	struct spdk_fs_channel *channel;
	int rwerrno;
};

static int
__send_rw_from_file(struct spdk_file *file, void *payload,
		    uint64_t offset, uint64_t length, bool is_read,
		    struct rw_from_file_arg *arg)
{
	struct spdk_fs_request *req;
	struct spdk_fs_cb_args *args;

	req = alloc_fs_request_with_iov(arg->channel, 1);
	if (req == NULL) {
		sem_post(&arg->channel->sem);
		return -ENOMEM;
	}

	args = &req->args;
	args->file = file;
	args->sem = &arg->channel->sem;
	args->iovs[0].iov_base = payload;
	args->iovs[0].iov_len = (size_t)length;
	args->op.rw.offset = offset;
	args->op.rw.is_read = is_read;
	args->rwerrno = &arg->rwerrno;
	file->fs->send_request(__rw_from_file, req);
	return 0;
}

int
spdk_file_write(struct spdk_file *file, struct spdk_fs_thread_ctx *ctx,
		void *payload, uint64_t offset, uint64_t length)
{
	struct spdk_fs_channel *channel = (struct spdk_fs_channel *)ctx;
	struct spdk_fs_request *flush_req;
	uint64_t rem_length, copy, blob_size, cluster_sz;
	uint32_t cache_buffers_filled = 0;
	uint8_t *cur_payload;
	struct cache_buffer *last;

	BLOBFS_TRACE_RW(file, "offset=%jx length=%jx\n", offset, length);

	if (length == 0) {
		return 0;
	}

	if (offset != file->append_pos) {
		BLOBFS_TRACE(file, " error offset=%jx append_pos=%jx\n", offset, file->append_pos);
		return -EINVAL;
	}

	pthread_spin_lock(&file->lock);
	file->open_for_writing = true;

	if ((file->last == NULL) && (file->append_pos % CACHE_BUFFER_SIZE == 0)) {
		cache_append_buffer(file);
	}

	if (file->last == NULL) {
		struct rw_from_file_arg arg = {};
		int rc;

		arg.channel = channel;
		arg.rwerrno = 0;
		file->append_pos += length;
		pthread_spin_unlock(&file->lock);
		rc = __send_rw_from_file(file, payload, offset, length, false, &arg);
		if (rc != 0) {
			return rc;
		}
		sem_wait(&channel->sem);
		return arg.rwerrno;
	}

	blob_size = __file_get_blob_size(file);

	if ((offset + length) > blob_size) {
		struct spdk_fs_cb_args extend_args = {};

		cluster_sz = file->fs->bs_opts.cluster_sz;
		extend_args.sem = &channel->sem;
		extend_args.op.resize.num_clusters = __bytes_to_clusters((offset + length), cluster_sz);
		extend_args.file = file;
		BLOBFS_TRACE(file, "start resize to %u clusters\n", extend_args.op.resize.num_clusters);
		pthread_spin_unlock(&file->lock);
		file->fs->send_request(__file_extend_blob, &extend_args);
		sem_wait(&channel->sem);
		if (extend_args.rc) {
			return extend_args.rc;
		}
	}

	flush_req = alloc_fs_request(channel);
	if (flush_req == NULL) {
		pthread_spin_unlock(&file->lock);
		return -ENOMEM;
	}

	last = file->last;
	rem_length = length;
	cur_payload = payload;
	while (rem_length > 0) {
		copy = last->buf_size - last->bytes_filled;
		if (copy > rem_length) {
			copy = rem_length;
		}
		BLOBFS_TRACE_RW(file, "  fill offset=%jx length=%jx\n", file->append_pos, copy);
		memcpy(&last->buf[last->bytes_filled], cur_payload, copy);
		file->append_pos += copy;
		if (file->length < file->append_pos) {
			file->length = file->append_pos;
		}
		cur_payload += copy;
		last->bytes_filled += copy;
		rem_length -= copy;
		if (last->bytes_filled == last->buf_size) {
			cache_buffers_filled++;
			last = cache_append_buffer(file);
			if (last == NULL) {
				BLOBFS_TRACE(file, "nomem\n");
				free_fs_request(flush_req);
				pthread_spin_unlock(&file->lock);
				return -ENOMEM;
			}
		}
	}

	pthread_spin_unlock(&file->lock);

	if (cache_buffers_filled == 0) {
		free_fs_request(flush_req);
		return 0;
	}

	flush_req->args.file = file;
	file->fs->send_request(__file_flush, flush_req);
	return 0;
}

static void
__readahead_done(void *ctx, int bserrno)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;
	struct cache_buffer *cache_buffer = args->op.readahead.cache_buffer;
	struct spdk_file *file = args->file;

	BLOBFS_TRACE(file, "offset=%jx\n", cache_buffer->offset);

	pthread_spin_lock(&file->lock);
	cache_buffer->bytes_filled = args->op.readahead.length;
	cache_buffer->bytes_flushed = args->op.readahead.length;
	cache_buffer->in_progress = false;
	pthread_spin_unlock(&file->lock);

	free_fs_request(req);
}

static void
__readahead(void *ctx)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;
	struct spdk_file *file = args->file;
	uint64_t offset, length, start_lba, num_lba;
	uint32_t lba_size;

	offset = args->op.readahead.offset;
	length = args->op.readahead.length;
	assert(length > 0);

	__get_page_parameters(file, offset, length, &start_lba, &lba_size, &num_lba);

	BLOBFS_TRACE(file, "offset=%jx length=%jx page start=%jx num=%jx\n",
		     offset, length, start_lba, num_lba);
	spdk_blob_io_read(file->blob, file->fs->sync_target.sync_fs_channel->bs_channel,
			  args->op.readahead.cache_buffer->buf,
			  start_lba, num_lba, __readahead_done, req);
}

static uint64_t
__next_cache_buffer_offset(uint64_t offset)
{
	return (offset + CACHE_BUFFER_SIZE) & ~(CACHE_TREE_LEVEL_MASK(0));
}

static void
check_readahead(struct spdk_file *file, uint64_t offset,
		struct spdk_fs_channel *channel)
{
	struct spdk_fs_request *req;
	struct spdk_fs_cb_args *args;

	offset = __next_cache_buffer_offset(offset);
	if (tree_find_buffer(file->tree, offset) != NULL || file->length <= offset) {
		return;
	}

	req = alloc_fs_request(channel);
	if (req == NULL) {
		return;
	}
	args = &req->args;

	BLOBFS_TRACE(file, "offset=%jx\n", offset);

	args->file = file;
	args->op.readahead.offset = offset;
	args->op.readahead.cache_buffer = cache_insert_buffer(file, offset);
	if (!args->op.readahead.cache_buffer) {
		BLOBFS_TRACE(file, "Cannot allocate buf for offset=%jx\n", offset);
		free_fs_request(req);
		return;
	}

	args->op.readahead.cache_buffer->in_progress = true;
	if (file->length < (offset + CACHE_BUFFER_SIZE)) {
		args->op.readahead.length = file->length & (CACHE_BUFFER_SIZE - 1);
	} else {
		args->op.readahead.length = CACHE_BUFFER_SIZE;
	}
	file->fs->send_request(__readahead, req);
}

int64_t
spdk_file_read(struct spdk_file *file, struct spdk_fs_thread_ctx *ctx,
	       void *payload, uint64_t offset, uint64_t length)
{
	struct spdk_fs_channel *channel = (struct spdk_fs_channel *)ctx;
	uint64_t final_offset, final_length;
	uint32_t sub_reads = 0;
	struct cache_buffer *buf;
	uint64_t read_len;
	struct rw_from_file_arg arg = {};

	pthread_spin_lock(&file->lock);

	BLOBFS_TRACE_RW(file, "offset=%ju length=%ju\n", offset, length);

	file->open_for_writing = false;

	if (length == 0 || offset >= file->append_pos) {
		pthread_spin_unlock(&file->lock);
		return 0;
	}

	if (offset + length > file->append_pos) {
		length = file->append_pos - offset;
	}

	if (offset != file->next_seq_offset) {
		file->seq_byte_count = 0;
	}
	file->seq_byte_count += length;
	file->next_seq_offset = offset + length;
	if (file->seq_byte_count >= CACHE_READAHEAD_THRESHOLD) {
		check_readahead(file, offset, channel);
		check_readahead(file, offset + CACHE_BUFFER_SIZE, channel);
	}

	arg.channel = channel;
	arg.rwerrno = 0;
	final_length = 0;
	final_offset = offset + length;
	while (offset < final_offset) {
		int ret = 0;
		length = NEXT_CACHE_BUFFER_OFFSET(offset) - offset;
		if (length > (final_offset - offset)) {
			length = final_offset - offset;
		}

		buf = tree_find_filled_buffer(file->tree, offset);
		if (buf == NULL) {
			pthread_spin_unlock(&file->lock);
			ret = __send_rw_from_file(file, payload, offset, length, true, &arg);
			pthread_spin_lock(&file->lock);
			if (ret == 0) {
				sub_reads++;
			}
		} else {
			read_len = length;
			if ((offset + length) > (buf->offset + buf->bytes_filled)) {
				read_len = buf->offset + buf->bytes_filled - offset;
			}
			BLOBFS_TRACE(file, "read %p offset=%ju length=%ju\n", payload, offset, read_len);
			memcpy(payload, &buf->buf[offset - buf->offset], read_len);
			if ((offset + read_len) % CACHE_BUFFER_SIZE == 0) {
				tree_remove_buffer(file->tree, buf);
				if (file->tree->present_mask == 0) {
					spdk_thread_send_msg(g_cache_pool_thread, _remove_file_from_cache_pool, file);
				}
			}
		}

		if (ret == 0) {
			final_length += length;
		} else {
			arg.rwerrno = ret;
			break;
		}
		payload += length;
		offset += length;
	}
	pthread_spin_unlock(&file->lock);
	while (sub_reads > 0) {
		sem_wait(&channel->sem);
		sub_reads--;
	}
	if (arg.rwerrno == 0) {
		return final_length;
	} else {
		return arg.rwerrno;
	}
}

static void
_file_sync(struct spdk_file *file, struct spdk_fs_channel *channel,
	   spdk_file_op_complete cb_fn, void *cb_arg)
{
	struct spdk_fs_request *sync_req;
	struct spdk_fs_request *flush_req;
	struct spdk_fs_cb_args *sync_args;
	struct spdk_fs_cb_args *flush_args;

	BLOBFS_TRACE(file, "offset=%jx\n", file->append_pos);

	pthread_spin_lock(&file->lock);
	if (file->append_pos <= file->length_xattr) {
		BLOBFS_TRACE(file, "done - file already synced\n");
		pthread_spin_unlock(&file->lock);
		cb_fn(cb_arg, 0);
		return;
	}

	sync_req = alloc_fs_request(channel);
	if (!sync_req) {
		SPDK_ERRLOG("Cannot allocate sync req for file=%s\n", file->name);
		pthread_spin_unlock(&file->lock);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	sync_args = &sync_req->args;

	flush_req = alloc_fs_request(channel);
	if (!flush_req) {
		SPDK_ERRLOG("Cannot allocate flush req for file=%s\n", file->name);
		free_fs_request(sync_req);
		pthread_spin_unlock(&file->lock);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	flush_args = &flush_req->args;

	sync_args->file = file;
	sync_args->fn.file_op = cb_fn;
	sync_args->arg = cb_arg;
	sync_args->op.sync.offset = file->append_pos;
	sync_args->op.sync.xattr_in_progress = false;
	TAILQ_INSERT_TAIL(&file->sync_requests, sync_req, args.op.sync.tailq);
	pthread_spin_unlock(&file->lock);

	flush_args->file = file;
	channel->send_request(__file_flush, flush_req);
}

int
spdk_file_sync(struct spdk_file *file, struct spdk_fs_thread_ctx *ctx)
{
	struct spdk_fs_channel *channel = (struct spdk_fs_channel *)ctx;
	struct spdk_fs_cb_args args = {};

	args.sem = &channel->sem;
	_file_sync(file, channel, __wake_caller, &args);
	sem_wait(&channel->sem);

	return args.rc;
}

void
spdk_file_sync_async(struct spdk_file *file, struct spdk_io_channel *_channel,
		     spdk_file_op_complete cb_fn, void *cb_arg)
{
	struct spdk_fs_channel *channel = spdk_io_channel_get_ctx(_channel);

	_file_sync(file, channel, cb_fn, cb_arg);
}

void
spdk_file_set_priority(struct spdk_file *file, uint32_t priority)
{
	BLOBFS_TRACE(file, "priority=%u\n", priority);
	file->priority = priority;

}

/*
 * Close routines
 */

static void
__file_close_async_done(void *ctx, int bserrno)
{
	struct spdk_fs_request *req = ctx;
	struct spdk_fs_cb_args *args = &req->args;
	struct spdk_file *file = args->file;

	spdk_trace_record(TRACE_BLOBFS_CLOSE, 0, 0, 0, file->name);

	if (file->is_deleted) {
		spdk_fs_delete_file_async(file->fs, file->name, blob_delete_cb, ctx);
		return;
	}

	args->fn.file_op(args->arg, bserrno);
	free_fs_request(req);
}

static void
__file_close_async(struct spdk_file *file, struct spdk_fs_request *req)
{
	struct spdk_blob *blob;

	pthread_spin_lock(&file->lock);
	if (file->ref_count == 0) {
		pthread_spin_unlock(&file->lock);
		__file_close_async_done(req, -EBADF);
		return;
	}

	file->ref_count--;
	if (file->ref_count > 0) {
		pthread_spin_unlock(&file->lock);
		req->args.fn.file_op(req->args.arg, 0);
		free_fs_request(req);
		return;
	}

	pthread_spin_unlock(&file->lock);

	blob = file->blob;
	file->blob = NULL;
	spdk_blob_close(blob, __file_close_async_done, req);
}

static void
__file_close_async__sync_done(void *arg, int fserrno)
{
	struct spdk_fs_request *req = arg;
	struct spdk_fs_cb_args *args = &req->args;

	__file_close_async(args->file, req);
}

void
spdk_file_close_async(struct spdk_file *file, spdk_file_op_complete cb_fn, void *cb_arg)
{
	struct spdk_fs_request *req;
	struct spdk_fs_cb_args *args;

	req = alloc_fs_request(file->fs->md_target.md_fs_channel);
	if (req == NULL) {
		SPDK_ERRLOG("Cannot allocate close async req for file=%s\n", file->name);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	args = &req->args;
	args->file = file;
	args->fn.file_op = cb_fn;
	args->arg = cb_arg;

	spdk_file_sync_async(file, file->fs->md_target.md_io_channel, __file_close_async__sync_done, req);
}

static void
__file_close(void *arg)
{
	struct spdk_fs_request *req = arg;
	struct spdk_fs_cb_args *args = &req->args;
	struct spdk_file *file = args->file;

	__file_close_async(file, req);
}

int
spdk_file_close(struct spdk_file *file, struct spdk_fs_thread_ctx *ctx)
{
	struct spdk_fs_channel *channel = (struct spdk_fs_channel *)ctx;
	struct spdk_fs_request *req;
	struct spdk_fs_cb_args *args;

	req = alloc_fs_request(channel);
	if (req == NULL) {
		SPDK_ERRLOG("Cannot allocate close req for file=%s\n", file->name);
		return -ENOMEM;
	}

	args = &req->args;

	spdk_file_sync(file, ctx);
	BLOBFS_TRACE(file, "name=%s\n", file->name);
	args->file = file;
	args->sem = &channel->sem;
	args->fn.file_op = __wake_caller;
	args->arg = args;
	channel->send_request(__file_close, req);
	sem_wait(&channel->sem);

	return args->rc;
}

int
spdk_file_get_id(struct spdk_file *file, void *id, size_t size)
{
	if (size < sizeof(spdk_blob_id)) {
		return -EINVAL;
	}

	memcpy(id, &file->blobid, sizeof(spdk_blob_id));

	return sizeof(spdk_blob_id);
}

static void
_file_free(void *ctx)
{
	struct spdk_file *file = ctx;

	TAILQ_REMOVE(&g_caches, file, cache_tailq);

	free(file->name);
	free(file->tree);
	free(file);
}

static void
file_free(struct spdk_file *file)
{
	BLOBFS_TRACE(file, "free=%s\n", file->name);
	pthread_spin_lock(&file->lock);
	if (file->tree->present_mask == 0) {
		pthread_spin_unlock(&file->lock);
		free(file->name);
		free(file->tree);
		free(file);
		return;
	}

	tree_free_buffers(file->tree);
	assert(file->tree->present_mask == 0);
	spdk_thread_send_msg(g_cache_pool_thread, _file_free, file);
	pthread_spin_unlock(&file->lock);
}

SPDK_LOG_REGISTER_COMPONENT(blobfs)
SPDK_LOG_REGISTER_COMPONENT(blobfs_rw)
