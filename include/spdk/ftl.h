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

#ifndef FTL_H
#define FTL_H

#include <spdk/stdinc.h>
#include <spdk/nvme.h>
#include <spdk/nvme_ocssd.h>
#include <spdk/uuid.h>

struct ftl_dev;
struct ftl_io;

/* Limit thresholds */
enum {
	FTL_LIMIT_CRIT,
	FTL_LIMIT_HIGH,
	FTL_LIMIT_LOW,
	FTL_LIMIT_START,
	FTL_LIMIT_MAX
};

struct ftl_limit {
	/* Threshold from which the limiting starts */
	size_t					thld;

	/* Limit percentage */
	size_t					limit;
};

struct ftl_conf {
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
		struct ftl_limit		limits[FTL_LIMIT_MAX];
	} defrag;
};

/* Range of parallel units (inclusive) */
struct ftl_punit_range {
	unsigned int				begin;
	unsigned int				end;
};

enum ftl_mode {
	/* Create new device */
	FTL_MODE_CREATE		= (1 << 0),
};

struct ftl_nvme_ops {
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

struct ftl_init_opts {
	/* NVMe controller */
	struct spdk_nvme_ctrlr			*ctrlr;
	/* Controller's transport ID */
	struct spdk_nvme_transport_id		trid;

	/* Thread responsible for core tasks execution */
	struct spdk_thread			*core_thread;
	/* Thread responsible for read requets */
	struct spdk_thread			*read_thread;

	/* Device's config */
	struct ftl_conf				*conf;
	/* Device's name */
	const char				*name;
	/* Parallel unit range */
	struct ftl_punit_range			range;

	/* Mode flags */
	unsigned int				mode;

	/* Device UUID (valid when restoring device from disk) */
	struct spdk_uuid			uuid;
};

struct ftl_attrs {
	/* Device's UUID */
	struct spdk_uuid			uuid;

	/* Parallel unit range */
	struct ftl_punit_range			range;

	/* Number of logical blocks */
	uint64_t				lbk_cnt;

	/* Logical block size */
	size_t					lbk_size;
};

typedef void (*ftl_fn)(void *, int);
typedef void (*ftl_init_fn)(struct ftl_dev *, void *, int);

/**
 * Initialize the FTL module.
 * Covers initialization of ANM thread.
 *
 * \param anm_thread thread to use to poll for ANM events
 *
 * \return 0 if successfully initialized, negative values if resources could not
 * be allocated.
 */
int	spdk_ftl_init(struct spdk_thread *anm_thread);

/**
 * Deinitialize the FTL module.
 * All FTL devices have to be unregistered prior to calling this function.
 */
void	spdk_ftl_deinit(void);

/**
 * Initialize the FTL on given NVMe device and parallel unit range. If the callback is called with
 * an error, the pointer returned by this functions is no longer valid.
 *
 * Covers the following:
 * - initialize and register NVMe ctrlr,
 * - retrieve geometry and check if the device has proper configuration,
 * - allocate buffers and resources,
 * - initialize internal structures,
 * - initialize internal thread(s),
 * - restore or create L2P table.
 *
 * \param opts Configuration for new device
 * \param cb callback function to call when the device is created
 * \param cb_arg callback's argument
 *
 * \return 0 if initialization was started successfully, negative errno otherwise.
 */
int	spdk_ftl_dev_init(const struct ftl_init_opts *opts, ftl_init_fn cb, void *cb_arg);

/**
 * Deinitialize and free given device.
 *
 * \param dev device
 * \param cb callback function to call when the device is freed
 * \param cb_arg callback's argument
 *
 * \return 0 if successfully scheduled free, negitive errno otherwise.
 */
int	spdk_ftl_dev_free(struct ftl_dev *dev, ftl_fn cb, void *cb_arg);

/**
 * Initialize FTL configuration structure with default values.
 *
 * \param conf FTL configuration to initialize
 */
void	spdk_ftl_conf_init_defaults(struct ftl_conf *conf);

/**
 * Retrieve device’s attributes.
 *
 * \param dev device
 * \param attr Attribute structure to fill
 *
 * \return 0 if successfully initialized, negated EINVAL otherwise.
 */
int	spdk_ftl_dev_get_attrs(const struct ftl_dev *dev, struct ftl_attrs *attr);

/**
 * Submits a read to the specified device.
 *
 * \param io I/O handle
 * \param lba Starting LBA to read the data
 * \param lba_cnt Number of sectors to read
 * \param iov Single IO vector or pointer to IO vector table
 * \param iov_cnt Number of IO vectors
 * \param cb_fn Callback function to invoke when the I/O is completed
 * \param cb_arg Argument to pass to the callback function
 *
 * \return 0 if successfully submitted, negated EINVAL otherwise.
 */
int	spdk_ftl_read(struct ftl_io *io, uint64_t lba, size_t lba_cnt,
		      struct iovec *iov, size_t iov_cnt, const ftl_fn cb_fn, void *cb_arg);

/**
 * Submits a write to the specified device.
 *
 * \param io I/O handle
 * \param lba Starting LBA to write the data
 * \param lba_cnt Number of sectors to write
 * \param iov Single IO vector or pointer to IO vector table
 * \param iov_cnt Number of IO vectors
 * \param cb_fn Callback function to invoke when the I/O is completed
 * \param cb_arg Argument to pass to the callback function
 *
 * \return 0 if successfully submitted, negative values otherwise.
 */
int
spdk_ftl_write(struct ftl_io *io, uint64_t lba, size_t lba_cnt,
	       struct iovec *iov, size_t iov_cnt, const ftl_fn cb_fn, void *cb_arg);

/**
 * Submits a flush request to the specified device.
 *
 * \param dev device
 * \param cb_fn Callback function to invoke when all prior IOs have been completed
 * \param cb_arg Argument to pass to the callback function
 *
 * \return 0 if successfully submitted, negated EINVAL or ENOMEM otherwise.
 */
int	spdk_ftl_flush(struct ftl_dev *dev, const ftl_fn cb_fn, void *cb_arg);

/**
 * Allocate FTL I/O structure for given device.
 *
 * \param dev device
 *
 * \return Pointer to FTL I/O handle, NULL otherwise.
 */
struct ftl_io *spdk_ftl_io_alloc(struct ftl_dev *dev);

/**
 * Free FTL I/O handle.
 *
 * \param io FTL I/O handle to free
 */
void	spdk_ftl_io_free(struct ftl_io *io);

/**
 * Initialize and register NVMe driver for a given FTL device.
 *
 * \param trid Transport ID for NVMe device
 * \param ops Device configuration
 *
 * \return 0 if successfully registered, negative values otherwise.
 */
int	spdk_ftl_register_nvme_driver(const struct spdk_nvme_transport_id *trid,
				      const struct ftl_nvme_ops *ops);

#endif /* FTL_H */
