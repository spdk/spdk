/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
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

#include "scsi_internal.h"

struct spdk_scsi_port *
spdk_scsi_port_create(uint64_t id, uint16_t index, const char *name)
{
	struct spdk_scsi_port *port;

	port = calloc(1, sizeof(struct spdk_scsi_port));

	if (!port) {
		return NULL;
	}

	if (spdk_scsi_port_construct(port, id, index, name) != 0) {
		spdk_scsi_port_free(&port);
		return NULL;
	}

	return port;
}

void
spdk_scsi_port_free(struct spdk_scsi_port **pport)
{
	struct spdk_scsi_port *port;

	if (!pport) {
		return;
	}

	port = *pport;
	*pport = NULL;
	free(port);
}

int
spdk_scsi_port_construct(struct spdk_scsi_port *port, uint64_t id, uint16_t index,
			 const char *name)
{
	if (strlen(name) >= sizeof(port->name)) {
		SPDK_ERRLOG("port name too long\n");
		return -1;
	}

	port->id = id;
	port->index = index;
	snprintf(port->name, sizeof(port->name), "%s", name);
	return 0;
}

const char *
spdk_scsi_port_get_name(const struct spdk_scsi_port *port)
{
	return port->name;
}
