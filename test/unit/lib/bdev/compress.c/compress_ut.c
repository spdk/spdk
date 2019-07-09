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
#include "spdk/reduce.h"

#include <rte_compressdev.h>

struct spdk_bdev_io *g_bdev_io;
struct spdk_io_channel *g_io_ch;
struct rte_comp_op g_comp_op[2];
struct vbdev_compress g_comp_bdev;
struct comp_device_qp g_device_qp;
struct compress_dev g_device;
struct rte_compressdev_capabilities g_cdev_cap;
static struct rte_mbuf *g_src_mbufs[2];
static struct rte_mbuf *g_dst_mbufs[2];
static struct rte_mbuf g_expected_src_mbufs[2];
static struct rte_mbuf g_expected_dst_mbufs[2];
struct comp_bdev_io *g_io_ctx;
struct comp_io_channel *g_comp_ch;
struct rte_config *g_test_config;

/* Those functions are defined as static inline in DPDK, so we can't
 * mock them straight away. We use defines to redirect them into
 * our custom functions.
 */

static void mock_rte_pktmbuf_attach_extbuf(struct rte_mbuf *m, void *buf_addr, rte_iova_t buf_iova,
		uint16_t buf_len, struct rte_mbuf_ext_shared_info *shinfo);
#define rte_pktmbuf_attach_extbuf mock_rte_pktmbuf_attach_extbuf
static void mock_rte_pktmbuf_attach_extbuf(struct rte_mbuf *m, void *buf_addr, rte_iova_t buf_iova,
		uint16_t buf_len, struct rte_mbuf_ext_shared_info *shinfo)
{
	m->buf_addr = buf_addr;
	m->buf_iova = buf_iova;
	m->buf_len = buf_len;
	m->data_len = m->pkt_len = 0;
}

static char *mock_rte_pktmbuf_append(struct rte_mbuf *m, uint16_t len);
#define rte_pktmbuf_append mock_rte_pktmbuf_append
static char *mock_rte_pktmbuf_append(struct rte_mbuf *m, uint16_t len)
{
	m->pkt_len = m->pkt_len + len;
	return NULL;
}

static inline int mock_rte_pktmbuf_chain(struct rte_mbuf *head, struct rte_mbuf *tail);
#define rte_pktmbuf_chain mock_rte_pktmbuf_chain
static inline int mock_rte_pktmbuf_chain(struct rte_mbuf *head, struct rte_mbuf *tail)
{
	head->next = tail;
	return 0;
}

uint16_t ut_max_nb_queue_pairs = 0;
void __rte_experimental mock_rte_compressdev_info_get(uint8_t dev_id,
		struct rte_compressdev_info *dev_info);
#define rte_compressdev_info_get mock_rte_compressdev_info_get
void __rte_experimental
mock_rte_compressdev_info_get(uint8_t dev_id, struct rte_compressdev_info *dev_info)
{
	dev_info->max_nb_queue_pairs = ut_max_nb_queue_pairs;
	dev_info->capabilities = &g_cdev_cap;
	dev_info->driver_name = "compress_isal";
}

int ut_rte_compressdev_configure = 0;
int __rte_experimental mock_rte_compressdev_configure(uint8_t dev_id,
		struct rte_compressdev_config *config);
#define rte_compressdev_configure mock_rte_compressdev_configure
int __rte_experimental
mock_rte_compressdev_configure(uint8_t dev_id, struct rte_compressdev_config *config)
{
	return ut_rte_compressdev_configure;
}

int ut_rte_compressdev_queue_pair_setup = 0;
int __rte_experimental mock_rte_compressdev_queue_pair_setup(uint8_t dev_id, uint16_t queue_pair_id,
		uint32_t max_inflight_ops, int socket_id);
#define rte_compressdev_queue_pair_setup mock_rte_compressdev_queue_pair_setup
int __rte_experimental
mock_rte_compressdev_queue_pair_setup(uint8_t dev_id, uint16_t queue_pair_id,
				      uint32_t max_inflight_ops, int socket_id)
{
	return ut_rte_compressdev_queue_pair_setup;
}

int ut_rte_compressdev_start = 0;
int __rte_experimental mock_rte_compressdev_start(uint8_t dev_id);
#define rte_compressdev_start mock_rte_compressdev_start
int __rte_experimental
mock_rte_compressdev_start(uint8_t dev_id)
{
	return ut_rte_compressdev_start;
}

int ut_rte_compressdev_private_xform_create = 0;
int __rte_experimental mock_rte_compressdev_private_xform_create(uint8_t dev_id,
		const struct rte_comp_xform *xform, void **private_xform);
#define rte_compressdev_private_xform_create mock_rte_compressdev_private_xform_create
int __rte_experimental
mock_rte_compressdev_private_xform_create(uint8_t dev_id,
		const struct rte_comp_xform *xform, void **private_xform)
{
	return ut_rte_compressdev_private_xform_create;
}

uint8_t ut_rte_compressdev_count = 0;
uint8_t __rte_experimental mock_rte_compressdev_count(void);
#define rte_compressdev_count mock_rte_compressdev_count
uint8_t __rte_experimental
mock_rte_compressdev_count(void)
{
	return ut_rte_compressdev_count;
}

struct rte_mempool *ut_rte_comp_op_pool_create = NULL;
struct rte_mempool *__rte_experimental mock_rte_comp_op_pool_create(const char *name,
		unsigned int nb_elts, unsigned int cache_size, uint16_t user_size,
		int socket_id);
#define rte_comp_op_pool_create mock_rte_comp_op_pool_create
struct rte_mempool *__rte_experimental
mock_rte_comp_op_pool_create(const char *name, unsigned int nb_elts,
			     unsigned int cache_size, uint16_t user_size, int socket_id)
{
	return ut_rte_comp_op_pool_create;
}

void mock_rte_pktmbuf_free(struct rte_mbuf *m);
#define rte_pktmbuf_free mock_rte_pktmbuf_free
void mock_rte_pktmbuf_free(struct rte_mbuf *m)
{
}

static int ut_rte_pktmbuf_alloc_bulk = 0;
int mock_rte_pktmbuf_alloc_bulk(struct rte_mempool *pool, struct rte_mbuf **mbufs,
				unsigned count);
#define rte_pktmbuf_alloc_bulk mock_rte_pktmbuf_alloc_bulk
int mock_rte_pktmbuf_alloc_bulk(struct rte_mempool *pool, struct rte_mbuf **mbufs,
				unsigned count)
{
	/* This mocked function only supports the alloc of 2 src and 2 dst. */
	CU_ASSERT(count == 2);
	ut_rte_pktmbuf_alloc_bulk += count;
	if (ut_rte_pktmbuf_alloc_bulk == 2) {
		*mbufs++ = g_src_mbufs[0];
		*mbufs = g_src_mbufs[1];
	} else if (ut_rte_pktmbuf_alloc_bulk == 4) {
		*mbufs++ = g_dst_mbufs[0];
		*mbufs = g_dst_mbufs[1];
		ut_rte_pktmbuf_alloc_bulk = 0;
	} else {
		return -1;
	}
	return 0;
}

struct rte_mempool *
rte_pktmbuf_pool_create(const char *name, unsigned n, unsigned cache_size,
			uint16_t priv_size, uint16_t data_room_size, int socket_id)
{
	struct spdk_mempool *tmp;

	tmp = spdk_mempool_create("mbuf_mp", 1024, sizeof(struct rte_mbuf),
				  SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
				  SPDK_ENV_SOCKET_ID_ANY);

	return (struct rte_mempool *)tmp;
}

void
rte_mempool_free(struct rte_mempool *mp)
{
	if (mp) {
		spdk_mempool_free((struct spdk_mempool *)mp);
	}
}

static int ut_spdk_reduce_vol_op_complete_err = 0;
void
spdk_reduce_vol_writev(struct spdk_reduce_vol *vol, struct iovec *iov, int iovcnt,
		       uint64_t offset, uint64_t length, spdk_reduce_vol_op_complete cb_fn,
		       void *cb_arg)
{
	cb_fn(cb_arg, ut_spdk_reduce_vol_op_complete_err);
}

void
spdk_reduce_vol_readv(struct spdk_reduce_vol *vol, struct iovec *iov, int iovcnt,
		      uint64_t offset, uint64_t length, spdk_reduce_vol_op_complete cb_fn,
		      void *cb_arg)
{
	cb_fn(cb_arg, ut_spdk_reduce_vol_op_complete_err);
}

#include "bdev/compress/vbdev_compress.c"

/* SPDK stubs */
DEFINE_STUB_V(spdk_bdev_module_list_add, (struct spdk_bdev_module *bdev_module));
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *g_bdev_io));
DEFINE_STUB(spdk_bdev_io_type_supported, bool, (struct spdk_bdev *bdev,
		enum spdk_bdev_io_type io_type), 0);
DEFINE_STUB_V(spdk_bdev_module_release_bdev, (struct spdk_bdev *bdev));
DEFINE_STUB_V(spdk_bdev_close, (struct spdk_bdev_desc *desc));
DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_get_io_channel, struct spdk_io_channel *, (struct spdk_bdev_desc *desc), 0);
DEFINE_STUB_V(spdk_bdev_unregister, (struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn,
				     void *cb_arg));
DEFINE_STUB(spdk_bdev_open, int, (struct spdk_bdev *bdev, bool write,
				  spdk_bdev_remove_cb_t remove_cb,
				  void *remove_ctx, struct spdk_bdev_desc **_desc), 0);
DEFINE_STUB(spdk_bdev_module_claim_bdev, int, (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
		struct spdk_bdev_module *module), 0);
DEFINE_STUB_V(spdk_bdev_module_examine_done, (struct spdk_bdev_module *module));
DEFINE_STUB(spdk_bdev_register, int, (struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_get_by_name, struct spdk_bdev *, (const char *bdev_name), NULL);
DEFINE_STUB(spdk_bdev_io_get_io_channel, struct spdk_io_channel *, (struct spdk_bdev_io *bdev_io),
	    0);
DEFINE_STUB(spdk_bdev_queue_io_wait, int, (struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		struct spdk_bdev_io_wait_entry *entry), 0);
DEFINE_STUB_V(spdk_reduce_vol_unload, (struct spdk_reduce_vol *vol,
				       spdk_reduce_vol_op_complete cb_fn, void *cb_arg));
DEFINE_STUB_V(spdk_reduce_vol_load, (struct spdk_reduce_backing_dev *backing_dev,
				     spdk_reduce_vol_op_with_handle_complete cb_fn, void *cb_arg));
DEFINE_STUB(spdk_reduce_vol_get_params, const struct spdk_reduce_vol_params *,
	    (struct spdk_reduce_vol *vol), NULL);

/* DPDK stubs */
DEFINE_STUB(rte_socket_id, unsigned, (void), 0);
DEFINE_STUB(rte_eal_get_configuration, struct rte_config *, (void), NULL);
DEFINE_STUB(rte_vdev_init, int, (const char *name, const char *args), 0);
DEFINE_STUB_V(rte_comp_op_free, (struct rte_comp_op *op));
DEFINE_STUB(rte_comp_op_alloc, struct rte_comp_op *, (struct rte_mempool *mempool), NULL);

void
spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb, uint64_t len)
{
	cb(g_io_ch, g_bdev_io, true);
}

/* Mock these functions to call the callback and then return the value we require */
int ut_spdk_bdev_readv_blocks = 0;
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

static uint16_t ut_rte_compressdev_dequeue_burst = 0;
uint16_t
rte_compressdev_dequeue_burst(uint8_t dev_id, uint16_t qp_id, struct rte_comp_op **ops,
			      uint16_t nb_op)
{
	if (ut_rte_compressdev_dequeue_burst == 0) {
		return 0;
	}

	ops[0] = &g_comp_op[0];
	ops[1] = &g_comp_op[1];

	return ut_rte_compressdev_dequeue_burst;
}

static int ut_compress_done[2];
/* done_count and done_idx together control which expected assertion
 * value to use when dequeuing 2 operations.
 */
static uint16_t done_count = 1;
static uint16_t done_idx = 0;
static void
_compress_done(void *_req, int reduce_errno)
{
	if (done_count == 1) {
		CU_ASSERT(reduce_errno == ut_compress_done[0]);
	} else if (done_count == 2) {
		CU_ASSERT(reduce_errno == ut_compress_done[done_idx++]);
	}
}

#define FAKE_ENQUEUE_SUCCESS 255
static uint16_t ut_enqueue_value = 0;
static struct rte_comp_op ut_expected_op;
uint16_t
rte_compressdev_enqueue_burst(uint8_t dev_id, uint16_t qp_id, struct rte_comp_op **ops,
			      uint16_t nb_ops)
{
	struct rte_comp_op *op = *ops;

	if (ut_enqueue_value == 0) {
		return 0;
	}

	if (ut_enqueue_value == FAKE_ENQUEUE_SUCCESS) {
		return 1;
	}
	/* by design the compress module will never send more than 1 op at a time */
	CU_ASSERT(op->private_xform == ut_expected_op.private_xform);

	/* check src mbuf values, some that are faked in our stub are done so
	 * to indirectly test functionality in the code under test.
	 */
	CU_ASSERT(op->m_src->buf_addr == ut_expected_op.m_src->buf_addr);
	CU_ASSERT(op->m_src->buf_iova == ut_expected_op.m_src->buf_iova);
	CU_ASSERT(op->m_src->buf_len == ut_expected_op.m_src->buf_len);
	CU_ASSERT(op->m_src->pkt_len == ut_expected_op.m_src->pkt_len);
	CU_ASSERT(op->m_src->userdata == ut_expected_op.m_src->userdata);
	CU_ASSERT(op->src.offset == ut_expected_op.src.offset);
	CU_ASSERT(op->src.length == ut_expected_op.src.length);

	/* check dst mbuf values */
	CU_ASSERT(op->m_dst->buf_addr == ut_expected_op.m_dst->buf_addr);
	CU_ASSERT(op->m_dst->buf_iova == ut_expected_op.m_dst->buf_iova);
	CU_ASSERT(op->m_dst->buf_len == ut_expected_op.m_dst->buf_len);
	CU_ASSERT(op->m_dst->pkt_len == ut_expected_op.m_dst->pkt_len);
	CU_ASSERT(op->dst.offset == ut_expected_op.dst.offset);

	return ut_enqueue_value;
}

/* Global setup for all tests that share a bunch of preparation... */
static int
test_setup(void)
{
	g_mbuf_mp = rte_pktmbuf_pool_create("mbuf_mp", NUM_MBUFS, POOL_CACHE_SIZE,
					    sizeof(struct rte_mbuf), 0, rte_socket_id());
	assert(g_mbuf_mp != NULL);

	g_comp_bdev.backing_dev.unmap = _comp_reduce_unmap;
	g_comp_bdev.backing_dev.readv = _comp_reduce_readv;
	g_comp_bdev.backing_dev.writev = _comp_reduce_writev;
	g_comp_bdev.backing_dev.compress = _comp_reduce_compress;
	g_comp_bdev.backing_dev.decompress = _comp_reduce_decompress;
	g_comp_bdev.backing_dev.blocklen = 512;
	g_comp_bdev.backing_dev.blockcnt = 1024 * 16;

	g_comp_bdev.device_qp = &g_device_qp;
	g_comp_bdev.device_qp->device = &g_device;

	TAILQ_INIT(&g_comp_bdev.queued_comp_ops);

	g_comp_xform = (struct rte_comp_xform) {
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

	g_decomp_xform = (struct rte_comp_xform) {
		.type = RTE_COMP_DECOMPRESS,
		.decompress = {
			.algo = RTE_COMP_ALGO_DEFLATE,
			.chksum = RTE_COMP_CHECKSUM_NONE,
			.window_size = DEFAULT_WINDOW_SIZE,
			.hash_algo = RTE_COMP_HASH_ALGO_NONE
		}
	};
	g_device.comp_xform = &g_comp_xform;
	g_device.decomp_xform = &g_decomp_xform;
	g_cdev_cap.comp_feature_flags = RTE_COMP_FF_SHAREABLE_PRIV_XFORM;
	g_device.cdev_info.driver_name = "compress_isal";
	g_device.cdev_info.capabilities = &g_cdev_cap;

	g_src_mbufs[0] = calloc(1, sizeof(struct rte_mbuf));
	g_src_mbufs[1] = calloc(1, sizeof(struct rte_mbuf));
	g_dst_mbufs[0] = calloc(1, sizeof(struct rte_mbuf));
	g_dst_mbufs[1] = calloc(1, sizeof(struct rte_mbuf));

	g_bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct comp_bdev_io));
	g_bdev_io->u.bdev.iovs = calloc(128, sizeof(struct iovec));
	g_bdev_io->bdev = &g_comp_bdev.comp_bdev;
	g_io_ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct comp_io_channel));
	g_comp_ch = (struct comp_io_channel *)((uint8_t *)g_io_ch + sizeof(struct spdk_io_channel));
	g_io_ctx = (struct comp_bdev_io *)g_bdev_io->driver_ctx;

	g_io_ctx->comp_ch = g_comp_ch;
	g_io_ctx->comp_bdev = &g_comp_bdev;
	g_comp_bdev.device_qp = &g_device_qp;

	g_test_config = calloc(1, sizeof(struct rte_config));
	g_test_config->lcore_count = 1;

	return 0;
}

/* Global teardown for all tests */
static int
test_cleanup(void)
{
	spdk_mempool_free((struct spdk_mempool *)g_mbuf_mp);
	free(g_dst_mbufs[0]);
	free(g_src_mbufs[0]);
	free(g_dst_mbufs[1]);
	free(g_src_mbufs[1]);
	free(g_bdev_io->u.bdev.iovs);
	free(g_bdev_io);
	free(g_io_ch);
	free(g_test_config);
	return 0;
}

static void
test_compress_operation(void)
{
	struct iovec src_iovs[2] = {};
	int src_iovcnt;
	struct iovec dst_iovs[2] = {};
	int dst_iovcnt;
	struct spdk_reduce_vol_cb_args cb_arg;
	int rc;
	struct vbdev_comp_op *op;

	src_iovcnt = dst_iovcnt = 2;
	src_iovs[0].iov_len = 1024 * 4;
	dst_iovs[0].iov_len = 1024 * 4;

	src_iovs[1].iov_len = 1024 * 2;
	dst_iovs[1].iov_len = 1024 * 2;

	src_iovs[0].iov_base = (void *)0xfeedbeef;
	dst_iovs[0].iov_base = (void *)0xdeadbeef;

	src_iovs[1].iov_base = (void *)0xdeadbeef;
	dst_iovs[1].iov_base = (void *)0xfeedbeef;

	/* test rte_comp_op_alloc failure */
	MOCK_SET(rte_comp_op_alloc, NULL);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == true);
	rc = _compress_operation(&g_comp_bdev.backing_dev, &src_iovs[0], src_iovcnt,
				 &dst_iovs[0], dst_iovcnt, true, &cb_arg);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == false);
	while (!TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops)) {
		op = TAILQ_FIRST(&g_comp_bdev.queued_comp_ops);
		TAILQ_REMOVE(&g_comp_bdev.queued_comp_ops, op, link);
		free(op);
	}
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == true);
	CU_ASSERT(rc == 0);
	MOCK_SET(rte_comp_op_alloc, &g_comp_op[0]);

	/* test mempool get failure */
	ut_rte_pktmbuf_alloc_bulk = -1;
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == true);
	rc = _compress_operation(&g_comp_bdev.backing_dev, &src_iovs[0], src_iovcnt,
				 &dst_iovs[0], dst_iovcnt, true, &cb_arg);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == false);
	while (!TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops)) {
		op = TAILQ_FIRST(&g_comp_bdev.queued_comp_ops);
		TAILQ_REMOVE(&g_comp_bdev.queued_comp_ops, op, link);
		free(op);
	}
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == true);
	CU_ASSERT(rc == 0);
	ut_rte_pktmbuf_alloc_bulk = 0;

	/* test enqueue failure */
	ut_enqueue_value = 0;
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == true);
	rc = _compress_operation(&g_comp_bdev.backing_dev, &src_iovs[0], src_iovcnt,
				 &dst_iovs[0], dst_iovcnt, true, &cb_arg);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == false);
	while (!TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops)) {
		op = TAILQ_FIRST(&g_comp_bdev.queued_comp_ops);
		TAILQ_REMOVE(&g_comp_bdev.queued_comp_ops, op, link);
		free(op);
	}
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == true);
	CU_ASSERT(rc == 0);
	ut_enqueue_value = 1;

	/* test success with 2 vector iovec */
	ut_expected_op.private_xform = &g_decomp_xform;
	ut_expected_op.src.offset = 0;
	ut_expected_op.src.length = src_iovs[0].iov_len + src_iovs[1].iov_len;
	ut_expected_op.m_src = &g_expected_src_mbufs[0];
	ut_expected_op.m_src->buf_addr = src_iovs[0].iov_base;
	ut_expected_op.m_src->next = &g_expected_src_mbufs[1];
	ut_expected_op.m_src->next->buf_addr = src_iovs[1].iov_base;
	ut_expected_op.m_src->buf_iova = spdk_vtophys((void *)ut_expected_op.m_src->buf_addr, NULL);
	ut_expected_op.m_src->buf_len = src_iovs[0].iov_len;
	ut_expected_op.m_src->pkt_len = src_iovs[0].iov_len;
	ut_expected_op.m_src->userdata = &cb_arg;

	ut_expected_op.dst.offset = 0;
	ut_expected_op.m_dst = &g_expected_dst_mbufs[0];
	ut_expected_op.m_dst->buf_addr = dst_iovs[0].iov_base;
	ut_expected_op.m_dst->next = &g_expected_dst_mbufs[1];
	ut_expected_op.m_dst->next->buf_addr = dst_iovs[1].iov_base;
	ut_expected_op.m_dst->buf_iova = spdk_vtophys((void *)ut_expected_op.m_dst->buf_addr, NULL);
	ut_expected_op.m_dst->buf_len = dst_iovs[0].iov_len;
	ut_expected_op.m_dst->pkt_len = dst_iovs[0].iov_len;

	rc = _compress_operation(&g_comp_bdev.backing_dev, &src_iovs[0], src_iovcnt,
				 &dst_iovs[0], dst_iovcnt, false, &cb_arg);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == true);
	CU_ASSERT(rc == 0);
}

static void
test_poller(void)
{
	int rc;
	struct spdk_reduce_vol_cb_args *cb_args;
	struct rte_mbuf mbuf[2];
	struct vbdev_comp_op *op_to_queue;
	struct iovec src_iovs[2] = {};
	struct iovec dst_iovs[2] = {};

	cb_args = calloc(1, sizeof(*cb_args));
	SPDK_CU_ASSERT_FATAL(cb_args != NULL);
	cb_args->cb_fn = _compress_done;
	memset(&g_comp_op[0], 0, sizeof(struct rte_comp_op));
	g_comp_op[0].m_src = &mbuf[0];
	g_comp_op[1].m_src = &mbuf[1];

	/* Error from dequeue, nothing needing to be resubmitted.
	 */
	ut_rte_compressdev_dequeue_burst = 1;
	/* setup what we want dequeue to return for the op */
	g_comp_op[0].m_src->userdata = (void *)cb_args;
	g_comp_op[0].produced = 1;
	g_comp_op[0].status = 1;
	/* value asserted in the reduce callback */
	ut_compress_done[0] = -EINVAL;
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == true);
	rc = comp_dev_poller((void *)&g_comp_bdev);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == true);
	CU_ASSERT(rc == 0);

	/* Success from dequeue, 2 ops. nothing needing to be resubmitted.
	 */
	ut_rte_compressdev_dequeue_burst = 2;
	/* setup what we want dequeue to return for the op */
	g_comp_op[0].m_src->userdata = (void *)cb_args;
	g_comp_op[0].produced = 16;
	g_comp_op[0].status = 0;
	g_comp_op[1].m_src->userdata = (void *)cb_args;
	g_comp_op[1].produced = 32;
	g_comp_op[1].status = 0;
	/* value asserted in the reduce callback */
	ut_compress_done[0] = 16;
	ut_compress_done[1] = 32;
	done_count = 2;
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == true);
	rc = comp_dev_poller((void *)&g_comp_bdev);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == true);
	CU_ASSERT(rc == 0);

	/* Success from dequeue, one op to be resubmitted.
	 */
	ut_rte_compressdev_dequeue_burst = 1;
	/* setup what we want dequeue to return for the op */
	g_comp_op[0].m_src->userdata = (void *)cb_args;
	g_comp_op[0].produced = 16;
	g_comp_op[0].status = 0;
	/* value asserted in the reduce callback */
	ut_compress_done[0] = 16;
	done_count = 1;
	op_to_queue = calloc(1, sizeof(struct vbdev_comp_op));
	SPDK_CU_ASSERT_FATAL(op_to_queue != NULL);
	op_to_queue->backing_dev = &g_comp_bdev.backing_dev;
	op_to_queue->src_iovs = &src_iovs[0];
	op_to_queue->src_iovcnt = 2;
	op_to_queue->dst_iovs = &dst_iovs[0];
	op_to_queue->dst_iovcnt = 2;
	op_to_queue->compress = true;
	op_to_queue->cb_arg = cb_args;
	ut_enqueue_value = FAKE_ENQUEUE_SUCCESS;
	TAILQ_INSERT_TAIL(&g_comp_bdev.queued_comp_ops,
			  op_to_queue,
			  link);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == false);
	rc = comp_dev_poller((void *)&g_comp_bdev);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == true);
	CU_ASSERT(rc == 0);

	/* op_to_queue is freed in code under test */
	free(cb_args);
}

static void
test_vbdev_compress_submit_request(void)
{
	/* Single element block size write */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
	g_completion_called = false;
	MOCK_SET(spdk_bdev_io_get_io_channel, g_io_ch);
	vbdev_compress_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_completion_called == true);
	CU_ASSERT(g_io_ctx->orig_io == g_bdev_io);
	CU_ASSERT(g_io_ctx->comp_bdev == &g_comp_bdev);
	CU_ASSERT(g_io_ctx->comp_ch == g_comp_ch);

	/* same write but now fail it */
	ut_spdk_reduce_vol_op_complete_err = 1;
	g_completion_called = false;
	vbdev_compress_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	CU_ASSERT(g_completion_called == true);

	/* test a read success */
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
	ut_spdk_reduce_vol_op_complete_err = 0;
	g_completion_called = false;
	vbdev_compress_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_completion_called == true);

	/* test a read failure */
	ut_spdk_reduce_vol_op_complete_err = 1;
	g_completion_called = false;
	vbdev_compress_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	CU_ASSERT(g_completion_called == true);
}

static void
test_passthru(void)
{

}

static void
test_reset(void)
{
	/* TODO: There are a few different ways to do this given that
	 * the code uses spdk_for_each_channel() to implement reset
	 * handling. SUbmitting w/o UT for this function for now and
	 * will follow up with something shortly.
	 */
}

static void
test_initdrivers(void)
{
	int rc;
	static struct rte_mempool *orig_mbuf_mp;
	struct comp_device_qp *dev_qp;
	struct comp_device_qp *tmp_qp;

	orig_mbuf_mp = g_mbuf_mp;
	g_mbuf_mp = NULL;

	/* test return values from rte_vdev_init() */
	MOCK_SET(rte_eal_get_configuration, g_test_config);
	MOCK_SET(rte_vdev_init, -EEXIST);
	rc = vbdev_init_compress_drivers();
	/* This is not an error condition, we already have one */
	CU_ASSERT(rc == 0);

	/* success */
	MOCK_SET(rte_vdev_init, 0);
	rc = vbdev_init_compress_drivers();
	CU_ASSERT(rc == 0);

	/* error */
	MOCK_SET(rte_vdev_init, -2);
	rc = vbdev_init_compress_drivers();
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_mbuf_mp == NULL);
	CU_ASSERT(g_comp_op_mp == NULL);

	/* compressdev count 0 */
	ut_rte_compressdev_count = 0;
	MOCK_SET(rte_vdev_init, 0);
	rc = vbdev_init_compress_drivers();
	CU_ASSERT(rc == 0);

	/* bogus count */
	ut_rte_compressdev_count = RTE_COMPRESS_MAX_DEVS + 1;
	rc = vbdev_init_compress_drivers();
	CU_ASSERT(rc == -EINVAL);

	/* can't get mbuf pool */
	ut_rte_compressdev_count = 1;
	MOCK_SET(spdk_mempool_create, NULL);
	rc = vbdev_init_compress_drivers();
	CU_ASSERT(rc == -ENOMEM);
	MOCK_CLEAR(spdk_mempool_create);

	/* can't get comp op pool */
	ut_rte_comp_op_pool_create = NULL;
	rc = vbdev_init_compress_drivers();
	CU_ASSERT(rc == -ENOMEM);

	/* error on create_compress_dev() */
	ut_rte_comp_op_pool_create = (struct rte_mempool *)&test_initdrivers;
	ut_rte_compressdev_configure = -1;
	rc = vbdev_init_compress_drivers();
	CU_ASSERT(rc == -1);

	/* error on create_compress_dev() but coverage for large num queues */
	ut_max_nb_queue_pairs = 99;
	rc = vbdev_init_compress_drivers();
	CU_ASSERT(rc == -1);

	/* qpair setup fails */
	ut_rte_compressdev_configure = 0;
	ut_max_nb_queue_pairs = 0;
	ut_rte_compressdev_queue_pair_setup = -1;
	rc = vbdev_init_compress_drivers();
	CU_ASSERT(rc == -EINVAL);

	/* rte_compressdev_start fails */
	ut_rte_compressdev_queue_pair_setup = 0;
	ut_rte_compressdev_start = -1;
	rc = vbdev_init_compress_drivers();
	CU_ASSERT(rc == -1);

	/* rte_compressdev_private_xform_create() fails */
	ut_rte_compressdev_start = 0;
	ut_rte_compressdev_private_xform_create = -2;
	rc = vbdev_init_compress_drivers();
	CU_ASSERT(rc == -2);

	/* rte_compressdev_private_xform_create()succeeds */
	ut_rte_compressdev_start = 0;
	ut_rte_compressdev_private_xform_create = 0;
	rc = vbdev_init_compress_drivers();
	CU_ASSERT(rc == 0);

	TAILQ_FOREACH_SAFE(dev_qp, &g_comp_device_qp, link, tmp_qp) {
		TAILQ_REMOVE(&g_comp_device_qp, dev_qp, link);
		free(dev_qp->device);
		free(dev_qp);
	}

	free(g_mbuf_mp);
	g_mbuf_mp = orig_mbuf_mp;
}

static void
test_supported_io(void)
{

}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("compress", test_setup, test_cleanup);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (CU_add_test(suite, "test_compress_operation",
			test_compress_operation) == NULL ||
	    CU_add_test(suite, "vbdev_compress_submit_request",
			test_vbdev_compress_submit_request) == NULL ||
	    CU_add_test(suite, "test_passthru",
			test_passthru) == NULL ||
	    CU_add_test(suite, "test_initdrivers",
			test_initdrivers) == NULL ||
	    CU_add_test(suite, "test_supported_io",
			test_supported_io) == NULL ||
	    CU_add_test(suite, "test_poller",
			test_poller) == NULL ||
	    CU_add_test(suite, "test_reset",
			test_reset) == NULL
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
