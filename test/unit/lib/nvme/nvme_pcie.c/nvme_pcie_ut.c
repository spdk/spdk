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

#include "spdk_cunit.h"
#include "common/lib/test_env.c"

#include "nvme/nvme_pcie.c"

struct spdk_log_flag SPDK_LOG_NVME = {
	.name = "nvme",
	.enabled = false,
};

static struct nvme_driver _g_nvme_driver = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
};
struct nvme_driver *g_spdk_nvme_driver = &_g_nvme_driver;

struct spdk_nvme_ctrlr *g_ctrlr = NULL;

bool g_add_device = false;

bool g_device_is_enumerated = false;

bool g_device_is_removed = false;

bool g_remove_cb_done = false;

bool g_vfio_is_enabled = false;

char *g_pcie_traddr_str = "";

void
nvme_ctrlr_fail(struct spdk_nvme_ctrlr *ctrlr, bool hot_remove)
{
	CU_ASSERT(ctrlr != NULL);
	if (hot_remove) {
		ctrlr->is_removed = true;
	}

	ctrlr->is_failed = true;
}

int
spdk_get_uevent(int fd, struct spdk_uevent *uevent)
{
	snprintf(uevent->traddr, sizeof(uevent->traddr), "%s", g_pcie_traddr_str);

	if (g_vfio_is_enabled) {
		uevent->subsystem = SPDK_NVME_UEVENT_SUBSYSTEM_VFIO;
	} else {
		uevent->subsystem = SPDK_NVME_UEVENT_SUBSYSTEM_UIO;
	}

	if (g_add_device) {
		uevent->action = SPDK_NVME_UEVENT_ADD;
	} else {
		uevent->action = SPDK_NVME_UEVENT_REMOVE;
	}

	if (!g_remove_cb_done && !g_device_is_enumerated) {
		return 1;
	}

	return 0;
}

int
spdk_pci_enumerate(struct spdk_pci_driver *driver, spdk_pci_enum_cb enum_cb, void *enum_ctx)
{
	g_device_is_enumerated = true;

	return 0;
}

struct spdk_pci_addr
spdk_pci_device_get_addr(struct spdk_pci_device *dev)
{
	abort();
}

int
nvme_ctrlr_add_process(struct spdk_nvme_ctrlr *ctrlr, void *devhandle)
{
	abort();
}

int
nvme_ctrlr_probe(const struct spdk_nvme_transport_id *trid,
		 struct spdk_nvme_probe_ctx *probe_ctx, void *devhandle)
{
	abort();
}

struct spdk_nvme_ctrlr_process *
spdk_nvme_ctrlr_get_current_process(struct spdk_nvme_ctrlr *ctrlr)
{
	return (struct spdk_nvme_ctrlr_process *)0x1;
}

bool
spdk_pci_device_is_removed(struct spdk_pci_device *dev)
{
	return g_device_is_removed;
}

static void
ut_remove_cb(void *cb_ctx, struct spdk_nvme_ctrlr *ctrlr)
{
	g_remove_cb_done = true;
	if (cb_ctx == NULL) {
		return;
	}

	CU_ASSERT(ctrlr != NULL);
	CU_ASSERT(g_ctrlr == NULL || g_ctrlr == ctrlr);
	free(g_ctrlr);
	g_ctrlr = NULL;
}

struct spdk_nvme_ctrlr *
spdk_nvme_get_ctrlr_by_trid_unsafe(const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvme_ctrlr *ctrlr;

	ctrlr = calloc(1, sizeof(*ctrlr));
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	ctrlr->remove_cb = (void *)ut_remove_cb;
	ctrlr->is_failed = false;
	ctrlr->is_removed = false;
	g_ctrlr = ctrlr;

	return ctrlr;
}

union spdk_nvme_csts_register spdk_nvme_ctrlr_get_regs_csts(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_csts_register csts = {};

	if (g_device_is_removed) {
		csts.raw = 0xFFFFFFFFu;
	}

	return csts;
}

static void
prp_list_prep(struct nvme_tracker *tr, struct nvme_request *req, uint32_t *prp_index)
{
	memset(req, 0, sizeof(*req));
	memset(tr, 0, sizeof(*tr));
	tr->req = req;
	tr->prp_sgl_bus_addr = 0xDEADBEEF;
	*prp_index = 0;
}

static void
test_prp_list_append(void)
{
	struct nvme_request req;
	struct nvme_tracker tr;
	uint32_t prp_index;

	/* Non-DWORD-aligned buffer (invalid) */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100001, 0x1000, 0x1000) == -EFAULT);

	/* 512-byte buffer, 4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100000, 0x200, 0x1000) == 0);
	CU_ASSERT(prp_index == 1);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100000);

	/* 512-byte buffer, non-4K-aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x108000, 0x200, 0x1000) == 0);
	CU_ASSERT(prp_index == 1);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x108000);

	/* 4K buffer, 4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100000, 0x1000, 0x1000) == 0);
	CU_ASSERT(prp_index == 1);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100000);

	/* 4K buffer, non-4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100800, 0x1000, 0x1000) == 0);
	CU_ASSERT(prp_index == 2);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100800);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == 0x101000);

	/* 8K buffer, 4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100000, 0x2000, 0x1000) == 0);
	CU_ASSERT(prp_index == 2);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100000);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == 0x101000);

	/* 8K buffer, non-4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100800, 0x2000, 0x1000) == 0);
	CU_ASSERT(prp_index == 3);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100800);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == tr.prp_sgl_bus_addr);
	CU_ASSERT(tr.u.prp[0] == 0x101000);
	CU_ASSERT(tr.u.prp[1] == 0x102000);

	/* 12K buffer, 4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100000, 0x3000, 0x1000) == 0);
	CU_ASSERT(prp_index == 3);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100000);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == tr.prp_sgl_bus_addr);
	CU_ASSERT(tr.u.prp[0] == 0x101000);
	CU_ASSERT(tr.u.prp[1] == 0x102000);

	/* 12K buffer, non-4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100800, 0x3000, 0x1000) == 0);
	CU_ASSERT(prp_index == 4);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100800);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == tr.prp_sgl_bus_addr);
	CU_ASSERT(tr.u.prp[0] == 0x101000);
	CU_ASSERT(tr.u.prp[1] == 0x102000);
	CU_ASSERT(tr.u.prp[2] == 0x103000);

	/* Two 4K buffers, both 4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100000, 0x1000, 0x1000) == 0);
	CU_ASSERT(prp_index == 1);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x900000, 0x1000, 0x1000) == 0);
	CU_ASSERT(prp_index == 2);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100000);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == 0x900000);

	/* Two 4K buffers, first non-4K aligned, second 4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100800, 0x1000, 0x1000) == 0);
	CU_ASSERT(prp_index == 2);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x900000, 0x1000, 0x1000) == 0);
	CU_ASSERT(prp_index == 3);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100800);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == tr.prp_sgl_bus_addr);
	CU_ASSERT(tr.u.prp[0] == 0x101000);
	CU_ASSERT(tr.u.prp[1] == 0x900000);

	/* Two 4K buffers, both non-4K aligned (invalid) */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100800, 0x1000, 0x1000) == 0);
	CU_ASSERT(prp_index == 2);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x900800, 0x1000, 0x1000) == -EFAULT);
	CU_ASSERT(prp_index == 2);

	/* 4K buffer, 4K aligned, but vtophys fails */
	MOCK_SET(spdk_vtophys, SPDK_VTOPHYS_ERROR);
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100000, 0x1000, 0x1000) == -EFAULT);
	MOCK_CLEAR(spdk_vtophys);

	/* Largest aligned buffer that can be described in NVME_MAX_PRP_LIST_ENTRIES (plus PRP1) */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100000,
					    (NVME_MAX_PRP_LIST_ENTRIES + 1) * 0x1000, 0x1000) == 0);
	CU_ASSERT(prp_index == NVME_MAX_PRP_LIST_ENTRIES + 1);

	/* Largest non-4K-aligned buffer that can be described in NVME_MAX_PRP_LIST_ENTRIES (plus PRP1) */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100800,
					    NVME_MAX_PRP_LIST_ENTRIES * 0x1000, 0x1000) == 0);
	CU_ASSERT(prp_index == NVME_MAX_PRP_LIST_ENTRIES + 1);

	/* Buffer too large to be described in NVME_MAX_PRP_LIST_ENTRIES */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100000,
					    (NVME_MAX_PRP_LIST_ENTRIES + 2) * 0x1000, 0x1000) == -EFAULT);

	/* Non-4K-aligned buffer too large to be described in NVME_MAX_PRP_LIST_ENTRIES */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100800,
					    (NVME_MAX_PRP_LIST_ENTRIES + 1) * 0x1000, 0x1000) == -EFAULT);
}

static void
test_nvme_pcie_hotplug_monitor(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_driver dummy;
	pthread_mutexattr_t attr;
	struct spdk_nvme_probe_ctx test_nvme_probe_ctx = {};

	/* Initiate variables and ctrlr */
	g_device_is_removed = false;
	dummy.initialized = true;
	CU_ASSERT(pthread_mutexattr_init(&attr) == 0);
	CU_ASSERT(pthread_mutex_init(&dummy.lock, &attr) == 0);
	TAILQ_INIT(&dummy.shared_attached_ctrlrs);
	g_spdk_nvme_driver = &dummy;
	test_nvme_probe_ctx.cb_ctx = (void *)ut_remove_cb;

	/* Case 1:  SPDK_NVME_UEVENT_ADD/ NVME_VFIO */
	g_add_device = true;
	g_vfio_is_enabled = true;
	g_pcie_traddr_str = "0000:05:00.0";
	CU_ASSERT(g_device_is_enumerated == false);

	_nvme_pcie_hotplug_monitor(&test_nvme_probe_ctx);

	CU_ASSERT(g_device_is_enumerated == true);
	g_device_is_enumerated = false;

	/* Case 2:  SPDK_NVME_UEVENT_ADD/ NVME_UIO */
	g_vfio_is_enabled = false;

	_nvme_pcie_hotplug_monitor(&test_nvme_probe_ctx);

	CU_ASSERT(g_device_is_enumerated == true);
	g_device_is_enumerated = false;

	/* Case 3: SPDK_NVME_UEVENT_REMOVE/ NVME_UIO */
	g_add_device = false;
	g_pcie_traddr_str = "0000:04:00.0";
	test_nvme_probe_ctx.cb_ctx = NULL;

	_nvme_pcie_hotplug_monitor(&test_nvme_probe_ctx);

	CU_ASSERT(g_remove_cb_done == true);
	CU_ASSERT(g_ctrlr != NULL);
	free(g_ctrlr);
	g_ctrlr = NULL;
	g_remove_cb_done = false;

	/* Case 4: SPDK_NVME_UEVENT_REMOVE/ NVME_VFIO */
	g_vfio_is_enabled = true;
	test_nvme_probe_ctx.cb_ctx = (void *)ut_remove_cb;

	_nvme_pcie_hotplug_monitor(&test_nvme_probe_ctx);

	CU_ASSERT(g_remove_cb_done == true);
	CU_ASSERT(g_ctrlr == NULL);

	/* Case 5:  test only for VFIO hot remove detection  */
	g_remove_cb_done  = true;
	ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	snprintf(ctrlr.trid.traddr, sizeof(ctrlr.trid.traddr), "0000:02:00.0");
	ctrlr.remove_cb = NULL;
	ctrlr.is_failed = false;
	ctrlr.is_removed = false;
	TAILQ_INSERT_TAIL(&g_spdk_nvme_driver->shared_attached_ctrlrs, &ctrlr, tailq);

	_nvme_pcie_hotplug_monitor(&test_nvme_probe_ctx);

	CU_ASSERT(ctrlr.is_failed == false);

	g_device_is_removed = true;
	g_remove_cb_done = false;
	ctrlr.remove_cb = (void *)ut_remove_cb;

	_nvme_pcie_hotplug_monitor(&test_nvme_probe_ctx);

	CU_ASSERT(ctrlr.is_failed == true);
	CU_ASSERT(ctrlr.is_removed == true);
	CU_ASSERT(g_remove_cb_done == true);

	pthread_mutex_destroy(&dummy.lock);
	pthread_mutexattr_destroy(&attr);
	g_spdk_nvme_driver = &_g_nvme_driver;
}

static void test_shadow_doorbell_update(void)
{
	bool ret;

	/* nvme_pcie_qpair_need_event(uint16_t event_idx, uint16_t new_idx, uint16_t old) */
	ret = nvme_pcie_qpair_need_event(10, 15, 14);
	CU_ASSERT(ret == false);

	ret = nvme_pcie_qpair_need_event(14, 15, 14);
	CU_ASSERT(ret == true);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvme_pcie", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (CU_add_test(suite, "prp_list_append", test_prp_list_append) == NULL
	    || CU_add_test(suite, "nvme_pcie_hotplug_monitor", test_nvme_pcie_hotplug_monitor) == NULL
	    || CU_add_test(suite, "shadow_doorbell_update",
			   test_shadow_doorbell_update) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
