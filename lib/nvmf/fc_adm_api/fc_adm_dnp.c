/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 1992-2018 NetApp, Inc.
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

#include "spdk/trace.h"
#include "spdk_internal/log.h"
#include "spdk/nvmf_spec.h"
#include "spdk/log.h"
#include "spdk/endian.h"
#include "spdk/string.h"
#include "spdk/fc_adm_api.h"
#include "../nvmf_fc.h"

/*
 * FC ADM API - Dump and Print functions
 */

void
spdk_nvmf_fc_print_port_list(void *arg1, void *arg2)
{
	uint8_t port_hdl;
	struct spdk_nvmf_fc_port *port;
	SPDK_NOTICELOG("\nPort list\n");
	SPDK_NOTICELOG("\n*******************************\n");

	/*
	 * Go through all possible port handles. Make no assumptions on
	 * how many ports may have been set up in the system in this function.
	 */
	for (port_hdl = 0; port_hdl < SPDK_MAX_NUM_OF_FC_PORTS; port_hdl++) {
		port = spdk_nvmf_fc_port_list_get(port_hdl);
		if (port) {
			SPDK_NOTICELOG("Port Hdl: %d\n", port->port_hdl);
		}
	}
}

void
spdk_nvmf_fc_print_port(void *arg1, void *arg2)
{
	uint8_t *port_hdl = (uint8_t *)arg1;
	struct spdk_nvmf_fc_port *port;
	struct spdk_nvmf_fc_hwqp *ls;
	struct spdk_nvmf_fc_hwqp *io;
	struct spdk_nvmf_fc_nport *nport;
	struct spdk_nvmf_fc_xchg_info xchg_info;
	uint32_t i;

	SPDK_NOTICELOG("\nDump port details\n");
	SPDK_NOTICELOG("\n*******************************\n");

	port = spdk_nvmf_fc_port_list_get(*port_hdl);
	if (port == NULL) {
		SPDK_NOTICELOG("Port handle not found. Port Hdl: %d\n", *port_hdl);
		goto out;
	}

	ls = &(port->ls_queue);

	SPDK_NOTICELOG("Port Hdl: %d\n", port->port_hdl);
	SPDK_NOTICELOG("Hw Port Status: %d\n", port->hw_port_status);
	SPDK_NOTICELOG("FCP RQ ID: %d\n", port->fcp_rq_id);
	SPDK_NOTICELOG("LS Queue:\n");
	SPDK_NOTICELOG("\tThread name: '%s', HWQP ID: %d\n",
		       spdk_thread_get_name(ls->thread), ls->hwqp_id);
	SPDK_NOTICELOG("\tNum of Conns: %d, State: %d\n", ls->num_conns, ls->state);
	SPDK_NOTICELOG("\tXCHG Base: %d, ", xchg_info.xchg_base);
	SPDK_NOTICELOG("XCHG Total Count: %d, ", xchg_info.xchg_total_count);
	SPDK_NOTICELOG("XCHG Avail Count: %d\n", xchg_info.xchg_avail_count);
	/* if (ls->fc_request_pool) {
		SPDK_NOTICELOG("\tRequest Pool Max Count: %d Avail Count: %zd\n",
			       ls->rq_size, spdk_mempool_count(ls->fc_request_pool));
	} else {
		SPDK_NOTICELOG("\tLS Queue Request Pool not present\n");
	} */
	SPDK_NOTICELOG("Max IO Queues: %d\n", port->num_io_queues);
	SPDK_NOTICELOG("HWQP IO Queues:\n");
	SPDK_NOTICELOG("\n");
	for (i = 0; i < port->num_io_queues; i++) {
		io = &(port->io_queues[i]);
		if (io->thread) {
			SPDK_NOTICELOG("\tThread name: '%s', HWQP ID: %d\n",
				       spdk_thread_get_name(io->thread), io->hwqp_id);
		} else {
			SPDK_NOTICELOG("\tHWQP ID: %d\n", io->hwqp_id);
		}
		SPDK_NOTICELOG("\tNum of Conns: %d, State: %d\n", io->num_conns, io->state);
		if (io->fc_request_pool) {
			SPDK_NOTICELOG("\tRequest Pool Max Count: %d Avail Count: %zd\n",
				       io->rq_size, spdk_mempool_count(io->fc_request_pool));
		} else {
			SPDK_NOTICELOG("\tIO Queue %d Request Pool not present\n", i);
		}

		spdk_nvmf_fc_lld_ops.get_xchg_info(io, &xchg_info);
		SPDK_NOTICELOG("\tXCHG: Base=%d, Count=%d, Avail=%d\n", xchg_info.xchg_base,
			       xchg_info.xchg_total_count, xchg_info.xchg_avail_count);
		SPDK_NOTICELOG("\n");
	}
	SPDK_NOTICELOG("Num of Nports: %d\n", port->num_nports);
	TAILQ_FOREACH(nport, &port->nport_list, link) {
		SPDK_NOTICELOG("\tNport Hdl: %d, Nport State: %d\n", nport->nport_hdl, nport->nport_state);
	}
	if (port->io_rsrc_pool) {
		SPDK_NOTICELOG("\tIO Resource Pool Avail Count: %zd\n",
			       spdk_mempool_count(port->io_rsrc_pool));
	} else {
		SPDK_NOTICELOG("\tIO Resource Pool not present\n");
	}
	SPDK_NOTICELOG("\n");
	SPDK_NOTICELOG("\n*******************************\n");

out:
	free(arg1);
}

void
spdk_nvmf_fc_print_nport(void *arg1, void *arg2)
{
	/*
	 * *arg1=physical port id.
	 * *arg2=nport id.
	 */
	uint32_t *port_hdl = (uint32_t *)arg1;
	uint32_t *nport_hdl = (uint32_t *)arg2;
	struct spdk_nvmf_fc_nport *nport = spdk_nvmf_fc_nport_get(*port_hdl, *nport_hdl);
	struct spdk_nvmf_fc_remote_port_info *rport;
	struct spdk_nvmf_fc_association *association;
	struct spdk_nvmf_fc_conn *conn;

	if (nport == NULL) {
		SPDK_NOTICELOG("\nNport not found. Port Hdl: %d, Nport Hdl: %d\n", *port_hdl, *nport_hdl);
		goto out;
	}

	SPDK_NOTICELOG("\nNport Details. Port Hdl: %d, Nport Hdl: %d\n", *port_hdl, *nport_hdl);
	SPDK_NOTICELOG("\n*******************************\n");
	SPDK_NOTICELOG("Dest ID: 0x%x, State: %d\n", nport->d_id, nport->nport_state);
	SPDK_NOTICELOG("NodeName: 0x%lx, PortName: 0x%lx\n", from_be64(&nport->fc_nodename.u.wwn),
		       from_be64(&nport->fc_portname.u.wwn));
	SPDK_NOTICELOG("Remote Port Count: %d\n", nport->rport_count);

	TAILQ_FOREACH(rport, &nport->rem_port_list, link) {
		SPDK_NOTICELOG("\tSID: 0x%x, RPI: %d", rport->s_id, rport->rpi);
		SPDK_NOTICELOG(" Assoc Count: %d, State: %d\n", rport->assoc_count, rport->rport_state);
		SPDK_NOTICELOG("\tInit NodeName: 0x%lx, Init PortName: 0x%lx\n",
			       from_be64(&rport->fc_nodename.u.wwn), from_be64(&rport->fc_portname.u.wwn));
	}

	SPDK_NOTICELOG("Association Count: %d\n", nport->assoc_count);
	TAILQ_FOREACH(association, &nport->fc_associations, link) {
		SPDK_NOTICELOG("\tAssoc ID: 0x%lx, State: %d\n", association->assoc_id,
			       association->assoc_state);
		TAILQ_FOREACH(conn, &association->fc_conns, assoc_link) {
			SPDK_NOTICELOG("\t\tConn ID: 0x%lx, HWQP ID: %d, Outstanding IO Count: %d\n", conn->conn_id,
				       conn->hwqp->hwqp_id, conn->cur_queue_depth);
		}
	}

	SPDK_NOTICELOG("\n");

out:
	free(arg1);
	free(arg2);
}

static struct spdk_nvmf_fc_hwqp *
nvmf_fc_get_hwqp(uint32_t hwqp_id)
{
	struct spdk_nvmf_fc_port *port;

	/* Get the HWQP - a bit inefficient, but this is just a dump tool */
	for (int port_hdl = 0; port_hdl < SPDK_MAX_NUM_OF_FC_PORTS; port_hdl++) {
		port = spdk_nvmf_fc_port_list_get(port_hdl);
		if (port) {
			if (port->ls_queue.hwqp_id == hwqp_id) {
				return &(port->ls_queue);
			}
			for (uint32_t i = 0; i < port->num_io_queues; i++) {
				if (port->io_queues[i].hwqp_id == hwqp_id) {
					return &(port->io_queues[i]);
				}
			}
		}
	}
	return NULL;
}

void
spdk_nvmf_fc_print_hwqp(void *arg1, void *arg2)
{
	struct spdk_nvmf_fc_hwqp *hwqp;
	struct spdk_nvmf_fc_conn *conn;
	struct spdk_nvmf_fc_xchg_info xchg_info;

	/*
	 * *arg1=hwqp id.
	 */
	uint32_t *hwqp_id = (uint32_t *)arg1;
	hwqp = nvmf_fc_get_hwqp(*hwqp_id);

	if (!hwqp) {
		SPDK_NOTICELOG("\nHWQP not found. HWQP ID: %d\n", *hwqp_id);
		goto out;
	}

	SPDK_NOTICELOG("\nHWQP Details. Port Hdl: %d, HWQP ID: %d\n", hwqp->fc_port->port_hdl,
		       *hwqp_id);
	SPDK_NOTICELOG("\n*******************************\n");
	SPDK_NOTICELOG("Thread name: '%s', Num of Conns: %d\n",
		       spdk_thread_get_name(hwqp->thread), hwqp->num_conns);
	SPDK_NOTICELOG("State: %d,\n", hwqp->state);
	SPDK_NOTICELOG("Request Pool Max Count: %d Avail Count: %zd\n",
		       hwqp->rq_size, spdk_mempool_count(hwqp->fc_request_pool));
	spdk_nvmf_fc_lld_ops.get_xchg_info(hwqp, &xchg_info);
	SPDK_NOTICELOG("XCHG Base: %d, ", xchg_info.xchg_base);
	SPDK_NOTICELOG("XCHG Total Count: %d, ", xchg_info.xchg_total_count);
	SPDK_NOTICELOG("XCHG Avail Count: %d\n", xchg_info.xchg_avail_count);

	SPDK_NOTICELOG("Send Frame XCHG ID: %d Send Frame SeqID: %d\n", xchg_info.send_frame_xchg_id,
		       xchg_info.send_frame_seqid);
	TAILQ_FOREACH(conn, &hwqp->connection_list, link) {
		SPDK_NOTICELOG("\tConn ID: 0x%lx, HWQP ID: %d, Outstanding IO Count: %d\n", conn->conn_id,
			       conn->hwqp->hwqp_id, conn->cur_queue_depth);
	}
out:

	free(arg1);
}

void
spdk_nvmf_fc_print_assoc(void *arg1, void *arg2)
{
	/*
	 * *arg1=spdk_nvmf_fc_dump_assoc_id_args_t
	 */
	struct spdk_nvmf_fc_dump_assoc_id_args *args = arg1;
	uint8_t port_hdl = args->pport_handle;
	uint16_t nport_hdl = args->nport_handle;
	uint32_t assoc_id = args->assoc_id;
	struct spdk_nvmf_fc_association *association = NULL, *assoc;
	struct spdk_nvmf_fc_conn *conn;
	struct spdk_nvmf_fc_nport *nport = spdk_nvmf_fc_nport_get(port_hdl, nport_hdl);

	if (nport == NULL) {
		SPDK_NOTICELOG("\nNport not found. Port Hdl: %d, Nport Hdl: %d\n", port_hdl, nport_hdl);
		goto out;
	}

	TAILQ_FOREACH(assoc, &nport->fc_associations, link) {
		if (assoc_id == assoc->assoc_id) {
			association = assoc;
			break;
		}
	}
	if (association == NULL) {
		SPDK_NOTICELOG("\nAssociation not found. Port Hdl: %d, Nport Hdl: %d, Assoc ID: %d\n",
			       port_hdl, nport_hdl, assoc_id);
		goto out;
	}

	SPDK_NOTICELOG("\nAssociation Details. Port Hdl: %d, Nport Hdl: %d, Assoc ID: 0x%x\n",
		       port_hdl, nport_hdl, assoc_id);
	SPDK_NOTICELOG("State: %d, Connection Count: %d\n", association->assoc_state,
		       association->conn_count);
	TAILQ_FOREACH(conn, &association->fc_conns, assoc_link) {
		SPDK_NOTICELOG("\tConn ID: 0x%lx, HWQP ID: %d, Outstanding IO Count: %d\n", conn->conn_id,
			       conn->hwqp->hwqp_id, conn->cur_queue_depth);
	}
	SPDK_NOTICELOG("SID: 0x%x\n", association->s_id);
	SPDK_NOTICELOG("Rport SID: 0x%x, Rport RPI: 0x%x\n", association->rport->s_id,
		       association->rport->rpi);
	SPDK_NOTICELOG("Rport State: %d,\n", association->rport->rport_state);
	SPDK_NOTICELOG("Init NodeName: 0x%lx, Init PortName: 0x%lx\n",
		       from_be64(&association->rport->fc_nodename.u.wwn),
		       from_be64(&association->rport->fc_portname.u.wwn));
	SPDK_NOTICELOG("Init NQN: %s\n", association->host->nqn);
	SPDK_NOTICELOG("Init Host ID: %s\n", association->host_id);
	SPDK_NOTICELOG("Init Host NQN: %s\n", association->host_nqn);
	SPDK_NOTICELOG("Init Subsystem NQN: %s\n", association->sub_nqn);
	SPDK_NOTICELOG("Subsystem NQN: %s\n", association->subsystem->subnqn);
	SPDK_NOTICELOG("Subsystem ID: %d, State: %d\n", association->subsystem->id,
		       association->subsystem->state);

out:

	free(arg1);
}

void
spdk_nvmf_fc_print_conn(void *arg1, void *arg2)
{
	/*
	 * *arg1=hwqp id.
	 * *arg2=conn-id
	 */
	uint32_t *hwqp_id = (uint32_t *)arg1;
	uint32_t *conn_id = (uint32_t *)arg2;

	struct spdk_nvmf_fc_hwqp *hwqp;
	struct spdk_nvmf_fc_conn *connection = NULL;
	struct spdk_nvmf_fc_conn *conn;

	hwqp = nvmf_fc_get_hwqp(*hwqp_id);

	if (!hwqp) {
		SPDK_NOTICELOG("\nHWQP not found. HWQP ID: %d\n", *hwqp_id);
		goto out;
	}

	TAILQ_FOREACH(conn, &hwqp->connection_list, link) {
		if (conn->conn_id == *conn_id) {
			connection = conn;
			break;
		}
	}

	if (connection == NULL) {
		SPDK_NOTICELOG("\nConnection not found. HWQP ID: %d, Conn ID: %d\n", *hwqp_id, *conn_id);
		goto out;
	}


	SPDK_NOTICELOG("\nConnection Details. HWQP ID: %d, Conn ID: 0x%x\n", *hwqp_id, *conn_id);
	SPDK_NOTICELOG("Conn ID: 0x%lx, Outstanding IO Count: %d\n", connection->conn_id,
		       connection->cur_queue_depth);
	SPDK_NOTICELOG("Assoc ID: 0x%lx\n", connection->fc_assoc->assoc_id);
	SPDK_NOTICELOG("SQ Head: %d, SQ Head Max: %d, QID: 0x%x\n", connection->qpair.sq_head,
		       connection->qpair.sq_head_max, connection->qpair.qid);
	SPDK_NOTICELOG("Ersp Ratio: %d, Rsp Count: %d, Rsn: %d\n", connection->esrp_ratio,
		       connection->rsp_count, connection->rsn);
	SPDK_NOTICELOG("Max Queue Depth: %d, Max RW Depth: %d, Current RW Depth: %d\n",
		       connection->max_queue_depth, connection->max_rw_depth, connection->cur_fc_rw_depth);

out:

	free(arg1);
	free(arg2);
}
