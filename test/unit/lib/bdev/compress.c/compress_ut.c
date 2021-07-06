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
/* We have our own mock for this */
#define UNIT_TEST_NO_VTOPHYS
#include "common/lib/test_env.c"
#include "spdk_internal/mock.h"
#include "thread/thread_internal.h"
#include "unit/lib/json_mock.c"
#include "spdk/reduce.h"

#include <rte_compressdev.h>

/* There will be one if the data perfectly matches the chunk size,
 * or there could be an offset into the data and a remainder after
 * the data or both for a max of 3.
 */
#define UT_MBUFS_PER_OP 3
/* For testing the crossing of a huge page boundary on address translation,
 * we'll have an extra one but we only test on the source side.
 */
#define UT_MBUFS_PER_OP_BOUND_TEST 4

struct spdk_bdev_io *g_bdev_io;
struct spdk_io_channel *g_io_ch;
struct rte_comp_op g_comp_op[2];
struct vbdev_compress g_comp_bdev;
struct comp_device_qp g_device_qp;
struct compress_dev g_device;
struct rte_compressdev_capabilities g_cdev_cap;
static struct rte_mbuf *g_src_mbufs[UT_MBUFS_PER_OP_BOUND_TEST];
static struct rte_mbuf *g_dst_mbufs[UT_MBUFS_PER_OP];
static struct rte_mbuf g_expected_src_mbufs[UT_MBUFS_PER_OP_BOUND_TEST];
static struct rte_mbuf g_expected_dst_mbufs[UT_MBUFS_PER_OP];
struct comp_bdev_io *g_io_ctx;
struct comp_io_channel *g_comp_ch;

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
	assert(m != NULL);
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
	struct rte_mbuf *cur_tail;

	cur_tail = rte_pktmbuf_lastseg(head);
	cur_tail->next = tail;

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

static bool ut_boundary_alloc = false;
static int ut_rte_pktmbuf_alloc_bulk = 0;
int mock_rte_pktmbuf_alloc_bulk(struct rte_mempool *pool, struct rte_mbuf **mbufs,
				unsigned count);
#define rte_pktmbuf_alloc_bulk mock_rte_pktmbuf_alloc_bulk
int mock_rte_pktmbuf_alloc_bulk(struct rte_mempool *pool, struct rte_mbuf **mbufs,
				unsigned count)
{
	int i;

	/* This mocked function only supports the alloc of up to 3 src and 3 dst. */
	ut_rte_pktmbuf_alloc_bulk += count;

	if (ut_rte_pktmbuf_alloc_bulk == 1) {
		/* allocation of an extra mbuf for boundary cross test */
		ut_boundary_alloc = true;
		g_src_mbufs[UT_MBUFS_PER_OP_BOUND_TEST - 1]->next = NULL;
		*mbufs = g_src_mbufs[UT_MBUFS_PER_OP_BOUND_TEST - 1];
		ut_rte_pktmbuf_alloc_bulk = 0;
	} else if (ut_rte_pktmbuf_alloc_bulk == UT_MBUFS_PER_OP) {
		/* first test allocation, src mbufs */
		for (i = 0; i < UT_MBUFS_PER_OP; i++) {
			g_src_mbufs[i]->next = NULL;
			*mbufs++ = g_src_mbufs[i];
		}
	} else if (ut_rte_pktmbuf_alloc_bulk == UT_MBUFS_PER_OP * 2) {
		/* second test allocation, dst mbufs */
		for (i = 0; i < UT_MBUFS_PER_OP; i++) {
			g_dst_mbufs[i]->next = NULL;
			*mbufs++ = g_dst_mbufs[i];
		}
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
DEFINE_STUB(spdk_bdev_get_aliases, const struct spdk_bdev_aliases_list *,
	    (const struct spdk_bdev *bdev), NULL);
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
DEFINE_STUB(spdk_bdev_open_ext, int, (const char *bdev_name, bool write,
				      spdk_bdev_event_cb_t event_cb,
				      void *event_ctx, struct spdk_bdev_desc **_desc), 0);
DEFINE_STUB(spdk_bdev_desc_get_bdev, struct spdk_bdev *, (struct spdk_bdev_desc *desc), NULL);
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
DEFINE_STUB_V(spdk_reduce_vol_init, (struct spdk_reduce_vol_params *params,
				     struct spdk_reduce_backing_dev *backing_dev,
				     const char *pm_file_dir,
				     spdk_reduce_vol_op_with_handle_complete cb_fn, void *cb_arg));
DEFINE_STUB_V(spdk_reduce_vol_destroy, (struct spdk_reduce_backing_dev *backing_dev,
					spdk_reduce_vol_op_complete cb_fn, void *cb_arg));

/* DPDK stubs */
#define DPDK_DYNFIELD_OFFSET offsetof(struct rte_mbuf, dynfield1[1])
DEFINE_STUB(rte_mbuf_dynfield_register, int, (const struct rte_mbuf_dynfield *params),
	    DPDK_DYNFIELD_OFFSET);
DEFINE_STUB(rte_socket_id, unsigned, (void), 0);
DEFINE_STUB(rte_vdev_init, int, (const char *name, const char *args), 0);
DEFINE_STUB_V(rte_comp_op_free, (struct rte_comp_op *op));
DEFINE_STUB(rte_comp_op_alloc, struct rte_comp_op *, (struct rte_mempool *mempool), NULL);

int g_small_size_counter = 0;
int g_small_size_modify = 0;
uint64_t g_small_size = 0;
uint64_t
spdk_vtophys(const void *buf, uint64_t *size)
{
	g_small_size_counter++;
	if (g_small_size_counter == g_small_size_modify) {
		*size = g_small_size;
		g_small_size_counter = 0;
		g_small_size_modify = 0;
	}
	return (uint64_t)buf;
}

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

static void
_get_mbuf_array(struct rte_mbuf **mbuf_array, struct rte_mbuf *mbuf_head,
		int mbuf_count, bool null_final)
{
	int i;

	for (i = 0; i < mbuf_count; i++) {
		mbuf_array[i] = mbuf_head;
		if (mbuf_head) {
			mbuf_head = mbuf_head->next;
		}
	}
	if (null_final) {
		mbuf_array[i - 1] = NULL;
	}
}

#define FAKE_ENQUEUE_SUCCESS 255
#define FAKE_ENQUEUE_ERROR 128
#define FAKE_ENQUEUE_BUSY 64
static uint16_t ut_enqueue_value = FAKE_ENQUEUE_SUCCESS;
static struct rte_comp_op ut_expected_op;
uint16_t
rte_compressdev_enqueue_burst(uint8_t dev_id, uint16_t qp_id, struct rte_comp_op **ops,
			      uint16_t nb_ops)
{
	struct rte_comp_op *op = *ops;
	struct rte_mbuf *op_mbuf[UT_MBUFS_PER_OP_BOUND_TEST];
	struct rte_mbuf *exp_mbuf[UT_MBUFS_PER_OP_BOUND_TEST];
	int i, num_src_mbufs = UT_MBUFS_PER_OP;

	switch (ut_enqueue_value) {
	case FAKE_ENQUEUE_BUSY:
		op->status = RTE_COMP_OP_STATUS_NOT_PROCESSED;
		return 0;
		break;
	case FAKE_ENQUEUE_SUCCESS:
		op->status = RTE_COMP_OP_STATUS_SUCCESS;
		return 1;
		break;
	case FAKE_ENQUEUE_ERROR:
		op->status = RTE_COMP_OP_STATUS_ERROR;
		return 0;
		break;
	default:
		break;
	}

	/* by design the compress module will never send more than 1 op at a time */
	CU_ASSERT(op->private_xform == ut_expected_op.private_xform);

	/* setup our local pointers to the chained mbufs, those pointed to in the
	 * operation struct and the expected values.
	 */
	_get_mbuf_array(op_mbuf, op->m_src, SPDK_COUNTOF(op_mbuf), true);
	_get_mbuf_array(exp_mbuf, ut_expected_op.m_src, SPDK_COUNTOF(exp_mbuf), true);

	if (ut_boundary_alloc == true) {
		/* if we crossed a boundary, we need to check the 4th src mbuf and
		 * reset the global that is used to identify whether we crossed
		 * or not
		 */
		num_src_mbufs = UT_MBUFS_PER_OP_BOUND_TEST;
		exp_mbuf[UT_MBUFS_PER_OP_BOUND_TEST - 1] = ut_expected_op.m_src->next->next->next;
		op_mbuf[UT_MBUFS_PER_OP_BOUND_TEST - 1] = op->m_src->next->next->next;
		ut_boundary_alloc = false;
	}


	for (i = 0; i < num_src_mbufs; i++) {
		CU_ASSERT(op_mbuf[i]->buf_addr == exp_mbuf[i]->buf_addr);
		CU_ASSERT(op_mbuf[i]->buf_iova == exp_mbuf[i]->buf_iova);
		CU_ASSERT(op_mbuf[i]->buf_len == exp_mbuf[i]->buf_len);
		CU_ASSERT(op_mbuf[i]->pkt_len == exp_mbuf[i]->pkt_len);
	}

	/* if only 3 mbufs were used in the test, the 4th should be zeroed */
	if (num_src_mbufs == UT_MBUFS_PER_OP) {
		CU_ASSERT(op_mbuf[UT_MBUFS_PER_OP_BOUND_TEST - 1] == NULL);
		CU_ASSERT(exp_mbuf[UT_MBUFS_PER_OP_BOUND_TEST - 1] == NULL);
	}
	CU_ASSERT(*RTE_MBUF_DYNFIELD(op->m_src, g_mbuf_offset, uint64_t *) ==
		  *RTE_MBUF_DYNFIELD(ut_expected_op.m_src, g_mbuf_offset, uint64_t *));
	CU_ASSERT(op->src.offset == ut_expected_op.src.offset);
	CU_ASSERT(op->src.length == ut_expected_op.src.length);

	/* check dst mbuf values */
	_get_mbuf_array(op_mbuf, op->m_dst, SPDK_COUNTOF(op_mbuf), true);
	_get_mbuf_array(exp_mbuf, ut_expected_op.m_dst, SPDK_COUNTOF(exp_mbuf), true);

	for (i = 0; i < UT_MBUFS_PER_OP; i++) {
		CU_ASSERT(op_mbuf[i]->buf_addr == exp_mbuf[i]->buf_addr);
		CU_ASSERT(op_mbuf[i]->buf_iova == exp_mbuf[i]->buf_iova);
		CU_ASSERT(op_mbuf[i]->buf_len == exp_mbuf[i]->buf_len);
		CU_ASSERT(op_mbuf[i]->pkt_len == exp_mbuf[i]->pkt_len);
	}
	CU_ASSERT(op->dst.offset == ut_expected_op.dst.offset);

	return ut_enqueue_value;
}

/* Global setup for all tests that share a bunch of preparation... */
static int
test_setup(void)
{
	struct spdk_thread *thread;
	int i;

	spdk_thread_lib_init(NULL, 0);

	thread = spdk_thread_create(NULL, NULL);
	spdk_set_thread(thread);

	g_comp_bdev.reduce_thread = thread;
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
	for (i = 0; i < UT_MBUFS_PER_OP_BOUND_TEST; i++) {
		g_src_mbufs[i] = calloc(1, sizeof(struct rte_mbuf));
	}
	for (i = 0; i < UT_MBUFS_PER_OP; i++) {
		g_dst_mbufs[i] = calloc(1, sizeof(struct rte_mbuf));
	}

	g_bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct comp_bdev_io));
	g_bdev_io->u.bdev.iovs = calloc(128, sizeof(struct iovec));
	g_bdev_io->bdev = &g_comp_bdev.comp_bdev;
	g_io_ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct comp_io_channel));
	g_io_ch->thread = thread;
	g_comp_ch = (struct comp_io_channel *)spdk_io_channel_get_ctx(g_io_ch);
	g_io_ctx = (struct comp_bdev_io *)g_bdev_io->driver_ctx;

	g_io_ctx->comp_ch = g_comp_ch;
	g_io_ctx->comp_bdev = &g_comp_bdev;
	g_comp_bdev.device_qp = &g_device_qp;

	for (i = 0; i < UT_MBUFS_PER_OP_BOUND_TEST - 1; i++) {
		g_expected_src_mbufs[i].next = &g_expected_src_mbufs[i + 1];
	}
	g_expected_src_mbufs[UT_MBUFS_PER_OP_BOUND_TEST - 1].next = NULL;

	/* we only test w/4 mbufs on src side */
	for (i = 0; i < UT_MBUFS_PER_OP - 1; i++) {
		g_expected_dst_mbufs[i].next = &g_expected_dst_mbufs[i + 1];
	}
	g_expected_dst_mbufs[UT_MBUFS_PER_OP - 1].next = NULL;
	g_mbuf_offset = DPDK_DYNFIELD_OFFSET;

	return 0;
}

/* Global teardown for all tests */
static int
test_cleanup(void)
{
	struct spdk_thread *thread;
	int i;

	for (i = 0; i < UT_MBUFS_PER_OP_BOUND_TEST; i++) {
		free(g_src_mbufs[i]);
	}
	for (i = 0; i < UT_MBUFS_PER_OP; i++) {
		free(g_dst_mbufs[i]);
	}
	free(g_bdev_io->u.bdev.iovs);
	free(g_bdev_io);
	free(g_io_ch);

	thread = spdk_get_thread();
	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}
	spdk_thread_destroy(thread);

	spdk_thread_lib_fini();

	return 0;
}

static void
test_compress_operation(void)
{
	struct iovec src_iovs[3] = {};
	int src_iovcnt;
	struct iovec dst_iovs[3] = {};
	int dst_iovcnt;
	struct spdk_reduce_vol_cb_args cb_arg;
	int rc, i;
	struct vbdev_comp_op *op;
	struct rte_mbuf *exp_src_mbuf[UT_MBUFS_PER_OP];
	struct rte_mbuf *exp_dst_mbuf[UT_MBUFS_PER_OP];

	src_iovcnt = dst_iovcnt = 3;
	for (i = 0; i < dst_iovcnt; i++) {
		src_iovs[i].iov_len = 0x1000;
		dst_iovs[i].iov_len = 0x1000;
		src_iovs[i].iov_base = (void *)0x10000000 + 0x1000 * i;
		dst_iovs[i].iov_base = (void *)0x20000000 + 0x1000 * i;
	}

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

	/* test enqueue failure busy */
	ut_enqueue_value = FAKE_ENQUEUE_BUSY;
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

	/* test enqueue failure error */
	ut_enqueue_value = FAKE_ENQUEUE_ERROR;
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == true);
	rc = _compress_operation(&g_comp_bdev.backing_dev, &src_iovs[0], src_iovcnt,
				 &dst_iovs[0], dst_iovcnt, true, &cb_arg);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == true);
	CU_ASSERT(rc == -EINVAL);
	ut_enqueue_value = FAKE_ENQUEUE_SUCCESS;

	/* test success with 3 vector iovec */
	ut_expected_op.private_xform = &g_decomp_xform;
	ut_expected_op.src.offset = 0;
	ut_expected_op.src.length = src_iovs[0].iov_len + src_iovs[1].iov_len + src_iovs[2].iov_len;

	/* setup the src expected values */
	_get_mbuf_array(exp_src_mbuf, &g_expected_src_mbufs[0], SPDK_COUNTOF(exp_src_mbuf), false);
	ut_expected_op.m_src = exp_src_mbuf[0];

	for (i = 0; i < UT_MBUFS_PER_OP; i++) {
		*RTE_MBUF_DYNFIELD(exp_src_mbuf[i], g_mbuf_offset, uint64_t *) = (uint64_t)&cb_arg;
		exp_src_mbuf[i]->buf_addr = src_iovs[i].iov_base;
		exp_src_mbuf[i]->buf_iova = spdk_vtophys(src_iovs[i].iov_base, &src_iovs[i].iov_len);
		exp_src_mbuf[i]->buf_len = src_iovs[i].iov_len;
		exp_src_mbuf[i]->pkt_len = src_iovs[i].iov_len;
	}

	/* setup the dst expected values */
	_get_mbuf_array(exp_dst_mbuf, &g_expected_dst_mbufs[0], SPDK_COUNTOF(exp_dst_mbuf), false);
	ut_expected_op.dst.offset = 0;
	ut_expected_op.m_dst = exp_dst_mbuf[0];

	for (i = 0; i < UT_MBUFS_PER_OP; i++) {
		exp_dst_mbuf[i]->buf_addr = dst_iovs[i].iov_base;
		exp_dst_mbuf[i]->buf_iova = spdk_vtophys(dst_iovs[i].iov_base, &dst_iovs[i].iov_len);
		exp_dst_mbuf[i]->buf_len = dst_iovs[i].iov_len;
		exp_dst_mbuf[i]->pkt_len = dst_iovs[i].iov_len;
	}

	rc = _compress_operation(&g_comp_bdev.backing_dev, &src_iovs[0], src_iovcnt,
				 &dst_iovs[0], dst_iovcnt, false, &cb_arg);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == true);
	CU_ASSERT(rc == 0);

}

static void
test_compress_operation_cross_boundary(void)
{
	struct iovec src_iovs[3] = {};
	int src_iovcnt;
	struct iovec dst_iovs[3] = {};
	int dst_iovcnt;
	struct spdk_reduce_vol_cb_args cb_arg;
	int rc, i;
	struct rte_mbuf *exp_src_mbuf[UT_MBUFS_PER_OP_BOUND_TEST];
	struct rte_mbuf *exp_dst_mbuf[UT_MBUFS_PER_OP_BOUND_TEST];

	/* Setup the same basic 3 IOV test as used in the simple success case
	 * but then we'll start testing a vtophy boundary crossing at each
	 * position.
	 */
	src_iovcnt = dst_iovcnt = 3;
	for (i = 0; i < dst_iovcnt; i++) {
		src_iovs[i].iov_len = 0x1000;
		dst_iovs[i].iov_len = 0x1000;
		src_iovs[i].iov_base = (void *)0x10000000 + 0x1000 * i;
		dst_iovs[i].iov_base = (void *)0x20000000 + 0x1000 * i;
	}

	ut_expected_op.private_xform = &g_decomp_xform;
	ut_expected_op.src.offset = 0;
	ut_expected_op.src.length = src_iovs[0].iov_len + src_iovs[1].iov_len + src_iovs[2].iov_len;

	/* setup the src expected values */
	_get_mbuf_array(exp_src_mbuf, &g_expected_src_mbufs[0], SPDK_COUNTOF(exp_src_mbuf), false);
	ut_expected_op.m_src = exp_src_mbuf[0];

	for (i = 0; i < UT_MBUFS_PER_OP; i++) {
		*RTE_MBUF_DYNFIELD(exp_src_mbuf[i], g_mbuf_offset, uint64_t *) = (uint64_t)&cb_arg;
		exp_src_mbuf[i]->buf_addr = src_iovs[i].iov_base;
		exp_src_mbuf[i]->buf_iova = spdk_vtophys(src_iovs[i].iov_base, &src_iovs[i].iov_len);
		exp_src_mbuf[i]->buf_len = src_iovs[i].iov_len;
		exp_src_mbuf[i]->pkt_len = src_iovs[i].iov_len;
	}

	/* setup the dst expected values, we don't test needing a 4th dst mbuf */
	_get_mbuf_array(exp_dst_mbuf, &g_expected_dst_mbufs[0], SPDK_COUNTOF(exp_dst_mbuf), false);
	ut_expected_op.dst.offset = 0;
	ut_expected_op.m_dst = exp_dst_mbuf[0];

	for (i = 0; i < UT_MBUFS_PER_OP; i++) {
		exp_dst_mbuf[i]->buf_addr = dst_iovs[i].iov_base;
		exp_dst_mbuf[i]->buf_iova = spdk_vtophys(dst_iovs[i].iov_base, &dst_iovs[i].iov_len);
		exp_dst_mbuf[i]->buf_len = dst_iovs[i].iov_len;
		exp_dst_mbuf[i]->pkt_len = dst_iovs[i].iov_len;
	}

	/* force the 1st IOV to get partial length from spdk_vtophys */
	g_small_size_counter = 0;
	g_small_size_modify = 1;
	g_small_size = 0x800;
	*RTE_MBUF_DYNFIELD(exp_src_mbuf[3], g_mbuf_offset, uint64_t *) = (uint64_t)&cb_arg;

	/* first only has shorter length */
	exp_src_mbuf[0]->pkt_len = exp_src_mbuf[0]->buf_len = 0x800;

	/* 2nd was inserted by the boundary crossing condition and finishes off
	 * the length from the first */
	exp_src_mbuf[1]->buf_addr = (void *)0x10000800;
	exp_src_mbuf[1]->buf_iova = 0x10000800;
	exp_src_mbuf[1]->pkt_len = exp_src_mbuf[1]->buf_len = 0x800;

	/* 3rd looks like that the 2nd would have */
	exp_src_mbuf[2]->buf_addr = (void *)0x10001000;
	exp_src_mbuf[2]->buf_iova = 0x10001000;
	exp_src_mbuf[2]->pkt_len = exp_src_mbuf[2]->buf_len = 0x1000;

	/* a new 4th looks like what the 3rd would have */
	exp_src_mbuf[3]->buf_addr = (void *)0x10002000;
	exp_src_mbuf[3]->buf_iova = 0x10002000;
	exp_src_mbuf[3]->pkt_len = exp_src_mbuf[3]->buf_len = 0x1000;

	rc = _compress_operation(&g_comp_bdev.backing_dev, &src_iovs[0], src_iovcnt,
				 &dst_iovs[0], dst_iovcnt, false, &cb_arg);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == true);
	CU_ASSERT(rc == 0);

	/* Now force the 2nd IOV to get partial length from spdk_vtophys */
	g_small_size_counter = 0;
	g_small_size_modify = 2;
	g_small_size = 0x800;

	/* first is normal */
	exp_src_mbuf[0]->buf_addr = (void *)0x10000000;
	exp_src_mbuf[0]->buf_iova = 0x10000000;
	exp_src_mbuf[0]->pkt_len = exp_src_mbuf[0]->buf_len = 0x1000;

	/* second only has shorter length */
	exp_src_mbuf[1]->buf_addr = (void *)0x10001000;
	exp_src_mbuf[1]->buf_iova = 0x10001000;
	exp_src_mbuf[1]->pkt_len = exp_src_mbuf[1]->buf_len = 0x800;

	/* 3rd was inserted by the boundary crossing condition and finishes off
	 * the length from the first */
	exp_src_mbuf[2]->buf_addr = (void *)0x10001800;
	exp_src_mbuf[2]->buf_iova = 0x10001800;
	exp_src_mbuf[2]->pkt_len = exp_src_mbuf[2]->buf_len = 0x800;

	/* a new 4th looks like what the 3rd would have */
	exp_src_mbuf[3]->buf_addr = (void *)0x10002000;
	exp_src_mbuf[3]->buf_iova = 0x10002000;
	exp_src_mbuf[3]->pkt_len = exp_src_mbuf[3]->buf_len = 0x1000;

	rc = _compress_operation(&g_comp_bdev.backing_dev, &src_iovs[0], src_iovcnt,
				 &dst_iovs[0], dst_iovcnt, false, &cb_arg);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == true);
	CU_ASSERT(rc == 0);

	/* Finally force the 3rd IOV to get partial length from spdk_vtophys */
	g_small_size_counter = 0;
	g_small_size_modify = 3;
	g_small_size = 0x800;

	/* first is normal */
	exp_src_mbuf[0]->buf_addr = (void *)0x10000000;
	exp_src_mbuf[0]->buf_iova = 0x10000000;
	exp_src_mbuf[0]->pkt_len = exp_src_mbuf[0]->buf_len = 0x1000;

	/* second is normal */
	exp_src_mbuf[1]->buf_addr = (void *)0x10001000;
	exp_src_mbuf[1]->buf_iova = 0x10001000;
	exp_src_mbuf[1]->pkt_len = exp_src_mbuf[1]->buf_len = 0x1000;

	/* 3rd has shorter length */
	exp_src_mbuf[2]->buf_addr = (void *)0x10002000;
	exp_src_mbuf[2]->buf_iova = 0x10002000;
	exp_src_mbuf[2]->pkt_len = exp_src_mbuf[2]->buf_len = 0x800;

	/* a new 4th handles the remainder from the 3rd */
	exp_src_mbuf[3]->buf_addr = (void *)0x10002800;
	exp_src_mbuf[3]->buf_iova = 0x10002800;
	exp_src_mbuf[3]->pkt_len = exp_src_mbuf[3]->buf_len = 0x800;

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
	struct rte_mbuf mbuf[4]; /* one src, one dst, 2 ops */
	struct vbdev_comp_op *op_to_queue;
	struct iovec src_iovs[3] = {};
	struct iovec dst_iovs[3] = {};
	int i;

	cb_args = calloc(1, sizeof(*cb_args));
	SPDK_CU_ASSERT_FATAL(cb_args != NULL);
	cb_args->cb_fn = _compress_done;
	memset(&g_comp_op[0], 0, sizeof(struct rte_comp_op));
	g_comp_op[0].m_src = &mbuf[0];
	g_comp_op[1].m_src = &mbuf[1];
	g_comp_op[0].m_dst = &mbuf[2];
	g_comp_op[1].m_dst = &mbuf[3];
	for (i = 0; i < 3; i++) {
		src_iovs[i].iov_len = 0x1000;
		dst_iovs[i].iov_len = 0x1000;
		src_iovs[i].iov_base = (void *)0x10000000 + 0x1000 * i;
		dst_iovs[i].iov_base = (void *)0x20000000 + 0x1000 * i;
	}

	/* Error from dequeue, nothing needing to be resubmitted.
	 */
	ut_rte_compressdev_dequeue_burst = 1;
	/* setup what we want dequeue to return for the op */
	*RTE_MBUF_DYNFIELD(g_comp_op[0].m_src, g_mbuf_offset, uint64_t *) = (uint64_t)cb_args;
	g_comp_op[0].produced = 1;
	g_comp_op[0].status = 1;
	/* value asserted in the reduce callback */
	ut_compress_done[0] = -EINVAL;
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == true);
	rc = comp_dev_poller((void *)&g_comp_bdev);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == true);
	CU_ASSERT(rc == SPDK_POLLER_BUSY);

	/* Success from dequeue, 2 ops. nothing needing to be resubmitted.
	 */
	ut_rte_compressdev_dequeue_burst = 2;
	/* setup what we want dequeue to return for the op */
	*RTE_MBUF_DYNFIELD(g_comp_op[0].m_src, g_mbuf_offset, uint64_t *) = (uint64_t)cb_args;
	g_comp_op[0].produced = 16;
	g_comp_op[0].status = 0;
	*RTE_MBUF_DYNFIELD(g_comp_op[1].m_src, g_mbuf_offset, uint64_t *) = (uint64_t)cb_args;
	g_comp_op[1].produced = 32;
	g_comp_op[1].status = 0;
	/* value asserted in the reduce callback */
	ut_compress_done[0] = 16;
	ut_compress_done[1] = 32;
	done_count = 2;
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == true);
	rc = comp_dev_poller((void *)&g_comp_bdev);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == true);
	CU_ASSERT(rc == SPDK_POLLER_BUSY);

	/* Success from dequeue, one op to be resubmitted.
	 */
	ut_rte_compressdev_dequeue_burst = 1;
	/* setup what we want dequeue to return for the op */
	*RTE_MBUF_DYNFIELD(g_comp_op[0].m_src, g_mbuf_offset, uint64_t *) = (uint64_t)cb_args;
	g_comp_op[0].produced = 16;
	g_comp_op[0].status = 0;
	/* value asserted in the reduce callback */
	ut_compress_done[0] = 16;
	done_count = 1;
	op_to_queue = calloc(1, sizeof(struct vbdev_comp_op));
	SPDK_CU_ASSERT_FATAL(op_to_queue != NULL);
	op_to_queue->backing_dev = &g_comp_bdev.backing_dev;
	op_to_queue->src_iovs = &src_iovs[0];
	op_to_queue->src_iovcnt = 3;
	op_to_queue->dst_iovs = &dst_iovs[0];
	op_to_queue->dst_iovcnt = 3;
	op_to_queue->compress = true;
	op_to_queue->cb_arg = cb_args;
	ut_enqueue_value = FAKE_ENQUEUE_SUCCESS;
	TAILQ_INSERT_TAIL(&g_comp_bdev.queued_comp_ops,
			  op_to_queue,
			  link);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == false);
	rc = comp_dev_poller((void *)&g_comp_bdev);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_bdev.queued_comp_ops) == true);
	CU_ASSERT(rc == SPDK_POLLER_BUSY);

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

	/* test return values from rte_vdev_init() */
	MOCK_SET(rte_vdev_init, -EEXIST);
	rc = vbdev_init_compress_drivers();
	/* This is not an error condition, we already have one */
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

	/* success */
	ut_rte_compressdev_private_xform_create = 0;
	rc = vbdev_init_compress_drivers();
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_mbuf_offset == DPDK_DYNFIELD_OFFSET);
	spdk_mempool_free((struct spdk_mempool *)g_mbuf_mp);
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

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("compress", test_setup, test_cleanup);
	CU_ADD_TEST(suite, test_compress_operation);
	CU_ADD_TEST(suite, test_compress_operation_cross_boundary);
	CU_ADD_TEST(suite, test_vbdev_compress_submit_request);
	CU_ADD_TEST(suite, test_passthru);
	CU_ADD_TEST(suite, test_initdrivers);
	CU_ADD_TEST(suite, test_supported_io);
	CU_ADD_TEST(suite, test_poller);
	CU_ADD_TEST(suite, test_reset);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
