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
#include "spdk/endian.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"
#include "spdk/bdev_module.h"

#include "spdk/log.h"

#include <rte_config.h>
#include <rte_bus_vdev.h>
#include <rte_compressdev.h>
#include <rte_comp.h>

#define NUM_MAX_XFORMS 2
#define NUM_MAX_INFLIGHT_OPS 128
#define DEFAULT_WINDOW_SIZE 15
/* We need extra mbufs per operation to accommodate host buffers that
 *  span a 2MB boundary.
 */
#define MAX_MBUFS_PER_OP (REDUCE_MAX_IOVECS * 2)
#define CHUNK_SIZE (1024 * 16)
#define COMP_BDEV_NAME "compress"
#define BACKING_IO_SZ (4 * 1024)

#define ISAL_PMD "compress_isal"
#define QAT_PMD "compress_qat"
#define NUM_MBUFS		8192
#define POOL_CACHE_SIZE		256

static enum compress_pmd g_opts;

/* Global list of available compression devices. */
struct compress_dev {
	struct rte_compressdev_info	cdev_info;	/* includes device friendly name */
	uint8_t				cdev_id;	/* identifier for the device */
	void				*comp_xform;	/* shared private xform for comp on this PMD */
	void				*decomp_xform;	/* shared private xform for decomp on this PMD */
	TAILQ_ENTRY(compress_dev)	link;
};
static TAILQ_HEAD(, compress_dev) g_compress_devs = TAILQ_HEAD_INITIALIZER(g_compress_devs);

/* Although ISAL PMD reports 'unlimited' qpairs, it has an unplanned limit of 99 due to
 * the length of the internal ring name that it creates, it breaks a limit in the generic
 * ring code and fails the qp initialization.
 */
#define MAX_NUM_QP 99
/* Global list and lock for unique device/queue pair combos */
struct comp_device_qp {
	struct compress_dev		*device;	/* ptr to compression device */
	uint8_t				qp;		/* queue pair for this node */
	struct spdk_thread		*thread;	/* thead that this qp is assigned to */
	TAILQ_ENTRY(comp_device_qp)	link;
};
static TAILQ_HEAD(, comp_device_qp) g_comp_device_qp = TAILQ_HEAD_INITIALIZER(g_comp_device_qp);
static pthread_mutex_t g_comp_device_qp_lock = PTHREAD_MUTEX_INITIALIZER;

/* For queueing up compression operations that we can't submit for some reason */
struct vbdev_comp_op {
	struct spdk_reduce_backing_dev	*backing_dev;
	struct iovec			*src_iovs;
	int				src_iovcnt;
	struct iovec			*dst_iovs;
	int				dst_iovcnt;
	bool				compress;
	void				*cb_arg;
	TAILQ_ENTRY(vbdev_comp_op)	link;
};

struct vbdev_comp_delete_ctx {
	spdk_delete_compress_complete	cb_fn;
	void				*cb_arg;
	int				cb_rc;
	struct spdk_thread		*orig_thread;
};

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
	struct vbdev_comp_delete_ctx	*delete_ctx;
	bool				orphaned;	/* base bdev claimed but comp_bdev not registered */
	int				reduce_errno;
	TAILQ_HEAD(, vbdev_comp_op)	queued_comp_ops;
	TAILQ_ENTRY(vbdev_compress)	link;
	struct spdk_thread		*thread;	/* thread where base device is opened */
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
	int				status;			/* save for completion on orig thread */
};

/* Shared mempools between all devices on this system */
static struct rte_mempool *g_mbuf_mp = NULL;			/* mbuf mempool */
static struct rte_mempool *g_comp_op_mp = NULL;			/* comp operations, must be rte* mempool */
static struct rte_mbuf_ext_shared_info g_shinfo = {};		/* used by DPDK mbuf macros */
static bool g_qat_available = false;
static bool g_isal_available = false;

/* Create shared (between all ops per PMD) compress xforms. */
static struct rte_comp_xform g_comp_xform = {
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
/* Create shared (between all ops per PMD) decompress xforms. */
static struct rte_comp_xform g_decomp_xform = {
	.type = RTE_COMP_DECOMPRESS,
	.decompress = {
		.algo = RTE_COMP_ALGO_DEFLATE,
		.chksum = RTE_COMP_CHECKSUM_NONE,
		.window_size = DEFAULT_WINDOW_SIZE,
		.hash_algo = RTE_COMP_HASH_ALGO_NONE
	}
};

static void vbdev_compress_examine(struct spdk_bdev *bdev);
static int vbdev_compress_claim(struct vbdev_compress *comp_bdev);
static void vbdev_compress_queue_io(struct spdk_bdev_io *bdev_io);
struct vbdev_compress *_prepare_for_load_init(struct spdk_bdev_desc *bdev_desc, uint32_t lb_size);
static void vbdev_compress_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);
static void comp_bdev_ch_destroy_cb(void *io_device, void *ctx_buf);
static void vbdev_compress_delete_done(void *cb_arg, int bdeverrno);

/* Dummy function used by DPDK to free ext attached buffers
 * to mbufs, we free them ourselves but this callback has to
 * be here.
 */
static void
shinfo_free_cb(void *arg1, void *arg2)
{
}

/* Called by vbdev_init_compress_drivers() to init each discovered compression device */
static int
create_compress_dev(uint8_t index)
{
	struct compress_dev *device;
	uint16_t q_pairs;
	uint8_t cdev_id;
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
		q_pairs = MAX_NUM_QP;
	} else {
		q_pairs = spdk_min(device->cdev_info.max_nb_queue_pairs, MAX_NUM_QP);
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
			if (i > 0) {
				q_pairs = i;
				SPDK_NOTICELOG("FYI failed to setup a queue pair on "
					       "compressdev %u with error %u "
					       "so limiting to %u qpairs\n",
					       cdev_id, rc, q_pairs);
				break;
			} else {
				SPDK_ERRLOG("Failed to setup queue pair on "
					    "compressdev %u with error %u\n", cdev_id, rc);
				rc = -EINVAL;
				goto err;
			}
		}
	}

	rc = rte_compressdev_start(cdev_id);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to start device %u: error %d\n",
			    cdev_id, rc);
		goto err;
	}

	if (device->cdev_info.capabilities->comp_feature_flags & RTE_COMP_FF_SHAREABLE_PRIV_XFORM) {
		rc = rte_compressdev_private_xform_create(cdev_id, &g_comp_xform,
				&device->comp_xform);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to create private comp xform device %u: error %d\n",
				    cdev_id, rc);
			goto err;
		}

		rc = rte_compressdev_private_xform_create(cdev_id, &g_decomp_xform,
				&device->decomp_xform);
		if (rc) {
			SPDK_ERRLOG("Failed to create private decomp xform device %u: error %d\n",
				    cdev_id, rc);
			goto err;
		}
	} else {
		SPDK_ERRLOG("PMD does not support shared transforms\n");
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
		dev_qp->thread = NULL;
		TAILQ_INSERT_TAIL(&g_comp_device_qp, dev_qp, link);
	}

	TAILQ_INSERT_TAIL(&g_compress_devs, device, link);

	if (strcmp(device->cdev_info.driver_name, QAT_PMD) == 0) {
		g_qat_available = true;
	}
	if (strcmp(device->cdev_info.driver_name, ISAL_PMD) == 0) {
		g_isal_available = true;
	}

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

	g_mbuf_mp = rte_pktmbuf_pool_create("comp_mbuf_mp", NUM_MBUFS, POOL_CACHE_SIZE,
					    sizeof(struct rte_mbuf), 0, rte_socket_id());
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
		rc = create_compress_dev(i);
		if (rc != 0) {
			goto error_create_compress_devs;
		}
	}

	if (g_qat_available == true) {
		SPDK_NOTICELOG("initialized QAT PMD\n");
	}

	g_shinfo.free_cb = shinfo_free_cb;

	return 0;

	/* Error cleanup paths. */
error_create_compress_devs:
	TAILQ_FOREACH_SAFE(device, &g_compress_devs, link, tmp_dev) {
		TAILQ_REMOVE(&g_compress_devs, device, link);
		free(device);
	}
error_create_op:
error_create_mbuf:
	rte_mempool_free(g_mbuf_mp);

	return rc;
}

/* for completing rw requests on the orig IO thread. */
static void
_reduce_rw_blocks_cb(void *arg)
{
	struct comp_bdev_io *io_ctx = arg;

	if (io_ctx->status == 0) {
		spdk_bdev_io_complete(io_ctx->orig_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		SPDK_ERRLOG("status %d on operation from reduce API\n", io_ctx->status);
		spdk_bdev_io_complete(io_ctx->orig_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/* Completion callback for r/w that were issued via reducelib. */
static void
reduce_rw_blocks_cb(void *arg, int reduce_errno)
{
	struct spdk_bdev_io *bdev_io = arg;
	struct comp_bdev_io *io_ctx = (struct comp_bdev_io *)bdev_io->driver_ctx;
	struct spdk_io_channel *ch = spdk_io_channel_from_ctx(io_ctx->comp_ch);
	struct spdk_thread *orig_thread;

	/* TODO: need to decide which error codes are bdev_io success vs failure;
	 * example examine calls reading metadata */

	io_ctx->status = reduce_errno;

	/* Send this request to the orig IO thread. */
	orig_thread = spdk_io_channel_get_thread(ch);
	if (orig_thread != spdk_get_thread()) {
		spdk_thread_send_msg(orig_thread, _reduce_rw_blocks_cb, io_ctx);
	} else {
		_reduce_rw_blocks_cb(io_ctx);
	}
}

static uint64_t
_setup_compress_mbuf(struct rte_mbuf **mbufs, int *mbuf_total, uint64_t *total_length,
		     struct iovec *iovs, int iovcnt, void *reduce_cb_arg)
{
	uint64_t updated_length, remainder, phys_addr;
	uint8_t *current_base = NULL;
	int iov_index, mbuf_index;
	int rc = 0;

	/* Setup mbufs */
	iov_index = mbuf_index = 0;
	while (iov_index < iovcnt) {

		current_base = iovs[iov_index].iov_base;
		if (total_length) {
			*total_length += iovs[iov_index].iov_len;
		}
		assert(mbufs[mbuf_index] != NULL);
		mbufs[mbuf_index]->userdata = reduce_cb_arg;
		updated_length = iovs[iov_index].iov_len;
		phys_addr = spdk_vtophys((void *)current_base, &updated_length);

		rte_pktmbuf_attach_extbuf(mbufs[mbuf_index],
					  current_base,
					  phys_addr,
					  updated_length,
					  &g_shinfo);
		rte_pktmbuf_append(mbufs[mbuf_index], updated_length);
		remainder = iovs[iov_index].iov_len - updated_length;

		if (mbuf_index > 0) {
			rte_pktmbuf_chain(mbufs[0], mbufs[mbuf_index]);
		}

		/* If we crossed 2 2MB boundary we need another mbuf for the remainder */
		if (remainder > 0) {
			/* allocate an mbuf at the end of the array */
			rc = rte_pktmbuf_alloc_bulk(g_mbuf_mp,
						    (struct rte_mbuf **)&mbufs[*mbuf_total], 1);
			if (rc) {
				SPDK_ERRLOG("ERROR trying to get an extra mbuf!\n");
				return -1;
			}
			(*mbuf_total)++;
			mbuf_index++;
			mbufs[mbuf_index]->userdata = reduce_cb_arg;
			current_base += updated_length;
			phys_addr = spdk_vtophys((void *)current_base, &remainder);
			/* assert we don't cross another */
			assert(remainder == iovs[iov_index].iov_len - updated_length);

			rte_pktmbuf_attach_extbuf(mbufs[mbuf_index],
						  current_base,
						  phys_addr,
						  remainder,
						  &g_shinfo);
			rte_pktmbuf_append(mbufs[mbuf_index], remainder);
			rte_pktmbuf_chain(mbufs[0], mbufs[mbuf_index]);
		}
		iov_index++;
		mbuf_index++;
	}

	return 0;
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
	int rc = 0;
	struct vbdev_comp_op *op_to_queue;
	int i;
	int src_mbuf_total = src_iovcnt;
	int dst_mbuf_total = dst_iovcnt;
	bool device_error = false;

	assert(src_iovcnt < MAX_MBUFS_PER_OP);

#ifdef DEBUG
	memset(src_mbufs, 0, sizeof(src_mbufs));
	memset(dst_mbufs, 0, sizeof(dst_mbufs));
#endif

	comp_op = rte_comp_op_alloc(g_comp_op_mp);
	if (!comp_op) {
		SPDK_ERRLOG("trying to get a comp op!\n");
		goto error_get_op;
	}

	/* get an mbuf per iov, src and dst */
	rc = rte_pktmbuf_alloc_bulk(g_mbuf_mp, (struct rte_mbuf **)&src_mbufs[0], src_iovcnt);
	if (rc) {
		SPDK_ERRLOG("ERROR trying to get src_mbufs!\n");
		goto error_get_src;
	}

	rc = rte_pktmbuf_alloc_bulk(g_mbuf_mp, (struct rte_mbuf **)&dst_mbufs[0], dst_iovcnt);
	if (rc) {
		SPDK_ERRLOG("ERROR trying to get dst_mbufs!\n");
		goto error_get_dst;
	}

	/* There is a 1:1 mapping between a bdev_io and a compression operation, but
	 * all compression PMDs that SPDK uses support chaining so build our mbuf chain
	 * and associate with our single comp_op.
	 */

	rc = _setup_compress_mbuf(&src_mbufs[0], &src_mbuf_total, &total_length,
				  src_iovs, src_iovcnt, reduce_cb_arg);
	if (rc < 0) {
		goto error_src_dst;
	}

	comp_op->m_src = src_mbufs[0];
	comp_op->src.offset = 0;
	comp_op->src.length = total_length;

	/* setup dst mbufs, for the current test being used with this code there's only one vector */
	rc = _setup_compress_mbuf(&dst_mbufs[0], &dst_mbuf_total, NULL,
				  dst_iovs, dst_iovcnt, reduce_cb_arg);
	if (rc < 0) {
		goto error_src_dst;
	}

	comp_op->m_dst = dst_mbufs[0];
	comp_op->dst.offset = 0;

	if (compress == true) {
		comp_op->private_xform = comp_bdev->device_qp->device->comp_xform;
	} else {
		comp_op->private_xform = comp_bdev->device_qp->device->decomp_xform;
	}

	comp_op->op_type = RTE_COMP_OP_STATELESS;
	comp_op->flush_flag = RTE_COMP_FLUSH_FINAL;

	rc = rte_compressdev_enqueue_burst(cdev_id, comp_bdev->device_qp->qp, &comp_op, 1);
	assert(rc <= 1);

	/* We always expect 1 got queued, if 0 then we need to queue it up. */
	if (rc == 1) {
		return 0;
	} else if (comp_op->status == RTE_COMP_OP_STATUS_NOT_PROCESSED) {
		/* we free mbufs differently depending on whether they were chained or not */
		rte_pktmbuf_free(comp_op->m_src);
		rte_pktmbuf_free(comp_op->m_dst);
		goto error_enqueue;
	} else {
		device_error = true;
		goto error_src_dst;
	}

	/* Error cleanup paths. */
error_src_dst:
	for (i = 0; i < dst_mbuf_total; i++) {
		rte_pktmbuf_free((struct rte_mbuf *)&dst_mbufs[i]);
	}
error_get_dst:
	for (i = 0; i < src_mbuf_total; i++) {
		rte_pktmbuf_free((struct rte_mbuf *)&src_mbufs[i]);
	}
error_get_src:
error_enqueue:
	rte_comp_op_free(comp_op);
error_get_op:

	if (device_error == true) {
		/* There was an error sending the op to the device, most
		 * likely with the parameters.
		 */
		SPDK_ERRLOG("Compression API returned 0x%x\n", comp_op->status);
		return -EINVAL;
	}

	op_to_queue = calloc(1, sizeof(struct vbdev_comp_op));
	if (op_to_queue == NULL) {
		SPDK_ERRLOG("unable to allocate operation for queueing.\n");
		return -ENOMEM;
	}
	op_to_queue->backing_dev = backing_dev;
	op_to_queue->src_iovs = src_iovs;
	op_to_queue->src_iovcnt = src_iovcnt;
	op_to_queue->dst_iovs = dst_iovs;
	op_to_queue->dst_iovcnt = dst_iovcnt;
	op_to_queue->compress = compress;
	op_to_queue->cb_arg = cb_arg;
	TAILQ_INSERT_TAIL(&comp_bdev->queued_comp_ops,
			  op_to_queue,
			  link);
	return 0;
}

/* Poller for the DPDK compression driver. */
static int
comp_dev_poller(void *args)
{
	struct vbdev_compress *comp_bdev = args;
	uint8_t cdev_id = comp_bdev->device_qp->device->cdev_id;
	struct rte_comp_op *deq_ops[NUM_MAX_INFLIGHT_OPS];
	uint16_t num_deq;
	struct spdk_reduce_vol_cb_args *reduce_args;
	struct vbdev_comp_op *op_to_resubmit;
	int rc, i;

	num_deq = rte_compressdev_dequeue_burst(cdev_id, comp_bdev->device_qp->qp, deq_ops,
						NUM_MAX_INFLIGHT_OPS);
	for (i = 0; i < num_deq; i++) {
		reduce_args = (struct spdk_reduce_vol_cb_args *)deq_ops[i]->m_src->userdata;

		if (deq_ops[i]->status == RTE_COMP_OP_STATUS_SUCCESS) {

			/* tell reduce this is done and what the bytecount was */
			reduce_args->cb_fn(reduce_args->cb_arg, deq_ops[i]->produced);
		} else {
			SPDK_NOTICELOG("FYI storing data uncompressed due to deque status %u\n",
				       deq_ops[i]->status);

			/* Reduce will simply store uncompressed on neg errno value. */
			reduce_args->cb_fn(reduce_args->cb_arg, -EINVAL);
		}

		/* Now free both mbufs and the compress operation. The rte_pktmbuf_free()
		 * call takes care of freeing all of the mbufs in the chain back to their
		 * original pool.
		 */
		rte_pktmbuf_free(deq_ops[i]->m_src);
		rte_pktmbuf_free(deq_ops[i]->m_dst);

		/* There is no bulk free for com ops so we have to free them one at a time
		 * here however it would be rare that we'd ever have more than 1 at a time
		 * anyways.
		 */
		rte_comp_op_free(deq_ops[i]);

		/* Check if there are any pending comp ops to process, only pull one
		 * at a time off as _compress_operation() may re-queue the op.
		 */
		if (!TAILQ_EMPTY(&comp_bdev->queued_comp_ops)) {
			op_to_resubmit = TAILQ_FIRST(&comp_bdev->queued_comp_ops);
			rc = _compress_operation(op_to_resubmit->backing_dev,
						 op_to_resubmit->src_iovs,
						 op_to_resubmit->src_iovcnt,
						 op_to_resubmit->dst_iovs,
						 op_to_resubmit->dst_iovcnt,
						 op_to_resubmit->compress,
						 op_to_resubmit->cb_arg);
			if (rc == 0) {
				TAILQ_REMOVE(&comp_bdev->queued_comp_ops, op_to_resubmit, link);
				free(op_to_resubmit);
			}
		}
	}
	return num_deq == 0 ? SPDK_POLLER_IDLE : SPDK_POLLER_BUSY;
}

/* Entry point for reduce lib to issue a compress operation. */
static void
_comp_reduce_compress(struct spdk_reduce_backing_dev *dev,
		      struct iovec *src_iovs, int src_iovcnt,
		      struct iovec *dst_iovs, int dst_iovcnt,
		      struct spdk_reduce_vol_cb_args *cb_arg)
{
	int rc;

	rc = _compress_operation(dev, src_iovs, src_iovcnt, dst_iovs, dst_iovcnt, true, cb_arg);
	if (rc) {
		SPDK_ERRLOG("with compress operation code %d (%s)\n", rc, spdk_strerror(-rc));
		cb_arg->cb_fn(cb_arg->cb_arg, rc);
	}
}

/* Entry point for reduce lib to issue a decompress operation. */
static void
_comp_reduce_decompress(struct spdk_reduce_backing_dev *dev,
			struct iovec *src_iovs, int src_iovcnt,
			struct iovec *dst_iovs, int dst_iovcnt,
			struct spdk_reduce_vol_cb_args *cb_arg)
{
	int rc;

	rc = _compress_operation(dev, src_iovs, src_iovcnt, dst_iovs, dst_iovcnt, false, cb_arg);
	if (rc) {
		SPDK_ERRLOG("with decompress operation code %d (%s)\n", rc, spdk_strerror(-rc));
		cb_arg->cb_fn(cb_arg->cb_arg, rc);
	}
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
			      reduce_rw_blocks_cb, bdev_io);
}

/* scheduled for completion on IO thread */
static void
_complete_other_io(void *arg)
{
	struct comp_bdev_io *io_ctx = (struct comp_bdev_io *)arg;
	if (io_ctx->status == 0) {
		spdk_bdev_io_complete(io_ctx->orig_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		spdk_bdev_io_complete(io_ctx->orig_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/* scheduled for submission on reduce thread */
static void
_comp_bdev_io_submit(void *arg)
{
	struct spdk_bdev_io *bdev_io = arg;
	struct comp_bdev_io *io_ctx = (struct comp_bdev_io *)bdev_io->driver_ctx;
	struct spdk_io_channel *ch = spdk_io_channel_from_ctx(io_ctx->comp_ch);
	struct vbdev_compress *comp_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_compress,
					   comp_bdev);
	struct spdk_thread *orig_thread;
	int rc = 0;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, comp_read_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return;
	case SPDK_BDEV_IO_TYPE_WRITE:
		spdk_reduce_vol_writev(comp_bdev->vol, bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				       bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
				       reduce_rw_blocks_cb, bdev_io);
		return;
	/* TODO in future patch in the series */
	case SPDK_BDEV_IO_TYPE_RESET:
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	default:
		SPDK_ERRLOG("Unknown I/O type %d\n", bdev_io->type);
		rc = -EINVAL;
	}

	if (rc) {
		if (rc == -ENOMEM) {
			SPDK_ERRLOG("No memory, start to queue io for compress.\n");
			io_ctx->ch = ch;
			vbdev_compress_queue_io(bdev_io);
			return;
		} else {
			SPDK_ERRLOG("on bdev_io submission!\n");
			io_ctx->status = rc;
		}
	}

	/* Complete this on the orig IO thread. */
	orig_thread = spdk_io_channel_get_thread(ch);
	if (orig_thread != spdk_get_thread()) {
		spdk_thread_send_msg(orig_thread, _complete_other_io, io_ctx);
	} else {
		_complete_other_io(io_ctx);
	}
}

/* Called when someone above submits IO to this vbdev. */
static void
vbdev_compress_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct comp_bdev_io *io_ctx = (struct comp_bdev_io *)bdev_io->driver_ctx;
	struct vbdev_compress *comp_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_compress,
					   comp_bdev);
	struct comp_io_channel *comp_ch = spdk_io_channel_get_ctx(ch);

	memset(io_ctx, 0, sizeof(struct comp_bdev_io));
	io_ctx->comp_bdev = comp_bdev;
	io_ctx->comp_ch = comp_ch;
	io_ctx->orig_io = bdev_io;

	/* Send this request to the reduce_thread if that's not what we're on. */
	if (spdk_get_thread() != comp_bdev->reduce_thread) {
		spdk_thread_send_msg(comp_bdev->reduce_thread, _comp_bdev_io_submit, bdev_io);
	} else {
		_comp_bdev_io_submit(bdev_io);
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

	rc = spdk_bdev_queue_io_wait(bdev_io->bdev, io_ctx->comp_bdev->base_ch, &io_ctx->bdev_io_wait);
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
_vbdev_compress_destruct_cb(void *ctx)
{
	struct vbdev_compress *comp_bdev = ctx;

	TAILQ_REMOVE(&g_vbdev_comp, comp_bdev, link);
	spdk_bdev_module_release_bdev(comp_bdev->base_bdev);
	/* Close the underlying bdev on its same opened thread. */
	spdk_bdev_close(comp_bdev->base_desc);
	comp_bdev->vol = NULL;
	if (comp_bdev->orphaned == false) {
		spdk_io_device_unregister(comp_bdev, _device_unregister_cb);
	} else {
		vbdev_compress_delete_done(comp_bdev->delete_ctx, 0);
		_device_unregister_cb(comp_bdev);
	}
}

static void
vbdev_compress_destruct_cb(void *cb_arg, int reduce_errno)
{
	struct vbdev_compress *comp_bdev = (struct vbdev_compress *)cb_arg;

	if (reduce_errno) {
		SPDK_ERRLOG("number %d\n", reduce_errno);
	} else {
		if (comp_bdev->thread && comp_bdev->thread != spdk_get_thread()) {
			spdk_thread_send_msg(comp_bdev->thread,
					     _vbdev_compress_destruct_cb, comp_bdev);
		} else {
			_vbdev_compress_destruct_cb(comp_bdev);
		}
	}
}

static void
_reduce_destroy_cb(void *ctx, int reduce_errno)
{
	struct vbdev_compress *comp_bdev = (struct vbdev_compress *)ctx;

	if (reduce_errno) {
		SPDK_ERRLOG("number %d\n", reduce_errno);
	}

	comp_bdev->vol = NULL;
	spdk_put_io_channel(comp_bdev->base_ch);
	if (comp_bdev->orphaned == false) {
		spdk_bdev_unregister(&comp_bdev->comp_bdev, vbdev_compress_delete_done,
				     comp_bdev->delete_ctx);
	} else {
		vbdev_compress_destruct_cb((void *)comp_bdev, 0);
	}

}

static void
_delete_vol_unload_cb(void *ctx)
{
	struct vbdev_compress *comp_bdev = ctx;

	/* FIXME: Assert if these conditions are not satisified for now. */
	assert(!comp_bdev->reduce_thread ||
	       comp_bdev->reduce_thread == spdk_get_thread());

	/* reducelib needs a channel to comm with the backing device */
	comp_bdev->base_ch = spdk_bdev_get_io_channel(comp_bdev->base_desc);

	/* Clean the device before we free our resources. */
	spdk_reduce_vol_destroy(&comp_bdev->backing_dev, _reduce_destroy_cb, comp_bdev);
}

/* Called by reduceLib after performing unload vol actions */
static void
delete_vol_unload_cb(void *cb_arg, int reduce_errno)
{
	struct vbdev_compress *comp_bdev = (struct vbdev_compress *)cb_arg;

	if (reduce_errno) {
		SPDK_ERRLOG("number %d\n", reduce_errno);
		/* FIXME: callback should be executed. */
		return;
	}

	pthread_mutex_lock(&comp_bdev->reduce_lock);
	if (comp_bdev->reduce_thread && comp_bdev->reduce_thread != spdk_get_thread()) {
		spdk_thread_send_msg(comp_bdev->reduce_thread,
				     _delete_vol_unload_cb, comp_bdev);
		pthread_mutex_unlock(&comp_bdev->reduce_lock);
	} else {
		pthread_mutex_unlock(&comp_bdev->reduce_lock);

		_delete_vol_unload_cb(comp_bdev);
	}
}

const char *
compress_get_name(const struct vbdev_compress *comp_bdev)
{
	return comp_bdev->comp_bdev.name;
}

struct vbdev_compress *
compress_bdev_first(void)
{
	struct vbdev_compress *comp_bdev;

	comp_bdev = TAILQ_FIRST(&g_vbdev_comp);

	return comp_bdev;
}

struct vbdev_compress *
compress_bdev_next(struct vbdev_compress *prev)
{
	struct vbdev_compress *comp_bdev;

	comp_bdev = TAILQ_NEXT(prev, link);

	return comp_bdev;
}

bool
compress_has_orphan(const char *name)
{
	struct vbdev_compress *comp_bdev;

	TAILQ_FOREACH(comp_bdev, &g_vbdev_comp, link) {
		if (comp_bdev->orphaned && strcmp(name, comp_bdev->comp_bdev.name) == 0) {
			return true;
		}
	}
	return false;
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

/* This is the output for bdev_get_bdevs() for this vbdev */
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
		spdk_json_write_named_string(w, "method", "bdev_compress_create");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(comp_bdev->base_bdev));
		spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&comp_bdev->comp_bdev));
		spdk_json_write_named_string(w, "compression_pmd", comp_bdev->drv_name);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
	return 0;
}

static void
_vbdev_reduce_init_cb(void *ctx)
{
	struct vbdev_compress *meta_ctx = ctx;
	int rc;

	assert(meta_ctx->base_desc != NULL);

	/* We're done with metadata operations */
	spdk_put_io_channel(meta_ctx->base_ch);

	if (meta_ctx->vol) {
		rc = vbdev_compress_claim(meta_ctx);
		if (rc == 0) {
			return;
		}
	}

	/* Close the underlying bdev on its same opened thread. */
	spdk_bdev_close(meta_ctx->base_desc);
	free(meta_ctx);
}

/* Callback from reduce for when init is complete. We'll pass the vbdev_comp struct
 * used for initial metadata operations to claim where it will be further filled out
 * and added to the global list.
 */
static void
vbdev_reduce_init_cb(void *cb_arg, struct spdk_reduce_vol *vol, int reduce_errno)
{
	struct vbdev_compress *meta_ctx = cb_arg;

	if (reduce_errno == 0) {
		meta_ctx->vol = vol;
	} else {
		SPDK_ERRLOG("for vol %s, error %u\n",
			    spdk_bdev_get_name(meta_ctx->base_bdev), reduce_errno);
	}

	if (meta_ctx->thread && meta_ctx->thread != spdk_get_thread()) {
		spdk_thread_send_msg(meta_ctx->thread, _vbdev_reduce_init_cb, meta_ctx);
	} else {
		_vbdev_reduce_init_cb(meta_ctx);
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

/* This is the function provided to the reduceLib for sending unmaps directly to
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

	comp_bdev->vol = NULL;
	spdk_bdev_unregister(&comp_bdev->comp_bdev, NULL, NULL);
}

static void
vbdev_compress_base_bdev_hotremove_cb(struct spdk_bdev *bdev_find)
{
	struct vbdev_compress *comp_bdev, *tmp;

	TAILQ_FOREACH_SAFE(comp_bdev, &g_vbdev_comp, link, tmp) {
		if (bdev_find == comp_bdev->base_bdev) {
			/* Tell reduceLib that we're done with this volume. */
			spdk_reduce_vol_unload(comp_bdev->vol, bdev_hotremove_vol_unload_cb, comp_bdev);
		}
	}
}

/* Called when the underlying base bdev triggers asynchronous event such as bdev removal. */
static void
vbdev_compress_base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
				  void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		vbdev_compress_base_bdev_hotremove_cb(bdev);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
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
_prepare_for_load_init(struct spdk_bdev_desc *bdev_desc, uint32_t lb_size)
{
	struct vbdev_compress *meta_ctx;
	struct spdk_bdev *bdev;

	meta_ctx = calloc(1, sizeof(struct vbdev_compress));
	if (meta_ctx == NULL) {
		SPDK_ERRLOG("failed to alloc init contexts\n");
		return NULL;
	}

	meta_ctx->drv_name = "None";
	meta_ctx->backing_dev.unmap = _comp_reduce_unmap;
	meta_ctx->backing_dev.readv = _comp_reduce_readv;
	meta_ctx->backing_dev.writev = _comp_reduce_writev;
	meta_ctx->backing_dev.compress = _comp_reduce_compress;
	meta_ctx->backing_dev.decompress = _comp_reduce_decompress;

	meta_ctx->base_desc = bdev_desc;
	bdev = spdk_bdev_desc_get_bdev(bdev_desc);
	meta_ctx->base_bdev = bdev;

	meta_ctx->backing_dev.blocklen = bdev->blocklen;
	meta_ctx->backing_dev.blockcnt = bdev->blockcnt;

	meta_ctx->params.chunk_size = CHUNK_SIZE;
	if (lb_size == 0) {
		meta_ctx->params.logical_block_size = bdev->blocklen;
	} else {
		meta_ctx->params.logical_block_size = lb_size;
	}

	meta_ctx->params.backing_io_unit_size = BACKING_IO_SZ;
	return meta_ctx;
}

static bool
_set_pmd(struct vbdev_compress *comp_dev)
{
	if (g_opts == COMPRESS_PMD_AUTO) {
		if (g_qat_available) {
			comp_dev->drv_name = QAT_PMD;
		} else {
			comp_dev->drv_name = ISAL_PMD;
		}
	} else if (g_opts == COMPRESS_PMD_QAT_ONLY && g_qat_available) {
		comp_dev->drv_name = QAT_PMD;
	} else if (g_opts == COMPRESS_PMD_ISAL_ONLY && g_isal_available) {
		comp_dev->drv_name = ISAL_PMD;
	} else {
		SPDK_ERRLOG("Requested PMD is not available.\n");
		return false;
	}
	SPDK_NOTICELOG("PMD being used: %s\n", comp_dev->drv_name);
	return true;
}

/* Call reducelib to initialize a new volume */
static int
vbdev_init_reduce(const char *bdev_name, const char *pm_path, uint32_t lb_size)
{
	struct spdk_bdev_desc *bdev_desc = NULL;
	struct vbdev_compress *meta_ctx;
	int rc;

	rc = spdk_bdev_open_ext(bdev_name, true, vbdev_compress_base_bdev_event_cb,
				NULL, &bdev_desc);
	if (rc) {
		SPDK_ERRLOG("could not open bdev %s\n", bdev_name);
		return rc;
	}

	meta_ctx = _prepare_for_load_init(bdev_desc, lb_size);
	if (meta_ctx == NULL) {
		spdk_bdev_close(bdev_desc);
		return -EINVAL;
	}

	if (_set_pmd(meta_ctx) == false) {
		SPDK_ERRLOG("could not find required pmd\n");
		free(meta_ctx);
		spdk_bdev_close(bdev_desc);
		return -EINVAL;
	}

	/* Save the thread where the base device is opened */
	meta_ctx->thread = spdk_get_thread();

	meta_ctx->base_ch = spdk_bdev_get_io_channel(meta_ctx->base_desc);

	spdk_reduce_vol_init(&meta_ctx->params, &meta_ctx->backing_dev,
			     pm_path,
			     vbdev_reduce_init_cb,
			     meta_ctx);
	return 0;
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

	/* Now set the reduce channel if it's not already set. */
	pthread_mutex_lock(&comp_bdev->reduce_lock);
	if (comp_bdev->ch_count == 0) {
		/* We use this queue to track outstanding IO in our layer. */
		TAILQ_INIT(&comp_bdev->pending_comp_ios);

		/* We use this to queue up compression operations as needed. */
		TAILQ_INIT(&comp_bdev->queued_comp_ops);

		comp_bdev->base_ch = spdk_bdev_get_io_channel(comp_bdev->base_desc);
		comp_bdev->reduce_thread = spdk_get_thread();
		comp_bdev->poller = SPDK_POLLER_REGISTER(comp_dev_poller, comp_bdev, 0);
		/* Now assign a q pair */
		pthread_mutex_lock(&g_comp_device_qp_lock);
		TAILQ_FOREACH(device_qp, &g_comp_device_qp, link) {
			if (strcmp(device_qp->device->cdev_info.driver_name, comp_bdev->drv_name) == 0) {
				if (device_qp->thread == spdk_get_thread()) {
					comp_bdev->device_qp = device_qp;
					break;
				}
				if (device_qp->thread == NULL) {
					comp_bdev->device_qp = device_qp;
					device_qp->thread = spdk_get_thread();
					break;
				}
			}
		}
		pthread_mutex_unlock(&g_comp_device_qp_lock);
	}
	comp_bdev->ch_count++;
	pthread_mutex_unlock(&comp_bdev->reduce_lock);

	if (comp_bdev->device_qp != NULL) {
		return 0;
	} else {
		SPDK_ERRLOG("out of qpairs, cannot assign one to comp_bdev %p\n", comp_bdev);
		assert(false);
		return -ENOMEM;
	}
}

static void
_channel_cleanup(struct vbdev_compress *comp_bdev)
{
	/* Note: comp_bdevs can share a device_qp if they are
	 * on the same thread so we leave the device_qp element
	 * alone for this comp_bdev and just clear the reduce thread.
	 */
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
	_channel_cleanup(comp_bdev);
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
			_channel_cleanup(comp_bdev);
		}
	}
	pthread_mutex_unlock(&comp_bdev->reduce_lock);
}

/* RPC entry point for compression vbdev creation. */
int
create_compress_bdev(const char *bdev_name, const char *pm_path, uint32_t lb_size)
{
	if ((lb_size != 0) && (lb_size != LB_SIZE_4K) && (lb_size != LB_SIZE_512B)) {
		SPDK_ERRLOG("Logical block size must be 512 or 4096\n");
		return -EINVAL;
	}

	return vbdev_init_reduce(bdev_name, pm_path, lb_size);
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
	rte_mempool_free(g_mbuf_mp);
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
	.get_ctx_size = vbdev_compress_get_ctx_size,
	.examine_disk = vbdev_compress_examine,
	.module_fini = vbdev_compress_finish,
	.config_json = vbdev_compress_config_json
};

SPDK_BDEV_MODULE_REGISTER(compress, &compress_if)

static int _set_compbdev_name(struct vbdev_compress *comp_bdev)
{
	struct spdk_bdev_alias *aliases;

	if (!TAILQ_EMPTY(spdk_bdev_get_aliases(comp_bdev->base_bdev))) {
		aliases = TAILQ_FIRST(spdk_bdev_get_aliases(comp_bdev->base_bdev));
		comp_bdev->comp_bdev.name = spdk_sprintf_alloc("COMP_%s", aliases->alias);
		if (!comp_bdev->comp_bdev.name) {
			SPDK_ERRLOG("could not allocate comp_bdev name for alias\n");
			return -ENOMEM;
		}
	} else {
		comp_bdev->comp_bdev.name = spdk_sprintf_alloc("COMP_%s", comp_bdev->base_bdev->name);
		if (!comp_bdev->comp_bdev.name) {
			SPDK_ERRLOG("could not allocate comp_bdev name for unique name\n");
			return -ENOMEM;
		}
	}
	return 0;
}

static int
vbdev_compress_claim(struct vbdev_compress *comp_bdev)
{
	int rc;

	if (_set_compbdev_name(comp_bdev)) {
		return -EINVAL;
	}

	/* Note: some of the fields below will change in the future - for example,
	 * blockcnt specifically will not match (the compressed volume size will
	 * be slightly less than the base bdev size)
	 */
	comp_bdev->comp_bdev.product_name = COMP_BDEV_NAME;
	comp_bdev->comp_bdev.write_cache = comp_bdev->base_bdev->write_cache;

	if (strcmp(comp_bdev->drv_name, QAT_PMD) == 0) {
		comp_bdev->comp_bdev.required_alignment =
			spdk_max(spdk_u32log2(comp_bdev->base_bdev->blocklen),
				 comp_bdev->base_bdev->required_alignment);
		SPDK_NOTICELOG("QAT in use: Required alignment set to %u\n",
			       comp_bdev->comp_bdev.required_alignment);
	} else {
		comp_bdev->comp_bdev.required_alignment = comp_bdev->base_bdev->required_alignment;
	}
	comp_bdev->comp_bdev.optimal_io_boundary =
		comp_bdev->params.chunk_size / comp_bdev->params.logical_block_size;

	comp_bdev->comp_bdev.split_on_optimal_io_boundary = true;

	comp_bdev->comp_bdev.blocklen = comp_bdev->params.logical_block_size;
	comp_bdev->comp_bdev.blockcnt = comp_bdev->params.vol_size / comp_bdev->comp_bdev.blocklen;
	assert(comp_bdev->comp_bdev.blockcnt > 0);

	/* This is the context that is passed to us when the bdev
	 * layer calls in so we'll save our comp_bdev node here.
	 */
	comp_bdev->comp_bdev.ctxt = comp_bdev;
	comp_bdev->comp_bdev.fn_table = &vbdev_compress_fn_table;
	comp_bdev->comp_bdev.module = &compress_if;

	pthread_mutex_init(&comp_bdev->reduce_lock, NULL);

	/* Save the thread where the base device is opened */
	comp_bdev->thread = spdk_get_thread();

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

	TAILQ_INSERT_TAIL(&g_vbdev_comp, comp_bdev, link);

	SPDK_NOTICELOG("registered io_device and virtual bdev for: %s\n", comp_bdev->comp_bdev.name);

	return 0;

	/* Error cleanup paths. */
error_bdev_register:
	spdk_bdev_module_release_bdev(comp_bdev->base_bdev);
error_claim:
	spdk_io_device_unregister(comp_bdev, NULL);
	free(comp_bdev->comp_bdev.name);
	return rc;
}

static void
_vbdev_compress_delete_done(void *_ctx)
{
	struct vbdev_comp_delete_ctx *ctx = _ctx;

	ctx->cb_fn(ctx->cb_arg, ctx->cb_rc);

	free(ctx);
}

static void
vbdev_compress_delete_done(void *cb_arg, int bdeverrno)
{
	struct vbdev_comp_delete_ctx *ctx = cb_arg;

	ctx->cb_rc = bdeverrno;

	if (ctx->orig_thread != spdk_get_thread()) {
		spdk_thread_send_msg(ctx->orig_thread, _vbdev_compress_delete_done, ctx);
	} else {
		_vbdev_compress_delete_done(ctx);
	}
}

void
bdev_compress_delete(const char *name, spdk_delete_compress_complete cb_fn, void *cb_arg)
{
	struct vbdev_compress *comp_bdev = NULL;
	struct vbdev_comp_delete_ctx *ctx;

	TAILQ_FOREACH(comp_bdev, &g_vbdev_comp, link) {
		if (strcmp(name, comp_bdev->comp_bdev.name) == 0) {
			break;
		}
	}

	if (comp_bdev == NULL) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate delete context\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	/* Save these for after the vol is destroyed. */
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->orig_thread = spdk_get_thread();

	comp_bdev->delete_ctx = ctx;

	/* Tell reducelib that we're done with this volume. */
	if (comp_bdev->orphaned == false) {
		spdk_reduce_vol_unload(comp_bdev->vol, delete_vol_unload_cb, comp_bdev);
	} else {
		delete_vol_unload_cb(comp_bdev, 0);
	}
}

static void
_vbdev_reduce_load_cb(void *ctx)
{
	struct vbdev_compress *meta_ctx = ctx;
	int rc;

	assert(meta_ctx->base_desc != NULL);

	/* Done with metadata operations */
	spdk_put_io_channel(meta_ctx->base_ch);

	if (meta_ctx->reduce_errno == 0) {
		if (_set_pmd(meta_ctx) == false) {
			SPDK_ERRLOG("could not find required pmd\n");
			goto err;
		}

		rc = vbdev_compress_claim(meta_ctx);
		if (rc != 0) {
			goto err;
		}
	} else if (meta_ctx->reduce_errno == -ENOENT) {
		if (_set_compbdev_name(meta_ctx)) {
			goto err;
		}

		/* Save the thread where the base device is opened */
		meta_ctx->thread = spdk_get_thread();

		meta_ctx->comp_bdev.module = &compress_if;
		pthread_mutex_init(&meta_ctx->reduce_lock, NULL);
		rc = spdk_bdev_module_claim_bdev(meta_ctx->base_bdev, meta_ctx->base_desc,
						 meta_ctx->comp_bdev.module);
		if (rc) {
			SPDK_ERRLOG("could not claim bdev %s\n", spdk_bdev_get_name(meta_ctx->base_bdev));
			free(meta_ctx->comp_bdev.name);
			goto err;
		}

		meta_ctx->orphaned = true;
		TAILQ_INSERT_TAIL(&g_vbdev_comp, meta_ctx, link);
	} else {
		if (meta_ctx->reduce_errno != -EILSEQ) {
			SPDK_ERRLOG("for vol %s, error %u\n",
				    spdk_bdev_get_name(meta_ctx->base_bdev), meta_ctx->reduce_errno);
		}
		goto err;
	}

	spdk_bdev_module_examine_done(&compress_if);
	return;

err:
	/* Close the underlying bdev on its same opened thread. */
	spdk_bdev_close(meta_ctx->base_desc);
	free(meta_ctx);
	spdk_bdev_module_examine_done(&compress_if);
}

/* Callback from reduce for then load is complete. We'll pass the vbdev_comp struct
 * used for initial metadata operations to claim where it will be further filled out
 * and added to the global list.
 */
static void
vbdev_reduce_load_cb(void *cb_arg, struct spdk_reduce_vol *vol, int reduce_errno)
{
	struct vbdev_compress *meta_ctx = cb_arg;

	if (reduce_errno == 0) {
		/* Update information following volume load. */
		meta_ctx->vol = vol;
		memcpy(&meta_ctx->params, spdk_reduce_vol_get_params(vol),
		       sizeof(struct spdk_reduce_vol_params));
	}

	meta_ctx->reduce_errno = reduce_errno;

	if (meta_ctx->thread && meta_ctx->thread != spdk_get_thread()) {
		spdk_thread_send_msg(meta_ctx->thread, _vbdev_reduce_load_cb, meta_ctx);
	} else {
		_vbdev_reduce_load_cb(meta_ctx);
	}

}

/* Examine_disk entry point: will do a metadata load to see if this is ours,
 * and if so will go ahead and claim it.
 */
static void
vbdev_compress_examine(struct spdk_bdev *bdev)
{
	struct spdk_bdev_desc *bdev_desc = NULL;
	struct vbdev_compress *meta_ctx;
	int rc;

	if (strcmp(bdev->product_name, COMP_BDEV_NAME) == 0) {
		spdk_bdev_module_examine_done(&compress_if);
		return;
	}

	rc = spdk_bdev_open_ext(spdk_bdev_get_name(bdev), false,
				vbdev_compress_base_bdev_event_cb, NULL, &bdev_desc);
	if (rc) {
		SPDK_ERRLOG("could not open bdev %s\n", spdk_bdev_get_name(bdev));
		spdk_bdev_module_examine_done(&compress_if);
		return;
	}

	meta_ctx = _prepare_for_load_init(bdev_desc, 0);
	if (meta_ctx == NULL) {
		spdk_bdev_close(bdev_desc);
		spdk_bdev_module_examine_done(&compress_if);
		return;
	}

	/* Save the thread where the base device is opened */
	meta_ctx->thread = spdk_get_thread();

	meta_ctx->base_ch = spdk_bdev_get_io_channel(meta_ctx->base_desc);
	spdk_reduce_vol_load(&meta_ctx->backing_dev, vbdev_reduce_load_cb, meta_ctx);
}

int
compress_set_pmd(enum compress_pmd *opts)
{
	g_opts = *opts;

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(vbdev_compress)
