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

struct crypto_io_channel;
static int crypto_pmd_poller(void *args);
static void _crypto_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
struct rte_cryptodev_config g_dev_cfg;
static int vbdev_crypto_init(void);
static void vbdev_crypto_get_spdk_running_config(FILE *fp);
static int vbdev_crypto_get_ctx_size(void);
static void vbdev_crypto_examine(struct spdk_bdev *bdev);
static void vbdev_crypto_finish(void);

static struct spdk_bdev_module crypto_if = {
	.name = "crypto",
	.module_init = vbdev_crypto_init,
	.config_text = vbdev_crypto_get_spdk_running_config,
	.get_ctx_size = vbdev_crypto_get_ctx_size,
	.examine = vbdev_crypto_examine,
	.module_fini = vbdev_crypto_finish
};

SPDK_BDEV_MODULE_REGISTER(&crypto_if)

/* list of crypto_bdev names and their base bdevs via configuration file.
 * Used so we can parse the conf once at init and use this list in examine().
 */
struct bdev_names {
	char			*vbdev_name;
	char			*bdev_name;
	uint8_t			*key;
	TAILQ_ENTRY(bdev_names)	link;
};
static TAILQ_HEAD(, bdev_names) g_bdev_names = TAILQ_HEAD_INITIALIZER(g_bdev_names);

/* List of virtual bdevs and associated info for each. */
struct vbdev_crypto {
	struct spdk_bdev		*base_bdev;		/* the thing we're attaching to */
	struct spdk_bdev_desc		*base_desc;		/* its descriptor we get from open */
	struct spdk_bdev		crypto_bdev;		/* the crypto virtual bdev */
	uint8_t				*key;			/* key per bdev */
	TAILQ_ENTRY(vbdev_crypto)	link;
};
static TAILQ_HEAD(, vbdev_crypto) g_vbdev_crypto = TAILQ_HEAD_INITIALIZER(g_vbdev_crypto);

/* global count of virtual cryptop PMDs created */
int g_virtual_pmd_count = 0;

/* The crypto vbdev channel struct. It is allocated and freed on my behalf by the io channel code.
 * We store things in here that are needed on  per thread basis like the base_channel for this thread,
 * the poller for this thread, and the PMD identifier for this thread.
 */
// TODO: the crypto and mbuf pools can be global, probably the session too but need to double check
struct crypto_io_channel {
	struct spdk_io_channel		*base_ch;		/* IO channel of base device */
	char				*crypto_name;		/* friendly name */
	int				cdev_id;		/* identifier */
	struct rte_mempool		*session_mp;		/* session mempool */
	struct rte_mempool		*crypto_op_pool;	/* operations mempool */
	struct rte_mempool		*mbuf_pool;		/* mbuf mempool */
	struct spdk_poller		*poller;		/* completion poller */
	struct rte_crypto_sym_xform	cipher_xform;		/* crypto control struct */
};

struct crypto_bdev_io {
	int cryop_cnt_remaining; /* counter when completing crypto ops which are 1:1 with iovecs */
	struct crypto_io_channel *crypto_ch; /* need to store in IO context for read completions */
	struct vbdev_crypto *crypto_node; /* the crypto node struct associated with this IO */
	enum rte_crypto_cipher_operation crypto_op; /* the crypto control struct */
	struct spdk_bdev_io *orig_io; /* the oringinal IO */
	/* Used for the single contigous buffer that serves as the crypto dest target */
	uint64_t cry_num_blocks; /* num of blocks for the contigous buffer */
	uint64_t cry_offset_blocks; /* block offset on media */
	struct iovec cry_iov; /* iov representing contig buffer */
};

// TODO: review these, decide on good values
#define CRYPTO_NUM_QPAIRS 1
#define CRYPTO_QP_DESCRIPTORS 1024
#define MAX_CRYOP_LENGTH (1024 * 32)
#define NUM_SESSIONS 1024
#define SESS_MEMPOOL_CACHE_SIZE 128
#define MAX_LIST 1024
#define NUM_MBUFS            2048
#define POOL_CACHE_SIZE      128
#define AES_CBC_IV_LENGTH    16
#define AES_CBC_KEY_LENGTH   16
#define IV_OFFSET            (sizeof(struct rte_crypto_op) + \
				sizeof(struct rte_crypto_sym_op))

/* Create, init and start a PMD given channel and options struct.  Also allocate a
 * mempool for the session and save in private channel.
 */
static int
_initialize_crypto_pmd(struct crypto_io_channel *crypto_ch)
{
	int rc;
	uint32_t sess_size;
	struct rte_cryptodev_info cdev_info;
	uint8_t socket_id = rte_socket_id();
	char mp_name[RTE_MEMPOOL_NAMESIZE];
	unsigned int crypto_op_private_data = AES_CBC_IV_LENGTH;
	char args[32];

	/* Create a unique name for each PMD that we want. */
	crypto_ch->crypto_name = spdk_sprintf_alloc("%s%d",
				 "crypto_aesni_mb", g_virtual_pmd_count);
	if (crypto_ch->crypto_name == NULL) {
		SPDK_ERRLOG("error aloocating PMD name\n");
		return -ENOMEM;
	}

	/* Create the virtual crypto PMD */
	snprintf(args, sizeof(args), "socket_id=%d", socket_id);
	rc = rte_vdev_init(crypto_ch->crypto_name, args);
	if (rc) {
		SPDK_ERRLOG("error on rte_vdev_init\n");
		free(crypto_ch->crypto_name);
		return -ENOMEM;
	}
	SPDK_NOTICELOG("created crypto PMD: %s\n", crypto_ch->crypto_name);

	/* The crypto ID is used by many pof the cryptodev API */
	crypto_ch->cdev_id = rte_cryptodev_get_dev_id(crypto_ch->crypto_name);

	/* Session size is specfic to the PMD */
	sess_size = rte_cryptodev_get_private_session_size(crypto_ch->cdev_id);

	/* Make sre we aren't asing for more than it can give us */
	rte_cryptodev_info_get(crypto_ch->cdev_id, &cdev_info);
	if (CRYPTO_NUM_QPAIRS > cdev_info.max_nb_queue_pairs) {
		SPDK_ERRLOG("No more queue paris available on %s\n", crypto_ch->crypto_name);
		free(crypto_ch->crypto_name);
		return -ENOMEM;
	}

	/* Create mempool for this PMD */
	// TODO: make these global but how is socket_id accounted for if no done in the ch cb?
	snprintf(mp_name, RTE_MEMPOOL_NAMESIZE,
		 "session_mp_%u", g_virtual_pmd_count);
	crypto_ch->session_mp = rte_mempool_create(mp_name,
				NUM_SESSIONS,
				sess_size,
				SESS_MEMPOOL_CACHE_SIZE,
				0, NULL, NULL, NULL,
				NULL, socket_id,
				0);
	if (crypto_ch->session_mp == NULL) {
		SPDK_ERRLOG("Cannot create session pool max size 0x%x on socket %d\n", sess_size, socket_id);
		free(crypto_ch->crypto_name);
		return -ENOMEM;
	}
	SPDK_NOTICELOG("Allocated session pool max size 0x%x on socket %d\n", sess_size, socket_id);

	snprintf(mp_name, RTE_MEMPOOL_NAMESIZE,
		 "mbuf_mp_%u", g_virtual_pmd_count);
	/* use general mempool as all we use the mbufs for are control structs, our data is seaprate */
	crypto_ch->mbuf_pool = rte_mempool_create(mp_name,
			       NUM_MBUFS,
			       sizeof(struct rte_mbuf),
			       POOL_CACHE_SIZE,
			       0, NULL, NULL, NULL,
			       NULL, socket_id,
			       0);
	if (crypto_ch->mbuf_pool == NULL) {
		SPDK_ERRLOG("Cannot create mbuf pool on socket %d\n", socket_id);
		free(crypto_ch->crypto_name);
		return -ENOMEM;
	}

	SPDK_NOTICELOG("Allocated mbuf pool on socket %d\n", socket_id);

	snprintf(mp_name, RTE_MEMPOOL_NAMESIZE,
		 "op_mp_%u", g_virtual_pmd_count);
	crypto_ch->crypto_op_pool = rte_crypto_op_pool_create(mp_name,
				    RTE_CRYPTO_OP_TYPE_SYMMETRIC,
				    NUM_MBUFS,
				    POOL_CACHE_SIZE,
				    crypto_op_private_data,
				    socket_id);
	if (crypto_ch->crypto_op_pool == NULL) {
		SPDK_ERRLOG("Cannot create crypto_op_pool on socket %d\n", socket_id);
		free(crypto_ch->crypto_name);
		return -ENOMEM;
	}
	SPDK_NOTICELOG("Allocated crypto_op_pool on socket %d\n", socket_id);

	/* Configure and setup the queue pair */
	struct rte_cryptodev_config conf = {
		.nb_queue_pairs = CRYPTO_NUM_QPAIRS,
		.socket_id = socket_id
	};
	struct rte_cryptodev_qp_conf qp_conf = {
		.nb_descriptors = CRYPTO_QP_DESCRIPTORS
	};

	rc = rte_cryptodev_configure(crypto_ch->cdev_id, &conf);
	if (rc) {
		SPDK_ERRLOG("Error with rte_cryptodev_configure() forn %s\n", crypto_ch->crypto_name);
		free(crypto_ch->crypto_name);
		return rc;
	}

	rc = rte_cryptodev_queue_pair_setup(crypto_ch->cdev_id, 0, &qp_conf,
					    socket_id, crypto_ch->session_mp);
	if (rc) {
		SPDK_ERRLOG("Failed to setup queue pair %u on "
			    "cryptodev %s", 0, crypto_ch->crypto_name);
		free(crypto_ch->crypto_name);
		return rc;
	}
	SPDK_NOTICELOG("Setup qpair OK for PMD: %s\n", crypto_ch->crypto_name);

	/* Start the PMD */
	rc = rte_cryptodev_start(crypto_ch->cdev_id);
	if (rc) {
		SPDK_ERRLOG("Failed to start device %s: error %d\n",
			    crypto_ch->crypto_name, rc);
		free(crypto_ch->crypto_name);
		return rc;
	}

	SPDK_NOTICELOG("Started (%u) PMD: %s\n", crypto_ch->cdev_id, crypto_ch->crypto_name);

	/* TODO: confirm capabilities via rte_cryptodev_info_get() */

	/* This is used to make pool names and PMD names unique. */
	g_virtual_pmd_count++;
	return 0;
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
	uint16_t i, num_dequeued_ops;
	struct spdk_bdev_io *bdev_io = NULL;
	struct crypto_bdev_io *io_ctx = NULL;
	struct rte_crypto_op *dequeued_ops[NUM_MBUFS];

	num_dequeued_ops = rte_cryptodev_dequeue_burst(crypto_ch->cdev_id, 0,
			   dequeued_ops, NUM_MBUFS);

	/* Check if operation was processed successfully */
	for (i = 0; i < num_dequeued_ops; i++) {

		/* I don't know the orrder or association of the crypto ops wrt any
		 * partiular bdev_io so need to look at each oand determine if it's
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
		rte_mempool_put(crypto_ch->mbuf_pool, dequeued_ops[i]->sym->m_src);

		/* For encryption, free the mbuf we used to encrypt, the data buffer
		 * will be freed on write completion.
		 */
		if (dequeued_ops[i]->sym->m_dst) {
			rte_mempool_put(crypto_ch->mbuf_pool, dequeued_ops[i]->sym->m_dst);
		}

		/* done encrypting complete bdev_io */
		if (--io_ctx->cryop_cnt_remaining == 0) {

			/* do the bdev_io operation */
			_crypto_operation_complete(crypto_ch, bdev_io, io_ctx->crypto_op);

			/* return session */
			rte_cryptodev_sym_session_clear(crypto_ch->cdev_id,
							dequeued_ops[i]->sym->session);
			rte_cryptodev_sym_session_free(dequeued_ops[i]->sym->session);
		}
	}

	if (num_dequeued_ops > 0) {

		/* return all crypto ops at once since we dqueued this batch */
		rte_mempool_put_bulk(crypto_ch->crypto_op_pool, (void **)dequeued_ops,
				     num_dequeued_ops);
	}
	return 0;
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
	struct crypto_io_channel *crypto_ch = io_ctx->crypto_ch;
	uint64_t total_length =
		bdev_io->u.bdev.num_blocks * io_ctx->crypto_node->crypto_bdev.blocklen;
	int rc;
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
	rc = rte_mempool_get_bulk(crypto_ch->mbuf_pool, (void **)&mbufs[0], cryop_cnt);
	if (rc) {
		SPDK_ERRLOG("ERROR trying to get mbufs!\n");
		return -ENOMEM;
	}

	/* Get the same amount but these buffers decribe the encrypted data location. */
	if (crypto_op == RTE_CRYPTO_CIPHER_OP_ENCRYPT) {
		rc = rte_mempool_get_bulk(crypto_ch->mbuf_pool, (void **)&en_mbufs[0], cryop_cnt);
		if (rc) {
			SPDK_ERRLOG("ERROR trying to get mbufs!\n");
			rte_mempool_put_bulk(crypto_ch->mbuf_pool, (void **)(void **)&mbufs[0],
					     cryop_cnt);
			return -ENOMEM;
		}
	}

	rc = rte_crypto_op_bulk_alloc(crypto_ch->crypto_op_pool,
				      RTE_CRYPTO_OP_TYPE_SYMMETRIC,
				      crypto_ops, cryop_cnt);
	if (rc < cryop_cnt) {
		rte_mempool_put_bulk(crypto_ch->mbuf_pool, (void **)(void **)&mbufs[0],
				     cryop_cnt);
		rte_mempool_put_bulk(crypto_ch->mbuf_pool, (void **)(void **)&en_mbufs[0],
				     cryop_cnt);
		if (rc > 0) {
			rte_mempool_put_bulk(crypto_ch->crypto_op_pool, (void **)crypto_ops,
					     rc);
		}
		SPDK_ERRLOG("ERROR trying to get crypto ops!\n");
		return -ENOMEM;
	}

	/* We will decrement this counter in the poller to determine when this bdev_io is done. */
	io_ctx->cryop_cnt_remaining = cryop_cnt;

	session = rte_cryptodev_sym_session_create(crypto_ch->session_mp);
	if (NULL == session) {
		rte_mempool_put_bulk(crypto_ch->mbuf_pool, (void **)(void **)&mbufs[0],
				     cryop_cnt);
		rte_mempool_put_bulk(crypto_ch->mbuf_pool, (void **)(void **)&en_mbufs[0],
				     cryop_cnt);
		rte_mempool_put_bulk(crypto_ch->crypto_op_pool, (void **)crypto_ops,
				     cryop_cnt);
		SPDK_ERRLOG("ERROR trying to create crypto session!\n");
		return -ENOMEM;
	}

	/* Init our session with the desired cipher options. */
	crypto_ch->cipher_xform.cipher.key.data = io_ctx->crypto_node->key;
	crypto_ch->cipher_xform.cipher.op = io_ctx->crypto_op = crypto_op;

	rc = rte_cryptodev_sym_session_init(crypto_ch->cdev_id, session,
					    &crypto_ch->cipher_xform, crypto_ch->session_mp);
	if (rc < 0) {
		rte_mempool_put_bulk(crypto_ch->mbuf_pool, (void **)(void **)&mbufs[0],
				     cryop_cnt);
		rte_mempool_put_bulk(crypto_ch->mbuf_pool, (void **)(void **)&en_mbufs[0],
				     cryop_cnt);
		rte_mempool_put_bulk(crypto_ch->crypto_op_pool, (void **)crypto_ops,
				     cryop_cnt);
		rte_cryptodev_sym_session_clear(crypto_ch->cdev_id, session);
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

			/* Set the IV */
			uint8_t *iv_ptr = rte_crypto_op_ctod_offset(op, uint8_t *, IV_OFFSET);

			// TODO: replace this with spdk random function
			*iv_ptr = 5;
			//generate_random_bytes(iv_ptr, AES_CBC_IV_LENGTH);

			/* Attach the crypto session to the operation */
			rc = rte_crypto_op_attach_sym_session(op, session);
			if (rc) {
				SPDK_ERRLOG("ERROR trying to attach to crypto session!\n");
				rte_mempool_put_bulk(crypto_ch->mbuf_pool, (void **)(void **)&mbufs[0],
						     cryop_cnt);
				rte_mempool_put_bulk(crypto_ch->mbuf_pool, (void **)(void **)&en_mbufs[0],
						     cryop_cnt);
				rte_mempool_put_bulk(crypto_ch->crypto_op_pool, (void **)crypto_ops,
						     cryop_cnt);
				rte_cryptodev_sym_session_clear(crypto_ch->cdev_id, session);
				rte_cryptodev_sym_session_free(session);
				return rc;
			}

			/* Increment index into crypto arrays, operations and mbufs. */
			cry_index++;
		} while (remaining > 0);
	}

	do {
		num_enqueued_ops += rte_cryptodev_enqueue_burst(crypto_ch->cdev_id, 0,
				    crypto_ops, cryop_cnt);
		if (num_enqueued_ops < cryop_cnt) {
			/* in case the PMD is ful, run the poller here */
			crypto_pmd_poller(crypto_ch);
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
 * and call our cpl callback provided below along with the original bdiv_io so that we can
 * complete it once this IO completes. For cypto operations, we'll either encrypt it first
 * (writes) then call back into bdev to submit it or we'll submite a read and the catch it
 * ont the way back for decryption.
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

/* We'll just call the base bdev and let it answer however if we were more
 * restrictive for some reason (or less) we could get the repsonse back
 * and modify according to our purposes.
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

	// TODO: add any relevnt crypto parms

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
	int rc;

	crypto_ch->base_ch = spdk_bdev_get_io_channel(crypto_node->base_desc);

	rc = _initialize_crypto_pmd(crypto_ch);
	if (rc) {
		SPDK_ERRLOG("could not create crypto PMD\n");
		return rc;
	}

	crypto_ch->poller = spdk_poller_register(crypto_pmd_poller, crypto_ch, 0);

	/* opcode and key are set per operation */
	memset(&crypto_ch->cipher_xform, 0, sizeof(struct rte_crypto_sym_xform));
	crypto_ch->cipher_xform.type = RTE_CRYPTO_SYM_XFORM_CIPHER;
	crypto_ch->cipher_xform.cipher.algo = RTE_CRYPTO_CIPHER_AES_CBC;
	crypto_ch->cipher_xform.cipher.key.length = AES_CBC_KEY_LENGTH;
	crypto_ch->cipher_xform.cipher.iv.offset = IV_OFFSET;
	crypto_ch->cipher_xform.cipher.iv.length = AES_CBC_IV_LENGTH;

	return 0;
}

/* We provide this callback for the SPDK channel code to destroy a channel
 * created with our create callback. We just need to undo anything we did
 * when we created. If this bdev used its own poller, we'd unregsiter it here.
 */
static void
crypto_bdev_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct crypto_io_channel *crypto_ch = ctx_buf;

	free(crypto_ch->crypto_name);
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
	struct bdev_names *name;
	int i;

	sp = spdk_conf_find_section(NULL, "crypto");
	if (sp != NULL) {
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
	}

	/* TODO: decide what conf file options are relevant for crypto device
	 * and store them per vbdev for use in setting up the crypto device later.
	 */

	return 0;
}

/* Called when the entire module is being torn down. */
static void
vbdev_crypto_finish(void)
{
	struct bdev_names *name;

	while ((name = TAILQ_FIRST(&g_bdev_names))) {
		TAILQ_REMOVE(&g_bdev_names, name, link);
		free(name->key);
		free(name->bdev_name);
		free(name->vbdev_name);
		free(name);
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
	// TODO: add relevant crypto details
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
		if (strcmp(name->bdev_name, bdev->name) == 0) {
			SPDK_NOTICELOG("Match on %s\n", bdev->name);
			crypto_node = calloc(1, sizeof(struct vbdev_crypto));
			if (!crypto_node) {
				SPDK_ERRLOG("could not allocate crypto_node\n");
				return;
			}

			/* The base bdev that we're attaching to. */
			crypto_node->base_bdev = bdev;
			crypto_node->crypto_bdev.name = strdup(name->vbdev_name);
			if (!crypto_node->crypto_bdev.name) {
				SPDK_ERRLOG("could not allocate crypto_bdev name\n");
				free(crypto_node);
				return;
			}

			crypto_node->key = strdup(name->key);
			if (!crypto_node->key) {
				SPDK_ERRLOG("could not allocate crypto_bdev key\n");
				free(crypto_node->crypto_bdev.name);
				free(crypto_node);
				return;
			}

			crypto_node->crypto_bdev.product_name = "crypto";
			crypto_node->crypto_bdev.write_cache = bdev->write_cache;;
			crypto_node->crypto_bdev.need_aligned_buffer = bdev->need_aligned_buffer;
			crypto_node->crypto_bdev.optimal_io_boundary = bdev->optimal_io_boundary;
			crypto_node->crypto_bdev.blocklen = bdev->blocklen;
			crypto_node->crypto_bdev.blockcnt = bdev->blockcnt;

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

			rc = spdk_bdev_open(bdev, false, vbdev_crypto_examine_hotremove_cb,
					    bdev, &crypto_node->base_desc);
			if (rc) {
				SPDK_ERRLOG("could not open bdev %s\n", spdk_bdev_get_name(bdev));
				TAILQ_REMOVE(&g_vbdev_crypto, crypto_node, link);
				free(crypto_node->crypto_bdev.name);
				free(crypto_node->key);
				free(crypto_node);
				return;
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
				return;
			}
			SPDK_NOTICELOG("bdev claimed\n");

			rc = spdk_vbdev_register(&crypto_node->crypto_bdev, &bdev, 1);
			if (rc) {
				SPDK_ERRLOG("could not register crypto_bdev\n");
				spdk_bdev_close(crypto_node->base_desc);
				TAILQ_REMOVE(&g_vbdev_crypto, crypto_node, link);
				free(crypto_node->crypto_bdev.name);
				free(crypto_node->key);
				free(crypto_node);
				return;
			}
			SPDK_NOTICELOG("crypto_bdev registered\n");
			SPDK_NOTICELOG("created crypto_bdev for: %s\n", name->vbdev_name);
		}
	}
	spdk_bdev_module_examine_done(&crypto_if);
}

SPDK_LOG_REGISTER_COMPONENT("vbdev_crypto", SPDK_LOG_VBDEV_crypto)
