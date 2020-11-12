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

#include "iscsi/iscsi.h"
#include "iscsi/conn.h"
#include "iscsi/tgt_node.h"
#include "iscsi/portal_grp.h"
#include "iscsi/init_grp.h"

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"

static void
rpc_iscsi_get_initiator_groups(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "iscsi_get_initiator_groups requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	iscsi_init_grps_info_json(w);
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("iscsi_get_initiator_groups", rpc_iscsi_get_initiator_groups,
		  SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_get_initiator_groups, get_initiator_groups)

struct rpc_initiator_list {
	size_t num_initiators;
	char *initiators[MAX_INITIATOR];
};

static int
decode_rpc_initiator_list(const struct spdk_json_val *val, void *out)
{
	struct rpc_initiator_list *list = out;

	return spdk_json_decode_array(val, spdk_json_decode_string, list->initiators, MAX_INITIATOR,
				      &list->num_initiators, sizeof(char *));
}

static void
free_rpc_initiator_list(struct rpc_initiator_list *list)
{
	size_t i;

	for (i = 0; i < list->num_initiators; i++) {
		free(list->initiators[i]);
	}
}

struct rpc_netmask_list {
	size_t num_netmasks;
	char *netmasks[MAX_NETMASK];
};

static int
decode_rpc_netmask_list(const struct spdk_json_val *val, void *out)
{
	struct rpc_netmask_list *list = out;

	return spdk_json_decode_array(val, spdk_json_decode_string, list->netmasks, MAX_NETMASK,
				      &list->num_netmasks, sizeof(char *));
}

static void
free_rpc_netmask_list(struct rpc_netmask_list *list)
{
	size_t i;

	for (i = 0; i < list->num_netmasks; i++) {
		free(list->netmasks[i]);
	}
}

struct rpc_initiator_group {
	int32_t tag;
	struct rpc_initiator_list initiator_list;
	struct rpc_netmask_list netmask_list;
};

static void
free_rpc_initiator_group(struct rpc_initiator_group *ig)
{
	free_rpc_initiator_list(&ig->initiator_list);
	free_rpc_netmask_list(&ig->netmask_list);
}

static const struct spdk_json_object_decoder rpc_initiator_group_decoders[] = {
	{"tag", offsetof(struct rpc_initiator_group, tag), spdk_json_decode_int32},
	{"initiators", offsetof(struct rpc_initiator_group, initiator_list), decode_rpc_initiator_list},
	{"netmasks", offsetof(struct rpc_initiator_group, netmask_list), decode_rpc_netmask_list},
};

static void
rpc_iscsi_create_initiator_group(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_initiator_group req = {};

	if (spdk_json_decode_object(params, rpc_initiator_group_decoders,
				    SPDK_COUNTOF(rpc_initiator_group_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.initiator_list.num_initiators == 0 ||
	    req.netmask_list.num_netmasks == 0) {
		goto invalid;
	}

	if (iscsi_init_grp_create_from_initiator_list(req.tag,
			req.initiator_list.num_initiators,
			req.initiator_list.initiators,
			req.netmask_list.num_netmasks,
			req.netmask_list.netmasks)) {
		SPDK_ERRLOG("create_from_initiator_list failed\n");
		goto invalid;
	}

	free_rpc_initiator_group(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_initiator_group(&req);
}
SPDK_RPC_REGISTER("iscsi_create_initiator_group", rpc_iscsi_create_initiator_group,
		  SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_create_initiator_group, add_initiator_group)

static const struct spdk_json_object_decoder rpc_add_or_delete_initiators_decoders[] = {
	{"tag", offsetof(struct rpc_initiator_group, tag), spdk_json_decode_int32},
	{"initiators", offsetof(struct rpc_initiator_group, initiator_list), decode_rpc_initiator_list, true},
	{"netmasks", offsetof(struct rpc_initiator_group, netmask_list), decode_rpc_netmask_list, true},
};

static void
rpc_iscsi_initiator_group_add_initiators(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	struct rpc_initiator_group req = {};

	if (spdk_json_decode_object(params, rpc_add_or_delete_initiators_decoders,
				    SPDK_COUNTOF(rpc_add_or_delete_initiators_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (iscsi_init_grp_add_initiators_from_initiator_list(req.tag,
			req.initiator_list.num_initiators,
			req.initiator_list.initiators,
			req.netmask_list.num_netmasks,
			req.netmask_list.netmasks)) {
		SPDK_ERRLOG("add_initiators_from_initiator_list failed\n");
		goto invalid;
	}

	free_rpc_initiator_group(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_initiator_group(&req);
}
SPDK_RPC_REGISTER("iscsi_initiator_group_add_initiators",
		  rpc_iscsi_initiator_group_add_initiators, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_initiator_group_add_initiators,
				   add_initiators_to_initiator_group)

static void
rpc_iscsi_initiator_group_remove_initiators(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	struct rpc_initiator_group req = {};

	if (spdk_json_decode_object(params, rpc_add_or_delete_initiators_decoders,
				    SPDK_COUNTOF(rpc_add_or_delete_initiators_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (iscsi_init_grp_delete_initiators_from_initiator_list(req.tag,
			req.initiator_list.num_initiators,
			req.initiator_list.initiators,
			req.netmask_list.num_netmasks,
			req.netmask_list.netmasks)) {
		SPDK_ERRLOG("delete_initiators_from_initiator_list failed\n");
		goto invalid;
	}

	free_rpc_initiator_group(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_initiator_group(&req);
}
SPDK_RPC_REGISTER("iscsi_initiator_group_remove_initiators",
		  rpc_iscsi_initiator_group_remove_initiators, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_initiator_group_remove_initiators,
				   delete_initiators_from_initiator_group)

struct rpc_iscsi_delete_initiator_group {
	int32_t tag;
};

static const struct spdk_json_object_decoder rpc_iscsi_delete_initiator_group_decoders[] = {
	{"tag", offsetof(struct rpc_iscsi_delete_initiator_group, tag), spdk_json_decode_int32},
};

static void
rpc_iscsi_delete_initiator_group(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_iscsi_delete_initiator_group req = {};
	struct spdk_iscsi_init_grp *ig;

	if (spdk_json_decode_object(params, rpc_iscsi_delete_initiator_group_decoders,
				    SPDK_COUNTOF(rpc_iscsi_delete_initiator_group_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	ig = iscsi_init_grp_unregister(req.tag);
	if (!ig) {
		goto invalid;
	}
	iscsi_tgt_node_delete_map(NULL, ig);
	iscsi_init_grp_destroy(ig);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
}
SPDK_RPC_REGISTER("iscsi_delete_initiator_group", rpc_iscsi_delete_initiator_group,
		  SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_delete_initiator_group, delete_initiator_group)

static void
rpc_iscsi_get_target_nodes(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "iscsi_get_target_nodes requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	iscsi_tgt_nodes_info_json(w);
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("iscsi_get_target_nodes", rpc_iscsi_get_target_nodes, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_get_target_nodes, get_target_nodes)

struct rpc_pg_ig_map {
	int32_t pg_tag;
	int32_t ig_tag;
};

static const struct spdk_json_object_decoder rpc_pg_ig_map_decoders[] = {
	{"pg_tag", offsetof(struct rpc_pg_ig_map, pg_tag), spdk_json_decode_int32},
	{"ig_tag", offsetof(struct rpc_pg_ig_map, ig_tag), spdk_json_decode_int32},
};

static int
decode_rpc_pg_ig_map(const struct spdk_json_val *val, void *out)
{
	struct rpc_pg_ig_map *pg_ig_map = out;

	return spdk_json_decode_object(val, rpc_pg_ig_map_decoders,
				       SPDK_COUNTOF(rpc_pg_ig_map_decoders),
				       pg_ig_map);
}

struct rpc_pg_ig_maps {
	size_t num_maps;
	struct rpc_pg_ig_map maps[MAX_TARGET_MAP];
};

static int
decode_rpc_pg_ig_maps(const struct spdk_json_val *val, void *out)
{
	struct rpc_pg_ig_maps *pg_ig_maps = out;

	return spdk_json_decode_array(val, decode_rpc_pg_ig_map, pg_ig_maps->maps,
				      MAX_TARGET_MAP, &pg_ig_maps->num_maps,
				      sizeof(struct rpc_pg_ig_map));
}

#define RPC_ISCSI_CREATE_TARGET_NODE_MAX_LUN	64

struct rpc_lun {
	char *bdev_name;
	int32_t lun_id;
};

static const struct spdk_json_object_decoder rpc_lun_decoders[] = {
	{"bdev_name", offsetof(struct rpc_lun, bdev_name), spdk_json_decode_string},
	{"lun_id", offsetof(struct rpc_lun, lun_id), spdk_json_decode_int32},
};

static int
decode_rpc_lun(const struct spdk_json_val *val, void *out)
{
	struct rpc_lun *lun = out;

	return spdk_json_decode_object(val, rpc_lun_decoders,
				       SPDK_COUNTOF(rpc_lun_decoders), lun);
}

struct rpc_luns {
	size_t num_luns;
	struct rpc_lun luns[RPC_ISCSI_CREATE_TARGET_NODE_MAX_LUN];
};

static int
decode_rpc_luns(const struct spdk_json_val *val, void *out)
{
	struct rpc_luns *luns = out;

	return spdk_json_decode_array(val, decode_rpc_lun, luns->luns,
				      RPC_ISCSI_CREATE_TARGET_NODE_MAX_LUN,
				      &luns->num_luns, sizeof(struct rpc_lun));
}

static void
free_rpc_luns(struct rpc_luns *p)
{
	size_t i;

	for (i = 0; i < p->num_luns; i++) {
		free(p->luns[i].bdev_name);
	}
}

struct rpc_target_node {
	char *name;
	char *alias_name;

	struct rpc_pg_ig_maps pg_ig_maps;
	struct rpc_luns luns;

	int32_t queue_depth;
	bool disable_chap;
	bool require_chap;
	bool mutual_chap;
	int32_t chap_group;

	bool header_digest;
	bool data_digest;
};

static void
free_rpc_target_node(struct rpc_target_node *req)
{
	free(req->name);
	free(req->alias_name);
	free_rpc_luns(&req->luns);
}

static const struct spdk_json_object_decoder rpc_target_node_decoders[] = {
	{"name", offsetof(struct rpc_target_node, name), spdk_json_decode_string},
	{"alias_name", offsetof(struct rpc_target_node, alias_name), spdk_json_decode_string},
	{"pg_ig_maps", offsetof(struct rpc_target_node, pg_ig_maps), decode_rpc_pg_ig_maps},
	{"luns", offsetof(struct rpc_target_node, luns), decode_rpc_luns},
	{"queue_depth", offsetof(struct rpc_target_node, queue_depth), spdk_json_decode_int32},
	{"disable_chap", offsetof(struct rpc_target_node, disable_chap), spdk_json_decode_bool, true},
	{"require_chap", offsetof(struct rpc_target_node, require_chap), spdk_json_decode_bool, true},
	{"mutual_chap", offsetof(struct rpc_target_node, mutual_chap), spdk_json_decode_bool, true},
	{"chap_group", offsetof(struct rpc_target_node, chap_group), spdk_json_decode_int32, true},
	{"header_digest", offsetof(struct rpc_target_node, header_digest), spdk_json_decode_bool, true},
	{"data_digest", offsetof(struct rpc_target_node, data_digest), spdk_json_decode_bool, true},
};

static void
rpc_iscsi_create_target_node(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_target_node req = {};
	struct spdk_iscsi_tgt_node *target;
	int32_t pg_tags[MAX_TARGET_MAP] = {0}, ig_tags[MAX_TARGET_MAP] = {0};
	char *bdev_names[RPC_ISCSI_CREATE_TARGET_NODE_MAX_LUN] = {0};
	int32_t lun_ids[RPC_ISCSI_CREATE_TARGET_NODE_MAX_LUN] = {0};
	size_t i;

	if (spdk_json_decode_object(params, rpc_target_node_decoders,
				    SPDK_COUNTOF(rpc_target_node_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	for (i = 0; i < req.pg_ig_maps.num_maps; i++) {
		pg_tags[i] = req.pg_ig_maps.maps[i].pg_tag;
		ig_tags[i] = req.pg_ig_maps.maps[i].ig_tag;
	}

	for (i = 0; i < req.luns.num_luns; i++) {
		bdev_names[i] = req.luns.luns[i].bdev_name;
		lun_ids[i] = req.luns.luns[i].lun_id;
	}

	/*
	 * Use default parameters in a few places:
	 *  index = -1 : automatically pick an index for the new target node
	 *  alias = NULL
	 */
	target = iscsi_tgt_node_construct(-1, req.name, req.alias_name,
					  pg_tags,
					  ig_tags,
					  req.pg_ig_maps.num_maps,
					  (const char **)bdev_names,
					  lun_ids,
					  req.luns.num_luns,
					  req.queue_depth,
					  req.disable_chap,
					  req.require_chap,
					  req.mutual_chap,
					  req.chap_group,
					  req.header_digest,
					  req.data_digest);

	if (target == NULL) {
		goto invalid;
	}

	free_rpc_target_node(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_target_node(&req);
}
SPDK_RPC_REGISTER("iscsi_create_target_node", rpc_iscsi_create_target_node, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_create_target_node, construct_target_node)

struct rpc_tgt_node_pg_ig_maps {
	char *name;
	struct rpc_pg_ig_maps pg_ig_maps;
};

static const struct spdk_json_object_decoder rpc_tgt_node_pg_ig_maps_decoders[] = {
	{"name", offsetof(struct rpc_tgt_node_pg_ig_maps, name), spdk_json_decode_string},
	{"pg_ig_maps", offsetof(struct rpc_tgt_node_pg_ig_maps, pg_ig_maps), decode_rpc_pg_ig_maps},
};

static void
rpc_iscsi_target_node_add_pg_ig_maps(struct spdk_jsonrpc_request *request,
				     const struct spdk_json_val *params)
{
	struct rpc_tgt_node_pg_ig_maps req = {};
	struct spdk_iscsi_tgt_node *target;
	int32_t pg_tags[MAX_TARGET_MAP] = {0}, ig_tags[MAX_TARGET_MAP] = {0};
	size_t i;
	int rc;

	if (spdk_json_decode_object(params, rpc_tgt_node_pg_ig_maps_decoders,
				    SPDK_COUNTOF(rpc_tgt_node_pg_ig_maps_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	target = iscsi_find_tgt_node(req.name);
	if (target == NULL) {
		SPDK_ERRLOG("target is not found\n");
		goto invalid;
	}

	for (i = 0; i < req.pg_ig_maps.num_maps; i++) {
		pg_tags[i] = req.pg_ig_maps.maps[i].pg_tag;
		ig_tags[i] = req.pg_ig_maps.maps[i].ig_tag;
	}

	rc = iscsi_target_node_add_pg_ig_maps(target, pg_tags, ig_tags,
					      req.pg_ig_maps.num_maps);
	if (rc < 0) {
		SPDK_ERRLOG("add pg-ig maps failed\n");
		goto invalid;
	}

	free(req.name);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 "Invalid parameters");
	free(req.name);
}
SPDK_RPC_REGISTER("iscsi_target_node_add_pg_ig_maps",
		  rpc_iscsi_target_node_add_pg_ig_maps, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_target_node_add_pg_ig_maps, add_pg_ig_maps)

static void
rpc_iscsi_target_node_remove_pg_ig_maps(struct spdk_jsonrpc_request *request,
					const struct spdk_json_val *params)
{
	struct rpc_tgt_node_pg_ig_maps req = {};
	struct spdk_iscsi_tgt_node *target;
	int32_t pg_tags[MAX_TARGET_MAP] = {0}, ig_tags[MAX_TARGET_MAP] = {0};
	size_t i;
	int rc;

	if (spdk_json_decode_object(params, rpc_tgt_node_pg_ig_maps_decoders,
				    SPDK_COUNTOF(rpc_tgt_node_pg_ig_maps_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	target = iscsi_find_tgt_node(req.name);
	if (target == NULL) {
		SPDK_ERRLOG("target is not found\n");
		goto invalid;
	}

	for (i = 0; i < req.pg_ig_maps.num_maps; i++) {
		pg_tags[i] = req.pg_ig_maps.maps[i].pg_tag;
		ig_tags[i] = req.pg_ig_maps.maps[i].ig_tag;
	}

	rc = iscsi_target_node_remove_pg_ig_maps(target, pg_tags, ig_tags,
			req.pg_ig_maps.num_maps);
	if (rc < 0) {
		SPDK_ERRLOG("remove pg-ig maps failed\n");
		goto invalid;
	}

	free(req.name);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 "Invalid parameters");
	free(req.name);
}
SPDK_RPC_REGISTER("iscsi_target_node_remove_pg_ig_maps",
		  rpc_iscsi_target_node_remove_pg_ig_maps, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_target_node_remove_pg_ig_maps,
				   delete_pg_ig_maps)

struct rpc_iscsi_delete_target_node {
	char *name;
};

static void
free_rpc_iscsi_delete_target_node(struct rpc_iscsi_delete_target_node *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_iscsi_delete_target_node_decoders[] = {
	{"name", offsetof(struct rpc_iscsi_delete_target_node, name), spdk_json_decode_string},
};

struct rpc_iscsi_delete_target_node_ctx {
	struct rpc_iscsi_delete_target_node req;
	struct spdk_jsonrpc_request *request;
};

static void
rpc_iscsi_delete_target_node_done(void *cb_arg, int rc)
{
	struct rpc_iscsi_delete_target_node_ctx *ctx = cb_arg;

	free_rpc_iscsi_delete_target_node(&ctx->req);
	spdk_jsonrpc_send_bool_response(ctx->request, rc == 0);
	free(ctx);
}

static void
rpc_iscsi_delete_target_node(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_iscsi_delete_target_node_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(ENOMEM));
		return;
	}

	if (spdk_json_decode_object(params, rpc_iscsi_delete_target_node_decoders,
				    SPDK_COUNTOF(rpc_iscsi_delete_target_node_decoders),
				    &ctx->req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (ctx->req.name == NULL) {
		SPDK_ERRLOG("missing name param\n");
		goto invalid;
	}

	ctx->request = request;

	iscsi_shutdown_tgt_node_by_name(ctx->req.name,
					rpc_iscsi_delete_target_node_done, ctx);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_iscsi_delete_target_node(&ctx->req);
	free(ctx);
}
SPDK_RPC_REGISTER("iscsi_delete_target_node", rpc_iscsi_delete_target_node, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_delete_target_node, delete_target_node)

static void
rpc_iscsi_get_portal_groups(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "iscsi_get_portal_groups requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	iscsi_portal_grps_info_json(w);
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("iscsi_get_portal_groups", rpc_iscsi_get_portal_groups, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_get_portal_groups, get_portal_groups)

struct rpc_portal {
	char *host;
	char *port;
};

struct rpc_portal_list {
	size_t num_portals;
	struct rpc_portal portals[MAX_PORTAL];
};

struct rpc_portal_group {
	int32_t tag;
	struct rpc_portal_list portal_list;
	bool is_private;
	bool wait;
};

static void
free_rpc_portal(struct rpc_portal *portal)
{
	free(portal->host);
	free(portal->port);
}

static void
free_rpc_portal_list(struct rpc_portal_list *pl)
{
	size_t i;

	for (i = 0; i < pl->num_portals; i++) {
		free_rpc_portal(&pl->portals[i]);
	}
	pl->num_portals = 0;
}

static void
free_rpc_portal_group(struct rpc_portal_group *pg)
{
	free_rpc_portal_list(&pg->portal_list);
}

static const struct spdk_json_object_decoder rpc_portal_decoders[] = {
	{"host", offsetof(struct rpc_portal, host), spdk_json_decode_string},
	{"port", offsetof(struct rpc_portal, port), spdk_json_decode_string},
};

static int
decode_rpc_portal(const struct spdk_json_val *val, void *out)
{
	struct rpc_portal *portal = out;

	return spdk_json_decode_object(val, rpc_portal_decoders,
				       SPDK_COUNTOF(rpc_portal_decoders),
				       portal);
}

static int
decode_rpc_portal_list(const struct spdk_json_val *val, void *out)
{
	struct rpc_portal_list *list = out;

	return spdk_json_decode_array(val, decode_rpc_portal, list->portals, MAX_PORTAL, &list->num_portals,
				      sizeof(struct rpc_portal));
}

static const struct spdk_json_object_decoder rpc_portal_group_decoders[] = {
	{"tag", offsetof(struct rpc_portal_group, tag), spdk_json_decode_int32},
	{"portals", offsetof(struct rpc_portal_group, portal_list), decode_rpc_portal_list},
	{"private", offsetof(struct rpc_portal_group, is_private), spdk_json_decode_bool, true},
	{"wait", offsetof(struct rpc_portal_group, wait), spdk_json_decode_bool, true},
};

static void
rpc_iscsi_create_portal_group(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_portal_group req = {};
	struct spdk_iscsi_portal_grp *pg = NULL;
	struct spdk_iscsi_portal *portal;
	size_t i = 0;
	int rc = -1;

	if (spdk_json_decode_object(params, rpc_portal_group_decoders,
				    SPDK_COUNTOF(rpc_portal_group_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto out;
	}

	pg = iscsi_portal_grp_create(req.tag, req.is_private);
	if (pg == NULL) {
		SPDK_ERRLOG("portal_grp_create failed\n");
		goto out;
	}
	for (i = 0; i < req.portal_list.num_portals; i++) {
		portal = iscsi_portal_create(req.portal_list.portals[i].host,
					     req.portal_list.portals[i].port);
		if (portal == NULL) {
			SPDK_ERRLOG("portal_create failed\n");
			goto out;
		}
		iscsi_portal_grp_add_portal(pg, portal);
	}

	rc = iscsi_portal_grp_open(pg, req.wait);
	if (rc != 0) {
		SPDK_ERRLOG("portal_grp_open failed\n");
		goto out;
	}

	rc = iscsi_portal_grp_register(pg);
	if (rc != 0) {
		SPDK_ERRLOG("portal_grp_register failed\n");
	}

out:
	if (rc == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");

		if (pg != NULL) {
			iscsi_portal_grp_release(pg);
		}
	}
	free_rpc_portal_group(&req);
}
SPDK_RPC_REGISTER("iscsi_create_portal_group", rpc_iscsi_create_portal_group, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_create_portal_group, add_portal_group)

struct rpc_iscsi_change_portal_group {
	int32_t tag;
};

static const struct spdk_json_object_decoder rpc_iscsi_change_portal_group_decoders[] = {
	{"tag", offsetof(struct rpc_iscsi_change_portal_group, tag), spdk_json_decode_int32},
};

typedef int (*iscsi_change_portal_grp_fn)(int pg_tag);

static void
_rpc_iscsi_change_portal_group(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params,
			       iscsi_change_portal_grp_fn fn)
{
	struct rpc_iscsi_change_portal_group req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_iscsi_change_portal_group_decoders,
				    SPDK_COUNTOF(rpc_iscsi_change_portal_group_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	rc = fn(req.tag);
	if (rc != 0) {
		goto invalid;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
}

static int
_rpc_iscsi_delete_portal_group(int pg_tag)
{
	struct spdk_iscsi_portal_grp *pg;

	pg = iscsi_portal_grp_unregister(pg_tag);
	if (!pg) {
		return -ENODEV;
	}

	iscsi_tgt_node_delete_map(pg, NULL);
	iscsi_portal_grp_release(pg);
	return 0;
}

static void
rpc_iscsi_delete_portal_group(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	_rpc_iscsi_change_portal_group(request, params, _rpc_iscsi_delete_portal_group);
}
SPDK_RPC_REGISTER("iscsi_delete_portal_group", rpc_iscsi_delete_portal_group, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_delete_portal_group, delete_portal_group)

static int
_rpc_iscsi_start_portal_group(int pg_tag)
{
	struct spdk_iscsi_portal_grp *pg;

	pg = iscsi_portal_grp_find_by_tag(pg_tag);
	if (!pg) {
		return -ENODEV;
	}

	iscsi_portal_grp_resume(pg);
	return 0;
}

static void
rpc_iscsi_start_portal_group(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	_rpc_iscsi_change_portal_group(request, params, _rpc_iscsi_start_portal_group);
}
SPDK_RPC_REGISTER("iscsi_start_portal_group", rpc_iscsi_start_portal_group, SPDK_RPC_RUNTIME)

struct rpc_portal_group_auth {
	int32_t tag;
	bool disable_chap;
	bool require_chap;
	bool mutual_chap;
	int32_t chap_group;
};

static const struct spdk_json_object_decoder rpc_portal_group_auth_decoders[] = {
	{"tag", offsetof(struct rpc_portal_group_auth, tag), spdk_json_decode_int32},
	{"disable_chap", offsetof(struct rpc_portal_group_auth, disable_chap), spdk_json_decode_bool, true},
	{"require_chap", offsetof(struct rpc_portal_group_auth, require_chap), spdk_json_decode_bool, true},
	{"mutual_chap", offsetof(struct rpc_portal_group_auth, mutual_chap), spdk_json_decode_bool, true},
	{"chap_group", offsetof(struct rpc_portal_group_auth, chap_group), spdk_json_decode_int32, true},
};

static void
rpc_iscsi_portal_group_set_auth(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_portal_group_auth req = {};
	struct spdk_iscsi_portal_grp *pg;
	int rc;

	if (spdk_json_decode_object(params, rpc_portal_group_auth_decoders,
				    SPDK_COUNTOF(rpc_portal_group_auth_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	pthread_mutex_lock(&g_iscsi.mutex);

	pg = iscsi_portal_grp_find_by_tag(req.tag);
	if (pg == NULL) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Could not find portal group %d", req.tag);
		goto exit;
	}

	rc = iscsi_portal_grp_set_chap_params(pg, req.disable_chap, req.require_chap,
					      req.mutual_chap, req.chap_group);
	if (rc < 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid combination of auth params");
		goto exit;
	}

	pthread_mutex_unlock(&g_iscsi.mutex);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

exit:
	pthread_mutex_unlock(&g_iscsi.mutex);
}
SPDK_RPC_REGISTER("iscsi_portal_group_set_auth", rpc_iscsi_portal_group_set_auth,
		  SPDK_RPC_RUNTIME)

struct rpc_iscsi_get_connections_ctx {
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;
};

static void
_rpc_iscsi_get_connections_done(struct spdk_io_channel_iter *i, int status)
{
	struct rpc_iscsi_get_connections_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	spdk_json_write_array_end(ctx->w);
	spdk_jsonrpc_end_result(ctx->request, ctx->w);

	free(ctx);
}

static void
_rpc_iscsi_get_connections(struct spdk_io_channel_iter *i)
{
	struct rpc_iscsi_get_connections_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_iscsi_poll_group *pg = spdk_io_channel_get_ctx(ch);
	struct spdk_iscsi_conn *conn;

	STAILQ_FOREACH(conn, &pg->connections, pg_link) {
		iscsi_conn_info_json(ctx->w, conn);
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
rpc_iscsi_get_connections(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_iscsi_get_connections_ctx *ctx;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "iscsi_get_connections requires no parameters");
		return;
	}

	ctx = calloc(1, sizeof(struct rpc_iscsi_get_connections_ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate rpc_get_iscsi_conns_ctx struct\n");
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	ctx->request = request;
	ctx->w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_array_begin(ctx->w);

	spdk_for_each_channel(&g_iscsi,
			      _rpc_iscsi_get_connections,
			      ctx,
			      _rpc_iscsi_get_connections_done);
}
SPDK_RPC_REGISTER("iscsi_get_connections", rpc_iscsi_get_connections, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_get_connections, get_iscsi_connections)

struct rpc_target_lun {
	char *name;
	char *bdev_name;
	int32_t lun_id;
};

static void
free_rpc_target_lun(struct rpc_target_lun *req)
{
	free(req->name);
	free(req->bdev_name);
}

static const struct spdk_json_object_decoder rpc_target_lun_decoders[] = {
	{"name", offsetof(struct rpc_target_lun, name), spdk_json_decode_string},
	{"bdev_name", offsetof(struct rpc_target_lun, bdev_name), spdk_json_decode_string},
	{"lun_id", offsetof(struct rpc_target_lun, lun_id), spdk_json_decode_int32, true},
};

static void
rpc_iscsi_target_node_add_lun(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_target_lun req = {};
	struct spdk_iscsi_tgt_node *target;
	int rc;

	req.lun_id = -1;

	if (spdk_json_decode_object(params, rpc_target_lun_decoders,
				    SPDK_COUNTOF(rpc_target_lun_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	target = iscsi_find_tgt_node(req.name);
	if (target == NULL) {
		SPDK_ERRLOG("target is not found\n");
		goto invalid;
	}

	rc = iscsi_tgt_node_add_lun(target, req.bdev_name, req.lun_id);
	if (rc < 0) {
		SPDK_ERRLOG("add lun failed\n");
		goto invalid;
	}

	free_rpc_target_lun(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 "Invalid parameters");
	free_rpc_target_lun(&req);
}
SPDK_RPC_REGISTER("iscsi_target_node_add_lun", rpc_iscsi_target_node_add_lun, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_target_node_add_lun, target_node_add_lun)

struct rpc_target_auth {
	char *name;
	bool disable_chap;
	bool require_chap;
	bool mutual_chap;
	int32_t chap_group;
};

static void
free_rpc_target_auth(struct rpc_target_auth *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_target_auth_decoders[] = {
	{"name", offsetof(struct rpc_target_auth, name), spdk_json_decode_string},
	{"disable_chap", offsetof(struct rpc_target_auth, disable_chap), spdk_json_decode_bool, true},
	{"require_chap", offsetof(struct rpc_target_auth, require_chap), spdk_json_decode_bool, true},
	{"mutual_chap", offsetof(struct rpc_target_auth, mutual_chap), spdk_json_decode_bool, true},
	{"chap_group", offsetof(struct rpc_target_auth, chap_group), spdk_json_decode_int32, true},
};

static void
rpc_iscsi_target_node_set_auth(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_target_auth req = {};
	struct spdk_iscsi_tgt_node *target;
	int rc;

	if (spdk_json_decode_object(params, rpc_target_auth_decoders,
				    SPDK_COUNTOF(rpc_target_auth_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto exit;
	}

	target = iscsi_find_tgt_node(req.name);
	if (target == NULL) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Could not find target %s", req.name);
		goto exit;
	}

	rc = iscsi_tgt_node_set_chap_params(target, req.disable_chap, req.require_chap,
					    req.mutual_chap, req.chap_group);
	if (rc < 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid combination of auth params");
		goto exit;
	}

	free_rpc_target_auth(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

exit:
	free_rpc_target_auth(&req);
}
SPDK_RPC_REGISTER("iscsi_target_node_set_auth", rpc_iscsi_target_node_set_auth,
		  SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_target_node_set_auth, set_iscsi_target_node_auth)

struct rpc_target_redirect {
	char *name;
	int32_t pg_tag;
	char *redirect_host;
	char *redirect_port;
};

static void
free_rpc_target_redirect(struct rpc_target_redirect *req)
{
	free(req->name);
	free(req->redirect_host);
	free(req->redirect_port);
}

static const struct spdk_json_object_decoder rpc_target_redirect_decoders[] = {
	{"name", offsetof(struct rpc_target_redirect, name), spdk_json_decode_string},
	{"pg_tag", offsetof(struct rpc_target_redirect, pg_tag), spdk_json_decode_int32},
	{"redirect_host", offsetof(struct rpc_target_redirect, redirect_host), spdk_json_decode_string, true},
	{"redirect_port", offsetof(struct rpc_target_redirect, redirect_port), spdk_json_decode_string, true},
};

static void
rpc_iscsi_target_node_set_redirect(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	struct rpc_target_redirect req = {};
	struct spdk_iscsi_tgt_node *target;
	int rc;

	if (spdk_json_decode_object(params, rpc_target_redirect_decoders,
				    SPDK_COUNTOF(rpc_target_redirect_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free_rpc_target_redirect(&req);
		return;
	}

	target = iscsi_find_tgt_node(req.name);
	if (target == NULL) {
		SPDK_ERRLOG("target %s is not found\n", req.name);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Target %s is not found", req.name);
		free_rpc_target_redirect(&req);
		return;
	}

	rc = iscsi_tgt_node_redirect(target, req.pg_tag, req.redirect_host, req.redirect_port);
	if (rc != 0) {
		SPDK_ERRLOG("failed to redirect target %s\n", req.name);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Failed to redirect target %s, (%d): %s",
						     req.name, rc, spdk_strerror(-rc));
		free_rpc_target_redirect(&req);
		return;
	}

	free_rpc_target_redirect(&req);

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("iscsi_target_node_set_redirect", rpc_iscsi_target_node_set_redirect,
		  SPDK_RPC_RUNTIME)

struct rpc_target_logout {
	char *name;
	int32_t pg_tag;
};

static void
free_rpc_target_logout(struct rpc_target_logout *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_target_logout_decoders[] = {
	{"name", offsetof(struct rpc_target_logout, name), spdk_json_decode_string},
	{"pg_tag", offsetof(struct rpc_target_logout, pg_tag), spdk_json_decode_int32, true},
};

static void
rpc_iscsi_target_node_request_logout(struct spdk_jsonrpc_request *request,
				     const struct spdk_json_val *params)
{
	struct rpc_target_logout req = {};
	struct spdk_iscsi_tgt_node *target;

	/* If pg_tag is omitted, request all connections to the specified target
	 * to logout.
	 */
	req.pg_tag = -1;

	if (spdk_json_decode_object(params, rpc_target_logout_decoders,
				    SPDK_COUNTOF(rpc_target_logout_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free_rpc_target_logout(&req);
		return;
	}

	target = iscsi_find_tgt_node(req.name);
	if (target == NULL) {
		SPDK_ERRLOG("target %s is not found\n", req.name);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Target %s is not found", req.name);
		free_rpc_target_logout(&req);
		return;
	}

	iscsi_conns_request_logout(target, req.pg_tag);

	free_rpc_target_logout(&req);

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("iscsi_target_node_request_logout", rpc_iscsi_target_node_request_logout,
		  SPDK_RPC_RUNTIME)

static void
rpc_iscsi_get_options(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "iscsi_get_options requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	iscsi_opts_info_json(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("iscsi_get_options", rpc_iscsi_get_options, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_get_options, get_iscsi_global_params)

struct rpc_discovery_auth {
	bool disable_chap;
	bool require_chap;
	bool mutual_chap;
	int32_t chap_group;
};

static const struct spdk_json_object_decoder rpc_discovery_auth_decoders[] = {
	{"disable_chap", offsetof(struct rpc_discovery_auth, disable_chap), spdk_json_decode_bool, true},
	{"require_chap", offsetof(struct rpc_discovery_auth, require_chap), spdk_json_decode_bool, true},
	{"mutual_chap", offsetof(struct rpc_discovery_auth, mutual_chap), spdk_json_decode_bool, true},
	{"chap_group", offsetof(struct rpc_discovery_auth, chap_group), spdk_json_decode_int32, true},
};

static void
rpc_iscsi_set_discovery_auth(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_discovery_auth req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_discovery_auth_decoders,
				    SPDK_COUNTOF(rpc_discovery_auth_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	rc = iscsi_set_discovery_auth(req.disable_chap, req.require_chap,
				      req.mutual_chap, req.chap_group);
	if (rc < 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid combination of CHAP params");
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("iscsi_set_discovery_auth", rpc_iscsi_set_discovery_auth, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_set_discovery_auth, set_iscsi_discovery_auth)

#define MAX_AUTH_SECRETS	64

struct rpc_auth_secret {
	char *user;
	char *secret;
	char *muser;
	char *msecret;
};

static void
free_rpc_auth_secret(struct rpc_auth_secret *_secret)
{
	free(_secret->user);
	free(_secret->secret);
	free(_secret->muser);
	free(_secret->msecret);
}

static const struct spdk_json_object_decoder rpc_auth_secret_decoders[] = {
	{"user", offsetof(struct rpc_auth_secret, user), spdk_json_decode_string},
	{"secret", offsetof(struct rpc_auth_secret, secret), spdk_json_decode_string},
	{"muser", offsetof(struct rpc_auth_secret, muser), spdk_json_decode_string, true},
	{"msecret", offsetof(struct rpc_auth_secret, msecret), spdk_json_decode_string, true},
};

static int
decode_rpc_auth_secret(const struct spdk_json_val *val, void *out)
{
	struct rpc_auth_secret *_secret = out;

	return spdk_json_decode_object(val, rpc_auth_secret_decoders,
				       SPDK_COUNTOF(rpc_auth_secret_decoders), _secret);
}

struct rpc_auth_secrets {
	size_t num_secret;
	struct rpc_auth_secret secrets[MAX_AUTH_SECRETS];
};

static void
free_rpc_auth_secrets(struct rpc_auth_secrets *secrets)
{
	size_t i;

	for (i = 0; i < secrets->num_secret; i++) {
		free_rpc_auth_secret(&secrets->secrets[i]);
	}
}

static int
decode_rpc_auth_secrets(const struct spdk_json_val *val, void *out)
{
	struct rpc_auth_secrets *secrets = out;

	return spdk_json_decode_array(val, decode_rpc_auth_secret, secrets->secrets,
				      MAX_AUTH_SECRETS, &secrets->num_secret,
				      sizeof(struct rpc_auth_secret));
}

struct rpc_auth_group {
	int32_t tag;
	struct rpc_auth_secrets secrets;
};

static void
free_rpc_auth_group(struct rpc_auth_group *group)
{
	free_rpc_auth_secrets(&group->secrets);
}

static const struct spdk_json_object_decoder rpc_auth_group_decoders[] = {
	{"tag", offsetof(struct rpc_auth_group, tag), spdk_json_decode_int32},
	{"secrets", offsetof(struct rpc_auth_group, secrets), decode_rpc_auth_secrets, true},
};

static void
rpc_iscsi_create_auth_group(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_auth_group req = {};
	struct rpc_auth_secret *_secret;
	struct spdk_iscsi_auth_group *group = NULL;
	int rc;
	size_t i;

	if (spdk_json_decode_object(params, rpc_auth_group_decoders,
				    SPDK_COUNTOF(rpc_auth_group_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free_rpc_auth_group(&req);
		return;
	}

	pthread_mutex_lock(&g_iscsi.mutex);

	rc = iscsi_add_auth_group(req.tag, &group);
	if (rc != 0) {
		pthread_mutex_unlock(&g_iscsi.mutex);

		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Could not add auth group (%d), %s",
						     req.tag, spdk_strerror(-rc));
		free_rpc_auth_group(&req);
		return;
	}

	for (i = 0; i < req.secrets.num_secret; i++) {
		_secret = &req.secrets.secrets[i];
		rc = iscsi_auth_group_add_secret(group, _secret->user, _secret->secret,
						 _secret->muser, _secret->msecret);
		if (rc != 0) {
			iscsi_delete_auth_group(group);
			pthread_mutex_unlock(&g_iscsi.mutex);

			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							     "Could not add secret to auth group (%d), %s",
							     req.tag, spdk_strerror(-rc));
			free_rpc_auth_group(&req);
			return;
		}
	}

	pthread_mutex_unlock(&g_iscsi.mutex);

	free_rpc_auth_group(&req);

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("iscsi_create_auth_group", rpc_iscsi_create_auth_group, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_create_auth_group, add_iscsi_auth_group)

struct rpc_delete_auth_group {
	int32_t tag;
};

static const struct spdk_json_object_decoder rpc_delete_auth_group_decoders[] = {
	{"tag", offsetof(struct rpc_delete_auth_group, tag), spdk_json_decode_int32},
};

static void
rpc_iscsi_delete_auth_group(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_delete_auth_group req = {};
	struct spdk_iscsi_auth_group *group;

	if (spdk_json_decode_object(params, rpc_delete_auth_group_decoders,
				    SPDK_COUNTOF(rpc_delete_auth_group_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	pthread_mutex_lock(&g_iscsi.mutex);

	group = iscsi_find_auth_group_by_tag(req.tag);
	if (group == NULL) {
		pthread_mutex_unlock(&g_iscsi.mutex);

		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Could not find auth group (%d)", req.tag);
		return;
	}

	iscsi_delete_auth_group(group);

	pthread_mutex_unlock(&g_iscsi.mutex);

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("iscsi_delete_auth_group", rpc_iscsi_delete_auth_group, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_delete_auth_group, delete_iscsi_auth_group)

struct rpc_add_auth_secret {
	int32_t tag;
	char *user;
	char *secret;
	char *muser;
	char *msecret;
};

static void
free_rpc_add_auth_secret(struct rpc_add_auth_secret *_secret)
{
	free(_secret->user);
	free(_secret->secret);
	free(_secret->muser);
	free(_secret->msecret);
}

static const struct spdk_json_object_decoder rpc_add_auth_secret_decoders[] = {
	{"tag", offsetof(struct rpc_add_auth_secret, tag), spdk_json_decode_int32},
	{"user", offsetof(struct rpc_add_auth_secret, user), spdk_json_decode_string},
	{"secret", offsetof(struct rpc_add_auth_secret, secret), spdk_json_decode_string},
	{"muser", offsetof(struct rpc_add_auth_secret, muser), spdk_json_decode_string, true},
	{"msecret", offsetof(struct rpc_add_auth_secret, msecret), spdk_json_decode_string, true},
};

static void
rpc_iscsi_auth_group_add_secret(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_add_auth_secret req = {};
	struct spdk_iscsi_auth_group *group;
	int rc;

	if (spdk_json_decode_object(params, rpc_add_auth_secret_decoders,
				    SPDK_COUNTOF(rpc_add_auth_secret_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free_rpc_add_auth_secret(&req);
		return;
	}

	pthread_mutex_lock(&g_iscsi.mutex);

	group = iscsi_find_auth_group_by_tag(req.tag);
	if (group == NULL) {
		pthread_mutex_unlock(&g_iscsi.mutex);

		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Could not find auth group (%d)", req.tag);
		free_rpc_add_auth_secret(&req);
		return;
	}

	rc = iscsi_auth_group_add_secret(group, req.user, req.secret, req.muser, req.msecret);
	if (rc != 0) {
		pthread_mutex_unlock(&g_iscsi.mutex);

		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Could not add secret to auth group (%d), %s",
						     req.tag, spdk_strerror(-rc));
		free_rpc_add_auth_secret(&req);
		return;
	}

	pthread_mutex_unlock(&g_iscsi.mutex);

	free_rpc_add_auth_secret(&req);

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("iscsi_auth_group_add_secret", rpc_iscsi_auth_group_add_secret,
		  SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_auth_group_add_secret, add_secret_to_iscsi_auth_group)


struct rpc_remove_auth_secret {
	int32_t tag;
	char *user;
};

static void
free_rpc_remove_auth_secret(struct rpc_remove_auth_secret *_secret)
{
	free(_secret->user);
}

static const struct spdk_json_object_decoder rpc_remove_auth_secret_decoders[] = {
	{"tag", offsetof(struct rpc_remove_auth_secret, tag), spdk_json_decode_int32},
	{"user", offsetof(struct rpc_remove_auth_secret, user), spdk_json_decode_string},
};

static void
rpc_iscsi_auth_group_remove_secret(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	struct rpc_remove_auth_secret req = {};
	struct spdk_iscsi_auth_group *group;
	int rc;

	if (spdk_json_decode_object(params, rpc_remove_auth_secret_decoders,
				    SPDK_COUNTOF(rpc_remove_auth_secret_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free_rpc_remove_auth_secret(&req);
		return;
	}

	pthread_mutex_lock(&g_iscsi.mutex);

	group = iscsi_find_auth_group_by_tag(req.tag);
	if (group == NULL) {
		pthread_mutex_unlock(&g_iscsi.mutex);

		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Could not find auth group (%d)", req.tag);
		free_rpc_remove_auth_secret(&req);
		return;
	}

	rc = iscsi_auth_group_delete_secret(group, req.user);
	if (rc != 0) {
		pthread_mutex_unlock(&g_iscsi.mutex);

		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Could not delete secret from CHAP group (%d), %s",
						     req.tag, spdk_strerror(-rc));
		free_rpc_remove_auth_secret(&req);
		return;
	}

	pthread_mutex_unlock(&g_iscsi.mutex);

	free_rpc_remove_auth_secret(&req);

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("iscsi_auth_group_remove_secret",
		  rpc_iscsi_auth_group_remove_secret, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_auth_group_remove_secret,
				   delete_secret_from_iscsi_auth_group)

static void
rpc_iscsi_get_auth_groups(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "iscsi_get_auth_groups requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	iscsi_auth_groups_info_json(w);
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("iscsi_get_auth_groups", rpc_iscsi_get_auth_groups, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_get_auth_groups, get_iscsi_auth_groups)

static const struct spdk_json_object_decoder rpc_set_iscsi_opts_decoders[] = {
	{"auth_file", offsetof(struct spdk_iscsi_opts, authfile), spdk_json_decode_string, true},
	{"node_base", offsetof(struct spdk_iscsi_opts, nodebase), spdk_json_decode_string, true},
	{"nop_timeout", offsetof(struct spdk_iscsi_opts, timeout), spdk_json_decode_int32, true},
	{"nop_in_interval", offsetof(struct spdk_iscsi_opts, nopininterval), spdk_json_decode_int32, true},
	{"no_discovery_auth", offsetof(struct spdk_iscsi_opts, disable_chap), spdk_json_decode_bool, true},
	{"req_discovery_auth", offsetof(struct spdk_iscsi_opts, require_chap), spdk_json_decode_bool, true},
	{"req_discovery_auth_mutual", offsetof(struct spdk_iscsi_opts, mutual_chap), spdk_json_decode_bool, true},
	{"discovery_auth_group", offsetof(struct spdk_iscsi_opts, chap_group), spdk_json_decode_int32, true},
	{"disable_chap", offsetof(struct spdk_iscsi_opts, disable_chap), spdk_json_decode_bool, true},
	{"require_chap", offsetof(struct spdk_iscsi_opts, require_chap), spdk_json_decode_bool, true},
	{"mutual_chap", offsetof(struct spdk_iscsi_opts, mutual_chap), spdk_json_decode_bool, true},
	{"chap_group", offsetof(struct spdk_iscsi_opts, chap_group), spdk_json_decode_int32, true},
	{"max_sessions", offsetof(struct spdk_iscsi_opts, MaxSessions), spdk_json_decode_uint32, true},
	{"max_queue_depth", offsetof(struct spdk_iscsi_opts, MaxQueueDepth), spdk_json_decode_uint32, true},
	{"max_connections_per_session", offsetof(struct spdk_iscsi_opts, MaxConnectionsPerSession), spdk_json_decode_uint32, true},
	{"default_time2wait", offsetof(struct spdk_iscsi_opts, DefaultTime2Wait), spdk_json_decode_uint32, true},
	{"default_time2retain", offsetof(struct spdk_iscsi_opts, DefaultTime2Retain), spdk_json_decode_uint32, true},
	{"first_burst_length", offsetof(struct spdk_iscsi_opts, FirstBurstLength), spdk_json_decode_uint32, true},
	{"immediate_data", offsetof(struct spdk_iscsi_opts, ImmediateData), spdk_json_decode_bool, true},
	{"error_recovery_level", offsetof(struct spdk_iscsi_opts, ErrorRecoveryLevel), spdk_json_decode_uint32, true},
	{"allow_duplicated_isid", offsetof(struct spdk_iscsi_opts, AllowDuplicateIsid), spdk_json_decode_bool, true},
	{"max_large_datain_per_connection", offsetof(struct spdk_iscsi_opts, MaxLargeDataInPerConnection), spdk_json_decode_uint32, true},
	{"max_r2t_per_connection", offsetof(struct spdk_iscsi_opts, MaxR2TPerConnection), spdk_json_decode_uint32, true},
};

static void
rpc_iscsi_set_options(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct spdk_iscsi_opts *opts;

	if (g_spdk_iscsi_opts != NULL) {
		SPDK_ERRLOG("this RPC must not be called more than once.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Must not call more than once");
		return;
	}

	opts = iscsi_opts_alloc();
	if (opts == NULL) {
		SPDK_ERRLOG("iscsi_opts_alloc() failed.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Out of memory");
		return;
	}

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_set_iscsi_opts_decoders,
					    SPDK_COUNTOF(rpc_set_iscsi_opts_decoders), opts)) {
			SPDK_ERRLOG("spdk_json_decode_object() failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			iscsi_opts_free(opts);
			return;
		}
	}

	g_spdk_iscsi_opts = iscsi_opts_copy(opts);
	iscsi_opts_free(opts);

	if (g_spdk_iscsi_opts == NULL) {
		SPDK_ERRLOG("iscsi_opts_copy() failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Out of memory");
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("iscsi_set_options", rpc_iscsi_set_options, SPDK_RPC_STARTUP)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iscsi_set_options, set_iscsi_options)
