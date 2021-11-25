/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
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

#include "spdk_cunit.h"

#include "spdk/env.h"

#include "nvme/nvme_ns.c"

#include "common/lib/test_env.c"

SPDK_LOG_REGISTER_COMPONENT(nvme)

DEFINE_STUB(nvme_wait_for_completion_robust_lock, int,
	    (struct spdk_nvme_qpair *qpair,
	     struct nvme_completion_poll_status *status,
	     pthread_mutex_t *robust_mutex), 0);
DEFINE_STUB(nvme_ctrlr_multi_iocs_enabled, bool, (struct spdk_nvme_ctrlr *ctrlr), true);

static struct spdk_nvme_cpl fake_cpl = {};
static enum spdk_nvme_generic_command_status_code set_status_code = SPDK_NVME_SC_SUCCESS;

static void
fake_cpl_sc(spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	fake_cpl.status.sc = set_status_code;
	cb_fn(cb_arg, &fake_cpl);
}

static struct spdk_nvme_ns_data *fake_nsdata;
static struct spdk_nvme_zns_ns_data nsdata_zns = {
	.mar = 1024,
	.mor = 1024,
};

struct spdk_nvme_cmd g_ut_cmd = {};

int
nvme_ctrlr_cmd_identify(struct spdk_nvme_ctrlr *ctrlr, uint8_t cns, uint16_t cntid, uint32_t nsid,
			uint8_t csi, void *payload, size_t payload_size,
			spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	memset(&g_ut_cmd, 0, sizeof(g_ut_cmd));

	if (cns == SPDK_NVME_IDENTIFY_NS) {
		assert(payload_size == sizeof(struct spdk_nvme_ns_data));
		if (fake_nsdata) {
			memcpy(payload, fake_nsdata, sizeof(*fake_nsdata));
		} else {
			memset(payload, 0, payload_size);
		}
		fake_cpl_sc(cb_fn, cb_arg);
		return 0;
	} else if (cns == SPDK_NVME_IDENTIFY_NS_IOCS) {
		assert(payload_size == sizeof(struct spdk_nvme_zns_ns_data));
		memcpy(payload, &nsdata_zns, sizeof(struct spdk_nvme_zns_ns_data));
		return 0;
	} else if (cns == SPDK_NVME_IDENTIFY_NS_ID_DESCRIPTOR_LIST) {
		g_ut_cmd.cdw10_bits.identify.cns = cns;
		g_ut_cmd.cdw10_bits.identify.cntid = cntid;
		g_ut_cmd.cdw11_bits.identify.csi = csi;
		g_ut_cmd.nsid = nsid;
		return 0;
	}
	return -1;
}

void
nvme_completion_poll_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
}

int32_t
spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	return -1;
}

static void
test_nvme_ns_construct(void)
{
	struct spdk_nvme_ns ns = {};
	uint32_t id = 1;
	struct spdk_nvme_ctrlr ctrlr = { };

	nvme_ns_construct(&ns, id, &ctrlr);
	CU_ASSERT(ns.id == 1);
}

static void
test_nvme_ns_uuid(void)
{
	struct spdk_nvme_ns ns = {};
	uint32_t id = 1;
	struct spdk_nvme_ctrlr ctrlr = {};
	const struct spdk_uuid *uuid;
	struct spdk_uuid expected_uuid;

	memset(&expected_uuid, 0xA5, sizeof(expected_uuid));

	/* Empty list - no UUID should be found */
	nvme_ns_construct(&ns, id, &ctrlr);
	uuid = spdk_nvme_ns_get_uuid(&ns);
	CU_ASSERT(uuid == NULL);
	nvme_ns_destruct(&ns);

	/* NGUID only (no UUID in list) */
	nvme_ns_construct(&ns, id, &ctrlr);
	ns.id_desc_list[0] = 0x02; /* NIDT == NGUID */
	ns.id_desc_list[1] = 0x10; /* NIDL */
	memset(&ns.id_desc_list[4], 0xCC, 0x10);
	uuid = spdk_nvme_ns_get_uuid(&ns);
	CU_ASSERT(uuid == NULL);
	nvme_ns_destruct(&ns);

	/* Just UUID in the list */
	nvme_ns_construct(&ns, id, &ctrlr);
	ns.id_desc_list[0] = 0x03; /* NIDT == UUID */
	ns.id_desc_list[1] = 0x10; /* NIDL */
	memcpy(&ns.id_desc_list[4], &expected_uuid, sizeof(expected_uuid));
	uuid = spdk_nvme_ns_get_uuid(&ns);
	SPDK_CU_ASSERT_FATAL(uuid != NULL);
	CU_ASSERT(memcmp(uuid, &expected_uuid, sizeof(*uuid)) == 0);
	nvme_ns_destruct(&ns);

	/* UUID followed by NGUID */
	nvme_ns_construct(&ns, id, &ctrlr);
	ns.id_desc_list[0] = 0x03; /* NIDT == UUID */
	ns.id_desc_list[1] = 0x10; /* NIDL */
	memcpy(&ns.id_desc_list[4], &expected_uuid, sizeof(expected_uuid));
	ns.id_desc_list[20] = 0x02; /* NIDT == NGUID */
	ns.id_desc_list[21] = 0x10; /* NIDL */
	memset(&ns.id_desc_list[24], 0xCC, 0x10);
	uuid = spdk_nvme_ns_get_uuid(&ns);
	SPDK_CU_ASSERT_FATAL(uuid != NULL);
	CU_ASSERT(memcmp(uuid, &expected_uuid, sizeof(*uuid)) == 0);
	nvme_ns_destruct(&ns);

	/* NGUID followed by UUID */
	nvme_ns_construct(&ns, id, &ctrlr);
	ns.id_desc_list[0] = 0x02; /* NIDT == NGUID */
	ns.id_desc_list[1] = 0x10; /* NIDL */
	memset(&ns.id_desc_list[4], 0xCC, 0x10);
	ns.id_desc_list[20] = 0x03; /* NIDT = UUID */
	ns.id_desc_list[21] = 0x10; /* NIDL */
	memcpy(&ns.id_desc_list[24], &expected_uuid, sizeof(expected_uuid));
	uuid = spdk_nvme_ns_get_uuid(&ns);
	SPDK_CU_ASSERT_FATAL(uuid != NULL);
	CU_ASSERT(memcmp(uuid, &expected_uuid, sizeof(*uuid)) == 0);
	nvme_ns_destruct(&ns);
}

static void
test_nvme_ns_csi(void)
{
	struct spdk_nvme_ns ns = {};
	uint32_t id = 1;
	struct spdk_nvme_ctrlr ctrlr = {};
	enum spdk_nvme_csi csi;

	/* Empty list - SPDK_NVME_CSI_NVM should be returned */
	nvme_ns_construct(&ns, id, &ctrlr);
	csi = nvme_ns_get_csi(&ns);
	CU_ASSERT(csi == SPDK_NVME_CSI_NVM);
	nvme_ns_destruct(&ns);

	/* NVM CSI - SPDK_NVME_CSI_NVM should be returned */
	nvme_ns_construct(&ns, id, &ctrlr);
	ns.id_desc_list[0] = 0x4; /* NIDT == CSI */
	ns.id_desc_list[1] = 0x1; /* NIDL */
	ns.id_desc_list[4] = 0x0; /* SPDK_NVME_CSI_NVM */
	csi = nvme_ns_get_csi(&ns);
	CU_ASSERT(csi == SPDK_NVME_CSI_NVM);
	nvme_ns_destruct(&ns);

	/* NGUID followed by ZNS CSI - SPDK_NVME_CSI_ZNS should be returned */
	nvme_ns_construct(&ns, id, &ctrlr);
	ns.id_desc_list[0] = 0x02; /* NIDT == NGUID */
	ns.id_desc_list[1] = 0x10; /* NIDL */
	memset(&ns.id_desc_list[4], 0xCC, 0x10);
	ns.id_desc_list[20] = 0x4; /* NIDT == CSI */
	ns.id_desc_list[21] = 0x1; /* NIDL */
	ns.id_desc_list[24] = 0x2; /* SPDK_NVME_CSI_ZNS */
	csi = nvme_ns_get_csi(&ns);
	CU_ASSERT(csi == SPDK_NVME_CSI_ZNS);
	nvme_ns_destruct(&ns);

	/* KV CSI followed by NGUID - SPDK_NVME_CSI_KV should be returned */
	nvme_ns_construct(&ns, id, &ctrlr);
	ns.id_desc_list[0] = 0x4; /* NIDT == CSI */
	ns.id_desc_list[1] = 0x1; /* NIDL */
	ns.id_desc_list[4] = 0x1; /* SPDK_NVME_CSI_KV */
	ns.id_desc_list[5] = 0x02; /* NIDT == NGUID */
	ns.id_desc_list[6] = 0x10; /* NIDL */
	memset(&ns.id_desc_list[9], 0xCC, 0x10);
	csi = nvme_ns_get_csi(&ns);
	CU_ASSERT(csi == SPDK_NVME_CSI_KV);
	nvme_ns_destruct(&ns);
}

static void
test_nvme_ns_data(void)
{
	struct spdk_nvme_ns ns = {};
	struct spdk_nvme_ctrlr ctrlr = { };
	struct spdk_nvme_ns_data expected_nsdata = {
		.nsze = 1000,
		.ncap = 1000,
	};
	const struct spdk_nvme_ns_data *nsdata;

	fake_nsdata = &expected_nsdata;
	SPDK_CU_ASSERT_FATAL(nvme_ns_construct(&ns, 1, &ctrlr) == 0);
	fake_nsdata = NULL;
	CU_ASSERT(spdk_nvme_ns_is_active(&ns));
	CU_ASSERT(spdk_nvme_ns_get_id(&ns) == 1);
	CU_ASSERT(spdk_nvme_ns_get_num_sectors(&ns) == 1000);

	nsdata = spdk_nvme_ns_get_data(&ns);
	CU_ASSERT(nsdata != NULL);
	CU_ASSERT(nsdata->ncap == 1000);

	nvme_ns_destruct(&ns);

	/* Cached NS data is still accessible after destruction. But is cleared. */
	CU_ASSERT(!spdk_nvme_ns_is_active(&ns));
	CU_ASSERT(spdk_nvme_ns_get_id(&ns) == 1);
	CU_ASSERT(spdk_nvme_ns_get_num_sectors(&ns) == 0);
	CU_ASSERT(nsdata->ncap == 0);
	CU_ASSERT(nsdata == spdk_nvme_ns_get_data(&ns));
}

static void
test_nvme_ns_set_identify_data(void)
{
	struct spdk_nvme_ns ns = {};
	struct spdk_nvme_ctrlr ctrlr = {};

	ns.id = 1;
	ns.ctrlr = &ctrlr;

	ns.ctrlr->cdata.oncs.dsm = 1;
	ns.ctrlr->cdata.oncs.compare = 1;
	ns.ctrlr->cdata.vwc.present = 1;
	ns.ctrlr->cdata.oncs.write_zeroes = 1;
	ns.ctrlr->cdata.oncs.write_unc = 1;
	ns.ctrlr->min_page_size = 4096;
	ns.ctrlr->max_xfer_size = 131072;

	ns.nsdata.flbas.extended = 1;
	ns.nsdata.nsrescap.raw = 1;
	ns.nsdata.dps.pit = SPDK_NVME_FMT_NVM_PROTECTION_TYPE1;
	ns.nsdata.flbas.format = 0;
	ns.nsdata.lbaf[0].lbads = 9;
	ns.nsdata.lbaf[0].ms = 8;

	/* case 1:  nsdata->noiob > 0 */
	ns.nsdata.noiob = 1;
	nvme_ns_set_identify_data(&ns);
	CU_ASSERT(spdk_nvme_ns_get_optimal_io_boundary(&ns) == 1)

	CU_ASSERT(spdk_nvme_ns_get_sector_size(&ns) == 512);
	CU_ASSERT(spdk_nvme_ns_get_extended_sector_size(&ns) == 520);
	CU_ASSERT(spdk_nvme_ns_get_md_size(&ns) == 8);
	CU_ASSERT(spdk_nvme_ns_get_max_io_xfer_size(&ns) == 131072);
	CU_ASSERT(ns.sectors_per_max_io == 252);
	CU_ASSERT(ns.sectors_per_max_io_no_md == 256);
	CU_ASSERT(spdk_nvme_ns_get_pi_type(&ns) == SPDK_NVME_FMT_NVM_PROTECTION_TYPE1);

	CU_ASSERT(spdk_nvme_ns_get_flags(&ns) & SPDK_NVME_NS_EXTENDED_LBA_SUPPORTED);
	CU_ASSERT(spdk_nvme_ns_get_flags(&ns) & SPDK_NVME_NS_RESERVATION_SUPPORTED);
	CU_ASSERT(spdk_nvme_ns_get_flags(&ns) & SPDK_NVME_NS_COMPARE_SUPPORTED);
	CU_ASSERT(spdk_nvme_ns_get_flags(&ns) & SPDK_NVME_NS_FLUSH_SUPPORTED);
	CU_ASSERT(spdk_nvme_ns_get_flags(&ns) & SPDK_NVME_NS_WRITE_ZEROES_SUPPORTED);
	CU_ASSERT(spdk_nvme_ns_get_flags(&ns) & SPDK_NVME_NS_WRITE_UNCORRECTABLE_SUPPORTED);
	CU_ASSERT(spdk_nvme_ns_get_flags(&ns) & SPDK_NVME_NS_RESERVATION_SUPPORTED);
	CU_ASSERT(spdk_nvme_ns_get_flags(&ns) & SPDK_NVME_NS_DPS_PI_SUPPORTED);

	/* case 2: quirks for NVME_QUIRK_MDTS_EXCLUDE_MD */
	ns.ctrlr->quirks = NVME_QUIRK_MDTS_EXCLUDE_MD;
	nvme_ns_set_identify_data(&ns);
	CU_ASSERT(ns.sectors_per_max_io == 256);
	CU_ASSERT(ns.sectors_per_max_io_no_md == 256);
}

static void
test_spdk_nvme_ns_get_values(void)
{
	struct spdk_nvme_ns ns = {};
	struct spdk_nvme_ctrlr nsctrlr = {};

	ns.ctrlr = &nsctrlr;

	/* case1: spdk_nvme_ns_get_id */
	ns.id = 1;
	CU_ASSERT(spdk_nvme_ns_get_id(&ns) == 1);

	/* case2: spdk_nvme_ns_get_ctrlr */
	CU_ASSERT(spdk_nvme_ns_get_ctrlr(&ns) == &nsctrlr);

	/* case3: spdk_nvme_ns_get_max_io_xfer_size */
	ns.ctrlr->max_xfer_size = 65536;
	CU_ASSERT(spdk_nvme_ns_get_max_io_xfer_size(&ns) == 65536);

	/* case4: spdk_nvme_ns_get_sector_size */
	ns.sector_size = 512;
	CU_ASSERT(spdk_nvme_ns_get_sector_size(&ns) == 512);

	/* case5: spdk_nvme_ns_get_extended_sector_size */
	ns.extended_lba_size = 512;
	CU_ASSERT(spdk_nvme_ns_get_extended_sector_size(&ns) == 512);

	/* case6: spdk_nvme_ns_get_num_sectors */
	ns.nsdata.nsze = 1024;
	CU_ASSERT(spdk_nvme_ns_get_num_sectors(&ns) == 1024);

	/* case7: spdk_nvme_ns_get_size */
	CU_ASSERT(spdk_nvme_ns_get_size(&ns) == 524288);

	/* case8: spdk_nvme_ns_get_flags */
	ns.flags = 255;
	CU_ASSERT(spdk_nvme_ns_get_flags(&ns) == 255);

	/* case9: spdk_nvme_ns_get_pi_type */
	ns.pi_type = SPDK_NVME_FMT_NVM_PROTECTION_DISABLE;
	CU_ASSERT(spdk_nvme_ns_get_pi_type(&ns) == SPDK_NVME_FMT_NVM_PROTECTION_DISABLE);

	/* case10: spdk_nvme_ns_get_md_size */
	ns.md_size = 512;
	CU_ASSERT(spdk_nvme_ns_get_md_size(&ns) == 512);

	/* case11: spdk_nvme_ns_get_data */
	CU_ASSERT(spdk_nvme_ns_get_data(&ns) != NULL);

	/* case12: spdk_nvme_ns_get_optimal_io_boundary */
	ns.sectors_per_stripe = 1;
	CU_ASSERT(spdk_nvme_ns_get_optimal_io_boundary(&ns) == 1);

	/* case13: spdk_nvme_ns_get_dealloc_logical_block_read_value */
	ns.ctrlr->quirks = NVME_QUIRK_READ_ZERO_AFTER_DEALLOCATE | NVME_INTEL_QUIRK_WRITE_LATENCY;
	ns.nsdata.dlfeat.bits.read_value = SPDK_NVME_DEALLOC_NOT_REPORTED;
	CU_ASSERT(spdk_nvme_ns_get_dealloc_logical_block_read_value(&ns) == SPDK_NVME_DEALLOC_READ_00);

	ns.ctrlr->quirks = NVME_INTEL_QUIRK_READ_LATENCY;
	CU_ASSERT(spdk_nvme_ns_get_dealloc_logical_block_read_value(&ns) == SPDK_NVME_DEALLOC_NOT_REPORTED);

	/* case14: spdk_nvme_ns_get_csi */
	ns.csi = SPDK_NVME_CSI_NVM;
	CU_ASSERT(spdk_nvme_ns_get_csi(&ns) == SPDK_NVME_CSI_NVM);

	/* case15: spdk_nvme_ns_get_ana_group_id */
	ns.ana_group_id = 15;
	CU_ASSERT(spdk_nvme_ns_get_ana_group_id(&ns) == 15);

	/* case16: spdk_nvme_ns_get_ana_state */
	ns.ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;
	CU_ASSERT(spdk_nvme_ns_get_ana_state(&ns) == SPDK_NVME_ANA_OPTIMIZED_STATE);
}

static void
test_spdk_nvme_ns_is_active(void)
{
	struct spdk_nvme_ns ns = {};

	/* case1: nsdata->id == 0 return false */
	ns.id = 0;
	CU_ASSERT(spdk_nvme_ns_is_active(&ns) == false);

	/* case2: nsdata->ncap == 0 return false */
	ns.id = 1;
	ns.nsdata.ncap = 0;
	CU_ASSERT(spdk_nvme_ns_is_active(&ns) == false);

	/* case3: ns->ncap != 0 return true */
	ns.nsdata.ncap = 1;
	CU_ASSERT(spdk_nvme_ns_is_active(&ns) == true);
}

static void
spdk_nvme_ns_supports(void)
{
	struct spdk_nvme_ns ns = {};

	/* case1: spdk_nvme_ns_supports_extended_lba */
	ns.flags = SPDK_NVME_NS_DEALLOCATE_SUPPORTED;
	CU_ASSERT(spdk_nvme_ns_supports_extended_lba(&ns) == false);
	ns.flags = SPDK_NVME_NS_EXTENDED_LBA_SUPPORTED | SPDK_NVME_NS_DEALLOCATE_SUPPORTED;
	CU_ASSERT(spdk_nvme_ns_supports_extended_lba(&ns) == true);

	/* case2: spdk_nvme_ns_supports_compare */
	ns.flags = SPDK_NVME_NS_DEALLOCATE_SUPPORTED;
	CU_ASSERT(spdk_nvme_ns_supports_compare(&ns) == false);
	ns.flags = SPDK_NVME_NS_COMPARE_SUPPORTED | SPDK_NVME_NS_DEALLOCATE_SUPPORTED;
	CU_ASSERT(spdk_nvme_ns_supports_compare(&ns) == true);
}

static void
test_nvme_ns_has_supported_iocs_specific_data(void)
{
	struct spdk_nvme_ns ns = {};

	/* case 1: ns.csi == SPDK_NVME_CSI_NVM. Expect: false */
	ns.csi = SPDK_NVME_CSI_NVM;
	CU_ASSERT(nvme_ns_has_supported_iocs_specific_data(&ns) == false);
	/* case 2: ns.csi == SPDK_NVME_CSI_ZNS. Expect: true */
	ns.csi = SPDK_NVME_CSI_ZNS;
	CU_ASSERT(nvme_ns_has_supported_iocs_specific_data(&ns) == true);
	/* case 3: default ns.csi == SPDK_NVME_CSI_KV. Expect: false */
	ns.csi = SPDK_NVME_CSI_KV;
	CU_ASSERT(nvme_ns_has_supported_iocs_specific_data(&ns) == false);
}

static void
test_nvme_ctrlr_identify_ns_iocs_specific(void)
{
	struct spdk_nvme_ns ns = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	int rc = 0;

	ns.ctrlr = &ctrlr;

	ns.csi = SPDK_NVME_CSI_ZNS;
	ns.id = 1;

	/* case 1: Test nvme_ctrlr_identify_ns_iocs_specific. Expect: PASS. */
	rc = nvme_ctrlr_identify_ns_iocs_specific(&ns);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ns.nsdata_zns->mar == 1024);
	CU_ASSERT(ns.nsdata_zns->mor == 1024);

	/* case 2: Test nvme_ns_free_zns_specific_data. Expect: PASS. */
	nvme_ns_free_zns_specific_data(&ns);
	CU_ASSERT(ns.nsdata_zns == NULL);
}

static void
test_nvme_ctrlr_identify_id_desc(void)
{
	struct spdk_nvme_ns ns = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	int rc;

	ns.ctrlr = &ctrlr;
	ns.ctrlr->vs.raw = SPDK_NVME_VERSION(1, 3, 0);
	ns.ctrlr->cap.bits.css |= SPDK_NVME_CAP_CSS_IOCS;
	ns.id = 1;

	rc = nvme_ctrlr_identify_id_desc(&ns);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_ut_cmd.cdw10_bits.identify.cns == SPDK_NVME_IDENTIFY_NS_ID_DESCRIPTOR_LIST);
	CU_ASSERT(g_ut_cmd.cdw10_bits.identify.cntid == 0);
	CU_ASSERT(g_ut_cmd.cdw11_bits.identify.csi == spdk_nvme_ns_get_csi(&ns));
	CU_ASSERT(g_ut_cmd.nsid == 1);

	/* NVME version and css unsupported */
	ns.ctrlr->vs.raw = SPDK_NVME_VERSION(1, 2, 0);
	ns.ctrlr->cap.bits.css &= ~SPDK_NVME_CAP_CSS_IOCS;

	rc = nvme_ctrlr_identify_id_desc(&ns);
	CU_ASSERT(rc == 0);
}

static void
test_nvme_ns_find_id_desc(void)
{
	struct spdk_nvme_ns ns = {};
	struct spdk_nvme_ns_id_desc *desc = NULL;
	const uint8_t *csi = NULL;
	size_t length = 0;

	desc = (void *)ns.id_desc_list;
	desc->nidl = 4;
	desc->nidt = SPDK_NVME_NIDT_CSI;

	/* Case 1: get id descriptor successfully */
	csi = nvme_ns_find_id_desc(&ns, SPDK_NVME_NIDT_CSI, &length);
	CU_ASSERT(csi == desc->nid);
	CU_ASSERT(length == 4);

	/* Case 2: ns_id length invalid, expect fail */
	desc->nidl = 0;

	csi = nvme_ns_find_id_desc(&ns, SPDK_NVME_NIDT_CSI, &length);
	CU_ASSERT(csi == NULL);

	/* Case 3: No correct id descriptor type entry, expect fail */
	desc->nidl = 4;
	desc->nidt = SPDK_NVME_NIDT_CSI;

	csi = nvme_ns_find_id_desc(&ns, SPDK_NVME_NIDT_UUID, &length);
	CU_ASSERT(csi == NULL);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme", NULL, NULL);

	CU_ADD_TEST(suite, test_nvme_ns_construct);
	CU_ADD_TEST(suite, test_nvme_ns_uuid);
	CU_ADD_TEST(suite, test_nvme_ns_csi);
	CU_ADD_TEST(suite, test_nvme_ns_data);
	CU_ADD_TEST(suite, test_nvme_ns_set_identify_data);
	CU_ADD_TEST(suite, test_spdk_nvme_ns_get_values);
	CU_ADD_TEST(suite, test_spdk_nvme_ns_is_active);
	CU_ADD_TEST(suite, spdk_nvme_ns_supports);
	CU_ADD_TEST(suite, test_nvme_ns_has_supported_iocs_specific_data);
	CU_ADD_TEST(suite, test_nvme_ctrlr_identify_ns_iocs_specific);
	CU_ADD_TEST(suite, test_nvme_ctrlr_identify_id_desc);
	CU_ADD_TEST(suite, test_nvme_ns_find_id_desc);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
