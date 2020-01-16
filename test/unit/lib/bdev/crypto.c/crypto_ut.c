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

#include "spdk_cunit.h"

#include "common/lib/test_env.c"
#include "spdk_internal/mock.h"
#include "unit/lib/json_mock.c"

#include <rte_crypto.h>
#include <rte_cryptodev.h>

#define MAX_TEST_BLOCKS 8192
struct rte_crypto_op *g_test_crypto_ops[MAX_TEST_BLOCKS];
struct rte_crypto_op *g_test_dev_full_ops[MAX_TEST_BLOCKS];

uint16_t g_dequeue_mock;
uint16_t g_enqueue_mock;
unsigned ut_rte_crypto_op_bulk_alloc;
int ut_rte_crypto_op_attach_sym_session = 0;
#define MOCK_INFO_GET_1QP_AESNI 0
#define MOCK_INFO_GET_1QP_QAT 1
#define MOCK_INFO_GET_1QP_BOGUS_PMD 2
int ut_rte_cryptodev_info_get = 0;
bool ut_rte_cryptodev_info_get_mocked = false;

/* Those functions are defined as static inline in DPDK, so we can't
 * mock them straight away. We use defines to redirect them into
 * our custom functions.
 */
static bool g_resubmit_test = false;
#define rte_cryptodev_enqueue_burst mock_rte_cryptodev_enqueue_burst
static inline uint16_t
mock_rte_cryptodev_enqueue_burst(uint8_t dev_id, uint16_t qp_id,
				 struct rte_crypto_op **ops, uint16_t nb_ops)
{
	int i;

	CU_ASSERT(nb_ops > 0);

	for (i = 0; i < nb_ops; i++) {
		/* Use this empty (til now) array of pointers to store
		 * enqueued operations for assertion in dev_full test.
		 */
		g_test_dev_full_ops[i] = *ops++;
		if (g_resubmit_test == true) {
			CU_ASSERT(g_test_dev_full_ops[i] == (void *)0xDEADBEEF);
		}
	}

	return g_enqueue_mock;
}

#define rte_cryptodev_dequeue_burst mock_rte_cryptodev_dequeue_burst
static inline uint16_t
mock_rte_cryptodev_dequeue_burst(uint8_t dev_id, uint16_t qp_id,
				 struct rte_crypto_op **ops, uint16_t nb_ops)
{
	int i;

	CU_ASSERT(nb_ops > 0);

	for (i = 0; i < g_dequeue_mock; i++) {
		*ops++ = g_test_crypto_ops[i];
	}

	return g_dequeue_mock;
}

/* Instead of allocating real memory, assign the allocations to our
 * test array for assertion in tests.
 */
#define rte_crypto_op_bulk_alloc mock_rte_crypto_op_bulk_alloc
static inline unsigned
mock_rte_crypto_op_bulk_alloc(struct rte_mempool *mempool,
			      enum rte_crypto_op_type type,
			      struct rte_crypto_op **ops, uint16_t nb_ops)
{
	int i;

	for (i = 0; i < nb_ops; i++) {
		*ops++ = g_test_crypto_ops[i];
	}
	return ut_rte_crypto_op_bulk_alloc;
}

#define rte_mempool_put_bulk mock_rte_mempool_put_bulk
static __rte_always_inline void
mock_rte_mempool_put_bulk(struct rte_mempool *mp, void *const *obj_table,
			  unsigned int n)
{
	return;
}

#define rte_crypto_op_attach_sym_session mock_rte_crypto_op_attach_sym_session
static inline int
mock_rte_crypto_op_attach_sym_session(struct rte_crypto_op *op,
				      struct rte_cryptodev_sym_session *sess)
{
	return ut_rte_crypto_op_attach_sym_session;
}

#define rte_lcore_count mock_rte_lcore_count
static inline unsigned
mock_rte_lcore_count(void)
{
	return 1;
}

#include "bdev/crypto/vbdev_crypto.c"

/* SPDK stubs */
DEFINE_STUB(spdk_bdev_queue_io_wait, int, (struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		struct spdk_bdev_io_wait_entry *entry), 0);
DEFINE_STUB(spdk_conf_find_section, struct spdk_conf_section *,
	    (struct spdk_conf *cp, const char *name), NULL);
DEFINE_STUB(spdk_conf_section_get_nval, char *,
	    (struct spdk_conf_section *sp, const char *key, int idx), NULL);
DEFINE_STUB(spdk_conf_section_get_nmval, char *,
	    (struct spdk_conf_section *sp, const char *key, int idx1, int idx2), NULL);
DEFINE_STUB_V(spdk_bdev_module_list_add, (struct spdk_bdev_module *bdev_module));
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *g_bdev_io));
DEFINE_STUB(spdk_bdev_io_type_supported, bool, (struct spdk_bdev *bdev,
		enum spdk_bdev_io_type io_type), 0);
DEFINE_STUB_V(spdk_bdev_module_release_bdev, (struct spdk_bdev *bdev));
DEFINE_STUB_V(spdk_bdev_close, (struct spdk_bdev_desc *desc));
DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_get_buf_align, size_t, (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_get_io_channel, struct spdk_io_channel *, (struct spdk_bdev_desc *desc), 0);
DEFINE_STUB_V(spdk_bdev_unregister, (struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn,
				     void *cb_arg));
DEFINE_STUB(spdk_bdev_open, int, (struct spdk_bdev *bdev, bool write,
				  spdk_bdev_remove_cb_t remove_cb,
				  void *remove_ctx, struct spdk_bdev_desc **_desc), 0);
DEFINE_STUB(spdk_bdev_module_claim_bdev, int, (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
		struct spdk_bdev_module *module), 0);
DEFINE_STUB_V(spdk_bdev_module_examine_done, (struct spdk_bdev_module *module));
DEFINE_STUB(spdk_bdev_register, int, (struct spdk_bdev *vbdev), 0);

/* DPDK stubs */
DEFINE_STUB(rte_cryptodev_count, uint8_t, (void), 0);
DEFINE_STUB_V(rte_mempool_free, (struct rte_mempool *mp));
DEFINE_STUB(rte_mempool_create, struct rte_mempool *, (const char *name, unsigned n,
		unsigned elt_size,
		unsigned cache_size, unsigned private_data_size,
		rte_mempool_ctor_t *mp_init, void *mp_init_arg,
		rte_mempool_obj_cb_t *obj_init, void *obj_init_arg,
		int socket_id, unsigned flags), (struct rte_mempool *)1);
DEFINE_STUB(rte_socket_id, unsigned, (void), 0);
DEFINE_STUB(rte_crypto_op_pool_create, struct rte_mempool *,
	    (const char *name, enum rte_crypto_op_type type, unsigned nb_elts,
	     unsigned cache_size, uint16_t priv_size, int socket_id), (struct rte_mempool *)1);
DEFINE_STUB(rte_cryptodev_device_count_by_driver, uint8_t, (uint8_t driver_id), 0);
DEFINE_STUB(rte_cryptodev_configure, int, (uint8_t dev_id, struct rte_cryptodev_config *config), 0);
#if RTE_VERSION >= RTE_VERSION_NUM(19, 02, 0, 0)
DEFINE_STUB(rte_cryptodev_queue_pair_setup, int, (uint8_t dev_id, uint16_t queue_pair_id,
		const struct rte_cryptodev_qp_conf *qp_conf, int socket_id), 0);
DEFINE_STUB(rte_cryptodev_sym_session_pool_create, struct rte_mempool *, (const char *name,
		uint32_t nb_elts,
		uint32_t elt_size, uint32_t cache_size, uint16_t priv_size,
		int socket_id), (struct rte_mempool *)1);
#else
DEFINE_STUB(rte_cryptodev_queue_pair_setup, int, (uint8_t dev_id, uint16_t queue_pair_id,
		const struct rte_cryptodev_qp_conf *qp_conf,
		int socket_id, struct rte_mempool *session_pool), 0);
#endif
DEFINE_STUB(rte_cryptodev_start, int, (uint8_t dev_id), 0);
DEFINE_STUB_V(rte_cryptodev_stop, (uint8_t dev_id));
DEFINE_STUB(rte_cryptodev_sym_session_create, struct rte_cryptodev_sym_session *,
	    (struct rte_mempool *mempool), (struct rte_cryptodev_sym_session *)1);
DEFINE_STUB(rte_cryptodev_sym_session_init, int, (uint8_t dev_id,
		struct rte_cryptodev_sym_session *sess,
		struct rte_crypto_sym_xform *xforms, struct rte_mempool *mempool), 0);
DEFINE_STUB(rte_vdev_init, int, (const char *name, const char *args), 0);
DEFINE_STUB(rte_cryptodev_sym_session_free, int, (struct rte_cryptodev_sym_session *sess), 0);
DEFINE_STUB(rte_vdev_uninit, int, (const char *name), 0);

struct rte_cryptodev *rte_cryptodevs;

/* global vars and setup/cleanup functions used for all test functions */
struct spdk_bdev_io *g_bdev_io;
struct crypto_bdev_io *g_io_ctx;
struct crypto_io_channel *g_crypto_ch;
struct spdk_io_channel *g_io_ch;
struct vbdev_dev g_device;
struct vbdev_crypto g_crypto_bdev;
struct device_qp g_dev_qp;

void
rte_cryptodev_info_get(uint8_t dev_id, struct rte_cryptodev_info *dev_info)
{
	dev_info->max_nb_queue_pairs = 1;
	if (ut_rte_cryptodev_info_get == MOCK_INFO_GET_1QP_AESNI) {
		dev_info->driver_name = g_driver_names[0];
	} else if (ut_rte_cryptodev_info_get == MOCK_INFO_GET_1QP_QAT) {
		dev_info->driver_name = g_driver_names[1];
	} else if (ut_rte_cryptodev_info_get == MOCK_INFO_GET_1QP_BOGUS_PMD) {
		dev_info->driver_name = "junk";
	}
}

unsigned int
rte_cryptodev_sym_get_private_session_size(uint8_t dev_id)
{
	return (unsigned int)dev_id;
}

void
spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb, uint64_t len)
{
	cb(g_io_ch, g_bdev_io, true);
}

/* Mock these functions to call the callback and then return the value we require */
int ut_spdk_bdev_readv_blocks = 0;
bool ut_spdk_bdev_readv_blocks_mocked = false;
int
spdk_bdev_readv_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       struct iovec *iov, int iovcnt,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	cb(g_bdev_io, !ut_spdk_bdev_readv_blocks, cb_arg);
	return ut_spdk_bdev_readv_blocks;
}

int ut_spdk_bdev_writev_blocks = 0;
bool ut_spdk_bdev_writev_blocks_mocked = false;
int
spdk_bdev_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			struct iovec *iov, int iovcnt,
			uint64_t offset_blocks, uint64_t num_blocks,
			spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	cb(g_bdev_io, !ut_spdk_bdev_writev_blocks, cb_arg);
	return ut_spdk_bdev_writev_blocks;
}

int ut_spdk_bdev_unmap_blocks = 0;
bool ut_spdk_bdev_unmap_blocks_mocked = false;
int
spdk_bdev_unmap_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	cb(g_bdev_io, !ut_spdk_bdev_unmap_blocks, cb_arg);
	return ut_spdk_bdev_unmap_blocks;
}

int ut_spdk_bdev_flush_blocks = 0;
bool ut_spdk_bdev_flush_blocks_mocked = false;
int
spdk_bdev_flush_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
		       void *cb_arg)
{
	cb(g_bdev_io, !ut_spdk_bdev_flush_blocks, cb_arg);
	return ut_spdk_bdev_flush_blocks;
}

int ut_spdk_bdev_reset = 0;
bool ut_spdk_bdev_reset_mocked = false;
int
spdk_bdev_reset(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	cb(g_bdev_io, !ut_spdk_bdev_reset, cb_arg);
	return ut_spdk_bdev_reset;
}

bool g_completion_called = false;
void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	bdev_io->internal.status = status;
	g_completion_called = true;
}

/* Global setup for all tests that share a bunch of preparation... */
static int
test_setup(void)
{
	int i, rc;

	/* Prepare essential variables for test routines */
	g_bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct crypto_bdev_io));
	g_bdev_io->u.bdev.iovs = calloc(1, sizeof(struct iovec) * 128);
	g_bdev_io->bdev = &g_crypto_bdev.crypto_bdev;
	g_io_ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct crypto_io_channel));
	g_crypto_ch = (struct crypto_io_channel *)((uint8_t *)g_io_ch + sizeof(struct spdk_io_channel));
	g_io_ctx = (struct crypto_bdev_io *)g_bdev_io->driver_ctx;
	memset(&g_device, 0, sizeof(struct vbdev_dev));
	memset(&g_crypto_bdev, 0, sizeof(struct vbdev_crypto));
	g_dev_qp.device = &g_device;
	g_io_ctx->crypto_ch = g_crypto_ch;
	g_io_ctx->crypto_bdev = &g_crypto_bdev;
	g_crypto_ch->device_qp = &g_dev_qp;
	TAILQ_INIT(&g_crypto_ch->pending_cry_ios);
	TAILQ_INIT(&g_crypto_ch->queued_cry_ops);

	/* Allocate a real mbuf pool so we can test error paths */
	g_mbuf_mp = spdk_mempool_create("mbuf_mp", NUM_MBUFS, sizeof(struct rte_mbuf),
					SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
					SPDK_ENV_SOCKET_ID_ANY);

	/* Instead of allocating real rte mempools for these, it's easier and provides the
	 * same coverage just calloc them here.
	 */
	for (i = 0; i < MAX_TEST_BLOCKS; i++) {
		rc = posix_memalign((void **)&g_test_crypto_ops[i], 64,
				    sizeof(struct rte_crypto_op) + sizeof(struct rte_crypto_sym_op) +
				    AES_CBC_IV_LENGTH + QUEUED_OP_LENGTH);
		if (rc != 0) {
			assert(false);
		}
		memset(g_test_crypto_ops[i], 0, sizeof(struct rte_crypto_op) +
		       sizeof(struct rte_crypto_sym_op) + QUEUED_OP_LENGTH);
	}
	return 0;
}

/* Global teardown for all tests */
static int
test_cleanup(void)
{
	int i;

	spdk_mempool_free(g_mbuf_mp);
	for (i = 0; i < MAX_TEST_BLOCKS; i++) {
		free(g_test_crypto_ops[i]);
	}
	free(g_bdev_io->u.bdev.iovs);
	free(g_bdev_io);
	free(g_io_ch);
	return 0;
}

static void
test_error_paths(void)
{
	/* Single element block size write, just to test error paths
	 * in vbdev_crypto_submit_request().
	 */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	g_bdev_io->u.bdev.iovcnt = 1;
	g_bdev_io->u.bdev.num_blocks = 1;
	g_bdev_io->u.bdev.iovs[0].iov_len = 512;
	g_crypto_bdev.crypto_bdev.blocklen = 512;
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
	g_enqueue_mock = g_dequeue_mock = ut_rte_crypto_op_bulk_alloc = 1;

	/* test failure of spdk_mempool_get_bulk(), will result in success because it
	 * will get queued.
	 */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	MOCK_SET(spdk_mempool_get, NULL);
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);

	/* same thing but switch to reads to test error path in _crypto_complete_io() */
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	TAILQ_INSERT_TAIL(&g_crypto_ch->pending_cry_ios, g_bdev_io, module_link);
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	/* Now with the read_blocks failing */
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	MOCK_SET(spdk_bdev_readv_blocks, -1);
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	MOCK_SET(spdk_bdev_readv_blocks, 0);
	MOCK_CLEAR(spdk_mempool_get);

	/* test failure of rte_crypto_op_bulk_alloc() */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	ut_rte_crypto_op_bulk_alloc = 0;
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	ut_rte_crypto_op_bulk_alloc = 1;

	/* test failure of rte_crypto_op_attach_sym_session() */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	ut_rte_crypto_op_attach_sym_session = -1;
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	ut_rte_crypto_op_attach_sym_session = 0;
}

static void
test_simple_write(void)
{
	/* Single element block size write */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	g_bdev_io->u.bdev.iovcnt = 1;
	g_bdev_io->u.bdev.num_blocks = 1;
	g_bdev_io->u.bdev.offset_blocks = 0;
	g_bdev_io->u.bdev.iovs[0].iov_len = 512;
	g_bdev_io->u.bdev.iovs[0].iov_base = &test_simple_write;
	g_crypto_bdev.crypto_bdev.blocklen = 512;
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
	g_enqueue_mock = g_dequeue_mock = ut_rte_crypto_op_bulk_alloc = 1;

	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_io_ctx->cryop_cnt_remaining == 1);
	CU_ASSERT(g_io_ctx->cry_iov.iov_len == 512);
	CU_ASSERT(g_io_ctx->cry_iov.iov_base != NULL);
	CU_ASSERT(g_io_ctx->cry_offset_blocks == 0);
	CU_ASSERT(g_io_ctx->cry_num_blocks == 1);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->buf_addr == &test_simple_write);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->data_len == 512);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->next == NULL);
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.length == 512);
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.offset == 0);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->userdata == g_bdev_io);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_dst->buf_addr != NULL);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_dst->data_len == 512);

	spdk_free(g_io_ctx->cry_iov.iov_base);
	spdk_mempool_put(g_mbuf_mp, g_test_crypto_ops[0]->sym->m_src);
	spdk_mempool_put(g_mbuf_mp, g_test_crypto_ops[0]->sym->m_dst);
}

static void
test_simple_read(void)
{
	/* Single element block size read */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	g_bdev_io->u.bdev.iovcnt = 1;
	g_bdev_io->u.bdev.num_blocks = 1;
	g_bdev_io->u.bdev.iovs[0].iov_len = 512;
	g_bdev_io->u.bdev.iovs[0].iov_base = &test_simple_read;
	g_crypto_bdev.crypto_bdev.blocklen = 512;
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
	g_enqueue_mock = g_dequeue_mock = ut_rte_crypto_op_bulk_alloc = 1;

	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_io_ctx->cryop_cnt_remaining == 1);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->buf_addr == &test_simple_read);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->data_len == 512);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->next == NULL);
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.length == 512);
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.offset == 0);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->userdata == g_bdev_io);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_dst == NULL);

	spdk_mempool_put(g_mbuf_mp, g_test_crypto_ops[0]->sym->m_src);
}

static void
test_large_rw(void)
{
	unsigned block_len = 512;
	unsigned num_blocks = CRYPTO_MAX_IO / block_len;
	unsigned io_len = block_len * num_blocks;
	unsigned i;

	/* Multi block size read, multi-element */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	g_bdev_io->u.bdev.iovcnt = 1;
	g_bdev_io->u.bdev.num_blocks = num_blocks;
	g_bdev_io->u.bdev.iovs[0].iov_len = io_len;
	g_bdev_io->u.bdev.iovs[0].iov_base = &test_large_rw;
	g_crypto_bdev.crypto_bdev.blocklen = block_len;
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
	g_enqueue_mock = g_dequeue_mock = ut_rte_crypto_op_bulk_alloc = num_blocks;

	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_io_ctx->cryop_cnt_remaining == (int)num_blocks);

	for (i = 0; i < num_blocks; i++) {
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->buf_addr == &test_large_rw + (i * block_len));
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->data_len == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->next == NULL);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.length == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.offset == 0);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->userdata == g_bdev_io);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst == NULL);
		spdk_mempool_put(g_mbuf_mp, g_test_crypto_ops[i]->sym->m_src);
	}

	/* Multi block size write, multi-element */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	g_bdev_io->u.bdev.iovcnt = 1;
	g_bdev_io->u.bdev.num_blocks = num_blocks;
	g_bdev_io->u.bdev.iovs[0].iov_len = io_len;
	g_bdev_io->u.bdev.iovs[0].iov_base = &test_large_rw;
	g_crypto_bdev.crypto_bdev.blocklen = block_len;
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
	g_enqueue_mock = g_dequeue_mock = ut_rte_crypto_op_bulk_alloc = num_blocks;

	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_io_ctx->cryop_cnt_remaining == (int)num_blocks);

	for (i = 0; i < num_blocks; i++) {
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->buf_addr == &test_large_rw + (i * block_len));
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->data_len == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->next == NULL);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.length == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.offset == 0);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->userdata == g_bdev_io);
		CU_ASSERT(g_io_ctx->cry_iov.iov_len == io_len);
		CU_ASSERT(g_io_ctx->cry_iov.iov_base != NULL);
		CU_ASSERT(g_io_ctx->cry_offset_blocks == 0);
		CU_ASSERT(g_io_ctx->cry_num_blocks == num_blocks);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst->buf_addr != NULL);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst->data_len == block_len);
		spdk_mempool_put(g_mbuf_mp, g_test_crypto_ops[i]->sym->m_src);
		spdk_mempool_put(g_mbuf_mp, g_test_crypto_ops[i]->sym->m_dst);
	}
	spdk_free(g_io_ctx->cry_iov.iov_base);
}

static void
test_dev_full(void)
{
	struct vbdev_crypto_op *queued_op;
	struct rte_crypto_sym_op *sym_op;
	struct crypto_bdev_io *io_ctx;

	/* Two element block size read */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	g_bdev_io->u.bdev.iovcnt = 1;
	g_bdev_io->u.bdev.num_blocks = 2;
	g_bdev_io->u.bdev.iovs[0].iov_len = 512;
	g_bdev_io->u.bdev.iovs[0].iov_base = (void *)0xDEADBEEF;
	g_bdev_io->u.bdev.iovs[1].iov_len = 512;
	g_bdev_io->u.bdev.iovs[1].iov_base = (void *)0xFEEDBEEF;
	g_crypto_bdev.crypto_bdev.blocklen = 512;
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
	g_enqueue_mock = g_dequeue_mock = 1;
	ut_rte_crypto_op_bulk_alloc = 2;

	g_test_crypto_ops[1]->status = RTE_CRYPTO_OP_STATUS_NOT_PROCESSED;
	CU_ASSERT(TAILQ_EMPTY(&g_crypto_ch->queued_cry_ops) == true);

	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_io_ctx->cryop_cnt_remaining == 2);
	sym_op = g_test_crypto_ops[0]->sym;
	CU_ASSERT(sym_op->m_src->buf_addr == (void *)0xDEADBEEF);
	CU_ASSERT(sym_op->m_src->data_len == 512);
	CU_ASSERT(sym_op->m_src->next == NULL);
	CU_ASSERT(sym_op->cipher.data.length == 512);
	CU_ASSERT(sym_op->cipher.data.offset == 0);
	CU_ASSERT(sym_op->m_src->userdata == g_bdev_io);
	CU_ASSERT(sym_op->m_dst == NULL);

	/* make sure one got queued and confirm its values */
	CU_ASSERT(TAILQ_EMPTY(&g_crypto_ch->queued_cry_ops) == false);
	queued_op = TAILQ_FIRST(&g_crypto_ch->queued_cry_ops);
	sym_op = queued_op->crypto_op->sym;
	TAILQ_REMOVE(&g_crypto_ch->queued_cry_ops, queued_op, link);
	CU_ASSERT(queued_op->bdev_io == g_bdev_io);
	CU_ASSERT(queued_op->crypto_op == g_test_crypto_ops[1]);
	CU_ASSERT(sym_op->m_src->buf_addr == (void *)0xFEEDBEEF);
	CU_ASSERT(sym_op->m_src->data_len == 512);
	CU_ASSERT(sym_op->m_src->next == NULL);
	CU_ASSERT(sym_op->cipher.data.length == 512);
	CU_ASSERT(sym_op->cipher.data.offset == 0);
	CU_ASSERT(sym_op->m_src->userdata == g_bdev_io);
	CU_ASSERT(sym_op->m_dst == NULL);
	CU_ASSERT(TAILQ_EMPTY(&g_crypto_ch->queued_cry_ops) == true);
	spdk_mempool_put(g_mbuf_mp, g_test_crypto_ops[0]->sym->m_src);
	spdk_mempool_put(g_mbuf_mp, g_test_crypto_ops[1]->sym->m_src);

	/* Non-busy reason for enqueue failure, all were rejected. */
	g_enqueue_mock = 0;
	g_test_crypto_ops[0]->status = RTE_CRYPTO_OP_STATUS_ERROR;
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	io_ctx = (struct crypto_bdev_io *)g_bdev_io->driver_ctx;
	CU_ASSERT(io_ctx->bdev_io_status == SPDK_BDEV_IO_STATUS_FAILED);
}

static void
test_crazy_rw(void)
{
	unsigned block_len = 512;
	int num_blocks = 4;
	int i;

	/* Multi block size read, single element, strange IOV makeup */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	g_bdev_io->u.bdev.iovcnt = 3;
	g_bdev_io->u.bdev.num_blocks = num_blocks;
	g_bdev_io->u.bdev.iovs[0].iov_len = 512;
	g_bdev_io->u.bdev.iovs[0].iov_base = &test_crazy_rw;
	g_bdev_io->u.bdev.iovs[1].iov_len = 1024;
	g_bdev_io->u.bdev.iovs[1].iov_base = &test_crazy_rw + 512;
	g_bdev_io->u.bdev.iovs[2].iov_len = 512;
	g_bdev_io->u.bdev.iovs[2].iov_base = &test_crazy_rw + 512 + 1024;

	g_crypto_bdev.crypto_bdev.blocklen = block_len;
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
	g_enqueue_mock = g_dequeue_mock = ut_rte_crypto_op_bulk_alloc = num_blocks;

	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_io_ctx->cryop_cnt_remaining == num_blocks);

	for (i = 0; i < num_blocks; i++) {
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->buf_addr == &test_crazy_rw + (i * block_len));
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->data_len == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->next == NULL);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.length == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.offset == 0);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->userdata == g_bdev_io);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src == g_test_crypto_ops[i]->sym->m_src);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst == NULL);
		spdk_mempool_put(g_mbuf_mp, g_test_crypto_ops[i]->sym->m_src);
	}

	/* Multi block size write, single element strange IOV makeup */
	num_blocks = 8;
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	g_bdev_io->u.bdev.iovcnt = 4;
	g_bdev_io->u.bdev.num_blocks = num_blocks;
	g_bdev_io->u.bdev.iovs[0].iov_len = 2048;
	g_bdev_io->u.bdev.iovs[0].iov_base = &test_crazy_rw;
	g_bdev_io->u.bdev.iovs[1].iov_len = 512;
	g_bdev_io->u.bdev.iovs[1].iov_base = &test_crazy_rw + 2048;
	g_bdev_io->u.bdev.iovs[2].iov_len = 512;
	g_bdev_io->u.bdev.iovs[2].iov_base = &test_crazy_rw + 2048 + 512;
	g_bdev_io->u.bdev.iovs[3].iov_len = 1024;
	g_bdev_io->u.bdev.iovs[3].iov_base = &test_crazy_rw + 2048 + 512 + 512;

	g_crypto_bdev.crypto_bdev.blocklen = block_len;
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
	g_enqueue_mock = g_dequeue_mock = ut_rte_crypto_op_bulk_alloc = num_blocks;

	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_io_ctx->cryop_cnt_remaining == num_blocks);

	for (i = 0; i < num_blocks; i++) {
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->buf_addr == &test_crazy_rw + (i * block_len));
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->data_len == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->next == NULL);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.length == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.offset == 0);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->userdata == g_bdev_io);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src == g_test_crypto_ops[i]->sym->m_src);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst == g_test_crypto_ops[i]->sym->m_dst);
		spdk_mempool_put(g_mbuf_mp, g_test_crypto_ops[i]->sym->m_src);
		spdk_mempool_put(g_mbuf_mp, g_test_crypto_ops[i]->sym->m_dst);
	}
	spdk_free(g_io_ctx->cry_iov.iov_base);
}

static void
test_passthru(void)
{
	/* Make sure these follow our completion callback, test success & fail. */
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_UNMAP;
	MOCK_SET(spdk_bdev_unmap_blocks, 0);
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	MOCK_SET(spdk_bdev_unmap_blocks, -1);
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	MOCK_CLEAR(spdk_bdev_unmap_blocks);

	g_bdev_io->type = SPDK_BDEV_IO_TYPE_FLUSH;
	MOCK_SET(spdk_bdev_flush_blocks, 0);
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	MOCK_SET(spdk_bdev_flush_blocks, -1);
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	MOCK_CLEAR(spdk_bdev_flush_blocks);

	/* We should never get a WZ command, we report that we don't support it. */
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE_ZEROES;
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
}

static void
test_reset(void)
{
	/* TODO: There are a few different ways to do this given that
	 * the code uses spdk_for_each_channel() to implement reset
	 * handling. Submitting w/o UT for this function for now and
	 * will follow up with something shortly.
	 */
}

static void
init_cleanup(void)
{
	spdk_mempool_free(g_mbuf_mp);
	rte_mempool_free(g_session_mp);
	g_mbuf_mp = NULL;
	g_session_mp = NULL;
	if (g_session_mp_priv != NULL) {
		/* g_session_mp_priv may or may not be set depending on the DPDK version */
		rte_mempool_free(g_session_mp_priv);
	}
}

static void
test_initdrivers(void)
{
	int rc;
	static struct spdk_mempool *orig_mbuf_mp;
	static struct rte_mempool *orig_session_mp;
	static struct rte_mempool *orig_session_mp_priv;

	/* These tests will alloc and free our g_mbuf_mp
	 * so save that off here and restore it after each test is over.
	 */
	orig_mbuf_mp = g_mbuf_mp;
	orig_session_mp = g_session_mp;
	orig_session_mp_priv = g_session_mp_priv;

	g_session_mp_priv = NULL;
	g_session_mp = NULL;
	g_mbuf_mp = NULL;

	/* No drivers available, not an error though */
	MOCK_SET(rte_cryptodev_count, 0);
	rc = vbdev_crypto_init_crypto_drivers();
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_mbuf_mp == NULL);
	CU_ASSERT(g_session_mp == NULL);
	CU_ASSERT(g_session_mp_priv == NULL);

	/* Test failure of DPDK dev init. */
	MOCK_SET(rte_cryptodev_count, 2);
	MOCK_SET(rte_vdev_init, -1);
	rc = vbdev_crypto_init_crypto_drivers();
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_mbuf_mp == NULL);
	CU_ASSERT(g_session_mp == NULL);
	CU_ASSERT(g_session_mp_priv == NULL);
	MOCK_SET(rte_vdev_init, 0);

	/* Can't create session pool. */
	MOCK_SET(spdk_mempool_create, NULL);
	rc = vbdev_crypto_init_crypto_drivers();
	CU_ASSERT(rc == -ENOMEM);
	CU_ASSERT(g_mbuf_mp == NULL);
	CU_ASSERT(g_session_mp == NULL);
	CU_ASSERT(g_session_mp_priv == NULL);
	MOCK_CLEAR(spdk_mempool_create);

	/* Can't create op pool. */
	MOCK_SET(rte_crypto_op_pool_create, NULL);
	rc = vbdev_crypto_init_crypto_drivers();
	CU_ASSERT(rc == -ENOMEM);
	CU_ASSERT(g_mbuf_mp == NULL);
	CU_ASSERT(g_session_mp == NULL);
	CU_ASSERT(g_session_mp_priv == NULL);
	MOCK_SET(rte_crypto_op_pool_create, (struct rte_mempool *)1);

	/* Check resources are not sufficient */
	MOCK_CLEARED_ASSERT(spdk_mempool_create);
	rc = vbdev_crypto_init_crypto_drivers();
	CU_ASSERT(rc == -EINVAL);

	/* Test crypto dev configure failure. */
	MOCK_SET(rte_cryptodev_device_count_by_driver, 2);
	MOCK_SET(rte_cryptodev_info_get, MOCK_INFO_GET_1QP_AESNI);
	MOCK_SET(rte_cryptodev_configure, -1);
	MOCK_CLEARED_ASSERT(spdk_mempool_create);
	rc = vbdev_crypto_init_crypto_drivers();
	MOCK_SET(rte_cryptodev_configure, 0);
	CU_ASSERT(g_mbuf_mp == NULL);
	CU_ASSERT(g_session_mp == NULL);
	CU_ASSERT(g_session_mp_priv == NULL);
	CU_ASSERT(rc == -EINVAL);

	/* Test failure of qp setup. */
	MOCK_SET(rte_cryptodev_queue_pair_setup, -1);
	MOCK_CLEARED_ASSERT(spdk_mempool_create);
	rc = vbdev_crypto_init_crypto_drivers();
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_mbuf_mp == NULL);
	CU_ASSERT(g_session_mp == NULL);
	CU_ASSERT(g_session_mp_priv == NULL);
	MOCK_SET(rte_cryptodev_queue_pair_setup, 0);

	/* Test failure of dev start. */
	MOCK_SET(rte_cryptodev_start, -1);
	MOCK_CLEARED_ASSERT(spdk_mempool_create);
	rc = vbdev_crypto_init_crypto_drivers();
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_mbuf_mp == NULL);
	CU_ASSERT(g_session_mp == NULL);
	CU_ASSERT(g_session_mp_priv == NULL);
	MOCK_SET(rte_cryptodev_start, 0);

	/* Test bogus PMD */
	MOCK_CLEARED_ASSERT(spdk_mempool_create);
	MOCK_SET(rte_cryptodev_info_get, MOCK_INFO_GET_1QP_BOGUS_PMD);
	rc = vbdev_crypto_init_crypto_drivers();
	CU_ASSERT(g_mbuf_mp == NULL);
	CU_ASSERT(g_session_mp == NULL);
	CU_ASSERT(rc == -EINVAL);

	/* Test happy path QAT. */
	MOCK_CLEARED_ASSERT(spdk_mempool_create);
	MOCK_SET(rte_cryptodev_info_get, MOCK_INFO_GET_1QP_QAT);
	rc = vbdev_crypto_init_crypto_drivers();
	CU_ASSERT(g_mbuf_mp != NULL);
	CU_ASSERT(g_session_mp != NULL);
	init_cleanup();
	CU_ASSERT(rc == 0);

	/* Test happy path AESNI. */
	MOCK_CLEARED_ASSERT(spdk_mempool_create);
	MOCK_SET(rte_cryptodev_info_get, MOCK_INFO_GET_1QP_AESNI);
	rc = vbdev_crypto_init_crypto_drivers();
	init_cleanup();
	CU_ASSERT(rc == 0);

	/* restore our initial values. */
	g_mbuf_mp = orig_mbuf_mp;
	g_session_mp = orig_session_mp;
	g_session_mp_priv = orig_session_mp_priv;
}

static void
test_crypto_op_complete(void)
{
	/* Need to prove to scan-build that we are setting iov_bases properly. */
	void *old_iov_base;
	struct crypto_bdev_io *orig_ctx;

	/* Make sure completion code respects failure. */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
	g_completion_called = false;
	_crypto_operation_complete(g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	CU_ASSERT(g_completion_called == true);

	/* Test read completion. */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
	g_completion_called = false;
	_crypto_operation_complete(g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_completion_called == true);

	/* Test write completion success. */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
	g_completion_called = false;
	MOCK_SET(spdk_bdev_writev_blocks, 0);
	/* Code under test will free this, if not ASAN will complain. */
	g_io_ctx->cry_iov.iov_base = spdk_malloc(16, 0x10, NULL, SPDK_ENV_LCORE_ID_ANY,
				     SPDK_MALLOC_DMA);
	orig_ctx = (struct crypto_bdev_io *)g_bdev_io->driver_ctx;
	old_iov_base = orig_ctx->cry_iov.iov_base;
	_crypto_operation_complete(g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_completion_called == true);

	/* Test write completion failed. */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
	g_completion_called = false;
	MOCK_SET(spdk_bdev_writev_blocks, -1);
	/* Code under test will free this, if not ASAN will complain. */
	g_io_ctx->cry_iov.iov_base = spdk_malloc(16, 0x40, NULL, SPDK_ENV_LCORE_ID_ANY,
				     SPDK_MALLOC_DMA);
	/* To Do: remove this garbage assert as soon as scan-build stops throwing a
	 * heap use after free error.
	 */
	SPDK_CU_ASSERT_FATAL(old_iov_base != orig_ctx->cry_iov.iov_base);
	_crypto_operation_complete(g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	CU_ASSERT(g_completion_called == true);

	/* Test bogus type for this completion. */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_RESET;
	g_completion_called = false;
	_crypto_operation_complete(g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	CU_ASSERT(g_completion_called == true);
}

static void
test_supported_io(void)
{
	void *ctx = NULL;
	bool rc = true;

	/* Make sure we always report false to WZ, we need the bdev layer to
	 * send real 0's so we can encrypt/decrypt them.
	 */
	rc = vbdev_crypto_io_type_supported(ctx, SPDK_BDEV_IO_TYPE_WRITE_ZEROES);
	CU_ASSERT(rc == false);
}

static void
test_poller(void)
{
	int rc;
	struct rte_mbuf *src_mbufs[2];
	struct vbdev_crypto_op *op_to_resubmit;

	/* test regular 1 op to dequeue and complete */
	g_dequeue_mock = g_enqueue_mock = 1;
	spdk_mempool_get_bulk(g_mbuf_mp, (void **)&src_mbufs[0], 1);
	g_test_crypto_ops[0]->sym->m_src = src_mbufs[0];
	g_test_crypto_ops[0]->sym->m_src->userdata = g_bdev_io;
	g_test_crypto_ops[0]->sym->m_dst = NULL;
	g_io_ctx->cryop_cnt_remaining = 1;
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
	rc = crypto_dev_poller(g_crypto_ch);
	CU_ASSERT(rc == 1);

	/* We have nothing dequeued but have some to resubmit */
	g_dequeue_mock = 0;
	CU_ASSERT(TAILQ_EMPTY(&g_crypto_ch->queued_cry_ops) == true);

	/* add an op to the queued list. */
	g_resubmit_test = true;
	op_to_resubmit = (struct vbdev_crypto_op *)((uint8_t *)g_test_crypto_ops[0] + QUEUED_OP_OFFSET);
	op_to_resubmit->crypto_op = (void *)0xDEADBEEF;
	op_to_resubmit->bdev_io = g_bdev_io;
	TAILQ_INSERT_TAIL(&g_crypto_ch->queued_cry_ops,
			  op_to_resubmit,
			  link);
	CU_ASSERT(TAILQ_EMPTY(&g_crypto_ch->queued_cry_ops) == false);
	rc = crypto_dev_poller(g_crypto_ch);
	g_resubmit_test = false;
	CU_ASSERT(rc == 0);
	CU_ASSERT(TAILQ_EMPTY(&g_crypto_ch->queued_cry_ops) == true);

	/* 2 to dequeue but 2nd one failed */
	g_dequeue_mock = g_enqueue_mock = 2;
	g_io_ctx->cryop_cnt_remaining = 2;
	spdk_mempool_get_bulk(g_mbuf_mp, (void **)&src_mbufs[0], 2);
	g_test_crypto_ops[0]->sym->m_src = src_mbufs[0];
	g_test_crypto_ops[0]->sym->m_src->userdata = g_bdev_io;
	g_test_crypto_ops[0]->sym->m_dst = NULL;
	g_test_crypto_ops[0]->status =  RTE_CRYPTO_OP_STATUS_SUCCESS;
	g_test_crypto_ops[1]->sym->m_src = src_mbufs[1];
	g_test_crypto_ops[1]->sym->m_src->userdata = g_bdev_io;
	g_test_crypto_ops[1]->sym->m_dst = NULL;
	g_test_crypto_ops[1]->status = RTE_CRYPTO_OP_STATUS_NOT_PROCESSED;
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	rc = crypto_dev_poller(g_crypto_ch);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	CU_ASSERT(rc == 2);
}

/* Helper function for test_assign_device_qp() */
static void
_clear_device_qp_lists(void)
{
	struct device_qp *device_qp = NULL;

	while (!TAILQ_EMPTY(&g_device_qp_qat)) {
		device_qp = TAILQ_FIRST(&g_device_qp_qat);
		TAILQ_REMOVE(&g_device_qp_qat, device_qp, link);
		free(device_qp);

	}
	CU_ASSERT(TAILQ_EMPTY(&g_device_qp_qat) == true);
	while (!TAILQ_EMPTY(&g_device_qp_aesni_mb)) {
		device_qp = TAILQ_FIRST(&g_device_qp_aesni_mb);
		TAILQ_REMOVE(&g_device_qp_aesni_mb, device_qp, link);
		free(device_qp);
	}
	CU_ASSERT(TAILQ_EMPTY(&g_device_qp_aesni_mb) == true);
}

/* Helper function for test_assign_device_qp() */
static void
_check_expected_values(struct vbdev_crypto *crypto_bdev, struct device_qp *device_qp,
		       struct crypto_io_channel *crypto_ch, uint8_t expected_index,
		       uint8_t current_index)
{
	_assign_device_qp(&g_crypto_bdev, device_qp, g_crypto_ch);
	CU_ASSERT(g_crypto_ch->device_qp->index == expected_index);
	CU_ASSERT(g_next_qat_index == current_index);
}

static void
test_assign_device_qp(void)
{
	struct device_qp *device_qp = NULL;
	int i;

	/* start with a known state, clear the device/qp lists */
	_clear_device_qp_lists();

	/* make sure that one AESNI_MB qp is found */
	device_qp = calloc(1, sizeof(struct device_qp));
	TAILQ_INSERT_TAIL(&g_device_qp_aesni_mb, device_qp, link);
	g_crypto_ch->device_qp = NULL;
	g_crypto_bdev.drv_name = AESNI_MB;
	_assign_device_qp(&g_crypto_bdev, device_qp, g_crypto_ch);
	CU_ASSERT(g_crypto_ch->device_qp != NULL);

	/* QAT testing is more complex as the code under test load balances by
	 * assigning each subsequent device/qp to every QAT_VF_SPREAD modulo
	 * g_qat_total_qp. For the current latest QAT we'll have 48 virtual functions
	 * each with 2 qp so the "spread" betwen assignments is 32.
	 */
	g_qat_total_qp = 96;
	for (i = 0; i < g_qat_total_qp; i++) {
		device_qp = calloc(1, sizeof(struct device_qp));
		device_qp->index = i;
		TAILQ_INSERT_TAIL(&g_device_qp_qat, device_qp, link);
	}
	g_crypto_ch->device_qp = NULL;
	g_crypto_bdev.drv_name = QAT;

	/* First assignment will assign to 0 and next at 32. */
	_check_expected_values(&g_crypto_bdev, device_qp, g_crypto_ch,
			       0, QAT_VF_SPREAD);

	/* Second assignment will assign to 32 and next at 64. */
	_check_expected_values(&g_crypto_bdev, device_qp, g_crypto_ch,
			       QAT_VF_SPREAD, QAT_VF_SPREAD * 2);

	/* Third assignment will assign to 64 and next at 0. */
	_check_expected_values(&g_crypto_bdev, device_qp, g_crypto_ch,
			       QAT_VF_SPREAD * 2, 0);

	/* Fourth assignment will assign to 1 and next at 33. */
	_check_expected_values(&g_crypto_bdev, device_qp, g_crypto_ch,
			       1, QAT_VF_SPREAD + 1);

	_clear_device_qp_lists();
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("crypto", test_setup, test_cleanup);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (CU_add_test(suite, "test_error_paths",
			test_error_paths) == NULL ||
	    CU_add_test(suite, "test_simple_write",
			test_simple_write) == NULL ||
	    CU_add_test(suite, "test_simple_read",
			test_simple_read) == NULL ||
	    CU_add_test(suite, "test_large_rw",
			test_large_rw) == NULL ||
	    CU_add_test(suite, "test_dev_full",
			test_dev_full) == NULL ||
	    CU_add_test(suite, "test_crazy_rw",
			test_crazy_rw) == NULL ||
	    CU_add_test(suite, "test_passthru",
			test_passthru) == NULL ||
	    CU_add_test(suite, "test_initdrivers",
			test_initdrivers) == NULL ||
	    CU_add_test(suite, "test_crypto_op_complete",
			test_crypto_op_complete) == NULL ||
	    CU_add_test(suite, "test_supported_io",
			test_supported_io) == NULL ||
	    CU_add_test(suite, "test_reset",
			test_reset) == NULL ||
	    CU_add_test(suite, "test_poller",
			test_poller) == NULL ||
	    CU_add_test(suite, "test_assign_device_qp",
			test_assign_device_qp) == NULL
	   ) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
