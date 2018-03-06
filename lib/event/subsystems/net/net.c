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

#include "spdk/net.h"

#include "spdk_internal/event.h"

static void
spdk_interface_subsystem_init(void)
{
	int rc;

	rc = spdk_interface_init();

	spdk_subsystem_init_next(rc);
}

static void
spdk_interface_subsystem_destroy(void)
{
	spdk_interface_destroy();
	spdk_subsystem_fini_next();
}

static struct spdk_subsystem g_spdk_subsystem_interface = {
	.name = "interface",
	.init = spdk_interface_subsystem_init,
	.fini = spdk_interface_subsystem_destroy,
	.config = NULL,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_interface);

static void
spdk_net_subsystem_start(void)
{
	int rc;

	rc = spdk_net_framework_start();

	spdk_subsystem_init_next(rc);
}

static void
spdk_net_subsystem_fini(void)
{
	spdk_net_framework_fini();
	spdk_subsystem_fini_next();
}

static struct spdk_subsystem g_spdk_subsystem_net_framework = {
	.name = "net_framework",
	.init = spdk_net_subsystem_start,
	.fini = spdk_net_subsystem_fini,
	.config = NULL,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_net_framework);
SPDK_SUBSYSTEM_DEPEND(net_framework, interface)
