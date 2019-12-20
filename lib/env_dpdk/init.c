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

#include "env_internal.h"

#include "spdk/version.h"
#include "spdk/env_dpdk.h"

#include <rte_config.h>
#include <rte_eal.h>
#include <rte_errno.h>

#define SPDK_ENV_DPDK_DEFAULT_NAME		"spdk"
#define SPDK_ENV_DPDK_DEFAULT_SHM_ID		-1
#define SPDK_ENV_DPDK_DEFAULT_MEM_SIZE		-1
#define SPDK_ENV_DPDK_DEFAULT_MASTER_CORE	-1
#define SPDK_ENV_DPDK_DEFAULT_MEM_CHANNEL	-1
#define SPDK_ENV_DPDK_DEFAULT_CORE_MASK		"0x1"

static char **g_eal_cmdline;
static int g_eal_cmdline_argcount;
static bool g_external_init = true;

static char *
_sprintf_alloc(const char *format, ...)
{
	va_list args;
	va_list args_copy;
	char *buf;
	size_t bufsize;
	int rc;

	va_start(args, format);

	/* Try with a small buffer first. */
	bufsize = 32;

	/* Limit maximum buffer size to something reasonable so we don't loop forever. */
	while (bufsize <= 1024 * 1024) {
		buf = malloc(bufsize);
		if (buf == NULL) {
			va_end(args);
			return NULL;
		}

		va_copy(args_copy, args);
		rc = vsnprintf(buf, bufsize, format, args_copy);
		va_end(args_copy);

		/*
		 * If vsnprintf() returned a count within our current buffer size, we are done.
		 * The count does not include the \0 terminator, so rc == bufsize is not OK.
		 */
		if (rc >= 0 && (size_t)rc < bufsize) {
			va_end(args);
			return buf;
		}

		/*
		 * vsnprintf() should return the required space, but some libc versions do not
		 * implement this correctly, so just double the buffer size and try again.
		 *
		 * We don't need the data in buf, so rather than realloc(), use free() and malloc()
		 * again to avoid a copy.
		 */
		free(buf);
		bufsize *= 2;
	}

	va_end(args);
	return NULL;
}

static void
spdk_env_unlink_shared_files(void)
{
	/* Starting with DPDK 18.05, there are more files with unpredictable paths
	 * and filenames. The --no-shconf option prevents from creating them, but
	 * only for DPDK 18.08+. For DPDK 18.05 we just leave them be.
	 */
#if RTE_VERSION < RTE_VERSION_NUM(18, 05, 0, 0)
	char buffer[PATH_MAX];

	snprintf(buffer, PATH_MAX, "/var/run/.spdk_pid%d_hugepage_info", getpid());
	if (unlink(buffer)) {
		fprintf(stderr, "Unable to unlink shared memory file: %s. Error code: %d\n", buffer, errno);
	}
#endif
}

void
spdk_env_opts_init(struct spdk_env_opts *opts)
{
	if (!opts) {
		return;
	}

	memset(opts, 0, sizeof(*opts));

	opts->name = SPDK_ENV_DPDK_DEFAULT_NAME;
	opts->core_mask = SPDK_ENV_DPDK_DEFAULT_CORE_MASK;
	opts->shm_id = SPDK_ENV_DPDK_DEFAULT_SHM_ID;
	opts->mem_size = SPDK_ENV_DPDK_DEFAULT_MEM_SIZE;
	opts->master_core = SPDK_ENV_DPDK_DEFAULT_MASTER_CORE;
	opts->mem_channel = SPDK_ENV_DPDK_DEFAULT_MEM_CHANNEL;
}

static void
spdk_free_args(char **args, int argcount)
{
	int i;

	for (i = 0; i < argcount; i++) {
		free(args[i]);
	}

	if (argcount) {
		free(args);
	}
}

static char **
spdk_push_arg(char *args[], int *argcount, char *arg)
{
	char **tmp;

	if (arg == NULL) {
		fprintf(stderr, "%s: NULL arg supplied\n", __func__);
		spdk_free_args(args, *argcount);
		return NULL;
	}

	tmp = realloc(args, sizeof(char *) * (*argcount + 1));
	if (tmp == NULL) {
		free(arg);
		spdk_free_args(args, *argcount);
		return NULL;
	}

	tmp[*argcount] = arg;
	(*argcount)++;

	return tmp;
}

#if defined(__linux__) && defined(__x86_64__)

/* TODO: Can likely get this value from rlimits in the future */
#define SPDK_IOMMU_VA_REQUIRED_WIDTH 48
#define VTD_CAP_MGAW_SHIFT 16
#define VTD_CAP_MGAW_MASK (0x3F << VTD_CAP_MGAW_SHIFT)

static int
spdk_get_iommu_width(void)
{
	DIR *dir;
	FILE *file;
	struct dirent *entry;
	char mgaw_path[64];
	char buf[64];
	char *end;
	long long int val;
	int width, tmp;

	dir = opendir("/sys/devices/virtual/iommu/");
	if (dir == NULL) {
		return -EINVAL;
	}

	width = 0;

	while ((entry = readdir(dir)) != NULL) {
		/* Find directories named "dmar0", "dmar1", etc */
		if (strncmp(entry->d_name, "dmar", sizeof("dmar") - 1) != 0) {
			continue;
		}

		tmp = snprintf(mgaw_path, sizeof(mgaw_path), "/sys/devices/virtual/iommu/%s/intel-iommu/cap",
			       entry->d_name);
		if ((unsigned)tmp >= sizeof(mgaw_path)) {
			continue;
		}

		file = fopen(mgaw_path, "r");
		if (file == NULL) {
			continue;
		}

		if (fgets(buf, sizeof(buf), file) == NULL) {
			fclose(file);
			continue;
		}

		val = strtoll(buf, &end, 16);
		if (val == LLONG_MIN || val == LLONG_MAX) {
			fclose(file);
			continue;
		}

		tmp = ((val & VTD_CAP_MGAW_MASK) >> VTD_CAP_MGAW_SHIFT) + 1;
		if (width == 0 || tmp < width) {
			width = tmp;
		}

		fclose(file);
	}

	closedir(dir);

	return width;
}

#endif

static int
spdk_build_eal_cmdline(const struct spdk_env_opts *opts)
{
	int argcount = 0;
	char **args;

	args = NULL;

	/* set the program name */
	args = spdk_push_arg(args, &argcount, _sprintf_alloc("%s", opts->name));
	if (args == NULL) {
		return -1;
	}

	/* disable shared configuration files when in single process mode. This allows for cleaner shutdown */
	if (opts->shm_id < 0) {
		args = spdk_push_arg(args, &argcount, _sprintf_alloc("%s", "--no-shconf"));
		if (args == NULL) {
			return -1;
		}
	}

	/* set the coremask */
	/* NOTE: If coremask starts with '[' and ends with ']' it is a core list
	 */
	if (opts->core_mask[0] == '[') {
		char *l_arg = _sprintf_alloc("-l %s", opts->core_mask + 1);
		int len = strlen(l_arg);
		if (l_arg[len - 1] == ']') {
			l_arg[len - 1] = '\0';
		}
		args = spdk_push_arg(args, &argcount, l_arg);
	} else {
		args = spdk_push_arg(args, &argcount, _sprintf_alloc("-c %s", opts->core_mask));
	}

	if (args == NULL) {
		return -1;
	}

	/* set the memory channel number */
	if (opts->mem_channel > 0) {
		args = spdk_push_arg(args, &argcount, _sprintf_alloc("-n %d", opts->mem_channel));
		if (args == NULL) {
			return -1;
		}
	}

	/* set the memory size */
	if (opts->mem_size >= 0) {
		args = spdk_push_arg(args, &argcount, _sprintf_alloc("-m %d", opts->mem_size));
		if (args == NULL) {
			return -1;
		}
	}

	/* set the master core */
	if (opts->master_core > 0) {
		args = spdk_push_arg(args, &argcount, _sprintf_alloc("--master-lcore=%d",
				     opts->master_core));
		if (args == NULL) {
			return -1;
		}
	}

	/* set no pci  if enabled */
	if (opts->no_pci) {
		args = spdk_push_arg(args, &argcount, _sprintf_alloc("--no-pci"));
		if (args == NULL) {
			return -1;
		}
	}

	/* create just one hugetlbfs file */
	if (opts->hugepage_single_segments) {
		args = spdk_push_arg(args, &argcount, _sprintf_alloc("--single-file-segments"));
		if (args == NULL) {
			return -1;
		}
	}

	/* unlink hugepages after initialization */
	if (opts->unlink_hugepage) {
		args = spdk_push_arg(args, &argcount, _sprintf_alloc("--huge-unlink"));
		if (args == NULL) {
			return -1;
		}
	}

	/* use a specific hugetlbfs mount */
	if (opts->hugedir) {
		args = spdk_push_arg(args, &argcount, _sprintf_alloc("--huge-dir=%s", opts->hugedir));
		if (args == NULL) {
			return -1;
		}
	}

#if RTE_VERSION >= RTE_VERSION_NUM(18, 05, 0, 0) && RTE_VERSION < RTE_VERSION_NUM(18, 5, 1, 0)
	/* Dynamic memory management is buggy in DPDK 18.05.0. Don't use it. */
	if (!opts->env_context || strstr(opts->env_context, "--legacy-mem") == NULL) {
		args = spdk_push_arg(args, &argcount, _sprintf_alloc("--legacy-mem"));
		if (args == NULL) {
			return -1;
		}
	}
#endif

	if (opts->num_pci_addr) {
		size_t i;
		char bdf[32];
		struct spdk_pci_addr *pci_addr =
				opts->pci_blacklist ? opts->pci_blacklist : opts->pci_whitelist;

		for (i = 0; i < opts->num_pci_addr; i++) {
			spdk_pci_addr_fmt(bdf, 32, &pci_addr[i]);
			args = spdk_push_arg(args, &argcount, _sprintf_alloc("%s=%s",
					     (opts->pci_blacklist ? "--pci-blacklist" : "--pci-whitelist"),
					     bdf));
			if (args == NULL) {
				return -1;
			}
		}
	}

	/* The following log-level options are not understood by older DPDKs */
#if RTE_VERSION >= RTE_VERSION_NUM(18, 05, 0, 0)
	/* Lower default EAL loglevel to RTE_LOG_NOTICE - normal, but significant messages.
	 * This can be overridden by specifying the same option in opts->env_context
	 */
	args = spdk_push_arg(args, &argcount, strdup("--log-level=lib.eal:6"));
	if (args == NULL) {
		return -1;
	}

	/* Lower default CRYPTO loglevel to RTE_LOG_ERR to avoid a ton of init msgs.
	 * This can be overridden by specifying the same option in opts->env_context
	 */
	args = spdk_push_arg(args, &argcount, strdup("--log-level=lib.cryptodev:5"));
	if (args == NULL) {
		return -1;
	}

	/* `user1` log type is used by rte_vhost, which prints an INFO log for each received
	 * vhost user message. We don't want that. The same log type is also used by a couple
	 * of other DPDK libs, but none of which we make use right now. If necessary, this can
	 * be overridden via opts->env_context.
	 */
	args = spdk_push_arg(args, &argcount, strdup("--log-level=user1:6"));
	if (args == NULL) {
		return -1;
	}
#endif

	if (opts->env_context) {
		args = spdk_push_arg(args, &argcount, strdup(opts->env_context));
		if (args == NULL) {
			return -1;
		}
	}

#ifdef __linux__

#if defined(__x86_64__)
	/* DPDK by default guesses that it should be using iova-mode=va so that it can
	 * support running as an unprivileged user. However, some systems (especially
	 * virtual machines) don't have an IOMMU capable of handling the full virtual
	 * address space and DPDK doesn't currently catch that. Add a check in SPDK
	 * and force iova-mode=pa here. */
	if (spdk_get_iommu_width() < SPDK_IOMMU_VA_REQUIRED_WIDTH) {
		args = spdk_push_arg(args, &argcount, _sprintf_alloc("--iova-mode=pa"));
		if (args == NULL) {
			return -1;
		}
	}
#elif defined(__PPC64__)
	/* On Linux + PowerPC, DPDK doesn't support VA mode at all. Unfortunately, it doesn't correctly
	 * auto-detect at the moment, so we'll just force it here. */
	args = spdk_push_arg(args, &argcount, _sprintf_alloc("--iova-mode=pa"));
	if (args == NULL) {
		return -1;
	}
#endif


	/* Set the base virtual address - it must be an address that is not in the
	 * ASAN shadow region, otherwise ASAN-enabled builds will ignore the
	 * mmap hint.
	 *
	 * Ref: https://github.com/google/sanitizers/wiki/AddressSanitizerAlgorithm
	 */
	args = spdk_push_arg(args, &argcount, _sprintf_alloc("--base-virtaddr=0x200000000000"));
	if (args == NULL) {
		return -1;
	}

	/* --match-allocation prevents DPDK from merging or splitting system memory allocations under the hood.
	 * This is critical for RDMA when attempting to use an rte_mempool based buffer pool. If DPDK merges two
	 * physically or IOVA contiguous memory regions, then when we go to allocate a buffer pool, it can split
	 * the memory for a buffer over two allocations meaning the buffer will be split over a memory region.
	 */
#if RTE_VERSION >= RTE_VERSION_NUM(19, 02, 0, 0)
	if (!opts->env_context || strstr(opts->env_context, "--legacy-mem") == NULL) {
		args = spdk_push_arg(args, &argcount, _sprintf_alloc("%s", "--match-allocations"));
		if (args == NULL) {
			return -1;
		}
	}
#endif

	if (opts->shm_id < 0) {
		args = spdk_push_arg(args, &argcount, _sprintf_alloc("--file-prefix=spdk_pid%d",
				     getpid()));
		if (args == NULL) {
			return -1;
		}
	} else {
		args = spdk_push_arg(args, &argcount, _sprintf_alloc("--file-prefix=spdk%d",
				     opts->shm_id));
		if (args == NULL) {
			return -1;
		}

		/* set the process type */
		args = spdk_push_arg(args, &argcount, _sprintf_alloc("--proc-type=auto"));
		if (args == NULL) {
			return -1;
		}
	}
#endif

	g_eal_cmdline = args;
	g_eal_cmdline_argcount = argcount;
	return argcount;
}

int
spdk_env_dpdk_post_init(bool legacy_mem)
{
	int rc;

	spdk_pci_init();

	rc = spdk_mem_map_init(legacy_mem);
	if (rc < 0) {
		fprintf(stderr, "Failed to allocate mem_map\n");
		return rc;
	}

	rc = spdk_vtophys_init();
	if (rc < 0) {
		fprintf(stderr, "Failed to initialize vtophys\n");
		return rc;
	}

	return 0;
}

void
spdk_env_dpdk_post_fini(void)
{
	spdk_pci_fini();

	spdk_free_args(g_eal_cmdline, g_eal_cmdline_argcount);
}

int
spdk_env_init(const struct spdk_env_opts *opts)
{
	char **dpdk_args = NULL;
	int i, rc;
	int orig_optind;
	bool legacy_mem;

	g_external_init = false;

	rc = spdk_build_eal_cmdline(opts);
	if (rc < 0) {
		fprintf(stderr, "Invalid arguments to initialize DPDK\n");
		return -EINVAL;
	}

	printf("Starting %s / %s initialization...\n", SPDK_VERSION_STRING, rte_version());
	printf("[ DPDK EAL parameters: ");
	for (i = 0; i < g_eal_cmdline_argcount; i++) {
		printf("%s ", g_eal_cmdline[i]);
	}
	printf("]\n");

	/* DPDK rearranges the array we pass to it, so make a copy
	 * before passing so we can still free the individual strings
	 * correctly.
	 */
	dpdk_args = calloc(g_eal_cmdline_argcount, sizeof(char *));
	if (dpdk_args == NULL) {
		fprintf(stderr, "Failed to allocate dpdk_args\n");
		return -ENOMEM;
	}
	memcpy(dpdk_args, g_eal_cmdline, sizeof(char *) * g_eal_cmdline_argcount);

	fflush(stdout);
	orig_optind = optind;
	optind = 1;
	rc = rte_eal_init(g_eal_cmdline_argcount, dpdk_args);
	optind = orig_optind;

	free(dpdk_args);

	if (rc < 0) {
		if (rte_errno == EALREADY) {
			fprintf(stderr, "DPDK already initialized\n");
		} else {
			fprintf(stderr, "Failed to initialize DPDK\n");
		}
		return -rte_errno;
	}

	if (opts->shm_id < 0 && !opts->hugepage_single_segments) {
		/*
		 * Unlink hugepage and config info files after init.  This will ensure they get
		 *  deleted on app exit, even if the app crashes and does not exit normally.
		 *  Only do this when not in multi-process mode, since for multi-process other
		 *  apps will need to open these files. These files are not created for
		 *  "single file segments".
		 */
		spdk_env_unlink_shared_files();
	}

	legacy_mem = false;
	if (opts->env_context && strstr(opts->env_context, "--legacy-mem") != NULL) {
		legacy_mem = true;
	}

	return spdk_env_dpdk_post_init(legacy_mem);
}

void
spdk_env_fini(void)
{
	spdk_env_dpdk_post_fini();
}

bool
spdk_env_dpdk_external_init(void)
{
	return g_external_init;
}
