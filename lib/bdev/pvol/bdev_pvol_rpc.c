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
#include "bdev_pvol.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk_internal/log.h"
#include "spdk/event.h"
#include "spdk/env.h"

#define RPC_MAX_BASE_BDEVS 255

static void pvol_bdev_config_destroy(struct pvol_bdev_config *pvol_bdev_config);

SPDK_LOG_REGISTER_COMPONENT("pvolrpc", SPDK_LOG_PVOL_RPC)

/*
 * brief:
 * check_pvol_present function tells if the pvol with given name already
 * exists or not.
 * params:
 * name - pvol name
 * returns:
 * NULL - pvol not present
 * non NULL - pvol present, returns pvol_bdev_ctxt
 */
static struct pvol_bdev_ctxt *
check_pvol_present(char *pvol_name)
{
	struct pvol_bdev       *pvol_bdev;
	struct pvol_bdev_ctxt  *pvol_bdev_ctxt;

	TAILQ_FOREACH(pvol_bdev, &spdk_pvol_bdev_list, link_global_list) {
		pvol_bdev_ctxt = SPDK_CONTAINEROF(pvol_bdev, struct pvol_bdev_ctxt, pvol_bdev);
		if (strcmp(pvol_bdev_ctxt->bdev.name, pvol_name) == 0) {
			/* pvol found */
			return pvol_bdev_ctxt;
		}
	}

	return NULL;
}

/*
 * brief:
 * check_and_remove_pvol function free base bdev descriptors, unclaim the base
 * bdevs and free the pvol. This function is used to cleanup when pvol is not
 * able to successfully create during constructing the pvol via RPC
 * params:
 * pvol_bdev_config - pointer to pvol_bdev_config structure
 * returns:
 * NULL - pvol not present
 * non NULL - pvol present, returns pvol_bdev_ctxt
 */
static void
check_and_remove_pvol(struct pvol_bdev_config *pvol_bdev_config)
{
	struct pvol_bdev       *pvol_bdev;
	struct pvol_bdev_ctxt  *pvol_bdev_ctxt;
	uint32_t               iter;

	/* Get the pvol structured allocated if exists */
	pvol_bdev_ctxt = pvol_bdev_config->pvol_bdev_ctxt;
	if (pvol_bdev_ctxt == NULL) {
		return;
	}

	/*
	 * pvol should be in configuring state as this function is used to cleanup
	 * the pvol during unsuccessful construction of pvol
	 */
	assert(pvol_bdev_ctxt->pvol_bdev.state == PVOL_BDEV_STATE_CONFIGURING);
	pvol_bdev = &pvol_bdev_ctxt->pvol_bdev;
	for (iter = 0; iter < pvol_bdev->num_base_bdevs; iter++) {
		assert(pvol_bdev->base_bdev_info != NULL);
		if (pvol_bdev->base_bdev_info[iter].base_bdev) {
			/* Release base bdev related resources */
			spdk_bdev_module_release_bdev(pvol_bdev->base_bdev_info[iter].base_bdev);
			spdk_bdev_close(pvol_bdev->base_bdev_info[iter].base_bdev_desc);
			pvol_bdev->base_bdev_info[iter].base_bdev_desc = NULL;
			pvol_bdev->base_bdev_info[iter].base_bdev = NULL;
			assert(pvol_bdev->num_base_bdevs_discovered);
			pvol_bdev->num_base_bdevs_discovered--;
		}
	}

	/* Free pvol */
	assert(pvol_bdev->num_base_bdevs_discovered == 0);
	TAILQ_REMOVE(&spdk_pvol_bdev_configuring_list, pvol_bdev, link_specific_list);
	TAILQ_REMOVE(&spdk_pvol_bdev_list, pvol_bdev, link_global_list);
	free(pvol_bdev->base_bdev_info);
	free(pvol_bdev_ctxt);
	pvol_bdev_config->pvol_bdev_ctxt = NULL;
}

/*
 * Input structure for get_pvols RPC
 */
struct rpc_get_pvols {
	/* category - all or online or configuring or offline */
	char *category;
};

/*
 * brief:
 * free_rpc_get_pvols function frees RPC get_pvols related parameters
 * params:
 * req - pointer to RPC request
 * returns:
 * none
 */
static void
free_rpc_get_pvols(struct rpc_get_pvols *req)
{
	free(req->category);
}

/*
 * Decoder object for RPC get_pvols
 */
static const struct spdk_json_object_decoder rpc_get_pvols_decoders[] = {
	{"category", offsetof(struct rpc_get_pvols, category), spdk_json_decode_string},
};

/*
 * brief:
 * spdk_rpc_get_pvols function is the RPC for get_pvols. This is used to list
 * all the pvol names based on the input category requested. Category should be
 * one of "all", "online", "configuring" or "offline". "all" means all the pvols
 * whether they are online or configuring or offline. "online" is the pvol which
 * is registered with bdev layer. "configuring" is the pvol which does not have
 * full configuration discovered yet. "offline" is the pvol which is not
 * registered with bdev as of now and it has encountered any error or user has
 * requested to offline the pvol.
 * params:
 * requuest - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
spdk_rpc_get_pvols(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_get_pvols        req = {};
	struct spdk_json_write_ctx  *w;
	struct pvol_bdev            *pvol_bdev;
	struct pvol_bdev_ctxt       *pvol_bdev_ctxt;

	if (spdk_json_decode_object(params, rpc_get_pvols_decoders,
				    SPDK_COUNTOF(rpc_get_pvols_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		return;
	}

	if (!(strcmp(req.category, "all") == 0 ||
	      strcmp(req.category, "online") == 0 ||
	      strcmp(req.category, "configuring") == 0 ||
	      strcmp(req.category, "offline") == 0)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free_rpc_get_pvols(&req);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		free_rpc_get_pvols(&req);
		return;
	}

	spdk_json_write_array_begin(w);

	/* Get pvol list based on the category requested */
	if (strcmp(req.category, "all") == 0) {
		TAILQ_FOREACH(pvol_bdev, &spdk_pvol_bdev_list, link_global_list) {
			pvol_bdev_ctxt = SPDK_CONTAINEROF(pvol_bdev, struct pvol_bdev_ctxt, pvol_bdev);
			spdk_json_write_string(w, pvol_bdev_ctxt->bdev.name);
		}
	} else if (strcmp(req.category, "online") == 0) {
		TAILQ_FOREACH(pvol_bdev, &spdk_pvol_bdev_configured_list, link_specific_list) {
			pvol_bdev_ctxt = SPDK_CONTAINEROF(pvol_bdev, struct pvol_bdev_ctxt, pvol_bdev);
			spdk_json_write_string(w, pvol_bdev_ctxt->bdev.name);
		}
	} else if (strcmp(req.category, "configuring") == 0) {
		TAILQ_FOREACH(pvol_bdev, &spdk_pvol_bdev_configuring_list, link_specific_list) {
			pvol_bdev_ctxt = SPDK_CONTAINEROF(pvol_bdev, struct pvol_bdev_ctxt, pvol_bdev);
			spdk_json_write_string(w, pvol_bdev_ctxt->bdev.name);
		}
	} else {
		TAILQ_FOREACH(pvol_bdev, &spdk_pvol_bdev_offline_list, link_specific_list) {
			pvol_bdev_ctxt = SPDK_CONTAINEROF(pvol_bdev, struct pvol_bdev_ctxt, pvol_bdev);
			spdk_json_write_string(w, pvol_bdev_ctxt->bdev.name);
		}
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_get_pvols(&req);
}
SPDK_RPC_REGISTER("get_pvols", spdk_rpc_get_pvols, SPDK_RPC_RUNTIME)

/*
 * Base bdevs in RPC construct_pvol
 */
struct rpc_construct_pvol_base_bdevs {
	/* Number of base bdevs */
	size_t           num_base_bdevs;

	/* List of base bdevs names */
	char             *base_bdevs[RPC_MAX_BASE_BDEVS];
};

/*
 * Input structure for RPC construct_pvol
 */
struct rpc_construct_pvol {
	/* Pvol name */
	char                                 *name;

	/* PVOL strip size */
	uint32_t                             strip_size;

	/* PVOL rail level */
	uint8_t                              raid_level;

	/* Base bdevs information */
	struct rpc_construct_pvol_base_bdevs base_bdevs;
};

/*
 * brief:
 * free_rpc_construct_pvol function is to free RPC construct_pvol related parameters
 * params:
 * req - pointer to RPC request
 * returns:
 * none
 */
static void
free_rpc_construct_pvol(struct rpc_construct_pvol *req)
{
	size_t iter;

	free(req->name);
	for (iter = 0; iter < req->base_bdevs.num_base_bdevs; iter++) {
		free(req->base_bdevs.base_bdevs[iter]);
	}
}

/*
 * Decoder function for RPC construct_pvol to decode base bdevs list
 */
static int
decode_base_bdevs(const struct spdk_json_val *val, void *out)
{
	struct rpc_construct_pvol_base_bdevs *base_bdevs = out;
	return spdk_json_decode_array(val, spdk_json_decode_string, base_bdevs->base_bdevs,
				      RPC_MAX_BASE_BDEVS, &base_bdevs->num_base_bdevs, sizeof(char *));
}

/*
 * Decoder object for RPC construct_pvol
 */
static const struct spdk_json_object_decoder rpc_construct_pvol_decoders[] = {
	{"name", offsetof(struct rpc_construct_pvol, name), spdk_json_decode_string},
	{"strip_size", offsetof(struct rpc_construct_pvol, strip_size), spdk_json_decode_uint32},
	{"raid_level", offsetof(struct rpc_construct_pvol, raid_level), spdk_json_decode_uint32},
	{"base_bdevs", offsetof(struct rpc_construct_pvol, base_bdevs), decode_base_bdevs},
};

/*
 * brief:
 * pvol_bdev_config_cleanup function is used to free memory for one pvol_bdev in configuration
 * params:
 * none
 * returns:
 * none
 */
static void
pvol_bdev_config_cleanup(void)
{
	void       *temp_ptr;

	temp_ptr = realloc(spdk_pvol_config.pvol_bdev_config,
			   sizeof(struct pvol_bdev_config) * (spdk_pvol_config.total_pvol_bdev - 1));
	if (temp_ptr != NULL) {
		spdk_pvol_config.pvol_bdev_config = temp_ptr;
	} else {
		SPDK_ERRLOG("Config memory allocation failed\n");
		assert(0);
	}
	spdk_pvol_config.total_pvol_bdev--;
}

/*
 * brief:
 * spdk_rpc_construct_pvol function is the RPC for construct_pvols. It takes
 * input as pvol name, raid level, strip size in KB and list of base bdev names.
 * params:
 * requuest - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
spdk_rpc_construct_pvol(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_construct_pvol      req = {};
	struct spdk_json_write_ctx     *w;
	struct pvol_bdev_ctxt          *pvol_bdev_ctxt;
	void                           *temp_ptr;
	struct pvol_base_bdev_config   *base_bdevs;
	struct pvol_bdev_config        *pvol_bdev_config;
	struct spdk_bdev               *base_bdev;
	size_t                         iter;

	if (spdk_json_decode_object(params, rpc_construct_pvol_decoders,
				    SPDK_COUNTOF(rpc_construct_pvol_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		return;
	}

	/* Fail the command if pvol is already present */
	pvol_bdev_ctxt = check_pvol_present(req.name);
	if (pvol_bdev_ctxt != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "pvol already present");
		free_rpc_construct_pvol(&req);
		return;
	}

	/* Fail the command if input raid level is other than 0 */
	if (req.raid_level != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "invalid raid level");
		free_rpc_construct_pvol(&req);
		return;
	}

	if (spdk_u32_is_pow2(req.strip_size) == false) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "invalid strip size");
		free_rpc_construct_pvol(&req);
		return;
	}

	base_bdevs = calloc(req.base_bdevs.num_base_bdevs, sizeof(struct pvol_base_bdev_config));
	if (base_bdevs == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(ENOMEM));
		free_rpc_construct_pvol(&req);
		return;
	}

	/* Insert the new pvol config entry */
	temp_ptr = realloc(spdk_pvol_config.pvol_bdev_config,
			   sizeof(struct pvol_bdev_config) * (spdk_pvol_config.total_pvol_bdev + 1));
	if (temp_ptr == NULL) {
		free(base_bdevs);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(ENOMEM));
		free_rpc_construct_pvol(&req);
		return;
	}
	spdk_pvol_config.pvol_bdev_config = temp_ptr;
	for (iter = 0; iter < spdk_pvol_config.total_pvol_bdev; iter++) {
		spdk_pvol_config.pvol_bdev_config[iter].pvol_bdev_ctxt->pvol_bdev.pvol_bdev_config =
			&spdk_pvol_config.pvol_bdev_config[iter];
	}
	pvol_bdev_config = &spdk_pvol_config.pvol_bdev_config[spdk_pvol_config.total_pvol_bdev];
	memset(pvol_bdev_config, 0, sizeof(*pvol_bdev_config));
	pvol_bdev_config->name = req.name;
	pvol_bdev_config->strip_size = req.strip_size;
	pvol_bdev_config->num_base_bdevs = req.base_bdevs.num_base_bdevs;
	pvol_bdev_config->raid_level = req.raid_level;
	spdk_pvol_config.total_pvol_bdev++;
	pvol_bdev_config->base_bdev = base_bdevs;
	for (iter = 0; iter < pvol_bdev_config->num_base_bdevs; iter++) {
		pvol_bdev_config->base_bdev[iter].bdev_name = req.base_bdevs.base_bdevs[iter];
	}

	for (iter = 0; iter < pvol_bdev_config->num_base_bdevs; iter++) {
		/* Check if base_bdev exists already, if not fail the command */
		base_bdev = spdk_bdev_get_by_name(req.base_bdevs.base_bdevs[iter]);
		if (base_bdev == NULL) {
			check_and_remove_pvol(&spdk_pvol_config.pvol_bdev_config[spdk_pvol_config.total_pvol_bdev - 1]);
			pvol_bdev_config_cleanup();
			free(base_bdevs);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "base bdev not found");
			free_rpc_construct_pvol(&req);
			return;
		}

		/*
		 * Try to add base_bdev to this pvol, if not able to add fail the
		 * command. This might be because this base_bdev may already be claimed
		 * by some other module
		 */
		if (pvol_bdev_add_base_device(base_bdev)) {
			check_and_remove_pvol(&spdk_pvol_config.pvol_bdev_config[spdk_pvol_config.total_pvol_bdev - 1]);
			pvol_bdev_config_cleanup();
			free(base_bdevs);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "base bdev can't be added");
			free_rpc_construct_pvol(&req);
			return;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		pvol_bdev_config = &spdk_pvol_config.pvol_bdev_config[spdk_pvol_config.total_pvol_bdev - 1];
		for (iter = 0; iter < pvol_bdev_config->num_base_bdevs; iter++) {
			base_bdev = spdk_bdev_get_by_name(pvol_bdev_config->base_bdev[iter].bdev_name);
			if (base_bdev != NULL) {
				pvol_bdev_remove_base_bdev(base_bdev);
			}
		}
		pvol_bdev_config_destroy(pvol_bdev_config);
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("construct_pvol", spdk_rpc_construct_pvol, SPDK_RPC_RUNTIME)

/*
 * Input structure for RPC destroy_pvol
 */
struct rpc_destroy_pvol {
	/* pvol name */
	char *name;
};

/*
 * brief:
 * free_rpc_destroy_pvol function is used to free RPC destroy_pvol related parameters
 * params:
 * req - pointer to RPC request
 * params:
 * none
 */
static void
free_rpc_destroy_pvol(struct rpc_destroy_pvol *req)
{
	free(req->name);
}

/*
 * Decoder object for RPC destroy_pvol
 */
static const struct spdk_json_object_decoder rpc_destroy_pvol_decoders[] = {
	{"name", offsetof(struct rpc_destroy_pvol, name), spdk_json_decode_string},
};

/*
 * brief:
 * Since destroying pvol_bdev is asynchronous operation, so this function is
 * used to check if pvol still exists. If pvol is still there it will create
 * event and check later, otherwise it will proceed with cleanup
 * params:
 * slot_addr - pointer to slot information
 * arg - second arg, not used
 * returns:
 * none
 */
static void
pvol_bdev_config_destroy_check_pvol_bdev_exists(void *arg1, void *arg2)
{
	struct pvol_bdev_config  *pvol_bdev_config = arg1;
	struct spdk_event        *destroy_config_event;

	assert(pvol_bdev_config != NULL);
	if (pvol_bdev_config->pvol_bdev_ctxt != NULL) {
		/* If pvol still exists, schedule event and come back later */
		destroy_config_event = spdk_event_allocate(spdk_env_get_current_core(),
				       pvol_bdev_config_destroy_check_pvol_bdev_exists, pvol_bdev_config, NULL);
		spdk_event_call(destroy_config_event);
		return;
	} else {
		/* If pvol does not exist now, go for pvol config cleanup */
		pvol_bdev_config_destroy(pvol_bdev_config);
	}
}

/*
 * brief:
 * This function will destroy the pvol at given slot
 * params:
 * slot - slot number of pvol config to destroy
 * returns:
 * none
 */
static void
pvol_bdev_config_destroy(struct pvol_bdev_config *pvol_cfg)
{
	void                     *temp_ptr;
	uint8_t                  iter;
	uint8_t                  iter2;
	struct pvol_bdev_config  *pvol_cfg_next;
	struct spdk_event        *destroy_config_event;
	uint8_t                  slot;

	assert(pvol_cfg != NULL);
	if (pvol_cfg->pvol_bdev_ctxt != NULL) {
		/*
		 * If pvol bdev exists for this config, wait for pvol bdev to get
		 * destroyed and come back later
		 */
		destroy_config_event = spdk_event_allocate(spdk_env_get_current_core(),
				       pvol_bdev_config_destroy_check_pvol_bdev_exists, pvol_cfg, NULL);
		spdk_event_call(destroy_config_event);
		return;
	}

	/* Destroy pvol config and cleanup */
	for (iter2 = 0; iter2 < pvol_cfg->num_base_bdevs; iter2++) {
		free(pvol_cfg->base_bdev[iter2].bdev_name);
	}
	free(pvol_cfg->base_bdev);
	free(pvol_cfg->name);
	slot = pvol_cfg - spdk_pvol_config.pvol_bdev_config;
	assert(slot < spdk_pvol_config.total_pvol_bdev);
	if (slot != spdk_pvol_config.total_pvol_bdev - 1) {
		iter = slot;
		while (iter < spdk_pvol_config.total_pvol_bdev - 1) {
			pvol_cfg = &spdk_pvol_config.pvol_bdev_config[iter];
			pvol_cfg_next = &spdk_pvol_config.pvol_bdev_config[iter + 1];
			pvol_cfg->base_bdev = pvol_cfg_next->base_bdev;
			pvol_cfg->pvol_bdev_ctxt = pvol_cfg_next->pvol_bdev_ctxt;
			pvol_cfg->name = pvol_cfg_next->name;
			pvol_cfg->strip_size = pvol_cfg_next->strip_size;
			pvol_cfg->num_base_bdevs = pvol_cfg_next->num_base_bdevs;
			pvol_cfg->raid_level = pvol_cfg_next->raid_level;
			iter++;
		}
	}
	temp_ptr = realloc(spdk_pvol_config.pvol_bdev_config,
			   sizeof(struct pvol_bdev_config) * (spdk_pvol_config.total_pvol_bdev - 1));
	if (temp_ptr != NULL) {
		spdk_pvol_config.pvol_bdev_config = temp_ptr;
		spdk_pvol_config.total_pvol_bdev--;
		for (iter = 0; iter < spdk_pvol_config.total_pvol_bdev; iter++) {
			spdk_pvol_config.pvol_bdev_config[iter].pvol_bdev_ctxt->pvol_bdev.pvol_bdev_config =
				&spdk_pvol_config.pvol_bdev_config[iter];
		}
	} else {
		if (spdk_pvol_config.total_pvol_bdev == 1) {
			spdk_pvol_config.total_pvol_bdev--;
			spdk_pvol_config.pvol_bdev_config = NULL;
		} else {
			SPDK_ERRLOG("Config memory allocation failed\n");
			assert(0);
		}
	}
}

/*
 * brief:
 * spdk_rpc_destroy_pvol function is the RPC for destroy_pvol. It takes pvol
 * name as input and destroy that pvol including freeing the base bdev
 * resources.
 * params:
 * requuest - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
spdk_rpc_destroy_pvol(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_destroy_pvol      req = {};
	struct spdk_json_write_ctx   *w;
	uint32_t                     iter;
	struct pvol_bdev_config      *pvol_bdev_config = NULL;
	struct spdk_bdev             *base_bdev;

	if (spdk_json_decode_object(params, rpc_destroy_pvol_decoders,
				    SPDK_COUNTOF(rpc_destroy_pvol_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		return;
	}

	/* Find pvol config for this pvol */
	for (iter = 0; iter < spdk_pvol_config.total_pvol_bdev; iter++) {
		if (strcmp(spdk_pvol_config.pvol_bdev_config[iter].name, req.name) == 0) {
			pvol_bdev_config = &spdk_pvol_config.pvol_bdev_config[iter];
			break;
		}
	}

	if (pvol_bdev_config == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "pvol name not found");
		free_rpc_destroy_pvol(&req);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		free_rpc_destroy_pvol(&req);
		return;
	}

	/* Remove all the base bdevs from this pvol before destroying the pvol */
	for (iter = 0; iter < pvol_bdev_config->num_base_bdevs; iter++) {
		base_bdev = spdk_bdev_get_by_name(pvol_bdev_config->base_bdev[iter].bdev_name);
		if (base_bdev != NULL) {
			pvol_bdev_remove_base_bdev(base_bdev);
		}
	}

	/*
	 * Call to destroy the pvol, but it will only destroy pvol if underlying
	 * cleanup is done
	 */
	pvol_bdev_config_destroy(pvol_bdev_config);

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_destroy_pvol(&req);
}
SPDK_RPC_REGISTER("destroy_pvol", spdk_rpc_destroy_pvol, SPDK_RPC_RUNTIME)
