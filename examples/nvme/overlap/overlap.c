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

struct overlap_sequence {
	int		is_completed;
	uint64_t	end1;
	uint64_t	end2;
};

static void
io_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct overlap_sequence *sequence = arg;

	if (sequence->end1 == 0) {
		sequence->end1 = spdk_get_ticks();
	} else {
		sequence->end2 = spdk_get_ticks();
		sequence->is_completed = 1;
	}
}

static void
run_overlap_test(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_qpair			*qpair;
	struct spdk_nvme_io_qpair_opts		opts;
	struct overlap_sequence			sequence = {};
	uint64_t				start;
	void					*buf;

	printf("  Namespace ID: %d size: %juGB\n", spdk_nvme_ns_get_id(ns),
	       spdk_nvme_ns_get_size(ns) / 1000000000);

	spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
	opts.delay_pcie_doorbell = true;
	opts.max_delay_pcie_cq_doorbell = 1000 * 1000;

	qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, &opts, sizeof(opts));
	if (qpair == NULL) {
		printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
		return;
	}

	buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (buf == NULL) {
		printf("ERROR: buffer allocation failed\n");
		return;
	}

	start = spdk_get_ticks();
	spdk_nvme_ns_cmd_read(ns, qpair, buf, 0, 1, io_complete, &sequence, 0);
	spdk_nvme_ns_cmd_read(ns, qpair, buf, 0, 1, io_complete, &sequence, 0);

	while (!sequence.is_completed) {
		spdk_nvme_qpair_process_completions_tsc(qpair, 0, spdk_get_ticks());
	}

	printf("end1 = %juus\n", (sequence.end1 - start) * 1000000 / spdk_get_ticks_hz());
	printf("end2 = %juus\n", (sequence.end2 - start) * 1000000 / spdk_get_ticks_hz());
	spdk_free(buf);
	spdk_nvme_ctrlr_free_io_qpair(qpair);
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attaching to %s\n", trid->traddr);

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	int					nsid, num_ns;
	struct spdk_nvme_ns			*ns;
	const struct spdk_nvme_ctrlr_data	*cdata;
	char					name[1024];

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	printf("Attached to %s\n", trid->traddr);

	snprintf(name, sizeof(name), "%-20.20s (FW:%-8.8s)", cdata->mn, cdata->fr);

	num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
	printf("Using controller %s with %d namespaces.\n", name, num_ns);
	for (nsid = 1; nsid <= num_ns; nsid++) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns == NULL) {
			continue;
		}

		if (!spdk_nvme_ns_is_active(ns)) {
			printf("Skipping inactive NS %u\n", nsid);
			continue;
		}

		run_overlap_test(ctrlr, ns);
	}
}

int main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	int rc;

	spdk_env_opts_init(&opts);
	opts.name = "overlap";
	opts.shm_id = 0;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	printf("Initializing NVMe Controllers\n");

	rc = spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	return 0;
}
