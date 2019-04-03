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

#include "vbdev_compress.h"

#include "spdk/reduce.h"
#include "spdk/stdinc.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"
#include "spdk/bdev_module.h"

#include "spdk_internal/log.h"

#include <rte_config.h>
#include <rte_bus_vdev.h>
#include <rte_compressdev.h>
#include <rte_comp.h>

/* TODO: valdiate these are good starting values */
#define NUM_MAX_XFORMS 16
#define NUM_MAX_INFLIGHT_OPS 512
#define DEFAULT_WINDOW_SIZE 15
#define MAX_MBUFS_PER_OP 16
#define CHUNK_SIZE (1024 * 16)

#define COMP_BDEV_NAME "compress"

/* TODO: need to get this from RPC on create or reduce metadata on load */
#define TEST_MD_PATH "/tmp"
#define DEV_CHUNK_SZ (16 * 1024)
#define DEV_LBA_SZ 512
#define DEV_BACKING_IO_SZ (4 * 1024)

/* To add support for new device types, follow the examples of the following...
 * Note that the string names are defined by the DPDK PMD in question so be
 * sure to use the exact names.
 */
#define MAX_NUM_DRV_TYPES 1
#define ISAL_PMD "compress_isal"
/* TODO: #define QAT_PMD "tbd" */
const char *g_drv_names[MAX_NUM_DRV_TYPES] = { ISAL_PMD };

#define NUM_MBUFS		512
#define POOL_CACHE_SIZE		256

/* Global list of available compression devices. */
struct compress_dev {
	struct rte_compressdev_info	cdev_info;	/* includes device friendly name */
	uint8_t				cdev_id;	/* identifier for the device */
	void				*comp_xform;	/* shared private xform for comp on this PMD */
	void				*decomp_xform;	/* shared private xform for decomp on this PMD */
	TAILQ_ENTRY(compress_dev)	link;
};
static TAILQ_HEAD(, compress_dev) g_compress_devs = TAILQ_HEAD_INITIALIZER(g_compress_devs);

/* Global list and lock for unique device/queue pair combos */
struct comp_device_qp {
	struct compress_dev		*device;	/* ptr to compression device */
	uint8_t				qp;		/* queue pair for this node */
	bool				in_use;		/* whether this node is in use or not */
	TAILQ_ENTRY(comp_device_qp)	link;
};
static TAILQ_HEAD(, comp_device_qp) g_comp_device_qp = TAILQ_HEAD_INITIALIZER(g_comp_device_qp);
static pthread_mutex_t g_comp_device_qp_lock = PTHREAD_MUTEX_INITIALIZER;

/* List of virtual bdevs and associated info for each. */
struct vbdev_compress {
	struct spdk_bdev		*base_bdev;	/* the thing we're attaching to */
	struct spdk_bdev_desc		*base_desc;	/* its descriptor we get from open */
	struct spdk_io_channel		*base_ch;	/* IO channel of base device */
	struct spdk_bdev		comp_bdev;	/* the compression virtual bdev */
	struct comp_io_channel		*comp_ch;	/* channel associated with this bdev */
	char				*drv_name;	/* name of the compression device driver */
	struct comp_device_qp		*device_qp;
	struct spdk_thread		*reduce_thread;
	pthread_mutex_t			reduce_lock;
	uint32_t			ch_count;
	TAILQ_HEAD(, spdk_bdev_io)	pending_comp_ios;	/* outstanding operations to a comp library */
	struct spdk_poller		*poller;	/* completion poller */
	struct spdk_reduce_vol_params	params;		/* params for the reduce volume */
	struct spdk_reduce_backing_dev	backing_dev;	/* backing device info for the reduce volume */
	struct spdk_reduce_vol		*vol;		/* the reduce volume */
	spdk_delete_compress_complete	delete_cb_fn;
	void				*delete_cb_arg;
	TAILQ_ENTRY(vbdev_compress)	link;
};
static TAILQ_HEAD(, vbdev_compress) g_vbdev_comp = TAILQ_HEAD_INITIALIZER(g_vbdev_comp);

/* The comp vbdev channel struct. It is allocated and freed on my behalf by the io channel code.
 */
struct comp_io_channel {
	struct spdk_io_channel_iter	*iter;	/* used with for_each_channel in reset */
};

/* Per I/O context for the compression vbdev. */
struct comp_bdev_io {
	struct comp_io_channel		*comp_ch;		/* used in completion handling */
	struct vbdev_compress		*comp_bdev;		/* vbdev associated with this IO */
	struct spdk_bdev_io_wait_entry	bdev_io_wait;		/* for bdev_io_wait */
	struct spdk_bdev_io		*orig_io;		/* the original IO */
	struct spdk_io_channel		*ch;			/* for resubmission */
};

/* Shared mempools between all devices on this system */
static struct spdk_mempool *g_mbuf_mp = NULL;			/* mbuf mempool */
static struct rte_mempool *g_comp_op_mp = NULL;			/* comp operations, must be rte* mempool */

static void vbdev_compress_examine(struct spdk_bdev *bdev);
static void vbdev_compress_claim(struct vbdev_compress *comp_bdev);
static void vbdev_compress_queue_io(struct spdk_bdev_io *bdev_io);
struct vbdev_compress *_prepare_for_load_init(struct spdk_bdev *bdev);
static void vbdev_compress_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);
static void comp_bdev_ch_destroy_cb(void *io_device, void *ctx_buf);

/* Called by vbdev_init_compress_drivers() to init each discovered compression device */
static int
create_compress_dev(uint8_t index, uint16_t num_lcores)
{
	struct compress_dev *device;
	uint16_t q_pairs = num_lcores;
	uint8_t cdev_id;
	struct rte_comp_xform comp_xform = {};
	struct rte_comp_xform decomp_xform = {};
	int rc, i;
	struct comp_device_qp *dev_qp;
	struct comp_device_qp *tmp_qp;

	device = calloc(1, sizeof(struct compress_dev));
	if (!device) {
		return -ENOMEM;
	}

	/* Get details about this device. */
	rte_compressdev_info_get(index, &device->cdev_info);

	cdev_id = device->cdev_id = index;

	/* Zero means no limit so choose number of lcores. */
	if (device->cdev_info.max_nb_queue_pairs == 0) {
		q_pairs = num_lcores;
	} else {
		q_pairs = spdk_min(device->cdev_info.max_nb_queue_pairs, num_lcores);
	}

	/* Configure the compression device. */
	struct rte_compressdev_config config = {
		.socket_id = rte_socket_id(),
		.nb_queue_pairs = q_pairs,
		.max_nb_priv_xforms = NUM_MAX_XFORMS,
		.max_nb_streams = 0
	};
	rc = rte_compressdev_configure(cdev_id, &config);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to configure compressdev %u\n", cdev_id);
		goto err;
	}

	/* Pre-setup all potential qpairs now and assign them in the channel
	 * callback.
	 */
	for (i = 0; i < q_pairs; i++) {
		rc = rte_compressdev_queue_pair_setup(cdev_id, i,
						      NUM_MAX_INFLIGHT_OPS,
						      rte_socket_id());
		if (rc) {
			SPDK_ERRLOG("Failed to setup queue pair on "
				    "compressdev %u\n", cdev_id);
			rc = -EINVAL;
			goto err;
		}
	}

	rc = rte_compressdev_start(cdev_id);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to start device %u: error %d\n",
			    cdev_id, rc);
		goto err;
	}

	/* TODO: if later on all elements remain static, move these
	 * xform structs to static globals.
	 */

	/* Create shrared (between all ops per PMD) compress xforms. */
	comp_xform = (struct rte_comp_xform) {
		.type = RTE_COMP_COMPRESS,
		.compress = {
			.algo = RTE_COMP_ALGO_DEFLATE,
			.deflate.huffman = RTE_COMP_HUFFMAN_DEFAULT,
			.level = RTE_COMP_LEVEL_MAX,
			.window_size = DEFAULT_WINDOW_SIZE,
			.chksum = RTE_COMP_CHECKSUM_NONE,
			.hash_algo = RTE_COMP_HASH_ALGO_NONE
		}
	};
	rc = rte_compressdev_private_xform_create(cdev_id, &comp_xform,
			&device->comp_xform);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to create private comp xform device %u: error %d\n",
			    cdev_id, rc);
		goto err;
	}

	/* Create shrared (between all ops per PMD) decompress xforms. *
	 * also TODO make this global static if everything stays like this
	 */
	decomp_xform = (struct rte_comp_xform) {
		.type = RTE_COMP_DECOMPRESS,
		.decompress = {
			.algo = RTE_COMP_ALGO_DEFLATE,
			.chksum = RTE_COMP_CHECKSUM_NONE,
			.window_size = DEFAULT_WINDOW_SIZE,
			.hash_algo = RTE_COMP_HASH_ALGO_NONE
		}
	};
	rc = rte_compressdev_private_xform_create(cdev_id, &decomp_xform,
			&device->decomp_xform);
	if (rc) {
		SPDK_ERRLOG("Failed to create private decomp xform device %u: error %d\n",
			    cdev_id, rc);
		goto err;
	}

	/* Build up list of device/qp combinations */
	for (i = 0; i < q_pairs; i++) {
		dev_qp = calloc(1, sizeof(struct comp_device_qp));
		if (!dev_qp) {
			rc = -ENOMEM;
			goto err;
		}
		dev_qp->device = device;
		dev_qp->qp = i;
		dev_qp->in_use = false;
		TAILQ_INSERT_TAIL(&g_comp_device_qp, dev_qp, link);
	}

	TAILQ_INSERT_TAIL(&g_compress_devs, device, link);

	return 0;

err:
	TAILQ_FOREACH_SAFE(dev_qp, &g_comp_device_qp, link, tmp_qp) {
		TAILQ_REMOVE(&g_comp_device_qp, dev_qp, link);
		free(dev_qp);
	}
	free(device);
	return rc;
}

/* Called from driver init entry point, vbdev_compress_init() */
static int
vbdev_init_compress_drivers(void)
{
	uint8_t cdev_count, i;
	uint16_t num_lcores = rte_lcore_count();
	struct compress_dev *tmp_dev;
	struct compress_dev *device;
	int rc;

	/* We always init the compress_isal PMD */
	rc = rte_vdev_init(ISAL_PMD, NULL);
	if (rc == 0) {
		SPDK_NOTICELOG("created virtual PMD %s\n", ISAL_PMD);
	} else if (rc == -EEXIST) {
		SPDK_NOTICELOG("virtual PMD %s already exists.\n", ISAL_PMD);
	} else {
		SPDK_ERRLOG("creating virtual PMD %s\n", ISAL_PMD);
		return -EINVAL;
	}

	/* If we have no compression devices, there's no reason to continue. */
	cdev_count = rte_compressdev_count();
	if (cdev_count == 0) {
		return 0;
	}
	if (cdev_count > RTE_COMPRESS_MAX_DEVS) {
		SPDK_ERRLOG("invalid device count from rte_compressdev_count()\n");
		return -EINVAL;
	}

	g_mbuf_mp = spdk_mempool_create("comp_mbuf_mp", NUM_MBUFS, sizeof(struct rte_mbuf),
					SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
					SPDK_ENV_SOCKET_ID_ANY);
	if (g_mbuf_mp == NULL) {
		SPDK_ERRLOG("Cannot create mbuf pool\n");
		rc = -ENOMEM;
		goto error_create_mbuf;
	}

	g_comp_op_mp = rte_comp_op_pool_create("comp_op_pool", NUM_MBUFS, POOL_CACHE_SIZE,
					       0, rte_socket_id());
	if (g_comp_op_mp == NULL) {
		SPDK_ERRLOG("Cannot create comp op pool\n");
		rc = -ENOMEM;
		goto error_create_op;
	}

	/* Init all devices */
	for (i = 0; i < cdev_count; i++) {
		rc = create_compress_dev(i, num_lcores);
		if (rc != 0) {
			goto error_create_compress_devs;
		}
	}

	return 0;

	/* Error cleanup paths. */
error_create_compress_devs:
	TAILQ_FOREACH_SAFE(device, &g_compress_devs, link, tmp_dev) {
		TAILQ_REMOVE(&g_compress_devs, device, link);
		free(device);
	}
error_create_op:
error_create_mbuf:
	spdk_mempool_free(g_mbuf_mp);

	return rc;
}

/* Poller for the DPDK compression driver. */
static int
comp_dev_poller(void *args)
{
	struct vbdev_compress *comp_bdev = args;
	uint8_t cdev_id = comp_bdev->device_qp->device->cdev_id;
	struct rte_comp_op *deq_ops;
	uint16_t num_deq;
	struct spdk_reduce_vol_cb_args *reduce_args;

	num_deq = rte_compressdev_dequeue_burst(cdev_id, 0, &deq_ops, 1);

	if (num_deq > 0) {
		reduce_args = (struct spdk_reduce_vol_cb_args *)deq_ops->m_src->userdata;

		if (deq_ops->status != RTE_COMP_OP_STATUS_SUCCESS) {
			SPDK_ERRLOG("deque status %u\n", deq_ops->status);

			/* TODO update produced with translated -errno */
			/*
			 * RTE_COMP_OP_STATUS_SUCCESS = 0,
			 * RTE_COMP_OP_STATUS_NOT_PROCESSED,
			 * RTE_COMP_OP_STATUS_INVALID_ARGS,
			 * RTE_COMP_OP_STATUS_ERROR,
			 * RTE_COMP_OP_STATUS_INVALID_STATE,
			 * RTE_COMP_OP_STATUS_OUT_OF_SPACE_TERMINATED,
			 * RTE_COMP_OP_STATUS_OUT_OF_SPACE_RECOVERABLE,
			 */
		}
		reduce_args->cb_fn(reduce_args->cb_arg, deq_ops->produced);

		/* Now bulk free both mbufs and the compress operation. */
		spdk_mempool_put(g_mbuf_mp, deq_ops->m_src);
		spdk_mempool_put(g_mbuf_mp, deq_ops->m_dst);
		rte_comp_op_free(deq_ops);
	}
	return 0;
}

/* Completion callback for r/w that were issued via reducelib. */
static void
spdk_reduce_rw_blocks_cb(void *arg, int reduce_errno)
{
	struct spdk_bdev_io *bdev_io = arg;

	/* TODO: need to decide which error codes are bdev_io success vs failure;
	 * example examine calls reading metadata */
	if (reduce_errno == 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		SPDK_ERRLOG("ERROR %d on operation from reduce API\n", reduce_errno);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static int
_compress_operation(struct spdk_reduce_backing_dev *backing_dev, struct iovec *src_iovs,
		    int src_iovcnt, struct iovec *dst_iovs,
		    int dst_iovcnt, bool compress, void *cb_arg)
{
	void *reduce_cb_arg = cb_arg;
	struct vbdev_compress *comp_bdev = SPDK_CONTAINEROF(backing_dev, struct vbdev_compress,
					   backing_dev);
	struct rte_comp_op *comp_op;
	struct rte_mbuf *src_mbufs[MAX_MBUFS_PER_OP];
	struct rte_mbuf *dst_mbufs[MAX_MBUFS_PER_OP];
	uint8_t cdev_id = comp_bdev->device_qp->device->cdev_id;
	uint64_t total_length = 0;
	struct iovec *current_src_iov = NULL;
	struct iovec *current_dst_iov = NULL;
	int iov_index;
	int rc = 0;
	struct rte_mbuf_ext_shared_info shinfo_src;
	struct rte_mbuf_ext_shared_info shinfo_dst;

	assert(src_iovcnt < MAX_MBUFS_PER_OP);
	comp_op = rte_comp_op_alloc(g_comp_op_mp);
	if (!comp_op) {
		SPDK_ERRLOG("trying to get a comp op!\n");
		return -ENOMEM;
	}

	/* get an mbuf per iov, src and dst */
	rc = spdk_mempool_get_bulk(g_mbuf_mp, (void **)&src_mbufs[0], src_iovcnt);
	if (rc) {
		SPDK_ERRLOG("trying to get src_mbufs!\n");
		rc = -ENOMEM;
		goto error_get_src;

	}
	rc = spdk_mempool_get_bulk(g_mbuf_mp, (void **)&dst_mbufs[0], dst_iovcnt);
	if (rc) {
		SPDK_ERRLOG("trying to get dst_mbufs!\n");
		rc = -ENOMEM;
		goto error_get_dst;
	}

	/* There is a 1:1 mapping between a bdev_io and a compression operation, but
	 * all compression PMDs that SPDK uses support chaining so build our mbuf chain
	 * and associate with our single comp_op.
	 */

	/* Setup src mbufs */
	for (iov_index = 0; iov_index < src_iovcnt; iov_index++) {

		current_src_iov = src_iovs[iov_index].iov_base;
		total_length += src_iovs[iov_index].iov_len;
		src_mbufs[iov_index]->userdata = reduce_cb_arg;

		rte_pktmbuf_attach_extbuf(src_mbufs[iov_index],
					  current_src_iov,
					  spdk_vtophys((void *)current_src_iov, NULL),
					  src_iovs[iov_index].iov_len,
					  &shinfo_src);
		rte_pktmbuf_append(src_mbufs[iov_index], src_iovs[iov_index].iov_len);

		if (iov_index > 0) {
			rte_pktmbuf_chain(src_mbufs[0], src_mbufs[iov_index]);
		}
	}

	comp_op->m_src = src_mbufs[0];
	comp_op->src.offset = 0;
	comp_op->src.length = total_length;

	/* setup dst mbufs, for the current test being used with this code there's only one vector */
	for (iov_index = 0; iov_index < dst_iovcnt; iov_index++) {

		current_dst_iov = dst_iovs[iov_index].iov_base;

		rte_pktmbuf_attach_extbuf(dst_mbufs[iov_index],
					  current_dst_iov,
					  spdk_vtophys((void *)current_dst_iov, NULL),
					  dst_iovs[iov_index].iov_len,
					  &shinfo_dst);
		rte_pktmbuf_append(dst_mbufs[iov_index], dst_iovs[iov_index].iov_len);

		if (iov_index > 0) {
			rte_pktmbuf_chain(dst_mbufs[0], dst_mbufs[iov_index]);
		}
	}
	comp_op->m_dst = dst_mbufs[0];
	comp_op->dst.offset = 0;

	if (compress == true) {
		comp_op->private_xform = comp_bdev->device_qp->device->comp_xform;
	} else {
		comp_op->private_xform = comp_bdev->device_qp->device->decomp_xform;
	}

	rte_compressdev_enqueue_burst(cdev_id, 0, &comp_op, 1);
	return rc;

	/* Error cleanup paths. */
error_get_dst:
	spdk_mempool_put_bulk(g_mbuf_mp, (void **)&src_mbufs[0], src_iovcnt);
error_get_src:
	rte_comp_op_free(comp_op);
	return rc;
}

/* Entry point for reduce lib to issue a compress operation. */
static void
_comp_reduce_compress(struct spdk_reduce_backing_dev *dev,
		      struct iovec *src_iovs, int src_iovcnt,
		      struct iovec *dst_iovs, int dst_iovcnt,
		      struct spdk_reduce_vol_cb_args *cb_arg)
{
	_compress_operation(dev, src_iovs, src_iovcnt, dst_iovs, dst_iovcnt, true, cb_arg);
}

/* Entry point for reduce lib to issue a decompress operation. */
static void
_comp_reduce_decompress(struct spdk_reduce_backing_dev *dev,
			struct iovec *src_iovs, int src_iovcnt,
			struct iovec *dst_iovs, int dst_iovcnt,
			struct spdk_reduce_vol_cb_args *cb_arg)
{
	_compress_operation(dev, src_iovs, src_iovcnt, dst_iovs, dst_iovcnt, false, cb_arg);
}

/* Callback for getting a buf from the bdev pool in the event that the caller passed
 * in NULL, we need to own the buffer so it doesn't get freed by another vbdev module
 * beneath us before we're done with it.
 */
static void
comp_read_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	struct vbdev_compress *comp_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_compress,
					   comp_bdev);

	spdk_reduce_vol_readv(comp_bdev->vol, bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
			      bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
			      spdk_reduce_rw_blocks_cb, bdev_io);
}

static void
_spdk_bdev_io_submit(void *arg)
{
	struct comp_bdev_io *io_ctx = arg;

	vbdev_compress_submit_request(spdk_io_channel_from_ctx(io_ctx->comp_ch), io_ctx->orig_io);
}

/* Called when someone above submits IO to this vbdev. */
static void
vbdev_compress_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_compress *comp_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_compress,
					   comp_bdev);
	struct comp_io_channel *comp_ch = spdk_io_channel_get_ctx(ch);
	struct comp_bdev_io *io_ctx = (struct comp_bdev_io *)bdev_io->driver_ctx;
	int rc = 0;

	memset(io_ctx, 0, sizeof(struct comp_bdev_io));
	io_ctx->comp_bdev = comp_bdev;
	io_ctx->comp_ch = comp_ch;
	io_ctx->orig_io = bdev_io;

	/* Send this request to the reduce_thread. */
	if (spdk_io_channel_get_thread(spdk_bdev_io_get_io_channel(bdev_io))
	    != comp_bdev->reduce_thread) {
		spdk_thread_send_msg(comp_bdev->reduce_thread, _spdk_bdev_io_submit, io_ctx);
		return;
	}

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, comp_read_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		spdk_reduce_vol_writev(comp_bdev->vol, bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				       bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
				       spdk_reduce_rw_blocks_cb, bdev_io);
		break;
	/* TODO in future patch in the series */
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
	default:
		SPDK_ERRLOG("Unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (rc) {
		if (rc == -ENOMEM) {
			SPDK_ERRLOG("No memory, start to queue io for compress.\n");
			io_ctx->ch = ch;
			vbdev_compress_queue_io(bdev_io);
		} else {
			SPDK_ERRLOG("on bdev_io submission!\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

static bool
vbdev_compress_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct vbdev_compress *comp_bdev = (struct vbdev_compress *)ctx;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return spdk_bdev_io_type_supported(comp_bdev->base_bdev, io_type);
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	default:
		return false;
	}
}

/* Resubmission function used by the bdev layer when a queued IO is ready to be
 * submitted.
 */
static void
vbdev_compress_resubmit_io(void *arg)
{
	struct spdk_bdev_io *bdev_io = (struct spdk_bdev_io *)arg;
	struct comp_bdev_io *io_ctx = (struct comp_bdev_io *)bdev_io->driver_ctx;

	vbdev_compress_submit_request(io_ctx->ch, bdev_io);
}

/* Used to queue an IO in the event of resource issues. */
static void
vbdev_compress_queue_io(struct spdk_bdev_io *bdev_io)
{
	struct comp_bdev_io *io_ctx = (struct comp_bdev_io *)bdev_io->driver_ctx;
	int rc;

	io_ctx->bdev_io_wait.bdev = bdev_io->bdev;
	io_ctx->bdev_io_wait.cb_fn = vbdev_compress_resubmit_io;
	io_ctx->bdev_io_wait.cb_arg = bdev_io;

	rc = spdk_bdev_queue_io_wait(bdev_io->bdev, io_ctx->ch, &io_ctx->bdev_io_wait);
	if (rc) {
		SPDK_ERRLOG("Queue io failed in vbdev_compress_queue_io, rc=%d.\n", rc);
		assert(false);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/* Callback for unregistering the IO device. */
static void
_device_unregister_cb(void *io_device)
{
	struct vbdev_compress *comp_bdev = io_device;

	/* Done with this comp_bdev. */
	pthread_mutex_destroy(&comp_bdev->reduce_lock);
	free(comp_bdev->comp_bdev.name);
	free(comp_bdev);
}

static void
_reduce_destroy_cb(void *ctx, int reduce_errno)
{
	struct vbdev_compress *comp_bdev = (struct vbdev_compress *)ctx;

	if (reduce_errno) {
		SPDK_ERRLOG("number %d\n", reduce_errno);
	}

	comp_bdev->vol = NULL;
	spdk_bdev_unregister(&comp_bdev->comp_bdev, comp_bdev->delete_cb_fn,
			     comp_bdev->delete_cb_arg);
}

/* Called by reduceLib after performing unload vol actions */
static void
delete_vol_unload_cb(void *cb_arg, int reduce_errno)
{
	struct vbdev_compress *comp_bdev = (struct vbdev_compress *)cb_arg;

	/* Close the underlying bdev. */
	spdk_bdev_close(comp_bdev->base_desc);
	if (reduce_errno) {
		SPDK_ERRLOG("number %d\n", reduce_errno);
	} else {
		/* Clean the device before we free our resources. */
		spdk_reduce_vol_destroy(&comp_bdev->backing_dev, _reduce_destroy_cb, comp_bdev);
	}
}

static void
vbdev_compress_destruct_cb(void *cb_arg, int reduce_errno)
{
	struct vbdev_compress *comp_bdev = (struct vbdev_compress *)cb_arg;

	if (reduce_errno) {
		SPDK_ERRLOG("number %d\n", reduce_errno);
	} else {
		TAILQ_REMOVE(&g_vbdev_comp, comp_bdev, link);
		spdk_bdev_module_release_bdev(comp_bdev->base_bdev);
		spdk_bdev_close(comp_bdev->base_desc);
		comp_bdev->vol = NULL;
		spdk_io_device_unregister(comp_bdev, _device_unregister_cb);
	}
}

/* Called after we've unregistered following a hot remove callback.
 * Our finish entry point will be called next.
 */
static int
vbdev_compress_destruct(void *ctx)
{
	struct vbdev_compress *comp_bdev = (struct vbdev_compress *)ctx;

	if (comp_bdev->vol != NULL) {
		/* Tell reducelib that we're done with this volume. */
		spdk_reduce_vol_unload(comp_bdev->vol, vbdev_compress_destruct_cb, comp_bdev);
	} else {
		vbdev_compress_destruct_cb(comp_bdev, 0);
	}

	return 0;
}

/* We supplied this as an entry point for upper layers who want to communicate to this
 * bdev.  This is how they get a channel.
 */
static struct spdk_io_channel *
vbdev_compress_get_io_channel(void *ctx)
{
	struct vbdev_compress *comp_bdev = (struct vbdev_compress *)ctx;

	/* The IO channel code will allocate a channel for us which consists of
	 * the SPDK channel structure plus the size of our comp_io_channel struct
	 * that we passed in when we registered our IO device. It will then call
	 * our channel create callback to populate any elements that we need to
	 * update.
	 */
	return spdk_get_io_channel(comp_bdev);
}

/* This is the output for get_bdevs() for this vbdev */
static int
vbdev_compress_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct vbdev_compress *comp_bdev = (struct vbdev_compress *)ctx;

	spdk_json_write_name(w, "compress");
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&comp_bdev->comp_bdev));
	spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(comp_bdev->base_bdev));
	spdk_json_write_named_string(w, "compression_pmd", comp_bdev->drv_name);
	spdk_json_write_object_end(w);

	return 0;
}

/* This is used to generate JSON that can configure this module to its current state. */
static int
vbdev_compress_config_json(struct spdk_json_write_ctx *w)
{
	struct vbdev_compress *comp_bdev;

	TAILQ_FOREACH(comp_bdev, &g_vbdev_comp, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "construct_compress_bdev");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(comp_bdev->base_bdev));
		spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&comp_bdev->comp_bdev));
		spdk_json_write_named_string(w, "compression_pmd", comp_bdev->drv_name);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
	return 0;
}

/* Callback from reduce for when init is complete. We'll pass the vbdev_comp struct
 * used for initial metadata operations to claim where it will be further filled out
 * and added to the global list.
 */
static void
vbdev_reduce_init_cb(void *cb_arg, struct spdk_reduce_vol *vol, int reduce_errno)
{
	struct vbdev_compress *meta_ctx = cb_arg;

	/* We're done with metadata operations */
	spdk_put_io_channel(meta_ctx->base_ch);
	spdk_bdev_close(meta_ctx->base_desc);
	meta_ctx->base_desc = NULL;

	if (reduce_errno == 0) {
		meta_ctx->vol = vol;
		vbdev_compress_claim(meta_ctx);
	} else {
		SPDK_ERRLOG("for vol %s, error %u\n",
			    spdk_bdev_get_name(meta_ctx->base_bdev), reduce_errno);
		free(meta_ctx);
	}
}

/* Callback for the function used by reduceLib to perform IO to/from the backing device. We just
 * call the callback provided by reduceLib when it called the read/write/unmap function and
 * free the bdev_io.
 */
static void
comp_reduce_io_cb(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct spdk_reduce_vol_cb_args *cb_args = arg;
	int reduce_errno;

	if (success) {
		reduce_errno = 0;
	} else {
		reduce_errno = -EIO;
	}
	spdk_bdev_free_io(bdev_io);
	cb_args->cb_fn(cb_args->cb_arg, reduce_errno);
}

/* This is the function provided to the reduceLib for sending reads directly to
 * the backing device.
 */
static void
_comp_reduce_readv(struct spdk_reduce_backing_dev *dev, struct iovec *iov, int iovcnt,
		   uint64_t lba, uint32_t lba_count, struct spdk_reduce_vol_cb_args *args)
{
	struct vbdev_compress *comp_bdev = SPDK_CONTAINEROF(dev, struct vbdev_compress,
					   backing_dev);
	int rc;

	rc = spdk_bdev_readv_blocks(comp_bdev->base_desc, comp_bdev->base_ch,
				    iov, iovcnt, lba, lba_count,
				    comp_reduce_io_cb,
				    args);
	if (rc) {
		if (rc == -ENOMEM) {
			SPDK_ERRLOG("No memory, start to queue io.\n");
			/* TODO: there's no bdev_io to queue */
		} else {
			SPDK_ERRLOG("submitting readv request\n");
		}
		args->cb_fn(args->cb_arg, rc);
	}
}

/* This is the function provided to the reduceLib for sending writes directly to
 * the backing device.
 */
static void
_comp_reduce_writev(struct spdk_reduce_backing_dev *dev, struct iovec *iov, int iovcnt,
		    uint64_t lba, uint32_t lba_count, struct spdk_reduce_vol_cb_args *args)
{
	struct vbdev_compress *comp_bdev = SPDK_CONTAINEROF(dev, struct vbdev_compress,
					   backing_dev);
	int rc;

	rc = spdk_bdev_writev_blocks(comp_bdev->base_desc, comp_bdev->base_ch,
				     iov, iovcnt, lba, lba_count,
				     comp_reduce_io_cb,
				     args);
	if (rc) {
		if (rc == -ENOMEM) {
			SPDK_ERRLOG("No memory, start to queue io.\n");
			/* TODO: there's no bdev_io to queue */
		} else {
			SPDK_ERRLOG("error submitting writev request\n");
		}
		args->cb_fn(args->cb_arg, rc);
	}
}

/* This is the function provided to the reduceLib for sending umaps directly to
 * the backing device.
 */
static void
_comp_reduce_unmap(struct spdk_reduce_backing_dev *dev,
		   uint64_t lba, uint32_t lba_count, struct spdk_reduce_vol_cb_args *args)
{
	struct vbdev_compress *comp_bdev = SPDK_CONTAINEROF(dev, struct vbdev_compress,
					   backing_dev);
	int rc;

	rc = spdk_bdev_unmap_blocks(comp_bdev->base_desc, comp_bdev->base_ch,
				    lba, lba_count,
				    comp_reduce_io_cb,
				    args);

	if (rc) {
		if (rc == -ENOMEM) {
			SPDK_ERRLOG("No memory, start to queue io.\n");
			/* TODO: there's no bdev_io to queue */
		} else {
			SPDK_ERRLOG("submitting unmap request\n");
		}
		args->cb_fn(args->cb_arg, rc);
	}
}

/* Called by reduceLib after performing unload vol actions following base bdev hotremove */
static void
bdev_hotremove_vol_unload_cb(void *cb_arg, int reduce_errno)
{
	struct vbdev_compress *comp_bdev = (struct vbdev_compress *)cb_arg;

	if (reduce_errno) {
		SPDK_ERRLOG("number %d\n", reduce_errno);
	}

	spdk_bdev_unregister(&comp_bdev->comp_bdev, NULL, NULL);
}

/* Called when the underlying base bdev goes away. */
static void
vbdev_compress_base_bdev_hotremove_cb(void *ctx)
{
	struct vbdev_compress *comp_bdev, *tmp;
	struct spdk_bdev *bdev_find = ctx;

	TAILQ_FOREACH_SAFE(comp_bdev, &g_vbdev_comp, link, tmp) {
		if (bdev_find == comp_bdev->base_bdev) {
			/* Tell reduceLiib that we're done with this volume. */
			spdk_reduce_vol_unload(comp_bdev->vol, bdev_hotremove_vol_unload_cb, comp_bdev);
		}
	}
}

/* TODO: determine which parms we want user configurable, HC for now
 * params.vol_size
 * params.chunk_size
 * compression PMD, algorithm, window size, comp level, etc.
 * DEV_MD_PATH
 */

/* Common function for init and load to allocate and populate the minimal
 * information for reducelib to init or load.
 */
struct vbdev_compress *
_prepare_for_load_init(struct spdk_bdev *bdev)
{
	struct vbdev_compress *meta_ctx;

	meta_ctx = calloc(1, sizeof(struct vbdev_compress));
	if (meta_ctx == NULL) {
		SPDK_ERRLOG("failed to alloc init contexs\n");
		return NULL;
	}

	meta_ctx->base_bdev = bdev;
	meta_ctx->backing_dev.unmap = _comp_reduce_unmap;
	meta_ctx->backing_dev.readv = _comp_reduce_readv;
	meta_ctx->backing_dev.writev = _comp_reduce_writev;
	meta_ctx->backing_dev.compress = _comp_reduce_compress;
	meta_ctx->backing_dev.decompress = _comp_reduce_decompress;

	meta_ctx->backing_dev.blocklen = bdev->blocklen;
	meta_ctx->backing_dev.blockcnt = bdev->blockcnt;

	/* TODO, configurable chunk size & logical block size */
	meta_ctx->params.chunk_size = DEV_CHUNK_SZ;
	meta_ctx->params.logical_block_size = DEV_LBA_SZ;
	meta_ctx->params.backing_io_unit_size = DEV_BACKING_IO_SZ;
	return meta_ctx;
}

/* Call reducelib to initialize a new volume */
static void
vbdev_init_reduce(struct spdk_bdev *bdev, const char *vbdev_name, const char *comp_pmd)
{
	struct vbdev_compress *meta_ctx;
	int rc;

	meta_ctx = _prepare_for_load_init(bdev);
	if (meta_ctx == NULL) {
		return;
	}

	rc = spdk_bdev_open(meta_ctx->base_bdev, true, vbdev_compress_base_bdev_hotremove_cb,
			    meta_ctx->base_bdev, &meta_ctx->base_desc);
	if (rc) {
		SPDK_ERRLOG("could not open bdev %s\n", spdk_bdev_get_name(meta_ctx->base_bdev));
		free(meta_ctx);
		return;
	}
	meta_ctx->base_ch = spdk_bdev_get_io_channel(meta_ctx->base_desc);

	/* TODO: we'll want to pass name and compression parms to this
	 * function so they can be persisted.  We'll need to retrieve them
	 * in load.
	 */
	spdk_reduce_vol_init(&meta_ctx->params, &meta_ctx->backing_dev,
			     TEST_MD_PATH,
			     vbdev_reduce_init_cb,
			     meta_ctx);
}

/* We provide this callback for the SPDK channel code to create a channel using
 * the channel struct we provided in our module get_io_channel() entry point. Here
 * we get and save off an underlying base channel of the device below us so that
 * we can communicate with the base bdev on a per channel basis.  If we needed
 * our own poller for this vbdev, we'd register it here.
 */
static int
comp_bdev_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct vbdev_compress *comp_bdev = io_device;
	struct comp_device_qp *device_qp;

	/* We use this queue to track outstanding IO in our lyaer. */
	TAILQ_INIT(&comp_bdev->pending_comp_ios);

	/* Now set the reduce channel if it's not already set. */
	pthread_mutex_lock(&comp_bdev->reduce_lock);
	if (comp_bdev->ch_count == 0) {
		comp_bdev->base_ch = spdk_bdev_get_io_channel(comp_bdev->base_desc);
		comp_bdev->reduce_thread = spdk_get_thread();
		comp_bdev->poller = spdk_poller_register(comp_dev_poller, comp_bdev, 0);
		/* Now assign a q pair */
		pthread_mutex_lock(&g_comp_device_qp_lock);
		TAILQ_FOREACH(device_qp, &g_comp_device_qp, link) {
			if ((strcmp(device_qp->device->cdev_info.driver_name, comp_bdev->drv_name) == 0) &&
			    (device_qp->in_use == false)) {
				comp_bdev->device_qp = device_qp;
				device_qp->in_use = true;
				break;
			}
		}
		pthread_mutex_unlock(&g_comp_device_qp_lock);
		assert(comp_bdev->device_qp);
	}
	comp_bdev->ch_count++;
	pthread_mutex_unlock(&comp_bdev->reduce_lock);

	return 0;
}

static void
_clear_qp_and_put_channel(struct vbdev_compress *comp_bdev)
{
	pthread_mutex_lock(&g_comp_device_qp_lock);
	comp_bdev->device_qp->in_use = false;
	pthread_mutex_unlock(&g_comp_device_qp_lock);

	spdk_put_io_channel(comp_bdev->base_ch);
	comp_bdev->reduce_thread = NULL;
	spdk_poller_unregister(&comp_bdev->poller);
}

/* Used to reroute destroy_ch to the correct thread */
static void
_comp_bdev_ch_destroy_cb(void *arg)
{
	struct vbdev_compress *comp_bdev = arg;

	pthread_mutex_lock(&comp_bdev->reduce_lock);
	if (comp_bdev->ch_count == 0) {
		_clear_qp_and_put_channel(comp_bdev);
	}
	pthread_mutex_unlock(&comp_bdev->reduce_lock);
}

/* We provide this callback for the SPDK channel code to destroy a channel
 * created with our create callback. We just need to undo anything we did
 * when we created. If this bdev used its own poller, we'd unregister it here.
 */
static void
comp_bdev_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct vbdev_compress *comp_bdev = io_device;

	pthread_mutex_lock(&comp_bdev->reduce_lock);
	comp_bdev->ch_count--;
	if (comp_bdev->ch_count == 0) {
		/* Send this request to the thread where the channel was created. */
		if (comp_bdev->reduce_thread != spdk_get_thread()) {
			spdk_thread_send_msg(comp_bdev->reduce_thread,
					     _comp_bdev_ch_destroy_cb, comp_bdev);
		} else {
			_clear_qp_and_put_channel(comp_bdev);
		}
	}
	pthread_mutex_unlock(&comp_bdev->reduce_lock);
}

/* RPC entry point for compression vbdev creation. */
int
create_compress_bdev(const char *bdev_name, const char *vbdev_name, const char *comp_pmd)
{
	struct spdk_bdev *bdev;

	bdev = spdk_bdev_get_by_name(bdev_name);
	if (!bdev) {
		return -ENODEV;
	}

	vbdev_init_reduce(bdev, vbdev_name, comp_pmd);
	return 0;
}

/* On init, just init the compress drivers. All metadata is stored on disk. */
static int
vbdev_compress_init(void)
{
	if (vbdev_init_compress_drivers()) {
		SPDK_ERRLOG("Error setting up compression devices\n");
		return -EINVAL;
	}

	return 0;
}

/* Called when the entire module is being torn down. */
static void
vbdev_compress_finish(void)
{
	struct comp_device_qp *dev_qp;
	/* TODO: unload vol in a future patch */

	while ((dev_qp = TAILQ_FIRST(&g_comp_device_qp))) {
		TAILQ_REMOVE(&g_comp_device_qp, dev_qp, link);
		free(dev_qp);
	}
	pthread_mutex_destroy(&g_comp_device_qp_lock);

	rte_mempool_free(g_comp_op_mp);
	spdk_mempool_free(g_mbuf_mp);
}

/* During init we'll be asked how much memory we'd like passed to us
 * in bev_io structures as context. Here's where we specify how
 * much context we want per IO.
 */
static int
vbdev_compress_get_ctx_size(void)
{
	return sizeof(struct comp_bdev_io);
}

/* When we register our bdev this is how we specify our entry points. */
static const struct spdk_bdev_fn_table vbdev_compress_fn_table = {
	.destruct		= vbdev_compress_destruct,
	.submit_request		= vbdev_compress_submit_request,
	.io_type_supported	= vbdev_compress_io_type_supported,
	.get_io_channel		= vbdev_compress_get_io_channel,
	.dump_info_json		= vbdev_compress_dump_info_json,
	.write_config_json	= NULL,
};

static struct spdk_bdev_module compress_if = {
	.name = "compress",
	.module_init = vbdev_compress_init,
	.config_text = NULL,
	.get_ctx_size = vbdev_compress_get_ctx_size,
	.examine_disk = vbdev_compress_examine,
	.module_fini = vbdev_compress_finish,
	.config_json = vbdev_compress_config_json
};

SPDK_BDEV_MODULE_REGISTER(compress, &compress_if)

static void
vbdev_compress_claim(struct vbdev_compress *comp_bdev)
{
	int rc;

	comp_bdev->comp_bdev.name = spdk_sprintf_alloc("COMP_%s", comp_bdev->base_bdev->name);
	if (!comp_bdev->comp_bdev.name) {
		SPDK_ERRLOG("could not allocate comp_bdev name\n");
		goto error_bdev_name;
	}

	/* TODO: need to persist either PMD name or ALGO and a bunch of
	 * other parms to reduce via init and read them back in the load path.
	 */
	comp_bdev->drv_name = ISAL_PMD;

	/* Note: some of the fields below will change in the future - for example,
	 * blockcnt specifically will not match (the compressed volume size will
	 * be slightly less than the base bdev size)
	 */
	comp_bdev->comp_bdev.product_name = COMP_BDEV_NAME;
	comp_bdev->comp_bdev.write_cache = comp_bdev->base_bdev->write_cache;

	comp_bdev->comp_bdev.required_alignment = comp_bdev->base_bdev->required_alignment;

	comp_bdev->comp_bdev.optimal_io_boundary =
		comp_bdev->params.chunk_size / comp_bdev->params.logical_block_size;

	comp_bdev->comp_bdev.split_on_optimal_io_boundary = true;

	comp_bdev->comp_bdev.blocklen = comp_bdev->base_bdev->blocklen;
	comp_bdev->comp_bdev.blockcnt = comp_bdev->base_bdev->blockcnt;

	/* This is the context that is passed to us when the bdev
	 * layer calls in so we'll save our comp_bdev node here.
	 */
	comp_bdev->comp_bdev.ctxt = comp_bdev;
	comp_bdev->comp_bdev.fn_table = &vbdev_compress_fn_table;
	comp_bdev->comp_bdev.module = &compress_if;

	pthread_mutex_init(&comp_bdev->reduce_lock, NULL);

	TAILQ_INSERT_TAIL(&g_vbdev_comp, comp_bdev, link);

	rc = spdk_bdev_open(comp_bdev->base_bdev, true, vbdev_compress_base_bdev_hotremove_cb,
			    comp_bdev->base_bdev, &comp_bdev->base_desc);
	if (rc) {
		SPDK_ERRLOG("could not open bdev %s\n", spdk_bdev_get_name(comp_bdev->base_bdev));
		goto error_open;
	}

	spdk_io_device_register(comp_bdev, comp_bdev_ch_create_cb, comp_bdev_ch_destroy_cb,
				sizeof(struct comp_io_channel),
				comp_bdev->comp_bdev.name);

	rc = spdk_bdev_module_claim_bdev(comp_bdev->base_bdev, comp_bdev->base_desc,
					 comp_bdev->comp_bdev.module);
	if (rc) {
		SPDK_ERRLOG("could not claim bdev %s\n", spdk_bdev_get_name(comp_bdev->base_bdev));
		goto error_claim;
	}

	rc = spdk_bdev_register(&comp_bdev->comp_bdev);
	if (rc < 0) {
		SPDK_ERRLOG("trying to register bdev\n");
		goto error_bdev_register;
	}

	SPDK_NOTICELOG("registered io_device and virtual bdev for: %s\n", comp_bdev->comp_bdev.name);

	return;
	/* Error cleanup paths. */
error_bdev_register:
	spdk_bdev_module_release_bdev(comp_bdev->base_bdev);
error_claim:
	TAILQ_REMOVE(&g_vbdev_comp, comp_bdev, link);
	spdk_io_device_unregister(comp_bdev, NULL);
error_open:
	free(comp_bdev->comp_bdev.name);
error_bdev_name:
	spdk_put_io_channel(comp_bdev->base_ch);
	spdk_bdev_close(comp_bdev->base_desc);
	free(comp_bdev);
	spdk_bdev_module_examine_done(&compress_if);
}

void
delete_compress_bdev(struct spdk_bdev *bdev, spdk_delete_compress_complete cb_fn, void *cb_arg)
{
	struct vbdev_compress *comp_bdev = NULL;

	if (!bdev || bdev->module != &compress_if) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	comp_bdev = SPDK_CONTAINEROF(bdev, struct vbdev_compress, comp_bdev);

	/* Save these for after the vol is destroyed. */
	comp_bdev->delete_cb_fn = cb_fn;
	comp_bdev->delete_cb_arg = cb_arg;

	/* Tell reducelib that we're done with this volume. */
	spdk_reduce_vol_unload(comp_bdev->vol, delete_vol_unload_cb, comp_bdev);
}

/* Callback from reduce for then load is complete. We'll pass the vbdev_comp struct
 * used for initial metadata operations to claim where it will be further filled out
 * and added to the global list.
 */
static void
vbdev_reduce_load_cb(void *cb_arg, struct spdk_reduce_vol *vol, int reduce_errno)
{
	struct vbdev_compress *meta_ctx = cb_arg;

	/* Done with metadata operations */
	spdk_put_io_channel(meta_ctx->base_ch);
	spdk_bdev_close(meta_ctx->base_desc);

	if (reduce_errno != 0) {
		/* This error means it is not a compress disk. */
		if (reduce_errno != -EILSEQ) {
			SPDK_ERRLOG("for vol %s, error %u\n",
				    spdk_bdev_get_name(meta_ctx->base_bdev), reduce_errno);
		}
		free(meta_ctx);
		spdk_bdev_module_examine_done(&compress_if);
		return;
	}

	meta_ctx->vol = vol;
	vbdev_compress_claim(meta_ctx);
	spdk_bdev_module_examine_done(&compress_if);
}

/* Examine_disk entry point: will do a metadata load to see if this is ours,
 * and if so will go ahead and claim it.
 */
static void
vbdev_compress_examine(struct spdk_bdev *bdev)
{
	struct vbdev_compress *meta_ctx;
	int rc;

	if (strcmp(bdev->product_name, COMP_BDEV_NAME) == 0) {
		spdk_bdev_module_examine_done(&compress_if);
		return;
	}

	meta_ctx = _prepare_for_load_init(bdev);
	if (meta_ctx == NULL) {
		spdk_bdev_module_examine_done(&compress_if);
		return;
	}

	rc = spdk_bdev_open(meta_ctx->base_bdev, false, vbdev_compress_base_bdev_hotremove_cb,
			    meta_ctx->base_bdev, &meta_ctx->base_desc);
	if (rc) {
		SPDK_ERRLOG("could not open bdev %s\n", spdk_bdev_get_name(meta_ctx->base_bdev));
		free(meta_ctx);
		spdk_bdev_module_examine_done(&compress_if);
		return;
	}

	meta_ctx->base_ch = spdk_bdev_get_io_channel(meta_ctx->base_desc);
	spdk_reduce_vol_load(&meta_ctx->backing_dev, vbdev_reduce_load_cb, meta_ctx);
}

SPDK_LOG_REGISTER_COMPONENT("vbdev_compress", SPDK_LOG_VBDEV_COMPRESS)
