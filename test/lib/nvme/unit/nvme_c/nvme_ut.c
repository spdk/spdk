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

#include "nvme/nvme.c"

#include "lib/test_env.c"

int
spdk_pci_nvme_enumerate(spdk_pci_enum_cb enum_cb, void *enum_ctx)
{
	return -1;
}

struct spdk_pci_id
spdk_pci_device_get_id(struct spdk_pci_device *pci_dev)
{
	struct spdk_pci_id pci_id;

	memset(&pci_id, 0xFF, sizeof(pci_id));

	return pci_id;
}

bool
spdk_nvme_transport_available(enum spdk_nvme_transport_type trtype)
{
	return true;
}

struct spdk_nvme_ctrlr *nvme_transport_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
		const struct spdk_nvme_ctrlr_opts *opts,
		void *devhandle)
{
	return NULL;
}

int
nvme_transport_ctrlr_scan(const struct spdk_nvme_transport_id *trid,
			  void *cb_ctx,
			  spdk_nvme_probe_cb probe_cb,
			  spdk_nvme_remove_cb remove_cb)
{
	return 0;
}

void
nvme_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
}

int
nvme_ctrlr_add_process(struct spdk_nvme_ctrlr *ctrlr, void *devhandle)
{
	return 0;
}

int
nvme_ctrlr_process_init(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

int
nvme_ctrlr_start(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

void
nvme_ctrlr_fail(struct spdk_nvme_ctrlr *ctrlr, bool hot_remove)
{
}

void
spdk_nvme_ctrlr_opts_set_defaults(struct spdk_nvme_ctrlr_opts *opts)
{
	memset(opts, 0, sizeof(*opts));
}

struct spdk_pci_addr
spdk_pci_device_get_addr(struct spdk_pci_device *pci_dev)
{
	struct spdk_pci_addr pci_addr;

	memset(&pci_addr, 0, sizeof(pci_addr));
	return pci_addr;
}

int
spdk_pci_addr_compare(const struct spdk_pci_addr *a1, const struct spdk_pci_addr *a2)
{
	return true;
}

void
nvme_ctrlr_proc_get_ref(struct spdk_nvme_ctrlr *ctrlr)
{
	return;
}

void
nvme_ctrlr_proc_put_ref(struct spdk_nvme_ctrlr *ctrlr)
{
	return;
}

int
nvme_ctrlr_get_ref_count(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

static void
test_opc_data_transfer(void)
{
	enum spdk_nvme_data_transfer xfer;

	xfer = spdk_nvme_opc_get_data_transfer(SPDK_NVME_OPC_FLUSH);
	CU_ASSERT(xfer == SPDK_NVME_DATA_NONE);

	xfer = spdk_nvme_opc_get_data_transfer(SPDK_NVME_OPC_WRITE);
	CU_ASSERT(xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER);

	xfer = spdk_nvme_opc_get_data_transfer(SPDK_NVME_OPC_READ);
	CU_ASSERT(xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST);

	xfer = spdk_nvme_opc_get_data_transfer(SPDK_NVME_OPC_GET_LOG_PAGE);
	CU_ASSERT(xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST);
}

static void
test_trid_parse(void)
{
	struct spdk_nvme_transport_id trid1, trid2;

	memset(&trid1, 0, sizeof(trid1));
	CU_ASSERT(spdk_nvme_transport_id_parse(&trid1,
					       "trtype:rdma\n"
					       "adrfam:ipv4\n"
					       "traddr:192.168.100.8\n"
					       "trsvcid:4420\n"
					       "subnqn:nqn.2014-08.org.nvmexpress.discovery") == 0);
	CU_ASSERT(trid1.trtype == SPDK_NVME_TRANSPORT_RDMA);
	CU_ASSERT(trid1.adrfam == SPDK_NVMF_ADRFAM_IPV4);
	CU_ASSERT(strcmp(trid1.traddr, "192.168.100.8") == 0);
	CU_ASSERT(strcmp(trid1.trsvcid, "4420") == 0);
	CU_ASSERT(strcmp(trid1.subnqn, "nqn.2014-08.org.nvmexpress.discovery") == 0);

	memset(&trid2, 0, sizeof(trid2));
	CU_ASSERT(spdk_nvme_transport_id_parse(&trid2, "trtype:PCIe traddr:0000:04:00.0") == 0);
	CU_ASSERT(trid2.trtype == SPDK_NVME_TRANSPORT_PCIE);
	CU_ASSERT(strcmp(trid2.traddr, "0000:04:00.0") == 0);

	CU_ASSERT(spdk_nvme_transport_id_compare(&trid1, &trid2) != 0);
}

static void
test_spdk_nvme_transport_id_parse_trtype(void)
{

	enum spdk_nvme_transport_type *trtype;
	enum spdk_nvme_transport_type sct;
	char *str;

	trtype = NULL;
	str = "unit_test";

	/* test function returned value when trtype is NULL but str not NULL */
	CU_ASSERT(spdk_nvme_transport_id_parse_trtype(trtype, str) == (-EINVAL));

	/* test function returned value when str is NULL but trtype not NULL */
	trtype = &sct;
	str = NULL;
	CU_ASSERT(spdk_nvme_transport_id_parse_trtype(trtype, str) == (-EINVAL));

	/* test function returned value when str and strtype not NULL, but str value
	 * not "PCIe" or "RDMA" */
	str = "unit_test";
	CU_ASSERT(spdk_nvme_transport_id_parse_trtype(trtype, str) == (-ENOENT));

	/* test trtype value when use function "strcasecmp" to compare str and "PCIe"，not case-sensitive */
	str = "PCIe";
	spdk_nvme_transport_id_parse_trtype(trtype, str);
	CU_ASSERT((*trtype) == SPDK_NVME_TRANSPORT_PCIE);

	str = "pciE";
	spdk_nvme_transport_id_parse_trtype(trtype, str);
	CU_ASSERT((*trtype) == SPDK_NVME_TRANSPORT_PCIE);

	/* test trtype value when use function "strcasecmp" to compare str and "RDMA"，not case-sensitive */
	str = "RDMA";
	spdk_nvme_transport_id_parse_trtype(trtype, str);
	CU_ASSERT((*trtype) == SPDK_NVME_TRANSPORT_RDMA);

	str = "rdma";
	spdk_nvme_transport_id_parse_trtype(trtype, str);
	CU_ASSERT((*trtype) == SPDK_NVME_TRANSPORT_RDMA);

}

static void
test_spdk_nvme_transport_id_parse_adrfam(void)
{

	enum spdk_nvmf_adrfam *adrfam;
	enum spdk_nvmf_adrfam sct;
	char *str;

	adrfam = NULL;
	str = "unit_test";

	/* test function returned value when adrfam is NULL but str not NULL */
	CU_ASSERT(spdk_nvme_transport_id_parse_adrfam(adrfam, str) == (-EINVAL));

	/* test function returned value when str is NULL but adrfam not NULL */
	adrfam = &sct;
	str = NULL;
	CU_ASSERT(spdk_nvme_transport_id_parse_adrfam(adrfam, str) == (-EINVAL));

	/* test function returned value when str and adrfam not NULL, but str value
	 * not "IPv4" or "IPv6" or "IB" or "FC" */
	str = "unit_test";
	CU_ASSERT(spdk_nvme_transport_id_parse_adrfam(adrfam, str) == (-ENOENT));

	/* test adrfam value when use function "strcasecmp" to compare str and "IPv4"，not case-sensitive */
	str = "IPv4";
	spdk_nvme_transport_id_parse_adrfam(adrfam, str);
	CU_ASSERT((*adrfam) == SPDK_NVMF_ADRFAM_IPV4);

	str = "ipV4";
	spdk_nvme_transport_id_parse_adrfam(adrfam, str);
	CU_ASSERT((*adrfam) == SPDK_NVMF_ADRFAM_IPV4);

	/* test adrfam value when use function "strcasecmp" to compare str and "IPv6"，not case-sensitive */
	str = "IPv6";
	spdk_nvme_transport_id_parse_adrfam(adrfam, str);
	CU_ASSERT((*adrfam) == SPDK_NVMF_ADRFAM_IPV6);

	str = "ipV6";
	spdk_nvme_transport_id_parse_adrfam(adrfam, str);
	CU_ASSERT((*adrfam) == SPDK_NVMF_ADRFAM_IPV6);

	/* test adrfam value when use function "strcasecmp" to compare str and "IB"，not case-sensitive */
	str = "IB";
	spdk_nvme_transport_id_parse_adrfam(adrfam, str);
	CU_ASSERT((*adrfam) == SPDK_NVMF_ADRFAM_IB);

	str = "ib";
	spdk_nvme_transport_id_parse_adrfam(adrfam, str);
	CU_ASSERT((*adrfam) == SPDK_NVMF_ADRFAM_IB);

	/* test adrfam value when use function "strcasecmp" to compare str and "FC"，not case-sensitive */
	str = "FC";
	spdk_nvme_transport_id_parse_adrfam(adrfam, str);
	CU_ASSERT((*adrfam) == SPDK_NVMF_ADRFAM_FC);

	str = "fc";
	spdk_nvme_transport_id_parse_adrfam(adrfam, str);
	CU_ASSERT((*adrfam) == SPDK_NVMF_ADRFAM_FC);

}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvme", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test_opc_data_transfer", test_opc_data_transfer) == NULL ||
		CU_add_test(suite, "test_spdk_nvme_transport_id_parse_trtype",
			    test_spdk_nvme_transport_id_parse_trtype) == NULL ||
		CU_add_test(suite, "test_spdk_nvme_transport_id_parse_adrfam",
			    test_spdk_nvme_transport_id_parse_adrfam) == NULL ||
		CU_add_test(suite, "test_trid_parse", test_trid_parse) == NULL
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
