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

#ifndef OCSSD_H
#define OCSSD_H

#include <spdk/stdinc.h>
#include <spdk/nvme.h>
#include <spdk/nvme_ocssd.h>
#include <spdk/uuid.h>

struct ocssd_dev;
struct ocssd_io;

/* Limit thresholds */
enum {
	OCSSD_LIMIT_CRIT,
	OCSSD_LIMIT_HIGH,
	OCSSD_LIMIT_LOW,
	OCSSD_LIMIT_START,
	OCSSD_LIMIT_MAX
};

struct ocssd_limit {
	/* Threshold from which the limiting starts */
	size_t					thld;

	/* Limit percentage */
	size_t					limit;
};

struct ocssd_conf {
	/* Number of reserved addresses not exposed to the user */
	size_t					lba_rsvd;

	/* Write buffer size */
	size_t					rwb_size;

	/* Threshold for opening new band */
	size_t					band_thld;

	/* Trace enabled flag */
	int					trace;

	/* Trace file name */
	const char				*trace_path;

	/* Maximum IO depth per band relocate */
	size_t					max_reloc_qdepth;

	/* Maximum active band relocates */
	size_t					max_active_relocs;

	struct {
		/* Lowest percentage of invalid lbks for a band to be defragged */
		size_t				invld_thld;

		/* User writes limits */
		struct ocssd_limit		limits[OCSSD_LIMIT_MAX];
	} defrag;
};

/* Range of parallel units (inclusive) */
struct ocssd_punit_range {
	unsigned int				begin;
	unsigned int				end;
};

enum ocssd_mode {
	/* Create new device */
	OCSSD_MODE_CREATE		= (1 << 0),
	/* Separated read thread */
	OCSSD_MODE_READ_ISOLATION	= (1 << 1),
};

struct ocssd_nvme_ops {
	int	(*read)(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			void *payload, uint64_t lba, uint32_t lba_count,
			spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags);

	int	(*write)(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			 void *buffer, uint64_t lba, uint32_t lba_count,
			 spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags);

	int	(*read_with_md)(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				void *payload, void *metadata,
				uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
				void *cb_arg, uint32_t io_flags,
				uint16_t apptag_mask, uint16_t apptag);

	int	(*write_with_md)(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				 void *buffer, void *metadata, uint64_t lba,
				 uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				 uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag);

	int	(*vector_reset)(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				uint64_t *lba_list, uint32_t num_lbas,
				struct spdk_ocssd_chunk_information_entry *chunk_info,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg);

	int	(*get_log_page)(struct spdk_nvme_ctrlr *ctrlr,
				uint8_t log_page, uint32_t nsid,
				void *payload, uint32_t payload_size,
				uint64_t offset,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg);

	int	(*get_geometry)(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				void *payload, uint32_t payload_size,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg);

	void	(*register_aer_callback)(struct spdk_nvme_ctrlr *ctrlr,
					 spdk_nvme_aer_cb aer_cb_fn,
					 void *aer_cb_arg);

	int32_t	(*process_completions)(struct spdk_nvme_qpair *qpair,
				       uint32_t max_completions);

	int32_t (*process_admin_completions)(struct spdk_nvme_ctrlr *ctrlr);

	struct spdk_nvme_ns *(*get_ns)(struct spdk_nvme_ctrlr *ctrlr,
				       uint32_t ns_id);

	uint32_t (*get_md_size)(struct spdk_nvme_ns *ns);

	struct spdk_nvme_qpair *(*alloc_io_qpair)(struct spdk_nvme_ctrlr *ctrlr,
			const struct spdk_nvme_io_qpair_opts *opts,
			size_t opts_size);

	int	(*free_io_qpair)(struct spdk_nvme_qpair *qpair);
};

struct ocssd_init_opts {
	/* NVMe controller */
	struct spdk_nvme_ctrlr			*ctrlr;

	/* Controller's transport ID */
	struct spdk_nvme_transport_id		trid;

	/* Device's config */
	struct ocssd_conf			*conf;

	/* Device's name */
	const char				*name;

	/* Parallel unit range */
	struct ocssd_punit_range		range;

	/* Mode flags */
	unsigned int				mode;

	/* Device UUID (valid when restoring device from disk) */
	struct spdk_uuid			uuid;
};

struct ocssd_attrs {
	/* Device's UUID */
	struct spdk_uuid			uuid;

	/* Number of logical blocks */
	uint64_t				lbk_cnt;

	/* Logical block size */
	size_t					lbk_size;
};

typedef void (*ocssd_fn)(void *, int);

struct ocssd_cb {
	/* Callback function */
	ocssd_fn				fn;

	/* Callback's context */
	void					*ctx;
};

int	spdk_ocssd_init(void);
void	spdk_ocssd_deinit(void);
struct ocssd_dev *spdk_ocssd_dev_init(const struct ocssd_init_opts *opts);
void	spdk_ocssd_dev_free(struct ocssd_dev *dev);
void	spdk_ocssd_conf_init_defaults(struct ocssd_conf *conf);
int	spdk_ocssd_dev_get_attrs(const struct ocssd_dev *dev, struct ocssd_attrs *attr);
int	spdk_ocssd_read(struct ocssd_io *io, uint64_t lba, size_t lba_cnt,
			struct iovec *iov, size_t iov_cnt, const struct ocssd_cb *cb);
int	spdk_ocssd_write(struct ocssd_io *io, uint64_t lba, size_t lba_cnt,
			 struct iovec *iov, size_t iov_cnt, const struct ocssd_cb *cb);
int	spdk_ocssd_flush(struct ocssd_dev *dev, const struct ocssd_cb *cb);
struct ocssd_io *spdk_ocssd_io_alloc(struct ocssd_dev *dev);
void	spdk_ocssd_io_free(struct ocssd_io *io);
int	spdk_ocssd_register_nvme_driver(const struct spdk_nvme_transport_id *trid,
					const struct ocssd_nvme_ops *ops);

#endif /* OCSSD_H */
