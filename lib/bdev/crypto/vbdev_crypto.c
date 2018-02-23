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
//#include <rte_common.h>
//#include <rte_hexdump.h>
//#include <rte_mbuf.h>
//#include <rte_malloc.h>
//#include <rte_memcpy.h>
//#include <rte_pause.h>
#include <rte_bus_vdev.h>

#include <rte_crypto.h>
#include <rte_cryptodev.h>
#include <rte_cryptodev_pmd.h>

struct crypto_io_channel;
static void crypto_pmd_poller(void *args);
static void _crypto_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
struct rte_cryptodev_config g_dev_cfg;

SPDK_DECLARE_BDEV_MODULE(crypto);

/* list of crypto_bdev names and their base bdevs via configuration file.
 * Used so we can parse the conf once at init and use this list in examine().
 */
struct bdev_names {
	char			*vbdev_name;
	char			*bdev_name;
	TAILQ_ENTRY(bdev_names)	link;
};
static TAILQ_HEAD(, bdev_names) g_bdev_names = TAILQ_HEAD_INITIALIZER(g_bdev_names);

/* List of virtual bdevs and associated info for each. */
struct crypto_nodes {
	struct spdk_bdev	*base_bdev;	/* the thing we're attaching to */
	struct spdk_bdev_desc	*base_desc; 	/* its descriptor we get from open */
	struct spdk_bdev	crypto_bdev;	/* the crypto virtual bdev */
	TAILQ_ENTRY(crypto_nodes)	link;
};
static TAILQ_HEAD(, crypto_nodes) g_crypto_nodes = TAILQ_HEAD_INITIALIZER(g_crypto_nodes);

/* global count of virtual cryptop PMDs created */
int g_virtual_pmd_count = 0;

/* The crypto vbdev channel struct. It is allocated and freed on my behalf by the io channel code.
 * If this vbdev needed to implement a poller or a queue for IO, this is where those things
 * would be defined. This crypto bdev doesn't actually need to allocate a channel, it could
 * simply pass back the channel of the bdev underneath it but for example purposes we will
 * present its own to the upper layers.
 */
// TODO: the crypto and mbuf pools can be global, probably the session too but need to double check
struct crypto_io_channel {
	struct spdk_io_channel	*base_ch; 		/* IO channel of base device */
	char			*crypto_name;		/* friendly name */
	int			cdev_id;		/* identifier */
	struct rte_mempool 	*session_mp;		/* session mempool */
	struct rte_mempool 	*crypto_op_pool;	/* operations mempool */
	struct rte_mempool 	*mbuf_pool;		/* mbuf mempool */
	struct spdk_poller	*poller;		/* completion poller */
};

/* Just for fun, this crypto_bdev module doesn't need it but this is essentially a per IO
 * context that we get handed by the bdev layer.
 */
struct crypto_bdev_io {
	int iovcnt_remaining; /* counter when completing crypto ops which are 1:1 with iovecs */
	struct crypto_io_channel *crypto_ch; /* need to store in IO context for read completions */
	enum rte_crypto_cipher_operation crypto_op;
	struct spdk_bdev_io *orig_io;
};

/* Called after we've unregistered following a hot remove callback.
 * Our finish entry point will be called next.
 */
static int
vbdev_crypto_destruct(void *ctx)
{
	struct crypto_nodes *crypto_node = (struct crypto_nodes *)ctx;

	SPDK_NOTICELOG("Entry\n");

	/* Unclaim the underlying bdev. */
	spdk_bdev_module_release_bdev(crypto_node->base_bdev);

	/* Close the underlying bdev. */
	spdk_bdev_close(crypto_node->base_desc);

	/* Done with this crypto_node. */
	TAILQ_REMOVE(&g_crypto_nodes, crypto_node, link);
	free(crypto_node->crypto_bdev.name);
	free(crypto_node);
	return 0;
}

#define CRYPTO_NUM_QPAIRS 1
#define CRYPTO_QP_DESCRIPTORS 1024
#define NUM_SESSIONS 2048
#define SESS_MEMPOOL_CACHE_SIZE 128
#define MAX_LIST 1024
#define NUM_MBUFS            2048
#define POOL_CACHE_SIZE      128
#define AES_CBC_IV_LENGTH    16
#define AES_CBC_KEY_LENGTH   16
#define IV_OFFSET            (sizeof(struct rte_crypto_op) + \
				sizeof(struct rte_crypto_sym_op))
#define MAX_IOVS 1024 // TODO: not sure this is defined anywhere but need a reasonable number

/* Global (for now) crypto transform. TODO: decide how many keys, where they come from, etc.*/
uint8_t g_cipher_key[AES_CBC_KEY_LENGTH] = { 0 } ;
struct rte_crypto_sym_xform g_cipher_xform = {
    .next = NULL,
    .type = RTE_CRYPTO_SYM_XFORM_CIPHER,
    .cipher = {
        .op = RTE_CRYPTO_CIPHER_OP_ENCRYPT,
        .algo = RTE_CRYPTO_CIPHER_AES_CBC,
        .key = {
            .data = g_cipher_key,
            .length = AES_CBC_KEY_LENGTH
        },
        .iv = {
            .offset = IV_OFFSET,
            .length = AES_CBC_IV_LENGTH
        }
    }
};

/* Following an ecnrypt or decrypt we need to then either write the encrypted data or finish
 * the read on decrypted data.  Do that here.
  */
static void
_crypto_in_place_complete(struct crypto_io_channel *crypto_ch, struct spdk_bdev_io *bdev_io,
			  enum rte_crypto_cipher_operation crypto_op)
{
	struct crypto_nodes *crypto_node = SPDK_CONTAINEROF(bdev_io->bdev, struct crypto_nodes, crypto_bdev);
	int rc = 0;

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		struct crypto_bdev_io *io_ctx = (struct crypto_bdev_io *)bdev_io->driver_ctx;

		/* Complete the original IO and then free the one that we created
		 * as a result of issuing an IO via submit_reqeust.
	 	 */
		spdk_bdev_io_complete(io_ctx->orig_io, bdev_io->status);
		spdk_bdev_free_io(bdev_io);
	} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		rc = spdk_bdev_writev_blocks(crypto_node->base_desc, crypto_ch->base_ch, bdev_io->u.bdev.iovs,
					     bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks,
					     bdev_io->u.bdev.num_blocks, _crypto_complete_io,
					     bdev_io);
	}

	if (rc != 0) {
		SPDK_ERRLOG("ERROR on crypto completion!\n");
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}

}

/* We're either encrypting on the way down or decrypting on the way back .*/
static int
_crypto_in_place(struct spdk_bdev_io *bdev_io, enum rte_crypto_cipher_operation crypto_op)
{
	struct rte_cryptodev_sym_session *session;
	struct rte_crypto_op *crypto_ops[MAX_IOVS];
	struct rte_mbuf *mbufs[MAX_IOVS];
	uint16_t num_enqueued_ops = 0;
	int iovcnt = bdev_io->u.bdev.iovcnt;
	struct crypto_bdev_io *io_ctx = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	struct crypto_io_channel *crypto_ch = io_ctx->crypto_ch;
	int rc;
	int i;

	/* NOTE: for reads, the bdev_io passed in the the one we created, for writes
	 * it's the origninal IO. Either way though, the io_ctx will be valid.
	 */

	/* Get an iovcnt worth of crypto ops and mbufs as there's a 1:1 mapping. */
	rc = rte_mempool_get_bulk(crypto_ch->mbuf_pool, (void **)&mbufs[0], iovcnt);
	if (rc) {
		SPDK_ERRLOG("ERROR trying to get mbufs!\n");
		// TODO all error paths.
		return rc;
	}

	rc = rte_crypto_op_bulk_alloc(crypto_ch->crypto_op_pool,
				      RTE_CRYPTO_OP_TYPE_SYMMETRIC,
				      crypto_ops, iovcnt);
	if (rc < iovcnt) {
		// TODO: return mbufs to mempool, or try a smaller number or something
		SPDK_ERRLOG("ERROR trying to get crypto ops!\n");
		return rc;
	}

	/* We will decrement this counter in the poller to determine when the bdev_io is done. */
	io_ctx->iovcnt_remaining = iovcnt;

	session = rte_cryptodev_sym_session_create(crypto_ch->session_mp);
	if (NULL == session) {
		// TODO: return stuff to mempool, etc
		SPDK_ERRLOG("ERROR trying to create crypto session!\n");
		return -1;
	}

	/* Init our session with the desired cipher options. */
	g_cipher_xform.cipher.op = io_ctx->crypto_op = crypto_op;
	rc = rte_cryptodev_sym_session_init(crypto_ch->cdev_id, session,
					    &g_cipher_xform, crypto_ch->session_mp);
	if (rc < 0) {
		// TODO: return stuff to mempool, etc
		SPDK_ERRLOG("ERROR trying to init crypto session!\n");
		return rc;
	}


	/* Walk through bdev iovs and build up one mbuf for each iov */
	for (i = 0; i < iovcnt; i++) {
		struct rte_crypto_op *op = crypto_ops[i];

		/* Point the mbuf data_addr to the bdev io vector, this is the only element
		 * in the mbuf structure that we use other than IO context. Length is kepy in
		 * the crypto op .
		 */
		mbufs[i]->buf_addr = bdev_io->u.bdev.iovs[i].iov_base;

		/* Set the data to encrypt/decrypt length */
		crypto_ops[i]->sym->cipher.data.length= bdev_io->u.bdev.iovs[i].iov_len;
		crypto_ops[i]->sym->cipher.data.offset = 0;

		/* Store context in every mbuf as we don't know aything about completion order */
		mbufs[i]->userdata = bdev_io;

		/* link the mbuf to the crypto op. The AESNI PMD only supprots in place operations
		 * so we leave m_dst blank.
		 */
		crypto_ops[i]->sym->m_src = mbufs[i];

		/* Set the IV */
		uint8_t *iv_ptr = rte_crypto_op_ctod_offset(op, uint8_t *,
				  IV_OFFSET);

		// TODO: replace this with spdk random function
		*iv_ptr = 5;
		//generate_random_bytes(iv_ptr, AES_CBC_IV_LENGTH);

		/* Attach the crypto session to the operation */
		rc = rte_crypto_op_attach_sym_session(op, session);
		if (rc) {
			// TODO: return stuff to mempool
			SPDK_ERRLOG("ERROR trying to attach to crypto session!\n");
			return rc;
		}
	}

	num_enqueued_ops = rte_cryptodev_enqueue_burst(crypto_ch->cdev_id, 0,
				    crypto_ops, iovcnt);

	assert(num_enqueued_ops == iovcnt);
	// TODO: if this can happen probably need to loop on a smaller number of ops?
	// or just explode or what?

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
	int rc = 0;

	/* check and see if this needs to be decrypted or just completed */
	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		struct crypto_bdev_io *io_ctx = (struct crypto_bdev_io *)bdev_io->driver_ctx;
		struct crypto_bdev_io *orig_ctx = (struct crypto_bdev_io *)orig_io->driver_ctx;

		/* Copy relevant context fields from the original IO so they are in the io_ctx
		 * available in the generic function for both encryption/decryption.
		 */
		io_ctx->orig_io = orig_ctx->orig_io;
		io_ctx->crypto_ch = orig_ctx->crypto_ch;

		rc = _crypto_in_place(bdev_io, RTE_CRYPTO_CIPHER_OP_DECRYPT);
		if (rc) {
			// TODO: figure out error path, we couldn't decrypt to the poller
			// isn't going to complete this IO, need to fail it here, etc.
			SPDK_ERRLOG("ERROR decrypting");
		}
	} else {

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
	struct crypto_nodes *crypto_node = SPDK_CONTAINEROF(bdev_io->bdev, struct crypto_nodes, crypto_bdev);
	struct crypto_io_channel *crypto_ch = spdk_io_channel_get_ctx(ch);
	struct crypto_bdev_io *io_ctx = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	int rc = 1;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:

		io_ctx->crypto_ch = crypto_ch;
		io_ctx->orig_io = bdev_io;
		rc = spdk_bdev_readv_blocks(crypto_node->base_desc, crypto_ch->base_ch, bdev_io->u.bdev.iovs,
					    bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks, _crypto_complete_io,
					    bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:

		io_ctx->crypto_ch = crypto_ch;
		rc = _crypto_in_place(bdev_io, RTE_CRYPTO_CIPHER_OP_ENCRYPT);

		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		rc = spdk_bdev_write_zeroes_blocks(crypto_node->base_desc, crypto_ch->base_ch,
						   bdev_io->u.bdev.offset_blocks,
						   bdev_io->u.bdev.num_blocks,
						   _crypto_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
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
	struct crypto_nodes *crypto_node = (struct crypto_nodes *)ctx;

	return spdk_bdev_io_type_supported(crypto_node->base_bdev, io_type);
}

/* This is the poller for PMDs. It uses a single API to dequeue whatever is ready at
 * the PMD. Then we need to decide if what we've got so far (including previous poller
 * runs) totals up to one or more complete bdev_ios and if so continue with the bdev_io
 * accordingly. This means either completing a read or issuing a write.
 */
static void
crypto_pmd_poller(void *args)
{
	struct crypto_io_channel *crypto_ch = args;
	uint16_t i, num_dequeued_ops;
	struct spdk_bdev_io *bdev_io = NULL;
	struct crypto_bdev_io *io_ctx = NULL;
	struct rte_crypto_op *dequeued_ops[MAX_IOVS];

	num_dequeued_ops = rte_cryptodev_dequeue_burst(crypto_ch->cdev_id, 0,
			   dequeued_ops, MAX_IOVS);

	/* Check if operation was processed successfully */
	for (i = 0; i < num_dequeued_ops; i++) {
		if (dequeued_ops[i]->status != RTE_CRYPTO_OP_STATUS_SUCCESS) {
			// TODO:  decide what to do here
			SPDK_ERRLOG("error with op %d\n", i);
		}

		/* I don't know the orrder or association of the crypto ops wrt any
		 * partiular bdev_io so need to look at each oand determine if it's
		 * the last one for it's bdev_io or not.
		 */
		bdev_io = (struct spdk_bdev_io *)dequeued_ops[i]->sym->m_src->userdata;
		assert(bdev_io != NULL);
		io_ctx = (struct crypto_bdev_io *)bdev_io->driver_ctx;
		assert(io_ctx->iovcnt_remaining > 0);

		/* return the assicated mbuf */
		rte_mempool_put(crypto_ch->mbuf_pool, dequeued_ops[i]->sym->m_src);

		/* done encrypting complete bdev_io */
		if (--io_ctx->iovcnt_remaining == 0) {

			/* do the bdev_io operation */
			_crypto_in_place_complete(crypto_ch, bdev_io, io_ctx->crypto_op);

			/* return session */
			rte_mempool_put(crypto_ch->session_mp,
					dequeued_ops[i]->sym->session);
		}
	}

	if (num_dequeued_ops > 0) {

		/* return all crypto ops at once since we dqueued this batch */
		rte_mempool_put_bulk(crypto_ch->crypto_op_pool, (void **)dequeued_ops,
				     num_dequeued_ops);
	}

}

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
		// TODO error paths
		SPDK_ERRLOG("error aloocating PMD name\n");
		return 1;
	}

	/* Create the virtual crypto PMD */
	snprintf(args, sizeof(args), "socket_id=%d", socket_id);
	rc = rte_vdev_init(crypto_ch->crypto_name, args);
	if (rc) {
		SPDK_ERRLOG("error on rte_vdev_init\n");
		return 1;
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
		return 0;
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
		return rc;
	}

	rc = rte_cryptodev_queue_pair_setup(crypto_ch->cdev_id, 0, &qp_conf,
					    socket_id, crypto_ch->session_mp);
	if (rc) {
		SPDK_ERRLOG("Failed to setup queue pair %u on "
		       "cryptodev %s", 0, crypto_ch->crypto_name);
		return -EINVAL;
	}
	SPDK_NOTICELOG("Setup qpair OK for PMD: %s\n", crypto_ch->crypto_name);

	/* Start the PMD */
	rc = rte_cryptodev_start(crypto_ch->cdev_id);
	if (rc) {
		SPDK_ERRLOG("Failed to start device %s: error %d\n",
		       crypto_ch->crypto_name, rc);
		return -EPERM;
	}

	SPDK_NOTICELOG("Started (%u) PMD: %s\n", crypto_ch->cdev_id, crypto_ch->crypto_name);

	/* TODO: confirm capabilities via rte_cryptodev_info_get() */

	/* This is used to make pool names and PMD names unique. */
	g_virtual_pmd_count++;
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
	struct crypto_nodes *crypto_node = (struct crypto_nodes *)ctx;
	struct spdk_io_channel *io_ch;

	SPDK_NOTICELOG("Entry\n");

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
vbdev_crypto_dump_config_json(void *ctx, struct spdk_json_write_ctx *write_ctx)
{
	struct crypto_nodes *crypto_node = (struct crypto_nodes *)ctx;

	SPDK_NOTICELOG("Entry\n");

	/* This is the output for get_bdevs() for this vbdev */
	spdk_json_write_name(write_ctx, "crypto");
	spdk_json_write_object_begin(write_ctx);

	spdk_json_write_name(write_ctx, "crypto_bdev name");
	spdk_json_write_string(write_ctx, spdk_bdev_get_name(&crypto_node->crypto_bdev));

	spdk_json_write_name(write_ctx, "base_bdev name");
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
	struct crypto_nodes *crypto_node = io_device;
	int rc;

	SPDK_NOTICELOG("Entry\n");

	crypto_ch->base_ch = spdk_bdev_get_io_channel(crypto_node->base_desc);

	rc = _initialize_crypto_pmd(crypto_ch);
	if (rc) {
		SPDK_ERRLOG("could not create crypto PMD\n");
		// TODO: eeor path
		return rc;
	}

	crypto_ch->poller = spdk_poller_register(crypto_pmd_poller, crypto_ch, 0);

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

	// TODO: I'll remove all these prints leftover from the PT template
	SPDK_NOTICELOG("Entry\n");

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
	struct bdev_names *name;
	int i;

	SPDK_NOTICELOG("Entry\n");

	sp = spdk_conf_find_section(NULL, "crypto");
	if (sp != NULL) {
		for (i = 0; ; i++) {
			if (!spdk_conf_section_get_nval(sp, "CRY", i)) {
				break;
			}

			conf_bdev_name = spdk_conf_section_get_nmval(sp, "CRY", i, 0);
			if (!conf_bdev_name) {
				SPDK_ERRLOG("crypto configuration missing bdev name\n");
				break;
			}

			conf_vbdev_name = spdk_conf_section_get_nmval(sp, "CRY", i, 1);
			if (!conf_vbdev_name) {
				SPDK_ERRLOG("crypto configuration missing crypto_bdev name\n");
				break;
			}

			name = calloc(1, sizeof(struct bdev_names));
			if (!name) {
				SPDK_ERRLOG("could not allocate bdev_names\n");
				return 1;
			}
			name->bdev_name = strdup(conf_bdev_name);
			if (!name->bdev_name) {
				SPDK_ERRLOG("could not allocate name->bdev_name\n");
				free(name);
				return 1;
			}

			name->vbdev_name = strdup(conf_vbdev_name);
			if (!name->vbdev_name) {
				SPDK_ERRLOG("could not allocate name->vbdev_name\n");
				free(name->bdev_name);
				free(name);
				return 1;
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

	SPDK_NOTICELOG("Entry\n");

	while ((name = TAILQ_FIRST(&g_bdev_names))) {
		TAILQ_REMOVE(&g_bdev_names, name, link);
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
	SPDK_NOTICELOG("Entry\n");
	return sizeof(struct crypto_bdev_io);
}

/* Called when SPDK wants to save the current config of this vbdev module to
 * a file.
 */
static void
vbdev_crypto_get_spdk_running_config(FILE *fp)
{
	struct bdev_names *names = NULL;

	SPDK_NOTICELOG("Entry\n");
	fprintf(fp, "\n[crypto]\n");
	TAILQ_FOREACH(names, &g_bdev_names, link) {
		fprintf(fp, "  crypto %s %s ", names->bdev_name, names->vbdev_name);
		fprintf(fp, "\n");
	}
	// TODO: add relevant crypto details
	fprintf(fp, "\n");
}

/* When we regsiter our bdev this is how we specify our entry points. */
static const struct spdk_bdev_fn_table vbdev_crypto_fn_table = {
	.destruct		= vbdev_crypto_destruct,
	.submit_request		= vbdev_crypto_submit_request,
	.io_type_supported	= vbdev_crypto_io_type_supported,
	.get_io_channel		= vbdev_crypto_get_io_channel,
	.dump_config_json	= vbdev_crypto_dump_config_json,
};

/* Called when the underlying base bdev goes away. */
static void
vbdev_crypto_examine_hotremove_cb(void *ctx)
{
	struct crypto_nodes *crypto_node, *tmp;
	struct spdk_bdev *bdev_find = ctx;

	SPDK_NOTICELOG("Entry\n");

	TAILQ_FOREACH_SAFE(crypto_node, &g_crypto_nodes, link, tmp) {
		if (bdev_find == crypto_node->base_bdev) {
			spdk_bdev_unregister(&crypto_node->crypto_bdev, NULL, NULL);
		}
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
	struct bdev_names *name;
	struct crypto_nodes *crypto_node;
	int rc;

	/* Check our list of names from config versus this bdev and if
	 * there's a match, create the crypto_node & bdev accordingly.
	 */
	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(name->bdev_name, bdev->name) == 0) {
			SPDK_NOTICELOG("Match on %s\n", bdev->name);
			crypto_node = calloc(1, sizeof(struct crypto_nodes));

			/* The base bdev that we're attaching to. */
			crypto_node->base_bdev = bdev;
			crypto_node->crypto_bdev.name = strdup(name->vbdev_name);
			if (!crypto_node->crypto_bdev.name) {
				SPDK_ERRLOG("could not allocate crypto_bdev name\n");
				free(crypto_node);
				return;
			}
			crypto_node->crypto_bdev.product_name = "crypto";

			crypto_node->crypto_bdev.write_cache = 0;
			crypto_node->crypto_bdev.blocklen = bdev->blocklen;
			crypto_node->crypto_bdev.blockcnt = bdev->blockcnt;

			/* This is the context that is passed to us when the bdev
			 * layer calls in so we'll save our crypto_bdev node here.
			 */
			crypto_node->crypto_bdev.ctxt = crypto_node;
			crypto_node->crypto_bdev.fn_table = &vbdev_crypto_fn_table;
			crypto_node->crypto_bdev.module = SPDK_GET_BDEV_MODULE(crypto);
			TAILQ_INSERT_TAIL(&g_crypto_nodes, crypto_node, link);

			spdk_io_device_register(crypto_node, crypto_bdev_ch_create_cb, crypto_bdev_ch_destroy_cb,
						sizeof(struct crypto_io_channel));
			SPDK_NOTICELOG("io_device created at: 0x%p\n", crypto_node);

			rc = spdk_bdev_open(bdev, false, vbdev_crypto_examine_hotremove_cb,
					    bdev, &crypto_node->base_desc);
			if (rc) {
				SPDK_ERRLOG("could not open bdev %s\n", spdk_bdev_get_name(bdev));
				TAILQ_REMOVE(&g_crypto_nodes, crypto_node, link);
				free(crypto_node->crypto_bdev.name);
				free(crypto_node);
				return;
			}
			SPDK_NOTICELOG("bdev opened\n");

			rc = spdk_bdev_module_claim_bdev(bdev, crypto_node->base_desc, crypto_node->crypto_bdev.module);
			if (rc) {
				SPDK_ERRLOG("could not claim bdev %s\n", spdk_bdev_get_name(bdev));
				spdk_bdev_close(crypto_node->base_desc);
				TAILQ_REMOVE(&g_crypto_nodes, crypto_node, link);
				free(crypto_node->crypto_bdev.name);
				free(crypto_node);
				return;
			}
			SPDK_NOTICELOG("bdev claimed\n");

			rc = spdk_vbdev_register(&crypto_node->crypto_bdev, &bdev, 1);
			if (rc) {
				SPDK_ERRLOG("could not register crypto_bdev\n");
				spdk_bdev_close(crypto_node->base_desc);
				TAILQ_REMOVE(&g_crypto_nodes, crypto_node, link);
				free(crypto_node->crypto_bdev.name);
				free(crypto_node);
				return;
			}
			SPDK_NOTICELOG("crypto_bdev registered\n");
			SPDK_NOTICELOG("created crypto_bdev for: %s\n", name->vbdev_name);
		}
	}
	spdk_bdev_module_examine_done(SPDK_GET_BDEV_MODULE(crypto));
}

SPDK_BDEV_MODULE_REGISTER(crypto, vbdev_crypto_init, vbdev_crypto_finish,
			  vbdev_crypto_get_spdk_running_config,
			  vbdev_crypto_get_ctx_size, vbdev_crypto_examine)
SPDK_LOG_REGISTER_COMPONENT("vbdev_crypto", SPDK_LOG_VBDEV_crypto)
