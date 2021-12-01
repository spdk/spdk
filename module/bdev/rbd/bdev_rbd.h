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
};

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
 * \param bdev Pointer to rbd bdev.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Argument to pass to cb_fn.
 */
void bdev_rbd_delete(struct spdk_bdev *bdev, spdk_delete_rbd_complete cb_fn,
		     void *cb_arg);

/**
 * Resize rbd bdev.
 *
 * \param bdev Pointer to rbd bdev.
 * \param new_size_in_mb The new size in MiB for this bdev.
 */
int bdev_rbd_resize(struct spdk_bdev *bdev, const uint64_t new_size_in_mb);

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
