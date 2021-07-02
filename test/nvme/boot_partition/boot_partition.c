/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Samsung Electronics Co., Ltd.
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
 *     * Neither the name of Samsung Electronics Co., Ltd. nor the names of its
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
#include "spdk/util.h"
#include "spdk/env.h"

struct ctrlr {
	struct spdk_nvme_transport_id trid;
	struct spdk_nvme_ctrlr	*ctrlr;
	char			*write_buf;
	char			*read_buf;
	int			write_completed;
};

static struct ctrlr g_ctrlr;

static void cleanup(void);

static void
fill_pattern(char *buf, size_t num_bytes, char pattern)
{
	size_t	i;

	for (i = 0; i < num_bytes; i++) {
		buf[i] = pattern;
	}
}

static void
write_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	printf("Boot Partition Write - SCT : %d, SC : %d\n",
	       completion->status.sct, completion->status.sc);
	g_ctrlr.write_completed = 1;
}

static int
boot_partition_test(void)
{
	struct spdk_nvme_ctrlr			*ctrlr;
	union spdk_nvme_cap_register		cap;
	int					rc;
	union spdk_nvme_bpinfo_register		bpinfo;
	unsigned int				bpsize;
	unsigned int				bpsize_in_4k;

	ctrlr = g_ctrlr.ctrlr;

	cap = spdk_nvme_ctrlr_get_regs_cap(ctrlr);

	if (cap.bits.bps) {
		printf("Boot Partitions are Supported by the Controller\n");
	} else {
		printf("Boot Partitions are Not Supported by the Controller\n");
		return -ENOTSUP;
	}

	bpinfo = spdk_nvme_ctrlr_get_regs_bpinfo(ctrlr);
	bpsize = bpinfo.bits.bpsz * 131072;
	bpsize_in_4k = bpsize / 4096;

	printf("Boot Partition Info\n");
	printf("Active Boot Partition ID : %d\n", bpinfo.bits.abpid);
	printf("Boot Read Status : %d\n", bpinfo.bits.brs);
	printf("Boot Partition Size : %d bytes\n", bpsize);

	g_ctrlr.write_buf = spdk_zmalloc(bpsize, 0x1000, NULL,
					 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

	if (g_ctrlr.write_buf == NULL) {
		printf("Error - could not allocate write buffer for test\n");
		cleanup();
		return -ENOMEM;
	}

	g_ctrlr.read_buf = spdk_memzone_reserve("boot_partition", bpsize,
						SPDK_ENV_SOCKET_ID_ANY, 0);

	if (g_ctrlr.read_buf == NULL) {
		printf("Error - could not allocate read buffer for test\n");
		cleanup();
		return -ENOMEM;
	}

	fill_pattern(g_ctrlr.write_buf, bpsize, 0xDE);

	g_ctrlr.write_completed = 0;
	rc = spdk_nvme_ctrlr_write_boot_partition(ctrlr, g_ctrlr.write_buf,
			bpsize, 0, write_complete, NULL);
	if (rc) {
		printf("Error - Boot Partition write failure. rc: %d", rc);
		cleanup();
		return rc;
	}

	while (!g_ctrlr.write_completed) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	rc = spdk_nvme_ctrlr_read_boot_partition_start(ctrlr, g_ctrlr.read_buf,
			bpsize_in_4k, 0, 0);

	if (rc) {
		printf("Error - Boot Partition read start failure. rc: %d", rc);
		cleanup();
		return rc;
	}

	do {
		rc = spdk_nvme_ctrlr_read_boot_partition_poll(ctrlr);
	} while (rc == -EAGAIN);

	if (rc != 0) {
		printf("Error - Boot Partition read poll failure. rc: %d", rc);
		cleanup();
		return rc;
	}

	rc = memcmp(g_ctrlr.write_buf, g_ctrlr.read_buf, bpsize);
	if (rc) {
		printf("Error - Boot Partition written data does not match Boot Partition read data, rc: %d\n", rc);
		cleanup();
		return rc;
	}

	printf("Boot Partition 0 written data matches Boot Partition 0 read data\n");

	fill_pattern(g_ctrlr.write_buf, bpsize, 0xAD);

	g_ctrlr.write_completed = 0;
	rc = spdk_nvme_ctrlr_write_boot_partition(ctrlr, g_ctrlr.write_buf,
			bpsize, 1, write_complete, NULL);
	if (rc) {
		printf("Error - Boot Partition write failure. rc: %d", rc);
		cleanup();
		return rc;
	}

	while (!g_ctrlr.write_completed) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	rc = spdk_nvme_ctrlr_read_boot_partition_start(ctrlr, g_ctrlr.read_buf,
			bpsize_in_4k, 0, 1);

	if (rc) {
		printf("Error - Boot Partition read start failure. rc: %d", rc);
		cleanup();
		return rc;
	}

	do {
		rc = spdk_nvme_ctrlr_read_boot_partition_poll(ctrlr);
	} while (rc == -EAGAIN);

	if (rc != 0) {
		printf("Error - Boot Partition read poll failure. rc: %d", rc);
		cleanup();
		return rc;
	}

	rc = memcmp(g_ctrlr.write_buf, g_ctrlr.read_buf, bpsize);
	if (rc) {
		printf("Error - Boot Partition written data does not match Boot Partition read data, rc: %d\n", rc);
		cleanup();
		return rc;
	}

	printf("Boot Partition 1 written data matches Boot Partition 1 read data\n");

	cleanup();

	return 0;
}

static void
cleanup(void)
{
	spdk_memzone_free("boot_partition");
	spdk_free(g_ctrlr.write_buf);
	spdk_nvme_detach(g_ctrlr.ctrlr);
}

static void
usage(char *program_name)
{
	printf("%s Option (Mandatory)", program_name);
	printf("\n");
	printf("\t[-p PCIe address of the NVMe Device with Boot Partition support]\n");
	printf("\n");
}

static int
parse_args(int argc, char **argv)
{
	int op;
	unsigned num_args = 0;

	while ((op = getopt(argc, argv, "p:")) != -1) {
		switch (op) {
		case 'p':
			snprintf(&g_ctrlr.trid.traddr[0], SPDK_NVMF_TRADDR_MAX_LEN + 1,
				 "%s", optarg);

			g_ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;

			spdk_nvme_transport_id_populate_trstring(&g_ctrlr.trid,
					spdk_nvme_transport_id_trtype_str(g_ctrlr.trid.trtype));

			num_args++;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (num_args != 1) {
		usage(argv[0]);
		return 1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	int			rc;
	struct spdk_env_opts	opts;

	/*
	 * Parse the input arguments. For now we use the following
	 * format list:
	 *
	 * -p <pci id>
	 *
	 */
	rc = parse_args(argc, argv);
	if (rc) {
		fprintf(stderr, "Error in parse_args(): %d\n", rc);
		return rc;
	}

	spdk_env_opts_init(&opts);
	opts.name = "boot_partition";
	opts.shm_id = 0;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	printf("Initializing NVMe Controller\n");

	g_ctrlr.ctrlr = spdk_nvme_connect(&g_ctrlr.trid, NULL, 0);
	if (!g_ctrlr.ctrlr) {
		fprintf(stderr, "spdk_nvme_connect() failed\n");
		return 1;
	}

	printf("Initialization complete.\n");
	rc = boot_partition_test();
	return rc;
}
