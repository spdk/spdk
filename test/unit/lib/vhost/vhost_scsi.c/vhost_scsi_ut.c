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
#include "vhost/vhost_scsi.c"
#include "scsi/scsi_internal.h"
#include "unit/lib/vhost/test_vhost.c"

#include "spdk/env.h"

DEFINE_STUB_V(spdk_scsi_task_put, (struct spdk_scsi_task *task));
DEFINE_STUB(spdk_scsi_dev_allocate_io_channels, int, (struct spdk_scsi_dev *dev), 0);
DEFINE_STUB_P(spdk_scsi_lun_get_bdev_name, const char, (const struct spdk_scsi_lun *lun), {0});
DEFINE_STUB(spdk_scsi_lun_get_id, int, (const struct spdk_scsi_lun *lun), 0);
DEFINE_STUB(spdk_scsi_dev_has_pending_tasks, bool, (const struct spdk_scsi_dev *dev), false);
DEFINE_STUB_V(spdk_scsi_dev_free_io_channels, (struct spdk_scsi_dev *dev));
DEFINE_STUB_V(spdk_scsi_dev_destruct, (struct spdk_scsi_dev *dev));
DEFINE_STUB_V(spdk_scsi_dev_queue_task, (struct spdk_scsi_dev *dev, struct spdk_scsi_task *task));
DEFINE_STUB_V(spdk_scsi_dev_queue_mgmt_task, (struct spdk_scsi_dev *dev,
		struct spdk_scsi_task *task, enum spdk_scsi_task_func func));
DEFINE_STUB_P(spdk_scsi_dev_find_port_by_id, struct spdk_scsi_port, (struct spdk_scsi_dev *dev,
		uint64_t id), {0});
DEFINE_STUB_V(spdk_scsi_task_construct, (struct spdk_scsi_task *task, spdk_scsi_task_cpl cpl_fn,
		spdk_scsi_task_free free_fn));
DEFINE_STUB_P(spdk_scsi_dev_get_lun, struct spdk_scsi_lun, (struct spdk_scsi_dev *dev, int lun_id), {0});
DEFINE_STUB_V(spdk_scsi_task_process_null_lun, (struct spdk_scsi_task *task));
DEFINE_STUB_P(spdk_scsi_lun_get_dev, const struct spdk_scsi_dev, (const struct spdk_scsi_lun *lun), {0});
DEFINE_STUB_P(spdk_scsi_dev_get_name, const char, (const struct spdk_scsi_dev *dev), {0});
DEFINE_STUB_P(spdk_scsi_dev_construct, struct spdk_scsi_dev, (const char *name,
		const char *bdev_name_list[], int *lun_id_list, int num_luns, uint8_t protocol_id,
		void (*hotremove_cb)(const struct spdk_scsi_lun *, void *), void *hotremove_ctx), {0});
DEFINE_STUB(spdk_scsi_dev_add_port, int, (struct spdk_scsi_dev *dev, uint64_t id, const char *name),
	    0);

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
	svdev->vdev.registered = true;
	svdev->vdev.backend = &spdk_vhost_scsi_device_backend;
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
	MOCK_SET(spdk_vhost_dev_unregister_fail, bool, false);
	rc = spdk_vhost_scsi_dev_remove(g_spdk_vhost_device);
	CU_ASSERT(rc == 0);
}

static void
vhost_scsi_dev_remove_test(void)
{
	int rc;
	struct spdk_vhost_scsi_dev *svdev = NULL;
	struct spdk_scsi_dev *scsi_dev;

	MOCK_SET(spdk_vhost_dev_unregister_fail, bool, false);

	/* Try to remove controller which is occupied */
	svdev = alloc_svdev();
	scsi_dev = alloc_scsi_dev();
	svdev->scsi_dev[0] = scsi_dev;
	rc = spdk_vhost_scsi_dev_remove(&svdev->vdev);
	CU_ASSERT(rc == -EBUSY);
	free(scsi_dev);
	svdev->scsi_dev[0] = NULL;

	/* Failed to remove device */
	MOCK_SET(spdk_vhost_dev_unregister_fail, bool, true);
	rc = spdk_vhost_scsi_dev_remove(&svdev->vdev);
	CU_ASSERT(rc == -1);

	free(svdev);
}

static void
vhost_scsi_dev_construct_test(void)
{
	int rc;

	/* Failed to construct vhost device */
	MOCK_SET(spdk_vhost_dev_register_fail, bool, true);
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
	rc = spdk_vhost_scsi_dev_remove_tgt(&svdev->vdev, SPDK_VHOST_SCSI_CTRLR_MAX_DEVS + 1, NULL,
					    NULL);
	CU_ASSERT(rc == -EINVAL);

	/* Try to remove nonexistent device */
	rc = spdk_vhost_scsi_dev_remove_tgt(&svdev->vdev, 0, NULL, NULL);
	CU_ASSERT(rc == -ENODEV);

	/* Try to remove device when controller is in use */
	svdev->vdev.lcore = 0;
	scsi_dev = alloc_scsi_dev();
	svdev->scsi_dev[0] = scsi_dev;
	MOCK_SET(spdk_vhost_dev_has_feature, bool, false);
	rc = spdk_vhost_scsi_dev_remove_tgt(&svdev->vdev, 0, NULL, NULL);
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
	rc = spdk_vhost_scsi_dev_add_tgt(NULL, 0, "Malloc0");
	CU_ASSERT(rc == -EINVAL);

	svdev = alloc_svdev();
	vdev = &svdev->vdev;
	MOCK_SET(spdk_vhost_dev_has_feature, bool, false);

	/* Add device when max devices is reached */
	rc = spdk_vhost_scsi_dev_add_tgt(vdev,
					 SPDK_VHOST_SCSI_CTRLR_MAX_DEVS + 1, "Malloc0");
	CU_ASSERT(rc == -EINVAL);

	/* Add device but lun has no name */
	rc = spdk_vhost_scsi_dev_add_tgt(vdev, 0, NULL);
	CU_ASSERT(rc == -EINVAL);

	/* Add device but lun has too long name */
	memset(long_name, 'x', sizeof(long_name));
	long_name[SPDK_SCSI_DEV_MAX_NAME] = 0;
	rc = spdk_vhost_scsi_dev_add_tgt(vdev, 0, long_name);
	CU_ASSERT(rc != 0);

	/* Add device to a controller which is in use */
	svdev->vdev.lcore = 0;
	rc = spdk_vhost_scsi_dev_add_tgt(vdev, 0, "Malloc0");
	CU_ASSERT(rc == -ENOTSUP);

	/* Add device to controller with already occupied device */
	vdev->lcore = -1;
	scsi_dev = alloc_scsi_dev();
	svdev->scsi_dev[0] = scsi_dev;
	rc = spdk_vhost_scsi_dev_add_tgt(vdev, 0, "Malloc0");
	CU_ASSERT(rc == -EEXIST);
	free(scsi_dev);
	svdev->scsi_dev[0] = NULL;

	/* Failed to create device */
	MOCK_SET_P(spdk_scsi_dev_construct, struct spdk_scsi_dev *, NULL);
	rc = spdk_vhost_scsi_dev_add_tgt(vdev, 0, "Malloc0");
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
