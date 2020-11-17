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

#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/opal.h"

#define MAX_DEVS 64

struct dev {
	struct spdk_pci_addr			pci_addr;
	struct spdk_nvme_ctrlr			*ctrlr;
	const struct spdk_nvme_ctrlr_data	*cdata;
	struct spdk_nvme_ns_data		*common_ns_data;
	int					outstanding_admin_cmds;
	struct spdk_opal_dev			*opal_dev;
};

static struct dev devs[MAX_DEVS];
static int num_devs = 0;
static int g_shm_id = -1;

#define foreach_dev(iter) \
	for (iter = devs; iter - devs < num_devs; iter++)

enum controller_display_model {
	CONTROLLER_DISPLAY_ALL			= 0x0,
	CONTROLLER_DISPLAY_SIMPLISTIC		= 0x1,
};

static int
cmp_devs(const void *ap, const void *bp)
{
	const struct dev *a = ap, *b = bp;

	return spdk_pci_addr_compare(&a->pci_addr, &b->pci_addr);
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	return true;
}

static void
identify_common_ns_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct dev *dev = cb_arg;

	if (cpl->status.sc != SPDK_NVME_SC_SUCCESS) {
		/* Identify Namespace for NSID = FFFFFFFFh is optional, so failure is not fatal. */
		spdk_dma_free(dev->common_ns_data);
		dev->common_ns_data = NULL;
	}

	dev->outstanding_admin_cmds--;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct dev *dev;
	struct spdk_nvme_cmd cmd;

	/* add to dev list */
	dev = &devs[num_devs++];
	spdk_pci_addr_parse(&dev->pci_addr, trid->traddr);
	dev->ctrlr = ctrlr;

	/* Retrieve controller data */
	dev->cdata = spdk_nvme_ctrlr_get_data(dev->ctrlr);

	dev->common_ns_data = spdk_dma_zmalloc(sizeof(struct spdk_nvme_ns_data), 4096, NULL);
	if (dev->common_ns_data == NULL) {
		fprintf(stderr, "common_ns_data allocation failure\n");
		return;
	}

	/* Identify Namespace with NSID set to FFFFFFFFh to get common namespace capabilities. */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_IDENTIFY;
	cmd.cdw10_bits.identify.cns = 0; /* CNS = 0 (Identify Namespace) */
	cmd.nsid = SPDK_NVME_GLOBAL_NS_TAG;

	dev->outstanding_admin_cmds++;
	if (spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, dev->common_ns_data,
					  sizeof(struct spdk_nvme_ns_data), identify_common_ns_cb, dev) != 0) {
		dev->outstanding_admin_cmds--;
		spdk_dma_free(dev->common_ns_data);
		dev->common_ns_data = NULL;
	}

	while (dev->outstanding_admin_cmds) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}
}

static void usage(void)
{
	printf("NVMe Management Options");
	printf("\n");
	printf("\t[1: list controllers]\n");
	printf("\t[2: create namespace]\n");
	printf("\t[3: delete namespace]\n");
	printf("\t[4: attach namespace to controller]\n");
	printf("\t[5: detach namespace from controller]\n");
	printf("\t[6: format namespace or controller]\n");
	printf("\t[7: firmware update]\n");
	printf("\t[8: opal]\n");
	printf("\t[9: quit]\n");
}

static void
display_namespace_dpc(const struct spdk_nvme_ns_data *nsdata)
{
	if (nsdata->dpc.pit1 || nsdata->dpc.pit2 || nsdata->dpc.pit3) {
		if (nsdata->dpc.pit1) {
			printf("PIT1 ");
		}

		if (nsdata->dpc.pit2) {
			printf("PIT2 ");
		}

		if (nsdata->dpc.pit3) {
			printf("PIT3 ");
		}
	} else {
		printf("Not Supported\n");
		return;
	}

	if (nsdata->dpc.md_start && nsdata->dpc.md_end) {
		printf("Location: Head or Tail\n");
	} else if (nsdata->dpc.md_start) {
		printf("Location: Head\n");
	} else if (nsdata->dpc.md_end) {
		printf("Location: Tail\n");
	} else {
		printf("Not Supported\n");
	}
}

static void
display_namespace(struct spdk_nvme_ns *ns)
{
	const struct spdk_nvme_ns_data		*nsdata;
	uint32_t				i;

	nsdata = spdk_nvme_ns_get_data(ns);

	printf("Namespace ID:%d\n", spdk_nvme_ns_get_id(ns));

	printf("Size (in LBAs):              %lld (%lldM)\n",
	       (long long)nsdata->nsze,
	       (long long)nsdata->nsze / 1024 / 1024);
	printf("Capacity (in LBAs):          %lld (%lldM)\n",
	       (long long)nsdata->ncap,
	       (long long)nsdata->ncap / 1024 / 1024);
	printf("Utilization (in LBAs):       %lld (%lldM)\n",
	       (long long)nsdata->nuse,
	       (long long)nsdata->nuse / 1024 / 1024);
	printf("Format Progress Indicator:   %s\n",
	       nsdata->fpi.fpi_supported ? "Supported" : "Not Supported");
	if (nsdata->fpi.fpi_supported && nsdata->fpi.percentage_remaining) {
		printf("Formatted Percentage:	%d%%\n", 100 - nsdata->fpi.percentage_remaining);
	}
	printf("Number of LBA Formats:       %d\n", nsdata->nlbaf + 1);
	printf("Current LBA Format:          LBA Format #%02d\n",
	       nsdata->flbas.format);
	for (i = 0; i <= nsdata->nlbaf; i++)
		printf("LBA Format #%02d: Data Size: %5d  Metadata Size: %5d\n",
		       i, 1 << nsdata->lbaf[i].lbads, nsdata->lbaf[i].ms);
	printf("Data Protection Capabilities:");
	display_namespace_dpc(nsdata);
	if (SPDK_NVME_FMT_NVM_PROTECTION_DISABLE == nsdata->dps.pit) {
		printf("Data Protection Setting:     N/A\n");
	} else {
		printf("Data Protection Setting:     PIT%d Location: %s\n",
		       nsdata->dps.pit, nsdata->dps.md_start ? "Head" : "Tail");
	}
	printf("Multipath IO and Sharing:    %s\n",
	       nsdata->nmic.can_share ? "Supported" : "Not Supported");
	printf("\n");
}

static void
display_controller(struct dev *dev, int model)
{
	struct spdk_nvme_ns			*ns;
	const struct spdk_nvme_ctrlr_data	*cdata;
	uint8_t					str[128];
	uint32_t				nsid;

	cdata = spdk_nvme_ctrlr_get_data(dev->ctrlr);

	if (model == CONTROLLER_DISPLAY_SIMPLISTIC) {
		printf("%04x:%02x:%02x.%02x ",
		       dev->pci_addr.domain, dev->pci_addr.bus, dev->pci_addr.dev, dev->pci_addr.func);
		printf("%-40.40s %-20.20s ",
		       cdata->mn, cdata->sn);
		printf("%5d ", cdata->cntlid);
		printf("\n");
		return;
	}

	printf("=====================================================\n");
	printf("NVMe Controller:	%04x:%02x:%02x.%02x\n",
	       dev->pci_addr.domain, dev->pci_addr.bus, dev->pci_addr.dev, dev->pci_addr.func);
	printf("============================\n");
	printf("Controller Capabilities/Features\n");
	printf("Controller ID:		%d\n", cdata->cntlid);
	snprintf(str, sizeof(cdata->sn) + 1, "%s", cdata->sn);
	printf("Serial Number:		%s\n", str);
	printf("\n");

	printf("Admin Command Set Attributes\n");
	printf("============================\n");
	printf("Namespace Manage And Attach:		%s\n",
	       cdata->oacs.ns_manage ? "Supported" : "Not Supported");
	printf("Namespace Format:			%s\n",
	       cdata->oacs.format ? "Supported" : "Not Supported");
	printf("\n");
	printf("NVM Command Set Attributes\n");
	printf("============================\n");
	if (cdata->fna.format_all_ns) {
		printf("Namespace format operation applies to all namespaces\n");
	} else {
		printf("Namespace format operation applies to per namespace\n");
	}
	printf("\n");
	printf("Namespace Attributes\n");
	printf("============================\n");
	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(dev->ctrlr);
	     nsid != 0; nsid = spdk_nvme_ctrlr_get_next_active_ns(dev->ctrlr, nsid)) {
		ns = spdk_nvme_ctrlr_get_ns(dev->ctrlr, nsid);
		assert(ns != NULL);
		display_namespace(ns);
	}
}

static void
display_controller_list(void)
{
	struct dev			*iter;

	foreach_dev(iter) {
		display_controller(iter, CONTROLLER_DISPLAY_ALL);
	}
}

static char *
get_line(char *buf, int buf_size, FILE *f, bool secret)
{
	char *ch;
	size_t len;
	struct termios default_attr = {}, new_attr = {};
	int ret;

	if (secret) {
		ret = tcgetattr(STDIN_FILENO, &default_attr);
		if (ret) {
			return NULL;
		}

		new_attr = default_attr;
		new_attr.c_lflag &= ~ECHO;  /* disable echo */
		ret = tcsetattr(STDIN_FILENO, TCSAFLUSH, &new_attr);
		if (ret) {
			return NULL;
		}
	}

	ch = fgets(buf, buf_size, f);
	if (ch == NULL) {
		return NULL;
	}

	if (secret) {
		ret = tcsetattr(STDIN_FILENO, TCSAFLUSH, &default_attr); /* restore default confing */
		if (ret) {
			return NULL;
		}
	}

	len = strlen(buf);
	if (len > 0 && buf[len - 1] == '\n') {
		buf[len - 1] = '\0';
	}
	return buf;
}

static struct dev *
get_controller(void)
{
	struct spdk_pci_addr			pci_addr;
	char					address[64];
	char					*p;
	int					ch;
	struct dev				*iter;

	memset(address, 0, sizeof(address));

	foreach_dev(iter) {
		display_controller(iter, CONTROLLER_DISPLAY_SIMPLISTIC);
	}

	printf("Please Input PCI Address(domain:bus:dev.func):\n");

	while ((ch = getchar()) != '\n' && ch != EOF);
	p = get_line(address, 64, stdin, false);
	if (p == NULL) {
		return NULL;
	}

	while (isspace(*p)) {
		p++;
	}

	if (spdk_pci_addr_parse(&pci_addr, p) < 0) {
		return NULL;
	}

	foreach_dev(iter) {
		if (spdk_pci_addr_compare(&pci_addr, &iter->pci_addr) == 0) {
			return iter;
		}
	}
	return NULL;
}

static int
get_lba_format(const struct spdk_nvme_ns_data *ns_data)
{
	int lbaf, i;

	printf("\nSupported LBA formats:\n");
	for (i = 0; i <= ns_data->nlbaf; i++) {
		printf("%2d: %d data bytes", i, 1 << ns_data->lbaf[i].lbads);
		if (ns_data->lbaf[i].ms) {
			printf(" + %d metadata bytes", ns_data->lbaf[i].ms);
		}
		printf("\n");
	}

	printf("Please input LBA format index (0 - %d):\n", ns_data->nlbaf);
	if (scanf("%d", &lbaf) != 1 || lbaf > ns_data->nlbaf) {
		return -1;
	}

	return lbaf;
}

static void
identify_allocated_ns_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct dev *dev = cb_arg;

	dev->outstanding_admin_cmds--;
}

static uint32_t
get_allocated_nsid(struct dev *dev)
{
	uint32_t nsid;
	size_t i;
	struct spdk_nvme_ns_list *ns_list;
	struct spdk_nvme_cmd cmd = {0};

	ns_list = spdk_dma_zmalloc(sizeof(*ns_list), 4096, NULL);
	if (ns_list == NULL) {
		printf("Allocation error\n");
		return 0;
	}

	cmd.opc = SPDK_NVME_OPC_IDENTIFY;
	cmd.cdw10_bits.identify.cns = SPDK_NVME_IDENTIFY_ALLOCATED_NS_LIST;
	cmd.nsid = 0;

	dev->outstanding_admin_cmds++;
	if (spdk_nvme_ctrlr_cmd_admin_raw(dev->ctrlr, &cmd, ns_list, sizeof(*ns_list),
					  identify_allocated_ns_cb, dev)) {
		printf("Identify command failed\n");
		spdk_dma_free(ns_list);
		return 0;
	}

	while (dev->outstanding_admin_cmds) {
		spdk_nvme_ctrlr_process_admin_completions(dev->ctrlr);
	}

	printf("Allocated Namespace IDs:\n");
	for (i = 0; i < SPDK_COUNTOF(ns_list->ns_list); i++) {
		if (ns_list->ns_list[i] == 0) {
			break;
		}
		printf("%u\n", ns_list->ns_list[i]);
	}

	spdk_dma_free(ns_list);

	printf("Please Input Namespace ID:\n");
	if (!scanf("%u", &nsid)) {
		printf("Invalid Namespace ID\n");
		nsid = 0;
	}

	return nsid;
}

static void
ns_attach(struct dev *device, int attachment_op, int ctrlr_id, int ns_id)
{
	int ret = 0;
	struct spdk_nvme_ctrlr_list *ctrlr_list;

	ctrlr_list = spdk_dma_zmalloc(sizeof(struct spdk_nvme_ctrlr_list),
				      4096, NULL);
	if (ctrlr_list == NULL) {
		printf("Allocation error (controller list)\n");
		exit(1);
	}

	ctrlr_list->ctrlr_count = 1;
	ctrlr_list->ctrlr_list[0] = ctrlr_id;

	if (attachment_op == SPDK_NVME_NS_CTRLR_ATTACH) {
		ret = spdk_nvme_ctrlr_attach_ns(device->ctrlr, ns_id, ctrlr_list);
	} else if (attachment_op == SPDK_NVME_NS_CTRLR_DETACH) {
		ret = spdk_nvme_ctrlr_detach_ns(device->ctrlr, ns_id, ctrlr_list);
	}

	if (ret) {
		fprintf(stdout, "ns attach: Failed\n");
	}

	spdk_dma_free(ctrlr_list);
}

static void
ns_manage_add(struct dev *device, uint64_t ns_size, uint64_t ns_capacity, int ns_lbasize,
	      uint8_t ns_dps_type, uint8_t ns_dps_location, uint8_t ns_nmic)
{
	uint32_t nsid;
	struct spdk_nvme_ns_data *ndata;

	ndata = spdk_dma_zmalloc(sizeof(struct spdk_nvme_ns_data), 4096, NULL);
	if (ndata == NULL) {
		printf("Allocation error (namespace data)\n");
		exit(1);
	}

	ndata->nsze = ns_size;
	ndata->ncap = ns_capacity;
	ndata->flbas.format = ns_lbasize;
	if (SPDK_NVME_FMT_NVM_PROTECTION_DISABLE != ns_dps_type) {
		ndata->dps.pit = ns_dps_type;
		ndata->dps.md_start = ns_dps_location;
	}
	ndata->nmic.can_share = ns_nmic;
	nsid = spdk_nvme_ctrlr_create_ns(device->ctrlr, ndata);
	if (nsid == 0) {
		fprintf(stdout, "ns manage: Failed\n");
	} else {
		printf("Created namespace ID %u\n", nsid);
	}

	spdk_dma_free(ndata);
}

static void
ns_manage_delete(struct dev *device, int ns_id)
{
	int ret = 0;

	ret = spdk_nvme_ctrlr_delete_ns(device->ctrlr, ns_id);
	if (ret) {
		fprintf(stdout, "ns manage: Failed\n");
		return;
	}
}

static void
nvme_manage_format(struct dev *device, int ns_id, int ses, int pi, int pil, int ms, int lbaf)
{
	int ret = 0;
	struct spdk_nvme_format format = {};

	format.lbaf	= lbaf;
	format.ms	= ms;
	format.pi	= pi;
	format.pil	= pil;
	format.ses	= ses;
	ret = spdk_nvme_ctrlr_format(device->ctrlr, ns_id, &format);
	if (ret) {
		fprintf(stdout, "nvme format: Failed\n");
		return;
	}
}

static void
attach_and_detach_ns(int attachment_op)
{
	uint32_t	nsid;
	struct dev	*ctrlr;

	ctrlr = get_controller();
	if (ctrlr == NULL) {
		printf("Invalid controller PCI Address.\n");
		return;
	}

	if (!ctrlr->cdata->oacs.ns_manage) {
		printf("Controller does not support ns management\n");
		return;
	}

	nsid = get_allocated_nsid(ctrlr);
	if (nsid == 0) {
		printf("Invalid Namespace ID\n");
		return;
	}

	ns_attach(ctrlr, attachment_op, ctrlr->cdata->cntlid, nsid);
}

static void
add_ns(void)
{
	uint64_t	ns_size		= 0;
	uint64_t	ns_capacity	= 0;
	int		ns_lbasize;
	int		ns_dps_type	= 0;
	int		ns_dps_location	= 0;
	int		ns_nmic		= 0;
	struct dev	*ctrlr		= NULL;

	ctrlr = get_controller();
	if (ctrlr == NULL) {
		printf("Invalid controller PCI Address.\n");
		return;
	}

	if (!ctrlr->cdata->oacs.ns_manage) {
		printf("Controller does not support ns management\n");
		return;
	}

	if (!ctrlr->common_ns_data) {
		printf("Controller did not return common namespace capabilities\n");
		return;
	}

	ns_lbasize = get_lba_format(ctrlr->common_ns_data);
	if (ns_lbasize < 0) {
		printf("Invalid LBA format number\n");
		return;
	}

	printf("Please Input Namespace Size (in LBAs):\n");
	if (!scanf("%" SCNu64, &ns_size)) {
		printf("Invalid Namespace Size\n");
		while (getchar() != '\n');
		return;
	}

	printf("Please Input Namespace Capacity (in LBAs):\n");
	if (!scanf("%" SCNu64, &ns_capacity)) {
		printf("Invalid Namespace Capacity\n");
		while (getchar() != '\n');
		return;
	}

	printf("Please Input Data Protection Type (0 - 3):\n");
	if (!scanf("%d", &ns_dps_type)) {
		printf("Invalid Data Protection Type\n");
		while (getchar() != '\n');
		return;
	}

	if (SPDK_NVME_FMT_NVM_PROTECTION_DISABLE != ns_dps_type) {
		printf("Please Input Data Protection Location (1: Head; 0: Tail):\n");
		if (!scanf("%d", &ns_dps_location)) {
			printf("Invalid Data Protection Location\n");
			while (getchar() != '\n');
			return;
		}
	}

	printf("Please Input Multi-path IO and Sharing Capabilities (1: Share; 0: Private):\n");
	if (!scanf("%d", &ns_nmic)) {
		printf("Invalid Multi-path IO and Sharing Capabilities\n");
		while (getchar() != '\n');
		return;
	}

	ns_manage_add(ctrlr, ns_size, ns_capacity, ns_lbasize,
		      ns_dps_type, ns_dps_location, ns_nmic);
}

static void
delete_ns(void)
{
	int					ns_id;
	struct dev				*ctrlr;

	ctrlr = get_controller();
	if (ctrlr == NULL) {
		printf("Invalid controller PCI Address.\n");
		return;
	}

	if (!ctrlr->cdata->oacs.ns_manage) {
		printf("Controller does not support ns management\n");
		return;
	}

	printf("Please Input Namespace ID:\n");
	if (!scanf("%d", &ns_id)) {
		printf("Invalid Namespace ID\n");
		while (getchar() != '\n');
		return;
	}

	ns_manage_delete(ctrlr, ns_id);
}

static void
format_nvm(void)
{
	int					ns_id;
	int					ses;
	int					pil;
	int					pi;
	int					ms;
	int					lbaf;
	char					option;
	struct dev				*ctrlr;
	const struct spdk_nvme_ctrlr_data	*cdata;
	struct spdk_nvme_ns			*ns;
	const struct spdk_nvme_ns_data		*nsdata;

	ctrlr = get_controller();
	if (ctrlr == NULL) {
		printf("Invalid controller PCI BDF.\n");
		return;
	}

	cdata = ctrlr->cdata;

	if (!cdata->oacs.format) {
		printf("Controller does not support Format NVM command\n");
		return;
	}

	if (cdata->fna.format_all_ns) {
		ns_id = SPDK_NVME_GLOBAL_NS_TAG;
		ns = spdk_nvme_ctrlr_get_ns(ctrlr->ctrlr, 1);
	} else {
		printf("Please Input Namespace ID (1 - %d):\n", cdata->nn);
		if (!scanf("%d", &ns_id)) {
			printf("Invalid Namespace ID\n");
			while (getchar() != '\n');
			return;
		}
		ns = spdk_nvme_ctrlr_get_ns(ctrlr->ctrlr, ns_id);
	}

	if (ns == NULL) {
		printf("Namespace ID %d not found\n", ns_id);
		while (getchar() != '\n');
		return;
	}

	nsdata = spdk_nvme_ns_get_data(ns);

	printf("Please Input Secure Erase Setting:\n");
	printf("	0: No secure erase operation requested\n");
	printf("	1: User data erase\n");
	if (cdata->fna.crypto_erase_supported) {
		printf("	2: Cryptographic erase\n");
	}
	if (!scanf("%d", &ses)) {
		printf("Invalid Secure Erase Setting\n");
		while (getchar() != '\n');
		return;
	}

	lbaf = get_lba_format(nsdata);
	if (lbaf < 0) {
		printf("Invalid LBA format number\n");
		return;
	}

	if (nsdata->lbaf[lbaf].ms) {
		printf("Please Input Protection Information:\n");
		printf("	0: Protection information is not enabled\n");
		printf("	1: Protection information is enabled, Type 1\n");
		printf("	2: Protection information is enabled, Type 2\n");
		printf("	3: Protection information is enabled, Type 3\n");
		if (!scanf("%d", &pi)) {
			printf("Invalid protection information\n");
			while (getchar() != '\n');
			return;
		}

		if (pi) {
			printf("Please Input Protection Information Location:\n");
			printf("	0: Protection information transferred as the last eight bytes of metadata\n");
			printf("	1: Protection information transferred as the first eight bytes of metadata\n");
			if (!scanf("%d", &pil)) {
				printf("Invalid protection information location\n");
				while (getchar() != '\n');
				return;
			}
		} else {
			pil = 0;
		}

		printf("Please Input Metadata Setting:\n");
		printf("	0: Metadata is transferred as part of a separate buffer\n");
		printf("	1: Metadata is transferred as part of an extended data LBA\n");
		if (!scanf("%d", &ms)) {
			printf("Invalid metadata setting\n");
			while (getchar() != '\n');
			return;
		}
	} else {
		ms = 0;
		pi = 0;
		pil = 0;
	}

	printf("Warning: use this utility at your own risk.\n"
	       "This command will format your namespace and all data will be lost.\n"
	       "This command may take several minutes to complete,\n"
	       "so do not interrupt the utility until it completes.\n"
	       "Press 'Y' to continue with the format operation.\n");

	while (getchar() != '\n');
	if (!scanf("%c", &option)) {
		printf("Invalid option\n");
		while (getchar() != '\n');
		return;
	}

	if (option == 'y' || option == 'Y') {
		nvme_manage_format(ctrlr, ns_id, ses, pi, pil, ms, lbaf);
	} else {
		printf("NVMe format abort\n");
	}
}

static void
update_firmware_image(void)
{
	int					rc;
	int					fd = -1;
	int					slot;
	unsigned int				size;
	struct stat				fw_stat;
	char					path[256];
	void					*fw_image;
	struct dev				*ctrlr;
	const struct spdk_nvme_ctrlr_data	*cdata;
	enum spdk_nvme_fw_commit_action		commit_action;
	struct spdk_nvme_status			status;

	ctrlr = get_controller();
	if (ctrlr == NULL) {
		printf("Invalid controller PCI BDF.\n");
		return;
	}

	cdata = ctrlr->cdata;

	if (!cdata->oacs.firmware) {
		printf("Controller does not support firmware download and commit command\n");
		return;
	}

	printf("Please Input The Path Of Firmware Image\n");

	if (get_line(path, sizeof(path), stdin, false) == NULL) {
		printf("Invalid path setting\n");
		while (getchar() != '\n');
		return;
	}

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("Open file failed");
		return;
	}
	rc = fstat(fd, &fw_stat);
	if (rc < 0) {
		printf("Fstat failed\n");
		close(fd);
		return;
	}

	if (fw_stat.st_size % 4) {
		printf("Firmware image size is not multiple of 4\n");
		close(fd);
		return;
	}

	size = fw_stat.st_size;

	fw_image = spdk_dma_zmalloc(size, 4096, NULL);
	if (fw_image == NULL) {
		printf("Allocation error\n");
		close(fd);
		return;
	}

	if (read(fd, fw_image, size) != ((ssize_t)(size))) {
		printf("Read firmware image failed\n");
		close(fd);
		spdk_dma_free(fw_image);
		return;
	}
	close(fd);

	printf("Please Input Slot(0 - 7):\n");
	if (!scanf("%d", &slot)) {
		printf("Invalid Slot\n");
		spdk_dma_free(fw_image);
		while (getchar() != '\n');
		return;
	}

	commit_action = SPDK_NVME_FW_COMMIT_REPLACE_AND_ENABLE_IMG;
	rc = spdk_nvme_ctrlr_update_firmware(ctrlr->ctrlr, fw_image, size, slot, commit_action, &status);
	if (rc == -ENXIO && status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC &&
	    status.sc == SPDK_NVME_SC_FIRMWARE_REQ_CONVENTIONAL_RESET) {
		printf("conventional reset is needed to enable firmware !\n");
	} else if (rc) {
		printf("spdk_nvme_ctrlr_update_firmware failed\n");
	} else {
		printf("spdk_nvme_ctrlr_update_firmware success\n");
	}
	spdk_dma_free(fw_image);
}

static void
opal_dump_info(struct spdk_opal_d0_features_info *feat)
{
	if (feat->tper.hdr.code) {
		printf("\nOpal TPer feature:\n");
		printf("ACKNACK = %s", (feat->tper.acknack ? "Y, " : "N, "));
		printf("ASYNC = %s", (feat->tper.async ? "Y, " : "N, "));
		printf("BufferManagement = %s\n", (feat->tper.buffer_management ? "Y, " : "N, "));
		printf("ComIDManagement = %s", (feat->tper.comid_management ? "Y, " : "N, "));
		printf("Streaming = %s", (feat->tper.streaming ? "Y, " : "N, "));
		printf("Sync = %s\n", (feat->tper.sync ? "Y" : "N"));
		printf("\n");
	}

	if (feat->locking.hdr.code) {
		printf("Opal Locking feature:\n");
		printf("Locked = %s", (feat->locking.locked ? "Y, " : "N, "));
		printf("Locking Enabled = %s", (feat->locking.locking_enabled ? "Y, " : "N, "));
		printf("Locking supported = %s\n", (feat->locking.locking_supported ? "Y" : "N"));

		printf("MBR done = %s", (feat->locking.mbr_done ? "Y, " : "N, "));
		printf("MBR enabled = %s", (feat->locking.mbr_enabled ? "Y, " : "N, "));
		printf("Media encrypt = %s\n", (feat->locking.media_encryption ? "Y" : "N"));
		printf("\n");
	}

	if (feat->geo.hdr.code) {
		printf("Opal Geometry feature:\n");
		printf("Align = %s", (feat->geo.alignment_granularity ? "Y, " : "N, "));
		printf("Logical block size = %d, ", from_be32(&feat->geo.logical_block_size));
		printf("Lowest aligned LBA = %" PRIu64 "\n", from_be64(&feat->geo.lowest_aligned_lba));
		printf("\n");
	}

	if (feat->single_user.hdr.code) {
		printf("Opal Single User Mode feature:\n");
		printf("Any in SUM = %s", (feat->single_user.any ? "Y, " : "N, "));
		printf("All in SUM = %s", (feat->single_user.all ? "Y, " : "N, "));
		printf("Policy: %s Authority,\n", (feat->single_user.policy ? "Admin" : "Users"));
		printf("Number of locking objects = %d\n ", from_be32(&feat->single_user.num_locking_objects));
		printf("\n");
	}

	if (feat->datastore.hdr.code) {
		printf("Opal DataStore feature:\n");
		printf("Table alignment = %d, ", from_be32(&feat->datastore.alignment));
		printf("Max number of tables = %d, ", from_be16(&feat->datastore.max_tables));
		printf("Max size of tables = %d\n", from_be32(&feat->datastore.max_table_size));
		printf("\n");
	}

	if (feat->v100.hdr.code) {
		printf("Opal V100 feature:\n");
		printf("Base comID = %d, ", from_be16(&feat->v100.base_comid));
		printf("Number of comIDs = %d, ", from_be16(&feat->v100.number_comids));
		printf("Range crossing = %s\n", (feat->v100.range_crossing ? "N" : "Y"));
		printf("\n");
	}

	if (feat->v200.hdr.code) {
		printf("Opal V200 feature:\n");
		printf("Base comID = %d, ", from_be16(&feat->v200.base_comid));
		printf("Number of comIDs = %d, ", from_be16(&feat->v200.num_comids));
		printf("Initial PIN = %d,\n", feat->v200.initial_pin);
		printf("Reverted PIN = %d, ", feat->v200.reverted_pin);
		printf("Number of admins = %d, ", from_be16(&feat->v200.num_locking_admin_auth));
		printf("Number of users = %d\n", from_be16(&feat->v200.num_locking_user_auth));
		printf("\n");
	}
}

static void
opal_usage(void)
{
	printf("Opal General Usage:\n");
	printf("\n");
	printf("\t[1: scan device]\n");
	printf("\t[2: init - take ownership and activate locking]\n");
	printf("\t[3: revert tper]\n");
	printf("\t[4: setup locking range]\n");
	printf("\t[5: list locking ranges]\n");
	printf("\t[6: enable user]\n");
	printf("\t[7: set new password]\n");
	printf("\t[8: add user to locking range]\n");
	printf("\t[9: lock/unlock range]\n");
	printf("\t[10: erase locking range]\n");
	printf("\t[0: quit]\n");
}

static void
opal_scan(struct dev *iter)
{
	while (getchar() != '\n');
	if (spdk_nvme_ctrlr_get_flags(iter->ctrlr) & SPDK_NVME_CTRLR_SECURITY_SEND_RECV_SUPPORTED) {
		iter->opal_dev = spdk_opal_dev_construct(iter->ctrlr);
		if (iter->opal_dev == NULL) {
			return;
		}

		printf("\n\nOpal Supported:\n");
		display_controller(iter, CONTROLLER_DISPLAY_SIMPLISTIC);
		opal_dump_info(spdk_opal_get_d0_features_info(iter->opal_dev));
		spdk_opal_dev_destruct(iter->opal_dev);
	} else {
		printf("%04x:%02x:%02x.%02x: NVMe Security Support/Receive Not supported.\n",
		       iter->pci_addr.domain, iter->pci_addr.bus, iter->pci_addr.dev, iter->pci_addr.func);
		printf("%04x:%02x:%02x.%02x: Opal Not Supported\n\n\n",
		       iter->pci_addr.domain, iter->pci_addr.bus, iter->pci_addr.dev, iter->pci_addr.func);
	}
}

static void
opal_init(struct dev *iter)
{
	char new_passwd[SPDK_OPAL_MAX_PASSWORD_SIZE] = {0};
	char *passwd_p;
	int ret;
	int ch;

	if (spdk_nvme_ctrlr_get_flags(iter->ctrlr) & SPDK_NVME_CTRLR_SECURITY_SEND_RECV_SUPPORTED) {
		iter->opal_dev = spdk_opal_dev_construct(iter->ctrlr);
		if (iter->opal_dev == NULL) {
			return;
		}
		printf("Please input the new password for ownership:");
		while ((ch = getchar()) != '\n' && ch != EOF);
		passwd_p = get_line(new_passwd, SPDK_OPAL_MAX_PASSWORD_SIZE, stdin, true);
		printf("\n...\n");
		if (passwd_p) {
			ret = spdk_opal_cmd_take_ownership(iter->opal_dev, passwd_p);
			if (ret) {
				printf("Take ownership failure: %d\n", ret);
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}

			ret = spdk_opal_cmd_activate_locking_sp(iter->opal_dev, passwd_p);
			if (ret) {
				printf("Locking SP activate failure: %d\n", ret);
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}
			printf("...\nOpal Init Success\n");
		} else {
			printf("Input password invalid. Opal Init failure\n");
		}
		spdk_opal_dev_destruct(iter->opal_dev);
	} else {
		printf("%04x:%02x:%02x.%02x: NVMe Security Support/Receive Not supported.\nOpal Not Supported\n\n\n",
		       iter->pci_addr.domain, iter->pci_addr.bus, iter->pci_addr.dev, iter->pci_addr.func);
	}
}

static void
opal_locking_usage(void)
{
	printf("Choose Opal locking state:\n");
	printf("\n");
	printf("\t[1: read write lock]\n");
	printf("\t[2: read only]\n");
	printf("\t[3: read write unlock]\n");
}

static void
opal_setup_lockingrange(struct dev *iter)
{
	char passwd[SPDK_OPAL_MAX_PASSWORD_SIZE] = {0};
	char *passwd_p;
	int ret;
	int ch;
	uint64_t range_start;
	uint64_t range_length;
	int locking_range_id;
	struct spdk_opal_locking_range_info *info;

	if (spdk_nvme_ctrlr_get_flags(iter->ctrlr) & SPDK_NVME_CTRLR_SECURITY_SEND_RECV_SUPPORTED) {
		iter->opal_dev = spdk_opal_dev_construct(iter->ctrlr);
		if (iter->opal_dev == NULL) {
			return;
		}
		printf("Please input the password for setting up locking range:");
		while ((ch = getchar()) != '\n' && ch != EOF);
		passwd_p = get_line(passwd, SPDK_OPAL_MAX_PASSWORD_SIZE, stdin, true);
		printf("\n");
		if (passwd_p) {
			printf("Specify locking range id:\n");
			if (!scanf("%d", &locking_range_id)) {
				printf("Invalid locking range id\n");
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}

			printf("range length:\n");
			if (!scanf("%" SCNu64, &range_length)) {
				printf("Invalid range length\n");
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}

			printf("range start:\n");
			if (!scanf("%" SCNu64, &range_start)) {
				printf("Invalid range start address\n");
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}
			while (getchar() != '\n');

			ret = spdk_opal_cmd_setup_locking_range(iter->opal_dev,
								OPAL_ADMIN1, locking_range_id, range_start, range_length, passwd_p);
			if (ret) {
				printf("Setup locking range failure: %d\n", ret);
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}

			ret = spdk_opal_cmd_get_locking_range_info(iter->opal_dev,
					passwd_p, OPAL_ADMIN1, locking_range_id);
			if (ret) {
				printf("Get locking range info failure: %d\n", ret);
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}
			info = spdk_opal_get_locking_range_info(iter->opal_dev, locking_range_id);

			printf("\nlocking range ID: %d\n", info->locking_range_id);
			printf("range start: %" PRIu64 "\n", info->range_start);
			printf("range length: %" PRIu64 "\n", info->range_length);
			printf("read lock enabled: %d\n", info->read_lock_enabled);
			printf("write lock enabled: %d\n", info->write_lock_enabled);
			printf("read locked: %d\n", info->read_locked);
			printf("write locked: %d\n", info->write_locked);

			printf("...\n...\nOpal setup locking range success\n");
		} else {
			printf("Input password invalid. Opal setup locking range failure\n");
		}
		spdk_opal_dev_destruct(iter->opal_dev);
	} else {
		printf("%04x:%02x:%02x.%02x: NVMe Security Support/Receive Not supported.\nOpal Not Supported\n\n\n",
		       iter->pci_addr.domain, iter->pci_addr.bus, iter->pci_addr.dev, iter->pci_addr.func);
	}
}

static void
opal_list_locking_ranges(struct dev *iter)
{
	char passwd[SPDK_OPAL_MAX_PASSWORD_SIZE] = {0};
	char *passwd_p;
	int ret;
	int ch;
	int max_ranges;
	int i;
	struct spdk_opal_locking_range_info *info;

	if (spdk_nvme_ctrlr_get_flags(iter->ctrlr) & SPDK_NVME_CTRLR_SECURITY_SEND_RECV_SUPPORTED) {
		iter->opal_dev = spdk_opal_dev_construct(iter->ctrlr);
		if (iter->opal_dev == NULL) {
			return;
		}
		printf("Please input password:");
		while ((ch = getchar()) != '\n' && ch != EOF);
		passwd_p = get_line(passwd, SPDK_OPAL_MAX_PASSWORD_SIZE, stdin, true);
		printf("\n");
		if (passwd_p) {
			ret = spdk_opal_cmd_get_max_ranges(iter->opal_dev, passwd_p);
			if (ret <= 0) {
				printf("get max ranges failure: %d\n", ret);
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}

			max_ranges = ret;
			for (i = 0; i < max_ranges; i++) {
				ret = spdk_opal_cmd_get_locking_range_info(iter->opal_dev,
						passwd_p, OPAL_ADMIN1, i);
				if (ret) {
					printf("Get locking range info failure: %d\n", ret);
					spdk_opal_dev_destruct(iter->opal_dev);
					return;
				}
				info = spdk_opal_get_locking_range_info(iter->opal_dev, i);
				if (info == NULL) {
					continue;
				}

				printf("===============================================\n");
				printf("locking range ID: %d\t", info->locking_range_id);
				if (i == 0) { printf("(Global Range)"); }
				printf("\n===============================================\n");
				printf("range start: %" PRIu64 "\t", info->range_start);
				printf("range length: %" PRIu64 "\n", info->range_length);
				printf("read lock enabled: %d\t", info->read_lock_enabled);
				printf("write lock enabled: %d\t", info->write_lock_enabled);
				printf("read locked: %d\t", info->read_locked);
				printf("write locked: %d\n", info->write_locked);
				printf("\n");
			}
		} else {
			printf("Input password invalid. List locking ranges failure\n");
		}
		spdk_opal_dev_destruct(iter->opal_dev);
	} else {
		printf("%04x:%02x:%02x.%02x: NVMe Security Support/Receive Not supported.\nOpal Not Supported\n\n\n",
		       iter->pci_addr.domain, iter->pci_addr.bus, iter->pci_addr.dev, iter->pci_addr.func);
	}
}

static void
opal_new_user_enable(struct dev *iter)
{
	int user_id;
	char passwd[SPDK_OPAL_MAX_PASSWORD_SIZE] = {0};
	char *passwd_p;
	char user_pw[SPDK_OPAL_MAX_PASSWORD_SIZE] = {0};
	char *user_pw_p;
	int ret;
	int ch;

	if (spdk_nvme_ctrlr_get_flags(iter->ctrlr) & SPDK_NVME_CTRLR_SECURITY_SEND_RECV_SUPPORTED) {
		iter->opal_dev = spdk_opal_dev_construct(iter->ctrlr);
		if (iter->opal_dev == NULL) {
			return;
		}
		printf("Please input admin password:");
		while ((ch = getchar()) != '\n' && ch != EOF);
		passwd_p = get_line(passwd, SPDK_OPAL_MAX_PASSWORD_SIZE, stdin, true);
		printf("\n");
		if (passwd_p) {
			printf("which user to enable: ");
			if (!scanf("%d", &user_id)) {
				printf("Invalid user id\n");
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}

			ret = spdk_opal_cmd_enable_user(iter->opal_dev, user_id, passwd_p);
			if (ret) {
				printf("Enable user failure error code: %d\n", ret);
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}
			printf("Please set a new password for this user:");
			while ((ch = getchar()) != '\n' && ch != EOF);
			user_pw_p = get_line(user_pw, SPDK_OPAL_MAX_PASSWORD_SIZE, stdin, true);
			if (user_pw_p == NULL) {
				printf("Input password invalid. Enable user failure\n");
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}

			ret = spdk_opal_cmd_set_new_passwd(iter->opal_dev, user_id, user_pw_p, passwd_p, true);
			if (ret) {
				printf("Set new password failure error code: %d\n", ret);
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}

			printf("\n...\n...\nEnable User Success\n");
		} else {
			printf("Input password invalid. Enable user failure\n");
		}
		spdk_opal_dev_destruct(iter->opal_dev);
	} else {
		printf("%04x:%02x:%02x.%02x: NVMe Security Support/Receive Not supported.\nOpal Not Supported\n\n\n",
		       iter->pci_addr.domain, iter->pci_addr.bus, iter->pci_addr.dev, iter->pci_addr.func);
	}
}

static void
opal_change_password(struct dev *iter)
{
	int user_id;
	char old_passwd[SPDK_OPAL_MAX_PASSWORD_SIZE] = {0};
	char *old_passwd_p;
	char new_passwd[SPDK_OPAL_MAX_PASSWORD_SIZE] = {0};
	char *new_passwd_p;
	int ret;
	int ch;

	if (spdk_nvme_ctrlr_get_flags(iter->ctrlr) & SPDK_NVME_CTRLR_SECURITY_SEND_RECV_SUPPORTED) {
		iter->opal_dev = spdk_opal_dev_construct(iter->ctrlr);
		if (iter->opal_dev == NULL) {
			return;
		}
		printf("user id: ");
		if (!scanf("%d", &user_id)) {
			printf("Invalid user id\n");
			spdk_opal_dev_destruct(iter->opal_dev);
			return;
		}
		printf("Password:");
		while ((ch = getchar()) != '\n' && ch != EOF);
		old_passwd_p = get_line(old_passwd, SPDK_OPAL_MAX_PASSWORD_SIZE, stdin, true);
		printf("\n");
		if (old_passwd_p) {
			printf("Please input new password:\n");
			new_passwd_p = get_line(new_passwd, SPDK_OPAL_MAX_PASSWORD_SIZE, stdin, true);
			printf("\n");
			if (new_passwd_p == NULL) {
				printf("Input password invalid. Change password failure\n");
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}

			ret = spdk_opal_cmd_set_new_passwd(iter->opal_dev, user_id, new_passwd_p, old_passwd_p, false);
			if (ret) {
				printf("Set new password failure error code: %d\n", ret);
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}

			printf("...\n...\nChange password Success\n");
		} else {
			printf("Input password invalid. Change password failure\n");
		}
		spdk_opal_dev_destruct(iter->opal_dev);
	} else {
		printf("%04x:%02x:%02x.%02x: NVMe Security Support/Receive Not supported.\nOpal Not Supported\n\n\n",
		       iter->pci_addr.domain, iter->pci_addr.bus, iter->pci_addr.dev, iter->pci_addr.func);
	}
}

static void
opal_add_user_to_locking_range(struct dev *iter)
{
	int locking_range_id, user_id;
	char passwd[SPDK_OPAL_MAX_PASSWORD_SIZE] = {0};
	char *passwd_p;
	int ret;
	int ch;

	if (spdk_nvme_ctrlr_get_flags(iter->ctrlr) & SPDK_NVME_CTRLR_SECURITY_SEND_RECV_SUPPORTED) {
		iter->opal_dev = spdk_opal_dev_construct(iter->ctrlr);
		if (iter->opal_dev == NULL) {
			return;
		}
		printf("Please input admin password:");
		while ((ch = getchar()) != '\n' && ch != EOF);
		passwd_p = get_line(passwd, SPDK_OPAL_MAX_PASSWORD_SIZE, stdin, true);
		printf("\n");
		if (passwd_p) {
			printf("Specify locking range id:\n");
			if (!scanf("%d", &locking_range_id)) {
				printf("Invalid locking range id\n");
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}

			printf("which user to enable:\n");
			if (!scanf("%d", &user_id)) {
				printf("Invalid user id\n");
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}
			while (getchar() != '\n');

			ret = spdk_opal_cmd_add_user_to_locking_range(iter->opal_dev, user_id, locking_range_id,
					OPAL_READONLY, passwd_p);
			ret += spdk_opal_cmd_add_user_to_locking_range(iter->opal_dev, user_id, locking_range_id,
					OPAL_READWRITE, passwd_p);
			if (ret) {
				printf("Add user to locking range error: %d\n", ret);
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}

			printf("...\n...\nAdd user to locking range Success\n");
		} else {
			printf("Input password invalid. Add user to locking range failure\n");
		}
		spdk_opal_dev_destruct(iter->opal_dev);
	} else {
		printf("%04x:%02x:%02x.%02x: NVMe Security Support/Receive Not supported.\nOpal Not Supported\n\n\n",
		       iter->pci_addr.domain, iter->pci_addr.bus, iter->pci_addr.dev, iter->pci_addr.func);
	}
}

static void
opal_user_lock_unlock_range(struct dev *iter)
{
	char passwd[SPDK_OPAL_MAX_PASSWORD_SIZE] = {0};
	char *passwd_p;
	int ch;
	int ret;
	int user_id;
	int locking_range_id;
	int state;
	enum spdk_opal_lock_state state_flag;

	if (spdk_nvme_ctrlr_get_flags(iter->ctrlr) & SPDK_NVME_CTRLR_SECURITY_SEND_RECV_SUPPORTED) {
		iter->opal_dev = spdk_opal_dev_construct(iter->ctrlr);
		if (iter->opal_dev == NULL) {
			return;
		}
		printf("User id: ");
		if (!scanf("%d", &user_id)) {
			printf("Invalid user id\n");
			spdk_opal_dev_destruct(iter->opal_dev);
			return;
		}

		printf("Please input password:");
		while ((ch = getchar()) != '\n' && ch != EOF);
		passwd_p = get_line(passwd, SPDK_OPAL_MAX_PASSWORD_SIZE, stdin, true);
		printf("\n");
		if (passwd_p) {
			printf("Specify locking range id:\n");
			if (!scanf("%d", &locking_range_id)) {
				printf("Invalid locking range id\n");
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}

			opal_locking_usage();
			if (!scanf("%d", &state)) {
				printf("Invalid option\n");
			}
			switch (state) {
			case 1:
				state_flag = OPAL_RWLOCK;
				break;
			case 2:
				state_flag = OPAL_READONLY;
				break;
			case 3:
				state_flag = OPAL_READWRITE;
				break;
			default:
				printf("Invalid options\n");
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}
			while (getchar() != '\n');

			ret = spdk_opal_cmd_lock_unlock(iter->opal_dev, user_id, state_flag,
							locking_range_id, passwd_p);
			if (ret) {
				printf("lock/unlock range failure: %d\n", ret);
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}
			printf("...\n...\nLock/unlock range Success\n");
		} else {
			printf("Input password invalid. lock/unlock range failure\n");
		}
		spdk_opal_dev_destruct(iter->opal_dev);
	} else {
		printf("%04x:%02x:%02x.%02x: NVMe Security Support/Receive Not supported.\nOpal Not Supported\n\n\n",
		       iter->pci_addr.domain, iter->pci_addr.bus, iter->pci_addr.dev, iter->pci_addr.func);
	}
}

static void
opal_revert_tper(struct dev *iter)
{
	char passwd[SPDK_OPAL_MAX_PASSWORD_SIZE] = {0};
	char *passwd_p;
	int ret;
	int ch;

	if (spdk_nvme_ctrlr_get_flags(iter->ctrlr) & SPDK_NVME_CTRLR_SECURITY_SEND_RECV_SUPPORTED) {
		iter->opal_dev = spdk_opal_dev_construct(iter->ctrlr);
		if (iter->opal_dev == NULL) {
			return;
		}
		printf("Please be noted this operation will erase ALL DATA on this drive\n");
		printf("Please don't ternminate this excecution. Otherwise undefined error may occur\n");
		printf("Please input password for revert TPer:");
		while ((ch = getchar()) != '\n' && ch != EOF);
		passwd_p = get_line(passwd, SPDK_OPAL_MAX_PASSWORD_SIZE, stdin, true);
		printf("\n...\n");
		if (passwd_p) {
			ret = spdk_opal_cmd_revert_tper(iter->opal_dev, passwd_p);
			if (ret) {
				printf("Revert TPer failure: %d\n", ret);
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}
			printf("...\nRevert TPer Success\n");
		} else {
			printf("Input password invalid. Revert TPer failure\n");
		}
		spdk_opal_dev_destruct(iter->opal_dev);
	} else {
		printf("%04x:%02x:%02x.%02x: NVMe Security Support/Receive Not supported.\nOpal Not Supported\n\n\n",
		       iter->pci_addr.domain, iter->pci_addr.bus, iter->pci_addr.dev, iter->pci_addr.func);
	}
}

static void
opal_erase_locking_range(struct dev *iter)
{
	char passwd[SPDK_OPAL_MAX_PASSWORD_SIZE] = {0};
	char *passwd_p;
	int ret;
	int ch;
	int locking_range_id;

	if (spdk_nvme_ctrlr_get_flags(iter->ctrlr) & SPDK_NVME_CTRLR_SECURITY_SEND_RECV_SUPPORTED) {
		iter->opal_dev = spdk_opal_dev_construct(iter->ctrlr);
		if (iter->opal_dev == NULL) {
			return;
		}
		printf("Please be noted this operation will erase ALL DATA on this range\n");
		printf("Please input password for erase locking range:");
		while ((ch = getchar()) != '\n' && ch != EOF);
		passwd_p = get_line(passwd, SPDK_OPAL_MAX_PASSWORD_SIZE, stdin, true);
		if (passwd_p) {
			printf("\nSpecify locking range id:\n");
			if (!scanf("%d", &locking_range_id)) {
				printf("Invalid locking range id\n");
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}
			printf("\n...\n");
			ret = spdk_opal_cmd_secure_erase_locking_range(iter->opal_dev, OPAL_ADMIN1, locking_range_id,
					passwd_p);
			if (ret) {
				printf("Erase locking range failure: %d\n", ret);
				spdk_opal_dev_destruct(iter->opal_dev);
				return;
			}
			printf("...\nErase locking range Success\n");
		} else {
			printf("Input password invalid. Erase locking range failure\n");
		}
		spdk_opal_dev_destruct(iter->opal_dev);
	} else {
		printf("%04x:%02x:%02x.%02x: NVMe Security Support/Receive Not supported.\nOpal Not Supported\n\n\n",
		       iter->pci_addr.domain, iter->pci_addr.bus, iter->pci_addr.dev, iter->pci_addr.func);
	}
}

static void
test_opal(void)
{
	int exit_flag = false;
	struct dev *ctrlr;

	ctrlr = get_controller();
	if (ctrlr == NULL) {
		printf("Invalid controller PCI Address.\n");
		return;
	}

	opal_usage();
	while (!exit_flag) {
		int cmd;
		if (!scanf("%d", &cmd)) {
			printf("Invalid Command: command must be number 0-9\n");
			while (getchar() != '\n');
			opal_usage();
			continue;
		}

		switch (cmd) {
		case 0:
			exit_flag = true;
			continue;
		case 1:
			opal_scan(ctrlr);
			break;
		case 2:
			opal_init(ctrlr);   /* Take ownership, Activate Locking SP */
			break;
		case 3:
			opal_revert_tper(ctrlr);
			break;
		case 4:
			opal_setup_lockingrange(ctrlr);
			break;
		case 5:
			opal_list_locking_ranges(ctrlr);
			break;
		case 6:
			opal_new_user_enable(ctrlr);
			break;
		case 7:
			opal_change_password(ctrlr);
			break;
		case 8:
			opal_add_user_to_locking_range(ctrlr);
			break;
		case 9:
			opal_user_lock_unlock_range(ctrlr);
			break;
		case 10:
			opal_erase_locking_range(ctrlr);
			break;

		default:
			printf("Invalid option\n");
		}

		printf("\npress Enter to display Opal cmd menu ...\n");
		while (getchar() != '\n');
		opal_usage();
	}
}

static void
args_usage(const char *program_name)
{
	printf("%s [options]", program_name);
	printf("\n");
	printf("options:\n");
	printf(" -i         shared memory group ID\n");
}

static int
parse_args(int argc, char **argv)
{
	int op;

	while ((op = getopt(argc, argv, "i:")) != -1) {
		switch (op) {
		case 'i':
			g_shm_id = spdk_strtol(optarg, 10);
			if (g_shm_id < 0) {
				fprintf(stderr, "Invalid shared memory ID\n");
				return g_shm_id;
			}
			break;
		default:
			args_usage(argv[0]);
			return 1;
		}
	}

	return 0;
}

int main(int argc, char **argv)
{
	int			rc;
	struct spdk_env_opts	opts;
	struct dev		*dev;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	spdk_env_opts_init(&opts);
	opts.name = "nvme_manage";
	opts.core_mask = "0x1";
	opts.shm_id = g_shm_id;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	if (spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	qsort(devs, num_devs, sizeof(devs[0]), cmp_devs);

	usage();

	while (1) {
		int cmd;
		bool exit_flag = false;

		if (!scanf("%d", &cmd)) {
			printf("Invalid Command: command must be number 1-8\n");
			while (getchar() != '\n');
			usage();
			continue;
		}
		switch (cmd) {
		case 1:
			display_controller_list();
			break;
		case 2:
			add_ns();
			break;
		case 3:
			delete_ns();
			break;
		case 4:
			attach_and_detach_ns(SPDK_NVME_NS_CTRLR_ATTACH);
			break;
		case 5:
			attach_and_detach_ns(SPDK_NVME_NS_CTRLR_DETACH);
			break;
		case 6:
			format_nvm();
			break;
		case 7:
			update_firmware_image();
			break;
		case 8:
			test_opal();
			break;
		case 9:
			exit_flag = true;
			break;
		default:
			printf("Invalid Command\n");
			break;
		}

		if (exit_flag) {
			break;
		}

		while (getchar() != '\n');
		printf("press Enter to display cmd menu ...\n");
		while (getchar() != '\n');
		usage();
	}

	printf("Cleaning up...\n");

	foreach_dev(dev) {
		spdk_nvme_detach_async(dev->ctrlr, &detach_ctx);
	}

	while (detach_ctx && spdk_nvme_detach_poll_async(detach_ctx) == -EAGAIN) {
		;
	}

	return 0;
}
