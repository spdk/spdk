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
#include "lib/test_env.c"

#include "spdk/scsi.h"
#include "vhost_scsi.c"
#include "../scsi/scsi_internal.h"

#include "spdk/env.h"

struct spdk_conf_section {
	struct spdk_conf_section *next;
	char *name;
	int num;
	struct spdk_conf_item *item;
};

DEFINE_STUB_V(spdk_scsi_task_put, (struct spdk_scsi_task *task));
DEFINE_STUB(spdk_ring_enqueue, size_t, (struct spdk_ring *ring, void **objs, size_t count), 0);
DEFINE_STUB(spdk_ring_dequeue, size_t, (struct spdk_ring *ring, void **objs, size_t count), 0);
DEFINE_STUB(spdk_scsi_dev_allocate_io_channels, int, (struct spdk_scsi_dev *dev), 0);
DEFINE_STUB_P(spdk_scsi_lun_get_name, const char, (const struct spdk_scsi_lun *lun), {0});
DEFINE_STUB(spdk_scsi_lun_get_id, int, (const struct spdk_scsi_lun *lun), 0);
DEFINE_STUB(spdk_vhost_vq_avail_ring_get, uint16_t, (struct rte_vhost_vring *vq, uint16_t *reqs,
		uint16_t reqs_len), 0);
DEFINE_STUB(spdk_vhost_vq_get_desc, int, (struct spdk_vhost_dev *vdev, struct rte_vhost_vring *vq,
		uint16_t req_idx, struct vring_desc **desc, struct vring_desc **desc_table,
		uint32_t *desc_table_size), 0);
DEFINE_STUB_VP(spdk_vhost_gpa_to_vva, (struct spdk_vhost_dev *vdev, uint64_t addr), {0});
DEFINE_STUB_V(spdk_vhost_vq_used_ring_enqueue, (struct spdk_vhost_dev *vdev,
		struct rte_vhost_vring *vq, uint16_t id, uint32_t len));
DEFINE_STUB(spdk_scsi_dev_has_pending_tasks, bool, (const struct spdk_scsi_dev *dev), false);
DEFINE_STUB_V(spdk_scsi_dev_free_io_channels, (struct spdk_scsi_dev *dev));
DEFINE_STUB_V(spdk_scsi_dev_destruct, (struct spdk_scsi_dev *dev));
DEFINE_STUB_V(spdk_scsi_dev_queue_task, (struct spdk_scsi_dev *dev, struct spdk_scsi_task *task));
DEFINE_STUB_V(spdk_scsi_dev_queue_mgmt_task, (struct spdk_scsi_dev *dev,
		struct spdk_scsi_task *task, enum spdk_scsi_task_func func));
DEFINE_STUB_P(spdk_scsi_dev_find_port_by_id, struct spdk_scsi_port, (struct spdk_scsi_dev *dev,
		uint64_t id), {0});
DEFINE_STUB_V(spdk_scsi_task_construct, (struct spdk_scsi_task *task, spdk_scsi_task_cpl cpl_fn,
		spdk_scsi_task_free free_fn, struct spdk_scsi_task *parent));
DEFINE_STUB(spdk_vhost_vring_desc_has_next, bool, (struct vring_desc *cur_desc), false);
DEFINE_STUB(spdk_vhost_vring_desc_get_next, int, (struct vring_desc **desc,
		struct vring_desc *desc_table, uint32_t desc_table_size), 0);
DEFINE_STUB_P(spdk_scsi_dev_get_lun, struct spdk_scsi_lun, (struct spdk_scsi_dev *dev, int lun_id), {0});
DEFINE_STUB(spdk_vhost_vring_desc_is_wr, bool, (struct vring_desc *cur_desc), false);
DEFINE_STUB(spdk_vhost_vring_desc_to_iov, int, (struct spdk_vhost_dev *vdev, struct iovec *iov,
		uint16_t *iov_index, const struct vring_desc *desc), 0);
DEFINE_STUB_V(spdk_scsi_task_process_null_lun, (struct spdk_scsi_task *task));
DEFINE_STUB_V(spdk_vhost_dev_mem_register, (struct spdk_vhost_dev *vdev));
DEFINE_STUB_V(spdk_poller_register, (struct spdk_poller **ppoller, spdk_poller_fn fn, void *arg,
				     uint32_t lcore, uint64_t period_microseconds));
DEFINE_STUB_V(spdk_ring_free, (struct spdk_ring *ring));
DEFINE_STUB(spdk_vhost_dev_has_feature, bool, (struct spdk_vhost_dev *vdev, unsigned feature_id),
	    false);
DEFINE_STUB_P(spdk_scsi_lun_get_dev, const struct spdk_scsi_dev, (const struct spdk_scsi_lun *lun), {0});
DEFINE_STUB_P(spdk_scsi_dev_get_name, const char, (const struct spdk_scsi_dev *dev), {0});
DEFINE_STUB_V(spdk_vhost_dev_mem_unregister, (struct spdk_vhost_dev *vdev));
DEFINE_STUB_P(spdk_vhost_dev_find, struct spdk_vhost_dev, (const char *ctrlr_name), {0});
DEFINE_STUB_P(spdk_scsi_dev_construct, struct spdk_scsi_dev, (const char *name,
		char *lun_name_list[], int *lun_id_list, int num_luns, uint8_t protocol_id,
		void (*hotremove_cb)(const struct spdk_scsi_lun *, void *), void *hotremove_ctx), {0});
DEFINE_STUB(spdk_scsi_dev_add_port, int, (struct spdk_scsi_dev *dev, uint64_t id, const char *name),
	    0);
DEFINE_STUB_P(spdk_conf_first_section, struct spdk_conf_section, (struct spdk_conf *cp), {0});
DEFINE_STUB_P(spdk_conf_section_get_nmval, char, (struct spdk_conf_section *sp, const char *key,
		int idx1, int idx2), {0});
DEFINE_STUB(spdk_conf_section_match_prefix, bool, (const struct spdk_conf_section *sp,
		const char *name_prefix), false);
DEFINE_STUB_P(spdk_conf_next_section, struct spdk_conf_section, (struct spdk_conf_section *sp), {0});
DEFINE_STUB_P(spdk_conf_section_get_name, const char, (const struct spdk_conf_section *sp), {0});
DEFINE_STUB(spdk_env_get_socket_id, uint32_t, (uint32_t core), 0);
DEFINE_STUB(spdk_vhost_event_send, int, (struct spdk_vhost_dev *vdev, spdk_vhost_event_fn cb_fn,
		void *arg, unsigned timeout_sec, const char *errmsg), 0);
DEFINE_STUB_V(spdk_poller_unregister, (struct spdk_poller **ppoller, struct spdk_event *complete));
DEFINE_STUB(spdk_json_write_name, int, (struct spdk_json_write_ctx *w, const char *name), 0);
DEFINE_STUB(spdk_json_write_object_begin, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_uint32, int, (struct spdk_json_write_ctx *w, uint32_t val), 0);
DEFINE_STUB(spdk_scsi_dev_get_id, int, (const struct spdk_scsi_dev *dev), {0});
DEFINE_STUB(spdk_json_write_int32, int, (struct spdk_json_write_ctx *w, int32_t val), 0);
DEFINE_STUB(spdk_json_write_string, int, (struct spdk_json_write_ctx *w, const char *val), 0);
DEFINE_STUB(spdk_json_write_array_begin, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_object_end, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_array_end, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB_V(spdk_vhost_dev_backend_event_done, (void *event_ctx, int response));
DEFINE_STUB_V(spdk_vhost_lock, (void));
DEFINE_STUB_V(spdk_vhost_unlock, (void));

/* This sets spdk_vhost_dev_remove to either to fail or success */
DEFINE_STUB(spdk_vhost_dev_remove_fail, bool, (void), false);
/* This sets spdk_vhost_dev_construct to either to fail or success */
DEFINE_STUB(spdk_vhost_dev_construct_fail, bool, (void), false);

static struct spdk_vhost_dev *g_spdk_vhost_device;
int
spdk_vhost_dev_construct(struct spdk_vhost_dev *vdev, const char *name, const char *mask_str,
			 enum spdk_vhost_dev_type type, const struct spdk_vhost_dev_backend *backend)
{
	if (spdk_vhost_dev_construct_fail()) {
		return -1;
	}

	g_spdk_vhost_device = vdev;
	return 0;
}

int
spdk_vhost_dev_remove(struct spdk_vhost_dev *vdev)
{
	if (spdk_vhost_dev_remove_fail()) {
		return -1;
	}

	free(vdev->name);
	g_spdk_vhost_device = NULL;
	return 0;
}

struct spdk_ring *
spdk_ring_create(enum spdk_ring_type type, size_t count, int socket_id)
{
	return NULL;
}

char *
spdk_conf_section_get_nval(struct spdk_conf_section *sp, const char *key, int idx)
{
	if (idx == 0) {
		return "0";
	}

	return NULL;
}

char *
spdk_conf_section_get_val(struct spdk_conf_section *sp, const char *key)
{
	if (strcmp(key, "Name") == 0) {
		return "Vhost.0";
	} else if (strcmp(key, "Cpumask") == 0) {
		return "0x1";
	}

	return NULL;
}

static int
test_setup(void)
{
	return 0;
}

static struct spdk_vhost_scsi_dev *
alloc_svdev(void)
{
	struct spdk_vhost_scsi_dev *svdev = spdk_dma_zmalloc(sizeof(struct spdk_vhost_scsi_dev),
					    SPDK_CACHE_LINE_SIZE, NULL);

	SPDK_CU_ASSERT_FATAL(svdev != NULL);
	return svdev;
}

static struct spdk_scsi_dev *
alloc_scsi_dev(void)
{
	struct spdk_scsi_dev *sdev;

	sdev = calloc(1, sizeof(*sdev));
	return sdev;
}

static void
vhost_scsi_controller_construct_test(void)
{
	int rc;

	MOCK_SET_P(spdk_conf_next_section, struct spdk_conf_section *, NULL);

	/* VhostScsi section has non numeric suffix */
	MOCK_SET(spdk_conf_section_match_prefix, bool, true);
	MOCK_SET_P(spdk_conf_section_get_name, const char *, "VhostScsix");
	rc = spdk_vhost_scsi_controller_construct();
	CU_ASSERT(rc != 0);

	/* Dev number has no value */
	MOCK_SET_P(spdk_conf_section_get_name, const char *, "VhostScsi0");
	MOCK_SET_P(spdk_conf_section_get_nmval, char *, NULL);
	rc = spdk_vhost_scsi_controller_construct();
	CU_ASSERT(rc != 0);
	/*
	 * Expecting that device has been created during the test but wasn't initialized as
	 * spdk_vhost_scsi_controller_construct failed after creating device
	 */
	CU_ASSERT(g_spdk_vhost_device != NULL);

	/* Remove created device */
	MOCK_SET(spdk_vhost_dev_remove_fail, bool, false);
	rc = spdk_vhost_scsi_dev_remove(g_spdk_vhost_device);
	CU_ASSERT(rc == 0);
}

static void
vhost_scsi_dev_remove_test(void)
{
	int rc;
	struct spdk_vhost_scsi_dev *svdev = NULL;
	struct spdk_scsi_dev *scsi_dev;

	MOCK_SET(spdk_vhost_dev_remove_fail, bool, false);

	/* Try to remove controller which is occupied */
	svdev = alloc_svdev();
	scsi_dev = alloc_scsi_dev();
	svdev->scsi_dev[0] = scsi_dev;
	rc = spdk_vhost_scsi_dev_remove(&svdev->vdev);
	CU_ASSERT(rc == -EBUSY);
	free(scsi_dev);
	svdev->scsi_dev[0] = NULL;

	/* Failed to remove device */
	MOCK_SET(spdk_vhost_dev_remove_fail, bool, true);
	rc = spdk_vhost_scsi_dev_remove(&svdev->vdev);
	CU_ASSERT(rc == -1);

	free(svdev);
}

static void
vhost_scsi_dev_construct_test(void)
{
	int rc;

	/* Failed to construct vhost device */
	MOCK_SET(spdk_vhost_dev_construct_fail, bool, true);
	rc = spdk_vhost_scsi_dev_construct("vhost.0", "0x1");
	CU_ASSERT(rc != 0);
}

static void
vhost_scsi_dev_remove_dev_test(void)
{
	int rc;
	struct spdk_vhost_scsi_dev *svdev;
	struct spdk_scsi_dev *scsi_dev;

	svdev = alloc_svdev();
	svdev->vdev.name = strdup("vhost.0");

	/* Invalid device number */
	rc = spdk_vhost_scsi_dev_remove_dev(&svdev->vdev, SPDK_VHOST_SCSI_CTRLR_MAX_DEVS + 1, NULL, NULL);
	CU_ASSERT(rc == -EINVAL);

	/* Try to remove nonexistent device */
	rc = spdk_vhost_scsi_dev_remove_dev(&svdev->vdev, 0, NULL, NULL);
	CU_ASSERT(rc == -ENODEV);

	/* Try to remove device when controller is in use */
	svdev->vdev.lcore = 0;
	scsi_dev = alloc_scsi_dev();
	svdev->scsi_dev[0] = scsi_dev;
	MOCK_SET(spdk_vhost_dev_has_feature, bool, false);
	rc = spdk_vhost_scsi_dev_remove_dev(&svdev->vdev, 0, NULL, NULL);
	CU_ASSERT(rc == -ENOTSUP);
	free(scsi_dev);
	free(svdev->vdev.name);
	free(svdev);
}

static void
vhost_scsi_dev_add_dev_test(void)
{
	int rc;
	char long_name[SPDK_SCSI_DEV_MAX_NAME + 1];
	struct spdk_vhost_scsi_dev *svdev;
	struct spdk_vhost_dev *vdev;
	struct spdk_scsi_dev *scsi_dev;

	/* Add device to controller without name */
	rc = spdk_vhost_scsi_dev_add_dev(NULL, 0, "Malloc0");
	CU_ASSERT(rc == -EINVAL);

	svdev = alloc_svdev();
	vdev = &svdev->vdev;
	MOCK_SET(spdk_vhost_dev_has_feature, bool, false);

	/* Add device when max devices is reached */
	rc = spdk_vhost_scsi_dev_add_dev(vdev,
					 SPDK_VHOST_SCSI_CTRLR_MAX_DEVS + 1, "Malloc0");
	CU_ASSERT(rc == -EINVAL);

	/* Add device but lun has no name */
	rc = spdk_vhost_scsi_dev_add_dev(vdev, 0, NULL);
	CU_ASSERT(rc == -EINVAL);

	/* Add device but lun has too long name */
	memset(long_name, 'x', sizeof(long_name));
	long_name[SPDK_SCSI_DEV_MAX_NAME] = 0;
	rc = spdk_vhost_scsi_dev_add_dev(vdev, 0, long_name);
	CU_ASSERT(rc != 0);

	/* Add device to a controller which is in use */
	svdev->vdev.lcore = 0;
	rc = spdk_vhost_scsi_dev_add_dev(vdev, 0, "Malloc0");
	CU_ASSERT(rc == -ENOTSUP);

	/* Add device to controller with already occupied device */
	vdev->lcore = -1;
	scsi_dev = alloc_scsi_dev();
	svdev->scsi_dev[0] = scsi_dev;
	rc = spdk_vhost_scsi_dev_add_dev(vdev, 0, "Malloc0");
	CU_ASSERT(rc == -EEXIST);
	free(scsi_dev);
	svdev->scsi_dev[0] = NULL;

	/* Failed to create device */
	MOCK_SET_P(spdk_scsi_dev_construct, struct spdk_scsi_dev *, NULL);
	rc = spdk_vhost_scsi_dev_add_dev(vdev, 0, "Malloc0");
	CU_ASSERT(rc == -EINVAL);

	free(svdev);
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
		CU_add_test(suite, "vhost_scsi_controller_construct",
			    vhost_scsi_controller_construct_test) == NULL ||
		CU_add_test(suite, "vhost_scsi_dev_remove_dev", vhost_scsi_dev_remove_dev_test) == NULL ||
		CU_add_test(suite, "vhost_scsi_dev_remove", vhost_scsi_dev_remove_test) == NULL ||
		CU_add_test(suite, "vhost_scsi_dev_construct", vhost_scsi_dev_construct_test) == NULL ||
		CU_add_test(suite, "vhost_scsi_dev_add_dev", vhost_scsi_dev_add_dev_test) == NULL
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
