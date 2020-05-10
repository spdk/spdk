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

void bdev_rbd_free_config(char **config);
char **bdev_rbd_dup_config(const char *const *config);

typedef void (*spdk_delete_rbd_complete)(void *cb_arg, int bdeverrno);

int bdev_rbd_create(struct spdk_bdev **bdev, const char *name, const char *user_id,
		    const char *pool_name,
		    const char *const *config,
		    const char *rbd_name, uint32_t block_size);
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

#endif /* SPDK_BDEV_RBD_H */
