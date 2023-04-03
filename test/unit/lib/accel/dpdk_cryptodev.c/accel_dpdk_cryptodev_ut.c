/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   Copyright (c) 2022, 2023 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "spdk_cunit.h"

#include "spdk_internal/mock.h"
#include "thread/thread_internal.h"
#include "unit/lib/json_mock.c"
#include "common/lib/ut_multithread.c"

#include <rte_crypto.h>
#include <rte_cryptodev.h>
#include <rte_version.h>

#define MAX_TEST_BLOCKS 8192
struct rte_crypto_op *g_test_crypto_ops[MAX_TEST_BLOCKS];
struct rte_crypto_op *g_test_dev_full_ops[MAX_TEST_BLOCKS];

uint16_t g_dequeue_mock;
uint16_t g_enqueue_mock;
unsigned ut_rte_crypto_op_bulk_alloc;
int ut_rte_crypto_op_attach_sym_session = 0;
#define MOCK_INFO_GET_1QP_AESNI 0
#define MOCK_INFO_GET_1QP_QAT 1
#define MOCK_INFO_GET_1QP_MLX5 2
#define MOCK_INFO_GET_1QP_BOGUS_PMD 3
int ut_rte_cryptodev_info_get = 0;
bool ut_rte_cryptodev_info_get_mocked = false;

void mock_rte_pktmbuf_free_bulk(struct rte_mbuf **m, unsigned int cnt);
#define rte_pktmbuf_free_bulk mock_rte_pktmbuf_free_bulk
void
mock_rte_pktmbuf_free_bulk(struct rte_mbuf **m, unsigned int cnt)
{
	spdk_mempool_put_bulk((struct spdk_mempool *)m[0]->pool, (void **)m, cnt);
}

void mock_rte_pktmbuf_free(struct rte_mbuf *m);
#define rte_pktmbuf_free mock_rte_pktmbuf_free
void
mock_rte_pktmbuf_free(struct rte_mbuf *m)
{
	spdk_mempool_put((struct spdk_mempool *)m->pool, (void *)m);
}

void
rte_mempool_free(struct rte_mempool *mp)
{
	spdk_mempool_free((struct spdk_mempool *)mp);
}

int mock_rte_pktmbuf_alloc_bulk(struct rte_mempool *pool, struct rte_mbuf **mbufs,
				unsigned count);
#define rte_pktmbuf_alloc_bulk mock_rte_pktmbuf_alloc_bulk
int
mock_rte_pktmbuf_alloc_bulk(struct rte_mempool *pool, struct rte_mbuf **mbufs,
			    unsigned count)
{
	int rc;

	rc = spdk_mempool_get_bulk((struct spdk_mempool *)pool, (void **)mbufs, count);
	if (rc) {
		return rc;
	}
	for (unsigned i = 0; i < count; i++) {
		rte_pktmbuf_reset(mbufs[i]);
		mbufs[i]->pool = pool;
	}
	return rc;
}

struct rte_mempool *
rte_cryptodev_sym_session_pool_create(const char *name, uint32_t nb_elts,
				      uint32_t elt_size, uint32_t cache_size,
				      uint16_t priv_size, int socket_id)
{
	struct spdk_mempool *tmp;

	tmp = spdk_mempool_create(name, nb_elts, elt_size + priv_size,
				  cache_size, socket_id);

	return (struct rte_mempool *)tmp;

}

struct rte_mempool *
rte_pktmbuf_pool_create(const char *name, unsigned n, unsigned cache_size,
			uint16_t priv_size, uint16_t data_room_size, int socket_id)
{
	struct spdk_mempool *tmp;

	tmp = spdk_mempool_create(name, n, sizeof(struct rte_mbuf) + priv_size,
				  cache_size, socket_id);

	return (struct rte_mempool *)tmp;
}

struct rte_mempool *
rte_mempool_create(const char *name, unsigned n, unsigned elt_size,
		   unsigned cache_size, unsigned private_data_size,
		   rte_mempool_ctor_t *mp_init, void *mp_init_arg,
		   rte_mempool_obj_cb_t *obj_init, void *obj_init_arg,
		   int socket_id, unsigned flags)
{
	struct spdk_mempool *tmp;

	tmp = spdk_mempool_create(name, n, elt_size + private_data_size,
				  cache_size, socket_id);

	return (struct rte_mempool *)tmp;
}

DEFINE_RETURN_MOCK(rte_crypto_op_pool_create, struct rte_mempool *);
struct rte_mempool *
rte_crypto_op_pool_create(const char *name, enum rte_crypto_op_type type,
			  unsigned nb_elts, unsigned cache_size,
			  uint16_t priv_size, int socket_id)
{
	struct spdk_mempool *tmp;

	HANDLE_RETURN_MOCK(rte_crypto_op_pool_create);

	tmp = spdk_mempool_create(name, nb_elts,
				  sizeof(struct rte_crypto_op) + priv_size,
				  cache_size, socket_id);

	return (struct rte_mempool *)tmp;

}

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
#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
static inline int
mock_rte_crypto_op_attach_sym_session(struct rte_crypto_op *op, void *sess)
#else
static inline int
mock_rte_crypto_op_attach_sym_session(struct rte_crypto_op *op,
				      struct rte_cryptodev_sym_session *sess)
#endif
{
	return ut_rte_crypto_op_attach_sym_session;
}

#define rte_lcore_count mock_rte_lcore_count
static inline unsigned
mock_rte_lcore_count(void)
{
	return 1;
}

#include "accel/dpdk_cryptodev/accel_dpdk_cryptodev.c"

/* accel stubs */
DEFINE_STUB_V(spdk_accel_task_complete, (struct spdk_accel_task *task, int status));
DEFINE_STUB_V(spdk_accel_module_finish, (void));
DEFINE_STUB_V(spdk_accel_module_list_add, (struct spdk_accel_module_if *accel_module));

/* DPDK stubs */
#define DPDK_DYNFIELD_OFFSET offsetof(struct rte_mbuf, dynfield1[1])
DEFINE_STUB(rte_mbuf_dynfield_register, int, (const struct rte_mbuf_dynfield *params),
	    DPDK_DYNFIELD_OFFSET);
DEFINE_STUB(rte_cryptodev_count, uint8_t, (void), 0);
DEFINE_STUB(rte_socket_id, unsigned, (void), 0);
DEFINE_STUB(rte_cryptodev_device_count_by_driver, uint8_t, (uint8_t driver_id), 0);
DEFINE_STUB(rte_cryptodev_configure, int, (uint8_t dev_id, struct rte_cryptodev_config *config), 0);
DEFINE_STUB(rte_cryptodev_queue_pair_setup, int, (uint8_t dev_id, uint16_t queue_pair_id,
		const struct rte_cryptodev_qp_conf *qp_conf, int socket_id), 0);
DEFINE_STUB(rte_cryptodev_start, int, (uint8_t dev_id), 0);
DEFINE_STUB_V(rte_cryptodev_stop, (uint8_t dev_id));
DEFINE_STUB(rte_cryptodev_close, int, (uint8_t dev_id), 0);
DEFINE_STUB(rte_vdev_init, int, (const char *name, const char *args), 0);
DEFINE_STUB(rte_vdev_uninit, int, (const char *name), 0);

#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
DEFINE_STUB(rte_cryptodev_sym_session_create, void *,
	    (uint8_t dev_id, struct rte_crypto_sym_xform *xforms, struct rte_mempool *mempool), (void *)1);
DEFINE_STUB(rte_cryptodev_sym_session_free, int, (uint8_t dev_id, void *sess), 0);
#else
DEFINE_STUB(rte_cryptodev_sym_session_create, struct rte_cryptodev_sym_session *,
	    (struct rte_mempool *mempool), (void *)1);
DEFINE_STUB(rte_cryptodev_sym_session_init, int, (uint8_t dev_id,
		struct rte_cryptodev_sym_session *sess,
		struct rte_crypto_sym_xform *xforms, struct rte_mempool *mempool), 0);
DEFINE_STUB(rte_cryptodev_sym_session_free, int, (struct rte_cryptodev_sym_session *sess), 0);
#endif

struct rte_cryptodev *rte_cryptodevs;

/* global vars and setup/cleanup functions used for all test functions */
struct spdk_io_channel *g_io_ch;
struct accel_dpdk_cryptodev_io_channel *g_crypto_ch;
struct accel_dpdk_cryptodev_device g_aesni_crypto_dev;
struct accel_dpdk_cryptodev_qp g_aesni_qp;
struct accel_dpdk_cryptodev_key_handle g_key_handle;
struct accel_dpdk_cryptodev_key_priv g_key_priv;
struct spdk_accel_crypto_key g_key;

void
rte_cryptodev_info_get(uint8_t dev_id, struct rte_cryptodev_info *dev_info)
{
	dev_info->max_nb_queue_pairs = 1;
	if (ut_rte_cryptodev_info_get == MOCK_INFO_GET_1QP_AESNI) {
		dev_info->driver_name = g_driver_names[0];
	} else if (ut_rte_cryptodev_info_get == MOCK_INFO_GET_1QP_QAT) {
		dev_info->driver_name = g_driver_names[1];
	} else if (ut_rte_cryptodev_info_get == MOCK_INFO_GET_1QP_MLX5) {
		dev_info->driver_name = g_driver_names[2];
	} else if (ut_rte_cryptodev_info_get == MOCK_INFO_GET_1QP_BOGUS_PMD) {
		dev_info->driver_name = "junk";
	}
}

unsigned int
rte_cryptodev_sym_get_private_session_size(uint8_t dev_id)
{
	return (unsigned int)dev_id;
}

/* Global setup for all tests that share a bunch of preparation... */
static int
test_setup(void)
{
	int i, rc;

	/* Prepare essential variables for test routines */
	g_io_ch = calloc(1, sizeof(*g_io_ch) + sizeof(struct accel_dpdk_cryptodev_io_channel));
	g_crypto_ch = (struct accel_dpdk_cryptodev_io_channel *)spdk_io_channel_get_ctx(g_io_ch);
	TAILQ_INIT(&g_crypto_ch->queued_cry_ops);
	TAILQ_INIT(&g_crypto_ch->queued_tasks);

	g_aesni_crypto_dev.type = ACCEL_DPDK_CRYPTODEV_DRIVER_AESNI_MB;
	g_aesni_crypto_dev.qp_desc_nr = ACCEL_DPDK_CRYPTODEV_QP_DESCRIPTORS;
	TAILQ_INIT(&g_aesni_crypto_dev.qpairs);

	g_aesni_qp.device = &g_aesni_crypto_dev;
	g_crypto_ch->device_qp[ACCEL_DPDK_CRYPTODEV_DRIVER_AESNI_MB] = &g_aesni_qp;

	g_key_handle.device = &g_aesni_crypto_dev;
	g_key_priv.driver = ACCEL_DPDK_CRYPTODEV_DRIVER_AESNI_MB;
	g_key_priv.cipher = ACCEL_DPDK_CRYPTODEV_CIPHER_AES_CBC;
	TAILQ_INIT(&g_key_priv.dev_keys);
	TAILQ_INSERT_TAIL(&g_key_priv.dev_keys, &g_key_handle, link);
	g_key.priv = &g_key_priv;
	g_key.module_if = &g_accel_dpdk_cryptodev_module;


	/* Allocate a real mbuf pool so we can test error paths */
	g_mbuf_mp = rte_pktmbuf_pool_create("mbuf_mp", ACCEL_DPDK_CRYPTODEV_NUM_MBUFS,
					    (unsigned)SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
					    0, 0, SPDK_ENV_SOCKET_ID_ANY);
	/* Instead of allocating real rte mempools for these, it's easier and provides the
	 * same coverage just calloc them here.
	 */
	for (i = 0; i < MAX_TEST_BLOCKS; i++) {
		size_t size = ACCEL_DPDK_CRYPTODEV_IV_OFFSET + ACCEL_DPDK_CRYPTODEV_IV_LENGTH +
			      ACCEL_DPDK_CRYPTODEV_QUEUED_OP_LENGTH;
		rc = posix_memalign((void **)&g_test_crypto_ops[i], 64, size);
		if (rc != 0) {
			assert(false);
		}
		memset(g_test_crypto_ops[i], 0,
		       ACCEL_DPDK_CRYPTODEV_IV_OFFSET + ACCEL_DPDK_CRYPTODEV_QUEUED_OP_LENGTH);
	}
	g_mbuf_offset = DPDK_DYNFIELD_OFFSET;

	return 0;
}

/* Global teardown for all tests */
static int
test_cleanup(void)
{
	int i;

	if (g_crypto_op_mp) {
		rte_mempool_free(g_crypto_op_mp);
		g_crypto_op_mp = NULL;
	}
	if (g_mbuf_mp) {
		rte_mempool_free(g_mbuf_mp);
		g_mbuf_mp = NULL;
	}
	if (g_session_mp) {
		rte_mempool_free(g_session_mp);
		g_session_mp = NULL;
	}
	if (g_session_mp_priv != NULL) {
		/* g_session_mp_priv may or may not be set depending on the DPDK version */
		rte_mempool_free(g_session_mp_priv);
		g_session_mp_priv = NULL;
	}

	for (i = 0; i < MAX_TEST_BLOCKS; i++) {
		free(g_test_crypto_ops[i]);
	}
	free(g_io_ch);
	return 0;
}

static void
test_error_paths(void)
{
	/* Single element block size encrypt, just to test error paths
	 * in accel_dpdk_cryptodev_submit_tasks() */
	struct iovec src_iov = {.iov_base = (void *)0xDEADBEEF, .iov_len = 512 };
	struct iovec dst_iov = src_iov;
	struct accel_dpdk_cryptodev_task task = {};
	struct accel_dpdk_cryptodev_key_priv key_priv = {};
	struct spdk_accel_crypto_key key = {};
	int rc;

	task.base.op_code = ACCEL_OPC_ENCRYPT;
	task.base.s.iovcnt = 1;
	task.base.s.iovs = &src_iov;
	task.base.d.iovcnt = 1;
	task.base.d.iovs = &dst_iov;
	task.base.block_size = 512;
	task.base.crypto_key = &g_key;
	task.base.iv = 1;
	g_enqueue_mock = g_dequeue_mock = ut_rte_crypto_op_bulk_alloc = 1;

	/* case 1 - no crypto key */
	task.base.crypto_key = NULL;
	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(rc == -EINVAL);
	task.base.crypto_key = &g_key;

	/* case 2 - crypto key with wrong module_if  */
	key_priv.driver = ACCEL_DPDK_CRYPTODEV_DRIVER_AESNI_MB;
	key_priv.cipher = ACCEL_DPDK_CRYPTODEV_CIPHER_AES_CBC;
	TAILQ_INIT(&key_priv.dev_keys);
	key.priv = &key_priv;
	key.module_if = (struct spdk_accel_module_if *) 0x1;
	task.base.crypto_key = &key;
	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(rc == -EINVAL);
	key.module_if = &g_accel_dpdk_cryptodev_module;

	/* case 3 - no key handle in the channel */
	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(rc == -EINVAL);
	task.base.crypto_key = &g_key;

	/* case 4 - invalid op */
	task.base.op_code = ACCEL_OPC_COMPARE;
	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(rc == -EINVAL);
	task.base.op_code = ACCEL_OPC_ENCRYPT;

	/* case 5 - no entries in g_mbuf_mp */
	MOCK_SET(spdk_mempool_get, NULL);
	CU_ASSERT(TAILQ_EMPTY(&g_crypto_ch->queued_tasks) == true);
	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(rc == 0);
	CU_ASSERT(TAILQ_EMPTY(&g_crypto_ch->queued_tasks) == false);
	CU_ASSERT(TAILQ_FIRST(&g_crypto_ch->queued_tasks) == &task);
	MOCK_CLEAR(spdk_mempool_get);
	TAILQ_INIT(&g_crypto_ch->queued_tasks);

	/* case 6 - vtophys error in accel_dpdk_cryptodev_mbuf_attach_buf */
	MOCK_SET(spdk_vtophys, SPDK_VTOPHYS_ERROR);
	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(rc == -EFAULT);
	MOCK_CLEAR(spdk_vtophys);
}

static void
test_simple_encrypt(void)
{
	struct iovec src_iov[4] = {[0] = {.iov_base = (void *)0xDEADBEEF, .iov_len = 512 }};
	struct iovec dst_iov = src_iov[0];
	struct accel_dpdk_cryptodev_task task = {};
	struct rte_mbuf *mbuf, *next;
	int rc, i;

	task.base.op_code = ACCEL_OPC_ENCRYPT;
	task.base.s.iovcnt = 1;
	task.base.s.iovs = src_iov;
	task.base.d.iovcnt = 1;
	task.base.d.iovs = &dst_iov;
	task.base.block_size = 512;
	task.base.crypto_key = &g_key;
	task.base.iv = 1;
	g_enqueue_mock = g_dequeue_mock = ut_rte_crypto_op_bulk_alloc = 1;

	/* Inplace encryption */
	g_aesni_qp.num_enqueued_ops = 0;
	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.cryop_submitted == 1);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->buf_addr == src_iov[0].iov_base);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->data_len == src_iov[0].iov_len);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->next == NULL);
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.length == 512);
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.offset == 0);
	CU_ASSERT(*RTE_MBUF_DYNFIELD(g_test_crypto_ops[0]->sym->m_src, g_mbuf_offset,
				     uint64_t *) == (uint64_t)&task);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_dst == NULL);

	rte_pktmbuf_free(g_test_crypto_ops[0]->sym->m_src);

	/* out-of-place encryption */
	g_aesni_qp.num_enqueued_ops = 0;
	task.cryop_submitted = 0;
	dst_iov.iov_base = (void *)0xFEEDBEEF;

	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.cryop_submitted == 1);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->buf_addr == src_iov[0].iov_base);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->data_len == src_iov[0].iov_len);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->next == NULL);
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.length == 512);
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.offset == 0);
	CU_ASSERT(*RTE_MBUF_DYNFIELD(g_test_crypto_ops[0]->sym->m_src, g_mbuf_offset,
				     uint64_t *) == (uint64_t)&task);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_dst->buf_addr == dst_iov.iov_base);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_dst->data_len == dst_iov.iov_len);

	rte_pktmbuf_free(g_test_crypto_ops[0]->sym->m_src);
	rte_pktmbuf_free(g_test_crypto_ops[0]->sym->m_dst);

	/* out-of-place encryption, fragmented payload */
	g_aesni_qp.num_enqueued_ops = 0;
	task.base.s.iovcnt = 4;
	for (i = 0; i < 4; i++) {
		src_iov[i].iov_base = (void *)0xDEADBEEF + i * 128;
		src_iov[i].iov_len = 128;
	}
	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.cryop_submitted == 1);
	mbuf = g_test_crypto_ops[0]->sym->m_src;
	SPDK_CU_ASSERT_FATAL(mbuf != NULL);
	CU_ASSERT(mbuf->buf_addr == src_iov[0].iov_base);
	CU_ASSERT(mbuf->data_len == src_iov[0].iov_len);
	mbuf = mbuf->next;
	for (i = 1; i < 4; i++) {
		SPDK_CU_ASSERT_FATAL(mbuf != NULL);
		CU_ASSERT(mbuf->buf_addr == src_iov[i].iov_base);
		CU_ASSERT(mbuf->data_len == src_iov[i].iov_len);
		next = mbuf->next;
		rte_pktmbuf_free(mbuf);
		mbuf = next;
	}
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.length == 512);
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.offset == 0);
	CU_ASSERT(*RTE_MBUF_DYNFIELD(g_test_crypto_ops[0]->sym->m_src, g_mbuf_offset,
				     uint64_t *) == (uint64_t)&task);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_dst->buf_addr == dst_iov.iov_base);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_dst->data_len == dst_iov.iov_len);

	rte_pktmbuf_free(g_test_crypto_ops[0]->sym->m_src);
	rte_pktmbuf_free(g_test_crypto_ops[0]->sym->m_dst);

	/* Big logical block size, inplace encryption */
	src_iov[0].iov_len = ACCEL_DPDK_CRYPTODEV_MAX_MBUF_LEN * 4;
	dst_iov = src_iov[0];
	task.base.block_size = ACCEL_DPDK_CRYPTODEV_MAX_MBUF_LEN * 4;
	task.base.s.iovcnt = 1;
	g_enqueue_mock = g_dequeue_mock = ut_rte_crypto_op_bulk_alloc = 1;

	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.cryop_submitted == 1);
	mbuf = g_test_crypto_ops[0]->sym->m_src;
	SPDK_CU_ASSERT_FATAL(mbuf != NULL);
	CU_ASSERT(mbuf->buf_addr == src_iov[0].iov_base);
	CU_ASSERT(mbuf->data_len == ACCEL_DPDK_CRYPTODEV_MAX_MBUF_LEN);
	mbuf = mbuf->next;
	for (i = 1; i < 4; i++) {
		SPDK_CU_ASSERT_FATAL(mbuf != NULL);
		CU_ASSERT(mbuf->buf_addr == (char *)src_iov[0].iov_base + i * ACCEL_DPDK_CRYPTODEV_MAX_MBUF_LEN);
		CU_ASSERT(mbuf->data_len == ACCEL_DPDK_CRYPTODEV_MAX_MBUF_LEN);
		next = mbuf->next;
		rte_pktmbuf_free(mbuf);
		mbuf = next;
	}
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.length == ACCEL_DPDK_CRYPTODEV_MAX_MBUF_LEN * 4);
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.offset == 0);
	CU_ASSERT(*RTE_MBUF_DYNFIELD(g_test_crypto_ops[0]->sym->m_src, g_mbuf_offset,
				     uint64_t *) == (uint64_t)&task);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_dst == NULL);

	rte_pktmbuf_free(g_test_crypto_ops[0]->sym->m_src);
}

static void
test_simple_decrypt(void)
{
	struct iovec src_iov[4] = {[0] = {.iov_base = (void *)0xDEADBEEF, .iov_len = 512 }};
	struct iovec dst_iov = src_iov[0];
	struct accel_dpdk_cryptodev_task task = {};
	struct rte_mbuf *mbuf, *next;
	int rc, i;

	task.base.op_code = ACCEL_OPC_DECRYPT;
	task.base.s.iovcnt = 1;
	task.base.s.iovs = src_iov;
	task.base.d.iovcnt = 1;
	task.base.d.iovs = &dst_iov;
	task.base.block_size = 512;
	task.base.crypto_key = &g_key;
	task.base.iv = 1;
	g_enqueue_mock = g_dequeue_mock = ut_rte_crypto_op_bulk_alloc = 1;

	/* Inplace decryption */
	g_aesni_qp.num_enqueued_ops = 0;
	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.cryop_submitted == 1);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->buf_addr == src_iov[0].iov_base);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->data_len == src_iov[0].iov_len);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->next == NULL);
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.length == 512);
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.offset == 0);
	CU_ASSERT(*RTE_MBUF_DYNFIELD(g_test_crypto_ops[0]->sym->m_src, g_mbuf_offset,
				     uint64_t *) == (uint64_t)&task);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_dst == NULL);

	rte_pktmbuf_free(g_test_crypto_ops[0]->sym->m_src);

	/* out-of-place decryption */
	g_aesni_qp.num_enqueued_ops = 0;
	task.cryop_submitted = 0;
	dst_iov.iov_base = (void *)0xFEEDBEEF;

	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.cryop_submitted == 1);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->buf_addr == src_iov[0].iov_base);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->data_len == src_iov[0].iov_len);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->next == NULL);
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.length == 512);
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.offset == 0);
	CU_ASSERT(*RTE_MBUF_DYNFIELD(g_test_crypto_ops[0]->sym->m_src, g_mbuf_offset,
				     uint64_t *) == (uint64_t)&task);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_dst->buf_addr == dst_iov.iov_base);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_dst->data_len == dst_iov.iov_len);

	rte_pktmbuf_free(g_test_crypto_ops[0]->sym->m_src);
	rte_pktmbuf_free(g_test_crypto_ops[0]->sym->m_dst);

	/* out-of-place decryption, fragmented payload */
	g_aesni_qp.num_enqueued_ops = 0;
	task.base.s.iovcnt = 4;
	for (i = 0; i < 4; i++) {
		src_iov[i].iov_base = (void *)0xDEADBEEF + i * 128;
		src_iov[i].iov_len = 128;
	}
	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.cryop_submitted == 1);
	mbuf = g_test_crypto_ops[0]->sym->m_src;
	SPDK_CU_ASSERT_FATAL(mbuf != NULL);
	CU_ASSERT(mbuf->buf_addr == src_iov[0].iov_base);
	CU_ASSERT(mbuf->data_len == src_iov[0].iov_len);
	mbuf = mbuf->next;
	for (i = 1; i < 4; i++) {
		SPDK_CU_ASSERT_FATAL(mbuf != NULL);
		CU_ASSERT(mbuf->buf_addr == src_iov[i].iov_base);
		CU_ASSERT(mbuf->data_len == src_iov[i].iov_len);
		next = mbuf->next;
		rte_pktmbuf_free(mbuf);
		mbuf = next;
	}
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.length == 512);
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.offset == 0);
	CU_ASSERT(*RTE_MBUF_DYNFIELD(g_test_crypto_ops[0]->sym->m_src, g_mbuf_offset,
				     uint64_t *) == (uint64_t)&task);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_dst->buf_addr == dst_iov.iov_base);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_dst->data_len == dst_iov.iov_len);

	rte_pktmbuf_free(g_test_crypto_ops[0]->sym->m_src);
	rte_pktmbuf_free(g_test_crypto_ops[0]->sym->m_dst);

	/* Big logical block size, inplace encryption */
	src_iov[0].iov_len = ACCEL_DPDK_CRYPTODEV_MAX_MBUF_LEN * 4;
	dst_iov = src_iov[0];
	task.base.block_size = ACCEL_DPDK_CRYPTODEV_MAX_MBUF_LEN * 4;
	task.base.s.iovcnt = 1;
	g_enqueue_mock = g_dequeue_mock = ut_rte_crypto_op_bulk_alloc = 1;

	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.cryop_submitted == 1);
	mbuf = g_test_crypto_ops[0]->sym->m_src;
	SPDK_CU_ASSERT_FATAL(mbuf != NULL);
	CU_ASSERT(mbuf->buf_addr == src_iov[0].iov_base);
	CU_ASSERT(mbuf->data_len == ACCEL_DPDK_CRYPTODEV_MAX_MBUF_LEN);
	mbuf = mbuf->next;
	for (i = 1; i < 4; i++) {
		SPDK_CU_ASSERT_FATAL(mbuf != NULL);
		CU_ASSERT(mbuf->buf_addr == (char *)src_iov[0].iov_base + i * ACCEL_DPDK_CRYPTODEV_MAX_MBUF_LEN);
		CU_ASSERT(mbuf->data_len == ACCEL_DPDK_CRYPTODEV_MAX_MBUF_LEN);
		next = mbuf->next;
		rte_pktmbuf_free(mbuf);
		mbuf = next;
	}
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.length == ACCEL_DPDK_CRYPTODEV_MAX_MBUF_LEN * 4);
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.offset == 0);
	CU_ASSERT(*RTE_MBUF_DYNFIELD(g_test_crypto_ops[0]->sym->m_src, g_mbuf_offset,
				     uint64_t *) == (uint64_t)&task);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_dst == NULL);

	rte_pktmbuf_free(g_test_crypto_ops[0]->sym->m_src);
}

static void
test_large_enc_dec(void)
{
	struct accel_dpdk_cryptodev_task task = {};
	uint32_t block_len = 512;
	uint32_t num_blocks = ACCEL_DPDK_CRYPTODEV_MAX_ENQUEUE_ARRAY_SIZE * 2;
	uint32_t iov_len = num_blocks * block_len / 16;
	uint32_t blocks_in_iov = num_blocks / 16;
	uint32_t iov_idx;
	struct iovec src_iov[16];
	struct iovec dst_iov[16];
	uint32_t i;
	int rc;

	for (i = 0; i < 16; i++) {
		src_iov[i].iov_base = (void *)0xDEADBEEF + i * iov_len;
		src_iov[i].iov_len = iov_len;

		dst_iov[i].iov_base = (void *)0xDEADBEEF + i * iov_len;
		dst_iov[i].iov_len = iov_len;
	}

	task.base.op_code = ACCEL_OPC_DECRYPT;
	task.base.s.iovcnt = 16;
	task.base.s.iovs = src_iov;
	task.base.d.iovcnt = 16;
	task.base.d.iovs = dst_iov;
	task.base.block_size = 512;
	task.base.crypto_key = &g_key;
	task.base.iv = 1;

	/* Test 1. Multi block size decryption, multi-element, inplace */
	g_aesni_qp.num_enqueued_ops = 0;
	g_enqueue_mock = g_dequeue_mock = ut_rte_crypto_op_bulk_alloc = num_blocks;

	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.inplace == true);
	CU_ASSERT(task.cryop_submitted == ACCEL_DPDK_CRYPTODEV_MAX_ENQUEUE_ARRAY_SIZE);
	CU_ASSERT(task.cryop_total == num_blocks);
	CU_ASSERT(task.cryop_completed == 0);

	for (i = 0; i < ACCEL_DPDK_CRYPTODEV_MAX_ENQUEUE_ARRAY_SIZE; i++) {
		iov_idx = i / blocks_in_iov;
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->buf_addr == src_iov[iov_idx].iov_base + ((
					i % blocks_in_iov) * block_len));
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->data_len == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->next == NULL);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.length == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.offset == 0);
		CU_ASSERT(*RTE_MBUF_DYNFIELD(g_test_crypto_ops[i]->sym->m_src, g_mbuf_offset,
					     uint64_t *) == (uint64_t)&task);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst == NULL);
		rte_pktmbuf_free(g_test_crypto_ops[i]->sym->m_src);
	}

	/* Call accel_dpdk_cryptodev_process_task like it was called by completion poller */
	g_aesni_qp.num_enqueued_ops = 0;
	task.cryop_completed = task.cryop_submitted;
	rc = accel_dpdk_cryptodev_process_task(g_crypto_ch, &task);

	CU_ASSERT(rc == 0);
	CU_ASSERT(task.cryop_submitted == num_blocks);
	CU_ASSERT(task.cryop_total == task.cryop_submitted);

	for (i = 0; i < ACCEL_DPDK_CRYPTODEV_MAX_ENQUEUE_ARRAY_SIZE; i++) {
		iov_idx = i / blocks_in_iov + 8;
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->buf_addr == src_iov[iov_idx].iov_base + ((
					i % blocks_in_iov) * block_len));
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->data_len == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->next == NULL);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.length == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.offset == 0);
		CU_ASSERT(*RTE_MBUF_DYNFIELD(g_test_crypto_ops[i]->sym->m_src, g_mbuf_offset,
					     uint64_t *) == (uint64_t)&task);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst == NULL);
		rte_pktmbuf_free(g_test_crypto_ops[i]->sym->m_src);
	}

	/* Test 2. Multi block size decryption, multi-element, out-of-place */
	g_aesni_qp.num_enqueued_ops = 0;
	g_enqueue_mock = g_dequeue_mock = ut_rte_crypto_op_bulk_alloc = num_blocks;
	/* Modify dst to make payload out-of-place */
	dst_iov[0].iov_base -= 1;

	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.inplace == false);
	CU_ASSERT(task.cryop_submitted == ACCEL_DPDK_CRYPTODEV_MAX_ENQUEUE_ARRAY_SIZE);
	CU_ASSERT(task.cryop_total == num_blocks);
	CU_ASSERT(task.cryop_completed == 0);

	for (i = 0; i < ACCEL_DPDK_CRYPTODEV_MAX_ENQUEUE_ARRAY_SIZE; i++) {
		iov_idx = i / blocks_in_iov;
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->buf_addr == src_iov[iov_idx].iov_base + ((
					i % blocks_in_iov) * block_len));
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->data_len == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->next == NULL);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.length == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.offset == 0);
		CU_ASSERT(*RTE_MBUF_DYNFIELD(g_test_crypto_ops[i]->sym->m_src, g_mbuf_offset,
					     uint64_t *) == (uint64_t)&task);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst->buf_addr == dst_iov[iov_idx].iov_base + ((
					i % blocks_in_iov) * block_len));
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst->data_len == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst->next == NULL);
		rte_pktmbuf_free(g_test_crypto_ops[i]->sym->m_src);
		rte_pktmbuf_free(g_test_crypto_ops[i]->sym->m_dst);
	}

	/* Call accel_dpdk_cryptodev_process_task like it was called by completion poller */
	g_aesni_qp.num_enqueued_ops = 0;
	task.cryop_completed = task.cryop_submitted;
	rc = accel_dpdk_cryptodev_process_task(g_crypto_ch, &task);

	CU_ASSERT(rc == 0);
	CU_ASSERT(task.cryop_submitted == num_blocks);
	CU_ASSERT(task.cryop_total == task.cryop_submitted);

	for (i = 0; i < ACCEL_DPDK_CRYPTODEV_MAX_ENQUEUE_ARRAY_SIZE; i++) {
		iov_idx = i / blocks_in_iov + 8;
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->buf_addr == src_iov[iov_idx].iov_base + ((
					i % blocks_in_iov) * block_len));
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->data_len == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->next == NULL);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.length == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.offset == 0);
		CU_ASSERT(*RTE_MBUF_DYNFIELD(g_test_crypto_ops[i]->sym->m_src, g_mbuf_offset,
					     uint64_t *) == (uint64_t)&task);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst->buf_addr == dst_iov[iov_idx].iov_base + ((
					i % blocks_in_iov) * block_len));
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst->data_len == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst->next == NULL);
		rte_pktmbuf_free(g_test_crypto_ops[i]->sym->m_src);
		rte_pktmbuf_free(g_test_crypto_ops[i]->sym->m_dst);
	}

	/* Test 3. Multi block size encryption, multi-element, inplace */
	g_aesni_qp.num_enqueued_ops = 0;
	task.base.op_code = ACCEL_OPC_ENCRYPT;
	task.cryop_submitted = 0;
	g_enqueue_mock = g_dequeue_mock = ut_rte_crypto_op_bulk_alloc = num_blocks;
	/* Modify dst to make payload iplace */
	dst_iov[0].iov_base += 1;

	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.inplace == true);
	CU_ASSERT(task.cryop_submitted == ACCEL_DPDK_CRYPTODEV_MAX_ENQUEUE_ARRAY_SIZE);
	CU_ASSERT(task.cryop_total == num_blocks);
	CU_ASSERT(task.cryop_completed == 0);

	for (i = 0; i < ACCEL_DPDK_CRYPTODEV_MAX_ENQUEUE_ARRAY_SIZE; i++) {
		iov_idx = i / blocks_in_iov;
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->buf_addr == src_iov[iov_idx].iov_base + ((
					i % blocks_in_iov) * block_len));
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->data_len == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->next == NULL);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.length == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.offset == 0);
		CU_ASSERT(*RTE_MBUF_DYNFIELD(g_test_crypto_ops[i]->sym->m_src, g_mbuf_offset,
					     uint64_t *) == (uint64_t)&task);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst == NULL);
		rte_pktmbuf_free(g_test_crypto_ops[i]->sym->m_src);
	}

	/* Call accel_dpdk_cryptodev_process_task like it was called by completion poller */
	g_aesni_qp.num_enqueued_ops = 0;
	task.cryop_completed = task.cryop_submitted;
	rc = accel_dpdk_cryptodev_process_task(g_crypto_ch, &task);

	CU_ASSERT(rc == 0);
	CU_ASSERT(task.cryop_submitted == num_blocks);
	CU_ASSERT(task.cryop_total == task.cryop_submitted);

	for (i = 0; i < ACCEL_DPDK_CRYPTODEV_MAX_ENQUEUE_ARRAY_SIZE; i++) {
		iov_idx = i / blocks_in_iov + 8;
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->buf_addr == src_iov[iov_idx].iov_base + ((
					i % blocks_in_iov) * block_len));
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->data_len == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->next == NULL);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.length == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.offset == 0);
		CU_ASSERT(*RTE_MBUF_DYNFIELD(g_test_crypto_ops[i]->sym->m_src, g_mbuf_offset,
					     uint64_t *) == (uint64_t)&task);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst == NULL);
		rte_pktmbuf_free(g_test_crypto_ops[i]->sym->m_src);
	}

	/* Multi block size encryption, multi-element, out-of-place */
	g_aesni_qp.num_enqueued_ops = 0;
	task.cryop_submitted = 0;
	g_enqueue_mock = g_dequeue_mock = ut_rte_crypto_op_bulk_alloc = num_blocks;
	/* Modify dst to make payload out-of-place */
	dst_iov[0].iov_base -= 1;

	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(task.inplace == false);
	CU_ASSERT(task.cryop_submitted == ACCEL_DPDK_CRYPTODEV_MAX_ENQUEUE_ARRAY_SIZE);
	CU_ASSERT(task.cryop_total == num_blocks);
	CU_ASSERT(task.cryop_completed == 0);

	for (i = 0; i < ACCEL_DPDK_CRYPTODEV_MAX_ENQUEUE_ARRAY_SIZE; i++) {
		iov_idx = i / blocks_in_iov;
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->buf_addr == src_iov[iov_idx].iov_base + ((
					i % blocks_in_iov) * block_len));
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->data_len == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->next == NULL);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.length == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.offset == 0);
		CU_ASSERT(*RTE_MBUF_DYNFIELD(g_test_crypto_ops[i]->sym->m_src, g_mbuf_offset,
					     uint64_t *) == (uint64_t)&task);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst->buf_addr == dst_iov[iov_idx].iov_base + ((
					i % blocks_in_iov) * block_len));
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst->data_len == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst->next == NULL);
		rte_pktmbuf_free(g_test_crypto_ops[i]->sym->m_src);
		rte_pktmbuf_free(g_test_crypto_ops[i]->sym->m_dst);
	}

	/* Call accel_dpdk_cryptodev_process_task  like it was called by completion poller */
	g_aesni_qp.num_enqueued_ops = 0;
	task.cryop_completed = task.cryop_submitted;
	rc = accel_dpdk_cryptodev_process_task(g_crypto_ch, &task);

	CU_ASSERT(rc == 0);
	CU_ASSERT(task.cryop_submitted == num_blocks);
	CU_ASSERT(task.cryop_total == task.cryop_submitted);

	for (i = 0; i < ACCEL_DPDK_CRYPTODEV_MAX_ENQUEUE_ARRAY_SIZE; i++) {
		iov_idx = i / blocks_in_iov + 8;
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->buf_addr == src_iov[iov_idx].iov_base + ((
					i % blocks_in_iov) * block_len));
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->data_len == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->next == NULL);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.length == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.offset == 0);
		CU_ASSERT(*RTE_MBUF_DYNFIELD(g_test_crypto_ops[i]->sym->m_src, g_mbuf_offset,
					     uint64_t *) == (uint64_t)&task);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst->buf_addr == dst_iov[iov_idx].iov_base + ((
					i % blocks_in_iov) * block_len));
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst->data_len == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst->next == NULL);
		rte_pktmbuf_free(g_test_crypto_ops[i]->sym->m_src);
		rte_pktmbuf_free(g_test_crypto_ops[i]->sym->m_dst);
	}
}

static void
test_dev_full(void)
{
	struct accel_dpdk_cryptodev_task task = {};
	struct accel_dpdk_cryptodev_queued_op *queued_op;
	struct rte_crypto_sym_op *sym_op;
	struct iovec src_iov = {.iov_base = (void *)0xDEADBEEF, .iov_len = 1024 };
	struct iovec dst_iov = src_iov;
	int rc;

	task.base.op_code = ACCEL_OPC_DECRYPT;
	task.base.s.iovcnt = 1;
	task.base.s.iovs = &src_iov;
	task.base.d.iovcnt = 1;
	task.base.d.iovs = &dst_iov;
	task.base.block_size = 512;
	task.base.crypto_key = &g_key;
	task.base.iv = 1;

	/* Two element block size decryption */
	g_aesni_qp.num_enqueued_ops = 0;
	g_enqueue_mock = g_dequeue_mock = 1;
	ut_rte_crypto_op_bulk_alloc = 2;

	g_test_crypto_ops[1]->status = RTE_CRYPTO_OP_STATUS_NOT_PROCESSED;
	CU_ASSERT(TAILQ_EMPTY(&g_crypto_ch->queued_cry_ops) == true);

	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.cryop_submitted == 2);
	sym_op = g_test_crypto_ops[0]->sym;
	CU_ASSERT(sym_op->m_src->buf_addr == src_iov.iov_base);
	CU_ASSERT(sym_op->m_src->data_len == 512);
	CU_ASSERT(sym_op->m_src->next == NULL);
	CU_ASSERT(sym_op->cipher.data.length == 512);
	CU_ASSERT(sym_op->cipher.data.offset == 0);
	CU_ASSERT(*RTE_MBUF_DYNFIELD(sym_op->m_src, g_mbuf_offset, uint64_t *) == (uint64_t)&task);
	CU_ASSERT(sym_op->m_dst == NULL);

	/* make sure one got queued and confirm its values */
	CU_ASSERT(TAILQ_EMPTY(&g_crypto_ch->queued_cry_ops) == false);
	queued_op = TAILQ_FIRST(&g_crypto_ch->queued_cry_ops);
	sym_op = queued_op->crypto_op->sym;
	TAILQ_REMOVE(&g_crypto_ch->queued_cry_ops, queued_op, link);
	CU_ASSERT(queued_op->task == &task);
	CU_ASSERT(queued_op->crypto_op == g_test_crypto_ops[1]);
	CU_ASSERT(sym_op->m_src->buf_addr == (void *)0xDEADBEEF + 512);
	CU_ASSERT(sym_op->m_src->data_len == 512);
	CU_ASSERT(sym_op->m_src->next == NULL);
	CU_ASSERT(sym_op->cipher.data.length == 512);
	CU_ASSERT(sym_op->cipher.data.offset == 0);
	CU_ASSERT(*RTE_MBUF_DYNFIELD(sym_op->m_src, g_mbuf_offset, uint64_t *) == (uint64_t)&task);
	CU_ASSERT(sym_op->m_dst == NULL);
	CU_ASSERT(TAILQ_EMPTY(&g_crypto_ch->queued_cry_ops) == true);
	rte_pktmbuf_free(g_test_crypto_ops[0]->sym->m_src);
	rte_pktmbuf_free(g_test_crypto_ops[1]->sym->m_src);

	/* Non-busy reason for enqueue failure, all were rejected. */
	g_enqueue_mock = 0;
	g_aesni_qp.num_enqueued_ops = 0;
	g_test_crypto_ops[0]->status = RTE_CRYPTO_OP_STATUS_ERROR;
	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(rc == -EINVAL);

	/* QP is full, task should be queued */
	g_aesni_qp.num_enqueued_ops = g_aesni_crypto_dev.qp_desc_nr;
	CU_ASSERT(TAILQ_EMPTY(&g_crypto_ch->queued_tasks) == true);
	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(rc == 0);
	CU_ASSERT(!TAILQ_EMPTY(&g_crypto_ch->queued_tasks));
	CU_ASSERT(TAILQ_FIRST(&g_crypto_ch->queued_tasks) == &task);

	TAILQ_INIT(&g_crypto_ch->queued_tasks);
}

static void
test_crazy_rw(void)
{
	struct accel_dpdk_cryptodev_task task = {};
	struct iovec src_iov[4] = {
		[0] = {.iov_base = (void *)0xDEADBEEF, .iov_len = 512 },
		[1] = {.iov_base = (void *)0xDEADBEEF + 512, .iov_len = 1024 },
		[2] = {.iov_base = (void *)0xDEADBEEF + 512 + 1024, .iov_len = 512 }
	};
	struct iovec *dst_iov = src_iov;
	uint32_t block_len = 512, num_blocks = 4, i;
	int rc;

	task.base.op_code = ACCEL_OPC_DECRYPT;
	task.base.s.iovcnt = 3;
	task.base.s.iovs = src_iov;
	task.base.d.iovcnt = 3;
	task.base.d.iovs = dst_iov;
	task.base.block_size = 512;
	task.base.crypto_key = &g_key;
	task.base.iv = 1;

	/* Multi block size read, single element, strange IOV makeup */
	g_enqueue_mock = g_dequeue_mock = ut_rte_crypto_op_bulk_alloc = num_blocks;
	g_aesni_qp.num_enqueued_ops = 0;

	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.cryop_submitted == num_blocks);

	for (i = 0; i < num_blocks; i++) {
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.length == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.offset == 0);
		CU_ASSERT(*RTE_MBUF_DYNFIELD(g_test_crypto_ops[i]->sym->m_src, g_mbuf_offset,
					     uint64_t *) == (uint64_t)&task);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->next == NULL);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->buf_addr == src_iov[0].iov_base + (i * block_len));
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->data_len == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst == NULL);
		rte_pktmbuf_free(g_test_crypto_ops[i]->sym->m_src);
	}

	/* Multi block size write, single element strange IOV makeup */
	num_blocks = 8;
	task.base.op_code = ACCEL_OPC_ENCRYPT;
	task.cryop_submitted = 0;
	task.base.s.iovcnt = 4;
	task.base.d.iovcnt = 4;
	task.base.s.iovs[0].iov_len = 2048;
	task.base.s.iovs[0].iov_base = (void *)0xDEADBEEF;
	task.base.s.iovs[1].iov_len = 512;
	task.base.s.iovs[1].iov_base = (void *)0xDEADBEEF + 2048;
	task.base.s.iovs[2].iov_len = 512;
	task.base.s.iovs[2].iov_base = (void *)0xDEADBEEF + 2048 + 512;
	task.base.s.iovs[3].iov_len = 1024;
	task.base.s.iovs[3].iov_base = (void *)0xDEADBEEF + 2048 + 512 + 512;

	g_enqueue_mock = g_dequeue_mock = ut_rte_crypto_op_bulk_alloc = num_blocks;
	g_aesni_qp.num_enqueued_ops = 0;

	rc = accel_dpdk_cryptodev_submit_tasks(g_io_ch, &task.base);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.cryop_submitted == num_blocks);

	for (i = 0; i < num_blocks; i++) {
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.length == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->cipher.data.offset == 0);
		CU_ASSERT(*RTE_MBUF_DYNFIELD(g_test_crypto_ops[i]->sym->m_src, g_mbuf_offset,
					     uint64_t *) == (uint64_t)&task);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->next == NULL);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->buf_addr == src_iov[0].iov_base + (i * block_len));
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_src->data_len == block_len);
		CU_ASSERT(g_test_crypto_ops[i]->sym->m_dst == NULL);
		rte_pktmbuf_free(g_test_crypto_ops[i]->sym->m_src);
	}
}

static void
init_cleanup(void)
{
	struct accel_dpdk_cryptodev_device *dev, *tmp;

	if (g_crypto_op_mp) {
		rte_mempool_free(g_crypto_op_mp);
		g_crypto_op_mp = NULL;
	}
	if (g_mbuf_mp) {
		rte_mempool_free(g_mbuf_mp);
		g_mbuf_mp = NULL;
	}
	if (g_session_mp) {
		rte_mempool_free(g_session_mp);
		g_session_mp = NULL;
	}
	if (g_session_mp_priv != NULL) {
		/* g_session_mp_priv may or may not be set depending on the DPDK version */
		rte_mempool_free(g_session_mp_priv);
		g_session_mp_priv = NULL;
	}

	TAILQ_FOREACH_SAFE(dev, &g_crypto_devices, link, tmp) {
		TAILQ_REMOVE(&g_crypto_devices, dev, link);
		accel_dpdk_cryptodev_release(dev);
	}

	spdk_io_device_unregister(&g_accel_dpdk_cryptodev_module, NULL);
}

static void
test_initdrivers(void)
{
	int rc;
	static struct rte_mempool *orig_mbuf_mp;
	static struct rte_mempool *orig_session_mp;
	static struct rte_mempool *orig_session_mp_priv;

	/* accel_dpdk_cryptodev_init calls spdk_io_device_register, we need to have a thread */
	allocate_threads(1);
	set_thread(0);

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
	rc = accel_dpdk_cryptodev_init();
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_mbuf_mp == NULL);
	CU_ASSERT(g_session_mp == NULL);
	CU_ASSERT(g_session_mp_priv == NULL);

	/* Can't create session pool. */
	MOCK_SET(rte_cryptodev_count, 2);
	MOCK_SET(spdk_mempool_create, NULL);
	rc = accel_dpdk_cryptodev_init();
	CU_ASSERT(rc == -ENOMEM);
	CU_ASSERT(g_mbuf_mp == NULL);
	CU_ASSERT(g_session_mp == NULL);
	CU_ASSERT(g_session_mp_priv == NULL);
	MOCK_CLEAR(spdk_mempool_create);

	/* Can't create op pool. */
	MOCK_SET(rte_crypto_op_pool_create, NULL);
	rc = accel_dpdk_cryptodev_init();
	CU_ASSERT(rc == -ENOMEM);
	CU_ASSERT(g_mbuf_mp == NULL);
	CU_ASSERT(g_session_mp == NULL);
	CU_ASSERT(g_session_mp_priv == NULL);
	MOCK_CLEAR(rte_crypto_op_pool_create);

	/* Check resources are not sufficient */
	MOCK_CLEARED_ASSERT(spdk_mempool_create);
	rc = accel_dpdk_cryptodev_init();
	CU_ASSERT(rc == -EINVAL);

	/* Test crypto dev configure failure. */
	MOCK_SET(rte_cryptodev_device_count_by_driver, 2);
	MOCK_SET(rte_cryptodev_info_get, MOCK_INFO_GET_1QP_AESNI);
	MOCK_SET(rte_cryptodev_configure, -1);
	MOCK_CLEARED_ASSERT(spdk_mempool_create);
	rc = accel_dpdk_cryptodev_init();
	MOCK_SET(rte_cryptodev_configure, 0);
	CU_ASSERT(g_mbuf_mp == NULL);
	CU_ASSERT(g_session_mp == NULL);
	CU_ASSERT(g_session_mp_priv == NULL);
	CU_ASSERT(rc == -EINVAL);

	/* Test failure of qp setup. */
	MOCK_SET(rte_cryptodev_queue_pair_setup, -1);
	MOCK_CLEARED_ASSERT(spdk_mempool_create);
	rc = accel_dpdk_cryptodev_init();
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_mbuf_mp == NULL);
	CU_ASSERT(g_session_mp == NULL);
	CU_ASSERT(g_session_mp_priv == NULL);
	MOCK_SET(rte_cryptodev_queue_pair_setup, 0);

	/* Test failure of dev start. */
	MOCK_SET(rte_cryptodev_start, -1);
	MOCK_CLEARED_ASSERT(spdk_mempool_create);
	rc = accel_dpdk_cryptodev_init();
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_mbuf_mp == NULL);
	CU_ASSERT(g_session_mp == NULL);
	CU_ASSERT(g_session_mp_priv == NULL);
	MOCK_SET(rte_cryptodev_start, 0);

	/* Test bogus PMD */
	MOCK_CLEARED_ASSERT(spdk_mempool_create);
	MOCK_SET(rte_cryptodev_info_get, MOCK_INFO_GET_1QP_BOGUS_PMD);
	rc = accel_dpdk_cryptodev_init();
	CU_ASSERT(g_mbuf_mp == NULL);
	CU_ASSERT(g_session_mp == NULL);
	CU_ASSERT(rc == -EINVAL);

	/* Test happy path QAT. */
	MOCK_CLEARED_ASSERT(spdk_mempool_create);
	MOCK_SET(rte_cryptodev_info_get, MOCK_INFO_GET_1QP_QAT);
	rc = accel_dpdk_cryptodev_init();
	CU_ASSERT(g_mbuf_mp != NULL);
	CU_ASSERT(g_session_mp != NULL);
	init_cleanup();
	CU_ASSERT(rc == 0);

	/* Test happy path AESNI. */
	MOCK_CLEARED_ASSERT(spdk_mempool_create);
	MOCK_SET(rte_cryptodev_info_get, MOCK_INFO_GET_1QP_AESNI);
	rc = accel_dpdk_cryptodev_init();
	CU_ASSERT(g_mbuf_offset == DPDK_DYNFIELD_OFFSET);
	init_cleanup();
	CU_ASSERT(rc == 0);

	/* Test happy path MLX5. */
	MOCK_CLEARED_ASSERT(spdk_mempool_create);
	MOCK_SET(rte_cryptodev_info_get, MOCK_INFO_GET_1QP_MLX5);
	rc = accel_dpdk_cryptodev_init();
	CU_ASSERT(g_mbuf_offset == DPDK_DYNFIELD_OFFSET);
	init_cleanup();
	CU_ASSERT(rc == 0);

	/* Test failure of DPDK dev init. By now it is not longer an error
	 * situation for entire crypto framework. */
	MOCK_SET(rte_cryptodev_count, 2);
	MOCK_SET(rte_cryptodev_device_count_by_driver, 2);
	MOCK_SET(rte_vdev_init, -1);
	MOCK_CLEARED_ASSERT(spdk_mempool_create);
	MOCK_SET(rte_cryptodev_info_get, MOCK_INFO_GET_1QP_QAT);
	rc = accel_dpdk_cryptodev_init();
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_mbuf_mp != NULL);
	CU_ASSERT(g_session_mp != NULL);
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
	CU_ASSERT(g_session_mp_priv != NULL);
#endif
	init_cleanup();
	MOCK_SET(rte_vdev_init, 0);
	MOCK_CLEAR(rte_cryptodev_device_count_by_driver);

	/* restore our initial values. */
	g_mbuf_mp = orig_mbuf_mp;
	g_session_mp = orig_session_mp;
	g_session_mp_priv = orig_session_mp_priv;
	free_threads();
}

static void
test_supported_opcodes(void)
{
	bool rc = true;
	enum accel_opcode opc;

	for (opc = 0; opc < ACCEL_OPC_LAST; opc++) {
		rc = accel_dpdk_cryptodev_supports_opcode(opc);
		switch (opc) {
		case ACCEL_OPC_ENCRYPT:
		case ACCEL_OPC_DECRYPT:
			CU_ASSERT(rc == true);
			break;
		default:
			CU_ASSERT(rc == false);
		}
	}
}

static void
test_poller(void)
{
	struct accel_dpdk_cryptodev_task task = {};
	struct iovec src_iov = {.iov_base = (void *)0xDEADBEEF, .iov_len = 1024 };
	struct iovec dst_iov = src_iov;
	struct rte_mbuf *src_mbufs[2];
	struct accel_dpdk_cryptodev_queued_op *op_to_resubmit;
	int rc;

	task.base.op_code = ACCEL_OPC_DECRYPT;
	task.base.s.iovcnt = 1;
	task.base.s.iovs = &src_iov;
	task.base.d.iovcnt = 1;
	task.base.d.iovs = &dst_iov;
	task.base.block_size = 512;
	task.base.crypto_key = &g_key;
	task.base.iv = 1;
	task.inplace = true;

	/* test regular 1 op to dequeue and complete */
	g_dequeue_mock = g_enqueue_mock = 1;
	g_aesni_qp.num_enqueued_ops = 1;
	rte_pktmbuf_alloc_bulk(g_mbuf_mp, src_mbufs, 1);
	g_test_crypto_ops[0]->sym->m_src = src_mbufs[0];
	*RTE_MBUF_DYNFIELD(g_test_crypto_ops[0]->sym->m_src, g_mbuf_offset,
			   uint64_t *) = (uintptr_t)&task;
	g_test_crypto_ops[0]->sym->m_dst = NULL;
	task.cryop_submitted = 1;
	task.cryop_total = 1;
	task.cryop_completed = 0;
	task.base.op_code = ACCEL_OPC_DECRYPT;
	rc = accel_dpdk_cryptodev_poller(g_crypto_ch);
	CU_ASSERT(rc == 1);
	CU_ASSERT(task.cryop_completed == task.cryop_submitted);
	CU_ASSERT(g_aesni_qp.num_enqueued_ops == 0);

	/* We have nothing dequeued but have some to resubmit */
	g_dequeue_mock = 0;
	CU_ASSERT(TAILQ_EMPTY(&g_crypto_ch->queued_cry_ops) == true);

	/* add an op to the queued list. */
	task.cryop_submitted = 1;
	task.cryop_total = 1;
	task.cryop_completed = 0;
	g_resubmit_test = true;
	op_to_resubmit = (struct accel_dpdk_cryptodev_queued_op *)((uint8_t *)g_test_crypto_ops[0] +
			 ACCEL_DPDK_CRYPTODEV_QUEUED_OP_OFFSET);
	op_to_resubmit->crypto_op = (void *)0xDEADBEEF;
	op_to_resubmit->task = &task;
	op_to_resubmit->qp = &g_aesni_qp;
	TAILQ_INSERT_TAIL(&g_crypto_ch->queued_cry_ops,
			  op_to_resubmit,
			  link);
	CU_ASSERT(TAILQ_EMPTY(&g_crypto_ch->queued_cry_ops) == false);
	rc = accel_dpdk_cryptodev_poller(g_crypto_ch);
	g_resubmit_test = false;
	CU_ASSERT(rc == 1);
	CU_ASSERT(TAILQ_EMPTY(&g_crypto_ch->queued_cry_ops) == true);
	CU_ASSERT(g_aesni_qp.num_enqueued_ops == 1);

	/* 2 to dequeue but 2nd one failed */
	g_dequeue_mock = g_enqueue_mock = 2;
	g_aesni_qp.num_enqueued_ops = 2;
	task.cryop_submitted = 2;
	task.cryop_total = 2;
	task.cryop_completed = 0;
	rte_pktmbuf_alloc_bulk(g_mbuf_mp, src_mbufs, 2);
	g_test_crypto_ops[0]->sym->m_src = src_mbufs[0];
	*RTE_MBUF_DYNFIELD(g_test_crypto_ops[0]->sym->m_src, g_mbuf_offset,
			   uint64_t *) = (uint64_t)&task;
	g_test_crypto_ops[0]->sym->m_dst = NULL;
	g_test_crypto_ops[0]->status =  RTE_CRYPTO_OP_STATUS_SUCCESS;
	g_test_crypto_ops[1]->sym->m_src = src_mbufs[1];
	*RTE_MBUF_DYNFIELD(g_test_crypto_ops[1]->sym->m_src, g_mbuf_offset,
			   uint64_t *) = (uint64_t)&task;
	g_test_crypto_ops[1]->sym->m_dst = NULL;
	g_test_crypto_ops[1]->status = RTE_CRYPTO_OP_STATUS_NOT_PROCESSED;
	rc = accel_dpdk_cryptodev_poller(g_crypto_ch);
	CU_ASSERT(task.is_failed == true);
	CU_ASSERT(rc == 1);
	CU_ASSERT(g_aesni_qp.num_enqueued_ops == 0);

	/* Dequeue a task which needs to be submitted again */
	g_dequeue_mock = g_enqueue_mock = ut_rte_crypto_op_bulk_alloc = 1;
	task.cryop_submitted = 1;
	task.cryop_total = 2;
	task.cryop_completed = 0;
	g_aesni_qp.num_enqueued_ops = 1;
	rte_pktmbuf_alloc_bulk(g_mbuf_mp, src_mbufs, 1);
	SPDK_CU_ASSERT_FATAL(src_mbufs[0] != NULL);
	g_test_crypto_ops[0]->sym->m_src = src_mbufs[0];
	*RTE_MBUF_DYNFIELD(g_test_crypto_ops[0]->sym->m_src, g_mbuf_offset,
			   uint64_t *) = (uintptr_t)&task;
	g_test_crypto_ops[0]->sym->m_dst = NULL;
	rc = accel_dpdk_cryptodev_poller(g_crypto_ch);
	CU_ASSERT(rc == 1);
	CU_ASSERT(task.cryop_submitted == 2);
	CU_ASSERT(task.cryop_total == 2);
	CU_ASSERT(task.cryop_completed == 1);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->buf_addr == src_iov.iov_base + task.base.block_size);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->data_len == task.base.block_size);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->next == NULL);
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.length == task.base.block_size);
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.offset == 0);
	CU_ASSERT(*RTE_MBUF_DYNFIELD(g_test_crypto_ops[0]->sym->m_src, g_mbuf_offset,
				     uint64_t *) == (uint64_t)&task);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_dst == NULL);
	CU_ASSERT(g_aesni_qp.num_enqueued_ops == 1);
	rte_pktmbuf_free(g_test_crypto_ops[0]->sym->m_src);

	/* Process queued tasks, qp is full */
	g_dequeue_mock = g_enqueue_mock = 0;
	g_aesni_qp.num_enqueued_ops = g_aesni_crypto_dev.qp_desc_nr;
	task.cryop_submitted = 1;
	task.cryop_total = 2;
	task.cryop_completed = 1;
	CU_ASSERT(TAILQ_EMPTY(&g_crypto_ch->queued_tasks));
	TAILQ_INSERT_TAIL(&g_crypto_ch->queued_tasks, &task, link);

	rc = accel_dpdk_cryptodev_poller(g_crypto_ch);
	CU_ASSERT(rc == 0);
	CU_ASSERT(TAILQ_FIRST(&g_crypto_ch->queued_tasks) == &task);

	/* Try again when queue is empty, task should be submitted */
	g_enqueue_mock = 1;
	g_aesni_qp.num_enqueued_ops = 0;
	rc = accel_dpdk_cryptodev_poller(g_crypto_ch);
	CU_ASSERT(rc == 1);
	CU_ASSERT(task.cryop_submitted == 2);
	CU_ASSERT(task.cryop_total == 2);
	CU_ASSERT(task.cryop_completed == 1);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->buf_addr == src_iov.iov_base + task.base.block_size);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->data_len == task.base.block_size);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_src->next == NULL);
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.length == task.base.block_size);
	CU_ASSERT(g_test_crypto_ops[0]->sym->cipher.data.offset == 0);
	CU_ASSERT(*RTE_MBUF_DYNFIELD(g_test_crypto_ops[0]->sym->m_src, g_mbuf_offset,
				     uint64_t *) == (uint64_t)&task);
	CU_ASSERT(g_test_crypto_ops[0]->sym->m_dst == NULL);
	CU_ASSERT(g_aesni_qp.num_enqueued_ops == 1);
	CU_ASSERT(TAILQ_EMPTY(&g_crypto_ch->queued_tasks));
	rte_pktmbuf_free(g_test_crypto_ops[0]->sym->m_src);
}

/* Helper function for accel_dpdk_cryptodev_assign_device_qps() */
static void
_check_expected_values(struct accel_dpdk_cryptodev_io_channel *crypto_ch,
		       uint8_t expected_qat_index,
		       uint8_t next_qat_index)
{
	uint32_t num_qpairs;

	memset(crypto_ch->device_qp, 0, sizeof(crypto_ch->device_qp));

	num_qpairs = accel_dpdk_cryptodev_assign_device_qps(crypto_ch);
	CU_ASSERT(num_qpairs == 3);

	SPDK_CU_ASSERT_FATAL(crypto_ch->device_qp[ACCEL_DPDK_CRYPTODEV_DRIVER_QAT] != NULL);
	CU_ASSERT(crypto_ch->device_qp[ACCEL_DPDK_CRYPTODEV_DRIVER_QAT]->index == expected_qat_index);
	CU_ASSERT(crypto_ch->device_qp[ACCEL_DPDK_CRYPTODEV_DRIVER_QAT]->in_use == true);
	CU_ASSERT(g_next_qat_index == next_qat_index);
	SPDK_CU_ASSERT_FATAL(crypto_ch->device_qp[ACCEL_DPDK_CRYPTODEV_DRIVER_AESNI_MB] != NULL);
	CU_ASSERT(crypto_ch->device_qp[ACCEL_DPDK_CRYPTODEV_DRIVER_AESNI_MB]->in_use == true);
	SPDK_CU_ASSERT_FATAL(crypto_ch->device_qp[ACCEL_DPDK_CRYPTODEV_DRIVER_MLX5_PCI] != NULL);
	CU_ASSERT(crypto_ch->device_qp[ACCEL_DPDK_CRYPTODEV_DRIVER_MLX5_PCI]->in_use == true);
}

static void
test_assign_device_qp(void)
{
	struct accel_dpdk_cryptodev_device qat_dev = {
		.type = ACCEL_DPDK_CRYPTODEV_DRIVER_QAT,
		.qpairs = TAILQ_HEAD_INITIALIZER(qat_dev.qpairs)
	};
	struct accel_dpdk_cryptodev_device aesni_dev = {
		.type = ACCEL_DPDK_CRYPTODEV_DRIVER_AESNI_MB,
		.qpairs = TAILQ_HEAD_INITIALIZER(aesni_dev.qpairs)
	};
	struct accel_dpdk_cryptodev_device mlx5_dev = {
		.type = ACCEL_DPDK_CRYPTODEV_DRIVER_MLX5_PCI,
		.qpairs = TAILQ_HEAD_INITIALIZER(mlx5_dev.qpairs)
	};
	struct accel_dpdk_cryptodev_qp *qat_qps;
	struct accel_dpdk_cryptodev_qp aesni_qps[4] = {};
	struct accel_dpdk_cryptodev_qp mlx5_qps[4] = {};
	struct accel_dpdk_cryptodev_io_channel io_ch = {};
	TAILQ_HEAD(, accel_dpdk_cryptodev_device) devs_tmp = TAILQ_HEAD_INITIALIZER(devs_tmp);
	int i;

	g_qat_total_qp = 96;
	qat_qps = calloc(g_qat_total_qp, sizeof(*qat_qps));
	SPDK_CU_ASSERT_FATAL(qat_qps != NULL);

	for (i = 0; i < 4; i++) {
		aesni_qps[i].index = i;
		aesni_qps[i].device = &aesni_dev;
		TAILQ_INSERT_TAIL(&aesni_dev.qpairs, &aesni_qps[i], link);

		mlx5_qps[i].index = i;
		mlx5_qps[i].device = &mlx5_dev;
		TAILQ_INSERT_TAIL(&mlx5_dev.qpairs, &mlx5_qps[i], link);
	}
	for (i = 0; i < g_qat_total_qp; i++) {
		qat_qps[i].index = i;
		qat_qps[i].device = &qat_dev;
		TAILQ_INSERT_TAIL(&qat_dev.qpairs, &qat_qps[i], link);
	}

	/* Swap g_crypto_devices so that other tests are not affected */
	TAILQ_SWAP(&g_crypto_devices, &devs_tmp, accel_dpdk_cryptodev_device, link);

	TAILQ_INSERT_TAIL(&g_crypto_devices, &qat_dev, link);
	TAILQ_INSERT_TAIL(&g_crypto_devices, &aesni_dev, link);
	TAILQ_INSERT_TAIL(&g_crypto_devices, &mlx5_dev, link);

	/* QAT testing is more complex as the code under test load balances by
	 * assigning each subsequent device/qp to every QAT_VF_SPREAD modulo
	 * g_qat_total_qp. For the current latest QAT we'll have 48 virtual functions
	 * each with 2 qp so the "spread" between assignments is 32. */

	/* First assignment will assign to 0 and next at 32. */
	_check_expected_values(&io_ch, 0, ACCEL_DPDK_CRYPTODEV_QAT_VF_SPREAD);

	/* Second assignment will assign to 32 and next at 64. */
	_check_expected_values(&io_ch, ACCEL_DPDK_CRYPTODEV_QAT_VF_SPREAD,
			       ACCEL_DPDK_CRYPTODEV_QAT_VF_SPREAD * 2);

	/* Third assignment will assign to 64 and next at 0. */
	_check_expected_values(&io_ch, ACCEL_DPDK_CRYPTODEV_QAT_VF_SPREAD * 2, 0);

	/* Fourth assignment will assign to 1 and next at 33. */
	_check_expected_values(&io_ch, 1, ACCEL_DPDK_CRYPTODEV_QAT_VF_SPREAD + 1);

	TAILQ_SWAP(&devs_tmp, &g_crypto_devices, accel_dpdk_cryptodev_device, link);

	free(qat_qps);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("dpdk_cryptodev", test_setup, test_cleanup);
	CU_ADD_TEST(suite, test_error_paths);
	CU_ADD_TEST(suite, test_simple_encrypt);
	CU_ADD_TEST(suite, test_simple_decrypt);
	CU_ADD_TEST(suite, test_large_enc_dec);
	CU_ADD_TEST(suite, test_dev_full);
	CU_ADD_TEST(suite, test_crazy_rw);
	CU_ADD_TEST(suite, test_initdrivers);
	CU_ADD_TEST(suite, test_supported_opcodes);
	CU_ADD_TEST(suite, test_poller);
	CU_ADD_TEST(suite, test_assign_device_qp);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
