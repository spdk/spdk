/*
 *   Copyright © 2022 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
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
#include "spdk/env.h"
#include "spdk/cpuset.h"
#include "spdk/thread.h"
#include "spdk/config.h"
#include "spdk/log.h"
#include "spdk/vrdma_io_mgr.h"

#define SPDK_IO_MGR_THREAD_NAME_PREFIX "VrdmaSnapThread"
#define SPDK_IO_MGR_THREAD_NAME_LEN 32

static size_t g_num_spdk_threads;
static struct spdk_thread **g_spdk_threads;
static struct spdk_thread *app_thread;

size_t spdk_io_mgr_get_num_threads(void)
{
    return g_num_spdk_threads;
}

struct spdk_thread *spdk_io_mgr_get_thread(int id)
{
    if (id == -1)
        return app_thread;
    return g_spdk_threads[id];
}

static void spdk_thread_exit_wrapper(void *uarg)
{
    (void)spdk_thread_exit((struct spdk_thread *)uarg);
}

int spdk_io_mgr_init(void)
{
    struct spdk_cpuset *cpumask;
    int i, j;
    char thread_name[SPDK_IO_MGR_THREAD_NAME_LEN];

    app_thread = spdk_get_thread();

    g_num_spdk_threads = spdk_env_get_core_count();
    g_spdk_threads = calloc(g_num_spdk_threads, sizeof(*g_spdk_threads));
    if (!g_spdk_threads) {
        SPDK_ERRLOG("Failed to allocate IO threads");
        goto err;
    }

    cpumask = spdk_cpuset_alloc();
    if (!cpumask) {
        SPDK_ERRLOG("Failed to allocate SPDK CPU mask");
        goto free_threads;
    }

    j = 0;
    SPDK_ENV_FOREACH_CORE(i) {
        spdk_cpuset_zero(cpumask);
        spdk_cpuset_set_cpu(cpumask, i, true);
        snprintf(thread_name, SPDK_IO_MGR_THREAD_NAME_LEN, "%s%d",
                 SPDK_IO_MGR_THREAD_NAME_PREFIX, j);
        g_spdk_threads[j] = spdk_thread_create(thread_name, cpumask);
        if (!g_spdk_threads[j]) {
            SPDK_ERRLOG("Failed to create thread %s", thread_name);
            spdk_cpuset_free(cpumask);
            goto exit_threads;
        }

        j++;
    }
    spdk_cpuset_free(cpumask);

    return 0;

exit_threads:
    for (j--; j >= 0; j--)
        spdk_thread_send_msg(g_spdk_threads[j], spdk_thread_exit_wrapper,
                             g_spdk_threads[j]);
free_threads:
    free(g_spdk_threads);
    g_spdk_threads = NULL;
    g_num_spdk_threads = 0;
err:
    return -1;
}

void spdk_io_mgr_clear(void)
{
    uint32_t i;

    for (i = 0; i < g_num_spdk_threads; i++)
        spdk_thread_send_msg(g_spdk_threads[i], spdk_thread_exit_wrapper,
                             g_spdk_threads[i]);
    free(g_spdk_threads);
    g_spdk_threads = NULL;
    g_num_spdk_threads = 0;
}
