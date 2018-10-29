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

#ifndef SPDK_BDEV_OCSSD_H
#define SPDK_BDEV_OCSSD_H

#include <spdk/stdinc.h>
#include <spdk/nvme.h>
#include <spdk/bdev_module.h>
#include <spdk/ftl.h>

#define OCSSD_MAX_CONTROLLERS 1024
#define OCSSD_MAX_INSTANCES 16
#define OCSSD_RANGE_MAX_LENGTH 32

struct spdk_bdev;
struct spdk_uuid;

struct ocssd_bdev_info {
	const char		*name;
	struct spdk_uuid	uuid;
};

struct ocssd_bdev_init_opts {
	size_t				count;

	struct spdk_nvme_transport_id	trids[OCSSD_MAX_CONTROLLERS];

	size_t				range_count[OCSSD_MAX_CONTROLLERS];

	struct ftl_punit_range		punit_ranges[OCSSD_MAX_CONTROLLERS][OCSSD_MAX_INSTANCES];

	const char			*names[OCSSD_MAX_CONTROLLERS];

	unsigned int			mode;

	struct spdk_uuid		uuids[OCSSD_MAX_CONTROLLERS];
};

typedef void (*ocssd_bdev_init_fn)(const struct ocssd_bdev_info *, void *, int);

int
bdev_ocssd_parse_punits(struct ftl_punit_range *range_array,
			size_t num_ranges, const char *range_string);
int
bdev_ocssd_init_bdevs(struct ocssd_bdev_init_opts *opts, size_t *count,
		      ocssd_bdev_init_fn cb, void *cb_arg);
void
bdev_ocssd_delete_bdev(const char *name, spdk_bdev_unregister_cb cb_fn, void *cb_arg);

bool
bdev_ocssd_module_init_done(void);

#endif /* SPDK_BDEV_OCSSD_H */
