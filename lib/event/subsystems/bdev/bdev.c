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

#include "spdk/bdev.h"

#include "spdk_internal/event.h"

static void
spdk_bdev_initialize_complete(void *cb_arg, int rc)
{
	spdk_subsystem_init_next(rc);
}

static void
spdk_bdev_subsystem_start_poller(struct spdk_bdev_poller **ppoller,
				 spdk_bdev_poller_fn fn,
				 void *arg,
				 uint32_t lcore,
				 uint64_t period_microseconds)
{
	spdk_poller_register((struct spdk_poller **)ppoller,
			     fn,
			     arg,
			     lcore,
			     period_microseconds);
}

static void
spdk_bdev_subsystem_stop_poller(struct spdk_bdev_poller **ppoller)
{
	spdk_poller_unregister((struct spdk_poller **)ppoller, NULL);
}

static void
spdk_bdev_subsystem_initialize(void)
{
	spdk_bdev_initialize(spdk_bdev_initialize_complete, NULL,
			     spdk_bdev_subsystem_start_poller,
			     spdk_bdev_subsystem_stop_poller);
}

static int
spdk_bdev_subsystem_finish(void)
{
	return spdk_bdev_finish();
}

SPDK_SUBSYSTEM_REGISTER(bdev, spdk_bdev_subsystem_initialize,
			spdk_bdev_subsystem_finish, spdk_bdev_config_text)
SPDK_SUBSYSTEM_DEPEND(bdev, copy)
