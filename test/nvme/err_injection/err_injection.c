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

#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/env.h"

#define MAX_DEVS 64

struct dev {
	bool						error_expected;
	struct spdk_nvme_ctrlr				*ctrlr;
	struct spdk_nvme_ns				*ns;
	struct spdk_nvme_qpair				*qpair;
	void						*data;
	char						name[SPDK_NVMF_TRADDR_MAX_LEN + 1];
};

static struct dev devs[MAX_DEVS];
static int num_devs = 0;

#define foreach_dev(iter) \
	for (iter = devs; iter - devs < num_devs; iter++)

static int outstanding_commands = 0;
static int failed = 0;

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
	struct dev *dev;
	uint32_t nsid;

	/* add to dev list */
	dev = &devs[num_devs++];
	if (num_devs >= MAX_DEVS) {
		return;
	}

	dev->ctrlr = ctrlr;
	nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	dev->ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);

	dev->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
	if (dev->qpair == NULL) {
		failed = 1;
		return;
	}

	snprintf(dev->name, sizeof(dev->name), "%s",
		 trid->traddr);

	printf("Attached to %s\n", dev->name);
}

static void
get_feature_test_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct dev *dev = cb_arg;

	outstanding_commands--;

	if (spdk_nvme_cpl_is_error(cpl) && dev->error_expected) {
		if (cpl->status.sct != SPDK_NVME_SCT_GENERIC ||
		    cpl->status.sc != SPDK_NVME_SC_INVALID_FIELD) {
			failed = 1;
		}
		printf("%s: get features failed as expected\n", dev->name);
		return;
	}

	if (!spdk_nvme_cpl_is_error(cpl) && !dev->error_expected) {
		printf("%s: get features successfully as expected\n", dev->name);
		return;
	}

	failed = 1;
}

static void
get_feature_test(bool error_expected)
{
	struct dev *dev;
	struct spdk_nvme_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_NUMBER_OF_QUEUES;

	foreach_dev(dev) {
		dev->error_expected = error_expected;
		if (spdk_nvme_ctrlr_cmd_admin_raw(dev->ctrlr, &cmd, NULL, 0,
						  get_feature_test_cb, dev) != 0) {
			printf("Error: failed to send Get Features command for dev=%p\n", dev);
			failed = 1;
			goto cleanup;
		}
		outstanding_commands++;
	}

cleanup:

	while (outstanding_commands) {
		foreach_dev(dev) {
			spdk_nvme_ctrlr_process_admin_completions(dev->ctrlr);
		}
	}
}

static void
read_test_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct dev *dev = cb_arg;

	outstanding_commands--;
	spdk_free(dev->data);

	if (spdk_nvme_cpl_is_error(cpl) && dev->error_expected) {
		if (cpl->status.sct != SPDK_NVME_SCT_MEDIA_ERROR ||
		    cpl->status.sc != SPDK_NVME_SC_UNRECOVERED_READ_ERROR) {
			failed = 1;
		}
		printf("%s: read failed as expected\n", dev->name);
		return;
	}

	if (!spdk_nvme_cpl_is_error(cpl) && !dev->error_expected) {
		printf("%s: read successfully as expected\n", dev->name);
		return;
	}

	failed = 1;
}

static void
read_test(bool error_expected)
{
	struct dev *dev;

	foreach_dev(dev) {
		if (dev->ns == NULL) {
			continue;
		}

		dev->error_expected = error_expected;
		dev->data = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (!dev->data) {
			failed = 1;
			goto cleanup;
		}

		if (spdk_nvme_ns_cmd_read(dev->ns, dev->qpair, dev->data,
					  0, 1, read_test_cb, dev, 0) != 0) {
			printf("Error: failed to send Read command for dev=%p\n", dev);
			failed = 1;
			goto cleanup;
		}

		outstanding_commands++;
	}

cleanup:

	while (outstanding_commands) {
		foreach_dev(dev) {
			spdk_nvme_qpair_process_completions(dev->qpair, 0);
		}
	}
}

int main(int argc, char **argv)
{
	struct dev		*dev;
	struct spdk_env_opts	opts;
	int			rc;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	spdk_env_opts_init(&opts);
	opts.name = "err_injection";
	opts.core_mask = "0x1";
	opts.shm_id = 0;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	printf("NVMe Error Injection test\n");

	if (spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	if (failed) {
		goto exit;
	}

	if (!num_devs) {
		printf("No NVMe controller found, %s exiting\n", argv[0]);
		return 1;
	}

	foreach_dev(dev) {
		/* Admin error injection at submission path */
		rc = spdk_nvme_qpair_add_cmd_error_injection(dev->ctrlr, NULL,
				SPDK_NVME_OPC_GET_FEATURES, true, 5000, 1,
				SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_INVALID_FIELD);
		failed += rc;
		/* IO error injection at completion path */
		rc = spdk_nvme_qpair_add_cmd_error_injection(dev->ctrlr, dev->qpair,
				SPDK_NVME_OPC_READ, false, 0, 1,
				SPDK_NVME_SCT_MEDIA_ERROR, SPDK_NVME_SC_UNRECOVERED_READ_ERROR);
		failed += rc;
	}

	if (failed) {
		goto exit;
	}

	/* Admin Get Feature, expect error return */
	get_feature_test(true);
	/* Admin Get Feature, expect successful return */
	get_feature_test(false);
	/* Read, expect error return */
	read_test(true);
	/* Read, expect successful return */
	read_test(false);

exit:
	printf("Cleaning up...\n");
	foreach_dev(dev) {
		spdk_nvme_detach_async(dev->ctrlr, &detach_ctx);
	}
	while (detach_ctx && spdk_nvme_detach_poll_async(detach_ctx) == -EAGAIN) {
		;
	}

	return failed;
}
