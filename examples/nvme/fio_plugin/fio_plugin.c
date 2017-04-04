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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>

#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/string.h"

#include "config-host.h"
#include "fio.h"
#include "optgroup.h"

#define NVME_IO_ALIGN		4096

#define MAX_LCORE_COUNT		63

struct spdk_fio_request {
	struct io_u		*io;

	struct spdk_fio_thread	*fio_thread;
};

struct spdk_fio_ns {
	struct fio_file		*f;

	struct spdk_nvme_ns	*ns;
	struct spdk_fio_ns	*next;
};

struct spdk_fio_ctrlr {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_fio_ctrlr	*next;

	struct spdk_nvme_qpair	*qpair;

	struct spdk_fio_ns	*ns_list;
};

struct spdk_fio_thread {
	struct thread_data	*td;

	struct spdk_fio_ctrlr	*ctrlr_list;

	struct io_u		**iocq;	// io completion queue
	unsigned int		iocq_count;	// number of iocq entries filled by last getevents
	unsigned int		iocq_size;	// number of iocq entries allocated

};

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	struct fio_file		*f;
	unsigned int		i;
	struct thread_data 	*td = cb_ctx;
	int rc;
	struct spdk_pci_addr pci_addr;

	if (spdk_pci_addr_parse(&pci_addr, trid->traddr)) {
		return false;
	}

	/* Check if we want to claim this device */
	for_each_file(td, f, i) {
		int domain, bus, slot, func, nsid;
		rc = sscanf(f->file_name, "%x.%x.%x.%x/%x", &domain, &bus, &slot, &func, &nsid);
		if (rc != 5) {
			fprintf(stderr, "Invalid filename: %s\n", f->file_name);
			continue;
		}
		if (bus == pci_addr.bus &&
		    slot == pci_addr.dev &&
		    func == pci_addr.func) {
			return true;
		}
	}

	return false;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct thread_data 	*td = cb_ctx;
	struct spdk_fio_thread	*fio_thread = td->io_ops_data;
	struct spdk_fio_ctrlr	*fio_ctrlr;
	struct spdk_fio_ns	*fio_ns;
	struct fio_file *f;
	unsigned int i;
	struct spdk_pci_addr pci_addr;

	spdk_pci_addr_parse(&pci_addr, trid->traddr);

	/* Create an fio_ctrlr and add it to the list */
	fio_ctrlr = calloc(1, sizeof(*fio_ctrlr));
	fio_ctrlr->ctrlr = ctrlr;
	fio_ctrlr->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, 0);
	fio_ctrlr->ns_list = NULL;
	fio_ctrlr->next = fio_thread->ctrlr_list;
	fio_thread->ctrlr_list = fio_ctrlr;

	/* Loop through all of the file names provided and grab the matching namespaces */
	for_each_file(fio_thread->td, f, i) {
		int domain, bus, slot, func, nsid, rc;
		rc = sscanf(f->file_name, "%x.%x.%x.%x/%x", &domain, &bus, &slot, &func, &nsid);
		if (rc == 5 &&
		    bus == pci_addr.bus &&
		    slot == pci_addr.dev &&
		    func == pci_addr.func) {
			fio_ns = calloc(1, sizeof(*fio_ns));
			if (fio_ns == NULL) {
				continue;
			}
			fio_ns->f = f;
			fio_ns->ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
			if (fio_ns->ns == NULL) {
				free(fio_ns);
				continue;
			}


			f->real_file_size = spdk_nvme_ns_get_size(fio_ns->ns);
			if (f->real_file_size <= 0) {
				free(fio_ns);
				continue;
			}

			f->filetype = FIO_TYPE_BLOCK;
			fio_file_set_size_known(f);

			fio_ns->next = fio_ctrlr->ns_list;
			fio_ctrlr->ns_list = fio_ns;
		}
	}
}

/* Called once at initialization. This is responsible for gathering the size of
 * each "file", which in our case are in the form
 * "05:00.0/0" (PCI bus:device.function/NVMe NSID) */
static int spdk_fio_setup(struct thread_data *td)
{
	struct spdk_fio_thread *fio_thread;
	struct spdk_env_opts opts;

	if (!td->o.use_thread) {
		log_err("spdk: must set thread=1 when using spdk plugin\n");
		return 1;
	}

	fio_thread = calloc(1, sizeof(*fio_thread));
	assert(fio_thread != NULL);

	td->io_ops_data = fio_thread;
	fio_thread->td = td;

	fio_thread->iocq_size = td->o.iodepth;
	fio_thread->iocq = calloc(fio_thread->iocq_size, sizeof(struct io_u *));
	assert(fio_thread->iocq != NULL);

	spdk_env_opts_init(&opts);
	opts.name = "fio";
	spdk_env_init(&opts);

	/* Enumerate all of the controllers */
	if (spdk_nvme_probe(NULL, td, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	return 0;
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
	td->orig_buffer = spdk_zmalloc(total_mem, NVME_IO_ALIGN, NULL);
	return td->orig_buffer == NULL;
}

static void spdk_fio_iomem_free(struct thread_data *td)
{
	spdk_free(td->orig_buffer);
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

static void spdk_fio_completion_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_fio_request		*fio_req = ctx;
	struct spdk_fio_thread		*fio_thread = fio_req->fio_thread;

	assert(fio_thread->iocq_count < fio_thread->iocq_size);
	fio_thread->iocq[fio_thread->iocq_count++] = fio_req->io;
}

static int spdk_fio_queue(struct thread_data *td, struct io_u *io_u)
{
	int rc = 1;
	struct spdk_fio_thread	*fio_thread = td->io_ops_data;
	struct spdk_fio_request	*fio_req = io_u->engine_data;
	struct spdk_fio_ctrlr	*fio_ctrlr;
	struct spdk_fio_ns	*fio_ns;
	bool found_ns = false;

	/* Find the namespace that corresponds to the file in the io_u */
	fio_ctrlr = fio_thread->ctrlr_list;
	while (fio_ctrlr != NULL) {
		fio_ns = fio_ctrlr->ns_list;
		while (fio_ns != NULL) {
			if (fio_ns->f == io_u->file) {
				found_ns = true;
				break;
			}
			fio_ns = fio_ns->next;
		}
		if (found_ns) {
			break;
		}
		fio_ctrlr = fio_ctrlr->next;
	}
	if (fio_ctrlr == NULL || fio_ns == NULL) {
		return FIO_Q_COMPLETED;
	}
	assert(found_ns == true);

	uint32_t block_size = spdk_nvme_ns_get_sector_size(fio_ns->ns);
	uint64_t lba = io_u->offset / block_size;
	uint32_t lba_count = io_u->xfer_buflen / block_size;

	switch (io_u->ddir) {
	case DDIR_READ:
		rc = spdk_nvme_ns_cmd_read(fio_ns->ns, fio_ctrlr->qpair, io_u->buf, lba, lba_count,
					   spdk_fio_completion_cb, fio_req, 0);
		break;
	case DDIR_WRITE:
		rc = spdk_nvme_ns_cmd_write(fio_ns->ns, fio_ctrlr->qpair, io_u->buf, lba, lba_count,
					    spdk_fio_completion_cb, fio_req, 0);
		break;
	default:
		assert(false);
		break;
	}

	assert(rc == 0);

	return rc ? FIO_Q_COMPLETED : FIO_Q_QUEUED;
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
	struct spdk_fio_ctrlr *fio_ctrlr;
	struct timespec t0, t1;
	uint64_t timeout = 0;

	if (t) {
		timeout = t->tv_sec * 1000000000L + t->tv_nsec;
		clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
	}

	fio_thread->iocq_count = 0;

	for (;;) {
		fio_ctrlr = fio_thread->ctrlr_list;
		while (fio_ctrlr != NULL) {
			spdk_nvme_qpair_process_completions(fio_ctrlr->qpair, max - fio_thread->iocq_count);

			if (fio_thread->iocq_count >= min) {
				return fio_thread->iocq_count;
			}

			fio_ctrlr = fio_ctrlr->next;
		}

		if (t) {
			clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
			uint64_t elapse = ((t1.tv_sec - t0.tv_sec) * 1000000000L)
					  + t1.tv_nsec - t0.tv_nsec;
			if (elapse > timeout) {
				break;
			}
		}
	}

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
	struct spdk_fio_ctrlr	*fio_ctrlr, *fio_ctrlr_tmp;
	struct spdk_fio_ns	*fio_ns, *fio_ns_tmp;

	fio_ctrlr = fio_thread->ctrlr_list;
	while (fio_ctrlr != NULL) {
		fio_ns = fio_ctrlr->ns_list;
		while (fio_ns != NULL) {
			fio_ns_tmp = fio_ns->next;
			free(fio_ns);
			fio_ns = fio_ns_tmp;
		}
		spdk_nvme_ctrlr_free_io_qpair(fio_ctrlr->qpair);
		spdk_nvme_detach(fio_ctrlr->ctrlr);
		fio_ctrlr_tmp = fio_ctrlr->next;
		free(fio_ctrlr);
		fio_ctrlr = fio_ctrlr_tmp;
	}

	free(fio_thread);
}

/* FIO imports this structure using dlsym */
struct ioengine_ops ioengine = {
	.name			= "spdk_fio",
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
};
