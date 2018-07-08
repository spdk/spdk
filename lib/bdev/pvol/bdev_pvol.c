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

#include "bdev_pvol.h"
#include "spdk/env.h"
#include "spdk/io_channel.h"
#include "spdk/conf.h"
#include "spdk_internal/log.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/json.h"
#include "spdk/string.h"

/* pvol config as read from config file */
struct pvol_config          spdk_pvol_config;

/*
 * List of pvol bdev in configured list, these pvol bdevs are registered with
 * bdev layer
 */
struct spdk_pvol_configured_tailq       spdk_pvol_bdev_configured_list;

/* List of pvol bdev in configuring list */
struct spdk_pvol_configuring_tailq      spdk_pvol_bdev_configuring_list;

/* List of all pvol bdevs */
struct spdk_pvol_all_tailq              spdk_pvol_bdev_list;

/* List of all pvol bdevs that are offline */
struct spdk_pvol_offline_tailq          spdk_pvol_bdev_offline_list;

/* Function declarations */
static void   pvol_bdev_examine(struct spdk_bdev *bdev);
static int    pvol_bdev_init(void);
static void   pvol_bdev_waitq_io_process(void *ctx);


/*
 * brief:
 * pvol_bdev_create_cb function is a cb function for pvol which creates the
 * hierarchy from pvol to base bdev io channels. It will be called per core
 * params:
 * io_device - pointer to pvol io device represented by pvol_bdev
 * ctx_buf - pointer to context buffer for pvol bdev io channel
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
pvol_bdev_create_cb(void *io_device, void *ctx_buf)
{
	struct pvol_bdev            *pvol_bdev = io_device;
	struct pvol_bdev_io_channel *ch = ctx_buf;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_PVOL, "pvol_bdev_create_cb, %p\n", ch);

	assert(pvol_bdev != NULL);
	assert(pvol_bdev->state == PVOL_BDEV_STATE_ONLINE);

	/*
	 * Store pvol_bdev_ctxt in each channel which is used to get the read only
	 * pvol bdev specific information during io split logic like base bdev
	 * descriptors, strip size etc
	 */
	ch->pvol_bdev_ctxt = SPDK_CONTAINEROF(pvol_bdev, struct pvol_bdev_ctxt, pvol_bdev);

	ch->base_bdevs_io_channel = calloc(ch->pvol_bdev_ctxt->pvol_bdev.num_base_bdevs,
					   sizeof(struct spdk_io_channel *));
	if (!ch->base_bdevs_io_channel) {
		SPDK_ERRLOG("Unable to allocate base bdevs io channel\n");
		return -1;
	}
	for (uint32_t iter = 0; iter < ch->pvol_bdev_ctxt->pvol_bdev.num_base_bdevs; iter++) {
		/*
		 * Get the spdk_io_channel for all the base bdevs. This is used during
		 * split logic to send the respective child bdev ios to respective base
		 * bdev io channel.
		 */
		ch->base_bdevs_io_channel[iter] = spdk_bdev_get_io_channel(
				pvol_bdev->base_bdev_info[iter].base_bdev_desc);
		if (!ch->base_bdevs_io_channel[iter]) {
			for (uint32_t iter1 = 0; iter1 < iter ; iter1++) {
				spdk_put_io_channel(ch->base_bdevs_io_channel[iter1]);
			}
			free(ch->base_bdevs_io_channel);
			SPDK_ERRLOG("Unable to create io channel for base bdev\n");
			return -1;
		}
	}

	return 0;
}

/*
 * brief:
 * pvol_bdev_destroy_cb function is a cb function for pvol which deletes the
 * hierarchy from pvol to base bdev io channels. It will be called per core
 * params:
 * io_device - pointer to pvol io device represented by pvol_bdev
 * ctx_buf - pointer to context buffer for pvol bdev io channel
 * returns:
 * none
 */
static void
pvol_bdev_destroy_cb(void *io_device, void *ctx_buf)
{
	struct pvol_bdev_io_channel *ch = ctx_buf;
	struct pvol_bdev            *pvol_bdev = io_device;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_PVOL, "pvol_bdev_destroy_cb\n");

	assert(pvol_bdev != NULL);
	assert(ch != NULL);
	assert(ch->base_bdevs_io_channel);
	for (uint32_t iter = 0; iter < pvol_bdev->num_base_bdevs; iter++) {
		/* Free base bdev channels */
		assert(ch->base_bdevs_io_channel[iter] != NULL);
		spdk_put_io_channel(ch->base_bdevs_io_channel[iter]);
		ch->base_bdevs_io_channel[iter] = NULL;
	}
	ch->pvol_bdev_ctxt = NULL;
	free(ch->base_bdevs_io_channel);
	ch->base_bdevs_io_channel = NULL;
}

/*
 * brief:
 * pvol_bdev_cleanup is used to cleanup and free pvol_bdev related data
 * structures.
 * params:
 * pvol_bdev_ctxt - pointer to pvol_bdev_ctxt
 * returns:
 * none
 */
static void
pvol_bdev_cleanup(struct pvol_bdev_ctxt *pvol_bdev_ctxt)
{
	struct pvol_bdev *pvol_bdev = &pvol_bdev_ctxt->pvol_bdev;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_PVOL, "pvol_bdev_cleanup, %p name %s, state %u, pvol_bdev_config %p\n",
		      pvol_bdev_ctxt,
		      pvol_bdev_ctxt->bdev.name, pvol_bdev->state, pvol_bdev->pvol_bdev_config);
	if (pvol_bdev->state == PVOL_BDEV_STATE_CONFIGURING) {
		TAILQ_REMOVE(&spdk_pvol_bdev_configuring_list, pvol_bdev, link_specific_list);
	} else if (pvol_bdev->state == PVOL_BDEV_STATE_OFFLINE) {
		TAILQ_REMOVE(&spdk_pvol_bdev_offline_list, pvol_bdev, link_specific_list);
	} else {
		assert(0);
	}
	TAILQ_REMOVE(&spdk_pvol_bdev_list, pvol_bdev, link_global_list);
	assert(pvol_bdev_ctxt->bdev.name);
	free(pvol_bdev_ctxt->bdev.name);
	pvol_bdev_ctxt->bdev.name = NULL;
	assert(pvol_bdev->base_bdev_info);
	free(pvol_bdev->base_bdev_info);
	pvol_bdev->base_bdev_info = NULL;
	if (pvol_bdev->pvol_bdev_config) {
		pvol_bdev->pvol_bdev_config->pvol_bdev_ctxt = NULL;
	}
	free(pvol_bdev_ctxt);
}

/*
 * brief:
 * pvol_bdev_destruct is the destruct function table pointer for pvol bdev
 * params:
 * ctxt - pointer to pvol_bdev_ctxt
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
pvol_bdev_destruct(void *ctxt)
{
	struct pvol_bdev_ctxt *pvol_bdev_ctxt = ctxt;
	struct pvol_bdev      *pvol_bdev = &pvol_bdev_ctxt->pvol_bdev;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_PVOL, "pvol_bdev_destruct\n");

	pvol_bdev->destruct_called = true;
	for (uint16_t iter = 0; iter < pvol_bdev->num_base_bdevs; iter++) {
		/*
		 * Close all base bdev descriptors for which call has come from below
		 * layers
		 */
		if ((pvol_bdev->base_bdev_info[iter].base_bdev_remove_scheduled == true) &&
		    (pvol_bdev->base_bdev_info[iter].base_bdev != NULL)) {
			spdk_bdev_module_release_bdev(pvol_bdev->base_bdev_info[iter].base_bdev);
			spdk_bdev_close(pvol_bdev->base_bdev_info[iter].base_bdev_desc);
			pvol_bdev->base_bdev_info[iter].base_bdev_desc = NULL;
			pvol_bdev->base_bdev_info[iter].base_bdev = NULL;
			assert(pvol_bdev->num_base_bdevs_discovered);
			pvol_bdev->num_base_bdevs_discovered--;
		}
	}

	if (pvol_bdev->num_base_bdevs_discovered == 0) {
		/* Free pvol_bdev when there no base bdevs left */
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_PVOL, "pvol base bdevs is 0, going to free all in destruct\n");
		pvol_bdev_cleanup(pvol_bdev_ctxt);
	}

	return 0;
}

/*
 * brief:
 * pvol_bdev_io_completion function is called by lower layers to notify pvol
 * module that particular bdev_io is completed.
 * params:
 * bdev_io - pointer to bdev io submitted to lower layers, like child io
 * success - bdev_io status
 * cb_arg - function callback context, like parent io pointer
 * returns:
 * none
 */
static void
pvol_bdev_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io         *parent_io = cb_arg;
	struct pvol_bdev_io         *pvol_bdev_io = (struct pvol_bdev_io *)parent_io->driver_ctx;
	enum   spdk_bdev_io_status  status;

	assert(pvol_bdev_io->splits_comp_outstanding);
	pvol_bdev_io->splits_comp_outstanding--;
	if (pvol_bdev_io->status == SPDK_BDEV_IO_STATUS_SUCCESS) {
		/*
		 * Store failure status if any of the child bdev io. If any of the child
		 * fails, overall parent bdev_io is considered failed but parent bdev io
		 * status is only communicated to above layers on all child completions
		 */
		pvol_bdev_io->status = success;
	}
	/* Free child bdev io */
	spdk_bdev_free_io(bdev_io);

	if (!pvol_bdev_io->splits_pending && !pvol_bdev_io->splits_comp_outstanding) {
		/*
		 * If all childs are submitted and all childs are completed, process
		 * parent bdev io completion and complete the parent bdev io with
		 * appropriate status. If any of the child bdev io is failed, parent
		 * bdev io is considered failed.
		 */
		if (pvol_bdev_io->status) {
			status = SPDK_BDEV_IO_STATUS_SUCCESS;
		} else {
			status = SPDK_BDEV_IO_STATUS_FAILED;
		}
		if (pvol_bdev_io->waitq_entry) {
			free(pvol_bdev_io->waitq_entry);
		}
		spdk_bdev_io_complete(parent_io, status);
	}
}

/*
 * brief:
 * pvol_bdev_send_passthru function sends the bdev_io to the underlying
 * base device by-passing the splitting logic. This is used for optimization
 * when the total number of base devices in a pvol is only 1.
 * params:
 * ch - pointer to io channel for this io
 * bdev_io - pointer to bdev_io
 * returns:
 * 0 - success
 * non-zero - error
 */
static int
pvol_bdev_send_passthru(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct   pvol_bdev_io_channel *pvol_bdev_io_channel;
	struct   pvol_bdev_io         *pvol_bdev_io;
	struct   pvol_bdev            *pvol_bdev;
	int                           ret;

	pvol_bdev_io_channel = spdk_io_channel_get_ctx(ch);
	pvol_bdev = &pvol_bdev_io_channel->pvol_bdev_ctxt->pvol_bdev;
	pvol_bdev_io = (struct pvol_bdev_io *)bdev_io->driver_ctx;
	pvol_bdev_io->status = SPDK_BDEV_IO_STATUS_SUCCESS;

	if (pvol_bdev->base_bdev_info[0].base_bdev_desc == NULL) {
		SPDK_ERRLOG("base bdev desc null for pd_idx %u\n", 0);
		assert(0);
	}
	pvol_bdev_io->splits_pending = 0;
	pvol_bdev_io->splits_comp_outstanding = 1;
	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		ret = spdk_bdev_read_blocks(pvol_bdev->base_bdev_info[0].base_bdev_desc,
					    pvol_bdev_io_channel->base_bdevs_io_channel[0],
					    bdev_io->u.bdev.iovs->iov_base,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks, pvol_bdev_io_completion,
					    bdev_io);
	} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		ret = spdk_bdev_write_blocks(pvol_bdev->base_bdev_info[0].base_bdev_desc,
					     pvol_bdev_io_channel->base_bdevs_io_channel[0],
					     bdev_io->u.bdev.iovs->iov_base,
					     bdev_io->u.bdev.offset_blocks,
					     bdev_io->u.bdev.num_blocks, pvol_bdev_io_completion,
					     bdev_io);
	} else {
		ret = -EINVAL;
	}
	if (ret != 0) {
		/*
		 * If failed to submit child io to bdev layer then queue the parent
		 * bdev io with current active split information in the wait queue
		 * for that core. This will get resume from this point only. Assume
		 * if 4 splits are required and 2 childs are submitted, then parent
		 * io is queued to io waitq of this core and it will get resumed and
		 * try to submit the remaining 3 and 4 childs
		 */
		pvol_bdev_io->splits_pending = 1;
		pvol_bdev_io->splits_comp_outstanding = 0;
		pvol_bdev_io->ch = ch;
		return ret;
	}

	return 0;
}

/*
 * brief:
 * pvol_bdev_submit_children function is used to split the parent io and submit
 * the childs to bdev layer. bdev layer redirects the childs to appropriate base
 * bdev nvme module
 * params:
 * ch - pointer to spdk_io_channel for the pvol bdev
 * bdev_io - parent bdev io
 * start_strip - start strip number of this io
 * end_strip - end strip number of this io
 * cur_strip - current strip number of this io to start processing
 * buf - pointer to buffer for this io
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
pvol_bdev_submit_children(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
			  uint64_t start_strip, uint64_t end_strip, uint64_t cur_strip, uint8_t *buf)
{
	struct   pvol_bdev_io_channel *pvol_bdev_io_channel = spdk_io_channel_get_ctx(ch);
	struct   pvol_bdev_io         *pvol_bdev_io = (struct pvol_bdev_io *)bdev_io->driver_ctx;
	struct   pvol_bdev            *pvol_bdev = &pvol_bdev_io_channel->pvol_bdev_ctxt->pvol_bdev;
	uint64_t                      pd_strip;
	uint32_t                      offset_in_strip;
	uint64_t                      pd_lba;
	uint64_t                      pd_blocks;
	uint32_t                      pd_idx;
	int                           ret;

	for (uint64_t strip = cur_strip; strip <= end_strip; strip++) {
		/*
		 * For each strip of parent bdev io, process for each strip and submit
		 * child io to bdev layer. Calculate base bdev level start lba, length
		 * and buffer for this child io
		 */
		pd_strip = strip / pvol_bdev->num_base_bdevs;
		pd_idx = strip % pvol_bdev->num_base_bdevs;
		if (strip == start_strip) {
			offset_in_strip = bdev_io->u.bdev.offset_blocks & (pvol_bdev->strip_size - 1);
			pd_lba = (pd_strip << pvol_bdev->strip_size_shift) + offset_in_strip;
			if (strip == end_strip) {
				pd_blocks = bdev_io->u.bdev.num_blocks;
			} else {
				pd_blocks = pvol_bdev->strip_size - offset_in_strip;
			}
		} else if (strip == end_strip) {
			pd_lba = pd_strip << pvol_bdev->strip_size_shift;
			pd_blocks = ((bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks - 1) &
				     (pvol_bdev->strip_size - 1)) + 1;
		} else {
			pd_lba = pd_strip << pvol_bdev->strip_size_shift;
			pd_blocks = pvol_bdev->strip_size;
		}
		pvol_bdev_io->splits_comp_outstanding++;
		assert(pvol_bdev_io->splits_pending);
		pvol_bdev_io->splits_pending--;
		if (pvol_bdev->base_bdev_info[pd_idx].base_bdev_desc == NULL) {
			SPDK_ERRLOG("base bdev desc null for pd_idx %u\n", pd_idx);
			assert(0);
		}

		/*
		 * Submit child io to bdev layer with using base bdev descriptors, base
		 * bdev lba, base bdev child io length in blocks, buffer, completion
		 * function and function callback context
		 */
		if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
			ret = spdk_bdev_read_blocks(pvol_bdev->base_bdev_info[pd_idx].base_bdev_desc,
						    pvol_bdev_io_channel->base_bdevs_io_channel[pd_idx],
						    buf, pd_lba, pd_blocks, pvol_bdev_io_completion,
						    bdev_io);

		} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
			ret = spdk_bdev_write_blocks(pvol_bdev->base_bdev_info[pd_idx].base_bdev_desc,
						     pvol_bdev_io_channel->base_bdevs_io_channel[pd_idx],
						     buf, pd_lba, pd_blocks, pvol_bdev_io_completion,
						     bdev_io);
		} else {
			SPDK_ERRLOG("Recvd not supported io type %u\n", bdev_io->type);
			assert(0);
		}
		if (ret != 0) {
			/*
			 * If failed to submit child io to bdev layer then queue the parent
			 * bdev io with current active split information in the wait queue
			 * for that core. This will get resume from this point only. Assume
			 * if 4 splits are required and 2 childs are submitted, then parent
			 * io is queued to io waitq of this core and it will get resumed and
			 * try to submit the remaining 3 and 4 childs
			 */
			pvol_bdev_io->buf = buf;
			pvol_bdev_io->ch = ch;
			pvol_bdev_io->splits_comp_outstanding--;
			pvol_bdev_io->splits_pending++;
			return ret;
		}
		buf += (pd_blocks << pvol_bdev->blocklen_shift);
	}

	return 0;
}

/*
 * brief:
 * get_curr_base_bdev_index function calculates the base bdev index
 * which should be processed next based on splits_pending parameter
 * params:
 * pvol_bdev - pointer to pooled bdev
 * pvol_bdev_io - pointer to parent io context
 * returns:
 * base bdev index
 */
static uint8_t
get_curr_base_bdev_index(struct pvol_bdev *pvol_bdev, struct pvol_bdev_io *pvol_bdev_io)
{
	struct spdk_bdev_io *bdev_io;
	uint64_t            start_strip;
	uint64_t            end_strip;
	uint64_t            cur_strip;

	bdev_io = SPDK_CONTAINEROF(pvol_bdev_io, struct spdk_bdev_io, driver_ctx);
	start_strip = bdev_io->u.bdev.offset_blocks >> pvol_bdev->strip_size_shift;
	end_strip = (bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks - 1) >>
		    pvol_bdev->strip_size_shift;
	cur_strip = start_strip + ((end_strip - start_strip + 1) - pvol_bdev_io->splits_pending);

	return (cur_strip % pvol_bdev->num_base_bdevs);
}

/*
 * brief:
 * pvol_bdev_io_terminate function terminates the execution of the IO. If
 * any outstanding children are there it waits for completion, otherwise it
 * immediately completes the IO with failure.
 * params:
 * bdev_io - pointer to parent io
 * pvol_bdev_io - pointer to parent io context
 * returns:
 * none
 */
static void
pvol_bdev_io_terminate(struct spdk_bdev_io *bdev_io, struct pvol_bdev_io *pvol_bdev_io)
{
	if (pvol_bdev_io->splits_comp_outstanding == 0) {
		/* If no children is outstanding, immediately fail the parent IO */
		if (pvol_bdev_io->waitq_entry) {
			free(pvol_bdev_io->waitq_entry);
		}
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	} else {
		/* If any children is outstanding,
		 * wait for them to complete but don't send further Ios */
		pvol_bdev_io->splits_pending = 0;
		pvol_bdev_io->status = SPDK_BDEV_IO_STATUS_FAILED;
	}
}

/*
 * brief:
 * pvol_bdev_io_submit_fail_process function processes the IO which failed to submit.
 * It will try to queue the IOs after storing the context to bdev wait queue logic.
 * params:
 * bdev_io - pointer to bdev_io
 * pvol_bdev_io - pointer to pvol bdev io
 * ret - return code
 * returns:
 * none
 */
static void
pvol_bdev_io_submit_fail_process(struct pvol_bdev *pvol_bdev, struct spdk_bdev_io *bdev_io,
				 struct pvol_bdev_io *pvol_bdev_io, int ret)
{
	struct   pvol_bdev_io_channel *pvol_bdev_io_channel;
	uint8_t pd_idx;

	if (ret != -ENOMEM) {
		pvol_bdev_io_terminate(bdev_io, pvol_bdev_io);
	} else {
		/* Queue the IO to bdev layer wait queue */
		if (pvol_bdev_io->waitq_entry == NULL) {
			/* Only allocate if entry is not allocated yet */
			pvol_bdev_io->waitq_entry = calloc(1, sizeof(struct spdk_bdev_io_wait_entry));
		}
		if (pvol_bdev_io->waitq_entry == NULL) {
			pvol_bdev_io_terminate(bdev_io, pvol_bdev_io);
		} else {
			pd_idx = get_curr_base_bdev_index(pvol_bdev, pvol_bdev_io);
			pvol_bdev_io->waitq_entry->bdev = pvol_bdev->base_bdev_info[pd_idx].base_bdev;
			pvol_bdev_io->waitq_entry->cb_fn = pvol_bdev_waitq_io_process;
			pvol_bdev_io->waitq_entry->cb_arg = pvol_bdev_io;
			pvol_bdev_io_channel = spdk_io_channel_get_ctx(pvol_bdev_io->ch);
			if (spdk_bdev_queue_io_wait(pvol_bdev->base_bdev_info[pd_idx].base_bdev,
						    pvol_bdev_io_channel->base_bdevs_io_channel[pd_idx],
						    pvol_bdev_io->waitq_entry) != 0) {
				SPDK_ERRLOG("bdev io waitq error, it should not happen\n");
				assert(0);
				pvol_bdev_io_terminate(bdev_io, pvol_bdev_io);
			}
		}
	}
}

/*
 * brief:
 * pvol_bdev_waitq_io_process function is the callback function
 * registerd by pvol module to bdev when bdev_io was unavailable.
 * params:
 * ctx - pointer to pvol_bdev_io
 * returns:
 * none
 */
static void
pvol_bdev_waitq_io_process(void *ctx)
{
	struct   pvol_bdev_io         *pvol_bdev_io = ctx;
	struct   spdk_bdev_io         *bdev_io;
	struct   pvol_bdev_io_channel *pvol_bdev_io_channel;
	struct   pvol_bdev            *pvol_bdev;
	int                           ret;
	uint64_t                      start_strip;
	uint64_t                      end_strip;
	uint64_t                      cur_strip;

	bdev_io = SPDK_CONTAINEROF(pvol_bdev_io, struct spdk_bdev_io, driver_ctx);
	/*
	 * Try to submit childs of parent bdev io. If failed due to resource
	 * crunch then break the loop and don't try to process other queued IOs.
	 */
	pvol_bdev_io_channel = spdk_io_channel_get_ctx(pvol_bdev_io->ch);
	pvol_bdev = &pvol_bdev_io_channel->pvol_bdev_ctxt->pvol_bdev;
	if (pvol_bdev->num_base_bdevs > 1) {
		start_strip = bdev_io->u.bdev.offset_blocks >> pvol_bdev->strip_size_shift;
		end_strip = (bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks - 1) >>
			    pvol_bdev->strip_size_shift;
		cur_strip = start_strip + ((end_strip - start_strip + 1) - pvol_bdev_io->splits_pending);
		ret = pvol_bdev_submit_children(pvol_bdev_io->ch, bdev_io, start_strip, end_strip, cur_strip,
						pvol_bdev_io->buf);
	} else {
		ret = pvol_bdev_send_passthru(pvol_bdev_io->ch, bdev_io);
	}
	if (ret != 0) {
		pvol_bdev_io_submit_fail_process(pvol_bdev, bdev_io, pvol_bdev_io, ret);
	}
}

/*
 * brief:
 * pvol_bdev_submit_request function is the submit_request function pointer of
 * pvol bdev function table. This is used to submit the io on pvol_bdev to below
 * layers. If iowaitq is not empty, it will queue the parent bdev_io to the end
 * of the queue.
 * params:
 * ch - pointer to pvol bdev io channel
 * bdev_io - pointer to parent bdev_io on pvol device
 * returns:
 * none
 */
static void
pvol_bdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct   pvol_bdev_io_channel *pvol_bdev_io_channel;
	struct   pvol_bdev_io         *pvol_bdev_io;
	struct   pvol_bdev            *pvol_bdev;
	uint64_t                      start_strip = 0;
	uint64_t                      end_strip = 0;
	int                           ret;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		if (bdev_io->u.bdev.iovcnt != 1) {
			SPDK_ERRLOG("iov vector count is not 1\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
			break;
		}
		/*
		 * IO parameters used during io split and io completion
		 */
		pvol_bdev_io_channel = spdk_io_channel_get_ctx(ch);
		pvol_bdev = &pvol_bdev_io_channel->pvol_bdev_ctxt->pvol_bdev;
		pvol_bdev_io = (struct pvol_bdev_io *)bdev_io->driver_ctx;
		pvol_bdev_io->waitq_entry = NULL;
		if (pvol_bdev->num_base_bdevs > 1) {
			start_strip = bdev_io->u.bdev.offset_blocks >> pvol_bdev->strip_size_shift;
			end_strip = (bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks - 1) >>
				    pvol_bdev->strip_size_shift;
			/*
			 * IO parameters used during io split and io completion
			 */
			pvol_bdev_io->splits_pending = (end_strip - start_strip + 1);
			pvol_bdev_io->splits_comp_outstanding = 0;
			pvol_bdev_io->status = SPDK_BDEV_IO_STATUS_SUCCESS;
			ret = pvol_bdev_submit_children(ch, bdev_io, start_strip, end_strip, start_strip,
							bdev_io->u.bdev.iovs->iov_base);
		} else {
			ret = pvol_bdev_send_passthru(ch, bdev_io);
		}
		if (ret != 0) {
			pvol_bdev_io_submit_fail_process(pvol_bdev, bdev_io, pvol_bdev_io, ret);
		}
		break;

	case SPDK_BDEV_IO_TYPE_FLUSH:
		// TODO: support flush if requirement comes
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		break;

	default:
		SPDK_ERRLOG("submit request, invalid io type %u\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}

}

/*
 * brief:
 * pvol_bdev_io_type_supported is the io_supported function for bdev function
 * table which returns whether the particular io type is supported or not by
 * pvol bdev module
 * params:
 * ctx - pointer to pvol bdev context
 * type - io type
 * returns:
 * true - io_type is supported
 * false - io_type is not supported
 */
static bool
pvol_bdev_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
		return true;
	default:
		return false;
	}

	return false;
}

/*
 * brief:
 * pvol_bdev_get_io_channel is the get_io_channel function table pointer for
 * pvol bdev. This is used to return the io channel for this pvol bdev
 * params:
 * ctxt - pointer to pvol_bdev_ctxt
 * returns:
 * pointer to io channel for pvol bdev
 */
static struct spdk_io_channel *
pvol_bdev_get_io_channel(void *ctxt)
{
	struct pvol_bdev_ctxt *pvol_bdev_ctxt = ctxt;

	return spdk_get_io_channel(&pvol_bdev_ctxt->pvol_bdev);
}

/*
 * brief:
 * pvol_bdev_dump_info_json is the function table pointer for pvol bdev
 * params:
 * ctx - pointer to pvol_bdev_ctxt
 * w - pointer to json context
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
pvol_bdev_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct pvol_bdev_ctxt *pvol_bdev_ctxt = ctx;
	struct pvol_bdev      *pvol_bdev;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_PVOL, "pvol_bdev_dump_config_json\n");
	assert(pvol_bdev_ctxt != NULL);
	pvol_bdev = &pvol_bdev_ctxt->pvol_bdev;

	/* Dump the pvol bdev configuration related information */
	spdk_json_write_name(w, "pvol");
	spdk_json_write_object_begin(w);
	spdk_json_write_named_uint32(w, "strip_size", pvol_bdev->strip_size);
	spdk_json_write_named_uint32(w, "state", pvol_bdev->state);
	spdk_json_write_named_uint32(w, "raid_level", pvol_bdev->raid_level);
	spdk_json_write_named_uint32(w, "destruct_called", pvol_bdev->destruct_called);
	spdk_json_write_named_uint32(w, "num_base_bdevs", pvol_bdev->num_base_bdevs);
	spdk_json_write_named_uint32(w, "num_base_bdevs_discovered", pvol_bdev->num_base_bdevs_discovered);
	spdk_json_write_name(w, "base_bdevs_list");
	spdk_json_write_array_begin(w);
	for (uint16_t iter = 0; iter < pvol_bdev->num_base_bdevs; iter++) {
		if (pvol_bdev->base_bdev_info[iter].base_bdev) {
			spdk_json_write_string(w, pvol_bdev->base_bdev_info[iter].base_bdev->name);
		} else {
			spdk_json_write_string(w, "SlotEmpty");
		}
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);

	return 0;
}

/* g_pvol_bdev_fn_table is the function table for pvol bdev */
static const struct spdk_bdev_fn_table g_pvol_bdev_fn_table = {
	.destruct           = pvol_bdev_destruct,
	.submit_request     = pvol_bdev_submit_request,
	.io_type_supported  = pvol_bdev_io_type_supported,
	.get_io_channel     = pvol_bdev_get_io_channel,
	.dump_info_json     = pvol_bdev_dump_info_json,
};

/*
 * brief:
 * pvol_bdev_free is the pvol bdev function table function pointer. This is
 * called on bdev free path
 * params:
 * none
 * returns:
 * none
 */
static void
pvol_bdev_free(void)
{
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_PVOL, "pvol_bdev_free\n");
	for (uint32_t pvol_bdev = 0; pvol_bdev < spdk_pvol_config.total_pvol_bdev; pvol_bdev++) {
		if (spdk_pvol_config.pvol_bdev_config[pvol_bdev].base_bdev) {
			for (uint32_t iter = 0; iter < spdk_pvol_config.pvol_bdev_config[pvol_bdev].num_base_bdevs;
			     iter++) {
				free(spdk_pvol_config.pvol_bdev_config[pvol_bdev].base_bdev[iter].bdev_name);
			}
			free(spdk_pvol_config.pvol_bdev_config[pvol_bdev].base_bdev);
			spdk_pvol_config.pvol_bdev_config[pvol_bdev].base_bdev = NULL;
		}
		free(spdk_pvol_config.pvol_bdev_config[pvol_bdev].name);
	}
	if (spdk_pvol_config.pvol_bdev_config) {
		if (spdk_pvol_config.pvol_bdev_config->pvol_bdev_ctxt) {
			spdk_pvol_config.pvol_bdev_config->pvol_bdev_ctxt->pvol_bdev.pvol_bdev_config = NULL;
		}
		free(spdk_pvol_config.pvol_bdev_config);
		spdk_pvol_config.pvol_bdev_config = NULL;
		spdk_pvol_config.total_pvol_bdev = 0;
	}
}

/*
 * brief:
 * pvol_bdev_parse_pvol is used to parse the pvol from config file based on
 * pre-defined pvol format in config file.
 * Format of config file:
 *   [Pvol1]
 *   Name pvol1
 *   StripSize 64
 *   NumDevices 2
 *   RaidLevel 0
 *   Devices Nvme0n1 Nvme1n1
 *
 *   [Pvol2]
 *   Name pvol2
 *   StripSize 64
 *   NumDevices 3
 *   RaidLevel 0
 *   Devices Nvme2n1 Nvme3n1 Nvme4n1
 *
 * params:
 * conf_section - pointer to config section
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
pvol_bdev_parse_pvol(struct spdk_conf_section *conf_section)
{
	const char *pvol_name;
	int strip_size;
	int num_base_bdevs;
	int raid_level;
	const char *base_bdev_name;
	uint32_t iter;
	void *temp_ptr;
	struct pvol_bdev_config *pvol_bdev_config;

	pvol_name = spdk_conf_section_get_val(conf_section, "Name");
	if (pvol_name == NULL) {
		SPDK_ERRLOG("pvol_name %s is null\n", pvol_name);
		return -1;
	}
	strip_size = spdk_conf_section_get_intval(conf_section, "StripSize");
	if (spdk_u32_is_pow2(strip_size) == false) {
		SPDK_ERRLOG("Invalid strip size %d\n", strip_size);
		return -1;
	}
	num_base_bdevs = spdk_conf_section_get_intval(conf_section, "NumDevices");
	if (num_base_bdevs <= 0) {
		SPDK_ERRLOG("Invalid base device count %d\n", num_base_bdevs);
		return -1;
	}
	raid_level = spdk_conf_section_get_intval(conf_section, "RaidLevel");
	if (raid_level != 0) {
		SPDK_ERRLOG("invalid raid level %d, only raid level 0 is supported\n", raid_level);
		return -1;
	}

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_PVOL, "%s %d %d %d\n", pvol_name, strip_size, num_base_bdevs,
		      raid_level);

	for (iter = 0; iter < spdk_pvol_config.total_pvol_bdev; iter++) {
		if (!strcmp(spdk_pvol_config.pvol_bdev_config[iter].name, pvol_name)) {
			SPDK_ERRLOG("Duplicate pvol name found in config file %s\n", pvol_name);
			return -1;
		}
	}
	temp_ptr = realloc(spdk_pvol_config.pvol_bdev_config,
			   sizeof(struct pvol_bdev_config) * (spdk_pvol_config.total_pvol_bdev + 1));
	if (temp_ptr == NULL) {
		SPDK_ERRLOG("unable to allocate memory\n");
		return -1;
	}

	spdk_pvol_config.pvol_bdev_config = temp_ptr;
	pvol_bdev_config = &spdk_pvol_config.pvol_bdev_config[spdk_pvol_config.total_pvol_bdev];
	memset(pvol_bdev_config, 0, sizeof(*pvol_bdev_config));
	pvol_bdev_config->name = strdup(pvol_name);
	if (!pvol_bdev_config->name) {
		SPDK_ERRLOG("unable to allocate memory\n");
		return -1;
	}
	pvol_bdev_config->strip_size = strip_size;
	pvol_bdev_config->num_base_bdevs = num_base_bdevs;
	pvol_bdev_config->raid_level = raid_level;
	spdk_pvol_config.total_pvol_bdev++;
	pvol_bdev_config->base_bdev = calloc(num_base_bdevs, sizeof(*pvol_bdev_config->base_bdev));
	if (pvol_bdev_config->base_bdev == NULL) {
		SPDK_ERRLOG("unable to allocate memory\n");
		return -1;
	}

	for (iter = 0; true; iter++) {
		base_bdev_name = spdk_conf_section_get_nmval(conf_section, "Devices", 0, iter);
		if (base_bdev_name == NULL) {
			break;
		}
		if (iter >= pvol_bdev_config->num_base_bdevs) {
			SPDK_ERRLOG("Number of devices mentioned is more than count\n");
			return -1;
		}
		for (uint32_t iter2 = 0; iter2 < spdk_pvol_config.total_pvol_bdev; iter2++) {
			for (uint32_t iter3 = 0; iter3 < spdk_pvol_config.pvol_bdev_config[iter2].num_base_bdevs; iter3++) {
				if (spdk_pvol_config.pvol_bdev_config[iter2].base_bdev[iter3].bdev_name != NULL) {
					if (!strcmp(spdk_pvol_config.pvol_bdev_config[iter2].base_bdev[iter3].bdev_name, base_bdev_name)) {
						SPDK_ERRLOG("duplicate base bdev name %s mentioned\n", base_bdev_name);
						return -1;
					}
				}
			}
		}
		pvol_bdev_config->base_bdev[iter].bdev_name = strdup(base_bdev_name);
	}

	if (iter != pvol_bdev_config->num_base_bdevs) {
		SPDK_ERRLOG("Number of devices mentioned is less than count\n");
		return -1;
	}
	return 0;
}

/*
 * brief:
 * pvol_bdev_parse_config is used to find the pvol config section and parse it
 * Format of config file:
 * params:
 * none
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
pvol_bdev_parse_config(void)
{
	int                      ret;
	struct spdk_conf_section *conf_section;

	conf_section = spdk_conf_first_section(NULL);
	while (conf_section != NULL) {
		if (spdk_conf_section_match_prefix(conf_section, "Pvol")) {
			ret = pvol_bdev_parse_pvol(conf_section);
			if (ret < 0) {
				SPDK_ERRLOG("Unable to parse pvol section\n");
				return ret;
			}
		}
		conf_section = spdk_conf_next_section(conf_section);
	}

	return 0;
}

/*
 * brief:
 * pvol_bdev_exit is called on pvol bdev module exit time by bdev layer
 * params:
 * none
 * returns:
 * none
 */
static void
pvol_bdev_exit(void)
{
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_PVOL, "pvol_bdev_exit\n");
	pvol_bdev_free();
}

/*
 * brief:
 * pvol_bdev_get_ctx_size is used to return the context size of bdev_io for pvol
 * module
 * params:
 * none
 * returns:
 * size of spdk_bdev_io context for pvol
 */
static int
pvol_bdev_get_ctx_size(void)
{
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_PVOL, "pvol_bdev_get_ctx_size\n");
	return sizeof(struct pvol_bdev_io);
}

/*
 * brief:
 * pvol_bdev_can_claim_bdev is the function to check if this base_bdev can be
 * claimed by pvol or not.
 * params:
 * bdev_name - represents base bdev name
 * pvol_bdev_config - pointer to pvol bdev config parsed from config file
 * base_bdev_slot - if bdev can be claimed, it represents the base_bdev correct
 * slot. This field is only valid if return value of this function is true
 * returns:
 * true - if bdev can be claimed
 * false - if bdev can't be claimed
 */
static bool
pvol_bdev_can_claim_bdev(const char *bdev_name, struct pvol_bdev_config **pvol_bdev_config,
			 uint32_t *base_bdev_slot)
{
	bool     rv = false;

	for (uint32_t iter1 = 0; iter1 < spdk_pvol_config.total_pvol_bdev && !rv; iter1++) {
		for (uint32_t iter2 = 0; iter2 < spdk_pvol_config.pvol_bdev_config[iter1].num_base_bdevs; iter2++) {
			/*
			 * Check if the base bdev name is part of pvol bdev configuration.
			 * If match is found then return true and the slot information where
			 * this base bdev should be inserted in pvol bdev
			 */
			if (!strcmp(bdev_name, spdk_pvol_config.pvol_bdev_config[iter1].base_bdev[iter2].bdev_name)) {
				*pvol_bdev_config = &spdk_pvol_config.pvol_bdev_config[iter1];
				*base_bdev_slot = iter2;
				rv = true;
				break;
			}
		}
	}

	return rv;
}


static struct spdk_bdev_module g_pvol_if = {
	.name = "pvol",
	.module_init = pvol_bdev_init,
	.module_fini = pvol_bdev_exit,
	.get_ctx_size = pvol_bdev_get_ctx_size,
	.examine = pvol_bdev_examine,
	.config_text = NULL,
	.async_init = false,
	.async_fini = false,
};
SPDK_BDEV_MODULE_REGISTER(&g_pvol_if)

/*
 * brief:
 * pvol_bdev_init is the initialization function for pvol bdev module
 * params:
 * none
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
pvol_bdev_init(void)
{
	int ret;

	memset(&spdk_pvol_config, 0, sizeof(spdk_pvol_config));
	TAILQ_INIT(&spdk_pvol_bdev_configured_list);
	TAILQ_INIT(&spdk_pvol_bdev_configuring_list);
	TAILQ_INIT(&spdk_pvol_bdev_list);
	TAILQ_INIT(&spdk_pvol_bdev_offline_list);

	/* Parse config file for pvols */
	ret = pvol_bdev_parse_config();
	if (ret < 0) {
		SPDK_ERRLOG("pvol bdev init failed parsing\n");
		pvol_bdev_free();
		return ret;
	}

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_PVOL, "pvol_bdev_init completed successfully\n");

	return 0;
}

/*
 * brief:
 * pvol_bdev_remove_base_bdev function is called by below layers when base_bdev
 * is removed. This function checks if this base bdev is part of any pvol bdev
 * or not. If yes, it takes necessary action on that particular pvol bdev.
 * params:
 * ctx - pointer to base bdev pointer which got removed
 * returns:
 * none
 */
void
pvol_bdev_remove_base_bdev(void *ctx)
{
	struct    spdk_bdev       *base_bdev = ctx;
	struct    pvol_bdev       *pvol_bdev;
	struct    pvol_bdev       *next_pvol_bdev;
	struct    pvol_bdev_ctxt  *pvol_bdev_ctxt;
	uint16_t                  iter;
	bool                      found = false;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_PVOL, "pvol_bdev_remove_base_bdev\n");

	/* Find the pvol_bdev which has claimed this base_bdev */
	TAILQ_FOREACH_SAFE(pvol_bdev, &spdk_pvol_bdev_list, link_global_list, next_pvol_bdev) {
		for (iter = 0; iter < pvol_bdev->num_base_bdevs; iter++) {
			if (pvol_bdev->base_bdev_info[iter].base_bdev == base_bdev) {
				found = true;
				break;
			}
		}
		if (found == true) {
			break;
		}
	}

	if (found == false) {
		SPDK_ERRLOG("bdev to remove '%s' not found\n", base_bdev->name);
		return;
	}

	assert(pvol_bdev != NULL);
	assert(pvol_bdev->base_bdev_info[iter].base_bdev);
	assert(pvol_bdev->base_bdev_info[iter].base_bdev_desc);
	pvol_bdev_ctxt = SPDK_CONTAINEROF(pvol_bdev, struct pvol_bdev_ctxt, pvol_bdev);
	pvol_bdev->base_bdev_info[iter].base_bdev_remove_scheduled = true;

	if (pvol_bdev->destruct_called == true && pvol_bdev->base_bdev_info[iter].base_bdev != NULL) {
		/* As pvol bdev is already unregistered, so cleanup should be done here itself */
		spdk_bdev_module_release_bdev(pvol_bdev->base_bdev_info[iter].base_bdev);
		spdk_bdev_close(pvol_bdev->base_bdev_info[iter].base_bdev_desc);
		pvol_bdev->base_bdev_info[iter].base_bdev_desc = NULL;
		pvol_bdev->base_bdev_info[iter].base_bdev = NULL;
		assert(pvol_bdev->num_base_bdevs_discovered);
		pvol_bdev->num_base_bdevs_discovered--;
		if (pvol_bdev->num_base_bdevs_discovered == 0) {
			/* Since there is no base bdev for this pvol, so free the pvol device */
			pvol_bdev_cleanup(pvol_bdev_ctxt);
			return;
		}
	}

	if (pvol_bdev->state == PVOL_BDEV_STATE_ONLINE) {
		/*
		 * If pvol bdev is online and registered, change the bdev state to
		 * configuring and unregister this pvol device. Queue this pvol device
		 * in configuring list
		 */
		assert(pvol_bdev->num_base_bdevs == pvol_bdev->num_base_bdevs_discovered);
		TAILQ_REMOVE(&spdk_pvol_bdev_configured_list, pvol_bdev, link_specific_list);
		pvol_bdev->state = PVOL_BDEV_STATE_OFFLINE;
		assert(pvol_bdev->num_base_bdevs_discovered);
		TAILQ_INSERT_TAIL(&spdk_pvol_bdev_offline_list, pvol_bdev, link_specific_list);
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_PVOL, "pvol state chaning from online to offline\n");
		spdk_io_device_unregister(&pvol_bdev_ctxt->pvol_bdev, NULL);
		spdk_bdev_unregister(&pvol_bdev_ctxt->bdev, NULL, NULL);
	}
}

/*
 * brief:
 * pvol_bdev_add_base_device function is the actual function which either adds
 * the nvme base device to existing pvol or create a new pvol. It also claims
 * the base device and keep the open descriptor.
 * params:
 * bdev - pointer to base bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
int
pvol_bdev_add_base_device(struct spdk_bdev *bdev)
{
	struct    pvol_bdev_config  *pvol_bdev_config = NULL;
	struct    pvol_bdev_ctxt    *pvol_bdev_ctxt;
	struct    pvol_bdev         *pvol_bdev;
	struct    spdk_bdev_desc    *desc;
	struct    spdk_bdev         *pvol_bdev_gen;
	uint32_t                    blocklen;
	uint64_t                    min_blockcnt;
	uint32_t                    base_bdev_slot;
	bool                        can_claim;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_PVOL, "pvol_bdev_examine %p\n", bdev);

	can_claim = pvol_bdev_can_claim_bdev(bdev->name, &pvol_bdev_config, &base_bdev_slot);

	if (!can_claim) {
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_PVOL, "bdev %s can't be claimed\n", bdev->name);
		return -1;
	}
	assert(pvol_bdev_config);

	if (spdk_bdev_open(bdev, true, pvol_bdev_remove_base_bdev, bdev, &desc)) {
		SPDK_ERRLOG("Unable to create desc on bdev '%s'\n", bdev->name);
		return -1;
	}

	if (spdk_bdev_module_claim_bdev(bdev, NULL, &g_pvol_if)) {
		SPDK_ERRLOG("Unable to claim this bdev as it is already claimed\n");
		spdk_bdev_close(desc);
		return -1;
	}
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_PVOL, "bdev %s is claimed\n", bdev->name);
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_PVOL, "pvol_bdev_config->pvol_bdev_ctxt %p\n",
		      pvol_bdev_config->pvol_bdev_ctxt);

	if (!pvol_bdev_config->pvol_bdev_ctxt) {
		/* Allocate pvol_bdev entity if it is not already allocated */
		pvol_bdev_ctxt = calloc(1, sizeof(*pvol_bdev_ctxt));
		if (!pvol_bdev_ctxt) {
			SPDK_ERRLOG("Unable to allocate memory for pvol bdev for bdev '%s'\n", bdev->name);
			spdk_bdev_module_release_bdev(bdev);
			spdk_bdev_close(desc);
			return -1;
		}
		pvol_bdev = &pvol_bdev_ctxt->pvol_bdev;
		pvol_bdev->num_base_bdevs = pvol_bdev_config->num_base_bdevs;
		pvol_bdev->base_bdev_info = calloc(pvol_bdev->num_base_bdevs, sizeof(struct pvol_base_bdev_info));
		if (!pvol_bdev->base_bdev_info) {
			SPDK_ERRLOG("Unable able to allocate base bdev info\n");
			free(pvol_bdev_ctxt);
			spdk_bdev_module_release_bdev(bdev);
			spdk_bdev_close(desc);
			return -1;
		}
		pvol_bdev_config->pvol_bdev_ctxt = pvol_bdev_ctxt;
		pvol_bdev->strip_size = pvol_bdev_config->strip_size;
		pvol_bdev->state = PVOL_BDEV_STATE_CONFIGURING;
		pvol_bdev->pvol_bdev_config = pvol_bdev_config;
		TAILQ_INSERT_TAIL(&spdk_pvol_bdev_configuring_list, pvol_bdev, link_specific_list);
		TAILQ_INSERT_TAIL(&spdk_pvol_bdev_list, pvol_bdev, link_global_list);
	} else {
		pvol_bdev = &pvol_bdev_config->pvol_bdev_ctxt->pvol_bdev;
	}

	assert(pvol_bdev->state != PVOL_BDEV_STATE_ONLINE);
	assert(base_bdev_slot < pvol_bdev->num_base_bdevs);

	pvol_bdev->base_bdev_info[base_bdev_slot].base_bdev = bdev;
	pvol_bdev->base_bdev_info[base_bdev_slot].base_bdev_desc = desc;
	pvol_bdev->num_base_bdevs_discovered++;

	assert(pvol_bdev->num_base_bdevs_discovered <= pvol_bdev->num_base_bdevs);

	if (pvol_bdev->num_base_bdevs_discovered == pvol_bdev->num_base_bdevs) {
		/* If pvol bdev config is complete, then only register the pvol bdev to
		 * bdev layer and remove this pvol bdev from configuring list and
		 * insert the pvol bdev to configured list
		 */
		blocklen = pvol_bdev->base_bdev_info[0].base_bdev->blocklen;
		min_blockcnt = pvol_bdev->base_bdev_info[0].base_bdev->blockcnt;
		for (uint32_t iter = 1; iter < pvol_bdev->num_base_bdevs; iter++) {
			/* Calculate minimum block count from all base bdevs */
			if (pvol_bdev->base_bdev_info[iter].base_bdev->blockcnt < min_blockcnt) {
				min_blockcnt = pvol_bdev->base_bdev_info[iter].base_bdev->blockcnt;
			}

			/* Check blocklen for all base bdevs that it should be same */
			if (blocklen != pvol_bdev->base_bdev_info[iter].base_bdev->blocklen) {
				/*
				 * Assumption is that all the base bdevs for any pvol should
				 * have same blocklen
				 */
				SPDK_ERRLOG("Blocklen of various bdevs not matching\n");
				pvol_bdev->state = PVOL_BDEV_STATE_OFFLINE;
				TAILQ_REMOVE(&spdk_pvol_bdev_configuring_list, pvol_bdev, link_specific_list);
				TAILQ_INSERT_TAIL(&spdk_pvol_bdev_offline_list, pvol_bdev, link_specific_list);
				return -1;
			}
		}
		pvol_bdev_ctxt = SPDK_CONTAINEROF(pvol_bdev, struct pvol_bdev_ctxt, pvol_bdev);
		pvol_bdev_gen = &pvol_bdev_ctxt->bdev;
		pvol_bdev_gen->name = strdup(pvol_bdev_config->name);
		if (!pvol_bdev_gen->name) {
			SPDK_ERRLOG("Unable to allocate name for pvol\n");
			pvol_bdev->state = PVOL_BDEV_STATE_OFFLINE;
			TAILQ_REMOVE(&spdk_pvol_bdev_configuring_list, pvol_bdev, link_specific_list);
			TAILQ_INSERT_TAIL(&spdk_pvol_bdev_offline_list, pvol_bdev, link_specific_list);
			return -1;
		}
		pvol_bdev_gen->product_name = "Pooled Device";
		pvol_bdev_gen->write_cache = 0;
		pvol_bdev_gen->blocklen = blocklen;
		pvol_bdev_gen->optimal_io_boundary = 0;
		pvol_bdev_gen->ctxt = pvol_bdev_ctxt;
		pvol_bdev_gen->fn_table = &g_pvol_bdev_fn_table;
		pvol_bdev_gen->module = &g_pvol_if;
		pvol_bdev->strip_size = (pvol_bdev->strip_size * 1024) / blocklen;
		pvol_bdev->strip_size_shift = spdk_u32log2(pvol_bdev->strip_size);
		pvol_bdev->blocklen_shift = spdk_u32log2(blocklen);

		/*
		 * Pvol bdev logic is for striping so take the minimum block count based
		 * approach where total block count of pvol bdev is the number of base
		 * bdev times the minimum block count of any base bdev
		 */
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_PVOL, "min blockcount %lu,  numbasedev %u, strip size shift %u\n",
			      min_blockcnt,
			      pvol_bdev->num_base_bdevs, pvol_bdev->strip_size_shift);
		pvol_bdev_gen->blockcnt = ((min_blockcnt >> pvol_bdev->strip_size_shift) <<
					   pvol_bdev->strip_size_shift)  * pvol_bdev->num_base_bdevs;
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_PVOL, "io device register %p\n", pvol_bdev);
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_PVOL, "blockcnt %lu, blocklen %u\n", pvol_bdev_gen->blockcnt,
			      pvol_bdev_gen->blocklen);
		if (pvol_bdev->state == PVOL_BDEV_STATE_CONFIGURING) {
			pvol_bdev->state = PVOL_BDEV_STATE_ONLINE;
			spdk_io_device_register(pvol_bdev, pvol_bdev_create_cb, pvol_bdev_destroy_cb,
						sizeof(struct pvol_bdev_io_channel));
			if (spdk_bdev_register(pvol_bdev_gen)) {
				/*
				 * If failed to register pvol to bdev layer, make pvol offline
				 * and add to offline list
				 */
				SPDK_ERRLOG("Unable to register pooled bdev\n");
				spdk_io_device_unregister(pvol_bdev, NULL);
				pvol_bdev->state = PVOL_BDEV_STATE_OFFLINE;
				TAILQ_REMOVE(&spdk_pvol_bdev_configuring_list, pvol_bdev, link_specific_list);
				TAILQ_INSERT_TAIL(&spdk_pvol_bdev_offline_list, pvol_bdev, link_specific_list);
				return -1;
			}
			SPDK_DEBUGLOG(SPDK_LOG_BDEV_PVOL, "pvol bdev generic %p\n", pvol_bdev_gen);
			TAILQ_REMOVE(&spdk_pvol_bdev_configuring_list, pvol_bdev, link_specific_list);
			TAILQ_INSERT_TAIL(&spdk_pvol_bdev_configured_list, pvol_bdev, link_specific_list);
			SPDK_DEBUGLOG(SPDK_LOG_BDEV_PVOL, "pvol is created with name %s, pvol_bdev %p\n",
				      pvol_bdev_gen->name, pvol_bdev);
		}
	}

	return 0;
}

/*
 * brief:
 * pvol_bdev_examine function is the examine function call by the below layers
 * like bdev_nvme layer. This function will check if this base bdev can be
 * claimed by this pvol or not.
 * params:
 * bdev - pointer to base bdev
 * returns:
 * none
 */
static void
pvol_bdev_examine(struct spdk_bdev *bdev)
{
	pvol_bdev_add_base_device(bdev);
	spdk_bdev_module_examine_done(&g_pvol_if);
}

/* Log component for bdev pvol module */
SPDK_LOG_REGISTER_COMPONENT("bdev_pvol", SPDK_LOG_BDEV_PVOL)
