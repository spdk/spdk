/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_RBD_H
#define SPDK_BDEV_RBD_H

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/rpc.h"

struct cluster_register_info {
	char *name;
	char *user_id;
	char **config_param;
	char *config_file;
	char *key_file;
	char *core_mask;
};

void dump_cluster_nonce(struct spdk_json_write_ctx *w, const char *name);
void bdev_rbd_free_config(char **config);
char **bdev_rbd_dup_config(const char *const *config);

typedef void (*spdk_delete_rbd_complete)(void *cb_arg, int bdeverrno);

int bdev_rbd_create(struct spdk_bdev **bdev, const char *name, const char *user_id,
		    const char *pool_name,
		    const char *const *config,
		    const char *rbd_name, uint32_t block_size, const char *cluster_name, const struct spdk_uuid *uuid);
/**
 * Delete rbd bdev.
 *
 * \param name Name of rbd bdev.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Argument to pass to cb_fn.
 */
void bdev_rbd_delete(const char *name, spdk_delete_rbd_complete cb_fn,
		     void *cb_arg);

/**
 * Resize rbd bdev.
 *
 * \param bdev Name of rbd bdev.
 * \param new_size_in_mb The new size in MiB for this bdev.
 */
int bdev_rbd_resize(const char *name, const uint64_t new_size_in_mb);

/**
 * Create a Rados cluster.
 *
 * \param info the info to register the Rados cluster object
 */
int bdev_rbd_register_cluster(struct cluster_register_info *info);

/**
 * Delete a registered cluster.
 *
 * \param name the name of the cluster to be deleted.
 */
int bdev_rbd_unregister_cluster(const char *name);

/**
 * Show the cluster info of a given name. If given name is empty,
 * the info of every registered cluster name will be showed.
 *
 * \param request the json request.
 * \param name the name of the cluster.
 */
int bdev_rbd_get_clusters_info(struct spdk_jsonrpc_request *request, const char *name);

#endif /* SPDK_BDEV_RBD_H */
