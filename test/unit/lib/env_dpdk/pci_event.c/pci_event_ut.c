/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk_cunit.h"

#include "spdk/env.h"

#include "common/lib/test_env.c"

#include "env_dpdk/pci_event.c"

#ifdef __linux__

enum pci_parse_event_return_type {
	uevent_normal_exit = 0,
	uevent_expected_continue = 1
};

static void
test_pci_parse_event(void)
{
	char *commands;
	struct spdk_pci_event event = {};
	struct spdk_pci_addr addr = {};
	int rc = uevent_normal_exit;

	/* Simulate commands to check expected behaviors */
	/* Linux kernel puts null characters after every uevent */
	spdk_pci_addr_parse(&addr, "0000:81:00.0");

	/* Case 1: Add wrong non-uio or vfio-pci /devices/pci0000:80/0000:80:01.0/0000:81:00.0/uio/uio0 */
	commands =
		"ACTION=add\0DEVPATH=/devices/pci0000:80/0000:80:01.0/0000:81:00.0/uio/uio0\0SUBSYSTEM= \0DRIVER= \0PCI_SLOT_NAME= \0";

	rc = parse_subsystem_event(commands, &event);
	CU_ASSERT(rc == uevent_normal_exit);

	/* Case 2: Add pci event /devices/pci0000:80/0000:80:01.0/0000:81:00.0/uio/uio0 */
	commands =
		"ACTION=bind\0DEVPATH=/devices/pci0000:80/0000:80:01.0/0000:81:00.0\0SUBSYSTEM=pci\0DRIVER=uio_pci_generic\0PCI_SLOT_NAME=0000:81:00.0\0";

	memset(&event, 0, sizeof(event));
	rc = parse_subsystem_event(commands, &event);
	CU_ASSERT(rc == uevent_normal_exit);

	/* Case 3: Add wrong uio addr 000000 */
	commands =
		"ACTION=add \0DEVPATH=/devices/pci0000:80/0000/0000/uio/uio0\0SUBSYSTEM=uio\0DRIVER=\0PCI_SLOT_NAME= \0";

	memset(&event, 0, sizeof(event));
	rc = parse_subsystem_event(commands, &event);
	CU_ASSERT(rc < 0);

	/* Case 3: Add uio /devices/pci0000:80/0000:80:01.0/0000:81:00.0/uio/uio0 */
	commands =
		"ACTION=add \0DEVPATH=/devices/pci0000:80/0000:80:01.0/0000:81:00.0/uio/uio0\0SUBSYSTEM=uio\0DRIVER=\0PCI_SLOT_NAME= \0";

	memset(&event, 0, sizeof(event));
	rc = parse_subsystem_event(commands, &event);
	CU_ASSERT(rc == uevent_expected_continue);
	CU_ASSERT(event.action == SPDK_UEVENT_ADD);
	CU_ASSERT(spdk_pci_addr_compare(&addr, &event.traddr) == 0);

	/* Case 4: Remove uio /devices/pci0000:80/0000:80:01.0/0000:81:00.0/uio/uio0 */
	commands =
		"ACTION=remove\0DEVPATH=/devices/pci0000:80/0000:80:01.0/0000:81:00.0/uio/uio0\0SUBSYSTEM=uio\0DRIVER=\0PCI_SLOT_NAME= \0";

	memset(&event, 0, sizeof(event));
	rc = parse_subsystem_event(commands, &event);
	CU_ASSERT(rc == uevent_expected_continue);
	CU_ASSERT(event.action == SPDK_UEVENT_REMOVE);
	CU_ASSERT(spdk_pci_addr_compare(&addr, &event.traddr) == 0);

	/* Case 5: Add vfio-pci 0000:81:00.0 */
	commands = "ACTION=bind\0DEVPATH=\0SUBSYSTEM= \0DRIVER=vfio-pci\0PCI_SLOT_NAME=0000:81:00.0\0";

	memset(&event, 0, sizeof(event));
	rc = parse_subsystem_event(commands, &event);
	CU_ASSERT(rc == uevent_expected_continue);
	CU_ASSERT(event.action == SPDK_UEVENT_ADD);
	CU_ASSERT(spdk_pci_addr_compare(&addr, &event.traddr) == 0);
	memset(&event, 0, sizeof(event));

	/* Case 6: Remove vfio-pci 0000:81:00.0 but We don't parse vfio-pci remove uevent */
	commands = "ACTION=remove\0DEVPATH= \0SUBSYSTEM= \0DRIVER=vfio-pci \0PCI_SLOT_NAME=0000:81:00.0\0";

	memset(&event, 0, sizeof(event));
	rc = parse_subsystem_event(commands, &event);
	CU_ASSERT(rc == uevent_normal_exit);

	/* Case 7: Add wrong vfio-pci addr 000000 */
	commands = "ACTION=bind\0DEVPATH= \0SUBSYSTEM= \0DRIVER=vfio-pci \0PCI_SLOT_NAME=000000\0";

	memset(&event, 0, sizeof(event));
	rc = parse_subsystem_event(commands, &event);
	CU_ASSERT(rc < 0);

	/* Case 8: Add wrong driver vfio 0000:81:00.0 */
	commands = "ACTION=bind\0DEVPATH= \0SUBSYSTEM= \0DRIVER=vfio \0PCI_SLOT_NAME=0000:81:00.0\0";

	memset(&event, 0, sizeof(event));
	rc = parse_subsystem_event(commands, &event);
	CU_ASSERT(rc == uevent_normal_exit);
}

#else

static void
test_pci_parse_event(void)
{
	CU_ASSERT(1);
}

#endif

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("pci_event", NULL, NULL);

	CU_ADD_TEST(suite, test_pci_parse_event);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
