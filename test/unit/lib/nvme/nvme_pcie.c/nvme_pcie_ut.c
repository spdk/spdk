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

#define UNIT_TEST_NO_VTOPHYS
#include "common/lib/test_env.c"

#include "nvme/nvme_pcie.c"

const char *
spdk_nvme_transport_id_trtype_str(enum spdk_nvme_transport_type trtype)
{
	switch (trtype) {
	case SPDK_NVME_TRANSPORT_PCIE:
		return "PCIe";
	case SPDK_NVME_TRANSPORT_RDMA:
		return "RDMA";
	case SPDK_NVME_TRANSPORT_FC:
		return "FC";
	default:
		return NULL;
	}
}

int
spdk_nvme_transport_id_populate_trstring(struct spdk_nvme_transport_id *trid, const char *trstring)
{
	int len, i;

	if (trstring == NULL) {
		return -EINVAL;
	}

	len = strnlen(trstring, SPDK_NVMF_TRSTRING_MAX_LEN);
	if (len == SPDK_NVMF_TRSTRING_MAX_LEN) {
		return -EINVAL;
	}

	/* cast official trstring to uppercase version of input. */
	for (i = 0; i < len; i++) {
		trid->trstring[i] = toupper(trstring[i]);
	}
	return 0;
}

struct spdk_log_flag SPDK_LOG_NVME = {
	.name = "nvme",
	.enabled = false,
};

struct nvme_driver *g_spdk_nvme_driver = NULL;

bool g_device_is_enumerated = false;

void
nvme_ctrlr_fail(struct spdk_nvme_ctrlr *ctrlr, bool hot_remove)
{
	CU_ASSERT(ctrlr != NULL);
	if (hot_remove) {
		ctrlr->is_removed = true;
	}

	ctrlr->is_failed = true;
}

struct spdk_uevent_entry {
	struct spdk_uevent		uevent;
	STAILQ_ENTRY(spdk_uevent_entry)	link;
};

static STAILQ_HEAD(, spdk_uevent_entry) g_uevents = STAILQ_HEAD_INITIALIZER(g_uevents);

int
spdk_get_uevent(int fd, struct spdk_uevent *uevent)
{
	struct spdk_uevent_entry *entry;

	if (STAILQ_EMPTY(&g_uevents)) {
		return 0;
	}

	entry = STAILQ_FIRST(&g_uevents);
	STAILQ_REMOVE_HEAD(&g_uevents, link);

	*uevent = entry->uevent;

	return 1;
}

int
spdk_pci_enumerate(struct spdk_pci_driver *driver, spdk_pci_enum_cb enum_cb, void *enum_ctx)
{
	g_device_is_enumerated = true;

	return 0;
}

static uint64_t g_vtophys_size = 0;

DEFINE_RETURN_MOCK(spdk_vtophys, uint64_t);
uint64_t
spdk_vtophys(void *buf, uint64_t *size)
{
	if (size) {
		*size = g_vtophys_size;
	}

	HANDLE_RETURN_MOCK(spdk_vtophys);

	return (uintptr_t)buf;
}

DEFINE_STUB(spdk_pci_device_get_addr, struct spdk_pci_addr, (struct spdk_pci_device *dev), {});
DEFINE_STUB(nvme_ctrlr_add_process, int, (struct spdk_nvme_ctrlr *ctrlr, void *devhandle), 0);
DEFINE_STUB(nvme_ctrlr_probe, int, (const struct spdk_nvme_transport_id *trid,
				    struct spdk_nvme_probe_ctx *probe_ctx, void *devhandle), 0);
DEFINE_STUB(spdk_nvme_ctrlr_get_current_process, struct spdk_nvme_ctrlr_process *,
	    (struct spdk_nvme_ctrlr *ctrlr), NULL);
DEFINE_STUB(spdk_pci_device_is_removed, bool, (struct spdk_pci_device *dev), false);
DEFINE_STUB(spdk_nvme_get_ctrlr_by_trid_unsafe, struct spdk_nvme_ctrlr *,
	    (const struct spdk_nvme_transport_id *trid), NULL);
DEFINE_STUB(spdk_nvme_ctrlr_get_regs_csts, union spdk_nvme_csts_register,
	    (struct spdk_nvme_ctrlr *ctrlr), {});
DEFINE_STUB(spdk_nvme_ctrlr_get_process, struct spdk_nvme_ctrlr_process *,
	    (struct spdk_nvme_ctrlr *ctrlr, pid_t pid), NULL);
DEFINE_STUB(nvme_completion_is_retry, bool, (const struct spdk_nvme_cpl *cpl), false);
DEFINE_STUB_V(spdk_nvme_qpair_print_command, (struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_cmd *cmd));
DEFINE_STUB_V(spdk_nvme_qpair_print_completion, (struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_cpl *cpl));
DEFINE_STUB_V(spdk_nvme_trid_populate_transport, (struct spdk_nvme_transport_id *trid,
		enum spdk_nvme_transport_type trtype));

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
	struct nvme_pcie_ctrlr pctrlr = {};
	struct spdk_uevent_entry entry = {};
	struct nvme_driver driver;
	pthread_mutexattr_t attr;
	struct spdk_nvme_probe_ctx test_nvme_probe_ctx = {};

	/* Initiate variables and ctrlr */
	driver.initialized = true;
	CU_ASSERT(pthread_mutexattr_init(&attr) == 0);
	CU_ASSERT(pthread_mutex_init(&driver.lock, &attr) == 0);
	TAILQ_INIT(&driver.shared_attached_ctrlrs);
	g_spdk_nvme_driver = &driver;

	/* Case 1:  SPDK_NVME_UEVENT_ADD/ NVME_VFIO */
	entry.uevent.subsystem = SPDK_NVME_UEVENT_SUBSYSTEM_VFIO;
	entry.uevent.action = SPDK_NVME_UEVENT_ADD;
	snprintf(entry.uevent.traddr, sizeof(entry.uevent.traddr), "0000:05:00.0");
	CU_ASSERT(STAILQ_EMPTY(&g_uevents));
	STAILQ_INSERT_TAIL(&g_uevents, &entry, link);

	_nvme_pcie_hotplug_monitor(&test_nvme_probe_ctx);

	CU_ASSERT(STAILQ_EMPTY(&g_uevents));
	CU_ASSERT(g_device_is_enumerated == true);
	g_device_is_enumerated = false;

	/* Case 2:  SPDK_NVME_UEVENT_ADD/ NVME_UIO */
	entry.uevent.subsystem = SPDK_NVME_UEVENT_SUBSYSTEM_UIO;
	entry.uevent.action = SPDK_NVME_UEVENT_ADD;
	snprintf(entry.uevent.traddr, sizeof(entry.uevent.traddr), "0000:05:00.0");
	CU_ASSERT(STAILQ_EMPTY(&g_uevents));
	STAILQ_INSERT_TAIL(&g_uevents, &entry, link);

	_nvme_pcie_hotplug_monitor(&test_nvme_probe_ctx);

	CU_ASSERT(STAILQ_EMPTY(&g_uevents));
	CU_ASSERT(g_device_is_enumerated == true);
	g_device_is_enumerated = false;

	/* Case 3: SPDK_NVME_UEVENT_REMOVE/ NVME_UIO */
	entry.uevent.subsystem = SPDK_NVME_UEVENT_SUBSYSTEM_UIO;
	entry.uevent.action = SPDK_NVME_UEVENT_REMOVE;
	snprintf(entry.uevent.traddr, sizeof(entry.uevent.traddr), "0000:05:00.0");
	CU_ASSERT(STAILQ_EMPTY(&g_uevents));
	STAILQ_INSERT_TAIL(&g_uevents, &entry, link);

	MOCK_SET(spdk_nvme_get_ctrlr_by_trid_unsafe, &pctrlr.ctrlr);

	_nvme_pcie_hotplug_monitor(&test_nvme_probe_ctx);

	CU_ASSERT(STAILQ_EMPTY(&g_uevents));
	CU_ASSERT(pctrlr.ctrlr.is_failed == true);
	pctrlr.ctrlr.is_failed = false;
	MOCK_CLEAR(spdk_nvme_get_ctrlr_by_trid_unsafe);

	/* Case 4: SPDK_NVME_UEVENT_REMOVE/ NVME_VFIO */
	entry.uevent.subsystem = SPDK_NVME_UEVENT_SUBSYSTEM_VFIO;
	entry.uevent.action = SPDK_NVME_UEVENT_REMOVE;
	snprintf(entry.uevent.traddr, sizeof(entry.uevent.traddr), "0000:05:00.0");
	CU_ASSERT(STAILQ_EMPTY(&g_uevents));
	STAILQ_INSERT_TAIL(&g_uevents, &entry, link);
	MOCK_SET(spdk_nvme_get_ctrlr_by_trid_unsafe, &pctrlr.ctrlr);

	_nvme_pcie_hotplug_monitor(&test_nvme_probe_ctx);

	CU_ASSERT(STAILQ_EMPTY(&g_uevents));
	CU_ASSERT(pctrlr.ctrlr.is_failed == true);
	pctrlr.ctrlr.is_failed = false;
	MOCK_CLEAR(spdk_nvme_get_ctrlr_by_trid_unsafe);

	/* Case 5:  Removed device detected in another process  */
	pctrlr.ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	snprintf(pctrlr.ctrlr.trid.traddr, sizeof(pctrlr.ctrlr.trid.traddr), "0000:02:00.0");
	pctrlr.ctrlr.remove_cb = NULL;
	pctrlr.ctrlr.is_failed = false;
	pctrlr.ctrlr.is_removed = false;
	TAILQ_INSERT_TAIL(&g_spdk_nvme_driver->shared_attached_ctrlrs, &pctrlr.ctrlr, tailq);

	MOCK_SET(spdk_pci_device_is_removed, false);

	_nvme_pcie_hotplug_monitor(&test_nvme_probe_ctx);

	CU_ASSERT(pctrlr.ctrlr.is_failed == false);

	MOCK_SET(spdk_pci_device_is_removed, true);

	_nvme_pcie_hotplug_monitor(&test_nvme_probe_ctx);

	CU_ASSERT(pctrlr.ctrlr.is_failed == true);

	pthread_mutex_destroy(&driver.lock);
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
	int rc;

	/* Test 1: Payload covered by a single mapping */
	req.payload_size = 100;
	req.payload = NVME_PAYLOAD_CONTIG(0, 0);
	g_vtophys_size = 100;
	MOCK_SET(spdk_vtophys, 0xDEADBEEF);

	rc = nvme_pcie_qpair_build_contig_hw_sgl_request(&qpair, &req, &tr);
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
	req.payload_size = 100;
	req.payload_offset = 50;
	req.payload = NVME_PAYLOAD_CONTIG(0, 0);
	g_vtophys_size = 1000;
	MOCK_SET(spdk_vtophys, 0xDEADBEEF);

	rc = nvme_pcie_qpair_build_contig_hw_sgl_request(&qpair, &req, &tr);
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
	req.payload_size = 100;
	req.payload = NVME_PAYLOAD_CONTIG(0, 0);
	g_vtophys_size = 60;
	tr.prp_sgl_bus_addr = 0xFF0FF;
	MOCK_SET(spdk_vtophys, 0xDEADBEEF);

	rc = nvme_pcie_qpair_build_contig_hw_sgl_request(&qpair, &req, &tr);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.type == SPDK_NVME_SGL_TYPE_LAST_SEGMENT);
	CU_ASSERT(req.cmd.dptr.sgl1.address == tr.prp_sgl_bus_addr);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.length == 2 * sizeof(struct spdk_nvme_sgl_descriptor));
	CU_ASSERT(tr.u.sgl[0].unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(tr.u.sgl[0].unkeyed.length = 60);
	CU_ASSERT(tr.u.sgl[0].address = 0xDEADBEEF);
	CU_ASSERT(tr.u.sgl[1].unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(tr.u.sgl[1].unkeyed.length = 40);
	CU_ASSERT(tr.u.sgl[1].address = 0xDEADBEEF);

	MOCK_CLEAR(spdk_vtophys);
	g_vtophys_size = 0;
	memset(&qpair, 0, sizeof(qpair));
	memset(&req, 0, sizeof(req));
	memset(&tr, 0, sizeof(tr));
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

	if (CU_add_test(suite, "prp_list_append", test_prp_list_append) == NULL ||
	    CU_add_test(suite, "nvme_pcie_hotplug_monitor", test_nvme_pcie_hotplug_monitor) == NULL ||
	    CU_add_test(suite, "shadow_doorbell_update", test_shadow_doorbell_update) == NULL ||
	    CU_add_test(suite, "build_contig_hw_sgl_request", test_build_contig_hw_sgl_request) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
