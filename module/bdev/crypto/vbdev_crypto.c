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
 *   DATA, OR PROFITS; OR BUSINESS INTERRUcryptoION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "vbdev_crypto.h"

#include "spdk/env.h"
#include "spdk/endian.h"
#include "spdk/thread.h"
#include "spdk/bdev_module.h"
#include "spdk/log.h"

#include <rte_config.h>
#include <rte_bus_vdev.h>
#include <rte_crypto.h>
#include <rte_cryptodev.h>
#include <rte_mbuf_dyn.h>

/* Used to store IO context in mbuf */
static const struct rte_mbuf_dynfield rte_mbuf_dynfield_io_context = {
	.name = "context_bdev_io",
	.size = sizeof(uint64_t),
	.align = __alignof__(uint64_t),
	.flags = 0,
};
static int g_mbuf_offset;

/* To add support for new device types, follow the examples of the following...
 * Note that the string names are defined by the DPDK PMD in question so be
 * sure to use the exact names.
 */
#define MAX_NUM_DRV_TYPES 2

/* The VF spread is the number of queue pairs between virtual functions, we use this to
 * load balance the QAT device.
 */
#define QAT_VF_SPREAD 32
static uint8_t g_qat_total_qp = 0;
static uint8_t g_next_qat_index;

const char *g_driver_names[MAX_NUM_DRV_TYPES] = { AESNI_MB, QAT };

/* Global list of available crypto devices. */
struct vbdev_dev {
	struct rte_cryptodev_info	cdev_info;	/* includes device friendly name */
	uint8_t				cdev_id;	/* identifier for the device */
	TAILQ_ENTRY(vbdev_dev)		link;
};
static TAILQ_HEAD(, vbdev_dev) g_vbdev_devs = TAILQ_HEAD_INITIALIZER(g_vbdev_devs);

/* Global list and lock for unique device/queue pair combos. We keep 1 list per supported PMD
 * so that we can optimize per PMD where it make sense. For example, with QAT there an optimal
 * pattern for assigning queue pairs where with AESNI there is not.
 */
struct device_qp {
	struct vbdev_dev		*device;	/* ptr to crypto device */
	uint8_t				qp;		/* queue pair for this node */
	bool				in_use;		/* whether this node is in use or not */
	uint8_t				index;		/* used by QAT to load balance placement of qpairs */
	TAILQ_ENTRY(device_qp)		link;
};
static TAILQ_HEAD(, device_qp) g_device_qp_qat = TAILQ_HEAD_INITIALIZER(g_device_qp_qat);
static TAILQ_HEAD(, device_qp) g_device_qp_aesni_mb = TAILQ_HEAD_INITIALIZER(g_device_qp_aesni_mb);
static pthread_mutex_t g_device_qp_lock = PTHREAD_MUTEX_INITIALIZER;


/* In order to limit the number of resources we need to do one crypto
 * operation per LBA (we use LBA as IV), we tell the bdev layer that
 * our max IO size is something reasonable. Units here are in bytes.
 */
#define CRYPTO_MAX_IO		(64 * 1024)

/* This controls how many ops will be dequeued from the crypto driver in one run
 * of the poller. It is mainly a performance knob as it effectively determines how
 * much work the poller has to do.  However even that can vary between crypto drivers
 * as the AESNI_MB driver for example does all the crypto work on dequeue whereas the
 * QAT driver just dequeues what has been completed already.
 */
#define MAX_DEQUEUE_BURST_SIZE	64

/* When enqueueing, we need to supply the crypto driver with an array of pointers to
 * operation structs. As each of these can be max 512B, we can adjust the CRYPTO_MAX_IO
 * value in conjunction with the other defines to make sure we're not using crazy amounts
 * of memory. All of these numbers can and probably should be adjusted based on the
 * workload. By default we'll use the worst case (smallest) block size for the
 * minimum number of array entries. As an example, a CRYPTO_MAX_IO size of 64K with 512B
 * blocks would give us an enqueue array size of 128.
 */
#define MAX_ENQUEUE_ARRAY_SIZE (CRYPTO_MAX_IO / 512)

/* The number of MBUFS we need must be a power of two and to support other small IOs
 * in addition to the limits mentioned above, we go to the next power of two. It is
 * big number because it is one mempool for source and destination mbufs. It may
 * need to be bigger to support multiple crypto drivers at once.
 */
#define NUM_MBUFS		32768
#define POOL_CACHE_SIZE		256
#define MAX_CRYPTO_VOLUMES	128
#define NUM_SESSIONS		(2 * MAX_CRYPTO_VOLUMES)
#define SESS_MEMPOOL_CACHE_SIZE 0
uint8_t g_number_of_claimed_volumes = 0;

/* This is the max number of IOs we can supply to any crypto device QP at one time.
 * It can vary between drivers.
 */
#define CRYPTO_QP_DESCRIPTORS	2048

/* Specific to AES_CBC. */
#define AES_CBC_IV_LENGTH	16
#define AES_CBC_KEY_LENGTH	16
#define AES_XTS_KEY_LENGTH	16	/* XTS uses 2 keys, each of this size. */
#define AESNI_MB_NUM_QP		64

/* Common for suported devices. */
#define IV_OFFSET            (sizeof(struct rte_crypto_op) + \
				sizeof(struct rte_crypto_sym_op))
#define QUEUED_OP_OFFSET (IV_OFFSET + AES_CBC_IV_LENGTH)

static void _complete_internal_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void _complete_internal_read(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void _complete_internal_write(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void vbdev_crypto_examine(struct spdk_bdev *bdev);
static int vbdev_crypto_claim(const char *bdev_name);
static void vbdev_crypto_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);

/* List of crypto_bdev names and their base bdevs via configuration file. */
struct bdev_names {
	char			*vbdev_name;	/* name of the vbdev to create */
	char			*bdev_name;	/* base bdev name */

	/* Note, for dev/test we allow use of key in the config file, for production
	 * use, you must use an RPC to specify the key for security reasons.
	 */
	uint8_t			*key;		/* key per bdev */
	char			*drv_name;	/* name of the crypto device driver */
	char			*cipher;	/* AES_CBC or AES_XTS */
	uint8_t			*key2;		/* key #2 for AES_XTS, per bdev */
	TAILQ_ENTRY(bdev_names)	link;
};
static TAILQ_HEAD(, bdev_names) g_bdev_names = TAILQ_HEAD_INITIALIZER(g_bdev_names);

/* List of virtual bdevs and associated info for each. We keep the device friendly name here even
 * though its also in the device struct because we use it early on.
 */
struct vbdev_crypto {
	struct spdk_bdev		*base_bdev;		/* the thing we're attaching to */
	struct spdk_bdev_desc		*base_desc;		/* its descriptor we get from open */
	struct spdk_bdev		crypto_bdev;		/* the crypto virtual bdev */
	uint8_t				*key;			/* key per bdev */
	uint8_t				*key2;			/* for XTS */
	uint8_t				*xts_key;		/* key + key 2 */
	char				*drv_name;		/* name of the crypto device driver */
	char				*cipher;		/* cipher used */
	struct rte_cryptodev_sym_session *session_encrypt;	/* encryption session for this bdev */
	struct rte_cryptodev_sym_session *session_decrypt;	/* decryption session for this bdev */
	struct rte_crypto_sym_xform	cipher_xform;		/* crypto control struct for this bdev */
	TAILQ_ENTRY(vbdev_crypto)	link;
	struct spdk_thread		*thread;		/* thread where base device is opened */
};
static TAILQ_HEAD(, vbdev_crypto) g_vbdev_crypto = TAILQ_HEAD_INITIALIZER(g_vbdev_crypto);

/* Shared mempools between all devices on this system */
static struct rte_mempool *g_session_mp = NULL;
static struct rte_mempool *g_session_mp_priv = NULL;
static struct rte_mempool *g_mbuf_mp = NULL;            /* mbuf mempool */
static struct rte_mempool *g_crypto_op_mp = NULL;	/* crypto operations, must be rte* mempool */

static struct rte_mbuf_ext_shared_info g_shinfo = {};   /* used by DPDK mbuf macro */

/* For queueing up crypto operations that we can't submit for some reason */
struct vbdev_crypto_op {
	uint8_t					cdev_id;
	uint8_t					qp;
	struct rte_crypto_op			*crypto_op;
	struct spdk_bdev_io			*bdev_io;
	TAILQ_ENTRY(vbdev_crypto_op)		link;
};
#define QUEUED_OP_LENGTH (sizeof(struct vbdev_crypto_op))

/* The crypto vbdev channel struct. It is allocated and freed on my behalf by the io channel code.
 * We store things in here that are needed on per thread basis like the base_channel for this thread,
 * and the poller for this thread.
 */
struct crypto_io_channel {
	struct spdk_io_channel		*base_ch;		/* IO channel of base device */
	struct spdk_poller		*poller;		/* completion poller */
	struct device_qp		*device_qp;		/* unique device/qp combination for this channel */
	TAILQ_HEAD(, spdk_bdev_io)	pending_cry_ios;	/* outstanding operations to the crypto device */
	struct spdk_io_channel_iter	*iter;			/* used with for_each_channel in reset */
	TAILQ_HEAD(, vbdev_crypto_op)	queued_cry_ops;		/* queued for re-submission to CryptoDev */
};

/* This is the crypto per IO context that the bdev layer allocates for us opaquely and attaches to
 * each IO for us.
 */
struct crypto_bdev_io {
	int cryop_cnt_remaining;			/* counter used when completing crypto ops */
	struct crypto_io_channel *crypto_ch;		/* need to store for crypto completion handling */
	struct vbdev_crypto *crypto_bdev;		/* the crypto node struct associated with this IO */
	struct spdk_bdev_io *orig_io;			/* the original IO */
	struct spdk_bdev_io *read_io;			/* the read IO we issued */
	int8_t bdev_io_status;				/* the status we'll report back on the bdev IO */
	bool on_pending_list;
	/* Used for the single contiguous buffer that serves as the crypto destination target for writes */
	uint64_t aux_num_blocks;			/* num of blocks for the contiguous buffer */
	uint64_t aux_offset_blocks;			/* block offset on media */
	void *aux_buf_raw;				/* raw buffer that the bdev layer gave us for write buffer */
	struct iovec aux_buf_iov;			/* iov representing aligned contig write buffer */

	/* for bdev_io_wait */
	struct spdk_bdev_io_wait_entry bdev_io_wait;
	struct spdk_io_channel *ch;
};

/* Called by vbdev_crypto_init_crypto_drivers() to init each discovered crypto device */
static int
create_vbdev_dev(uint8_t index, uint16_t num_lcores)
{
	struct vbdev_dev *device;
	uint8_t j, cdev_id, cdrv_id;
	struct device_qp *dev_qp;
	struct device_qp *tmp_qp;
	int rc;
	TAILQ_HEAD(device_qps, device_qp) *dev_qp_head;

	device = calloc(1, sizeof(struct vbdev_dev));
	if (!device) {
		return -ENOMEM;
	}

	/* Get details about this device. */
	rte_cryptodev_info_get(index, &device->cdev_info);
	cdrv_id = device->cdev_info.driver_id;
	cdev_id = device->cdev_id = index;

	/* QAT_ASYM devices are not supported at this time. */
	if (strcmp(device->cdev_info.driver_name, QAT_ASYM) == 0) {
		free(device);
		return 0;
	}

	/* Before going any further, make sure we have enough resources for this
	 * device type to function.  We need a unique queue pair per core accross each
	 * device type to remain lockless....
	 */
	if ((rte_cryptodev_device_count_by_driver(cdrv_id) *
	     device->cdev_info.max_nb_queue_pairs) < num_lcores) {
		SPDK_ERRLOG("Insufficient unique queue pairs available for %s\n",
			    device->cdev_info.driver_name);
		SPDK_ERRLOG("Either add more crypto devices or decrease core count\n");
		rc = -EINVAL;
		goto err;
	}

	/* Setup queue pairs. */
	struct rte_cryptodev_config conf = {
		.nb_queue_pairs = device->cdev_info.max_nb_queue_pairs,
		.socket_id = SPDK_ENV_SOCKET_ID_ANY
	};

	rc = rte_cryptodev_configure(cdev_id, &conf);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to configure cryptodev %u\n", cdev_id);
		rc = -EINVAL;
		goto err;
	}

	struct rte_cryptodev_qp_conf qp_conf = {
		.nb_descriptors = CRYPTO_QP_DESCRIPTORS,
		.mp_session = g_session_mp,
		.mp_session_private = g_session_mp_priv,
	};

	/* Pre-setup all potential qpairs now and assign them in the channel
	 * callback. If we were to create them there, we'd have to stop the
	 * entire device affecting all other threads that might be using it
	 * even on other queue pairs.
	 */
	for (j = 0; j < device->cdev_info.max_nb_queue_pairs; j++) {
		rc = rte_cryptodev_queue_pair_setup(cdev_id, j, &qp_conf, SOCKET_ID_ANY);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to setup queue pair %u on "
				    "cryptodev %u\n", j, cdev_id);
			rc = -EINVAL;
			goto err;
		}
	}

	rc = rte_cryptodev_start(cdev_id);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to start device %u: error %d\n",
			    cdev_id, rc);
		rc = -EINVAL;
		goto err;
	}

	/* Select the right device/qp list based on driver name
	 * or error if it does not exist.
	 */
	if (strcmp(device->cdev_info.driver_name, QAT) == 0) {
		dev_qp_head = (struct device_qps *)&g_device_qp_qat;
	} else if (strcmp(device->cdev_info.driver_name, AESNI_MB) == 0) {
		dev_qp_head = (struct device_qps *)&g_device_qp_aesni_mb;
	} else {
		rc = -EINVAL;
		goto err;
	}

	/* Build up lists of device/qp combinations per PMD */
	for (j = 0; j < device->cdev_info.max_nb_queue_pairs; j++) {
		dev_qp = calloc(1, sizeof(struct device_qp));
		if (!dev_qp) {
			rc = -ENOMEM;
			goto err_qp_alloc;
		}
		dev_qp->device = device;
		dev_qp->qp = j;
		dev_qp->in_use = false;
		if (strcmp(device->cdev_info.driver_name, QAT) == 0) {
			g_qat_total_qp++;
		}
		TAILQ_INSERT_TAIL(dev_qp_head, dev_qp, link);
	}

	/* Add to our list of available crypto devices. */
	TAILQ_INSERT_TAIL(&g_vbdev_devs, device, link);

	return 0;
err_qp_alloc:
	TAILQ_FOREACH_SAFE(dev_qp, dev_qp_head, link, tmp_qp) {
		TAILQ_REMOVE(dev_qp_head, dev_qp, link);
		free(dev_qp);
	}
err:
	free(device);

	return rc;
}

/* Dummy function used by DPDK to free ext attached buffers to mbufs, we free them ourselves but
 * this callback has to be here. */
static void shinfo_free_cb(void *arg1, void *arg2)
{
}

/* This is called from the module's init function. We setup all crypto devices early on as we are unable
 * to easily dynamically configure queue pairs after the drivers are up and running.  So, here, we
 * configure the max capabilities of each device and assign threads to queue pairs as channels are
 * requested.
 */
static int
vbdev_crypto_init_crypto_drivers(void)
{
	uint8_t cdev_count;
	uint8_t cdev_id;
	int i, rc = 0;
	struct vbdev_dev *device;
	struct vbdev_dev *tmp_dev;
	struct device_qp *dev_qp;
	unsigned int max_sess_size = 0, sess_size;
	uint16_t num_lcores = rte_lcore_count();
	char aesni_args[32];

	/* Only the first call, via RPC or module init should init the crypto drivers. */
	if (g_session_mp != NULL) {
		return 0;
	}

	/* We always init AESNI_MB */
	snprintf(aesni_args, sizeof(aesni_args), "max_nb_queue_pairs=%d", AESNI_MB_NUM_QP);
	rc = rte_vdev_init(AESNI_MB, aesni_args);
	if (rc) {
		SPDK_ERRLOG("error creating virtual PMD %s\n", AESNI_MB);
		return -EINVAL;
	}

	/* If we have no crypto devices, there's no reason to continue. */
	cdev_count = rte_cryptodev_count();
	if (cdev_count == 0) {
		return 0;
	}

	g_mbuf_offset = rte_mbuf_dynfield_register(&rte_mbuf_dynfield_io_context);
	if (g_mbuf_offset < 0) {
		SPDK_ERRLOG("error registering dynamic field with DPDK\n");
		return -EINVAL;
	}

	/*
	 * Create global mempools, shared by all devices regardless of type.
	 */

	/* First determine max session size, most pools are shared by all the devices,
	 * so we need to find the global max sessions size.
	 */
	for (cdev_id = 0; cdev_id < cdev_count; cdev_id++) {
		sess_size = rte_cryptodev_sym_get_private_session_size(cdev_id);
		if (sess_size > max_sess_size) {
			max_sess_size = sess_size;
		}
	}

	g_session_mp_priv = rte_mempool_create("session_mp_priv", NUM_SESSIONS, max_sess_size,
					       SESS_MEMPOOL_CACHE_SIZE, 0, NULL, NULL, NULL,
					       NULL, SOCKET_ID_ANY, 0);
	if (g_session_mp_priv == NULL) {
		SPDK_ERRLOG("Cannot create private session pool max size 0x%x\n", max_sess_size);
		return -ENOMEM;
	}

	g_session_mp = rte_cryptodev_sym_session_pool_create(
			       "session_mp",
			       NUM_SESSIONS, 0, SESS_MEMPOOL_CACHE_SIZE, 0,
			       SOCKET_ID_ANY);
	if (g_session_mp == NULL) {
		SPDK_ERRLOG("Cannot create session pool max size 0x%x\n", max_sess_size);
		rc = -ENOMEM;
		goto error_create_session_mp;
	}

	g_mbuf_mp = rte_pktmbuf_pool_create("mbuf_mp", NUM_MBUFS, POOL_CACHE_SIZE,
					    0, 0, SPDK_ENV_SOCKET_ID_ANY);
	if (g_mbuf_mp == NULL) {
		SPDK_ERRLOG("Cannot create mbuf pool\n");
		rc = -ENOMEM;
		goto error_create_mbuf;
	}

	/* We use per op private data to store the IV and our own struct
	 * for queueing ops.
	 */
	g_crypto_op_mp = rte_crypto_op_pool_create("op_mp",
			 RTE_CRYPTO_OP_TYPE_SYMMETRIC,
			 NUM_MBUFS,
			 POOL_CACHE_SIZE,
			 AES_CBC_IV_LENGTH + QUEUED_OP_LENGTH,
			 rte_socket_id());

	if (g_crypto_op_mp == NULL) {
		SPDK_ERRLOG("Cannot create op pool\n");
		rc = -ENOMEM;
		goto error_create_op;
	}

	/* Init all devices */
	for (i = 0; i < cdev_count; i++) {
		rc = create_vbdev_dev(i, num_lcores);
		if (rc) {
			goto err;
		}
	}

	/* Assign index values to the QAT device qp nodes so that we can
	 * assign them for optimal performance.
	 */
	i = 0;
	TAILQ_FOREACH(dev_qp, &g_device_qp_qat, link) {
		dev_qp->index = i++;
	}

	g_shinfo.free_cb = shinfo_free_cb;
	return 0;

	/* Error cleanup paths. */
err:
	TAILQ_FOREACH_SAFE(device, &g_vbdev_devs, link, tmp_dev) {
		TAILQ_REMOVE(&g_vbdev_devs, device, link);
		free(device);
	}
	rte_mempool_free(g_crypto_op_mp);
	g_crypto_op_mp = NULL;
error_create_op:
	rte_mempool_free(g_mbuf_mp);
	g_mbuf_mp = NULL;
error_create_mbuf:
	rte_mempool_free(g_session_mp);
	g_session_mp = NULL;
error_create_session_mp:
	if (g_session_mp_priv != NULL) {
		rte_mempool_free(g_session_mp_priv);
		g_session_mp_priv = NULL;
	}
	return rc;
}

/* Following an encrypt or decrypt we need to then either write the encrypted data or finish
 * the read on decrypted data. Do that here.
 */
static void
_crypto_operation_complete(struct spdk_bdev_io *bdev_io)
{
	struct vbdev_crypto *crypto_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_crypto,
					   crypto_bdev);
	struct crypto_bdev_io *io_ctx = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	struct crypto_io_channel *crypto_ch = io_ctx->crypto_ch;
	struct spdk_bdev_io *free_me = io_ctx->read_io;
	int rc = 0;

	/* Can also be called from the crypto_dev_poller() to fail the stuck re-enqueue ops IO. */
	if (io_ctx->on_pending_list) {
		TAILQ_REMOVE(&crypto_ch->pending_cry_ios, bdev_io, module_link);
		io_ctx->on_pending_list = false;
	}

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {

		/* Complete the original IO and then free the one that we created
		 * as a result of issuing an IO via submit_request.
		 */
		if (io_ctx->bdev_io_status != SPDK_BDEV_IO_STATUS_FAILED) {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		} else {
			SPDK_ERRLOG("Issue with decryption on bdev_io %p\n", bdev_io);
			rc = -EINVAL;
		}
		spdk_bdev_free_io(free_me);

	} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {

		if (io_ctx->bdev_io_status != SPDK_BDEV_IO_STATUS_FAILED) {
			/* Write the encrypted data. */
			rc = spdk_bdev_writev_blocks(crypto_bdev->base_desc, crypto_ch->base_ch,
						     &io_ctx->aux_buf_iov, 1, io_ctx->aux_offset_blocks,
						     io_ctx->aux_num_blocks, _complete_internal_write,
						     bdev_io);
		} else {
			SPDK_ERRLOG("Issue with encryption on bdev_io %p\n", bdev_io);
			rc = -EINVAL;
		}

	} else {
		SPDK_ERRLOG("Unknown bdev type %u on crypto operation completion\n",
			    bdev_io->type);
		rc = -EINVAL;
	}

	if (rc) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
cancel_queued_crypto_ops(struct crypto_io_channel *crypto_ch, struct spdk_bdev_io *bdev_io)
{
	struct rte_mbuf *mbufs_to_free[2 * MAX_DEQUEUE_BURST_SIZE];
	struct rte_crypto_op *dequeued_ops[MAX_DEQUEUE_BURST_SIZE];
	struct vbdev_crypto_op *op_to_cancel, *tmp_op;
	struct rte_crypto_op *crypto_op;
	int num_mbufs, num_dequeued_ops;

	/* Remove all ops from the failed IO. Since we don't know the
	 * order we have to check them all. */
	num_mbufs = 0;
	num_dequeued_ops = 0;
	TAILQ_FOREACH_SAFE(op_to_cancel, &crypto_ch->queued_cry_ops, link, tmp_op) {
		/* Checking if this is our op. One IO contains multiple ops. */
		if (bdev_io == op_to_cancel->bdev_io) {
			crypto_op = op_to_cancel->crypto_op;
			TAILQ_REMOVE(&crypto_ch->queued_cry_ops, op_to_cancel, link);

			/* Populating lists for freeing mbufs and ops. */
			mbufs_to_free[num_mbufs++] = (void *)crypto_op->sym->m_src;
			if (crypto_op->sym->m_dst) {
				mbufs_to_free[num_mbufs++] = (void *)crypto_op->sym->m_dst;
			}
			dequeued_ops[num_dequeued_ops++] = crypto_op;
		}
	}

	/* Now bulk free both mbufs and crypto operations. */
	if (num_dequeued_ops > 0) {
		rte_mempool_put_bulk(g_crypto_op_mp, (void **)dequeued_ops,
				     num_dequeued_ops);
		assert(num_mbufs > 0);
		rte_pktmbuf_free_bulk(mbufs_to_free, num_mbufs);
	}
}

static int _crypto_operation(struct spdk_bdev_io *bdev_io,
			     enum rte_crypto_cipher_operation crypto_op,
			     void *aux_buf);

/* This is the poller for the crypto device. It uses a single API to dequeue whatever is ready at
 * the device. Then we need to decide if what we've got so far (including previous poller
 * runs) totals up to one or more complete bdev_ios and if so continue with the bdev_io
 * accordingly. This means either completing a read or issuing a new write.
 */
static int
crypto_dev_poller(void *args)
{
	struct crypto_io_channel *crypto_ch = args;
	uint8_t cdev_id = crypto_ch->device_qp->device->cdev_id;
	int i, num_dequeued_ops, num_enqueued_ops;
	struct spdk_bdev_io *bdev_io = NULL;
	struct crypto_bdev_io *io_ctx = NULL;
	struct rte_crypto_op *dequeued_ops[MAX_DEQUEUE_BURST_SIZE];
	struct rte_mbuf *mbufs_to_free[2 * MAX_DEQUEUE_BURST_SIZE];
	int num_mbufs = 0;
	struct vbdev_crypto_op *op_to_resubmit;

	/* Each run of the poller will get just what the device has available
	 * at the moment we call it, we don't check again after draining the
	 * first batch.
	 */
	num_dequeued_ops = rte_cryptodev_dequeue_burst(cdev_id, crypto_ch->device_qp->qp,
			   dequeued_ops, MAX_DEQUEUE_BURST_SIZE);

	/* Check if operation was processed successfully */
	for (i = 0; i < num_dequeued_ops; i++) {

		/* We don't know the order or association of the crypto ops wrt any
		 * particular bdev_io so need to look at each and determine if it's
		 * the last one for it's bdev_io or not.
		 */
		bdev_io = (struct spdk_bdev_io *)*RTE_MBUF_DYNFIELD(dequeued_ops[i]->sym->m_src, g_mbuf_offset,
				uint64_t *);
		assert(bdev_io != NULL);
		io_ctx = (struct crypto_bdev_io *)bdev_io->driver_ctx;

		if (dequeued_ops[i]->status != RTE_CRYPTO_OP_STATUS_SUCCESS) {
			SPDK_ERRLOG("error with op %d status %u\n", i,
				    dequeued_ops[i]->status);
			/* Update the bdev status to error, we'll still process the
			 * rest of the crypto ops for this bdev_io though so they
			 * aren't left hanging.
			 */
			io_ctx->bdev_io_status = SPDK_BDEV_IO_STATUS_FAILED;
		}

		assert(io_ctx->cryop_cnt_remaining > 0);

		/* Return the associated src and dst mbufs by collecting them into
		 * an array that we can use the bulk API to free after the loop.
		 */
		*RTE_MBUF_DYNFIELD(dequeued_ops[i]->sym->m_src, g_mbuf_offset, uint64_t *) = 0;
		mbufs_to_free[num_mbufs++] = (void *)dequeued_ops[i]->sym->m_src;
		if (dequeued_ops[i]->sym->m_dst) {
			mbufs_to_free[num_mbufs++] = (void *)dequeued_ops[i]->sym->m_dst;
		}

		/* done encrypting, complete the bdev_io */
		if (--io_ctx->cryop_cnt_remaining == 0) {

			/* If we're completing this with an outstanding reset we need
			 * to fail it.
			 */
			if (crypto_ch->iter) {
				io_ctx->bdev_io_status = SPDK_BDEV_IO_STATUS_FAILED;
			}

			/* Complete the IO */
			_crypto_operation_complete(bdev_io);
		}
	}

	/* Now bulk free both mbufs and crypto operations. */
	if (num_dequeued_ops > 0) {
		rte_mempool_put_bulk(g_crypto_op_mp,
				     (void **)dequeued_ops,
				     num_dequeued_ops);
		assert(num_mbufs > 0);
		rte_pktmbuf_free_bulk(mbufs_to_free, num_mbufs);
	}

	/* Check if there are any pending crypto ops to process */
	while (!TAILQ_EMPTY(&crypto_ch->queued_cry_ops)) {
		op_to_resubmit = TAILQ_FIRST(&crypto_ch->queued_cry_ops);
		bdev_io = op_to_resubmit->bdev_io;
		io_ctx = (struct crypto_bdev_io *)bdev_io->driver_ctx;
		num_enqueued_ops = rte_cryptodev_enqueue_burst(op_to_resubmit->cdev_id,
				   op_to_resubmit->qp,
				   &op_to_resubmit->crypto_op,
				   1);
		if (num_enqueued_ops == 1) {
			/* Make sure we don't put this on twice as one bdev_io is made up
			 * of many crypto ops.
			 */
			if (io_ctx->on_pending_list == false) {
				TAILQ_INSERT_TAIL(&crypto_ch->pending_cry_ios, bdev_io, module_link);
				io_ctx->on_pending_list = true;
			}
			TAILQ_REMOVE(&crypto_ch->queued_cry_ops, op_to_resubmit, link);
		} else {
			if (op_to_resubmit->crypto_op->status == RTE_CRYPTO_OP_STATUS_NOT_PROCESSED) {
				/* If we couldn't get one, just break and try again later. */
				break;
			} else {
				/* Something is really wrong with the op. Most probably the
				 * mbuf is broken or the HW is not able to process the request.
				 * Fail the IO and remove its ops from the queued ops list. */
				io_ctx->bdev_io_status = SPDK_BDEV_IO_STATUS_FAILED;

				cancel_queued_crypto_ops(crypto_ch, bdev_io);

				/* Fail the IO if there is nothing left on device. */
				if (--io_ctx->cryop_cnt_remaining == 0) {
					_crypto_operation_complete(bdev_io);
				}
			}

		}
	}

	/* If the channel iter is not NULL, we need to continue to poll
	 * until the pending list is empty, then we can move on to the
	 * next channel.
	 */
	if (crypto_ch->iter && TAILQ_EMPTY(&crypto_ch->pending_cry_ios)) {
		SPDK_NOTICELOG("Channel %p has been quiesced.\n", crypto_ch);
		spdk_for_each_channel_continue(crypto_ch->iter, 0);
		crypto_ch->iter = NULL;
	}

	return num_dequeued_ops;
}

/* We're either encrypting on the way down or decrypting on the way back. */
static int
_crypto_operation(struct spdk_bdev_io *bdev_io, enum rte_crypto_cipher_operation crypto_op,
		  void *aux_buf)
{
	uint16_t num_enqueued_ops = 0;
	uint32_t cryop_cnt = bdev_io->u.bdev.num_blocks;
	struct crypto_bdev_io *io_ctx = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	struct crypto_io_channel *crypto_ch = io_ctx->crypto_ch;
	uint8_t cdev_id = crypto_ch->device_qp->device->cdev_id;
	uint32_t crypto_len = io_ctx->crypto_bdev->crypto_bdev.blocklen;
	uint64_t total_length = bdev_io->u.bdev.num_blocks * crypto_len;
	int rc;
	uint32_t iov_index = 0;
	uint32_t allocated = 0;
	uint8_t *current_iov = NULL;
	uint64_t total_remaining = 0;
	uint64_t current_iov_remaining = 0;
	uint32_t crypto_index = 0;
	uint32_t en_offset = 0;
	struct rte_crypto_op *crypto_ops[MAX_ENQUEUE_ARRAY_SIZE];
	struct rte_mbuf *src_mbufs[MAX_ENQUEUE_ARRAY_SIZE];
	struct rte_mbuf *dst_mbufs[MAX_ENQUEUE_ARRAY_SIZE];
	int burst;
	struct vbdev_crypto_op *op_to_queue;
	uint64_t alignment = spdk_bdev_get_buf_align(&io_ctx->crypto_bdev->crypto_bdev);

	assert((bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen) <= CRYPTO_MAX_IO);

	/* Get the number of source mbufs that we need. These will always be 1:1 because we
	 * don't support chaining. The reason we don't is because of our decision to use
	 * LBA as IV, there can be no case where we'd need >1 mbuf per crypto op or the
	 * op would be > 1 LBA.
	 */
	rc = rte_pktmbuf_alloc_bulk(g_mbuf_mp, src_mbufs, cryop_cnt);
	if (rc) {
		SPDK_ERRLOG("ERROR trying to get src_mbufs!\n");
		return -ENOMEM;
	}

	/* Get the same amount but these buffers to describe the encrypted data location (dst). */
	if (crypto_op == RTE_CRYPTO_CIPHER_OP_ENCRYPT) {
		rc = rte_pktmbuf_alloc_bulk(g_mbuf_mp, dst_mbufs, cryop_cnt);
		if (rc) {
			SPDK_ERRLOG("ERROR trying to get dst_mbufs!\n");
			rc = -ENOMEM;
			goto error_get_dst;
		}
	}

#ifdef __clang_analyzer__
	/* silence scan-build false positive */
	SPDK_CLANG_ANALYZER_PREINIT_PTR_ARRAY(crypto_ops, MAX_ENQUEUE_ARRAY_SIZE, 0x1000);
#endif
	/* Allocate crypto operations. */
	allocated = rte_crypto_op_bulk_alloc(g_crypto_op_mp,
					     RTE_CRYPTO_OP_TYPE_SYMMETRIC,
					     crypto_ops, cryop_cnt);
	if (allocated < cryop_cnt) {
		SPDK_ERRLOG("ERROR trying to get crypto ops!\n");
		rc = -ENOMEM;
		goto error_get_ops;
	}

	/* For encryption, we need to prepare a single contiguous buffer as the encryption
	 * destination, we'll then pass that along for the write after encryption is done.
	 * This is done to avoiding encrypting the provided write buffer which may be
	 * undesirable in some use cases.
	 */
	if (crypto_op == RTE_CRYPTO_CIPHER_OP_ENCRYPT) {
		io_ctx->aux_buf_iov.iov_len = total_length;
		io_ctx->aux_buf_raw = aux_buf;
		io_ctx->aux_buf_iov.iov_base  = (void *)(((uintptr_t)aux_buf + (alignment - 1)) & ~(alignment - 1));
		io_ctx->aux_offset_blocks = bdev_io->u.bdev.offset_blocks;
		io_ctx->aux_num_blocks = bdev_io->u.bdev.num_blocks;
	}

	/* This value is used in the completion callback to determine when the bdev_io is
	 * complete.
	 */
	io_ctx->cryop_cnt_remaining = cryop_cnt;

	/* As we don't support chaining because of a decision to use LBA as IV, construction
	 * of crypto operations is straightforward. We build both the op, the mbuf and the
	 * dst_mbuf in our local arrays by looping through the length of the bdev IO and
	 * picking off LBA sized blocks of memory from the IOVs as we walk through them. Each
	 * LBA sized chunk of memory will correspond 1:1 to a crypto operation and a single
	 * mbuf per crypto operation.
	 */
	total_remaining = total_length;
	current_iov = bdev_io->u.bdev.iovs[iov_index].iov_base;
	current_iov_remaining = bdev_io->u.bdev.iovs[iov_index].iov_len;
	do {
		uint8_t *iv_ptr;
		uint8_t *buf_addr;
		uint64_t phys_addr;
		uint64_t op_block_offset;
		uint64_t phys_len;

		/* Store context in every mbuf as we don't know anything about completion order */
		*RTE_MBUF_DYNFIELD(src_mbufs[crypto_index], g_mbuf_offset, uint64_t *) = (uint64_t)bdev_io;

		phys_len = crypto_len;
		phys_addr = spdk_vtophys((void *)current_iov, &phys_len);
		if (phys_addr == SPDK_VTOPHYS_ERROR) {
			rc = -EFAULT;
			goto error_attach_session;
		}

		/* Set the mbuf elements address and length. */
		rte_pktmbuf_attach_extbuf(src_mbufs[crypto_index], current_iov,
					  phys_addr, crypto_len, &g_shinfo);
		rte_pktmbuf_append(src_mbufs[crypto_index], crypto_len);

		/* Set the IV - we use the LBA of the crypto_op */
		iv_ptr = rte_crypto_op_ctod_offset(crypto_ops[crypto_index], uint8_t *,
						   IV_OFFSET);
		memset(iv_ptr, 0, AES_CBC_IV_LENGTH);
		op_block_offset = bdev_io->u.bdev.offset_blocks + crypto_index;
		rte_memcpy(iv_ptr, &op_block_offset, sizeof(uint64_t));

		/* Set the data to encrypt/decrypt length */
		crypto_ops[crypto_index]->sym->cipher.data.length = crypto_len;
		crypto_ops[crypto_index]->sym->cipher.data.offset = 0;

		/* link the mbuf to the crypto op. */
		crypto_ops[crypto_index]->sym->m_src = src_mbufs[crypto_index];

		/* For encrypt, point the destination to a buffer we allocate and redirect the bdev_io
		 * that will be used to process the write on completion to the same buffer. Setting
		 * up the en_buffer is a little simpler as we know the destination buffer is single IOV.
		 */
		if (crypto_op == RTE_CRYPTO_CIPHER_OP_ENCRYPT) {
			buf_addr = io_ctx->aux_buf_iov.iov_base + en_offset;
			phys_addr = spdk_vtophys((void *)buf_addr, NULL);
			if (phys_addr == SPDK_VTOPHYS_ERROR) {
				rc = -EFAULT;
				goto error_attach_session;
			}
			rte_pktmbuf_attach_extbuf(dst_mbufs[crypto_index], buf_addr,
						  phys_addr, crypto_len, &g_shinfo);
			rte_pktmbuf_append(dst_mbufs[crypto_index], crypto_len);

			crypto_ops[crypto_index]->sym->m_dst = dst_mbufs[crypto_index];
			en_offset += crypto_len;

			/* Attach the crypto session to the operation */
			rc = rte_crypto_op_attach_sym_session(crypto_ops[crypto_index],
							      io_ctx->crypto_bdev->session_encrypt);
			if (rc) {
				rc = -EINVAL;
				goto error_attach_session;
			}

		} else {
			crypto_ops[crypto_index]->sym->m_dst = NULL;

			/* Attach the crypto session to the operation */
			rc = rte_crypto_op_attach_sym_session(crypto_ops[crypto_index],
							      io_ctx->crypto_bdev->session_decrypt);
			if (rc) {
				rc = -EINVAL;
				goto error_attach_session;
			}


		}

		/* Subtract our running totals for the op in progress and the overall bdev io */
		total_remaining -= crypto_len;
		current_iov_remaining -= crypto_len;

		/* move our current IOV pointer accordingly. */
		current_iov += crypto_len;

		/* move on to the next crypto operation */
		crypto_index++;

		/* If we're done with this IOV, move to the next one. */
		if (current_iov_remaining == 0 && total_remaining > 0) {
			iov_index++;
			current_iov = bdev_io->u.bdev.iovs[iov_index].iov_base;
			current_iov_remaining = bdev_io->u.bdev.iovs[iov_index].iov_len;
		}
	} while (total_remaining > 0);

	/* Enqueue everything we've got but limit by the max number of descriptors we
	 * configured the crypto device for.
	 */
	burst = spdk_min(cryop_cnt, CRYPTO_QP_DESCRIPTORS);
	num_enqueued_ops = rte_cryptodev_enqueue_burst(cdev_id, crypto_ch->device_qp->qp,
			   &crypto_ops[0],
			   burst);

	/* Add this bdev_io to our outstanding list if any of its crypto ops made it. */
	if (num_enqueued_ops > 0) {
		TAILQ_INSERT_TAIL(&crypto_ch->pending_cry_ios, bdev_io, module_link);
		io_ctx->on_pending_list = true;
	}
	/* We were unable to enqueue everything but did get some, so need to decide what
	 * to do based on the status of the last op.
	 */
	if (num_enqueued_ops < cryop_cnt) {
		switch (crypto_ops[num_enqueued_ops]->status) {
		case RTE_CRYPTO_OP_STATUS_NOT_PROCESSED:
			/* Queue them up on a linked list to be resubmitted via the poller. */
			for (crypto_index = num_enqueued_ops; crypto_index < cryop_cnt; crypto_index++) {
				op_to_queue = (struct vbdev_crypto_op *)rte_crypto_op_ctod_offset(crypto_ops[crypto_index],
						uint8_t *, QUEUED_OP_OFFSET);
				op_to_queue->cdev_id = cdev_id;
				op_to_queue->qp = crypto_ch->device_qp->qp;
				op_to_queue->crypto_op = crypto_ops[crypto_index];
				op_to_queue->bdev_io = bdev_io;
				TAILQ_INSERT_TAIL(&crypto_ch->queued_cry_ops,
						  op_to_queue,
						  link);
			}
			break;
		default:
			/* For all other statuses, set the io_ctx bdev_io status so that
			 * the poller will pick the failure up for the overall bdev status.
			 */
			io_ctx->bdev_io_status = SPDK_BDEV_IO_STATUS_FAILED;
			if (num_enqueued_ops == 0) {
				/* If nothing was enqueued, but the last one wasn't because of
				 * busy, fail it now as the poller won't know anything about it.
				 */
				rc = -EINVAL;
				goto error_attach_session;
			}
			break;
		}
	}

	return rc;

	/* Error cleanup paths. */
error_attach_session:
error_get_ops:
	if (crypto_op == RTE_CRYPTO_CIPHER_OP_ENCRYPT) {
		rte_pktmbuf_free_bulk(dst_mbufs, cryop_cnt);
	}
	if (allocated > 0) {
		rte_mempool_put_bulk(g_crypto_op_mp, (void **)crypto_ops,
				     allocated);
	}
error_get_dst:
	rte_pktmbuf_free_bulk(src_mbufs, cryop_cnt);
	return rc;
}

/* This function is called after all channels have been quiesced following
 * a bdev reset.
 */
static void
_ch_quiesce_done(struct spdk_io_channel_iter *i, int status)
{
	struct crypto_bdev_io *io_ctx = spdk_io_channel_iter_get_ctx(i);

	assert(TAILQ_EMPTY(&io_ctx->crypto_ch->pending_cry_ios));
	assert(io_ctx->orig_io != NULL);

	spdk_bdev_io_complete(io_ctx->orig_io, SPDK_BDEV_IO_STATUS_SUCCESS);
}

/* This function is called per channel to quiesce IOs before completing a
 * bdev reset that we received.
 */
static void
_ch_quiesce(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct crypto_io_channel *crypto_ch = spdk_io_channel_get_ctx(ch);

	crypto_ch->iter = i;
	/* When the poller runs, it will see the non-NULL iter and handle
	 * the quiesce.
	 */
}

/* Completion callback for IO that were issued from this bdev other than read/write.
 * They have their own for readability.
 */
static void
_complete_internal_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_RESET) {
		struct crypto_bdev_io *orig_ctx = (struct crypto_bdev_io *)orig_io->driver_ctx;

		assert(orig_io == orig_ctx->orig_io);

		spdk_bdev_free_io(bdev_io);

		spdk_for_each_channel(orig_ctx->crypto_bdev,
				      _ch_quiesce,
				      orig_ctx,
				      _ch_quiesce_done);
		return;
	}

	spdk_bdev_io_complete(orig_io, status);
	spdk_bdev_free_io(bdev_io);
}

/* Completion callback for writes that were issued from this bdev. */
static void
_complete_internal_write(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	struct crypto_bdev_io *orig_ctx = (struct crypto_bdev_io *)orig_io->driver_ctx;

	spdk_bdev_io_put_aux_buf(orig_io, orig_ctx->aux_buf_raw);

	spdk_bdev_io_complete(orig_io, status);
	spdk_bdev_free_io(bdev_io);
}

/* Completion callback for reads that were issued from this bdev. */
static void
_complete_internal_read(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	struct crypto_bdev_io *orig_ctx = (struct crypto_bdev_io *)orig_io->driver_ctx;

	if (success) {

		/* Save off this bdev_io so it can be freed after decryption. */
		orig_ctx->read_io = bdev_io;

		if (!_crypto_operation(orig_io, RTE_CRYPTO_CIPHER_OP_DECRYPT, NULL)) {
			return;
		} else {
			SPDK_ERRLOG("ERROR decrypting\n");
		}
	} else {
		SPDK_ERRLOG("ERROR on read prior to decrypting\n");
	}

	spdk_bdev_io_complete(orig_io, SPDK_BDEV_IO_STATUS_FAILED);
	spdk_bdev_free_io(bdev_io);
}

static void
vbdev_crypto_resubmit_io(void *arg)
{
	struct spdk_bdev_io *bdev_io = (struct spdk_bdev_io *)arg;
	struct crypto_bdev_io *io_ctx = (struct crypto_bdev_io *)bdev_io->driver_ctx;

	vbdev_crypto_submit_request(io_ctx->ch, bdev_io);
}

static void
vbdev_crypto_queue_io(struct spdk_bdev_io *bdev_io)
{
	struct crypto_bdev_io *io_ctx = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	int rc;

	io_ctx->bdev_io_wait.bdev = bdev_io->bdev;
	io_ctx->bdev_io_wait.cb_fn = vbdev_crypto_resubmit_io;
	io_ctx->bdev_io_wait.cb_arg = bdev_io;

	rc = spdk_bdev_queue_io_wait(bdev_io->bdev, io_ctx->crypto_ch->base_ch, &io_ctx->bdev_io_wait);
	if (rc != 0) {
		SPDK_ERRLOG("Queue io failed in vbdev_crypto_queue_io, rc=%d.\n", rc);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/* Callback for getting a buf from the bdev pool in the event that the caller passed
 * in NULL, we need to own the buffer so it doesn't get freed by another vbdev module
 * beneath us before we're done with it.
 */
static void
crypto_read_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		       bool success)
{
	struct vbdev_crypto *crypto_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_crypto,
					   crypto_bdev);
	struct crypto_io_channel *crypto_ch = spdk_io_channel_get_ctx(ch);
	struct crypto_bdev_io *io_ctx = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	int rc;

	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	rc = spdk_bdev_readv_blocks(crypto_bdev->base_desc, crypto_ch->base_ch, bdev_io->u.bdev.iovs,
				    bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks,
				    bdev_io->u.bdev.num_blocks, _complete_internal_read,
				    bdev_io);
	if (rc != 0) {
		if (rc == -ENOMEM) {
			SPDK_DEBUGLOG(vbdev_crypto, "No memory, queue the IO.\n");
			io_ctx->ch = ch;
			vbdev_crypto_queue_io(bdev_io);
		} else {
			SPDK_ERRLOG("ERROR on bdev_io submission!\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

/* For encryption we don't want to encrypt the data in place as the host isn't
 * expecting us to mangle its data buffers so we need to encrypt into the bdev
 * aux buffer, then we can use that as the source for the disk data transfer.
 */
static void
crypto_write_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
			void *aux_buf)
{
	struct crypto_bdev_io *io_ctx = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	int rc = 0;

	rc = _crypto_operation(bdev_io, RTE_CRYPTO_CIPHER_OP_ENCRYPT, aux_buf);
	if (rc != 0) {
		spdk_bdev_io_put_aux_buf(bdev_io, aux_buf);
		if (rc == -ENOMEM) {
			SPDK_DEBUGLOG(vbdev_crypto, "No memory, queue the IO.\n");
			io_ctx->ch = ch;
			vbdev_crypto_queue_io(bdev_io);
		} else {
			SPDK_ERRLOG("ERROR on bdev_io submission!\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

/* Called when someone submits IO to this crypto vbdev. For IO's not relevant to crypto,
 * we're simply passing it on here via SPDK IO calls which in turn allocate another bdev IO
 * and call our cpl callback provided below along with the original bdev_io so that we can
 * complete it once this IO completes. For crypto operations, we'll either encrypt it first
 * (writes) then call back into bdev to submit it or we'll submit a read and then catch it
 * on the way back for decryption.
 */
static void
vbdev_crypto_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_crypto *crypto_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_crypto,
					   crypto_bdev);
	struct crypto_io_channel *crypto_ch = spdk_io_channel_get_ctx(ch);
	struct crypto_bdev_io *io_ctx = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	int rc = 0;

	memset(io_ctx, 0, sizeof(struct crypto_bdev_io));
	io_ctx->crypto_bdev = crypto_bdev;
	io_ctx->crypto_ch = crypto_ch;
	io_ctx->orig_io = bdev_io;
	io_ctx->bdev_io_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, crypto_read_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		/* Tell the bdev layer that we need an aux buf in addition to the data
		 * buf already associated with the bdev.
		 */
		spdk_bdev_io_get_aux_buf(bdev_io, crypto_write_get_buf_cb);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = spdk_bdev_unmap_blocks(crypto_bdev->base_desc, crypto_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _complete_internal_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = spdk_bdev_flush_blocks(crypto_bdev->base_desc, crypto_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _complete_internal_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		rc = spdk_bdev_reset(crypto_bdev->base_desc, crypto_ch->base_ch,
				     _complete_internal_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	default:
		SPDK_ERRLOG("crypto: unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (rc != 0) {
		if (rc == -ENOMEM) {
			SPDK_DEBUGLOG(vbdev_crypto, "No memory, queue the IO.\n");
			io_ctx->ch = ch;
			vbdev_crypto_queue_io(bdev_io);
		} else {
			SPDK_ERRLOG("ERROR on bdev_io submission!\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

/* We'll just call the base bdev and let it answer except for WZ command which
 * we always say we don't support so that the bdev layer will actually send us
 * real writes that we can encrypt.
 */
static bool
vbdev_crypto_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct vbdev_crypto *crypto_bdev = (struct vbdev_crypto *)ctx;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_FLUSH:
		return spdk_bdev_io_type_supported(crypto_bdev->base_bdev, io_type);
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	/* Force the bdev layer to issue actual writes of zeroes so we can
	 * encrypt them as regular writes.
	 */
	default:
		return false;
	}
}

/* Callback for unregistering the IO device. */
static void
_device_unregister_cb(void *io_device)
{
	struct vbdev_crypto *crypto_bdev = io_device;

	/* Done with this crypto_bdev. */
	rte_cryptodev_sym_session_free(crypto_bdev->session_decrypt);
	rte_cryptodev_sym_session_free(crypto_bdev->session_encrypt);
	free(crypto_bdev->drv_name);
	if (crypto_bdev->key) {
		memset(crypto_bdev->key, 0, strnlen(crypto_bdev->key, (AES_CBC_KEY_LENGTH + 1)));
		free(crypto_bdev->key);
	}
	if (crypto_bdev->key2) {
		memset(crypto_bdev->key2, 0, strnlen(crypto_bdev->key2, (AES_XTS_KEY_LENGTH + 1)));
		free(crypto_bdev->key2);
	}
	if (crypto_bdev->xts_key) {
		memset(crypto_bdev->xts_key, 0, strnlen(crypto_bdev->xts_key, (AES_XTS_KEY_LENGTH * 2) + 1));
		free(crypto_bdev->xts_key);
	}
	free(crypto_bdev->crypto_bdev.name);
	free(crypto_bdev);
}

/* Wrapper for the bdev close operation. */
static void
_vbdev_crypto_destruct(void *ctx)
{
	struct spdk_bdev_desc *desc = ctx;

	spdk_bdev_close(desc);
}

/* Called after we've unregistered following a hot remove callback.
 * Our finish entry point will be called next.
 */
static int
vbdev_crypto_destruct(void *ctx)
{
	struct vbdev_crypto *crypto_bdev = (struct vbdev_crypto *)ctx;

	/* Remove this device from the internal list */
	TAILQ_REMOVE(&g_vbdev_crypto, crypto_bdev, link);

	/* Unclaim the underlying bdev. */
	spdk_bdev_module_release_bdev(crypto_bdev->base_bdev);

	/* Close the underlying bdev on its same opened thread. */
	if (crypto_bdev->thread && crypto_bdev->thread != spdk_get_thread()) {
		spdk_thread_send_msg(crypto_bdev->thread, _vbdev_crypto_destruct, crypto_bdev->base_desc);
	} else {
		spdk_bdev_close(crypto_bdev->base_desc);
	}

	/* Unregister the io_device. */
	spdk_io_device_unregister(crypto_bdev, _device_unregister_cb);

	g_number_of_claimed_volumes--;

	return 0;
}

/* We supplied this as an entry point for upper layers who want to communicate to this
 * bdev.  This is how they get a channel. We are passed the same context we provided when
 * we created our crypto vbdev in examine() which, for this bdev, is the address of one of
 * our context nodes. From here we'll ask the SPDK channel code to fill out our channel
 * struct and we'll keep it in our crypto node.
 */
static struct spdk_io_channel *
vbdev_crypto_get_io_channel(void *ctx)
{
	struct vbdev_crypto *crypto_bdev = (struct vbdev_crypto *)ctx;

	/* The IO channel code will allocate a channel for us which consists of
	 * the SPDK channel structure plus the size of our crypto_io_channel struct
	 * that we passed in when we registered our IO device. It will then call
	 * our channel create callback to populate any elements that we need to
	 * update.
	 */
	return spdk_get_io_channel(crypto_bdev);
}

/* This is the output for bdev_get_bdevs() for this vbdev */
static int
vbdev_crypto_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct vbdev_crypto *crypto_bdev = (struct vbdev_crypto *)ctx;

	spdk_json_write_name(w, "crypto");
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(crypto_bdev->base_bdev));
	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&crypto_bdev->crypto_bdev));
	spdk_json_write_named_string(w, "crypto_pmd", crypto_bdev->drv_name);
	spdk_json_write_named_string(w, "key", crypto_bdev->key);
	if (strcmp(crypto_bdev->cipher, AES_XTS) == 0) {
		spdk_json_write_named_string(w, "key2", crypto_bdev->key);
	}
	spdk_json_write_named_string(w, "cipher", crypto_bdev->cipher);
	spdk_json_write_object_end(w);
	return 0;
}

static int
vbdev_crypto_config_json(struct spdk_json_write_ctx *w)
{
	struct vbdev_crypto *crypto_bdev;

	TAILQ_FOREACH(crypto_bdev, &g_vbdev_crypto, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "bdev_crypto_create");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(crypto_bdev->base_bdev));
		spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&crypto_bdev->crypto_bdev));
		spdk_json_write_named_string(w, "crypto_pmd", crypto_bdev->drv_name);
		spdk_json_write_named_string(w, "key", crypto_bdev->key);
		if (strcmp(crypto_bdev->cipher, AES_XTS) == 0) {
			spdk_json_write_named_string(w, "key2", crypto_bdev->key);
		}
		spdk_json_write_named_string(w, "cipher", crypto_bdev->cipher);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
	return 0;
}

/* Helper function for the channel creation callback. */
static void
_assign_device_qp(struct vbdev_crypto *crypto_bdev, struct device_qp *device_qp,
		  struct crypto_io_channel *crypto_ch)
{
	pthread_mutex_lock(&g_device_qp_lock);
	if (strcmp(crypto_bdev->drv_name, QAT) == 0) {
		/* For some QAT devices, the optimal qp to use is every 32nd as this spreads the
		 * workload out over the multiple virtual functions in the device. For the devices
		 * where this isn't the case, it doesn't hurt.
		 */
		TAILQ_FOREACH(device_qp, &g_device_qp_qat, link) {
			if (device_qp->index != g_next_qat_index) {
				continue;
			}
			if (device_qp->in_use == false) {
				crypto_ch->device_qp = device_qp;
				device_qp->in_use = true;
				g_next_qat_index = (g_next_qat_index + QAT_VF_SPREAD) % g_qat_total_qp;
				break;
			} else {
				/* if the preferred index is used, skip to the next one in this set. */
				g_next_qat_index = (g_next_qat_index + 1) % g_qat_total_qp;
			}
		}
	} else if (strcmp(crypto_bdev->drv_name, AESNI_MB) == 0) {
		TAILQ_FOREACH(device_qp, &g_device_qp_aesni_mb, link) {
			if (device_qp->in_use == false) {
				crypto_ch->device_qp = device_qp;
				device_qp->in_use = true;
				break;
			}
		}
	}
	pthread_mutex_unlock(&g_device_qp_lock);
}

/* We provide this callback for the SPDK channel code to create a channel using
 * the channel struct we provided in our module get_io_channel() entry point. Here
 * we get and save off an underlying base channel of the device below us so that
 * we can communicate with the base bdev on a per channel basis. We also register the
 * poller used to complete crypto operations from the device.
 */
static int
crypto_bdev_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct crypto_io_channel *crypto_ch = ctx_buf;
	struct vbdev_crypto *crypto_bdev = io_device;
	struct device_qp *device_qp = NULL;

	crypto_ch->base_ch = spdk_bdev_get_io_channel(crypto_bdev->base_desc);
	crypto_ch->poller = SPDK_POLLER_REGISTER(crypto_dev_poller, crypto_ch, 0);
	crypto_ch->device_qp = NULL;

	/* Assign a device/qp combination that is unique per channel per PMD. */
	_assign_device_qp(crypto_bdev, device_qp, crypto_ch);
	assert(crypto_ch->device_qp);

	/* We use this queue to track outstanding IO in our layer. */
	TAILQ_INIT(&crypto_ch->pending_cry_ios);

	/* We use this to queue up crypto ops when the device is busy. */
	TAILQ_INIT(&crypto_ch->queued_cry_ops);

	return 0;
}

/* We provide this callback for the SPDK channel code to destroy a channel
 * created with our create callback. We just need to undo anything we did
 * when we created.
 */
static void
crypto_bdev_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct crypto_io_channel *crypto_ch = ctx_buf;

	pthread_mutex_lock(&g_device_qp_lock);
	crypto_ch->device_qp->in_use = false;
	pthread_mutex_unlock(&g_device_qp_lock);

	spdk_poller_unregister(&crypto_ch->poller);
	spdk_put_io_channel(crypto_ch->base_ch);
}

/* Create the association from the bdev and vbdev name and insert
 * on the global list. */
static int
vbdev_crypto_insert_name(const char *bdev_name, const char *vbdev_name,
			 const char *crypto_pmd, const char *key,
			 const char *cipher, const char *key2)
{
	struct bdev_names *name;
	int rc, j;
	bool found = false;

	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(vbdev_name, name->vbdev_name) == 0) {
			SPDK_ERRLOG("crypto bdev %s already exists\n", vbdev_name);
			return -EEXIST;
		}
	}

	name = calloc(1, sizeof(struct bdev_names));
	if (!name) {
		SPDK_ERRLOG("could not allocate bdev_names\n");
		return -ENOMEM;
	}

	name->bdev_name = strdup(bdev_name);
	if (!name->bdev_name) {
		SPDK_ERRLOG("could not allocate name->bdev_name\n");
		rc = -ENOMEM;
		goto error_alloc_bname;
	}

	name->vbdev_name = strdup(vbdev_name);
	if (!name->vbdev_name) {
		SPDK_ERRLOG("could not allocate name->vbdev_name\n");
		rc = -ENOMEM;
		goto error_alloc_vname;
	}

	name->drv_name = strdup(crypto_pmd);
	if (!name->drv_name) {
		SPDK_ERRLOG("could not allocate name->drv_name\n");
		rc = -ENOMEM;
		goto error_alloc_dname;
	}
	for (j = 0; j < MAX_NUM_DRV_TYPES ; j++) {
		if (strcmp(crypto_pmd, g_driver_names[j]) == 0) {
			found = true;
			break;
		}
	}
	if (!found) {
		SPDK_ERRLOG("invalid crypto PMD type %s\n", crypto_pmd);
		rc = -EINVAL;
		goto error_invalid_pmd;
	}

	name->key = strdup(key);
	if (!name->key) {
		SPDK_ERRLOG("could not allocate name->key\n");
		rc = -ENOMEM;
		goto error_alloc_key;
	}
	if (strnlen(name->key, (AES_CBC_KEY_LENGTH + 1)) != AES_CBC_KEY_LENGTH) {
		SPDK_ERRLOG("invalid AES_CBC key length\n");
		rc = -EINVAL;
		goto error_invalid_key;
	}

	if (strncmp(cipher, AES_XTS, sizeof(AES_XTS)) == 0) {
		/* To please scan-build, input validation makes sure we can't
		 * have this cipher without providing a key2.
		 */
		name->cipher = AES_XTS;
		assert(key2);
		if (strnlen(key2, (AES_XTS_KEY_LENGTH + 1)) != AES_XTS_KEY_LENGTH) {
			SPDK_ERRLOG("invalid AES_XTS key length\n");
			rc = -EINVAL;
			goto error_invalid_key2;
		}

		name->key2 = strdup(key2);
		if (!name->key2) {
			SPDK_ERRLOG("could not allocate name->key2\n");
			rc = -ENOMEM;
			goto error_alloc_key2;
		}
	} else if (strncmp(cipher, AES_CBC, sizeof(AES_CBC)) == 0) {
		name->cipher = AES_CBC;
	} else {
		SPDK_ERRLOG("Invalid cipher: %s\n", cipher);
		rc = -EINVAL;
		goto error_cipher;
	}

	TAILQ_INSERT_TAIL(&g_bdev_names, name, link);

	return 0;

	/* Error cleanup paths. */
error_cipher:
	free(name->key2);
error_alloc_key2:
error_invalid_key2:
error_invalid_key:
	free(name->key);
error_alloc_key:
error_invalid_pmd:
	free(name->drv_name);
error_alloc_dname:
	free(name->vbdev_name);
error_alloc_vname:
	free(name->bdev_name);
error_alloc_bname:
	free(name);
	return rc;
}

/* RPC entry point for crypto creation. */
int
create_crypto_disk(const char *bdev_name, const char *vbdev_name,
		   const char *crypto_pmd, const char *key,
		   const char *cipher, const char *key2)
{
	int rc;

	rc = vbdev_crypto_insert_name(bdev_name, vbdev_name, crypto_pmd, key, cipher, key2);
	if (rc) {
		return rc;
	}

	rc = vbdev_crypto_claim(bdev_name);
	if (rc == -ENODEV) {
		SPDK_NOTICELOG("vbdev creation deferred pending base bdev arrival\n");
		rc = 0;
	}

	return rc;
}

/* Called at driver init time, parses config file to prepare for examine calls,
 * also fully initializes the crypto drivers.
 */
static int
vbdev_crypto_init(void)
{
	int rc = 0;

	/* Fully configure both SW and HW drivers. */
	rc = vbdev_crypto_init_crypto_drivers();
	if (rc) {
		SPDK_ERRLOG("Error setting up crypto devices\n");
	}

	return rc;
}

/* Called when the entire module is being torn down. */
static void
vbdev_crypto_finish(void)
{
	struct bdev_names *name;
	struct vbdev_dev *device;
	struct device_qp *dev_qp;
	int rc;

	while ((name = TAILQ_FIRST(&g_bdev_names))) {
		TAILQ_REMOVE(&g_bdev_names, name, link);
		free(name->drv_name);
		free(name->key);
		free(name->bdev_name);
		free(name->vbdev_name);
		free(name->key2);
		free(name);
	}

	while ((device = TAILQ_FIRST(&g_vbdev_devs))) {
		TAILQ_REMOVE(&g_vbdev_devs, device, link);
		rte_cryptodev_stop(device->cdev_id);
		rc = rte_cryptodev_close(device->cdev_id);
		assert(rc == 0);
		free(device);
	}

	rc = rte_vdev_uninit(AESNI_MB);
	if (rc) {
		SPDK_ERRLOG("%d from rte_vdev_uninit\n", rc);
	}

	while ((dev_qp = TAILQ_FIRST(&g_device_qp_qat))) {
		TAILQ_REMOVE(&g_device_qp_qat, dev_qp, link);
		free(dev_qp);
	}

	while ((dev_qp = TAILQ_FIRST(&g_device_qp_aesni_mb))) {
		TAILQ_REMOVE(&g_device_qp_aesni_mb, dev_qp, link);
		free(dev_qp);
	}

	rte_mempool_free(g_crypto_op_mp);
	rte_mempool_free(g_mbuf_mp);
	rte_mempool_free(g_session_mp);
	if (g_session_mp_priv != NULL) {
		rte_mempool_free(g_session_mp_priv);
	}
}

/* During init we'll be asked how much memory we'd like passed to us
 * in bev_io structures as context. Here's where we specify how
 * much context we want per IO.
 */
static int
vbdev_crypto_get_ctx_size(void)
{
	return sizeof(struct crypto_bdev_io);
}

static void
vbdev_crypto_base_bdev_hotremove_cb(struct spdk_bdev *bdev_find)
{
	struct vbdev_crypto *crypto_bdev, *tmp;

	TAILQ_FOREACH_SAFE(crypto_bdev, &g_vbdev_crypto, link, tmp) {
		if (bdev_find == crypto_bdev->base_bdev) {
			spdk_bdev_unregister(&crypto_bdev->crypto_bdev, NULL, NULL);
		}
	}
}

/* Called when the underlying base bdev triggers asynchronous event such as bdev removal. */
static void
vbdev_crypto_base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
				void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		vbdev_crypto_base_bdev_hotremove_cb(bdev);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

static void
vbdev_crypto_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	/* No config per bdev needed */
}

/* When we register our bdev this is how we specify our entry points. */
static const struct spdk_bdev_fn_table vbdev_crypto_fn_table = {
	.destruct		= vbdev_crypto_destruct,
	.submit_request		= vbdev_crypto_submit_request,
	.io_type_supported	= vbdev_crypto_io_type_supported,
	.get_io_channel		= vbdev_crypto_get_io_channel,
	.dump_info_json		= vbdev_crypto_dump_info_json,
	.write_config_json	= vbdev_crypto_write_config_json
};

static struct spdk_bdev_module crypto_if = {
	.name = "crypto",
	.module_init = vbdev_crypto_init,
	.get_ctx_size = vbdev_crypto_get_ctx_size,
	.examine_config = vbdev_crypto_examine,
	.module_fini = vbdev_crypto_finish,
	.config_json = vbdev_crypto_config_json
};

SPDK_BDEV_MODULE_REGISTER(crypto, &crypto_if)

static int
vbdev_crypto_claim(const char *bdev_name)
{
	struct bdev_names *name;
	struct vbdev_crypto *vbdev;
	struct vbdev_dev *device;
	struct spdk_bdev *bdev;
	bool found = false;
	int rc = 0;

	if (g_number_of_claimed_volumes >= MAX_CRYPTO_VOLUMES) {
		SPDK_DEBUGLOG(vbdev_crypto, "Reached max number of claimed volumes\n");
		return -EINVAL;
	}
	g_number_of_claimed_volumes++;

	/* Check our list of names from config versus this bdev and if
	 * there's a match, create the crypto_bdev & bdev accordingly.
	 */
	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(name->bdev_name, bdev_name) != 0) {
			continue;
		}
		SPDK_DEBUGLOG(vbdev_crypto, "Match on %s\n", bdev_name);

		vbdev = calloc(1, sizeof(struct vbdev_crypto));
		if (!vbdev) {
			SPDK_ERRLOG("could not allocate crypto_bdev\n");
			rc = -ENOMEM;
			goto error_vbdev_alloc;
		}

		vbdev->crypto_bdev.name = strdup(name->vbdev_name);
		if (!vbdev->crypto_bdev.name) {
			SPDK_ERRLOG("could not allocate crypto_bdev name\n");
			rc = -ENOMEM;
			goto error_bdev_name;
		}

		vbdev->key = strdup(name->key);
		if (!vbdev->key) {
			SPDK_ERRLOG("could not allocate crypto_bdev key\n");
			rc = -ENOMEM;
			goto error_alloc_key;
		}

		if (name->key2) {
			vbdev->key2 = strdup(name->key2);
			if (!vbdev->key2) {
				SPDK_ERRLOG("could not allocate crypto_bdev key2\n");
				rc = -ENOMEM;
				goto error_alloc_key2;
			}
		}

		vbdev->drv_name = strdup(name->drv_name);
		if (!vbdev->drv_name) {
			SPDK_ERRLOG("could not allocate crypto_bdev drv_name\n");
			rc = -ENOMEM;
			goto error_drv_name;
		}

		vbdev->crypto_bdev.product_name = "crypto";

		rc = spdk_bdev_open_ext(bdev_name, true, vbdev_crypto_base_bdev_event_cb,
					NULL, &vbdev->base_desc);
		if (rc) {
			if (rc != -ENODEV) {
				SPDK_ERRLOG("could not open bdev %s\n", bdev_name);
			}
			goto error_open;
		}

		bdev = spdk_bdev_desc_get_bdev(vbdev->base_desc);
		vbdev->base_bdev = bdev;

		vbdev->crypto_bdev.write_cache = bdev->write_cache;
		vbdev->cipher = AES_CBC;
		if (strcmp(vbdev->drv_name, QAT) == 0) {
			vbdev->crypto_bdev.required_alignment =
				spdk_max(spdk_u32log2(bdev->blocklen), bdev->required_alignment);
			SPDK_NOTICELOG("QAT in use: Required alignment set to %u\n",
				       vbdev->crypto_bdev.required_alignment);
			if (strcmp(name->cipher, AES_CBC) == 0) {
				SPDK_NOTICELOG("QAT using cipher: AES_CBC\n");
			} else {
				SPDK_NOTICELOG("QAT using cipher: AES_XTS\n");
				vbdev->cipher = AES_XTS;
				/* DPDK expects they keys to be concatenated together. */
				vbdev->xts_key = calloc(1, (AES_XTS_KEY_LENGTH * 2) + 1);
				if (vbdev->xts_key == NULL) {
					SPDK_ERRLOG("could not allocate memory for XTS key\n");
					rc = -ENOMEM;
					goto error_xts_key;
				}
				memcpy(vbdev->xts_key, vbdev->key, AES_XTS_KEY_LENGTH);
				assert(name->key2);
				memcpy(vbdev->xts_key + AES_XTS_KEY_LENGTH, name->key2, AES_XTS_KEY_LENGTH + 1);
			}
		} else {
			vbdev->crypto_bdev.required_alignment = bdev->required_alignment;
		}
		/* Note: CRYPTO_MAX_IO is in units of bytes, optimal_io_boundary is
		 * in units of blocks.
		 */
		if (bdev->optimal_io_boundary > 0) {
			vbdev->crypto_bdev.optimal_io_boundary =
				spdk_min((CRYPTO_MAX_IO / bdev->blocklen), bdev->optimal_io_boundary);
		} else {
			vbdev->crypto_bdev.optimal_io_boundary = (CRYPTO_MAX_IO / bdev->blocklen);
		}
		vbdev->crypto_bdev.split_on_optimal_io_boundary = true;
		vbdev->crypto_bdev.blocklen = bdev->blocklen;
		vbdev->crypto_bdev.blockcnt = bdev->blockcnt;

		/* This is the context that is passed to us when the bdev
		 * layer calls in so we'll save our crypto_bdev node here.
		 */
		vbdev->crypto_bdev.ctxt = vbdev;
		vbdev->crypto_bdev.fn_table = &vbdev_crypto_fn_table;
		vbdev->crypto_bdev.module = &crypto_if;
		TAILQ_INSERT_TAIL(&g_vbdev_crypto, vbdev, link);

		spdk_io_device_register(vbdev, crypto_bdev_ch_create_cb, crypto_bdev_ch_destroy_cb,
					sizeof(struct crypto_io_channel), vbdev->crypto_bdev.name);

		/* Save the thread where the base device is opened */
		vbdev->thread = spdk_get_thread();

		rc = spdk_bdev_module_claim_bdev(bdev, vbdev->base_desc, vbdev->crypto_bdev.module);
		if (rc) {
			SPDK_ERRLOG("could not claim bdev %s\n", spdk_bdev_get_name(bdev));
			goto error_claim;
		}

		/* To init the session we have to get the cryptoDev device ID for this vbdev */
		TAILQ_FOREACH(device, &g_vbdev_devs, link) {
			if (strcmp(device->cdev_info.driver_name, vbdev->drv_name) == 0) {
				found = true;
				break;
			}
		}
		if (found == false) {
			SPDK_ERRLOG("ERROR can't match crypto device driver to crypto vbdev!\n");
			rc = -EINVAL;
			goto error_cant_find_devid;
		}

		/* Get sessions. */
		vbdev->session_encrypt = rte_cryptodev_sym_session_create(g_session_mp);
		if (NULL == vbdev->session_encrypt) {
			SPDK_ERRLOG("ERROR trying to create crypto session!\n");
			rc = -EINVAL;
			goto error_session_en_create;
		}

		vbdev->session_decrypt = rte_cryptodev_sym_session_create(g_session_mp);
		if (NULL == vbdev->session_decrypt) {
			SPDK_ERRLOG("ERROR trying to create crypto session!\n");
			rc = -EINVAL;
			goto error_session_de_create;
		}

		/* Init our per vbdev xform with the desired cipher options. */
		vbdev->cipher_xform.type = RTE_CRYPTO_SYM_XFORM_CIPHER;
		vbdev->cipher_xform.cipher.iv.offset = IV_OFFSET;
		if (strcmp(name->cipher, AES_CBC) == 0) {
			vbdev->cipher_xform.cipher.key.data = vbdev->key;
			vbdev->cipher_xform.cipher.algo = RTE_CRYPTO_CIPHER_AES_CBC;
			vbdev->cipher_xform.cipher.key.length = AES_CBC_KEY_LENGTH;
		} else {
			vbdev->cipher_xform.cipher.key.data = vbdev->xts_key;
			vbdev->cipher_xform.cipher.algo = RTE_CRYPTO_CIPHER_AES_XTS;
			vbdev->cipher_xform.cipher.key.length = AES_XTS_KEY_LENGTH * 2;
		}
		vbdev->cipher_xform.cipher.iv.length = AES_CBC_IV_LENGTH;

		vbdev->cipher_xform.cipher.op = RTE_CRYPTO_CIPHER_OP_ENCRYPT;
		rc = rte_cryptodev_sym_session_init(device->cdev_id, vbdev->session_encrypt,
						    &vbdev->cipher_xform,
						    g_session_mp_priv ? g_session_mp_priv : g_session_mp);
		if (rc < 0) {
			SPDK_ERRLOG("ERROR trying to init encrypt session!\n");
			rc = -EINVAL;
			goto error_session_init;
		}

		vbdev->cipher_xform.cipher.op = RTE_CRYPTO_CIPHER_OP_DECRYPT;
		rc = rte_cryptodev_sym_session_init(device->cdev_id, vbdev->session_decrypt,
						    &vbdev->cipher_xform,
						    g_session_mp_priv ? g_session_mp_priv : g_session_mp);
		if (rc < 0) {
			SPDK_ERRLOG("ERROR trying to init decrypt session!\n");
			rc = -EINVAL;
			goto error_session_init;
		}

		rc = spdk_bdev_register(&vbdev->crypto_bdev);
		if (rc < 0) {
			SPDK_ERRLOG("ERROR trying to register bdev\n");
			rc = -EINVAL;
			goto error_bdev_register;
		}
		SPDK_DEBUGLOG(vbdev_crypto, "registered io_device and virtual bdev for: %s\n",
			      name->vbdev_name);
		break;
	}

	return rc;

	/* Error cleanup paths. */
error_bdev_register:
error_session_init:
	rte_cryptodev_sym_session_free(vbdev->session_decrypt);
error_session_de_create:
	rte_cryptodev_sym_session_free(vbdev->session_encrypt);
error_session_en_create:
error_cant_find_devid:
	spdk_bdev_module_release_bdev(vbdev->base_bdev);
error_claim:
	TAILQ_REMOVE(&g_vbdev_crypto, vbdev, link);
	spdk_io_device_unregister(vbdev, NULL);
	if (vbdev->xts_key) {
		memset(vbdev->xts_key, 0, AES_XTS_KEY_LENGTH * 2);
		free(vbdev->xts_key);
	}
error_xts_key:
	spdk_bdev_close(vbdev->base_desc);
error_open:
	free(vbdev->drv_name);
error_drv_name:
	if (vbdev->key2) {
		memset(vbdev->key2, 0, strlen(vbdev->key2));
		free(vbdev->key2);
	}
error_alloc_key2:
	if (vbdev->key) {
		memset(vbdev->key, 0, strlen(vbdev->key));
		free(vbdev->key);
	}
error_alloc_key:
	free(vbdev->crypto_bdev.name);
error_bdev_name:
	free(vbdev);
error_vbdev_alloc:
	g_number_of_claimed_volumes--;
	return rc;
}

/* RPC entry for deleting a crypto vbdev. */
void
delete_crypto_disk(struct spdk_bdev *bdev, spdk_delete_crypto_complete cb_fn,
		   void *cb_arg)
{
	struct bdev_names *name;

	if (!bdev || bdev->module != &crypto_if) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	/* Remove the association (vbdev, bdev) from g_bdev_names. This is required so that the
	 * vbdev does not get re-created if the same bdev is constructed at some other time,
	 * unless the underlying bdev was hot-removed.
	 */
	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(name->vbdev_name, bdev->name) == 0) {
			TAILQ_REMOVE(&g_bdev_names, name, link);
			free(name->bdev_name);
			free(name->vbdev_name);
			free(name->drv_name);
			free(name->key);
			free(name->key2);
			free(name);
			break;
		}
	}

	/* Additional cleanup happens in the destruct callback. */
	spdk_bdev_unregister(bdev, cb_fn, cb_arg);
}

/* Because we specified this function in our crypto bdev function table when we
 * registered our crypto bdev, we'll get this call anytime a new bdev shows up.
 * Here we need to decide if we care about it and if so what to do. We
 * parsed the config file at init so we check the new bdev against the list
 * we built up at that time and if the user configured us to attach to this
 * bdev, here's where we do it.
 */
static void
vbdev_crypto_examine(struct spdk_bdev *bdev)
{
	vbdev_crypto_claim(spdk_bdev_get_name(bdev));
	spdk_bdev_module_examine_done(&crypto_if);
}

SPDK_LOG_REGISTER_COMPONENT(vbdev_crypto)
