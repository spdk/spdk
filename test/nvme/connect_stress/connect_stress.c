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

#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/likely.h"
#include "spdk/sock.h"

static int g_time_in_sec;

static struct spdk_nvme_transport_id g_trid;

static void usage(char *program_name)
{
	printf("%s options", program_name);
	printf("\n");
	printf("\t[-t, --time <sec> time in seconds]\n");
	printf("\t[-c, --core-mask <mask>]\n");
	printf("\t\t(default: 1)\n");
	printf("\t[-r, --transport <fmt> Transport ID for local PCIe NVMe or NVMeoF]\n");
	printf("\t Format: 'key:value [key:value] ...'\n");
	printf("\t Keys:\n");
	printf("\t  trtype      Transport type (e.g. PCIe, RDMA)\n");
	printf("\t  adrfam      Address family (e.g. IPv4, IPv6)\n");
	printf("\t  traddr      Transport address (e.g. 0000:04:00.0 for PCIe or 192.168.100.8 for RDMA)\n");
	printf("\t  trsvcid     Transport service identifier (e.g. 4420)\n");
	printf("\t  subnqn      Subsystem NQN\n");
	printf("\t Example: -r 'trtype:PCIe traddr:0000:04:00.0' for PCIe or\n");
	printf("\t          -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.100.8 trsvcid:4420' for NVMeoF\n");
	printf("\t[-s, --hugemem-size <MB> DPDK huge memory size in MB.]\n");
	printf("\t\t(default: 0 - unlimited)\n");
	printf("\t[-i, --shmem-grp-id <id> shared memory group ID]\n");
	printf("\t");
	spdk_log_usage(stdout, "-T");
	printf("\t[-S, --default-sock-impl <impl> set the default sock impl, e.g. \"posix\"]\n");
#ifdef DEBUG
	printf("\t[-G, --enable-debug enable debug logging]\n");
#else
	printf("\t[-G, --enable-debug enable debug logging (flag disabled, must reconfigure with --enable-debug)\n");
	printf("\t[--iova-mode <mode> specify DPDK IOVA mode: va|pa]\n");
#endif
}

static int
add_trid(const char *trid_str)
{
	g_trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	if (spdk_nvme_transport_id_parse(&g_trid, trid_str) != 0) {
		fprintf(stderr, "Invalid transport ID format '%s'\n", trid_str);
		return 1;
	}

	spdk_nvme_transport_id_populate_trstring(&g_trid,
			spdk_nvme_transport_id_trtype_str(g_trid.trtype));

	return 0;
}

#define PERF_GETOPT_SHORT "c:i:r:s:t:GS:T:"

static const struct option g_cmdline_opts[] = {
#define PERF_CORE_MASK	'c'
	{"core-mask",			required_argument,	NULL, PERF_CORE_MASK},
#define PERF_SHMEM_GROUP_ID	'i'
	{"shmem-grp-id",		required_argument,	NULL, PERF_SHMEM_GROUP_ID},
#define PERF_TRANSPORT	'r'
	{"transport",			required_argument,	NULL, PERF_TRANSPORT},
#define PERF_HUGEMEM_SIZE	's'
	{"hugemem-size",		required_argument,	NULL, PERF_HUGEMEM_SIZE},
#define PERF_TIME	't'
	{"time",			required_argument,	NULL, PERF_TIME},
#define PERF_ENABLE_DEBUG	'G'
	{"enable-debug",		no_argument,		NULL, PERF_ENABLE_DEBUG},
#define PERF_DEFAULT_SOCK_IMPL	'S'
	{"default-sock-impl",		required_argument,	NULL, PERF_DEFAULT_SOCK_IMPL},
#define PERF_LOG_FLAG	'T'
	{"logflag",			required_argument,	NULL, PERF_LOG_FLAG},
#define PERF_IOVA_MODE		258
	{"iova-mode",			required_argument,	NULL, PERF_IOVA_MODE},
	/* Should be the last element */
	{0, 0, 0, 0}
};

static int
parse_args(int argc, char **argv, struct spdk_env_opts *env_opts)
{
	bool trid_set = false;
	int op, long_idx;
	long int val;
	int rc;

	while ((op = getopt_long(argc, argv, PERF_GETOPT_SHORT, g_cmdline_opts, &long_idx)) != -1) {
		switch (op) {
		case PERF_SHMEM_GROUP_ID:
		case PERF_HUGEMEM_SIZE:
		case PERF_TIME:
			val = spdk_strtol(optarg, 10);
			if (val < 0) {
				fprintf(stderr, "Converting a string to integer failed\n");
				return val;
			}
			switch (op) {
			case PERF_SHMEM_GROUP_ID:
				env_opts->shm_id = val;
				break;
			case PERF_HUGEMEM_SIZE:
				env_opts->mem_size = val;
				break;
			case PERF_TIME:
				g_time_in_sec = val;
				break;
			}
			break;
		case PERF_CORE_MASK:
			env_opts->core_mask = optarg;
			break;
		case PERF_TRANSPORT:
			if (trid_set) {
				fprintf(stderr, "Only one trid can be specified\n");
				usage(argv[0]);
				return 1;
			}
			trid_set = true;
			if (add_trid(optarg)) {
				usage(argv[0]);
				return 1;
			}
			break;
		case PERF_ENABLE_DEBUG:
#ifndef DEBUG
			fprintf(stderr, "%s must be configured with --enable-debug for -G flag\n",
				argv[0]);
			usage(argv[0]);
			return 1;
#else
			spdk_log_set_flag("nvme");
			spdk_log_set_print_level(SPDK_LOG_DEBUG);
			break;
#endif
		case PERF_LOG_FLAG:
			rc = spdk_log_set_flag(optarg);
			if (rc < 0) {
				fprintf(stderr, "unknown flag\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
#ifdef DEBUG
			spdk_log_set_print_level(SPDK_LOG_DEBUG);
#endif
			break;
		case PERF_DEFAULT_SOCK_IMPL:
			rc = spdk_sock_set_default_impl(optarg);
			if (rc) {
				fprintf(stderr, "Failed to set sock impl %s, err %d (%s)\n", optarg, errno, strerror(errno));
				return 1;
			}
			break;
		case PERF_IOVA_MODE:
			env_opts->iova_mode = optarg;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!g_time_in_sec) {
		fprintf(stderr, "missing -t (--time) operand\n");
		usage(argv[0]);
		return 1;
	}

	if (!trid_set) {
		fprintf(stderr, "missing -r operand\n");
		usage(argv[0]);
		return 1;
	}

	env_opts->no_pci = true;
	if (g_trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
		env_opts->no_pci = false;
	}

	return 0;
}

static int
test_controller(void)
{
	const int LOOP_COUNT = 5;
	struct spdk_nvme_qpair *qpair[LOOP_COUNT];
	union spdk_nvme_csts_register csts;
	struct spdk_nvme_ctrlr *ctrlr;

	ctrlr = spdk_nvme_connect(&g_trid, NULL, 0);
	if (ctrlr == NULL) {
		fprintf(stderr, "spdk_nvme_connect() failed for transport address '%s'\n",
			g_trid.traddr);
		return -1;
	}
	if (spdk_nvme_ctrlr_is_discovery(ctrlr)) {
		fprintf(stderr, "discovery controller not allowed for this test\n");
		spdk_nvme_detach(ctrlr);
		return -1;
	}

	for (int i = 0; i < LOOP_COUNT; i++) {
		csts = spdk_nvme_ctrlr_get_regs_csts(ctrlr);
		if (csts.raw == 0xFFFFFFFF) {
			fprintf(stderr, "could not read csts\n");
			spdk_nvme_detach(ctrlr);
			return -1;
		}
		qpair[i] = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
		if (qpair[i] == NULL) {
			fprintf(stderr, "could not allocate io qpair\n");
			spdk_nvme_detach(ctrlr);
			return -1;
		}
	}
	for (int i = 0; i < LOOP_COUNT; i++) {
		spdk_nvme_ctrlr_free_io_qpair(qpair[i]);
	}

	spdk_nvme_detach(ctrlr);

	return 0;
}

int main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	uint64_t tsc_end;
	int rc;

	spdk_env_opts_init(&opts);
	opts.name = "connect_stress";
	rc = parse_args(argc, argv, &opts);
	if (rc != 0) {
		return rc;
	}
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return -1;
	}

	if (g_trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
		printf("Testing NVMe over Fabrics controller at %s:%s: %s\n",
		       g_trid.traddr, g_trid.trsvcid,
		       g_trid.subnqn);
	} else {
		printf("Testing NVMe PCI controller at %s\n", g_trid.traddr);
	}

	tsc_end = spdk_get_ticks() + g_time_in_sec * spdk_get_ticks_hz();
	while (spdk_get_ticks() < tsc_end && rc == 0) {
		rc = test_controller();
	}

	return 0;
}
