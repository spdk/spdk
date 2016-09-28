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
#include <inttypes.h>
#include <string.h>

#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_lcore.h>

#include "spdk/nvme.h"
#include "spdk/pci.h"

struct rte_mempool *request_mempool;

#define MAX_DEVS 64

#define MAX_IOVS 128

#define DATA_PATTERN 0x5A

#define BASE_LBA_START 0x100000

struct dev {
	struct spdk_nvme_ctrlr			*ctrlr;
	char 					name[100];
};

static struct dev devs[MAX_DEVS];
static int num_devs = 0;

#define foreach_dev(iter) \
	for (iter = devs; iter - devs < num_devs; iter++)

static int io_complete_flag = 0;

struct sgl_element {
	void *base;
	uint64_t phys_addr;
	size_t offset;
	size_t len;
};

struct io_request {
	uint32_t current_iov_index;
	uint32_t current_iov_bytes_left;
	struct sgl_element iovs[MAX_IOVS];
	uint32_t nseg;
};

static void nvme_request_reset_sgl(void *cb_arg, uint32_t sgl_offset)
{
	uint32_t i;
	uint32_t offset = 0;
	struct sgl_element *iov;
	struct io_request *req = (struct io_request *)cb_arg;

	for (i = 0; i < req->nseg; i++) {
		iov = &req->iovs[i];
		offset += iov->len;
		if (offset > sgl_offset)
			break;
	}
	req->current_iov_index = i;
	req->current_iov_bytes_left = offset - sgl_offset;
	return;
}

static int nvme_request_next_sge(void *cb_arg, uint64_t *address, uint32_t *length)
{
	struct io_request *req = (struct io_request *)cb_arg;
	struct sgl_element *iov;

	if (req->current_iov_index >= req->nseg) {
		*length = 0;
		*address = 0;
		return 0;
	}

	iov = &req->iovs[req->current_iov_index];

	if (req->current_iov_bytes_left) {
		*address = iov->phys_addr + iov->len - req->current_iov_bytes_left;
		*length = req->current_iov_bytes_left;
		req->current_iov_bytes_left = 0;
	} else {
		*address = iov->phys_addr;
		*length = iov->len;
	}

	req->current_iov_index++;

	return 0;
}

static void
io_complete(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl))
		io_complete_flag = 2;
	else
		io_complete_flag = 1;
}

static void build_io_request_0(struct io_request *req)
{
	req->nseg = 1;

	req->iovs[0].base = rte_zmalloc(NULL, 0x800, 4);
	req->iovs[0].len = 0x800;
}

static void build_io_request_1(struct io_request *req)
{
	req->nseg = 1;

	/* 512B for 1st sge */
	req->iovs[0].base = rte_zmalloc(NULL, 0x200, 0x200);
	req->iovs[0].len = 0x200;
}

static void build_io_request_2(struct io_request *req)
{
	req->nseg = 1;

	/* 256KB for 1st sge */
	req->iovs[0].base = rte_zmalloc(NULL, 0x40000, 0x1000);
	req->iovs[0].len = 0x40000;
}

static void build_io_request_3(struct io_request *req)
{
	req->nseg = 3;

	/* 2KB for 1st sge, make sure the iov address start at 0x800 boundary,
	 *  and end with 0x1000 boundary */
	req->iovs[0].base = rte_zmalloc(NULL, 0x1000, 0x1000);
	req->iovs[0].offset = 0x800;
	req->iovs[0].len = 0x800;

	/* 4KB for 2th sge */
	req->iovs[1].base = rte_zmalloc(NULL, 0x1000, 0x1000);
	req->iovs[1].len = 0x1000;

	/* 12KB for 3th sge */
	req->iovs[2].base = rte_zmalloc(NULL, 0x3000, 0x1000);
	req->iovs[2].len = 0x3000;
}

static void build_io_request_4(struct io_request *req)
{
	uint32_t i;

	req->nseg = 32;

	/* 4KB for 1st sge */
	req->iovs[0].base = rte_zmalloc(NULL, 0x1000, 0x1000);
	req->iovs[0].len = 0x1000;

	/* 8KB for the rest 31 sge */
	for (i = 1; i < req->nseg; i++) {
		req->iovs[i].base = rte_zmalloc(NULL, 0x2000, 0x1000);
		req->iovs[i].len = 0x2000;
	}
}

static void build_io_request_5(struct io_request *req)
{
	req->nseg = 1;

	/* 8KB for 1st sge */
	req->iovs[0].base = rte_zmalloc(NULL, 0x2000, 0x1000);
	req->iovs[0].len = 0x2000;
}

static void build_io_request_6(struct io_request *req)
{
	req->nseg = 2;

	/* 4KB for 1st sge */
	req->iovs[0].base = rte_zmalloc(NULL, 0x1000, 0x1000);
	req->iovs[0].len = 0x1000;

	/* 4KB for 2st sge */
	req->iovs[1].base = rte_zmalloc(NULL, 0x1000, 0x1000);
	req->iovs[1].len = 0x1000;
}

typedef void (*nvme_build_io_req_fn_t)(struct io_request *req);

static void
free_req(struct io_request *req)
{
	uint32_t i;

	if (req == NULL) {
		return;
	}

	for (i = 0; i < req->nseg; i++) {
		rte_free(req->iovs[i].base);
	}

	rte_free(req);
}

static int
writev_readv_tests(struct dev *dev, nvme_build_io_req_fn_t build_io_fn, const char *test_name)
{
	int rc = 0;
	uint32_t len, lba_count;
	uint32_t i, j, nseg;
	char *buf;

	struct io_request *req;
	struct spdk_nvme_ns *ns;
	struct spdk_nvme_qpair *qpair;
	const struct spdk_nvme_ns_data *nsdata;

	ns = spdk_nvme_ctrlr_get_ns(dev->ctrlr, 1);
	if (!ns) {
		fprintf(stderr, "Null namespace\n");
		return 0;
	}
	nsdata = spdk_nvme_ns_get_data(ns, 0);
	if (!nsdata || !spdk_nvme_ns_get_sector_size(ns)) {
		fprintf(stderr, "Empty nsdata or wrong sector size\n");
		return 0;
	}

	req = rte_zmalloc(NULL, sizeof(*req), 0);
	if (!req) {
		fprintf(stderr, "Allocate request failed\n");
		return 0;
	}

	/* IO parameters setting */
	build_io_fn(req);

	len = 0;
	for (i = 0; i < req->nseg; i++) {
		struct sgl_element *sge = &req->iovs[i];

		sge->phys_addr = rte_malloc_virt2phy(sge->base) + sge->offset;
		len += sge->len;
	}

	lba_count = len / spdk_nvme_ns_get_sector_size(ns);
	if (!lba_count || (BASE_LBA_START + lba_count > (uint32_t)nsdata->nsze)) {
		fprintf(stderr, "%s: %s Invalid IO length parameter\n", dev->name, test_name);
		free_req(req);
		return 0;
	}

	qpair = spdk_nvme_ctrlr_alloc_io_qpair(dev->ctrlr, 0);
	if (!qpair) {
		free_req(req);
		return -1;
	}

	nseg = req->nseg;
	for (i = 0; i < nseg; i++) {
		memset(req->iovs[i].base + req->iovs[i].offset, DATA_PATTERN, req->iovs[i].len);
	}

	rc = spdk_nvme_ns_cmd_writev(ns, qpair, BASE_LBA_START, lba_count,
				     io_complete, req, 0,
				     nvme_request_reset_sgl,
				     nvme_request_next_sge);

	if (rc != 0) {
		fprintf(stderr, "%s: %s writev failed\n", dev->name, test_name);
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		free_req(req);
		return -1;
	}

	io_complete_flag = 0;

	while (!io_complete_flag)
		spdk_nvme_qpair_process_completions(qpair, 1);

	if (io_complete_flag != 1) {
		fprintf(stderr, "%s: %s writev failed\n", dev->name, test_name);
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		free_req(req);
		return -1;
	}

	/* reset completion flag */
	io_complete_flag = 0;

	for (i = 0; i < nseg; i++) {
		memset(req->iovs[i].base + req->iovs[i].offset, 0, req->iovs[i].len);
	}

	rc = spdk_nvme_ns_cmd_readv(ns, qpair, BASE_LBA_START, lba_count,
				    io_complete, req, 0,
				    nvme_request_reset_sgl,
				    nvme_request_next_sge);

	if (rc != 0) {
		fprintf(stderr, "%s: %s readv failed\n", dev->name, test_name);
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		free_req(req);
		return -1;
	}

	while (!io_complete_flag)
		spdk_nvme_qpair_process_completions(qpair, 1);

	if (io_complete_flag != 1) {
		fprintf(stderr, "%s: %s readv failed\n", dev->name, test_name);
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		free_req(req);
		return -1;
	}

	for (i = 0; i < nseg; i++) {
		buf = (char *)req->iovs[i].base + req->iovs[i].offset;
		for (j = 0; j < req->iovs[i].len; j++) {
			if (buf[j] != DATA_PATTERN) {
				fprintf(stderr, "%s: %s write/read success, but memcmp Failed\n", dev->name, test_name);
				spdk_nvme_ctrlr_free_io_qpair(qpair);
				free_req(req);
				return -1;
			}
		}
	}

	fprintf(stdout, "%s: %s test passed\n", dev->name, test_name);
	spdk_nvme_ctrlr_free_io_qpair(qpair);
	free_req(req);
	return rc;
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *dev, struct spdk_nvme_ctrlr_opts *opts)
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

	printf("Attaching to %04x:%02x:%02x.%02x\n",
	       spdk_pci_device_get_domain(dev),
	       spdk_pci_device_get_bus(dev),
	       spdk_pci_device_get_dev(dev),
	       spdk_pci_device_get_func(dev));

	return true;
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *pci_dev, struct spdk_nvme_ctrlr *ctrlr,
	  const struct spdk_nvme_ctrlr_opts *opts)
{
	struct dev *dev;

	/* add to dev list */
	dev = &devs[num_devs++];

	dev->ctrlr = ctrlr;

	snprintf(dev->name, sizeof(dev->name), "%04X:%02X:%02X.%02X",
		 spdk_pci_device_get_domain(pci_dev),
		 spdk_pci_device_get_bus(pci_dev),
		 spdk_pci_device_get_dev(pci_dev),
		 spdk_pci_device_get_func(pci_dev));

	printf("Attached to %s\n", dev->name);
}


static const char *ealargs[] = {
	"nvme_sgl",
	"-c 0x1",
	"-n 4",
};

int main(int argc, char **argv)
{
	struct dev			*iter;
	int				rc, i;

	printf("NVMe Readv/Writev Request test\n");

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

	if (spdk_nvme_probe(NULL, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "nvme_probe() failed\n");
		exit(1);
	}

	rc = 0;
	foreach_dev(iter) {
#define TEST(x) writev_readv_tests(iter, x, #x)
		if (TEST(build_io_request_0)
		    || TEST(build_io_request_1)
		    || TEST(build_io_request_2)
		    || TEST(build_io_request_3)
		    || TEST(build_io_request_4)
		    || TEST(build_io_request_5)
		    || TEST(build_io_request_6)) {
#undef TEST
			rc = 1;
			printf("%s: failed sgl tests\n", iter->name);
		}
	}

	printf("Cleaning up...\n");

	for (i = 0; i < num_devs; i++) {
		struct dev *dev = &devs[i];

		spdk_nvme_detach(dev->ctrlr);
	}

	return rc;
}
