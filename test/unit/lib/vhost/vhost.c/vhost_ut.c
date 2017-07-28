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

#include "vhost.c"

int
rte_vhost_driver_unregister(const char *path)
{
	return 0;
}

struct spdk_event *
spdk_event_allocate(uint32_t lcore, spdk_event_fn fn, void *arg1, void *arg2)
{
	return NULL;
}

void
spdk_mem_register(void *vaddr, size_t len)
{
}

void
spdk_mem_unregister(void *vaddr, size_t len)
{
}

int
spdk_iommu_mem_register(uint64_t addr, uint64_t len)
{
	return 0;
}

uint64_t
spdk_app_get_core_mask(void)
{
	return 0;
}

void
spdk_app_stop(int rc)
{
}

void
spdk_event_call(struct spdk_event *event)
{
}

int
spdk_iommu_mem_unregister(uint64_t addr, uint64_t len)
{
	return 0;
}

int
rte_vhost_get_mem_table(int vid, struct rte_vhost_memory **mem)
{
	return 0;
}

int
rte_vhost_get_negotiated_features(int vid, uint64_t *features)
{
	return 0;
}

int
rte_vhost_get_vhost_vring(int vid, uint16_t vring_idx, struct rte_vhost_vring *vring)
{
	return 0;
}

int
rte_vhost_enable_guest_notification(int vid, uint16_t queue_id, int enable)
{
	return 0;
}

int
rte_vhost_get_ifname(int vid, char *buf, size_t len)
{
	return 0;
}

uint16_t
rte_vhost_get_vring_num(int vid)
{
	return 0;
}

int
rte_vhost_driver_start(const char *name)
{
	return 0;
}

int
rte_vhost_driver_callback_register(const char *path, struct vhost_device_ops const * const ops)
{
	return 0;
}

int
rte_vhost_driver_disable_features(const char *path, uint64_t features)
{
	return 0;
}

int
rte_vhost_driver_set_features(const char *path, uint64_t features)
{
	return 0;
}

int
rte_vhost_driver_register(const char *path, uint64_t flags)
{
	return 0;
}

int
spdk_vhost_scsi_controller_construct(void)
{
	return 0;
}

int spdk_vhost_blk_controller_construct(void)
{
	return 0;
}

int
rte_vhost_set_vhost_vring_last_idx(int vid, uint16_t vring_idx, uint16_t last_avail_idx,
				   uint16_t last_used_idx)
{
	return 0;
}

static int
test_setup(void)
{
	return 0;
}

static void
null_test(void)
{
	CU_ASSERT(true);
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
		CU_add_test(suite, "null test", null_test) == NULL
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
