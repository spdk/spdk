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

#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/endian.h"
#include "spdk/crc16.h"

#include "config-host.h"
#include "fio.h"
#include "optgroup.h"

#define NVME_IO_ALIGN		4096
#define FIO_NVME_PI_APPTAG	0x1234

static bool spdk_env_initialized;
static int spdk_enable_sgl = 0;
static uint32_t spdk_pract_flag;
static uint32_t spdk_prchk_flags;

struct spdk_fio_options {
	void	*pad;	/* off1 used in option descriptions may not be 0 */
	int	mem_size;
	int	shm_id;
	int	enable_sgl;
	char	*hostnqn;
	int	pi_act;
	char	*pi_chk;
};

struct spdk_fio_request {
	struct io_u		*io;
	/** Offset in current iovec, fio only uses 1 vector */
	uint32_t iov_offset;

	/** Application tag and its mask for NVMe PI */
	uint16_t		appmask;
	uint16_t		apptag;

	struct spdk_fio_thread	*fio_thread;
};

struct spdk_fio_ctrlr {
	struct spdk_nvme_transport_id	tr_id;
	struct spdk_nvme_ctrlr_opts	opts;
	struct spdk_nvme_ctrlr		*ctrlr;
	struct spdk_fio_ctrlr		*next;
};

static struct spdk_fio_ctrlr *ctrlr_g;
static int td_count;
static pthread_t g_ctrlr_thread_id = 0;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_error;

struct spdk_fio_qpair {
	struct fio_file		*f;
	struct spdk_nvme_qpair	*qpair;
	struct spdk_nvme_ns	*ns;
	uint32_t		io_flags;
	bool			do_nvme_pi;
	struct spdk_fio_qpair	*next;
	struct spdk_fio_ctrlr	*fio_ctrlr;
};

struct spdk_fio_thread {
	struct thread_data	*td;

	struct spdk_fio_qpair	*fio_qpair;
	struct spdk_fio_qpair	*fio_qpair_current; // the current fio_qpair to be handled.

	struct io_u		**iocq;	// io completion queue
	unsigned int		iocq_count;	// number of iocq entries filled by last getevents
	unsigned int		iocq_size;	// number of iocq entries allocated
	struct fio_file		*current_f;   // fio_file given by user

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

		pthread_mutex_lock(&mutex);
		fio_ctrlr = ctrlr_g;

		while (fio_ctrlr) {
			spdk_nvme_ctrlr_process_admin_completions(fio_ctrlr->ctrlr);
			fio_ctrlr = fio_ctrlr->next;
		}

		pthread_mutex_unlock(&mutex);

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

	return true;
}

static struct spdk_fio_ctrlr *
get_fio_ctrlr(const struct spdk_nvme_transport_id *trid)
{
	struct spdk_fio_ctrlr	*fio_ctrlr = ctrlr_g;
	while (fio_ctrlr) {
		if (spdk_nvme_transport_id_compare(trid, &fio_ctrlr->tr_id) == 0) {
			return fio_ctrlr;
		}

		fio_ctrlr = fio_ctrlr->next;
	}

	return NULL;
}

static bool
fio_do_nvme_pi_check(struct spdk_fio_qpair *fio_qpair)
{
	struct spdk_nvme_ns	*ns = NULL;
	const struct spdk_nvme_ns_data *nsdata;

	ns = fio_qpair->ns;
	nsdata = spdk_nvme_ns_get_data(ns);

	if (!spdk_nvme_ns_supports_extended_lba(ns)) {
		return false;
	}

	if (spdk_nvme_ns_get_pi_type(ns) ==
	    SPDK_NVME_FMT_NVM_PROTECTION_DISABLE) {
		return false;
	}

	/* PI locates at the first 8 bytes of metadata,
	 * doesn't support now
	 */
	if (nsdata->dps.md_start) {
		return false;
	}

	/* Controller performs PI setup and check */
	if (fio_qpair->io_flags & SPDK_NVME_IO_FLAGS_PRACT) {
		return false;
	}

	/* Type3 don't support REFTAG */
	if (spdk_nvme_ns_get_pi_type(ns) ==
	    SPDK_NVME_FMT_NVM_PROTECTION_TYPE3) {
		return false;
	}

	return true;
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
	struct fio_file		*f = fio_thread->current_f;
	uint32_t		ns_id;
	char			*p;

	p = strstr(f->file_name, "ns=");
	assert(p != NULL);
	ns_id = atoi(p + 3);
	if (!ns_id) {
		SPDK_ERRLOG("namespace id should be >=1, but current value=0\n");
		g_error = true;
		return;
	}

	fio_ctrlr = get_fio_ctrlr(trid);
	/* it is a new ctrlr and needs to be added */
	if (!fio_ctrlr) {
		/* Create an fio_ctrlr and add it to the list */
		fio_ctrlr = calloc(1, sizeof(*fio_ctrlr));
		if (!fio_ctrlr) {
			SPDK_ERRLOG("Cannot allocate space for fio_ctrlr\n");
			g_error = true;
			return;
		}
		fio_ctrlr->opts = *opts;
		fio_ctrlr->ctrlr = ctrlr;
		fio_ctrlr->tr_id = *trid;
		fio_ctrlr->next = ctrlr_g;
		ctrlr_g = fio_ctrlr;
	}

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

	fio_qpair = fio_thread->fio_qpair;
	while (fio_qpair != NULL) {
		if ((fio_qpair->f == f) ||
		    ((spdk_nvme_transport_id_compare(trid, &fio_qpair->fio_ctrlr->tr_id) == 0) &&
		     (spdk_nvme_ns_get_id(fio_qpair->ns) == ns_id))) {
			/* Not the error case. Avoid duplicated connection */
			return;
		}
		fio_qpair = fio_qpair->next;
	}

	/* create a new qpair */
	fio_qpair = calloc(1, sizeof(*fio_qpair));
	if (!fio_qpair) {
		g_error = true;
		SPDK_ERRLOG("Cannot allocate space for fio_qpair\n");
		return;
	}

	fio_qpair->qpair = spdk_nvme_ctrlr_alloc_io_qpair(fio_ctrlr->ctrlr, NULL, 0);
	if (!fio_qpair->qpair) {
		SPDK_ERRLOG("Cannot allocate nvme io_qpair any more\n");
		g_error = true;
		free(fio_qpair);
		return;
	}

	fio_qpair->ns = ns;
	fio_qpair->f = f;
	fio_qpair->fio_ctrlr = fio_ctrlr;
	fio_qpair->next = fio_thread->fio_qpair;
	fio_thread->fio_qpair = fio_qpair;

	if (spdk_nvme_ns_get_flags(ns) & SPDK_NVME_NS_DPS_PI_SUPPORTED) {
		fio_qpair->io_flags = spdk_pract_flag | spdk_prchk_flags;
	}

	fio_qpair->do_nvme_pi = fio_do_nvme_pi_check(fio_qpair);

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
		spdk_prchk_flags = SPDK_NVME_IO_FLAGS_PRCHK_GUARD;
	}
	if (strstr(prchk_str, "REFTAG") != NULL) {
		spdk_prchk_flags |= SPDK_NVME_IO_FLAGS_PRCHK_REFTAG;
	}
	if (strstr(prchk_str, "APPTAG") != NULL) {
		spdk_prchk_flags |= SPDK_NVME_IO_FLAGS_PRCHK_APPTAG;
	}
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

	if (!td->o.use_thread) {
		log_err("spdk: must set thread=1 when using spdk plugin\n");
		return 1;
	}

	pthread_mutex_lock(&mutex);

	fio_thread = calloc(1, sizeof(*fio_thread));
	assert(fio_thread != NULL);

	td->io_ops_data = fio_thread;
	fio_thread->td = td;

	fio_thread->iocq_size = td->o.iodepth;
	fio_thread->iocq = calloc(fio_thread->iocq_size, sizeof(struct io_u *));
	assert(fio_thread->iocq != NULL);

	if (!spdk_env_initialized) {
		spdk_env_opts_init(&opts);
		opts.name = "fio";
		opts.mem_size = fio_options->mem_size;
		opts.shm_id = fio_options->shm_id;
		spdk_enable_sgl = fio_options->enable_sgl;
		spdk_pract_flag = fio_options->pi_act;
		parse_prchk_flags(fio_options->pi_chk);
		if (spdk_env_init(&opts) < 0) {
			SPDK_ERRLOG("Unable to initialize SPDK env\n");
			free(fio_thread->iocq);
			free(fio_thread);
			fio_thread = NULL;
			pthread_mutex_unlock(&mutex);
			return 1;
		}
		spdk_env_initialized = true;
		spdk_unaffinitize_thread();

		/* Spawn a thread to continue polling the controllers */
		rc = pthread_create(&g_ctrlr_thread_id, NULL, &spdk_fio_poll_ctrlrs, NULL);
		if (rc != 0) {
			SPDK_ERRLOG("Unable to spawn a thread to poll admin queues. They won't be polled.\n");
		}
	}

	for_each_file(td, f, i) {
		memset(&trid, 0, sizeof(trid));

		trid.trtype = SPDK_NVME_TRANSPORT_PCIE;

		p = strstr(f->file_name, " ns=");
		if (p == NULL) {
			SPDK_ERRLOG("Failed to find namespace 'ns=X'\n");
			continue;
		}

		trid_info = strndup(f->file_name, p - f->file_name);
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

		fio_ctrlr = get_fio_ctrlr(&trid);
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

	td_count++;

	pthread_mutex_unlock(&mutex);

	return rc;
}

static int spdk_fio_open(struct thread_data *td, struct fio_file *f)
{
	return 0;
}

static int spdk_fio_close(struct thread_data *td, struct fio_file *f)
{
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

	fio_req = calloc(1, sizeof(*fio_req));
	if (fio_req == NULL) {
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
		free(fio_req);
		io_u->engine_data = NULL;
	}
}

static void
fio_extended_lba_setup_pi(struct spdk_fio_qpair *fio_qpair, struct io_u *io_u)
{
	struct spdk_nvme_ns	*ns = NULL;
	struct spdk_fio_request *fio_req = io_u->engine_data;
	struct spdk_nvme_protection_info *pi;
	uint16_t crc16;
	uint32_t i, md_size, sector_size, pi_offset, extended_lba_size, lba_count;
	uint64_t lba;

	ns = fio_qpair->ns;

	sector_size = spdk_nvme_ns_get_sector_size(ns);
	md_size = spdk_nvme_ns_get_md_size(ns);
	extended_lba_size = sector_size + md_size;
	lba = io_u->offset / extended_lba_size;
	lba_count = io_u->xfer_buflen / extended_lba_size;

	if (fio_qpair->io_flags & SPDK_NVME_IO_FLAGS_PRCHK_APPTAG) {
		/* Let's use number of lbas for application tag */
		fio_req->appmask = 0xffff;
		fio_req->apptag = FIO_NVME_PI_APPTAG;
	}

	for (i = 0; i < lba_count; i++) {
		pi_offset = (extended_lba_size * (i + 1)) - 8;
		pi = (struct spdk_nvme_protection_info *)(io_u->buf + pi_offset);
		memset(pi, 0, sizeof(*pi));

		if (io_u->ddir == DDIR_WRITE) {
			if (fio_qpair->io_flags & SPDK_NVME_IO_FLAGS_PRCHK_GUARD) {
				/* CRC buffer should not include PI */
				crc16 = spdk_crc16_t10dif(io_u->buf + extended_lba_size * i,
							  extended_lba_size - 8);
				to_be16(&pi->guard, crc16);
			}
			if (fio_qpair->io_flags & SPDK_NVME_IO_FLAGS_PRCHK_APPTAG) {
				/* Let's use number of lbas for application tag */
				to_be16(&pi->app_tag, FIO_NVME_PI_APPTAG);
			}
			if (fio_qpair->io_flags & SPDK_NVME_IO_FLAGS_PRCHK_REFTAG) {
				to_be32(&pi->ref_tag, (uint32_t)lba + i);
			}
		}
	}
}

static void
fio_extended_lba_verify_pi(struct spdk_fio_qpair *fio_qpair, struct io_u *io_u)
{
	struct spdk_nvme_ns	*ns = NULL;
	struct spdk_nvme_protection_info *pi;
	uint16_t crc16, guard, app_tag;
	uint32_t i, md_size, sector_size, pi_offset, extended_lba_size, ref_tag, lba_count;
	uint64_t lba;

	ns = fio_qpair->ns;
	sector_size = spdk_nvme_ns_get_sector_size(ns);
	md_size = spdk_nvme_ns_get_md_size(ns);
	extended_lba_size = sector_size + md_size;
	lba = io_u->offset / extended_lba_size;
	lba_count = io_u->xfer_buflen / extended_lba_size;

	for (i = 0; i < lba_count; i++) {
		pi_offset = (extended_lba_size * (i + 1)) - 8;
		pi = (struct spdk_nvme_protection_info *)(io_u->buf + pi_offset);

		if (fio_qpair->io_flags & SPDK_NVME_IO_FLAGS_PRCHK_GUARD) {
			/* CRC buffer should not include last 8 bytes of PI */
			crc16 = spdk_crc16_t10dif(io_u->buf + extended_lba_size * i,
						  extended_lba_size - 8);
			to_be16(&guard, crc16);
			if (pi->guard != guard) {
				fprintf(stdout, "Get Guard Error LBA 0x%16.16"PRIx64","
					" Expected 0x%04x but returned with 0x%04x,"
					" may read the LBA without write it first\n",
					lba + i, guard, pi->guard);
			}

		}
		if (fio_qpair->io_flags & SPDK_NVME_IO_FLAGS_PRCHK_APPTAG) {
			/* Previously we used the number of lbas as
			 * application tag for writes
			 */
			to_be16(&app_tag, FIO_NVME_PI_APPTAG);
			if (pi->app_tag != app_tag) {
				fprintf(stdout, "Get Application Tag Error LBA 0x%16.16"PRIx64","
					" Expected 0x%04x but returned with 0x%04x,"
					" may read the LBA without write it first\n",
					lba + i, app_tag, pi->app_tag);
			}
		}
		if (fio_qpair->io_flags & SPDK_NVME_IO_FLAGS_PRCHK_REFTAG) {
			to_be32(&ref_tag, (uint32_t)lba + i);
			if (pi->ref_tag != ref_tag) {
				fprintf(stdout, "Get Reference Tag Error LBA 0x%16.16"PRIx64","
					" Expected 0x%08x but returned with 0x%08x,"
					" may read the LBA without write it first\n",
					lba + i, ref_tag, pi->ref_tag);
			}
		}
	}
}

static void spdk_fio_completion_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_fio_request		*fio_req = ctx;
	struct spdk_fio_thread		*fio_thread = fio_req->fio_thread;

	if (fio_thread->fio_qpair->do_nvme_pi) {
		fio_extended_lba_verify_pi(fio_thread->fio_qpair, fio_req->io);
	}

	assert(fio_thread->iocq_count < fio_thread->iocq_size);
	fio_thread->iocq[fio_thread->iocq_count++] = fio_req->io;
}

static void
spdk_nvme_io_reset_sgl(void *ref, uint32_t sgl_offset)
{
	struct spdk_fio_request *fio_req = (struct spdk_fio_request *)ref;

	fio_req->iov_offset = sgl_offset;
}

static int
spdk_nvme_io_next_sge(void *ref, void **address, uint32_t *length)
{
	struct spdk_fio_request *fio_req = (struct spdk_fio_request *)ref;
	struct io_u *io_u = fio_req->io;

	*address = io_u->buf;
	*length = io_u->xfer_buflen;

	if (fio_req->iov_offset) {
		assert(fio_req->iov_offset <= io_u->xfer_buflen);
		*address += fio_req->iov_offset;
		*length -= fio_req->iov_offset;
	}

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
	uint32_t		block_size;
	uint64_t		lba;
	uint32_t		lba_count;

	/* Find the namespace that corresponds to the file in the io_u */
	fio_qpair = fio_thread->fio_qpair;
	while (fio_qpair != NULL) {
		if (fio_qpair->f == io_u->file) {
			ns = fio_qpair->ns;
			break;
		}
		fio_qpair = fio_qpair->next;
	}
	if (fio_qpair == NULL || ns == NULL) {
		return -ENXIO;
	}

	block_size = spdk_nvme_ns_get_extended_sector_size(ns);

	lba = io_u->offset / block_size;
	lba_count = io_u->xfer_buflen / block_size;

	// TODO: considering situations that fio will randomize and verify io_u
	if (fio_qpair->do_nvme_pi) {
		fio_extended_lba_setup_pi(fio_qpair, io_u);
	}

	switch (io_u->ddir) {
	case DDIR_READ:
		if (!spdk_enable_sgl) {
			rc = spdk_nvme_ns_cmd_read_with_md(ns, fio_qpair->qpair, io_u->buf, NULL, lba, lba_count,
							   spdk_fio_completion_cb, fio_req,
							   fio_qpair->io_flags, fio_req->appmask, fio_req->apptag);
		} else {
			rc = spdk_nvme_ns_cmd_readv_with_md(ns, fio_qpair->qpair, lba,
							    lba_count, spdk_fio_completion_cb, fio_req, fio_qpair->io_flags,
							    spdk_nvme_io_reset_sgl, spdk_nvme_io_next_sge, NULL,
							    fio_req->appmask, fio_req->apptag);
		}
		break;
	case DDIR_WRITE:
		if (!spdk_enable_sgl) {
			rc = spdk_nvme_ns_cmd_write_with_md(ns, fio_qpair->qpair, io_u->buf, NULL, lba, lba_count,
							    spdk_fio_completion_cb, fio_req,
							    fio_qpair->io_flags, fio_req->appmask, fio_req->apptag);
		} else {
			rc = spdk_nvme_ns_cmd_writev_with_md(ns, fio_qpair->qpair, lba,
							     lba_count, spdk_fio_completion_cb, fio_req, fio_qpair->io_flags,
							     spdk_nvme_io_reset_sgl, spdk_nvme_io_next_sge, NULL,
							     fio_req->appmask, fio_req->apptag);
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
		return -abs(rc);
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
		fio_qpair = fio_thread->fio_qpair_current->next;
	}

	for (;;) {
		if (fio_qpair == NULL) {
			fio_qpair = fio_thread->fio_qpair;
		}

		while (fio_qpair != NULL) {
			spdk_nvme_qpair_process_completions(fio_qpair->qpair, max - fio_thread->iocq_count);

			if (fio_thread->iocq_count >= min) {
				/* reset the currrent handling qpair */
				fio_thread->fio_qpair_current = fio_qpair;
				return fio_thread->iocq_count;
			}

			fio_qpair = fio_qpair->next;
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

	/* reset the currrent handling qpair */
	fio_thread->fio_qpair_current = fio_qpair;
	return fio_thread->iocq_count;
}

static int spdk_fio_invalidate(struct thread_data *td, struct fio_file *f)
{
	/* TODO: This should probably send a flush to the device, but for now just return successful. */
	return 0;
}

static void spdk_fio_cleanup(struct thread_data *td)
{
	struct spdk_fio_thread	*fio_thread = td->io_ops_data;
	struct spdk_fio_qpair	*fio_qpair, *fio_qpair_tmp;

	fio_qpair = fio_thread->fio_qpair;
	while (fio_qpair != NULL) {
		spdk_nvme_ctrlr_free_io_qpair(fio_qpair->qpair);
		fio_qpair_tmp = fio_qpair->next;
		free(fio_qpair);
		fio_qpair = fio_qpair_tmp;
	}

	free(fio_thread);

	pthread_mutex_lock(&mutex);
	td_count--;
	if (td_count == 0) {
		struct spdk_fio_ctrlr	*fio_ctrlr, *fio_ctrlr_tmp;

		fio_ctrlr = ctrlr_g;
		while (fio_ctrlr != NULL) {
			spdk_nvme_detach(fio_ctrlr->ctrlr);
			fio_ctrlr_tmp = fio_ctrlr->next;
			free(fio_ctrlr);
			fio_ctrlr = fio_ctrlr_tmp;
		}
		ctrlr_g = NULL;
	}
	pthread_mutex_unlock(&mutex);
	if (!ctrlr_g) {
		if (pthread_cancel(g_ctrlr_thread_id) == 0) {
			pthread_join(g_ctrlr_thread_id, NULL);
		}
	}
}

/* This function enables addition of SPDK parameters to the fio config
 * Adding new parameters by defining them here and defining a callback
 * function to read the parameter value. */
static struct fio_option options[] = {
	{
		.name		= "mem_size_mb",
		.lname		= "Memory size in MB",
		.type		= FIO_OPT_INT,
		.off1		= offsetof(struct spdk_fio_options, mem_size),
		.def		= "512",
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
