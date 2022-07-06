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
#include "spdk_internal/mock.h"

#include "env_dpdk/pci.c"

static void
pci_claim_test(struct spdk_pci_device *dev)
{
	int rc = 0;
	pid_t childPid;
	int status, ret;

	rc = spdk_pci_device_claim(dev);
	CU_ASSERT(rc >= 0);

	childPid = fork();
	CU_ASSERT(childPid >= 0);
	if (childPid == 0) {
		ret = spdk_pci_device_claim(dev);
		CU_ASSERT(ret == -1);
		exit(0);
	} else {
		waitpid(childPid, &status, 0);
	}
}

static struct spdk_pci_driver ut_pci_driver;

struct ut_pci_dev {
	struct spdk_pci_device pci;
	char config[16];
	char bar[16];
	bool attached;
};

static int
ut_map_bar(struct spdk_pci_device *dev, uint32_t bar,
	   void **mapped_addr, uint64_t *phys_addr, uint64_t *size)
{
	struct ut_pci_dev *ut_dev = (struct ut_pci_dev *)dev;

	/* just one bar */
	if (bar > 0) {
		return -1;
	}

	*mapped_addr = ut_dev->bar;
	*phys_addr = 0;
	*size = sizeof(ut_dev->bar);
	return 0;
}

static int
ut_unmap_bar(struct spdk_pci_device *device, uint32_t bar, void *addr)
{
	return 0;
}

static int
ut_cfg_read(struct spdk_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	struct ut_pci_dev *ut_dev = (struct ut_pci_dev *)dev;

	if (len + offset >= sizeof(ut_dev->config)) {
		return -1;
	}

	memcpy(value, (void *)((uintptr_t)ut_dev->config + offset), len);
	return 0;
}

static int ut_cfg_write(struct spdk_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	struct ut_pci_dev *ut_dev = (struct ut_pci_dev *)dev;

	if (len + offset >= sizeof(ut_dev->config)) {
		return -1;
	}

	memcpy((void *)((uintptr_t)ut_dev->config + offset), value, len);
	return 0;
}


static int
ut_enum_cb(void *ctx, struct spdk_pci_device *dev)
{
	struct ut_pci_dev *ut_dev = (struct ut_pci_dev *)dev;

	ut_dev->attached = true;
	return 0;
}

static int
ut_attach_cb(const struct spdk_pci_addr *addr)
{
	return -ENODEV;
}

static void
ut_detach_cb(struct spdk_pci_device *dev)
{
}

static struct spdk_pci_device_provider g_ut_provider = {
	.name = "custom",
	.attach_cb = ut_attach_cb,
	.detach_cb = ut_detach_cb,
};

SPDK_PCI_REGISTER_DEVICE_PROVIDER(ut, &g_ut_provider);

static void
pci_hook_test(void)
{
	struct ut_pci_dev ut_dev = {};
	uint32_t value_32;
	void *bar0_vaddr;
	uint64_t bar0_paddr, bar0_size;
	int rc;

	ut_dev.pci.type = "custom";
	ut_dev.pci.id.vendor_id = 0x4;
	ut_dev.pci.id.device_id = 0x8;

	/* Use add parse for initialization */
	spdk_pci_addr_parse(&ut_dev.pci.addr, "10000:00:01.0");
	CU_ASSERT(ut_dev.pci.addr.domain == 0x10000);
	CU_ASSERT(ut_dev.pci.addr.bus == 0x0);
	CU_ASSERT(ut_dev.pci.addr.dev == 0x1);
	CU_ASSERT(ut_dev.pci.addr.func == 0x0);

	ut_dev.pci.map_bar = ut_map_bar;
	ut_dev.pci.unmap_bar = ut_unmap_bar;
	ut_dev.pci.cfg_read = ut_cfg_read;
	ut_dev.pci.cfg_write = ut_cfg_write;

	/* hook the device into the PCI layer */
	spdk_pci_hook_device(&ut_pci_driver, &ut_dev.pci);

	/* try to attach a device with the matching driver and bdf */
	rc = spdk_pci_device_attach(&ut_pci_driver, ut_enum_cb, NULL, &ut_dev.pci.addr);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ut_dev.pci.internal.attached);
	CU_ASSERT(ut_dev.attached);

	/* check PCI config writes and reads */
	value_32 = 0xDEADBEEF;
	rc = spdk_pci_device_cfg_write32(&ut_dev.pci, value_32, 0);
	CU_ASSERT(rc == 0);

	value_32 = 0x0BADF00D;
	rc = spdk_pci_device_cfg_write32(&ut_dev.pci, value_32, 4);
	CU_ASSERT(rc == 0);

	rc = spdk_pci_device_cfg_read32(&ut_dev.pci, &value_32, 0);
	CU_ASSERT(rc == 0);
	CU_ASSERT(value_32 == 0xDEADBEEF);
	CU_ASSERT(memcmp(&value_32, &ut_dev.config[0], 4) == 0);

	rc = spdk_pci_device_cfg_read32(&ut_dev.pci, &value_32, 4);
	CU_ASSERT(rc == 0);
	CU_ASSERT(value_32 == 0x0BADF00D);
	CU_ASSERT(memcmp(&value_32, &ut_dev.config[4], 4) == 0);

	/* out-of-bounds write */
	rc = spdk_pci_device_cfg_read32(&ut_dev.pci, &value_32, sizeof(ut_dev.config));
	CU_ASSERT(rc != 0);

	/* map a bar */
	rc = spdk_pci_device_map_bar(&ut_dev.pci, 0, &bar0_vaddr, &bar0_paddr, &bar0_size);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bar0_vaddr == ut_dev.bar);
	CU_ASSERT(bar0_size == sizeof(ut_dev.bar));
	spdk_pci_device_unmap_bar(&ut_dev.pci, 0, bar0_vaddr);

	/* map an inaccessible bar */
	rc = spdk_pci_device_map_bar(&ut_dev.pci, 1, &bar0_vaddr, &bar0_paddr, &bar0_size);
	CU_ASSERT(rc != 0);

	/* test spdk_pci_device_claim() */
	pci_claim_test(&ut_dev.pci);

	spdk_pci_device_detach(&ut_dev.pci);
	CU_ASSERT(!ut_dev.pci.internal.attached);

	/* unhook the device */
	spdk_pci_unhook_device(&ut_dev.pci);

	/* try to attach the same device again */
	rc = spdk_pci_device_attach(&ut_pci_driver, ut_enum_cb, NULL, &ut_dev.pci.addr);
	CU_ASSERT(rc != 0);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("pci", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "pci_hook", pci_hook_test) == NULL
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
