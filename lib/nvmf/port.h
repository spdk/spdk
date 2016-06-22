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

#ifndef NVMF_PORT_H
#define NVMF_PORT_H

#include <stdint.h>

#include "spdk/conf.h"
#include "spdk/queue.h"
#include "spdk/nvmf_spec.h"

/** \file
* An NVMf subsystem port, referred to as simply "port" is defined by the
* specification as follows:
*
* An NVM subsystem port (port) is a collection of one or more physical fabric
* interfaces that together act as a single interface between the NVM subsystem
* and a fabric. When link aggregation (e.g., Ethernet) is used, the physical
* ports for the group of aggregated links constitute a single NVM subsystem port.
*/

enum fabric_type {
	FABRIC_RDMA = 0x1,
	FABRIC_PCI = 0x2,
	FABRIC_ETHERNET = 0x3,
};

enum group_state {
	GROUP_INIT = 0x0,
	GROUP_READY = 0x1,
	GROUP_DESTROY = 0x2,
};

struct spdk_nvmf_fabric_intf {
	char					*host;
	char					*sin_port;
	struct spdk_nvmf_port			*port;
	enum spdk_nvmf_transport_types		trtype;
	enum spdk_nvmf_address_family_types	adrfam;
	enum spdk_nvmf_transport_requirements	treq;
	uint32_t				num_sessions;
	TAILQ_ENTRY(spdk_nvmf_fabric_intf)	tailq;
};

struct spdk_nvmf_port {
	int tag;
	enum group_state state;
	enum fabric_type type;
	struct spdk_nvmf_rdma_transport_specific_address rdma;
	TAILQ_HEAD(, spdk_nvmf_fabric_intf)	head;
	TAILQ_ENTRY(spdk_nvmf_port)		tailq;
};

struct spdk_nvmf_fabric_intf *
spdk_nvmf_fabric_intf_create(char *host, char *sin_port);

void
spdk_nvmf_fabric_intf_destroy(struct spdk_nvmf_fabric_intf *fabric_intf);

struct spdk_nvmf_fabric_intf *
spdk_nvmf_port_find_fabric_intf_by_addr(char *addr);

struct spdk_nvmf_port *
spdk_nvmf_port_create(int tag);

void
spdk_nvmf_port_destroy(struct spdk_nvmf_port *port);

struct spdk_nvmf_port *
spdk_nvmf_port_find_by_tag(int tag);

void
spdk_nvmf_port_add_fabric_intf(struct spdk_nvmf_port *port,
			       struct spdk_nvmf_fabric_intf *fabric_intf);

void
spdk_nvmf_port_destroy_all(void);

#endif
