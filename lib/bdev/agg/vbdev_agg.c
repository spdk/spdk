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

/*
 * This is a virtual block device that consists multiple smaller bdevs.
 */

#include "spdk/stdinc.h"

#include "spdk/rpc.h"
#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/string.h"
#include "spdk/io_channel.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

static int vbdev_agg_init(void);
static void vbdev_agg_fini(void);
static void vbdev_agg_examine(struct spdk_bdev*);

static struct spdk_bdev_module agg_if = {
    .name = "agg",
    .module_init = vbdev_agg_init,
    .module_fini = vbdev_agg_fini,
    .examine = vbdev_agg_examine,
    .config_json = NULL,
};

SPDK_BDEV_MODULE_REGISTER(&agg_if)

struct agg_task {
	int    num_outstanding;
	enum spdk_bdev_io_status status;
	struct spdk_bdev_io *bdev_io;
};

struct agg_reset_task {
	struct agg_task* task;
	struct spdk_io_channel* base_channel;
};
	
#define AGG_MAX_BASE_NUM 32

#define AGG_CHUNK_SIZE (16 * 1024)
                                  
struct agg_disk {
	char* name;
	int base_dev_total;
	int base_dev_added;
	uint32_t blocklen;
	uint64_t blockcnt;
	int write_cache;
	int need_aligned_buffer;
	struct spdk_bdev* spdk_bdevs[AGG_MAX_BASE_NUM];
	struct spdk_bdev_desc* descs[AGG_MAX_BASE_NUM];
	char* bdev_names[AGG_MAX_BASE_NUM];
	struct spdk_bdev agg_vbdev;
};

static struct agg_disk g_agg_vbdev;

static int
agg_offset_to_base_dev_idx(uint64_t offset_blocks,
			   struct agg_disk *agg_disk)
{
	uint64_t agg_chunk_blocks = AGG_CHUNK_SIZE / agg_disk->blocklen;
	uint64_t agg_chunk_id = offset_blocks / agg_chunk_blocks;
	return agg_chunk_id % agg_disk->base_dev_total;
}

static uint64_t
agg_offset_to_base_dev_offset(uint64_t offset_blocks,
			      struct agg_disk *agg_disk)
{
	uint64_t agg_chunk_blocks = AGG_CHUNK_SIZE / agg_disk->blocklen;
	uint32_t agg_stripe_id = offset_blocks /
		(agg_chunk_blocks * agg_disk->base_dev_total);
	uint32_t offset_blocks_within_chunk = offset_blocks % agg_chunk_blocks;
	return agg_stripe_id * agg_chunk_blocks + offset_blocks_within_chunk;
}

static uint64_t
agg_next_chunk_boundary(uint64_t offset_blocks, struct agg_disk *agg_disk)
{
	uint64_t agg_chunk_blocks = AGG_CHUNK_SIZE / agg_disk->blocklen;
	return (offset_blocks / agg_chunk_blocks + 1) * agg_chunk_blocks;
}

static bool
agg_is_chunk_aligned(uint64_t offset_blocks, struct agg_disk *agg_disk)
{
	uint64_t agg_chunk_blocks = AGG_CHUNK_SIZE / agg_disk->blocklen;
	return offset_blocks == (offset_blocks / agg_chunk_blocks) * agg_chunk_blocks;
}

static int
agg_num_chunks(uint64_t offset_blocks, uint64_t num_blocks,
	       struct agg_disk *agg_disk){
	uint64_t agg_chunk_blocks = AGG_CHUNK_SIZE / agg_disk->blocklen;
	int num_chunks = (offset_blocks + num_blocks) / agg_chunk_blocks -
		offset_blocks / agg_chunk_blocks;
	if (!agg_is_chunk_aligned(offset_blocks + num_blocks, agg_disk)) num_chunks++;

	return num_chunks;
}

static uint64_t
agg_chop_to_align(uint64_t num_blocks){
	return num_blocks / 32 * 32;
}

static void
__agg_task_done(struct spdk_bdev_io *bdev_io, bool success, void *caller_ctx)
{
	struct agg_task* task = caller_ctx;
	if (!success) {
		SPDK_ERRLOG("agg sub task fails\n");
		task->status = SPDK_BDEV_IO_STATUS_FAILED;
	}

	if (--task->num_outstanding == 0) {
		spdk_bdev_io_complete(task->bdev_io, task->status);
		free(task);
	}

	spdk_bdev_free_io(bdev_io);
}

static void
agg_read(struct spdk_io_channel* ch, struct spdk_bdev_io *bdev_io, struct agg_disk* agg_disk)
{
	int rc;
	struct spdk_io_channel **base_channels = spdk_io_channel_get_ctx(ch);
	
	if (bdev_io->u.bdev.iovcnt == 1) {
		void * buf = bdev_io->u.bdev.iov.iov_base;
		uint64_t num_blocks = bdev_io->u.bdev.num_blocks;
		uint64_t offset_blocks = bdev_io->u.bdev.offset_blocks;

		struct agg_task *task = calloc(1, sizeof(*task));

		if (!task) {
			SPDK_ERRLOG("cannot calloc for task!\n");
			goto err_out;
		}
	
		task->bdev_io = bdev_io;
		task->num_outstanding = agg_num_chunks(offset_blocks, num_blocks, agg_disk);
		task->status = SPDK_BDEV_IO_STATUS_SUCCESS;

		while (num_blocks > 0) {
			uint64_t next_boundary = agg_next_chunk_boundary(offset_blocks, agg_disk);
			uint64_t nblocks = next_boundary - offset_blocks;
			if (num_blocks < nblocks)
				nblocks = num_blocks;

			int idx = agg_offset_to_base_dev_idx(offset_blocks, agg_disk);
			uint64_t base_offset_blocks = agg_offset_to_base_dev_offset(offset_blocks, agg_disk);

			struct spdk_bdev_desc *desc = agg_disk->descs[idx]; 
			struct spdk_io_channel *base_ch = base_channels[idx];
			
			rc = spdk_bdev_read_blocks(desc, base_ch, buf, base_offset_blocks, nblocks,
					    __agg_task_done, task);

			if (rc < 0) {
				SPDK_ERRLOG("base dev read failed!\n");
				goto err_out;
			}
			
			offset_blocks += nblocks;
			buf += nblocks * agg_disk->blocklen;
			num_blocks -= nblocks;
		}		
	} else {
		SPDK_ERRLOG("agg write does not support writev yet!\n");
		goto err_out;
	}
	return;
	
 err_out:
	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
}

static void
agg_write(struct spdk_io_channel* ch, struct spdk_bdev_io *bdev_io, struct agg_disk* agg_disk)
{
	int rc;
	struct spdk_io_channel **base_channels = spdk_io_channel_get_ctx(ch);

	if (bdev_io->u.bdev.iovcnt == 1) {
		void * buf = bdev_io->u.bdev.iov.iov_base;
		uint64_t num_blocks = bdev_io->u.bdev.num_blocks;
		uint64_t offset_blocks = bdev_io->u.bdev.offset_blocks;

		struct agg_task *task = calloc(1, sizeof(*task));

		if (!task) {
			SPDK_ERRLOG("cannot calloc for task!\n");
			goto err_out;
		}
	
		task->bdev_io = bdev_io;
		task->num_outstanding = agg_num_chunks(offset_blocks, num_blocks, agg_disk);
		task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
		
		while (num_blocks > 0) {
			uint64_t next_boundary = agg_next_chunk_boundary(offset_blocks, agg_disk);
			uint64_t nblocks = next_boundary - offset_blocks;
			if (num_blocks < nblocks)
				nblocks = num_blocks;

			int idx = agg_offset_to_base_dev_idx(offset_blocks, agg_disk);
			uint64_t base_offset_blocks = agg_offset_to_base_dev_offset(offset_blocks, agg_disk);

			struct spdk_bdev_desc *desc = agg_disk->descs[idx]; 
			struct spdk_io_channel *base_ch = base_channels[idx];

			rc = spdk_bdev_write_blocks(desc, base_ch, buf, base_offset_blocks, nblocks,
					     __agg_task_done, task);

			if (rc < 0) {
				SPDK_ERRLOG("base dev write failed!\n");
				goto err_out;
			}
					
			offset_blocks += nblocks;
			buf += nblocks * agg_disk->blocklen;
			num_blocks -= nblocks;
		}		
	} else {
		SPDK_ERRLOG("agg write does not support writev yet!\n");
		goto err_out;

	}
	return;

 err_out:
	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
}

static void
agg_unmap(struct spdk_io_channel* ch, struct spdk_bdev_io *bdev_io, struct agg_disk* agg_disk)
{
	uint64_t offset_blocks_vec[AGG_MAX_BASE_NUM];
	uint64_t num_blocks_vec[AGG_MAX_BASE_NUM];
	bool bitmap[AGG_MAX_BASE_NUM]; // mark which base devices are covered
	int rc;


	for (int i = 0; i < agg_disk->base_dev_total; i++) {
		bitmap[i] = false;
		num_blocks_vec[i] = offset_blocks_vec[i] = 0;
	}

	struct agg_task *task = calloc(1, sizeof(*task));

	if (!task) {
		SPDK_ERRLOG("cannot calloc for task!\n");
		goto err_out;
	}
	
	task->bdev_io = bdev_io;
	task->num_outstanding = 0;
	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	
	uint64_t offset_blocks = bdev_io->u.bdev.offset_blocks;
	uint64_t num_blocks = bdev_io->u.bdev.num_blocks;
	
	while (num_blocks > 0) {
		uint64_t next_boundary = agg_next_chunk_boundary(offset_blocks, agg_disk);
		uint64_t nblocks = next_boundary - offset_blocks;
		if (num_blocks < nblocks)
			nblocks = num_blocks;

		int idx = agg_offset_to_base_dev_idx(offset_blocks, agg_disk);
		uint64_t base_offset_blocks = agg_offset_to_base_dev_offset(offset_blocks, agg_disk);

		if (!bitmap[idx]) {
			bitmap[idx] = true;
			offset_blocks_vec[idx] = base_offset_blocks;
			task->num_outstanding++;
		}

		num_blocks_vec[idx] += nblocks;

		offset_blocks += nblocks;
		num_blocks -= nblocks;
	}

	struct spdk_io_channel **base_channels = spdk_io_channel_get_ctx(ch);

	for (int i = 0; i < agg_disk->base_dev_total; i++) {
		if (!bitmap[i]) continue;
		struct spdk_bdev_desc *desc = agg_disk->descs[i]; 
		struct spdk_io_channel *base_ch = base_channels[i];
		rc = spdk_bdev_unmap_blocks(desc, base_ch,
				     offset_blocks_vec[i], num_blocks_vec[i], __agg_task_done, task);

		if (rc < 0) {
			SPDK_ERRLOG("sub_bdev_io unmap failed!\n");
			goto err_out;
		}
	}
	return;
 err_out:
	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
}

static void
agg_flush(struct spdk_io_channel* ch, struct spdk_bdev_io *bdev_io, struct agg_disk* agg_disk)
{
	uint64_t offset_blocks_vec[AGG_MAX_BASE_NUM];
	uint64_t num_blocks_vec[AGG_MAX_BASE_NUM];
	bool bitmap[AGG_MAX_BASE_NUM]; // mark which base devices are covered
	int rc;

	for (int i = 0; i < agg_disk->base_dev_total; i++) {
		bitmap[i] = false;
		num_blocks_vec[i] = offset_blocks_vec[i] = 0;
	}

	struct agg_task *task = calloc(1, sizeof(*task));

	if (!task) {
		SPDK_ERRLOG("cannot calloc for task!\n");
		goto err_out;
	}
	
	task->bdev_io = bdev_io;
	task->num_outstanding = 0;
	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	
	uint64_t offset_blocks = bdev_io->u.bdev.offset_blocks;
	uint64_t num_blocks = bdev_io->u.bdev.num_blocks;
	
	while (num_blocks > 0) {
		uint64_t next_boundary = agg_next_chunk_boundary(offset_blocks, agg_disk);
		uint64_t nblocks = next_boundary - offset_blocks;
		if (num_blocks < nblocks)
			nblocks = num_blocks;

		int idx = agg_offset_to_base_dev_idx(offset_blocks, agg_disk);
		uint64_t base_offset_blocks = agg_offset_to_base_dev_offset(offset_blocks, agg_disk);

		if (!bitmap[idx]) {
			bitmap[idx] = true;
			offset_blocks_vec[idx] = base_offset_blocks;
			task->num_outstanding++;
		}

		num_blocks_vec[idx] += nblocks;

		offset_blocks += nblocks;
		num_blocks -= nblocks;
	}

	struct spdk_io_channel **base_channels = spdk_io_channel_get_ctx(ch);
	
	for (int i = 0; i < agg_disk->base_dev_total; i++) {
		if (!bitmap[i]) continue;
		struct spdk_bdev_desc *desc = agg_disk->descs[i]; 
		struct spdk_io_channel *base_ch = base_channels[i];
		rc = spdk_bdev_flush_blocks(desc, base_ch,
				     offset_blocks_vec[i], num_blocks_vec[i], __agg_task_done, task);

		if (rc < 0) {
			SPDK_ERRLOG("sub_bdev_io flush failed!\n");
			goto err_out;
		}
	}
	return;
 err_out:
	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
}

static void
__agg_reset_task_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct agg_reset_task* reset_task = cb_arg;
	struct spdk_io_channel* base_ch = reset_task->base_channel;
	spdk_put_io_channel(base_ch);
	__agg_task_done(bdev_io, success, reset_task->task);
	free(reset_task);
}

static void
agg_reset(struct spdk_io_channel* ch, struct spdk_bdev_io *bdev_io, struct agg_disk* agg_disk)
{
	struct agg_task *task = calloc(1, sizeof(*task));
	int rc;

	if (!task) {
		SPDK_ERRLOG("agg_reset: cannot allocate space for agg_task!\n");
		goto err_out;
	}
	
	task->bdev_io = bdev_io;
	task->num_outstanding = agg_disk->base_dev_total;
	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;


	struct spdk_io_channel **base_channels = spdk_io_channel_get_ctx(ch);

	for (int i = 0; i < agg_disk->base_dev_total; i++) {
		struct spdk_bdev_desc *desc = agg_disk->descs[i]; 
		struct spdk_io_channel* base_ch = base_channels[i];
		struct agg_reset_task *reset_task = calloc(1, sizeof(*reset_task));
		if (!reset_task) {
			SPDK_ERRLOG("agg_reset: cannot allocate space "
				    "for reset_task!\n");
			goto err_out;
		}
		reset_task->task = task;
		reset_task->base_channel = base_ch;
		
		rc = spdk_bdev_reset(desc, base_ch,
				     __agg_reset_task_done, reset_task);
		if (rc < 0) {
			SPDK_ERRLOG("sub_bdev_io reset failed!\n");
			goto err_out;
		}
	}
	return;
 err_out:
	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);

}

static void
agg_write_zeroes(struct spdk_io_channel* ch, struct spdk_bdev_io *bdev_io, struct agg_disk* agg_disk)
{
	uint64_t offset_blocks_vec[AGG_MAX_BASE_NUM];
	uint64_t num_blocks_vec[AGG_MAX_BASE_NUM];
	bool bitmap[AGG_MAX_BASE_NUM]; // mark which base devices are covered
	int rc;

	for (int i = 0; i < agg_disk->base_dev_total; i++) {
		bitmap[i] = false;
		num_blocks_vec[i] = offset_blocks_vec[i] = 0;
	}

	struct agg_task *task = calloc(1, sizeof(*task));

	if (!task) {
		SPDK_ERRLOG("cannot calloc for task!\n");
		goto err_out;
	}
	
	task->bdev_io = bdev_io;
	task->num_outstanding = 0;
	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	
	uint64_t offset_blocks = bdev_io->u.bdev.offset_blocks;
	uint64_t num_blocks = bdev_io->u.bdev.num_blocks;
	
	while (num_blocks > 0) {
		uint64_t next_boundary = agg_next_chunk_boundary(offset_blocks, agg_disk);
		uint64_t nblocks = next_boundary - offset_blocks;
		if (num_blocks < nblocks)
			nblocks = num_blocks;

		int idx = agg_offset_to_base_dev_idx(offset_blocks, agg_disk);
		uint64_t base_offset_blocks = agg_offset_to_base_dev_offset(offset_blocks, agg_disk);

		if (!bitmap[idx]) {
			bitmap[idx] = true;
			offset_blocks_vec[idx] = base_offset_blocks;
			task->num_outstanding++;
		}

		num_blocks_vec[idx] += nblocks;

		offset_blocks += nblocks;
		num_blocks -= nblocks;
	}

	struct spdk_io_channel **base_channels = spdk_io_channel_get_ctx(ch);
	
	for (int i = 0; i < agg_disk->base_dev_total; i++) {
		if (!bitmap[i]) continue;
		struct spdk_bdev_desc *desc = agg_disk->descs[i]; 
		struct spdk_io_channel *base_ch = base_channels[i];
		rc = spdk_bdev_write_zeroes_blocks(desc, base_ch,
				     offset_blocks_vec[i], num_blocks_vec[i], __agg_task_done, task);

		if (rc < 0) {
			SPDK_ERRLOG("sub_bdev_io write_zeroes failed!\n");
			goto err_out;
		}
	}
	return;
 err_out:
	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
}

static void
vbdev_agg_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct agg_disk *vbdev = bdev_io->bdev->ctxt;

	/* Modify the I/O to adjust for the offset within the base bdev. */
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		agg_read(ch, bdev_io, vbdev);
		return;
	case SPDK_BDEV_IO_TYPE_WRITE:
		agg_write(ch, bdev_io, vbdev);
		return;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		agg_unmap(ch, bdev_io, vbdev);
		return;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		agg_flush(ch, bdev_io, vbdev);
		return;
	case SPDK_BDEV_IO_TYPE_RESET:
		agg_reset(ch, bdev_io, vbdev);
		return;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		agg_write_zeroes(ch, bdev_io, vbdev);
		return;
	default:
		SPDK_ERRLOG("agg: unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
}

static int
vbdev_agg_destruct(void *ctx)
{
	/* what does this fuction supposed to do? */
	return 0;
}

static void
vbdev_agg_base_bdev_hotremove_cb(void *remove_ctx)
{
	struct spdk_bdev* bdev = remove_ctx;
	SPDK_NOTICELOG("base dev %s got removed!!\n",
		    bdev->name);
	/*what action should be taken? */
}

static bool
vbdev_agg_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct agg_disk *vbdev = ctx;
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_READ:
		/* return true if all base supported */
		for (int i = 0; i < vbdev->base_dev_total; i++) {
			struct spdk_bdev *base_bdev = vbdev->spdk_bdevs[i];
			if (!base_bdev->fn_table->io_type_supported(base_bdev->ctxt, io_type))
				return false;
		}
		return true;
	default:
		return false;
	}
}

static struct spdk_io_channel *
vbdev_agg_get_io_channel(void *ctx)
{
	struct agg_disk *agg_disk = ctx;
	return spdk_get_io_channel(agg_disk);
}

static int
vbdev_agg_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct agg_disk *vbdev = ctx;
	struct spdk_bdev *agg_vbdev = &vbdev->agg_vbdev;

	spdk_json_write_name(w, "agg");
	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "agg_vbdev");
	spdk_json_write_string(w, spdk_bdev_get_name(agg_vbdev));

	for (int i = 0; i < vbdev->base_dev_total; i++) {
		struct spdk_bdev *base_bdev = vbdev->spdk_bdevs[i];
		spdk_json_write_name(w, "base_bdev");
		spdk_json_write_string(w, spdk_bdev_get_name(base_bdev));
	}

	spdk_json_write_object_end(w);

	return 0;
}

static void
vbdev_agg_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	/*TODO. What does this function do? */
}

static struct spdk_bdev_fn_table vbdev_agg_fn_table = {
	.destruct		= vbdev_agg_destruct,
	.io_type_supported	= vbdev_agg_io_type_supported,
	.submit_request		= vbdev_agg_submit_request,
	.get_io_channel		= vbdev_agg_get_io_channel,
	.dump_info_json	        = vbdev_agg_dump_info_json,
	.write_config_json	= vbdev_agg_write_config_json,
};

/*
 * Basic Logic: read conf, record all the NVMe devices that to be 
 * aggregated.
 */
static int
vbdev_agg_init(void)
{
	struct spdk_conf_section *sp;
	int i, rc = 0, base_dev_cnt;
	
	sp = spdk_conf_find_section(NULL, "Agg");
	if (sp == NULL) {
		return 0;
	}
       
	for (i = 0; ; i++) {
		if (!spdk_conf_section_get_nval(sp, "VBDev", i)) {
			break;
		}

		if (i) {
			SPDK_ERRLOG("current we only support one VBDev defined.\n");
			return -1;
		}

		char* vbdev_name = spdk_conf_section_get_nmval(sp, "VBDev", i, 0);

		if (!vbdev_name) {
			SPDK_ERRLOG("Agg configuration missing vbdev name\n");
			continue;
		}
		
		for(base_dev_cnt = 0;; base_dev_cnt++){
			char* base_dev_name =
				spdk_conf_section_get_nmval(sp, "VBDev",
							    i, 1 + base_dev_cnt);
			if (!base_dev_name) break;
			g_agg_vbdev.bdev_names[base_dev_cnt] =
				spdk_sprintf_alloc("%s", base_dev_name);
			if (!g_agg_vbdev.bdev_names[base_dev_cnt]) {
				rc = -ENOMEM;
				goto free_bdev_names;
			}
		}

		if (!base_dev_cnt) {
			SPDK_ERRLOG("Agg configuration missing base bdev for vbdev %s\n",
				    vbdev_name);
			continue;
		}

		g_agg_vbdev.name = spdk_sprintf_alloc("%s", vbdev_name);
		if (!g_agg_vbdev.name) {
			rc = -ENOMEM;
			goto free_bdev_names;
		}


		g_agg_vbdev.base_dev_total = base_dev_cnt;
		g_agg_vbdev.base_dev_added = 0;
	}

	return 0;
	
 free_bdev_names:
	for (i = 0; i < base_dev_cnt; i++)
		free(g_agg_vbdev.bdev_names[i]);

	return rc;
}


static int
vbdev_agg_open_and_claim_base(struct agg_disk* agg_disk) {
	int i, rc;
	for (i = 0; i < g_agg_vbdev.base_dev_total; i++) {
		rc = spdk_bdev_open(agg_disk->spdk_bdevs[i], false,
				    vbdev_agg_base_bdev_hotremove_cb,
				    agg_disk->spdk_bdevs[i],
				    &agg_disk->descs[i]);
		if (rc) {
			SPDK_ERRLOG("cannot open bdev: %s\n",
				    spdk_bdev_get_name(agg_disk->spdk_bdevs[i]));
			return -1;
		}

		rc = spdk_bdev_module_claim_bdev(agg_disk->spdk_bdevs[i],
						 agg_disk->descs[i],
						 agg_disk->agg_vbdev.module);

		if (rc) {
			SPDK_ERRLOG("cannot claim bdev: %s\n",
				    spdk_bdev_get_name(agg_disk->spdk_bdevs[i]));
			spdk_bdev_close(agg_disk->descs[i]);
			return -1;
		}
	}
	return 0;
}

static void
vbdev_agg_close_and_release_base(struct agg_disk* agg_disk) {
	for (int i = 0; i < g_agg_vbdev.base_dev_total; i++) {
		spdk_bdev_module_release_bdev(agg_disk->spdk_bdevs[i]);
		spdk_bdev_close(agg_disk->descs[i]);
	}
}

static int
vbdev_agg_create_cb(void *io_device, void *ctx_buf){
	struct spdk_io_channel **base_channels = ctx_buf;
	struct agg_disk *agg_disk = io_device;

	for(int i = 0; i < agg_disk->base_dev_total; i++){
		base_channels[i] = spdk_bdev_get_io_channel(agg_disk->descs[i]);
		if (base_channels[i] == NULL) {
			SPDK_ERRLOG("cannot get_io_channel for base bdev[%d] %s\n",
				    i, spdk_bdev_get_name(agg_disk->spdk_bdevs[i]));
			return -1;
		}
	}
	return 0;
}

static void
vbdev_agg_destroy_cb(void *io_device, void *ctx_buf){
	struct spdk_io_channel **base_channels = ctx_buf;
	struct agg_disk *agg_disk = io_device;

	for(int i = 0; i < agg_disk->base_dev_total; i++)
		spdk_put_io_channel(base_channels[i]);
}

static void
vbdev_agg_examine(struct spdk_bdev *bdev)
{
	struct spdk_conf_section *sp;
	int i;
	
	sp = spdk_conf_find_section(NULL, "Agg");
	if (sp == NULL) {
		spdk_bdev_module_examine_done(&agg_if);
		return;
	}
	
	bool match = false;
	for (i = 0; i < g_agg_vbdev.base_dev_total; i++) {
		if (!strncmp(bdev->name, g_agg_vbdev.bdev_names[i],
			    strlen(g_agg_vbdev.bdev_names[i]))) {
			match = true;
			break;
		}
	}

	if (!match) {
		goto out;
	}

	if (g_agg_vbdev.base_dev_added >= g_agg_vbdev.base_dev_total) {
		SPDK_ERRLOG("More SSDs (%d) found than needed in "
			    "conf file (%d)!\n",
			    g_agg_vbdev.base_dev_added,
			    g_agg_vbdev.base_dev_total);
	}

	g_agg_vbdev.spdk_bdevs[i] = bdev;
	uint64_t new_blockcnt = agg_chop_to_align(bdev->blockcnt);

	
	if (!g_agg_vbdev.base_dev_added) {
	    g_agg_vbdev.base_dev_added++;
	    g_agg_vbdev.blockcnt = new_blockcnt;
	} else {
	    uint64_t min_base_blockcnt = g_agg_vbdev.blockcnt /
		g_agg_vbdev.base_dev_added;

	    if (min_base_blockcnt > new_blockcnt)
		min_base_blockcnt = new_blockcnt;
	
	    g_agg_vbdev.base_dev_added++;
	    g_agg_vbdev.blockcnt = min_base_blockcnt *
		g_agg_vbdev.base_dev_added;

	}
	
	if (!g_agg_vbdev.blocklen)
	    g_agg_vbdev.blocklen = bdev->blocklen;
	else if (g_agg_vbdev.blocklen != bdev->blocklen) {
		SPDK_ERRLOG("current version requires all SSD having the same blocklen\n"
			    "info.blocklen=%d bdev->blocklen=%d\n",
			    g_agg_vbdev.blocklen,
			    bdev->blocklen);
		goto out;
	}

	g_agg_vbdev.write_cache |= bdev->write_cache;
	g_agg_vbdev.need_aligned_buffer |= bdev->need_aligned_buffer;

	if (g_agg_vbdev.base_dev_added == g_agg_vbdev.base_dev_total) {

		if (vbdev_agg_open_and_claim_base(&g_agg_vbdev)) {
			SPDK_ERRLOG("cannot agg_open_and_claim_base!\n");
			goto out;
		}
		
		struct spdk_bdev* agg_vbdev = &g_agg_vbdev.agg_vbdev;
		
		/* Copy properties of the base bdev */
		agg_vbdev->blocklen = g_agg_vbdev.blocklen;
		agg_vbdev->write_cache = g_agg_vbdev.write_cache;
		agg_vbdev->need_aligned_buffer = g_agg_vbdev.need_aligned_buffer;

		agg_vbdev->ctxt = &g_agg_vbdev;
		agg_vbdev->name = spdk_sprintf_alloc("%s", g_agg_vbdev.name);
		if (!agg_vbdev->name) {
			SPDK_ERRLOG("ERROR NOMEM\n");
			goto close_release_base;
		}

		agg_vbdev->product_name = "Agg Disk";
		
		/*should be the total.*/
		agg_vbdev->blockcnt = g_agg_vbdev.blockcnt; 

		agg_vbdev->fn_table = &vbdev_agg_fn_table;
		agg_vbdev->module = &agg_if;

		spdk_io_device_register(&g_agg_vbdev, vbdev_agg_create_cb,
					vbdev_agg_destroy_cb,
					sizeof(struct spdk_io_channel*) *
					g_agg_vbdev.base_dev_total);
		spdk_vbdev_register(agg_vbdev, g_agg_vbdev.spdk_bdevs,
				    g_agg_vbdev.base_dev_total);

		SPDK_NOTICELOG("total_size %luGB (%d base devices)\n",
			    agg_vbdev->blockcnt * agg_vbdev->blocklen / 1024 / 1024 / 1024,
			    g_agg_vbdev.base_dev_added);
	}

 out:
	spdk_bdev_module_examine_done(&agg_if);
	return;

 close_release_base:
	vbdev_agg_close_and_release_base(&g_agg_vbdev);
	spdk_bdev_module_examine_done(&agg_if);
	return;
}

static void
vbdev_agg_fini(void)
{
	free(g_agg_vbdev.name);
	for (int i = 0; i < g_agg_vbdev.base_dev_total; i++)
		free(g_agg_vbdev.bdev_names[i]);
}
