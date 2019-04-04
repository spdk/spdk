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

#include "spdk/bdev.h"
#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/string.h"
#include "spdk/likely.h"

#include "spdk/bdev_module.h"
#include "spdk_internal/log.h"

#include "bdev_null.h"

struct null_bdev {
	struct spdk_bdev	bdev;
	TAILQ_ENTRY(null_bdev)	tailq;
};

struct null_io_channel {
	struct spdk_poller		*poller;
	TAILQ_HEAD(, spdk_bdev_io)	io;
};

static TAILQ_HEAD(, null_bdev) g_null_bdev_head;
static void *g_null_read_buf;

static int bdev_null_initialize(void);
static void bdev_null_finish(void);
static void bdev_null_get_spdk_running_config(FILE *fp);

static struct spdk_bdev_module null_if = {
	.name = "null",
	.module_init = bdev_null_initialize,
	.module_fini = bdev_null_finish,
	.config_text = bdev_null_get_spdk_running_config,
	.async_fini = true,
};

SPDK_BDEV_MODULE_REGISTER(null, &null_if)

static int
bdev_null_destruct(void *ctx)
{
	struct null_bdev *bdev = ctx;

	TAILQ_REMOVE(&g_null_bdev_head, bdev, tailq);
	free(bdev->bdev.name);
	free(bdev);

	return 0;
}

static void
bdev_null_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct null_io_channel *ch = spdk_io_channel_get_ctx(_ch);

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		if (bdev_io->u.bdev.iovs[0].iov_base == NULL) {
			assert(bdev_io->u.bdev.iovcnt == 1);
			if (spdk_likely(bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen <=
					SPDK_BDEV_LARGE_BUF_MAX_SIZE)) {
				bdev_io->u.bdev.iovs[0].iov_base = g_null_read_buf;
				bdev_io->u.bdev.iovs[0].iov_len = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;
			} else {
				SPDK_ERRLOG("Overflow occurred. Read I/O size %" PRIu64 " was larger than permitted %d\n",
					    bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
					    SPDK_BDEV_LARGE_BUF_MAX_SIZE);
				spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
		}
		TAILQ_INSERT_TAIL(&ch->io, bdev_io, module_link);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_RESET:
		TAILQ_INSERT_TAIL(&ch->io, bdev_io, module_link);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	default:
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}

static bool
bdev_null_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_RESET:
		return true;
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	default:
		return false;
	}
}

static struct spdk_io_channel *
bdev_null_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(&g_null_bdev_head);
}

static void
bdev_null_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	char uuid_str[SPDK_UUID_STRING_LEN];

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "construct_null_bdev");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_uint64(w, "num_blocks", bdev->blockcnt);
	spdk_json_write_named_uint32(w, "block_size", bdev->blocklen);
	spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &bdev->uuid);
	spdk_json_write_named_string(w, "uuid", uuid_str);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table null_fn_table = {
	.destruct		= bdev_null_destruct,
	.submit_request		= bdev_null_submit_request,
	.io_type_supported	= bdev_null_io_type_supported,
	.get_io_channel		= bdev_null_get_io_channel,
	.write_config_json	= bdev_null_write_config_json,
};

struct spdk_bdev *
create_null_bdev(const char *name, const struct spdk_uuid *uuid,
		 uint64_t num_blocks, uint32_t block_size)
{
	struct null_bdev *bdev;
	int rc;

	if (block_size % 512 != 0) {
		SPDK_ERRLOG("Block size %u is not a multiple of 512.\n", block_size);
		return NULL;
	}

	if (num_blocks == 0) {
		SPDK_ERRLOG("Disk must be more than 0 blocks\n");
		return NULL;
	}

	bdev = calloc(1, sizeof(*bdev));
	if (!bdev) {
		SPDK_ERRLOG("could not allocate null_bdev\n");
		return NULL;
	}

	bdev->bdev.name = strdup(name);
	if (!bdev->bdev.name) {
		free(bdev);
		return NULL;
	}
	bdev->bdev.product_name = "Null disk";

	bdev->bdev.write_cache = 0;
	bdev->bdev.blocklen = block_size;
	bdev->bdev.blockcnt = num_blocks;
	if (uuid) {
		bdev->bdev.uuid = *uuid;
	} else {
		spdk_uuid_generate(&bdev->bdev.uuid);
	}

	bdev->bdev.ctxt = bdev;
	bdev->bdev.fn_table = &null_fn_table;
	bdev->bdev.module = &null_if;

	rc = spdk_bdev_register(&bdev->bdev);
	if (rc) {
		free(bdev->bdev.name);
		free(bdev);
		return NULL;
	}

	TAILQ_INSERT_TAIL(&g_null_bdev_head, bdev, tailq);

	return &bdev->bdev;
}

void
delete_null_bdev(struct spdk_bdev *bdev, spdk_delete_null_complete cb_fn, void *cb_arg)
{
	if (!bdev || bdev->module != &null_if) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	spdk_bdev_unregister(bdev, cb_fn, cb_arg);
}

static int
null_io_poll(void *arg)
{
	struct null_io_channel		*ch = arg;
	TAILQ_HEAD(, spdk_bdev_io)	io;
	struct spdk_bdev_io		*bdev_io;

	TAILQ_INIT(&io);
	TAILQ_SWAP(&ch->io, &io, spdk_bdev_io, module_link);

	if (TAILQ_EMPTY(&io)) {
		return 0;
	}

	while (!TAILQ_EMPTY(&io)) {
		bdev_io = TAILQ_FIRST(&io);
		TAILQ_REMOVE(&io, bdev_io, module_link);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	}

	return 1;
}

static int
null_bdev_create_cb(void *io_device, void *ctx_buf)
{
	struct null_io_channel *ch = ctx_buf;

	TAILQ_INIT(&ch->io);
	ch->poller = spdk_poller_register(null_io_poll, ch, 0);

	return 0;
}

static void
null_bdev_destroy_cb(void *io_device, void *ctx_buf)
{
	struct null_io_channel *ch = ctx_buf;

	spdk_poller_unregister(&ch->poller);
}

static void
_bdev_null_cleanup_cb(void *arg)
{
	spdk_free(g_null_read_buf);
}

static int
bdev_null_initialize(void)
{
	struct spdk_conf_section *sp = spdk_conf_find_section(NULL, "Null");
	uint64_t size_in_mb, num_blocks;
	int block_size, i, rc = 0;
	struct spdk_bdev *bdev;
	const char *name, *val;

	TAILQ_INIT(&g_null_bdev_head);

	/*
	 * This will be used if upper layer expects us to allocate the read buffer.
	 *  Instead of using a real rbuf from the bdev pool, just always point to
	 *  this same zeroed buffer.
	 */
	g_null_read_buf = spdk_zmalloc(SPDK_BDEV_LARGE_BUF_MAX_SIZE, 0, NULL,
				       SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);

	/*
	 * We need to pick some unique address as our "io device" - so just use the
	 *  address of the global tailq.
	 */
	spdk_io_device_register(&g_null_bdev_head, null_bdev_create_cb, null_bdev_destroy_cb,
				sizeof(struct null_io_channel),
				"null_bdev");

	if (sp == NULL) {
		goto end;
	}

	i = 0;
	while (true) {
		val = spdk_conf_section_get_nval(sp, "Dev", i);
		if (val == NULL) {
			break;
		}

		name = spdk_conf_section_get_nmval(sp, "Dev", i, 0);
		if (name == NULL) {
			SPDK_ERRLOG("Null entry %d: Name must be provided\n", i);
			continue;
		}

		val = spdk_conf_section_get_nmval(sp, "Dev", i, 1);
		if (val == NULL) {
			SPDK_ERRLOG("Null entry %d: Size in MB must be provided\n", i);
			continue;
		}

		errno = 0;
		size_in_mb = strtoull(val, NULL, 10);
		if (errno) {
			SPDK_ERRLOG("Null entry %d: Invalid size in MB %s\n", i, val);
			continue;
		}

		val = spdk_conf_section_get_nmval(sp, "Dev", i, 2);
		if (val == NULL) {
			block_size = 512;
		} else {
			block_size = (int)spdk_strtol(val, 10);
			if (block_size <= 0) {
				SPDK_ERRLOG("Null entry %d: Invalid block size %s\n", i, val);
				continue;
			}
		}

		num_blocks = size_in_mb * (1024 * 1024) / block_size;

		bdev = create_null_bdev(name, NULL, num_blocks, block_size);
		if (bdev == NULL) {
			SPDK_ERRLOG("Could not create null bdev\n");
			rc = EINVAL;
			goto cleanup;
		}

		i++;
	}
cleanup:
	spdk_io_device_unregister(&g_null_bdev_head, _bdev_null_cleanup_cb);
end:
	return rc;
}

static void
_bdev_null_finish_cb(void *arg)
{
	spdk_free(g_null_read_buf);
	spdk_bdev_module_finish_done();
}

static void
bdev_null_finish(void)
{
	spdk_io_device_unregister(&g_null_bdev_head, _bdev_null_finish_cb);
}

static void
bdev_null_get_spdk_running_config(FILE *fp)
{
	struct null_bdev *bdev;
	uint64_t null_bdev_size;

	fprintf(fp, "\n[Null]\n");

	TAILQ_FOREACH(bdev, &g_null_bdev_head, tailq) {
		null_bdev_size = bdev->bdev.blocklen * bdev->bdev.blockcnt;
		null_bdev_size /= (1024 * 1024);
		fprintf(fp, "  %s %" PRIu64 " %d\n",
			bdev->bdev.name, null_bdev_size, bdev->bdev.blocklen);
	}
}

SPDK_LOG_REGISTER_COMPONENT("bdev_null", SPDK_LOG_BDEV_NULL)
