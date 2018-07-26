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

#include "spdk/rpc.h"
#include "spdk/bdev.h"
#include "bdev_raid.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk_internal/log.h"
#include "spdk/env.h"

#define RPC_MAX_BASE_BDEVS 255

static void raid_bdev_config_destroy(struct raid_bdev_config *raid_bdev_config);

SPDK_LOG_REGISTER_COMPONENT("raidrpc", SPDK_LOG_RAID_RPC)

/*
 * brief:
 * check_raid_bdev_present function tells if the raid bdev with given name already
 * exists or not.
 * params:
 * name - raid bdev name
 * returns:
 * NULL - raid bdev not present
 * non NULL - raid bdev present, returns raid_bdev_ctxt
 */
static struct raid_bdev_ctxt *
check_raid_bdev_present(char *raid_bdev_name)
{
	struct raid_bdev       *raid_bdev;
	struct raid_bdev_ctxt  *raid_bdev_ctxt;

	TAILQ_FOREACH(raid_bdev, &g_spdk_raid_bdev_list, link_global_list) {
		raid_bdev_ctxt = SPDK_CONTAINEROF(raid_bdev, struct raid_bdev_ctxt, raid_bdev);
		if (strcmp(raid_bdev_ctxt->bdev.name, raid_bdev_name) == 0) {
			/* raid bdev found */
			return raid_bdev_ctxt;
		}
	}

	return NULL;
}

/*
 * Input structure for get_raid_bdevs RPC
 */
struct rpc_get_raid_bdevs {
	/* category - all or online or configuring or offline */
	char *category;
};

/*
 * brief:
 * free_rpc_get_raids function frees RPC get_raids related parameters
 * params:
 * req - pointer to RPC request
 * returns:
 * none
 */
static void
free_rpc_get_raid_bdevs(struct rpc_get_raid_bdevs *req)
{
	free(req->category);
}

/*
 * Decoder object for RPC get_raids
 */
static const struct spdk_json_object_decoder rpc_get_raid_bdevs_decoders[] = {
	{"category", offsetof(struct rpc_get_raid_bdevs, category), spdk_json_decode_string},
};

/*
 * brief:
 * spdk_rpc_get_raids function is the RPC for get_raids. This is used to list
 * all the raid bdev names based on the input category requested. Category should be
 * one of "all", "online", "configuring" or "offline". "all" means all the raids
 * whether they are online or configuring or offline. "online" is the raid bdev which
 * is registered with bdev layer. "configuring" is the raid bdev which does not have
 * full configuration discovered yet. "offline" is the raid bdev which is not
 * registered with bdev as of now and it has encountered any error or user has
 * requested to offline the raid.
 * params:
 * requuest - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
spdk_rpc_get_raid_bdevs(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_get_raid_bdevs   req = {};
	struct spdk_json_write_ctx  *w;
	struct raid_bdev            *raid_bdev;
	struct raid_bdev_ctxt       *raid_bdev_ctxt;

	if (spdk_json_decode_object(params, rpc_get_raid_bdevs_decoders,
				    SPDK_COUNTOF(rpc_get_raid_bdevs_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free_rpc_get_raid_bdevs(&req);
		return;
	}

	if (!(strcmp(req.category, "all") == 0 ||
	      strcmp(req.category, "online") == 0 ||
	      strcmp(req.category, "configuring") == 0 ||
	      strcmp(req.category, "offline") == 0)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free_rpc_get_raid_bdevs(&req);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		free_rpc_get_raid_bdevs(&req);
		return;
	}

	spdk_json_write_array_begin(w);

	/* Get raid bdev list based on the category requested */
	if (strcmp(req.category, "all") == 0) {
		TAILQ_FOREACH(raid_bdev, &g_spdk_raid_bdev_list, link_global_list) {
			raid_bdev_ctxt = SPDK_CONTAINEROF(raid_bdev, struct raid_bdev_ctxt, raid_bdev);
			spdk_json_write_string(w, raid_bdev_ctxt->bdev.name);
		}
	} else if (strcmp(req.category, "online") == 0) {
		TAILQ_FOREACH(raid_bdev, &g_spdk_raid_bdev_configured_list, link_specific_list) {
			raid_bdev_ctxt = SPDK_CONTAINEROF(raid_bdev, struct raid_bdev_ctxt, raid_bdev);
			spdk_json_write_string(w, raid_bdev_ctxt->bdev.name);
		}
	} else if (strcmp(req.category, "configuring") == 0) {
		TAILQ_FOREACH(raid_bdev, &g_spdk_raid_bdev_configuring_list, link_specific_list) {
			raid_bdev_ctxt = SPDK_CONTAINEROF(raid_bdev, struct raid_bdev_ctxt, raid_bdev);
			spdk_json_write_string(w, raid_bdev_ctxt->bdev.name);
		}
	} else {
		TAILQ_FOREACH(raid_bdev, &g_spdk_raid_bdev_offline_list, link_specific_list) {
			raid_bdev_ctxt = SPDK_CONTAINEROF(raid_bdev, struct raid_bdev_ctxt, raid_bdev);
			spdk_json_write_string(w, raid_bdev_ctxt->bdev.name);
		}
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_get_raid_bdevs(&req);
}
SPDK_RPC_REGISTER("get_raid_bdevs", spdk_rpc_get_raid_bdevs, SPDK_RPC_RUNTIME)

/*
 * Base bdevs in RPC construct_raid
 */
struct rpc_construct_raid_base_bdevs {
	/* Number of base bdevs */
	size_t           num_base_bdevs;

	/* List of base bdevs names */
	char             *base_bdevs[RPC_MAX_BASE_BDEVS];
};

/*
 * Input structure for RPC construct_raid
 */
struct rpc_construct_raid_bdev {
	/* Raid bdev name */
	char                                 *name;

	/* RAID strip size */
	uint32_t                             strip_size;

	/* RAID raid level */
	uint8_t                              raid_level;

	/* Base bdevs information */
	struct rpc_construct_raid_base_bdevs base_bdevs;
};

/*
 * brief:
 * free_rpc_construct_raid_bdev function is to free RPC construct_raid_bdev related parameters
 * params:
 * req - pointer to RPC request
 * returns:
 * none
 */
static void
free_rpc_construct_raid_bdev(struct rpc_construct_raid_bdev *req)
{
	free(req->name);
	for (size_t iter = 0; iter < req->base_bdevs.num_base_bdevs; iter++) {
		free(req->base_bdevs.base_bdevs[iter]);
	}
}

/*
 * Decoder function for RPC construct_raid_bdev to decode base bdevs list
 */
static int
decode_base_bdevs(const struct spdk_json_val *val, void *out)
{
	struct rpc_construct_raid_base_bdevs *base_bdevs = out;
	return spdk_json_decode_array(val, spdk_json_decode_string, base_bdevs->base_bdevs,
				      RPC_MAX_BASE_BDEVS, &base_bdevs->num_base_bdevs, sizeof(char *));
}

/*
 * Decoder object for RPC construct_raid
 */
static const struct spdk_json_object_decoder rpc_construct_raid_bdev_decoders[] = {
	{"name", offsetof(struct rpc_construct_raid_bdev, name), spdk_json_decode_string},
	{"strip_size", offsetof(struct rpc_construct_raid_bdev, strip_size), spdk_json_decode_uint32},
	{"raid_level", offsetof(struct rpc_construct_raid_bdev, raid_level), spdk_json_decode_uint32},
	{"base_bdevs", offsetof(struct rpc_construct_raid_bdev, base_bdevs), decode_base_bdevs},
};

/*
 * brief:
 * check_and_remove_raid_bdev function free base bdev descriptors, unclaim the base
 * bdevs and free the raid. This function is used to cleanup when raid is not
 * able to successfully create during constructing the raid via RPC
 * params:
 * raid_bdev_config - pointer to raid_bdev_config structure
 * returns:
 * NULL - raid not present
 * non NULL - raid present, returns raid_bdev_ctxt
 */
static void
check_and_remove_raid_bdev(struct raid_bdev_config *raid_bdev_config)
{
	struct raid_bdev       *raid_bdev;
	struct raid_bdev_ctxt  *raid_bdev_ctxt;

	/* Get the raid structured allocated if exists */
	raid_bdev_ctxt = raid_bdev_config->raid_bdev_ctxt;
	if (raid_bdev_ctxt == NULL) {
		return;
	}

	/*
	 * raid should be in configuring state as this function is used to cleanup
	 * the raid during unsuccessful construction of raid
	 */
	assert(raid_bdev_ctxt->raid_bdev.state == RAID_BDEV_STATE_CONFIGURING);
	raid_bdev = &raid_bdev_ctxt->raid_bdev;
	for (uint32_t i = 0; i < raid_bdev->num_base_bdevs; i++) {
		assert(raid_bdev->base_bdev_info != NULL);
		if (raid_bdev->base_bdev_info[i].base_bdev) {
			/* Release base bdev related resources */
			spdk_bdev_module_release_bdev(raid_bdev->base_bdev_info[i].base_bdev);
			spdk_bdev_close(raid_bdev->base_bdev_info[i].base_bdev_desc);
			raid_bdev->base_bdev_info[i].base_bdev_desc = NULL;
			raid_bdev->base_bdev_info[i].base_bdev = NULL;
			assert(raid_bdev->num_base_bdevs_discovered);
			raid_bdev->num_base_bdevs_discovered--;
		}
	}
	/* Free raid */
	assert(raid_bdev->num_base_bdevs_discovered == 0);
	TAILQ_REMOVE(&g_spdk_raid_bdev_configuring_list, raid_bdev, link_specific_list);
	TAILQ_REMOVE(&g_spdk_raid_bdev_list, raid_bdev, link_global_list);
	free(raid_bdev->base_bdev_info);
	free(raid_bdev_ctxt);
	raid_bdev_config->raid_bdev_ctxt = NULL;
}

/*
 * brief:
 * spdk_rpc_construct_raid_bdev function is the RPC for construct_raids. It takes
 * input as raid bdev name, raid level, strip size in KB and list of base bdev names.
 * params:
 * requuest - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
spdk_rpc_construct_raid_bdev(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_construct_raid_bdev req = {};
	struct spdk_json_write_ctx     *w;
	struct raid_bdev_ctxt          *raid_bdev_ctxt;
	struct raid_bdev_config        *raid_bdev_config;
	struct spdk_bdev               *base_bdev;
	int			       rc;

	if (spdk_json_decode_object(params, rpc_construct_raid_bdev_decoders,
				    SPDK_COUNTOF(rpc_construct_raid_bdev_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free_rpc_construct_raid_bdev(&req);
		return;
	}

	/* Fail the command if raid bdev is already present */
	raid_bdev_ctxt = check_raid_bdev_present(req.name);
	if (raid_bdev_ctxt != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "raid bdev already present");
		free_rpc_construct_raid_bdev(&req);
		return;
	}

	/* Fail the command if input raid level is other than 0 */
	if (req.raid_level != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "invalid raid level");
		free_rpc_construct_raid_bdev(&req);
		return;
	}

	if (spdk_u32_is_pow2(req.strip_size) == false) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "invalid strip size");
		free_rpc_construct_raid_bdev(&req);
		return;
	}

	rc = raid_bdev_config_add(req.name, req.strip_size, req.base_bdevs.num_base_bdevs, req.raid_level,
				  &raid_bdev_config);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
		free_rpc_construct_raid_bdev(&req);
		return;
	}

	for (size_t i = 0; i < raid_bdev_config->num_base_bdevs; i++) {
		raid_bdev_config->base_bdev[i].bdev_name = strdup(req.base_bdevs.base_bdevs[i]);
		if (raid_bdev_config->base_bdev[i].bdev_name == NULL) {
			raid_bdev_config_cleanup(raid_bdev_config);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(ENOMEM));
			free_rpc_construct_raid_bdev(&req);
			return;
		}
	}

	for (size_t i = 0; i < raid_bdev_config->num_base_bdevs; i++) {
		/* Check if base_bdev exists already, if not fail the command */
		base_bdev = spdk_bdev_get_by_name(req.base_bdevs.base_bdevs[i]);
		if (base_bdev == NULL) {
			check_and_remove_raid_bdev(raid_bdev_config);
			raid_bdev_config_cleanup(raid_bdev_config);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "base bdev not found");
			free_rpc_construct_raid_bdev(&req);
			return;
		}

		/*
		 * Try to add base_bdev to this raid bdev, if not able to add fail the
		 * command. This might be because this base_bdev may already be claimed
		 * by some other module
		 */
		if (raid_bdev_add_base_device(base_bdev)) {
			check_and_remove_raid_bdev(raid_bdev_config);
			raid_bdev_config_cleanup(raid_bdev_config);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "base bdev can't be added because of either memory allocation failed or not able to claim");
			free_rpc_construct_raid_bdev(&req);
			return;
		}
	}

	free_rpc_construct_raid_bdev(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("construct_raid_bdev", spdk_rpc_construct_raid_bdev, SPDK_RPC_RUNTIME)

/*
 * Input structure for RPC destroy_raid
 */
struct rpc_destroy_raid_bdev {
	/* raid bdev name */
	char *name;
};

/*
 * brief:
 * free_rpc_destroy_raid_bdev function is used to free RPC destroy_raid_bdev related parameters
 * params:
 * req - pointer to RPC request
 * params:
 * none
 */
static void
free_rpc_destroy_raid_bdev(struct rpc_destroy_raid_bdev *req)
{
	free(req->name);
}

/*
 * Decoder object for RPC destroy_raid
 */
static const struct spdk_json_object_decoder rpc_destroy_raid_bdev_decoders[] = {
	{"name", offsetof(struct rpc_destroy_raid_bdev, name), spdk_json_decode_string},
};

/*
 * brief:
 * Since destroying raid_bdev is asynchronous operation, so this function is
 * used to check if raid bdev still exists. If raid bdev is still there it will create
 * event and check later, otherwise it will proceed with cleanup
 * params:
 * arg - pointer to raid bdev cfg
 * returns:
 * none
 */
static void
raid_bdev_config_destroy_check_raid_bdev_exists(void *arg)
{
	struct raid_bdev_config  *raid_cfg = arg;

	assert(raid_cfg != NULL);
	if (raid_cfg->raid_bdev_ctxt != NULL) {
		/* If raid bdev still exists, schedule event and come back later */
		spdk_thread_send_msg(spdk_get_thread(), raid_bdev_config_destroy_check_raid_bdev_exists, raid_cfg);
		return;
	} else {
		/* If raid bdev does not exist now, go for raid bdev config cleanup */
		raid_bdev_config_destroy(raid_cfg);
	}
}

/*
 * brief:
 * This function will destroy the raid bdev at given slot
 * params:
 * slot - slot number of raid bdev config to destroy
 * returns:
 * none
 */
static void
raid_bdev_config_destroy(struct raid_bdev_config *raid_cfg)
{
	assert(raid_cfg != NULL);
	if (raid_cfg->raid_bdev_ctxt != NULL) {
		/*
		 * If raid bdev exists for this config, wait for raid bdev to get
		 * destroyed and come back later
		 */
		spdk_thread_send_msg(spdk_get_thread(), raid_bdev_config_destroy_check_raid_bdev_exists, raid_cfg);
		return;
	}

	raid_bdev_config_cleanup(raid_cfg);
}

/*
 * brief:
 * spdk_rpc_destroy_raid_bdev function is the RPC for destroy_raid. It takes raid
 * name as input and destroy that raid bdev including freeing the base bdev
 * resources.
 * params:
 * requuest - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
spdk_rpc_destroy_raid_bdev(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_destroy_raid_bdev req = {};
	struct spdk_json_write_ctx   *w;
	struct raid_bdev_config      *raid_bdev_config = NULL;
	struct spdk_bdev             *base_bdev;

	if (spdk_json_decode_object(params, rpc_destroy_raid_bdev_decoders,
				    SPDK_COUNTOF(rpc_destroy_raid_bdev_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free_rpc_destroy_raid_bdev(&req);
		return;
	}

	/* Find raid bdev config for this raid bdev */
	TAILQ_FOREACH(raid_bdev_config, &g_spdk_raid_config.raid_bdev_config_head, link) {
		if (strcmp(raid_bdev_config->name, req.name) == 0) {
			break;
		}
	}

	if (raid_bdev_config == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "raid bdev name not found");
		free_rpc_destroy_raid_bdev(&req);
		return;
	}

	/* Remove all the base bdevs from this raid bdev before destroying the raid bdev */
	for (uint32_t i = 0; i < raid_bdev_config->num_base_bdevs; i++) {
		base_bdev = spdk_bdev_get_by_name(raid_bdev_config->base_bdev[i].bdev_name);
		if (base_bdev != NULL) {
			raid_bdev_remove_base_bdev(base_bdev);
		}
	}

	/*
	 * Call to destroy the raid bdev, but it will only destroy raid bdev if underlying
	 * cleanup is done
	 */
	raid_bdev_config_destroy(raid_bdev_config);

	free_rpc_destroy_raid_bdev(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("destroy_raid_bdev", spdk_rpc_destroy_raid_bdev, SPDK_RPC_RUNTIME)
