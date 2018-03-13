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

#include "spdk/log.h"
#include "spdk/net.h"
#include "spdk/queue.h"

static STAILQ_HEAD(, spdk_net_framework) g_net_frameworks =
	STAILQ_HEAD_INITIALIZER(g_net_frameworks);

int spdk_net_framework_start(void)
{
	struct spdk_net_framework *net_framework = NULL;
	int rc;

	STAILQ_FOREACH_FROM(net_framework, &g_net_frameworks, link) {
		rc = net_framework->init();
		if (rc != 0) {
			SPDK_ERRLOG("Net framework %s failed to initalize\n", net_framework->name);
			return rc;
		}
	}

	return 0;
}

void spdk_net_framework_fini(void)
{
	struct spdk_net_framework *net_framework = NULL;

	STAILQ_FOREACH_FROM(net_framework, &g_net_frameworks, link) {
		net_framework->fini();
	}
}

void
spdk_net_framework_register(struct spdk_net_framework *frame)
{
	STAILQ_INSERT_TAIL(&g_net_frameworks, frame, link);
}
