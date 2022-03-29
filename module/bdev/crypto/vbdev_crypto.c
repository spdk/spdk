/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
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
#include "spdk/likely.h"
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
#define MAX_NUM_DRV_TYPES 3

/* The VF spread is the number of queue pairs between virtual functions, we use this to
 * load balance the QAT device.
 */
#define QAT_VF_SPREAD 32
static uint8_t g_qat_total_qp = 0;
static uint8_t g_next_qat_index;

const char *g_driver_names[MAX_NUM_DRV_TYPES] = { AESNI_MB, QAT, MLX5 };

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
static TAILQ_HEAD(, device_qp) g_device_qp_mlx5 = TAILQ_HEAD_INITIALIZER(g_device_qp_mlx5);
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

/* At this moment DPDK descriptors allocation for mlx5 has some issues. We use 512
 * as an compromise value between performance and the time spent for initialization. */
#define CRYPTO_QP_DESCRIPTORS_MLX5	512

#define AESNI_MB_NUM_QP		64

/* Common for suported devices. */
#define DEFAULT_NUM_XFORMS           2
#define IV_OFFSET (sizeof(struct rte_crypto_op) + \
                sizeof(struct rte_crypto_sym_op) + \
                (DEFAULT_NUM_XFORMS * \
                 sizeof(struct rte_crypto_sym_xform)))
#define IV_LENGTH		     16
#define QUEUED_OP_OFFSET (IV_OFFSET + IV_LENGTH)

static void _complete_internal_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void _complete_internal_read(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void _complete_internal_write(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void vbdev_crypto_examine(struct spdk_bdev *bdev);
static int vbdev_crypto_claim(const char *bdev_name);
static void vbdev_crypto_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);

struct bdev_names {
	struct vbdev_crypto_opts	*opts;
	TAILQ_ENTRY(bdev_names)		link;
};

/* List of crypto_bdev names and their base bdevs via configuration file. */
static TAILQ_HEAD(, bdev_names) g_bdev_names = TAILQ_HEAD_INITIALIZER(g_bdev_names);

struct vbdev_crypto {
	struct spdk_bdev		*base_bdev;		/* the thing we're attaching to */
	struct spdk_bdev_desc		*base_desc;		/* its descriptor we get from open */
	struct spdk_bdev		crypto_bdev;		/* the crypto virtual bdev */
	struct vbdev_crypto_opts	*opts;			/* crypto options such as key, cipher */
	uint32_t			qp_desc_nr;             /* number of qp descriptors */
	struct rte_cryptodev_sym_session *session_encrypt;	/* encryption session for this bdev */
	struct rte_cryptodev_sym_session *session_decrypt;	/* decryption session for this bdev */
	struct rte_crypto_sym_xform	cipher_xform;		/* crypto control struct for this bdev */
	TAILQ_ENTRY(vbdev_crypto)	link;
	struct spdk_thread		*thread;		/* thread where base device is opened */
};

/* List of virtual bdevs and associated info for each. We keep the device friendly name here even
 * though its also in the device struct because we use it early on.
 */
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
	uint32_t qp_desc_nr;
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
		SPDK_ERRLOG("Failed to configure cryptodev %u: error %d\n",
			    cdev_id, rc);
		rc = -EINVAL;
		goto err;
	}

	/* Select the right device/qp list based on driver name
	 * or error if it does not exist.
	 */
	if (strcmp(device->cdev_info.driver_name, QAT) == 0) {
		dev_qp_head = (struct device_qps *)&g_device_qp_qat;
		qp_desc_nr = CRYPTO_QP_DESCRIPTORS;
	} else if (strcmp(device->cdev_info.driver_name, AESNI_MB) == 0) {
		dev_qp_head = (struct device_qps *)&g_device_qp_aesni_mb;
		qp_desc_nr = CRYPTO_QP_DESCRIPTORS;
	} else if (strcmp(device->cdev_info.driver_name, MLX5) == 0) {
		dev_qp_head = (struct device_qps *)&g_device_qp_mlx5;
		qp_desc_nr = CRYPTO_QP_DESCRIPTORS_MLX5;
	} else {
		SPDK_ERRLOG("Failed to start device %u. Invalid driver name \"%s\"\n",
			    cdev_id, device->cdev_info.driver_name);
		rc = -EINVAL;
		goto err_qp_setup;
	}

	struct rte_cryptodev_qp_conf qp_conf = {
		.nb_descriptors = qp_desc_nr,
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
				    "cryptodev %u: error %d\n", j, cdev_id, rc);
			rc = -EINVAL;
			goto err_qp_setup;
		}
	}

	rc = rte_cryptodev_start(cdev_id);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to start device %u: error %d\n",
			    cdev_id, rc);
		rc = -EINVAL;
		goto err_dev_start;
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
		if (dev_qp->device->cdev_id != device->cdev_id) {
			continue;
		}
		TAILQ_REMOVE(dev_qp_head, dev_qp, link);
		if (dev_qp_head == (struct device_qps *)&g_device_qp_qat) {
			g_qat_total_qp--;
		}
		free(dev_qp);
	}
	rte_cryptodev_stop(cdev_id);
err_dev_start:
err_qp_setup:
	rte_cryptodev_close(cdev_id);
err:
	free(device);

	return rc;
}

static void
release_vbdev_dev(struct vbdev_dev *device)
{
	struct device_qp *dev_qp;
	struct device_qp *tmp_qp;
	TAILQ_HEAD(device_qps, device_qp) *dev_qp_head = NULL;

	assert(device);

	/* Select the right device/qp list based on driver name. */
	if (strcmp(device->cdev_info.driver_name, QAT) == 0) {
		dev_qp_head = (struct device_qps *)&g_device_qp_qat;
	} else if (strcmp(device->cdev_info.driver_name, AESNI_MB) == 0) {
		dev_qp_head = (struct device_qps *)&g_device_qp_aesni_mb;
	} else if (strcmp(device->cdev_info.driver_name, MLX5) == 0) {
		dev_qp_head = (struct device_qps *)&g_device_qp_mlx5;
	}
	if (dev_qp_head) {
		TAILQ_FOREACH_SAFE(dev_qp, dev_qp_head, link, tmp_qp) {
			/* Remove only qps of our device even if the driver names matches. */
			if (dev_qp->device->cdev_id != device->cdev_id) {
				continue;
			}
			TAILQ_REMOVE(dev_qp_head, dev_qp, link);
			if (dev_qp_head == (struct device_qps *)&g_device_qp_qat) {
				g_qat_total_qp--;
			}
			free(dev_qp);
		}
	}
	rte_cryptodev_stop(device->cdev_id);
	rte_cryptodev_close(device->cdev_id);
	free(device);
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
	int i, rc;
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
		SPDK_NOTICELOG("Failed to create virtual PMD %s: error %d. "
			       "Possibly %s is not supported by DPDK library. "
			       "Keep going...\n", AESNI_MB, rc, AESNI_MB);
	}

	/* If we have no crypto devices, there's no reason to continue. */
	cdev_count = rte_cryptodev_count();
	SPDK_NOTICELOG("Found crypto devices: %d\n", (int)cdev_count);
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

	/* We use per op private data as suggested by DPDK and to store the IV and
	 * our own struct for queueing ops.
	 */
	g_crypto_op_mp = rte_crypto_op_pool_create("op_mp",
			 RTE_CRYPTO_OP_TYPE_SYMMETRIC,
			 NUM_MBUFS,
			 POOL_CACHE_SIZE,
			 (DEFAULT_NUM_XFORMS *
			  sizeof(struct rte_crypto_sym_xform)) +
			 IV_LENGTH + QUEUED_OP_LENGTH,
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
		release_vbdev_dev(device);
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
		/* This also releases chained mbufs if any. */
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
		/* This also releases chained mbufs if any. */
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

/* Allocate the new mbuf of @remainder size with data pointed by @addr and attach
 * it to the @orig_mbuf. */
static int
mbuf_chain_remainder(struct spdk_bdev_io *bdev_io, struct rte_mbuf *orig_mbuf,
		     uint8_t *addr, uint32_t remainder)
{
	uint64_t phys_addr, phys_len;
	struct rte_mbuf *chain_mbuf;
	int rc;

	phys_len = remainder;
	phys_addr = spdk_vtophys((void *)addr, &phys_len);
	if (spdk_unlikely(phys_addr == SPDK_VTOPHYS_ERROR || phys_len != remainder)) {
		return -EFAULT;
	}
	rc = rte_pktmbuf_alloc_bulk(g_mbuf_mp, (struct rte_mbuf **)&chain_mbuf, 1);
	if (spdk_unlikely(rc)) {
		return -ENOMEM;
	}
	/* Store context in every mbuf as we don't know anything about completion order */
	*RTE_MBUF_DYNFIELD(chain_mbuf, g_mbuf_offset, uint64_t *) = (uint64_t)bdev_io;
	rte_pktmbuf_attach_extbuf(chain_mbuf, addr, phys_addr, phys_len, &g_shinfo);
	rte_pktmbuf_append(chain_mbuf, phys_len);

	/* Chained buffer is released by rte_pktbuf_free_bulk() automagicaly. */
	rte_pktmbuf_chain(orig_mbuf, chain_mbuf);
	return 0;
}

/* Attach data buffer pointed by @addr to @mbuf. Return utilized len of the
 * contiguous space that was physically available. */
static uint64_t
mbuf_attach_buf(struct spdk_bdev_io *bdev_io, struct rte_mbuf *mbuf,
		uint8_t *addr, uint32_t len)
{
	uint64_t phys_addr, phys_len;

	/* Store context in every mbuf as we don't know anything about completion order */
	*RTE_MBUF_DYNFIELD(mbuf, g_mbuf_offset, uint64_t *) = (uint64_t)bdev_io;

	phys_len = len;
	phys_addr = spdk_vtophys((void *)addr, &phys_len);
	if (spdk_unlikely(phys_addr == SPDK_VTOPHYS_ERROR || phys_len == 0)) {
		return 0;
	}
	assert(phys_len <= len);

	/* Set the mbuf elements address and length. */
	rte_pktmbuf_attach_extbuf(mbuf, addr, phys_addr, phys_len, &g_shinfo);
	rte_pktmbuf_append(mbuf, phys_len);

	return phys_len;
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
		SPDK_ERRLOG("Failed to get src_mbufs!\n");
		return -ENOMEM;
	}

	/* Get the same amount but these buffers to describe the encrypted data location (dst). */
	if (crypto_op == RTE_CRYPTO_CIPHER_OP_ENCRYPT) {
		rc = rte_pktmbuf_alloc_bulk(g_mbuf_mp, dst_mbufs, cryop_cnt);
		if (rc) {
			SPDK_ERRLOG("Failed to get dst_mbufs!\n");
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
		SPDK_ERRLOG("Failed to allocate crypto ops!\n");
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
		uint64_t phys_len;
		uint32_t remainder;
		uint64_t op_block_offset;

		phys_len = mbuf_attach_buf(bdev_io, src_mbufs[crypto_index],
					   current_iov, crypto_len);
		if (spdk_unlikely(phys_len == 0)) {
			goto error_attach_session;
			rc = -EFAULT;
		}

		/* Handle the case of page boundary. */
		remainder = crypto_len - phys_len;
		if (spdk_unlikely(remainder > 0)) {
			rc = mbuf_chain_remainder(bdev_io, src_mbufs[crypto_index],
						  current_iov + phys_len, remainder);
			if (spdk_unlikely(rc)) {
				goto error_attach_session;
			}
		}

		/* Set the IV - we use the LBA of the crypto_op */
		iv_ptr = rte_crypto_op_ctod_offset(crypto_ops[crypto_index], uint8_t *,
						   IV_OFFSET);
		memset(iv_ptr, 0, IV_LENGTH);
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
			phys_len = mbuf_attach_buf(bdev_io, dst_mbufs[crypto_index],
						   buf_addr, crypto_len);
			if (spdk_unlikely(phys_len == 0)) {
				rc = -EFAULT;
				goto error_attach_session;
			}

			crypto_ops[crypto_index]->sym->m_dst = dst_mbufs[crypto_index];
			en_offset += phys_len;

			/* Handle the case of page boundary. */
			remainder = crypto_len - phys_len;
			if (spdk_unlikely(remainder > 0)) {
				rc = mbuf_chain_remainder(bdev_io, dst_mbufs[crypto_index],
							  buf_addr + phys_len, remainder);
				if (spdk_unlikely(rc)) {
					goto error_attach_session;
				}
				en_offset += remainder;
			}

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
	burst = spdk_min(cryop_cnt, io_ctx->crypto_bdev->qp_desc_nr);
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
		/* This also releases chained mbufs if any. */
		rte_pktmbuf_free_bulk(dst_mbufs, cryop_cnt);
	}
	if (allocated > 0) {
		rte_mempool_put_bulk(g_crypto_op_mp, (void **)crypto_ops,
				     allocated);
	}
error_get_dst:
	/* This also releases chained mbufs if any. */
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
			SPDK_ERRLOG("Failed to decrypt!\n");
		}
	} else {
		SPDK_ERRLOG("Failed to read prior to decrypting!\n");
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
			SPDK_ERRLOG("Failed to submit bdev_io!\n");
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
			SPDK_ERRLOG("Failed to submit bdev_io!\n");
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
			SPDK_ERRLOG("Failed to submit bdev_io!\n");
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
	crypto_bdev->opts = NULL;
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
	char *hexkey = NULL, *hexkey2 = NULL;
	int rc = 0;

	hexkey = hexlify(crypto_bdev->opts->key,
			 crypto_bdev->opts->key_size);
	if (!hexkey) {
		return -ENOMEM;
	}

	if (crypto_bdev->opts->key2) {
		hexkey2 = hexlify(crypto_bdev->opts->key2,
				  crypto_bdev->opts->key2_size);
		if (!hexkey2) {
			rc = -ENOMEM;
			goto out_err;
		}
	}

	spdk_json_write_name(w, "crypto");
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(crypto_bdev->base_bdev));
	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&crypto_bdev->crypto_bdev));
	spdk_json_write_named_string(w, "crypto_pmd", crypto_bdev->opts->drv_name);
	spdk_json_write_named_string(w, "key", hexkey);
	if (hexkey2) {
		spdk_json_write_named_string(w, "key2", hexkey2);
	}
	spdk_json_write_named_string(w, "cipher", crypto_bdev->opts->cipher);
	spdk_json_write_object_end(w);
out_err:
	if (hexkey) {
		memset(hexkey, 0, strlen(hexkey));
		free(hexkey);
	}
	if (hexkey2) {
		memset(hexkey2, 0, strlen(hexkey2));
		free(hexkey2);
	}
	return rc;
}

static int
vbdev_crypto_config_json(struct spdk_json_write_ctx *w)
{
	struct vbdev_crypto *crypto_bdev;

	TAILQ_FOREACH(crypto_bdev, &g_vbdev_crypto, link) {
		char *hexkey = NULL, *hexkey2 = NULL;

		hexkey = hexlify(crypto_bdev->opts->key,
				 crypto_bdev->opts->key_size);
		if (!hexkey) {
			return -ENOMEM;
		}

		if (crypto_bdev->opts->key2) {
			hexkey2 = hexlify(crypto_bdev->opts->key2,
					  crypto_bdev->opts->key2_size);
			if (!hexkey2) {
				memset(hexkey, 0, strlen(hexkey));
				free(hexkey);
				return -ENOMEM;
			}
		}

		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "bdev_crypto_create");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(crypto_bdev->base_bdev));
		spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&crypto_bdev->crypto_bdev));
		spdk_json_write_named_string(w, "crypto_pmd", crypto_bdev->opts->drv_name);
		spdk_json_write_named_string(w, "key", hexkey);
		if (hexkey2) {
			spdk_json_write_named_string(w, "key2", hexkey2);
		}
		spdk_json_write_named_string(w, "cipher", crypto_bdev->opts->cipher);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);

		if (hexkey) {
			memset(hexkey, 0, strlen(hexkey));
			free(hexkey);
		}
		if (hexkey2) {
			memset(hexkey2, 0, strlen(hexkey2));
			free(hexkey2);
		}
	}
	return 0;
}

/* Helper function for the channel creation callback. */
static void
_assign_device_qp(struct vbdev_crypto *crypto_bdev, struct device_qp *device_qp,
		  struct crypto_io_channel *crypto_ch)
{
	pthread_mutex_lock(&g_device_qp_lock);
	if (strcmp(crypto_bdev->opts->drv_name, QAT) == 0) {
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
	} else if (strcmp(crypto_bdev->opts->drv_name, AESNI_MB) == 0) {
		TAILQ_FOREACH(device_qp, &g_device_qp_aesni_mb, link) {
			if (device_qp->in_use == false) {
				crypto_ch->device_qp = device_qp;
				device_qp->in_use = true;
				break;
			}
		}
	} else if (strcmp(crypto_bdev->opts->drv_name, MLX5) == 0) {
		TAILQ_FOREACH(device_qp, &g_device_qp_mlx5, link) {
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
vbdev_crypto_insert_name(struct vbdev_crypto_opts *opts, struct bdev_names **out)
{
	struct bdev_names *name;
	bool found = false;
	int j;

	assert(opts);
	assert(out);

	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(opts->vbdev_name, name->opts->vbdev_name) == 0) {
			SPDK_ERRLOG("Crypto bdev %s already exists\n", opts->vbdev_name);
			return -EEXIST;
		}
	}

	for (j = 0; j < MAX_NUM_DRV_TYPES ; j++) {
		if (strcmp(opts->drv_name, g_driver_names[j]) == 0) {
			found = true;
			break;
		}
	}
	if (!found) {
		SPDK_ERRLOG("Crypto PMD type %s is not supported.\n", opts->drv_name);
		return -EINVAL;
	}

	name = calloc(1, sizeof(struct bdev_names));
	if (!name) {
		SPDK_ERRLOG("Failed to allocate memory for bdev_names.\n");
		return -ENOMEM;
	}

	name->opts = opts;
	TAILQ_INSERT_TAIL(&g_bdev_names, name, link);
	*out = name;

	return 0;
}

void
free_crypto_opts(struct vbdev_crypto_opts *opts)
{
	free(opts->bdev_name);
	free(opts->vbdev_name);
	free(opts->drv_name);
	if (opts->xts_key) {
		memset(opts->xts_key, 0,
		       opts->key_size + opts->key2_size);
		free(opts->xts_key);
	}
	memset(opts->key, 0, opts->key_size);
	free(opts->key);
	opts->key_size = 0;
	if (opts->key2) {
		memset(opts->key2, 0, opts->key2_size);
		free(opts->key2);
	}
	opts->key2_size = 0;
	free(opts);
}

static void
vbdev_crypto_delete_name(struct bdev_names *name)
{
	TAILQ_REMOVE(&g_bdev_names, name, link);
	if (name->opts) {
		free_crypto_opts(name->opts);
		name->opts = NULL;
	}
	free(name);
}

/* RPC entry point for crypto creation. */
int
create_crypto_disk(struct vbdev_crypto_opts *opts)
{
	struct bdev_names *name = NULL;
	int rc;

	rc = vbdev_crypto_insert_name(opts, &name);
	if (rc) {
		return rc;
	}

	rc = vbdev_crypto_claim(opts->bdev_name);
	if (rc == -ENODEV) {
		SPDK_NOTICELOG("vbdev creation deferred pending base bdev arrival\n");
		rc = 0;
	}

	if (rc) {
		assert(name != NULL);
		/* In case of error we let the caller function to deallocate @opts
		 * since it is its responsibiltiy. Setting name->opts = NULL let's
		 * vbdev_crypto_delete_name() know it does not have to do anything
		 * about @opts.
		 */
		name->opts = NULL;
		vbdev_crypto_delete_name(name);
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

	while ((name = TAILQ_FIRST(&g_bdev_names))) {
		vbdev_crypto_delete_name(name);
	}

	while ((device = TAILQ_FIRST(&g_vbdev_devs))) {
		TAILQ_REMOVE(&g_vbdev_devs, device, link);
		release_vbdev_dev(device);
	}
	rte_vdev_uninit(AESNI_MB);

	/* These are removed in release_vbdev_dev() */
	assert(TAILQ_EMPTY(&g_device_qp_qat));
	assert(TAILQ_EMPTY(&g_device_qp_aesni_mb));
	assert(TAILQ_EMPTY(&g_device_qp_mlx5));

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
	uint8_t key_size;
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
		if (strcmp(name->opts->bdev_name, bdev_name) != 0) {
			continue;
		}
		SPDK_DEBUGLOG(vbdev_crypto, "Match on %s\n", bdev_name);

		vbdev = calloc(1, sizeof(struct vbdev_crypto));
		if (!vbdev) {
			SPDK_ERRLOG("Failed to allocate memory for crypto_bdev.\n");
			rc = -ENOMEM;
			goto error_vbdev_alloc;
		}
		vbdev->crypto_bdev.product_name = "crypto";

		vbdev->crypto_bdev.name = strdup(name->opts->vbdev_name);
		if (!vbdev->crypto_bdev.name) {
			SPDK_ERRLOG("Failed to allocate memory for crypto_bdev name.\n");
			rc = -ENOMEM;
			goto error_bdev_name;
		}

		rc = spdk_bdev_open_ext(bdev_name, true, vbdev_crypto_base_bdev_event_cb,
					NULL, &vbdev->base_desc);
		if (rc) {
			if (rc != -ENODEV) {
				SPDK_ERRLOG("Failed to open bdev %s: error %d\n", bdev_name, rc);
			}
			goto error_open;
		}

		bdev = spdk_bdev_desc_get_bdev(vbdev->base_desc);
		vbdev->base_bdev = bdev;

		if (strcmp(name->opts->drv_name, MLX5) == 0) {
			vbdev->qp_desc_nr = CRYPTO_QP_DESCRIPTORS_MLX5;
		} else {
			vbdev->qp_desc_nr = CRYPTO_QP_DESCRIPTORS;
		}

		vbdev->crypto_bdev.write_cache = bdev->write_cache;
		if (strcmp(name->opts->drv_name, QAT) == 0) {
			vbdev->crypto_bdev.required_alignment =
				spdk_max(spdk_u32log2(bdev->blocklen), bdev->required_alignment);
			SPDK_NOTICELOG("QAT in use: Required alignment set to %u\n",
				       vbdev->crypto_bdev.required_alignment);
			SPDK_NOTICELOG("QAT using cipher: %s\n", name->opts->cipher);
		} else if (strcmp(name->opts->drv_name, MLX5) == 0) {
			vbdev->crypto_bdev.required_alignment = bdev->required_alignment;
			SPDK_NOTICELOG("MLX5 using cipher: %s\n", name->opts->cipher);
		} else {
			vbdev->crypto_bdev.required_alignment = bdev->required_alignment;
			SPDK_NOTICELOG("AESNI_MB using cipher: %s\n", name->opts->cipher);
		}
		vbdev->cipher_xform.cipher.iv.length = IV_LENGTH;

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

		/* Assign crypto opts from the name. The pointer is valid up to the point
		 * the module is unloaded and all names removed from the list. */
		vbdev->opts = name->opts;

		TAILQ_INSERT_TAIL(&g_vbdev_crypto, vbdev, link);

		spdk_io_device_register(vbdev, crypto_bdev_ch_create_cb, crypto_bdev_ch_destroy_cb,
					sizeof(struct crypto_io_channel), vbdev->crypto_bdev.name);

		/* Save the thread where the base device is opened */
		vbdev->thread = spdk_get_thread();

		rc = spdk_bdev_module_claim_bdev(bdev, vbdev->base_desc, vbdev->crypto_bdev.module);
		if (rc) {
			SPDK_ERRLOG("Failed to claim bdev %s\n", spdk_bdev_get_name(bdev));
			goto error_claim;
		}

		/* To init the session we have to get the cryptoDev device ID for this vbdev */
		TAILQ_FOREACH(device, &g_vbdev_devs, link) {
			if (strcmp(device->cdev_info.driver_name, vbdev->opts->drv_name) == 0) {
				found = true;
				break;
			}
		}
		if (found == false) {
			SPDK_ERRLOG("Failed to match crypto device driver to crypto vbdev.\n");
			rc = -EINVAL;
			goto error_cant_find_devid;
		}

		/* Get sessions. */
		vbdev->session_encrypt = rte_cryptodev_sym_session_create(g_session_mp);
		if (NULL == vbdev->session_encrypt) {
			SPDK_ERRLOG("Failed to create encrypt crypto session.\n");
			rc = -EINVAL;
			goto error_session_en_create;
		}

		vbdev->session_decrypt = rte_cryptodev_sym_session_create(g_session_mp);
		if (NULL == vbdev->session_decrypt) {
			SPDK_ERRLOG("Failed to create decrypt crypto session.\n");
			rc = -EINVAL;
			goto error_session_de_create;
		}

		/* Init our per vbdev xform with the desired cipher options. */
		vbdev->cipher_xform.type = RTE_CRYPTO_SYM_XFORM_CIPHER;
		vbdev->cipher_xform.cipher.iv.offset = IV_OFFSET;
		if (strcmp(vbdev->opts->cipher, AES_CBC) == 0) {
			vbdev->cipher_xform.cipher.key.data = vbdev->opts->key;
			vbdev->cipher_xform.cipher.key.length = vbdev->opts->key_size;
			vbdev->cipher_xform.cipher.algo = RTE_CRYPTO_CIPHER_AES_CBC;
		} else if (strcmp(vbdev->opts->cipher, AES_XTS) == 0) {
			key_size = vbdev->opts->key_size + vbdev->opts->key2_size;
			vbdev->cipher_xform.cipher.key.data = vbdev->opts->xts_key;
			vbdev->cipher_xform.cipher.key.length = key_size;
			vbdev->cipher_xform.cipher.algo = RTE_CRYPTO_CIPHER_AES_XTS;
		} else {
			SPDK_ERRLOG("Invalid cipher name %s.\n", vbdev->opts->cipher);
			rc = -EINVAL;
			goto error_session_de_create;
		}
		vbdev->cipher_xform.cipher.iv.length = IV_LENGTH;

		vbdev->cipher_xform.cipher.op = RTE_CRYPTO_CIPHER_OP_ENCRYPT;
		rc = rte_cryptodev_sym_session_init(device->cdev_id, vbdev->session_encrypt,
						    &vbdev->cipher_xform,
						    g_session_mp_priv ? g_session_mp_priv : g_session_mp);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to init encrypt session: error %d\n", rc);
			rc = -EINVAL;
			goto error_session_init;
		}

		vbdev->cipher_xform.cipher.op = RTE_CRYPTO_CIPHER_OP_DECRYPT;
		rc = rte_cryptodev_sym_session_init(device->cdev_id, vbdev->session_decrypt,
						    &vbdev->cipher_xform,
						    g_session_mp_priv ? g_session_mp_priv : g_session_mp);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to init decrypt session: error %d\n", rc);
			rc = -EINVAL;
			goto error_session_init;
		}

		rc = spdk_bdev_register(&vbdev->crypto_bdev);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to register vbdev: error %d\n", rc);
			rc = -EINVAL;
			goto error_bdev_register;
		}
		SPDK_DEBUGLOG(vbdev_crypto, "Registered io_device and virtual bdev for: %s\n",
			      vbdev->opts->vbdev_name);
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
	spdk_bdev_close(vbdev->base_desc);
error_open:
	free(vbdev->crypto_bdev.name);
error_bdev_name:
	free(vbdev);
error_vbdev_alloc:
	g_number_of_claimed_volumes--;
	return rc;
}

/* RPC entry for deleting a crypto vbdev. */
void
delete_crypto_disk(const char *bdev_name, spdk_delete_crypto_complete cb_fn,
		   void *cb_arg)
{
	struct bdev_names *name;
	int rc;

	/* Some cleanup happens in the destruct callback. */
	rc = spdk_bdev_unregister_by_name(bdev_name, &crypto_if, cb_fn, cb_arg);
	if (rc == 0) {
		/* Remove the association (vbdev, bdev) from g_bdev_names. This is required so that the
		 * vbdev does not get re-created if the same bdev is constructed at some other time,
		 * unless the underlying bdev was hot-removed.
		 */
		TAILQ_FOREACH(name, &g_bdev_names, link) {
			if (strcmp(name->opts->vbdev_name, bdev_name) == 0) {
				vbdev_crypto_delete_name(name);
				break;
			}
		}
	} else {
		cb_fn(cb_arg, rc);
	}
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
