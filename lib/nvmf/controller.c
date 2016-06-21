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

#include "controller.h"
#include "spdk/conf.h"
#include "spdk/nvme.h"
#include "spdk/log.h"
#include "spdk/trace.h"

static TAILQ_HEAD(, spdk_nvmf_ctrlr) g_ctrlrs = TAILQ_HEAD_INITIALIZER(g_ctrlrs);

#define SPDK_NVMF_MAX_NVME_DEVICES 64

struct nvme_bdf_whitelist {
	uint16_t	domain;
	uint8_t		bus;
	uint8_t		dev;
	uint8_t		func;
	char		name[MAX_NVME_NAME_LENGTH];
};

struct spdk_nvmf_probe_ctx {
	bool claim_all;
	bool unbind_from_kernel;
	int whitelist_count;
	struct nvme_bdf_whitelist whitelist[SPDK_NVMF_MAX_NVME_DEVICES];
};

static void
spdk_nvmf_complete_ctrlr_aer(struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_cpl *cpl)
{
	/* TODO: Temporarily disabled during refactoring. */
#if 0
	struct spdk_nvmf_subsystem *subsystem;
	struct nvmf_session *sess;
	int i;



	/*
	 * Scan the whitelist for any subsystems claiming namespaces
	 * associated with this NVMe controller.
	 */
	for (i = 0; i < g_num_nvme_devices; i++) {
		if (g_whitelist[i].ctrlr == ctrlr &&
		    g_whitelist[i].subsystem != NULL) {

			subsystem = g_whitelist[i].subsystem;
			TAILQ_FOREACH(sess, &subsystem->sessions, entries) {
				if (sess->aer_req == NULL) {
					continue;
				}

				SPDK_TRACELOG(SPDK_TRACE_NVMF, "Process session AER request, sess %p, req %p\n",
					      sess, sess->aer_req);
				nvmf_complete_cmd(sess->aer_req, cpl);
				/* clear this AER from the session */
				sess->aer_req = NULL;
			}
		}
	}
#endif
}

static void
aer_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		fprintf(stderr, "Nvme AER failed!\n");
		return;
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    Nvme AER callback, log_page_id %x\n",
		      (cpl->cdw0 & 0xFF0000) >> 16);

	spdk_nvmf_complete_ctrlr_aer(ctrlr, cpl);
}

static void
spdk_nvmf_ctrlr_create(char *name, int domain, int bus, int dev, int func,
		       struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvmf_ctrlr *nvmf_ctrlr;

	nvmf_ctrlr = calloc(1, sizeof(struct spdk_nvmf_ctrlr));
	if (nvmf_ctrlr == NULL) {
		SPDK_ERRLOG("allocate nvmf_ctrlr failed.\n");
		return;
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Found physical NVMe device. Name: %s\n", name);

	nvmf_ctrlr->ctrlr = ctrlr;
	snprintf(nvmf_ctrlr->name, MAX_NVME_NAME_LENGTH, "%s", name);

	spdk_nvme_ctrlr_register_aer_callback(ctrlr, aer_cb, ctrlr);

	TAILQ_INSERT_HEAD(&g_ctrlrs, nvmf_ctrlr, entry);
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *dev, struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_nvmf_probe_ctx *ctx = cb_ctx;
	uint16_t found_domain = spdk_pci_device_get_domain(dev);
	uint8_t found_bus    = spdk_pci_device_get_bus(dev);
	uint8_t found_dev    = spdk_pci_device_get_dev(dev);
	uint8_t found_func   = spdk_pci_device_get_func(dev);
	int i;
	bool claim_device = false;

	SPDK_NOTICELOG("Probing device %x:%x:%x.%x\n",
		       found_domain, found_bus, found_dev, found_func);

	if (ctx->claim_all) {
		claim_device = true;
	} else {
		for (i = 0; i < SPDK_NVMF_MAX_NVME_DEVICES; i++) {
			if (found_domain == ctx->whitelist[i].domain &&
			    found_bus == ctx->whitelist[i].bus &&
			    found_dev == ctx->whitelist[i].dev &&
			    found_func == ctx->whitelist[i].func) {
				claim_device = true;
				break;
			}
		}
	}

	if (!claim_device) {
		return false;
	}

	if (spdk_pci_device_has_non_uio_driver(dev)) {
		if (ctx->unbind_from_kernel) {
			if (spdk_pci_device_switch_to_uio_driver(dev) == 0) {
				return true;
			}
		}
	} else {
		return true;
	}

	return false;
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *dev, struct spdk_nvme_ctrlr *ctrlr,
	  const struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_nvmf_probe_ctx *ctx = cb_ctx;
	uint16_t found_domain = spdk_pci_device_get_domain(dev);
	uint8_t found_bus    = spdk_pci_device_get_bus(dev);
	uint8_t found_dev    = spdk_pci_device_get_dev(dev);
	uint8_t found_func   = spdk_pci_device_get_func(dev);
	int i;

	SPDK_NOTICELOG("Attempting to claim device %x:%x:%x.%x\n",
		       found_domain, found_bus, found_dev, found_func);

	if (ctx->claim_all) {
		/* If claim_all is true, whitelist_count can be repurposed here safely */
		char name[64];
		snprintf(name, 64, "Nvme%d", ctx->whitelist_count);
		spdk_nvmf_ctrlr_create(name, found_domain, found_bus,
				       found_dev, found_func, ctrlr);
		ctx->whitelist_count++;
		return;
	}

	for (i = 0; i < SPDK_NVMF_MAX_NVME_DEVICES; i++) {
		if (found_domain == ctx->whitelist[i].domain &&
		    found_bus == ctx->whitelist[i].bus &&
		    found_dev == ctx->whitelist[i].dev &&
		    found_func == ctx->whitelist[i].func) {
			spdk_nvmf_ctrlr_create(ctx->whitelist[i].name, found_domain, found_bus,
					       found_dev, found_func, ctrlr);
			return;
		}
	}

}

int
spdk_nvmf_init_nvme(void)
{
	struct spdk_conf_section *sp;
	struct spdk_nvmf_probe_ctx ctx = { 0 };
	const char *val;
	int i, rc;

	SPDK_NOTICELOG("*** Initialize NVMe Devices ***\n");
	sp = spdk_conf_find_section(NULL, "Nvme");
	if (sp == NULL) {
		SPDK_ERRLOG("NVMe device section in config file not found!\n");
		return -1;
	}

	val = spdk_conf_section_get_val(sp, "ClaimAllDevices");
	if (val != NULL) {
		if (!strcmp(val, "Yes")) {
			ctx.claim_all = true;
		}
	}

	val = spdk_conf_section_get_val(sp, "UnbindFromKernel");
	if (val != NULL) {
		if (!strcmp(val, "Yes")) {
			ctx.unbind_from_kernel = true;
		}
	}

	if (!ctx.claim_all) {
		for (i = 0; ; i++) {
			unsigned int domain, bus, dev, func;

			val = spdk_conf_section_get_nmval(sp, "BDF", i, 0);
			if (val == NULL) {
				break;
			}

			rc = sscanf(val, "%x:%x:%x.%x", &domain, &bus, &dev, &func);
			if (rc != 4) {
				SPDK_ERRLOG("Invalid format for BDF: %s\n", val);
				return -1;
			}

			ctx.whitelist[ctx.whitelist_count].domain = domain;
			ctx.whitelist[ctx.whitelist_count].bus = bus;
			ctx.whitelist[ctx.whitelist_count].dev = dev;
			ctx.whitelist[ctx.whitelist_count].func = func;

			val = spdk_conf_section_get_nmval(sp, "BDF", i, 1);
			if (val == NULL) {
				SPDK_ERRLOG("BDF section with no device name\n");
				return -1;
			}

			snprintf(ctx.whitelist[ctx.whitelist_count].name, MAX_NVME_NAME_LENGTH, "%s", val);

			ctx.whitelist_count++;
		}

		if (ctx.whitelist_count == 0) {
			SPDK_ERRLOG("No BDF section\n");
			return -1;
		}
	}

	/* Probe the physical NVMe devices */
	if (spdk_nvme_probe(&ctx, probe_cb, attach_cb, NULL)) {
		SPDK_ERRLOG("One or more controllers failed in spdk_nvme_probe()\n");
	}

	/* check whether any nvme controller is probed */
	if (TAILQ_EMPTY(&g_ctrlrs)) {
		SPDK_ERRLOG("No nvme controllers are probed\n");
		return -1;
	}

	return 0;
}

int
spdk_nvmf_shutdown_nvme(void)
{
	struct spdk_nvmf_ctrlr *ctrlr, *tctrlr;

	TAILQ_FOREACH_SAFE(ctrlr, &g_ctrlrs, entry, tctrlr) {
		TAILQ_REMOVE(&g_ctrlrs, ctrlr, entry);
		spdk_nvme_detach(ctrlr->ctrlr);
		free(ctrlr);
	}

	return 0;
}

struct spdk_nvmf_ctrlr *
spdk_nvmf_ctrlr_claim(const char *name)
{
	struct spdk_nvmf_ctrlr *ctrlr, *tctrlr;

	if (name == NULL) {
		return NULL;
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Attempting to claim NVMe controller %s\n", name);

	TAILQ_FOREACH_SAFE(ctrlr, &g_ctrlrs, entry, tctrlr) {
		if (strncmp(ctrlr->name, name, MAX_NVME_NAME_LENGTH) == 0) {
			if (ctrlr->claimed) {
				SPDK_ERRLOG("Two subsystems are attempting to claim the same NVMe controller.\n");
				return NULL;
			}
			ctrlr->claimed = true;
			return ctrlr;
		}
	}

	return NULL;
}
