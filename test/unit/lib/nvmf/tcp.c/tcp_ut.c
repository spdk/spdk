/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021, 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/nvmf_spec.h"
#include "spdk_internal/cunit.h"
#include "spdk/bdev_zone.h"

#include "common/lib/test_env.c"
#include "common/lib/test_sock.c"

#include "nvmf/ctrlr.c"
#include "nvmf/tcp.c"
#include "spdk/sock.h"
#include "spdk/hexlify.h"

#define UT_IPV4_ADDR "192.168.0.1"
#define UT_PORT "4420"
#define UT_NVMF_ADRFAM_INVALID 0xf
#define UT_MAX_QUEUE_DEPTH 128
#define UT_MAX_QPAIRS_PER_CTRLR 128
#define UT_IN_CAPSULE_DATA_SIZE 1024
#define UT_MAX_IO_SIZE 4096
#define UT_IO_UNIT_SIZE 1024
#define UT_MAX_AQ_DEPTH 64
#define UT_SQ_HEAD_MAX 128
#define UT_NUM_SHARED_BUFFERS 128

static void *g_accel_p = (void *)0xdeadbeaf;

SPDK_LOG_REGISTER_COMPONENT(nvmf)

DEFINE_STUB(spdk_nvmf_qpair_get_listen_trid,
	    int,
	    (struct spdk_nvmf_qpair *qpair, struct spdk_nvme_transport_id *trid),
	    0);
DEFINE_STUB(spdk_nvmf_qpair_disconnect, int, (struct spdk_nvmf_qpair *qpair), 0);

DEFINE_STUB(nvmf_subsystem_add_ctrlr,
	    int,
	    (struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ctrlr *ctrlr),
	    0);

DEFINE_STUB(nvmf_subsystem_get_ctrlr,
	    struct spdk_nvmf_ctrlr *,
	    (struct spdk_nvmf_subsystem *subsystem, uint16_t cntlid),
	    NULL);

DEFINE_STUB(spdk_nvmf_tgt_find_subsystem,
	    struct spdk_nvmf_subsystem *,
	    (struct spdk_nvmf_tgt *tgt, const char *subnqn),
	    NULL);

DEFINE_STUB(spdk_nvmf_subsystem_listener_allowed,
	    bool,
	    (struct spdk_nvmf_subsystem *subsystem, const struct spdk_nvme_transport_id *trid),
	    true);

DEFINE_STUB(nvmf_subsystem_find_listener,
	    struct spdk_nvmf_subsystem_listener *,
	    (struct spdk_nvmf_subsystem *subsystem,
	     const struct spdk_nvme_transport_id *trid),
	    (void *)0x1);

DEFINE_STUB(spdk_sock_get_numa_id, int32_t, (struct spdk_sock *sock), 0);

DEFINE_STUB(spdk_nvmf_ns_find_host,
	    struct spdk_nvmf_host *,
	    (struct spdk_nvmf_ns *ns, const char *hostnqn),
	    NULL);

DEFINE_STUB_V(nvmf_get_discovery_log_page,
	      (struct spdk_nvmf_tgt *tgt, const char *hostnqn, struct iovec *iov,
	       uint32_t iovcnt, uint64_t offset, uint32_t length, struct spdk_nvme_transport_id *cmd_src_trid));

DEFINE_STUB_V(nvmf_subsystem_remove_ctrlr,
	      (struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ctrlr *ctrlr));

DEFINE_STUB(spdk_nvmf_subsystem_get_first_ns,
	    struct spdk_nvmf_ns *,
	    (struct spdk_nvmf_subsystem *subsystem),
	    NULL);

DEFINE_STUB(spdk_nvmf_subsystem_get_next_ns,
	    struct spdk_nvmf_ns *,
	    (struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ns *prev_ns),
	    NULL);
DEFINE_STUB(nvmf_subsystem_zone_append_supported, bool,
	    (struct spdk_nvmf_subsystem *subsystem), false);
DEFINE_STUB(spdk_nvmf_subsystem_host_allowed,
	    bool,
	    (struct spdk_nvmf_subsystem *subsystem, const char *hostnqn),
	    true);

DEFINE_STUB(nvmf_ctrlr_dsm_supported,
	    bool,
	    (struct spdk_nvmf_ctrlr *ctrlr),
	    false);

DEFINE_STUB(nvmf_ctrlr_write_zeroes_supported,
	    bool,
	    (struct spdk_nvmf_ctrlr *ctrlr),
	    false);

DEFINE_STUB(nvmf_ctrlr_copy_supported,
	    bool,
	    (struct spdk_nvmf_ctrlr *ctrlr),
	    false);

DEFINE_STUB(nvmf_bdev_ctrlr_read_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(nvmf_bdev_ctrlr_write_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(nvmf_bdev_ctrlr_compare_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(nvmf_bdev_ctrlr_compare_and_write_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *cmp_req, struct spdk_nvmf_request *write_req),
	    0);

DEFINE_STUB(nvmf_bdev_ctrlr_write_zeroes_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(nvmf_bdev_ctrlr_flush_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(nvmf_bdev_ctrlr_dsm_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(nvmf_bdev_ctrlr_copy_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(nvmf_bdev_ctrlr_nvme_passthru_io,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(spdk_nvmf_bdev_ctrlr_abort_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req, struct spdk_nvmf_request *req_to_abort),
	    0);

DEFINE_STUB(nvmf_bdev_ctrlr_get_dif_ctx,
	    bool,
	    (struct spdk_bdev_desc *desc, struct spdk_nvme_cmd *cmd, struct spdk_dif_ctx *dif_ctx),
	    false);

DEFINE_STUB_V(nvmf_bdev_ctrlr_identify_iocs_nvm,
	      (struct spdk_nvmf_ns *ns, struct spdk_nvme_nvm_ns_data *nsdata_nvm));

DEFINE_STUB(nvmf_transport_req_complete,
	    int,
	    (struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(nvmf_bdev_zcopy_enabled,
	    bool,
	    (struct spdk_bdev *bdev),
	    false);

DEFINE_STUB(nvmf_bdev_ctrlr_zcopy_start,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB_V(nvmf_bdev_ctrlr_zcopy_end, (struct spdk_nvmf_request *req, bool commit));

DEFINE_STUB_V(spdk_nvmf_request_free_buffers,
	      (struct spdk_nvmf_request *req, struct spdk_nvmf_transport_poll_group *group,
	       struct spdk_nvmf_transport *transport));

DEFINE_STUB(spdk_sock_get_optimal_sock_group,
	    int,
	    (struct spdk_sock *sock, struct spdk_sock_group **group, struct spdk_sock_group *hint),
	    0);

DEFINE_STUB(spdk_sock_group_get_ctx,
	    void *,
	    (struct spdk_sock_group *group),
	    NULL);

DEFINE_STUB_V(nvmf_ns_reservation_request, (void *ctx));

DEFINE_STUB_V(spdk_nvme_trid_populate_transport, (struct spdk_nvme_transport_id *trid,
		enum spdk_nvme_transport_type trtype));
DEFINE_STUB_V(spdk_nvmf_transport_register, (const struct spdk_nvmf_transport_ops *ops));

DEFINE_STUB_V(spdk_nvmf_tgt_new_qpair, (struct spdk_nvmf_tgt *tgt, struct spdk_nvmf_qpair *qpair));

DEFINE_STUB_V(nvmf_transport_qpair_abort_request,
	      (struct spdk_nvmf_qpair *qpair, struct spdk_nvmf_request *req));

DEFINE_STUB_V(nvmf_qpair_set_state, (struct spdk_nvmf_qpair *q, enum spdk_nvmf_qpair_state s));

DEFINE_STUB_V(spdk_nvme_print_command, (uint16_t qid, struct spdk_nvme_cmd *cmd));
DEFINE_STUB_V(spdk_nvme_print_completion, (uint16_t qid, struct spdk_nvme_cpl *cpl));

DEFINE_STUB(nvmf_transport_req_free,
	    int,
	    (struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(accel_channel_create, int, (void *io_device, void *ctx_buf), 0);
DEFINE_STUB_V(accel_channel_destroy, (void *io_device, void *ctx_buf));
DEFINE_STUB(spdk_bdev_reset, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				   spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *bdev_io));

DEFINE_STUB(spdk_bdev_get_max_active_zones, uint32_t,
	    (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_get_max_open_zones, uint32_t,
	    (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_is_zoned, bool, (const struct spdk_bdev *bdev), false);
DEFINE_STUB(spdk_bdev_get_zone_size, uint64_t, (const struct spdk_bdev *bdev), 0);

DEFINE_STUB(spdk_nvme_ns_get_format_index, uint32_t,
	    (const struct spdk_nvme_ns_data *nsdata), 0);

DEFINE_STUB(spdk_sock_get_impl_name, const char *, (struct spdk_sock *sock), "");

DEFINE_STUB(spdk_sock_group_register_interrupt, int, (struct spdk_sock_group *group,
		uint32_t events, spdk_interrupt_fn fn, void *arg, const char *name), 0);
DEFINE_STUB_V(spdk_sock_group_unregister_interrupt, (struct spdk_sock_group *group));

DEFINE_STUB(spdk_nvmf_subsystem_is_discovery, bool, (struct spdk_nvmf_subsystem *subsystem), false);
DEFINE_STUB(spdk_nvmf_subsystem_get_nqn, const char *,
	    (const struct spdk_nvmf_subsystem *subsystem), NULL);
DEFINE_STUB_V(spdk_keyring_put_key, (struct spdk_key *k));

DEFINE_STUB(nvmf_ns_is_ptpl_capable, bool, (const struct spdk_nvmf_ns *ns), false);
DEFINE_STUB(nvmf_subsystem_host_auth_required, bool, (struct spdk_nvmf_subsystem *s, const char *n),
	    false);
DEFINE_STUB(nvmf_qpair_auth_init, int, (struct spdk_nvmf_qpair *q), 0);
DEFINE_STUB(nvmf_auth_request_exec, int, (struct spdk_nvmf_request *r),
	    SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
DEFINE_STUB(nvmf_request_get_buffers_abort, bool, (struct spdk_nvmf_request *r), false);
DEFINE_STUB(spdk_bdev_io_type_supported, bool,
	    (struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type), false);
struct spdk_io_channel *
spdk_accel_get_io_channel(void)
{
	return spdk_get_io_channel(g_accel_p);
}

DEFINE_STUB(spdk_accel_submit_crc32cv,
	    int,
	    (struct spdk_io_channel *ch, uint32_t *dst, struct iovec *iovs,
	     uint32_t iovcnt, uint32_t seed, spdk_accel_completion_cb cb_fn, void *cb_arg),
	    0);

DEFINE_STUB(spdk_nvmf_bdev_ctrlr_nvme_passthru_admin,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
	     struct spdk_io_channel *ch, struct spdk_nvmf_request *req,
	     spdk_nvmf_nvme_passthru_cmd_cb cb_fn),
	    0)

struct spdk_key {
	const char *name;
	char data[4096];
	int len;
} g_ut_psk = {
	.name = "ut-key",
};

struct spdk_key *
spdk_keyring_get_key(const char *name)
{
	if (strcmp(name, g_ut_psk.name) == 0) {
		return &g_ut_psk;
	}

	return NULL;
}

int
spdk_key_get_key(struct spdk_key *key, void *buf, int len)
{
	len = spdk_min(key->len, len);

	memcpy(buf, key->data, len);

	return len;
}

const char *
spdk_key_get_name(struct spdk_key *k)
{
	return k->name;
}

struct spdk_bdev {
	int ut_mock;
	uint64_t blockcnt;
};

int
spdk_nvme_transport_id_compare(const struct spdk_nvme_transport_id *trid1,
			       const struct spdk_nvme_transport_id *trid2)
{
	return 0;
}

const char *
spdk_nvme_transport_id_trtype_str(enum spdk_nvme_transport_type trtype)
{
	switch (trtype) {
	case SPDK_NVME_TRANSPORT_PCIE:
		return "PCIe";
	case SPDK_NVME_TRANSPORT_RDMA:
		return "RDMA";
	case SPDK_NVME_TRANSPORT_FC:
		return "FC";
	default:
		return NULL;
	}
}

int
spdk_nvme_transport_id_populate_trstring(struct spdk_nvme_transport_id *trid, const char *trstring)
{
	int len, i;

	if (trstring == NULL) {
		return -EINVAL;
	}

	len = strnlen(trstring, SPDK_NVMF_TRSTRING_MAX_LEN);
	if (len == SPDK_NVMF_TRSTRING_MAX_LEN) {
		return -EINVAL;
	}

	/* cast official trstring to uppercase version of input. */
	for (i = 0; i < len; i++) {
		trid->trstring[i] = toupper(trstring[i]);
	}
	return 0;
}

int
spdk_nvmf_request_get_buffers(struct spdk_nvmf_request *req,
			      struct spdk_nvmf_transport_poll_group *group,
			      struct spdk_nvmf_transport *transport,
			      uint32_t length)
{
	/* length more than 1 io unit length will fail. */
	if (length >= transport->opts.io_unit_size) {
		return -EINVAL;
	}

	req->iovcnt = 1;
	req->iov[0].iov_base = (void *)0xDEADBEEF;

	return 0;
}


void
nvmf_bdev_ctrlr_identify_ns(struct spdk_nvmf_ns *ns, struct spdk_nvme_ns_data *nsdata,
			    bool dif_insert_or_strip)
{
	uint64_t num_blocks;

	SPDK_CU_ASSERT_FATAL(ns->bdev != NULL);
	num_blocks = ns->bdev->blockcnt;
	nsdata->nsze = num_blocks;
	nsdata->ncap = num_blocks;
	nsdata->nuse = num_blocks;
	nsdata->nlbaf = 0;
	nsdata->flbas.format = 0;
	nsdata->flbas.msb_format = 0;
	nsdata->lbaf[0].lbads = spdk_u32log2(512);
}

const char *
spdk_nvmf_subsystem_get_sn(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->sn;
}

const char *
spdk_nvmf_subsystem_get_mn(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->mn;
}

static void
test_nvmf_tcp_create(void)
{
	struct spdk_thread *thread;
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_transport_opts opts;
	struct spdk_sock_group grp = {};

	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	spdk_set_thread(thread);

	MOCK_SET(spdk_sock_group_create, &grp);

	/* case 1 */
	memset(&opts, 0, sizeof(opts));
	opts.max_queue_depth = UT_MAX_QUEUE_DEPTH;
	opts.max_qpairs_per_ctrlr = UT_MAX_QPAIRS_PER_CTRLR;
	opts.in_capsule_data_size = UT_IN_CAPSULE_DATA_SIZE;
	opts.max_io_size = UT_MAX_IO_SIZE;
	opts.io_unit_size = UT_IO_UNIT_SIZE;
	opts.max_aq_depth = UT_MAX_AQ_DEPTH;
	opts.num_shared_buffers = UT_NUM_SHARED_BUFFERS;
	/* expect success */
	transport = nvmf_tcp_create(&opts);
	CU_ASSERT_PTR_NOT_NULL(transport);
	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);
	SPDK_CU_ASSERT_FATAL(ttransport != NULL);
	transport->opts = opts;
	CU_ASSERT(transport->opts.max_queue_depth == UT_MAX_QUEUE_DEPTH);
	CU_ASSERT(transport->opts.max_io_size == UT_MAX_IO_SIZE);
	CU_ASSERT(transport->opts.in_capsule_data_size == UT_IN_CAPSULE_DATA_SIZE);
	CU_ASSERT(transport->opts.io_unit_size == UT_IO_UNIT_SIZE);
	/* destroy transport */
	CU_ASSERT(nvmf_tcp_destroy(transport, NULL, NULL) == 0);

	/* case 2 */
	memset(&opts, 0, sizeof(opts));
	opts.max_queue_depth = UT_MAX_QUEUE_DEPTH;
	opts.max_qpairs_per_ctrlr = UT_MAX_QPAIRS_PER_CTRLR;
	opts.in_capsule_data_size = UT_IN_CAPSULE_DATA_SIZE;
	opts.max_io_size = UT_MAX_IO_SIZE;
	opts.io_unit_size = UT_MAX_IO_SIZE + 1;
	opts.max_aq_depth = UT_MAX_AQ_DEPTH;
	opts.num_shared_buffers = UT_NUM_SHARED_BUFFERS;
	/* expect success */
	transport = nvmf_tcp_create(&opts);
	CU_ASSERT_PTR_NOT_NULL(transport);
	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);
	SPDK_CU_ASSERT_FATAL(ttransport != NULL);
	transport->opts = opts;
	CU_ASSERT(transport->opts.max_queue_depth == UT_MAX_QUEUE_DEPTH);
	CU_ASSERT(transport->opts.max_io_size == UT_MAX_IO_SIZE);
	CU_ASSERT(transport->opts.in_capsule_data_size == UT_IN_CAPSULE_DATA_SIZE);
	CU_ASSERT(transport->opts.io_unit_size == UT_MAX_IO_SIZE);
	/* destroy transport */
	CU_ASSERT(nvmf_tcp_destroy(transport, NULL, NULL) == 0);

	/* case 3 */
	memset(&opts, 0, sizeof(opts));
	opts.max_queue_depth = UT_MAX_QUEUE_DEPTH;
	opts.max_qpairs_per_ctrlr = UT_MAX_QPAIRS_PER_CTRLR;
	opts.in_capsule_data_size = UT_IN_CAPSULE_DATA_SIZE;
	opts.max_io_size = UT_MAX_IO_SIZE;
	opts.io_unit_size = 16;
	opts.max_aq_depth = UT_MAX_AQ_DEPTH;
	/* expect fails */
	transport = nvmf_tcp_create(&opts);
	CU_ASSERT_PTR_NULL(transport);

	MOCK_CLEAR_P(spdk_sock_group_create);

	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}
	spdk_thread_destroy(thread);
}

static void
test_nvmf_tcp_destroy(void)
{
	struct spdk_thread *thread;
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_transport_opts opts;
	struct spdk_sock_group grp = {};

	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	spdk_set_thread(thread);

	/* case 1 */
	memset(&opts, 0, sizeof(opts));
	opts.max_queue_depth = UT_MAX_QUEUE_DEPTH;
	opts.max_qpairs_per_ctrlr = UT_MAX_QPAIRS_PER_CTRLR;
	opts.in_capsule_data_size = UT_IN_CAPSULE_DATA_SIZE;
	opts.max_io_size = UT_MAX_IO_SIZE;
	opts.io_unit_size = UT_IO_UNIT_SIZE;
	opts.max_aq_depth = UT_MAX_AQ_DEPTH;
	opts.num_shared_buffers = UT_NUM_SHARED_BUFFERS;
	MOCK_SET(spdk_sock_group_create, &grp);
	transport = nvmf_tcp_create(&opts);
	MOCK_CLEAR_P(spdk_sock_group_create);
	CU_ASSERT_PTR_NOT_NULL(transport);
	transport->opts = opts;
	/* destroy transport */
	CU_ASSERT(nvmf_tcp_destroy(transport, NULL, NULL) == 0);

	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}
	spdk_thread_destroy(thread);
}

static void
init_accel(void)
{
	spdk_io_device_register(g_accel_p, accel_channel_create, accel_channel_destroy,
				sizeof(int), "accel_p");
}

static void
fini_accel(void)
{
	spdk_io_device_unregister(g_accel_p, NULL);
}

static void
test_nvmf_tcp_poll_group_create(void)
{
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_transport_poll_group *group;
	struct spdk_nvmf_tcp_poll_group *tgroup;
	struct spdk_thread *thread;
	struct spdk_nvmf_transport_opts opts;
	struct spdk_sock_group grp = {};

	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	spdk_set_thread(thread);

	init_accel();

	memset(&opts, 0, sizeof(opts));
	opts.max_queue_depth = UT_MAX_QUEUE_DEPTH;
	opts.max_qpairs_per_ctrlr = UT_MAX_QPAIRS_PER_CTRLR;
	opts.in_capsule_data_size = UT_IN_CAPSULE_DATA_SIZE;
	opts.max_io_size = UT_MAX_IO_SIZE;
	opts.io_unit_size = UT_IO_UNIT_SIZE;
	opts.max_aq_depth = UT_MAX_AQ_DEPTH;
	opts.num_shared_buffers = UT_NUM_SHARED_BUFFERS;
	MOCK_SET(spdk_sock_group_create, &grp);
	transport = nvmf_tcp_create(&opts);
	MOCK_CLEAR_P(spdk_sock_group_create);
	CU_ASSERT_PTR_NOT_NULL(transport);
	transport->opts = opts;
	MOCK_SET(spdk_sock_group_create, &grp);
	group = nvmf_tcp_poll_group_create(transport, NULL);
	MOCK_CLEAR_P(spdk_sock_group_create);
	SPDK_CU_ASSERT_FATAL(group);
	if (opts.in_capsule_data_size < SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE) {
		tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);
		SPDK_CU_ASSERT_FATAL(tgroup->control_msg_list);
	}
	group->transport = transport;
	nvmf_tcp_poll_group_destroy(group);
	nvmf_tcp_destroy(transport, NULL, NULL);

	fini_accel();
	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}
	spdk_thread_destroy(thread);
}

static void
test_nvmf_tcp_send_c2h_data(void)
{
	struct spdk_thread *thread;
	struct spdk_nvmf_tcp_transport ttransport = {};
	struct spdk_nvmf_tcp_qpair tqpair = {};
	struct spdk_nvmf_tcp_req tcp_req = {};
	struct nvme_tcp_pdu pdu = {};
	struct spdk_nvme_tcp_c2h_data_hdr *c2h_data;

	ttransport.tcp_opts.c2h_success = true;
	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	spdk_set_thread(thread);

	tcp_req.pdu = &pdu;
	tcp_req.req.length = 300;
	tcp_req.req.qpair = &tqpair.qpair;

	tqpair.qpair.transport = &ttransport.transport;

	/* Set qpair state to make unrelated operations NOP */
	tqpair.state = NVMF_TCP_QPAIR_STATE_RUNNING;
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_ERROR;

	tcp_req.req.cmd = (union nvmf_h2c_msg *)&tcp_req.cmd;

	tcp_req.req.iov[0].iov_base = (void *)0xDEADBEEF;
	tcp_req.req.iov[0].iov_len = 101;
	tcp_req.req.iov[1].iov_base = (void *)0xFEEDBEEF;
	tcp_req.req.iov[1].iov_len = 100;
	tcp_req.req.iov[2].iov_base = (void *)0xC0FFEE;
	tcp_req.req.iov[2].iov_len = 99;
	tcp_req.req.iovcnt = 3;
	tcp_req.req.length = 300;

	nvmf_tcp_send_c2h_data(&tqpair, &tcp_req);

	c2h_data = &pdu.hdr.c2h_data;
	CU_ASSERT(c2h_data->datao == 0);
	CU_ASSERT(c2h_data->datal == 300);
	CU_ASSERT(c2h_data->common.plen == sizeof(*c2h_data) + 300);
	CU_ASSERT(c2h_data->common.flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU);
	CU_ASSERT(c2h_data->common.flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS);

	CU_ASSERT(pdu.data_iovcnt == 3);
	CU_ASSERT((uint64_t)pdu.data_iov[0].iov_base == 0xDEADBEEF);
	CU_ASSERT(pdu.data_iov[0].iov_len == 101);
	CU_ASSERT((uint64_t)pdu.data_iov[1].iov_base == 0xFEEDBEEF);
	CU_ASSERT(pdu.data_iov[1].iov_len == 100);
	CU_ASSERT((uint64_t)pdu.data_iov[2].iov_base == 0xC0FFEE);
	CU_ASSERT(pdu.data_iov[2].iov_len == 99);

	tcp_req.pdu_in_use = false;
	tcp_req.rsp.cdw0 = 1;
	nvmf_tcp_send_c2h_data(&tqpair, &tcp_req);

	CU_ASSERT(c2h_data->common.flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU);
	CU_ASSERT((c2h_data->common.flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS) == 0);

	ttransport.tcp_opts.c2h_success = false;
	tcp_req.pdu_in_use = false;
	tcp_req.rsp.cdw0 = 0;
	nvmf_tcp_send_c2h_data(&tqpair, &tcp_req);

	CU_ASSERT(c2h_data->common.flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU);
	CU_ASSERT((c2h_data->common.flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS) == 0);

	tcp_req.pdu_in_use = false;
	tcp_req.rsp.cdw0 = 1;
	nvmf_tcp_send_c2h_data(&tqpair, &tcp_req);

	CU_ASSERT(c2h_data->common.flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU);
	CU_ASSERT((c2h_data->common.flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS) == 0);

	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}
	spdk_thread_destroy(thread);
}

#define NVMF_TCP_PDU_MAX_H2C_DATA_SIZE (128 * 1024)

static void
test_nvmf_tcp_h2c_data_hdr_handle(void)
{
	struct spdk_nvmf_tcp_transport ttransport = {};
	struct spdk_nvmf_tcp_qpair tqpair = {};
	struct nvme_tcp_pdu pdu = {};
	struct spdk_nvmf_tcp_req tcp_req = {};
	struct spdk_nvme_tcp_h2c_data_hdr *h2c_data;

	/* Set qpair state to make unrelated operations NOP */
	tqpair.state = NVMF_TCP_QPAIR_STATE_RUNNING;
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_ERROR;
	tqpair.resource_count = 1;
	tqpair.reqs = &tcp_req;

	tcp_req.req.iov[0].iov_base = (void *)0xDEADBEEF;
	tcp_req.req.iov[0].iov_len = 101;
	tcp_req.req.iov[1].iov_base = (void *)0xFEEDBEEF;
	tcp_req.req.iov[1].iov_len = 99;
	tcp_req.req.iovcnt = 2;
	tcp_req.req.length = 200;
	tcp_req.state = TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER;

	tcp_req.req.cmd = (union nvmf_h2c_msg *)&tcp_req.cmd;
	tcp_req.req.cmd->nvme_cmd.cid = 1;
	tcp_req.ttag = 1;

	h2c_data = &pdu.hdr.h2c_data;
	h2c_data->cccid = 1;
	h2c_data->ttag = 1;
	h2c_data->datao = 0;
	h2c_data->datal = 200;

	nvmf_tcp_h2c_data_hdr_handle(&ttransport, &tqpair, &pdu);

	CU_ASSERT(pdu.data_iovcnt == 2);
	CU_ASSERT((uint64_t)pdu.data_iov[0].iov_base == 0xDEADBEEF);
	CU_ASSERT(pdu.data_iov[0].iov_len == 101);
	CU_ASSERT((uint64_t)pdu.data_iov[1].iov_base == 0xFEEDBEEF);
	CU_ASSERT(pdu.data_iov[1].iov_len == 99);
}


static void
test_nvmf_tcp_in_capsule_data_handle(void)
{
	struct spdk_nvmf_tcp_transport ttransport = {};
	struct spdk_nvmf_transport_ops ops = {};
	struct spdk_nvmf_tcp_qpair tqpair = {};
	struct nvme_tcp_pdu *pdu, pdu_in_progress = {};
	union nvmf_c2h_msg rsp0 = {};
	union nvmf_c2h_msg rsp = {};

	struct spdk_nvmf_tcp_req tcp_req2 = {};
	struct spdk_nvmf_tcp_req tcp_req1 = {};

	struct spdk_nvme_tcp_cmd *capsule_data;
	struct spdk_nvmf_capsule_cmd *nvmf_capsule_data;
	struct spdk_nvme_sgl_descriptor *sgl;

	struct spdk_nvmf_transport_poll_group *group;
	struct spdk_nvmf_tcp_poll_group tcp_group = {};
	struct spdk_sock_group grp = {};

	tqpair.pdu_in_progress = &pdu_in_progress;
	ttransport.transport.opts.max_io_size = UT_MAX_IO_SIZE;
	ttransport.transport.opts.io_unit_size = UT_IO_UNIT_SIZE;
	ttransport.transport.ops = &ops;
	ops.req_get_buffers_done = nvmf_tcp_req_get_buffers_done;

	tcp_group.sock_group = &grp;
	TAILQ_INIT(&tcp_group.qpairs);
	group = &tcp_group.group;
	group->transport = &ttransport.transport;
	tqpair.group = &tcp_group;

	TAILQ_INIT(&tqpair.tcp_req_free_queue);
	TAILQ_INIT(&tqpair.tcp_req_working_queue);

	TAILQ_INSERT_TAIL(&tqpair.tcp_req_free_queue, &tcp_req2, state_link);
	tqpair.state_cntr[TCP_REQUEST_STATE_FREE]++;
	tqpair.qpair.transport = &ttransport.transport;
	tqpair.state = NVMF_TCP_QPAIR_STATE_RUNNING;
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH;
	tqpair.qpair.state = SPDK_NVMF_QPAIR_ENABLED;

	/* init a null tcp_req into tqpair TCP_REQUEST_STATE_FREE queue */
	tcp_req2.req.qpair = &tqpair.qpair;
	tcp_req2.req.cmd = (union nvmf_h2c_msg *)&tcp_req2.cmd;
	tcp_req2.req.rsp = &rsp;

	/* init tcp_req1 */
	tcp_req1.req.qpair = &tqpair.qpair;
	tcp_req1.req.cmd = (union nvmf_h2c_msg *)&tcp_req1.cmd;
	tcp_req1.req.rsp = &rsp0;
	tcp_req1.state = TCP_REQUEST_STATE_NEW;
	tcp_req1.req.data_from_pool = false;

	TAILQ_INSERT_TAIL(&tqpair.tcp_req_working_queue, &tcp_req1, state_link);
	tqpair.state_cntr[TCP_REQUEST_STATE_NEW]++;

	/* init pdu, make pdu need sgl buff */
	pdu = tqpair.pdu_in_progress;
	capsule_data = &pdu->hdr.capsule_cmd;
	nvmf_capsule_data = (struct spdk_nvmf_capsule_cmd *)&pdu->hdr.capsule_cmd.ccsqe;
	sgl = &capsule_data->ccsqe.dptr.sgl1;

	capsule_data->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD;
	capsule_data->common.hlen = sizeof(*capsule_data);
	capsule_data->common.plen = 1096;
	capsule_data->ccsqe.opc = SPDK_NVME_OPC_FABRIC;

	sgl->unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_TRANSPORT;
	sgl->generic.type = SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK;
	sgl->unkeyed.length = UT_IO_UNIT_SIZE;

	nvmf_capsule_data->fctype = SPDK_NVMF_FABRIC_COMMAND_CONNECT;

	/* pretend that tcp_req1 is waiting in the iobuf waiting queue */
	nvmf_tcp_req_process(&ttransport, &tcp_req1);
	CU_ASSERT(tcp_req1.req.data_from_pool == false);

	sgl->unkeyed.length = UT_IO_UNIT_SIZE - 1;

	/* process tqpair capsule req. */
	nvmf_tcp_capsule_cmd_hdr_handle(&ttransport, &tqpair, tqpair.pdu_in_progress);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	CU_ASSERT(tqpair.pdu_in_progress->req == (void *)&tcp_req2);

	/* pretend that buffer for tcp_req1 becomes available */
	spdk_nvmf_request_get_buffers(&tcp_req1.req, group, &ttransport.transport, UT_IO_UNIT_SIZE - 1);
	/* trigger callback as nvmf_request_iobuf_get_cb would */
	ttransport.transport.ops->req_get_buffers_done(&tcp_req1.req);
	CU_ASSERT(tcp_req1.state == TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);
}

static void
test_nvmf_tcp_qpair_init_mem_resource(void)
{
	int rc;
	struct spdk_nvmf_tcp_qpair *tqpair = NULL;
	struct spdk_nvmf_transport transport = {};
	struct spdk_thread *thread;

	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	spdk_set_thread(thread);

	tqpair = calloc(1, sizeof(*tqpair));
	tqpair->qpair.transport = &transport;

	nvmf_tcp_opts_init(&transport.opts);
	CU_ASSERT(transport.opts.max_queue_depth == SPDK_NVMF_TCP_DEFAULT_MAX_IO_QUEUE_DEPTH);
	CU_ASSERT(transport.opts.max_qpairs_per_ctrlr == SPDK_NVMF_TCP_DEFAULT_MAX_QPAIRS_PER_CTRLR);
	CU_ASSERT(transport.opts.in_capsule_data_size == SPDK_NVMF_TCP_DEFAULT_IN_CAPSULE_DATA_SIZE);
	CU_ASSERT(transport.opts.max_io_size ==	SPDK_NVMF_TCP_DEFAULT_MAX_IO_SIZE);
	CU_ASSERT(transport.opts.io_unit_size == SPDK_NVMF_TCP_DEFAULT_IO_UNIT_SIZE);
	CU_ASSERT(transport.opts.max_aq_depth == SPDK_NVMF_TCP_DEFAULT_MAX_ADMIN_QUEUE_DEPTH);
	CU_ASSERT(transport.opts.num_shared_buffers == SPDK_NVMF_TCP_DEFAULT_NUM_SHARED_BUFFERS);
	CU_ASSERT(transport.opts.buf_cache_size == SPDK_NVMF_TCP_DEFAULT_BUFFER_CACHE_SIZE);
	CU_ASSERT(transport.opts.dif_insert_or_strip ==	SPDK_NVMF_TCP_DEFAULT_DIF_INSERT_OR_STRIP);
	CU_ASSERT(transport.opts.abort_timeout_sec == SPDK_NVMF_TCP_DEFAULT_ABORT_TIMEOUT_SEC);
	CU_ASSERT(transport.opts.transport_specific == NULL);

	rc = nvmf_tcp_qpair_init(&tqpair->qpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(tqpair->host_hdgst_enable == true);
	CU_ASSERT(tqpair->host_ddgst_enable == true);

	rc = nvmf_tcp_qpair_init_mem_resource(tqpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(tqpair->resource_count == SPDK_NVMF_TCP_DEFAULT_MAX_IO_QUEUE_DEPTH);
	CU_ASSERT(tqpair->reqs != NULL);
	CU_ASSERT(tqpair->bufs != NULL);
	CU_ASSERT(tqpair->pdus != NULL);
	/* Just to check the first and last entry */
	CU_ASSERT(tqpair->reqs[0].ttag == 1);
	CU_ASSERT(tqpair->reqs[0].req.qpair == &tqpair->qpair);
	CU_ASSERT(tqpair->reqs[0].pdu == &tqpair->pdus[0]);
	CU_ASSERT(tqpair->reqs[0].pdu->qpair == &tqpair->qpair);
	CU_ASSERT(tqpair->reqs[0].buf == (void *)((uintptr_t)tqpair->bufs));
	CU_ASSERT(tqpair->reqs[0].req.rsp == (void *)&tqpair->reqs[0].rsp);
	CU_ASSERT(tqpair->reqs[0].req.cmd == (void *)&tqpair->reqs[0].cmd);
	CU_ASSERT(tqpair->reqs[0].state == TCP_REQUEST_STATE_FREE);
	CU_ASSERT(tqpair->reqs[127].ttag == 128);
	CU_ASSERT(tqpair->reqs[127].req.qpair == &tqpair->qpair);
	CU_ASSERT(tqpair->reqs[127].pdu == &tqpair->pdus[127]);
	CU_ASSERT(tqpair->reqs[127].pdu->qpair == &tqpair->qpair);
	CU_ASSERT(tqpair->reqs[127].buf == (void *)((uintptr_t)tqpair->bufs) + 127 * 4096);
	CU_ASSERT(tqpair->reqs[127].req.rsp == (void *)&tqpair->reqs[127].rsp);
	CU_ASSERT(tqpair->reqs[127].req.cmd == (void *)&tqpair->reqs[127].cmd);
	CU_ASSERT(tqpair->reqs[127].state == TCP_REQUEST_STATE_FREE);
	CU_ASSERT(tqpair->state_cntr[TCP_REQUEST_STATE_FREE] == SPDK_NVMF_TCP_DEFAULT_MAX_IO_QUEUE_DEPTH);
	CU_ASSERT(tqpair->mgmt_pdu == &tqpair->pdus[2 * SPDK_NVMF_TCP_DEFAULT_MAX_IO_QUEUE_DEPTH]);
	CU_ASSERT(tqpair->mgmt_pdu->qpair == tqpair);
	CU_ASSERT(tqpair->pdu_in_progress ==
		  &tqpair->pdus[2 * SPDK_NVMF_TCP_DEFAULT_MAX_IO_QUEUE_DEPTH - 1]);
	CU_ASSERT(tqpair->recv_buf_size == (4096 + sizeof(struct spdk_nvme_tcp_cmd) + 2 *
					    SPDK_NVME_TCP_DIGEST_LEN) * SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR);

	/* Free all of tqpair resource */
	nvmf_tcp_qpair_destroy(tqpair);

	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}
	spdk_thread_destroy(thread);
}

static void
test_nvmf_tcp_send_c2h_term_req(void)
{
	struct spdk_nvmf_tcp_qpair tqpair = {};
	struct nvme_tcp_pdu pdu = {}, mgmt_pdu = {}, pdu_in_progress = {};
	enum spdk_nvme_tcp_term_req_fes fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
	uint32_t error_offset = 1;
	struct spdk_thread *thread;

	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	spdk_set_thread(thread);

	mgmt_pdu.qpair = &tqpair;
	tqpair.mgmt_pdu = &mgmt_pdu;
	tqpair.pdu_in_progress = &pdu_in_progress;
	tqpair.tcp_pdu_working_count = 1;

	/* case1: hlen < SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE, Expect: copy_len == hlen */
	pdu.hdr.common.hlen = 64;
	nvmf_tcp_send_c2h_term_req(&tqpair, &pdu, fes, error_offset);
	spdk_thread_poll(thread, 0, 0);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.plen == tqpair.mgmt_pdu->hdr.term_req.common.hlen +
		  pdu.hdr.common.hlen);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD);

	/* case2: hlen > SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE, Expect: copy_len == SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE */
	pdu.hdr.common.hlen = 255;
	nvmf_tcp_send_c2h_term_req(&tqpair, &pdu, fes, error_offset);
	spdk_thread_poll(thread, 0, 0);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.plen == (unsigned)
		  tqpair.mgmt_pdu->hdr.term_req.common.hlen + SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD);

	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}

	spdk_thread_destroy(thread);
}

static void
test_nvmf_tcp_send_capsule_resp_pdu(void)
{
	struct spdk_nvmf_tcp_req tcp_req = {};
	struct spdk_nvmf_tcp_qpair tqpair = {};
	struct nvme_tcp_pdu pdu = {};

	tcp_req.pdu_in_use = false;
	tcp_req.req.qpair = &tqpair.qpair;
	tcp_req.pdu = &pdu;
	tcp_req.req.rsp = (union nvmf_c2h_msg *)&tcp_req.rsp;
	tcp_req.req.cmd = (union nvmf_h2c_msg *)&tcp_req.cmd;
	tqpair.host_hdgst_enable = true;

	nvmf_tcp_send_capsule_resp_pdu(&tcp_req, &tqpair);
	CU_ASSERT(pdu.hdr.capsule_resp.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP);
	CU_ASSERT(pdu.hdr.capsule_resp.common.plen == sizeof(struct spdk_nvme_tcp_rsp) +
		  SPDK_NVME_TCP_DIGEST_LEN);
	CU_ASSERT(pdu.hdr.capsule_resp.common.hlen == sizeof(struct spdk_nvme_tcp_rsp));
	CU_ASSERT(!memcmp(&pdu.hdr.capsule_resp.rccqe, &tcp_req.req.rsp->nvme_cpl,
			  sizeof(struct spdk_nvme_cpl)));
	CU_ASSERT(pdu.hdr.capsule_resp.common.flags & SPDK_NVME_TCP_CH_FLAGS_HDGSTF);
	CU_ASSERT(pdu.cb_fn == nvmf_tcp_request_free);
	CU_ASSERT(pdu.cb_arg == &tcp_req);
	CU_ASSERT(pdu.iov[0].iov_base == &pdu.hdr.raw);
	CU_ASSERT(pdu.iov[0].iov_len == sizeof(struct spdk_nvme_tcp_rsp) + SPDK_NVME_TCP_DIGEST_LEN);

	/* hdgst disable */
	tqpair.host_hdgst_enable = false;
	tcp_req.pdu_in_use = false;
	memset(&pdu, 0, sizeof(pdu));

	nvmf_tcp_send_capsule_resp_pdu(&tcp_req, &tqpair);
	CU_ASSERT(pdu.hdr.capsule_resp.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP);
	CU_ASSERT(pdu.hdr.capsule_resp.common.plen == sizeof(struct spdk_nvme_tcp_rsp));
	CU_ASSERT(pdu.hdr.capsule_resp.common.hlen == sizeof(struct spdk_nvme_tcp_rsp));
	CU_ASSERT(!memcmp(&pdu.hdr.capsule_resp.rccqe, &tcp_req.req.rsp->nvme_cpl,
			  sizeof(struct spdk_nvme_cpl)));
	CU_ASSERT(!(pdu.hdr.capsule_resp.common.flags & SPDK_NVME_TCP_CH_FLAGS_HDGSTF));
	CU_ASSERT(pdu.cb_fn == nvmf_tcp_request_free);
	CU_ASSERT(pdu.cb_arg == &tcp_req);
	CU_ASSERT(pdu.iov[0].iov_base == &pdu.hdr.raw);
	CU_ASSERT(pdu.iov[0].iov_len == sizeof(struct spdk_nvme_tcp_rsp));
}

static void
test_nvmf_tcp_icreq_handle(void)
{
	struct spdk_nvmf_tcp_transport ttransport = {};
	struct spdk_nvmf_tcp_qpair tqpair = {};
	struct nvme_tcp_pdu pdu = {};
	struct nvme_tcp_pdu mgmt_pdu = {};
	struct nvme_tcp_pdu pdu_in_progress = {};
	struct spdk_nvme_tcp_ic_resp *ic_resp;
	struct spdk_thread *thread;

	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	spdk_set_thread(thread);

	mgmt_pdu.qpair = &tqpair;
	tqpair.mgmt_pdu = &mgmt_pdu;
	tqpair.pdu_in_progress = &pdu_in_progress;
	tqpair.tcp_pdu_working_count = 1;

	/* case 1: Expected ICReq PFV 0 and got are different. */
	pdu.hdr.ic_req.pfv = 1;

	nvmf_tcp_icreq_handle(&ttransport, &tqpair, &pdu);

	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING);

	/* case 2: Expected ICReq HPDA in range 0-31 and got are different. */
	pdu.hdr.ic_req.hpda = SPDK_NVME_TCP_HPDA_MAX + 1;

	nvmf_tcp_icreq_handle(&ttransport, &tqpair, &pdu);
	spdk_thread_poll(thread, 0, 0);

	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING);

	/* case 3: Expect: PASS.  */
	ttransport.transport.opts.max_io_size = 32;
	pdu.hdr.ic_req.pfv = 0;
	tqpair.host_hdgst_enable = false;
	tqpair.host_ddgst_enable = false;
	tqpair.recv_buf_size = 64;
	pdu.hdr.ic_req.hpda = 16;

	nvmf_tcp_icreq_handle(&ttransport, &tqpair, &pdu);
	spdk_thread_poll(thread, 0, 0);

	ic_resp = &tqpair.mgmt_pdu->hdr.ic_resp;
	CU_ASSERT(tqpair.recv_buf_size == MIN_SOCK_PIPE_SIZE);
	CU_ASSERT(tqpair.cpda == pdu.hdr.ic_req.hpda);
	CU_ASSERT(ic_resp->common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_RESP);
	CU_ASSERT(ic_resp->common.hlen == sizeof(struct spdk_nvme_tcp_ic_resp));
	CU_ASSERT(ic_resp->common.plen ==  sizeof(struct spdk_nvme_tcp_ic_resp));
	CU_ASSERT(ic_resp->pfv == 0);
	CU_ASSERT(ic_resp->cpda == tqpair.cpda);
	CU_ASSERT(ic_resp->maxh2cdata == ttransport.transport.opts.max_io_size);
	CU_ASSERT(ic_resp->dgst.bits.hdgst_enable == 0);
	CU_ASSERT(ic_resp->dgst.bits.ddgst_enable == 0);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);

	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}

	spdk_thread_destroy(thread);
}

static void
test_nvmf_tcp_check_xfer_type(void)
{
	const uint16_t cid = 0xAA;
	struct spdk_nvmf_tcp_transport ttransport = {};
	struct spdk_nvmf_tcp_qpair tqpair = {};
	struct nvme_tcp_pdu pdu_in_progress = {};
	union nvmf_c2h_msg rsp0 = {};

	struct spdk_nvmf_tcp_req tcp_req = {};
	struct nvme_tcp_pdu rsp_pdu = {};

	struct spdk_nvme_tcp_cmd *capsule_data;
	struct spdk_nvme_sgl_descriptor *sgl;

	struct spdk_nvmf_transport_poll_group *group;
	struct spdk_nvmf_tcp_poll_group tcp_group = {};
	struct spdk_sock_group grp = {};

	tqpair.pdu_in_progress = &pdu_in_progress;
	ttransport.transport.opts.max_io_size = UT_MAX_IO_SIZE;
	ttransport.transport.opts.io_unit_size = UT_IO_UNIT_SIZE;

	tcp_group.sock_group = &grp;
	TAILQ_INIT(&tcp_group.qpairs);
	group = &tcp_group.group;
	group->transport = &ttransport.transport;
	tqpair.group = &tcp_group;

	TAILQ_INIT(&tqpair.tcp_req_free_queue);
	TAILQ_INIT(&tqpair.tcp_req_working_queue);

	tqpair.qpair.transport = &ttransport.transport;
	tqpair.state = NVMF_TCP_QPAIR_STATE_RUNNING;
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH;
	tqpair.qpair.state = SPDK_NVMF_QPAIR_ENABLED;

	/* init tcp_req */
	tcp_req.req.qpair = &tqpair.qpair;
	tcp_req.pdu = &rsp_pdu;
	tcp_req.req.cmd = (union nvmf_h2c_msg *)&tcp_req.cmd;
	tcp_req.req.rsp = &rsp0;
	tcp_req.state = TCP_REQUEST_STATE_NEW;

	TAILQ_INSERT_TAIL(&tqpair.tcp_req_working_queue, &tcp_req, state_link);
	tqpair.state_cntr[TCP_REQUEST_STATE_NEW]++;

	/* init pdu, make pdu need sgl buff */
	capsule_data = &tqpair.pdu_in_progress->hdr.capsule_cmd;
	sgl = &capsule_data->ccsqe.dptr.sgl1;

	capsule_data->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD;
	capsule_data->common.hlen = sizeof(*capsule_data);
	capsule_data->common.plen = 1096;
	capsule_data->ccsqe.opc = 0x10 | SPDK_NVME_DATA_BIDIRECTIONAL;
	/* Need to set to a non zero valid to check it gets copied to the response */
	capsule_data->ccsqe.cid = cid;

	/* Set up SGL to ensure nvmf_tcp_req_parse_sgl returns an error */
	sgl->unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_TRANSPORT;
	sgl->generic.type = SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK;
	sgl->unkeyed.length = UT_IO_UNIT_SIZE;

	/* Process a command and ensure that it fails and the request is set up to return an error */
	nvmf_tcp_req_process(&ttransport, &tcp_req);
	CU_ASSERT(tcp_req.state == TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	CU_ASSERT(tcp_req.req.rsp->nvme_cpl.cid == cid);
	CU_ASSERT(tcp_req.req.rsp->nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(tcp_req.req.rsp->nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_OPCODE);
}

static void
test_nvmf_tcp_invalid_sgl(void)
{
	const uint16_t cid = 0xAABB;
	struct spdk_nvmf_tcp_transport ttransport = {};
	struct spdk_nvmf_tcp_qpair tqpair = {};
	struct nvme_tcp_pdu pdu_in_progress = {};
	union nvmf_c2h_msg rsp0 = {};

	struct spdk_nvmf_tcp_req tcp_req = {};
	struct nvme_tcp_pdu rsp_pdu = {};
	struct nvme_tcp_pdu mgmt_pdu = {};

	struct spdk_nvme_tcp_cmd *capsule_data;
	struct spdk_nvme_sgl_descriptor *sgl;

	struct spdk_nvmf_transport_poll_group *group;
	struct spdk_nvmf_tcp_poll_group tcp_group = {};
	struct spdk_sock_group grp = {};

	struct spdk_thread *thread;

	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	spdk_set_thread(thread);

	tqpair.pdu_in_progress = &pdu_in_progress;
	ttransport.transport.opts.max_io_size = UT_MAX_IO_SIZE;
	ttransport.transport.opts.io_unit_size = UT_IO_UNIT_SIZE;

	tcp_group.sock_group = &grp;
	TAILQ_INIT(&tcp_group.qpairs);
	group = &tcp_group.group;
	group->transport = &ttransport.transport;
	tqpair.group = &tcp_group;

	TAILQ_INIT(&tqpair.tcp_req_free_queue);
	TAILQ_INIT(&tqpair.tcp_req_working_queue);

	tqpair.qpair.transport = &ttransport.transport;
	tqpair.state = NVMF_TCP_QPAIR_STATE_RUNNING;
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH;
	tqpair.qpair.state = SPDK_NVMF_QPAIR_ENABLED;

	/* init tcp_req */
	tcp_req.req.qpair = &tqpair.qpair;
	tcp_req.pdu = &rsp_pdu;
	tcp_req.pdu->qpair = &tqpair;
	tqpair.mgmt_pdu = &mgmt_pdu;
	tqpair.mgmt_pdu->qpair = &tqpair;
	tcp_req.req.cmd = (union nvmf_h2c_msg *)&tcp_req.cmd;
	tcp_req.req.rsp = &rsp0;
	tcp_req.state = TCP_REQUEST_STATE_NEW;

	TAILQ_INSERT_TAIL(&tqpair.tcp_req_working_queue, &tcp_req, state_link);
	tqpair.state_cntr[TCP_REQUEST_STATE_NEW]++;

	/* init pdu, make pdu need sgl buff */
	capsule_data = &tqpair.pdu_in_progress->hdr.capsule_cmd;
	sgl = &capsule_data->ccsqe.dptr.sgl1;

	capsule_data->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD;
	capsule_data->common.hlen = sizeof(*capsule_data);
	capsule_data->common.plen = 1096;
	capsule_data->ccsqe.opc = SPDK_NVME_OPC_WRITE;
	/* Need to set to a non zero valid to check it gets copied to the response */
	capsule_data->ccsqe.cid = cid;

	/* Set up SGL to ensure nvmf_tcp_req_parse_sgl returns an error */
	sgl->unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_TRANSPORT;
	sgl->generic.type = SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK;
	sgl->unkeyed.length = UT_MAX_IO_SIZE + 1;

	/* Process a command and ensure that it fails and the request is set up to return an error */
	nvmf_tcp_req_process(&ttransport, &tcp_req);
	spdk_thread_poll(thread, 0, 0);
	CU_ASSERT(tcp_req.state == TCP_REQUEST_STATE_NEED_BUFFER);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ);

	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}

	spdk_thread_destroy(thread);

}

static void
test_nvmf_tcp_pdu_ch_handle(void)
{
	struct spdk_nvmf_tcp_qpair tqpair = {};
	struct nvme_tcp_pdu mgmt_pdu = {}, pdu_in_progress = {};
	struct spdk_thread *thread;

	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	spdk_set_thread(thread);

	mgmt_pdu.qpair = &tqpair;
	tqpair.mgmt_pdu = &mgmt_pdu;
	tqpair.pdu_in_progress = &pdu_in_progress;
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH;
	tqpair.cpda = 0;

	/* Test case: Already received ICreq PDU. Expect: fail */
	tqpair.pdu_in_progress->hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_REQ;
	tqpair.state = NVMF_TCP_QPAIR_STATE_INITIALIZING;
	nvmf_tcp_pdu_ch_handle(&tqpair);
	spdk_thread_poll(thread, 0, 0);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.plen == tqpair.mgmt_pdu->hdr.term_req.common.hlen);

	/* Test case: Expected PDU header length and received are different. Expect: fail */
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH;
	tqpair.pdu_in_progress->hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_REQ;
	tqpair.state = NVMF_TCP_QPAIR_STATE_INVALID;
	tqpair.pdu_in_progress->hdr.common.plen = sizeof(struct spdk_nvme_tcp_ic_req);
	tqpair.pdu_in_progress->hdr.common.hlen = 0;
	nvmf_tcp_pdu_ch_handle(&tqpair);
	spdk_thread_poll(thread, 0, 0);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.plen == tqpair.mgmt_pdu->hdr.term_req.common.hlen);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.fei[0] == 2);

	/* Test case: The TCP/IP tqpair connection is not negotiated. Expect: fail */
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH;
	tqpair.pdu_in_progress->hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_RESP;
	tqpair.state = NVMF_TCP_QPAIR_STATE_INVALID;
	tqpair.pdu_in_progress->hdr.common.plen = sizeof(struct spdk_nvme_tcp_ic_req);
	tqpair.pdu_in_progress->hdr.common.hlen = 0;
	nvmf_tcp_pdu_ch_handle(&tqpair);
	spdk_thread_poll(thread, 0, 0);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.plen == tqpair.mgmt_pdu->hdr.term_req.common.hlen);

	/* Test case: Unexpected PDU type. Expect: fail */
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH;
	tqpair.pdu_in_progress->hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP;
	tqpair.state = NVMF_TCP_QPAIR_STATE_RUNNING;
	tqpair.pdu_in_progress->hdr.common.plen = 0;
	tqpair.pdu_in_progress->hdr.common.hlen = sizeof(struct spdk_nvme_tcp_ic_req);
	nvmf_tcp_pdu_ch_handle(&tqpair);
	spdk_thread_poll(thread, 0, 0);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.plen == tqpair.mgmt_pdu->hdr.term_req.common.hlen +
		  (unsigned)SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE);

	/* Test case: PDU type is SPDK_NVME_TCP_PDU_TYPE_IC_REQ, let plen error. Expect: fail */
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH;
	tqpair.pdu_in_progress->hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_REQ;
	tqpair.state = NVMF_TCP_QPAIR_STATE_INVALID;
	tqpair.pdu_in_progress->hdr.common.plen = 0;
	tqpair.pdu_in_progress->hdr.common.hlen = sizeof(struct spdk_nvme_tcp_ic_req);
	nvmf_tcp_pdu_ch_handle(&tqpair);
	spdk_thread_poll(thread, 0, 0);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.plen == tqpair.mgmt_pdu->hdr.term_req.common.hlen +
		  (unsigned)SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.fei[0] == 4);

	/* Test case: PDU type is SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD, let plen error. Expect: fail */
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH;
	tqpair.pdu_in_progress->hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD;
	tqpair.state = NVMF_TCP_QPAIR_STATE_RUNNING;
	tqpair.pdu_in_progress->hdr.common.flags = SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
	tqpair.pdu_in_progress->hdr.common.plen = 0;
	tqpair.pdu_in_progress->hdr.common.hlen = sizeof(struct spdk_nvme_tcp_cmd);
	nvmf_tcp_pdu_ch_handle(&tqpair);
	spdk_thread_poll(thread, 0, 0);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.plen == sizeof(struct spdk_nvme_tcp_cmd) + sizeof(
			  struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.fei[0] == 4);

	/* Test case: PDU type is SPDK_NVME_TCP_PDU_TYPE_H2C_DATA, let plen error. Expect: fail */
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH;
	tqpair.pdu_in_progress->hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_H2C_DATA;
	tqpair.state = NVMF_TCP_QPAIR_STATE_RUNNING;
	tqpair.pdu_in_progress->hdr.common.plen = 0;
	tqpair.pdu_in_progress->hdr.common.pdo = 64;
	tqpair.pdu_in_progress->hdr.common.hlen = sizeof(struct spdk_nvme_tcp_h2c_data_hdr);
	nvmf_tcp_pdu_ch_handle(&tqpair);
	spdk_thread_poll(thread, 0, 0);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.plen == tqpair.mgmt_pdu->hdr.term_req.common.hlen +
		  (unsigned)sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.fei[0] == 4);

	/* Test case: PDU type is SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ, let plen error. Expect: fail */
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH;
	tqpair.pdu_in_progress->hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ;
	tqpair.state = NVMF_TCP_QPAIR_STATE_RUNNING;
	tqpair.pdu_in_progress->hdr.common.plen = 0;
	tqpair.pdu_in_progress->hdr.common.hlen = sizeof(struct spdk_nvme_tcp_term_req_hdr);
	nvmf_tcp_pdu_ch_handle(&tqpair);
	spdk_thread_poll(thread, 0, 0);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.plen == tqpair.mgmt_pdu->hdr.term_req.common.hlen +
		  (unsigned)sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.fei[0] == 4);

	/* Test case: PDU type is SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD, let pdo error. Expect: fail */
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH;
	tqpair.pdu_in_progress->hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD;
	tqpair.state = NVMF_TCP_QPAIR_STATE_RUNNING;
	tqpair.cpda = 1;
	tqpair.pdu_in_progress->hdr.common.flags = SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
	tqpair.pdu_in_progress->hdr.common.plen = 0;
	tqpair.pdu_in_progress->hdr.common.pdo = 63;
	tqpair.pdu_in_progress->hdr.common.hlen = sizeof(struct spdk_nvme_tcp_cmd);
	nvmf_tcp_pdu_ch_handle(&tqpair);
	spdk_thread_poll(thread, 0, 0);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.plen == sizeof(struct spdk_nvme_tcp_cmd) + sizeof(
			  struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.fei[0] == 3);

	/* Test case: PDU type is SPDK_NVME_TCP_PDU_TYPE_H2C_DATA, let pdo error. Expect: fail */
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH;
	tqpair.pdu_in_progress->hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_H2C_DATA;
	tqpair.state = NVMF_TCP_QPAIR_STATE_RUNNING;
	tqpair.cpda = 1;
	tqpair.pdu_in_progress->hdr.common.plen = 0;
	tqpair.pdu_in_progress->hdr.common.pdo = 63;
	tqpair.pdu_in_progress->hdr.common.hlen = sizeof(struct spdk_nvme_tcp_h2c_data_hdr);
	nvmf_tcp_pdu_ch_handle(&tqpair);
	spdk_thread_poll(thread, 0, 0);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_QUIESCING);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ);
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.common.plen == tqpair.mgmt_pdu->hdr.term_req.common.hlen +
		  (unsigned)sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.mgmt_pdu->hdr.term_req.fei[0] == 3);

	/* Test case: All parameters is conformed to the function. Expect: PASS */
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH;
	tqpair.pdu_in_progress->hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_REQ;
	tqpair.state = NVMF_TCP_QPAIR_STATE_INVALID;
	tqpair.pdu_in_progress->hdr.common.plen = sizeof(struct spdk_nvme_tcp_ic_req);
	tqpair.pdu_in_progress->hdr.common.hlen = sizeof(struct spdk_nvme_tcp_ic_req);
	nvmf_tcp_pdu_ch_handle(&tqpair);
	spdk_thread_poll(thread, 0, 0);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);
	CU_ASSERT(tqpair.pdu_in_progress->psh_len == tqpair.pdu_in_progress->hdr.common.hlen - sizeof(
			  struct spdk_nvme_tcp_common_pdu_hdr));

	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}

	spdk_thread_destroy(thread);
}

static void
test_nvmf_tcp_tls_add_remove_credentials(void)
{
	struct spdk_thread *thread;
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_transport_opts opts;
	struct spdk_nvmf_subsystem subsystem;
	struct tcp_psk_entry *entry;
	struct spdk_sock_group grp = {};
	const char subnqn[] = {"nqn.2016-06.io.spdk:cnode1"};
	const char hostnqn[] = {"nqn.2016-06.io.spdk:host1"};
	const char *psk = "NVMeTLSkey-1:01:VRLbtnN9AQb2WXW3c9+wEf/DRLz0QuLdbYvEhwtdWwNf9LrZ:";
	bool found = false;

	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	spdk_set_thread(thread);

	memset(&opts, 0, sizeof(opts));
	opts.max_queue_depth = UT_MAX_QUEUE_DEPTH;
	opts.max_qpairs_per_ctrlr = UT_MAX_QPAIRS_PER_CTRLR;
	opts.in_capsule_data_size = UT_IN_CAPSULE_DATA_SIZE;
	opts.max_io_size = UT_MAX_IO_SIZE;
	opts.io_unit_size = UT_IO_UNIT_SIZE;
	opts.max_aq_depth = UT_MAX_AQ_DEPTH;
	opts.num_shared_buffers = UT_NUM_SHARED_BUFFERS;
	MOCK_SET(spdk_sock_group_create, &grp);
	transport = nvmf_tcp_create(&opts);
	MOCK_CLEAR_P(spdk_sock_group_create);

	memset(&subsystem, 0, sizeof(subsystem));
	snprintf(subsystem.subnqn, sizeof(subsystem.subnqn), "%s", subnqn);
	snprintf(g_ut_psk.data, sizeof(g_ut_psk.data), "%s", psk);
	g_ut_psk.len = strlen(psk) + 1;

	struct spdk_json_val psk_json[] = {
		{"", 2, SPDK_JSON_VAL_OBJECT_BEGIN},
		{"psk", 3, SPDK_JSON_VAL_NAME},
		{(void *)g_ut_psk.name, strlen(g_ut_psk.name), SPDK_JSON_VAL_STRING},
		{"", 0, SPDK_JSON_VAL_OBJECT_END},
	};

	nvmf_tcp_subsystem_add_host(transport, &subsystem, hostnqn, psk_json);

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);
	TAILQ_FOREACH(entry, &ttransport->psks, link) {
		if ((strcmp(subnqn, entry->subnqn) == 0) &&
		    (strcmp(hostnqn, entry->hostnqn) == 0)) {
			found = true;
		}
	}

	CU_ASSERT(found == true);
	found = false;

	nvmf_tcp_subsystem_remove_host(transport, &subsystem, hostnqn);

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);
	TAILQ_FOREACH(entry, &ttransport->psks, link) {
		if ((strcmp(subnqn, entry->subnqn) == 0) &&
		    (strcmp(hostnqn, entry->hostnqn) == 0)) {
			found = true;
		}
	}

	CU_ASSERT(found == false);

	CU_ASSERT(nvmf_tcp_destroy(transport, NULL, NULL) == 0);

	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}
	spdk_thread_destroy(thread);
}

static void
test_nvmf_tcp_tls_generate_psk_id(void)
{
	const char psk_id_reference[] = {"NVMe0R01 nqn.2016-06.io.spdk:host1 nqn.2016-06.io.spdk:cnode1"};
	const char subnqn[] = {"nqn.2016-06.io.spdk:cnode1"};
	const char hostnqn[] = {"nqn.2016-06.io.spdk:host1"};
	char psk_id[NVMF_PSK_IDENTITY_LEN] = {};
	char too_small_psk_id[5] = {};

	/* Check if we can generate expected PSK id. */
	CU_ASSERT(nvme_tcp_generate_psk_identity(psk_id, NVMF_PSK_IDENTITY_LEN, hostnqn,
			subnqn, NVME_TCP_CIPHER_AES_128_GCM_SHA256) == 0);
	CU_ASSERT(strcmp(psk_id, psk_id_reference) == 0);

	/* Test with a buffer that is too small to fit PSK id. */
	CU_ASSERT(nvme_tcp_generate_psk_identity(too_small_psk_id, sizeof(too_small_psk_id), hostnqn,
			subnqn, NVME_TCP_CIPHER_AES_128_GCM_SHA256) != 0);

	/* Test with unknown cipher suite. */
	CU_ASSERT(nvme_tcp_generate_psk_identity(psk_id, NVMF_PSK_IDENTITY_LEN, hostnqn,
			subnqn, UINT8_MAX) != 0);
}

static void
test_nvmf_tcp_tls_generate_retained_psk(void)
{
	const char hostnqn[] = {"nqn.2016-06.io.spdk:host1"};
	const char psk_reference1[] = {"1234567890ABCDEF"};
	const char psk_reference2[] = {"FEDCBA0987654321"};
	uint8_t unhexlified_str1[SPDK_TLS_PSK_MAX_LEN] = {};
	uint8_t unhexlified_str2[SPDK_TLS_PSK_MAX_LEN] = {};
	char *unhexlified1;
	char *unhexlified2;
	uint8_t psk_retained1[SPDK_TLS_PSK_MAX_LEN] = {};
	uint8_t psk_retained2[SPDK_TLS_PSK_MAX_LEN] = {};
	uint8_t too_small_psk_retained[5] = {};
	int psk_retained_len1, psk_retained_len2;
	int retained_size;

	unhexlified1 = spdk_unhexlify(psk_reference1);
	SPDK_CU_ASSERT_FATAL(unhexlified1 != NULL);
	unhexlified2 = spdk_unhexlify(psk_reference2);
	SPDK_CU_ASSERT_FATAL(unhexlified2 != NULL);

	memcpy(unhexlified_str1, unhexlified1, strlen(psk_reference1) / 2);
	memcpy(unhexlified_str2, unhexlified2, strlen(psk_reference2) / 2);
	free(unhexlified1);
	free(unhexlified2);

	/* Make sure that retained PSKs are different with different input PSKs and the same hash. */
	retained_size = nvme_tcp_derive_retained_psk(unhexlified_str1, strlen(psk_reference1) / 2, hostnqn,
			psk_retained1, SPDK_TLS_PSK_MAX_LEN, NVME_TCP_HASH_ALGORITHM_SHA256);
	CU_ASSERT(retained_size > 0);

	CU_ASSERT(nvme_tcp_derive_retained_psk(unhexlified_str2, strlen(psk_reference2) / 2, hostnqn,
					       psk_retained2,
					       SPDK_TLS_PSK_MAX_LEN, NVME_TCP_HASH_ALGORITHM_SHA256) > 0);
	CU_ASSERT(memcmp(psk_retained1, psk_retained2, retained_size) != 0);

	/* Make sure that retained PSKs are different with different hash and the same input PSKs. */
	psk_retained_len1 = nvme_tcp_derive_retained_psk(unhexlified_str1, strlen(psk_reference1) / 2,
			    hostnqn, psk_retained1, SPDK_TLS_PSK_MAX_LEN, NVME_TCP_HASH_ALGORITHM_SHA256);
	CU_ASSERT(psk_retained_len1 > 0);
	psk_retained_len2 = nvme_tcp_derive_retained_psk(unhexlified_str1, strlen(psk_reference1) / 2,
			    hostnqn, psk_retained2, SPDK_TLS_PSK_MAX_LEN, NVME_TCP_HASH_ALGORITHM_SHA384);
	CU_ASSERT(psk_retained_len2 > 0);
	CU_ASSERT(psk_retained_len1 < psk_retained_len2);

	/* Make sure that passing unknown value as hash errors out the function. */
	CU_ASSERT(nvme_tcp_derive_retained_psk(unhexlified_str1, strlen(psk_reference1) / 2, hostnqn,
					       psk_retained1, SPDK_TLS_PSK_MAX_LEN, -1) < 0);

	/* Make sure that passing buffer insufficient in size errors out the function. */
	CU_ASSERT(nvme_tcp_derive_retained_psk(unhexlified_str1, strlen(psk_reference1) / 2, hostnqn,
					       too_small_psk_retained, sizeof(too_small_psk_retained), NVME_TCP_HASH_ALGORITHM_SHA256) < 0);
}

static void
test_nvmf_tcp_tls_generate_tls_psk(void)
{
	const char psk_id_reference[] = {"NVMe0R01 nqn.2016-06.io.spdk:host1 nqn.2016-06.io.spdk:cnode1"};
	const char hostnqn[] = {"nqn.2016-06.io.spdk:host1"};
	const char psk_reference[] = {"1234567890ABCDEF"};
	char *unhexlified;
	uint8_t unhexlified_str[SPDK_TLS_PSK_MAX_LEN] = {};
	uint8_t psk_retained[SPDK_TLS_PSK_MAX_LEN] = {};
	uint8_t psk_key1[SPDK_TLS_PSK_MAX_LEN] = {}, psk_key2[SPDK_TLS_PSK_MAX_LEN] = {};
	uint8_t too_small_psk_tls[5] = {};
	int retained_size, tls_size;

	unhexlified = spdk_unhexlify(psk_reference);
	CU_ASSERT(unhexlified != NULL);

	memcpy(unhexlified_str, unhexlified, strlen(psk_reference) / 2);
	free(unhexlified);

	retained_size = nvme_tcp_derive_retained_psk(unhexlified_str, strlen(psk_reference) / 2, hostnqn,
			psk_retained, SPDK_TLS_PSK_MAX_LEN, NVME_TCP_HASH_ALGORITHM_SHA256);
	CU_ASSERT(retained_size > 0);

	/* Make sure that different cipher suites produce different TLS PSKs. */
	tls_size = nvme_tcp_derive_tls_psk(psk_retained, retained_size, psk_id_reference, psk_key1,
					   SPDK_TLS_PSK_MAX_LEN, NVME_TCP_CIPHER_AES_128_GCM_SHA256);
	CU_ASSERT(tls_size > 0);
	CU_ASSERT(nvme_tcp_derive_tls_psk(psk_retained, retained_size, psk_id_reference, psk_key2,
					  SPDK_TLS_PSK_MAX_LEN, NVME_TCP_CIPHER_AES_256_GCM_SHA384) > 0);
	CU_ASSERT(memcmp(psk_key1, psk_key2, tls_size) != 0);

	/* Make sure that passing unknown value as hash errors out the function. */
	CU_ASSERT(nvme_tcp_derive_tls_psk(psk_retained, retained_size, psk_id_reference,
					  psk_key1, SPDK_TLS_PSK_MAX_LEN, UINT8_MAX) < 0);

	/* Make sure that passing buffer insufficient in size errors out the function. */
	CU_ASSERT(nvme_tcp_derive_tls_psk(psk_retained, retained_size, psk_id_reference,
					  too_small_psk_tls, sizeof(too_small_psk_tls),
					  NVME_TCP_CIPHER_AES_128_GCM_SHA256) < 0);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("nvmf", NULL, NULL);

	CU_ADD_TEST(suite, test_nvmf_tcp_create);
	CU_ADD_TEST(suite, test_nvmf_tcp_destroy);
	CU_ADD_TEST(suite, test_nvmf_tcp_poll_group_create);
	CU_ADD_TEST(suite, test_nvmf_tcp_send_c2h_data);
	CU_ADD_TEST(suite, test_nvmf_tcp_h2c_data_hdr_handle);
	CU_ADD_TEST(suite, test_nvmf_tcp_in_capsule_data_handle);
	CU_ADD_TEST(suite, test_nvmf_tcp_qpair_init_mem_resource);
	CU_ADD_TEST(suite, test_nvmf_tcp_send_c2h_term_req);
	CU_ADD_TEST(suite, test_nvmf_tcp_send_capsule_resp_pdu);
	CU_ADD_TEST(suite, test_nvmf_tcp_icreq_handle);
	CU_ADD_TEST(suite, test_nvmf_tcp_check_xfer_type);
	CU_ADD_TEST(suite, test_nvmf_tcp_invalid_sgl);
	CU_ADD_TEST(suite, test_nvmf_tcp_pdu_ch_handle);
	CU_ADD_TEST(suite, test_nvmf_tcp_tls_add_remove_credentials);
	CU_ADD_TEST(suite, test_nvmf_tcp_tls_generate_psk_id);
	CU_ADD_TEST(suite, test_nvmf_tcp_tls_generate_retained_psk);
	CU_ADD_TEST(suite, test_nvmf_tcp_tls_generate_tls_psk);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
