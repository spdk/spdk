/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "accel_dpdk_compressdev.h"
#include "spdk/accel_module.h"

#include "spdk/stdinc.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/endian.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"
#include "spdk/likely.h"

#include "spdk/log.h"

#include <rte_config.h>
#include <rte_bus_vdev.h>
#include <rte_compressdev.h>
#include <rte_comp.h>
#include <rte_mbuf_dyn.h>

/* Used to store IO context in mbuf */
static const struct rte_mbuf_dynfield rte_mbuf_dynfield_io_context = {
	.name = "context_accel_comp",
	.size = sizeof(uint64_t),
	.align = __alignof__(uint64_t),
	.flags = 0,
};
static int g_mbuf_offset;
static enum compress_pmd g_opts;
static bool g_compressdev_enable = false;
static bool g_compressdev_initialized = false;

#define NUM_MAX_XFORMS		2
#define NUM_MAX_INFLIGHT_OPS	128
#define DEFAULT_WINDOW_SIZE	15
#define MBUF_SPLIT		(1UL << DEFAULT_WINDOW_SIZE)
#define QAT_PMD			"compress_qat"
#define MLX5_PMD		"mlx5_pci"
#define NUM_MBUFS		65536
#define POOL_CACHE_SIZE		256

/* Global list of available compression devices. */
struct compress_dev {
	struct rte_compressdev_info	cdev_info;	/* includes device friendly name */
	uint8_t				cdev_id;	/* identifier for the device */
	void				*comp_xform;	/* shared private xform for comp on this PMD */
	void				*decomp_xform;	/* shared private xform for decomp on this PMD */
	bool				sgl_in;
	bool				sgl_out;
	TAILQ_ENTRY(compress_dev)	link;
};
static TAILQ_HEAD(, compress_dev) g_compress_devs = TAILQ_HEAD_INITIALIZER(g_compress_devs);

#define MAX_NUM_QP 48
/* Global list and lock for unique device/queue pair combos */
struct comp_device_qp {
	struct compress_dev		*device;	/* ptr to compression device */
	uint8_t				qp;		/* queue pair for this node */
	struct compress_io_channel	*chan;
	TAILQ_ENTRY(comp_device_qp)	link;
};
static TAILQ_HEAD(, comp_device_qp) g_comp_device_qp = TAILQ_HEAD_INITIALIZER(g_comp_device_qp);
static pthread_mutex_t g_comp_device_qp_lock = PTHREAD_MUTEX_INITIALIZER;

struct compress_io_channel {
	char				*drv_name;	/* name of the compression device driver */
	struct comp_device_qp		*device_qp;
	struct spdk_poller		*poller;
	struct rte_mbuf			**src_mbufs;
	struct rte_mbuf			**dst_mbufs;
	TAILQ_HEAD(, spdk_accel_task)	queued_tasks;
};

/* Shared mempools between all devices on this system */
static struct rte_mempool *g_mbuf_mp = NULL;		/* mbuf mempool */
static struct rte_mempool *g_comp_op_mp = NULL;		/* comp operations, must be rte* mempool */
static struct rte_mbuf_ext_shared_info g_shinfo = {};	/* used by DPDK mbuf macros */
static bool g_qat_available = false;
static bool g_mlx5_pci_available = false;

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

/* Dummy function used by DPDK to free ext attached buffers
 * to mbufs, we free them ourselves but this callback has to
 * be here.
 */
static void
shinfo_free_cb(void *arg1, void *arg2)
{
}

/* Called by accel_init_compress_drivers() to init each discovered compression device */
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
		dev_qp->chan = NULL;
		TAILQ_INSERT_TAIL(&g_comp_device_qp, dev_qp, link);
	}

	TAILQ_INSERT_TAIL(&g_compress_devs, device, link);

	if (strcmp(device->cdev_info.driver_name, QAT_PMD) == 0) {
		g_qat_available = true;
	}

	if (strcmp(device->cdev_info.driver_name, MLX5_PMD) == 0) {
		g_mlx5_pci_available = true;
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

/* Called from driver init entry point, accel_compress_init() */
static int
accel_init_compress_drivers(void)
{
	uint8_t cdev_count, i;
	struct compress_dev *tmp_dev;
	struct compress_dev *device;
	int rc;

	/* If we have no compression devices, there's no reason to continue. */
	cdev_count = rte_compressdev_count();
	if (cdev_count == 0) {
		return 0;
	}
	if (cdev_count > RTE_COMPRESS_MAX_DEVS) {
		SPDK_ERRLOG("invalid device count from rte_compressdev_count()\n");
		return -EINVAL;
	}

	g_mbuf_offset = rte_mbuf_dynfield_register(&rte_mbuf_dynfield_io_context);
	if (g_mbuf_offset < 0) {
		SPDK_ERRLOG("error registering dynamic field with DPDK\n");
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

int
accel_compressdev_enable_probe(enum compress_pmd *opts)
{
	g_opts = *opts;
	g_compressdev_enable = true;

	return 0;
}

static int
_setup_compress_mbuf(struct rte_mbuf **mbufs, int *mbuf_total, uint64_t *total_length,
		     struct iovec *iovs, int iovcnt, struct spdk_accel_task *task)
{
	uint64_t iovec_length, updated_length, phys_addr;
	uint64_t processed, mbuf_length, remainder;
	uint8_t *current_base = NULL;
	int iov_index, mbuf_index;
	int rc = 0;

	/* Setup mbufs */
	iov_index = mbuf_index = 0;
	while (iov_index < iovcnt) {

		processed = 0;
		iovec_length = iovs[iov_index].iov_len;

		current_base = iovs[iov_index].iov_base;
		if (total_length) {
			*total_length += iovec_length;
		}

		assert(mbufs[mbuf_index] != NULL);
		*RTE_MBUF_DYNFIELD(mbufs[mbuf_index], g_mbuf_offset, uint64_t *) = (uint64_t)task;

		do {
			/* new length is min of remaining left or max mbuf size of MBUF_SPLIT */
			mbuf_length = updated_length = spdk_min(MBUF_SPLIT, iovec_length - processed);

			phys_addr = spdk_vtophys((void *)current_base, &updated_length);

			rte_pktmbuf_attach_extbuf(mbufs[mbuf_index],
						  current_base,
						  phys_addr,
						  updated_length,
						  &g_shinfo);
			rte_pktmbuf_append(mbufs[mbuf_index], updated_length);
			remainder = mbuf_length - updated_length;

			/* although the mbufs were preallocated, we still need to chain them */
			if (mbuf_index > 0) {
				rte_pktmbuf_chain(mbufs[0], mbufs[mbuf_index]);
			}

			/* keep track of the total we've put into the mbuf chain */
			processed += updated_length;
			/* bump the base by what was previously added */
			current_base += updated_length;

			/* If we crossed 2MB boundary we need another mbuf for the remainder */
			if (remainder > 0) {

				assert(remainder <= MBUF_SPLIT);

				/* allocate an mbuf at the end of the array */
				rc = rte_pktmbuf_alloc_bulk(g_mbuf_mp,
							    (struct rte_mbuf **)&mbufs[*mbuf_total], 1);
				if (rc) {
					SPDK_ERRLOG("ERROR trying to get an extra mbuf!\n");
					return -1;
				}
				(*mbuf_total)++;
				mbuf_index++;
				*RTE_MBUF_DYNFIELD(mbufs[mbuf_index], g_mbuf_offset, uint64_t *) = (uint64_t)task;

				/* bump the base by what was previously added */
				current_base += updated_length;

				updated_length = remainder;
				phys_addr = spdk_vtophys((void *)current_base, &updated_length);

				/* assert we don't cross another */
				assert(remainder == updated_length);

				rte_pktmbuf_attach_extbuf(mbufs[mbuf_index],
							  current_base,
							  phys_addr,
							  remainder,
							  &g_shinfo);
				rte_pktmbuf_append(mbufs[mbuf_index], remainder);
				rte_pktmbuf_chain(mbufs[0], mbufs[mbuf_index]);

				/* keep track of the total we've put into the mbuf chain */
				processed += remainder;
			}

			mbuf_index++;

		} while (processed < iovec_length);

		assert(processed == iovec_length);
		iov_index++;
	}

	return 0;
}

static int
_compress_operation(struct compress_io_channel *chan,  struct spdk_accel_task *task)
{
	int dst_iovcnt = task->d.iovcnt;
	struct iovec *dst_iovs = task->d.iovs;
	int src_iovcnt = task->s.iovcnt;
	struct iovec *src_iovs = task->s.iovs;
	struct rte_comp_op *comp_op;
	uint8_t cdev_id;
	uint64_t total_length = 0;
	int rc = 0, i;
	int src_mbuf_total = 0;
	int dst_mbuf_total = 0;
	bool device_error = false;
	bool compress = (task->op_code == ACCEL_OPC_COMPRESS);

	assert(chan->device_qp->device != NULL);
	cdev_id = chan->device_qp->device->cdev_id;

	/* calc our mbuf totals based on max MBUF size allowed so we can pre-alloc mbufs in bulk */
	for (i = 0 ; i < src_iovcnt; i++) {
		src_mbuf_total += spdk_divide_round_up(src_iovs[i].iov_len, MBUF_SPLIT);
	}
	for (i = 0 ; i < dst_iovcnt; i++) {
		dst_mbuf_total += spdk_divide_round_up(dst_iovs[i].iov_len, MBUF_SPLIT);
	}

	comp_op = rte_comp_op_alloc(g_comp_op_mp);
	if (!comp_op) {
		SPDK_ERRLOG("trying to get a comp op!\n");
		rc = -ENOMEM;
		goto error_get_op;
	}

	/* get an mbuf per iov, src and dst */
	rc = rte_pktmbuf_alloc_bulk(g_mbuf_mp, chan->src_mbufs, src_mbuf_total);
	if (rc) {
		SPDK_ERRLOG("ERROR trying to get src_mbufs!\n");
		rc = -ENOMEM;
		goto error_get_src;
	}
	assert(chan->src_mbufs[0]);

	rc = rte_pktmbuf_alloc_bulk(g_mbuf_mp, chan->dst_mbufs, dst_mbuf_total);
	if (rc) {
		SPDK_ERRLOG("ERROR trying to get dst_mbufs!\n");
		rc = -ENOMEM;
		goto error_get_dst;
	}
	assert(chan->dst_mbufs[0]);

	rc = _setup_compress_mbuf(chan->src_mbufs, &src_mbuf_total, &total_length,
				  src_iovs, src_iovcnt, task);

	if (rc < 0) {
		goto error_src_dst;
	}
	if (!chan->device_qp->device->sgl_in && src_mbuf_total > 1) {
		SPDK_ERRLOG("Src buffer uses chained mbufs but driver %s doesn't support SGL input\n",
			    chan->drv_name);
		rc = -EINVAL;
		goto error_src_dst;
	}

	comp_op->m_src = chan->src_mbufs[0];
	comp_op->src.offset = 0;
	comp_op->src.length = total_length;

	rc = _setup_compress_mbuf(chan->dst_mbufs, &dst_mbuf_total, NULL,
				  dst_iovs, dst_iovcnt, task);
	if (rc < 0) {
		goto error_src_dst;
	}
	if (!chan->device_qp->device->sgl_out && dst_mbuf_total > 1) {
		SPDK_ERRLOG("Dst buffer uses chained mbufs but driver %s doesn't support SGL output\n",
			    chan->drv_name);
		rc = -EINVAL;
		goto error_src_dst;
	}

	comp_op->m_dst = chan->dst_mbufs[0];
	comp_op->dst.offset = 0;

	if (compress == true) {
		comp_op->private_xform = chan->device_qp->device->comp_xform;
	} else {
		comp_op->private_xform = chan->device_qp->device->decomp_xform;
	}

	comp_op->op_type = RTE_COMP_OP_STATELESS;
	comp_op->flush_flag = RTE_COMP_FLUSH_FINAL;

	rc = rte_compressdev_enqueue_burst(cdev_id, chan->device_qp->qp, &comp_op, 1);
	assert(rc <= 1);

	/* We always expect 1 got queued, if 0 then we need to queue it up. */
	if (rc == 1) {
		return 0;
	} else if (comp_op->status == RTE_COMP_OP_STATUS_NOT_PROCESSED) {
		rc = -EAGAIN;
	} else {
		device_error = true;
	}

	/* Error cleanup paths. */
error_src_dst:
	rte_pktmbuf_free_bulk(chan->dst_mbufs, dst_iovcnt);
error_get_dst:
	rte_pktmbuf_free_bulk(chan->src_mbufs, src_iovcnt);
error_get_src:
	rte_comp_op_free(comp_op);
error_get_op:

	if (device_error == true) {
		/* There was an error sending the op to the device, most
		 * likely with the parameters.
		 */
		SPDK_ERRLOG("Compression API returned 0x%x\n", comp_op->status);
		return -EINVAL;
	}
	if (rc != -ENOMEM && rc != -EAGAIN) {
		return rc;
	}

	TAILQ_INSERT_TAIL(&chan->queued_tasks, task, link);
	return 0;
}

/* Poller for the DPDK compression driver. */
static int
comp_dev_poller(void *args)
{
	struct compress_io_channel *chan = args;
	uint8_t cdev_id;
	struct rte_comp_op *deq_ops[NUM_MAX_INFLIGHT_OPS];
	uint16_t num_deq;
	struct spdk_accel_task *task, *task_to_resubmit;
	int rc, i, status;

	assert(chan->device_qp->device != NULL);
	cdev_id = chan->device_qp->device->cdev_id;

	num_deq = rte_compressdev_dequeue_burst(cdev_id, chan->device_qp->qp, deq_ops,
						NUM_MAX_INFLIGHT_OPS);
	for (i = 0; i < num_deq; i++) {

		/* We store this off regardless of success/error so we know how to contruct the
		 * next task
		 */
		task = (struct spdk_accel_task *)*RTE_MBUF_DYNFIELD(deq_ops[i]->m_src, g_mbuf_offset,
				uint64_t *);
		status = deq_ops[i]->status;

		if (spdk_likely(status == RTE_COMP_OP_STATUS_SUCCESS)) {
			if (task->output_size != NULL) {
				*task->output_size = deq_ops[i]->produced;
			}
		} else {
			SPDK_NOTICELOG("Deque status %u\n", status);
		}

		spdk_accel_task_complete(task, status);

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
		if (!TAILQ_EMPTY(&chan->queued_tasks)) {
			task_to_resubmit = TAILQ_FIRST(&chan->queued_tasks);
			rc = _compress_operation(chan, task_to_resubmit);
			if (rc == 0) {
				TAILQ_REMOVE(&chan->queued_tasks, task_to_resubmit, link);
			}
		}
	}

	return num_deq == 0 ? SPDK_POLLER_IDLE : SPDK_POLLER_BUSY;
}

static int
_process_single_task(struct spdk_io_channel *ch, struct spdk_accel_task *task)
{
	struct compress_io_channel *chan = spdk_io_channel_get_ctx(ch);
	int rc;

	rc = _compress_operation(chan, task);
	if (rc) {
		SPDK_ERRLOG("Error (%d) in comrpess operation\n", rc);
		assert(false);
	}

	return rc;
}

static int
compress_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task *first_task)
{
	struct compress_io_channel *chan = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task, *tmp;
	int rc = 0;

	task = first_task;

	if (!TAILQ_EMPTY(&chan->queued_tasks)) {
		goto queue_tasks;
	}

	/* The caller will either submit a single task or a group of tasks that are
	 * linked together but they cannot be on a list. For example, see poller
	 * where a list of queued tasks is being resubmitted, the list they are on
	 * is initialized after saving off the first task from the list which is then
	 * passed in here.  Similar thing is done in the accel framework.
	 */
	while (task) {
		tmp = TAILQ_NEXT(task, link);
		rc = _process_single_task(ch, task);

		if (rc == -EBUSY) {
			goto queue_tasks;
		} else if (rc) {
			spdk_accel_task_complete(task, rc);
		}
		task = tmp;
	}

	return 0;

queue_tasks:
	while (task != NULL) {
		tmp = TAILQ_NEXT(task, link);
		TAILQ_INSERT_TAIL(&chan->queued_tasks, task, link);
		task = tmp;
	}
	return 0;
}

static bool
_set_pmd(struct compress_io_channel *chan)
{

	/* Note: the compress_isal PMD is not supported as accel_fw supports native ISAL
	 * using the accel_sw module */
	if (g_opts == COMPRESS_PMD_AUTO) {
		if (g_qat_available) {
			chan->drv_name = QAT_PMD;
		} else if (g_mlx5_pci_available) {
			chan->drv_name = MLX5_PMD;
		}
	} else if (g_opts == COMPRESS_PMD_QAT_ONLY && g_qat_available) {
		chan->drv_name = QAT_PMD;
	} else if (g_opts == COMPRESS_PMD_MLX5_PCI_ONLY && g_mlx5_pci_available) {
		chan->drv_name = MLX5_PMD;
	} else {
		SPDK_ERRLOG("Requested PMD is not available.\n");
		return false;
	}
	SPDK_NOTICELOG("Channel %p PMD being used: %s\n", chan, chan->drv_name);
	return true;
}

static int compress_create_cb(void *io_device, void *ctx_buf);
static void compress_destroy_cb(void *io_device, void *ctx_buf);
static struct spdk_accel_module_if g_compress_module;
static int
accel_compress_init(void)
{
	int rc;

	if (!g_compressdev_enable) {
		return -EINVAL;
	}

	rc = accel_init_compress_drivers();
	if (rc) {
		assert(TAILQ_EMPTY(&g_compress_devs));
		SPDK_NOTICELOG("no available compression devices\n");
		return -EINVAL;
	}

	g_compressdev_initialized = true;
	SPDK_NOTICELOG("Accel framework compressdev module initialized.\n");
	spdk_io_device_register(&g_compress_module, compress_create_cb, compress_destroy_cb,
				sizeof(struct compress_io_channel), "compressdev_accel_module");
	return 0;

}

static int
compress_create_cb(void *io_device, void *ctx_buf)
{
	struct compress_io_channel *chan = ctx_buf;
	const struct rte_compressdev_capabilities *capab;
	struct comp_device_qp *device_qp;
	size_t length;

	if (_set_pmd(chan) == false) {
		assert(false);
		return -ENODEV;
	}

	/* The following variable length arrays of mbuf pointers are required to submit to compressdev */
	length = NUM_MBUFS * sizeof(void *);
	chan->src_mbufs = spdk_zmalloc(length, 0x40, NULL,
				       SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (chan->src_mbufs == NULL) {
		return -ENOMEM;
	}
	chan->dst_mbufs = spdk_zmalloc(length, 0x40, NULL,
				       SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (chan->dst_mbufs == NULL) {
		free(chan->src_mbufs);
		return -ENOMEM;
	}

	chan->poller = SPDK_POLLER_REGISTER(comp_dev_poller, chan, 0);
	TAILQ_INIT(&chan->queued_tasks);

	pthread_mutex_lock(&g_comp_device_qp_lock);
	TAILQ_FOREACH(device_qp, &g_comp_device_qp, link) {
		if (strcmp(device_qp->device->cdev_info.driver_name, chan->drv_name) == 0) {
			if (device_qp->chan == NULL) {
				chan->device_qp = device_qp;
				device_qp->chan = chan;
				break;
			}
		}
	}
	pthread_mutex_unlock(&g_comp_device_qp_lock);

	if (chan->device_qp == NULL) {
		SPDK_ERRLOG("out of qpairs, cannot assign one\n");
		assert(false);
		return -ENOMEM;
	} else {
		capab = rte_compressdev_capability_get(0, RTE_COMP_ALGO_DEFLATE);

		if (capab->comp_feature_flags & (RTE_COMP_FF_OOP_SGL_IN_SGL_OUT | RTE_COMP_FF_OOP_SGL_IN_LB_OUT)) {
			chan->device_qp->device->sgl_in = true;
		}

		if (capab->comp_feature_flags & (RTE_COMP_FF_OOP_SGL_IN_SGL_OUT | RTE_COMP_FF_OOP_LB_IN_SGL_OUT)) {
			chan->device_qp->device->sgl_out = true;
		}
	}

	return 0;
}

static void
accel_compress_write_config_json(struct spdk_json_write_ctx *w)
{
	if (g_compressdev_enable) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "compressdev_scan_accel_module");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_uint32(w, "pmd", g_opts);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
}

static void
compress_destroy_cb(void *io_device, void *ctx_buf)
{
	struct compress_io_channel *chan = ctx_buf;
	struct comp_device_qp *device_qp = chan->device_qp;

	spdk_free(chan->src_mbufs);
	spdk_free(chan->dst_mbufs);

	spdk_poller_unregister(&chan->poller);

	pthread_mutex_lock(&g_comp_device_qp_lock);
	chan->device_qp = NULL;
	device_qp->chan = NULL;
	pthread_mutex_unlock(&g_comp_device_qp_lock);
}

static size_t
accel_compress_get_ctx_size(void)
{
	return 0;
}

static bool
compress_supports_opcode(enum accel_opcode opc)
{
	if (g_mlx5_pci_available || g_qat_available) {
		switch (opc) {
		case ACCEL_OPC_COMPRESS:
		case ACCEL_OPC_DECOMPRESS:
			return true;
		default:
			break;
		}
	}

	return false;
}

static struct spdk_io_channel *
compress_get_io_channel(void)
{
	return spdk_get_io_channel(&g_compress_module);
}

static void accel_compress_exit(void *ctx);
static struct spdk_accel_module_if g_compress_module = {
	.module_init		= accel_compress_init,
	.module_fini		= accel_compress_exit,
	.write_config_json	= accel_compress_write_config_json,
	.get_ctx_size		= accel_compress_get_ctx_size,
	.name			= "dpdk_compressdev",
	.supports_opcode	= compress_supports_opcode,
	.get_io_channel		= compress_get_io_channel,
	.submit_tasks		= compress_submit_tasks
};

void
accel_dpdk_compressdev_enable(void)
{
	spdk_accel_module_list_add(&g_compress_module);
}

/* Callback for unregistering the IO device. */
static void
_device_unregister_cb(void *io_device)
{
	struct comp_device_qp *dev_qp;
	struct compress_dev *device;

	while ((device = TAILQ_FIRST(&g_compress_devs))) {
		TAILQ_REMOVE(&g_compress_devs, device, link);
		free(device);
	}

	while ((dev_qp = TAILQ_FIRST(&g_comp_device_qp))) {
		TAILQ_REMOVE(&g_comp_device_qp, dev_qp, link);
		free(dev_qp);
	}

	pthread_mutex_destroy(&g_comp_device_qp_lock);

	rte_mempool_free(g_comp_op_mp);
	rte_mempool_free(g_mbuf_mp);

	spdk_accel_module_finish();
}

static void
accel_compress_exit(void *ctx)
{
	if (g_compressdev_initialized) {
		spdk_io_device_unregister(&g_compress_module, _device_unregister_cb);
		g_compressdev_initialized = false;
	} else {
		spdk_accel_module_finish();
	}
}
