/*-
 *   BSD LICENSE
 *
 *   Copyright(c) Intel Corporation. All rights reserved.
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
#include "spdk_internal/mock.h"

#include "vhost_scsi.c"

DEFINE_STUB(spdk_vhost_dev_construct, int, (struct spdk_vhost_dev *vdev, const char *name, const char *mask_str,
		 enum spdk_vhost_dev_type type, const struct spdk_vhost_dev_backend *backend), 0);
DEFINE_STUB_V(spdk_scsi_task_put, (struct spdk_scsi_task *task));
DEFINE_STUB(spdk_ring_enqueue, size_t, (struct spdk_ring *ring, void **objs, size_t count), 0);
DEFINE_STUB(spdk_ring_dequeue, size_t, (struct spdk_ring *ring, void **objs, size_t count), 0);
DEFINE_STUB(spdk_scsi_dev_allocate_io_channels, int, (struct spdk_scsi_dev *dev), 0);
DEFINE_STUB_P(spdk_scsi_lun_get_name, const char, (const struct spdk_scsi_lun *lun), {0});
DEFINE_STUB(spdk_scsi_lun_get_id, int, (const struct spdk_scsi_lun *lun), 0);
DEFINE_STUB(spdk_vhost_vq_avail_ring_get, uint16_t, (struct rte_vhost_vring *vq, uint16_t *reqs,
	      uint16_t reqs_len), 0);
DEFINE_STUB_P(spdk_vhost_vq_get_desc, struct vring_desc, (struct rte_vhost_vring *vq, uint16_t req_idx), {0});
DEFINE_STUB_VP(spdk_vhost_gpa_to_vva, (struct spdk_vhost_dev *vdev, uint64_t addr), {0});
DEFINE_STUB_V(spdk_vhost_vq_used_ring_enqueue, (struct spdk_vhost_dev *vdev, struct rte_vhost_vring *vq,
		uint16_t id,
		uint32_t len));
DEFINE_STUB_V(spdk_dma_free, (void *buf));
DEFINE_STUB(spdk_scsi_dev_has_pending_tasks, bool, (const struct spdk_scsi_dev *dev), false);
DEFINE_STUB_V(spdk_scsi_dev_free_io_channels, (struct spdk_scsi_dev *dev));
DEFINE_STUB_V(spdk_scsi_dev_destruct, (struct spdk_scsi_dev *dev));
DEFINE_STUB_VP(spdk_dma_zmalloc, (size_t size, size_t align, uint64_t *phys_addr), {0});
DEFINE_STUB_V(spdk_scsi_dev_queue_task, (struct spdk_scsi_dev *dev, struct spdk_scsi_task *task));
DEFINE_STUB_V(spdk_scsi_dev_queue_mgmt_task, (struct spdk_scsi_dev *dev,
	      struct spdk_scsi_task *task,
	      enum spdk_scsi_task_func func));
//DEFINE_STUB_P(spdk_scsi_dev_find_port_by_id, struct spdk_scsi_port, (struct spdk_scsi_dev *dev, uint64_t id), {0});
DEFINE_STUB_P(spdk_scsi_dev_find_port_by_id, struct spdk_scsi_port, (struct spdk_scsi_dev *dev, uint64_t id), NULL);
DEFINE_STUB_V(spdk_scsi_task_construct, (struct spdk_scsi_task *task,
		 spdk_scsi_task_cpl cpl_fn,
		 spdk_scsi_task_free free_fn,
		 struct spdk_scsi_task *parent));
DEFINE_STUB(spdk_vhost_vring_desc_has_next, bool, (struct vring_desc *cur_desc), false);
DEFINE_STUB_P(spdk_vhost_vring_desc_get_next, struct vring_desc, (struct vring_desc *vq_desc, struct vring_desc *cur_desc), {0});

static int
test_setup(void)
{
	return 0;
}

static void
dev_construct_test(void)
{
	int rc = 0;
	CU_ASSERT(rc == 0);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("vhost_scsi_suite", test_setup, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "dev_construct_", dev_construct_test) == NULL
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
