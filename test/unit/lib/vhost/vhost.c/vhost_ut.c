/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 Mellanox Technologies LTD. All rights reserved.
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

#include "spdk/stdinc.h"

#include "CUnit/Basic.h"
#include "spdk_cunit.h"
#include "spdk/thread.h"
#include "spdk_internal/mock.h"
#include "common/lib/test_env.c"
#include "unit/lib/json_mock.c"

#include "vhost/vhost.c"
#include <rte_version.h>
#include "vhost/rte_vhost_user.c"

DEFINE_STUB(rte_vhost_set_vring_base, int, (int vid, uint16_t queue_id,
		uint16_t last_avail_idx, uint16_t last_used_idx), 0);
DEFINE_STUB(rte_vhost_get_vring_base, int, (int vid, uint16_t queue_id,
		uint16_t *last_avail_idx, uint16_t *last_used_idx), 0);
DEFINE_STUB(spdk_mem_register, int, (void *vaddr, size_t len), 0);
DEFINE_STUB(spdk_mem_unregister, int, (void *vaddr, size_t len), 0);
DEFINE_STUB(rte_vhost_vring_call, int, (int vid, uint16_t vring_idx), 0);
DEFINE_STUB_V(rte_vhost_log_used_vring, (int vid, uint16_t vring_idx,
		uint64_t offset, uint64_t len));

DEFINE_STUB(rte_vhost_get_mem_table, int, (int vid, struct rte_vhost_memory **mem), 0);
DEFINE_STUB(rte_vhost_get_negotiated_features, int, (int vid, uint64_t *features), 0);
DEFINE_STUB(rte_vhost_get_vhost_vring, int,
	    (int vid, uint16_t vring_idx, struct rte_vhost_vring *vring), 0);
DEFINE_STUB(rte_vhost_enable_guest_notification, int,
	    (int vid, uint16_t queue_id, int enable), 0);
DEFINE_STUB(rte_vhost_get_ifname, int, (int vid, char *buf, size_t len), 0);
DEFINE_STUB(rte_vhost_driver_start, int, (const char *name), 0);
#if RTE_VERSION >= RTE_VERSION_NUM(21, 11, 0, 0)
DEFINE_STUB(rte_vhost_driver_callback_register, int,
	    (const char *path, struct rte_vhost_device_ops const *const ops), 0);
#else
DEFINE_STUB(rte_vhost_driver_callback_register, int,
	    (const char *path, struct vhost_device_ops const *const ops), 0);
#endif
DEFINE_STUB(rte_vhost_driver_disable_features, int, (const char *path, uint64_t features), 0);
DEFINE_STUB(rte_vhost_driver_set_features, int, (const char *path, uint64_t features), 0);
DEFINE_STUB(rte_vhost_driver_register, int, (const char *path, uint64_t flags), 0);
DEFINE_STUB(rte_vhost_driver_unregister, int, (const char *path), 0);
DEFINE_STUB(rte_vhost_driver_get_protocol_features, int,
	    (const char *path, uint64_t *protocol_features), 0);
DEFINE_STUB(rte_vhost_driver_set_protocol_features, int,
	    (const char *path, uint64_t protocol_features), 0);

DEFINE_STUB(rte_vhost_set_last_inflight_io_split, int,
	    (int vid, uint16_t vring_idx, uint16_t idx), 0);
DEFINE_STUB(rte_vhost_clr_inflight_desc_split, int,
	    (int vid, uint16_t vring_idx, uint16_t last_used_idx, uint16_t idx), 0);
DEFINE_STUB(rte_vhost_set_last_inflight_io_packed, int,
	    (int vid, uint16_t vring_idx, uint16_t head), 0);
DEFINE_STUB(rte_vhost_clr_inflight_desc_packed, int,
	    (int vid, uint16_t vring_idx, uint16_t head), 0);
DEFINE_STUB_V(rte_vhost_log_write, (int vid, uint64_t addr, uint64_t len));
DEFINE_STUB(rte_vhost_get_vhost_ring_inflight, int,
	    (int vid, uint16_t vring_idx, struct rte_vhost_ring_inflight *vring), 0);
DEFINE_STUB(rte_vhost_get_vring_base_from_inflight, int,
	    (int vid, uint16_t queue_id, uint16_t *last_avail_idx, uint16_t *last_used_idx), 0);
DEFINE_STUB(rte_vhost_extern_callback_register, int,
	    (int vid, struct rte_vhost_user_extern_ops const *const ops, void *ctx), 0);

void *
spdk_call_unaffinitized(void *cb(void *arg), void *arg)
{
	return cb(arg);
}

static struct spdk_vhost_dev_backend g_vdev_backend;

static int
test_setup(void)
{
	return 0;
}

static int
alloc_vdev(struct spdk_vhost_dev **vdev_p, const char *name, const char *cpumask)
{
	struct spdk_vhost_dev *vdev = NULL;
	int rc;

	/* spdk_vhost_dev must be allocated on a cache line boundary. */
	rc = posix_memalign((void **)&vdev, 64, sizeof(*vdev));
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(vdev != NULL);
	memset(vdev, 0, sizeof(*vdev));
	rc = vhost_dev_register(vdev, name, cpumask, &g_vdev_backend);
	if (rc == 0) {
		*vdev_p = vdev;
	} else {
		free(vdev);
		*vdev_p = NULL;
	}

	return rc;
}

static void
start_vdev(struct spdk_vhost_dev *vdev)
{
	struct rte_vhost_memory *mem;
	struct spdk_vhost_session *vsession = NULL;
	int rc;

	mem = calloc(1, sizeof(*mem) + 2 * sizeof(struct rte_vhost_mem_region));
	SPDK_CU_ASSERT_FATAL(mem != NULL);
	mem->nregions = 2;
	mem->regions[0].guest_phys_addr = 0;
	mem->regions[0].size = 0x400000; /* 4 MB */
	mem->regions[0].host_user_addr = 0x1000000;
	mem->regions[1].guest_phys_addr = 0x400000;
	mem->regions[1].size = 0x400000; /* 4 MB */
	mem->regions[1].host_user_addr = 0x2000000;

	assert(TAILQ_EMPTY(&vdev->vsessions));
	/* spdk_vhost_dev must be allocated on a cache line boundary. */
	rc = posix_memalign((void **)&vsession, 64, sizeof(*vsession));
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(vsession != NULL);
	vsession->started = true;
	vsession->vid = 0;
	vsession->mem = mem;
	TAILQ_INSERT_TAIL(&vdev->vsessions, vsession, tailq);
}

static void
stop_vdev(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_session *vsession = TAILQ_FIRST(&vdev->vsessions);

	TAILQ_REMOVE(&vdev->vsessions, vsession, tailq);
	free(vsession->mem);
	free(vsession);
}

static void
cleanup_vdev(struct spdk_vhost_dev *vdev)
{
	if (!TAILQ_EMPTY(&vdev->vsessions)) {
		stop_vdev(vdev);
	}
	vhost_dev_unregister(vdev);
	free(vdev);
}

static void
desc_to_iov_test(void)
{
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_session *vsession;
	struct iovec iov[SPDK_VHOST_IOVS_MAX];
	uint16_t iov_index;
	struct vring_desc desc;
	int rc;

	spdk_cpuset_set_cpu(&g_vhost_core_mask, 0, true);

	rc = alloc_vdev(&vdev, "vdev_name_0", "0x1");
	SPDK_CU_ASSERT_FATAL(rc == 0 && vdev);
	start_vdev(vdev);

	vsession = TAILQ_FIRST(&vdev->vsessions);

	/* Test simple case where iov falls fully within a 2MB page. */
	desc.addr = 0x110000;
	desc.len = 0x1000;
	iov_index = 0;
	rc = vhost_vring_desc_to_iov(vsession, iov, &iov_index, &desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(iov_index == 1);
	CU_ASSERT(iov[0].iov_base == (void *)0x1110000);
	CU_ASSERT(iov[0].iov_len == 0x1000);
	/*
	 * Always memset the iov to ensure each test validates data written by its call
	 * to the function under test.
	 */
	memset(iov, 0, sizeof(iov));

	/* Same test, but ensure it respects the non-zero starting iov_index. */
	iov_index = SPDK_VHOST_IOVS_MAX - 1;
	rc = vhost_vring_desc_to_iov(vsession, iov, &iov_index, &desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(iov_index == SPDK_VHOST_IOVS_MAX);
	CU_ASSERT(iov[SPDK_VHOST_IOVS_MAX - 1].iov_base == (void *)0x1110000);
	CU_ASSERT(iov[SPDK_VHOST_IOVS_MAX - 1].iov_len == 0x1000);
	memset(iov, 0, sizeof(iov));

	/* Test for failure if iov_index already equals SPDK_VHOST_IOVS_MAX. */
	iov_index = SPDK_VHOST_IOVS_MAX;
	rc = vhost_vring_desc_to_iov(vsession, iov, &iov_index, &desc);
	CU_ASSERT(rc != 0);
	memset(iov, 0, sizeof(iov));

	/* Test case where iov spans a 2MB boundary, but does not span a vhost memory region. */
	desc.addr = 0x1F0000;
	desc.len = 0x20000;
	iov_index = 0;
	rc = vhost_vring_desc_to_iov(vsession, iov, &iov_index, &desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(iov_index == 1);
	CU_ASSERT(iov[0].iov_base == (void *)0x11F0000);
	CU_ASSERT(iov[0].iov_len == 0x20000);
	memset(iov, 0, sizeof(iov));

	/* Same test, but ensure it respects the non-zero starting iov_index. */
	iov_index = SPDK_VHOST_IOVS_MAX - 1;
	rc = vhost_vring_desc_to_iov(vsession, iov, &iov_index, &desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(iov_index == SPDK_VHOST_IOVS_MAX);
	CU_ASSERT(iov[SPDK_VHOST_IOVS_MAX - 1].iov_base == (void *)0x11F0000);
	CU_ASSERT(iov[SPDK_VHOST_IOVS_MAX - 1].iov_len == 0x20000);
	memset(iov, 0, sizeof(iov));

	/* Test case where iov spans a vhost memory region. */
	desc.addr = 0x3F0000;
	desc.len = 0x20000;
	iov_index = 0;
	rc = vhost_vring_desc_to_iov(vsession, iov, &iov_index, &desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(iov_index == 2);
	CU_ASSERT(iov[0].iov_base == (void *)0x13F0000);
	CU_ASSERT(iov[0].iov_len == 0x10000);
	CU_ASSERT(iov[1].iov_base == (void *)0x2000000);
	CU_ASSERT(iov[1].iov_len == 0x10000);
	memset(iov, 0, sizeof(iov));

	cleanup_vdev(vdev);

	CU_ASSERT(true);
}

static void
create_controller_test(void)
{
	struct spdk_vhost_dev *vdev, *vdev2;
	int ret;
	char long_name[PATH_MAX];

	spdk_cpuset_parse(&g_vhost_core_mask, "0xf");

	/* Create device with cpumask implicitly matching whole application */
	ret = alloc_vdev(&vdev, "vdev_name_0", NULL);
	SPDK_CU_ASSERT_FATAL(ret == 0 && vdev);
	SPDK_CU_ASSERT_FATAL(!strcmp(spdk_cpuset_fmt(spdk_thread_get_cpumask(vdev->thread)), "f"));
	cleanup_vdev(vdev);

	/* Create device with cpumask matching whole application */
	ret = alloc_vdev(&vdev, "vdev_name_0", "0xf");
	SPDK_CU_ASSERT_FATAL(ret == 0 && vdev);
	SPDK_CU_ASSERT_FATAL(!strcmp(spdk_cpuset_fmt(spdk_thread_get_cpumask(vdev->thread)), "f"));
	cleanup_vdev(vdev);

	/* Create device with single core in cpumask */
	ret = alloc_vdev(&vdev, "vdev_name_0", "0x2");
	SPDK_CU_ASSERT_FATAL(ret == 0 && vdev);
	SPDK_CU_ASSERT_FATAL(!strcmp(spdk_cpuset_fmt(spdk_thread_get_cpumask(vdev->thread)), "2"));
	cleanup_vdev(vdev);

	/* Create device with cpumask spanning two cores */
	ret = alloc_vdev(&vdev, "vdev_name_0", "0x3");
	SPDK_CU_ASSERT_FATAL(ret == 0 && vdev);
	SPDK_CU_ASSERT_FATAL(!strcmp(spdk_cpuset_fmt(spdk_thread_get_cpumask(vdev->thread)), "3"));
	cleanup_vdev(vdev);

	/* Create device with incorrect cpumask outside of application cpumask */
	ret = alloc_vdev(&vdev, "vdev_name_0", "0xf0");
	SPDK_CU_ASSERT_FATAL(ret != 0);

	/* Create device with incorrect cpumask partially outside of application cpumask */
	ret = alloc_vdev(&vdev, "vdev_name_0", "0xff");
	SPDK_CU_ASSERT_FATAL(ret != 0);

	/* Create device with no name */
	ret = alloc_vdev(&vdev, NULL, NULL);
	CU_ASSERT(ret != 0);

	/* Create device with too long name and path */
	memset(long_name, 'x', sizeof(long_name));
	long_name[PATH_MAX - 1] = 0;
	snprintf(g_vhost_user_dev_dirname, sizeof(g_vhost_user_dev_dirname), "some_path/");
	ret = alloc_vdev(&vdev, long_name, NULL);
	CU_ASSERT(ret != 0);
	g_vhost_user_dev_dirname[0] = 0;

	/* Create device when device name is already taken */
	ret = alloc_vdev(&vdev, "vdev_name_0", NULL);
	SPDK_CU_ASSERT_FATAL(ret == 0 && vdev);
	ret = alloc_vdev(&vdev2, "vdev_name_0", NULL);
	CU_ASSERT(ret != 0);
	cleanup_vdev(vdev);
}

static void
session_find_by_vid_test(void)
{
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_session *vsession;
	struct spdk_vhost_session *tmp;
	int rc;

	rc = alloc_vdev(&vdev, "vdev_name_0", "0x1");
	SPDK_CU_ASSERT_FATAL(rc == 0 && vdev);
	start_vdev(vdev);

	vsession = TAILQ_FIRST(&vdev->vsessions);

	tmp = vhost_session_find_by_vid(vsession->vid);
	CU_ASSERT(tmp == vsession);

	/* Search for a device with incorrect vid */
	tmp = vhost_session_find_by_vid(vsession->vid + 0xFF);
	CU_ASSERT(tmp == NULL);

	cleanup_vdev(vdev);
}

static void
remove_controller_test(void)
{
	struct spdk_vhost_dev *vdev;
	int ret;

	ret = alloc_vdev(&vdev, "vdev_name_0", "0x1");
	SPDK_CU_ASSERT_FATAL(ret == 0 && vdev);

	/* Remove device when controller is in use */
	start_vdev(vdev);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&vdev->vsessions));
	ret = vhost_dev_unregister(vdev);
	CU_ASSERT(ret != 0);

	cleanup_vdev(vdev);
}

static void
vq_avail_ring_get_test(void)
{
	struct spdk_vhost_virtqueue vq = {};
	uint16_t avail_mem[34];
	uint16_t reqs[32];
	uint16_t reqs_len, ret, i;

	/* Basic example reap all requests */
	vq.vring.avail = (struct vring_avail *)avail_mem;
	vq.vring.size = 32;
	vq.last_avail_idx = 24;
	vq.vring.avail->idx = 29;
	reqs_len = 6;

	for (i = 0; i < 32; i++) {
		vq.vring.avail->ring[i] = i;
	}

	ret = vhost_vq_avail_ring_get(&vq, reqs, reqs_len);
	CU_ASSERT(ret == 5);
	CU_ASSERT(vq.last_avail_idx == 29);
	for (i = 0; i < ret; i++) {
		CU_ASSERT(reqs[i] == vq.vring.avail->ring[i + 24]);
	}

	/* Basic example reap only some requests */
	vq.last_avail_idx = 20;
	vq.vring.avail->idx = 29;
	reqs_len = 6;

	ret = vhost_vq_avail_ring_get(&vq, reqs, reqs_len);
	CU_ASSERT(ret == reqs_len);
	CU_ASSERT(vq.last_avail_idx == 26);
	for (i = 0; i < ret; i++) {
		CU_ASSERT(reqs[i] == vq.vring.avail->ring[i + 20]);
	}

	/* Test invalid example */
	vq.last_avail_idx = 20;
	vq.vring.avail->idx = 156;
	reqs_len = 6;

	ret = vhost_vq_avail_ring_get(&vq, reqs, reqs_len);
	CU_ASSERT(ret == 0);

	/* Test overflow in the avail->idx variable. */
	vq.last_avail_idx = 65535;
	vq.vring.avail->idx = 4;
	reqs_len = 6;
	ret = vhost_vq_avail_ring_get(&vq, reqs, reqs_len);
	CU_ASSERT(ret == 5);
	CU_ASSERT(vq.last_avail_idx == 4);
	CU_ASSERT(reqs[0] == vq.vring.avail->ring[31]);
	for (i = 1; i < ret; i++) {
		CU_ASSERT(reqs[i] == vq.vring.avail->ring[i - 1]);
	}
}

static bool
vq_desc_guest_is_used(struct spdk_vhost_virtqueue *vq, int16_t guest_last_used_idx,
		      int16_t guest_used_phase)
{
	return (!!(vq->vring.desc_packed[guest_last_used_idx].flags & VRING_DESC_F_USED) ==
		!!guest_used_phase);
}

static void
vq_desc_guest_set_avail(struct spdk_vhost_virtqueue *vq, int16_t *guest_last_avail_idx,
			int16_t *guest_avail_phase)
{
	if (*guest_avail_phase) {
		vq->vring.desc_packed[*guest_last_avail_idx].flags |= VRING_DESC_F_AVAIL;
		vq->vring.desc_packed[*guest_last_avail_idx].flags &= ~VRING_DESC_F_USED;
	} else {
		vq->vring.desc_packed[*guest_last_avail_idx].flags &= ~VRING_DESC_F_AVAIL;
		vq->vring.desc_packed[*guest_last_avail_idx].flags |= VRING_DESC_F_USED;
	}

	if (++(*guest_last_avail_idx) >= vq->vring.size) {
		*guest_last_avail_idx -= vq->vring.size;
		*guest_avail_phase = !(*guest_avail_phase);
	}
}

static int16_t
vq_desc_guest_handle_completed_desc(struct spdk_vhost_virtqueue *vq, int16_t *guest_last_used_idx,
				    int16_t *guest_used_phase)
{
	int16_t buffer_id = -1;

	if (vq_desc_guest_is_used(vq, *guest_last_used_idx, *guest_used_phase)) {
		buffer_id = vq->vring.desc_packed[*guest_last_used_idx].id;
		if (++(*guest_last_used_idx) >= vq->vring.size) {
			*guest_last_used_idx -= vq->vring.size;
			*guest_used_phase = !(*guest_used_phase);
		}

		return buffer_id;
	}

	return -1;
}

static void
vq_packed_ring_test(void)
{
	struct spdk_vhost_session vs = {};
	struct spdk_vhost_virtqueue vq = {};
	struct vring_packed_desc descs[4];
	uint16_t guest_last_avail_idx = 0, guest_last_used_idx = 0;
	uint16_t guest_avail_phase = 1, guest_used_phase = 1;
	int i;
	int16_t chain_num;

	vq.vring.desc_packed = descs;
	vq.vring.size = 4;

	/* avail and used wrap counter are initialized to 1 */
	vq.packed.avail_phase = 1;
	vq.packed.used_phase = 1;
	vq.packed.packed_ring = true;
	memset(descs, 0, sizeof(descs));

	CU_ASSERT(vhost_vq_packed_ring_is_avail(&vq) == false);

	/* Guest send requests */
	for (i = 0; i < vq.vring.size; i++) {
		descs[guest_last_avail_idx].id = i;
		/* Set the desc available */
		vq_desc_guest_set_avail(&vq, &guest_last_avail_idx, &guest_avail_phase);
	}
	CU_ASSERT(guest_last_avail_idx == 0);
	CU_ASSERT(guest_avail_phase == 0);

	/* Host handle available descs */
	CU_ASSERT(vhost_vq_packed_ring_is_avail(&vq) == true);
	i = 0;
	while (vhost_vq_packed_ring_is_avail(&vq)) {
		CU_ASSERT(vhost_vring_packed_desc_get_buffer_id(&vq, vq.last_avail_idx, &chain_num) == i++);
		CU_ASSERT(chain_num == 1);
	}

	/* Host complete them out of order: 1, 0, 2. */
	vhost_vq_packed_ring_enqueue(&vs, &vq, 1, 1, 1, 0);
	vhost_vq_packed_ring_enqueue(&vs, &vq, 1, 0, 1, 0);
	vhost_vq_packed_ring_enqueue(&vs, &vq, 1, 2, 1, 0);

	/* Host has got all the available request but only complete three requests */
	CU_ASSERT(vq.last_avail_idx == 0);
	CU_ASSERT(vq.packed.avail_phase == 0);
	CU_ASSERT(vq.last_used_idx == 3);
	CU_ASSERT(vq.packed.used_phase == 1);

	/* Guest handle completed requests */
	CU_ASSERT(vq_desc_guest_handle_completed_desc(&vq, &guest_last_used_idx, &guest_used_phase) == 1);
	CU_ASSERT(vq_desc_guest_handle_completed_desc(&vq, &guest_last_used_idx, &guest_used_phase) == 0);
	CU_ASSERT(vq_desc_guest_handle_completed_desc(&vq, &guest_last_used_idx, &guest_used_phase) == 2);
	CU_ASSERT(guest_last_used_idx == 3);
	CU_ASSERT(guest_used_phase == 1);

	/* There are three descs available the guest can send three request again */
	for (i = 0; i < 3; i++) {
		descs[guest_last_avail_idx].id = 2 - i;
		/* Set the desc available */
		vq_desc_guest_set_avail(&vq, &guest_last_avail_idx, &guest_avail_phase);
	}

	/* Host handle available descs */
	CU_ASSERT(vhost_vq_packed_ring_is_avail(&vq) == true);
	i = 2;
	while (vhost_vq_packed_ring_is_avail(&vq)) {
		CU_ASSERT(vhost_vring_packed_desc_get_buffer_id(&vq, vq.last_avail_idx, &chain_num) == i--);
		CU_ASSERT(chain_num == 1);
	}

	/* There are four requests in Host, the new three ones and left one */
	CU_ASSERT(vq.last_avail_idx == 3);
	/* Available wrap conter should overturn */
	CU_ASSERT(vq.packed.avail_phase == 0);

	/* Host complete all the requests */
	vhost_vq_packed_ring_enqueue(&vs, &vq, 1, 1, 1, 0);
	vhost_vq_packed_ring_enqueue(&vs, &vq, 1, 0, 1, 0);
	vhost_vq_packed_ring_enqueue(&vs, &vq, 1, 3, 1, 0);
	vhost_vq_packed_ring_enqueue(&vs, &vq, 1, 2, 1, 0);

	CU_ASSERT(vq.last_used_idx == vq.last_avail_idx);
	CU_ASSERT(vq.packed.used_phase == vq.packed.avail_phase);

	/* Guest handle completed requests */
	CU_ASSERT(vq_desc_guest_handle_completed_desc(&vq, &guest_last_used_idx, &guest_used_phase) == 1);
	CU_ASSERT(vq_desc_guest_handle_completed_desc(&vq, &guest_last_used_idx, &guest_used_phase) == 0);
	CU_ASSERT(vq_desc_guest_handle_completed_desc(&vq, &guest_last_used_idx, &guest_used_phase) == 3);
	CU_ASSERT(vq_desc_guest_handle_completed_desc(&vq, &guest_last_used_idx, &guest_used_phase) == 2);

	CU_ASSERT(guest_last_avail_idx == guest_last_used_idx);
	CU_ASSERT(guest_avail_phase == guest_used_phase);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("vhost_suite", test_setup, NULL);

	CU_ADD_TEST(suite, desc_to_iov_test);
	CU_ADD_TEST(suite, create_controller_test);
	CU_ADD_TEST(suite, session_find_by_vid_test);
	CU_ADD_TEST(suite, remove_controller_test);
	CU_ADD_TEST(suite, vq_avail_ring_get_test);
	CU_ADD_TEST(suite, vq_packed_ring_test);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
