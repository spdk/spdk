/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 SanDisk. All rights reserved.
 */

#include "sit_common.h"
/* Test-only capability override; no public API forces PRPs on SGL devices. */
#include "nvme_internal.h"
#include "nvme_pcie_internal.h"
#include <getopt.h>

static struct sit_context g_ctx = {0};
static struct sit_test_def *g_test_list_head = NULL;
static struct sit_test_def *g_test_list_tail = NULL;
static bool g_env_initialized = false;
static bool g_trids_saved = false;
static struct spdk_nvme_transport_id g_saved_trids[SIT_MAX_CTRLRS];
static int g_num_saved_trids = 0;
static struct spdk_pci_addr g_pci_addr;
static bool g_force_prp;

void
sit_register_test(struct sit_test_def *test)
{
	test->next = NULL;
	if (g_test_list_tail == NULL) {
		/* First test */
		g_test_list_head = test;
		g_test_list_tail = test;
	} else {
		/* Append to tail */
		g_test_list_tail->next = test;
		g_test_list_tail = test;
	}
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	int i;

	(void)cb_ctx;
	(void)opts;

	/* If no BDF filter specified, attach to everything */
	if (g_ctx.num_bdf_filters == 0) {
		printf("SIT: Found device at %s\n", trid->traddr);
		return true;
	}

	/* Otherwise only attach to explicitly requested BDFs */
	for (i = 0; i < g_ctx.num_bdf_filters; i++) {
		if (strcmp(trid->traddr, g_ctx.bdf_filters[i]) == 0) {
			printf("SIT: Found requested device at %s\n", trid->traddr);
			return true;
		}
	}

	printf("SIT: Skipping device at %s (not in --bdf list)\n", trid->traddr);
	return false;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	(void)cb_ctx;
	(void)opts;
	printf("SIT: Attached to %s\n", trid->traddr);
	if (g_force_prp) {
		ctrlr->flags &= ~(uint64_t)SPDK_NVME_CTRLR_SGL_SUPPORTED;
		printf("SIT: --force-prp: disabled SGL selection for this test attachment\n");
	}
	if (g_ctx.num_ctrlrs < SIT_MAX_CTRLRS) {
		g_ctx.ctrlrs[g_ctx.num_ctrlrs++] = ctrlr;
	}
	if (!g_trids_saved && g_num_saved_trids < SIT_MAX_CTRLRS) {
		g_saved_trids[g_num_saved_trids++] = *trid;
	}
}

int
sit_env_init(void)
{
	struct spdk_env_opts	opts = {.opts_size = sizeof(opts)};
	const char		*iova_mode;
	int			rc;
	int			i;

	/*
	 * Call spdk_env_init() only once per process.  On repeated calls
	 * (after sit_env_fini()), DPDK hugepage memory is still alive --
	 * skip directly to spdk_nvme_probe() to re-attach devices.
	 */
	if (!g_env_initialized) {
		spdk_env_opts_init(&opts);
		opts.name = "sit_runner";
		opts.shm_id = -1;
		opts.pci_allowed = &g_pci_addr;
		opts.num_pci_addr = 1;
		iova_mode = getenv("SIT_IOVA_MODE");
		opts.iova_mode = (iova_mode != NULL && iova_mode[0] != '\0') ? iova_mode : "pa";
		if (spdk_env_init(&opts) < 0) {
			fprintf(stderr, "Unable to initialize SPDK env\n");
			return -1;
		}
		g_env_initialized = true;
	}

	printf("SIT: Probing NVMe devices...\n");
	if (g_trids_saved) {
		/* Re-probe after sit_env_fini(): DPDK won't re-scan PCIe bus, so
		 * probe each saved BDF individually by explicit transport ID. */
		for (i = 0; i < g_num_saved_trids; i++) {
			rc = spdk_nvme_probe(&g_saved_trids[i], NULL,
					     probe_cb, attach_cb, NULL);
			if (rc != 0) {
				fprintf(stderr, "spdk_nvme_probe() failed for %s\n",
					g_saved_trids[i].traddr);
				return -1;
			}
		}
	} else {
		rc = spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL);
		if (rc != 0) {
			fprintf(stderr, "spdk_nvme_probe() failed\n");
			return -1;
		}
		g_trids_saved = true;
	}

	/* Verify all requested BDFs were found */
	if (g_ctx.num_bdf_filters > 0) {
		int i, j;
		for (i = 0; i < g_ctx.num_bdf_filters; i++) {
			bool found = false;
			const char *filter = g_ctx.bdf_filters[i];

			for (j = 0; j < g_ctx.num_ctrlrs; j++) {
				const struct spdk_nvme_transport_id *trid =
					spdk_nvme_ctrlr_get_transport_id(g_ctx.ctrlrs[j]);
				if (strcmp(trid->traddr, filter) == 0) {
					found = true;
					break;
				}
			}

			if (!found) {
				fprintf(stderr, "ERROR: Requested device %s was not found or failed to attach.\n",
					filter);
				return -1;
			}
		}
	}

	return 0;
}

void
sit_env_fini(void)
{
	int i;

	for (i = 0; i < g_ctx.num_ctrlrs; i++) {
		spdk_nvme_detach(g_ctx.ctrlrs[i]);
	}
	g_ctx.num_ctrlrs = 0;
	/*
	 * Do not call spdk_env_fini() here.  DPDK does not support
	 * re-initializing hugepage memory after spdk_env_fini() in
	 * the same process.  spdk_env_fini() is called once at exit
	 * from main().
	 */
}

static void
usage(const char *prog_name)
{
	printf("Usage: %s [options]\n", prog_name);
	printf("Options:\n");
	printf("  -t, --test=NAME              Run specific test by name\n");
	printf("  -b, --bdf=ADDR               Required: one dedicated test device BDF\n");
	printf("                               e.g. --bdf=0000:01:00.0\n");
	printf("  -l, --list                   List all registered tests\n");
	printf("  -p, --force-prp              Test-only: use PRPs even if SGLs are supported\n");
	printf("  -m, --test-2mib              Test-only: allow 2 MiB PRPs on an MDTS=0 device\n");
	printf("  -h, --help                   Show this help\n");
}

static void
list_tests(void)
{
	struct sit_test_def *test = g_test_list_head;
	int num = 1;

	printf("Registered Tests:\n");
	while (test) {
		printf("  %d. %s\n", num++, test->name);
		test = test->next;
	}
}

int
main(int argc, char **argv)
{
	struct sit_test_def  *test;
	int                   rc = 0;
	int                   fail_count = 0;
	int                   skip_count = 0;
	int                   opt, option_index;
	const char           *target_test = NULL;
	bool                  do_list = false;
	char                  bdf[sizeof(g_saved_trids[0].traddr)];

	static struct option long_options[] = {
		{"test", required_argument, 0, 't'},
		{"bdf",  required_argument, 0, 'b'},
		{"list", no_argument,       0, 'l'},
		{"force-prp", no_argument,  0, 'p'},
		{"test-2mib", no_argument,  0, 'm'},
		{"help", no_argument,       0, 'h'},
		{0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, argv, "t:b:lpmh", long_options, &option_index)) != -1) {
		switch (opt) {
		case 't':
			target_test = optarg;
			break;
		case 'b':
			if (g_ctx.num_bdf_filters < SIT_MAX_CTRLRS) {
				g_ctx.bdf_filters[g_ctx.num_bdf_filters++] = optarg;
			} else {
				fprintf(stderr, "Too many --bdf arguments (max %d)\n", SIT_MAX_CTRLRS);
				return 1;
			}
			break;
		case 'l':
			do_list = true;
			break;
		case 'p':
			g_force_prp = true;
			break;
		case 'm':
			g_ctx.test_2mib = true;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}


	if (argc == 1) {
		usage(argv[0]);
		return 0;
	}

	if (do_list) {
		list_tests();
		return 0;
	}

	if (optind != argc || g_ctx.num_bdf_filters != 1) {
		fprintf(stderr, "Specify exactly one --bdf for a dedicated test device\n");
		return 1;
	}
	if (spdk_pci_addr_parse(&g_pci_addr, g_ctx.bdf_filters[0]) != 0) {
		fprintf(stderr, "Invalid PCI BDF: %s\n", g_ctx.bdf_filters[0]);
		return 1;
	}
	if (spdk_pci_addr_fmt(bdf, sizeof(bdf), &g_pci_addr) != 0) {
		fprintf(stderr, "Could not format PCI BDF\n");
		return 1;
	}
	g_ctx.bdf_filters[0] = bdf;

	if (target_test) {
		for (test = g_test_list_head; test != NULL; test = test->next) {
			if (strcmp(test->name, target_test) == 0) {
				break;
			}
		}
		if (test == NULL) {
			fprintf(stderr, "Unknown SIT test: %s\n", target_test);
			return 1;
		}
	}

	if (sit_env_init() < 0) {
		sit_env_fini();
		if (g_env_initialized) {
			spdk_env_fini();
		}
		return 1;
	}

	printf("SIT: Found %d controllers\n", g_ctx.num_ctrlrs);

	if (g_ctx.test_2mib && g_ctx.num_ctrlrs == 0) {
		fprintf(stderr, "Test-only PRP override requires an attached controller\n");
		sit_env_fini();
		spdk_env_fini();
		return 1;
	}
	if (g_ctx.test_2mib) {
		struct spdk_nvme_ctrlr *ctrlr = g_ctx.ctrlrs[0];

		if (!g_force_prp || ctrlr->cdata.mdts != 0 ||
		    ctrlr->page_size != 4096 || nvme_pcie_ctrlr(ctrlr)->prp_pool != NULL) {
			fprintf(stderr, "--test-2mib requires --force-prp, MDTS=0, "
				"4 KiB pages, and no existing pool\n");
			rc = -EINVAL;
		} else {
			rc = nvme_pcie_prp_pool_init(ctrlr, 2 * 1024 * 1024);
			if (rc == 0) {
				ctrlr->max_xfer_size = 2 * 1024 * 1024;
				printf("SIT: test-only 2 MiB pool/transfer limit; Identify MDTS unchanged\n");
			} else {
				fprintf(stderr, "Test-only PRP pool initialization failed: %s\n",
					spdk_strerror(-rc));
			}
		}
		if (rc != 0) {
			sit_env_fini();
			spdk_env_fini();
			return 1;
		}
	}

	test = g_test_list_head;
	while (test) {
		if (target_test && strcmp(test->name, target_test) != 0) {
			test = test->next;
			continue;
		}

		printf("--------------------------------------------------\n");
		printf("SIT: Running %s...\n", test->name);

		if (test->setup) {
			rc = test->setup(&g_ctx);
			if (rc == SIT_SKIP) {
				printf("SIT: %s [SKIP]\n", test->name);
				skip_count++;
				test = test->next;
				continue;
			}
			if (rc != 0) {
				printf("SIT: Setup FAILED (rc=%d)\n", rc);
				fail_count++;
				test = test->next;
				continue;
			}
		}

		rc = test->run(&g_ctx);
		if (rc == 0) {
			printf("SIT: %s [PASS]\n", test->name);
		} else if (rc == SIT_SKIP) {
			printf("SIT: %s [SKIP]\n", test->name);
			skip_count++;
		} else {
			printf("SIT: %s [FAIL] (rc=%d)\n", test->name, rc);
			fail_count++;
		}

		if (test->teardown) {
			test->teardown(&g_ctx);
		}

		test = test->next;
	}

	sit_env_fini();
	spdk_env_fini();
	if (skip_count > 0) {
		printf("SIT: %d test(s) skipped\n", skip_count);
	}
	return fail_count > 0 ? 1 : (skip_count > 0 ? SIT_SKIP : 0);
}
