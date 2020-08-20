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

#include "spdk_cunit.h"

#include "spdk/env.h"

#include "common/lib/test_env.c"

#include "nvme/nvme_uevent.c"

#ifdef __linux__

enum uevent_parse_event_return_type {
	uevent_abnormal_exit = -1,
	uevent_normal_exit = 0,
	uevent_expected_continue = 1
};

static void
test_nvme_uevent_parse_event(void)
{
	char *commands;
	struct spdk_uevent uevent = {};
	int rc = uevent_normal_exit;

	/* Simulate commands to check expected behaviors */
	/* Linux kernel puts null characters after every uevent */

	/* Case 1: Add wrong non-uio or vfio-pci /devices/pci0000:80/0000:80:01.0/0000:81:00.0/uio/uio0 */
	commands =
		"ACTION=add\0DEVPATH=/devices/pci0000:80/0000:80:01.0/0000:81:00.0/uio/uio0\0SUBSYSTEM= \0DRIVER= \0PCI_SLOT_NAME= \0";
	uevent.subsystem = 0xFF;
	uevent.action = 0;

	rc = parse_event(commands, &uevent);

	CU_ASSERT(rc == uevent_expected_continue);
	CU_ASSERT(uevent.subsystem == SPDK_NVME_UEVENT_SUBSYSTEM_UNRECOGNIZED);
	CU_ASSERT(uevent.action == SPDK_NVME_UEVENT_ADD);

	/* Case 2: Add uio /devices/pci0000:80/0000:80:01.0/0000:81:00.0/uio/uio0 */
	commands =
		"ACTION=add \0DEVPATH=/devices/pci0000:80/0000:80:01.0/0000:81:00.0/uio/uio0\0SUBSYSTEM=uio\0DRIVER=\0PCI_SLOT_NAME= \0";
	uevent.subsystem = SPDK_NVME_UEVENT_SUBSYSTEM_UNRECOGNIZED;
	uevent.action = 0;

	rc = parse_event(commands, &uevent);

	CU_ASSERT(rc == uevent_expected_continue);
	CU_ASSERT(uevent.subsystem == SPDK_NVME_UEVENT_SUBSYSTEM_UIO);
	CU_ASSERT(uevent.action == SPDK_NVME_UEVENT_ADD);

	/* Case 3: Remove uio /devices/pci0000:80/0000:80:01.0/0000:81:00.0/uio/uio0 */
	commands =
		"ACTION=remove\0DEVPATH=/devices/pci0000:80/0000:80:01.0/0000:81:00.0/uio/uio0\0SUBSYSTEM=uio\0DRIVER=\0PCI_SLOT_NAME= \0";
	uevent.subsystem = SPDK_NVME_UEVENT_SUBSYSTEM_UNRECOGNIZED;

	rc = parse_event(commands, &uevent);

	CU_ASSERT(rc == uevent_expected_continue);
	CU_ASSERT(uevent.subsystem == SPDK_NVME_UEVENT_SUBSYSTEM_UIO);
	CU_ASSERT(uevent.action == SPDK_NVME_UEVENT_REMOVE);

	/* Case 4: Add vfio-pci 0000:81:00.0 */
	commands = "ACTION=bind\0DEVPATH=\0SUBSYSTEM= \0DRIVER=vfio-pci\0PCI_SLOT_NAME=0000:81:00.0\0";
	uevent.subsystem = SPDK_NVME_UEVENT_SUBSYSTEM_UNRECOGNIZED;

	rc = parse_event(commands, &uevent);

	CU_ASSERT(rc == uevent_expected_continue);
	CU_ASSERT(uevent.subsystem == SPDK_NVME_UEVENT_SUBSYSTEM_VFIO);
	CU_ASSERT(uevent.action == SPDK_NVME_UEVENT_ADD);

	/* Case 5: Remove vfio-pci 0000:81:00.0 */
	commands = "ACTION=remove\0DEVPATH= \0SUBSYSTEM= \0DRIVER=vfio-pci \0PCI_SLOT_NAME=0000:81:00.0\0";
	uevent.subsystem = SPDK_NVME_UEVENT_SUBSYSTEM_UNRECOGNIZED;

	rc = parse_event(commands, &uevent);

	CU_ASSERT(rc == uevent_expected_continue);
	CU_ASSERT(uevent.subsystem == SPDK_NVME_UEVENT_SUBSYSTEM_VFIO);
	CU_ASSERT(uevent.action == SPDK_NVME_UEVENT_REMOVE);

	/* Case 6: Add wrong vfio-pci addr 000000 */
	commands = "ACTION=bind\0DEVPATH= \0SUBSYSTEM= \0DRIVER=vfio-pci \0PCI_SLOT_NAME=000000\0";
	uevent.subsystem = SPDK_NVME_UEVENT_SUBSYSTEM_UNRECOGNIZED;

	rc = parse_event(commands, &uevent);

	CU_ASSERT(rc == uevent_abnormal_exit);
	CU_ASSERT(uevent.subsystem == SPDK_NVME_UEVENT_SUBSYSTEM_VFIO);
	CU_ASSERT(uevent.action == SPDK_NVME_UEVENT_ADD);

	/* Case 7: Add wrong type vfio 0000:81:00.0 */
	commands = "ACTION=bind\0DEVPATH= \0SUBSYSTEM= \0DRIVER=vfio \0PCI_SLOT_NAME=0000:81:00.0\0";
	uevent.subsystem = SPDK_NVME_UEVENT_SUBSYSTEM_UIO;
	uevent.action = 0;
	rc = parse_event(commands, &uevent);

	CU_ASSERT(rc == uevent_expected_continue);
	CU_ASSERT(uevent.subsystem == SPDK_NVME_UEVENT_SUBSYSTEM_UNRECOGNIZED);
	CU_ASSERT(uevent.action == SPDK_NVME_UEVENT_ADD);
}

#else

static void
test_nvme_uevent_parse_event(void)
{
	CU_ASSERT(1);
}

#endif

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_uevent", NULL, NULL);

	CU_ADD_TEST(suite, test_nvme_uevent_parse_event);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
