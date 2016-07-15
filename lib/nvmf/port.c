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

#include <rte_config.h>
#include <rte_debug.h>

#include "conn.h"
#include "rdma.h"
#include "port.h"
#include "spdk/log.h"
#include "spdk/trace.h"
#include "spdk/nvmf_spec.h"

static TAILQ_HEAD(, spdk_nvmf_port)	g_port_head = TAILQ_HEAD_INITIALIZER(g_port_head);

/* Assumes caller allocated host and port strings on the heap */
struct spdk_nvmf_fabric_intf *
spdk_nvmf_fabric_intf_create(char *host, char *sin_port)
{
	struct spdk_nvmf_fabric_intf *fabric_intf = NULL;

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Creating fabric intf: host address %s, port %s\n",
		      host, sin_port);

	RTE_VERIFY(host != NULL);
	RTE_VERIFY(sin_port != NULL);

	fabric_intf = calloc(1, sizeof(*fabric_intf));
	if (!fabric_intf) {
		SPDK_ERRLOG("fabric_intf calloc error\n");
		return NULL;
	}

	fabric_intf->host = host;
	fabric_intf->sin_port = sin_port;
	fabric_intf->trtype = SPDK_NVMF_TRANS_RDMA;
	fabric_intf->adrfam = SPDK_NVMF_ADDR_FAMILY_IPV4;
	fabric_intf->treq = SPDK_NVMF_TREQ_NOT_SPECIFIED;

	return fabric_intf;
}

void
spdk_nvmf_fabric_intf_destroy(struct spdk_nvmf_fabric_intf *fabric_intf)
{
	RTE_VERIFY(fabric_intf != NULL);

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Enter\n");
	free(fabric_intf);
}


struct spdk_nvmf_fabric_intf *
spdk_nvmf_port_find_fabric_intf_by_addr(char *addr)
{
	struct spdk_nvmf_port		*port;
	struct spdk_nvmf_fabric_intf	*fabric_intf;

	if (addr == NULL)
		goto find_error;

	TAILQ_FOREACH(port, &g_port_head, tailq) {
		TAILQ_FOREACH(fabric_intf, &port->head, tailq) {
			if (!strncasecmp(fabric_intf->host, addr, strlen(fabric_intf->host))) {
				return fabric_intf;
			}
		}
	}

find_error:
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "No device addr match for %s\n", addr);
	return NULL;
}

struct spdk_nvmf_port *
spdk_nvmf_port_create(int tag)
{
	struct spdk_nvmf_port *port;

	if (tag <= 0) {
		SPDK_ERRLOG("invalid port tag (%d)\n", tag);
		return NULL;
	}

	/* Make sure there are no duplicate port tags */
	if (spdk_nvmf_port_find_by_tag(tag)) {
		SPDK_ERRLOG("port creation failed.  duplicate port tag (%d)\n", tag);
		return NULL;
	}

	port = calloc(1, sizeof(*port));
	if (!port) {
		SPDK_ERRLOG("port calloc error (%d)\n", tag);
		return NULL;
	}

	port->state = GROUP_INIT;
	port->tag = tag;
	port->tsas.rdma.rdma_qptype = SPDK_NVMF_QP_TYPE_RELIABLE_CONNECTED;
	/* No provider specified */
	port->tsas.rdma.rdma_prtype = SPDK_NVMF_RDMA_NO_PROVIDER;
	port->tsas.rdma.rdma_cms = SPDK_NVMF_RDMA_CMS_RDMA_CM;

	TAILQ_INIT(&port->head);

	pthread_mutex_lock(&g_nvmf_tgt.mutex);
	port->state = GROUP_READY;
	TAILQ_INSERT_TAIL(&g_port_head, port, tailq);
	pthread_mutex_unlock(&g_nvmf_tgt.mutex);

	return port;
}

void
spdk_nvmf_port_destroy(struct spdk_nvmf_port *port)
{
	struct spdk_nvmf_fabric_intf	*fabric_intf;

	RTE_VERIFY(port != NULL);

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Enter\n");
	while (!TAILQ_EMPTY(&port->head)) {
		fabric_intf = TAILQ_FIRST(&port->head);
		TAILQ_REMOVE(&port->head, fabric_intf, tailq);
#if 0 // TODO: fix bogus scan-build warning about use-after-free
		spdk_nvmf_fabric_intf_destroy(fabric_intf);
#endif
	}

	TAILQ_REMOVE(&g_port_head, port, tailq);

#if 0 // TODO: fix bogus scan-build warning about use-after-free
	free(port);
#endif
}

void
spdk_nvmf_port_add_fabric_intf(struct spdk_nvmf_port *port,
			       struct spdk_nvmf_fabric_intf *fabric_intf)
{
	RTE_VERIFY(port != NULL);
	RTE_VERIFY(fabric_intf != NULL);

	fabric_intf->port = port;
	TAILQ_INSERT_TAIL(&port->head, fabric_intf, tailq);
}

struct spdk_nvmf_port *
spdk_nvmf_port_find_by_tag(int tag)
{
	struct spdk_nvmf_port *port;

	if (tag <= 0) {
		SPDK_ERRLOG("invalid port tag (%d)\n", tag);
		return NULL;
	}

	TAILQ_FOREACH(port, &g_port_head, tailq) {
		if (port->tag == tag) {
			SPDK_TRACELOG(SPDK_TRACE_DEBUG, " found port with tag: port %p\n", port);
			return port;
		}
	}

	return NULL;
}

void
spdk_nvmf_port_destroy_all(void)
{
	struct spdk_nvmf_port *port;

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Enter\n");
	pthread_mutex_lock(&g_nvmf_tgt.mutex);
	while (!TAILQ_EMPTY(&g_port_head)) {
		port = TAILQ_FIRST(&g_port_head);
		spdk_nvmf_port_destroy(port);
	}
	pthread_mutex_unlock(&g_nvmf_tgt.mutex);
}
