/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
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
#include "spdk/base64.h"
#include "spdk/histogram_data.h"
#include "spdk_internal/rpc_autogen.h"

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

static const struct spdk_json_object_decoder rpc_iscsi_create_initiator_group_decoders[] = {
	{"tag", offsetof(struct rpc_iscsi_create_initiator_group_ctx, tag), spdk_json_decode_int32},
	{"initiators", offsetof(struct rpc_iscsi_create_initiator_group_ctx, initiators), rpc_decode_iscsi_initiators},
	{"netmasks", offsetof(struct rpc_iscsi_create_initiator_group_ctx, netmasks), rpc_decode_iscsi_netmasks},
};

static void
rpc_iscsi_create_initiator_group(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_iscsi_create_initiator_group_ctx req = {};

	if (spdk_json_decode_object(params, rpc_iscsi_create_initiator_group_decoders,
				    SPDK_COUNTOF(rpc_iscsi_create_initiator_group_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.initiators.count == 0 ||
	    req.netmasks.count == 0) {
		goto invalid;
	}

	if (iscsi_init_grp_create_from_initiator_list(req.tag,
			req.initiators.count, req.initiators.items,
			req.netmasks.count, req.netmasks.items)) {
		SPDK_ERRLOG("create_from_initiator_list failed\n");
		goto invalid;
	}

	free_rpc_iscsi_create_initiator_group(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_iscsi_create_initiator_group(&req);
}
SPDK_RPC_REGISTER("iscsi_create_initiator_group", rpc_iscsi_create_initiator_group,
		  SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_iscsi_initiator_group_add_initiators_decoders[] = {
	{"tag", offsetof(struct rpc_iscsi_initiator_group_add_initiators_ctx, tag), spdk_json_decode_int32},
	{"initiators", offsetof(struct rpc_iscsi_initiator_group_add_initiators_ctx, initiators), rpc_decode_iscsi_initiators, true},
	{"netmasks", offsetof(struct rpc_iscsi_initiator_group_add_initiators_ctx, netmasks), rpc_decode_iscsi_netmasks, true},
};

static void
rpc_iscsi_initiator_group_add_initiators(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	struct rpc_iscsi_initiator_group_add_initiators_ctx req = {};

	if (spdk_json_decode_object(params, rpc_iscsi_initiator_group_add_initiators_decoders,
				    SPDK_COUNTOF(rpc_iscsi_initiator_group_add_initiators_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (iscsi_init_grp_add_initiators_from_initiator_list(req.tag,
			req.initiators.count, req.initiators.items,
			req.netmasks.count, req.netmasks.items)) {
		SPDK_ERRLOG("add_initiators_from_initiator_list failed\n");
		goto invalid;
	}

	free_rpc_iscsi_initiator_group_add_initiators(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_iscsi_initiator_group_add_initiators(&req);
}
SPDK_RPC_REGISTER("iscsi_initiator_group_add_initiators",
		  rpc_iscsi_initiator_group_add_initiators, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_iscsi_initiator_group_remove_initiators_decoders[]
	= {
	{"tag", offsetof(struct rpc_iscsi_initiator_group_remove_initiators_ctx, tag), spdk_json_decode_int32},
	{"initiators", offsetof(struct rpc_iscsi_initiator_group_remove_initiators_ctx, initiators), rpc_decode_iscsi_initiators, true},
	{"netmasks", offsetof(struct rpc_iscsi_initiator_group_remove_initiators_ctx, netmasks), rpc_decode_iscsi_netmasks, true},
};

static void
rpc_iscsi_initiator_group_remove_initiators(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	struct rpc_iscsi_initiator_group_remove_initiators_ctx req = {};

	if (spdk_json_decode_object(params, rpc_iscsi_initiator_group_remove_initiators_decoders,
				    SPDK_COUNTOF(rpc_iscsi_initiator_group_remove_initiators_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (iscsi_init_grp_delete_initiators_from_initiator_list(req.tag,
			req.initiators.count, req.initiators.items,
			req.netmasks.count, req.netmasks.items)) {
		SPDK_ERRLOG("delete_initiators_from_initiator_list failed\n");
		goto invalid;
	}

	free_rpc_iscsi_initiator_group_remove_initiators(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_iscsi_initiator_group_remove_initiators(&req);
}
SPDK_RPC_REGISTER("iscsi_initiator_group_remove_initiators",
		  rpc_iscsi_initiator_group_remove_initiators, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_iscsi_delete_initiator_group_decoders[] = {
	{"tag", offsetof(struct rpc_iscsi_delete_initiator_group_ctx, tag), spdk_json_decode_int32},
};

static void
rpc_iscsi_delete_initiator_group(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_iscsi_delete_initiator_group_ctx req = {};
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

static const struct spdk_json_object_decoder rpc_iscsi_pg_ig_map_decoders[] = {
	{"pg_tag", offsetof(struct rpc_iscsi_pg_ig_map, pg_tag), spdk_json_decode_int32},
	{"ig_tag", offsetof(struct rpc_iscsi_pg_ig_map, ig_tag), spdk_json_decode_int32},
};

static int
decode_rpc_iscsi_pg_ig_map(const struct spdk_json_val *val, void *out)
{
	struct rpc_iscsi_pg_ig_map *pg_ig_map = out;

	return spdk_json_decode_object(val, rpc_iscsi_pg_ig_map_decoders,
				       SPDK_COUNTOF(rpc_iscsi_pg_ig_map_decoders),
				       pg_ig_map);
}

static int
decode_rpc_iscsi_pg_ig_maps(const struct spdk_json_val *val, void *out)
{
	struct rpc_iscsi_pg_ig_maps *pg_ig_maps = out;

	return spdk_json_decode_array(val, decode_rpc_iscsi_pg_ig_map, pg_ig_maps->items,
				      RPC_ISCSI_PG_IG_MAPS_MAX, &pg_ig_maps->count,
				      sizeof(struct rpc_iscsi_pg_ig_map));
}

static const struct spdk_json_object_decoder rpc_iscsi_lun_decoders[] = {
	{"bdev_name", offsetof(struct rpc_iscsi_lun, bdev_name), spdk_json_decode_string},
	{"lun_id", offsetof(struct rpc_iscsi_lun, lun_id), spdk_json_decode_int32},
};

static int
decode_rpc_iscsi_lun(const struct spdk_json_val *val, void *out)
{
	struct rpc_iscsi_lun *lun = out;

	return spdk_json_decode_object(val, rpc_iscsi_lun_decoders,
				       SPDK_COUNTOF(rpc_iscsi_lun_decoders), lun);
}

static int
decode_rpc_iscsi_luns(const struct spdk_json_val *val, void *out)
{
	struct rpc_iscsi_luns *luns = out;

	return spdk_json_decode_array(val, decode_rpc_iscsi_lun, luns->items,
				      RPC_ISCSI_LUNS_MAX,
				      &luns->count, sizeof(struct rpc_iscsi_lun));
}

static const struct spdk_json_object_decoder rpc_iscsi_create_target_node_decoders[] = {
	{"name", offsetof(struct rpc_iscsi_create_target_node_ctx, name), spdk_json_decode_string},
	{"alias_name", offsetof(struct rpc_iscsi_create_target_node_ctx, alias_name), spdk_json_decode_string},
	{"pg_ig_maps", offsetof(struct rpc_iscsi_create_target_node_ctx, pg_ig_maps), decode_rpc_iscsi_pg_ig_maps},
	{"luns", offsetof(struct rpc_iscsi_create_target_node_ctx, luns), decode_rpc_iscsi_luns},
	{"queue_depth", offsetof(struct rpc_iscsi_create_target_node_ctx, queue_depth), spdk_json_decode_int32},
	{"disable_chap", offsetof(struct rpc_iscsi_create_target_node_ctx, disable_chap), spdk_json_decode_bool, true},
	{"require_chap", offsetof(struct rpc_iscsi_create_target_node_ctx, require_chap), spdk_json_decode_bool, true},
	{"mutual_chap", offsetof(struct rpc_iscsi_create_target_node_ctx, mutual_chap), spdk_json_decode_bool, true},
	{"chap_group", offsetof(struct rpc_iscsi_create_target_node_ctx, chap_group), spdk_json_decode_int32, true},
	{"header_digest", offsetof(struct rpc_iscsi_create_target_node_ctx, header_digest), spdk_json_decode_bool, true},
	{"data_digest", offsetof(struct rpc_iscsi_create_target_node_ctx, data_digest), spdk_json_decode_bool, true},
};

static void
rpc_iscsi_create_target_node(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_iscsi_create_target_node_ctx req = {};
	struct spdk_iscsi_tgt_node *target;
	int32_t pg_tags[RPC_ISCSI_PG_IG_MAPS_MAX] = {}, ig_tags[RPC_ISCSI_PG_IG_MAPS_MAX] = {};
	char *bdev_names[RPC_ISCSI_LUNS_MAX] = {};
	int32_t lun_ids[RPC_ISCSI_LUNS_MAX] = {};
	size_t i;

	if (spdk_json_decode_object(params, rpc_iscsi_create_target_node_decoders,
				    SPDK_COUNTOF(rpc_iscsi_create_target_node_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	for (i = 0; i < req.pg_ig_maps.count; i++) {
		pg_tags[i] = req.pg_ig_maps.items[i].pg_tag;
		ig_tags[i] = req.pg_ig_maps.items[i].ig_tag;
	}

	for (i = 0; i < req.luns.count; i++) {
		bdev_names[i] = req.luns.items[i].bdev_name;
		lun_ids[i] = req.luns.items[i].lun_id;
	}

	/*
	 * Use default parameters in a few places:
	 *  index = -1 : automatically pick an index for the new target node
	 *  alias = NULL
	 */
	target = iscsi_tgt_node_construct(-1, req.name, req.alias_name,
					  pg_tags,
					  ig_tags,
					  req.pg_ig_maps.count,
					  (const char **)bdev_names,
					  lun_ids,
					  req.luns.count,
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

	free_rpc_iscsi_create_target_node(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_iscsi_create_target_node(&req);
}
SPDK_RPC_REGISTER("iscsi_create_target_node", rpc_iscsi_create_target_node, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_iscsi_target_node_add_pg_ig_maps_decoders[] = {
	{"name", offsetof(struct rpc_iscsi_target_node_add_pg_ig_maps_ctx, name), spdk_json_decode_string},
	{"pg_ig_maps", offsetof(struct rpc_iscsi_target_node_add_pg_ig_maps_ctx, pg_ig_maps), decode_rpc_iscsi_pg_ig_maps},
};

static void
rpc_iscsi_target_node_add_pg_ig_maps(struct spdk_jsonrpc_request *request,
				     const struct spdk_json_val *params)
{
	struct rpc_iscsi_target_node_add_pg_ig_maps_ctx req = {};
	struct spdk_iscsi_tgt_node *target;
	int32_t pg_tags[RPC_ISCSI_PG_IG_MAPS_MAX] = {}, ig_tags[RPC_ISCSI_PG_IG_MAPS_MAX] = {};
	size_t i;
	int rc;

	if (spdk_json_decode_object(params, rpc_iscsi_target_node_add_pg_ig_maps_decoders,
				    SPDK_COUNTOF(rpc_iscsi_target_node_add_pg_ig_maps_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	target = iscsi_find_tgt_node(req.name);
	if (target == NULL) {
		SPDK_ERRLOG("target is not found\n");
		goto invalid;
	}

	for (i = 0; i < req.pg_ig_maps.count; i++) {
		pg_tags[i] = req.pg_ig_maps.items[i].pg_tag;
		ig_tags[i] = req.pg_ig_maps.items[i].ig_tag;
	}

	rc = iscsi_target_node_add_pg_ig_maps(target, pg_tags, ig_tags,
					      req.pg_ig_maps.count);
	if (rc < 0) {
		SPDK_ERRLOG("add pg-ig maps failed\n");
		goto invalid;
	}

	free_rpc_iscsi_target_node_add_pg_ig_maps(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 "Invalid parameters");
	free_rpc_iscsi_target_node_add_pg_ig_maps(&req);
}
SPDK_RPC_REGISTER("iscsi_target_node_add_pg_ig_maps",
		  rpc_iscsi_target_node_add_pg_ig_maps, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_iscsi_target_node_remove_pg_ig_maps_decoders[] = {
	{"name", offsetof(struct rpc_iscsi_target_node_remove_pg_ig_maps_ctx, name), spdk_json_decode_string},
	{"pg_ig_maps", offsetof(struct rpc_iscsi_target_node_remove_pg_ig_maps_ctx, pg_ig_maps), decode_rpc_iscsi_pg_ig_maps},
};

static void
rpc_iscsi_target_node_remove_pg_ig_maps(struct spdk_jsonrpc_request *request,
					const struct spdk_json_val *params)
{
	struct rpc_iscsi_target_node_remove_pg_ig_maps_ctx req = {};
	struct spdk_iscsi_tgt_node *target;
	int32_t pg_tags[RPC_ISCSI_PG_IG_MAPS_MAX] = {}, ig_tags[RPC_ISCSI_PG_IG_MAPS_MAX] = {};
	size_t i;
	int rc;

	if (spdk_json_decode_object(params, rpc_iscsi_target_node_remove_pg_ig_maps_decoders,
				    SPDK_COUNTOF(rpc_iscsi_target_node_remove_pg_ig_maps_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	target = iscsi_find_tgt_node(req.name);
	if (target == NULL) {
		SPDK_ERRLOG("target is not found\n");
		goto invalid;
	}

	for (i = 0; i < req.pg_ig_maps.count; i++) {
		pg_tags[i] = req.pg_ig_maps.items[i].pg_tag;
		ig_tags[i] = req.pg_ig_maps.items[i].ig_tag;
	}

	rc = iscsi_target_node_remove_pg_ig_maps(target, pg_tags, ig_tags,
			req.pg_ig_maps.count);
	if (rc < 0) {
		SPDK_ERRLOG("remove pg-ig maps failed\n");
		goto invalid;
	}

	free_rpc_iscsi_target_node_remove_pg_ig_maps(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 "Invalid parameters");
	free_rpc_iscsi_target_node_remove_pg_ig_maps(&req);
}
SPDK_RPC_REGISTER("iscsi_target_node_remove_pg_ig_maps",
		  rpc_iscsi_target_node_remove_pg_ig_maps, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_iscsi_delete_target_node_decoders[] = {
	{"name", offsetof(struct rpc_iscsi_delete_target_node_ctx, name), spdk_json_decode_string},
};

static void
rpc_iscsi_delete_target_node_done(void *cb_arg, int rc)
{
	struct rpc_iscsi_delete_target_node_ctx *ctx = cb_arg;

	if (rc == 0) {
		spdk_jsonrpc_send_bool_response(ctx->request, true);
	} else {
		spdk_jsonrpc_send_error_response(ctx->request, rc, spdk_strerror(-rc));
	}
	free_rpc_iscsi_delete_target_node(ctx);
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
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (ctx->name == NULL) {
		SPDK_ERRLOG("missing name param\n");
		goto invalid;
	}

	ctx->request = request;

	iscsi_shutdown_tgt_node_by_name(ctx->name,
					rpc_iscsi_delete_target_node_done, ctx);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_iscsi_delete_target_node(ctx);
	free(ctx);
}
SPDK_RPC_REGISTER("iscsi_delete_target_node", rpc_iscsi_delete_target_node, SPDK_RPC_RUNTIME)

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

static const struct spdk_json_object_decoder rpc_iscsi_portal_decoders[] = {
	{"host", offsetof(struct rpc_iscsi_portal, host), spdk_json_decode_string},
	{"port", offsetof(struct rpc_iscsi_portal, port), spdk_json_decode_string},
};

static int
decode_rpc_iscsi_portal(const struct spdk_json_val *val, void *out)
{
	struct rpc_iscsi_portal *portal = out;

	return spdk_json_decode_object(val, rpc_iscsi_portal_decoders,
				       SPDK_COUNTOF(rpc_iscsi_portal_decoders),
				       portal);
}

static int
decode_rpc_iscsi_portals(const struct spdk_json_val *val, void *out)
{
	struct rpc_iscsi_portals *list = out;

	return spdk_json_decode_array(val, decode_rpc_iscsi_portal, list->items, RPC_ISCSI_PORTALS_MAX,
				      &list->count, sizeof(struct rpc_iscsi_portal));
}

static const struct spdk_json_object_decoder rpc_iscsi_create_portal_group_decoders[] = {
	{"tag", offsetof(struct rpc_iscsi_create_portal_group_ctx, tag), spdk_json_decode_int32},
	{"portals", offsetof(struct rpc_iscsi_create_portal_group_ctx, portals), decode_rpc_iscsi_portals},
	{"private", offsetof(struct rpc_iscsi_create_portal_group_ctx, is_private), spdk_json_decode_bool, true},
	{"wait", offsetof(struct rpc_iscsi_create_portal_group_ctx, wait), spdk_json_decode_bool, true},
};

static void
rpc_iscsi_create_portal_group(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_iscsi_create_portal_group_ctx req = {};
	struct spdk_iscsi_portal_grp *pg = NULL;
	struct spdk_iscsi_portal *portal;
	size_t i = 0;
	int rc = -1;

	if (spdk_json_decode_object(params, rpc_iscsi_create_portal_group_decoders,
				    SPDK_COUNTOF(rpc_iscsi_create_portal_group_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto out;
	}

	pg = iscsi_portal_grp_create(req.tag, req.is_private);
	if (pg == NULL) {
		SPDK_ERRLOG("portal_grp_create failed\n");
		goto out;
	}
	for (i = 0; i < req.portals.count; i++) {
		portal = iscsi_portal_create(req.portals.items[i].host,
					     req.portals.items[i].port);
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
	free_rpc_iscsi_create_portal_group(&req);
}
SPDK_RPC_REGISTER("iscsi_create_portal_group", rpc_iscsi_create_portal_group, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_iscsi_delete_portal_group_decoders[] = {
	{"tag", offsetof(struct rpc_iscsi_delete_portal_group_ctx, tag), spdk_json_decode_int32},
};

static void
rpc_iscsi_delete_portal_group(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_iscsi_delete_portal_group_ctx req = {};
	struct spdk_iscsi_portal_grp *pg;

	if (spdk_json_decode_object(params, rpc_iscsi_delete_portal_group_decoders,
				    SPDK_COUNTOF(rpc_iscsi_delete_portal_group_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	pg = iscsi_portal_grp_unregister(req.tag);
	if (!pg) {
		goto invalid;
	}

	iscsi_tgt_node_delete_map(pg, NULL);
	iscsi_portal_grp_release(pg);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
}
SPDK_RPC_REGISTER("iscsi_delete_portal_group", rpc_iscsi_delete_portal_group, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_iscsi_start_portal_group_decoders[] = {
	{"tag", offsetof(struct rpc_iscsi_start_portal_group_ctx, tag), spdk_json_decode_int32},
};

static void
rpc_iscsi_start_portal_group(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_iscsi_start_portal_group_ctx req = {};
	struct spdk_iscsi_portal_grp *pg;

	if (spdk_json_decode_object(params, rpc_iscsi_start_portal_group_decoders,
				    SPDK_COUNTOF(rpc_iscsi_start_portal_group_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	pg = iscsi_portal_grp_find_by_tag(req.tag);
	if (!pg) {
		goto invalid;
	}

	spdk_poller_resume(pg->acceptor_poller);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
}
SPDK_RPC_REGISTER("iscsi_start_portal_group", rpc_iscsi_start_portal_group, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_iscsi_portal_group_set_auth_decoders[] = {
	{"tag", offsetof(struct rpc_iscsi_portal_group_set_auth_ctx, tag), spdk_json_decode_int32},
	{"disable_chap", offsetof(struct rpc_iscsi_portal_group_set_auth_ctx, disable_chap), spdk_json_decode_bool, true},
	{"require_chap", offsetof(struct rpc_iscsi_portal_group_set_auth_ctx, require_chap), spdk_json_decode_bool, true},
	{"mutual_chap", offsetof(struct rpc_iscsi_portal_group_set_auth_ctx, mutual_chap), spdk_json_decode_bool, true},
	{"chap_group", offsetof(struct rpc_iscsi_portal_group_set_auth_ctx, chap_group), spdk_json_decode_int32, true},
};

static void
rpc_iscsi_portal_group_set_auth(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_iscsi_portal_group_set_auth_ctx req = {};
	struct spdk_iscsi_portal_grp *pg;
	int rc;

	if (spdk_json_decode_object(params, rpc_iscsi_portal_group_set_auth_decoders,
				    SPDK_COUNTOF(rpc_iscsi_portal_group_set_auth_decoders), &req)) {
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

struct rpc_iscsi_get_stats_ctx {
	struct spdk_jsonrpc_request *request;
	uint32_t invalid;
	uint32_t running;
	uint32_t exiting;
	uint32_t exited;
};

static void
_rpc_iscsi_get_stats_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_json_write_ctx *w;
	struct rpc_iscsi_get_stats_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	w = spdk_jsonrpc_begin_result(ctx->request);
	spdk_json_write_object_begin(w);

	spdk_json_write_named_uint32(w, "invalid", ctx->invalid);
	spdk_json_write_named_uint32(w, "running", ctx->running);
	spdk_json_write_named_uint32(w, "exiting", ctx->exiting);
	spdk_json_write_named_uint32(w, "exited", ctx->exited);

	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(ctx->request, w);

	free(ctx);
}

static void
_iscsi_get_stats(struct rpc_iscsi_get_stats_ctx *ctx,
		 struct spdk_iscsi_conn *conn)
{
	switch (conn->state) {
	case ISCSI_CONN_STATE_INVALID:
		ctx->invalid += 1;
		break;
	case ISCSI_CONN_STATE_RUNNING:
		ctx->running += 1;
		break;
	case ISCSI_CONN_STATE_EXITING:
		ctx->exiting += 1;
		break;
	case ISCSI_CONN_STATE_EXITED:
		ctx->exited += 1;
		break;
	}
}

static void
_rpc_iscsi_get_stats(struct spdk_io_channel_iter *i)
{
	struct rpc_iscsi_get_stats_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_iscsi_poll_group *pg = spdk_io_channel_get_ctx(ch);
	struct spdk_iscsi_conn *conn;

	STAILQ_FOREACH(conn, &pg->connections, pg_link) {
		_iscsi_get_stats(ctx, conn);
	}

	spdk_for_each_channel_continue(i, 0);
}



static void
rpc_iscsi_get_stats(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_iscsi_get_stats_ctx *ctx;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "iscsi_get_stats requires no parameters");
		return;
	}

	ctx = calloc(1, sizeof(struct rpc_iscsi_get_stats_ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate rpc_iscsi_get_stats_ctx struct\n");
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	ctx->request = request;

	spdk_for_each_channel(&g_iscsi,
			      _rpc_iscsi_get_stats,
			      ctx,
			      _rpc_iscsi_get_stats_done);

}
SPDK_RPC_REGISTER("iscsi_get_stats", rpc_iscsi_get_stats, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_iscsi_target_node_add_lun_decoders[] = {
	{"name", offsetof(struct rpc_iscsi_target_node_add_lun_ctx, name), spdk_json_decode_string},
	{"bdev_name", offsetof(struct rpc_iscsi_target_node_add_lun_ctx, bdev_name), spdk_json_decode_string},
	{"lun_id", offsetof(struct rpc_iscsi_target_node_add_lun_ctx, lun_id), spdk_json_decode_int32, true},
};

static void
rpc_iscsi_target_node_add_lun(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_iscsi_target_node_add_lun_ctx req = {};
	struct spdk_iscsi_tgt_node *target;
	int rc;

	req.lun_id = -1;

	if (spdk_json_decode_object(params, rpc_iscsi_target_node_add_lun_decoders,
				    SPDK_COUNTOF(rpc_iscsi_target_node_add_lun_decoders), &req)) {
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

	free_rpc_iscsi_target_node_add_lun(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 "Invalid parameters");
	free_rpc_iscsi_target_node_add_lun(&req);
}
SPDK_RPC_REGISTER("iscsi_target_node_add_lun", rpc_iscsi_target_node_add_lun, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_iscsi_target_node_set_auth_decoders[] = {
	{"name", offsetof(struct rpc_iscsi_target_node_set_auth_ctx, name), spdk_json_decode_string},
	{"disable_chap", offsetof(struct rpc_iscsi_target_node_set_auth_ctx, disable_chap), spdk_json_decode_bool, true},
	{"require_chap", offsetof(struct rpc_iscsi_target_node_set_auth_ctx, require_chap), spdk_json_decode_bool, true},
	{"mutual_chap", offsetof(struct rpc_iscsi_target_node_set_auth_ctx, mutual_chap), spdk_json_decode_bool, true},
	{"chap_group", offsetof(struct rpc_iscsi_target_node_set_auth_ctx, chap_group), spdk_json_decode_int32, true},
};

static void
rpc_iscsi_target_node_set_auth(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_iscsi_target_node_set_auth_ctx req = {};
	struct spdk_iscsi_tgt_node *target;
	int rc;

	if (spdk_json_decode_object(params, rpc_iscsi_target_node_set_auth_decoders,
				    SPDK_COUNTOF(rpc_iscsi_target_node_set_auth_decoders), &req)) {
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

	free_rpc_iscsi_target_node_set_auth(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

exit:
	free_rpc_iscsi_target_node_set_auth(&req);
}
SPDK_RPC_REGISTER("iscsi_target_node_set_auth", rpc_iscsi_target_node_set_auth,
		  SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_iscsi_target_node_set_redirect_decoders[] = {
	{"name", offsetof(struct rpc_iscsi_target_node_set_redirect_ctx, name), spdk_json_decode_string},
	{"pg_tag", offsetof(struct rpc_iscsi_target_node_set_redirect_ctx, pg_tag), spdk_json_decode_int32},
	{"redirect_host", offsetof(struct rpc_iscsi_target_node_set_redirect_ctx, redirect_host), spdk_json_decode_string, true},
	{"redirect_port", offsetof(struct rpc_iscsi_target_node_set_redirect_ctx, redirect_port), spdk_json_decode_string, true},
};

static void
rpc_iscsi_target_node_set_redirect(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	struct rpc_iscsi_target_node_set_redirect_ctx req = {};
	struct spdk_iscsi_tgt_node *target;
	int rc;

	if (spdk_json_decode_object(params, rpc_iscsi_target_node_set_redirect_decoders,
				    SPDK_COUNTOF(rpc_iscsi_target_node_set_redirect_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free_rpc_iscsi_target_node_set_redirect(&req);
		return;
	}

	target = iscsi_find_tgt_node(req.name);
	if (target == NULL) {
		SPDK_ERRLOG("target %s is not found\n", req.name);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Target %s is not found", req.name);
		free_rpc_iscsi_target_node_set_redirect(&req);
		return;
	}

	rc = iscsi_tgt_node_redirect(target, req.pg_tag, req.redirect_host, req.redirect_port);
	if (rc != 0) {
		SPDK_ERRLOG("failed to redirect target %s\n", req.name);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Failed to redirect target %s, (%d): %s",
						     req.name, rc, spdk_strerror(-rc));
		free_rpc_iscsi_target_node_set_redirect(&req);
		return;
	}

	free_rpc_iscsi_target_node_set_redirect(&req);

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("iscsi_target_node_set_redirect", rpc_iscsi_target_node_set_redirect,
		  SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_iscsi_target_node_request_logout_decoders[] = {
	{"name", offsetof(struct rpc_iscsi_target_node_request_logout_ctx, name), spdk_json_decode_string},
	{"pg_tag", offsetof(struct rpc_iscsi_target_node_request_logout_ctx, pg_tag), spdk_json_decode_int32, true},
};

static void
rpc_iscsi_target_node_request_logout(struct spdk_jsonrpc_request *request,
				     const struct spdk_json_val *params)
{
	struct rpc_iscsi_target_node_request_logout_ctx req = {};
	struct spdk_iscsi_tgt_node *target;

	/* If pg_tag is omitted, request all connections to the specified target
	 * to logout.
	 */
	req.pg_tag = -1;

	if (spdk_json_decode_object(params, rpc_iscsi_target_node_request_logout_decoders,
				    SPDK_COUNTOF(rpc_iscsi_target_node_request_logout_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free_rpc_iscsi_target_node_request_logout(&req);
		return;
	}

	target = iscsi_find_tgt_node(req.name);
	if (target == NULL) {
		SPDK_ERRLOG("target %s is not found\n", req.name);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Target %s is not found", req.name);
		free_rpc_iscsi_target_node_request_logout(&req);
		return;
	}

	iscsi_conns_request_logout(target, req.pg_tag);

	free_rpc_iscsi_target_node_request_logout(&req);

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

static const struct spdk_json_object_decoder rpc_iscsi_set_discovery_auth_decoders[] = {
	{"disable_chap", offsetof(struct rpc_iscsi_set_discovery_auth_ctx, disable_chap), spdk_json_decode_bool, true},
	{"require_chap", offsetof(struct rpc_iscsi_set_discovery_auth_ctx, require_chap), spdk_json_decode_bool, true},
	{"mutual_chap", offsetof(struct rpc_iscsi_set_discovery_auth_ctx, mutual_chap), spdk_json_decode_bool, true},
	{"chap_group", offsetof(struct rpc_iscsi_set_discovery_auth_ctx, chap_group), spdk_json_decode_int32, true},
};

static void
rpc_iscsi_set_discovery_auth(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_iscsi_set_discovery_auth_ctx req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_iscsi_set_discovery_auth_decoders,
				    SPDK_COUNTOF(rpc_iscsi_set_discovery_auth_decoders), &req)) {
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

static const struct spdk_json_object_decoder rpc_iscsi_auth_secret_decoders[] = {
	{"user", offsetof(struct rpc_iscsi_auth_secret, user), spdk_json_decode_string},
	{"secret", offsetof(struct rpc_iscsi_auth_secret, secret), spdk_json_decode_string},
	{"muser", offsetof(struct rpc_iscsi_auth_secret, muser), spdk_json_decode_string, true},
	{"msecret", offsetof(struct rpc_iscsi_auth_secret, msecret), spdk_json_decode_string, true},
};

static int
decode_rpc_iscsi_auth_secret(const struct spdk_json_val *val, void *out)
{
	struct rpc_iscsi_auth_secret *_secret = out;

	return spdk_json_decode_object(val, rpc_iscsi_auth_secret_decoders,
				       SPDK_COUNTOF(rpc_iscsi_auth_secret_decoders), _secret);
}

static int
decode_rpc_iscsi_auth_secrets(const struct spdk_json_val *val, void *out)
{
	struct rpc_iscsi_auth_secrets *secrets = out;

	return spdk_json_decode_array(val, decode_rpc_iscsi_auth_secret, secrets->items,
				      RPC_ISCSI_AUTH_SECRETS_MAX, &secrets->count,
				      sizeof(struct rpc_iscsi_auth_secret));
}

static const struct spdk_json_object_decoder rpc_iscsi_create_auth_group_decoders[] = {
	{"tag", offsetof(struct rpc_iscsi_create_auth_group_ctx, tag), spdk_json_decode_int32},
	{"secrets", offsetof(struct rpc_iscsi_create_auth_group_ctx, secrets), decode_rpc_iscsi_auth_secrets, true},
};

static void
rpc_iscsi_create_auth_group(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_iscsi_create_auth_group_ctx req = {};
	struct rpc_iscsi_auth_secret *_secret;
	struct spdk_iscsi_auth_group *group = NULL;
	int rc;
	size_t i;

	if (spdk_json_decode_object(params, rpc_iscsi_create_auth_group_decoders,
				    SPDK_COUNTOF(rpc_iscsi_create_auth_group_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free_rpc_iscsi_create_auth_group(&req);
		return;
	}

	pthread_mutex_lock(&g_iscsi.mutex);

	rc = iscsi_add_auth_group(req.tag, &group);
	if (rc != 0) {
		pthread_mutex_unlock(&g_iscsi.mutex);

		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Could not add auth group (%d), %s",
						     req.tag, spdk_strerror(-rc));
		free_rpc_iscsi_create_auth_group(&req);
		return;
	}

	for (i = 0; i < req.secrets.count; i++) {
		_secret = &req.secrets.items[i];
		rc = iscsi_auth_group_add_secret(group, _secret->user, _secret->secret,
						 _secret->muser, _secret->msecret);
		if (rc != 0) {
			iscsi_delete_auth_group(group);
			pthread_mutex_unlock(&g_iscsi.mutex);

			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							     "Could not add secret to auth group (%d), %s",
							     req.tag, spdk_strerror(-rc));
			free_rpc_iscsi_create_auth_group(&req);
			return;
		}
	}

	pthread_mutex_unlock(&g_iscsi.mutex);

	free_rpc_iscsi_create_auth_group(&req);

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("iscsi_create_auth_group", rpc_iscsi_create_auth_group, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_iscsi_delete_auth_group_decoders[] = {
	{"tag", offsetof(struct rpc_iscsi_delete_auth_group_ctx, tag), spdk_json_decode_int32},
};

static void
rpc_iscsi_delete_auth_group(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_iscsi_delete_auth_group_ctx req = {};
	struct spdk_iscsi_auth_group *group;

	if (spdk_json_decode_object(params, rpc_iscsi_delete_auth_group_decoders,
				    SPDK_COUNTOF(rpc_iscsi_delete_auth_group_decoders), &req)) {
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

static const struct spdk_json_object_decoder rpc_iscsi_auth_group_add_secret_decoders[] = {
	{"tag", offsetof(struct rpc_iscsi_auth_group_add_secret_ctx, tag), spdk_json_decode_int32},
	{"user", offsetof(struct rpc_iscsi_auth_group_add_secret_ctx, user), spdk_json_decode_string},
	{"secret", offsetof(struct rpc_iscsi_auth_group_add_secret_ctx, secret), spdk_json_decode_string},
	{"muser", offsetof(struct rpc_iscsi_auth_group_add_secret_ctx, muser), spdk_json_decode_string, true},
	{"msecret", offsetof(struct rpc_iscsi_auth_group_add_secret_ctx, msecret), spdk_json_decode_string, true},
};

static void
rpc_iscsi_auth_group_add_secret(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_iscsi_auth_group_add_secret_ctx req = {};
	struct spdk_iscsi_auth_group *group;
	int rc;

	if (spdk_json_decode_object(params, rpc_iscsi_auth_group_add_secret_decoders,
				    SPDK_COUNTOF(rpc_iscsi_auth_group_add_secret_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free_rpc_iscsi_auth_group_add_secret(&req);
		return;
	}

	pthread_mutex_lock(&g_iscsi.mutex);

	group = iscsi_find_auth_group_by_tag(req.tag);
	if (group == NULL) {
		pthread_mutex_unlock(&g_iscsi.mutex);

		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Could not find auth group (%d)", req.tag);
		free_rpc_iscsi_auth_group_add_secret(&req);
		return;
	}

	rc = iscsi_auth_group_add_secret(group, req.user, req.secret, req.muser, req.msecret);
	if (rc != 0) {
		pthread_mutex_unlock(&g_iscsi.mutex);

		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Could not add secret to auth group (%d), %s",
						     req.tag, spdk_strerror(-rc));
		free_rpc_iscsi_auth_group_add_secret(&req);
		return;
	}

	pthread_mutex_unlock(&g_iscsi.mutex);

	free_rpc_iscsi_auth_group_add_secret(&req);

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("iscsi_auth_group_add_secret", rpc_iscsi_auth_group_add_secret,
		  SPDK_RPC_RUNTIME)


static const struct spdk_json_object_decoder rpc_iscsi_auth_group_remove_secret_decoders[] = {
	{"tag", offsetof(struct rpc_iscsi_auth_group_remove_secret_ctx, tag), spdk_json_decode_int32},
	{"user", offsetof(struct rpc_iscsi_auth_group_remove_secret_ctx, user), spdk_json_decode_string},
};

static void
rpc_iscsi_auth_group_remove_secret(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	struct rpc_iscsi_auth_group_remove_secret_ctx req = {};
	struct spdk_iscsi_auth_group *group;
	int rc;

	if (spdk_json_decode_object(params, rpc_iscsi_auth_group_remove_secret_decoders,
				    SPDK_COUNTOF(rpc_iscsi_auth_group_remove_secret_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free_rpc_iscsi_auth_group_remove_secret(&req);
		return;
	}

	pthread_mutex_lock(&g_iscsi.mutex);

	group = iscsi_find_auth_group_by_tag(req.tag);
	if (group == NULL) {
		pthread_mutex_unlock(&g_iscsi.mutex);

		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Could not find auth group (%d)", req.tag);
		free_rpc_iscsi_auth_group_remove_secret(&req);
		return;
	}

	rc = iscsi_auth_group_delete_secret(group, req.user);
	if (rc != 0) {
		pthread_mutex_unlock(&g_iscsi.mutex);

		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Could not delete secret from CHAP group (%d), %s",
						     req.tag, spdk_strerror(-rc));
		free_rpc_iscsi_auth_group_remove_secret(&req);
		return;
	}

	pthread_mutex_unlock(&g_iscsi.mutex);

	free_rpc_iscsi_auth_group_remove_secret(&req);

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("iscsi_auth_group_remove_secret",
		  rpc_iscsi_auth_group_remove_secret, SPDK_RPC_RUNTIME)

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

static const struct spdk_json_object_decoder rpc_iscsi_set_options_decoders[] = {
	{"auth_file", offsetof(struct spdk_iscsi_opts, authfile), spdk_json_decode_string, true},
	{"node_base", offsetof(struct spdk_iscsi_opts, nodebase), spdk_json_decode_string, true},
	{"nop_timeout", offsetof(struct spdk_iscsi_opts, timeout), spdk_json_decode_int32, true},
	{"nop_in_interval", offsetof(struct spdk_iscsi_opts, nopininterval), spdk_json_decode_int32, true},
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
	{"pdu_pool_size", offsetof(struct spdk_iscsi_opts, pdu_pool_size), spdk_json_decode_uint32, true},
	{"immediate_data_pool_size", offsetof(struct spdk_iscsi_opts, immediate_data_pool_size), spdk_json_decode_uint32, true},
	{"data_out_pool_size", offsetof(struct spdk_iscsi_opts, data_out_pool_size), spdk_json_decode_uint32, true},
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
		if (spdk_json_decode_object(params, rpc_iscsi_set_options_decoders,
					    SPDK_COUNTOF(rpc_iscsi_set_options_decoders), opts)) {
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

static const struct spdk_json_object_decoder rpc_iscsi_enable_histogram_decoders[] = {
	{"name", offsetof(struct rpc_iscsi_enable_histogram_ctx, name), spdk_json_decode_string},
	{"enable", offsetof(struct rpc_iscsi_enable_histogram_ctx, enable), spdk_json_decode_bool},
};

struct iscsi_enable_histogram_ctx {
	struct spdk_jsonrpc_request *request;
	struct spdk_iscsi_tgt_node *target;
	bool enable;
	int status;
	struct spdk_thread *orig_thread;
};

static void
rpc_iscsi_enable_histogram_done(void *_ctx)
{
	struct iscsi_enable_histogram_ctx *ctx = _ctx;

	if (ctx->status == 0) {
		spdk_jsonrpc_send_bool_response(ctx->request, true);
	} else {
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-ctx->status));
	}

	free(ctx);
}

static void
_iscsi_enable_histogram(void *_ctx)
{
	struct iscsi_enable_histogram_ctx *ctx = _ctx;

	ctx->status = iscsi_tgt_node_enable_histogram(ctx->target, ctx->enable);
}

static void
_rpc_iscsi_enable_histogram(void *_ctx)
{
	struct iscsi_enable_histogram_ctx *ctx = _ctx;

	pthread_mutex_lock(&ctx->target->mutex);
	_iscsi_enable_histogram(ctx);
	ctx->target->num_active_conns--;
	pthread_mutex_unlock(&ctx->target->mutex);

	spdk_thread_send_msg(ctx->orig_thread, rpc_iscsi_enable_histogram_done, ctx);
}

static void
rpc_iscsi_enable_histogram(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_iscsi_enable_histogram_ctx req = {};
	struct iscsi_enable_histogram_ctx *ctx;
	struct spdk_iscsi_tgt_node *target;
	struct spdk_thread *thread;
	spdk_msg_fn fn;

	if (spdk_json_decode_object(params, rpc_iscsi_enable_histogram_decoders,
				    SPDK_COUNTOF(rpc_iscsi_enable_histogram_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Memory allocation failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation failed");
		return;
	}

	target = iscsi_find_tgt_node(req.name);

	free_rpc_iscsi_enable_histogram(&req);

	if (target == NULL) {
		SPDK_ERRLOG("target is not found\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free(ctx);
		return;
	}

	ctx->request = request;
	ctx->target = target;
	ctx->enable = req.enable;
	ctx->orig_thread = spdk_get_thread();

	pthread_mutex_lock(&ctx->target->mutex);
	if (target->pg == NULL) {
		_iscsi_enable_histogram(ctx);
		thread = ctx->orig_thread;
		fn = rpc_iscsi_enable_histogram_done;
	} else {
		/**
		 * We get spdk thread of the target by using target->pg.
		 * If target->num_active_conns >= 1, target->pg will not change.
		 * So, It is safer to increase and decrease target->num_active_conns
		 * while updating target->histogram.
		 */
		target->num_active_conns++;
		thread = spdk_io_channel_get_thread(spdk_io_channel_from_ctx(target->pg));
		fn = _rpc_iscsi_enable_histogram;
	}
	pthread_mutex_unlock(&ctx->target->mutex);

	spdk_thread_send_msg(thread, fn, ctx);
}

SPDK_RPC_REGISTER("iscsi_enable_histogram", rpc_iscsi_enable_histogram, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_iscsi_get_histogram_decoders[] = {
	{"name", offsetof(struct rpc_iscsi_get_histogram_ctx, name), spdk_json_decode_string}
};

static void
rpc_iscsi_get_histogram(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_iscsi_get_histogram_ctx req = {};
	struct spdk_iscsi_tgt_node *target;
	struct spdk_json_write_ctx *w;
	char *encoded_histogram;
	size_t src_len, dst_len;
	int rc;

	if (spdk_json_decode_object(params, rpc_iscsi_get_histogram_decoders,
				    SPDK_COUNTOF(rpc_iscsi_get_histogram_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto free_req;
	}

	target = iscsi_find_tgt_node(req.name);
	if (target == NULL) {
		SPDK_ERRLOG("target is not found\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "target not found");
		goto free_req;
	}

	if (!target->histogram) {
		SPDK_ERRLOG("target's histogram function is not enabled\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "target's histogram function is not enabled");
		goto free_req;
	}

	src_len = SPDK_HISTOGRAM_NUM_BUCKETS(target->histogram) * sizeof(uint64_t);
	dst_len = spdk_base64_get_encoded_strlen(src_len) + 1;
	encoded_histogram = malloc(dst_len);
	if (encoded_histogram == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(ENOMEM));
		goto free_req;
	}

	rc = spdk_base64_encode(encoded_histogram, target->histogram->bucket, src_len);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-rc));
		goto free_encoded_histogram;
	}

	w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "histogram", encoded_histogram);
	spdk_json_write_named_int64(w, "granularity", target->histogram->granularity);
	spdk_json_write_named_uint32(w, "min_range", target->histogram->min_range);
	spdk_json_write_named_uint32(w, "max_range", target->histogram->max_range);
	spdk_json_write_named_int64(w, "tsc_rate", spdk_get_ticks_hz());

	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

free_encoded_histogram:
	free(encoded_histogram);
free_req:
	free_rpc_iscsi_get_histogram(&req);
}

SPDK_RPC_REGISTER("iscsi_get_histogram", rpc_iscsi_get_histogram, SPDK_RPC_RUNTIME)
