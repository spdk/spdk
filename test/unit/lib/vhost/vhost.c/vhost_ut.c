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

#include "spdk/stdinc.h"

#include "CUnit/Basic.h"
#include "spdk_cunit.h"
#include "spdk/io_channel.h"
#include "spdk_internal/mock.h"
#include "lib/test_env.c"

#include "vhost.c"

DEFINE_STUB(rte_vhost_driver_unregister, int, (const char *path), 0);
DEFINE_STUB(spdk_event_allocate, struct spdk_event *,
	    (uint32_t lcore, spdk_event_fn fn, void *arg1, void *arg2), NULL);
DEFINE_STUB(spdk_mem_register, int, (void *vaddr, size_t len), 0);
DEFINE_STUB(spdk_mem_unregister, int, (void *vaddr, size_t len), 0);

static struct spdk_cpuset *g_app_core_mask;
struct spdk_cpuset *spdk_app_get_core_mask(void)
{
	if (g_app_core_mask == NULL) {
		g_app_core_mask = spdk_cpuset_alloc();
		spdk_cpuset_set_cpu(g_app_core_mask, 0, true);
	}
	return g_app_core_mask;
}

int
spdk_app_parse_core_mask(const char *mask, struct spdk_cpuset *cpumask)
{
	int ret;
	struct spdk_cpuset *validmask;

	ret = spdk_cpuset_parse(cpumask, mask);
	if (ret < 0) {
		return ret;
	}

	validmask = spdk_app_get_core_mask();
	spdk_cpuset_and(cpumask, validmask);

	return 0;
}

DEFINE_STUB(spdk_env_get_first_core, uint32_t, (void), 0);
DEFINE_STUB(spdk_env_get_next_core, uint32_t, (uint32_t prev_core), 0);
DEFINE_STUB(spdk_env_get_last_core, uint32_t, (void), 0);
DEFINE_STUB_V(spdk_app_stop, (int rc));
DEFINE_STUB_V(spdk_event_call, (struct spdk_event *event));
DEFINE_STUB(spdk_poller_register, struct spdk_poller *, (spdk_poller_fn fn, void *arg,
		uint64_t period_microseconds), NULL);
DEFINE_STUB_V(spdk_poller_unregister, (struct spdk_poller **ppoller));
DEFINE_STUB(spdk_iommu_mem_unregister, int, (uint64_t addr, uint64_t len), 0);
DEFINE_STUB(rte_vhost_get_mem_table, int, (int vid, struct rte_vhost_memory **mem), 0);
DEFINE_STUB(rte_vhost_get_negotiated_features, int, (int vid, uint64_t *features), 0);
DEFINE_STUB(rte_vhost_get_vhost_vring, int,
	    (int vid, uint16_t vring_idx, struct rte_vhost_vring *vring), 0);
DEFINE_STUB(rte_vhost_enable_guest_notification, int,
	    (int vid, uint16_t queue_id, int enable), 0);
DEFINE_STUB(rte_vhost_get_ifname, int, (int vid, char *buf, size_t len), 0);
DEFINE_STUB(rte_vhost_get_vring_num, uint16_t, (int vid), 0);
DEFINE_STUB(rte_vhost_driver_start, int, (const char *name), 0);
DEFINE_STUB(rte_vhost_driver_callback_register, int,
	    (const char *path, struct vhost_device_ops const *const ops), 0);
DEFINE_STUB(rte_vhost_driver_disable_features, int, (const char *path, uint64_t features), 0);
DEFINE_STUB(rte_vhost_driver_set_features, int, (const char *path, uint64_t features), 0);
DEFINE_STUB(rte_vhost_driver_register, int, (const char *path, uint64_t flags), 0);
DEFINE_STUB_V(rte_vhost_log_used_vring, (int vid, uint16_t vring_idx, uint64_t offset,
		uint64_t len));
DEFINE_STUB_V(rte_vhost_log_write, (int vid, uint64_t addr, uint64_t len));
DEFINE_STUB(spdk_vhost_scsi_controller_construct, int, (void), 0);
DEFINE_STUB(spdk_vhost_blk_controller_construct, int, (void), 0);
DEFINE_STUB(rte_vhost_set_vhost_vring_last_idx, int,
	    (int vid, uint16_t vring_idx, uint16_t last_avail_idx, uint16_t last_used_idx), 0);
DEFINE_STUB(spdk_env_get_current_core, uint32_t, (void), 0);

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
	rc = spdk_vhost_dev_register(vdev, name, cpumask, &g_vdev_backend);
	if (rc == 0) {
		*vdev_p = vdev;
	} else {
		free(vdev);
	}

	return rc;
}

static void
start_vdev(struct spdk_vhost_dev *vdev)
{
	vdev->vid = 0;
	vdev->lcore = 0;
	vdev->mem = calloc(1, sizeof(*vdev->mem) + 2 * sizeof(struct rte_vhost_mem_region));
	SPDK_CU_ASSERT_FATAL(vdev->mem != NULL);
	vdev->mem->nregions = 2;
	vdev->mem->regions[0].guest_phys_addr = 0;
	vdev->mem->regions[0].size = 0x400000; /* 4 MB */
	vdev->mem->regions[0].host_user_addr = 0x1000000;
	vdev->mem->regions[1].guest_phys_addr = 0x400000;
	vdev->mem->regions[1].size = 0x400000; /* 4 MB */
	vdev->mem->regions[1].host_user_addr = 0x2000000;
}

static void
stop_vdev(struct spdk_vhost_dev *vdev)
{
	free(vdev->mem);
	vdev->mem = NULL;
	vdev->vid = -1;
}

static void
cleanup_vdev(struct spdk_vhost_dev *vdev)
{
	stop_vdev(vdev);
	spdk_vhost_dev_unregister(vdev);
	free(vdev);
}

static void
desc_to_iov_test(void)
{
	struct spdk_vhost_dev *vdev;
	struct iovec iov[SPDK_VHOST_IOVS_MAX];
	uint16_t iov_index;
	struct vring_desc desc;
	int rc;

	rc = alloc_vdev(&vdev, "vdev_name_0", "0x1");
	CU_ASSERT(rc == 0);
	start_vdev(vdev);

	/* Test simple case where iov falls fully within a 2MB page. */
	desc.addr = 0x110000;
	desc.len = 0x1000;
	iov_index = 0;
	rc = spdk_vhost_vring_desc_to_iov(vdev, iov, &iov_index, &desc);
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
	rc = spdk_vhost_vring_desc_to_iov(vdev, iov, &iov_index, &desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(iov_index == SPDK_VHOST_IOVS_MAX);
	CU_ASSERT(iov[SPDK_VHOST_IOVS_MAX - 1].iov_base == (void *)0x1110000);
	CU_ASSERT(iov[SPDK_VHOST_IOVS_MAX - 1].iov_len == 0x1000);
	memset(iov, 0, sizeof(iov));

	/* Test for failure if iov_index already equals SPDK_VHOST_IOVS_MAX. */
	iov_index = SPDK_VHOST_IOVS_MAX;
	rc = spdk_vhost_vring_desc_to_iov(vdev, iov, &iov_index, &desc);
	CU_ASSERT(rc != 0);
	memset(iov, 0, sizeof(iov));

	/* Test case where iov spans a 2MB boundary, but does not span a vhost memory region. */
	desc.addr = 0x1F0000;
	desc.len = 0x20000;
	iov_index = 0;
	rc = spdk_vhost_vring_desc_to_iov(vdev, iov, &iov_index, &desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(iov_index == 1);
	CU_ASSERT(iov[0].iov_base == (void *)0x11F0000);
	CU_ASSERT(iov[0].iov_len == 0x20000);
	memset(iov, 0, sizeof(iov));

	/* Same test, but ensure it respects the non-zero starting iov_index. */
	iov_index = SPDK_VHOST_IOVS_MAX - 1;
	rc = spdk_vhost_vring_desc_to_iov(vdev, iov, &iov_index, &desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(iov_index == SPDK_VHOST_IOVS_MAX);
	CU_ASSERT(iov[SPDK_VHOST_IOVS_MAX - 1].iov_base == (void *)0x11F0000);
	CU_ASSERT(iov[SPDK_VHOST_IOVS_MAX - 1].iov_len == 0x20000);
	memset(iov, 0, sizeof(iov));

	/* Test case where iov spans a vhost memory region. */
	desc.addr = 0x3F0000;
	desc.len = 0x20000;
	iov_index = 0;
	rc = spdk_vhost_vring_desc_to_iov(vdev, iov, &iov_index, &desc);
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

	/* NOTE: spdk_app_get_core_mask stub always sets coremask 0x01 */

	/* Create device with no name */
	ret = alloc_vdev(&vdev, NULL, "0x1");
	CU_ASSERT(ret != 0);

	/* Create device with incorrect cpumask */
	ret = alloc_vdev(&vdev, "vdev_name_0", "0x2");
	CU_ASSERT(ret != 0);

	/* Create device with too long name and path */
	memset(long_name, 'x', sizeof(long_name));
	long_name[PATH_MAX - 1] = 0;
	snprintf(dev_dirname, sizeof(dev_dirname), "some_path/");
	ret = alloc_vdev(&vdev, long_name, "0x1");
	CU_ASSERT(ret != 0);
	dev_dirname[0] = 0;

	/* Create device when device name is already taken */
	ret = alloc_vdev(&vdev, "vdev_name_0", "0x1");
	CU_ASSERT(ret == 0);
	ret = alloc_vdev(&vdev2, "vdev_name_0", "0x1");
	CU_ASSERT(ret != 0);
	cleanup_vdev(vdev);
}

static void
dev_find_by_vid_test(void)
{
	struct spdk_vhost_dev *vdev, *tmp;
	int rc;

	rc = alloc_vdev(&vdev, "vdev_name_0", "0x1");
	CU_ASSERT(rc == 0);

	tmp = spdk_vhost_dev_find_by_vid(vdev->vid);
	CU_ASSERT(tmp == vdev);

	/* Search for a device with incorrect vid */
	tmp = spdk_vhost_dev_find_by_vid(vdev->vid + 0xFF);
	CU_ASSERT(tmp == NULL);

	cleanup_vdev(vdev);
}

static void
remove_controller_test(void)
{
	struct spdk_vhost_dev *vdev;
	int ret;

	ret = alloc_vdev(&vdev, "vdev_name_0", "0x1");
	CU_ASSERT(ret == 0);

	/* Remove device when controller is in use */
	start_vdev(vdev);
	ret = spdk_vhost_dev_unregister(vdev);
	CU_ASSERT(ret != 0);

	cleanup_vdev(vdev);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("vhost_suite", test_setup, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "desc_to_iov", desc_to_iov_test) == NULL ||
		CU_add_test(suite, "create_controller", create_controller_test) == NULL ||
		CU_add_test(suite, "dev_find_by_vid", dev_find_by_vid_test) == NULL ||
		CU_add_test(suite, "remove_controller", remove_controller_test) == NULL
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
