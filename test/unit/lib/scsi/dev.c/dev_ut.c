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

#include "spdk/util.h"

#include "scsi/dev.c"
#include "scsi/port.c"

#include "spdk_internal/mock.h"

DEFINE_STUB(spdk_scsi_lun_is_removing, bool,
	    (const struct spdk_scsi_lun *lun), false);

static char *g_bdev_names[] = {
	"malloc0",
	"malloc1",
};

static struct spdk_scsi_port *g_initiator_port_with_pending_tasks = NULL;
static struct spdk_scsi_port *g_initiator_port_with_pending_mgmt_tasks = NULL;

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

struct spdk_scsi_lun *scsi_lun_construct(const char *bdev_name,
		void (*resize_cb)(const struct spdk_scsi_lun *, void *),
		void *resize_ctx,
		void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
		void *hotremove_ctx)
{
	struct spdk_scsi_lun *lun;
	size_t i;

	for (i = 0; i < SPDK_COUNTOF(g_bdev_names); i++) {
		if (strcmp(bdev_name, g_bdev_names[i]) == 0) {
			lun = calloc(1, sizeof(struct spdk_scsi_lun));
			SPDK_CU_ASSERT_FATAL(lun != NULL);

			return lun;
		}
	}

	return NULL;
}

void
scsi_lun_destruct(struct spdk_scsi_lun *lun)
{
	free(lun);
}

DEFINE_STUB_V(scsi_lun_execute_mgmt_task,
	      (struct spdk_scsi_lun *lun, struct spdk_scsi_task *task));

DEFINE_STUB_V(scsi_lun_execute_task,
	      (struct spdk_scsi_lun *lun, struct spdk_scsi_task *task));

DEFINE_STUB(scsi_lun_allocate_io_channel, int,
	    (struct spdk_scsi_lun *lun), 0);

DEFINE_STUB_V(scsi_lun_free_io_channel, (struct spdk_scsi_lun *lun));

bool
scsi_lun_has_pending_mgmt_tasks(const struct spdk_scsi_lun *lun,
				const struct spdk_scsi_port *initiator_port)
{
	return (g_initiator_port_with_pending_mgmt_tasks == initiator_port);
}

bool
scsi_lun_has_pending_tasks(const struct spdk_scsi_lun *lun,
			   const struct spdk_scsi_port *initiator_port)
{
	return (g_initiator_port_with_pending_tasks == initiator_port);
}

static void
dev_destruct_null_dev(void)
{
	/* pass null for the dev */
	spdk_scsi_dev_destruct(NULL, NULL, NULL);
}

static void
dev_destruct_zero_luns(void)
{
	struct spdk_scsi_dev dev = { .is_allocated = 1 };

	/* No luns attached to the dev */

	/* free the dev */
	spdk_scsi_dev_destruct(&dev, NULL, NULL);
}

static void
dev_destruct_null_lun(void)
{
	struct spdk_scsi_dev dev = { .is_allocated = 1 };

	/* pass null for the lun */
	dev.lun[0] = NULL;

	/* free the dev */
	spdk_scsi_dev_destruct(&dev, NULL, NULL);
}

static void
dev_destruct_success(void)
{
	struct spdk_scsi_dev dev = { .is_allocated = 1 };
	int rc;

	/* dev with a single lun */
	rc = spdk_scsi_dev_add_lun(&dev, "malloc0", 0, NULL, NULL);

	CU_ASSERT(rc == 0);

	/* free the dev */
	spdk_scsi_dev_destruct(&dev, NULL, NULL);

}

static void
dev_construct_num_luns_zero(void)
{
	struct spdk_scsi_dev *dev;
	const char *bdev_name_list[1] = {};
	int lun_id_list[1] = { 0 };

	dev = spdk_scsi_dev_construct("Name", bdev_name_list, lun_id_list, 0,
				      SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI, NULL, NULL);

	/* dev should be null since we passed num_luns = 0 */
	CU_ASSERT_TRUE(dev == NULL);
}

static void
dev_construct_no_lun_zero(void)
{
	struct spdk_scsi_dev *dev;
	const char *bdev_name_list[1] = {};
	int lun_id_list[1] = { 0 };

	lun_id_list[0] = 1;

	dev = spdk_scsi_dev_construct("Name", bdev_name_list, lun_id_list, 1,
				      SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI, NULL, NULL);

	/* dev should be null since no LUN0 was specified (lun_id_list[0] = 1) */
	CU_ASSERT_TRUE(dev == NULL);
}

static void
dev_construct_null_lun(void)
{
	struct spdk_scsi_dev *dev;
	const char *bdev_name_list[1] = {};
	int lun_id_list[1] = { 0 };

	dev = spdk_scsi_dev_construct("Name", bdev_name_list, lun_id_list, 1,
				      SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI, NULL, NULL);

	/* dev should be null since no LUN0 was specified (lun_list[0] = NULL) */
	CU_ASSERT_TRUE(dev == NULL);
}

static void
dev_construct_name_too_long(void)
{
	struct spdk_scsi_dev *dev;
	const char *bdev_name_list[1] = {"malloc0"};
	int lun_id_list[1] = { 0 };
	char name[SPDK_SCSI_DEV_MAX_NAME + 1 + 1];

	/* Try to construct a dev with a name that is one byte longer than allowed. */
	memset(name, 'x', sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';

	dev = spdk_scsi_dev_construct(name, bdev_name_list, lun_id_list, 1,
				      SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI, NULL, NULL);

	CU_ASSERT(dev == NULL);
}

static void
dev_construct_success(void)
{
	struct spdk_scsi_dev *dev;
	const char *bdev_name_list[1] = {"malloc0"};
	int lun_id_list[1] = { 0 };

	dev = spdk_scsi_dev_construct("Name", bdev_name_list, lun_id_list, 1,
				      SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI, NULL, NULL);

	/* Successfully constructs and returns a dev */
	CU_ASSERT_TRUE(dev != NULL);

	/* free the dev */
	spdk_scsi_dev_destruct(dev, NULL, NULL);
}

static void
dev_construct_success_lun_zero_not_first(void)
{
	struct spdk_scsi_dev *dev;
	const char *bdev_name_list[2] = {"malloc1", "malloc0"};
	int lun_id_list[2] = { 1, 0 };

	dev = spdk_scsi_dev_construct("Name", bdev_name_list, lun_id_list, 2,
				      SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI, NULL, NULL);

	/* Successfully constructs and returns a dev */
	CU_ASSERT_TRUE(dev != NULL);

	/* free the dev */
	spdk_scsi_dev_destruct(dev, NULL, NULL);
}

static void
dev_queue_mgmt_task_success(void)
{
	struct spdk_scsi_dev *dev;
	const char *bdev_name_list[1] = {"malloc0"};
	int lun_id_list[1] = { 0 };
	struct spdk_scsi_task *task;

	dev = spdk_scsi_dev_construct("Name", bdev_name_list, lun_id_list, 1,
				      SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI, NULL, NULL);

	/* Successfully constructs and returns a dev */
	CU_ASSERT_TRUE(dev != NULL);

	task = spdk_get_task(NULL);

	task->function = SPDK_SCSI_TASK_FUNC_LUN_RESET;
	spdk_scsi_dev_queue_mgmt_task(dev, task);

	spdk_scsi_task_put(task);

	spdk_scsi_dev_destruct(dev, NULL, NULL);
}

static void
dev_queue_task_success(void)
{
	struct spdk_scsi_dev *dev;
	const char *bdev_name_list[1] = {"malloc0"};
	int lun_id_list[1] = { 0 };
	struct spdk_scsi_task *task;

	dev = spdk_scsi_dev_construct("Name", bdev_name_list, lun_id_list, 1,
				      SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI, NULL, NULL);

	/* Successfully constructs and returns a dev */
	CU_ASSERT_TRUE(dev != NULL);

	task = spdk_get_task(NULL);

	spdk_scsi_dev_queue_task(dev, task);

	spdk_scsi_task_put(task);

	spdk_scsi_dev_destruct(dev, NULL, NULL);
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
	task->function = SPDK_SCSI_TASK_FUNC_LUN_RESET;
	spdk_scsi_dev_queue_mgmt_task(&dev, task_mgmt);

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
dev_add_port_construct_failure1(void)
{
	struct spdk_scsi_dev dev = { 0 };
	const int port_name_length = SPDK_SCSI_PORT_MAX_NAME_LENGTH + 2;
	char name[port_name_length];
	uint64_t id;
	int rc;

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
dev_add_port_construct_failure2(void)
{
	struct spdk_scsi_dev dev = { 0 };
	const char *name;
	uint64_t id;
	int rc;

	dev.num_ports = 1;
	name = "Name of Port";
	id = 1;

	/* Initialize port[0] to be valid and its index is set to 1 */
	dev.port[0].id = id;
	dev.port[0].is_used = 1;

	rc = spdk_scsi_dev_add_port(&dev, id, name);

	/* returns -1; since the dev already has a port whose index to be 1 */
	CU_ASSERT_TRUE(rc < 0);
}

static void
dev_add_port_success1(void)
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
dev_add_port_success2(void)
{
	struct spdk_scsi_dev dev = { 0 };
	const char *name;
	uint64_t id;
	int rc;

	dev.num_ports = 1;
	name = "Name of Port";
	id = 1;
	/* set id of invalid port[0] to 1. This must be ignored */
	dev.port[0].id = id;
	dev.port[0].is_used = 0;

	rc = spdk_scsi_dev_add_port(&dev, id, name);

	/* successfully adds a port */
	CU_ASSERT_EQUAL(rc, 0);
	/* Assert num_ports has been incremented to 1 */
	CU_ASSERT_EQUAL(dev.num_ports, 2);
}

static void
dev_add_port_success3(void)
{
	struct spdk_scsi_dev dev = { 0 };
	const char *name;
	uint64_t add_id;
	int rc;

	dev.num_ports = 1;
	name = "Name of Port";
	dev.port[0].id = 1;
	dev.port[0].is_used = 1;
	add_id = 2;

	/* Add a port with id = 2 */
	rc = spdk_scsi_dev_add_port(&dev, add_id, name);

	/* successfully adds a port */
	CU_ASSERT_EQUAL(rc, 0);
	/* Assert num_ports has been incremented to 2 */
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
dev_add_lun_bdev_not_found(void)
{
	int rc;
	struct spdk_scsi_dev dev = {0};

	rc = spdk_scsi_dev_add_lun(&dev, "malloc2", 0, NULL, NULL);

	SPDK_CU_ASSERT_FATAL(dev.lun[0] == NULL);
	CU_ASSERT_NOT_EQUAL(rc, 0);
}

static void
dev_add_lun_no_free_lun_id(void)
{
	int rc;
	int i;
	struct spdk_scsi_dev dev = {0};
	struct spdk_scsi_lun lun;

	for (i = 0; i < SPDK_SCSI_DEV_MAX_LUN; i++) {
		dev.lun[i] = &lun;
	}

	rc = spdk_scsi_dev_add_lun(&dev, "malloc0", -1, NULL, NULL);

	CU_ASSERT_NOT_EQUAL(rc, 0);
}

static void
dev_add_lun_success1(void)
{
	int rc;
	struct spdk_scsi_dev dev = {0};

	rc = spdk_scsi_dev_add_lun(&dev, "malloc0", -1, NULL, NULL);

	CU_ASSERT_EQUAL(rc, 0);

	spdk_scsi_dev_destruct(&dev, NULL, NULL);
}

static void
dev_add_lun_success2(void)
{
	int rc;
	struct spdk_scsi_dev dev = {0};

	rc = spdk_scsi_dev_add_lun(&dev, "malloc0", 0, NULL, NULL);

	CU_ASSERT_EQUAL(rc, 0);

	spdk_scsi_dev_destruct(&dev, NULL, NULL);
}

static void
dev_check_pending_tasks(void)
{
	struct spdk_scsi_dev dev = {};
	struct spdk_scsi_lun lun = {};
	struct spdk_scsi_port initiator_port = {};

	g_initiator_port_with_pending_tasks = NULL;
	g_initiator_port_with_pending_mgmt_tasks = NULL;

	CU_ASSERT(spdk_scsi_dev_has_pending_tasks(&dev, NULL) == false);

	dev.lun[SPDK_SCSI_DEV_MAX_LUN - 1] = &lun;

	CU_ASSERT(spdk_scsi_dev_has_pending_tasks(&dev, NULL) == true);
	CU_ASSERT(spdk_scsi_dev_has_pending_tasks(&dev, &initiator_port) == false);

	g_initiator_port_with_pending_tasks = &initiator_port;
	CU_ASSERT(spdk_scsi_dev_has_pending_tasks(&dev, NULL) == true);
	CU_ASSERT(spdk_scsi_dev_has_pending_tasks(&dev, &initiator_port) == true);

	g_initiator_port_with_pending_tasks = NULL;
	g_initiator_port_with_pending_mgmt_tasks = &initiator_port;
	CU_ASSERT(spdk_scsi_dev_has_pending_tasks(&dev, NULL) == true);
	CU_ASSERT(spdk_scsi_dev_has_pending_tasks(&dev, &initiator_port) == true);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("dev_suite", NULL, NULL);

	CU_ADD_TEST(suite, dev_destruct_null_dev);
	CU_ADD_TEST(suite, dev_destruct_zero_luns);
	CU_ADD_TEST(suite, dev_destruct_null_lun);
	CU_ADD_TEST(suite, dev_destruct_success);
	CU_ADD_TEST(suite, dev_construct_num_luns_zero);
	CU_ADD_TEST(suite, dev_construct_no_lun_zero);
	CU_ADD_TEST(suite, dev_construct_null_lun);
	CU_ADD_TEST(suite, dev_construct_name_too_long);
	CU_ADD_TEST(suite, dev_construct_success);
	CU_ADD_TEST(suite, dev_construct_success_lun_zero_not_first);
	CU_ADD_TEST(suite, dev_queue_mgmt_task_success);
	CU_ADD_TEST(suite, dev_queue_task_success);
	CU_ADD_TEST(suite, dev_stop_success);
	CU_ADD_TEST(suite, dev_add_port_max_ports);
	CU_ADD_TEST(suite, dev_add_port_construct_failure1);
	CU_ADD_TEST(suite, dev_add_port_construct_failure2);
	CU_ADD_TEST(suite, dev_add_port_success1);
	CU_ADD_TEST(suite, dev_add_port_success2);
	CU_ADD_TEST(suite, dev_add_port_success3);
	CU_ADD_TEST(suite, dev_find_port_by_id_num_ports_zero);
	CU_ADD_TEST(suite, dev_find_port_by_id_id_not_found_failure);
	CU_ADD_TEST(suite, dev_find_port_by_id_success);
	CU_ADD_TEST(suite, dev_add_lun_bdev_not_found);
	CU_ADD_TEST(suite, dev_add_lun_no_free_lun_id);
	CU_ADD_TEST(suite, dev_add_lun_success1);
	CU_ADD_TEST(suite, dev_add_lun_success2);
	CU_ADD_TEST(suite, dev_check_pending_tasks);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
