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

#include <rte_config.h>
#include <rte_eal.h>

#define SPDK_ENV_DPDK_DEFAULT_NAME		"spdk"
#define SPDK_ENV_DPDK_DEFAULT_SHM_ID		-1
#define SPDK_ENV_DPDK_DEFAULT_MEM_SIZE		-1
#define SPDK_ENV_DPDK_DEFAULT_MASTER_CORE	-1
#define SPDK_ENV_DPDK_DEFAULT_MEM_CHANNEL	-1
#define SPDK_ENV_DPDK_DEFAULT_CORE_MASK		"0x1"

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

	assert(args != NULL);

	for (i = 0; i < argcount; i++) {
		assert(args[i] != NULL);
		free(args[i]);
	}

	free(args);
}

static char **
spdk_push_arg(char *args[], int *argcount, char *arg)
{
	char **tmp;

	if (arg == NULL) {
		return NULL;
	}

	tmp = realloc(args, sizeof(char *) * (*argcount + 1));
	if (tmp == NULL) {
		spdk_free_args(args, *argcount);
		return NULL;
	}

	tmp[*argcount] = arg;
	(*argcount)++;

	return tmp;
}

static int
spdk_build_eal_cmdline(const struct spdk_env_opts *opts, char **out[])
{
	int argcount = 0;
	char **args;

	if (out == NULL) {
		return -1;
	}

	*out = NULL;
	args = NULL;

	/* set the program name */
	args = spdk_push_arg(args, &argcount, _sprintf_alloc("%s", opts->name));
	if (args == NULL) {
		return -1;
	}

	/* set the coremask */
	args = spdk_push_arg(args, &argcount, _sprintf_alloc("-c %s", opts->core_mask));
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
	if (opts->mem_size > 0) {
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

#ifdef __linux__
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

		/* set the base virtual address */
		args = spdk_push_arg(args, &argcount, _sprintf_alloc("--base-virtaddr=0x1000000000"));
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

	*out = args;

	return argcount;
}

void spdk_env_init(const struct spdk_env_opts *opts)
{
	char **args = NULL;
	char **dpdk_args = NULL;
	int argcount, i, rc;
	int orig_optind;

	argcount = spdk_build_eal_cmdline(opts, &args);
	if (argcount <= 0) {
		fprintf(stderr, "Invalid arguments to initialize DPDK\n");
		exit(-1);
	}

	printf("Starting %s initialization...\n", rte_version());
	printf("[ DPDK EAL parameters: ");
	for (i = 0; i < argcount; i++) {
		printf("%s ", args[i]);
	}
	printf("]\n");

	/* DPDK rearranges the array we pass to it, so make a copy
	 * before passing so we can still free the individual strings
	 * correctly.
	 */
	dpdk_args = calloc(argcount, sizeof(char *));
	if (dpdk_args == NULL) {
		fprintf(stderr, "Failed to allocate dpdk_args\n");
		exit(-1);
	}
	memcpy(dpdk_args, args, sizeof(char *) * argcount);

	fflush(stdout);
	orig_optind = optind;
	optind = 1;
	rc = rte_eal_init(argcount, dpdk_args);
	optind = orig_optind;

	spdk_free_args(args, argcount);
	free(dpdk_args);

	if (rc < 0) {
		fprintf(stderr, "Failed to initialize DPDK\n");
		exit(-1);
	}

	spdk_mem_map_init();
	spdk_vtophys_init();
}
