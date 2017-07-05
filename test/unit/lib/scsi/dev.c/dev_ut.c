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

#include "dev.c"
#include "port.c"

/* Unit test bdev mockup */
struct spdk_bdev {
	char name[100];
};

static struct spdk_bdev g_bdev = {};

struct lun_entry {
	TAILQ_ENTRY(lun_entry) lun_entries;
	struct spdk_scsi_lun *lun;
};
TAILQ_HEAD(, lun_entry) g_lun_head;

static int
test_setup(void)
{
	TAILQ_INIT(&g_lun_head);
	return 0;
}

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return "test";
}

static struct spdk_scsi_task *
spdk_get_task(uint32_t *owner_task_ctr)
{
	struct spdk_scsi_task *task;

	task = calloc(1, sizeof(*task));
	if (!task) {
		return NULL;
	}

	return task;
}

void
spdk_scsi_task_put(struct spdk_scsi_task *task)
{
	free(task);
}

_spdk_scsi_lun *
spdk_scsi_lun_construct(const char *name, struct spdk_bdev *bdev,
			void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
			void *hotremove_ctx)
{
	struct spdk_scsi_lun *lun;

	lun = calloc(1, sizeof(struct spdk_scsi_lun));
	SPDK_CU_ASSERT_FATAL(lun != NULL);

	snprintf(lun->name, sizeof(lun->name), "%s", name);
	lun->bdev = bdev;
	return lun;
}

int
spdk_scsi_lun_destruct(struct spdk_scsi_lun *lun)
{
	free(lun);
	return 0;
}

struct spdk_bdev *
spdk_bdev_get_by_name(const char *bdev_name)
{
	snprintf(g_bdev.name, sizeof(g_bdev.name), "%s", bdev_name);
	return &g_bdev;
}

int
spdk_scsi_lun_claim(struct spdk_scsi_lun *lun)
{
	struct lun_entry *p;

	TAILQ_FOREACH(p, &g_lun_head, lun_entries) {
		CU_ASSERT_FATAL(p->lun != NULL);
		if (strncmp(p->lun->name, lun->name, sizeof(lun->name)) == 0)
			return -1;
	}

	p = calloc(1, sizeof(struct lun_entry));
	SPDK_CU_ASSERT_FATAL(p != NULL);

	p->lun = lun;

	TAILQ_INSERT_TAIL(&g_lun_head, p, lun_entries);
	return 0;
}

int
spdk_scsi_lun_unclaim(struct spdk_scsi_lun *lun)
{
	struct lun_entry *p, *tmp;

	TAILQ_FOREACH_SAFE(p, &g_lun_head, lun_entries, tmp) {
		CU_ASSERT_FATAL(p->lun != NULL);
		if (strncmp(p->lun->name, lun->name, sizeof(lun->name)) == 0) {
			TAILQ_REMOVE(&g_lun_head, p, lun_entries);
			free(p);
			return 0;
		}
	}
	return 0;
}

int
spdk_scsi_lun_task_mgmt_execute(struct spdk_scsi_task *task, enum spdk_scsi_task_func func)
{
	return 0;
}

int
spdk_scsi_lun_append_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task)
{
	return 0;
}

void
spdk_scsi_lun_execute_tasks(struct spdk_scsi_lun *lun)
{
}

int
spdk_scsi_lun_allocate_io_channel(struct spdk_scsi_lun *lun)
{
	return 0;
}

void
spdk_scsi_lun_free_io_channel(struct spdk_scsi_lun *lun)
{
}

bool
spdk_scsi_lun_has_pending_tasks(const struct spdk_scsi_lun *lun)
{
	return false;
}

static void
dev_destruct_null_dev(void)
{
	/* pass null for the dev */
	spdk_scsi_dev_destruct(NULL);
}

static void
dev_destruct_zero_luns(void)
{
	struct spdk_scsi_dev dev = { 0 };

	/* No luns attached to the dev */

	/* free the dev */
	spdk_scsi_dev_destruct(&dev);
}

static void
dev_destruct_null_lun(void)
{
	struct spdk_scsi_dev dev = { 0 };

	/* pass null for the lun */
	dev.lun[0] = NULL;

	/* free the dev */
	spdk_scsi_dev_destruct(&dev);
}

static void
dev_destruct_success(void)
{
	struct spdk_scsi_dev dev = { 0 };
	struct spdk_scsi_lun *lun;

	lun = calloc(1, sizeof(struct spdk_scsi_lun));

	/* dev with a single lun */
	dev.lun[0] = lun;

	/* free the dev */
	spdk_scsi_dev_destruct(&dev);

}

static void
dev_construct_num_luns_zero(void)
{
	struct spdk_scsi_dev *dev;
	char *lun_name_list[1] = {};
	int lun_id_list[1] = { 0 };

	dev = spdk_scsi_dev_construct("Name", lun_name_list, lun_id_list, 0,
				      SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI, NULL, NULL);

	/* dev should be null since we passed num_luns = 0 */
	CU_ASSERT_TRUE(dev == NULL);
}

static void
dev_construct_no_lun_zero(void)
{
	struct spdk_scsi_dev *dev;
	char *lun_name_list[1] = {};
	int lun_id_list[1] = { 0 };

	lun_id_list[0] = 1;

	dev = spdk_scsi_dev_construct("Name", lun_name_list, lun_id_list, 1,
				      SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI, NULL, NULL);

	/* dev should be null since no LUN0 was specified (lun_id_list[0] = 1) */
	CU_ASSERT_TRUE(dev == NULL);
}

static void
dev_construct_null_lun(void)
{
	struct spdk_scsi_dev *dev;
	char *lun_name_list[1] = {};
	int lun_id_list[1] = { 0 };

	dev = spdk_scsi_dev_construct("Name", lun_name_list, lun_id_list, 1,
				      SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI, NULL, NULL);

	/* dev should be null since no LUN0 was specified (lun_list[0] = NULL) */
	CU_ASSERT_TRUE(dev == NULL);
}

static void
dev_construct_success(void)
{
	struct spdk_scsi_dev *dev;
	char *lun_name_list[1] = {"malloc0"};
	int lun_id_list[1] = { 0 };

	dev = spdk_scsi_dev_construct("Name", lun_name_list, lun_id_list, 1,
				      SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI, NULL, NULL);

	/* Successfully constructs and returns a dev */
	CU_ASSERT_TRUE(dev != NULL);

	/* free the dev */
	spdk_scsi_dev_destruct(dev);

	CU_ASSERT(TAILQ_EMPTY(&g_lun_head));
}

static void
dev_construct_same_lun_two_devices(void)
{
	struct spdk_scsi_dev *dev, *dev2;
	char *lun_name_list[1] = {"malloc0"};
	int lun_id_list[1] = { 0 };

	dev = spdk_scsi_dev_construct("Name", lun_name_list, lun_id_list, 1,
				      SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI, NULL, NULL);

	/* Successfully constructs and returns a dev */
	CU_ASSERT_TRUE(dev != NULL);

	dev2 = spdk_scsi_dev_construct("Name2", lun_name_list, lun_id_list, 1,
				       SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI, NULL, NULL);

	/* Fails to construct dev and returns NULL */
	CU_ASSERT_TRUE(dev2 == NULL);

	/* free the dev */
	spdk_scsi_dev_destruct(dev);

	CU_ASSERT(TAILQ_EMPTY(&g_lun_head));
}

static void
dev_construct_same_lun_one_device(void)
{
	struct spdk_scsi_dev *dev;
	char *lun_name_list[2] = {"malloc0", "malloc0"};
	int lun_id_list[2] = { 0, 1 };

	dev = spdk_scsi_dev_construct("Name", lun_name_list, lun_id_list, 2,
				      SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI, NULL, NULL);

	/* Fails to construct dev and returns NULL */
	CU_ASSERT_TRUE(dev == NULL);

	CU_ASSERT(TAILQ_EMPTY(&g_lun_head));
}

static void
dev_queue_mgmt_task_success(void)
{
	struct spdk_scsi_dev *dev;
	char *lun_name_list[1] = {"malloc0"};
	int lun_id_list[1] = { 0 };
	struct spdk_scsi_task *task;

	dev = spdk_scsi_dev_construct("Name", lun_name_list, lun_id_list, 1,
				      SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI, NULL, NULL);

	/* Successfully constructs and returns a dev */
	CU_ASSERT_TRUE(dev != NULL);

	task = spdk_get_task(NULL);

	spdk_scsi_dev_queue_mgmt_task(dev, task, SPDK_SCSI_TASK_FUNC_LUN_RESET);

	spdk_scsi_task_put(task);

	spdk_scsi_dev_destruct(dev);
}

static void
dev_queue_task_success(void)
{
	struct spdk_scsi_dev *dev;
	char *lun_name_list[1] = {"malloc0"};
	int lun_id_list[1] = { 0 };
	struct spdk_scsi_task *task;

	dev = spdk_scsi_dev_construct("Name", lun_name_list, lun_id_list, 1,
				      SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI, NULL, NULL);

	/* Successfully constructs and returns a dev */
	CU_ASSERT_TRUE(dev != NULL);

	task = spdk_get_task(NULL);

	spdk_scsi_dev_queue_task(dev, task);

	spdk_scsi_task_put(task);

	spdk_scsi_dev_destruct(dev);
}

static void
dev_stop_success(void)
{
	struct spdk_scsi_dev dev = { 0 };
	struct spdk_scsi_task *task;
	struct spdk_scsi_task *task_mgmt;

	task = spdk_get_task(NULL);

	spdk_scsi_dev_queue_task(&dev, task);

	task_mgmt = spdk_get_task(NULL);

	/* Enqueue the tasks into dev->task_mgmt_submit_queue */
	spdk_scsi_dev_queue_mgmt_task(&dev, task_mgmt, SPDK_SCSI_TASK_FUNC_LUN_RESET);

	spdk_scsi_task_put(task);
	spdk_scsi_task_put(task_mgmt);
}

static void
dev_add_port_max_ports(void)
{
	struct spdk_scsi_dev dev = { 0 };
	const char *name;
	int id, rc;

	/* dev is set to SPDK_SCSI_DEV_MAX_PORTS */
	dev.num_ports = SPDK_SCSI_DEV_MAX_PORTS;
	name = "Name of Port";
	id = 1;

	rc = spdk_scsi_dev_add_port(&dev, id, name);

	/* returns -1; since the dev already has maximum
	 * number of ports (SPDK_SCSI_DEV_MAX_PORTS) */
	CU_ASSERT_TRUE(rc < 0);
}

static void
dev_add_port_construct_failure(void)
{
	struct spdk_scsi_dev dev = { 0 };
	const int port_name_length = SPDK_SCSI_PORT_MAX_NAME_LENGTH + 2;
	char name[port_name_length];
	int id, rc;

	dev.num_ports = 1;
	/* Set the name such that the length exceeds SPDK_SCSI_PORT_MAX_NAME_LENGTH
	 * SPDK_SCSI_PORT_MAX_NAME_LENGTH = 256 */
	memset(name, 'a', port_name_length - 1);
	name[port_name_length - 1] = '\0';
	id = 1;

	rc = spdk_scsi_dev_add_port(&dev, id, name);

	/* returns -1; since the length of the name exceeds
	 * SPDK_SCSI_PORT_MAX_NAME_LENGTH */
	CU_ASSERT_TRUE(rc < 0);
}

static void
dev_add_port_success(void)
{
	struct spdk_scsi_dev dev = { 0 };
	const char *name;
	int id, rc;

	dev.num_ports = 1;
	name = "Name of Port";
	id = 1;

	rc = spdk_scsi_dev_add_port(&dev, id, name);

	/* successfully adds a port */
	CU_ASSERT_EQUAL(rc, 0);
	/* Assert num_ports has been incremented to  2 */
	CU_ASSERT_EQUAL(dev.num_ports, 2);
}

static void
dev_find_port_by_id_num_ports_zero(void)
{
	struct spdk_scsi_dev dev = { 0 };
	struct spdk_scsi_port *rp_port;
	uint64_t id;

	dev.num_ports = 0;
	id = 1;

	rp_port = spdk_scsi_dev_find_port_by_id(&dev, id);

	/* returns null; since dev's num_ports is 0 */
	CU_ASSERT_TRUE(rp_port == NULL);
}

static void
dev_find_port_by_id_id_not_found_failure(void)
{
	struct spdk_scsi_dev dev = { 0 };
	struct spdk_scsi_port *rp_port;
	const char *name;
	int rc;
	uint64_t id, find_id;

	id = 1;
	dev.num_ports = 1;
	name = "Name of Port";
	find_id = 2;

	/* Add a port with id = 1 */
	rc = spdk_scsi_dev_add_port(&dev, id, name);

	CU_ASSERT_EQUAL(rc, 0);

	/* Find port with id = 2 */
	rp_port = spdk_scsi_dev_find_port_by_id(&dev, find_id);

	/* returns null; failed to find port specified by id = 2 */
	CU_ASSERT_TRUE(rp_port == NULL);
}

static void
dev_find_port_by_id_success(void)
{
	struct spdk_scsi_dev dev = { 0 };
	struct spdk_scsi_port *rp_port;
	const char *name;
	int rc;
	uint64_t id;

	id = 1;
	dev.num_ports = 1;
	name = "Name of Port";

	/* Add a port */
	rc = spdk_scsi_dev_add_port(&dev, id, name);

	CU_ASSERT_EQUAL(rc, 0);

	/* Find port by the same id as the one added above */
	rp_port = spdk_scsi_dev_find_port_by_id(&dev, id);

	/* Successfully found port specified by id */
	CU_ASSERT_TRUE(rp_port != NULL);
	if (rp_port != NULL) {
		/* Assert the found port's id and name are same as
		 * the port added. */
		CU_ASSERT_EQUAL(rp_port->id, 1);
		CU_ASSERT_STRING_EQUAL(rp_port->name, "Name of Port");
	}
}

static void
dev_print_success(void)
{
	struct spdk_scsi_dev dev = { 0 };
	struct spdk_scsi_lun lun = { 0 };

	dev.lun[0] = &lun;

	/* Prints the dev and a list of the LUNs associated with
	 * the dev */
	spdk_scsi_dev_print(&dev);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("dev_suite", test_setup, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "destruct - null dev",
			    dev_destruct_null_dev) == NULL
		|| CU_add_test(suite, "destruct - zero luns", dev_destruct_zero_luns) == NULL
		|| CU_add_test(suite, "destruct - null lun", dev_destruct_null_lun) == NULL
		|| CU_add_test(suite, "destruct - success", dev_destruct_success) == NULL
		|| CU_add_test(suite, "construct  - queue depth gt max depth",
			       dev_construct_num_luns_zero) == NULL
		|| CU_add_test(suite, "construct  - no lun0",
			       dev_construct_no_lun_zero) == NULL
		|| CU_add_test(suite, "construct  - null lun",
			       dev_construct_null_lun) == NULL
		|| CU_add_test(suite, "construct  - success", dev_construct_success) == NULL
		|| CU_add_test(suite, "construct  - same lun on two devices",
			       dev_construct_same_lun_two_devices) == NULL
		|| CU_add_test(suite, "construct  - same lun on once device",
			       dev_construct_same_lun_one_device) == NULL
		|| CU_add_test(suite, "dev queue task mgmt - success",
			       dev_queue_mgmt_task_success) == NULL
		|| CU_add_test(suite, "dev queue task - success",
			       dev_queue_task_success) == NULL
		|| CU_add_test(suite, "dev stop - success", dev_stop_success) == NULL
		|| CU_add_test(suite, "dev add port - max ports",
			       dev_add_port_max_ports) == NULL
		|| CU_add_test(suite, "dev add port - construct port failure",
			       dev_add_port_construct_failure) == NULL
		|| CU_add_test(suite, "dev add port - success",
			       dev_add_port_success) == NULL
		|| CU_add_test(suite, "dev find port by id - num ports zero",
			       dev_find_port_by_id_num_ports_zero) == NULL
		|| CU_add_test(suite, "dev find port by id - different port id failure",
			       dev_find_port_by_id_id_not_found_failure) == NULL
		|| CU_add_test(suite, "dev find port by id - success",
			       dev_find_port_by_id_success) == NULL
		|| CU_add_test(suite, "dev print - success", dev_print_success) == NULL
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
