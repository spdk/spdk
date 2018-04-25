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

#include "spdk/bdev_user.h"

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/fd.h"
#include "spdk/io_channel.h"
#include "spdk/event.h"
#include "spdk_internal/log.h"
#include "spdk_internal/bdev.h"
#include "spdk/event.h"

#define BDEV_USER_DEBUG

#ifndef BDEV_USER_DEBUG
#define ASSERT_DEBUG()
#else
#define ASSERT_DEBUG() assert(0)
#endif

#define BLOCKDEV_USER_BATCH_SIZE 8
#define BDEV_USER_QUEUE_DEPTH (4096)

struct bdev_user_io_channel {
	long queue_depth;
	struct spdk_poller *poller;
	struct spdk_ring *cq;
};

struct bdev_user_disk {
	void *user_ctxt;
	struct spdk_bdev disk;
};

static int bdev_user_initialize(void);
static void bdev_user_get_spdk_running_config(FILE *fp);
static void free_user_disk(struct bdev_user_disk *udisk);

static struct bdev_user_fn_table g_user_fn_table;

static struct spdk_bdev_module user_if = {
	.name		= "user",
	.module_init	= bdev_user_initialize,
	.module_fini	= NULL,
	.config_text	= bdev_user_get_spdk_running_config,
	.get_ctx_size	= NULL, /* Not sure about this, or how it can be used */
	.examine	= NULL,
};

SPDK_BDEV_MODULE_REGISTER(&user_if);

int
bdev_user_destruct(void *ctx)
{
	struct bdev_user_disk *udisk = ctx;
	int rc = 0;

	free_user_disk(udisk);
	return rc;
}

static int
bdev_user_initialize_io_channel(struct bdev_user_io_channel *ch)
{
	int rc = 0;
	ch->queue_depth = BDEV_USER_QUEUE_DEPTH;

	ch->cq = spdk_ring_create(SPDK_RING_TYPE_MP_SC,
			ch->queue_depth,
			spdk_env_get_socket_id(spdk_env_get_current_core()));
	if (!ch->cq) {
		SPDK_ERRLOG("Unable to allocate completion queue for user IO "
			"channel\n");
		ASSERT_DEBUG();
		rc = -1;
		goto out;
	}

out:
	return 0;
}

static int
bdev_user_poll(void *arg)
{
	struct bdev_user_io_channel *ch = arg;
	size_t res_count, i;
	size_t count = BLOCKDEV_USER_BATCH_SIZE;
	struct spdk_bdev_io *bdev_io[BLOCKDEV_USER_BATCH_SIZE] = {0};

	res_count = spdk_ring_dequeue(ch->cq, (void **)bdev_io, count);

	for (i = 0; i < res_count; i++) {
		if (!bdev_io[i]) {
			SPDK_WARNLOG("Empty completion message, there may be"
				"an error in the completion path, and IO may "
				"time out");
			continue;
		}

		switch (bdev_io[i]->type) {
		case SPDK_BDEV_IO_TYPE_READ:
		case SPDK_BDEV_IO_TYPE_WRITE:
			spdk_bdev_io_complete_nvme_status(bdev_io[i],
				bdev_io[i]->error.nvme.sct,
				bdev_io[i]->error.nvme.sc);
			break;
		default:
			SPDK_ERRLOG("Invlaid type %d, only READ/WRITE are "
				"supported, bdev_io may be corrupted",
				bdev_io[i]->type);
		}
	}

	/* TODO - should the res_count take into account error events (i.e.
	 * empty or corrupt bdev) */
	return res_count;
}

static void
bdev_user_submit_request(struct spdk_io_channel *ch,
	struct spdk_bdev_io *bdev_io)
{
	int rc;
	struct bdev_user_disk *bdev_user = bdev_io->bdev->ctxt;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		/* TODO - this interface should be cleaned up so that the user
		 * does not know anything about the bdev_io except that it is
		 * the return context. E.G. read(buf, size, bdev_io).
		 * an alternative can be to pass the NVMeF header + buffer so
		 * that non read/write commands can be handled, or have all 3
		 * (read/write for simple IO, and submit_request(nvme_header,
		 * buf, size) for more advanced IO processing) */
		rc = g_user_fn_table.submit_request(bdev_user->user_ctxt, (void *)bdev_io);
		break;
	default:
		SPDK_ERRLOG("Invlaid type %d, only READ/WRITE are supported.",
			bdev_io->type);
		rc = -1;
	}

	if (rc < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}

	return;
}

static bool
bdev_user_io_type_supported(void *ctx,
	enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return true;

	default:
		return false;
	}
}

static int
bdev_user_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_user_io_channel *ch = ctx_buf;
	int rc;

	rc = bdev_user_initialize_io_channel(ch);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to initialize user IO channel for io_device"
			" %p", io_device);
		goto out;
	}

	ch->poller = spdk_poller_register(bdev_user_poll, ch, 0);

out:
	return 0;
}

static void
bdev_user_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_user_io_channel *ch = ctx_buf;

	spdk_ring_free(ch->cq);
	spdk_poller_unregister(&ch->poller);
}

static struct
spdk_io_channel *bdev_user_get_io_channel(void *ctx)
{
	struct bdev_user_disk *udisk = ctx;

	return spdk_get_io_channel(udisk);
}

static const struct spdk_bdev_fn_table bdev_user_fn_table = {
	.destruct		= bdev_user_destruct,
	.submit_request		= bdev_user_submit_request,
	.io_type_supported	= bdev_user_io_type_supported,
	.get_io_channel		= bdev_user_get_io_channel,
	.dump_info_json		= NULL, /* TODO */
	.write_config_json	= NULL, /* TODO */
};

static void
free_user_disk(struct bdev_user_disk *udisk)
{
	if (udisk == NULL)
		return;

	spdk_io_device_unregister(udisk, NULL);
	free(udisk->disk.name);
	free(udisk);
}

static void
bdev_user_register_device_evt(void *arg1, void *arg2)
{
	struct bdev_user_disk *udisk = arg1;

	spdk_io_device_register(udisk, bdev_user_create_cb,
		bdev_user_destroy_cb,
		sizeof(struct bdev_user_io_channel));
	spdk_bdev_register(&udisk->disk);

	return;
}

int
bdev_user_register_device(const char *name, uint64_t size_in_gb,
	uint32_t reactor_core, void *user_ctxt)
{
	struct spdk_event *event;
	struct bdev_user_disk *udisk;
	int rc = 0;

	udisk = calloc(sizeof(*udisk), 1);
	if (!udisk) {
		SPDK_ERRLOG("Unable to allocate enough memory for user dev\n");
		rc = -ENOMEM;
		goto out;
	}

	udisk->user_ctxt = user_ctxt;

	udisk->disk.module = &user_if;
	udisk->disk.name = strdup(name);
	if (!udisk->disk.name) {
		SPDK_ERRLOG("Unable to allocate enough memory to copy device "
			"name\n");
		rc = -ENOMEM;
		goto error_return;
	}
	udisk->disk.product_name = "USER disk";

	/* TODO - get all this from user, don't use defaults! */
	udisk->disk.need_aligned_buffer = 1;
	udisk->disk.write_cache = 1;
	udisk->disk.blocklen = 4096;
	udisk->disk.blockcnt = (size_in_gb * 1024 * 1024 * 1024L) / udisk->disk.blocklen;
	udisk->disk.ctxt = udisk;

	udisk->disk.fn_table = &bdev_user_fn_table;

	event = spdk_event_allocate(reactor_core,
		bdev_user_register_device_evt, udisk, NULL);
	spdk_event_call(event);

out:
	return rc;

error_return:
	free_user_disk(udisk);
	return rc;
}

/* TODO - should this be initialized as a singleton, we do not want this changed
 * during module operation, we can also use the function table per device so
 * that different devices can have different behaviours */
void
bdev_user_register_fn_table(struct bdev_user_fn_table *user_fn_table)
{
	g_user_fn_table.submit_request = user_fn_table->submit_request;
}

/* TODO - We need a richer interface in that the user needs to be able to convey
 * different error codes. In addition, the user needs to be able to pass DNR bit
 * if the error is unretryable.
 */
int
bdev_user_submit_completion(struct spdk_bdev_io *bdev_io,
	bool is_success)
{
	size_t res_count;
	size_t count = 1;
	struct bdev_user_io_channel *user_ch = spdk_bdev_io_get_ctxt(bdev_io);

	bdev_io->error.nvme.sct = SPDK_NVME_SCT_GENERIC;
	if (is_success) {
		bdev_io->error.nvme.sc = SPDK_NVME_SC_SUCCESS;
	} else {
		bdev_io->error.nvme.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
	}

	res_count = spdk_ring_enqueue(user_ch->cq, (void **)&bdev_io, count);

	assert(res_count == count);

	return 0;
}

static int
bdev_user_initialize(void)
{
	struct spdk_conf_section *sp;

	sp = spdk_conf_find_section(NULL, "USER");
	if (sp) {
		SPDK_ERRLOG("No need to create USER volumes in conf files");
		SPDK_ERRLOG("User volumes are created by the user application "
			"so if you are defining them in the conf, you are doing"
			" something wrong!");
	}

	return 0;
}

static void
bdev_user_get_spdk_running_config(FILE *fp)
{
	/* TODO - show current registered application devices */
	fprintf(fp, "\n");
}

SPDK_LOG_REGISTER_COMPONENT("user", SPDK_LOG_USER)
