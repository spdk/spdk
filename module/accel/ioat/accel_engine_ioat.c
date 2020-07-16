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

#include "accel_engine_ioat.h"

#include "spdk/stdinc.h"

#include "spdk_internal/accel_engine.h"
#include "spdk_internal/log.h"

#include "spdk/env.h"
#include "spdk/conf.h"
#include "spdk/event.h"
#include "spdk/thread.h"
#include "spdk/ioat.h"
#include "spdk/crc32.h"

#define ALIGN_4K 0x1000

enum ioat_accel_opcode {
	IOAT_ACCEL_OPCODE_MEMMOVE	= 0,
	IOAT_ACCEL_OPCODE_MEMFILL	= 1,
	IOAT_ACCEL_OPCODE_COMPARE	= 2,
	IOAT_ACCEL_OPCODE_CRC32C	= 3,
	IOAT_ACCEL_OPCODE_DUALCAST	= 4,
};

struct ioat_accel_op {
	struct ioat_io_channel		*ioat_ch;
	void				*cb_arg;
	spdk_accel_completion_cb	cb_fn;
	void				*src;
	union {
		void			*dst;
		void			*src2;
	};
	void				*dst2;
	uint32_t			seed;
	uint64_t			fill_pattern;
	enum ioat_accel_opcode		op_code;
	uint64_t			nbytes;
	TAILQ_ENTRY(ioat_accel_op)	link;
};

static int g_batch_size;
static bool g_ioat_enable = false;
static bool g_ioat_initialized = false;

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

struct pci_device {
	struct spdk_pci_device *pci_dev;
	TAILQ_ENTRY(pci_device) tailq;
};

static TAILQ_HEAD(, ioat_device) g_devices = TAILQ_HEAD_INITIALIZER(g_devices);
static pthread_mutex_t g_ioat_mutex = PTHREAD_MUTEX_INITIALIZER;

static TAILQ_HEAD(, pci_device) g_pci_devices = TAILQ_HEAD_INITIALIZER(g_pci_devices);

struct ioat_io_channel {
	struct spdk_ioat_chan		*ioat_ch;
	struct ioat_device		*ioat_dev;
	struct spdk_poller		*poller;
	TAILQ_HEAD(, ioat_accel_op)	op_pool;
	TAILQ_HEAD(, ioat_accel_op)	sw_batch; /* for operations not hw accelerated */
	bool				hw_batch; /* for operations that are hw accelerated */
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
	spdk_accel_completion_cb	cb;
};

static int accel_engine_ioat_init(void);
static void accel_engine_ioat_exit(void *ctx);
static void accel_engine_ioat_config_text(FILE *fp);

static size_t
accel_engine_ioat_get_ctx_size(void)
{
	return sizeof(struct ioat_task) + sizeof(struct spdk_accel_task);
}

SPDK_ACCEL_MODULE_REGISTER(accel_engine_ioat_init, accel_engine_ioat_exit,
			   accel_engine_ioat_config_text, NULL,
			   accel_engine_ioat_get_ctx_size)

static void
ioat_done(void *cb_arg)
{
	struct spdk_accel_task *accel_req;
	struct ioat_task *ioat_task = cb_arg;

	accel_req = (struct spdk_accel_task *)
		    ((uintptr_t)ioat_task -
		     offsetof(struct spdk_accel_task, offload_ctx));

	ioat_task->cb(accel_req, 0);
}

static int
ioat_submit_copy(struct spdk_io_channel *ch, void *dst, void *src, uint64_t nbytes,
		 spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct ioat_task *ioat_task = (struct ioat_task *)cb_arg;
	struct ioat_io_channel *ioat_ch = spdk_io_channel_get_ctx(ch);

	assert(ioat_ch->ioat_ch != NULL);

	ioat_task->cb = cb_fn;

	return spdk_ioat_submit_copy(ioat_ch->ioat_ch, ioat_task, ioat_done, dst, src, nbytes);
}

static int
ioat_submit_fill(struct spdk_io_channel *ch, void *dst, uint8_t fill,
		 uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct ioat_task *ioat_task = (struct ioat_task *)cb_arg;
	struct ioat_io_channel *ioat_ch = spdk_io_channel_get_ctx(ch);
	uint64_t fill64 = 0x0101010101010101ULL * fill;

	assert(ioat_ch->ioat_ch != NULL);

	ioat_task->cb = cb_fn;

	return spdk_ioat_submit_fill(ioat_ch->ioat_ch, ioat_task, ioat_done, dst, fill64, nbytes);
}

static int
ioat_poll(void *arg)
{
	struct spdk_ioat_chan *chan = arg;

	return spdk_ioat_process_events(chan) != 0 ? SPDK_POLLER_BUSY :
	       SPDK_POLLER_IDLE;
}

static struct spdk_io_channel *ioat_get_io_channel(void);

/*
 * The IOAT engine only supports these capabilities as hardware
 * accelerated. The accel fw will handle unsupported functions
 * by calling the software implementations of the functions.
 */
static uint64_t
ioat_get_capabilities(void)
{
	return ACCEL_COPY | ACCEL_FILL | ACCEL_BATCH;
}

/* The IOAT batch functions exposed by the accel fw do not match up 1:1
 * with the functions in the IOAT library. The IOAT library directly only
 * supports construction of accelerated functions via the IOAT native
 * interface.  The accel_fw batch capabilities are implemented here in the
 * plug-in and rely on either the IOAT library for accelerated commands
 * or software functions for non-accelerated.
 */
static uint32_t
ioat_batch_get_max(void)
{
	return g_batch_size;
}

static struct spdk_accel_batch *
ioat_batch_create(struct spdk_io_channel *ch)
{
	struct ioat_io_channel *ioat_ch = spdk_io_channel_get_ctx(ch);

	if (!TAILQ_EMPTY(&ioat_ch->sw_batch) || (ioat_ch->hw_batch == true)) {
		SPDK_ERRLOG("IOAT accel engine only supports one batch at a time.\n");
		return NULL;
	}

	return (struct spdk_accel_batch *)&ioat_ch->hw_batch;
}

static struct ioat_accel_op *
_prep_op(struct ioat_io_channel *ioat_ch, struct spdk_accel_batch *batch,
	 spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct ioat_accel_op *op;

	if ((struct spdk_accel_batch *)&ioat_ch->hw_batch != batch) {
		SPDK_ERRLOG("Invalid batch\n");
		return NULL;
	}

	if (!TAILQ_EMPTY(&ioat_ch->op_pool)) {
		op = TAILQ_FIRST(&ioat_ch->op_pool);
		TAILQ_REMOVE(&ioat_ch->op_pool, op, link);
	} else {
		SPDK_ERRLOG("Ran out of operations for batch\n");
		return NULL;
	}

	op->cb_arg = cb_arg;
	op->cb_fn = cb_fn;
	op->ioat_ch = ioat_ch;

	return op;
}

static int
ioat_batch_prep_copy(struct spdk_io_channel *ch, struct spdk_accel_batch *batch,
		     void *dst, void *src, uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct ioat_io_channel *ioat_ch = spdk_io_channel_get_ctx(ch);
	struct ioat_task *ioat_task = (struct ioat_task *)cb_arg;

	ioat_task->cb = cb_fn;
	ioat_ch->hw_batch = true;

	/* Call the IOAT library prep function. */
	return spdk_ioat_build_copy(ioat_ch->ioat_ch, ioat_task, ioat_done, dst, src, nbytes);
}

static int
ioat_batch_prep_fill(struct spdk_io_channel *ch, struct spdk_accel_batch *batch, void *dst,
		     uint8_t fill, uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct ioat_io_channel *ioat_ch = spdk_io_channel_get_ctx(ch);
	struct ioat_task *ioat_task = (struct ioat_task *)cb_arg;
	uint64_t fill_pattern;

	ioat_task->cb = cb_fn;
	ioat_ch->hw_batch = true;
	memset(&fill_pattern, fill, sizeof(uint64_t));

	/* Call the IOAT library prep function. */
	return spdk_ioat_build_fill(ioat_ch->ioat_ch, ioat_task, ioat_done, dst, fill_pattern, nbytes);
}

static int
ioat_batch_prep_dualcast(struct spdk_io_channel *ch,
			 struct spdk_accel_batch *batch, void *dst1, void *dst2,
			 void *src, uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct ioat_accel_op *op;
	struct ioat_io_channel *ioat_ch = spdk_io_channel_get_ctx(ch);

	if ((uintptr_t)dst1 & (ALIGN_4K - 1) || (uintptr_t)dst2 & (ALIGN_4K - 1)) {
		SPDK_ERRLOG("Dualcast requires 4K alignment on dst addresses\n");
		return -EINVAL;
	}

	op = _prep_op(ioat_ch, batch, cb_fn, cb_arg);
	if (op == NULL) {
		return -EINVAL;
	}

	/* Command specific. */
	op->src = src;
	op->dst = dst1;
	op->dst2 = dst2;
	op->nbytes = nbytes;
	op->op_code = IOAT_ACCEL_OPCODE_DUALCAST;
	TAILQ_INSERT_TAIL(&ioat_ch->sw_batch, op, link);

	return 0;
}

static int
ioat_batch_prep_compare(struct spdk_io_channel *ch,
			struct spdk_accel_batch *batch, void *src1,
			void *src2, uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct ioat_accel_op *op;
	struct ioat_io_channel *ioat_ch = spdk_io_channel_get_ctx(ch);

	op = _prep_op(ioat_ch, batch, cb_fn, cb_arg);
	if (op == NULL) {
		return -EINVAL;
	}

	/* Command specific. */
	op->src = src1;
	op->src2 = src2;
	op->nbytes = nbytes;
	op->op_code = IOAT_ACCEL_OPCODE_COMPARE;
	TAILQ_INSERT_TAIL(&ioat_ch->sw_batch, op, link);

	return 0;
}

static int
ioat_batch_prep_crc32c(struct spdk_io_channel *ch,
		       struct spdk_accel_batch *batch, uint32_t *dst, void *src,
		       uint32_t seed, uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct ioat_accel_op *op;
	struct ioat_io_channel *ioat_ch = spdk_io_channel_get_ctx(ch);

	op = _prep_op(ioat_ch, batch, cb_fn, cb_arg);
	if (op == NULL) {
		return -EINVAL;
	}

	/* Command specific. */
	op->dst = (void *)dst;
	op->src = src;
	op->seed = seed;
	op->nbytes = nbytes;
	op->op_code = IOAT_ACCEL_OPCODE_CRC32C;
	TAILQ_INSERT_TAIL(&ioat_ch->sw_batch, op, link);

	return 0;
}

static int
ioat_batch_cancel(struct spdk_io_channel *ch, struct spdk_accel_batch *batch)
{
	struct ioat_accel_op *op;
	struct ioat_io_channel *ioat_ch = spdk_io_channel_get_ctx(ch);

	if ((struct spdk_accel_batch *)&ioat_ch->hw_batch != batch) {
		SPDK_ERRLOG("Invalid batch\n");
		return -EINVAL;
	}

	/* Flush the batched HW items, there's no way to cancel these without resetting. */
	spdk_ioat_flush(ioat_ch->ioat_ch);
	ioat_ch->hw_batch = false;

	/* Return batched software items to the pool. */
	while ((op = TAILQ_FIRST(&ioat_ch->sw_batch))) {
		TAILQ_REMOVE(&ioat_ch->sw_batch, op, link);
		TAILQ_INSERT_TAIL(&ioat_ch->op_pool, op, link);
	}

	return 0;
}

static int
ioat_batch_submit(struct spdk_io_channel *ch, struct spdk_accel_batch *batch,
		  spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct ioat_accel_op *op;
	struct ioat_io_channel *ioat_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_req;
	int batch_status = 0, cmd_status = 0;

	if ((struct spdk_accel_batch *)&ioat_ch->hw_batch != batch) {
		SPDK_ERRLOG("Invalid batch\n");
		return -EINVAL;
	}

	/* Flush the batched HW items first. */
	spdk_ioat_flush(ioat_ch->ioat_ch);
	ioat_ch->hw_batch = false;

	/* Complete the batched software items. */
	while ((op = TAILQ_FIRST(&ioat_ch->sw_batch))) {
		TAILQ_REMOVE(&ioat_ch->sw_batch, op, link);
		accel_req = (struct spdk_accel_task *)((uintptr_t)op->cb_arg -
						       offsetof(struct spdk_accel_task, offload_ctx));

		switch (op->op_code) {
		case IOAT_ACCEL_OPCODE_DUALCAST:
			memcpy(op->dst, op->src, op->nbytes);
			memcpy(op->dst2, op->src, op->nbytes);
			break;
		case IOAT_ACCEL_OPCODE_COMPARE:
			cmd_status = memcmp(op->src, op->src2, op->nbytes);
			break;
		case IOAT_ACCEL_OPCODE_CRC32C:
			*(uint32_t *)op->dst = spdk_crc32c_update(op->src, op->nbytes, ~op->seed);
			break;
		default:
			assert(false);
			break;
		}

		batch_status |= cmd_status;
		op->cb_fn(accel_req, cmd_status);
		TAILQ_INSERT_TAIL(&ioat_ch->op_pool, op, link);
	}

	/* Now complete the batch request itself. */
	accel_req = (struct spdk_accel_task *)((uintptr_t)cb_arg -
					       offsetof(struct spdk_accel_task, offload_ctx));
	cb_fn(accel_req, batch_status);

	return 0;
}

static struct spdk_accel_engine ioat_accel_engine = {
	.get_capabilities	= ioat_get_capabilities,
	.copy			= ioat_submit_copy,
	.fill			= ioat_submit_fill,
	.batch_get_max		= ioat_batch_get_max,
	.batch_create		= ioat_batch_create,
	.batch_cancel		= ioat_batch_cancel,
	.batch_prep_copy	= ioat_batch_prep_copy,
	.batch_prep_dualcast	= ioat_batch_prep_dualcast,
	.batch_prep_compare	= ioat_batch_prep_compare,
	.batch_prep_fill	= ioat_batch_prep_fill,
	.batch_prep_crc32c	= ioat_batch_prep_crc32c,
	.batch_submit		= ioat_batch_submit,
	.get_io_channel		= ioat_get_io_channel,
};

static int
ioat_create_cb(void *io_device, void *ctx_buf)
{
	struct ioat_io_channel *ch = ctx_buf;
	struct ioat_device *ioat_dev;
	struct ioat_accel_op *op;
	int i;

	ioat_dev = ioat_allocate_device();
	if (ioat_dev == NULL) {
		return -1;
	}

	TAILQ_INIT(&ch->sw_batch);
	ch->hw_batch = false;
	TAILQ_INIT(&ch->op_pool);

	g_batch_size = spdk_ioat_get_max_descriptors(ioat_dev->ioat);
	for (i = 0 ; i < g_batch_size ; i++) {
		op = calloc(1, sizeof(struct ioat_accel_op));
		if (op == NULL) {
			SPDK_ERRLOG("Failed to allocate operation for batch.\n");
			while ((op = TAILQ_FIRST(&ch->op_pool))) {
				TAILQ_REMOVE(&ch->op_pool, op, link);
				free(op);
			}
			return -ENOMEM;
		}
		TAILQ_INSERT_TAIL(&ch->op_pool, op, link);
	}

	ch->ioat_dev = ioat_dev;
	ch->ioat_ch = ioat_dev->ioat;
	ch->poller = SPDK_POLLER_REGISTER(ioat_poll, ch->ioat_ch, 0);
	return 0;
}

static void
ioat_destroy_cb(void *io_device, void *ctx_buf)
{
	struct ioat_io_channel *ch = ctx_buf;
	struct ioat_accel_op *op;

	while ((op = TAILQ_FIRST(&ch->op_pool))) {
		TAILQ_REMOVE(&ch->op_pool, op, link);
		free(op);
	}

	ioat_free_device(ch->ioat_dev);
	spdk_poller_unregister(&ch->poller);
}

static struct spdk_io_channel *
ioat_get_io_channel(void)
{
	return spdk_get_io_channel(&ioat_accel_engine);
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *pci_dev)
{
	struct ioat_probe_ctx *ctx = cb_ctx;
	struct spdk_pci_addr pci_addr = spdk_pci_device_get_addr(pci_dev);
	struct pci_device *pdev;

	SPDK_INFOLOG(SPDK_LOG_ACCEL_IOAT,
		     " Found matching device at %04x:%02x:%02x.%x vendor:0x%04x device:0x%04x\n",
		     pci_addr.domain,
		     pci_addr.bus,
		     pci_addr.dev,
		     pci_addr.func,
		     spdk_pci_device_get_vendor_id(pci_dev),
		     spdk_pci_device_get_device_id(pci_dev));

	pdev = calloc(1, sizeof(*pdev));
	if (pdev == NULL) {
		return false;
	}
	pdev->pci_dev = pci_dev;
	TAILQ_INSERT_TAIL(&g_pci_devices, pdev, tailq);

	if (ctx->num_whitelist_devices > 0 &&
	    !ioat_find_dev_by_whitelist_bdf(&pci_addr, ctx->whitelist, ctx->num_whitelist_devices)) {
		return false;
	}

	/* Claim the device in case conflict with other process */
	if (spdk_pci_device_claim(pci_dev) < 0) {
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
accel_engine_ioat_enable_probe(void)
{
	g_ioat_enable = true;
}

static int
accel_engine_ioat_add_whitelist_device(const char *pci_bdf)
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
accel_engine_ioat_add_whitelist_devices(const char *pci_bdfs[], size_t num_pci_bdfs)
{
	size_t i;

	for (i = 0; i < num_pci_bdfs; i++) {
		if (accel_engine_ioat_add_whitelist_device(pci_bdfs[i]) < 0) {
			return -1;
		}
	}

	return 0;
}

static int
accel_engine_ioat_read_config_file_params(struct spdk_conf_section *sp)
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

		if (accel_engine_ioat_add_whitelist_device(pci_bdf) < 0) {
			return -1;
		}
	}

	return 0;
}

static int
accel_engine_ioat_init(void)
{
	struct spdk_conf_section *sp;
	int rc;

	sp = spdk_conf_find_section(NULL, "Ioat");
	if (sp != NULL) {
		rc = accel_engine_ioat_read_config_file_params(sp);
		if (rc != 0) {
			SPDK_ERRLOG("accel_engine_ioat_read_config_file_params() failed\n");
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

	g_ioat_initialized = true;
	SPDK_NOTICELOG("Accel engine updated to use IOAT engine.\n");
	spdk_accel_hw_engine_register(&ioat_accel_engine);
	spdk_io_device_register(&ioat_accel_engine, ioat_create_cb, ioat_destroy_cb,
				sizeof(struct ioat_io_channel), "ioat_accel_engine");
	return 0;
}

static void
accel_engine_ioat_exit(void *ctx)
{
	struct ioat_device *dev;
	struct pci_device *pci_dev;

	if (g_ioat_initialized) {
		spdk_io_device_unregister(&ioat_accel_engine, NULL);
	}

	while (!TAILQ_EMPTY(&g_devices)) {
		dev = TAILQ_FIRST(&g_devices);
		TAILQ_REMOVE(&g_devices, dev, tailq);
		spdk_ioat_detach(dev->ioat);
		ioat_free_device(dev);
		free(dev);
	}

	while (!TAILQ_EMPTY(&g_pci_devices)) {
		pci_dev = TAILQ_FIRST(&g_pci_devices);
		TAILQ_REMOVE(&g_pci_devices, pci_dev, tailq);
		spdk_pci_device_detach(pci_dev->pci_dev);
		free(pci_dev);
	}

	spdk_accel_engine_module_finish();
}

#define ACCEL_ENGINE_IOAT_HEADER_TMPL \
"[Ioat]\n" \
"  # Users may not want to use offload even it is available.\n" \
"  # Users may use the whitelist to initialize specified devices, IDS\n" \
"  #  uses BUS:DEVICE.FUNCTION to identify each Ioat channel.\n"

#define ACCEL_ENGINE_IOAT_ENABLE_TMPL \
"  Enable %s\n"

#define ACCEL_ENGINE_IOAT_WHITELIST_TMPL \
"  Whitelist %.4" PRIx16 ":%.2" PRIx8 ":%.2" PRIx8 ".%" PRIx8 "\n"

static void
accel_engine_ioat_config_text(FILE *fp)
{
	int i;
	struct spdk_pci_addr *dev;

	fprintf(fp, ACCEL_ENGINE_IOAT_HEADER_TMPL);
	fprintf(fp, ACCEL_ENGINE_IOAT_ENABLE_TMPL, g_ioat_enable ? "Yes" : "No");

	for (i = 0; i < g_probe_ctx.num_whitelist_devices; i++) {
		dev = &g_probe_ctx.whitelist[i];
		fprintf(fp, ACCEL_ENGINE_IOAT_WHITELIST_TMPL,
			dev->domain, dev->bus, dev->dev, dev->func);
	}
}

SPDK_LOG_REGISTER_COMPONENT("accel_ioat", SPDK_LOG_ACCEL_IOAT)
