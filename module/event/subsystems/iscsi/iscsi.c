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

#include "iscsi/iscsi.h"

#include "spdk_internal/event.h"

static void
iscsi_subsystem_init_complete(void *cb_arg, int rc)
{
	spdk_subsystem_init_next(rc);
}

static void
iscsi_subsystem_init(void)
{
	spdk_iscsi_init(iscsi_subsystem_init_complete, NULL);
}

static void
iscsi_subsystem_fini_done(void *arg)
{
	spdk_subsystem_fini_next();
}

static void
iscsi_subsystem_fini(void)
{
	spdk_iscsi_fini(iscsi_subsystem_fini_done, NULL);
}

static void
iscsi_subsystem_config_json(struct spdk_json_write_ctx *w)
{
	spdk_iscsi_config_json(w);
}

static struct spdk_subsystem g_spdk_subsystem_iscsi = {
	.name = "iscsi",
	.init = iscsi_subsystem_init,
	.fini = iscsi_subsystem_fini,
	.write_config_json = iscsi_subsystem_config_json,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_iscsi);
SPDK_SUBSYSTEM_DEPEND(iscsi, scsi)
SPDK_SUBSYSTEM_DEPEND(iscsi, sock)
