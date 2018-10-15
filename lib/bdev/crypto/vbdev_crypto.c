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
#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/io_channel.h"
#include "spdk/bdev_module.h"

#include <rte_config.h>
#include <rte_bus_vdev.h>
#include <rte_crypto.h>
#include <rte_cryptodev.h>
#include <rte_cryptodev_pmd.h>

/* To add support for new device types, follow the examples of the following...
 * Note that the string names are defined by the DPDK PMD in question so be
 * sure to use the exact names.
 */
#define MAX_NUM_DRV_TYPES 2
#define AESNI_MB "crypto_aesni_mb"
#define QAT "crypto_qat"
const char *g_driver_names[MAX_NUM_DRV_TYPES] = { AESNI_MB, QAT };

/* Global list of available crypto devices. */
struct vbdev_dev {
	struct rte_cryptodev_info	cdev_info;	/* includes device friendly name */
	uint8_t				cdev_id;	/* identifier for the device */
	TAILQ_ENTRY(vbdev_dev)		link;
};
static TAILQ_HEAD(, vbdev_dev) g_vbdev_devs = TAILQ_HEAD_INITIALIZER(g_vbdev_devs);

/* Global list and lock for unique device/queue pair combos */
struct device_qp {
	struct vbdev_dev		*device;	/* ptr to crypto device */
	uint8_t				qp;		/* queue pair for this node */
	bool				in_use;		/* whether this node is in use or not */
	TAILQ_ENTRY(device_qp)		link;
};
static TAILQ_HEAD(, device_qp) g_device_qp = TAILQ_HEAD_INITIALIZER(g_device_qp);
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
 * QAT drvier just dequeues what has been completed already.
 */
#define MAX_DEQUEUE_BURST_SIZE	64

/* When enqueueing, we need to supply the crypto driver with an array of pointers to
 * operation structs. As each of these can be max 512B, we can adjust the CRYPTO_MAX_IO
 * value in conjunction with the the other defines to make sure we're not using crazy amounts
 * of memory. All of these numbers can and probably should be adjusted based on the
 * workload. By default we'll use the worst case (smallest) block size for the
 * minimum number of array entries. As an example, a CRYPTO_MAX_IO size of 64K with 512B
 * blocks would give us an enqueue array size of 128.
 */
#define MAX_ENQUEUE_ARRAY_SIZE (CRYPTO_MAX_IO / 512)

/* The number of MBUFS we need must be a power of two and to support other small IOs
 * in addition to the limits mentioned above, we go to the next power of two. It is
 * big number because it is one mempool for source and desitnation mbufs. It may
 * need to be bigger to support multiple crypto drivers at once.
 */
#define NUM_MBUFS		32768
#define POOL_CACHE_SIZE		256
#define NUM_SESSIONS		NUM_MBUFS
#define SESS_MEMPOOL_CACHE_SIZE 256

/* This is the max number of IOs we can supply to any crypto device QP at one time.
 * It can vary between drivers.
 */
#define CRYPTO_QP_DESCRIPTORS	2048

/* Specific to AES_CBC. */
#define AES_CBC_IV_LENGTH	16
#define AES_CBC_KEY_LENGTH	16

/* Common for suported devices. */
#define IV_OFFSET            (sizeof(struct rte_crypto_op) + \
				sizeof(struct rte_crypto_sym_op))

static void _complete_internal_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void _complete_internal_read(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void _complete_internal_write(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void vbdev_crypto_examine(struct spdk_bdev *bdev);
static int vbdev_crypto_claim(struct spdk_bdev *bdev);

/* list of crypto_bdev names and their base bdevs via configuration file.
 * Used so we can parse the conf once at init and use this list in examine().
 */
struct bdev_names {
	char			*vbdev_name;	/* name of the vbdev to create */
	char			*bdev_name;	/* base bdev name */

	/* Note, for dev/test we allow use of key in the config file, for production
	 * use, you must use an RPC to specify the key for security reasons.
	 */
	uint8_t			*key;		/* key per bdev */
	char			*drv_name;	/* name of the crypto device driver */
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
	char				*drv_name;		/* name of the crypto device driver */
	TAILQ_ENTRY(vbdev_crypto)	link;
};
static TAILQ_HEAD(, vbdev_crypto) g_vbdev_crypto = TAILQ_HEAD_INITIALIZER(g_vbdev_crypto);

/* Shared mempools between all devices on this system */
static struct spdk_mempool *g_session_mp = NULL;	/* session mempool */
static struct spdk_mempool *g_mbuf_mp = NULL;		/* mbuf mempool */
static struct rte_mempool *g_crypto_op_mp = NULL;	/* crypto operations, must be rte* mempool */

/* The crypto vbdev channel struct. It is allocated and freed on my behalf by the io channel code.
 * We store things in here that are needed on per thread basis like the base_channel for this thread,
 * and the poller for this thread.
 */
struct crypto_io_channel {
	struct spdk_io_channel		*base_ch;		/* IO channel of base device */
	struct spdk_poller		*poller;		/* completion poller */
	struct device_qp		*device_qp;		/* unique device/qp combination for this channel */
};

/* This is the crypto per IO context that the bdev layer allocates for us opaquely and attaches to
 * each IO for us.
 */
struct crypto_bdev_io {
	int cryop_cnt_remaining;			/* counter used when completing crypto ops */
	struct crypto_io_channel *crypto_ch;		/* need to store for crypto completion handling */
	struct vbdev_crypto *crypto_bdev;		/* the crypto node struct associated with this IO */
	enum rte_crypto_cipher_operation crypto_op;	/* the crypto control struct */
	struct rte_crypto_sym_xform	cipher_xform;	/* crypto control struct for this IO */
	struct spdk_bdev_io *orig_io;			/* the original IO */
	struct spdk_bdev_io *read_io;			/* the read IO we issued */

	/* Used for the single contigous buffer that serves as the crypto destination target for writes */
	uint64_t cry_num_blocks;			/* num of blocks for the contiguous buffer */
	uint64_t cry_offset_blocks;			/* block offset on media */
	struct iovec cry_iov;				/* iov representing contig write buffer */
};

/* This is called from the module's init function. We setup all crypto devices early on as we are unable
 * to easily dynamically configure queue pairs after the drivers are up and running.  So, here, we
 * configure the max capabilities of each device and assign threads to queue pairs as channels are
 * requested.
 */
static int
vbdev_crypto_init_crypto_drivers(void)
{
	uint8_t cdev_count;
	uint8_t cdrv_id, cdev_id, i, j;
	int rc = 0;
	struct vbdev_dev *device = NULL;
	struct device_qp *dev_qp = NULL;
	unsigned int max_sess_size = 0, sess_size;
	uint16_t num_lcores = rte_lcore_count();

	/* Only the first call, via RPC or module init should init the crypto drivers. */
	if (g_session_mp != NULL) {
		return 0;
	}

	/* We always init AESNI_MB */
	rc = rte_vdev_init(AESNI_MB, NULL);
	if (rc == 0) {
		SPDK_NOTICELOG("created virtual PMD %s\n", AESNI_MB);
	} else {
		SPDK_ERRLOG("error creating virtual PMD %s\n", AESNI_MB);
		return -EINVAL;
	}

	/* If we have no crypto devices, there's no reason to continue. */
	cdev_count = rte_cryptodev_count();
	if (cdev_count == 0) {
		return 0;
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

	g_session_mp = spdk_mempool_create("session_mp", NUM_SESSIONS * 2, max_sess_size,
					   SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
					   SPDK_ENV_SOCKET_ID_ANY);
	if (g_session_mp == NULL) {
		SPDK_ERRLOG("Cannot create session pool max size 0x%x\n", max_sess_size);
		return -ENOMEM;
	}

	g_mbuf_mp = spdk_mempool_create("mbuf_mp", NUM_MBUFS, sizeof(struct rte_mbuf),
					SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
					SPDK_ENV_SOCKET_ID_ANY);
	if (g_mbuf_mp == NULL) {
		SPDK_ERRLOG("Cannot create mbuf pool\n");
		rc = -ENOMEM;
		goto error_create_mbuf;
	}

	g_crypto_op_mp = rte_crypto_op_pool_create("op_mp",
			 RTE_CRYPTO_OP_TYPE_SYMMETRIC,
			 NUM_MBUFS,
			 POOL_CACHE_SIZE,
			 AES_CBC_IV_LENGTH,
			 rte_socket_id());
	if (g_crypto_op_mp == NULL) {
		SPDK_ERRLOG("Cannot create op pool\n");
		rc = -ENOMEM;
		goto error_create_op;
	}

	/*
	 * Now lets configure each device.
	 */
	for (i = 0; i < cdev_count; i++) {
		device = calloc(1, sizeof(struct vbdev_dev));
		if (!device) {
			rc = -ENOMEM;
			goto error_create_device;
		}

		/* Get details about this device. */
		rte_cryptodev_info_get(i, &device->cdev_info);
		cdrv_id = device->cdev_info.driver_id;
		cdev_id = device->cdev_id = i;

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
			goto error_qp;
		}

		/* Setup queue pairs. */
		struct rte_cryptodev_config conf = {
			.nb_queue_pairs = device->cdev_info.max_nb_queue_pairs,
			.socket_id = SPDK_ENV_SOCKET_ID_ANY
		};

		rc = rte_cryptodev_configure(cdev_id, &conf);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to configure cryptodev %u", cdev_id);
			rc = -EINVAL;
			goto error_dev_config;
		}

		struct rte_cryptodev_qp_conf qp_conf = {
			.nb_descriptors = CRYPTO_QP_DESCRIPTORS
		};

		/* Pre-setup all pottential qpairs now and assign them in the channel
		 * callback. If we were to create them there, we'd have to stop the
		 * entire device affecting all other threads that might be using it
		 * even on other queue pairs.
		 */
		for (j = 0; j < device->cdev_info.max_nb_queue_pairs; j++) {
			rc = rte_cryptodev_queue_pair_setup(cdev_id, j, &qp_conf, SOCKET_ID_ANY,
							    (struct rte_mempool *)g_session_mp);

			if (rc < 0) {
				SPDK_ERRLOG("Failed to setup queue pair %u on "
					    "cryptodev %u", j, cdev_id);
				rc = -EINVAL;
				goto error_qp_setup;
			}
		}

		rc = rte_cryptodev_start(cdev_id);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to start device %u: error %d\n",
				    cdev_id, rc);
			rc = -EINVAL;
			goto error_device_start;
		}

		/* Add to our list of available crypto devices. */
		TAILQ_INSERT_TAIL(&g_vbdev_devs, device, link);

		/* Build up list of device/qp combinations */
		for (j = 0; j < device->cdev_info.max_nb_queue_pairs; j++) {
			dev_qp = calloc(1, sizeof(struct device_qp));
			if (!dev_qp) {
				rc = -ENOMEM;
				goto error_create_devqp;
			}
			dev_qp->device = device;
			dev_qp->qp = j;
			dev_qp->in_use = false;
			TAILQ_INSERT_TAIL(&g_device_qp, dev_qp, link);
		}
	}
	return 0;

	/* Error cleanup paths. */
error_create_devqp:
	while ((dev_qp = TAILQ_FIRST(&g_device_qp))) {
		TAILQ_REMOVE(&g_device_qp, dev_qp, link);
		free(dev_qp);
	}
error_device_start:
error_qp_setup:
error_dev_config:
error_qp:
	free(device);
error_create_device:
	rte_mempool_free(g_crypto_op_mp);
error_create_op:
	spdk_mempool_free(g_mbuf_mp);
error_create_mbuf:
	spdk_mempool_free(g_session_mp);
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

	if (bdev_io->internal.status != SPDK_BDEV_IO_STATUS_FAILED) {
		if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {

			/* Complete the original IO and then free the one that we created
			 * as a result of issuing an IO via submit_reqeust.
			 */
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
			spdk_bdev_free_io(free_me);

		} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {

			/* Write the encrypted data. */
			rc = spdk_bdev_writev_blocks(crypto_bdev->base_desc, crypto_ch->base_ch,
						     &io_ctx->cry_iov, 1, io_ctx->cry_offset_blocks,
						     io_ctx->cry_num_blocks, _complete_internal_write,
						     bdev_io);
		} else {

			/* Something really went haywire if this function got called with a type
			 * other than read or write.
			 */
			rc = -1;
		}
	} else {
		/* If the poller found that one of the crypto ops had failed as part of this
		 * bdev_io it would have updated the internal status indicate failure.
		 */
		rc = -1;
	}

	if (rc != 0) {
		SPDK_ERRLOG("ERROR on crypto operation completion!\n");
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}

}

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
	int i, num_dequeued_ops;
	struct spdk_bdev_io *bdev_io = NULL;
	struct crypto_bdev_io *io_ctx = NULL;
	struct rte_crypto_op *dequeued_ops[MAX_DEQUEUE_BURST_SIZE];
	struct rte_crypto_op *mbufs_to_free[2 * MAX_DEQUEUE_BURST_SIZE];
	int num_mbufs = 0;

	/* Each run of the poller will get just what the device has available
	 * at the moment we call it, we don't check again after draining the
	 * first batch.
	 */
	num_dequeued_ops = rte_cryptodev_dequeue_burst(cdev_id, crypto_ch->device_qp->qp,
			   dequeued_ops, MAX_DEQUEUE_BURST_SIZE);

	/* Check if operation was processed successfully */
	for (i = 0; i < num_dequeued_ops; i++) {

		/* We don't know the order or association of the crypto ops wrt any
		 * partiular bdev_io so need to look at each and determine if it's
		 * the last one for it's bdev_io or not.
		 */
		bdev_io = (struct spdk_bdev_io *)dequeued_ops[i]->sym->m_src->userdata;
		assert(bdev_io != NULL);

		if (dequeued_ops[i]->status != RTE_CRYPTO_OP_STATUS_SUCCESS) {
			SPDK_ERRLOG("error with op %d status %u\n", i,
				    dequeued_ops[i]->status);
			/* Update the bdev status to error, we'll still process the
			 * rest of the crypto ops for this bdev_io though so they
			 * aren't left hanging.
			 */
			bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
		}

		io_ctx = (struct crypto_bdev_io *)bdev_io->driver_ctx;
		assert(io_ctx->cryop_cnt_remaining > 0);

		/* Return the associated src and dst mbufs by collecting them into
		 * an array that we can use the bulk API to free after the loop.
		 */
		dequeued_ops[i]->sym->m_src->userdata = NULL;
		mbufs_to_free[num_mbufs++] = (void *)dequeued_ops[i]->sym->m_src;
		if (dequeued_ops[i]->sym->m_dst) {
			mbufs_to_free[num_mbufs++] = (void *)dequeued_ops[i]->sym->m_dst;
		}

		/* done encrypting, complete the bdev_io */
		if (--io_ctx->cryop_cnt_remaining == 0) {

			/* Complete the IO */
			_crypto_operation_complete(bdev_io);

			/* Return session */
			rte_cryptodev_sym_session_clear(cdev_id, dequeued_ops[i]->sym->session);
			rte_cryptodev_sym_session_free(dequeued_ops[i]->sym->session);
		}
	}

	/* Now bulk free both mbufs and crypto operations. */
	if (num_dequeued_ops > 0) {
		rte_mempool_put_bulk(g_crypto_op_mp,
				     (void **)dequeued_ops,
				     num_dequeued_ops);
		assert(num_mbufs > 0);
		spdk_mempool_put_bulk(g_mbuf_mp,
				      (void **)mbufs_to_free,
				      num_mbufs);
	}

	return num_dequeued_ops;
}

/* We're either encrypting on the way down or decrypting on the way back. */
static int
_crypto_operation(struct spdk_bdev_io *bdev_io, enum rte_crypto_cipher_operation crypto_op)
{
	struct rte_cryptodev_sym_session *session;
	uint16_t num_enqueued_ops = 0;
	uint32_t cryop_cnt = bdev_io->u.bdev.num_blocks;
	struct crypto_bdev_io *io_ctx = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	struct crypto_io_channel *crypto_ch = io_ctx->crypto_ch;
	uint8_t cdev_id = crypto_ch->device_qp->device->cdev_id;
	uint32_t crypto_len = io_ctx->crypto_bdev->crypto_bdev.blocklen;
	uint64_t total_length = bdev_io->u.bdev.num_blocks * crypto_len;
	int rc;
	uint32_t enqueued = 0;
	uint32_t iov_index = 0;
	uint32_t allocated = 0;
	uint8_t *current_iov = NULL;
	uint64_t total_remaining = 0;
	uint64_t current_iov_remaining = 0;
	int completed = 0;
	int crypto_index = 0;
	uint32_t en_offset = 0;
	struct rte_crypto_op *crypto_ops[MAX_ENQUEUE_ARRAY_SIZE];
	struct rte_mbuf *src_mbufs[MAX_ENQUEUE_ARRAY_SIZE];
	struct rte_mbuf *dst_mbufs[MAX_ENQUEUE_ARRAY_SIZE];
	int burst;

	assert((bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen) <= CRYPTO_MAX_IO);

	/* Get the number of source mbufs that we need. These will always be 1:1 because we
	 * don't support chaining. The reason we don't is because of our decision to use
	 * LBA as IV, there can be no case where we'd need >1 mbuf per crypto op or the
	 * op would be > 1 LBA.
	 */
	rc = spdk_mempool_get_bulk(g_mbuf_mp, (void **)&src_mbufs[0], cryop_cnt);
	if (rc) {
		SPDK_ERRLOG("ERROR trying to get src_mbufs!\n");
		return -ENOMEM;
	}

	/* Get the same amount but these buffers to describe the encrypted data location (dst). */
	if (crypto_op == RTE_CRYPTO_CIPHER_OP_ENCRYPT) {
		rc = spdk_mempool_get_bulk(g_mbuf_mp, (void **)&dst_mbufs[0], cryop_cnt);
		if (rc) {
			SPDK_ERRLOG("ERROR trying to get dst_mbufs!\n");
			rc = -ENOMEM;
			goto error_get_dst;
		}
	}

	/* Allocate crypto operations. */
	allocated = rte_crypto_op_bulk_alloc(g_crypto_op_mp,
					     RTE_CRYPTO_OP_TYPE_SYMMETRIC,
					     crypto_ops, cryop_cnt);
	if (allocated < cryop_cnt) {
		SPDK_ERRLOG("ERROR trying to get crypto ops!\n");
		rc = -ENOMEM;
		goto error_get_ops;
	}

	/* Get sessions. */
	session = rte_cryptodev_sym_session_create((struct rte_mempool *)g_session_mp);
	if (NULL == session) {
		SPDK_ERRLOG("ERROR trying to create crypto session!\n");
		rc = -EINVAL;
		goto error_session_create;
	}

	/* Init our session with the desired cipher options. */
	io_ctx->cipher_xform.type = RTE_CRYPTO_SYM_XFORM_CIPHER;
	io_ctx->cipher_xform.cipher.key.data = io_ctx->crypto_bdev->key;
	io_ctx->cipher_xform.cipher.op = io_ctx->crypto_op = crypto_op;
	io_ctx->cipher_xform.cipher.iv.offset = IV_OFFSET;
	io_ctx->cipher_xform.cipher.algo = RTE_CRYPTO_CIPHER_AES_CBC;
	io_ctx->cipher_xform.cipher.key.length = AES_CBC_KEY_LENGTH;
	io_ctx->cipher_xform.cipher.iv.length = AES_CBC_IV_LENGTH;

	rc = rte_cryptodev_sym_session_init(cdev_id, session,
					    &io_ctx->cipher_xform,
					    (struct rte_mempool *)g_session_mp);
	if (rc < 0) {
		SPDK_ERRLOG("ERROR trying to init crypto session!\n");
		rc = -EINVAL;
		goto error_session_init;
	}

	/* For encryption, we need to prepare a single contiguous buffer as the encryption
	 * destination, we'll then pass that along for the write after encryption is done.
	 * This is done to avoiding encrypting the provided write buffer which may be
	 * undesirable in some use cases.
	 */
	if (crypto_op == RTE_CRYPTO_CIPHER_OP_ENCRYPT) {
		io_ctx->cry_iov.iov_len = total_length;
		/* For now just allocate in the I/O path, not optimal but the current bdev API
		 * for getting a buffer from the pool won't work if the bdev_io passed in
		 * has a buffer, which ours always will.  So, until we modify that API
		 * or better yet the current ZCOPY work lands, this is the best we can do.
		 */
		io_ctx->cry_iov.iov_base = spdk_dma_malloc(total_length, 0x10, NULL);
		if (!io_ctx->cry_iov.iov_base) {
			SPDK_ERRLOG("ERROR trying to allocate write buffer for encryption!\n");
			rc = -ENOMEM;
			goto error_get_write_buffer;
		}
		io_ctx->cry_offset_blocks = bdev_io->u.bdev.offset_blocks;
		io_ctx->cry_num_blocks = bdev_io->u.bdev.num_blocks;
	}

	/* This value is used in the completion callback to determine when the bdev_io is
	 * complete.
	 */
	io_ctx->cryop_cnt_remaining = cryop_cnt;

	/* As we don't support chaining because of a decision to use LBA as IV, construction
	 * of crypto operaations is straightforward. We build both the op, the mbuf and the
	 * dst_mbuf in our local arrays by looping through the length of the bdev IO and
	 * picking off LBA sized blocks of memory from the IOVs as we walk through them. Each
	 * LBA sized chunck of memory will correspond 1:1 to a crypto operation and a single
	 * mbuf per crypto operation.
	 */
	total_remaining = total_length;
	current_iov = bdev_io->u.bdev.iovs[iov_index].iov_base;
	current_iov_remaining = bdev_io->u.bdev.iovs[iov_index].iov_len;
	do {
		uint8_t *iv_ptr;
		uint64_t op_block_offset;

		/* Set the mbuf elements address and length. Null out the next pointer. */
		src_mbufs[crypto_index]->buf_addr = current_iov;
		src_mbufs[crypto_index]->buf_iova = spdk_vtophys((void *)current_iov);
		src_mbufs[crypto_index]->data_len = crypto_len;
		src_mbufs[crypto_index]->next = NULL;
		/* Store context in every mbuf as we don't know anything about completion order */
		src_mbufs[crypto_index]->userdata = bdev_io;

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
		if (crypto_op == RTE_CRYPTO_CIPHER_OP_ENCRYPT) {
			crypto_ops[crypto_index]->sym->m_dst = src_mbufs[crypto_index];
		} else {
			crypto_ops[crypto_index]->sym->m_dst = NULL;
		}

		/* For encrypt, point the destination to a buffer we allocate and redirect the bdev_io
		 * that will be used to process the write on completion to the same buffer. Setting
		 * up the en_buffer is a little simpler as we know the destination buffer is single IOV.
		 */
		if (crypto_op == RTE_CRYPTO_CIPHER_OP_ENCRYPT) {

			/* Set the relevant destination en_mbuf elements. */
			dst_mbufs[crypto_index]->buf_addr = io_ctx->cry_iov.iov_base + en_offset;
			dst_mbufs[crypto_index]->buf_iova = spdk_vtophys(dst_mbufs[crypto_index]->buf_addr);
			dst_mbufs[crypto_index]->data_len = crypto_len;
			crypto_ops[crypto_index]->sym->m_dst = dst_mbufs[crypto_index];
			en_offset += crypto_len;
			dst_mbufs[crypto_index]->next = NULL;
		}

		/* Attach the crypto session to the operation */
		rc = rte_crypto_op_attach_sym_session(crypto_ops[crypto_index], session);
		if (rc) {
			rc = -EINVAL;
			goto error_attach_session;
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
	do {
		burst = spdk_min((cryop_cnt - enqueued), CRYPTO_QP_DESCRIPTORS);
		num_enqueued_ops = rte_cryptodev_enqueue_burst(cdev_id, crypto_ch->device_qp->qp,
				   &crypto_ops[enqueued],
				   burst);
		enqueued += num_enqueued_ops;

		/* Dequeue all inline if the device is full. We don't defer anything simply
		 * because of the complexity involved as we're building 1 or more crypto
		 * ops per IO. Dequeue will free up space for more enqueue.
		 */
		if (enqueued < cryop_cnt) {

			/* Dequeue everything, this may include ops that were already
			 * in the device before this submission....
			 */
			do {
				completed = crypto_dev_poller(crypto_ch);
			} while (completed > 0);
		}
	} while (enqueued < cryop_cnt);

	return rc;

	/* Error cleanup paths. */
error_attach_session:
error_get_write_buffer:
error_session_init:
	rte_cryptodev_sym_session_clear(cdev_id, session);
	rte_cryptodev_sym_session_free(session);
error_session_create:
	rte_mempool_put_bulk(g_crypto_op_mp, (void **)crypto_ops, cryop_cnt);
	allocated = 0;
error_get_ops:
	if (crypto_op == RTE_CRYPTO_CIPHER_OP_ENCRYPT) {
		spdk_mempool_put_bulk(g_mbuf_mp, (void **)&dst_mbufs[0],
				      cryop_cnt);
	}
	if (allocated > 0) {
		rte_mempool_put_bulk(g_crypto_op_mp, (void **)crypto_ops,
				     allocated);
	}
error_get_dst:
	spdk_mempool_put_bulk(g_mbuf_mp, (void **)&src_mbufs[0],
			      cryop_cnt);
	return rc;
}

/* Completion callback for IO that were issued from this bdev other than read/write.
 * They have their own for readability.
 */
static void
_complete_internal_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;

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

	spdk_dma_free(orig_ctx->cry_iov.iov_base);
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

		if (_crypto_operation(orig_io, RTE_CRYPTO_CIPHER_OP_DECRYPT)) {
			SPDK_ERRLOG("ERROR decrypting");
			spdk_bdev_io_complete(orig_io, SPDK_BDEV_IO_STATUS_FAILED);
			spdk_bdev_free_io(bdev_io);
		}
	} else {
		SPDK_ERRLOG("ERROR on read prior to decrypting");
		spdk_bdev_io_complete(orig_io, SPDK_BDEV_IO_STATUS_FAILED);
		spdk_bdev_free_io(bdev_io);
	}
}

/* Callback for getting a buf from the bdev pool in the event that the caller passed
 * in NULL, we need to own the buffer so it doesn't get freed by another vbdev module
 * beneath us before we're done with it.
 */
static void
crypto_read_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_crypto *crypto_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_crypto,
					   crypto_bdev);
	struct crypto_io_channel *crypto_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	rc = spdk_bdev_readv_blocks(crypto_bdev->base_desc, crypto_ch->base_ch, bdev_io->u.bdev.iovs,
				    bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks,
				    bdev_io->u.bdev.num_blocks, _complete_internal_read,
				    bdev_io);
	if (rc != 0) {
		SPDK_ERRLOG("ERROR on bdev_io submission!\n");
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
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

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, crypto_read_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		rc = _crypto_operation(bdev_io, RTE_CRYPTO_CIPHER_OP_ENCRYPT);
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
		SPDK_ERRLOG("ERROR on bdev_io submission!\n");
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
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

/* Called after we've unregistered following a hot remove callback.
 * Our finish entry point will be called next.
 */
static int
vbdev_crypto_destruct(void *ctx)
{
	struct vbdev_crypto *crypto_bdev = (struct vbdev_crypto *)ctx;

	/* Unclaim the underlying bdev. */
	spdk_bdev_module_release_bdev(crypto_bdev->base_bdev);

	/* Close the underlying bdev. */
	spdk_bdev_close(crypto_bdev->base_desc);

	/* Done with this crypto_bdev. */
	TAILQ_REMOVE(&g_vbdev_crypto, crypto_bdev, link);
	free(crypto_bdev->drv_name);
	free(crypto_bdev->key);
	free(crypto_bdev->crypto_bdev.name);
	free(crypto_bdev);
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
	 * the SPDK cahnnel structure plus the size of our crypto_io_channel struct
	 * that we passed in when we registered our IO device. It will then call
	 * our channel create callback to populate any elements that we need to
	 * update.
	 */
	return spdk_get_io_channel(crypto_bdev);
}

/* This is the output for get_bdevs() for this vbdev */
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
	spdk_json_write_object_end(w);
	return 0;
}

static int
vbdev_crypto_config_json(struct spdk_json_write_ctx *w)
{
	struct vbdev_crypto *crypto_bdev, *tmp;

	TAILQ_FOREACH_SAFE(crypto_bdev, &g_vbdev_crypto, link, tmp) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "construct_crypto_bdev");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(crypto_bdev->base_bdev));
		spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&crypto_bdev->crypto_bdev));
		spdk_json_write_named_string(w, "crypto_pmd", crypto_bdev->drv_name);
		spdk_json_write_named_string(w, "key", crypto_bdev->key);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
	return 0;
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
	struct device_qp *device_qp;

	crypto_ch->base_ch = spdk_bdev_get_io_channel(crypto_bdev->base_desc);
	crypto_ch->poller = spdk_poller_register(crypto_dev_poller, crypto_ch, 0);
	crypto_ch->device_qp = NULL;

	pthread_mutex_lock(&g_device_qp_lock);
	TAILQ_FOREACH(device_qp, &g_device_qp, link) {
		if ((strcmp(device_qp->device->cdev_info.driver_name, crypto_bdev->drv_name) == 0) &&
		    (device_qp->in_use == false)) {
			crypto_ch->device_qp = device_qp;
			device_qp->in_use = true;
			SPDK_NOTICELOG("Device queue pair assignment: ch %p device %p qpid %u %s\n",
				       crypto_ch, device_qp->device, crypto_ch->device_qp->qp, crypto_bdev->drv_name);
			break;
		}
	}
	pthread_mutex_unlock(&g_device_qp_lock);
	assert(crypto_ch->device_qp);
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
			 const char *crypto_pmd, const char *key)
{
	struct bdev_names *name;
	int rc, j;
	bool found = false;

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
	if (strlen(name->key) != AES_CBC_KEY_LENGTH) {
		SPDK_ERRLOG("invalid AES_CCB key length\n");
		rc = -EINVAL;
		goto error_invalid_key;
	}

	TAILQ_INSERT_TAIL(&g_bdev_names, name, link);

	return 0;

	/* Error cleanup paths. */
error_invalid_key:
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
		   const char *crypto_pmd, const char *key)
{
	struct spdk_bdev *bdev = NULL;
	struct vbdev_crypto *crypto_bdev, *tmp;
	int rc = 0;

	bdev = spdk_bdev_get_by_name(bdev_name);

	rc = vbdev_crypto_insert_name(bdev_name, vbdev_name, crypto_pmd, key);
	if (rc) {
		return rc;
	}

	if (!bdev) {
		return 0;
	}

	rc = vbdev_crypto_claim(bdev);
	if (rc) {
		return rc;
	}

	rc = vbdev_crypto_init_crypto_drivers();
	if (rc) {
		return rc;
	}

	TAILQ_FOREACH_SAFE(crypto_bdev, &g_vbdev_crypto, link, tmp) {
		if (strcmp(crypto_bdev->base_bdev->name, bdev->name) == 0) {
			rc = spdk_vbdev_register(&crypto_bdev->crypto_bdev,
						 &crypto_bdev->base_bdev, 1);
			if (rc) {
				SPDK_ERRLOG("could not register crypto_bdev\n");
				spdk_bdev_close(crypto_bdev->base_desc);
				TAILQ_REMOVE(&g_vbdev_crypto, crypto_bdev, link);
				free(crypto_bdev->crypto_bdev.name);
				free(crypto_bdev->key);
				free(crypto_bdev);
			}
			break;
		}
	}

	return rc;
}

/* Called at driver init time, parses config file to preapre for examine calls,
 * also fully initializes the crypto drivers.
 */
static int
vbdev_crypto_init(void)
{
	struct spdk_conf_section *sp = NULL;
	const char *conf_bdev_name = NULL;
	const char *conf_vbdev_name = NULL;
	const char *crypto_pmd = NULL;
	int i;
	int rc = 0;
	const char *key = NULL;

	sp = spdk_conf_find_section(NULL, "crypto");
	if (sp == NULL) {
		return 0;
	}

	for (i = 0; ; i++) {

		if (!spdk_conf_section_get_nval(sp, "CRY", i)) {
			break;
		}

		conf_bdev_name = spdk_conf_section_get_nmval(sp, "CRY", i, 0);
		if (!conf_bdev_name) {
			SPDK_ERRLOG("crypto configuration missing bdev name\n");
			return -EINVAL;
		}

		conf_vbdev_name = spdk_conf_section_get_nmval(sp, "CRY", i, 1);
		if (!conf_vbdev_name) {
			SPDK_ERRLOG("crypto configuration missing crypto_bdev name\n");
			return -EINVAL;
		}

		key = spdk_conf_section_get_nmval(sp, "CRY", i, 2);
		if (!key) {
			SPDK_ERRLOG("crypto configuration missing crypto_bdev key\n");
			return -EINVAL;
		}
		SPDK_NOTICELOG("WARNING: You are storing your key in a plain text file!!\n");

		crypto_pmd = spdk_conf_section_get_nmval(sp, "CRY", i, 3);
		if (!crypto_pmd) {
			SPDK_ERRLOG("crypto configuration missing driver type\n");
			return -EINVAL;
		}

		rc = vbdev_crypto_insert_name(conf_bdev_name, conf_vbdev_name,
					      crypto_pmd, key);
		if (rc != 0) {
			return rc;
		}
	}

	/* Fully configure both SW and HW drivers. */
	rc = vbdev_crypto_init_crypto_drivers();
	if (rc) {
		SPDK_ERRLOG("Error setting up crypto devices\n");
		return rc;
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

	while ((name = TAILQ_FIRST(&g_bdev_names))) {
		TAILQ_REMOVE(&g_bdev_names, name, link);
		free(name->drv_name);
		free(name->key);
		free(name->bdev_name);
		free(name->vbdev_name);
		free(name);
	}

	while ((device = TAILQ_FIRST(&g_vbdev_devs))) {
		TAILQ_REMOVE(&g_vbdev_devs, device, link);
		rte_cryptodev_stop(device->cdev_id);
		free(device);
	}

	while ((dev_qp = TAILQ_FIRST(&g_device_qp))) {
		TAILQ_REMOVE(&g_device_qp, dev_qp, link);
		free(dev_qp);
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

/* Called when SPDK wants to save the current config of this vbdev module to
 * a file.
 */
static void
vbdev_crypto_get_spdk_running_config(FILE *fp)
{
	struct bdev_names *names = NULL;
	fprintf(fp, "\n[crypto]\n");
	TAILQ_FOREACH(names, &g_bdev_names, link) {
		fprintf(fp, "  crypto %s %s ", names->bdev_name, names->vbdev_name);
		fprintf(fp, "\n");
	}

	fprintf(fp, "\n");
}

/* Called when the underlying base bdev goes away. */
static void
vbdev_crypto_examine_hotremove_cb(void *ctx)
{
	struct vbdev_crypto *crypto_bdev, *tmp;
	struct spdk_bdev *bdev_find = ctx;

	TAILQ_FOREACH_SAFE(crypto_bdev, &g_vbdev_crypto, link, tmp) {
		if (bdev_find == crypto_bdev->base_bdev) {
			spdk_bdev_unregister(&crypto_bdev->crypto_bdev, NULL, NULL);
		}
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
	.config_text = vbdev_crypto_get_spdk_running_config,
	.get_ctx_size = vbdev_crypto_get_ctx_size,
	.examine_config = vbdev_crypto_examine,
	.module_fini = vbdev_crypto_finish,
	.config_json = vbdev_crypto_config_json
};

SPDK_BDEV_MODULE_REGISTER(&crypto_if)

static int
vbdev_crypto_claim(struct spdk_bdev *bdev)
{
	struct bdev_names *name;
	struct vbdev_crypto *vbdev;
	int rc = 0;

	/* Check our list of names from config versus this bdev and if
	 * there's a match, create the crypto_bdev & bdev accordingly.
	 */
	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(name->bdev_name, bdev->name) != 0) {
			continue;
		}

		SPDK_NOTICELOG("Match on %s\n", bdev->name);
		vbdev = calloc(1, sizeof(struct vbdev_crypto));
		if (!vbdev) {
			SPDK_ERRLOG("could not allocate crypto_bdev\n");
			rc = -ENOMEM;
			goto error_vbdev_alloc;
		}

		/* The base bdev that we're attaching to. */
		vbdev->base_bdev = bdev;
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

		vbdev->drv_name = strdup(name->drv_name);
		if (!vbdev->drv_name) {
			SPDK_ERRLOG("could not allocate crypto_bdev drv_name\n");
			rc = -ENOMEM;
			goto error_drv_name;
		}

		vbdev->crypto_bdev.product_name = "crypto";
		vbdev->crypto_bdev.write_cache = bdev->write_cache;
		vbdev->crypto_bdev.need_aligned_buffer = bdev->need_aligned_buffer;
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

		rc = spdk_bdev_open(bdev, true, vbdev_crypto_examine_hotremove_cb,
				    bdev, &vbdev->base_desc);
		if (rc) {
			SPDK_ERRLOG("could not open bdev %s\n", spdk_bdev_get_name(bdev));
			goto error_open;
		}

		rc = spdk_bdev_module_claim_bdev(bdev, vbdev->base_desc, vbdev->crypto_bdev.module);
		if (rc) {
			SPDK_ERRLOG("could not claim bdev %s\n", spdk_bdev_get_name(bdev));
			goto error_claim;
		}

		SPDK_NOTICELOG("registered crypto_bdev for: %s\n", name->vbdev_name);
	}

	return rc;

	/* Error cleanup paths. */
error_claim:
	spdk_bdev_close(vbdev->base_desc);
error_open:
	TAILQ_REMOVE(&g_vbdev_crypto, vbdev, link);
	spdk_io_device_unregister(vbdev, NULL);
	free(vbdev->drv_name);
error_drv_name:
	free(vbdev->key);
error_alloc_key:
	free(vbdev->crypto_bdev.name);
error_bdev_name:
	free(vbdev);
error_vbdev_alloc:
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
			free(name);
			break;
		}
	}

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
	struct vbdev_crypto *crypto_bdev, *tmp;
	int rc;

	vbdev_crypto_claim(bdev);

	TAILQ_FOREACH_SAFE(crypto_bdev, &g_vbdev_crypto, link, tmp) {
		if (strcmp(crypto_bdev->base_bdev->name, bdev->name) == 0) {
			rc = spdk_vbdev_register(&crypto_bdev->crypto_bdev,
						 &crypto_bdev->base_bdev, 1);
			if (rc) {
				SPDK_ERRLOG("could not register crypto_bdev\n");
				spdk_bdev_close(crypto_bdev->base_desc);
				TAILQ_REMOVE(&g_vbdev_crypto, crypto_bdev, link);
				free(crypto_bdev->crypto_bdev.name);
				free(crypto_bdev->key);
				free(crypto_bdev);
			}
			break;
		}
	}

	spdk_bdev_module_examine_done(&crypto_if);
}

SPDK_LOG_REGISTER_COMPONENT("vbdev_crypto", SPDK_LOG_VBDEV_crypto)
