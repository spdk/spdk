/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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

#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/nvme_zns.h"
#include "spdk/vmd.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/endian.h"
#include "spdk/dif.h"
#include "spdk/util.h"

#include "config-host.h"
#include "fio.h"
#include "optgroup.h"

#ifdef for_each_rw_ddir
#define FIO_HAS_ZBD (FIO_IOOPS_VERSION >= 26)
#else
#define FIO_HAS_ZBD (0)
#endif

/* FreeBSD is missing CLOCK_MONOTONIC_RAW,
 * so alternative is provided. */
#ifndef CLOCK_MONOTONIC_RAW /* Defined in glibc bits/time.h */
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

#define NVME_IO_ALIGN		4096

static bool g_spdk_env_initialized;
static bool g_log_flag_error;
static int g_spdk_enable_sgl = 0;
static uint32_t g_spdk_sge_size = 4096;
static uint32_t g_spdk_bit_bucket_data_len = 0;
static uint32_t g_spdk_pract_flag;
static uint32_t g_spdk_prchk_flags;
static uint32_t g_spdk_md_per_io_size = 4096;
static uint16_t g_spdk_apptag;
static uint16_t g_spdk_apptag_mask;

struct spdk_fio_options {
	void	*pad;	/* off1 used in option descriptions may not be 0 */
	int	enable_wrr;
	int	arbitration_burst;
	int	low_weight;
	int	medium_weight;
	int	high_weight;
	int	wrr_priority;
	int	mem_size;
	int	shm_id;
	int	enable_sgl;
	int	sge_size;
	int	bit_bucket_data_len;
	char	*hostnqn;
	int	pi_act;
	char	*pi_chk;
	int	md_per_io_size;
	int	apptag;
	int	apptag_mask;
	char	*digest_enable;
	int	enable_vmd;
	int	initial_zone_reset;
	int	zone_append;
	int	print_qid_mappings;
	char	*log_flags;
};

struct spdk_fio_request {
	struct io_u		*io;
	/** Offset in current iovec, fio only uses 1 vector */
	uint32_t		iov_offset;

	/** Amount of data used for Bit Bucket SGL */
	uint32_t		bit_bucket_data_len;

	/** Context for NVMe PI */
	struct spdk_dif_ctx	dif_ctx;
	/** Separate metadata buffer pointer */
	void			*md_buf;

	struct spdk_fio_thread	*fio_thread;
	struct spdk_fio_qpair	*fio_qpair;
};

struct spdk_fio_ctrlr {
	struct spdk_nvme_transport_id	tr_id;
	struct spdk_nvme_ctrlr_opts	opts;
	struct spdk_nvme_ctrlr		*ctrlr;
	TAILQ_ENTRY(spdk_fio_ctrlr)	link;
};

static TAILQ_HEAD(, spdk_fio_ctrlr) g_ctrlrs = TAILQ_HEAD_INITIALIZER(g_ctrlrs);
static int g_td_count;
static pthread_t g_ctrlr_thread_id = 0;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_error;

struct spdk_fio_qpair {
	struct fio_file			*f;
	struct spdk_nvme_qpair		*qpair;
	struct spdk_nvme_ns		*ns;
	uint32_t			io_flags;
	bool				zone_append_enabled;
	bool				nvme_pi_enabled;
	/* True for DIF and false for DIX, and this is valid only if nvme_pi_enabled is true. */
	bool				extended_lba;
	/* True for protection info transferred at start of metadata,
	 * false for protection info transferred at end of metadata, and
	 * this is valid only if nvme_pi_enabled is true.
	 */
	bool				md_start;
	TAILQ_ENTRY(spdk_fio_qpair)	link;
	struct spdk_fio_ctrlr		*fio_ctrlr;
};

struct spdk_fio_thread {
	struct thread_data		*td;

	TAILQ_HEAD(, spdk_fio_qpair)	fio_qpair;
	struct spdk_fio_qpair		*fio_qpair_current;	/* the current fio_qpair to be handled. */

	struct io_u			**iocq;		/* io completion queue */
	unsigned int			iocq_count;	/* number of iocq entries filled by last getevents */
	unsigned int			iocq_size;	/* number of iocq entries allocated */
	struct fio_file			*current_f;	/* fio_file given by user */

};

static void *
spdk_fio_poll_ctrlrs(void *arg)
{
	struct spdk_fio_ctrlr *fio_ctrlr;
	int oldstate;
	int rc;

	/* Loop until the thread is cancelled */
	while (true) {
		rc = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);
		if (rc != 0) {
			SPDK_ERRLOG("Unable to set cancel state disabled on g_init_thread (%d): %s\n",
				    rc, spdk_strerror(rc));
		}

		pthread_mutex_lock(&g_mutex);

		TAILQ_FOREACH(fio_ctrlr, &g_ctrlrs, link) {
			spdk_nvme_ctrlr_process_admin_completions(fio_ctrlr->ctrlr);
		}

		pthread_mutex_unlock(&g_mutex);

		rc = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);
		if (rc != 0) {
			SPDK_ERRLOG("Unable to set cancel state enabled on g_init_thread (%d): %s\n",
				    rc, spdk_strerror(rc));
		}

		/* This is a pthread cancellation point and cannot be removed. */
		sleep(1);
	}

	return NULL;
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	struct thread_data	*td = cb_ctx;
	struct spdk_fio_options *fio_options = td->eo;

	if (fio_options->hostnqn) {
		snprintf(opts->hostnqn, sizeof(opts->hostnqn), "%s", fio_options->hostnqn);
	}

	if (fio_options->enable_wrr) {
		opts->arb_mechanism		= SPDK_NVME_CC_AMS_WRR;
		opts->arbitration_burst		= fio_options->arbitration_burst;
		opts->low_priority_weight	= fio_options->low_weight;
		opts->medium_priority_weight	= fio_options->medium_weight;
		opts->high_priority_weight	= fio_options->high_weight;
	}

	if (fio_options->digest_enable) {
		if (strcasecmp(fio_options->digest_enable, "HEADER") == 0) {
			opts->header_digest = true;
		} else if (strcasecmp(fio_options->digest_enable, "DATA") == 0) {
			opts->data_digest = true;
		} else if (strcasecmp(fio_options->digest_enable, "BOTH") == 0) {
			opts->header_digest = true;
			opts->data_digest = true;
		}
	}

	return true;
}

static struct spdk_fio_ctrlr *
get_fio_ctrlr(const struct spdk_nvme_transport_id *trid)
{
	struct spdk_fio_ctrlr	*fio_ctrlr;

	TAILQ_FOREACH(fio_ctrlr, &g_ctrlrs, link) {
		if (spdk_nvme_transport_id_compare(trid, &fio_ctrlr->tr_id) == 0) {
			return fio_ctrlr;
		}
	}

	return NULL;
}

/**
 * Returns the fio_qpair matching the given fio_file and has an associated ns
 */
static struct spdk_fio_qpair *
get_fio_qpair(struct spdk_fio_thread *fio_thread, struct fio_file *f)
{
	struct spdk_fio_qpair	*fio_qpair;

	TAILQ_FOREACH(fio_qpair, &fio_thread->fio_qpair, link) {
		if ((fio_qpair->f == f) && fio_qpair->ns) {
			return fio_qpair;
		}
	}

	return NULL;
}

#if FIO_HAS_ZBD
/**
 * Callback function to use while processing completions until completion-indicator turns non-zero
 */
static void
pcu_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	int *completed = ctx;

	*completed = spdk_nvme_cpl_is_error(cpl) ? -1 : 1;
}

/**
 * Process Completions Until the given 'completed' indicator turns non-zero or an error occurs
 */
static int32_t
pcu(struct spdk_nvme_qpair *qpair, int *completed)
{
	int32_t ret;

	while (!*completed) {
		ret = spdk_nvme_qpair_process_completions(qpair, 1);
		if (ret < 0) {
			log_err("spdk/nvme: process_compl(): ret: %d\n", ret);
			return ret;
		}
	}

	return 0;
}
#endif

static inline uint32_t
_nvme_get_host_buffer_sector_size(struct spdk_nvme_ns *ns, uint32_t io_flags)
{
	bool md_excluded_from_xfer = false;
	uint32_t md_size;
	uint32_t ns_flags;

	ns_flags = spdk_nvme_ns_get_flags(ns);
	md_size = spdk_nvme_ns_get_md_size(ns);

	/* For extended LBA format, if the metadata size is 8 bytes and PRACT is
	 * enabled(controller inserts/strips PI), we should reduce metadata size
	 * from block size.
	 */
	md_excluded_from_xfer = ((io_flags & SPDK_NVME_IO_FLAGS_PRACT) &&
				 (ns_flags & SPDK_NVME_NS_EXTENDED_LBA_SUPPORTED) &&
				 (ns_flags & SPDK_NVME_NS_DPS_PI_SUPPORTED) &&
				 (md_size == 8));

	return md_excluded_from_xfer ? spdk_nvme_ns_get_sector_size(ns) :
	       spdk_nvme_ns_get_extended_sector_size(ns);
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct thread_data	*td = cb_ctx;
	struct spdk_fio_thread	*fio_thread = td->io_ops_data;
	struct spdk_fio_ctrlr	*fio_ctrlr;
	struct spdk_fio_qpair	*fio_qpair;
	struct spdk_nvme_ns	*ns;
	const struct spdk_nvme_ns_data	*nsdata;
	struct fio_file		*f = fio_thread->current_f;
	uint32_t		ns_id;
	char			*p;
	long int		tmp;
	uint32_t		block_size;
	struct spdk_fio_options *fio_options = td->eo;

	p = strstr(f->file_name, "ns=");
	if (p != NULL) {
		tmp = spdk_strtol(p + 3, 10);
		if (tmp <= 0) {
			SPDK_ERRLOG("namespace id should be >=1, but was invalid: %ld\n", tmp);
			g_error = true;
			return;
		}
		ns_id = (uint32_t)tmp;
	} else {
		ns_id = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
		if (ns_id == 0) {
			/* The ctrlr has no active namespaces and we didn't specify any so nothing to do. */
			return;
		}
	}

	pthread_mutex_lock(&g_mutex);
	fio_ctrlr = get_fio_ctrlr(trid);
	/* it is a new ctrlr and needs to be added */
	if (!fio_ctrlr) {
		/* Create an fio_ctrlr and add it to the list */
		fio_ctrlr = calloc(1, sizeof(*fio_ctrlr));
		if (!fio_ctrlr) {
			SPDK_ERRLOG("Cannot allocate space for fio_ctrlr\n");
			g_error = true;
			pthread_mutex_unlock(&g_mutex);
			return;
		}
		fio_ctrlr->opts = *opts;
		fio_ctrlr->ctrlr = ctrlr;
		fio_ctrlr->tr_id = *trid;
		TAILQ_INSERT_TAIL(&g_ctrlrs, fio_ctrlr, link);
	}
	pthread_mutex_unlock(&g_mutex);

	ns = spdk_nvme_ctrlr_get_ns(fio_ctrlr->ctrlr, ns_id);
	if (ns == NULL) {
		SPDK_ERRLOG("Cannot get namespace by ns_id=%d\n", ns_id);
		g_error = true;
		return;
	}

	if (!spdk_nvme_ns_is_active(ns)) {
		SPDK_ERRLOG("Inactive namespace by ns_id=%d\n", ns_id);
		g_error = true;
		return;
	}
	nsdata = spdk_nvme_ns_get_data(ns);

	TAILQ_FOREACH(fio_qpair, &fio_thread->fio_qpair, link) {
		if ((fio_qpair->f == f) ||
		    ((spdk_nvme_transport_id_compare(trid, &fio_qpair->fio_ctrlr->tr_id) == 0) &&
		     (spdk_nvme_ns_get_id(fio_qpair->ns) == ns_id))) {
			/* Not the error case. Avoid duplicated connection */
			return;
		}
	}

	/* create a new qpair */
	fio_qpair = calloc(1, sizeof(*fio_qpair));
	if (!fio_qpair) {
		g_error = true;
		SPDK_ERRLOG("Cannot allocate space for fio_qpair\n");
		return;
	}

	f->engine_data = fio_qpair;
	fio_qpair->ns = ns;
	fio_qpair->f = f;
	fio_qpair->fio_ctrlr = fio_ctrlr;
	TAILQ_INSERT_TAIL(&fio_thread->fio_qpair, fio_qpair, link);

	if (spdk_nvme_ns_get_flags(ns) & SPDK_NVME_NS_DPS_PI_SUPPORTED) {
		assert(spdk_nvme_ns_get_pi_type(ns) != SPDK_NVME_FMT_NVM_PROTECTION_DISABLE);
		fio_qpair->io_flags = g_spdk_pract_flag | g_spdk_prchk_flags;
		fio_qpair->nvme_pi_enabled = true;
		fio_qpair->md_start = nsdata->dps.md_start;
		fio_qpair->extended_lba = spdk_nvme_ns_supports_extended_lba(ns);
		fprintf(stdout, "PI type%u enabled with %s\n", spdk_nvme_ns_get_pi_type(ns),
			fio_qpair->extended_lba ? "extended lba" : "separate metadata");
	}

	block_size = _nvme_get_host_buffer_sector_size(ns, fio_qpair->io_flags);
	if (td->o.bs[DDIR_READ] % block_size != 0 || td->o.bs[DDIR_WRITE] % block_size != 0) {
		if (spdk_nvme_ns_supports_extended_lba(ns)) {
			SPDK_ERRLOG("--bs has to be a multiple of (LBA data size + Metadata size)\n");
		} else {
			SPDK_ERRLOG("--bs has to be a multiple of LBA data size\n");
		}
		g_error = true;
		return;
	}

	if (fio_options->zone_append && spdk_nvme_ns_get_csi(ns) == SPDK_NVME_CSI_ZNS) {
		if (spdk_nvme_ctrlr_get_flags(ctrlr) & SPDK_NVME_CTRLR_ZONE_APPEND_SUPPORTED) {
			SPDK_DEBUGLOG(fio_nvme, "Using zone appends instead of writes on: '%s'\n",
				      f->file_name);
			fio_qpair->zone_append_enabled = true;
		} else {
			SPDK_WARNLOG("Falling back to writes on: '%s' - ns lacks zone append cmd\n",
				     f->file_name);
		}
	}

	if (fio_options->initial_zone_reset == 1 && spdk_nvme_ns_get_csi(ns) == SPDK_NVME_CSI_ZNS) {
#if FIO_HAS_ZBD
		struct spdk_nvme_qpair *tmp_qpair;
		int completed = 0, err;

		/* qpair has not been allocated yet (it gets allocated in spdk_fio_open()).
		 * Create a temporary qpair in order to perform the initial zone reset.
		 */
		assert(!fio_qpair->qpair);

		tmp_qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
		if (!tmp_qpair) {
			SPDK_ERRLOG("Cannot allocate a temporary qpair\n");
			g_error = true;
			return;
		}

		err = spdk_nvme_zns_reset_zone(ns, tmp_qpair, 0x0, true, pcu_cb, &completed);
		if (err || pcu(tmp_qpair, &completed) || completed < 0) {
			log_err("spdk/nvme: warn: initial_zone_reset: err: %d, cpl: %d\n",
				err, completed);
		}

		spdk_nvme_ctrlr_free_io_qpair(tmp_qpair);
#else
		log_err("spdk/nvme: ZBD/ZNS is not supported\n");
#endif
	}

	f->real_file_size = spdk_nvme_ns_get_size(fio_qpair->ns);
	if (f->real_file_size <= 0) {
		g_error = true;
		SPDK_ERRLOG("Cannot get namespace size by ns=%p\n", ns);
		return;
	}

	f->filetype = FIO_TYPE_BLOCK;
	fio_file_set_size_known(f);
}

static void parse_prchk_flags(const char *prchk_str)
{
	if (!prchk_str) {
		return;
	}

	if (strstr(prchk_str, "GUARD") != NULL) {
		g_spdk_prchk_flags = SPDK_NVME_IO_FLAGS_PRCHK_GUARD;
	}
	if (strstr(prchk_str, "REFTAG") != NULL) {
		g_spdk_prchk_flags |= SPDK_NVME_IO_FLAGS_PRCHK_REFTAG;
	}
	if (strstr(prchk_str, "APPTAG") != NULL) {
		g_spdk_prchk_flags |= SPDK_NVME_IO_FLAGS_PRCHK_APPTAG;
	}
}

static void parse_pract_flag(int pract)
{
	if (pract == 1) {
		g_spdk_pract_flag = SPDK_NVME_IO_FLAGS_PRACT;
	} else {
		g_spdk_pract_flag = 0;
	}
}

static bool
fio_redirected_to_dev_null(void)
{
	char path[PATH_MAX] = "";
	ssize_t ret;

	ret = readlink("/proc/self/fd/1", path, sizeof(path));

	if (ret == -1 || strcmp(path, "/dev/null") != 0) {
		return false;
	}

	ret = readlink("/proc/self/fd/2", path, sizeof(path));

	if (ret == -1 || strcmp(path, "/dev/null") != 0) {
		return false;
	}

	return true;
}

/* Called once at initialization. This is responsible for gathering the size of
 * each "file", which in our case are in the form
 * 'key=value [key=value] ... ns=value'
 * For example, For local PCIe NVMe device  - 'trtype=PCIe traddr=0000.04.00.0 ns=1'
 * For remote exported by NVMe-oF target, 'trtype=RDMA adrfam=IPv4 traddr=192.168.100.8 trsvcid=4420 ns=1' */
static int spdk_fio_setup(struct thread_data *td)
{
	struct spdk_fio_thread *fio_thread;
	struct spdk_fio_options *fio_options = td->eo;
	struct spdk_env_opts opts;
	struct fio_file *f;
	char *p;
	int rc = 0;
	struct spdk_nvme_transport_id trid;
	struct spdk_fio_ctrlr *fio_ctrlr;
	char *trid_info;
	unsigned int i;

	/*
	 * If we're running in a daemonized FIO instance, it's possible
	 * fd 1/2 were re-used for something important by FIO. Newer fio
	 * versions are careful to redirect those to /dev/null, but if we're
	 * not, we'll abort early, so we don't accidentally write messages to
	 * an important file, etc.
	 */
	if (is_backend && !fio_redirected_to_dev_null()) {
		char buf[1024];
		snprintf(buf, sizeof(buf),
			 "SPDK FIO plugin is in daemon mode, but stdout/stderr "
			 "aren't redirected to /dev/null. Aborting.");
		fio_server_text_output(FIO_LOG_ERR, buf, sizeof(buf));
		return -1;
	}

	if (!td->o.use_thread) {
		log_err("spdk: must set thread=1 when using spdk plugin\n");
		return 1;
	}

	if (g_log_flag_error) {
		/* The first thread found an error when parsing log flags, so
		 * just return error immediately for all of the other threads.
		 */
		return 1;
	}

	pthread_mutex_lock(&g_mutex);

	fio_thread = calloc(1, sizeof(*fio_thread));
	assert(fio_thread != NULL);

	td->io_ops_data = fio_thread;
	fio_thread->td = td;

	fio_thread->iocq_size = td->o.iodepth;
	fio_thread->iocq = calloc(fio_thread->iocq_size, sizeof(struct io_u *));
	assert(fio_thread->iocq != NULL);

	TAILQ_INIT(&fio_thread->fio_qpair);

	if (!g_spdk_env_initialized) {
		spdk_env_opts_init(&opts);
		opts.name = "fio";
		opts.mem_size = fio_options->mem_size;
		opts.shm_id = fio_options->shm_id;
		g_spdk_enable_sgl = fio_options->enable_sgl;
		g_spdk_sge_size = fio_options->sge_size;
		g_spdk_bit_bucket_data_len = fio_options->bit_bucket_data_len;
		parse_pract_flag(fio_options->pi_act);
		g_spdk_md_per_io_size = spdk_max(fio_options->md_per_io_size, 4096);
		g_spdk_apptag = (uint16_t)fio_options->apptag;
		g_spdk_apptag_mask = (uint16_t)fio_options->apptag_mask;
		parse_prchk_flags(fio_options->pi_chk);
		if (spdk_env_init(&opts) < 0) {
			SPDK_ERRLOG("Unable to initialize SPDK env\n");
			free(fio_thread->iocq);
			free(fio_thread);
			fio_thread = NULL;
			pthread_mutex_unlock(&g_mutex);
			return 1;
		}

		if (fio_options->log_flags) {
			char *tok = strtok(fio_options->log_flags, ",");
			do {
				rc = spdk_log_set_flag(tok);
				if (rc < 0) {
					SPDK_ERRLOG("unknown log flag %s\n", tok);
					g_log_flag_error = true;
					return 1;
				}
			} while ((tok = strtok(NULL, ",")) != NULL);
#ifdef DEBUG
			spdk_log_set_print_level(SPDK_LOG_DEBUG);
#endif
		}

		g_spdk_env_initialized = true;
		spdk_unaffinitize_thread();

		/* Spawn a thread to continue polling the controllers */
		rc = pthread_create(&g_ctrlr_thread_id, NULL, &spdk_fio_poll_ctrlrs, NULL);
		if (rc != 0) {
			SPDK_ERRLOG("Unable to spawn a thread to poll admin queues. They won't be polled.\n");
		}

		if (fio_options->enable_vmd && spdk_vmd_init()) {
			SPDK_ERRLOG("Failed to initialize VMD. Some NVMe devices can be unavailable.\n");
		}
	}
	pthread_mutex_unlock(&g_mutex);

	for_each_file(td, f, i) {
		memset(&trid, 0, sizeof(trid));

		trid.trtype = SPDK_NVME_TRANSPORT_PCIE;

		p = strstr(f->file_name, " ns=");
		if (p != NULL) {
			trid_info = strndup(f->file_name, p - f->file_name);
		} else {
			trid_info = strndup(f->file_name, strlen(f->file_name));
		}

		if (!trid_info) {
			SPDK_ERRLOG("Failed to allocate space for trid_info\n");
			continue;
		}

		rc = spdk_nvme_transport_id_parse(&trid, trid_info);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to parse given str: %s\n", trid_info);
			free(trid_info);
			continue;
		}
		free(trid_info);

		if (trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
			struct spdk_pci_addr pci_addr;
			if (spdk_pci_addr_parse(&pci_addr, trid.traddr) < 0) {
				SPDK_ERRLOG("Invalid traddr=%s\n", trid.traddr);
				continue;
			}
			spdk_pci_addr_fmt(trid.traddr, sizeof(trid.traddr), &pci_addr);
		} else {
			if (trid.subnqn[0] == '\0') {
				snprintf(trid.subnqn, sizeof(trid.subnqn), "%s",
					 SPDK_NVMF_DISCOVERY_NQN);
			}
		}

		fio_thread->current_f = f;

		pthread_mutex_lock(&g_mutex);
		fio_ctrlr = get_fio_ctrlr(&trid);
		pthread_mutex_unlock(&g_mutex);
		if (fio_ctrlr) {
			attach_cb(td, &trid, fio_ctrlr->ctrlr, &fio_ctrlr->opts);
		} else {
			/* Enumerate all of the controllers */
			if (spdk_nvme_probe(&trid, td, probe_cb, attach_cb, NULL) != 0) {
				SPDK_ERRLOG("spdk_nvme_probe() failed\n");
				continue;
			}
		}

		if (g_error) {
			log_err("Failed to initialize spdk fio plugin\n");
			rc = 1;
			break;
		}
	}

	pthread_mutex_lock(&g_mutex);
	g_td_count++;
	pthread_mutex_unlock(&g_mutex);

	return rc;
}

static int spdk_fio_open(struct thread_data *td, struct fio_file *f)
{
	struct spdk_fio_qpair *fio_qpair = f->engine_data;
	struct spdk_fio_ctrlr *fio_ctrlr = fio_qpair->fio_ctrlr;
	struct spdk_fio_options *fio_options = td->eo;
	struct spdk_nvme_io_qpair_opts	qpopts;

	assert(fio_qpair->qpair == NULL);
	spdk_nvme_ctrlr_get_default_io_qpair_opts(fio_ctrlr->ctrlr, &qpopts, sizeof(qpopts));
	qpopts.delay_cmd_submit = true;
	if (fio_options->enable_wrr) {
		qpopts.qprio = fio_options->wrr_priority;
	}

	fio_qpair->qpair = spdk_nvme_ctrlr_alloc_io_qpair(fio_ctrlr->ctrlr, &qpopts, sizeof(qpopts));
	if (!fio_qpair->qpair) {
		SPDK_ERRLOG("Cannot allocate nvme io_qpair any more\n");
		g_error = true;
		free(fio_qpair);
		return -1;
	}

	if (fio_options->print_qid_mappings == 1) {
		log_info("job %s: %s qid %d\n", td->o.name, f->file_name,
			 spdk_nvme_qpair_get_id(fio_qpair->qpair));
	}

	return 0;
}

static int spdk_fio_close(struct thread_data *td, struct fio_file *f)
{
	struct spdk_fio_qpair *fio_qpair = f->engine_data;

	assert(fio_qpair->qpair != NULL);
	spdk_nvme_ctrlr_free_io_qpair(fio_qpair->qpair);
	fio_qpair->qpair = NULL;
	return 0;
}

static int spdk_fio_iomem_alloc(struct thread_data *td, size_t total_mem)
{
	td->orig_buffer = spdk_dma_zmalloc(total_mem, NVME_IO_ALIGN, NULL);
	return td->orig_buffer == NULL;
}

static void spdk_fio_iomem_free(struct thread_data *td)
{
	spdk_dma_free(td->orig_buffer);
}

static int spdk_fio_io_u_init(struct thread_data *td, struct io_u *io_u)
{
	struct spdk_fio_thread	*fio_thread = td->io_ops_data;
	struct spdk_fio_request	*fio_req;

	io_u->engine_data = NULL;

	fio_req = calloc(1, sizeof(*fio_req));
	if (fio_req == NULL) {
		return 1;
	}

	fio_req->md_buf = spdk_dma_zmalloc(g_spdk_md_per_io_size, NVME_IO_ALIGN, NULL);
	if (fio_req->md_buf == NULL) {
		fprintf(stderr, "Allocate %u metadata failed\n", g_spdk_md_per_io_size);
		free(fio_req);
		return 1;
	}

	fio_req->io = io_u;
	fio_req->fio_thread = fio_thread;

	io_u->engine_data = fio_req;

	return 0;
}

static void spdk_fio_io_u_free(struct thread_data *td, struct io_u *io_u)
{
	struct spdk_fio_request *fio_req = io_u->engine_data;

	if (fio_req) {
		assert(fio_req->io == io_u);
		spdk_dma_free(fio_req->md_buf);
		free(fio_req);
		io_u->engine_data = NULL;
	}
}

static inline uint64_t
fio_offset_to_zslba(unsigned long long offset, struct spdk_nvme_ns *ns)
{
	return (offset / spdk_nvme_zns_ns_get_zone_size(ns)) * spdk_nvme_zns_ns_get_zone_size_sectors(ns);
}

static int
fio_extended_lba_setup_pi(struct spdk_fio_qpair *fio_qpair, struct io_u *io_u)
{
	struct spdk_nvme_ns *ns = fio_qpair->ns;
	struct spdk_fio_request *fio_req = io_u->engine_data;
	uint32_t md_size, extended_lba_size, lba_count;
	uint64_t lba;
	struct iovec iov;
	int rc;

	/* Set appmask and apptag when PRACT is enabled */
	if (fio_qpair->io_flags & SPDK_NVME_IO_FLAGS_PRACT) {
		fio_req->dif_ctx.apptag_mask = g_spdk_apptag_mask;
		fio_req->dif_ctx.app_tag = g_spdk_apptag;
		return 0;
	}

	extended_lba_size = spdk_nvme_ns_get_extended_sector_size(ns);
	md_size = spdk_nvme_ns_get_md_size(ns);
	lba = io_u->offset / extended_lba_size;
	lba_count = io_u->xfer_buflen / extended_lba_size;

	rc = spdk_dif_ctx_init(&fio_req->dif_ctx, extended_lba_size, md_size,
			       true, fio_qpair->md_start,
			       (enum spdk_dif_type)spdk_nvme_ns_get_pi_type(ns),
			       fio_qpair->io_flags, lba, g_spdk_apptag_mask, g_spdk_apptag, 0, 0);
	if (rc != 0) {
		fprintf(stderr, "Initialization of DIF context failed\n");
		return rc;
	}

	if (io_u->ddir != DDIR_WRITE) {
		return 0;
	}

	iov.iov_base = io_u->buf;
	iov.iov_len = io_u->xfer_buflen;
	rc = spdk_dif_generate(&iov, 1, lba_count, &fio_req->dif_ctx);
	if (rc != 0) {
		fprintf(stderr, "Generation of DIF failed\n");
	}

	return rc;
}

static int
fio_separate_md_setup_pi(struct spdk_fio_qpair *fio_qpair, struct io_u *io_u)
{
	struct spdk_nvme_ns *ns = fio_qpair->ns;
	struct spdk_fio_request *fio_req = io_u->engine_data;
	uint32_t md_size, block_size, lba_count;
	uint64_t lba;
	struct iovec iov, md_iov;
	int rc;

	/* Set appmask and apptag when PRACT is enabled */
	if (fio_qpair->io_flags & SPDK_NVME_IO_FLAGS_PRACT) {
		fio_req->dif_ctx.apptag_mask = g_spdk_apptag_mask;
		fio_req->dif_ctx.app_tag = g_spdk_apptag;
		return 0;
	}

	block_size = spdk_nvme_ns_get_sector_size(ns);
	md_size = spdk_nvme_ns_get_md_size(ns);
	lba = io_u->offset / block_size;
	lba_count = io_u->xfer_buflen / block_size;

	rc = spdk_dif_ctx_init(&fio_req->dif_ctx, block_size, md_size,
			       false, fio_qpair->md_start,
			       (enum spdk_dif_type)spdk_nvme_ns_get_pi_type(ns),
			       fio_qpair->io_flags, lba, g_spdk_apptag_mask, g_spdk_apptag, 0, 0);
	if (rc != 0) {
		fprintf(stderr, "Initialization of DIF context failed\n");
		return rc;
	}

	if (io_u->ddir != DDIR_WRITE) {
		return 0;
	}

	iov.iov_base = io_u->buf;
	iov.iov_len = io_u->xfer_buflen;
	md_iov.iov_base = fio_req->md_buf;
	md_iov.iov_len = spdk_min(md_size * lba_count, g_spdk_md_per_io_size);
	rc = spdk_dix_generate(&iov, 1, &md_iov, lba_count, &fio_req->dif_ctx);
	if (rc < 0) {
		fprintf(stderr, "Generation of DIX failed\n");
	}

	return rc;
}

static int
fio_extended_lba_verify_pi(struct spdk_fio_qpair *fio_qpair, struct io_u *io_u)
{
	struct spdk_nvme_ns *ns = fio_qpair->ns;
	struct spdk_fio_request *fio_req = io_u->engine_data;
	uint32_t lba_count;
	struct iovec iov;
	struct spdk_dif_error err_blk = {};
	int rc;

	/* Do nothing when PRACT is enabled */
	if (fio_qpair->io_flags & SPDK_NVME_IO_FLAGS_PRACT) {
		return 0;
	}

	iov.iov_base = io_u->buf;
	iov.iov_len = io_u->xfer_buflen;
	lba_count = io_u->xfer_buflen / spdk_nvme_ns_get_extended_sector_size(ns);

	rc = spdk_dif_verify(&iov, 1, lba_count, &fio_req->dif_ctx, &err_blk);
	if (rc != 0) {
		fprintf(stderr, "DIF error detected. type=%d, offset=%" PRIu32 "\n",
			err_blk.err_type, err_blk.err_offset);
	}

	return rc;
}

static int
fio_separate_md_verify_pi(struct spdk_fio_qpair *fio_qpair, struct io_u *io_u)
{
	struct spdk_nvme_ns *ns = fio_qpair->ns;
	struct spdk_fio_request *fio_req = io_u->engine_data;
	uint32_t md_size, lba_count;
	struct iovec iov, md_iov;
	struct spdk_dif_error err_blk = {};
	int rc;

	/* Do nothing when PRACT is enabled */
	if (fio_qpair->io_flags & SPDK_NVME_IO_FLAGS_PRACT) {
		return 0;
	}

	iov.iov_base = io_u->buf;
	iov.iov_len = io_u->xfer_buflen;
	lba_count = io_u->xfer_buflen / spdk_nvme_ns_get_sector_size(ns);
	md_size = spdk_nvme_ns_get_md_size(ns);
	md_iov.iov_base = fio_req->md_buf;
	md_iov.iov_len = spdk_min(md_size * lba_count, g_spdk_md_per_io_size);

	rc = spdk_dix_verify(&iov, 1, &md_iov, lba_count, &fio_req->dif_ctx, &err_blk);
	if (rc != 0) {
		fprintf(stderr, "DIX error detected. type=%d, offset=%" PRIu32 "\n",
			err_blk.err_type, err_blk.err_offset);
	}

	return rc;
}

static void spdk_fio_completion_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_fio_request		*fio_req = ctx;
	struct spdk_fio_thread		*fio_thread = fio_req->fio_thread;
	struct spdk_fio_qpair		*fio_qpair = fio_req->fio_qpair;
	int				rc;

	if (fio_qpair->nvme_pi_enabled && fio_req->io->ddir == DDIR_READ) {
		if (fio_qpair->extended_lba) {
			rc = fio_extended_lba_verify_pi(fio_qpair, fio_req->io);
		} else {
			rc = fio_separate_md_verify_pi(fio_qpair, fio_req->io);
		}
		if (rc != 0) {
			fio_req->io->error = abs(rc);
		}
	}

	if (spdk_nvme_cpl_is_error(cpl)) {
		fio_req->io->error = EIO;
	}

	assert(fio_thread->iocq_count < fio_thread->iocq_size);
	fio_thread->iocq[fio_thread->iocq_count++] = fio_req->io;
}

static void
spdk_nvme_io_reset_sgl(void *ref, uint32_t sgl_offset)
{
	struct spdk_fio_request *fio_req = (struct spdk_fio_request *)ref;

	fio_req->iov_offset = sgl_offset;
	fio_req->bit_bucket_data_len = 0;
}

static int
spdk_nvme_io_next_sge(void *ref, void **address, uint32_t *length)
{
	struct spdk_fio_request *fio_req = (struct spdk_fio_request *)ref;
	struct io_u *io_u = fio_req->io;
	uint32_t iov_len;
	uint32_t bit_bucket_len;

	*address = io_u->buf;

	if (fio_req->iov_offset) {
		assert(fio_req->iov_offset <= io_u->xfer_buflen);
		*address += fio_req->iov_offset;
	}

	iov_len = io_u->xfer_buflen - fio_req->iov_offset;
	if (iov_len > g_spdk_sge_size) {
		iov_len = g_spdk_sge_size;
	}

	if ((fio_req->bit_bucket_data_len < g_spdk_bit_bucket_data_len) && (io_u->ddir == DDIR_READ)) {
		assert(g_spdk_bit_bucket_data_len < io_u->xfer_buflen);
		*address = (void *)UINT64_MAX;
		bit_bucket_len = g_spdk_bit_bucket_data_len - fio_req->bit_bucket_data_len;
		if (iov_len > bit_bucket_len) {
			iov_len = bit_bucket_len;
		}
		fio_req->bit_bucket_data_len += iov_len;
	}

	fio_req->iov_offset += iov_len;
	*length = iov_len;

	return 0;
}

#if FIO_IOOPS_VERSION >= 24
typedef enum fio_q_status fio_q_status_t;
#else
typedef int fio_q_status_t;
#endif

static fio_q_status_t
spdk_fio_queue(struct thread_data *td, struct io_u *io_u)
{
	int rc = 1;
	struct spdk_fio_thread	*fio_thread = td->io_ops_data;
	struct spdk_fio_request	*fio_req = io_u->engine_data;
	struct spdk_fio_qpair	*fio_qpair;
	struct spdk_nvme_ns	*ns = NULL;
	void			*md_buf = NULL;
	struct spdk_dif_ctx	*dif_ctx = &fio_req->dif_ctx;
	uint32_t		block_size;
	uint64_t		lba;
	uint32_t		lba_count;

	fio_qpair = get_fio_qpair(fio_thread, io_u->file);
	if (fio_qpair == NULL) {
		return -ENXIO;
	}
	ns = fio_qpair->ns;

	if (fio_qpair->nvme_pi_enabled && !fio_qpair->extended_lba) {
		md_buf = fio_req->md_buf;
	}
	fio_req->fio_qpair = fio_qpair;

	block_size = _nvme_get_host_buffer_sector_size(ns, fio_qpair->io_flags);
	lba = io_u->offset / block_size;
	lba_count = io_u->xfer_buflen / block_size;

	/* TODO: considering situations that fio will randomize and verify io_u */
	if (fio_qpair->nvme_pi_enabled) {
		if (fio_qpair->extended_lba) {
			rc = fio_extended_lba_setup_pi(fio_qpair, io_u);
		} else {
			rc = fio_separate_md_setup_pi(fio_qpair, io_u);
		}
		if (rc < 0) {
			io_u->error = -rc;
			return FIO_Q_COMPLETED;
		}
	}

	switch (io_u->ddir) {
	case DDIR_READ:
		if (!g_spdk_enable_sgl) {
			rc = spdk_nvme_ns_cmd_read_with_md(ns, fio_qpair->qpair, io_u->buf, md_buf, lba, lba_count,
							   spdk_fio_completion_cb, fio_req,
							   fio_qpair->io_flags, dif_ctx->apptag_mask, dif_ctx->app_tag);
		} else {
			rc = spdk_nvme_ns_cmd_readv_with_md(ns, fio_qpair->qpair, lba,
							    lba_count, spdk_fio_completion_cb, fio_req, fio_qpair->io_flags,
							    spdk_nvme_io_reset_sgl, spdk_nvme_io_next_sge, md_buf,
							    dif_ctx->apptag_mask, dif_ctx->app_tag);
		}
		break;
	case DDIR_WRITE:
		if (!g_spdk_enable_sgl) {
			if (!fio_qpair->zone_append_enabled) {
				rc = spdk_nvme_ns_cmd_write_with_md(ns, fio_qpair->qpair, io_u->buf, md_buf, lba,
								    lba_count,
								    spdk_fio_completion_cb, fio_req,
								    fio_qpair->io_flags, dif_ctx->apptag_mask, dif_ctx->app_tag);
			} else {
				uint64_t zslba = fio_offset_to_zslba(io_u->offset, ns);
				rc = spdk_nvme_zns_zone_append_with_md(ns, fio_qpair->qpair, io_u->buf, md_buf, zslba,
								       lba_count,
								       spdk_fio_completion_cb, fio_req,
								       fio_qpair->io_flags, dif_ctx->apptag_mask, dif_ctx->app_tag);
			}
		} else {
			if (!fio_qpair->zone_append_enabled) {
				rc = spdk_nvme_ns_cmd_writev_with_md(ns, fio_qpair->qpair, lba,
								     lba_count, spdk_fio_completion_cb, fio_req, fio_qpair->io_flags,
								     spdk_nvme_io_reset_sgl, spdk_nvme_io_next_sge, md_buf,
								     dif_ctx->apptag_mask, dif_ctx->app_tag);
			} else {
				uint64_t zslba = fio_offset_to_zslba(io_u->offset, ns);
				rc = spdk_nvme_zns_zone_appendv_with_md(ns, fio_qpair->qpair, zslba,
									lba_count, spdk_fio_completion_cb, fio_req, fio_qpair->io_flags,
									spdk_nvme_io_reset_sgl, spdk_nvme_io_next_sge, md_buf,
									dif_ctx->apptag_mask, dif_ctx->app_tag);
			}
		}
		break;
	default:
		assert(false);
		break;
	}

	/* NVMe read/write functions return -ENOMEM if there are no free requests. */
	if (rc == -ENOMEM) {
		return FIO_Q_BUSY;
	}

	if (rc != 0) {
		io_u->error = abs(rc);
		return FIO_Q_COMPLETED;
	}

	return FIO_Q_QUEUED;
}

static struct io_u *spdk_fio_event(struct thread_data *td, int event)
{
	struct spdk_fio_thread *fio_thread = td->io_ops_data;

	assert(event >= 0);
	assert((unsigned)event < fio_thread->iocq_count);
	return fio_thread->iocq[event];
}

static int spdk_fio_getevents(struct thread_data *td, unsigned int min,
			      unsigned int max, const struct timespec *t)
{
	struct spdk_fio_thread *fio_thread = td->io_ops_data;
	struct spdk_fio_qpair *fio_qpair = NULL;
	struct timespec t0, t1;
	uint64_t timeout = 0;

	if (t) {
		timeout = t->tv_sec * 1000000000L + t->tv_nsec;
		clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
	}

	fio_thread->iocq_count = 0;

	/* fetch the next qpair */
	if (fio_thread->fio_qpair_current) {
		fio_qpair = TAILQ_NEXT(fio_thread->fio_qpair_current, link);
	}

	for (;;) {
		if (fio_qpair == NULL) {
			fio_qpair = TAILQ_FIRST(&fio_thread->fio_qpair);
		}

		while (fio_qpair != NULL) {
			/*
			 * We can be called while spdk_fio_open()s are still
			 * ongoing, in which case, ->qpair can still be NULL.
			 */
			if (fio_qpair->qpair == NULL) {
				fio_qpair = TAILQ_NEXT(fio_qpair, link);
				continue;
			}

			spdk_nvme_qpair_process_completions(fio_qpair->qpair, max - fio_thread->iocq_count);

			if (fio_thread->iocq_count >= min) {
				/* reset the current handling qpair */
				fio_thread->fio_qpair_current = fio_qpair;
				return fio_thread->iocq_count;
			}

			fio_qpair = TAILQ_NEXT(fio_qpair, link);
		}

		if (t) {
			uint64_t elapse;

			clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
			elapse = ((t1.tv_sec - t0.tv_sec) * 1000000000L)
				 + t1.tv_nsec - t0.tv_nsec;
			if (elapse > timeout) {
				break;
			}
		}
	}

	/* reset the current handling qpair */
	fio_thread->fio_qpair_current = fio_qpair;
	return fio_thread->iocq_count;
}

static int spdk_fio_invalidate(struct thread_data *td, struct fio_file *f)
{
	/* TODO: This should probably send a flush to the device, but for now just return successful. */
	return 0;
}

#if FIO_HAS_ZBD
static int
spdk_fio_get_zoned_model(struct thread_data *td, struct fio_file *f, enum zbd_zoned_model *model)
{
	struct spdk_fio_thread *fio_thread = td->io_ops_data;
	struct spdk_fio_qpair *fio_qpair = NULL;
	const struct spdk_nvme_zns_ns_data *zns_data = NULL;

	if (f->filetype != FIO_TYPE_BLOCK) {
		log_info("spdk/nvme: unsupported filetype: %d\n", f->filetype);
		return -EINVAL;
	}

	fio_qpair = get_fio_qpair(fio_thread, f);
	if (!fio_qpair) {
		log_err("spdk/nvme: no ns/qpair or file_name: '%s'\n", f->file_name);
		return -ENODEV;
	}

	switch (spdk_nvme_ns_get_csi(fio_qpair->ns)) {
	case SPDK_NVME_CSI_NVM:
		*model = ZBD_NONE;
		return 0;

	case SPDK_NVME_CSI_KV:
		log_err("spdk/nvme: KV namespace is currently not supported\n");
		return -ENOSYS;

	case SPDK_NVME_CSI_ZNS:
		zns_data = spdk_nvme_zns_ns_get_data(fio_qpair->ns);
		if (!zns_data) {
			log_err("spdk/nvme: file_name: '%s', ZNS is not enabled\n", f->file_name);
			return -EINVAL;
		}

		*model = ZBD_HOST_MANAGED;

		return 0;
	}

	return -EINVAL;
}

static int
spdk_fio_report_zones(struct thread_data *td, struct fio_file *f, uint64_t offset,
		      struct zbd_zone *zbdz, unsigned int nr_zones)
{
	struct spdk_fio_thread *fio_thread = td->io_ops_data;
	struct spdk_fio_qpair *fio_qpair = NULL;
	const struct spdk_nvme_zns_ns_data *zns = NULL;
	struct spdk_nvme_zns_zone_report *report;
	struct spdk_nvme_qpair *tmp_qpair;
	uint32_t report_nzones = 0, report_nzones_max, report_nbytes, mdts_nbytes;
	uint64_t zsze_nbytes, ns_nzones, lba_nbytes;
	int completed = 0, err;

	fio_qpair = get_fio_qpair(fio_thread, f);
	if (!fio_qpair) {
		log_err("spdk/nvme: no ns/qpair or file_name: '%s'\n", f->file_name);
		return -ENODEV;
	}
	zns = spdk_nvme_zns_ns_get_data(fio_qpair->ns);
	if (!zns) {
		log_err("spdk/nvme: file_name: '%s', zns is not enabled\n", f->file_name);
		return -EINVAL;
	}

	/* qpair has not been allocated yet (it gets allocated in spdk_fio_open()).
	 * Create a temporary qpair in order to perform report zones.
	 */
	assert(!fio_qpair->qpair);

	tmp_qpair = spdk_nvme_ctrlr_alloc_io_qpair(fio_qpair->fio_ctrlr->ctrlr, NULL, 0);
	if (!tmp_qpair) {
		log_err("spdk/nvme: cannot allocate a temporary qpair\n");
		return -EIO;
	}

	/** Retrieve device parameters */
	mdts_nbytes = spdk_nvme_ns_get_max_io_xfer_size(fio_qpair->ns);
	lba_nbytes = spdk_nvme_ns_get_sector_size(fio_qpair->ns);
	zsze_nbytes = spdk_nvme_zns_ns_get_zone_size(fio_qpair->ns);
	ns_nzones = spdk_nvme_zns_ns_get_num_zones(fio_qpair->ns);

	/** Allocate report-buffer without exceeding mdts, zbdz-storage, and what is needed */
	report_nzones_max = (mdts_nbytes - sizeof(*report)) / sizeof(report->descs[0]);
	report_nzones_max = spdk_min(spdk_min(report_nzones_max, nr_zones), ns_nzones);
	report_nbytes = sizeof(report->descs[0]) * report_nzones_max + sizeof(*report);
	report = calloc(1, report_nbytes);
	if (!report) {
		log_err("spdk/nvme: failed report_zones(): ENOMEM\n");
		err = -ENOMEM;
		goto exit;
	}

	err = spdk_nvme_zns_report_zones(fio_qpair->ns, tmp_qpair, report, report_nbytes,
					 offset / lba_nbytes, SPDK_NVME_ZRA_LIST_ALL, true, pcu_cb,
					 &completed);
	if (err || pcu(tmp_qpair, &completed) || completed < 0) {
		log_err("spdk/nvme: report_zones(): err: %d, cpl: %d\n", err, completed);
		err = err ? err : -EIO;
		goto exit;
	}
	assert(report->nr_zones <= report_nzones_max);
	report_nzones = report->nr_zones;

	for (uint64_t idx = 0; idx < report->nr_zones; ++idx) {
		struct spdk_nvme_zns_zone_desc *zdesc = &report->descs[idx];

		zbdz[idx].start = zdesc->zslba * lba_nbytes;
		zbdz[idx].len = zsze_nbytes;
		zbdz[idx].capacity = zdesc->zcap * lba_nbytes;
		zbdz[idx].wp = zdesc->wp * lba_nbytes;

		switch (zdesc->zt) {
		case SPDK_NVME_ZONE_TYPE_SEQWR:
			zbdz[idx].type = ZBD_ZONE_TYPE_SWR;
			break;

		default:
			log_err("spdk/nvme: %s: inv. zone-type: 0x%x\n", f->file_name, zdesc->zt);
			err = -EIO;
			goto exit;
		}

		switch (zdesc->zs) {
		case SPDK_NVME_ZONE_STATE_EMPTY:
			zbdz[idx].cond = ZBD_ZONE_COND_EMPTY;
			break;
		case SPDK_NVME_ZONE_STATE_IOPEN:
			zbdz[idx].cond = ZBD_ZONE_COND_IMP_OPEN;
			break;
		case SPDK_NVME_ZONE_STATE_EOPEN:
			zbdz[idx].cond = ZBD_ZONE_COND_EXP_OPEN;
			break;
		case SPDK_NVME_ZONE_STATE_CLOSED:
			zbdz[idx].cond = ZBD_ZONE_COND_CLOSED;
			break;
		case SPDK_NVME_ZONE_STATE_RONLY:
			zbdz[idx].cond = ZBD_ZONE_COND_READONLY;
			break;
		case SPDK_NVME_ZONE_STATE_FULL:
			zbdz[idx].cond = ZBD_ZONE_COND_FULL;
			break;
		case SPDK_NVME_ZONE_STATE_OFFLINE:
			zbdz[idx].cond = ZBD_ZONE_COND_OFFLINE;
			break;

		default:
			log_err("spdk/nvme: %s: inv. zone-state: 0x%x\n", f->file_name, zdesc->zs);
			err = -EIO;
			goto exit;
		}
	}

exit:
	spdk_nvme_ctrlr_free_io_qpair(tmp_qpair);
	free(report);

	return err ? err : (int)report_nzones;
}

static int
spdk_fio_reset_wp(struct thread_data *td, struct fio_file *f, uint64_t offset, uint64_t length)
{
	struct spdk_fio_thread *fio_thread = td->io_ops_data;
	struct spdk_fio_qpair *fio_qpair = NULL;
	const struct spdk_nvme_zns_ns_data *zns = NULL;
	uint64_t zsze_nbytes, lba_nbytes;
	int err = 0;

	fio_qpair = get_fio_qpair(fio_thread, f);
	if (!fio_qpair) {
		log_err("spdk/nvme: no ns/qpair or file_name: '%s'\n", f->file_name);
		return -ENODEV;
	}
	zns = spdk_nvme_zns_ns_get_data(fio_qpair->ns);
	if (!zns) {
		log_err("spdk/nvme: file_name: '%s', zns is not enabled\n", f->file_name);
		return -EINVAL;
	}
	zsze_nbytes = spdk_nvme_zns_ns_get_zone_size(fio_qpair->ns);
	lba_nbytes = spdk_nvme_ns_get_sector_size(fio_qpair->ns);

	/** check the assumption that offset is valid zone-start lba */
	if (offset % zsze_nbytes) {
		log_err("spdk/nvme: offset: %zu is not a valid zslba\n", offset);
		return -EINVAL;
	}

	for (uint64_t cur = offset; cur < offset + length; cur += zsze_nbytes) {
		int completed = 0;

		err = spdk_nvme_zns_reset_zone(fio_qpair->ns, fio_qpair->qpair, cur / lba_nbytes,
					       false, pcu_cb, &completed);
		if (err || pcu(fio_qpair->qpair, &completed) || completed < 0) {
			log_err("spdk/nvme: zns_reset_zone(): err: %d, cpl: %d\n", err, completed);
			err = err ? err : -EIO;
			break;
		}
	}

	return err;
}
#endif

#if FIO_IOOPS_VERSION >= 30
static int spdk_fio_get_max_open_zones(struct thread_data *td, struct fio_file *f,
				       unsigned int *max_open_zones)
{
	struct spdk_fio_thread *fio_thread = td->io_ops_data;
	struct spdk_fio_qpair *fio_qpair = NULL;

	fio_qpair = get_fio_qpair(fio_thread, f);
	if (!fio_qpair) {
		log_err("spdk/nvme: no ns/qpair or file_name: '%s'\n", f->file_name);
		return -ENODEV;
	}

	*max_open_zones = spdk_nvme_zns_ns_get_max_open_zones(fio_qpair->ns);

	return 0;
}
#endif

static void spdk_fio_cleanup(struct thread_data *td)
{
	struct spdk_fio_thread	*fio_thread = td->io_ops_data;
	struct spdk_fio_qpair	*fio_qpair, *fio_qpair_tmp;
	struct spdk_fio_options *fio_options = td->eo;

	TAILQ_FOREACH_SAFE(fio_qpair, &fio_thread->fio_qpair, link, fio_qpair_tmp) {
		TAILQ_REMOVE(&fio_thread->fio_qpair, fio_qpair, link);
		free(fio_qpair);
	}

	free(fio_thread->iocq);
	free(fio_thread);

	pthread_mutex_lock(&g_mutex);
	g_td_count--;
	if (g_td_count == 0) {
		struct spdk_fio_ctrlr	*fio_ctrlr, *fio_ctrlr_tmp;
		struct spdk_nvme_detach_ctx	*detach_ctx = NULL;

		TAILQ_FOREACH_SAFE(fio_ctrlr, &g_ctrlrs, link, fio_ctrlr_tmp) {
			TAILQ_REMOVE(&g_ctrlrs, fio_ctrlr, link);
			spdk_nvme_detach_async(fio_ctrlr->ctrlr, &detach_ctx);
			free(fio_ctrlr);
		}

		if (detach_ctx) {
			spdk_nvme_detach_poll(detach_ctx);
		}

		if (fio_options->enable_vmd) {
			spdk_vmd_fini();
		}
	}
	pthread_mutex_unlock(&g_mutex);
	if (TAILQ_EMPTY(&g_ctrlrs)) {
		if (pthread_cancel(g_ctrlr_thread_id) == 0) {
			pthread_join(g_ctrlr_thread_id, NULL);
		}
	}

	if (g_spdk_env_initialized) {
		spdk_env_fini();
	}
}

/* This function enables addition of SPDK parameters to the fio config
 * Adding new parameters by defining them here and defining a callback
 * function to read the parameter value. */
static struct fio_option options[] = {
	{
		.name           = "enable_wrr",
		.lname          = "Enable weighted round robin (WRR) for IO submission queues",
		.type           = FIO_OPT_INT,
		.off1           = offsetof(struct spdk_fio_options, enable_wrr),
		.def            = "0",
		.help           = "Enable weighted round robin (WRR) for IO submission queues",
		.category       = FIO_OPT_C_ENGINE,
		.group          = FIO_OPT_G_INVALID,
	},
	{
		.name           = "arbitration_burst",
		.lname          = "Arbitration Burst",
		.type           = FIO_OPT_INT,
		.off1           = offsetof(struct spdk_fio_options, arbitration_burst),
		.def            = "0",
		.help           = "Arbitration Burst used for WRR (valid range from 0 - 7)",
		.category       = FIO_OPT_C_ENGINE,
		.group          = FIO_OPT_G_INVALID,
	},
	{
		.name           = "low_weight",
		.lname          = "low_weight for WRR",
		.type           = FIO_OPT_INT,
		.off1           = offsetof(struct spdk_fio_options, low_weight),
		.def            = "0",
		.help           = "low_weight used for WRR (valid range from 0 - 255)",
		.category       = FIO_OPT_C_ENGINE,
		.group          = FIO_OPT_G_INVALID,
	},
	{
		.name           = "medium_weight",
		.lname          = "medium_weight for WRR",
		.type           = FIO_OPT_INT,
		.off1           = offsetof(struct spdk_fio_options, medium_weight),
		.def            = "0",
		.help           = "medium weight used for WRR (valid range from 0 - 255)",
		.category       = FIO_OPT_C_ENGINE,
		.group          = FIO_OPT_G_INVALID,
	},
	{
		.name           = "high_weight",
		.lname          = "high_weight for WRR",
		.type           = FIO_OPT_INT,
		.off1           = offsetof(struct spdk_fio_options, high_weight),
		.def            = "0",
		.help           = "high weight used for WRR (valid range from 0 - 255)",
		.category       = FIO_OPT_C_ENGINE,
		.group          = FIO_OPT_G_INVALID,
	},
	{
		.name           = "wrr_priority",
		.lname          = "priority used for WRR",
		.type           = FIO_OPT_INT,
		.off1           = offsetof(struct spdk_fio_options, wrr_priority),
		.def            = "0",
		.help           = "priority used for WRR (valid range from 0-3)",
		.category       = FIO_OPT_C_ENGINE,
		.group          = FIO_OPT_G_INVALID,
	},
	{
		.name		= "mem_size_mb",
		.lname		= "Memory size in MB",
		.type		= FIO_OPT_INT,
		.off1		= offsetof(struct spdk_fio_options, mem_size),
		.def		= "0",
		.help		= "Memory Size for SPDK (MB)",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "shm_id",
		.lname		= "shared memory ID",
		.type		= FIO_OPT_INT,
		.off1		= offsetof(struct spdk_fio_options, shm_id),
		.def		= "-1",
		.help		= "Shared Memory ID",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "enable_sgl",
		.lname		= "SGL used for I/O commands",
		.type		= FIO_OPT_INT,
		.off1		= offsetof(struct spdk_fio_options, enable_sgl),
		.def		= "0",
		.help		= "SGL Used for I/O Commands (enable_sgl=1 or enable_sgl=0)",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "sge_size",
		.lname		= "SGL size used for I/O commands",
		.type		= FIO_OPT_INT,
		.off1		= offsetof(struct spdk_fio_options, sge_size),
		.def		= "4096",
		.help		= "SGL size in bytes for I/O Commands (default 4096)",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "bit_bucket_data_len",
		.lname		= "Amount of data used for Bit Bucket",
		.type		= FIO_OPT_INT,
		.off1		= offsetof(struct spdk_fio_options, bit_bucket_data_len),
		.def		= "0",
		.help		= "Bit Bucket Data Length for READ commands (disabled by default)",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "hostnqn",
		.lname		= "Host NQN to use when connecting to controllers.",
		.type		= FIO_OPT_STR_STORE,
		.off1		= offsetof(struct spdk_fio_options, hostnqn),
		.help		= "Host NQN",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "pi_act",
		.lname		= "Protection Information Action",
		.type		= FIO_OPT_INT,
		.off1		= offsetof(struct spdk_fio_options, pi_act),
		.def		= "1",
		.help		= "Protection Information Action bit (pi_act=1 or pi_act=0)",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "pi_chk",
		.lname		= "Protection Information Check(GUARD|REFTAG|APPTAG)",
		.type		= FIO_OPT_STR_STORE,
		.off1		= offsetof(struct spdk_fio_options, pi_chk),
		.def		= NULL,
		.help		= "Control of Protection Information Checking (pi_chk=GUARD|REFTAG|APPTAG)",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "md_per_io_size",
		.lname		= "Separate Metadata Buffer Size per I/O",
		.type		= FIO_OPT_INT,
		.off1		= offsetof(struct spdk_fio_options, md_per_io_size),
		.def		= "4096",
		.help		= "Size of separate metadata buffer per I/O (Default: 4096)",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "apptag",
		.lname		= "Application Tag used in Protection Information",
		.type		= FIO_OPT_INT,
		.off1		= offsetof(struct spdk_fio_options, apptag),
		.def		= "0x1234",
		.help		= "Application Tag used in Protection Information field (Default: 0x1234)",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "apptag_mask",
		.lname		= "Application Tag Mask",
		.type		= FIO_OPT_INT,
		.off1		= offsetof(struct spdk_fio_options, apptag_mask),
		.def		= "0xffff",
		.help		= "Application Tag Mask used with Application Tag (Default: 0xffff)",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "digest_enable",
		.lname		= "PDU digest choice for NVMe/TCP Transport(NONE|HEADER|DATA|BOTH)",
		.type		= FIO_OPT_STR_STORE,
		.off1		= offsetof(struct spdk_fio_options, digest_enable),
		.def		= NULL,
		.help		= "Control the NVMe/TCP control(digest_enable=NONE|HEADER|DATA|BOTH)",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "enable_vmd",
		.lname		= "Enable VMD enumeration",
		.type		= FIO_OPT_INT,
		.off1		= offsetof(struct spdk_fio_options, enable_vmd),
		.def		= "0",
		.help		= "Enable VMD enumeration (enable_vmd=1 or enable_vmd=0)",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "initial_zone_reset",
		.lname		= "Reset Zones on initialization",
		.type		= FIO_OPT_INT,
		.off1		= offsetof(struct spdk_fio_options, initial_zone_reset),
		.def		= "0",
		.help		= "Reset Zones on initialization (0=disable, 1=Reset All Zones)",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "zone_append",
		.lname		= "Use zone append instead of write",
		.type		= FIO_OPT_INT,
		.off1		= offsetof(struct spdk_fio_options, zone_append),
		.def		= "0",
		.help		= "Use zone append instead of write (1=zone append, 0=write)",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "print_qid_mappings",
		.lname		= "Print job-to-qid mappings",
		.type		= FIO_OPT_INT,
		.off1		= offsetof(struct spdk_fio_options, print_qid_mappings),
		.def		= "0",
		.help		= "Print job-to-qid mappings (0=disable, 1=enable)",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "log_flags",
		.lname		= "log_flags",
		.type		= FIO_OPT_STR_STORE,
		.off1		= offsetof(struct spdk_fio_options, log_flags),
		.help		= "Enable log flags (comma-separated list)",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= NULL,
	},
};

/* FIO imports this structure using dlsym */
struct ioengine_ops ioengine = {
	.name			= "spdk",
	.version		= FIO_IOOPS_VERSION,
	.queue			= spdk_fio_queue,
	.getevents		= spdk_fio_getevents,
	.event			= spdk_fio_event,
	.cleanup		= spdk_fio_cleanup,
	.open_file		= spdk_fio_open,
	.close_file		= spdk_fio_close,
	.invalidate		= spdk_fio_invalidate,
	.iomem_alloc		= spdk_fio_iomem_alloc,
	.iomem_free		= spdk_fio_iomem_free,
	.setup			= spdk_fio_setup,
	.io_u_init		= spdk_fio_io_u_init,
	.io_u_free		= spdk_fio_io_u_free,
#if FIO_HAS_ZBD
	.get_zoned_model	= spdk_fio_get_zoned_model,
	.report_zones		= spdk_fio_report_zones,
	.reset_wp		= spdk_fio_reset_wp,
#endif
#if FIO_IOOPS_VERSION >= 30
	.get_max_open_zones	= spdk_fio_get_max_open_zones,
#endif
	.flags			= FIO_RAWIO | FIO_NOEXTEND | FIO_NODISKUTIL | FIO_MEMALIGN,
	.options		= options,
	.option_struct_size	= sizeof(struct spdk_fio_options),
};

static void fio_init fio_spdk_register(void)
{
	register_ioengine(&ioengine);
}

static void fio_exit fio_spdk_unregister(void)
{
	unregister_ioengine(&ioengine);
}

SPDK_LOG_REGISTER_COMPONENT(fio_nvme)
