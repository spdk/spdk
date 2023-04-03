/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   Copyright (c) 2022, 2023 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "accel_dpdk_cryptodev.h"

#include "spdk/accel.h"
#include "spdk_internal/accel_module.h"
#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/thread.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/json.h"
#include "spdk_internal/sgl.h"

#include <rte_bus_vdev.h>
#include <rte_crypto.h>
#include <rte_cryptodev.h>
#include <rte_mbuf_dyn.h>
#include <rte_version.h>

/* The VF spread is the number of queue pairs between virtual functions, we use this to
 * load balance the QAT device.
 */
#define ACCEL_DPDK_CRYPTODEV_QAT_VF_SPREAD		32

/* This controls how many ops will be dequeued from the crypto driver in one run
 * of the poller. It is mainly a performance knob as it effectively determines how
 * much work the poller has to do.  However even that can vary between crypto drivers
 * as the ACCEL_DPDK_CRYPTODEV_AESNI_MB driver for example does all the crypto work on dequeue whereas the
 * QAT driver just dequeues what has been completed already.
 */
#define ACCEL_DPDK_CRYPTODEV_MAX_DEQUEUE_BURST_SIZE	64

#define ACCEL_DPDK_CRYPTODEV_MAX_ENQUEUE_ARRAY_SIZE (128)

/* The number of MBUFS we need must be a power of two and to support other small IOs
 * in addition to the limits mentioned above, we go to the next power of two. It is
 * big number because it is one mempool for source and destination mbufs. It may
 * need to be bigger to support multiple crypto drivers at once.
 */
#define ACCEL_DPDK_CRYPTODEV_NUM_MBUFS			32768
#define ACCEL_DPDK_CRYPTODEV_POOL_CACHE_SIZE		256
#define ACCEL_DPDK_CRYPTODEV_MAX_CRYPTO_VOLUMES		128
#define ACCEL_DPDK_CRYPTODEV_NUM_SESSIONS		(2 * ACCEL_DPDK_CRYPTODEV_MAX_CRYPTO_VOLUMES)
#define ACCEL_DPDK_CRYPTODEV_SESS_MEMPOOL_CACHE_SIZE	0

/* This is the max number of IOs we can supply to any crypto device QP at one time.
 * It can vary between drivers.
 */
#define ACCEL_DPDK_CRYPTODEV_QP_DESCRIPTORS		2048

/* At this moment DPDK descriptors allocation for mlx5 has some issues. We use 512
 * as a compromise value between performance and the time spent for initialization. */
#define ACCEL_DPDK_CRYPTODEV_QP_DESCRIPTORS_MLX5	512

#define ACCEL_DPDK_CRYPTODEV_AESNI_MB_NUM_QP		64

/* Common for suported devices. */
#define ACCEL_DPDK_CRYPTODEV_DEFAULT_NUM_XFORMS		2
#define ACCEL_DPDK_CRYPTODEV_IV_OFFSET (sizeof(struct rte_crypto_op) + \
                sizeof(struct rte_crypto_sym_op) + \
                (ACCEL_DPDK_CRYPTODEV_DEFAULT_NUM_XFORMS * \
                 sizeof(struct rte_crypto_sym_xform)))
#define ACCEL_DPDK_CRYPTODEV_IV_LENGTH			16
#define ACCEL_DPDK_CRYPTODEV_QUEUED_OP_OFFSET (ACCEL_DPDK_CRYPTODEV_IV_OFFSET + ACCEL_DPDK_CRYPTODEV_IV_LENGTH)

/* Driver names */
#define ACCEL_DPDK_CRYPTODEV_AESNI_MB	"crypto_aesni_mb"
#define ACCEL_DPDK_CRYPTODEV_QAT	"crypto_qat"
#define ACCEL_DPDK_CRYPTODEV_QAT_ASYM	"crypto_qat_asym"
#define ACCEL_DPDK_CRYPTODEV_MLX5	"mlx5_pci"

/* Supported ciphers */
#define ACCEL_DPDK_CRYPTODEV_AES_CBC	"AES_CBC" /* QAT and ACCEL_DPDK_CRYPTODEV_AESNI_MB */
#define ACCEL_DPDK_CRYPTODEV_AES_XTS	"AES_XTS" /* QAT and MLX5 */

/* Specific to AES_CBC. */
#define ACCEL_DPDK_CRYPTODEV_AES_CBC_KEY_LENGTH			16
#define ACCEL_DPDK_CRYPTODEV_AES_XTS_128_BLOCK_KEY_LENGTH	16 /* AES-XTS-128 block key size. */
#define ACCEL_DPDK_CRYPTODEV_AES_XTS_256_BLOCK_KEY_LENGTH	32 /* AES-XTS-256 block key size. */
#define ACCEL_DPDK_CRYPTODEV_AES_XTS_512_BLOCK_KEY_LENGTH	64 /* AES-XTS-512 block key size. */

#define ACCEL_DPDK_CRYPTODEV_AES_XTS_TWEAK_KEY_LENGTH		16 /* XTS part key size is always 128 bit. */

/* Limit of the max memory len attached to mbuf - rte_pktmbuf_attach_extbuf has uint16_t `buf_len`
 * parameter, we use closes aligned value 32768 for better performance */
#define ACCEL_DPDK_CRYPTODEV_MAX_MBUF_LEN			32768

/* Used to store IO context in mbuf */
static const struct rte_mbuf_dynfield rte_mbuf_dynfield_io_context = {
	.name = "context_accel_dpdk_cryptodev",
	.size = sizeof(uint64_t),
	.align = __alignof__(uint64_t),
	.flags = 0,
};

struct accel_dpdk_cryptodev_device;

enum accel_dpdk_cryptodev_driver_type {
	ACCEL_DPDK_CRYPTODEV_DRIVER_AESNI_MB = 0,
	ACCEL_DPDK_CRYPTODEV_DRIVER_QAT,
	ACCEL_DPDK_CRYPTODEV_DRIVER_MLX5_PCI,
	ACCEL_DPDK_CRYPTODEV_DRIVER_LAST
};

enum accel_dpdk_crypto_dev_cipher_type {
	ACCEL_DPDK_CRYPTODEV_CIPHER_AES_CBC,
	ACCEL_DPDK_CRYPTODEV_CIPHER_AES_XTS
};

struct accel_dpdk_cryptodev_qp {
	struct accel_dpdk_cryptodev_device *device;	/* ptr to crypto device */
	uint32_t num_enqueued_ops;	/* Used to decide whether to poll the qp or not */
	uint8_t qp; /* queue identifier */
	bool in_use; /* whether this node is in use or not */
	uint8_t index; /* used by QAT to load balance placement of qpairs */
	TAILQ_ENTRY(accel_dpdk_cryptodev_qp) link;
};

struct accel_dpdk_cryptodev_device {
	enum accel_dpdk_cryptodev_driver_type type;
	struct rte_cryptodev_info cdev_info; /* includes DPDK device friendly name */
	uint32_t qp_desc_nr; /* max number of qp descriptors to be enqueued in burst */
	uint8_t cdev_id; /* identifier for the device */
	TAILQ_HEAD(, accel_dpdk_cryptodev_qp) qpairs;
	TAILQ_ENTRY(accel_dpdk_cryptodev_device) link;
};

struct accel_dpdk_cryptodev_key_handle {
	struct accel_dpdk_cryptodev_device *device;
	TAILQ_ENTRY(accel_dpdk_cryptodev_key_handle) link;
	void *session_encrypt;	/* encryption session for this key */
	void *session_decrypt;	/* decryption session for this key */
	struct rte_crypto_sym_xform cipher_xform;		/* crypto control struct for this key */
};

struct accel_dpdk_cryptodev_key_priv {
	enum accel_dpdk_cryptodev_driver_type driver;
	enum accel_dpdk_crypto_dev_cipher_type cipher;
	char *xts_key;
	TAILQ_HEAD(, accel_dpdk_cryptodev_key_handle) dev_keys;
};

/* For queueing up crypto operations that we can't submit for some reason */
struct accel_dpdk_cryptodev_queued_op {
	struct accel_dpdk_cryptodev_qp *qp;
	struct rte_crypto_op *crypto_op;
	struct accel_dpdk_cryptodev_task *task;
	TAILQ_ENTRY(accel_dpdk_cryptodev_queued_op) link;
};
#define ACCEL_DPDK_CRYPTODEV_QUEUED_OP_LENGTH (sizeof(struct accel_dpdk_cryptodev_queued_op))

/* The crypto channel struct. It is allocated and freed on my behalf by the io channel code.
 * We store things in here that are needed on per thread basis like the base_channel for this thread,
 * and the poller for this thread.
 */
struct accel_dpdk_cryptodev_io_channel {
	/* completion poller */
	struct spdk_poller *poller;
	/* Array of qpairs for each available device. The specific device will be selected depending on the crypto key */
	struct accel_dpdk_cryptodev_qp *device_qp[ACCEL_DPDK_CRYPTODEV_DRIVER_LAST];
	/* queued for re-submission to CryptoDev. Used when for some reason crypto op was not processed by the driver */
	TAILQ_HEAD(, accel_dpdk_cryptodev_queued_op) queued_cry_ops;
	/* Used to queue tasks when qpair is full. No crypto operation was submitted to the driver by the task */
	TAILQ_HEAD(, accel_dpdk_cryptodev_task) queued_tasks;
};

struct accel_dpdk_cryptodev_task {
	struct spdk_accel_task base;
	uint32_t cryop_completed;	/* The number of crypto operations completed by HW */
	uint32_t cryop_submitted;	/* The number of crypto operations submitted to HW */
	uint32_t cryop_total;		/* Total number of crypto operations in this task */
	bool is_failed;
	bool inplace;
	TAILQ_ENTRY(accel_dpdk_cryptodev_task) link;
};

/* Shared mempools between all devices on this system */
static struct rte_mempool *g_session_mp = NULL;
static struct rte_mempool *g_session_mp_priv = NULL;
static struct rte_mempool *g_mbuf_mp = NULL;            /* mbuf mempool */
static int g_mbuf_offset;
static struct rte_mempool *g_crypto_op_mp = NULL;	/* crypto operations, must be rte* mempool */

static struct rte_mbuf_ext_shared_info g_shinfo = {};   /* used by DPDK mbuf macro */

static uint8_t g_qat_total_qp = 0;
static uint8_t g_next_qat_index;

static const char *g_driver_names[] = {
	[ACCEL_DPDK_CRYPTODEV_DRIVER_AESNI_MB]	= ACCEL_DPDK_CRYPTODEV_AESNI_MB,
	[ACCEL_DPDK_CRYPTODEV_DRIVER_QAT]	= ACCEL_DPDK_CRYPTODEV_QAT,
	[ACCEL_DPDK_CRYPTODEV_DRIVER_MLX5_PCI]	= ACCEL_DPDK_CRYPTODEV_MLX5
};
static const char *g_cipher_names[] = {
	[ACCEL_DPDK_CRYPTODEV_CIPHER_AES_CBC]	= ACCEL_DPDK_CRYPTODEV_AES_CBC,
	[ACCEL_DPDK_CRYPTODEV_CIPHER_AES_XTS]	= ACCEL_DPDK_CRYPTODEV_AES_XTS,
};

static enum accel_dpdk_cryptodev_driver_type g_dpdk_cryptodev_driver =
	ACCEL_DPDK_CRYPTODEV_DRIVER_AESNI_MB;

/* Global list of all crypto devices */
static TAILQ_HEAD(, accel_dpdk_cryptodev_device) g_crypto_devices = TAILQ_HEAD_INITIALIZER(
			g_crypto_devices);
static pthread_mutex_t g_device_lock = PTHREAD_MUTEX_INITIALIZER;

static struct spdk_accel_module_if g_accel_dpdk_cryptodev_module;

static int accel_dpdk_cryptodev_process_task(struct accel_dpdk_cryptodev_io_channel *crypto_ch,
		struct accel_dpdk_cryptodev_task *task);

void
accel_dpdk_cryptodev_enable(void)
{
	spdk_accel_module_list_add(&g_accel_dpdk_cryptodev_module);
}

int
accel_dpdk_cryptodev_set_driver(const char *driver_name)
{
	if (strcmp(driver_name, ACCEL_DPDK_CRYPTODEV_QAT) == 0) {
		g_dpdk_cryptodev_driver = ACCEL_DPDK_CRYPTODEV_DRIVER_QAT;
	} else if (strcmp(driver_name, ACCEL_DPDK_CRYPTODEV_AESNI_MB) == 0) {
		g_dpdk_cryptodev_driver = ACCEL_DPDK_CRYPTODEV_DRIVER_AESNI_MB;
	} else if (strcmp(driver_name, ACCEL_DPDK_CRYPTODEV_MLX5) == 0) {
		g_dpdk_cryptodev_driver = ACCEL_DPDK_CRYPTODEV_DRIVER_MLX5_PCI;
	} else {
		SPDK_ERRLOG("Unsupported driver %s\n", driver_name);
		return -EINVAL;
	}

	SPDK_NOTICELOG("Using driver %s\n", driver_name);

	return 0;
}

const char *
accel_dpdk_cryptodev_get_driver(void)
{
	return g_driver_names[g_dpdk_cryptodev_driver];
}

static void
cancel_queued_crypto_ops(struct accel_dpdk_cryptodev_io_channel *crypto_ch,
			 struct accel_dpdk_cryptodev_task *task)
{
	struct rte_mbuf *mbufs_to_free[2 * ACCEL_DPDK_CRYPTODEV_MAX_DEQUEUE_BURST_SIZE];
	struct rte_crypto_op *cancelled_ops[ACCEL_DPDK_CRYPTODEV_MAX_DEQUEUE_BURST_SIZE];
	struct accel_dpdk_cryptodev_queued_op *op_to_cancel, *tmp_op;
	struct rte_crypto_op *crypto_op;
	int num_mbufs = 0, num_dequeued_ops = 0;

	/* Remove all ops from the failed IO. Since we don't know the
	 * order we have to check them all. */
	TAILQ_FOREACH_SAFE(op_to_cancel, &crypto_ch->queued_cry_ops, link, tmp_op) {
		/* Checking if this is our op. One IO contains multiple ops. */
		if (task == op_to_cancel->task) {
			crypto_op = op_to_cancel->crypto_op;
			TAILQ_REMOVE(&crypto_ch->queued_cry_ops, op_to_cancel, link);

			/* Populating lists for freeing mbufs and ops. */
			mbufs_to_free[num_mbufs++] = (void *)crypto_op->sym->m_src;
			if (crypto_op->sym->m_dst) {
				mbufs_to_free[num_mbufs++] = (void *)crypto_op->sym->m_dst;
			}
			cancelled_ops[num_dequeued_ops++] = crypto_op;
		}
	}

	/* Now bulk free both mbufs and crypto operations. */
	if (num_dequeued_ops > 0) {
		rte_mempool_put_bulk(g_crypto_op_mp, (void **)cancelled_ops,
				     num_dequeued_ops);
		assert(num_mbufs > 0);
		/* This also releases chained mbufs if any. */
		rte_pktmbuf_free_bulk(mbufs_to_free, num_mbufs);
	}
}

static inline uint16_t
accel_dpdk_cryptodev_poll_qp(struct accel_dpdk_cryptodev_qp *qp,
			     struct accel_dpdk_cryptodev_io_channel *crypto_ch)
{
	struct rte_crypto_op *dequeued_ops[ACCEL_DPDK_CRYPTODEV_MAX_DEQUEUE_BURST_SIZE];
	struct rte_mbuf *mbufs_to_free[2 * ACCEL_DPDK_CRYPTODEV_MAX_DEQUEUE_BURST_SIZE];
	struct accel_dpdk_cryptodev_task *task;
	uint32_t num_mbufs = 0;
	int i;
	uint16_t num_dequeued_ops;

	/* Each run of the poller will get just what the device has available
	 * at the moment we call it, we don't check again after draining the
	 * first batch.
	 */
	num_dequeued_ops = rte_cryptodev_dequeue_burst(qp->device->cdev_id, qp->qp,
			   dequeued_ops, ACCEL_DPDK_CRYPTODEV_MAX_DEQUEUE_BURST_SIZE);
	/* Check if operation was processed successfully */
	for (i = 0; i < num_dequeued_ops; i++) {

		/* We don't know the order or association of the crypto ops wrt any
		 * particular task so need to look at each and determine if it's
		 * the last one for it's task or not.
		 */
		task = (struct accel_dpdk_cryptodev_task *)*RTE_MBUF_DYNFIELD(dequeued_ops[i]->sym->m_src,
				g_mbuf_offset, uint64_t *);
		assert(task != NULL);

		if (dequeued_ops[i]->status != RTE_CRYPTO_OP_STATUS_SUCCESS) {
			SPDK_ERRLOG("error with op %d status %u\n", i, dequeued_ops[i]->status);
			/* Update the task status to error, we'll still process the
			 * rest of the crypto ops for this task though so they
			 * aren't left hanging.
			 */
			task->is_failed = true;
		}

		/* Return the associated src and dst mbufs by collecting them into
		 * an array that we can use the bulk API to free after the loop.
		 */
		*RTE_MBUF_DYNFIELD(dequeued_ops[i]->sym->m_src, g_mbuf_offset, uint64_t *) = 0;
		mbufs_to_free[num_mbufs++] = (void *)dequeued_ops[i]->sym->m_src;
		if (dequeued_ops[i]->sym->m_dst) {
			mbufs_to_free[num_mbufs++] = (void *)dequeued_ops[i]->sym->m_dst;
		}

		task->cryop_completed++;
		if (task->cryop_completed == task->cryop_total) {
			/* Complete the IO */
			spdk_accel_task_complete(&task->base, task->is_failed ? -EINVAL : 0);
		} else if (task->cryop_completed == task->cryop_submitted) {
			/* submit remaining crypto ops */
			int rc = accel_dpdk_cryptodev_process_task(crypto_ch, task);

			if (spdk_unlikely(rc)) {
				if (rc == -ENOMEM) {
					TAILQ_INSERT_TAIL(&crypto_ch->queued_tasks, task, link);
					continue;
				}
				spdk_accel_task_complete(&task->base, rc);
			}
		}
	}

	/* Now bulk free both mbufs and crypto operations. */
	if (num_dequeued_ops > 0) {
		rte_mempool_put_bulk(g_crypto_op_mp, (void **)dequeued_ops, num_dequeued_ops);
		assert(num_mbufs > 0);
		/* This also releases chained mbufs if any. */
		rte_pktmbuf_free_bulk(mbufs_to_free, num_mbufs);
	}

	assert(qp->num_enqueued_ops >= num_dequeued_ops);
	qp->num_enqueued_ops -= num_dequeued_ops;

	return num_dequeued_ops;
}

/* This is the poller for the crypto module. It uses a single API to dequeue whatever is ready at
 * the device. Then we need to decide if what we've got so far (including previous poller
 * runs) totals up to one or more complete task */
static int
accel_dpdk_cryptodev_poller(void *args)
{
	struct accel_dpdk_cryptodev_io_channel *crypto_ch = args;
	struct accel_dpdk_cryptodev_qp *qp;
	struct accel_dpdk_cryptodev_task *task, *task_tmp;
	struct accel_dpdk_cryptodev_queued_op *op_to_resubmit, *op_to_resubmit_tmp;
	TAILQ_HEAD(, accel_dpdk_cryptodev_task) queued_tasks_tmp;
	uint32_t num_dequeued_ops = 0, num_enqueued_ops = 0;
	uint16_t enqueued;
	int i, rc;

	for (i = 0; i < ACCEL_DPDK_CRYPTODEV_DRIVER_LAST; i++) {
		qp = crypto_ch->device_qp[i];
		/* Avoid polling "idle" qps since it may affect performance */
		if (qp && qp->num_enqueued_ops) {
			num_dequeued_ops += accel_dpdk_cryptodev_poll_qp(qp, crypto_ch);
		}
	}

	/* Check if there are any queued crypto ops to process */
	TAILQ_FOREACH_SAFE(op_to_resubmit, &crypto_ch->queued_cry_ops, link, op_to_resubmit_tmp) {
		task = op_to_resubmit->task;
		qp = op_to_resubmit->qp;
		if (qp->num_enqueued_ops == qp->device->qp_desc_nr) {
			continue;
		}
		enqueued = rte_cryptodev_enqueue_burst(qp->device->cdev_id,
						       qp->qp,
						       &op_to_resubmit->crypto_op,
						       1);
		if (enqueued == 1) {
			TAILQ_REMOVE(&crypto_ch->queued_cry_ops, op_to_resubmit, link);
			qp->num_enqueued_ops++;
			num_enqueued_ops++;
		} else {
			if (op_to_resubmit->crypto_op->status == RTE_CRYPTO_OP_STATUS_NOT_PROCESSED) {
				/* If we couldn't get one, just break and try again later. */
				break;
			} else {
				/* Something is really wrong with the op. Most probably the
				 * mbuf is broken or the HW is not able to process the request.
				 * Fail the IO and remove its ops from the queued ops list. */
				task->is_failed = true;

				cancel_queued_crypto_ops(crypto_ch, task);

				task->cryop_completed++;
				/* Fail the IO if there is nothing left on device. */
				if (task->cryop_completed == task->cryop_submitted) {
					spdk_accel_task_complete(&task->base, -EFAULT);
				}
			}
		}
	}

	if (!TAILQ_EMPTY(&crypto_ch->queued_tasks)) {
		TAILQ_INIT(&queued_tasks_tmp);

		TAILQ_FOREACH_SAFE(task, &crypto_ch->queued_tasks, link, task_tmp) {
			TAILQ_REMOVE(&crypto_ch->queued_tasks, task, link);
			rc = accel_dpdk_cryptodev_process_task(crypto_ch, task);
			if (spdk_unlikely(rc)) {
				if (rc == -ENOMEM) {
					TAILQ_INSERT_TAIL(&queued_tasks_tmp, task, link);
					/* Other queued tasks may belong to other qpairs,
					 * so process the whole list */
					continue;
				}
				spdk_accel_task_complete(&task->base, rc);
			} else {
				num_enqueued_ops++;
			}
		}

		TAILQ_SWAP(&crypto_ch->queued_tasks, &queued_tasks_tmp, accel_dpdk_cryptodev_task, link);
	}

	return !!(num_dequeued_ops + num_enqueued_ops);
}

/* Allocate the new mbuf of @remainder size with data pointed by @addr and attach
 * it to the @orig_mbuf. */
static inline int
accel_dpdk_cryptodev_mbuf_chain_remainder(struct accel_dpdk_cryptodev_task *task,
		struct rte_mbuf *orig_mbuf, uint8_t *addr, uint64_t *_remainder)
{
	uint64_t phys_addr, phys_len, remainder = *_remainder;
	struct rte_mbuf *chain_mbuf;
	int rc;

	phys_len = remainder;
	phys_addr = spdk_vtophys((void *)addr, &phys_len);
	if (spdk_unlikely(phys_addr == SPDK_VTOPHYS_ERROR)) {
		return -EFAULT;
	}
	remainder = spdk_min(remainder, phys_len);
	remainder = spdk_min(remainder, ACCEL_DPDK_CRYPTODEV_MAX_MBUF_LEN);
	rc = rte_pktmbuf_alloc_bulk(g_mbuf_mp, (struct rte_mbuf **)&chain_mbuf, 1);
	if (spdk_unlikely(rc)) {
		return -ENOMEM;
	}
	/* Store context in every mbuf as we don't know anything about completion order */
	*RTE_MBUF_DYNFIELD(chain_mbuf, g_mbuf_offset, uint64_t *) = (uint64_t)task;
	rte_pktmbuf_attach_extbuf(chain_mbuf, addr, phys_addr, remainder, &g_shinfo);
	rte_pktmbuf_append(chain_mbuf, remainder);

	/* Chained buffer is released by rte_pktbuf_free_bulk() automagicaly. */
	rte_pktmbuf_chain(orig_mbuf, chain_mbuf);
	*_remainder = remainder;

	return 0;
}

/* Attach data buffer pointed by @addr to @mbuf. Return utilized len of the
 * contiguous space that was physically available. */
static inline uint64_t
accel_dpdk_cryptodev_mbuf_attach_buf(struct accel_dpdk_cryptodev_task *task, struct rte_mbuf *mbuf,
				     uint8_t *addr, uint32_t len)
{
	uint64_t phys_addr, phys_len;

	/* Store context in every mbuf as we don't know anything about completion order */
	*RTE_MBUF_DYNFIELD(mbuf, g_mbuf_offset, uint64_t *) = (uint64_t)task;

	phys_len = len;
	phys_addr = spdk_vtophys((void *)addr, &phys_len);
	if (spdk_unlikely(phys_addr == SPDK_VTOPHYS_ERROR || phys_len == 0)) {
		return 0;
	}
	assert(phys_len <= len);
	phys_len = spdk_min(phys_len, ACCEL_DPDK_CRYPTODEV_MAX_MBUF_LEN);

	/* Set the mbuf elements address and length. */
	rte_pktmbuf_attach_extbuf(mbuf, addr, phys_addr, phys_len, &g_shinfo);
	rte_pktmbuf_append(mbuf, phys_len);

	return phys_len;
}

static inline struct accel_dpdk_cryptodev_key_handle *
accel_dpdk_find_key_handle_in_channel(struct accel_dpdk_cryptodev_io_channel *crypto_ch,
				      struct accel_dpdk_cryptodev_key_priv *key)
{
	struct accel_dpdk_cryptodev_key_handle *key_handle;

	if (key->driver == ACCEL_DPDK_CRYPTODEV_DRIVER_MLX5_PCI) {
		/* Crypto key is registered on all available devices while io_channel opens CQ/QP on a single device.
		 * We need to iterate a list of key entries to find a suitable device */
		TAILQ_FOREACH(key_handle, &key->dev_keys, link) {
			if (key_handle->device->cdev_id ==
			    crypto_ch->device_qp[ACCEL_DPDK_CRYPTODEV_DRIVER_MLX5_PCI]->device->cdev_id) {
				return key_handle;
			}
		}
		return NULL;
	} else {
		return TAILQ_FIRST(&key->dev_keys);
	}
}

static inline int
accel_dpdk_cryptodev_task_alloc_resources(struct rte_mbuf **src_mbufs, struct rte_mbuf **dst_mbufs,
		struct rte_crypto_op **crypto_ops, int count)
{
	int rc;

	/* Get the number of source mbufs that we need. These will always be 1:1 because we
	 * don't support chaining. The reason we don't is because of our decision to use
	 * LBA as IV, there can be no case where we'd need >1 mbuf per crypto op or the
	 * op would be > 1 LBA.
	 */
	rc = rte_pktmbuf_alloc_bulk(g_mbuf_mp, src_mbufs, count);
	if (rc) {
		SPDK_ERRLOG("Failed to get src_mbufs!\n");
		return -ENOMEM;
	}

	/* Get the same amount to describe destination. If crypto operation is inline then we don't just skip it */
	if (dst_mbufs) {
		rc = rte_pktmbuf_alloc_bulk(g_mbuf_mp, dst_mbufs, count);
		if (rc) {
			SPDK_ERRLOG("Failed to get dst_mbufs!\n");
			goto err_free_src;
		}
	}

#ifdef __clang_analyzer__
	/* silence scan-build false positive */
	SPDK_CLANG_ANALYZER_PREINIT_PTR_ARRAY(crypto_ops, ACCEL_DPDK_CRYPTODEV_MAX_ENQUEUE_ARRAY_SIZE,
					      0x1000);
#endif
	/* Allocate crypto operations. */
	rc = rte_crypto_op_bulk_alloc(g_crypto_op_mp,
				      RTE_CRYPTO_OP_TYPE_SYMMETRIC,
				      crypto_ops, count);
	if (rc < count) {
		SPDK_ERRLOG("Failed to allocate crypto ops! rc %d\n", rc);
		goto err_free_ops;
	}

	return 0;

err_free_ops:
	if (rc > 0) {
		rte_mempool_put_bulk(g_crypto_op_mp, (void **)crypto_ops, rc);
	}
	if (dst_mbufs) {
		/* This also releases chained mbufs if any. */
		rte_pktmbuf_free_bulk(dst_mbufs, count);
	}
err_free_src:
	/* This also releases chained mbufs if any. */
	rte_pktmbuf_free_bulk(src_mbufs, count);

	return -ENOMEM;
}

static inline int
accel_dpdk_cryptodev_mbuf_add_single_block(struct spdk_iov_sgl *sgl, struct rte_mbuf *mbuf,
		struct accel_dpdk_cryptodev_task *task)
{
	int rc;
	uint8_t *buf_addr;
	uint64_t phys_len;
	uint64_t remainder;
	uint64_t buf_len;

	assert(sgl->iov->iov_len > sgl->iov_offset);
	buf_len = spdk_min(task->base.block_size, sgl->iov->iov_len - sgl->iov_offset);
	buf_addr = sgl->iov->iov_base + sgl->iov_offset;
	phys_len = accel_dpdk_cryptodev_mbuf_attach_buf(task, mbuf, buf_addr, buf_len);
	if (spdk_unlikely(phys_len == 0)) {
		return -EFAULT;
	}
	buf_len = spdk_min(buf_len, phys_len);
	spdk_iov_sgl_advance(sgl, buf_len);

	/* Handle the case of page boundary. */
	assert(task->base.block_size >= buf_len);
	remainder = task->base.block_size - buf_len;
	while (remainder) {
		buf_len = spdk_min(remainder, sgl->iov->iov_len - sgl->iov_offset);
		buf_addr = sgl->iov->iov_base + sgl->iov_offset;
		rc = accel_dpdk_cryptodev_mbuf_chain_remainder(task, mbuf, buf_addr, &buf_len);
		if (spdk_unlikely(rc)) {
			return rc;
		}
		spdk_iov_sgl_advance(sgl, buf_len);
		remainder -= buf_len;
	}

	return 0;
}

static inline void
accel_dpdk_cryptodev_op_set_iv(struct rte_crypto_op *crypto_op, uint64_t iv)
{
	uint8_t *iv_ptr = rte_crypto_op_ctod_offset(crypto_op, uint8_t *, ACCEL_DPDK_CRYPTODEV_IV_OFFSET);

	/* Set the IV - we use the LBA of the crypto_op */
	memset(iv_ptr, 0, ACCEL_DPDK_CRYPTODEV_IV_LENGTH);
	rte_memcpy(iv_ptr, &iv, sizeof(uint64_t));
}

static int
accel_dpdk_cryptodev_process_task(struct accel_dpdk_cryptodev_io_channel *crypto_ch,
				  struct accel_dpdk_cryptodev_task *task)
{
	uint16_t num_enqueued_ops;
	uint32_t cryop_cnt;
	uint32_t crypto_len = task->base.block_size;
	uint64_t dst_length, total_length;
	uint32_t sgl_offset;
	uint32_t qp_capacity;
	uint64_t iv_start;
	struct accel_dpdk_cryptodev_queued_op *op_to_queue;
	uint32_t i, crypto_index;
	struct rte_crypto_op *crypto_ops[ACCEL_DPDK_CRYPTODEV_MAX_ENQUEUE_ARRAY_SIZE];
	struct rte_mbuf *src_mbufs[ACCEL_DPDK_CRYPTODEV_MAX_ENQUEUE_ARRAY_SIZE];
	struct rte_mbuf *dst_mbufs[ACCEL_DPDK_CRYPTODEV_MAX_ENQUEUE_ARRAY_SIZE];
	void *session;
	struct accel_dpdk_cryptodev_key_priv *priv;
	struct accel_dpdk_cryptodev_key_handle *key_handle;
	struct accel_dpdk_cryptodev_qp *qp;
	struct accel_dpdk_cryptodev_device *dev;
	struct spdk_iov_sgl src, dst = {};
	int rc;

	if (spdk_unlikely(!task->base.crypto_key ||
			  task->base.crypto_key->module_if != &g_accel_dpdk_cryptodev_module)) {
		return -EINVAL;
	}

	priv = task->base.crypto_key->priv;
	assert(priv->driver < ACCEL_DPDK_CRYPTODEV_DRIVER_LAST);

	if (task->cryop_completed) {
		/* We continue to process remaining blocks */
		assert(task->cryop_submitted == task->cryop_completed);
		assert(task->cryop_total > task->cryop_completed);
		cryop_cnt = task->cryop_total - task->cryop_completed;
		sgl_offset = task->cryop_completed * crypto_len;
		iv_start = task->base.iv + task->cryop_completed;
	} else {
		/* That is a new task */
		total_length = 0;
		for (i = 0; i < task->base.s.iovcnt; i++) {
			total_length += task->base.s.iovs[i].iov_len;
		}
		dst_length = 0;
		for (i = 0; i < task->base.d.iovcnt; i++) {
			dst_length += task->base.d.iovs[i].iov_len;
		}

		if (spdk_unlikely(total_length != dst_length || !total_length)) {
			return -ERANGE;
		}
		if (spdk_unlikely(total_length % task->base.block_size != 0)) {
			return -EINVAL;
		}

		cryop_cnt = total_length / task->base.block_size;
		task->cryop_total = cryop_cnt;
		sgl_offset = 0;
		iv_start = task->base.iv;
	}

	/* Limit the number of crypto ops that we can process once */
	cryop_cnt = spdk_min(cryop_cnt, ACCEL_DPDK_CRYPTODEV_MAX_ENQUEUE_ARRAY_SIZE);

	qp = crypto_ch->device_qp[priv->driver];
	assert(qp);
	dev = qp->device;
	assert(dev);
	assert(dev->qp_desc_nr >= qp->num_enqueued_ops);

	qp_capacity = dev->qp_desc_nr - qp->num_enqueued_ops;
	cryop_cnt = spdk_min(cryop_cnt, qp_capacity);
	if (spdk_unlikely(cryop_cnt == 0)) {
		/* QP is full */
		return -ENOMEM;
	}

	key_handle = accel_dpdk_find_key_handle_in_channel(crypto_ch, priv);
	if (spdk_unlikely(!key_handle)) {
		SPDK_ERRLOG("Failed to find a key handle, driver %s, cipher %s\n", g_driver_names[priv->driver],
			    g_cipher_names[priv->cipher]);
		return -EINVAL;
	}
	/* mlx5_pci binds keys to a specific device, we can't use a key with any device */
	assert(dev == key_handle->device || priv->driver != ACCEL_DPDK_CRYPTODEV_DRIVER_MLX5_PCI);

	if (task->base.op_code == ACCEL_OPC_ENCRYPT) {
		session = key_handle->session_encrypt;
	} else if (task->base.op_code == ACCEL_OPC_DECRYPT) {
		session = key_handle->session_decrypt;
	} else {
		return -EINVAL;
	}

	rc = accel_dpdk_cryptodev_task_alloc_resources(src_mbufs, task->inplace ? NULL : dst_mbufs,
			crypto_ops, cryop_cnt);
	if (rc) {
		return rc;
	}
	/* This value is used in the completion callback to determine when the accel task is complete. */
	task->cryop_submitted += cryop_cnt;

	/* As we don't support chaining because of a decision to use LBA as IV, construction
	 * of crypto operations is straightforward. We build both the op, the mbuf and the
	 * dst_mbuf in our local arrays by looping through the length of the accel task and
	 * picking off LBA sized blocks of memory from the IOVs as we walk through them. Each
	 * LBA sized chunk of memory will correspond 1:1 to a crypto operation and a single
	 * mbuf per crypto operation.
	 */
	spdk_iov_sgl_init(&src, task->base.s.iovs, task->base.s.iovcnt, 0);
	spdk_iov_sgl_advance(&src, sgl_offset);
	if (!task->inplace) {
		spdk_iov_sgl_init(&dst, task->base.d.iovs, task->base.d.iovcnt, 0);
		spdk_iov_sgl_advance(&dst, sgl_offset);
	}

	for (crypto_index = 0; crypto_index < cryop_cnt; crypto_index++) {
		rc = accel_dpdk_cryptodev_mbuf_add_single_block(&src, src_mbufs[crypto_index], task);
		if (spdk_unlikely(rc)) {
			goto err_free_ops;
		}
		accel_dpdk_cryptodev_op_set_iv(crypto_ops[crypto_index], iv_start);
		iv_start++;

		/* Set the data to encrypt/decrypt length */
		crypto_ops[crypto_index]->sym->cipher.data.length = crypto_len;
		crypto_ops[crypto_index]->sym->cipher.data.offset = 0;
		rte_crypto_op_attach_sym_session(crypto_ops[crypto_index], session);

		/* link the mbuf to the crypto op. */
		crypto_ops[crypto_index]->sym->m_src = src_mbufs[crypto_index];

		if (task->inplace) {
			crypto_ops[crypto_index]->sym->m_dst = NULL;
		} else {
#ifndef __clang_analyzer__
			/* scan-build thinks that dst_mbufs is not initialized */
			rc = accel_dpdk_cryptodev_mbuf_add_single_block(&dst, dst_mbufs[crypto_index], task);
			if (spdk_unlikely(rc)) {
				goto err_free_ops;
			}
			crypto_ops[crypto_index]->sym->m_dst = dst_mbufs[crypto_index];
#endif
		}
	}

	/* Enqueue everything we've got but limit by the max number of descriptors we
	 * configured the crypto device for.
	 */
	num_enqueued_ops = rte_cryptodev_enqueue_burst(dev->cdev_id, qp->qp, crypto_ops, cryop_cnt);

	qp->num_enqueued_ops += num_enqueued_ops;
	/* We were unable to enqueue everything but did get some, so need to decide what
	 * to do based on the status of the last op.
	 */
	if (num_enqueued_ops < cryop_cnt) {
		switch (crypto_ops[num_enqueued_ops]->status) {
		case RTE_CRYPTO_OP_STATUS_NOT_PROCESSED:
			/* Queue them up on a linked list to be resubmitted via the poller. */
			for (crypto_index = num_enqueued_ops; crypto_index < cryop_cnt; crypto_index++) {
				op_to_queue = (struct accel_dpdk_cryptodev_queued_op *)rte_crypto_op_ctod_offset(
						      crypto_ops[crypto_index],
						      uint8_t *, ACCEL_DPDK_CRYPTODEV_QUEUED_OP_OFFSET);
				op_to_queue->qp = qp;
				op_to_queue->crypto_op = crypto_ops[crypto_index];
				op_to_queue->task = task;
				TAILQ_INSERT_TAIL(&crypto_ch->queued_cry_ops, op_to_queue, link);
			}
			break;
		default:
			/* For all other statuses, mark task as failed so that the poller will pick
			 * the failure up for the overall task status.
			 */
			task->is_failed = true;
			if (num_enqueued_ops == 0) {
				/* If nothing was enqueued, but the last one wasn't because of
				 * busy, fail it now as the poller won't know anything about it.
				 */
				rc = -EINVAL;
				goto err_free_ops;
			}
			break;
		}
	}

	return 0;

	/* Error cleanup paths. */
err_free_ops:
	if (!task->inplace) {
		/* This also releases chained mbufs if any. */
		rte_pktmbuf_free_bulk(dst_mbufs, cryop_cnt);
	}
	rte_mempool_put_bulk(g_crypto_op_mp, (void **)crypto_ops, cryop_cnt);
	/* This also releases chained mbufs if any. */
	rte_pktmbuf_free_bulk(src_mbufs, cryop_cnt);
	return rc;
}

static inline struct accel_dpdk_cryptodev_qp *
accel_dpdk_cryptodev_get_next_device_qpair(enum accel_dpdk_cryptodev_driver_type type)
{
	struct accel_dpdk_cryptodev_device *device, *device_tmp;
	struct accel_dpdk_cryptodev_qp *qpair;

	TAILQ_FOREACH_SAFE(device, &g_crypto_devices, link, device_tmp) {
		if (device->type != type) {
			continue;
		}
		TAILQ_FOREACH(qpair, &device->qpairs, link) {
			if (!qpair->in_use) {
				qpair->in_use = true;
				return qpair;
			}
		}
	}

	return NULL;
}

/* Helper function for the channel creation callback.
 * Returns the number of drivers assigned to the channel */
static uint32_t
accel_dpdk_cryptodev_assign_device_qps(struct accel_dpdk_cryptodev_io_channel *crypto_ch)
{
	struct accel_dpdk_cryptodev_device *device;
	struct accel_dpdk_cryptodev_qp *device_qp;
	uint32_t num_drivers = 0;
	bool qat_found = false;

	pthread_mutex_lock(&g_device_lock);

	TAILQ_FOREACH(device, &g_crypto_devices, link) {
		if (device->type == ACCEL_DPDK_CRYPTODEV_DRIVER_QAT && !qat_found) {
			/* For some QAT devices, the optimal qp to use is every 32nd as this spreads the
			 * workload out over the multiple virtual functions in the device. For the devices
			 * where this isn't the case, it doesn't hurt.
			 */
			TAILQ_FOREACH(device_qp, &device->qpairs, link) {
				if (device_qp->index != g_next_qat_index) {
					continue;
				}
				if (device_qp->in_use == false) {
					assert(crypto_ch->device_qp[ACCEL_DPDK_CRYPTODEV_DRIVER_QAT] == NULL);
					crypto_ch->device_qp[ACCEL_DPDK_CRYPTODEV_DRIVER_QAT] = device_qp;
					device_qp->in_use = true;
					g_next_qat_index = (g_next_qat_index + ACCEL_DPDK_CRYPTODEV_QAT_VF_SPREAD) % g_qat_total_qp;
					qat_found = true;
					num_drivers++;
					break;
				} else {
					/* if the preferred index is used, skip to the next one in this set. */
					g_next_qat_index = (g_next_qat_index + 1) % g_qat_total_qp;
				}
			}
		}
	}

	/* For ACCEL_DPDK_CRYPTODEV_AESNI_MB and MLX5_PCI select devices in round-robin manner */
	device_qp = accel_dpdk_cryptodev_get_next_device_qpair(ACCEL_DPDK_CRYPTODEV_DRIVER_AESNI_MB);
	if (device_qp) {
		assert(crypto_ch->device_qp[ACCEL_DPDK_CRYPTODEV_DRIVER_AESNI_MB] == NULL);
		crypto_ch->device_qp[ACCEL_DPDK_CRYPTODEV_DRIVER_AESNI_MB] = device_qp;
		num_drivers++;
	}

	device_qp = accel_dpdk_cryptodev_get_next_device_qpair(ACCEL_DPDK_CRYPTODEV_DRIVER_MLX5_PCI);
	if (device_qp) {
		assert(crypto_ch->device_qp[ACCEL_DPDK_CRYPTODEV_DRIVER_MLX5_PCI] == NULL);
		crypto_ch->device_qp[ACCEL_DPDK_CRYPTODEV_DRIVER_MLX5_PCI] = device_qp;
		num_drivers++;
	}

	pthread_mutex_unlock(&g_device_lock);

	return num_drivers;
}

static void
_accel_dpdk_cryptodev_destroy_cb(void *io_device, void *ctx_buf)
{
	struct accel_dpdk_cryptodev_io_channel *crypto_ch = (struct accel_dpdk_cryptodev_io_channel *)
			ctx_buf;
	int i;

	pthread_mutex_lock(&g_device_lock);
	for (i = 0; i < ACCEL_DPDK_CRYPTODEV_DRIVER_LAST; i++) {
		if (crypto_ch->device_qp[i]) {
			crypto_ch->device_qp[i]->in_use = false;
		}
	}
	pthread_mutex_unlock(&g_device_lock);

	spdk_poller_unregister(&crypto_ch->poller);
}

static int
_accel_dpdk_cryptodev_create_cb(void *io_device, void *ctx_buf)
{
	struct accel_dpdk_cryptodev_io_channel *crypto_ch = (struct accel_dpdk_cryptodev_io_channel *)
			ctx_buf;

	crypto_ch->poller = SPDK_POLLER_REGISTER(accel_dpdk_cryptodev_poller, crypto_ch, 0);
	if (!accel_dpdk_cryptodev_assign_device_qps(crypto_ch)) {
		SPDK_ERRLOG("No crypto drivers assigned\n");
		spdk_poller_unregister(&crypto_ch->poller);
		return -EINVAL;
	}

	/* We use this to queue up crypto ops when the device is busy. */
	TAILQ_INIT(&crypto_ch->queued_cry_ops);
	/* We use this to queue tasks when qpair is full or no resources in pools */
	TAILQ_INIT(&crypto_ch->queued_tasks);

	return 0;
}

static struct spdk_io_channel *
accel_dpdk_cryptodev_get_io_channel(void)
{
	return spdk_get_io_channel(&g_accel_dpdk_cryptodev_module);
}

static size_t
accel_dpdk_cryptodev_ctx_size(void)
{
	return sizeof(struct accel_dpdk_cryptodev_task);
}

static bool
accel_dpdk_cryptodev_supports_opcode(enum accel_opcode opc)
{
	switch (opc) {
	case ACCEL_OPC_ENCRYPT:
	case ACCEL_OPC_DECRYPT:
		return true;
	default:
		return false;
	}
}

static int
accel_dpdk_cryptodev_submit_tasks(struct spdk_io_channel *_ch, struct spdk_accel_task *_task)
{
	struct accel_dpdk_cryptodev_task *task = SPDK_CONTAINEROF(_task, struct accel_dpdk_cryptodev_task,
			base);
	struct accel_dpdk_cryptodev_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	int rc;

	task->cryop_completed = 0;
	task->cryop_submitted = 0;
	task->cryop_total = 0;
	task->inplace = true;
	task->is_failed = false;

	/* Check if crypto operation is inplace: no destination or source == destination */
	if (task->base.s.iovcnt == task->base.d.iovcnt) {
		if (memcmp(task->base.s.iovs, task->base.d.iovs, sizeof(struct iovec) * task->base.s.iovcnt) != 0) {
			task->inplace = false;
		}
	} else if (task->base.d.iovcnt != 0) {
		task->inplace = false;
	}

	rc = accel_dpdk_cryptodev_process_task(ch, task);
	if (spdk_unlikely(rc == -ENOMEM)) {
		TAILQ_INSERT_TAIL(&ch->queued_tasks, task, link);
		rc = 0;
	}

	return rc;
}

/* Dummy function used by DPDK to free ext attached buffers to mbufs, we free them ourselves but
 * this callback has to be here. */
static void
shinfo_free_cb(void *arg1, void *arg2)
{
}

static int
accel_dpdk_cryptodev_create(uint8_t index, uint16_t num_lcores)
{
	struct rte_cryptodev_qp_conf qp_conf = {
		.mp_session = g_session_mp,
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
		.mp_session_private = g_session_mp_priv
#endif
	};
	/* Setup queue pairs. */
	struct rte_cryptodev_config conf = { .socket_id = SPDK_ENV_SOCKET_ID_ANY };
	struct accel_dpdk_cryptodev_device *device;
	uint8_t j, cdev_id, cdrv_id;
	struct accel_dpdk_cryptodev_qp *dev_qp;
	int rc;

	device = calloc(1, sizeof(*device));
	if (!device) {
		return -ENOMEM;
	}

	/* Get details about this device. */
	rte_cryptodev_info_get(index, &device->cdev_info);
	cdrv_id = device->cdev_info.driver_id;
	cdev_id = device->cdev_id = index;

	if (strcmp(device->cdev_info.driver_name, ACCEL_DPDK_CRYPTODEV_QAT) == 0) {
		device->qp_desc_nr = ACCEL_DPDK_CRYPTODEV_QP_DESCRIPTORS;
		device->type = ACCEL_DPDK_CRYPTODEV_DRIVER_QAT;
	} else if (strcmp(device->cdev_info.driver_name, ACCEL_DPDK_CRYPTODEV_AESNI_MB) == 0) {
		device->qp_desc_nr = ACCEL_DPDK_CRYPTODEV_QP_DESCRIPTORS;
		device->type = ACCEL_DPDK_CRYPTODEV_DRIVER_AESNI_MB;
	} else if (strcmp(device->cdev_info.driver_name, ACCEL_DPDK_CRYPTODEV_MLX5) == 0) {
		device->qp_desc_nr = ACCEL_DPDK_CRYPTODEV_QP_DESCRIPTORS_MLX5;
		device->type = ACCEL_DPDK_CRYPTODEV_DRIVER_MLX5_PCI;
	} else if (strcmp(device->cdev_info.driver_name, ACCEL_DPDK_CRYPTODEV_QAT_ASYM) == 0) {
		/* ACCEL_DPDK_CRYPTODEV_QAT_ASYM devices are not supported at this time. */
		rc = 0;
		goto err;
	} else {
		SPDK_ERRLOG("Failed to start device %u. Invalid driver name \"%s\"\n",
			    cdev_id, device->cdev_info.driver_name);
		rc = -EINVAL;
		goto err;
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

	conf.nb_queue_pairs = device->cdev_info.max_nb_queue_pairs;
	rc = rte_cryptodev_configure(cdev_id, &conf);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to configure cryptodev %u: error %d\n",
			    cdev_id, rc);
		rc = -EINVAL;
		goto err;
	}

	/* Pre-setup all potential qpairs now and assign them in the channel
	 * callback. If we were to create them there, we'd have to stop the
	 * entire device affecting all other threads that might be using it
	 * even on other queue pairs.
	 */
	qp_conf.nb_descriptors = device->qp_desc_nr;
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
		SPDK_ERRLOG("Failed to start device %u: error %d\n", cdev_id, rc);
		rc = -EINVAL;
		goto err_dev_start;
	}

	TAILQ_INIT(&device->qpairs);
	/* Build up lists of device/qp combinations per PMD */
	for (j = 0; j < device->cdev_info.max_nb_queue_pairs; j++) {
		dev_qp = calloc(1, sizeof(*dev_qp));
		if (!dev_qp) {
			rc = -ENOMEM;
			goto err_qp_alloc;
		}
		dev_qp->device = device;
		dev_qp->qp = j;
		dev_qp->in_use = false;
		TAILQ_INSERT_TAIL(&device->qpairs, dev_qp, link);
		if (device->type == ACCEL_DPDK_CRYPTODEV_DRIVER_QAT) {
			dev_qp->index = g_qat_total_qp++;
		}
	}
	/* Add to our list of available crypto devices. */
	TAILQ_INSERT_TAIL(&g_crypto_devices, device, link);

	return 0;

err_qp_alloc:
	TAILQ_FOREACH(dev_qp, &device->qpairs, link) {
		if (dev_qp->device->cdev_id != device->cdev_id) {
			continue;
		}
		free(dev_qp);
		if (device->type == ACCEL_DPDK_CRYPTODEV_DRIVER_QAT) {
			assert(g_qat_total_qp);
			g_qat_total_qp--;
		}
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
accel_dpdk_cryptodev_release(struct accel_dpdk_cryptodev_device *device)
{
	struct accel_dpdk_cryptodev_qp *dev_qp, *tmp;

	assert(device);

	TAILQ_FOREACH_SAFE(dev_qp, &device->qpairs, link, tmp) {
		free(dev_qp);
	}
	if (device->type == ACCEL_DPDK_CRYPTODEV_DRIVER_QAT) {
		assert(g_qat_total_qp >= device->cdev_info.max_nb_queue_pairs);
		g_qat_total_qp -= device->cdev_info.max_nb_queue_pairs;
	}
	rte_cryptodev_stop(device->cdev_id);
	rte_cryptodev_close(device->cdev_id);
	free(device);
}

static int
accel_dpdk_cryptodev_init(void)
{
	uint8_t cdev_count;
	uint8_t cdev_id;
	int i, rc;
	struct accel_dpdk_cryptodev_device *device, *tmp_dev;
	unsigned int max_sess_size = 0, sess_size;
	uint16_t num_lcores = rte_lcore_count();
	char aesni_args[32];

	/* Only the first call via module init should init the crypto drivers. */
	if (g_session_mp != NULL) {
		return 0;
	}

	/* We always init ACCEL_DPDK_CRYPTODEV_AESNI_MB */
	snprintf(aesni_args, sizeof(aesni_args), "max_nb_queue_pairs=%d",
		 ACCEL_DPDK_CRYPTODEV_AESNI_MB_NUM_QP);
	rc = rte_vdev_init(ACCEL_DPDK_CRYPTODEV_AESNI_MB, aesni_args);
	if (rc) {
		SPDK_NOTICELOG("Failed to create virtual PMD %s: error %d. "
			       "Possibly %s is not supported by DPDK library. "
			       "Keep going...\n", ACCEL_DPDK_CRYPTODEV_AESNI_MB, rc, ACCEL_DPDK_CRYPTODEV_AESNI_MB);
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

	/* Create global mempools, shared by all devices regardless of type */
	/* First determine max session size, most pools are shared by all the devices,
	 * so we need to find the global max sessions size. */
	for (cdev_id = 0; cdev_id < cdev_count; cdev_id++) {
		sess_size = rte_cryptodev_sym_get_private_session_size(cdev_id);
		if (sess_size > max_sess_size) {
			max_sess_size = sess_size;
		}
	}

#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
	g_session_mp_priv = rte_mempool_create("dpdk_crypto_ses_mp_priv",
					       ACCEL_DPDK_CRYPTODEV_NUM_SESSIONS, max_sess_size, ACCEL_DPDK_CRYPTODEV_SESS_MEMPOOL_CACHE_SIZE, 0,
					       NULL, NULL, NULL, NULL, SOCKET_ID_ANY, 0);
	if (g_session_mp_priv == NULL) {
		SPDK_ERRLOG("Cannot create private session pool max size 0x%x\n", max_sess_size);
		return -ENOMEM;
	}

	/* When session private data mempool allocated, the element size for the session mempool
	 * should be 0. */
	max_sess_size = 0;
#endif

	g_session_mp = rte_cryptodev_sym_session_pool_create("dpdk_crypto_ses_mp",
			ACCEL_DPDK_CRYPTODEV_NUM_SESSIONS, max_sess_size, ACCEL_DPDK_CRYPTODEV_SESS_MEMPOOL_CACHE_SIZE, 0,
			SOCKET_ID_ANY);
	if (g_session_mp == NULL) {
		SPDK_ERRLOG("Cannot create session pool max size 0x%x\n", max_sess_size);
		rc = -ENOMEM;
		goto error_create_session_mp;
	}

	g_mbuf_mp = rte_pktmbuf_pool_create("dpdk_crypto_mbuf_mp", ACCEL_DPDK_CRYPTODEV_NUM_MBUFS,
					    ACCEL_DPDK_CRYPTODEV_POOL_CACHE_SIZE,
					    0, 0, SPDK_ENV_SOCKET_ID_ANY);
	if (g_mbuf_mp == NULL) {
		SPDK_ERRLOG("Cannot create mbuf pool\n");
		rc = -ENOMEM;
		goto error_create_mbuf;
	}

	/* We use per op private data as suggested by DPDK and to store the IV and
	 * our own struct for queueing ops. */
	g_crypto_op_mp = rte_crypto_op_pool_create("dpdk_crypto_op_mp",
			 RTE_CRYPTO_OP_TYPE_SYMMETRIC, ACCEL_DPDK_CRYPTODEV_NUM_MBUFS, ACCEL_DPDK_CRYPTODEV_POOL_CACHE_SIZE,
			 (ACCEL_DPDK_CRYPTODEV_DEFAULT_NUM_XFORMS * sizeof(struct rte_crypto_sym_xform)) +
			 ACCEL_DPDK_CRYPTODEV_IV_LENGTH + ACCEL_DPDK_CRYPTODEV_QUEUED_OP_LENGTH, rte_socket_id());
	if (g_crypto_op_mp == NULL) {
		SPDK_ERRLOG("Cannot create op pool\n");
		rc = -ENOMEM;
		goto error_create_op;
	}

	/* Init all devices */
	for (i = 0; i < cdev_count; i++) {
		rc = accel_dpdk_cryptodev_create(i, num_lcores);
		if (rc) {
			goto err;
		}
	}

	g_shinfo.free_cb = shinfo_free_cb;

	spdk_io_device_register(&g_accel_dpdk_cryptodev_module, _accel_dpdk_cryptodev_create_cb,
				_accel_dpdk_cryptodev_destroy_cb, sizeof(struct accel_dpdk_cryptodev_io_channel),
				"accel_dpdk_cryptodev");

	return 0;

	/* Error cleanup paths. */
err:
	TAILQ_FOREACH_SAFE(device, &g_crypto_devices, link, tmp_dev) {
		TAILQ_REMOVE(&g_crypto_devices, device, link);
		accel_dpdk_cryptodev_release(device);
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

static void
accel_dpdk_cryptodev_fini_cb(void *io_device)
{
	struct accel_dpdk_cryptodev_device *device, *tmp;

	TAILQ_FOREACH_SAFE(device, &g_crypto_devices, link, tmp) {
		TAILQ_REMOVE(&g_crypto_devices, device, link);
		accel_dpdk_cryptodev_release(device);
	}
	rte_vdev_uninit(ACCEL_DPDK_CRYPTODEV_AESNI_MB);

	rte_mempool_free(g_crypto_op_mp);
	rte_mempool_free(g_mbuf_mp);
	rte_mempool_free(g_session_mp);
	if (g_session_mp_priv != NULL) {
		rte_mempool_free(g_session_mp_priv);
	}

	spdk_accel_module_finish();
}

/* Called when the entire module is being torn down. */
static void
accel_dpdk_cryptodev_fini(void *ctx)
{
	if (g_crypto_op_mp) {
		spdk_io_device_unregister(&g_accel_dpdk_cryptodev_module, accel_dpdk_cryptodev_fini_cb);
	}
}

static void
accel_dpdk_cryptodev_key_handle_session_free(struct accel_dpdk_cryptodev_device *device,
		void *session)
{
#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
	assert(device != NULL);

	rte_cryptodev_sym_session_free(device->cdev_id, session);
#else
	rte_cryptodev_sym_session_free(session);
#endif
}

static void *
accel_dpdk_cryptodev_key_handle_session_create(struct accel_dpdk_cryptodev_device *device,
		struct rte_crypto_sym_xform *cipher_xform)
{
	void *session;

#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
	session = rte_cryptodev_sym_session_create(device->cdev_id, cipher_xform, g_session_mp);
#else
	session = rte_cryptodev_sym_session_create(g_session_mp);
	if (!session) {
		return NULL;
	}

	if (rte_cryptodev_sym_session_init(device->cdev_id, session, cipher_xform, g_session_mp_priv) < 0) {
		accel_dpdk_cryptodev_key_handle_session_free(device, session);
		return NULL;
	}
#endif

	return session;
}

static int
accel_dpdk_cryptodev_key_handle_configure(struct spdk_accel_crypto_key *key,
		struct accel_dpdk_cryptodev_key_handle *key_handle)
{
	struct accel_dpdk_cryptodev_key_priv *priv = key->priv;

	key_handle->cipher_xform.type = RTE_CRYPTO_SYM_XFORM_CIPHER;
	key_handle->cipher_xform.cipher.iv.offset = ACCEL_DPDK_CRYPTODEV_IV_OFFSET;
	key_handle->cipher_xform.cipher.iv.length = ACCEL_DPDK_CRYPTODEV_IV_LENGTH;

	switch (priv->cipher) {
	case ACCEL_DPDK_CRYPTODEV_CIPHER_AES_CBC:
		key_handle->cipher_xform.cipher.key.data = key->key;
		key_handle->cipher_xform.cipher.key.length = key->key_size;
		key_handle->cipher_xform.cipher.algo = RTE_CRYPTO_CIPHER_AES_CBC;
		break;
	case ACCEL_DPDK_CRYPTODEV_CIPHER_AES_XTS:
		key_handle->cipher_xform.cipher.key.data = priv->xts_key;
		key_handle->cipher_xform.cipher.key.length = key->key_size + key->key2_size;
		key_handle->cipher_xform.cipher.algo = RTE_CRYPTO_CIPHER_AES_XTS;
		break;
	default:
		SPDK_ERRLOG("Invalid cipher name %s.\n", key->param.cipher);
		return -EINVAL;
	}

	key_handle->cipher_xform.cipher.op = RTE_CRYPTO_CIPHER_OP_ENCRYPT;
	key_handle->session_encrypt = accel_dpdk_cryptodev_key_handle_session_create(key_handle->device,
				      &key_handle->cipher_xform);
	if (!key_handle->session_encrypt) {
		SPDK_ERRLOG("Failed to init encrypt session\n");
		return -EINVAL;
	}

	key_handle->cipher_xform.cipher.op = RTE_CRYPTO_CIPHER_OP_DECRYPT;
	key_handle->session_decrypt = accel_dpdk_cryptodev_key_handle_session_create(key_handle->device,
				      &key_handle->cipher_xform);
	if (!key_handle->session_decrypt) {
		SPDK_ERRLOG("Failed to init decrypt session:");
		accel_dpdk_cryptodev_key_handle_session_free(key_handle->device, key_handle->session_encrypt);
		return -EINVAL;
	}

	return 0;
}

static int
accel_dpdk_cryptodev_validate_parameters(enum accel_dpdk_cryptodev_driver_type driver,
		enum accel_dpdk_crypto_dev_cipher_type cipher, struct spdk_accel_crypto_key *key)
{
	/* Check that all required parameters exist */
	switch (cipher) {
	case ACCEL_DPDK_CRYPTODEV_CIPHER_AES_CBC:
		if (!key->key || !key->key_size) {
			SPDK_ERRLOG("ACCEL_DPDK_CRYPTODEV_AES_CBC requires a key\n");
			return -1;
		}
		if (key->key2 || key->key2_size) {
			SPDK_ERRLOG("ACCEL_DPDK_CRYPTODEV_AES_CBC doesn't use key2\n");
			return -1;
		}
		break;
	case ACCEL_DPDK_CRYPTODEV_CIPHER_AES_XTS:
		if (!key->key || !key->key_size || !key->key2 || !key->key2_size) {
			SPDK_ERRLOG("ACCEL_DPDK_CRYPTODEV_AES_XTS requires both key and key2\n");
			return -1;
		}
		break;
	default:
		return -1;
	}

	/* Check driver/cipher combinations and key lengths */
	switch (cipher) {
	case ACCEL_DPDK_CRYPTODEV_CIPHER_AES_CBC:
		if (driver == ACCEL_DPDK_CRYPTODEV_DRIVER_MLX5_PCI) {
			SPDK_ERRLOG("Driver %s only supports cipher %s\n",
				    g_driver_names[ACCEL_DPDK_CRYPTODEV_DRIVER_MLX5_PCI],
				    g_cipher_names[ACCEL_DPDK_CRYPTODEV_CIPHER_AES_XTS]);
			return -1;
		}
		if (key->key_size != ACCEL_DPDK_CRYPTODEV_AES_CBC_KEY_LENGTH) {
			SPDK_ERRLOG("Invalid key size %zu for cipher %s, should be %d\n", key->key_size,
				    g_cipher_names[ACCEL_DPDK_CRYPTODEV_CIPHER_AES_CBC], ACCEL_DPDK_CRYPTODEV_AES_CBC_KEY_LENGTH);
			return -1;
		}
		break;
	case ACCEL_DPDK_CRYPTODEV_CIPHER_AES_XTS:
		switch (driver) {
		case ACCEL_DPDK_CRYPTODEV_DRIVER_MLX5_PCI:
			if (key->key_size != ACCEL_DPDK_CRYPTODEV_AES_XTS_256_BLOCK_KEY_LENGTH &&
			    key->key_size != ACCEL_DPDK_CRYPTODEV_AES_XTS_512_BLOCK_KEY_LENGTH) {
				SPDK_ERRLOG("Invalid key size %zu for driver %s, cipher %s, supported %d or %d\n",
					    key->key_size, g_driver_names[ACCEL_DPDK_CRYPTODEV_DRIVER_MLX5_PCI],
					    g_cipher_names[ACCEL_DPDK_CRYPTODEV_CIPHER_AES_XTS],
					    ACCEL_DPDK_CRYPTODEV_AES_XTS_256_BLOCK_KEY_LENGTH,
					    ACCEL_DPDK_CRYPTODEV_AES_XTS_512_BLOCK_KEY_LENGTH);
				return -1;
			}
			break;
		case ACCEL_DPDK_CRYPTODEV_DRIVER_QAT:
		case ACCEL_DPDK_CRYPTODEV_DRIVER_AESNI_MB:
			if (key->key_size != ACCEL_DPDK_CRYPTODEV_AES_XTS_128_BLOCK_KEY_LENGTH) {
				SPDK_ERRLOG("Invalid key size %zu, supported %d\n", key->key_size,
					    ACCEL_DPDK_CRYPTODEV_AES_XTS_128_BLOCK_KEY_LENGTH);
				return -1;
			}
			break;
		default:
			SPDK_ERRLOG("Incorrect driver type %d\n", driver);
			assert(0);
			return -1;
		}
		if (key->key2_size != ACCEL_DPDK_CRYPTODEV_AES_XTS_TWEAK_KEY_LENGTH) {
			SPDK_ERRLOG("Cipher %s requires key2 size %d\n",
				    g_cipher_names[ACCEL_DPDK_CRYPTODEV_CIPHER_AES_CBC], ACCEL_DPDK_CRYPTODEV_AES_XTS_TWEAK_KEY_LENGTH);
			return -1;
		}
		break;
	}

	return 0;
}

static void
accel_dpdk_cryptodev_key_deinit(struct spdk_accel_crypto_key *key)
{
	struct accel_dpdk_cryptodev_key_handle *key_handle, *key_handle_tmp;
	struct accel_dpdk_cryptodev_key_priv *priv = key->priv;

	TAILQ_FOREACH_SAFE(key_handle, &priv->dev_keys, link, key_handle_tmp) {
		accel_dpdk_cryptodev_key_handle_session_free(key_handle->device, key_handle->session_encrypt);
		accel_dpdk_cryptodev_key_handle_session_free(key_handle->device, key_handle->session_decrypt);
		TAILQ_REMOVE(&priv->dev_keys, key_handle, link);
		spdk_memset_s(key_handle, sizeof(*key_handle), 0, sizeof(*key_handle));
		free(key_handle);
	}

	if (priv->xts_key) {
		spdk_memset_s(priv->xts_key, key->key_size + key->key2_size, 0, key->key_size + key->key2_size);
	}
	free(priv->xts_key);
	free(priv);
}

static int
accel_dpdk_cryptodev_key_init(struct spdk_accel_crypto_key *key)
{
	struct accel_dpdk_cryptodev_device *device;
	struct accel_dpdk_cryptodev_key_priv *priv;
	struct accel_dpdk_cryptodev_key_handle *key_handle;
	enum accel_dpdk_cryptodev_driver_type driver;
	enum accel_dpdk_crypto_dev_cipher_type cipher;
	int rc;

	if (!key->param.cipher) {
		SPDK_ERRLOG("Cipher is missing\n");
		return -EINVAL;
	}

	if (strcmp(key->param.cipher, ACCEL_DPDK_CRYPTODEV_AES_CBC) == 0) {
		cipher = ACCEL_DPDK_CRYPTODEV_CIPHER_AES_CBC;
	} else if (strcmp(key->param.cipher, ACCEL_DPDK_CRYPTODEV_AES_XTS) == 0) {
		cipher = ACCEL_DPDK_CRYPTODEV_CIPHER_AES_XTS;
	} else {
		SPDK_ERRLOG("Unsupported cipher name %s.\n", key->param.cipher);
		return -EINVAL;
	}

	driver = g_dpdk_cryptodev_driver;

	if (accel_dpdk_cryptodev_validate_parameters(driver, cipher, key)) {
		return -EINVAL;
	}

	priv = calloc(1, sizeof(*priv));
	if (!priv) {
		SPDK_ERRLOG("Memory allocation failed\n");
		return -ENOMEM;
	}
	key->priv = priv;
	priv->driver = driver;
	priv->cipher = cipher;
	TAILQ_INIT(&priv->dev_keys);

	if (cipher == ACCEL_DPDK_CRYPTODEV_CIPHER_AES_XTS) {
		/* DPDK expects the keys to be concatenated together. */
		priv->xts_key = calloc(key->key_size + key->key2_size + 1, sizeof(char));
		if (!priv->xts_key) {
			SPDK_ERRLOG("Memory allocation failed\n");
			accel_dpdk_cryptodev_key_deinit(key);
			return -ENOMEM;
		}
		memcpy(priv->xts_key, key->key, key->key_size);
		memcpy(priv->xts_key + key->key_size, key->key2, key->key2_size);
	}

	pthread_mutex_lock(&g_device_lock);
	TAILQ_FOREACH(device, &g_crypto_devices, link) {
		if (device->type != driver) {
			continue;
		}
		key_handle = calloc(1, sizeof(*key_handle));
		if (!key_handle) {
			pthread_mutex_unlock(&g_device_lock);
			accel_dpdk_cryptodev_key_deinit(key);
			return -ENOMEM;
		}
		key_handle->device = device;
		TAILQ_INSERT_TAIL(&priv->dev_keys, key_handle, link);
		rc = accel_dpdk_cryptodev_key_handle_configure(key, key_handle);
		if (rc) {
			pthread_mutex_unlock(&g_device_lock);
			accel_dpdk_cryptodev_key_deinit(key);
			return rc;
		}
		if (driver != ACCEL_DPDK_CRYPTODEV_DRIVER_MLX5_PCI) {
			/* For MLX5_PCI we need to register a key on each device since
			 * the key is bound to a specific Protection Domain,
			 * so don't break the loop */
			break;
		}
	}
	pthread_mutex_unlock(&g_device_lock);

	if (TAILQ_EMPTY(&priv->dev_keys)) {
		free(priv);
		return -ENODEV;
	}

	return 0;
}

static void
accel_dpdk_cryptodev_write_config_json(struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "dpdk_cryptodev_scan_accel_module");
	spdk_json_write_object_end(w);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "dpdk_cryptodev_set_driver");
	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "driver_name", g_driver_names[g_dpdk_cryptodev_driver]);
	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);
}

static struct spdk_accel_module_if g_accel_dpdk_cryptodev_module = {
	.module_init		= accel_dpdk_cryptodev_init,
	.module_fini		= accel_dpdk_cryptodev_fini,
	.write_config_json	= accel_dpdk_cryptodev_write_config_json,
	.get_ctx_size		= accel_dpdk_cryptodev_ctx_size,
	.name			= "dpdk_cryptodev",
	.supports_opcode	= accel_dpdk_cryptodev_supports_opcode,
	.get_io_channel		= accel_dpdk_cryptodev_get_io_channel,
	.submit_tasks		= accel_dpdk_cryptodev_submit_tasks,
	.crypto_key_init	= accel_dpdk_cryptodev_key_init,
	.crypto_key_deinit	= accel_dpdk_cryptodev_key_deinit,
};
