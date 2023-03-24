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

#define __GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <infiniband/verbs.h>

#include "snap.h"
#include "snap_env.h"
#include "snap_vrdma.h"
#include "snap_vrdma_ctrl.h"

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/net.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/event.h"
#include "spdk_internal/event.h"
#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/json.h"
#include "spdk/jsonrpc.h"
#include "spdk/vrdma.h"
#include "spdk/vrdma_snap_pci_mgr.h"
#include "spdk/vrdma_emu_mgr.h"
#include "spdk/vrdma_controller.h"
#include "spdk/vrdma_qp.h"
#include "spdk/vrdma_rpc.h"
#include "spdk/vrdma_io_mgr.h"
#include "spdk/vrdma_mr.h"

static char *g_vrdma_qp_method_str = "VRDMA_RPC_SRV_QP";
static char *g_vrdma_mkey_method_str = "VRDMA_RPC_MKEY";
static SLIST_HEAD(, spdk_vrdma_rpc_method) g_vrdma_rpc_methods = SLIST_HEAD_INITIALIZER(g_vrdma_rpc_methods);
struct spdk_vrdma_rpc g_vrdma_rpc;
uint64_t g_node_ip = 0;
uint64_t g_node_rip = 0;
static uint32_t g_request_id = 0;

static void
spdk_vrdma_client_resp_handler(struct spdk_vrdma_rpc_client *client,
				    struct spdk_jsonrpc_client_response *resp);

/* RPC client configuration */
static void
spdk_vrdma_rpc_client_set_timeout(struct spdk_vrdma_rpc_client *client, uint64_t timeout_us)
{
	client->timeout =
		spdk_get_ticks() + timeout_us * spdk_get_ticks_hz() / (1000 * 1000);
}

static int
spdk_vrdma_rpc_client_check_timeout(struct spdk_vrdma_rpc_client *client)
{
	if (client->timeout < spdk_get_ticks()) {
		SPDK_WARNLOG("VRDMA SF RPC client command timeout.\n");
		return -ETIMEDOUT;
	}
	return 0;
}

static void
spdk_vrdma_close_rpc_client(struct spdk_vrdma_rpc_client *client)
{
    if (client->client_conn_poller) {
	    spdk_poller_unregister(&client->client_conn_poller);
        client->client_conn_poller = NULL;
    }
	if (client->client_conn) {
		spdk_jsonrpc_client_close(client->client_conn);
        client->client_conn = NULL;
    }
}

static int
spdk_vrdma_rpc_client_poller(void *arg)
{
	struct spdk_vrdma_rpc_client *client = arg;
	struct spdk_jsonrpc_client_response *resp;
	int rc;

    if (client->client_conn == NULL)
        return -1;
	rc = spdk_jsonrpc_client_poll(client->client_conn, 0);
	if (rc == 0) {
		rc = spdk_vrdma_rpc_client_check_timeout(client);
		if (rc == -ETIMEDOUT) {
			spdk_vrdma_rpc_client_set_timeout(client,
                        VRDMA_RPC_CLIENT_REQUEST_TIMEOUT_US);
			rc = 0;
		}
	}
	if (rc == 0) {
		/* No response yet */
		return -1;
	} else if (rc < 0) {
		spdk_vrdma_close_rpc_client(client);
		return -1;
	}
	resp = spdk_jsonrpc_client_get_response(client->client_conn);
	assert(resp);
	if (resp->error) {
		SPDK_ERRLOG("error response: %*s", (int)resp->error->len,
                (char *)resp->error->start);
		spdk_jsonrpc_client_free_response(resp);
		spdk_vrdma_close_rpc_client(client);
	} else {
		/* We have response so we must have callback for it. */
		assert(client->client_resp_cb != NULL);
		client->client_resp_cb(client, resp);
	}
	return -1;
}

static int
spdk_vrdma_client_connect_poller(void *arg)
{
	struct spdk_vrdma_rpc_client *client = arg;
	int rc;

    if (client->client_conn == NULL)
        return -1;
	rc = spdk_jsonrpc_client_poll(client->client_conn, 0);
	if (rc != -ENOTCONN) {
		/* We are connected. Start regular poller and issue first request */
        if (client->client_conn_poller)
		    spdk_poller_unregister(&client->client_conn_poller);
		client->client_conn_poller = spdk_poller_register(
                    spdk_vrdma_rpc_client_poller, client, 100);
	} else {
		rc = spdk_vrdma_rpc_client_check_timeout(client);
		if (rc) {
			spdk_vrdma_close_rpc_client(client);
		}
	}
	return -1;
}

static const struct spdk_json_object_decoder
spdk_vrdma_rpc_qp_msg_decoder[] = {
    {
        "emu_manager",
        offsetof(struct spdk_vrdma_rpc_qp_msg, emu_manager),
        spdk_json_decode_string,
    },
    {
        "request_id",
        offsetof(struct spdk_vrdma_rpc_qp_msg, request_id),
        spdk_json_decode_uint32,
    },
    {
        "mac",
        offsetof(struct spdk_vrdma_rpc_qp_msg, sf_mac),
        spdk_json_decode_uint64,
        true
    },
    {
        "bkqpn",
        offsetof(struct spdk_vrdma_rpc_qp_msg, bk_qpn),
        spdk_json_decode_uint32,
        true
    },
    {
        "state",
        offsetof(struct spdk_vrdma_rpc_qp_msg, qp_state),
        spdk_json_decode_uint32,
        true
    },
    {
        "mqp_idx",
        offsetof(struct spdk_vrdma_rpc_qp_msg, mqp_idx),
        spdk_json_decode_uint32,
        true
    },
    {
        "ltgid_prefix",
        offsetof(struct spdk_vrdma_rpc_qp_msg, local_tgid.global.subnet_prefix),
        spdk_json_decode_uint64,
        true
    },
    {
        "ltgid_ip",
        offsetof(struct spdk_vrdma_rpc_qp_msg, local_tgid.global.interface_id),
        spdk_json_decode_uint64,
        true
    },
    {
        "rtgid_prefix",
        offsetof(struct spdk_vrdma_rpc_qp_msg, remote_tgid.global.subnet_prefix),
        spdk_json_decode_uint64,
        true
    },
    {
        "rtgid_ip",
        offsetof(struct spdk_vrdma_rpc_qp_msg, remote_tgid.global.interface_id),
        spdk_json_decode_uint64,
        true
    },
    {
        "lmgid_prefix",
        offsetof(struct spdk_vrdma_rpc_qp_msg, local_mgid.global.subnet_prefix),
        spdk_json_decode_uint64,
        true
    },
    {
        "lmgid_ip",
        offsetof(struct spdk_vrdma_rpc_qp_msg, local_mgid.global.interface_id),
        spdk_json_decode_uint64,
        true
    },
    {
        "rmgid_prefix",
        offsetof(struct spdk_vrdma_rpc_qp_msg, remote_mgid.global.subnet_prefix),
        spdk_json_decode_uint64,
        true
    },
    {
        "rmgid_ip",
        offsetof(struct spdk_vrdma_rpc_qp_msg, remote_mgid.global.interface_id),
        spdk_json_decode_uint64,
        true
    },};

static void
spdk_vrdma_set_qp_attr(struct vrdma_ctrl *ctrl,
                       struct vrdma_tgid_node *tgid_node,
                       struct spdk_vrdma_rpc_qp_msg *attr,
                       struct ibv_qp_attr *qp_attr, int *attr_mask,
                       struct snap_vrdma_bk_qp_rdy_attr *rdy_attr) {
	uint32_t path_mtu;
	path_mtu = ctrl->vdev->vrdma_sf.mtu < ctrl->sctrl->bar_curr->mtu ?
				ctrl->vdev->vrdma_sf.mtu : ctrl->sctrl->bar_curr->mtu;
	if (path_mtu >= 4096)
		qp_attr->path_mtu = IBV_MTU_4096;
	else if (path_mtu >= 2048)
		qp_attr->path_mtu = IBV_MTU_2048;
	else if (path_mtu >= 1024)
		qp_attr->path_mtu = IBV_MTU_1024;
	else if (path_mtu >= 512)
		qp_attr->path_mtu = IBV_MTU_512;
	else
		qp_attr->path_mtu = IBV_MTU_256;
	qp_attr->rq_psn = 0;
	qp_attr->min_rnr_timer = VRDMA_MIN_RNR_TIMER;
	qp_attr->max_dest_rd_atomic = VRDMA_QP_MAX_DEST_RD_ATOMIC;
	qp_attr->dest_qp_num = attr->bk_qpn;
	qp_attr->rq_psn = 0;
	qp_attr->min_rnr_timer = VRDMA_MIN_RNR_TIMER;
	qp_attr->max_dest_rd_atomic = VRDMA_QP_MAX_DEST_RD_ATOMIC;
	*attr_mask = IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
				IBV_QP_RQ_PSN | IBV_QP_MIN_RNR_TIMER |
				IBV_QP_MAX_DEST_RD_ATOMIC;
    rdy_attr->dest_mac = (uint8_t *)&attr->sf_mac;
    rdy_attr->rgid_rip = &attr->local_mgid;
    rdy_attr->src_addr_index = VRDMA_MQP_SRC_ADDR_INDEX;
    rdy_attr->udp_src_port = tgid_node->src_udp[attr->mqp_idx].udp_src_port;
#ifdef MPATH_DBG
    SPDK_NOTICELOG("dest_mac=%2x%2x%2x%2x%2x%2x\n"
                   "rgid_rip.global.interface_id=%llx\n"
                   "rgid_rip.global.subnet_prefix=%llx\n"
                   "src_addr_index=%x, udp_src_port=%x\n",
                   rdy_attr->dest_mac[0],
                   rdy_attr->dest_mac[1],
                   rdy_attr->dest_mac[2],
                   rdy_attr->dest_mac[3],
                   rdy_attr->dest_mac[4],
                   rdy_attr->dest_mac[5],
                   rdy_attr->rgid_rip->global.interface_id,
                   rdy_attr->rgid_rip->global.subnet_prefix,
                   rdy_attr->src_addr_index, rdy_attr->udp_src_port);
#endif

    return;
}

static void
spdk_vrdma_client_qp_resp_handler(struct spdk_vrdma_rpc_client *client,
				    struct spdk_jsonrpc_client_response *resp)
{
    struct ibv_qp_attr qp_attr;
    struct spdk_vrdma_rpc_qp_msg *attr;
    uint32_t request_id = 0;
    struct vrdma_tgid_node *tgid_node = NULL;
    struct vrdma_backend_qp *mqp = NULL;
    int attr_mask;
    struct snap_vrdma_bk_qp_rdy_attr rdy_attr;
    struct vrdma_ctrl *ctrl = NULL;
    struct spdk_emu_ctx *ctx;

    attr = calloc(1, sizeof(*attr));
    if (!attr)
        goto close_rpc;

    if (spdk_json_decode_object(resp->result,
            spdk_vrdma_rpc_qp_msg_decoder,
            SPDK_COUNTOF(spdk_vrdma_rpc_qp_msg_decoder),
            attr)) {
        SPDK_ERRLOG("Failed to decode result for qp_msg\n");
        goto free_attr;
    }
    SPDK_NOTICELOG("emu_manager %s request_id=0x%x sf_mac=0x%lx bk_qpn =0x%x\n"
                   "qp_state=0x%x mqp_idx =0x%x local_tgid.prefix=0x%llx local_tgid.ip=0x%llx \n"
                   "remote_tgid.prefix=0x%llx remote_tgid.ip=0x%llx \n"
                   "local_mgid.prefix=0x%llx local_mgid.ip=0x%llx\n"
                   "remote_mgid.prefix=0x%llx remote_mgid.ip=0x%llx\n",
                   attr->emu_manager, attr->request_id, attr->sf_mac, attr->bk_qpn,
                   attr->qp_state, attr->mqp_idx, attr->local_tgid.global.subnet_prefix,
                   attr->local_tgid.global.interface_id, attr->remote_tgid.global.subnet_prefix,
                   attr->remote_tgid.global.interface_id, attr->local_mgid.global.subnet_prefix,
                   attr->local_mgid.global.interface_id, attr->remote_mgid.global.subnet_prefix,
                   attr->remote_mgid.global.interface_id);

    ctx = spdk_emu_ctx_find_by_gid_ip(attr->emu_manager, attr->remote_mgid.global.interface_id);
    if (ctx) {
        ctrl = ctx->ctrl;
        if (!ctrl) {
            SPDK_ERRLOG("Fail to find device controller for emu_manager %s\n",
                attr->emu_manager);
            goto free_attr;
        }
    }

    tgid_node = vrdma_find_tgid_node(&attr->local_tgid, &attr->remote_tgid);
    if (!tgid_node || attr->mqp_idx >= VRDMA_DEV_SRC_UDP_CNT ||
        !tgid_node->src_udp[attr->mqp_idx].mqp) {
        SPDK_ERRLOG("Failed to find tgid_node or mqp for response msg\n");
        goto free_attr;
    }
    mqp = tgid_node->src_udp[attr->mqp_idx].mqp;
    if (mqp->qp_state != IBV_QPS_RTS) {
        if (mqp->qp_state == IBV_QPS_INIT) {
            spdk_vrdma_set_qp_attr(ctrl, tgid_node, attr, &qp_attr, &attr_mask, &rdy_attr);
            vrdma_modify_backend_qp_to_rtr(mqp, &qp_attr, attr_mask, &rdy_attr);
            vrdma_qp_notify_remote_by_rpc(ctrl, tgid_node, attr->mqp_idx);
        }
        if (attr->qp_state == IBV_QPS_RTR && mqp->qp_state == IBV_QPS_RTR) {
            vrdma_modify_backend_qp_to_rts(mqp);
            set_spdk_vrdma_bk_qp_active(mqp);
        }
    }
free_attr:
    request_id = attr->request_id;
    free(attr);
close_rpc:
	spdk_jsonrpc_client_free_response(resp);
    if (request_id && client->client_conn) {
        spdk_jsonrpc_client_remove_request_from_list(client->client_conn,
            request_id);
        if (spdk_jsonrpc_client_request_list_empty(client->client_conn))
            spdk_vrdma_close_rpc_client(client);
    } else {
        spdk_vrdma_close_rpc_client(client);
    }
    return;
}

static int
spdk_vrdma_client_send_request(struct spdk_vrdma_rpc_client *client,
		struct spdk_jsonrpc_client_request *request)
{
	int rc;

	client->client_resp_cb = spdk_vrdma_client_resp_handler;
	spdk_vrdma_rpc_client_set_timeout(client,
            VRDMA_RPC_CLIENT_REQUEST_TIMEOUT_US);
	rc = spdk_jsonrpc_client_send_request(client->client_conn, request);
	if (rc)
		SPDK_ERRLOG("Sending request to client failed (%d)\n", rc);
	return rc;
}

static int
spdk_vrdma_rpc_client_configuration(const char *addr)
{
    struct spdk_vrdma_rpc_client *client = &g_vrdma_rpc.client;

    if (client->client_conn) {
		SPDK_NOTICELOG("RPC client connect to '%s' is already existed.\n", addr);
		return 0;
	}
    client->client_conn = spdk_jsonrpc_client_connect(addr, AF_UNSPEC);
	if (!client->client_conn) {
		SPDK_ERRLOG("Failed to connect to '%s'\n", addr);
		return -1;
	}
	spdk_vrdma_rpc_client_set_timeout(client,
            VRDMA_RPC_CLIENT_CONNECT_TIMEOUT_US);
	client->client_conn_poller = spdk_poller_register(
			spdk_vrdma_client_connect_poller, client, 100);
    return 0;
}

static void
spdk_vrdma_rpc_qp_info_json(struct spdk_vrdma_rpc_qp_msg *info,
			 struct spdk_json_write_ctx *w, uint32_t request_id)
{
	spdk_json_write_object_begin(w);
    spdk_json_write_named_string(w, "emu_manager", info->emu_manager);
    spdk_json_write_named_uint32(w, "request_id", request_id);
    spdk_json_write_named_uint64(w, "mac", info->sf_mac);
    spdk_json_write_named_uint32(w, "bkqpn", info->bk_qpn);
    spdk_json_write_named_uint32(w, "state", info->qp_state);
    spdk_json_write_named_uint32(w, "mqp_idx", info->mqp_idx);
    spdk_json_write_named_uint64(w, "ltgid_prefix", info->local_tgid.global.subnet_prefix);
    spdk_json_write_named_uint64(w, "ltgid_ip", info->local_tgid.global.interface_id);
    spdk_json_write_named_uint64(w, "rtgid_prefix", info->remote_tgid.global.subnet_prefix);
    spdk_json_write_named_uint64(w, "rtgid_ip", info->remote_tgid.global.interface_id);
    spdk_json_write_named_uint64(w, "lmgid_prefix", info->local_mgid.global.subnet_prefix);
    spdk_json_write_named_uint64(w, "lmgid_ip", info->local_mgid.global.interface_id);
    spdk_json_write_named_uint64(w, "rmgid_prefix", info->remote_mgid.global.subnet_prefix);
    spdk_json_write_named_uint64(w, "rmgid_ip", info->remote_mgid.global.interface_id);    spdk_json_write_object_end(w);
}

static int
spdk_vrdma_rpc_client_send_qp_msg(
            struct spdk_vrdma_rpc_qp_msg *msg)
{
    struct spdk_vrdma_rpc_client *client = &g_vrdma_rpc.client;
	struct spdk_jsonrpc_client_request *rpc_request;
	struct spdk_json_write_ctx *w;
    uint32_t request_id;
	int rc;

	rpc_request = spdk_jsonrpc_client_create_request();
	if (!rpc_request) {
        SPDK_ERRLOG("Failed to create request");
		goto out;
	}
	w = spdk_jsonrpc_begin_request(rpc_request, 1, g_vrdma_qp_method_str);
	if (!w) {
		spdk_jsonrpc_client_free_request(rpc_request);
        SPDK_ERRLOG("Failed to build request");
		goto out;
	}
    spdk_json_write_name(w, "params");
    request_id = ++g_request_id ? g_request_id : ++g_request_id;
    spdk_vrdma_rpc_qp_info_json(msg, w, request_id);
	spdk_jsonrpc_end_request(rpc_request, w);
    spdk_jsonrpc_set_request_id(rpc_request, request_id);

	rc = spdk_vrdma_client_send_request(client, rpc_request);
	if (rc != 0) {
        SPDK_ERRLOG("Failed to send request");
		goto out;
	}
#ifdef MPATH_DBG
    SPDK_NOTICELOG("emu_manager %s request_id=0x%x sf_mac=0x%lx bk_qpn =0x%x\n"
                   "qp_state=0x%x mqp_idx =0x%x local_tgid.prefix=0x%llx local_tgid.ip=0x%llx \n"
                   "remote_tgid.prefix=0x%llx remote_tgid.ip=0x%llx \n"
                   "local_mgid.prefix=0x%llx local_mgid.ip=0x%llx\n"
                   "remote_mgid.prefix=0x%llx remote_mgid.ip=0x%llx\n",
                   msg->emu_manager, msg->request_id, msg->sf_mac, msg->bk_qpn,
                   msg->qp_state, msg->mqp_idx, msg->local_tgid.global.subnet_prefix,
                   msg->local_tgid.global.interface_id, msg->remote_tgid.global.subnet_prefix,
                   msg->remote_tgid.global.interface_id, msg->local_mgid.global.subnet_prefix,
                   msg->local_mgid.global.interface_id, msg->remote_mgid.global.subnet_prefix,
                   msg->remote_mgid.global.interface_id);
#endif
    return 0;
out:
	spdk_vrdma_close_rpc_client(client);
    return -1;
}

int spdk_vrdma_rpc_send_qp_msg(const char *addr,
                struct spdk_vrdma_rpc_qp_msg *msg)
{
    if (spdk_vrdma_rpc_client_configuration(addr)) {
        SPDK_ERRLOG("%s: Failed to client configuration\n", __func__);
        return -1;
    }
    if (spdk_vrdma_rpc_client_send_qp_msg(msg)) {
        SPDK_ERRLOG("Failed to send request");
        return -1;
    }
    return 0;
}

/* RPC server configuration */
static struct spdk_vrdma_rpc_method *
_get_rpc_method(const struct spdk_json_val *method)
{
	struct spdk_vrdma_rpc_method *m;

	SLIST_FOREACH(m, &g_vrdma_rpc_methods, slist) {
		if (spdk_json_strequal(method, m->name)) {
			return m;
		}
	}
	return NULL;
}

static struct spdk_vrdma_rpc_method *
_get_rpc_method_raw(const char *method)
{
	struct spdk_json_val method_val;

	method_val.type = SPDK_JSON_VAL_STRING;
	method_val.len = strlen(method);
	method_val.start = (char *)method;

	return _get_rpc_method(&method_val);
}

static void
spdk_vrdma_rpc_register_method(const char *method, spdk_rpc_method_handler func,
        vrdma_client_resp_handler resp_cb)
{
	struct spdk_vrdma_rpc_method *m;

	m = _get_rpc_method_raw(method);
	if (m != NULL) {
		SPDK_ERRLOG("duplicate RPC %s registered - ignoring...\n", method);
		return;
	}
	m = calloc(1, sizeof(struct spdk_vrdma_rpc_method));
	assert(m != NULL);
	m->name = strdup(method);
	assert(m->name != NULL);
	m->func = func;
    m->resp_cb = resp_cb;
	/* TODO: use a hash table or sorted list */
	SLIST_INSERT_HEAD(&g_vrdma_rpc_methods, m, slist);
}

static void
spdk_vrdma_client_resp_handler(struct spdk_vrdma_rpc_client *client,
				    struct spdk_jsonrpc_client_response *resp)
{
    struct spdk_vrdma_rpc_method *m;

    if (!resp->method || resp->method->type != SPDK_JSON_VAL_STRING) {
		SPDK_ERRLOG("Failed to decode method for vrdma resp msg\n");
        goto close_rpc;
	}
	m = _get_rpc_method(resp->method);
	if (!m || !m->resp_cb) {
        SPDK_ERRLOG("Failed to find method\n");
		goto close_rpc;
	}
	m->resp_cb(client, resp);
    return;

close_rpc:
	spdk_jsonrpc_client_free_response(resp);
    spdk_vrdma_close_rpc_client(client);
}

static void
spdk_vrdma_rpc_srv_qp_req_handle(struct spdk_jsonrpc_request *request,
            const struct spdk_json_val *params)
{
    struct spdk_vrdma_rpc_client *client = &g_vrdma_rpc.client;
    struct spdk_vrdma_rpc_qp_msg *attr;
    struct spdk_json_write_ctx *w;
    struct spdk_emu_ctx *ctx;
    struct vrdma_ctrl *ctrl = NULL;
    struct spdk_vrdma_rpc_qp_msg msg = {0};
    struct vrdma_tgid_node *tgid_node = NULL;
    struct vrdma_backend_qp *mqp = NULL;
    struct snap_vrdma_bk_qp_rdy_attr rdy_attr = {0};
    struct ibv_qp_attr qp_attr = {0};
    int attr_mask;

    /* If local client running and retry send requests. */
    if (client->client_conn)
        spdk_jsonrpc_client_resend_request(client->client_conn);

    attr = calloc(1, sizeof(*attr));
    if (!attr)
        goto invalid;

    if (spdk_json_decode_object(params,
            spdk_vrdma_rpc_qp_msg_decoder,
            SPDK_COUNTOF(spdk_vrdma_rpc_qp_msg_decoder),
            attr)) {
        SPDK_ERRLOG("Failed to decode parameters for \n");
        goto invalid;
    }
    if (!attr->emu_manager) {
        SPDK_ERRLOG("invalid emu_manager\n");
        goto invalid;
    }
#ifdef MPATH_DBG
    SPDK_NOTICELOG("emu_manager %s request_id=0x%x sf_mac=0x%lx bk_qpn =0x%x\n"
                   "qp_state=0x%x mqp_idx =0x%x local_tgid.prefix=0x%llx local_tgid.ip=0x%llx \n"
                   "remote_tgid.prefix=0x%llx remote_tgid.ip=0x%llx \n"
                   "local_mgid.prefix=0x%llx local_mgid.ip=0x%llx\n"
                   "remote_mgid.prefix=0x%llx remote_mgid.ip=0x%llx\n",
                   attr->emu_manager, attr->request_id, attr->sf_mac, attr->bk_qpn,
                   attr->qp_state, attr->mqp_idx, attr->local_tgid.global.subnet_prefix,
                   attr->local_tgid.global.interface_id, attr->remote_tgid.global.subnet_prefix,
                   attr->remote_tgid.global.interface_id, attr->local_mgid.global.subnet_prefix,
                   attr->local_mgid.global.interface_id, attr->remote_mgid.global.subnet_prefix,
                   attr->remote_mgid.global.interface_id);
#endif
    /* Find device data by remote_gid_ip (remote SF IP)*/
    ctx = spdk_emu_ctx_find_by_gid_ip(attr->emu_manager, attr->remote_mgid.global.interface_id);
    if (ctx) {
        ctrl = ctx->ctrl;
        if (!ctrl) {
            SPDK_ERRLOG("Fail to find device controller for emu_manager %s\n",
                attr->emu_manager);
            goto invalid;
        }
    }

    tgid_node = vrdma_find_tgid_node(&attr->local_tgid, &attr->remote_tgid);
    if (!tgid_node) {
        tgid_node = vrdma_create_tgid_node(&attr->local_tgid,
                                           &attr->remote_tgid,
                                           ctrl->vdev,
                                           ctrl->vdev->vrdma_sf.sf_pd,
                                           0xc000, VRDMA_DEV_SRC_UDP_CNT);
        if (!tgid_node) {
            goto invalid;
        }
    }
    if (attr->mqp_idx >= VRDMA_DEV_SRC_UDP_CNT) {
        SPDK_ERRLOG("invalid mqp_idx=%u\n", attr->mqp_idx);
        goto invalid;
    }

    mqp = tgid_node->src_udp[attr->mqp_idx].mqp;
    if (!mqp) {
        mqp = vrdma_create_backend_qp(tgid_node, attr->mqp_idx);
        if (!mqp)
            goto invalid;
        vrdma_modify_backend_qp_to_init(mqp);
    }
    if (mqp->qp_state == IBV_QPS_INIT) {
        spdk_vrdma_set_qp_attr(ctrl, tgid_node, attr, &qp_attr, &attr_mask, &rdy_attr);
        if (vrdma_modify_backend_qp_to_rtr(mqp, &qp_attr, attr_mask, &rdy_attr))
            goto invalid;
    }
    if (attr->qp_state == IBV_QPS_RTR && mqp->qp_state == IBV_QPS_RTR) {
        if (vrdma_modify_backend_qp_to_rts(mqp))
            goto invalid;
        set_spdk_vrdma_bk_qp_active(mqp);
    }
    vrdma_set_rpc_msg_with_mqp_info(ctrl, tgid_node, attr->mqp_idx, &msg);

    w = spdk_jsonrpc_begin_result_with_method(request, g_vrdma_qp_method_str);
    msg.emu_manager = attr->emu_manager;
    spdk_vrdma_rpc_qp_info_json(&msg, w, attr->request_id);
    spdk_jsonrpc_end_result(request, w);
    free(attr);
    return;

invalid:
    free(attr);
    spdk_jsonrpc_send_error_response(request,
                     SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                     "Invalid parameters");
}

static const struct spdk_json_object_decoder
spdk_vrdma_rpc_mkey_resp_decoder[] = {
    {
        "request_id",
        offsetof(struct spdk_vrdma_rpc_mkey_attr, request_id),
        spdk_json_decode_uint32
    },
    {
        "gid",
        offsetof(struct spdk_vrdma_rpc_mkey_attr, gid_ip),
        spdk_json_decode_uint64
    },
    {
        "vqpn",
        offsetof(struct spdk_vrdma_rpc_mkey_attr, vqpn),
        spdk_json_decode_uint32,
        true
    },
    {
        "vkey",
        offsetof(struct spdk_vrdma_rpc_mkey_attr, vkey),
        spdk_json_decode_uint32,
        true
    },
    {
        "mkey",
        offsetof(struct spdk_vrdma_rpc_mkey_attr, mkey),
        spdk_json_decode_uint32,
        true
    },
};

static void
spdk_vrdma_client_mkey_resp_handler(struct spdk_vrdma_rpc_client *client,
				    struct spdk_jsonrpc_client_response *resp)
{
    struct spdk_vrdma_rpc_mkey_attr *attr;
    uint32_t request_id = 0;
    struct vrdma_r_vkey_entry r_vkey;

    attr = calloc(1, sizeof(*attr));
    if (!attr)
        goto close_rpc;

    if (spdk_json_decode_object(resp->result,
            spdk_vrdma_rpc_mkey_resp_decoder,
            SPDK_COUNTOF(spdk_vrdma_rpc_mkey_resp_decoder),
            attr)) {
        SPDK_ERRLOG("Failed to decode result for mkey_msg\n");
        goto free_attr;
    }
    SPDK_NOTICELOG("Decode mkey resp msg: request_id =0x%x "
    "gid_ip=0x%lx vqpn=%d vkey=0x%x mkey=0x%x\n",
    attr->request_id, attr->gid_ip, attr->vqpn,
    attr->vkey, attr->mkey);
    if (!attr->gid_ip) {
        SPDK_NOTICELOG("Skip decode mkey result for zero gid_ip\n");
        goto free_attr;
    }
    r_vkey.mkey = attr->mkey;
    vrdma_add_r_vkey_list(attr->gid_ip, attr->vkey, &r_vkey);
free_attr:
    request_id = attr->request_id;
    free(attr);
close_rpc:
	spdk_jsonrpc_client_free_response(resp);
    if (request_id && client->client_conn) {
        spdk_jsonrpc_client_remove_request_from_list(client->client_conn,
            request_id);
        if (spdk_jsonrpc_client_request_list_empty(client->client_conn))
            spdk_vrdma_close_rpc_client(client);
    } else {
        spdk_vrdma_close_rpc_client(client);
    }
    return;
}

static const struct spdk_json_object_decoder
spdk_vrdma_rpc_mkey_req_decoder[] = {
    {
        "request_id",
        offsetof(struct spdk_vrdma_rpc_mkey_attr, request_id),
        spdk_json_decode_uint32
    },
    {
        "gid",
        offsetof(struct spdk_vrdma_rpc_mkey_attr, gid_ip),
        spdk_json_decode_uint64
    },
    {
        "vqpn",
        offsetof(struct spdk_vrdma_rpc_mkey_attr, vqpn),
        spdk_json_decode_uint32,
        true
    },
    {
        "vkey",
        offsetof(struct spdk_vrdma_rpc_mkey_attr, vkey),
        spdk_json_decode_uint32,
        true
    },
    {
        "mkey",
        offsetof(struct spdk_vrdma_rpc_mkey_attr, mkey),
        spdk_json_decode_uint32,
        true
    },
};

static void
spdk_vrdma_rpc_mkey_info_json(struct spdk_vrdma_rpc_mkey_msg *info,
			 struct spdk_json_write_ctx *w, uint32_t request_id)
{
	spdk_json_write_object_begin(w);
    spdk_json_write_named_uint32(w, "request_id", request_id);
    spdk_json_write_named_uint64(w, "gid", info->mkey_attr.gid_ip);
    spdk_json_write_named_uint32(w, "vqpn", info->mkey_attr.vqpn);
    spdk_json_write_named_uint32(w, "vkey", info->mkey_attr.vkey);
    spdk_json_write_named_uint32(w, "mkey", info->mkey_attr.mkey);
    spdk_json_write_object_end(w);
}

static int
spdk_vrdma_rpc_client_send_mkey_msg(
            struct spdk_vrdma_rpc_mkey_msg *msg)
{
    struct spdk_vrdma_rpc_client *client = &g_vrdma_rpc.client;
	struct spdk_jsonrpc_client_request *rpc_request;
	struct spdk_json_write_ctx *w;
    uint32_t request_id;
	int rc;

	rpc_request = spdk_jsonrpc_client_create_request();
	if (!rpc_request) {
        SPDK_ERRLOG("Failed to create request for vkey %d\n",
            msg->mkey_attr.vkey);
		goto out;
	}
	w = spdk_jsonrpc_begin_request(rpc_request, 1, g_vrdma_mkey_method_str);
	if (!w) {
		spdk_jsonrpc_client_free_request(rpc_request);
        SPDK_ERRLOG("Failed to build request for vkey %d\n",
            msg->mkey_attr.vkey);
		goto out;
	}
    spdk_json_write_name(w, "params");
    request_id = ++g_request_id ? g_request_id : ++g_request_id;
    spdk_vrdma_rpc_mkey_info_json(msg, w, request_id);
	spdk_jsonrpc_end_request(rpc_request, w);
    spdk_jsonrpc_set_request_id(rpc_request, request_id);

	rc = spdk_vrdma_client_send_request(client, rpc_request);
	if (rc != 0) {
        SPDK_ERRLOG("Failed to send request for vkey %d\n",
            msg->mkey_attr.vkey);
		goto out;
	}
    SPDK_NOTICELOG("mkey rpc msg: request_id =0x%x "
    "gid_ip=0x%lx vqpn=%d vkey=0x%x mkey=0x%x\n",
     request_id, msg->mkey_attr.gid_ip, msg->mkey_attr.vqpn,
     msg->mkey_attr.vkey, msg->mkey_attr.mkey);
    return 0;
out:
	spdk_vrdma_close_rpc_client(client);
    return -1;
}

int spdk_vrdma_rpc_send_mkey_msg(const char *addr,
                struct spdk_vrdma_rpc_mkey_msg *msg)
{
    if (spdk_vrdma_rpc_client_configuration(addr)) {
        SPDK_ERRLOG("Failed to client configuration for vkey %d\n",
            msg->mkey_attr.vkey);
        return -1;
    }
    if (spdk_vrdma_rpc_client_send_mkey_msg(msg)) {
        SPDK_ERRLOG("Failed to send request for vkey %d\n",
            msg->mkey_attr.vkey);
        return -1;
    }
    return 0;
}

static void
spdk_vrdma_rpc_srv_mkey_req_handle(struct spdk_jsonrpc_request *request,
            const struct spdk_json_val *params)
{
    struct spdk_vrdma_rpc_client *client = &g_vrdma_rpc.client;
    struct spdk_vrdma_rpc_mkey_msg msg = {0};
    struct spdk_vrdma_rpc_mkey_attr *attr;
    struct spdk_json_write_ctx *w;
    struct spdk_vrdma_qp *vqp;
    struct spdk_emu_ctx *ctx;
    struct vrdma_ctrl *ctrl = NULL;

    /* If local client running and retry send requests. */
    if (client->client_conn)
        spdk_jsonrpc_client_resend_request(client->client_conn);

    attr = calloc(1, sizeof(*attr));
    if (!attr)
        goto invalid;

    if (spdk_json_decode_object(params,
            spdk_vrdma_rpc_mkey_req_decoder,
            SPDK_COUNTOF(spdk_vrdma_rpc_mkey_req_decoder),
            attr)) {
        SPDK_ERRLOG("Failed to decode parameters for \n");
        goto invalid;
    }
    SPDK_NOTICELOG("Decode:mkey req msg: request_id =0x%x "
    "gid_ip=0x%lx vqpn=%d vkey=0x%x mkey=0x%x\n",
     attr->request_id, attr->gid_ip, attr->vqpn,
     attr->vkey, attr->mkey);
    msg.mkey_attr.request_id = attr->request_id;
    msg.mkey_attr.gid_ip = attr->gid_ip;
    msg.mkey_attr.vqpn = attr->vqpn;
    msg.mkey_attr.vkey = attr->vkey;
    msg.mkey_attr.mkey = 0;
    if (attr->vkey >= VRDMA_DEV_MAX_MR) {
        SPDK_ERRLOG("invalid vkey index %d \n", attr->vkey);
        goto send_result;
    }
    /* Find device data by gid_ip */
    ctx = spdk_emu_ctx_find_by_gid_ip(NULL, attr->gid_ip);
    if (!ctx) {
        SPDK_ERRLOG("Fail to find device controller context for gid_ip 0x%lx\n",
                attr->gid_ip);
        goto send_result;
    }
    ctrl = ctx->ctrl;
    if (!ctrl) {
        SPDK_ERRLOG("Fail to find device controller for gid_ip 0x%lx\n",
                attr->gid_ip);
        goto send_result;
    }
    vqp = find_spdk_vrdma_qp_by_idx(ctrl, attr->vqpn);
    if (!vqp) {
        SPDK_ERRLOG("Fail to find vrdma_qpn %d for mkey\n",
                    attr->vqpn);
        goto send_result;
    }
    if (vqp->vpd != ctrl->vdev->l_vkey_tbl.vkey[attr->vkey].vpd)  {
        SPDK_ERRLOG("Fail to match vpd vrdma_qpn %d for vkey 0x%x\n",
                attr->vqpn, attr->vkey);
        goto send_result;
    }
    msg.mkey_attr.mkey = ctrl->vdev->l_vkey_tbl.vkey[attr->vkey].mkey;
    SPDK_NOTICELOG("Send mkey resp msg: request_id =0x%x "
        "gid_ip=0x%lx vqpn=%d vkey=0x%x mkey=0x%x\n",
        msg.mkey_attr.request_id, msg.mkey_attr.gid_ip, msg.mkey_attr.vqpn,
        msg.mkey_attr.vkey, msg.mkey_attr.mkey);
send_result:
    w = spdk_jsonrpc_begin_result_with_method(request, g_vrdma_mkey_method_str);
    spdk_vrdma_rpc_mkey_info_json(&msg, w, attr->request_id);
    spdk_jsonrpc_end_result(request, w);
    free(attr);
    return;

invalid:
    free(attr);
    spdk_jsonrpc_send_error_response(request,
                     SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                     "Invalid parameters");
}

static void
spdk_vrdma_srv_rpc_handler(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *method,
		     const struct spdk_json_val *params)
{
	struct spdk_vrdma_rpc_method *m;

	assert(method != NULL);
	m = _get_rpc_method(method);
	if (!m) {
		spdk_jsonrpc_send_error_response(request,
        SPDK_JSONRPC_ERROR_METHOD_NOT_FOUND, "Method not found");
		return;
	}
	m->func(request, params);
}

static int
spdk_vrdma_rpc_listen(struct spdk_vrdma_rpc_server *srv,
            const char *listen_addr)
{
	struct addrinfo		hints;
	struct addrinfo		*res;
	char *host, *port;
	char *tmp;

	memset(&srv->rpc_listen_addr_unix, 0, sizeof(srv->rpc_listen_addr_unix));
	tmp = strdup(listen_addr);
	if (!tmp) {
		SPDK_ERRLOG("Out of memory\n");
		return -1;
	}
	if (spdk_parse_ip_addr(tmp, &host, &port) < 0) {
		free(tmp);
		SPDK_ERRLOG("Invalid listen address '%s'\n", listen_addr);
		return -1;
	}
	if (!port) {
		port = VRDMA_RPC_DEFAULT_PORT;
	}
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	if (getaddrinfo(host, port, &hints, &res) != 0) {
		free(tmp);
		SPDK_ERRLOG("Unable to look up RPC listen address '%s'\n", listen_addr);
		return -1;
	}
	srv->rpc_server = spdk_jsonrpc_server_listen(res->ai_family, res->ai_protocol,
			res->ai_addr, res->ai_addrlen,
			spdk_vrdma_srv_rpc_handler);
	freeaddrinfo(res);
	free(tmp);
	if (!srv->rpc_server) {
		SPDK_ERRLOG("spdk_jsonrpc_server_listen() failed\n");
		return -1;
	}
	return 0;
}

static void
spdk_vrdma_rpc_accept(void *arg)
{
	spdk_jsonrpc_server_poll(arg);
}

static int
spdk_vrdma_rpc_srv_poll(void *arg)
{
	spdk_vrdma_rpc_accept(arg);
	return -1;
}

static void
spdk_vrdma_rpc_server_configuration(void)
{
    struct spdk_vrdma_rpc_server *srv = &g_vrdma_rpc.srv;
    char *addr = g_vrdma_rpc.node_ip;

    /* Listen on the requested address */
    if (spdk_vrdma_rpc_listen(srv, addr)) {
        SPDK_ERRLOG("Failed to set listen '%s'\n", addr);
		return;
    }
    srv->rpc_state = SPDK_RPC_STARTUP;
	/* Register a poller to periodically check for RPCs */
	srv->rpc_poller = spdk_poller_register(spdk_vrdma_rpc_srv_poll,
            srv->rpc_server, VRDMA_RPC_SELECT_INTERVAL);
    spdk_vrdma_rpc_register_method(g_vrdma_qp_method_str,
        spdk_vrdma_rpc_srv_qp_req_handle,
        spdk_vrdma_client_qp_resp_handler);
    spdk_vrdma_rpc_register_method(g_vrdma_mkey_method_str,
        spdk_vrdma_rpc_srv_mkey_req_handle,
        spdk_vrdma_client_mkey_resp_handler);
}

/* Controller RPC configuration*/
struct spdk_vrdma_rpc_controller_configue_attr {
    char *emu_manager;
    int dev_id;
    char *mac;
    int dev_state;
    uint64_t adminq_paddr;
    uint32_t adminq_length;
    char *dest_mac;
    char *sf_mac;
    char *subnet_prefix;
    char *intf_id;
    int vrdma_qpn;
    int backend_rqpn;
	char *backend_dev;
	int src_addr_idx;
    char *node_ip;
    char *node_rip;
	int32_t show_vqpn;
    int backend_mtu;
};

static const struct spdk_json_object_decoder
spdk_vrdma_rpc_controller_configue_decoder[] = {
    {
        "emu_manager",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, emu_manager),
        spdk_json_decode_string
    },
    {
        "dev_id",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, dev_id),
        spdk_json_decode_int32
    },
    {
        "mac",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, mac),
        spdk_json_decode_string,
        true
    },
    {
        "dev_state",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, dev_state),
        spdk_json_decode_int32,
        true
    },
    {
        "adminq_paddr",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, adminq_paddr),
        spdk_json_decode_uint64,
        true
    },
    {
        "adminq_length",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, adminq_length),
        spdk_json_decode_uint32,
        true
    },
    {
        "dest_mac",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, dest_mac),
        spdk_json_decode_string,
        true
    },
    {
        "subnet_prefix",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, subnet_prefix),
        spdk_json_decode_string,
        true
    },
    {
        "intf_id",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, intf_id),
        spdk_json_decode_string,
        true
    },
    {
        "vrdma_qpn",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, vrdma_qpn),
        spdk_json_decode_int32,
        true
    },
    {
        "backend_rqpn",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, backend_rqpn),
        spdk_json_decode_int32,
        true
    },
    {
        "backend_dev",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, backend_dev),
        spdk_json_decode_string,
        true
    },
    {
        "src_addr_idx",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, src_addr_idx),
        spdk_json_decode_int32,
        true
    },
    {
        "sf_mac",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, sf_mac),
        spdk_json_decode_string,
        true
    },
    {
        "node_ip",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, node_ip),
        spdk_json_decode_string,
        true
    },
    {
        "node_rip",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, node_rip),
        spdk_json_decode_string,
        true
    },
    {
        "show_vqpn",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, show_vqpn),
        spdk_json_decode_uint32,
        true
    },
    {
        "backend_mtu",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, backend_mtu),
        spdk_json_decode_int32,
        true
    },
};

static struct spdk_emu_ctx *
spdk_emu_ctx_find_by_pci_id_testrpc(const char *emu_manager, int pf_id)
{
    struct spdk_emu_ctx *ctx;

    LIST_FOREACH(ctx, &spdk_emu_list, entry) {
        if (strncmp(ctx->emu_manager, emu_manager,
                    SPDK_EMU_MANAGER_NAME_MAXLEN))
            continue;
        if (ctx->spci->id == pf_id)
            return ctx;
    }
    return NULL;
}

static int
vrdma_rpc_parse_mac_into_int(char *arg, uint64_t *int_mac, char *mac)
{
    char vrdma_dev[MAX_VRDMA_DEV_LEN];
    char *str, *mac_str;
    char mac_arg[6] = {0};
    uint64_t temp_mac = 0;
    uint64_t ret_mac = 0;
    int i;

    if (!mac)
        mac = mac_arg;
    if (!int_mac)
        int_mac = &ret_mac;
    snprintf(vrdma_dev, MAX_VRDMA_DEV_LEN, "%s", arg);
    mac_str = vrdma_dev;
    for (i = 0; i < 6; i++) {
        if ((i < 5 && mac_str[2] != ':')) {
            return -EINVAL;
        }
        if (i < 5)
            str = strchr(mac_str, ':');
        else
            str = mac_str + 3;
        *str = '\0';
        mac[i] = spdk_strtol(mac_str, 16);
        temp_mac = mac[i] & 0xFF;
        *int_mac |= temp_mac << ((5-i) * 8);
        mac_str += 3;
    }
    return 0;
}

static void
spdk_vrdma_rpc_vqp_info_json(struct vrdma_ctrl *ctrl,
			struct spdk_vrdma_qp *vqp,
			struct spdk_json_write_ctx *w)
{

	spdk_json_write_object_begin(w);
    spdk_json_write_named_string(w, "sf_name", ctrl->vdev->vrdma_sf.sf_name);
    spdk_json_write_named_uint32(w, "sf_gvmi", ctrl->vdev->vrdma_sf.gvmi);
	if (vqp->bk_qp) {
		spdk_json_write_named_uint32(w, "remote_bk_qpn", vqp->bk_qp->remote_qpn);
	}
	spdk_json_write_named_uint32(w, "sq pi", vqp->qp_pi->pi.sq_pi);
	spdk_json_write_named_uint32(w, "sq pre pi", vqp->sq.comm.pre_pi);
	spdk_json_write_named_uint32(w, "scq pi", vqp->sq_vcq->pi);
	spdk_json_write_named_uint32(w, "scq ci", vqp->sq_vcq->pici->ci);
	spdk_json_write_named_uint64(w, "scq write cnt", vqp->stats.sq_cq_write_cnt);
	spdk_json_write_named_uint64(w, "scq total wqe", vqp->stats.sq_cq_write_wqe);
	spdk_json_write_named_uint32(w, "scq write cnt", vqp->stats.sq_cq_write_cqe_max);
	if (vqp->bk_qp) {
		spdk_json_write_named_uint32(w, "msq pi", vqp->bk_qp->bk_qp.hw_qp.sq.pi);
		spdk_json_write_named_uint32(w, "msq dbred pi", vqp->stats.msq_dbred_pi);
		spdk_json_write_named_uint64(w, "msq send dbr cnt", vqp->bk_qp->bk_qp.stat.tx.total_dbs);
		spdk_json_write_named_uint32(w, "mscq ci", vqp->bk_qp->bk_qp.sq_hw_cq.ci);
		spdk_json_write_named_uint32(w, "mscq dbred ci", vqp->stats.mcq_dbred_ci);
	}
	spdk_json_write_named_uint64(w, "sq tx dma cnt", vqp->stats.sq_dma_tx_cnt);
	spdk_json_write_named_uint64(w, "sq rx dma cnt", vqp->stats.sq_dma_rx_cnt);
	spdk_json_write_named_uint64(w, "sq wqe fetched", vqp->stats.sq_wqe_fetched);
	spdk_json_write_named_uint64(w, "sq wqe submitted", vqp->stats.sq_wqe_submitted);
    spdk_json_write_named_uint64(w, "sq wqe mkey invalid", vqp->stats.sq_wqe_mkey_invalid);
	spdk_json_write_named_uint64(w, "sq wqe wr submitted", vqp->stats.sq_wqe_wr);
	spdk_json_write_named_uint64(w, "sq wqe atomic submitted", vqp->stats.sq_wqe_atomic);
	spdk_json_write_named_uint64(w, "sq wqe ud submitted", vqp->stats.sq_wqe_ud);
	spdk_json_write_named_uint64(w, "sq wqe parse latency", vqp->stats.latency_parse);
	spdk_json_write_named_uint64(w, "sq wqe map latency", vqp->stats.latency_map);
	spdk_json_write_named_uint64(w, "sq wqe submit latency", vqp->stats.latency_submit);
	spdk_json_write_named_uint64(w, "sq wqe total latency", vqp->stats.latency_one_total);
    spdk_json_write_named_uint32(w, "last remote vkey_idx", vqp->wait_vkey);
    spdk_json_write_named_uint32(w, "last remote mkey", vqp->last_r_mkey);
    spdk_json_write_named_uint32(w, "last local vkey_idx", vqp->last_l_vkey);
    spdk_json_write_named_uint32(w, "last local mkey", vqp->last_l_mkey);
    if (vqp->bk_qp) {
	    spdk_json_write_named_uint32(w, "msq pi", vqp->bk_qp->bk_qp.hw_qp.sq.pi);
	    spdk_json_write_named_uint32(w, "msq dbred pi", vqp->stats.msq_dbred_pi);
	    spdk_json_write_named_uint64(w, "msq send dbr cnt", vqp->bk_qp->bk_qp.stat.tx.total_dbs);
	    spdk_json_write_named_uint32(w, "mscq ci", vqp->bk_qp->bk_qp.sq_hw_cq.ci);
	    spdk_json_write_named_uint32(w, "mscq dbred ci", vqp->stats.mcq_dbred_ci);
    }
    spdk_json_write_object_end(w);
}

static void
spdk_vrdma_rpc_controller_configue(struct spdk_jsonrpc_request *request,
                                    const struct spdk_json_val *params)
{
    struct spdk_vrdma_rpc_controller_configue_attr *attr = NULL;
    struct spdk_json_write_ctx *w;
    struct spdk_emu_ctx *ctx;
    struct snap_vrdma_ctrl *sctrl;
    struct vrdma_ctrl *ctrl;
    struct spdk_vrdma_qp *vqp;
    bool send_vqp_result = false;

    attr = calloc(1, sizeof(*attr));
    if (!attr)
        goto invalid;

    // Set invalid index, to identify when value was not decoded
    attr->dev_id = -1;
    attr->dev_state = -1;
    attr->vrdma_qpn = -1;
    attr->backend_rqpn = -1;
	attr->src_addr_idx = -1;;
	attr->show_vqpn = -1;
    attr->backend_mtu = -1;

    if (spdk_json_decode_object(params,
            spdk_vrdma_rpc_controller_configue_decoder,
            SPDK_COUNTOF(spdk_vrdma_rpc_controller_configue_decoder),
            attr)) {
        SPDK_ERRLOG("Failed to decode parameters\n");
        goto free_attr;
    }
    if (attr->dev_id == -1 || !attr->emu_manager) {
        SPDK_ERRLOG("invalid device id -1\n");
        goto free_attr;
    }
    /* Find device data */
    ctx = spdk_emu_ctx_find_by_pci_id(attr->emu_manager,
                           attr->dev_id);
    if (!ctx) {
        ctx = spdk_emu_ctx_find_by_pci_id_testrpc(attr->emu_manager,
                           attr->dev_id);
        if (!ctx) {
            SPDK_ERRLOG("Fail to find device for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
    }
    if (attr->mac) {
        ctrl = ctx->ctrl;
        if (!ctrl) {
            SPDK_ERRLOG("Fail to find device controller for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        sctrl = ctrl->sctrl;
        if (!sctrl) {
            SPDK_ERRLOG("Fail to find device snap controller for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        sctrl->mac = 0;
        if (vrdma_rpc_parse_mac_into_int(attr->mac, &sctrl->mac, NULL)) {
            SPDK_ERRLOG("Fail to parse mac string %s for emu_manager %s\n",
            attr->mac, attr->emu_manager);
            goto free_attr;
        }
        g_bar_test.mac = sctrl->mac;
        if (snap_vrdma_device_mac_init(sctrl)) {
            SPDK_ERRLOG("Fail to change MAC after driver_ok for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
    }
    if (attr->dev_state != -1) {
        g_bar_test.status = attr->dev_state;
    }
    if (attr->adminq_paddr && attr->adminq_length) {
        g_bar_test.enabled = 1;
        g_bar_test.status = 4; /* driver_ok */
        g_bar_test.adminq_base_addr = attr->adminq_paddr;
        g_bar_test.adminq_size = attr->adminq_length;
    }
    if (attr->dest_mac) {
        struct vrdma_backend_qp *bk_qp;

        ctrl = ctx->ctrl;
        if (!ctrl) {
            SPDK_ERRLOG("Fail to find device controller for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        if (attr->vrdma_qpn == -1) {
            if (vrdma_rpc_parse_mac_into_int(attr->dest_mac, NULL,
                ctrl->vdev->vrdma_sf.dest_mac)) {
                SPDK_ERRLOG("Fail to parse dest_mac string %s for emu_manager %s\n",
                    attr->dest_mac, attr->emu_manager);
                goto free_attr;
            }
        } else {
            vqp = find_spdk_vrdma_qp_by_idx(ctrl, attr->vrdma_qpn);
            if (!vqp) {
                SPDK_ERRLOG("Fail to find vrdma_qpn %d for emu_manager %s\n",
                    attr->vrdma_qpn, attr->emu_manager);
                goto free_attr;
            }
            bk_qp = vqp->bk_qp;
            if (!bk_qp) {
                SPDK_ERRLOG("Fail to find vrdma_qpn %d's backend qp for emu_manager %s\n",
                    attr->vrdma_qpn, attr->emu_manager);
                goto free_attr;
            }
        }
    }
    if (attr->sf_mac) {
        ctrl = ctx->ctrl;
        if (!ctrl) {
            SPDK_ERRLOG("Fail to find device controller for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        if (!attr->backend_dev) {
            SPDK_ERRLOG("Invalid SF device for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        if (vrdma_rpc_parse_mac_into_int(attr->sf_mac, NULL,
            ctrl->vdev->vrdma_sf.mac)) {
            SPDK_ERRLOG("Fail to parse sf_mac string %s for emu_manager %s\n",
                    attr->sf_mac, attr->emu_manager);
            goto free_attr;
        }
    }
    if (attr->backend_rqpn != -1) {
        struct vrdma_backend_qp *bk_qp;

        ctrl = ctx->ctrl;
        if (!ctrl) {
            SPDK_ERRLOG("Fail to find device controller for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        if (attr->vrdma_qpn == -1) {
            SPDK_ERRLOG("Invalid vrdma_qpn for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        vqp = find_spdk_vrdma_qp_by_idx(ctrl, attr->vrdma_qpn);
        if (!vqp) {
            SPDK_ERRLOG("Fail to find vrdma_qpn %d for emu_manager %s\n",
                    attr->vrdma_qpn, attr->emu_manager);
            goto free_attr;
        }
        bk_qp = vqp->bk_qp;
        if (!bk_qp) {
            SPDK_ERRLOG("Fail to find vrdma_qpn %d's backend qp for emu_manager %s\n",
                    attr->vrdma_qpn, attr->emu_manager);
            goto free_attr;
        }
        bk_qp->remote_qpn = attr->backend_rqpn;
    }
    if (attr->subnet_prefix) {
        struct in_addr inaddr;
        uint64_t subnet_prefix;

        ctrl = ctx->ctrl;
        if (!ctrl) {
            SPDK_ERRLOG("Fail to find device controller for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        inet_aton(attr->subnet_prefix, &inaddr);
        subnet_prefix = inaddr.s_addr;
        subnet_prefix = subnet_prefix << 32;
        if (attr->vrdma_qpn == -1) {
            ctrl->vdev->vrdma_sf.remote_ip = subnet_prefix;
        }
    }
    if (attr->intf_id) {
        struct in_addr inaddr;
        uint64_t intf_id;

        ctrl = ctx->ctrl;
        if (!ctrl) {
            SPDK_ERRLOG("Fail to find device controller for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        inet_aton(attr->intf_id, &inaddr);
        intf_id = inaddr.s_addr;
        intf_id = intf_id << 32;
        if (attr->vrdma_qpn == -1) {
            ctrl->vdev->vrdma_sf.ip = intf_id;
        }
    }
	if (attr->backend_dev) {
		uint8_t name_size;

        ctrl = ctx->ctrl;
        if (!ctrl) {
            SPDK_ERRLOG("Fail to find device controller for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
		name_size = strlen(attr->backend_dev);
		if (name_size > (VRDMA_DEV_NAME_LEN - 1)) {
			SPDK_ERRLOG("invalid sf name %s, len %d\n", attr->backend_dev, name_size);
			name_size = VRDMA_DEV_NAME_LEN - 1;
		}
		memcpy(ctrl->vdev->vrdma_sf.sf_name, attr->backend_dev, name_size);
		ctrl->vdev->vrdma_sf.sf_name[name_size] = '\0';
        if (attr->backend_mtu != -1) {
            ctrl->vdev->vrdma_sf.mtu = attr->backend_mtu;
        }
    }
	if (attr->src_addr_idx != -1) {
        ctrl = ctx->ctrl;
        if (!ctrl) {
            SPDK_ERRLOG("Fail to find device controller for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        if (attr->vrdma_qpn == -1) {
            ctrl->vdev->vrdma_sf.gid_idx = attr->src_addr_idx;
        } else {
            struct vrdma_backend_qp *bk_qp;

            vqp = find_spdk_vrdma_qp_by_idx(ctrl, attr->vrdma_qpn);
            if (!vqp) {
                SPDK_ERRLOG("Fail to find vrdma_qpn %d for emu_manager %s\n",
                    attr->vrdma_qpn, attr->emu_manager);
                goto free_attr;
            }
            bk_qp = vqp->bk_qp;
            if (!bk_qp) {
                SPDK_ERRLOG("Fail to find vrdma_qpn %d's backend qp for emu_manager %s\n",
                    attr->vrdma_qpn, attr->emu_manager);
                goto free_attr;
            }
        }
    }
    if (attr->node_ip) {
        struct in_addr inaddr;
        uint32_t ip_len;

		ip_len = strlen(attr->node_ip);
		if (ip_len > (VRDMA_RPC_IP_LEN - 5)) {
			SPDK_ERRLOG("invalid node ip %s, len %d\n",
            attr->node_ip, ip_len);
		}
		snprintf(g_vrdma_rpc.node_ip, VRDMA_RPC_IP_LEN, "%s:%s",
            attr->node_ip, VRDMA_RPC_DEFAULT_PORT);
        spdk_vrdma_rpc_server_configuration();
        inet_aton(attr->node_ip, &inaddr);
        g_node_ip = inaddr.s_addr;
        g_node_ip = g_node_ip << 32;
    }
    if (attr->node_rip) {
        struct in_addr inaddr;
        uint32_t ip_len;

		ip_len = strlen(attr->node_rip);
		if (ip_len > (VRDMA_RPC_IP_LEN - 5)) {
			SPDK_ERRLOG("invalid remote node ip %s, len %d\n",
            attr->node_rip, ip_len);
		}
		snprintf(g_vrdma_rpc.node_rip, VRDMA_RPC_IP_LEN, "%s:%s",
            attr->node_rip, VRDMA_RPC_DEFAULT_PORT);
        inet_aton(attr->node_rip, &inaddr);
        g_node_rip = inaddr.s_addr;
        g_node_rip = g_node_rip << 32;
    }
	if (attr->show_vqpn != -1) {
        ctrl = ctx->ctrl;
        if (!ctrl) {
            SPDK_ERRLOG("Fail to find device controller for emu_manager %s\n",
                attr->emu_manager);
            goto free_attr;
        }
		vqp = find_spdk_vrdma_qp_by_idx(ctrl, attr->show_vqpn);
        if (!vqp) {
            SPDK_ERRLOG("show vqpn stats: Fail to find vrdma_qpn %d for emu_manager %s\n",
                attr->show_vqpn, attr->emu_manager);
            goto free_attr;
        }
		vrdma_dump_vqp_stats(ctrl, vqp);
        send_vqp_result = true;
    }
    w = spdk_jsonrpc_begin_result(request);
    if (send_vqp_result)
        spdk_vrdma_rpc_vqp_info_json(ctrl, vqp, w);
    else
        spdk_json_write_string(w, "Success");
    spdk_jsonrpc_end_result(request, w);

    free(attr);
    return;

free_attr:
    free(attr);
invalid:
    spdk_jsonrpc_send_error_response(request,
                     SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                     "Invalid parameters");
}

SPDK_RPC_REGISTER("controller_vrdma_configue",
                  spdk_vrdma_rpc_controller_configue, SPDK_RPC_RUNTIME)
