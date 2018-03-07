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

#include "spdk/stdinc.h"

#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/string.h"
#include "spdk/io_channel.h"
#include "spdk/util.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

#include <rte_config.h>
#include <rte_bus_vdev.h>
#include <rte_crypto.h>
#include <rte_cryptodev.h>
#include <rte_cryptodev_pmd.h>

#define CRYPTO_NUM_QPAIRS	1
#define CRYPTO_QP_DESCRIPTORS	1024

/* To add support for new PMD types, follow the examples of the following... */
#define AESNI_MB "crypto_aesni_mb"
#define QAT "qat"
#define MAX_SUPPORTED_PMD_TYPES 2
/* Indexed via the DPDK assigned cdev_id. */
struct rte_mempool *g_crypto_op_mp[MAX_SUPPORTED_PMD_TYPES] = { NULL, NULL };

/* Max size that we'll send in one crypto op, 32K is limit for AESNI however its recommended
 * that for storage we try to limit the max size to smooth out latency spikes so picking 4K
 * for now.  Further experimentation may change this. Note this is per IOV, not per overall IO.
 */
#define MAX_CRYOP_LENGTH	(1024 * 4)
#define NUM_SESSIONS		1024
#define SESS_MEMPOOL_CACHE_SIZE 128
#define MAX_LIST		1024
#define NUM_MBUFS		(MAX_LIST * 4)
#define POOL_CACHE_SIZE		128
#define AES_CBC_IV_LENGTH	16
#define AES_CBC_KEY_LENGTH	16
#define IV_OFFSET            (sizeof(struct rte_crypto_op) + \
				sizeof(struct rte_crypto_sym_op))

struct crypto_io_channel;
static int crypto_pmd_poller(void *args);
static void _crypto_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
struct rte_cryptodev_config g_dev_cfg;
static int vbdev_crypto_init(void);
static void vbdev_crypto_get_spdk_running_config(FILE *fp);
static int vbdev_crypto_get_ctx_size(void);
static void vbdev_crypto_examine(struct spdk_bdev *bdev);
static void vbdev_crypto_finish(void);
static void vbdev_crypto_init_complete(void);

static struct spdk_bdev_module crypto_if = {
	.name = "crypto",
	.module_init = vbdev_crypto_init,
	.config_text = vbdev_crypto_get_spdk_running_config,
	.get_ctx_size = vbdev_crypto_get_ctx_size,
	.examine = vbdev_crypto_examine,
	.module_fini = vbdev_crypto_finish,
	.init_complete = vbdev_crypto_init_complete
};

SPDK_BDEV_MODULE_REGISTER(&crypto_if)

/* list of crypto_bdev names and their base bdevs via configuration file.
 * Used so we can parse the conf once at init and use this list in examine().
 */
struct bdev_names {
	char			*vbdev_name;	/* name of the vbdev to create */
	char			*bdev_name;	/* base bdev name */
	uint8_t			*key;		/* key per bdev */
	char			*pmd_name;	/* PMD friendly name */
	TAILQ_ENTRY(bdev_names)	link;
};
static TAILQ_HEAD(, bdev_names) g_bdev_names = TAILQ_HEAD_INITIALIZER(g_bdev_names);

/* List of crypto PMDs */
struct vbdev_pmd {
	struct rte_cryptodev_info	cdev_info;	/* includes PMD friendly name */
	uint8_t				cdev_id;	/* used in cryptodev API to ref the PMD */
	uint8_t				socket_id;	/* socket where the PMD is if HW */
	TAILQ_ENTRY(vbdev_pmd)		link;
};
static TAILQ_HEAD(, vbdev_pmd) g_vbdev_pmds = TAILQ_HEAD_INITIALIZER(g_vbdev_pmds);

/* List of virtual bdevs and associated info for each. We keep the PMD friendly name here even
 * though its also in the PMD struct because we use it before we have the PMD struct.
 */
struct vbdev_crypto {
	struct spdk_bdev		*base_bdev;		/* the thing we're attaching to */
	struct spdk_bdev_desc		*base_desc;		/* its descriptor we get from open */
	struct spdk_bdev		crypto_bdev;		/* the crypto virtual bdev */
	uint8_t				*key;			/* key per bdev */
	char				*pmd_name;		/* PMD friendly name */
	struct vbdev_pmd		*pmd;			/* associted PMD for this vbdev */
	struct rte_crypto_sym_xform	cipher_xform;		/* crypto control struct for this vbdev */
	TAILQ_ENTRY(vbdev_crypto)	link;
};
static TAILQ_HEAD(, vbdev_crypto) g_vbdev_crypto = TAILQ_HEAD_INITIALIZER(g_vbdev_crypto);

/* To determine whether we can associate pmd<->vbdev in examine() or not. */
bool g_pmd_setup_complete = false;

/* Shared mempools between all PMDs on this system */
struct spdk_mempool *g_session_mp;	/* session mempool */
struct spdk_mempool *g_mbuf_mp;		/* mbuf mempool */

/* The crypto vbdev channel struct. It is allocated and freed on my behalf by the io channel code.
 * We store things in here that are needed on per thread basis like the base_channel for this thread,
 * and the poller for this thread.
 */
struct crypto_io_channel {
	struct spdk_io_channel		*base_ch;		/* IO channel of base device */
	struct spdk_poller		*poller;		/* completion poller */
	struct vbdev_crypto		*crypto_node;		/* the node associated with this channel */
	int				qp_id;			/* pmd queue pair ID */
};

/* This is the crypto per IO context that the bdev layer allocates for us opaqualy and attaches to
 * each IO for us.
 */
struct crypto_bdev_io {
	int cryop_cnt_remaining;			/* counter used when completing crypto ops */
	struct crypto_io_channel *crypto_ch;		/* need to store for read completions */
	struct vbdev_crypto *crypto_node;		/* the crypto node struct associated with this IO */
	enum rte_crypto_cipher_operation crypto_op;	/* the crypto control struct */
	struct spdk_bdev_io *orig_io;			/* the oringinal IO */

	/* Used for the single contigous buffer that serves as the crypto dest target */
	uint64_t cry_num_blocks;			/* num of blocks for the contiguous buffer */
	uint64_t cry_offset_blocks;			/* block offset on media */
	struct iovec cry_iov;				/* iov representing contig buffer */
};

/* For crypto we can't create a device up front and then easily modify queue pairs later on as
 * threads are allocated by the application so instead we create virtual PMDs at init time.
 * Then here we know that all PMDs are present and ready to be initialized in a way that is
 * optimal for the nuber of lcores on this system and the number of devices that are present.
 * Called when the bdev layer is done with subsystem init meaning that we aren't going to get
 * any more init() examine calls, we can go ahead now and init all of the crypto devices both virtual
 * as well as physical. For subsequent examine calls, via RPC vbdev creation for example, we will
 * associate the vbdev and PMD at that time in examine().
 */
static void
vbdev_crypto_init_complete(void)
{
	uint8_t cdev_count = rte_cryptodev_count();
	uint8_t cdev_id;
	struct vbdev_crypto *crypto_node, *tmp;
	int rc, j;
	char mp_name[RTE_MEMPOOL_NAMESIZE];
	struct vbdev_pmd *pmd;
	uint32_t max_sess_size = 0, sess_size;
	uint16_t num_qps = rte_lcore_count();	/* one QP per lcore per PMD */

	/*
	 * NOTE: If there are any errors setting up any of the PMDs, we'll log an error
	 * and won't register the vbdev associated with the PMD. No failures will be
	 * propgated in other ways though. TODO: good enough?
	 */

	/*
	 * Create global mempools, shared by all PMDs regardless of type.
	 */

	/* First determine max session size, our pool is shared by all the devices */
	for (cdev_id = 0; cdev_id < cdev_count; cdev_id++) {
		sess_size = rte_cryptodev_get_private_session_size(cdev_id);
		if (sess_size > max_sess_size) {
			max_sess_size = sess_size;
		}
	}

	snprintf(mp_name, RTE_MEMPOOL_NAMESIZE, "session_mp");
	g_session_mp = spdk_mempool_create(mp_name, NUM_SESSIONS, max_sess_size,
					   SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
					   SPDK_ENV_SOCKET_ID_ANY);
	if (g_session_mp == NULL) {
		SPDK_ERRLOG("Cannot create session pool max size 0x%x\n", max_sess_size);
		return;
	}

	snprintf(mp_name, RTE_MEMPOOL_NAMESIZE, "mbuf_mp");
	g_mbuf_mp = spdk_mempool_create(mp_name, NUM_MBUFS, sizeof(struct rte_mbuf),
					SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
					SPDK_ENV_SOCKET_ID_ANY);
	if (g_mbuf_mp == NULL) {
		SPDK_ERRLOG("Cannot create mbuf pool\n");
		spdk_mempool_free(g_session_mp);
		return;
	}

	/*
	 * Now lets configure each PMD.
	 */
	for (cdev_id = 0; cdev_id < cdev_count; cdev_id++) {
		pmd = calloc(1, sizeof(struct vbdev_pmd));
		if (!pmd) {
			SPDK_ERRLOG("could not allocate bdev_names\n");
			spdk_mempool_free(g_session_mp);
			spdk_mempool_free(g_mbuf_mp);
			return;
		}

		/* Get details about this PMD. */
		rte_cryptodev_info_get(cdev_id, &pmd->cdev_info);
		pmd->socket_id = rte_cryptodev_socket_id(pmd->cdev_info.driver_id);

		snprintf(mp_name, RTE_MEMPOOL_NAMESIZE, "op_mp");

		/* crypto_op_private_data is specific to PMD type. */
		if (strcmp(pmd->cdev_info.driver_name, AESNI_MB) == 0) {
			g_crypto_op_mp[cdev_id] = rte_crypto_op_pool_create(mp_name,
						  RTE_CRYPTO_OP_TYPE_SYMMETRIC,
						  NUM_MBUFS,
						  POOL_CACHE_SIZE,
						  AES_CBC_IV_LENGTH,
						  0);

			if (g_crypto_op_mp[cdev_id] == NULL) {
				SPDK_ERRLOG("Cannot create crypto_op_pool\n");
				spdk_mempool_free(g_session_mp);
				spdk_mempool_free(g_mbuf_mp);
				return;
			}
		} else {
			SPDK_ERRLOG("Invalid PMD driver.\n");
			spdk_mempool_free(g_session_mp);
			spdk_mempool_free(g_mbuf_mp);
		}

		/* Check max qpairs for this cdev vs what we figured we needed above. */
		if (num_qps > pmd->cdev_info.max_nb_queue_pairs) {
			SPDK_ERRLOG("Device doesn't support required number of queue pairs.\n");
			spdk_mempool_free(g_session_mp);
			spdk_mempool_free(g_mbuf_mp);
			return;
		}

		/* Setup queue pairs, just the total here.  Each QP wil be setup
		 * per socket in the create channel callback.
		 */
		struct rte_cryptodev_config conf = {
			.nb_queue_pairs = num_qps,
			.socket_id = pmd->socket_id
		};

		rc = rte_cryptodev_configure(cdev_id, &conf);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to configure cryptodev %u", cdev_id);
			spdk_mempool_free(g_session_mp);
			spdk_mempool_free(g_mbuf_mp);
			return;
		}

		struct rte_cryptodev_qp_conf qp_conf = {
			.nb_descriptors = CRYPTO_QP_DESCRIPTORS
		};

		/* Pre-setup all pottential qpairs now and assign them in the channel
		 * callback. If we were to create them there, we'd have to stop the
		 * entire PMD affecting all other threads that might be using it
		 * even on other queue pairs.
		 */
		for (j = 0; j < num_qps; j++) {
			rc = rte_cryptodev_queue_pair_setup(cdev_id, j, &qp_conf, SOCKET_ID_ANY,
							    (struct rte_mempool *)g_session_mp);

			if (rc < 0) {
				SPDK_ERRLOG("Failed to setup queue pair %u on "
					    "cryptodev %u", j, pmd->cdev_info.driver_id);
				spdk_mempool_free(g_session_mp);
				spdk_mempool_free(g_mbuf_mp);
				return;
			}
		}

		rc = rte_cryptodev_start(cdev_id);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to start device %u: error %d\n",
				    cdev_id, rc);
			spdk_mempool_free(g_session_mp);
			spdk_mempool_free(g_mbuf_mp);
			return;
		}

		/* Add to our list of PMDs. */
		TAILQ_INSERT_TAIL(&g_vbdev_pmds, pmd, link);
	}

	/*
	 * Now that our crypto_devices are ready, assocaite a pmd node with
	 * each vbdev and register our vbdevs. We do this here as opposed to
	 * the xamine callback because it will in turn generate another examine
	 * callback which we wouldn't be able to address until now.
	 */
	TAILQ_FOREACH_SAFE(crypto_node, &g_vbdev_crypto, link, tmp) {
		TAILQ_FOREACH(pmd, &g_vbdev_pmds, link) {
			if (strcmp(crypto_node->pmd_name, pmd->cdev_info.driver_name) == 0) {
				crypto_node->pmd = pmd;
				break;
			}
		}
		if (!crypto_node->pmd) {
			SPDK_ERRLOG("failed to associate crypto_bdev\n");
			spdk_bdev_close(crypto_node->base_desc);
			TAILQ_REMOVE(&g_vbdev_crypto, crypto_node, link);
			free(crypto_node->crypto_bdev.name);
			free(crypto_node->key);
			free(crypto_node);
			break;
		}

		rc = spdk_vbdev_register(&crypto_node->crypto_bdev,
					 &crypto_node->base_bdev, 1);
		if (rc) {
			SPDK_ERRLOG("could not register crypto_bdev\n");
			spdk_bdev_close(crypto_node->base_desc);
			TAILQ_REMOVE(&g_vbdev_crypto, crypto_node, link);
			free(crypto_node->crypto_bdev.name);
			free(crypto_node->key);
			free(crypto_node);
			break;
		}
	}

	/* Flag used in examine() to decide whether we need to associate a
	 * new vbdev at that time or not.
	 */
	g_pmd_setup_complete = true;
}

/* Following an ecnrypt or decrypt we need to then either write the encrypted data or finish
 * the read on decrypted data. Do that here.
 */
static void
_crypto_operation_complete(struct crypto_io_channel *crypto_ch, struct spdk_bdev_io *bdev_io,
			   enum rte_crypto_cipher_operation crypto_op)
{
	struct vbdev_crypto *crypto_node = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_crypto,
					   crypto_bdev);
	struct crypto_bdev_io *io_ctx = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	int rc = 0;

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {

		/* Complete the original IO and then free the one that we created
		 * as a result of issuing an IO via submit_reqeust.
		 */
		spdk_bdev_io_complete(io_ctx->orig_io, bdev_io->status);
		spdk_bdev_free_io(bdev_io);
	} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {

		/* Write using our single contiguous encrypted buffer */
		rc = spdk_bdev_writev_blocks(crypto_node->base_desc, crypto_ch->base_ch,
					     &io_ctx->cry_iov, 1, io_ctx->cry_offset_blocks,
					     io_ctx->cry_num_blocks, _crypto_complete_io,
					     bdev_io);
	}

	if (rc != 0) {
		SPDK_ERRLOG("ERROR on crypto completion!\n");
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/* This is the poller for PMDs. It uses a single API to dequeue whatever is ready at
 * the PMD. Then we need to decide if what we've got so far (including previous poller
 * runs) totals up to one or more complete bdev_ios and if so continue with the bdev_io
 * accordingly. This means either completing a read or issuing a new write.
 */
static int
crypto_pmd_poller(void *args)
{
	struct crypto_io_channel *crypto_ch = args;
	uint8_t cdev_id = crypto_ch->crypto_node->pmd->cdev_info.driver_id;
	int i, num_dequeued_ops;
	struct spdk_bdev_io *bdev_io = NULL;
	struct crypto_bdev_io *io_ctx = NULL;
	struct rte_crypto_op *dequeued_ops[NUM_MBUFS];

	/* Each run of the poller will get just what the PMD has available
	 * at the moment we call it, we don't check again after draining the
	 * first batch.
	 */
	num_dequeued_ops = rte_cryptodev_dequeue_burst(cdev_id, crypto_ch->qp_id,
			   dequeued_ops, NUM_MBUFS);

	/* Check if operation was processed successfully */
	for (i = 0; i < num_dequeued_ops; i++) {

		/* I don't know the order or association of the crypto ops wrt any
		 * partiular bdev_io so need to look at each and determine if it's
		 * the last one for it's bdev_io or not.
		 */
		bdev_io = (struct spdk_bdev_io *)dequeued_ops[i]->sym->m_src->userdata;
		assert(bdev_io != NULL);

		if (dequeued_ops[i]->status != RTE_CRYPTO_OP_STATUS_SUCCESS) {

			SPDK_ERRLOG("error with op %d\n", i);
			/* Update the bdev status to error, we'll still process the
			 * rest of the crypto ops for this bdev_io though so they
			 * aren't left hanging.
			 */
			bdev_io->status = SPDK_BDEV_IO_STATUS_FAILED;
		}

		io_ctx = (struct crypto_bdev_io *)bdev_io->driver_ctx;
		assert(io_ctx->cryop_cnt_remaining > 0);

		/* return the assicated mbufs */
		spdk_mempool_put(g_mbuf_mp, dequeued_ops[i]->sym->m_src);

		/* For encryption, free the mbuf we used to encrypt, the data buffer
		 * will be freed on write completion.
		 */
		if (dequeued_ops[i]->sym->m_dst) {
			spdk_mempool_put(g_mbuf_mp, dequeued_ops[i]->sym->m_dst);
		}

		/* done encrypting complete bdev_io */
		if (--io_ctx->cryop_cnt_remaining == 0) {

			/* do the bdev_io operation */
			_crypto_operation_complete(crypto_ch, bdev_io, io_ctx->crypto_op);

			/* return session */
			rte_cryptodev_sym_session_clear(cdev_id,
							dequeued_ops[i]->sym->session);
			rte_cryptodev_sym_session_free(dequeued_ops[i]->sym->session);
		}
	}

	if (num_dequeued_ops > 0) {

		/* return all crypto ops at once since we dqueued this batch */
		rte_mempool_put_bulk(g_crypto_op_mp[cdev_id],
				     (void **)dequeued_ops,
				     num_dequeued_ops);
	}
	return num_dequeued_ops;
}

/* We're either encrypting on the way down or decrypting on the way back. */
static int
_crypto_operation(struct spdk_bdev_io *bdev_io, enum rte_crypto_cipher_operation crypto_op)
{
	struct rte_cryptodev_sym_session *session;
	struct rte_crypto_op *crypto_ops[NUM_MBUFS];
	struct rte_mbuf *mbufs[NUM_MBUFS];
	struct rte_mbuf *en_mbufs[NUM_MBUFS];
	uint16_t num_enqueued_ops = 0;
	int iov_cnt = bdev_io->u.bdev.iovcnt;
	int cryop_cnt = 1;
	struct crypto_bdev_io *io_ctx = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	uint8_t cdev_id = io_ctx->crypto_node->pmd->cdev_info.driver_id;
	struct crypto_io_channel *crypto_ch = io_ctx->crypto_ch;
	uint64_t total_length =
		bdev_io->u.bdev.num_blocks * io_ctx->crypto_node->crypto_bdev.blocklen;
	int rc, enqueued = 0;
	int i, remaining, cry_index = 0;
	uint32_t offset = 0;
	uint32_t en_offset = 0;

	/* NOTE: for reads, the bdev_io passed in the the one we created, for writes
	 * it's the origninal IO. Either way though, the io_ctx will be valid for what
	 * each respective operation requires.
	 */

	/* The number of cry operations we need depends on the total size of the IO
	 * and the max data we can process in a single op. We choose the larger of that
	 * value or the iovec count.
	 */
	if (total_length > MAX_CRYOP_LENGTH) {
		cryop_cnt = total_length / MAX_CRYOP_LENGTH + (total_length % MAX_CRYOP_LENGTH > 0);
		cryop_cnt = spdk_max(cryop_cnt, iov_cnt);
	}

	/* Get the number of crypto ops and mbufs that we need to start with. */
	rc = spdk_mempool_get_bulk(g_mbuf_mp, (void **)&mbufs[0], cryop_cnt);
	if (rc) {
		SPDK_ERRLOG("ERROR trying to get mbufs!\n");
		return -ENOMEM;
	}

	/* Get the same amount but these buffers decribe the encrypted data location. */
	if (crypto_op == RTE_CRYPTO_CIPHER_OP_ENCRYPT) {
		rc = spdk_mempool_get_bulk(g_mbuf_mp, (void **)&en_mbufs[0], cryop_cnt);
		if (rc) {
			SPDK_ERRLOG("ERROR trying to get mbufs!\n");
			spdk_mempool_put_bulk(g_mbuf_mp, (void **)(void **)&mbufs[0],
					      cryop_cnt);
			return -ENOMEM;
		}
	}

	rc = rte_crypto_op_bulk_alloc(g_crypto_op_mp[cdev_id],
				      RTE_CRYPTO_OP_TYPE_SYMMETRIC,
				      crypto_ops, cryop_cnt);
	if (rc < cryop_cnt) {
		spdk_mempool_put_bulk(g_mbuf_mp, (void **)(void **)&mbufs[0],
				      cryop_cnt);
		spdk_mempool_put_bulk(g_mbuf_mp, (void **)(void **)&en_mbufs[0],
				      cryop_cnt);
		if (rc > 0) {
			rte_mempool_put_bulk(g_crypto_op_mp[0], (void **)crypto_ops,
					     rc);
		}
		SPDK_ERRLOG("ERROR trying to get crypto ops!\n");
		return -ENOMEM;
	}

	/* We will decrement this counter in the poller to determine when this bdev_io is done. */
	io_ctx->cryop_cnt_remaining = cryop_cnt;

	session = rte_cryptodev_sym_session_create((struct rte_mempool *)g_session_mp);
	if (NULL == session) {
		spdk_mempool_put_bulk(g_mbuf_mp, (void **)(void **)&mbufs[0],
				      cryop_cnt);
		spdk_mempool_put_bulk(g_mbuf_mp, (void **)(void **)&en_mbufs[0],
				      cryop_cnt);
		rte_mempool_put_bulk(g_crypto_op_mp[0], (void **)crypto_ops,
				     cryop_cnt);
		SPDK_ERRLOG("ERROR trying to create crypto session!\n");
		return -ENOMEM;
	}

	/* Init our session with the desired cipher options. */
	io_ctx->crypto_node->cipher_xform.cipher.key.data = io_ctx->crypto_node->key;
	io_ctx->crypto_node->cipher_xform.cipher.op = io_ctx->crypto_op = crypto_op;

	rc = rte_cryptodev_sym_session_init(cdev_id, session,
					    &io_ctx->crypto_node->cipher_xform,
					    (struct rte_mempool *)g_session_mp);
	if (rc < 0) {
		spdk_mempool_put_bulk(g_mbuf_mp, (void **)(void **)&mbufs[0],
				      cryop_cnt);
		spdk_mempool_put_bulk(g_mbuf_mp, (void **)(void **)&en_mbufs[0],
				      cryop_cnt);
		rte_mempool_put_bulk(g_crypto_op_mp[cdev_id], (void **)crypto_ops,
				     cryop_cnt);
		rte_cryptodev_sym_session_clear(cdev_id, session);
		rte_cryptodev_sym_session_free(session);
		SPDK_ERRLOG("ERROR trying to init crypto session!\n");
		return rc;
	}

	/* For encryption, we need to prepare a single contiguous buffer as the encryption
	 * destination, we'll then pass that along for the write after encryption is done.
	 */
	if (crypto_op == RTE_CRYPTO_CIPHER_OP_ENCRYPT) {
		io_ctx->cry_iov.iov_len = total_length;
		io_ctx->cry_iov.iov_base = spdk_dma_malloc(total_length, 0x1000, NULL);
		io_ctx->cry_offset_blocks = bdev_io->u.bdev.offset_blocks;
		io_ctx->cry_num_blocks = bdev_io->u.bdev.num_blocks;
	}

	/* Walk through bdev iovs and build up one or more mbufs for each iov */
	for (i = 0; i < iov_cnt; i++) {

		/* Build as many mbufs as we need per iovec taking into account the
		 * max data we can put in one crypto operation.
		 */
		remaining = bdev_io->u.bdev.iovs[i].iov_len;
		offset = 0;
		do {
			struct rte_crypto_op *op = crypto_ops[cry_index];

			/* Point the mbuf data_addr to the bdev io vector, this is the only element
			 * in the mbuf structure that we use other than IO context. Length is kept in
			 * the crypto op.
			 */
			mbufs[cry_index]->buf_addr = bdev_io->u.bdev.iovs[i].iov_base + offset;

			/* Set the data to encrypt/decrypt length */
			crypto_ops[cry_index]->sym->cipher.data.length = spdk_min(remaining, MAX_CRYOP_LENGTH);
			remaining -= crypto_ops[cry_index]->sym->cipher.data.length;
			assert(remaining >= 0);

			offset += crypto_ops[cry_index]->sym->cipher.data.length;
			crypto_ops[cry_index]->sym->cipher.data.offset = 0;

			/* Store context in every mbuf as we don't know aything about completion order */
			mbufs[cry_index]->userdata = bdev_io;

			/* link the mbuf to the crypto op for source. */
			crypto_ops[cry_index]->sym->m_src = mbufs[cry_index];

			/* For encrypt, point the dest to a buffer we allocate and redirect the bdev_io
			 * that will be used the process the write on completion to the same buffer.
			 */
			if (crypto_op == RTE_CRYPTO_CIPHER_OP_ENCRYPT) {

				/* Set dest crypto mbuf to offset within contig buffer for write
				 * and increment for the next iov keeping our running offet in the io_ctx.
				 */
				en_mbufs[cry_index]->buf_addr = io_ctx->cry_iov.iov_base + en_offset;
				crypto_ops[cry_index]->sym->m_dst = en_mbufs[cry_index];
				en_offset += crypto_ops[cry_index]->sym->cipher.data.length;
			}

			/* Set the IV - we use the vbdev name as its unique per bdev */
			uint8_t *iv_ptr = rte_crypto_op_ctod_offset(op, uint8_t *, IV_OFFSET);
			memcpy(iv_ptr, io_ctx->crypto_node->crypto_bdev.name,
			       spdk_min(AES_CBC_IV_LENGTH, strlen(io_ctx->crypto_node->crypto_bdev.name)));

			/* Attach the crypto session to the operation */
			rc = rte_crypto_op_attach_sym_session(op, session);
			if (rc) {
				SPDK_ERRLOG("ERROR trying to attach to crypto session!\n");
				spdk_mempool_put_bulk(g_mbuf_mp, (void **)(void **)&mbufs[0],
						      cryop_cnt);
				spdk_mempool_put_bulk(g_mbuf_mp, (void **)(void **)&en_mbufs[0],
						      cryop_cnt);
				rte_mempool_put_bulk(g_crypto_op_mp[cdev_id], (void **)crypto_ops,
						     cryop_cnt);
				rte_cryptodev_sym_session_clear(cdev_id, session);
				rte_cryptodev_sym_session_free(session);
				return rc;
			}

			/* Increment index into crypto arrays, operations and mbufs. */
			cry_index++;
		} while (remaining > 0);
	}

	/* Enqueue everything we've got. */
	do {
		num_enqueued_ops += rte_cryptodev_enqueue_burst(cdev_id, crypto_ch->qp_id,
				    &crypto_ops[enqueued], cryop_cnt - enqueued);

		/* Dequeue all inline if the PMD is full. We don't defer anything simply
		 * because of the complexity involved as we're building 1 or more crypto
		 * ops per IOV. Dequeue will free up space for more enqueue.
		 */
		if (num_enqueued_ops < cryop_cnt) {
			int completed = 0;
			/* Dequeue everything we just enqueued right now */
			do {
				completed += crypto_pmd_poller(crypto_ch);
			} while (completed < num_enqueued_ops);
			enqueued += num_enqueued_ops;
		}
	} while (num_enqueued_ops < cryop_cnt);

	assert(num_enqueued_ops == cryop_cnt);
	assert(cryop_cnt == cry_index);

	return rc;
}

/* Completion callback for IO that were issued from this bdev. The original bdev_io
 * is passed in as an arg so we'll complete that one with the appropriate status
 * and then free the one that this module issued.
 */
static void
_crypto_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	struct crypto_bdev_io *io_ctx = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	struct crypto_bdev_io *orig_ctx = (struct crypto_bdev_io *)orig_io->driver_ctx;
	int rc = 0;

	/* check and see if this needs to be decrypted or just completed */
	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {

		/* Copy relevant context fields from the original IO so they are in the io_ctx
		 * available in the generic function for both encryption/decryption.
		 */
		io_ctx->orig_io = orig_ctx->orig_io;
		io_ctx->crypto_ch = orig_ctx->crypto_ch;
		io_ctx->crypto_node = orig_ctx->crypto_node;

		rc = _crypto_operation(bdev_io, RTE_CRYPTO_CIPHER_OP_DECRYPT);
		if (rc) {
			SPDK_ERRLOG("ERROR decrypting");
			bdev_io->status = SPDK_BDEV_IO_STATUS_FAILED;
			spdk_bdev_io_complete(orig_io, status);
			spdk_bdev_free_io(bdev_io);
		}
	} else {

		if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {

			/* Free the buffer we allocated for the ecrypted data */
			spdk_dma_free(orig_ctx->cry_iov.iov_base);
		}

		/* Complete the original IO and then free the one that we created here
		 * as a result of issuing an IO via submit_reqeust.
		 */
		spdk_bdev_io_complete(orig_io, status);
		spdk_bdev_free_io(bdev_io);
	}
}

/* Called when someone above submits IO to this crypto vbdev. For IO's not relevant to crypto,
 * we're simply passing it on here via SPDK IO calls which in turn allocate another bdev IO
 * and call our cpl callback provided below along with the original bdev_io so that we can
 * complete it once this IO completes. For crypto operations, we'll either encrypt it first
 * (writes) then call back into bdev to submit it or we'll submit a read and then catch it
 * on the way back for decryption.
 */
static void
vbdev_crypto_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_crypto *crypto_node = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_crypto,
					   crypto_bdev);
	struct crypto_io_channel *crypto_ch = spdk_io_channel_get_ctx(ch);
	struct crypto_bdev_io *io_ctx = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	int rc = 1;

	memset(io_ctx, 0, sizeof(struct crypto_bdev_io));
	io_ctx->crypto_node = crypto_node;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:

		// TODO: when the iov incoming is NULL we need to allocate our own buffer
		// and decrypt there as dest, then update th IOC.  However, who frees it
		// and when??
		io_ctx->crypto_ch = crypto_ch;
		io_ctx->orig_io = bdev_io;
		rc = spdk_bdev_readv_blocks(crypto_node->base_desc, crypto_ch->base_ch, bdev_io->u.bdev.iovs,
					    bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks, _crypto_complete_io,
					    bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		io_ctx->crypto_ch = crypto_ch;
		rc = _crypto_operation(bdev_io, RTE_CRYPTO_CIPHER_OP_ENCRYPT);

		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		raise(SIGINT);
		rc = spdk_bdev_unmap_blocks(crypto_node->base_desc, crypto_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _crypto_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = spdk_bdev_flush_blocks(crypto_node->base_desc, crypto_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _crypto_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		rc = spdk_bdev_reset(crypto_node->base_desc, crypto_ch->base_ch,
				     _crypto_complete_io, bdev_io);
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
	struct vbdev_crypto *crypto_node = (struct vbdev_crypto *)ctx;

	/* Force the bdev layer to issue actual writes of zeroes so we can
	 * encrypt them as regular writes.
	 */
	if (io_type == SPDK_BDEV_IO_TYPE_WRITE_ZEROES) {
		return false;
	}
	return spdk_bdev_io_type_supported(crypto_node->base_bdev, io_type);
}

/* Called after we've unregistered following a hot remove callback.
 * Our finish entry point will be called next.
 */
static int
vbdev_crypto_destruct(void *ctx)
{
	struct vbdev_crypto *crypto_node = (struct vbdev_crypto *)ctx;

	/* Unclaim the underlying bdev. */
	spdk_bdev_module_release_bdev(crypto_node->base_bdev);

	/* Close the underlying bdev. */
	spdk_bdev_close(crypto_node->base_desc);

	/* Done with this crypto_node. */
	TAILQ_REMOVE(&g_vbdev_crypto, crypto_node, link);
	free(crypto_node->pmd_name);
	free(crypto_node->key);
	free(crypto_node->crypto_bdev.name);
	free(crypto_node);
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
	struct vbdev_crypto *crypto_node = (struct vbdev_crypto *)ctx;
	struct spdk_io_channel *io_ch;

	/* The IO channel code will allocate a channel for us which consists of
	 * the SPDK cahnnel structure plus the size of our crypto_io_channel struct
	 * that we passed in when we registered our IO device. It will then call
	 * our channel create callback to populate any elements that we need to
	 * update.
	 */
	io_ch = spdk_get_io_channel(crypto_node);

	return io_ch;
}

static int
vbdev_crypto_info_config_json(void *ctx, struct spdk_json_write_ctx *write_ctx)
{
	struct vbdev_crypto *crypto_node = (struct vbdev_crypto *)ctx;

	/* This is the output for get_bdevs() for this vbdev */
	spdk_json_write_name(write_ctx, "crypto");
	spdk_json_write_object_begin(write_ctx);

	spdk_json_write_name(write_ctx, "crypto_bdev_name");
	spdk_json_write_string(write_ctx, spdk_bdev_get_name(&crypto_node->crypto_bdev));

	spdk_json_write_name(write_ctx, "base_bdev_name");
	spdk_json_write_string(write_ctx, spdk_bdev_get_name(crypto_node->base_bdev));

	spdk_json_write_object_end(write_ctx);

	return 0;
}

/* We provide this callback for the SPDK channel code to create a channel using
 * the channel struct we provided in our module get_io_channel() entry point. Here
 * we get and save off an underlying base channel of the device below us so that
 * we can communicate with the base bdev on a per channel basis. We also register the
 * poller used to complete crypto operations from the PMD.
 */
static int
crypto_bdev_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct crypto_io_channel *crypto_ch = ctx_buf;
	struct vbdev_crypto *crypto_node = io_device;

	crypto_ch->qp_id = spdk_env_get_current_core();
	SPDK_NOTICELOG("Channel %p setup to use QPID: %u\n", crypto_ch, crypto_ch->qp_id);
	crypto_ch->base_ch = spdk_bdev_get_io_channel(crypto_node->base_desc);
	crypto_ch->crypto_node = crypto_node;
	crypto_ch->poller = spdk_poller_register(crypto_pmd_poller, crypto_ch, 0);

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

	SPDK_NOTICELOG("Channel %p\n", crypto_ch);
	spdk_poller_unregister(&crypto_ch->poller);
	spdk_put_io_channel(crypto_ch->base_ch);
}

/* On init, just parse config file and build list of crypto vbdevs and bdev name pairs. */
static int
vbdev_crypto_init(void)
{
	struct spdk_conf_section *sp = NULL;
	const char *conf_bdev_name = NULL;
	const char *conf_vbdev_name = NULL;
	const char *key = NULL;
	const char *pmd = NULL;
	struct bdev_names *name;
	int i;

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

		pmd = spdk_conf_section_get_nmval(sp, "CRY", i, 3);
		if (!pmd) {
			SPDK_ERRLOG("crypto configuration missing PMD type\n");
			return -EINVAL;
		}
		if (strcmp(pmd, AESNI_MB) != 0 && strcmp(pmd, QAT) != 0) {
			SPDK_ERRLOG("crypto configuration invalid PMD type\n");
			return -EINVAL;
		}

		name = calloc(1, sizeof(struct bdev_names));
		if (!name) {
			SPDK_ERRLOG("could not allocate bdev_names\n");
			return -ENOMEM;
		}

		name->bdev_name = strdup(conf_bdev_name);
		if (!name->bdev_name) {
			SPDK_ERRLOG("could not allocate name->bdev_name\n");
			free(name);
			return -ENOMEM;
		}

		name->vbdev_name = strdup(conf_vbdev_name);
		if (!name->vbdev_name) {
			SPDK_ERRLOG("could not allocate name->vbdev_name\n");
			free(name->bdev_name);
			free(name);
			return -ENOMEM;
		}

		/* TODO: remove key from config file and replace with RPC or
		 * something, this is convenient for testing/debug though.
		 */
		name->key = strdup(key);
		if (strlen(name->key) != AES_CBC_KEY_LENGTH) {
			SPDK_ERRLOG("invalid key length\n");
			free(name->vbdev_name);
			free(name->bdev_name);
			free(name);
			return -EINVAL;
		}
		if (!name->key) {
			SPDK_ERRLOG("could not allocate name->key\n");
			free(name->vbdev_name);
			free(name->bdev_name);
			free(name);
			return -ENOMEM;
		}

		name->pmd_name = strdup(pmd);
		if (!name->pmd_name) {
			SPDK_ERRLOG("could not allocate name->pmd_name\n");
			free(name->key);
			free(name->vbdev_name);
			free(name->bdev_name);
			free(name);
			return -ENOMEM;
		}
		TAILQ_INSERT_TAIL(&g_bdev_names, name, link);
	}
	TAILQ_FOREACH(name, &g_bdev_names, link) {
		SPDK_NOTICELOG("conf parse matched: %s\n", name->bdev_name);
	}

	return 0;
}

/* Called when the entire module is being torn down. */
static void
vbdev_crypto_finish(void)
{
	struct bdev_names *name;
	struct vbdev_pmd *pmd;

	while ((name = TAILQ_FIRST(&g_bdev_names))) {
		TAILQ_REMOVE(&g_bdev_names, name, link);
		free(name->pmd_name);
		free(name->key);
		free(name->bdev_name);
		free(name->vbdev_name);
		free(name);
	}

	while ((pmd = TAILQ_FIRST(&g_vbdev_pmds))) {
		rte_cryptodev_stop(pmd->cdev_info.driver_id);
		TAILQ_REMOVE(&g_vbdev_pmds, pmd, link);
		free(pmd);
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
	struct vbdev_crypto *crypto_node, *tmp;
	struct spdk_bdev *bdev_find = ctx;

	TAILQ_FOREACH_SAFE(crypto_node, &g_vbdev_crypto, link, tmp) {
		if (bdev_find == crypto_node->base_bdev) {
			spdk_bdev_unregister(&crypto_node->crypto_bdev, NULL, NULL);
		}
	}
}

/* When we regsiter our bdev this is how we specify our entry points. */
static const struct spdk_bdev_fn_table vbdev_crypto_fn_table = {
	.destruct		= vbdev_crypto_destruct,
	.submit_request		= vbdev_crypto_submit_request,
	.io_type_supported	= vbdev_crypto_io_type_supported,
	.get_io_channel		= vbdev_crypto_get_io_channel,
	.dump_info_json		= vbdev_crypto_info_config_json,
};

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
	struct bdev_names *name;
	struct vbdev_crypto *crypto_node;
	int rc;

	/* Check our list of names from config versus this bdev and if
	 * there's a match, create the crypto_node & bdev accordingly.
	 */
	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(name->bdev_name, bdev->name) != 0) {
			continue;
		}

		SPDK_NOTICELOG("Match on %s\n", bdev->name);
		crypto_node = calloc(1, sizeof(struct vbdev_crypto));
		if (!crypto_node) {
			SPDK_ERRLOG("could not allocate crypto_node\n");
			break;
		}

		/* The base bdev that we're attaching to. */
		crypto_node->base_bdev = bdev;
		crypto_node->crypto_bdev.name = strdup(name->vbdev_name);
		if (!crypto_node->crypto_bdev.name) {
			SPDK_ERRLOG("could not allocate crypto_bdev name\n");
			free(crypto_node);
			break;
		}

		crypto_node->key = strdup(name->key);
		if (!crypto_node->key) {
			SPDK_ERRLOG("could not allocate crypto_bdev key\n");
			free(crypto_node->crypto_bdev.name);
			free(crypto_node);
			break;
		}

		/* Supported PMD names are listed in the example .conf file */
		crypto_node->pmd_name = strdup(name->pmd_name);
		if (!crypto_node->pmd_name) {
			SPDK_ERRLOG("could not allocate crypto_bdev pmd\n");
			free(crypto_node->crypto_bdev.name);
			free(crypto_node->key);
			free(crypto_node);
			break;
		}

		crypto_node->crypto_bdev.product_name = "crypto";
		crypto_node->crypto_bdev.write_cache = bdev->write_cache;
		crypto_node->crypto_bdev.need_aligned_buffer = bdev->need_aligned_buffer;
		crypto_node->crypto_bdev.optimal_io_boundary = bdev->optimal_io_boundary;
		crypto_node->crypto_bdev.blocklen = bdev->blocklen;
		crypto_node->crypto_bdev.blockcnt = bdev->blockcnt;

		/* opcode and key are set per operation */
		memset(&crypto_node->cipher_xform, 0, sizeof(struct rte_crypto_sym_xform));
		crypto_node->cipher_xform.type = RTE_CRYPTO_SYM_XFORM_CIPHER;
		crypto_node->cipher_xform.cipher.algo = RTE_CRYPTO_CIPHER_AES_CBC;
		crypto_node->cipher_xform.cipher.key.length = AES_CBC_KEY_LENGTH;
		crypto_node->cipher_xform.cipher.iv.offset = IV_OFFSET;
		crypto_node->cipher_xform.cipher.iv.length = AES_CBC_IV_LENGTH;

		/* This is the context that is passed to us when the bdev
		 * layer calls in so we'll save our crypto_bdev node here.
		 */
		crypto_node->crypto_bdev.ctxt = crypto_node;
		crypto_node->crypto_bdev.fn_table = &vbdev_crypto_fn_table;
		crypto_node->crypto_bdev.module = &crypto_if;
		TAILQ_INSERT_TAIL(&g_vbdev_crypto, crypto_node, link);

		spdk_io_device_register(crypto_node, crypto_bdev_ch_create_cb, crypto_bdev_ch_destroy_cb,
					sizeof(struct crypto_io_channel));
		SPDK_NOTICELOG("io_device created at: 0x%p\n", crypto_node);

		rc = spdk_bdev_open(bdev, true, vbdev_crypto_examine_hotremove_cb,
				    bdev, &crypto_node->base_desc);
		if (rc) {
			SPDK_ERRLOG("could not open bdev %s\n", spdk_bdev_get_name(bdev));
			TAILQ_REMOVE(&g_vbdev_crypto, crypto_node, link);
			free(crypto_node->crypto_bdev.name);
			free(crypto_node->key);
			free(crypto_node);
			break;
		}
		SPDK_NOTICELOG("bdev opened\n");

		rc = spdk_bdev_module_claim_bdev(bdev, crypto_node->base_desc, crypto_node->crypto_bdev.module);
		if (rc) {
			SPDK_ERRLOG("could not claim bdev %s\n", spdk_bdev_get_name(bdev));
			spdk_bdev_close(crypto_node->base_desc);
			TAILQ_REMOVE(&g_vbdev_crypto, crypto_node, link);
			free(crypto_node->crypto_bdev.name);
			free(crypto_node->key);
			free(crypto_node);
			break;
		}
		SPDK_NOTICELOG("bdev claimed\n");

		/* Create any supported virtual crypto PMDs, we don't need to
		 * do anything with HW PMDs at this point. All PMDs will get
		 * initialized after the bdev subsystem init is complete.
		 */
		if (g_pmd_setup_complete == false) {
			if (strcmp(crypto_node->pmd_name, AESNI_MB) == 0) {
				rc = rte_vdev_init(AESNI_MB, NULL);
				/* There is no performance benefit to creating >1 virtual
				 * PMD, so just ignore any failures in creating duplicate
				 * ones...
				 */
				if (rc && rc != -EEXIST) {
					SPDK_ERRLOG("error on rte_vdev_init\n");
					spdk_bdev_close(crypto_node->base_desc);
					TAILQ_REMOVE(&g_vbdev_crypto, crypto_node, link);
					free(crypto_node->crypto_bdev.name);
					free(crypto_node->key);
					free(crypto_node);
					break;
				} else {
					SPDK_NOTICELOG("created virtual PMD %s\n", crypto_node->pmd_name);
				}
			}
		} else {
			// TODO: RPC created vbdev needs ot be registered and associated here.
		}

		SPDK_NOTICELOG("crypto_bdev registered\n");
		SPDK_NOTICELOG("created crypto_bdev for: %s\n", name->vbdev_name);
	}
	spdk_bdev_module_examine_done(&crypto_if);
}

SPDK_LOG_REGISTER_COMPONENT("vbdev_crypto", SPDK_LOG_VBDEV_crypto)
