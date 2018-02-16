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
#include "spdk/event.h"

#include "spdk_internal/log.h"

static void
dump_initiator_group(struct spdk_json_write_ctx *w, struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_initiator_name *iname;
	struct spdk_iscsi_initiator_netmask *imask;

	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "initiators");
	spdk_json_write_array_begin(w);
	TAILQ_FOREACH(iname, &ig->initiator_head, tailq) {
		spdk_json_write_string(w, iname->name);
	}
	spdk_json_write_array_end(w);

	spdk_json_write_name(w, "tag");
	spdk_json_write_int32(w, ig->tag);

	spdk_json_write_name(w, "netmasks");
	spdk_json_write_array_begin(w);
	TAILQ_FOREACH(imask, &ig->netmask_head, tailq) {
		spdk_json_write_string(w, imask->mask);
	}
	spdk_json_write_array_end(w);

	spdk_json_write_object_end(w);
}

static void
spdk_rpc_get_initiator_groups(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct spdk_iscsi_init_grp *ig;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "get_initiator_groups requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_array_begin(w);

	TAILQ_FOREACH(ig, &g_spdk_iscsi.ig_head, tailq) {
		dump_initiator_group(w, ig);
	}

	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_initiator_groups", spdk_rpc_get_initiator_groups)

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
spdk_rpc_add_initiator_group(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_initiator_group req = {};
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_initiator_group_decoders,
				    SPDK_COUNTOF(rpc_initiator_group_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.initiator_list.num_initiators == 0 ||
	    req.netmask_list.num_netmasks == 0) {
		goto invalid;
	}

	if (spdk_iscsi_init_grp_create_from_initiator_list(req.tag,
			req.initiator_list.num_initiators,
			req.initiator_list.initiators,
			req.netmask_list.num_netmasks,
			req.netmask_list.netmasks)) {
		SPDK_ERRLOG("create_from_initiator_list failed\n");
		goto invalid;
	}

	free_rpc_initiator_group(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_initiator_group(&req);
}
SPDK_RPC_REGISTER("add_initiator_group", spdk_rpc_add_initiator_group)

static const struct spdk_json_object_decoder rpc_add_or_delete_initiators_decoders[] = {
	{"tag", offsetof(struct rpc_initiator_group, tag), spdk_json_decode_int32},
	{"initiators", offsetof(struct rpc_initiator_group, initiator_list), decode_rpc_initiator_list, true},
	{"netmasks", offsetof(struct rpc_initiator_group, netmask_list), decode_rpc_netmask_list, true},
};

static void
spdk_rpc_add_initiators_to_initiator_group(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	struct rpc_initiator_group req = {};
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_add_or_delete_initiators_decoders,
				    SPDK_COUNTOF(rpc_add_or_delete_initiators_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (spdk_iscsi_init_grp_add_initiators_from_initiator_list(req.tag,
			req.initiator_list.num_initiators,
			req.initiator_list.initiators,
			req.netmask_list.num_netmasks,
			req.netmask_list.netmasks)) {
		SPDK_ERRLOG("add_initiators_from_initiator_list failed\n");
		goto invalid;
	}

	free_rpc_initiator_group(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_initiator_group(&req);
}
SPDK_RPC_REGISTER("add_initiators_to_initiator_group", spdk_rpc_add_initiators_to_initiator_group)

static void
spdk_rpc_delete_initiators_from_initiator_group(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	struct rpc_initiator_group req = {};
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_add_or_delete_initiators_decoders,
				    SPDK_COUNTOF(rpc_add_or_delete_initiators_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (spdk_iscsi_init_grp_delete_initiators_from_initiator_list(req.tag,
			req.initiator_list.num_initiators,
			req.initiator_list.initiators,
			req.netmask_list.num_netmasks,
			req.netmask_list.netmasks)) {
		SPDK_ERRLOG("delete_initiators_from_initiator_list failed\n");
		goto invalid;
	}

	free_rpc_initiator_group(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_initiator_group(&req);
}
SPDK_RPC_REGISTER("delete_initiators_from_initiator_group",
		  spdk_rpc_delete_initiators_from_initiator_group)

struct rpc_delete_initiator_group {
	int32_t tag;
};

static const struct spdk_json_object_decoder rpc_delete_initiator_group_decoders[] = {
	{"tag", offsetof(struct rpc_delete_initiator_group, tag), spdk_json_decode_int32},
};

static void
spdk_rpc_delete_initiator_group(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_delete_initiator_group req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_iscsi_init_grp *ig;

	if (spdk_json_decode_object(params, rpc_delete_initiator_group_decoders,
				    SPDK_COUNTOF(rpc_delete_initiator_group_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	ig = spdk_iscsi_init_grp_unregister(req.tag);
	if (!ig) {
		goto invalid;
	}
	spdk_iscsi_tgt_node_delete_map(NULL, ig);
	spdk_iscsi_init_grp_destroy(ig);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
}
SPDK_RPC_REGISTER("delete_initiator_group", spdk_rpc_delete_initiator_group)

static void
dump_target_node(struct spdk_json_write_ctx *w, struct spdk_iscsi_tgt_node *tgtnode)
{
	struct spdk_iscsi_pg_map *pg_map;
	struct spdk_iscsi_ig_map *ig_map;
	int i;

	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "name");
	spdk_json_write_string(w, tgtnode->name);

	if (tgtnode->alias) {
		spdk_json_write_name(w, "alias_name");
		spdk_json_write_string(w, tgtnode->alias);
	}

	spdk_json_write_name(w, "pg_ig_maps");
	spdk_json_write_array_begin(w);
	TAILQ_FOREACH(pg_map, &tgtnode->pg_map_head, tailq) {
		TAILQ_FOREACH(ig_map, &pg_map->ig_map_head, tailq) {
			spdk_json_write_object_begin(w);
			spdk_json_write_name(w, "pg_tag");
			spdk_json_write_int32(w, pg_map->pg->tag);
			spdk_json_write_name(w, "ig_tag");
			spdk_json_write_int32(w, ig_map->ig->tag);
			spdk_json_write_object_end(w);
		}
	}
	spdk_json_write_array_end(w);

	spdk_json_write_name(w, "luns");
	spdk_json_write_array_begin(w);
	for (i = 0; i < SPDK_SCSI_DEV_MAX_LUN; i++) {
		struct spdk_scsi_lun *lun = spdk_scsi_dev_get_lun(tgtnode->dev, i);

		if (lun) {
			spdk_json_write_object_begin(w);
			spdk_json_write_name(w, "bdev_name");
			spdk_json_write_string(w, spdk_scsi_lun_get_bdev_name(lun));
			spdk_json_write_name(w, "id");
			spdk_json_write_int32(w, spdk_scsi_lun_get_id(lun));
			spdk_json_write_object_end(w);
		}
	}
	spdk_json_write_array_end(w);

	spdk_json_write_name(w, "queue_depth");
	spdk_json_write_int32(w, tgtnode->queue_depth);

	spdk_json_write_name(w, "chap_disabled");
	spdk_json_write_bool(w, tgtnode->auth_chap_disabled);

	spdk_json_write_name(w, "chap_required");
	spdk_json_write_bool(w, tgtnode->auth_chap_required);

	spdk_json_write_name(w, "chap_mutual");
	spdk_json_write_bool(w, tgtnode->auth_chap_mutual);

	spdk_json_write_name(w, "chap_auth_group");
	spdk_json_write_int32(w, tgtnode->auth_group);

	spdk_json_write_name(w, "header_digest");
	spdk_json_write_int32(w, tgtnode->header_digest);

	spdk_json_write_name(w, "data_digest");
	spdk_json_write_int32(w, tgtnode->data_digest);

	spdk_json_write_object_end(w);
}

static void
spdk_rpc_get_target_nodes(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct spdk_iscsi_tgt_node *tgtnode;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "get_target_nodes requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_array_begin(w);

	TAILQ_FOREACH(tgtnode, &g_spdk_iscsi.target_head, tailq) {
		dump_target_node(w, tgtnode);
	}

	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_target_nodes", spdk_rpc_get_target_nodes)

struct rpc_pg_tags {
	size_t num_tags;
	int32_t tags[MAX_TARGET_MAP];
};

static int
decode_rpc_pg_tags(const struct spdk_json_val *val, void *out)
{
	struct rpc_pg_tags *pg_tags = out;

	return spdk_json_decode_array(val, spdk_json_decode_int32, pg_tags->tags, MAX_TARGET_MAP,
				      &pg_tags->num_tags, sizeof(int32_t));
}

struct rpc_ig_tags {
	size_t num_tags;
	int32_t tags[MAX_TARGET_MAP];
};

static int
decode_rpc_ig_tags(const struct spdk_json_val *val, void *out)
{
	struct rpc_ig_tags *ig_tags = out;

	return spdk_json_decode_array(val, spdk_json_decode_int32, ig_tags->tags, MAX_TARGET_MAP,
				      &ig_tags->num_tags, sizeof(int32_t));
}

#define RPC_CONSTRUCT_TARGET_NODE_MAX_LUN	64

struct rpc_bdev_names {
	size_t num_names;
	char *names[RPC_CONSTRUCT_TARGET_NODE_MAX_LUN];
};

static int
decode_rpc_bdev_names(const struct spdk_json_val *val, void *out)
{
	struct rpc_bdev_names *bdev_names = out;

	return spdk_json_decode_array(val, spdk_json_decode_string, bdev_names->names,
				      RPC_CONSTRUCT_TARGET_NODE_MAX_LUN,
				      &bdev_names->num_names, sizeof(char *));
}

static void
free_rpc_bdev_names(struct rpc_bdev_names *r)
{
	size_t i;

	for (i = 0; i < r->num_names; i++) {
		free(r->names[i]);
	}
}

struct rpc_lun_ids {
	size_t num_ids;
	int32_t ids[RPC_CONSTRUCT_TARGET_NODE_MAX_LUN];
};

static int
decode_rpc_lun_ids(const struct spdk_json_val *val, void *out)
{
	struct rpc_lun_ids *lun_ids = out;

	return spdk_json_decode_array(val, spdk_json_decode_int32, lun_ids->ids,
				      RPC_CONSTRUCT_TARGET_NODE_MAX_LUN,
				      &lun_ids->num_ids, sizeof(int32_t));
}

struct rpc_target_node {
	char *name;
	char *alias_name;

	struct rpc_pg_tags pg_tags;
	struct rpc_ig_tags ig_tags;

	struct rpc_bdev_names bdev_names;
	struct rpc_lun_ids lun_ids;

	int32_t queue_depth;
	bool chap_disabled;
	bool chap_required;
	bool chap_mutual;
	int32_t chap_auth_group;

	int32_t header_digest;
	int32_t data_digest;
};

static void
free_rpc_target_node(struct rpc_target_node *req)
{
	free(req->name);
	free(req->alias_name);
	free_rpc_bdev_names(&req->bdev_names);
}

static const struct spdk_json_object_decoder rpc_target_node_decoders[] = {
	{"name", offsetof(struct rpc_target_node, name), spdk_json_decode_string},
	{"alias_name", offsetof(struct rpc_target_node, alias_name), spdk_json_decode_string},
	{"pg_tags", offsetof(struct rpc_target_node, pg_tags), decode_rpc_pg_tags},
	{"ig_tags", offsetof(struct rpc_target_node, ig_tags), decode_rpc_ig_tags},
	{"bdev_names", offsetof(struct rpc_target_node, bdev_names), decode_rpc_bdev_names},
	{"lun_ids", offsetof(struct rpc_target_node, lun_ids), decode_rpc_lun_ids},
	{"queue_depth", offsetof(struct rpc_target_node, queue_depth), spdk_json_decode_int32},
	{"chap_disabled", offsetof(struct rpc_target_node, chap_disabled), spdk_json_decode_bool},
	{"chap_required", offsetof(struct rpc_target_node, chap_required), spdk_json_decode_bool},
	{"chap_mutual", offsetof(struct rpc_target_node, chap_mutual), spdk_json_decode_bool},
	{"chap_auth_group", offsetof(struct rpc_target_node, chap_auth_group), spdk_json_decode_int32},
	{"header_digest", offsetof(struct rpc_target_node, header_digest), spdk_json_decode_int32, true},
	{"data_digest", offsetof(struct rpc_target_node, data_digest), spdk_json_decode_int32, true},
};

static void
spdk_rpc_construct_target_node(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_target_node req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_iscsi_tgt_node *target;

	req.header_digest = 0;
	req.data_digest = 0;

	if (spdk_json_decode_object(params, rpc_target_node_decoders,
				    SPDK_COUNTOF(rpc_target_node_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.pg_tags.num_tags != req.ig_tags.num_tags) {
		SPDK_ERRLOG("pg_tags/ig_tags count mismatch\n");
		goto invalid;
	}

	if (req.bdev_names.num_names != req.lun_ids.num_ids) {
		SPDK_ERRLOG("bdev_names/lun_ids count mismatch\n");
		goto invalid;
	}

	/*
	 * Use default parameters in a few places:
	 *  index = -1 : automatically pick an index for the new target node
	 *  alias = NULL
	 *  0, 0 = disable header/data digests
	 */
	target = spdk_iscsi_tgt_node_construct(-1, req.name, req.alias_name,
					       req.pg_tags.tags,
					       req.ig_tags.tags,
					       req.pg_tags.num_tags,
					       (const char **)req.bdev_names.names,
					       req.lun_ids.ids,
					       req.bdev_names.num_names,
					       req.queue_depth,
					       req.chap_disabled,
					       req.chap_required,
					       req.chap_mutual,
					       req.chap_auth_group,
					       req.header_digest,
					       req.data_digest);

	if (target == NULL) {
		goto invalid;
	}

	free_rpc_target_node(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_target_node(&req);
}
SPDK_RPC_REGISTER("construct_target_node", spdk_rpc_construct_target_node)

struct rpc_pg_ig_maps {
	char *name;
	struct rpc_pg_tags pg_tags;
	struct rpc_ig_tags ig_tags;
};

static const struct spdk_json_object_decoder rpc_pg_ig_maps_decoders[] = {
	{"name", offsetof(struct rpc_pg_ig_maps, name), spdk_json_decode_string},
	{"pg_tags", offsetof(struct rpc_pg_ig_maps, pg_tags), decode_rpc_pg_tags},
	{"ig_tags", offsetof(struct rpc_pg_ig_maps, ig_tags), decode_rpc_ig_tags},
};

static void
spdk_rpc_add_pg_ig_maps(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_pg_ig_maps req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_iscsi_tgt_node *target;
	int rc;

	if (spdk_json_decode_object(params, rpc_pg_ig_maps_decoders,
				    SPDK_COUNTOF(rpc_pg_ig_maps_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.pg_tags.num_tags != req.ig_tags.num_tags) {
		SPDK_ERRLOG("pg_tags/ig_tags count mismatch\n");
		goto invalid;
	}

	target = spdk_iscsi_find_tgt_node(req.name);
	if (target == NULL) {
		SPDK_ERRLOG("target is not found\n");
		goto invalid;
	}

	rc = spdk_iscsi_tgt_node_add_pg_ig_maps(target, req.pg_tags.tags,
						req.ig_tags.tags,
						req.pg_tags.num_tags);
	if (rc < 0) {
		SPDK_ERRLOG("add pg-ig maps failed\n");
		goto invalid;
	}

	free(req.name);

	w = spdk_jsonrpc_begin_result(request);
	if (w != NULL) {
		spdk_json_write_bool(w, true);
		spdk_jsonrpc_end_result(request, w);
	}
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 "Invalid parameters");
	free(req.name);
}
SPDK_RPC_REGISTER("add_pg_ig_maps", spdk_rpc_add_pg_ig_maps)

static void
spdk_rpc_delete_pg_ig_maps(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_pg_ig_maps req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_iscsi_tgt_node *target;
	int rc;

	if (spdk_json_decode_object(params, rpc_pg_ig_maps_decoders,
				    SPDK_COUNTOF(rpc_pg_ig_maps_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.pg_tags.num_tags != req.ig_tags.num_tags) {
		SPDK_ERRLOG("pg_tags/ig_tags count mismatch\n");
		goto invalid;
	}

	target = spdk_iscsi_find_tgt_node(req.name);
	if (target == NULL) {
		SPDK_ERRLOG("target is not found\n");
		goto invalid;
	}

	rc = spdk_iscsi_tgt_node_delete_pg_ig_maps(target, req.pg_tags.tags,
			req.ig_tags.tags,
			req.pg_tags.num_tags);
	if (rc < 0) {
		SPDK_ERRLOG("remove pg-ig maps failed\n");
		goto invalid;
	}

	free(req.name);

	w = spdk_jsonrpc_begin_result(request);
	if (w != NULL) {
		spdk_json_write_bool(w, true);
		spdk_jsonrpc_end_result(request, w);
	}
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 "Invalid parameters");
	free(req.name);
}
SPDK_RPC_REGISTER("delete_pg_ig_maps", spdk_rpc_delete_pg_ig_maps)

struct rpc_delete_target_node {
	char *name;
};

static void
free_rpc_delete_target_node(struct rpc_delete_target_node *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_delete_target_node_decoders[] = {
	{"name", offsetof(struct rpc_delete_target_node, name), spdk_json_decode_string},
};

static void
spdk_rpc_delete_target_node(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_delete_target_node req = {};
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_delete_target_node_decoders,
				    SPDK_COUNTOF(rpc_delete_target_node_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.name == NULL) {
		SPDK_ERRLOG("missing name param\n");
		goto invalid;
	}

	if (spdk_iscsi_shutdown_tgt_node_by_name(req.name)) {
		SPDK_ERRLOG("shutdown_tgt_node_by_name failed\n");
		goto invalid;
	}

	free_rpc_delete_target_node(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_delete_target_node(&req);
}
SPDK_RPC_REGISTER("delete_target_node", spdk_rpc_delete_target_node)

static void
dump_portal_group(struct spdk_json_write_ctx *w, struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_portal *portal;

	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "portals");
	spdk_json_write_array_begin(w);
	TAILQ_FOREACH(portal, &pg->head, per_pg_tailq) {
		spdk_json_write_object_begin(w);
		spdk_json_write_name(w, "host");
		spdk_json_write_string(w, portal->host);
		spdk_json_write_name(w, "port");
		spdk_json_write_string(w, portal->port);
		spdk_json_write_name(w, "cpumask");
		spdk_json_write_string_fmt(w, "0x%s", spdk_cpuset_fmt(portal->cpumask));
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	spdk_json_write_name(w, "tag");
	spdk_json_write_int32(w, pg->tag);

	spdk_json_write_object_end(w);
}

static void
spdk_rpc_get_portal_groups(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct spdk_iscsi_portal_grp *pg;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "get_portal_groups requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_array_begin(w);

	TAILQ_FOREACH(pg, &g_spdk_iscsi.pg_head, tailq) {
		dump_portal_group(w, pg);
	}

	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_portal_groups", spdk_rpc_get_portal_groups)

struct rpc_portal {
	char *host;
	char *port;
	char *cpumask;
};

struct rpc_portal_list {
	size_t num_portals;
	struct rpc_portal portals[MAX_PORTAL];
};

struct rpc_portal_group {
	int32_t tag;
	struct rpc_portal_list portal_list;
};

static void
free_rpc_portal(struct rpc_portal *portal)
{
	free(portal->host);
	free(portal->port);
	free(portal->cpumask);
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
	{"cpumask", offsetof(struct rpc_portal, cpumask), spdk_json_decode_string, true},
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
};

static void
spdk_rpc_add_portal_group(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_portal_group req = {};
	struct spdk_iscsi_portal_grp *pg = NULL;
	struct spdk_iscsi_portal *portal;
	struct spdk_json_write_ctx *w;
	size_t i = 0;
	int rc = -1;

	if (spdk_json_decode_object(params, rpc_portal_group_decoders,
				    SPDK_COUNTOF(rpc_portal_group_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto out;
	}

	pg = spdk_iscsi_portal_grp_create(req.tag);
	if (pg == NULL) {
		SPDK_ERRLOG("portal_grp_create failed\n");
		goto out;
	}
	for (i = 0; i < req.portal_list.num_portals; i++) {
		portal = spdk_iscsi_portal_create(req.portal_list.portals[i].host,
						  req.portal_list.portals[i].port,
						  req.portal_list.portals[i].cpumask);
		if (portal == NULL) {
			SPDK_ERRLOG("portal_create failed\n");
			goto out;
		}
		spdk_iscsi_portal_grp_add_portal(pg, portal);
	}

	rc = spdk_iscsi_portal_grp_open(pg);
	if (rc != 0) {
		SPDK_ERRLOG("portal_grp_open failed\n");
		goto out;
	}

	rc = spdk_iscsi_portal_grp_register(pg);
	if (rc != 0) {
		SPDK_ERRLOG("portal_grp_register failed\n");
	}

out:
	if (rc == 0) {
		w = spdk_jsonrpc_begin_result(request);
		if (w != NULL) {
			spdk_json_write_bool(w, true);
			spdk_jsonrpc_end_result(request, w);
		}
	} else {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");

		if (pg != NULL) {
			spdk_iscsi_portal_grp_release(pg);
		}
	}
	free_rpc_portal_group(&req);
}
SPDK_RPC_REGISTER("add_portal_group", spdk_rpc_add_portal_group)

struct rpc_delete_portal_group {
	int32_t tag;
};

static const struct spdk_json_object_decoder rpc_delete_portal_group_decoders[] = {
	{"tag", offsetof(struct rpc_delete_portal_group, tag), spdk_json_decode_int32},
};

static void
spdk_rpc_delete_portal_group(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_delete_portal_group req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_iscsi_portal_grp *pg;

	if (spdk_json_decode_object(params, rpc_delete_portal_group_decoders,
				    SPDK_COUNTOF(rpc_delete_portal_group_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	pg = spdk_iscsi_portal_grp_unregister(req.tag);
	if (!pg) {
		goto invalid;
	}

	spdk_iscsi_tgt_node_delete_map(pg, NULL);
	spdk_iscsi_portal_grp_release(pg);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
}
SPDK_RPC_REGISTER("delete_portal_group", spdk_rpc_delete_portal_group)

static void
spdk_rpc_get_iscsi_connections(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct spdk_iscsi_conn *conns = g_conns_array;
	int i;
	uint16_t tsih;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "get_iscsi_connections requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_array_begin(w);

	for (i = 0; i < MAX_ISCSI_CONNECTIONS; i++) {
		struct spdk_iscsi_conn *c = &conns[i];

		if (!c->is_valid) {
			continue;
		}

		spdk_json_write_object_begin(w);

		spdk_json_write_name(w, "id");
		spdk_json_write_int32(w, c->id);

		spdk_json_write_name(w, "cid");
		spdk_json_write_int32(w, c->cid);

		/*
		 * If we try to return data for a connection that has not
		 *  logged in yet, the session will not be set.  So in this
		 *  case, return -1 for the tsih rather than segfaulting
		 *  on the null c->sess.
		 */
		if (c->sess == NULL) {
			tsih = -1;
		} else {
			tsih = c->sess->tsih;
		}
		spdk_json_write_name(w, "tsih");
		spdk_json_write_int32(w, tsih);

		spdk_json_write_name(w, "lcore_id");
		spdk_json_write_int32(w, c->lcore);

		spdk_json_write_name(w, "initiator_addr");
		spdk_json_write_string(w, c->initiator_addr);

		spdk_json_write_name(w, "target_addr");
		spdk_json_write_string(w, c->target_addr);

		spdk_json_write_name(w, "target_node_name");
		spdk_json_write_string(w, c->target_short_name);

		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_iscsi_connections", spdk_rpc_get_iscsi_connections)

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
spdk_rpc_target_node_add_lun(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_target_lun req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_iscsi_tgt_node *target;
	int rc;

	req.lun_id = -1;

	if (spdk_json_decode_object(params, rpc_target_lun_decoders,
				    SPDK_COUNTOF(rpc_target_lun_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	target = spdk_iscsi_find_tgt_node(req.name);
	if (target == NULL) {
		SPDK_ERRLOG("target is not found\n");
		goto invalid;
	}

	rc = spdk_iscsi_tgt_node_add_lun(target, req.bdev_name, req.lun_id);
	if (rc < 0) {
		SPDK_ERRLOG("add lun failed\n");
		goto invalid;
	}

	free_rpc_target_lun(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 "Invalid parameters");
	free_rpc_target_lun(&req);
}
SPDK_RPC_REGISTER("target_node_add_lun", spdk_rpc_target_node_add_lun)

static void
spdk_rpc_get_iscsi_global_params(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "get_iscsi_global_params requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "auth_file");
	spdk_json_write_string(w, g_spdk_iscsi.authfile);

	spdk_json_write_name(w, "node_base");
	spdk_json_write_string(w, g_spdk_iscsi.nodebase);

	spdk_json_write_name(w, "max_sessions");
	spdk_json_write_uint32(w, g_spdk_iscsi.MaxSessions);

	spdk_json_write_name(w, "max_connections_per_session");
	spdk_json_write_uint32(w, g_spdk_iscsi.MaxConnectionsPerSession);

	spdk_json_write_name(w, "max_queue_depth");
	spdk_json_write_uint32(w, g_spdk_iscsi.MaxQueueDepth);

	spdk_json_write_name(w, "default_time2wait");
	spdk_json_write_uint32(w, g_spdk_iscsi.DefaultTime2Wait);

	spdk_json_write_name(w, "default_time2retain");
	spdk_json_write_uint32(w, g_spdk_iscsi.DefaultTime2Retain);

	spdk_json_write_name(w, "immediate_data");
	spdk_json_write_bool(w, g_spdk_iscsi.ImmediateData);

	spdk_json_write_name(w, "allow_duplicated_isid");
	spdk_json_write_bool(w, g_spdk_iscsi.AllowDuplicateIsid);

	spdk_json_write_name(w, "error_recovery_level");
	spdk_json_write_uint32(w, g_spdk_iscsi.ErrorRecoveryLevel);

	spdk_json_write_name(w, "timeout");
	spdk_json_write_int32(w, g_spdk_iscsi.timeout);

	spdk_json_write_name(w, "nop_in_interval");
	spdk_json_write_int32(w, g_spdk_iscsi.nopininterval);

	spdk_json_write_name(w, "discovery_auth_method");
	if (g_spdk_iscsi.no_discovery_auth != 0) {
		spdk_json_write_string(w, "none");
	} else if (g_spdk_iscsi.req_discovery_auth == 0) {
		spdk_json_write_string(w, "auto");
	} else if (g_spdk_iscsi.req_discovery_auth_mutual != 0) {
		spdk_json_write_string(w, "chap mutual");
	} else {
		spdk_json_write_string(w, "chap");
	}

	spdk_json_write_name(w, "discovery_auth_group");
	spdk_json_write_int32(w, g_spdk_iscsi.discovery_auth_group);

	spdk_json_write_name(w, "min_connections_per_core");
	spdk_json_write_int32(w, spdk_iscsi_conn_get_min_per_core());

	spdk_json_write_object_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_iscsi_global_params", spdk_rpc_get_iscsi_global_params)
