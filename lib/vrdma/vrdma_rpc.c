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

static char *g_vrdma_qp_method_str = "VRDMA_RPC_SRV_QP";
static SLIST_HEAD(, spdk_vrdma_rpc_method) g_vrdma_rpc_methods = SLIST_HEAD_INITIALIZER(g_vrdma_rpc_methods);
struct spdk_vrdma_rpc g_vrdma_rpc;
uint64_t g_node_ip = 0;
uint64_t g_node_rip = 0;

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
    SPDK_NOTICELOG("lizh spdk_vrdma_close_rpc_client...start\n");
	spdk_poller_unregister(&client->client_conn_poller);
	if (client->client_conn)
		spdk_jsonrpc_client_close(client->client_conn);
}

static int
spdk_vrdma_rpc_client_poller(void *arg)
{
	struct spdk_vrdma_rpc_client *client = arg;
	struct spdk_jsonrpc_client_response *resp;
	vrdma_client_resp_handler cb;
	int rc;

    //SPDK_NOTICELOG("lizh spdk_vrdma_sf_rpc_client_poller...start\n");
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
		cb = client->client_resp_cb;
		assert(cb != NULL);
		/* Mark we are done with this handler. */
		client->client_resp_cb = NULL;
		cb(client, resp);
	}
	return -1;
}

static int
spdk_vrdma_client_connect_poller(void *arg)
{
	struct spdk_vrdma_rpc_client *client = arg;
	int rc;

    //SPDK_NOTICELOG("lizh spdk_vrdma_client_connect_poller...start\n");
	rc = spdk_jsonrpc_client_poll(client->client_conn, 0);
	if (rc != -ENOTCONN) {
		/* We are connected. Start regular poller and issue first request */
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
spdk_vrdma_rpc_qp_resp_decoder[] = {
    {
        "emu_manager",
        offsetof(struct spdk_vrdma_rpc_qp_attr, emu_manager),
        spdk_json_decode_string,
    },
    {
        "node",
        offsetof(struct spdk_vrdma_rpc_qp_attr, node_id),
        spdk_json_decode_uint64,
        true
    },
    {
        "device",
        offsetof(struct spdk_vrdma_rpc_qp_attr, dev_id),
        spdk_json_decode_uint32,
        true
    },
    {
        "vqpn",
        offsetof(struct spdk_vrdma_rpc_qp_attr, vqpn),
        spdk_json_decode_uint32,
        true
    },
    {
        "gid",
        offsetof(struct spdk_vrdma_rpc_qp_attr, gid_ip),
        spdk_json_decode_uint64,
        true
    },
    {
        "remote_node",
        offsetof(struct spdk_vrdma_rpc_qp_attr, remote_node_id),
        spdk_json_decode_uint64,
        true
    },
    {
        "remote_device",
        offsetof(struct spdk_vrdma_rpc_qp_attr, remote_dev_id),
        spdk_json_decode_uint32,
        true
    },
    {
        "remote_vqpn",
        offsetof(struct spdk_vrdma_rpc_qp_attr, remote_vqpn),
        spdk_json_decode_uint32,
        true
    },
    {
        "remote_gid",
        offsetof(struct spdk_vrdma_rpc_qp_attr, remote_gid_ip),
        spdk_json_decode_uint64,
        true
    },
    {
        "bkqpn",
        offsetof(struct spdk_vrdma_rpc_qp_attr, bk_qpn),
        spdk_json_decode_uint32,
        true
    },
    {
        "state",
        offsetof(struct spdk_vrdma_rpc_qp_attr, qp_state),
        spdk_json_decode_uint32,
        true
    },
    {
        "mac",
        offsetof(struct spdk_vrdma_rpc_qp_attr, sf_mac),
        spdk_json_decode_uint64,
        true
    },
};

static void
spdk_vrdma_client_qp_resp_handler(struct spdk_vrdma_rpc_client *client,
				    struct spdk_jsonrpc_client_response *resp)
{
    struct vrdma_remote_bk_qp_attr qp_attr;
    struct spdk_vrdma_rpc_qp_attr *attr;
    struct spdk_emu_ctx *ctx;
    struct vrdma_ctrl *ctrl;
    uint32_t i;

    attr = calloc(1, sizeof(*attr));
    if (!attr)
        goto close_rpc;

    SPDK_NOTICELOG("lizh spdk_vrdma_client_qp_resp_handler...start\n");
    if (spdk_json_decode_object(resp->result,
            spdk_vrdma_rpc_qp_resp_decoder,
            SPDK_COUNTOF(spdk_vrdma_rpc_qp_resp_decoder),
            attr)) {
        SPDK_ERRLOG("Failed to decode result for qp_msg\n");
        goto free_attr;
    }
    if (!attr->gid_ip) {
        SPDK_NOTICELOG("Skip decode result for zero gid_ip\n");
        goto free_attr;
    }
    SPDK_NOTICELOG("lizh spdk_vrdma_client_qp_resp_handler decode:\n"
    "emu_manager %s node_id=0x%lx  dev_id=0x%x vqpn=0x%x gid_ip=0x%lx "
    "mac=0x%lx remote_node_id=0x%lx remote_dev_id =0x%x "
    "remote_vqpn=0x%x remote_gid_ip=0x%lx bk_qpn=0x%x\n",
    attr->emu_manager, attr->node_id, attr->dev_id, attr->vqpn,
    attr->gid_ip, attr->sf_mac, attr->remote_node_id, attr->remote_dev_id,
    attr->remote_vqpn, attr->remote_gid_ip, attr->bk_qpn);
    /* Find device data by remote_gid_ip (remote SF IP)*/
    ctx = spdk_emu_ctx_find_by_gid_ip(attr->emu_manager, attr->remote_gid_ip);
    if (!ctx) {
        SPDK_ERRLOG("Fail to find device for emu_manager %s\n",
                attr->emu_manager);
        goto free_attr;
    }
    ctrl = ctx->ctrl;
    if (!ctrl) {
        SPDK_ERRLOG("Fail to find device controller for emu_manager %s\n",
            attr->emu_manager);
        goto free_attr;
    }
	/* update qp data */
    qp_attr.comm.node_id = attr->node_id;
    qp_attr.comm.dev_id = attr->dev_id;
    qp_attr.comm.vqpn = attr->vqpn;
    qp_attr.comm.gid_ip = attr->gid_ip;
    for (i = 0; i < 6; i++)
        qp_attr.comm.mac[5-i] = (attr->sf_mac >> (i * 8)) & 0xFF;
    if (vrdma_add_rbk_qp_list(ctrl, attr->remote_gid_ip,
        attr->remote_vqpn, attr->bk_qpn, &qp_attr)) {
        SPDK_ERRLOG("Fail to add remote backend qp %d "
        "in list for emu_manager %s\n",
        attr->bk_qpn, attr->emu_manager);
    }
free_attr:
    free(attr);
close_rpc:
	spdk_jsonrpc_client_free_response(resp);
    spdk_vrdma_close_rpc_client(client);
    return;
}

static int
spdk_vrdma_client_send_request(struct spdk_vrdma_rpc_client *client,
		struct spdk_jsonrpc_client_request *request,
		vrdma_client_resp_handler client_resp_cb)
{
	int rc;

    SPDK_NOTICELOG("lizh spdk_vrdma_sf_client_send_request...start\n");
	client->client_resp_cb = client_resp_cb;
	spdk_vrdma_rpc_client_set_timeout(client,
            VRDMA_RPC_CLIENT_REQUEST_TIMEOUT_US);
	rc = spdk_jsonrpc_client_send_request(client->client_conn, request);
	if (rc)
		SPDK_ERRLOG("Sending request to client failed (%d)\n", rc);
	return rc;
}

static int
spdk_vrdma_rpc_client_configuration(struct vrdma_ctrl *ctrl, const char *addr)
{
    struct spdk_vrdma_rpc_client *client = &g_vrdma_rpc.client;

    SPDK_NOTICELOG("lizh spdk_vrdma_sf_rpc_client_configuration...ipaddr %s\n", addr);
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

static const struct spdk_json_object_decoder
spdk_vrdma_rpc_qp_req_decoder[] = {
    {
        "emu_manager",
        offsetof(struct spdk_vrdma_rpc_qp_attr, emu_manager),
        spdk_json_decode_string
    },
    {
        "node",
        offsetof(struct spdk_vrdma_rpc_qp_attr, node_id),
        spdk_json_decode_uint64
    },
    {
        "device",
        offsetof(struct spdk_vrdma_rpc_qp_attr, dev_id),
        spdk_json_decode_uint32
    },
    {
        "vqpn",
        offsetof(struct spdk_vrdma_rpc_qp_attr, vqpn),
        spdk_json_decode_uint32
    },
    {
        "gid",
        offsetof(struct spdk_vrdma_rpc_qp_attr, gid_ip),
        spdk_json_decode_uint64
    },
    {
        "remote_node",
        offsetof(struct spdk_vrdma_rpc_qp_attr, remote_node_id),
        spdk_json_decode_uint64
    },
    {
        "remote_device",
        offsetof(struct spdk_vrdma_rpc_qp_attr, remote_dev_id),
        spdk_json_decode_uint32
    },
    {
        "remote_vqpn",
        offsetof(struct spdk_vrdma_rpc_qp_attr, remote_vqpn),
        spdk_json_decode_uint32
    },
    {
        "remote_gid",
        offsetof(struct spdk_vrdma_rpc_qp_attr, remote_gid_ip),
        spdk_json_decode_uint64,
        true
    },
    {
        "bkqpn",
        offsetof(struct spdk_vrdma_rpc_qp_attr, bk_qpn),
        spdk_json_decode_uint32
    },
    {
        "state",
        offsetof(struct spdk_vrdma_rpc_qp_attr, qp_state),
        spdk_json_decode_uint32
    },
    {
        "mac",
        offsetof(struct spdk_vrdma_rpc_qp_attr, sf_mac),
        spdk_json_decode_uint64
    },
};

static void
spdk_vrdma_rpc_qp_info_json(struct spdk_vrdma_rpc_qp_msg *info,
			 struct spdk_json_write_ctx *w, bool send_qp_info)
{
    uint64_t temp, sf_mac = 0;
    int i;

	spdk_json_write_object_begin(w);
    spdk_json_write_named_string(w, "emu_manager", info->emu_manager);
    if (send_qp_info) {
	    spdk_json_write_named_uint64(w, "node", info->qp_attr.node_id);
        spdk_json_write_named_uint32(w, "device", info->qp_attr.dev_id);
        spdk_json_write_named_uint32(w, "vqpn", info->qp_attr.vqpn);
        spdk_json_write_named_uint64(w, "gid", info->qp_attr.gid_ip);
	    spdk_json_write_named_uint64(w, "remote_node", info->remote_node_id);
        spdk_json_write_named_uint32(w, "remote_device", info->remote_dev_id);
        spdk_json_write_named_uint32(w, "remote_vqpn", info->remote_vqpn);
        spdk_json_write_named_uint64(w, "remote_gid", info->remote_gid_ip);
        spdk_json_write_named_uint32(w, "bkqpn", info->bk_qpn);
        spdk_json_write_named_uint32(w, "state", info->qp_state);
        for (i = 0; i < 6; i++) {
            temp = info->qp_attr.mac[5-i] & 0xFF;
            sf_mac |= temp << (i * 8);
        }
        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_qp_info_json...mac=0x%lx gid_ip=0x%lx\n",
        sf_mac, info->qp_attr.gid_ip);
        spdk_json_write_named_uint64(w, "mac", sf_mac);
    }
    spdk_json_write_object_end(w);
}

static int
spdk_vrdma_rpc_client_send_qp_msg(struct vrdma_ctrl *ctrl,
            struct spdk_vrdma_rpc_qp_msg *msg)
{
    struct spdk_vrdma_rpc_client *client = &g_vrdma_rpc.client;
	struct spdk_jsonrpc_client_request *rpc_request;
	struct spdk_json_write_ctx *w;
	int rc;

    SPDK_NOTICELOG("lizh spdk_vrdma_rpc_client_send_qp_msg...vqpn %d\n",
        msg->qp_attr.vqpn);
	rpc_request = spdk_jsonrpc_client_create_request();
	if (!rpc_request) {
        SPDK_ERRLOG("Failed to create request for vqp %d\n",
            msg->qp_attr.vqpn);
		goto out;
	}
	w = spdk_jsonrpc_begin_request(rpc_request, 1, g_vrdma_qp_method_str);
	if (!w) {
		spdk_jsonrpc_client_free_request(rpc_request);
        SPDK_ERRLOG("Failed to build request for vqp %d\n",
            msg->qp_attr.vqpn);
		goto out;
	}
    spdk_json_write_name(w, "params");
    spdk_vrdma_rpc_qp_info_json(msg, w, true);
	spdk_jsonrpc_end_request(rpc_request, w);

	rc = spdk_vrdma_client_send_request(client, rpc_request,
            spdk_vrdma_client_qp_resp_handler);
	if (rc != 0) {
        SPDK_ERRLOG("Failed to send request for vqp %d\n",
            msg->qp_attr.vqpn);
		goto out;
	}
    SPDK_NOTICELOG("lizh spdk_vrdma_rpc_client_send_qp_msg...vqpn %d...done\n",
        msg->qp_attr.vqpn);
    return 0;
out:
	spdk_vrdma_close_rpc_client(client);
    return -1;
}

int spdk_vrdma_rpc_send_qp_msg(struct vrdma_ctrl *ctrl, const char *addr,
                struct spdk_vrdma_rpc_qp_msg *msg)
{
    SPDK_NOTICELOG("lizh spdk_vrdma_rpc_send_qp_msg...vqpn %d...start\n",
        msg->qp_attr.vqpn);
    if (spdk_vrdma_rpc_client_configuration(ctrl, addr)) {
        SPDK_ERRLOG("Failed to client configuration for vqp %d\n",
            msg->qp_attr.vqpn);
        return -1;
    }
    if (spdk_vrdma_rpc_client_send_qp_msg(ctrl, msg)) {
        SPDK_ERRLOG("Failed to send request for vqp %d\n",
            msg->qp_attr.vqpn);
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
spdk_vrdma_rpc_register_method(const char *method, spdk_rpc_method_handler func)
{
	struct spdk_vrdma_rpc_method *m;

    SPDK_NOTICELOG("lizh spdk_vrdma_rpc_register_method..%s.start\n", method);
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
	/* TODO: use a hash table or sorted list */
	SLIST_INSERT_HEAD(&g_vrdma_rpc_methods, m, slist);
}

static void
spdk_vrdma_rpc_srv_qp_req_handle(struct spdk_jsonrpc_request *request,
            const struct spdk_json_val *params)
{
    struct vrdma_remote_bk_qp_attr qp_attr;
    struct spdk_vrdma_rpc_qp_attr *attr;
    struct spdk_json_write_ctx *w;
    struct spdk_emu_ctx *ctx;
    struct vrdma_ctrl *ctrl = NULL;
    struct spdk_vrdma_rpc_qp_msg msg = {0};
    struct vrdma_remote_bk_qp *rqp;
    struct vrdma_local_bk_qp *lqp;
    bool send_lqp_info = false;
    uint32_t i;

    SPDK_NOTICELOG("lizh spdk_vrdma_rpc_srv_qp_req_handle...start\n");
    attr = calloc(1, sizeof(*attr));
    if (!attr)
        goto invalid;

    if (spdk_json_decode_object(params,
            spdk_vrdma_rpc_qp_req_decoder,
            SPDK_COUNTOF(spdk_vrdma_rpc_qp_req_decoder),
            attr)) {
        SPDK_ERRLOG("Failed to decode parameters for \n");
        goto invalid;
    }
    if (!attr->emu_manager) {
        SPDK_ERRLOG("invalid emu_manager\n");
        goto invalid;
    }
    SPDK_NOTICELOG("lizh spdk_vrdma_rpc_srv_qp_req_handle decode:\n"
    "emu_manager %s node_id=0x%lx  dev_id=0x%x vqpn=0x%x gid_ip=0x%lx "
    "mac=0x%lx remote_node_id=0x%lx remote_dev_id =0x%x remote_vqpn=0x%x "
    "remote_gid_ip=0x%lx bk_qpn=0x%x qp_state=%d\n",
    attr->emu_manager, attr->node_id, attr->dev_id, attr->vqpn,
    attr->gid_ip, attr->sf_mac, attr->remote_node_id, attr->remote_dev_id,
    attr->remote_vqpn, attr->remote_gid_ip, attr->bk_qpn, attr->qp_state);
    if (attr->qp_state == SPDK_VRDMA_RPC_QP_DESTROYED) {
        /* Delete remote qp entry */
        rqp = vrdma_find_rbk_qp_by_vqp(attr->gid_ip, attr->vqpn);
        if (rqp)
            vrdma_del_rbk_qp_from_list(rqp);
        goto send_result;
    }
    /* Find device data by remote_gid_ip (remote SF IP)*/
    ctx = spdk_emu_ctx_find_by_gid_ip(attr->emu_manager, attr->remote_gid_ip);
    if (ctx) {
        ctrl = ctx->ctrl;
        if (!ctrl) {
            SPDK_ERRLOG("Fail to find device controller for emu_manager %s\n",
                attr->emu_manager);
            goto invalid;
        }
    }
	/* update qp data */
    qp_attr.comm.node_id = attr->node_id;
    qp_attr.comm.dev_id = attr->dev_id;
    qp_attr.comm.vqpn = attr->vqpn;
    qp_attr.comm.gid_ip = attr->gid_ip;
    for (i = 0; i < 6; i++)
        qp_attr.comm.mac[5-i] = (attr->sf_mac >> (i * 8)) & 0xFF;
    if (vrdma_add_rbk_qp_list(ctrl, attr->remote_gid_ip,
        attr->remote_vqpn, attr->bk_qpn, &qp_attr)) {
        SPDK_ERRLOG("Fail to add remote backend qp %d "
            "in list for emu_manager %s\n",
            attr->bk_qpn, attr->emu_manager);
        goto invalid;
    }
    if (attr->qp_state == SPDK_VRDMA_RPC_QP_WAIT_RQPN) {
        /* Send local qp message */
        lqp = vrdma_find_lbk_qp_by_vqp(attr->remote_gid_ip,
            attr->remote_vqpn);
        if (lqp) {
            memcpy(&msg.qp_attr, &lqp->attr.comm,
                sizeof(struct vrdma_bk_qp_connect));
	        msg.remote_node_id = lqp->remote_node_id;
	        msg.remote_dev_id = lqp->remote_dev_id;
            msg.remote_vqpn = attr->vqpn;
            msg.remote_gid_ip = lqp->remote_gid_ip;
            msg.bk_qpn = lqp->bk_qpn;
            msg.qp_state = SPDK_VRDMA_RPC_QP_READY;
            send_lqp_info = true;
        }
    }
send_result:
    w = spdk_jsonrpc_begin_result(request);
    msg.emu_manager = attr->emu_manager;
    spdk_vrdma_rpc_qp_info_json(&msg, w, send_lqp_info);
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

    SPDK_NOTICELOG("lizh spdk_vrdma_srv_rpc_handler...start\n");
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

    SPDK_NOTICELOG("lizh spdk_vrdma_rpc_listen...listen_addr %s\n", listen_addr);
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

    SPDK_NOTICELOG("lizh spdk_vrdma_sf_rpc_service_configuration...ipaddr %s\n", addr);
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
        spdk_vrdma_rpc_srv_qp_req_handle);
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
};

static struct spdk_emu_ctx *
spdk_emu_ctx_find_by_pci_id_testrpc(const char *emu_manager, int pf_id)
{
    struct spdk_emu_ctx *ctx;

    LIST_FOREACH(ctx, &spdk_emu_list, entry) {
        
        SPDK_NOTICELOG("lizh spdk_emu_ctx_find_by_pci_id...%s type %d id %d\n",
        ctx->emu_manager, ctx->spci->type, ctx->spci->id);
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

    SPDK_NOTICELOG("lizh vrdma_rpc_parse_mac_into_int arg %s \n", arg);
    snprintf(vrdma_dev, MAX_VRDMA_DEV_LEN, "%s", arg);
    mac_str = vrdma_dev;
    SPDK_NOTICELOG("lizh vrdma_rpc_parse_mac_into_int mac_str %s \n", mac_str);
    for (i = 0; i < 6; i++) {
        SPDK_NOTICELOG("lizh vrdma_rpc_parse_mac_into_int mac_str[0] 0x%x \n", mac_str[0]);
        if ((i < 5 && mac_str[2] != ':')) {
            SPDK_NOTICELOG("lizh vrdma_rpc_parse_mac_into_int mac_str[2] 0x%x\n", mac_str[2]);
            return -EINVAL;
        }
        if (i < 5)
            str = strchr(mac_str, ':');
        else
            str = mac_str + 3;
        *str = '\0';
        SPDK_NOTICELOG("lizh vrdma_rpc_parse_mac_into_int mac_str %s \n", mac_str);
        mac[i] = spdk_strtol(mac_str, 16);
        SPDK_NOTICELOG("lizh vrdma_rpc_parse_mac_into_int mac[%d] 0x%x \n", i, mac[i]);
        temp_mac = mac[i] & 0xFF;
        SPDK_NOTICELOG("lizh vrdma_rpc_parse_mac_into_int temp_mac 0x%lx \n", temp_mac);
        *int_mac |= temp_mac << ((5-i) * 8);
        SPDK_NOTICELOG("lizh vrdma_rpc_parse_mac_into_int int_mac 0x%lx \n", *int_mac);
        mac_str += 3;
    }
    return 0;
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

    SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...start\n");
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
        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...mac %s\n", attr->mac);
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
        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...dev_state=0x%x\n", attr->dev_state);
        g_bar_test.status = attr->dev_state;
    }
    if (attr->adminq_paddr && attr->adminq_length) {
        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...adminq_paddr=0x%lx adminq_length %d\n",
        attr->adminq_paddr, attr->adminq_length);
        g_bar_test.enabled = 1;
        g_bar_test.status = 4; /* driver_ok */
        g_bar_test.adminq_base_addr = attr->adminq_paddr;
        g_bar_test.adminq_size = attr->adminq_length;
    }
    if (attr->dest_mac) {
        struct vrdma_backend_qp *bk_qp;

        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...dest_mac %s\n", attr->dest_mac);
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
            if (vrdma_rpc_parse_mac_into_int(attr->dest_mac, NULL,
                bk_qp->dest_mac)) {
                SPDK_ERRLOG("Fail to parse dest_mac string %s for emu_manager %s\n",
                    attr->dest_mac, attr->emu_manager);
                goto free_attr;
            }
        }
    }
    if (attr->sf_mac) {
        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...sf_mac %s\n", attr->sf_mac);
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

        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...backend_rqpn=0x%x\n", attr->backend_rqpn);
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
        struct vrdma_backend_qp *bk_qp;
        struct in_addr inaddr;
        uint64_t subnet_prefix;

        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...subnet_prefix %s\n", attr->subnet_prefix);
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
            SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...remote_ip=0x%lx..done\n",
            ctrl->vdev->vrdma_sf.remote_ip);
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
            bk_qp->rgid_rip.global.subnet_prefix = subnet_prefix;
        }
    }
    if (attr->intf_id) {
        struct in_addr inaddr;
        uint64_t intf_id;

        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...intf_id %s\n", attr->intf_id);
        ctrl = ctx->ctrl;
        if (!ctrl) {
            SPDK_ERRLOG("Fail to find device controller for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        inet_aton(attr->intf_id, &inaddr);
        intf_id = inaddr.s_addr;
        intf_id = intf_id << 32;
        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...intf_id %s inaddr 0x%x\n", 
        attr->intf_id, inaddr.s_addr);
        if (attr->vrdma_qpn == -1) {
            ctrl->vdev->vrdma_sf.ip = intf_id;
            SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...sf ip=0x%lx.intf_id=0x%lx.done\n",
            ctrl->vdev->vrdma_sf.ip, intf_id);
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
            bk_qp->rgid_rip.global.interface_id = intf_id;
        }
    }
	if (attr->backend_dev) {
		uint8_t name_size;
		
        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...backend_dev %s\n", attr->backend_dev);
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
		SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...backend_dev done, sf name %s\n",
						ctrl->vdev->vrdma_sf.sf_name);
    }
	if (attr->src_addr_idx != -1) {
        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...src_addr_idx=0x%x\n",
        attr->src_addr_idx);
        ctrl = ctx->ctrl;
        if (!ctrl) {
            SPDK_ERRLOG("Fail to find device controller for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        if (attr->vrdma_qpn == -1) {
            ctrl->vdev->vrdma_sf.gid_idx = attr->src_addr_idx;
            SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...gid_idx=0x%x\n",
            ctrl->vdev->vrdma_sf.gid_idx);
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
            bk_qp->src_addr_idx = attr->src_addr_idx;
        }
    }
    if (attr->node_ip) {
        struct in_addr inaddr;
        uint32_t ip_len;

        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...node_ip %s\n", attr->node_ip);
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
        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...node_ip %s g_node_ip 0x%lx\n",
        g_vrdma_rpc.node_ip, g_node_ip);
    }
    if (attr->node_rip) {
        struct in_addr inaddr;
        uint32_t ip_len;

        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...node_rip %s\n",
        attr->node_rip);
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
        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...node_rip %s g_node_rip 0x%lx\n",
        g_vrdma_rpc.node_rip, g_node_rip);
    }
	if (attr->show_vqpn != -1) {
        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...show_vqpn=0x%x\n",
        attr->show_vqpn);
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
		vrdma_dump_vqp_stats(vqp);
    }
    w = spdk_jsonrpc_begin_result(request);
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
