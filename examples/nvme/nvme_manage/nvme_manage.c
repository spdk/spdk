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

#include <stdbool.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdlib.h>

#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_lcore.h>

#include "spdk/nvme.h"
#include "spdk/pci.h"

struct rte_mempool *request_mempool;

#define MAX_DEVS 64

struct dev {
	struct spdk_pci_device			*pci_dev;
	struct spdk_nvme_ctrlr 			*ctrlr;
	const struct spdk_nvme_ctrlr_data	*cdata;
};

static struct dev devs[MAX_DEVS];
static int num_devs = 0;

#define foreach_dev(iter) \
	for (iter = devs; iter - devs < num_devs; iter++)

static uint64_t
get_pci_addr(struct spdk_pci_device *pci_dev)
{
	uint64_t cmp;

	cmp = (uint64_t)spdk_pci_device_get_domain(pci_dev) << 24;
	cmp |= (uint64_t)spdk_pci_device_get_bus(pci_dev) << 16;
	cmp |= (uint64_t)spdk_pci_device_get_dev(pci_dev) << 8;
	cmp |= (uint64_t)spdk_pci_device_get_func(pci_dev);

	return cmp;
}

static int
cmp_devs(const void *ap, const void *bp)
{
	const struct dev *a = ap, *b = bp;
	uint64_t cmp_a = get_pci_addr(a->pci_dev);
	uint64_t cmp_b = get_pci_addr(b->pci_dev);

	if (cmp_a < cmp_b) {
		return -1;
	} else if (cmp_a > cmp_b) {
		return 1;
	} else {
		return 0;
	}
}

static struct dev *
get_controller(void)
{
	int 					bus;
	int 					devid;
	int 					function;
	struct dev				*iter;
	const struct spdk_nvme_ctrlr_data	*cdata;

	printf("Please Input Bus ID: \n");
	if (!scanf("%d", &bus)) {
		printf("Invalid Bus ID\n");
		while (getchar() != '\n');
		return NULL;
	}
	printf("Please Input Dev ID: \n");
	if (!scanf("%d", &devid)) {
		printf("Invalid Dev ID\n");
		while (getchar() != '\n');
		return NULL;
	}
	printf("Please Input Function ID: \n");
	if (!scanf("%d", &function)) {
		printf("Invalid Function ID\n");
		while (getchar() != '\n');
		return NULL;
	}

	foreach_dev(iter) {
		if (spdk_pci_device_get_bus(iter->pci_dev) == bus &&
		    spdk_pci_device_get_dev(iter->pci_dev) == devid &&
		    spdk_pci_device_get_func(iter->pci_dev) == function) {
			cdata = spdk_nvme_ctrlr_get_data(iter->ctrlr);
			iter->cdata = cdata;
			return iter;
		}
	}
	return NULL;
}

static void
ns_attach(struct dev *device, int attachment_op, int ctrlr_id, int ns_id)
{
	int ret = 0;
	struct spdk_nvme_ctrlr_list *ctrlr_list;

	ctrlr_list = rte_zmalloc("nvme controller list", sizeof(struct spdk_nvme_ctrlr_list),
				 4096);
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

	rte_free(ctrlr_list);
}

static void
ns_manage_add(struct dev *device, uint64_t ns_size, uint64_t ns_capacity, int ns_lbasize)
{
	int ret = 0;
	struct spdk_nvme_ns_data *ndata;

	ndata = rte_zmalloc("nvme namespace data", sizeof(struct spdk_nvme_ns_data), 4096);
	if (ndata == NULL) {
		printf("Allocation error (namespace data)\n");
		exit(1);
	}

	ndata->nsze = ns_size;
	ndata->ncap = ns_capacity;
	ndata->flbas.format = ns_lbasize;
	ret = spdk_nvme_ctrlr_create_ns(device->ctrlr, ndata);
	if (ret) {
		fprintf(stdout, "ns manage: Failed\n");
	}

	rte_free(ndata);
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

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *dev)
{
	if (spdk_pci_device_has_non_uio_driver(dev)) {
		fprintf(stderr, "non-uio kernel driver attached to NVMe\n");
		fprintf(stderr, " controller at PCI address %04x:%02x:%02x.%02x\n",
			spdk_pci_device_get_domain(dev),
			spdk_pci_device_get_bus(dev),
			spdk_pci_device_get_dev(dev),
			spdk_pci_device_get_func(dev));
		fprintf(stderr, " skipping...\n");
		return false;
	}

	return true;
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *pci_dev, struct spdk_nvme_ctrlr *ctrlr)
{
	struct dev *dev;

	/* add to dev list */
	dev = &devs[num_devs++];
	dev->pci_dev = pci_dev;
	dev->ctrlr = ctrlr;
}

static const char *ealargs[] = {
	"nvme_manage",
	"-c 0x1",
	"-n 4",
};

static void usage(void)
{
	printf("NVMe Management Options");
	printf("\n");
	printf("\t[1: list controllers]\n");
	printf("\t[2: create namespace]\n");
	printf("\t[3: delete namespace]\n");
	printf("\t[4: attach namespace to controller]\n");
	printf("\t[5: detach namespace from controller]\n");
	printf("\t[6: quit]\n");
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

	printf("Number of LBA Formats:       %d\n", nsdata->nlbaf + 1);
	printf("Current LBA Format:          LBA Format #%02d\n",
	       nsdata->flbas.format);
	for (i = 0; i <= nsdata->nlbaf; i++)
		printf("LBA Format #%02d: Data Size: %5d  Metadata Size: %5d\n",
		       i, 1 << nsdata->lbaf[i].lbads, nsdata->lbaf[i].ms);
	printf("\n");
}

static void
display_controller(struct dev *dev)
{
	const struct spdk_nvme_ctrlr_data	*cdata;
	uint8_t					str[128];
	uint32_t				i;

	cdata = spdk_nvme_ctrlr_get_data(dev->ctrlr);

	printf("=====================================================\n");
	printf("NVMe Controller at PCI bus %d, device %d, function %d\n",
	       spdk_pci_device_get_bus(dev->pci_dev), spdk_pci_device_get_dev(dev->pci_dev),
	       spdk_pci_device_get_func(dev->pci_dev));
	printf("=====================================================\n");
	printf("Controller Capabilities/Features\n");
	printf("Controller ID:		%d\n", cdata->cntlid);
	snprintf(str, sizeof(cdata->sn) + 1, "%s", cdata->sn);
	printf("Serial Number:		%s\n", str);
	printf("\n");
	printf("Admin Command Set Attributes\n");
	printf("============================\n");
	printf("Namespace Manage And Attach:		%s\n",
	       cdata->oacs.ns_manage ? "Supported" : "Not Supported");
	printf("\n");
	printf("Namespace Attributes\n");
	printf("============================\n");
	for (i = 1; i <= spdk_nvme_ctrlr_get_num_ns(dev->ctrlr); i++) {
		display_namespace(spdk_nvme_ctrlr_get_ns(dev->ctrlr, i));
	}
}

static void
display_controller_list(void)
{
	struct dev			*iter;

	foreach_dev(iter) {
		display_controller(iter);
	}
}

static void
attach_and_detach_ns(int attachment_op)
{
	int		ns_id;
	struct dev	*ctrlr;

	ctrlr = get_controller();
	if (ctrlr == NULL) {
		printf("Invalid controller PCI BDF.\n");
		return;
	}

	if (!ctrlr->cdata->oacs.ns_manage) {
		printf("Controller does not support ns management\n");
		return;
	}

	printf("Please Input Namespace ID: \n");
	if (!scanf("%d", &ns_id)) {
		printf("Invalid Namespace ID\n");
		while (getchar() != '\n');
		return;
	}

	ns_attach(ctrlr, attachment_op, ctrlr->cdata->cntlid, ns_id);
}

static void
add_ns(void)
{
	uint64_t	ns_size;
	uint64_t	ns_capacity;
	int		ns_lbasize;
	struct dev	*ctrlr;

	ctrlr = get_controller();
	if (ctrlr == NULL) {
		printf("Invalid controller PCI BDF.\n");
		return;
	}

	if (!ctrlr->cdata->oacs.ns_manage) {
		printf("Controller does not support ns management\n");
		return;
	}

	printf("Please Input Namespace Size (in LBAs): \n");
	if (!scanf("%ld", &ns_size)) {
		printf("Invalid Namespace Size\n");
		while (getchar() != '\n');
		return;
	}

	printf("Please Input Namespace Capacity (in LBAs): \n");
	if (!scanf("%ld", &ns_capacity)) {
		printf("Invalid Namespace Capacity\n");
		while (getchar() != '\n');
		return;
	}

	printf("Please Input LBA Format Number (0 - 15): \n");
	if (!scanf("%d", &ns_lbasize)) {
		printf("Invalid LBA format size\n");
		while (getchar() != '\n');
		return;
	}

	ns_manage_add(ctrlr, ns_size, ns_capacity, ns_lbasize);
}

static void
delete_ns(void)
{
	int 					ns_id;
	struct dev				*ctrlr;

	ctrlr = get_controller();
	if (ctrlr == NULL) {
		printf("Invalid controller PCI BDF.\n");
		return;
	}

	if (!ctrlr->cdata->oacs.ns_manage) {
		printf("Controller does not support ns management\n");
		return;
	}

	printf("Please Input Namespace ID: \n");
	if (!scanf("%d", &ns_id)) {
		printf("Invalid Namespace ID\n");
		while (getchar() != '\n');
		return;
	}

	ns_manage_delete(ctrlr, ns_id);
}

int main(int argc, char **argv)
{
	int				rc, i;

	rc = rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]),
			  (char **)(void *)(uintptr_t)ealargs);

	if (rc < 0) {
		fprintf(stderr, "could not initialize dpdk\n");
		exit(1);
	}

	request_mempool = rte_mempool_create("nvme_request", 8192,
					     spdk_nvme_request_size(), 128, 0,
					     NULL, NULL, NULL, NULL,
					     SOCKET_ID_ANY, 0);

	if (request_mempool == NULL) {
		fprintf(stderr, "could not initialize request mempool\n");
		exit(1);
	}

	if (spdk_nvme_probe(NULL, probe_cb, attach_cb) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	qsort(devs, num_devs, sizeof(devs[0]), cmp_devs);

	if (num_devs) {
		rc = spdk_nvme_register_io_thread();
		if (rc != 0)
			return rc;
	}

	usage();

	while (1) {
		int cmd;
		bool exit_flag = false;

		if (!scanf("%d", &cmd)) {
			printf("Invalid Command\n");
			while (getchar() != '\n');
			return 0;
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
			exit_flag = true;
			break;
		default:
			printf("Invalid Command\n");
			break;
		}

		if (exit_flag)
			break;

		while (getchar() != '\n');
		printf("press Enter to display cmd menu ...\n");
		while (getchar() != '\n');
		usage();
	}

	printf("Cleaning up...\n");

	for (i = 0; i < num_devs; i++) {
		struct dev *dev = &devs[i];
		spdk_nvme_detach(dev->ctrlr);
	}

	if (num_devs)
		spdk_nvme_unregister_io_thread();

	return rc;
}
