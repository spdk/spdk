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

#ifndef SPDK_COMMON_BDEV_NVME_H
#define SPDK_COMMON_BDEV_NVME_H

#include "spdk/nvme.h"
#include "spdk/bdev_module.h"

TAILQ_HEAD(nvme_bdev_ctrlrs, nvme_bdev_ctrlr);
TAILQ_HEAD(nvme_probe_skip_entries, nvme_probe_skip_entry);
extern struct nvme_bdev_ctrlrs g_nvme_bdev_ctrlrs;
extern struct nvme_probe_skip_entries g_skipped_nvme_ctrlrs;
extern pthread_mutex_t g_bdev_nvme_mutex;

#define NVME_MAX_CONTROLLERS 1024

struct nvme_bdev_ctrlr {
	/**
	 * points to pinned, physically contiguous memory region;
	 * contains 4KB IDENTIFY structure for controller which is
	 *  target for CONTROLLER IDENTIFY command during initialization
	 */
	struct spdk_nvme_ctrlr		*ctrlr;
	struct spdk_nvme_transport_id	trid;
	char				*name;
	int				ref;
	bool				destruct;
	/**
	 * PI check flags. This flags is set to NVMe controllers created only
	 * through construct_nvme_bdev RPC or .INI config file. Hot added
	 * NVMe controllers are not included.
	 */
	uint32_t			prchk_flags;
	uint32_t			num_ns;
	/** Array of bdevs indexed by nsid - 1 */
	struct nvme_bdev		*bdevs;

	struct spdk_poller		*adminq_timer_poller;

	void (*remove_fn)(struct nvme_bdev_ctrlr *);

	/** linked list pointer for device list */
	TAILQ_ENTRY(nvme_bdev_ctrlr)	tailq;
};

struct nvme_bdev {
	struct spdk_bdev	disk;
	struct nvme_bdev_ctrlr	*nvme_bdev_ctrlr;
	uint32_t		id;
	bool			active;
	struct spdk_nvme_ns	*ns;
};

#define NVME_MAX_BDEVS_PER_RPC 128

struct nvme_bdev_info {
	const char *names[NVME_MAX_BDEVS_PER_RPC];
	size_t count;
};

struct nvme_probe_skip_entry {
	struct spdk_nvme_transport_id		trid;
	TAILQ_ENTRY(nvme_probe_skip_entry)	tailq;
};

struct nvme_probe_ctx {
	size_t count;
	struct spdk_nvme_transport_id trids[NVME_MAX_CONTROLLERS];
	struct spdk_nvme_host_id hostids[NVME_MAX_CONTROLLERS];
	const char *names[NVME_MAX_CONTROLLERS];
	uint32_t prchk_flags[NVME_MAX_CONTROLLERS];
	const char *hostnqn;
	int (*create_ctrlr_fn)(struct spdk_nvme_ctrlr *, const char *,
			       const struct spdk_nvme_transport_id *, uint32_t);
};

struct nvme_bdev_construct_opts {
	/* NVMe controller's transport ID */
	struct spdk_nvme_transport_id		trid;
	/* Bdev's name */
	const char				*name;
	/* Transport address to be used by the host when connecting to the NVMe-oF endpoint */
	struct spdk_nvme_host_id		hostid;
	/* Host NQN */
	const char				*hostnqn;
	uint32_t				prchk_flags;
};

struct rpc_construct_nvme {
	char *name;
	char *trtype;
	char *adrfam;
	char *traddr;
	char *trsvcid;
	char *subnqn;
	char *hostnqn;
	char *hostaddr;
	char *hostsvcid;
	char *mode;
	bool prchk_reftag;
	bool prchk_guard;
};

typedef void (*spdk_rpc_construct_bdev_cb_fn)(struct nvme_bdev_info *bdev_info, void *ctx,
		int status);

struct nvme_bdev_ctrlr *nvme_bdev_ctrlr_get(const struct spdk_nvme_transport_id *trid);
struct nvme_bdev_ctrlr *nvme_bdev_ctrlr_get_by_name(const char *name);
struct nvme_bdev_ctrlr *nvme_bdev_first_ctrlr(void);
struct nvme_bdev_ctrlr *nvme_bdev_next_ctrlr(struct nvme_bdev_ctrlr *prev);

void nvme_bdev_dump_trid_json(struct spdk_nvme_transport_id *trid,
			      struct spdk_json_write_ctx *w);

/**
 * Delete NVMe controller with all bdevs on top of it.
 * Requires to pass name of NVMe controller.
 *
 * \param name NVMe controller name
 * \return zero on success, -EINVAL on wrong parameters or -ENODEV if controller is not found
 */
int spdk_bdev_nvme_delete(const char *name);
void spdk_bdev_nvme_delete_cb(void *cb_ctx, struct spdk_nvme_ctrlr *ctrlr);
void spdk_bdev_nvme_ctrlr_destruct(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr);
bool spdk_bdev_nvme_probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
			     struct spdk_nvme_ctrlr_opts *opts);
void spdk_bdev_nvme_attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
			      struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts);

#endif /* SPDK_COMMON_BDEV_NVME_H */
