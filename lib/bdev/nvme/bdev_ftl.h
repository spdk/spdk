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

#ifndef SPDK_BDEV_FTL_H
#define SPDK_BDEV_FTL_H

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/bdev_module.h"
#include "spdk/ftl.h"

#define FTL_MAX_CONTROLLERS	64
#define FTL_MAX_BDEVS		(FTL_MAX_CONTROLLERS * 128)
#define FTL_RANGE_MAX_LENGTH	32

struct spdk_bdev;
struct spdk_uuid;

struct ftl_bdev_info {
	const char		*name;
	struct spdk_uuid	uuid;
};

struct ftl_bdev_init_opts {
	/* NVMe controller's transport ID */
	struct spdk_nvme_transport_id		trid;
	/* Parallel unit range */
	struct spdk_ftl_punit_range		range;
	/* Bdev's name */
	const char				*name;
	/* Write buffer bdev's name */
	const char				*cache_bdev;
	/* Bdev's mode */
	uint32_t				mode;
	/* UUID if device is restored from SSD */
	struct spdk_uuid			uuid;
};

typedef void (*ftl_bdev_init_fn)(const struct ftl_bdev_info *, void *, int);

int	bdev_ftl_parse_punits(struct spdk_ftl_punit_range *range, const char *range_string);
int	bdev_ftl_init_bdev(struct ftl_bdev_init_opts *opts, ftl_bdev_init_fn cb,
			   void *cb_arg);
void	bdev_ftl_delete_bdev(const char *name, spdk_bdev_unregister_cb cb_fn, void *cb_arg);

#endif /* SPDK_BDEV_FTL_H */
