/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 Mellanox Technologies LTD. All rights reserved.
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

#define UNIT_TEST_NO_VTOPHYS

#include "nvme/nvme_pcie.c"
#include "nvme/nvme_pcie_common.c"
#include "common/lib/nvme/common_stubs.h"

pid_t g_spdk_nvme_pid;
DEFINE_STUB(spdk_mem_register, int, (void *vaddr, size_t len), 0);
DEFINE_STUB(spdk_mem_unregister, int, (void *vaddr, size_t len), 0);

DEFINE_STUB(nvme_get_quirks, uint64_t, (const struct spdk_pci_id *id), 0);

DEFINE_STUB(nvme_wait_for_completion, int,
	    (struct spdk_nvme_qpair *qpair,
	     struct nvme_completion_poll_status *status), 0);
DEFINE_STUB_V(nvme_completion_poll_cb, (void *arg, const struct spdk_nvme_cpl *cpl));

DEFINE_STUB(nvme_ctrlr_submit_admin_request, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct nvme_request *req), 0);
DEFINE_STUB_V(nvme_ctrlr_free_processes, (struct spdk_nvme_ctrlr *ctrlr));
DEFINE_STUB(nvme_ctrlr_proc_get_devhandle, struct spdk_pci_device *,
	    (struct spdk_nvme_ctrlr *ctrlr), NULL);
DEFINE_STUB(spdk_pci_device_unmap_bar, int, (struct spdk_pci_device *dev, uint32_t bar, void *addr),
	    0);
DEFINE_STUB(spdk_pci_device_attach, int, (struct spdk_pci_driver *driver, spdk_pci_enum_cb enum_cb,
		void *enum_ctx, struct spdk_pci_addr *pci_address), 0);
DEFINE_STUB(spdk_pci_device_claim, int, (struct spdk_pci_device *dev), 0);
DEFINE_STUB_V(spdk_pci_device_unclaim, (struct spdk_pci_device *dev));
DEFINE_STUB_V(spdk_pci_device_detach, (struct spdk_pci_device *device));
DEFINE_STUB(spdk_pci_device_cfg_write16, int, (struct spdk_pci_device *dev, uint16_t value,
		uint32_t offset), 0);
DEFINE_STUB(spdk_pci_device_cfg_read16, int, (struct spdk_pci_device *dev, uint16_t *value,
		uint32_t offset), 0);
DEFINE_STUB(spdk_pci_device_get_id, struct spdk_pci_id, (struct spdk_pci_device *dev), {0});
DEFINE_STUB(spdk_pci_event_listen, int, (void), 0);
DEFINE_STUB(spdk_pci_register_error_handler, int, (spdk_pci_error_handler sighandler, void *ctx),
	    0);
DEFINE_STUB_V(spdk_pci_unregister_error_handler, (spdk_pci_error_handler sighandler));
DEFINE_STUB(spdk_pci_enumerate, int,
	    (struct spdk_pci_driver *driver, spdk_pci_enum_cb enum_cb, void *enum_ctx),
	    -1);

DEFINE_STUB(nvme_transport_get_name, const char *, (const struct spdk_nvme_transport *transport),
	    NULL);

SPDK_LOG_REGISTER_COMPONENT(nvme)

struct dev_mem_resource {
	uint64_t phys_addr;
	uint64_t len;
	void *addr;
};

struct nvme_pcie_ut_bdev_io {
	struct iovec iovs[NVME_MAX_SGL_DESCRIPTORS];
	int iovpos;
};

struct nvme_driver *g_spdk_nvme_driver = NULL;

int
spdk_pci_device_map_bar(struct spdk_pci_device *dev, uint32_t bar,
			void **mapped_addr, uint64_t *phys_addr, uint64_t *size)
{
	struct dev_mem_resource *dev_mem_res = (void *)dev;

	*mapped_addr = dev_mem_res->addr;
	*phys_addr = dev_mem_res->phys_addr;
	*size = dev_mem_res->len;

	return 0;
}

void
nvme_ctrlr_fail(struct spdk_nvme_ctrlr *ctrlr, bool hot_remove)
{
	CU_ASSERT(ctrlr != NULL);
	if (hot_remove) {
		ctrlr->is_removed = true;
	}

	ctrlr->is_failed = true;
}

static uint64_t g_vtophys_size = 0;

DEFINE_RETURN_MOCK(spdk_vtophys, uint64_t);
uint64_t
spdk_vtophys(const void *buf, uint64_t *size)
{
	if (size) {
		*size = g_vtophys_size;
	}

	HANDLE_RETURN_MOCK(spdk_vtophys);

	return (uintptr_t)buf;
}

DEFINE_STUB(spdk_pci_device_get_addr, struct spdk_pci_addr, (struct spdk_pci_device *dev), {});
DEFINE_STUB(nvme_ctrlr_probe, int, (const struct spdk_nvme_transport_id *trid,
				    struct spdk_nvme_probe_ctx *probe_ctx, void *devhandle), 0);
DEFINE_STUB(spdk_pci_device_is_removed, bool, (struct spdk_pci_device *dev), false);
DEFINE_STUB(nvme_get_ctrlr_by_trid_unsafe, struct spdk_nvme_ctrlr *,
	    (const struct spdk_nvme_transport_id *trid), NULL);
DEFINE_STUB(spdk_nvme_ctrlr_get_regs_csts, union spdk_nvme_csts_register,
	    (struct spdk_nvme_ctrlr *ctrlr), {});
DEFINE_STUB(nvme_ctrlr_get_process, struct spdk_nvme_ctrlr_process *,
	    (struct spdk_nvme_ctrlr *ctrlr, pid_t pid), NULL);
DEFINE_STUB(nvme_completion_is_retry, bool, (const struct spdk_nvme_cpl *cpl), false);
DEFINE_STUB_V(nvme_ctrlr_process_async_event, (struct spdk_nvme_ctrlr *ctrlr,
		const struct spdk_nvme_cpl *cpl));
DEFINE_STUB_V(spdk_nvme_qpair_print_command, (struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_cmd *cmd));
DEFINE_STUB_V(spdk_nvme_qpair_print_completion, (struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_cpl *cpl));

static void
prp_list_prep(struct nvme_tracker *tr, struct nvme_request *req, uint32_t *prp_index)
{
	memset(req, 0, sizeof(*req));
	memset(tr, 0, sizeof(*tr));
	tr->req = req;
	tr->prp_sgl_bus_addr = 0xDEADBEEF;
	if (prp_index) {
		*prp_index = 0;
	}
}

static void
test_prp_list_append(void)
{
	struct nvme_request req;
	struct nvme_tracker tr;
	struct spdk_nvme_ctrlr ctrlr = {};
	uint32_t prp_index;

	ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	/* Non-DWORD-aligned buffer (invalid) */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&ctrlr, &tr, &prp_index, (void *)0x100001, 0x1000,
					    0x1000) == -EFAULT);

	/* 512-byte buffer, 4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&ctrlr, &tr, &prp_index, (void *)0x100000, 0x200, 0x1000) == 0);
	CU_ASSERT(prp_index == 1);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100000);

	/* 512-byte buffer, non-4K-aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&ctrlr, &tr, &prp_index, (void *)0x108000, 0x200, 0x1000) == 0);
	CU_ASSERT(prp_index == 1);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x108000);

	/* 4K buffer, 4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&ctrlr, &tr, &prp_index, (void *)0x100000, 0x1000,
					    0x1000) == 0);
	CU_ASSERT(prp_index == 1);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100000);

	/* 4K buffer, non-4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&ctrlr, &tr, &prp_index, (void *)0x100800, 0x1000,
					    0x1000) == 0);
	CU_ASSERT(prp_index == 2);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100800);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == 0x101000);

	/* 8K buffer, 4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&ctrlr, &tr, &prp_index, (void *)0x100000, 0x2000,
					    0x1000) == 0);
	CU_ASSERT(prp_index == 2);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100000);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == 0x101000);

	/* 8K buffer, non-4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&ctrlr, &tr, &prp_index, (void *)0x100800, 0x2000,
					    0x1000) == 0);
	CU_ASSERT(prp_index == 3);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100800);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == tr.prp_sgl_bus_addr);
	CU_ASSERT(tr.u.prp[0] == 0x101000);
	CU_ASSERT(tr.u.prp[1] == 0x102000);

	/* 12K buffer, 4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&ctrlr, &tr, &prp_index, (void *)0x100000, 0x3000,
					    0x1000) == 0);
	CU_ASSERT(prp_index == 3);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100000);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == tr.prp_sgl_bus_addr);
	CU_ASSERT(tr.u.prp[0] == 0x101000);
	CU_ASSERT(tr.u.prp[1] == 0x102000);

	/* 12K buffer, non-4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&ctrlr, &tr, &prp_index, (void *)0x100800, 0x3000,
					    0x1000) == 0);
	CU_ASSERT(prp_index == 4);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100800);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == tr.prp_sgl_bus_addr);
	CU_ASSERT(tr.u.prp[0] == 0x101000);
	CU_ASSERT(tr.u.prp[1] == 0x102000);
	CU_ASSERT(tr.u.prp[2] == 0x103000);

	/* Two 4K buffers, both 4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&ctrlr, &tr, &prp_index, (void *)0x100000, 0x1000,
					    0x1000) == 0);
	CU_ASSERT(prp_index == 1);
	CU_ASSERT(nvme_pcie_prp_list_append(&ctrlr, &tr, &prp_index, (void *)0x900000, 0x1000,
					    0x1000) == 0);
	CU_ASSERT(prp_index == 2);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100000);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == 0x900000);

	/* Two 4K buffers, first non-4K aligned, second 4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&ctrlr, &tr, &prp_index, (void *)0x100800, 0x1000,
					    0x1000) == 0);
	CU_ASSERT(prp_index == 2);
	CU_ASSERT(nvme_pcie_prp_list_append(&ctrlr, &tr, &prp_index, (void *)0x900000, 0x1000,
					    0x1000) == 0);
	CU_ASSERT(prp_index == 3);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100800);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == tr.prp_sgl_bus_addr);
	CU_ASSERT(tr.u.prp[0] == 0x101000);
	CU_ASSERT(tr.u.prp[1] == 0x900000);

	/* Two 4K buffers, both non-4K aligned (invalid) */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&ctrlr, &tr, &prp_index, (void *)0x100800, 0x1000,
					    0x1000) == 0);
	CU_ASSERT(prp_index == 2);
	CU_ASSERT(nvme_pcie_prp_list_append(&ctrlr, &tr, &prp_index, (void *)0x900800, 0x1000,
					    0x1000) == -EFAULT);
	CU_ASSERT(prp_index == 2);

	/* 4K buffer, 4K aligned, but vtophys fails */
	MOCK_SET(spdk_vtophys, SPDK_VTOPHYS_ERROR);
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&ctrlr, &tr, &prp_index, (void *)0x100000, 0x1000,
					    0x1000) == -EFAULT);
	MOCK_CLEAR(spdk_vtophys);

	/* Largest aligned buffer that can be described in NVME_MAX_PRP_LIST_ENTRIES (plus PRP1) */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&ctrlr, &tr, &prp_index, (void *)0x100000,
					    (NVME_MAX_PRP_LIST_ENTRIES + 1) * 0x1000, 0x1000) == 0);
	CU_ASSERT(prp_index == NVME_MAX_PRP_LIST_ENTRIES + 1);

	/* Largest non-4K-aligned buffer that can be described in NVME_MAX_PRP_LIST_ENTRIES (plus PRP1) */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&ctrlr, &tr, &prp_index, (void *)0x100800,
					    NVME_MAX_PRP_LIST_ENTRIES * 0x1000, 0x1000) == 0);
	CU_ASSERT(prp_index == NVME_MAX_PRP_LIST_ENTRIES + 1);

	/* Buffer too large to be described in NVME_MAX_PRP_LIST_ENTRIES */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&ctrlr, &tr, &prp_index, (void *)0x100000,
					    (NVME_MAX_PRP_LIST_ENTRIES + 2) * 0x1000, 0x1000) == -EFAULT);

	/* Non-4K-aligned buffer too large to be described in NVME_MAX_PRP_LIST_ENTRIES */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&ctrlr, &tr, &prp_index, (void *)0x100800,
					    (NVME_MAX_PRP_LIST_ENTRIES + 1) * 0x1000, 0x1000) == -EFAULT);
}

struct spdk_event_entry {
	struct spdk_pci_event		event;
	STAILQ_ENTRY(spdk_event_entry)	link;
};

static STAILQ_HEAD(, spdk_event_entry) g_events = STAILQ_HEAD_INITIALIZER(g_events);
static bool g_device_allowed = false;

int
spdk_pci_get_event(int fd, struct spdk_pci_event *event)
{
	struct spdk_event_entry *entry;

	if (STAILQ_EMPTY(&g_events)) {
		return 0;
	}

	entry = STAILQ_FIRST(&g_events);
	STAILQ_REMOVE_HEAD(&g_events, link);

	*event = entry->event;

	return 1;
}

int
spdk_pci_device_allow(struct spdk_pci_addr *pci_addr)
{
	g_device_allowed = true;

	return 0;
}

static void
test_nvme_pcie_hotplug_monitor(void)
{
	struct nvme_pcie_ctrlr pctrlr = {};
	struct spdk_event_entry entry = {};
	struct nvme_driver driver;
	pthread_mutexattr_t attr;
	struct spdk_nvme_probe_ctx test_nvme_probe_ctx = {};

	/* Initiate variables and ctrlr */
	driver.initialized = true;
	driver.hotplug_fd = 123;
	CU_ASSERT(pthread_mutexattr_init(&attr) == 0);
	CU_ASSERT(pthread_mutex_init(&pctrlr.ctrlr.ctrlr_lock, &attr) == 0);
	CU_ASSERT(pthread_mutex_init(&driver.lock, &attr) == 0);
	TAILQ_INIT(&driver.shared_attached_ctrlrs);
	g_spdk_nvme_driver = &driver;

	/* Case 1:  SPDK_NVME_UEVENT_ADD/ NVME_VFIO / NVME_UIO */
	entry.event.action = SPDK_UEVENT_ADD;
	spdk_pci_addr_parse(&entry.event.traddr, "0000:05:00.0");
	CU_ASSERT(STAILQ_EMPTY(&g_events));
	STAILQ_INSERT_TAIL(&g_events, &entry, link);

	_nvme_pcie_hotplug_monitor(&test_nvme_probe_ctx);

	CU_ASSERT(STAILQ_EMPTY(&g_events));
	CU_ASSERT(g_device_allowed == true);
	g_device_allowed = false;

	/* Case 2: SPDK_NVME_UEVENT_REMOVE/ NVME_UIO */
	entry.event.action = SPDK_UEVENT_REMOVE;
	spdk_pci_addr_parse(&entry.event.traddr, "0000:05:00.0");
	CU_ASSERT(STAILQ_EMPTY(&g_events));
	STAILQ_INSERT_TAIL(&g_events, &entry, link);

	MOCK_SET(nvme_get_ctrlr_by_trid_unsafe, &pctrlr.ctrlr);

	_nvme_pcie_hotplug_monitor(&test_nvme_probe_ctx);

	CU_ASSERT(STAILQ_EMPTY(&g_events));
	CU_ASSERT(pctrlr.ctrlr.is_failed == true);
	CU_ASSERT(pctrlr.ctrlr.is_removed == true);
	pctrlr.ctrlr.is_failed = false;
	pctrlr.ctrlr.is_removed = false;
	MOCK_CLEAR(nvme_get_ctrlr_by_trid_unsafe);

	/* Case 3: SPDK_NVME_UEVENT_REMOVE/ NVME_VFIO without event */
	pctrlr.ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	snprintf(pctrlr.ctrlr.trid.traddr, sizeof(pctrlr.ctrlr.trid.traddr), "0000:02:00.0");
	pctrlr.ctrlr.remove_cb = NULL;
	pctrlr.ctrlr.is_failed = false;
	pctrlr.ctrlr.is_removed = false;
	TAILQ_INSERT_TAIL(&g_spdk_nvme_driver->shared_attached_ctrlrs, &pctrlr.ctrlr, tailq);

	/* This should be set in the vfio req notifier cb */
	MOCK_SET(spdk_pci_device_is_removed, true);

	_nvme_pcie_hotplug_monitor(&test_nvme_probe_ctx);

	CU_ASSERT(STAILQ_EMPTY(&g_events));
	CU_ASSERT(pctrlr.ctrlr.is_failed == true);
	CU_ASSERT(pctrlr.ctrlr.is_removed == true);
	pctrlr.ctrlr.is_failed = false;
	pctrlr.ctrlr.is_removed = false;
	MOCK_CLEAR(spdk_pci_device_is_removed);

	/* Case 4:  Removed device detected in another process  */
	MOCK_SET(spdk_pci_device_is_removed, false);

	_nvme_pcie_hotplug_monitor(&test_nvme_probe_ctx);

	CU_ASSERT(pctrlr.ctrlr.is_failed == false);

	MOCK_SET(spdk_pci_device_is_removed, true);

	_nvme_pcie_hotplug_monitor(&test_nvme_probe_ctx);

	CU_ASSERT(pctrlr.ctrlr.is_failed == true);

	pthread_mutex_destroy(&driver.lock);
	pthread_mutex_destroy(&pctrlr.ctrlr.ctrlr_lock);
	pthread_mutexattr_destroy(&attr);
	g_spdk_nvme_driver = NULL;
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

static void
test_build_contig_hw_sgl_request(void)
{
	struct spdk_nvme_qpair qpair = {};
	struct nvme_request req = {};
	struct nvme_tracker tr = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	int rc;

	ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	qpair.ctrlr = &ctrlr;
	/* Test 1: Payload covered by a single mapping */
	req.payload_size = 100;
	req.payload = NVME_PAYLOAD_CONTIG(0, 0);
	g_vtophys_size = 100;
	MOCK_SET(spdk_vtophys, 0xDEADBEEF);

	rc = nvme_pcie_qpair_build_contig_hw_sgl_request(&qpair, &req, &tr, 0);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(req.cmd.dptr.sgl1.address == 0xDEADBEEF);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.length == 100);

	MOCK_CLEAR(spdk_vtophys);
	g_vtophys_size = 0;
	memset(&qpair, 0, sizeof(qpair));
	memset(&req, 0, sizeof(req));
	memset(&tr, 0, sizeof(tr));

	/* Test 2: Payload covered by a single mapping, but request is at an offset */
	qpair.ctrlr = &ctrlr;
	req.payload_size = 100;
	req.payload_offset = 50;
	req.payload = NVME_PAYLOAD_CONTIG(0, 0);
	g_vtophys_size = 1000;
	MOCK_SET(spdk_vtophys, 0xDEADBEEF);

	rc = nvme_pcie_qpair_build_contig_hw_sgl_request(&qpair, &req, &tr, 0);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(req.cmd.dptr.sgl1.address == 0xDEADBEEF);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.length == 100);

	MOCK_CLEAR(spdk_vtophys);
	g_vtophys_size = 0;
	memset(&qpair, 0, sizeof(qpair));
	memset(&req, 0, sizeof(req));
	memset(&tr, 0, sizeof(tr));

	/* Test 3: Payload spans two mappings */
	qpair.ctrlr = &ctrlr;
	req.payload_size = 100;
	req.payload = NVME_PAYLOAD_CONTIG(0, 0);
	g_vtophys_size = 60;
	tr.prp_sgl_bus_addr = 0xFF0FF;
	MOCK_SET(spdk_vtophys, 0xDEADBEEF);

	rc = nvme_pcie_qpair_build_contig_hw_sgl_request(&qpair, &req, &tr, 0);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.type == SPDK_NVME_SGL_TYPE_LAST_SEGMENT);
	CU_ASSERT(req.cmd.dptr.sgl1.address == tr.prp_sgl_bus_addr);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.length == 2 * sizeof(struct spdk_nvme_sgl_descriptor));
	CU_ASSERT(tr.u.sgl[0].unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(tr.u.sgl[0].unkeyed.length == 60);
	CU_ASSERT(tr.u.sgl[0].address == 0xDEADBEEF);
	CU_ASSERT(tr.u.sgl[1].unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(tr.u.sgl[1].unkeyed.length == 40);
	CU_ASSERT(tr.u.sgl[1].address == 0xDEADBEEF);

	MOCK_CLEAR(spdk_vtophys);
	g_vtophys_size = 0;
	memset(&qpair, 0, sizeof(qpair));
	memset(&req, 0, sizeof(req));
	memset(&tr, 0, sizeof(tr));
}

static void
test_nvme_pcie_qpair_build_metadata(void)
{
	struct spdk_nvme_qpair qpair = {};
	struct nvme_tracker tr = {};
	struct nvme_request req = {};
	struct spdk_nvme_ctrlr	ctrlr = {};
	int rc;

	ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	tr.req = &req;
	qpair.ctrlr = &ctrlr;

	req.payload.md = (void *)0xDEADBEE0;
	req.md_offset = 0;
	req.md_size = 4096;
	req.cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	tr.prp_sgl_bus_addr = 0xDBADBEEF;
	MOCK_SET(spdk_vtophys, 0xDCADBEE0);

	rc = nvme_pcie_qpair_build_metadata(&qpair, &tr, true, true);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_SGL);
	CU_ASSERT(tr.meta_sgl.address == 0xDCADBEE0);
	CU_ASSERT(tr.meta_sgl.unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(tr.meta_sgl.unkeyed.length == 4096);
	CU_ASSERT(tr.meta_sgl.unkeyed.subtype == 0);
	CU_ASSERT(req.cmd.mptr == (0xDBADBEEF - sizeof(struct spdk_nvme_sgl_descriptor)));
	MOCK_CLEAR(spdk_vtophys);

	/* Build non sgl metadata */
	MOCK_SET(spdk_vtophys, 0xDDADBEE0);

	rc = nvme_pcie_qpair_build_metadata(&qpair, &tr, false, true);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.mptr == 0xDDADBEE0);
	MOCK_CLEAR(spdk_vtophys);
}

static int
nvme_pcie_ut_next_sge(void *cb_arg, void **address, uint32_t *length)
{
	struct nvme_pcie_ut_bdev_io *bio = cb_arg;
	struct iovec *iov;

	SPDK_CU_ASSERT_FATAL(bio->iovpos < NVME_MAX_SGL_DESCRIPTORS);

	iov = &bio->iovs[bio->iovpos];

	*address = iov->iov_base;
	*length = iov->iov_len;
	bio->iovpos++;

	return 0;
}

static void
nvme_pcie_ut_reset_sgl(void *cb_arg, uint32_t offset)
{
	struct nvme_pcie_ut_bdev_io *bio = cb_arg;
	struct iovec *iov;

	for (bio->iovpos = 0; bio->iovpos < NVME_MAX_SGL_DESCRIPTORS; bio->iovpos++) {
		iov = &bio->iovs[bio->iovpos];
		/* Offset must be aligned with the start of any SGL entry */
		if (offset == 0) {
			break;
		}

		SPDK_CU_ASSERT_FATAL(offset >= iov->iov_len);
		offset -= iov->iov_len;
	}

	SPDK_CU_ASSERT_FATAL(offset == 0);
	SPDK_CU_ASSERT_FATAL(bio->iovpos < NVME_MAX_SGL_DESCRIPTORS);
}

static void
test_nvme_pcie_qpair_build_prps_sgl_request(void)
{
	struct spdk_nvme_qpair qpair = {};
	struct nvme_request req = {};
	struct nvme_tracker tr = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_pcie_ut_bdev_io bio = {};
	int rc;

	tr.req = &req;
	qpair.ctrlr = &ctrlr;
	req.payload.contig_or_cb_arg = &bio;

	req.payload.reset_sgl_fn = nvme_pcie_ut_reset_sgl;
	req.payload.next_sge_fn = nvme_pcie_ut_next_sge;
	req.payload_size = 4096;
	ctrlr.page_size = 4096;
	bio.iovs[0].iov_base = (void *)0x100000;
	bio.iovs[0].iov_len = 4096;

	rc = nvme_pcie_qpair_build_prps_sgl_request(&qpair, &req, &tr, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100000);
}

static void
test_nvme_pcie_qpair_build_hw_sgl_request(void)
{
	struct spdk_nvme_qpair qpair = {};
	struct nvme_request req = {};
	struct nvme_tracker tr = {};
	struct nvme_pcie_ut_bdev_io bio = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	int rc;

	ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	qpair.ctrlr = &ctrlr;
	req.payload.contig_or_cb_arg = &bio;
	req.payload.reset_sgl_fn = nvme_pcie_ut_reset_sgl;
	req.payload.next_sge_fn = nvme_pcie_ut_next_sge;
	req.cmd.opc = SPDK_NVME_OPC_WRITE;
	tr.prp_sgl_bus_addr =  0xDAADBEE0;
	g_vtophys_size = 4096;

	/* Multiple vectors, 2k + 4k + 2k */
	req.payload_size = 8192;
	bio.iovpos = 3;
	bio.iovs[0].iov_base = (void *)0xDBADBEE0;
	bio.iovs[0].iov_len = 2048;
	bio.iovs[1].iov_base = (void *)0xDCADBEE0;
	bio.iovs[1].iov_len = 4096;
	bio.iovs[2].iov_base = (void *)0xDDADBEE0;
	bio.iovs[2].iov_len = 2048;

	rc = nvme_pcie_qpair_build_hw_sgl_request(&qpair, &req, &tr, true);
	CU_ASSERT(rc == 0);
	CU_ASSERT(tr.u.sgl[0].unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(tr.u.sgl[0].unkeyed.length == 2048);
	CU_ASSERT(tr.u.sgl[0].address == 0xDBADBEE0);
	CU_ASSERT(tr.u.sgl[0].unkeyed.subtype == 0);
	CU_ASSERT(tr.u.sgl[1].unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(tr.u.sgl[1].unkeyed.length == 4096);
	CU_ASSERT(tr.u.sgl[1].address == 0xDCADBEE0);
	CU_ASSERT(tr.u.sgl[2].unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(tr.u.sgl[2].unkeyed.length == 2048);
	CU_ASSERT(tr.u.sgl[2].unkeyed.length == 2048);
	CU_ASSERT(tr.u.sgl[2].address == 0xDDADBEE0);
	CU_ASSERT(req.cmd.psdt == SPDK_NVME_PSDT_SGL_MPTR_CONTIG);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.subtype == 0);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.type == SPDK_NVME_SGL_TYPE_LAST_SEGMENT);
	CU_ASSERT(req.cmd.dptr.sgl1.address == 0xDAADBEE0);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.length == 48);

	/* Single vector */
	memset(&tr, 0, sizeof(tr));
	memset(&bio, 0, sizeof(bio));
	memset(&req, 0, sizeof(req));
	req.payload.contig_or_cb_arg = &bio;
	req.payload.reset_sgl_fn = nvme_pcie_ut_reset_sgl;
	req.payload.next_sge_fn = nvme_pcie_ut_next_sge;
	req.cmd.opc = SPDK_NVME_OPC_WRITE;
	req.payload_size = 4096;
	bio.iovpos = 1;
	bio.iovs[0].iov_base = (void *)0xDBADBEE0;
	bio.iovs[0].iov_len = 4096;

	rc = nvme_pcie_qpair_build_hw_sgl_request(&qpair, &req, &tr, true);
	CU_ASSERT(rc == 0);
	CU_ASSERT(tr.u.sgl[0].unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(tr.u.sgl[0].unkeyed.length == 4096);
	CU_ASSERT(tr.u.sgl[0].address == 0xDBADBEE0);
	CU_ASSERT(tr.u.sgl[0].unkeyed.subtype == 0);
	CU_ASSERT(req.cmd.psdt == SPDK_NVME_PSDT_SGL_MPTR_CONTIG);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.subtype == 0);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(req.cmd.dptr.sgl1.address == 0xDBADBEE0);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.length == 4096);
}

static void
test_nvme_pcie_qpair_build_contig_request(void)
{
	struct nvme_pcie_qpair pqpair = {};
	struct nvme_request req = {};
	struct nvme_tracker tr = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	int rc;

	pqpair.qpair.ctrlr = &ctrlr;
	ctrlr.page_size = 0x1000;

	/* 1 prp, 4k-aligned */
	prp_list_prep(&tr, &req, NULL);
	req.payload_size = 0x1000;
	req.payload.contig_or_cb_arg = (void *)0x100000;

	rc = nvme_pcie_qpair_build_contig_request(&pqpair.qpair, &req, &tr, true);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100000);

	/* 2 prps, non-4K-aligned */
	prp_list_prep(&tr, &req, NULL);
	req.payload_size = 0x1000;
	req.payload_offset = 0x800;
	req.payload.contig_or_cb_arg = (void *)0x100000;

	rc = nvme_pcie_qpair_build_contig_request(&pqpair.qpair, &req, &tr, true);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100800);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == 0x101000);

	/* 3 prps, 4k-aligned */
	prp_list_prep(&tr, &req, NULL);
	req.payload_size = 0x3000;
	req.payload.contig_or_cb_arg = (void *)0x100000;

	rc = nvme_pcie_qpair_build_contig_request(&pqpair.qpair, &req, &tr, true);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100000);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == tr.prp_sgl_bus_addr);
	CU_ASSERT(tr.u.prp[0] == 0x101000);
	CU_ASSERT(tr.u.prp[1] == 0x102000);

	/* address not dword aligned */
	prp_list_prep(&tr, &req, NULL);
	req.payload_size = 0x3000;
	req.payload.contig_or_cb_arg = (void *)0x100001;
	req.qpair = &pqpair.qpair;
	TAILQ_INIT(&pqpair.outstanding_tr);
	TAILQ_INSERT_TAIL(&pqpair.outstanding_tr, &tr, tq_list);

	rc = nvme_pcie_qpair_build_contig_request(&pqpair.qpair, &req, &tr, true);
	CU_ASSERT(rc == -EFAULT);
}

static void
test_nvme_pcie_ctrlr_regs_get_set(void)
{
	struct nvme_pcie_ctrlr pctrlr = {};
	volatile struct spdk_nvme_registers regs = {};
	uint32_t value_4;
	uint64_t value_8;
	int rc;

	pctrlr.regs = &regs;

	rc = nvme_pcie_ctrlr_set_reg_4(&pctrlr.ctrlr, 8, 4);
	CU_ASSERT(rc == 0);

	rc = nvme_pcie_ctrlr_get_reg_4(&pctrlr.ctrlr, 8, &value_4);
	CU_ASSERT(rc == 0);
	CU_ASSERT(value_4 == 4);

	rc = nvme_pcie_ctrlr_set_reg_8(&pctrlr.ctrlr, 0, 0x100000000);
	CU_ASSERT(rc == 0);

	rc = nvme_pcie_ctrlr_get_reg_8(&pctrlr.ctrlr, 0, &value_8);
	CU_ASSERT(rc == 0);
	CU_ASSERT(value_8 == 0x100000000);
}

static void
test_nvme_pcie_ctrlr_map_unmap_cmb(void)
{
	struct nvme_pcie_ctrlr pctrlr = {};
	volatile struct spdk_nvme_registers regs = {};
	union spdk_nvme_cmbsz_register cmbsz = {};
	union spdk_nvme_cmbloc_register cmbloc = {};
	struct dev_mem_resource cmd_res = {};
	int rc;

	pctrlr.regs = &regs;
	pctrlr.devhandle = (void *)&cmd_res;
	cmd_res.addr = (void *)0x7f7c0080d000;
	cmd_res.len = 0x800000;
	cmd_res.phys_addr = 0xFC800000;
	/* Configure cmb size with unit size 4k, offset 100, unsupported SQ */
	cmbsz.bits.sz = 512;
	cmbsz.bits.szu = 0;
	cmbsz.bits.sqs = 0;
	cmbloc.bits.bir = 0;
	cmbloc.bits.ofst = 100;

	nvme_pcie_ctrlr_set_reg_4(&pctrlr.ctrlr, offsetof(struct spdk_nvme_registers, cmbsz.raw),
				  cmbsz.raw);
	nvme_pcie_ctrlr_set_reg_4(&pctrlr.ctrlr, offsetof(struct spdk_nvme_registers, cmbloc.raw),
				  cmbloc.raw);

	nvme_pcie_ctrlr_map_cmb(&pctrlr);
	CU_ASSERT(pctrlr.cmb.bar_va == (void *)0x7f7c0080d000);
	CU_ASSERT(pctrlr.cmb.bar_pa == 0xFC800000);
	CU_ASSERT(pctrlr.cmb.size == 512 * 4096);
	CU_ASSERT(pctrlr.cmb.current_offset == 4096 * 100);
	CU_ASSERT(pctrlr.ctrlr.opts.use_cmb_sqs == false);

	rc = nvme_pcie_ctrlr_unmap_cmb(&pctrlr);
	CU_ASSERT(rc == 0);

	/* Invalid mapping information */
	memset(&pctrlr.cmb, 0, sizeof(pctrlr.cmb));
	nvme_pcie_ctrlr_set_reg_4(&pctrlr.ctrlr, offsetof(struct spdk_nvme_registers, cmbsz.raw), 0);
	nvme_pcie_ctrlr_set_reg_4(&pctrlr.ctrlr, offsetof(struct spdk_nvme_registers, cmbloc.raw), 0);

	nvme_pcie_ctrlr_map_cmb(&pctrlr);
	CU_ASSERT(pctrlr.cmb.bar_va == NULL);
	CU_ASSERT(pctrlr.cmb.bar_pa == 0);
	CU_ASSERT(pctrlr.cmb.size == 0);
	CU_ASSERT(pctrlr.cmb.current_offset == 0);
	CU_ASSERT(pctrlr.ctrlr.opts.use_cmb_sqs == false);
}


static void
prepare_map_io_cmd(struct nvme_pcie_ctrlr *pctrlr)
{
	union spdk_nvme_cmbsz_register cmbsz = {};
	union spdk_nvme_cmbloc_register cmbloc = {};

	cmbsz.bits.sz = 512;
	cmbsz.bits.wds = 1;
	cmbsz.bits.rds = 1;

	nvme_pcie_ctrlr_set_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, cmbsz.raw),
				  cmbsz.raw);
	nvme_pcie_ctrlr_set_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, cmbloc.raw),
				  cmbloc.raw);

	pctrlr->cmb.bar_va = (void *)0x7F7C0080D000;
	pctrlr->cmb.bar_pa = 0xFC800000;
	pctrlr->cmb.current_offset = 1ULL << 22;
	pctrlr->cmb.size = (1ULL << 22) * 512;
	pctrlr->cmb.mem_register_addr = NULL;
	pctrlr->ctrlr.opts.use_cmb_sqs = false;
}

static void
test_nvme_pcie_ctrlr_map_io_cmb(void)
{
	struct nvme_pcie_ctrlr pctrlr = {};
	volatile struct spdk_nvme_registers regs = {};
	union spdk_nvme_cmbsz_register cmbsz = {};
	void *mem_reg_addr = NULL;
	size_t size;
	int rc;

	pctrlr.regs = &regs;
	prepare_map_io_cmd(&pctrlr);

	mem_reg_addr = nvme_pcie_ctrlr_map_io_cmb(&pctrlr.ctrlr, &size);
	/* Ceil the current cmb vaddr and cmb size to 2MB_aligned */
	CU_ASSERT(mem_reg_addr == (void *)0x7F7C00E00000);
	CU_ASSERT(size == 0x7FE00000);

	rc = nvme_pcie_ctrlr_unmap_io_cmb(&pctrlr.ctrlr);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pctrlr.cmb.mem_register_addr == NULL);
	CU_ASSERT(pctrlr.cmb.mem_register_size == 0);

	/* cmb mem_register_addr not NULL */
	prepare_map_io_cmd(&pctrlr);
	pctrlr.cmb.mem_register_addr = (void *)0xDEADBEEF;
	pctrlr.cmb.mem_register_size = 1024;

	mem_reg_addr = nvme_pcie_ctrlr_map_io_cmb(&pctrlr.ctrlr, &size);
	CU_ASSERT(size == 1024);
	CU_ASSERT(mem_reg_addr == (void *)0xDEADBEEF);

	/* cmb.bar_va is NULL */
	prepare_map_io_cmd(&pctrlr);
	pctrlr.cmb.bar_va = NULL;

	mem_reg_addr = nvme_pcie_ctrlr_map_io_cmb(&pctrlr.ctrlr, &size);
	CU_ASSERT(mem_reg_addr == NULL);
	CU_ASSERT(size == 0);

	/* submission queue already used */
	prepare_map_io_cmd(&pctrlr);
	pctrlr.ctrlr.opts.use_cmb_sqs = true;

	mem_reg_addr = nvme_pcie_ctrlr_map_io_cmb(&pctrlr.ctrlr, &size);
	CU_ASSERT(mem_reg_addr == NULL);
	CU_ASSERT(size == 0);

	pctrlr.ctrlr.opts.use_cmb_sqs = false;

	/* Only SQS is supported */
	prepare_map_io_cmd(&pctrlr);
	cmbsz.bits.wds = 0;
	cmbsz.bits.rds = 0;
	nvme_pcie_ctrlr_set_reg_4(&pctrlr.ctrlr, offsetof(struct spdk_nvme_registers, cmbsz.raw),
				  cmbsz.raw);

	mem_reg_addr = nvme_pcie_ctrlr_map_io_cmb(&pctrlr.ctrlr, &size);
	CU_ASSERT(mem_reg_addr == NULL);
	CU_ASSERT(size == 0);

	/* CMB size is less than 4MB */
	prepare_map_io_cmd(&pctrlr);
	pctrlr.cmb.size = 1ULL << 16;

	mem_reg_addr = nvme_pcie_ctrlr_map_io_cmb(&pctrlr.ctrlr, &size);
	CU_ASSERT(mem_reg_addr == NULL);
	CU_ASSERT(size == 0);
}

static void
test_nvme_pcie_ctrlr_map_unmap_pmr(void)
{
	struct nvme_pcie_ctrlr pctrlr = {};
	volatile struct spdk_nvme_registers regs = {};
	union spdk_nvme_pmrcap_register pmrcap = {};
	struct dev_mem_resource cmd_res = {};
	int rc;

	pctrlr.regs = &regs;
	pctrlr.devhandle = (void *)&cmd_res;
	regs.cap.bits.pmrs = 1;
	cmd_res.addr = (void *)0x7F7C0080d000;
	cmd_res.len = 0x800000;
	cmd_res.phys_addr = 0xFC800000;
	pmrcap.bits.bir = 2;
	pmrcap.bits.cmss = 1;
	nvme_pcie_ctrlr_set_reg_4(&pctrlr.ctrlr,
				  offsetof(struct spdk_nvme_registers, pmrcap.raw),
				  pmrcap.raw);

	nvme_pcie_ctrlr_map_pmr(&pctrlr);
	CU_ASSERT(pctrlr.regs->pmrmscu == 0);
	/* Controller memory space enable, bit 1 */
	CU_ASSERT(pctrlr.regs->pmrmscl.raw == 0xFC800002);
	CU_ASSERT(pctrlr.regs->pmrsts.raw == 0);
	CU_ASSERT(pctrlr.pmr.bar_va == (void *)0x7F7C0080d000);
	CU_ASSERT(pctrlr.pmr.bar_pa == 0xFC800000);
	CU_ASSERT(pctrlr.pmr.size == 0x800000);

	rc = nvme_pcie_ctrlr_unmap_pmr(&pctrlr);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pctrlr.regs->pmrmscu == 0);
	CU_ASSERT(pctrlr.regs->pmrmscl.raw == 0);

	/* pmrcap value invalid */
	memset(&pctrlr, 0, sizeof(pctrlr));
	memset((void *)&regs, 0, sizeof(regs));
	memset(&cmd_res, 0, sizeof(cmd_res));

	pctrlr.regs = &regs;
	pctrlr.devhandle = (void *)&cmd_res;
	regs.cap.bits.pmrs = 1;
	cmd_res.addr = (void *)0x7F7C0080d000;
	cmd_res.len = 0x800000;
	cmd_res.phys_addr = 0xFC800000;
	pmrcap.raw = 0;
	nvme_pcie_ctrlr_set_reg_4(&pctrlr.ctrlr,
				  offsetof(struct spdk_nvme_registers, pmrcap.raw),
				  pmrcap.raw);

	nvme_pcie_ctrlr_map_pmr(&pctrlr);
	CU_ASSERT(pctrlr.pmr.bar_va == NULL);
	CU_ASSERT(pctrlr.pmr.bar_pa == 0);
	CU_ASSERT(pctrlr.pmr.size == 0);
}

static void
test_nvme_pcie_ctrlr_config_pmr(void)
{
	struct nvme_pcie_ctrlr pctrlr = {};
	union spdk_nvme_pmrcap_register pmrcap = {};
	union spdk_nvme_pmrsts_register pmrsts = {};
	union spdk_nvme_cap_register	cap = {};
	union spdk_nvme_pmrctl_register pmrctl = {};
	volatile struct spdk_nvme_registers regs = {};
	int rc;

	/* pmrctl enable */
	pctrlr.regs = &regs;
	pmrcap.bits.pmrtu = 0;
	pmrcap.bits.pmrto = 1;
	pmrsts.bits.nrdy = false;
	pmrctl.bits.en = 0;
	cap.bits.pmrs = 1;

	rc = nvme_pcie_ctrlr_set_pmrctl(&pctrlr, &pmrctl);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	rc = nvme_pcie_ctrlr_set_reg_8(&pctrlr.ctrlr, offsetof(struct spdk_nvme_registers, cap.raw),
				       cap.raw);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	rc = nvme_pcie_ctrlr_set_reg_4(&pctrlr.ctrlr, offsetof(struct spdk_nvme_registers, pmrcap.raw),
				       pmrcap.raw);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	rc = nvme_pcie_ctrlr_set_reg_4(&pctrlr.ctrlr, offsetof(struct spdk_nvme_registers, pmrsts.raw),
				       pmrsts.raw);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = nvme_pcie_ctrlr_config_pmr(&pctrlr.ctrlr, true);
	CU_ASSERT(rc == 0);
	rc = nvme_pcie_ctrlr_get_pmrctl(&pctrlr, &pmrctl);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pmrctl.bits.en == true);

	/* pmrctl disable */
	pmrsts.bits.nrdy = true;
	rc = nvme_pcie_ctrlr_set_reg_4(&pctrlr.ctrlr, offsetof(struct spdk_nvme_registers, pmrsts.raw),
				       pmrsts.raw);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	rc = nvme_pcie_ctrlr_set_pmrctl(&pctrlr, &pmrctl);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = nvme_pcie_ctrlr_config_pmr(&pctrlr.ctrlr, false);
	CU_ASSERT(rc == 0);
	rc = nvme_pcie_ctrlr_get_pmrctl(&pctrlr, &pmrctl);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pmrctl.bits.en == false);

	/* configuration exist */
	rc = nvme_pcie_ctrlr_config_pmr(&pctrlr.ctrlr, false);
	CU_ASSERT(rc == -EINVAL);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_pcie", NULL, NULL);
	CU_ADD_TEST(suite, test_prp_list_append);
	CU_ADD_TEST(suite, test_nvme_pcie_hotplug_monitor);
	CU_ADD_TEST(suite, test_shadow_doorbell_update);
	CU_ADD_TEST(suite, test_build_contig_hw_sgl_request);
	CU_ADD_TEST(suite, test_nvme_pcie_qpair_build_metadata);
	CU_ADD_TEST(suite, test_nvme_pcie_qpair_build_prps_sgl_request);
	CU_ADD_TEST(suite, test_nvme_pcie_qpair_build_hw_sgl_request);
	CU_ADD_TEST(suite, test_nvme_pcie_qpair_build_contig_request);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_regs_get_set);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_map_unmap_cmb);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_map_io_cmb);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_map_unmap_pmr);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_config_pmr);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
