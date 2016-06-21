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

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <inttypes.h>
#include <unistd.h>

#include <rte_config.h>
#include <rte_debug.h>
#include <rte_mempool.h>
#include <rte_cycles.h>
#include <rte_timer.h>

#include "nvmf.h"
#include "spdk/nvmf_spec.h"
#include "conn.h"
#include "rdma.h"
#include "session.h"
#include "spdk/queue.h"
#include "spdk/log.h"
#include "spdk/trace.h"


/** \file

*/

static rte_atomic32_t g_num_connections[RTE_MAX_LCORE];

static int g_max_conns;
struct spdk_nvmf_conn *g_conns_array;
char g_shm_name[64];
int g_conns_array_fd;

pthread_mutex_t g_conns_mutex;

struct rte_timer g_shutdown_timer;

static int nvmf_allocate_reactor(uint64_t cpumask);
static void spdk_nvmf_conn_do_work(void *arg);

static void
nvmf_active_tx_desc(struct nvme_qp_tx_desc *tx_desc)
{
	struct spdk_nvmf_conn *conn;

	RTE_VERIFY(tx_desc != NULL);
	conn = tx_desc->conn;
	RTE_VERIFY(conn != NULL);

	STAILQ_REMOVE(&conn->qp_tx_desc, tx_desc, nvme_qp_tx_desc, link);
	STAILQ_INSERT_TAIL(&conn->qp_tx_active_desc, tx_desc, link);
}

static void
nvmf_deactive_tx_desc(struct nvme_qp_tx_desc *tx_desc)
{
	struct spdk_nvmf_conn *conn;

	RTE_VERIFY(tx_desc != NULL);
	conn = tx_desc->conn;
	RTE_VERIFY(tx_desc->conn != NULL);

	STAILQ_REMOVE(&conn->qp_tx_active_desc, tx_desc, nvme_qp_tx_desc, link);
	STAILQ_INSERT_TAIL(&conn->qp_tx_desc, tx_desc, link);
}

static struct spdk_nvmf_conn *
allocate_conn(void)
{
	struct spdk_nvmf_conn	*conn;
	int				i;

	pthread_mutex_lock(&g_conns_mutex);
	for (i = 0; i < g_max_conns; i++) {
		conn = &g_conns_array[i];
		if (!conn->is_valid) {
			memset(conn, 0, sizeof(*conn));
			conn->is_valid = 1;
			pthread_mutex_unlock(&g_conns_mutex);
			return conn;
		}
	}
	pthread_mutex_unlock(&g_conns_mutex);

	return NULL;
}

static void
free_conn(struct spdk_nvmf_conn *conn)
{
	conn->sess = NULL;
	conn->cm_id = 0;
	conn->is_valid = 0;
}

struct spdk_nvmf_conn *
spdk_find_nvmf_conn_by_cm_id(struct rdma_cm_id *cm_id)
{
	int i;

	for (i = 0; i < g_max_conns; i++) {
		if ((g_conns_array[i].is_valid == 1) &&
		    (g_conns_array[i].cm_id == cm_id)) {
			return &g_conns_array[i];
		}
	}

	return NULL;
}

static struct spdk_nvmf_conn *
spdk_find_nvmf_conn_by_cntlid(int cntlid)
{
	int i;

	for (i = 0; i < g_max_conns; i++) {
		if ((g_conns_array[i].is_valid == 1) &&
		    (g_conns_array[i].cntlid == cntlid) &&
		    (g_conns_array[i].qid == 0)) {
			return &g_conns_array[i];
		}
	}

	return NULL;
}

int spdk_initialize_nvmf_conns(int max_connections)
{
	size_t conns_size;
	int i, rc;

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Enter\n");

	rc = pthread_mutex_init(&g_conns_mutex, NULL);
	if (rc != 0) {
		SPDK_ERRLOG("mutex_init() failed\n");
		return -1;
	}

	sprintf(g_shm_name, "nvmf_conns.%d", spdk_app_get_instance_id());
	g_conns_array_fd = shm_open(g_shm_name, O_RDWR | O_CREAT, 0600);
	if (g_conns_array_fd < 0) {
		SPDK_ERRLOG("could not shm_open %s\n", g_shm_name);
		return -1;
	}

	g_max_conns = max_connections;
	conns_size = sizeof(struct spdk_nvmf_conn) * g_max_conns;

	if (ftruncate(g_conns_array_fd, conns_size) != 0) {
		SPDK_ERRLOG("could not ftruncate\n");
		shm_unlink(g_shm_name);
		close(g_conns_array_fd);
		return -1;
	}
	g_conns_array = mmap(0, conns_size, PROT_READ | PROT_WRITE, MAP_SHARED,
			     g_conns_array_fd, 0);

	memset(g_conns_array, 0, conns_size);

	for (i = 0; i < RTE_MAX_LCORE; i++) {
		rte_atomic32_set(&g_num_connections[i], 0);
	}

	return 0;
}

struct spdk_nvmf_conn *
spdk_nvmf_allocate_conn(void)
{
	struct spdk_nvmf_conn *conn;

	conn = allocate_conn();
	if (conn == NULL) {
		SPDK_ERRLOG("Could not allocate new connection.\n");
		goto err0;
	}

	/* all new connections initially default as AQ until nvmf connect */
	conn->type = CONN_TYPE_AQ;

	/* no session association until nvmf connect */
	conn->sess = NULL;

	conn->state = CONN_STATE_INVALID;
	conn->sq_head = conn->sq_tail = 0;

	return conn;

err0:
	return NULL;
}

/**

\brief Create an NVMf fabric connection from the given parameters and schedule it
       on a reactor thread.

\code

# identify reactor where the new connections work item will be scheduled
reactor = nvmf_allocate_reactor()
schedule fabric connection work item on reactor

\endcode

*/
int
spdk_nvmf_startup_conn(struct spdk_nvmf_conn *conn)
{
	int lcore;
	struct spdk_nvmf_conn *admin_conn;
	uint64_t nvmf_session_core = spdk_app_get_core_mask();

	/*
	 * if starting IO connection then determine core
	 * allocated to admin queue to request core mask.
	 * Can not assume nvmf session yet created at time
	 * of fabric connection setup.  Rely on fabric
	 * function to locate matching controller session.
	 */
	if (conn->type == CONN_TYPE_IOQ && conn->cntlid != 0) {
		admin_conn = spdk_find_nvmf_conn_by_cntlid(conn->cntlid);
		if (admin_conn != NULL) {
			SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Located admin conn session core %d\n",
				      admin_conn->poller.lcore);
			nvmf_session_core = 1ULL << admin_conn->poller.lcore;
		}
	}

	lcore = nvmf_allocate_reactor(nvmf_session_core);
	if (lcore < 0) {
		SPDK_ERRLOG("Unable to find core to launch connection.\n");
		goto err0;
	}

	conn->state = CONN_STATE_RUNNING;
	SPDK_NOTICELOG("Launching nvmf connection[qid=%d] on core: %d\n",
		       conn->qid, lcore);
	conn->poller.fn = spdk_nvmf_conn_do_work;
	conn->poller.arg = conn;

	rte_atomic32_inc(&g_num_connections[lcore]);
	spdk_poller_register(&conn->poller, lcore, NULL);

	return 0;
err0:
	free_conn(conn);
	return -1;
}

static void
_conn_destruct(spdk_event_t event)
{
	struct spdk_nvmf_conn *conn = spdk_event_get_arg1(event);

	/*
	 * Notify NVMf library of the fabric connection
	 * going away.  If this is the AQ connection then
	 * set state for other connections to abort.
	 */
	nvmf_disconnect((void *)conn, conn->sess);

	if (conn->type == CONN_TYPE_AQ) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "AQ connection destruct, trigger session closure\n");
		/* Trigger all I/O connections to shutdown */
		conn->state = CONN_STATE_FABRIC_DISCONNECT;
	}

	nvmf_rdma_conn_cleanup(conn);

	pthread_mutex_lock(&g_conns_mutex);
	free_conn(conn);
	pthread_mutex_unlock(&g_conns_mutex);
}

static void spdk_nvmf_conn_destruct(struct spdk_nvmf_conn *conn)
{
	struct spdk_event *event;

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "conn %p\n", conn);
	conn->state = CONN_STATE_INVALID;

	event = spdk_event_allocate(rte_lcore_id(), _conn_destruct, conn, NULL, NULL);
	spdk_poller_unregister(&conn->poller, event);
	rte_atomic32_dec(&g_num_connections[rte_lcore_id()]);
}

static int
spdk_nvmf_get_active_conns(void)
{
	struct spdk_nvmf_conn *conn;
	int num = 0;
	int i;

	pthread_mutex_lock(&g_conns_mutex);
	for (i = 0; i < g_max_conns; i++) {
		conn = &g_conns_array[i];
		if (!conn->is_valid)
			continue;
		num++;
	}
	pthread_mutex_unlock(&g_conns_mutex);
	return num;
}

static void
spdk_nvmf_cleanup_conns(void)
{
	munmap(g_conns_array, sizeof(struct spdk_nvmf_conn) * g_max_conns);
	shm_unlink(g_shm_name);
	close(g_conns_array_fd);
}

static void
spdk_nvmf_conn_check_shutdown(struct rte_timer *timer, void *arg)
{
	if (spdk_nvmf_get_active_conns() == 0) {
		RTE_VERIFY(timer == &g_shutdown_timer);
		rte_timer_stop(timer);
		spdk_nvmf_cleanup_conns();
		spdk_app_stop(0);
	}
}

void spdk_shutdown_nvmf_conns(void)
{
	struct spdk_nvmf_conn	*conn;
	int				i;

	pthread_mutex_lock(&g_conns_mutex);

	for (i = 0; i < g_max_conns; i++) {
		conn = &g_conns_array[i];
		if (!conn->is_valid)
			continue;
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Set conn %d state to exiting\n", i);
		conn->state = CONN_STATE_EXITING;
	}

	pthread_mutex_unlock(&g_conns_mutex);
	rte_timer_init(&g_shutdown_timer);
	rte_timer_reset(&g_shutdown_timer, rte_get_timer_hz() / 1000, PERIODICAL,
			rte_get_master_lcore(), spdk_nvmf_conn_check_shutdown, NULL);
}

static int
spdk_nvmf_send_response(struct spdk_nvmf_conn *conn, struct nvmf_request *req)
{
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	/* Zero out fields reserved in NVMf */
	rsp->sqid = 0;
	rsp->status.p = 0;

	rsp->sqhd = conn->sq_head;
	rsp->cid = req->cid;

	SPDK_TRACELOG(SPDK_TRACE_NVMF,
		      "cpl: cdw0=0x%x rsvd1=0x%x sqhd=0x%x sqid=0x%x cid=0x%x status=0x%x\n",
		      rsp->cdw0, rsp->rsvd1, rsp->sqhd, rsp->sqid, rsp->cid, *(uint16_t *)&rsp->status);

	return nvmf_post_rdma_send(conn, req->fabric_tx_ctx);
}

static int
nvmf_io_cmd_continue(struct spdk_nvmf_conn *conn, struct nvmf_request *req)
{
	int ret;

	/* send to NVMf library for backend NVMe processing */
	ret = nvmf_process_io_cmd(req);
	if (ret) {
		/* library failed the request and should have
		   Updated the response */
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, " send nvme io cmd capsule error response\n");
		ret = spdk_nvmf_send_response(conn, req);
		if (ret) {
			SPDK_ERRLOG("Unable to send aq qp tx descriptor\n");
			return -1;
		}
	}
	return 0;
}

static void
nvmf_process_async_completion(struct nvmf_request *req)
{
	struct nvme_qp_tx_desc *tx_desc = (struct nvme_qp_tx_desc *)req->fabric_tx_ctx;
	struct spdk_nvme_cpl *response;
	struct nvme_qp_rx_desc *rx_desc = tx_desc->rx_desc;
	int ret;

	response = &req->rsp->nvme_cpl;

	/* Was the command successful */
	if (response->status.sc == SPDK_NVME_SC_SUCCESS &&
	    req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		/* data to be copied to host via memory RDMA */

		/* temporarily adjust SGE to only copy what the host is prepared to receive. */
		rx_desc->bb_sgl.length = req->length;

		ret = nvmf_post_rdma_write(tx_desc->conn, tx_desc);
		if (ret) {
			SPDK_ERRLOG("Unable to post rdma write tx descriptor\n");
			goto command_fail;
		}
	}

	/* Now send back the response */
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "send nvme cmd capsule response\n");
	ret = spdk_nvmf_send_response(tx_desc->conn, req);
	if (ret) {
		SPDK_ERRLOG("Unable to send aq qp tx descriptor\n");
		goto command_fail;
	}

	return;

command_fail:
	nvmf_deactive_tx_desc(tx_desc);
}

static int
nvmf_process_property_get(struct spdk_nvmf_conn *conn,
			  struct nvmf_request *req)
{
	struct spdk_nvmf_fabric_prop_get_rsp *response;
	struct spdk_nvmf_fabric_prop_get_cmd *cmd;
	int	ret;

	cmd = &req->cmd->prop_get_cmd;
	response = &req->rsp->prop_get_rsp;

	nvmf_property_get(conn->sess, cmd, response);

	/* send the nvmf response if setup by NVMf library */
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "send property get capsule response\n");
	ret = spdk_nvmf_send_response(conn, req);
	if (ret) {
		SPDK_ERRLOG("Unable to send aq qp tx descriptor\n");
		return -1;
	}

	return 0;
}

static int
nvmf_process_property_set(struct spdk_nvmf_conn *conn,
			  struct nvmf_request *req)
{
	struct spdk_nvmf_fabric_prop_set_rsp *response;
	struct spdk_nvmf_fabric_prop_set_cmd *cmd;
	bool	shutdown = false;
	int	ret;

	cmd = &req->cmd->prop_set_cmd;
	response = &req->rsp->prop_set_rsp;

	nvmf_property_set(conn->sess, cmd, response, &shutdown);
	if (shutdown == true) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Call to set properties has indicated shutdown\n");
		conn->state = CONN_STATE_FABRIC_DISCONNECT;
	}

	/* send the nvmf response if setup by NVMf library */
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "send property set capsule response\n");
	ret = spdk_nvmf_send_response(conn, req);
	if (ret) {
		SPDK_ERRLOG("Unable to send aq qp tx descriptor\n");
		return -1;
	}

	return 0;
}

/* Check the nvmf message received */
static void nvmf_trace_command(struct spdk_nvmf_capsule_cmd *cap_hdr, enum conn_type conn_type)
{
	struct spdk_nvme_cmd *cmd = (struct spdk_nvme_cmd *)cap_hdr;
	struct spdk_nvme_sgl_descriptor *sgl = &cmd->dptr.sgl1;
	uint8_t opc;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "NVMf %s%s Command:\n",
		      conn_type == CONN_TYPE_AQ ? "Admin" : "I/O",
		      cmd->opc == SPDK_NVMF_FABRIC_OPCODE ? " Fabrics" : "");

	if (cmd->opc == SPDK_NVMF_FABRIC_OPCODE) {
		opc = cap_hdr->fctype;
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "	SQE:  fctype 0x%02x\n", cap_hdr->fctype);
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "	SQE:  cid 0x%x\n", cap_hdr->cid);
	} else {
		opc = cmd->opc;
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "	SQE:  opc 0x%02x\n", cmd->opc);
		if (cmd->fuse) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "	SQE:  fuse %x\n", cmd->fuse);
		}
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "	SQE:  psdt %u\n", cmd->psdt);
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "	SQE:  cid 0x%x\n", cmd->cid);
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "	SQE:  nsid %u\n", cmd->nsid);
		if (cmd->mptr) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "	SQE:  mptr 0x%" PRIx64 "\n", cmd->mptr);
		}
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "	SQE:  cdw10 0x%08x\n", cmd->cdw10);
	}

	if (spdk_nvme_opc_get_data_transfer(opc) != SPDK_NVME_DATA_NONE) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "	SQE:  SGL type 0x%x\n", sgl->type);
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "	SQE:  SGL subtype 0x%x\n", sgl->type_specific);
		if (sgl->type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK) {

			SPDK_TRACELOG(SPDK_TRACE_NVMF, "	SQE:  SGL address 0x%lx\n",
				      ((struct spdk_nvmf_keyed_sgl_descriptor *)sgl)->address);
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "	SQE:  SGL key 0x%x\n",
				      ((struct spdk_nvmf_keyed_sgl_descriptor *)sgl)->key);
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "	SQE:  SGL length 0x%x\n",
				      ((struct spdk_nvmf_keyed_sgl_descriptor *)sgl)->length);
		} else if (sgl->type == SPDK_NVME_SGL_TYPE_DATA_BLOCK) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "	SQE:  SGL %s 0x%" PRIx64 "\n",
				      sgl->type_specific == SPDK_NVME_SGL_SUBTYPE_OFFSET ? "offset" : "address",
				      sgl->address);
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "	SQE:  SGL length 0x%x\n", sgl->length);
		}
	}
}

static int
nvmf_process_io_command(struct spdk_nvmf_conn *conn,
			struct nvme_qp_tx_desc *tx_desc)
{
	struct nvme_qp_rx_desc *rx_desc = tx_desc->rx_desc;
	struct nvmf_request *req;
	struct spdk_nvme_sgl_descriptor *sgl;
	struct spdk_nvmf_keyed_sgl_descriptor *keyed_sgl;
	struct spdk_nvme_cmd *cmd;
	enum spdk_nvme_data_transfer xfer;
	int	ret;

	req = &tx_desc->req_state;
	cmd = &req->cmd->nvme_cmd;
	sgl = (struct spdk_nvme_sgl_descriptor *)&cmd->dptr.sgl1;
	keyed_sgl = (struct spdk_nvmf_keyed_sgl_descriptor *)sgl;

	xfer = spdk_nvme_opc_get_data_transfer(cmd->opc);
	if (xfer != SPDK_NVME_DATA_NONE) {
		/*
		  NVMf does support in-capsule data for write comamnds.  If caller indicates SGL,
		  verify the SGL for in-capsule or RDMA read/write use and prepare
		  data buffer reference and length for the NVMf library.
		*/
		/* TBD: add code to handle I/O larger than default bb size */
		if (sgl->type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK &&
		    (sgl->type_specific == SPDK_NVME_SGL_SUBTYPE_ADDRESS ||
		     sgl->type_specific == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY)) {
			if (keyed_sgl->key == 0) {
				SPDK_ERRLOG("Host did not specify SGL key!\n");
				goto command_fail;
			}

			if (keyed_sgl->length > rx_desc->bb_sgl.length) {
				SPDK_ERRLOG("SGL length 0x%x exceeds BB length 0x%x\n",
					    (uint32_t)keyed_sgl->length, rx_desc->bb_sgl.length);
				goto command_fail;
			}

			req->data = rx_desc->bb;
			req->remote_addr = keyed_sgl->address;
			req->rkey = keyed_sgl->key;
			req->length = keyed_sgl->length;
		}  else if (sgl->type == SPDK_NVME_SGL_TYPE_DATA_BLOCK &&
			    sgl->type_specific == SPDK_NVME_SGL_SUBTYPE_OFFSET) {
			uint64_t offset = sgl->address;
			uint32_t max_len = rx_desc->bb_sgl.length;

			if (offset > max_len) {
				SPDK_ERRLOG("In-capsule offset 0x%" PRIx64 " exceeds capsule length 0x%x\n",
					    offset, max_len);
				goto command_fail;
			}
			max_len -= (uint32_t)offset;

			if (sgl->length > max_len) {
				SPDK_ERRLOG("In-capsule data length 0x%x exceeds capsule length 0x%x\n",
					    sgl->length, max_len);
				goto command_fail;
			}

			req->data = rx_desc->bb + offset;
			req->length = sgl->length;
		} else {
			SPDK_ERRLOG("Invalid NVMf I/O Command SGL:  Type %2x, Subtype %2x\n",
				    sgl->type, sgl->type_specific);
			goto command_fail;
		}

		if (req->length == 0) {
			xfer = SPDK_NVME_DATA_NONE;
		}

		req->xfer = xfer;

		/* for any I/O that requires rdma data to be
		   pulled into target BB before processing by
		   the backend NVMe device
		*/
		if (xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
			if (sgl->type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK) {
				SPDK_TRACELOG(SPDK_TRACE_RDMA, "	Issuing RDMA Read to get host data\n");
				/* data to be copied from remote host via memory RDMA */

				/* temporarily adjust SGE to only copy what the host is prepared to send. */
				rx_desc->bb_sgl.length = req->length;

				req->pending = NVMF_PENDING_WRITE;
				ret = nvmf_post_rdma_read(tx_desc->conn, tx_desc);
				if (ret) {
					SPDK_ERRLOG("Unable to post rdma read tx descriptor\n");
					goto command_fail;
				}
				/* Need to wait for RDMA completion indication where
				it will continue I/O operation */
				return 0;
			}
		}
	}

	/* send to NVMf library for backend NVMe processing */
	ret = nvmf_process_io_cmd(req);
	if (ret) {
		/* library failed the request and should have
		   Updated the response */
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "send nvme io cmd capsule error response\n");
		ret = spdk_nvmf_send_response(conn, req);
		if (ret) {
			SPDK_ERRLOG("Unable to send aq qp tx descriptor\n");
			goto command_fail;
		}
	}

	return 0;

command_fail:
	return -1;
}

static int
nvmf_process_admin_command(struct spdk_nvmf_conn *conn,
			   struct nvme_qp_tx_desc *tx_desc)
{
	struct nvme_qp_rx_desc *rx_desc = tx_desc->rx_desc;
	struct nvmf_request *req;
	struct spdk_nvme_cmd *cmd;
	struct spdk_nvme_sgl_descriptor *sgl;
	struct spdk_nvmf_keyed_sgl_descriptor *keyed_sgl;
	int	ret;

	req = &tx_desc->req_state;
	cmd = &req->cmd->nvme_cmd;
	sgl = (struct spdk_nvme_sgl_descriptor *)&cmd->dptr.sgl1;
	keyed_sgl = (struct spdk_nvmf_keyed_sgl_descriptor *)sgl;

	/*
	  NVMf does not support in-capsule data for admin command or response capsules.
	  If caller indicates SGL for return RDMA data, verify the SGL and prepare
	  data buffer reference and length for the NVMf library.  Only keyed type
	  SGLs are supported for return data
	 */
	if (sgl->type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK &&
	    (sgl->type_specific == SPDK_NVME_SGL_SUBTYPE_ADDRESS ||
	     sgl->type_specific == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY)) {
		req->data = rx_desc->bb;
		req->remote_addr = keyed_sgl->address;
		req->rkey = keyed_sgl->key;
		req->length = keyed_sgl->length;
		if (req->length) {
			req->xfer = spdk_nvme_opc_get_data_transfer(cmd->opc);
		}
	}

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "	tx_desc %p: req_state %p, rsp %p, addr %p\n",
		      tx_desc, req, (void *)req->rsp, (void *)tx_desc->send_sgl.addr);

	/* send to NVMf library for backend NVMe processing */
	ret = nvmf_process_admin_cmd(req);
	if (ret) {
		/* library failed the request and should have
		   Updated the response */
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "send nvme admin cmd capsule sync response\n");
		ret = spdk_nvmf_send_response(conn, req);
		if (ret) {
			SPDK_ERRLOG("Unable to send aq qp tx descriptor\n");
			goto command_fail;
		}
	}

	return 0;

command_fail:
	return -1;
}

static void
nvmf_init_conn_properites(struct spdk_nvmf_conn *conn,
			  struct nvmf_session *session,
			  struct spdk_nvmf_fabric_connect_rsp *response)
{

	struct spdk_nvmf_extended_identify_ctrlr_data *lcdata;
	uint32_t mdts;

	conn->cntlid = response->status_code_specific.success.cntlid;
	session->max_connections_allowed = g_nvmf_tgt.MaxConnectionsPerSession;
	nvmf_init_session_properties(session, conn->sq_depth);

	/* Update the session logical controller data with any
	 * application fabric side limits
	 */
	/* reset mdts in vcdata to equal the application default maximum */
	mdts = SPDK_NVMF_MAX_RECV_DATA_TRANSFER_SIZE /
	       (1 << (12 + session->vcprop.cap_hi.bits.mpsmin));
	if (mdts == 0) {
		SPDK_ERRLOG("Min page size exceeds max transfer size!\n");
		SPDK_ERRLOG("Verify setting of SPDK_NVMF_MAX_RECV_DATA_TRANSFER_SIZE and mpsmin\n");
		session->vcdata.mdts = 1; /* Support single page for now */
	} else {
		/* set mdts as a power of 2 representing number of mpsmin units */
		session->vcdata.mdts = 0;
		while ((1ULL << session->vcdata.mdts) < mdts) {
			session->vcdata.mdts++;
		}
	}

	/* increase the I/O recv capsule size for in_capsule data */
	lcdata = (struct spdk_nvmf_extended_identify_ctrlr_data *)&session->vcdata.reserved5[1088];
	lcdata->ioccsz += (g_nvmf_tgt.MaxInCapsuleData / 16);

}

static int
nvmf_connect_continue(struct spdk_nvmf_conn *conn,
		      struct nvmf_request *req)
{
	struct spdk_nvmf_fabric_connect_cmd *connect;
	struct spdk_nvmf_fabric_connect_data *connect_data;
	struct spdk_nvmf_fabric_connect_rsp *response;
	struct nvmf_session *session;
	int ret;

	connect = &req->cmd->connect_cmd;
	connect_data = (struct spdk_nvmf_fabric_connect_data *)req->data;

	RTE_VERIFY(connect_data != NULL);

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    *** Connect Capsule Data *** %p\n", connect_data);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    *** cntlid  = %x ***\n", connect_data->cntlid);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    *** hostid = %04x%04x-%04x-%04x-%04x-%04x%04x%04x ***\n",
		      htons(*(unsigned short *) &connect_data->hostid[0]),
		      htons(*(unsigned short *) &connect_data->hostid[2]),
		      htons(*(unsigned short *) &connect_data->hostid[4]),
		      htons(*(unsigned short *) &connect_data->hostid[6]),
		      htons(*(unsigned short *) &connect_data->hostid[8]),
		      htons(*(unsigned short *) &connect_data->hostid[10]),
		      htons(*(unsigned short *) &connect_data->hostid[12]),
		      htons(*(unsigned short *) &connect_data->hostid[14]));
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    *** subsiqn = %s ***\n", (char *)&connect_data->subnqn[0]);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    *** hostiqn = %s ***\n", (char *)&connect_data->hostnqn[0]);

	response = &req->rsp->connect_rsp;

	session = nvmf_connect((void *)conn, connect, connect_data, response);
	if (session != NULL) {
		conn->sess = session;
		conn->qid = connect->qid;
		if (connect->qid > 0) {
			conn->type = CONN_TYPE_IOQ; /* I/O Connection */
		} else {
			/* When session first created, set some attributes */
			nvmf_init_conn_properites(conn, session, response);
		}
	}

	/* synchronous call, nvmf library expected to init
	   response status.
	 */
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "send connect capsule response\n");
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    *** cntlid  = %x ***\n",
		      response->status_code_specific.success.cntlid);
	ret = spdk_nvmf_send_response(conn, req);
	if (ret) {
		SPDK_ERRLOG("Unable to send aq qp tx descriptor\n");
		return ret;
	}

	return 0;
}

static int
nvmf_process_connect(struct spdk_nvmf_conn *conn,
		     struct nvme_qp_tx_desc *tx_desc)
{
	struct spdk_nvmf_fabric_connect_cmd *connect;
	struct nvmf_request *req = &tx_desc->req_state;
	struct nvme_qp_rx_desc *rx_desc = tx_desc->rx_desc;
	union sgl_shift *sgl;
	int	ret;

	connect = &req->cmd->connect_cmd;
	sgl = (union sgl_shift *)&connect->sgl1;

	/* debug - display the connect capsule */
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    *** Connect Capsule *** %p\n", connect);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    *** cid              = %x ***\n", connect->cid);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    *** recfmt           = %x ***\n", connect->recfmt);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    *** qid              = %x ***\n", connect->qid);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    *** sqsize           = %x ***\n", connect->sqsize);

	if (sgl->nvmf_sgl.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK &&
	    sgl->nvmf_sgl.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET) {
		/*
		  Extended data was passed by initiator to target via in-capsule
		  data and not via RDMA SGL xfer.  So extended data resides in
		  the rx message buffer
		*/
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "	Using In-Capsule connect data\n");
		if (rx_desc->recv_bc < (sizeof(struct spdk_nvmf_fabric_connect_cmd) +
					sizeof(struct spdk_nvmf_fabric_connect_data))) {
			SPDK_ERRLOG("insufficient in-capsule data to satisfy connect!\n");
			goto connect_fail;
		}
		req->data = rx_desc->bb;
		req->length = sgl->nvmf_sgl.length;
		return nvmf_connect_continue(conn, req);
	} else if (sgl->nvmf_sgl.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK &&
		   (sgl->nvmf_sgl.subtype == SPDK_NVME_SGL_SUBTYPE_ADDRESS ||
		    sgl->nvmf_sgl.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY)) {
		/* setup a new SQE that uses local bounce buffer */
		req->data = rx_desc->bb;
		req->remote_addr = sgl->nvmf_sgl.address;
		req->rkey = sgl->nvmf_sgl.key;
		req->pending = NVMF_PENDING_CONNECT;
		req->length = sgl->nvmf_sgl.length;
		req->xfer = SPDK_NVME_DATA_HOST_TO_CONTROLLER;

		SPDK_TRACELOG(SPDK_TRACE_RDMA, "	Issuing RDMA Read to get host connect data\n");
		/* data to be copied from host via memory RDMA */

		/* temporarily adjust SGE to only copy what the host is prepared to send. */
		rx_desc->bb_sgl.length = req->length;

		ret = nvmf_post_rdma_read(tx_desc->conn, tx_desc);
		if (ret) {
			SPDK_ERRLOG("Unable to post rdma read tx descriptor\n");
			goto connect_fail;
		}
		/* Need to wait for RDMA completion indication where
		   it will continue connect operation */
	} else {
		SPDK_ERRLOG("Invalid NVMf Connect SGL:  Type %2x, Subtype %2x\n",
			    sgl->nvmf_sgl.type, sgl->nvmf_sgl.subtype);
		goto connect_fail;
	}
	return 0;

connect_fail:
	return -1;
}

static int
nvmf_process_fabrics_command(struct spdk_nvmf_conn *conn, struct nvme_qp_tx_desc *tx_desc)
{
	struct nvmf_request *req = &tx_desc->req_state;
	struct nvme_qp_rx_desc *rx_desc = tx_desc->rx_desc;
	struct spdk_nvmf_capsule_cmd *cap_hdr;

	cap_hdr = (struct spdk_nvmf_capsule_cmd *)&rx_desc->msg_buf;

	switch (cap_hdr->fctype) {
	case SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET:
		return nvmf_process_property_set(conn, req);
	case SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET:
		return nvmf_process_property_get(conn, req);
	case SPDK_NVMF_FABRIC_COMMAND_CONNECT:
		return nvmf_process_connect(conn, tx_desc);
	default:
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "recv capsule header type invalid [%x]!\n",
			      cap_hdr->fctype);
		return 1; /* skip, do nothing */
	}
}

static int nvmf_recv(struct spdk_nvmf_conn *conn, struct ibv_wc *wc)
{
	struct nvme_qp_rx_desc *rx_desc;
	struct nvme_qp_tx_desc *tx_desc = NULL;
	struct spdk_nvmf_capsule_cmd *cap_hdr;
	struct nvmf_request *req;
	int ret = 0;

	rx_desc = (struct nvme_qp_rx_desc *)wc->wr_id;
	cap_hdr = (struct spdk_nvmf_capsule_cmd *)&rx_desc->msg_buf;

	/* Update Connection SQ Tracking, increment
	   the SQ tail consuming a free RX recv slot.
	   Check for exceeding queue full - should
	   never happen.
	*/
	conn->sq_tail < (conn->sq_depth - 1) ? (conn->sq_tail++) : (conn->sq_tail = 0);
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "sq_head %x, sq_tail %x, sq_depth %x\n",
		      conn->sq_head, conn->sq_tail, conn->sq_depth);
	/* trap if initiator exceeds qdepth */
	if (conn->sq_head == conn->sq_tail) {
		SPDK_ERRLOG("	*** SQ Overflow !! ***\n");
		/* controller fatal status condition:
		   set the cfs flag in controller status
		   and stop processing this and any I/O
		   on this queue.
		*/
		if (conn->sess) {
			conn->sess->vcprop.csts.bits.cfs = 1;
			conn->state = CONN_STATE_OVERFLOW;
		}
		if (conn->type == CONN_TYPE_IOQ) {
			/* if overflow on the I/O queue
			   stop processing, allow for
			   remote host to query failure
			   via admin queue
			 */
			goto drop_recv;
		} else {
			/* if overflow on the admin queue
			   there is no recovery, error out
			   to trigger disconnect
			 */
			goto recv_error;
		}
	}

	if (wc->byte_len < sizeof(*cap_hdr)) {
		SPDK_ERRLOG("recv length less than capsule header\n");
		goto recv_error;
	}
	rx_desc->recv_bc = wc->byte_len;
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "recv byte count %x\n", rx_desc->recv_bc);

	/* get a response buffer */
	if (STAILQ_EMPTY(&conn->qp_tx_desc)) {
		SPDK_ERRLOG("tx desc pool empty!\n");
		goto recv_error;
	}
	tx_desc = STAILQ_FIRST(&conn->qp_tx_desc);
	nvmf_active_tx_desc(tx_desc);
	tx_desc->rx_desc = rx_desc;

	req = &tx_desc->req_state;
	req->session = conn->sess;
	req->fabric_tx_ctx = tx_desc;
	req->fabric_rx_ctx = rx_desc;
	req->cb_fn = nvmf_process_async_completion;
	req->length = 0;
	req->xfer = SPDK_NVME_DATA_NONE;
	req->data = NULL;
	req->cid = cap_hdr->cid;
	req->cmd = &rx_desc->msg_buf;

	nvmf_trace_command(cap_hdr, conn->type);

	if (cap_hdr->opcode == SPDK_NVMF_FABRIC_OPCODE) {
		ret = nvmf_process_fabrics_command(conn, tx_desc);
	} else if (conn->type == CONN_TYPE_AQ) {
		ret = nvmf_process_admin_command(conn, tx_desc);
	} else {
		ret = nvmf_process_io_command(conn, tx_desc);
	}

	if (ret < 0) {
		goto recv_error;
	}

	/* re-post rx_desc and re-queue tx_desc here,
	   there is not a delayed posting because of
	   command processing.
	 */
	if (ret == 1) {
		tx_desc->rx_desc = NULL;
		nvmf_deactive_tx_desc(tx_desc);
		tx_desc = NULL;
		if (nvmf_post_rdma_recv(conn, rx_desc)) {
			SPDK_ERRLOG("Unable to re-post aq rx descriptor\n");
			goto recv_error;
		}
	}

drop_recv:
	return 0;

recv_error:
	/* recover the tx_desc */
	if (tx_desc != NULL) {
		tx_desc->rx_desc = NULL;
		nvmf_deactive_tx_desc(tx_desc);
	}
	return -1;
}

static int nvmf_cq_event_handler(struct spdk_nvmf_conn *conn)
{
	struct ibv_wc wc;
	struct nvme_qp_tx_desc *tx_desc;
	struct nvmf_request *req;
	int rc;
	int cq_count = 0;
	int i;

	for (i = 0; i < conn->sq_depth; i++) {
		tx_desc = NULL;

		/* if an overflow condition was hit
		   we want to stop all processing, but
		   do not disconnect.
		 */
		if (conn->state == CONN_STATE_OVERFLOW)
			break;

		rc = ibv_poll_cq(conn->cq, 1, &wc);
		if (rc == 0) // No completions at this time
			break;

		if (rc < 0) {
			SPDK_ERRLOG("Poll CQ error!(%d): %s\n",
				    errno, strerror(errno));
			goto handler_error;
		}

		/* OK, process the single successful cq event */
		cq_count += rc;

		if (wc.status) {
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "CQ completion error status %d, exiting handler\n",
				      wc.status);
			break;
		}

		switch (wc.opcode) {
		case IBV_WC_SEND:
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "\nCQ send completion\n");
			tx_desc = (struct nvme_qp_tx_desc *)wc.wr_id;
			nvmf_deactive_tx_desc(tx_desc);
			break;

		case IBV_WC_RDMA_WRITE:
			/*
			 * Will get this event only if we set IBV_SEND_SIGNALED
			 * flag in rdma_write, to trace rdma write latency
			 */
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "\nCQ rdma write completion\n");
			tx_desc = (struct nvme_qp_tx_desc *)wc.wr_id;
			spdk_trace_record(TRACE_RDMA_WRITE_COMPLETE, 0, 0, (uint64_t)tx_desc->rx_desc, 0);
			break;

		case IBV_WC_RDMA_READ:
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "\nCQ rdma read completion\n");
			tx_desc = (struct nvme_qp_tx_desc *)wc.wr_id;
			spdk_trace_record(TRACE_RDMA_READ_COMPLETE, 0, 0, (uint64_t)tx_desc->rx_desc, 0);
			req = &tx_desc->req_state;
			if (req->pending == NVMF_PENDING_WRITE) {
				req->pending = NVMF_PENDING_NONE;
				rc = nvmf_io_cmd_continue(conn, req);
				if (rc) {
					SPDK_ERRLOG("error from io cmd continue\n");
					goto handler_error;
				}

				/*
				 * Check for any pending rdma_reads to start
				 */
				conn->pending_rdma_read_count--;
				if (!STAILQ_EMPTY(&conn->qp_pending_desc)) {
					tx_desc = STAILQ_FIRST(&conn->qp_pending_desc);
					STAILQ_REMOVE_HEAD(&conn->qp_pending_desc, link);
					STAILQ_INSERT_TAIL(&conn->qp_tx_active_desc, tx_desc, link);

					SPDK_TRACELOG(SPDK_TRACE_RDMA, "Issue rdma read from pending queue: tx_desc %p\n",
						      tx_desc);

					rc = nvmf_post_rdma_read(conn, tx_desc);
					if (rc) {
						SPDK_ERRLOG("Unable to post pending rdma read descriptor\n");
						goto handler_error;
					}
				}
			} else if (req->pending == NVMF_PENDING_CONNECT) {
				req->pending = NVMF_PENDING_NONE;
				if (nvmf_connect_continue(conn, req)) {
					SPDK_ERRLOG("nvmf_connect_continue() failed\n");
					goto handler_error;
				}
			}
			break;

		case IBV_WC_RECV:
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "\nCQ recv completion\n");
			spdk_trace_record(TRACE_NVMF_IO_START, 0, 0, wc.wr_id, 0);
			rc = nvmf_recv(conn, &wc);
			if (rc) {
				SPDK_ERRLOG("nvmf_recv processing failure\n");
				goto handler_error;
			}
			break;

		default:
			SPDK_ERRLOG("Poll cq opcode type unknown!!!!! completion\n");
			goto handler_error;
		}
	}
	return cq_count;

handler_error:
	if (tx_desc != NULL)
		nvmf_deactive_tx_desc(tx_desc);
	SPDK_ERRLOG("handler error, exiting!\n");
	return -1;
}


static int nvmf_execute_conn(struct spdk_nvmf_conn *conn)
{
	int		rc = 0;

	/* for an active session, process any pending NVMf completions */
	if (conn->sess) {
		if (conn->type == CONN_TYPE_AQ)
			nvmf_check_admin_completions(conn->sess);
		else
			nvmf_check_io_completions(conn->sess);
	}

	/* process all pending completions */
	rc = nvmf_cq_event_handler(conn);
	if (rc > 0) {
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "CQ event handler, %d CQ completions\n", rc);
	} else if (rc < 0) {
		SPDK_ERRLOG("CQ event handler error!\n");
		return -1;
	}

	return 0;
}


/**

\brief This is the main routine for the nvmf connection work item.

Serves mainly as a wrapper for the nvmf_execute_conn() function which
does the bulk of the work.  This function handles connection cleanup when
NVMf application is exiting or there is an error on the connection.
It also drains the connection if the work item is being suspended to
move to a different reactor.

*/
static void
spdk_nvmf_conn_do_work(void *arg)
{
	struct spdk_nvmf_conn *conn = arg;
	int rc;

	rc = nvmf_execute_conn(conn);

	if (rc != 0 || conn->state == CONN_STATE_EXITING ||
	    conn->state == CONN_STATE_FABRIC_DISCONNECT) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "state exiting to shutdown\n");
		spdk_nvmf_conn_destruct(conn);
	}
}

static int
nvmf_allocate_reactor(uint64_t cpumask)
{
	int i, selected_core;
	enum rte_lcore_state_t state;
	int master_lcore = rte_get_master_lcore();
	int32_t num_pollers, min_pollers;

	cpumask &= spdk_app_get_core_mask();
	if (cpumask == 0) {
		return 0;
	}

	min_pollers = INT_MAX;
	selected_core = 0;

	/* we use u64 as CPU core mask */
	for (i = 0; i < RTE_MAX_LCORE && i < 64; i++) {
		if (!((1ULL << i) & cpumask)) {
			continue;
		}

		/*
		 * DPDK returns WAIT for the master lcore instead of RUNNING.
		 * So we always treat the reactor on master core as RUNNING.
		 */
		if (i == master_lcore) {
			state = RUNNING;
		} else {
			state = rte_eal_get_lcore_state(i);
		}
		if (state == FINISHED) {
			rte_eal_wait_lcore(i);
		}

		switch (state) {
		case WAIT:
		case FINISHED:
			/* Idle cores have 0 pollers */
			if (0 < min_pollers) {
				selected_core = i;
				min_pollers = 0;
			}
			break;
		case RUNNING:
			/* This lcore is running, check how many pollers it already has */
			num_pollers = rte_atomic32_read(&g_num_connections[i]);

			/* Fill each lcore to target minimum, else select least loaded lcore */
			if (num_pollers < (SPDK_NVMF_DEFAULT_NUM_SESSIONS_PER_LCORE *
					   g_nvmf_tgt.MaxConnectionsPerSession)) {
				/* If fewer than the target number of session connections
				 * exist then add to this lcore
				 */
				return i;
			} else if (num_pollers < min_pollers) {
				/* Track the lcore that has the minimum number of pollers
				 * to be used if no lcores have already met our criteria
				 */
				selected_core = i;
				min_pollers = num_pollers;
			}
			break;
		}
	}

	return selected_core;
}

