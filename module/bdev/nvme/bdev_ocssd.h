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

#include "spdk/stdinc.h"
#include "common.h"

struct bdev_ocssd_range {
	uint64_t begin;
	uint64_t end;
};

typedef void (*bdev_ocssd_create_cb)(const char *bdev_name, int status, void *ctx);
typedef void (*bdev_ocssd_delete_cb)(int status, void *ctx);

void bdev_ocssd_create_bdev(const char *ctrlr_name, const char *bdev_name, uint32_t nsid,
			    const struct bdev_ocssd_range *range,
			    bdev_ocssd_create_cb cb_fn, void *cb_arg);
void bdev_ocssd_delete_bdev(const char *bdev_name, bdev_ocssd_delete_cb cb_fn, void *cb_arg);

void bdev_ocssd_populate_namespace(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr,
				   struct nvme_bdev_ns *nvme_ns,
				   struct nvme_async_probe_ctx *ctx);
void bdev_ocssd_depopulate_namespace(struct nvme_bdev_ns *ns);
void bdev_ocssd_namespace_config_json(struct spdk_json_write_ctx *w, struct nvme_bdev_ns *ns);

int bdev_ocssd_create_io_channel(struct nvme_io_channel *ioch);
void bdev_ocssd_destroy_io_channel(struct nvme_io_channel *ioch);

int bdev_ocssd_init_ctrlr(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr);
void bdev_ocssd_fini_ctrlr(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr);

void bdev_ocssd_handle_chunk_notification(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr);

#endif /* SPDK_BDEV_OCSSD_H */
