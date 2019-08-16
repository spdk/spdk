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

#include "copy_engine_ioat.h"

#include "spdk/stdinc.h"

#include "spdk_internal/copy_engine.h"
#include "spdk_internal/log.h"

#include "spdk/env.h"
#include "spdk/conf.h"
#include "spdk/event.h"
#include "spdk/thread.h"
#include "spdk/ioat.h"

static bool g_ioat_enable = false;

struct ioat_probe_ctx {
	int num_whitelist_devices;
	struct spdk_pci_addr whitelist[IOAT_MAX_CHANNELS];
};

static struct ioat_probe_ctx g_probe_ctx;

struct ioat_device {
	struct spdk_ioat_chan *ioat;
	bool is_allocated;
	/** linked list pointer for device list */
	TAILQ_ENTRY(ioat_device) tailq;
};

static TAILQ_HEAD(, ioat_device) g_devices = TAILQ_HEAD_INITIALIZER(g_devices);
static pthread_mutex_t g_ioat_mutex = PTHREAD_MUTEX_INITIALIZER;

struct ioat_io_channel {
	struct spdk_ioat_chan	*ioat_ch;
	struct ioat_device	*ioat_dev;
	struct spdk_poller	*poller;
};

static int
ioat_find_dev_by_whitelist_bdf(const struct spdk_pci_addr *pci_addr,
			       const struct spdk_pci_addr *whitelist,
			       int num_whitelist_devices)
{
	int i;

	for (i = 0; i < num_whitelist_devices; i++) {
		if (spdk_pci_addr_compare(pci_addr, &whitelist[i]) == 0) {
			return 1;
		}
	}
	return 0;
}

static struct ioat_device *
ioat_allocate_device(void)
{
	struct ioat_device *dev;

	pthread_mutex_lock(&g_ioat_mutex);
	TAILQ_FOREACH(dev, &g_devices, tailq) {
		if (!dev->is_allocated) {
			dev->is_allocated = true;
			pthread_mutex_unlock(&g_ioat_mutex);
			return dev;
		}
	}
	pthread_mutex_unlock(&g_ioat_mutex);

	return NULL;
}

static void
ioat_free_device(struct ioat_device *dev)
{
	pthread_mutex_lock(&g_ioat_mutex);
	dev->is_allocated = false;
	pthread_mutex_unlock(&g_ioat_mutex);
}

struct ioat_task {
	spdk_copy_completion_cb	cb;
};

static int copy_engine_ioat_init(void);
static void copy_engine_ioat_exit(void *ctx);
static void copy_engine_ioat_config_text(FILE *fp);

static size_t
copy_engine_ioat_get_ctx_size(void)
{
	return sizeof(struct ioat_task) + sizeof(struct spdk_copy_task);
}

SPDK_COPY_MODULE_REGISTER(copy_engine_ioat_init, copy_engine_ioat_exit,
			  copy_engine_ioat_config_text,
			  copy_engine_ioat_get_ctx_size)

static void
copy_engine_ioat_exit(void *ctx)
{
	struct ioat_device *dev;

	while (!TAILQ_EMPTY(&g_devices)) {
		dev = TAILQ_FIRST(&g_devices);
		TAILQ_REMOVE(&g_devices, dev, tailq);
		spdk_ioat_detach(dev->ioat);
		ioat_free_device(dev);
		free(dev);
	}
	spdk_copy_engine_module_finish();
}

static void
ioat_done(void *cb_arg)
{
	struct spdk_copy_task *copy_req;
	struct ioat_task *ioat_task = cb_arg;

	copy_req = (struct spdk_copy_task *)
		   ((uintptr_t)ioat_task -
		    offsetof(struct spdk_copy_task, offload_ctx));

	ioat_task->cb(copy_req, 0);
}

static int
ioat_copy_submit(void *cb_arg, struct spdk_io_channel *ch, void *dst, void *src, uint64_t nbytes,
		 spdk_copy_completion_cb cb)
{
	struct ioat_task *ioat_task = (struct ioat_task *)cb_arg;
	struct ioat_io_channel *ioat_ch = spdk_io_channel_get_ctx(ch);

	assert(ioat_ch->ioat_ch != NULL);

	ioat_task->cb = cb;

	return spdk_ioat_submit_copy(ioat_ch->ioat_ch, ioat_task, ioat_done, dst, src, nbytes);
}

static int
ioat_copy_submit_fill(void *cb_arg, struct spdk_io_channel *ch, void *dst, uint8_t fill,
		      uint64_t nbytes, spdk_copy_completion_cb cb)
{
	struct ioat_task *ioat_task = (struct ioat_task *)cb_arg;
	struct ioat_io_channel *ioat_ch = spdk_io_channel_get_ctx(ch);
	uint64_t fill64 = 0x0101010101010101ULL * fill;

	assert(ioat_ch->ioat_ch != NULL);

	ioat_task->cb = cb;

	return spdk_ioat_submit_fill(ioat_ch->ioat_ch, ioat_task, ioat_done, dst, fill64, nbytes);
}

static int
ioat_poll(void *arg)
{
	struct spdk_ioat_chan *chan = arg;

	spdk_ioat_process_events(chan);

	return -1;
}

static struct spdk_io_channel *ioat_get_io_channel(void);

static struct spdk_copy_engine ioat_copy_engine = {
	.copy		= ioat_copy_submit,
	.fill		= ioat_copy_submit_fill,
	.get_io_channel	= ioat_get_io_channel,
};

static int
ioat_create_cb(void *io_device, void *ctx_buf)
{
	struct ioat_io_channel *ch = ctx_buf;
	struct ioat_device *ioat_dev;

	ioat_dev = ioat_allocate_device();
	if (ioat_dev == NULL) {
		return -1;
	}

	ch->ioat_dev = ioat_dev;
	ch->ioat_ch = ioat_dev->ioat;
	ch->poller = spdk_poller_register(ioat_poll, ch->ioat_ch, 0);
	return 0;
}

static void
ioat_destroy_cb(void *io_device, void *ctx_buf)
{
	struct ioat_io_channel *ch = ctx_buf;

	ioat_free_device(ch->ioat_dev);
	spdk_poller_unregister(&ch->poller);
}

static struct spdk_io_channel *
ioat_get_io_channel(void)
{
	return spdk_get_io_channel(&ioat_copy_engine);
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *pci_dev)
{
	struct ioat_probe_ctx *ctx = cb_ctx;
	struct spdk_pci_addr pci_addr = spdk_pci_device_get_addr(pci_dev);

	SPDK_INFOLOG(SPDK_LOG_COPY_IOAT,
		     " Found matching device at %04x:%02x:%02x.%x vendor:0x%04x device:0x%04x\n",
		     pci_addr.domain,
		     pci_addr.bus,
		     pci_addr.dev,
		     pci_addr.func,
		     spdk_pci_device_get_vendor_id(pci_dev),
		     spdk_pci_device_get_device_id(pci_dev));

	if (ctx->num_whitelist_devices > 0 &&
	    !ioat_find_dev_by_whitelist_bdf(&pci_addr, ctx->whitelist, ctx->num_whitelist_devices)) {
		return false;
	}

	/* Claim the device in case conflict with other process */
	if (spdk_pci_device_claim(&pci_addr) < 0) {
		return false;
	}

	return true;
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *pci_dev, struct spdk_ioat_chan *ioat)
{
	struct ioat_device *dev;

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		SPDK_ERRLOG("Failed to allocate device struct\n");
		return;
	}

	dev->ioat = ioat;
	TAILQ_INSERT_TAIL(&g_devices, dev, tailq);
}

void
copy_engine_ioat_enable_probe(void)
{
	g_ioat_enable = true;
}

static int
copy_engine_ioat_add_whitelist_device(const char *pci_bdf)
{
	if (pci_bdf == NULL) {
		return -1;
	}

	if (g_probe_ctx.num_whitelist_devices >= IOAT_MAX_CHANNELS) {
		SPDK_ERRLOG("Ioat whitelist is full (max size is %d)\n",
			    IOAT_MAX_CHANNELS);
		return -1;
	}

	if (spdk_pci_addr_parse(&g_probe_ctx.whitelist[g_probe_ctx.num_whitelist_devices],
				pci_bdf) < 0) {
		SPDK_ERRLOG("Invalid address %s\n", pci_bdf);
		return -1;
	}

	g_probe_ctx.num_whitelist_devices++;

	return 0;
}

int
copy_engine_ioat_add_whitelist_devices(const char *pci_bdfs[], size_t num_pci_bdfs)
{
	size_t i;

	for (i = 0; i < num_pci_bdfs; i++) {
		if (copy_engine_ioat_add_whitelist_device(pci_bdfs[i]) < 0) {
			return -1;
		}
	}

	return 0;
}

static int
copy_engine_ioat_read_config_file_params(struct spdk_conf_section *sp)
{
	int i;
	char *val, *pci_bdf;

	if (spdk_conf_section_get_boolval(sp, "Enable", false)) {
		g_ioat_enable = true;
		/* Enable Ioat */
	}

	val = spdk_conf_section_get_val(sp, "Disable");
	if (val != NULL) {
		SPDK_WARNLOG("\"Disable\" option is deprecated and will be removed in a future release.\n");
		SPDK_WARNLOG("IOAT is now disabled by default. It may be enabled by \"Enable Yes\"\n");

		if (g_ioat_enable && (strcasecmp(val, "Yes") == 0)) {
			SPDK_ERRLOG("\"Enable Yes\" and \"Disable Yes\" cannot be set at the same time\n");
			return -1;
		}
	}

	/* Init the whitelist */
	for (i = 0; ; i++) {
		pci_bdf = spdk_conf_section_get_nmval(sp, "Whitelist", i, 0);
		if (!pci_bdf) {
			break;
		}

		if (copy_engine_ioat_add_whitelist_device(pci_bdf) < 0) {
			return -1;
		}
	}

	return 0;
}

static int
copy_engine_ioat_init(void)
{
	struct spdk_conf_section *sp;
	int rc;

	sp = spdk_conf_find_section(NULL, "Ioat");
	if (sp != NULL) {
		rc = copy_engine_ioat_read_config_file_params(sp);
		if (rc != 0) {
			SPDK_ERRLOG("copy_engine_ioat_read_config_file_params() failed\n");
			return rc;
		}
	}

	if (!g_ioat_enable) {
		return 0;
	}

	if (spdk_ioat_probe(&g_probe_ctx, probe_cb, attach_cb) != 0) {
		SPDK_ERRLOG("spdk_ioat_probe() failed\n");
		return -1;
	}

	SPDK_INFOLOG(SPDK_LOG_COPY_IOAT, "Ioat Copy Engine Offload Enabled\n");
	spdk_copy_engine_register(&ioat_copy_engine);
	spdk_io_device_register(&ioat_copy_engine, ioat_create_cb, ioat_destroy_cb,
				sizeof(struct ioat_io_channel), "ioat_copy_engine");
	return 0;
}

#define COPY_ENGINE_IOAT_HEADER_TMPL \
"[Ioat]\n" \
"  # Users may not want to use offload even it is available.\n" \
"  # Users may use the whitelist to initialize specified devices, IDS\n" \
"  #  uses BUS:DEVICE.FUNCTION to identify each Ioat channel.\n"

#define COPY_ENGINE_IOAT_ENABLE_TMPL \
"  Enable %s\n"

#define COPY_ENGINE_IOAT_WHITELIST_TMPL \
"  Whitelist %.4" PRIx16 ":%.2" PRIx8 ":%.2" PRIx8 ".%" PRIx8 "\n"

static void
copy_engine_ioat_config_text(FILE *fp)
{
	int i;
	struct spdk_pci_addr *dev;

	fprintf(fp, COPY_ENGINE_IOAT_HEADER_TMPL);
	fprintf(fp, COPY_ENGINE_IOAT_ENABLE_TMPL, g_ioat_enable ? "Yes" : "No");

	for (i = 0; i < g_probe_ctx.num_whitelist_devices; i++) {
		dev = &g_probe_ctx.whitelist[i];
		fprintf(fp, COPY_ENGINE_IOAT_WHITELIST_TMPL,
			dev->domain, dev->bus, dev->dev, dev->func);
	}
}

SPDK_LOG_REGISTER_COMPONENT("copy_ioat", SPDK_LOG_COPY_IOAT)
