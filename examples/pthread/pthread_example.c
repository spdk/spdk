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
#include "spdk/queue.h"

static uint32_t g_spdk_master_core = SPDK_ENV_LCORE_ID_ANY;

int (*pthread_create_orig)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);

int
pthread_create(pthread_t *tid, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
	uint32_t lcore;
	bool lcore_found = false;

	/* Only call our implementation on the master spdk lcore. */
	if (g_spdk_master_core == SPDK_ENV_LCORE_ID_ANY ||
	    spdk_env_get_current_core() != g_spdk_master_core) {
		if (!pthread_create_orig) {
			pthread_create_orig = dlsym(RTLD_NEXT, "pthread_create");
			assert(pthread_create_orig);
		}

		return pthread_create_orig(tid, attr, fn, arg);
	}

	/* attr is ignored for now */

	SPDK_ENV_FOREACH_CORE(lcore) {
		if (lcore != g_spdk_master_core && !spdk_env_core_is_pinned(lcore)) {
			lcore_found = true;
			break;
		}
	}

	if (!lcore_found) {
		fprintf(stderr, "Couldn't find any available lcore for the new pthread.\n");
		return EPERM;
	}

	*tid = (pthread_t)(uintptr_t)lcore;

	return spdk_env_thread_launch_pinned(lcore, (thread_start_fn)fn, arg);
}

static void *
thread_main(void *arg)
{
	printf("Echo from core %"PRIu32"\n", spdk_env_get_current_core());
	return NULL;
}

int
main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	pthread_t tid;
	int i, rc = 0;

	/* Initialize the environment library */
	spdk_env_opts_init(&opts);
	opts.name = "pthread_example";
	opts.core_mask = "0xf";

	spdk_env_init(&opts);
	g_spdk_master_core = spdk_env_get_current_core();

	printf("Master core: %"PRIu32"\n", g_spdk_master_core);
	for (i = 0; i < 3; ++i) {
		if (pthread_create(&tid, NULL, thread_main, NULL)) {
			fprintf(stderr, "Failed to create thread #%d\n", i);
			rc++;
		}
	}

	spdk_env_thread_wait_all();
	return 0;
}
