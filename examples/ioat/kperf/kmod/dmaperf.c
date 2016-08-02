/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 *   redistributing this file, you may do so under either license.
 *
 *   GPL LICENSE SUMMARY
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
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
 *     * Redistributions in binary form must reproduce the above copy
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
 *
 *   PCIe DMA Perf Linux driver
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/wait.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/dma-mapping.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/debugfs.h>
#include <linux/dmaengine.h>
#include <linux/delay.h>
#include <linux/printk.h>
#include <linux/nodemask.h>

#define DRIVER_NAME		"dma_perf"
#define DRIVER_DESCRIPTION	"PCIe DMA Performance Measurement Tool"

#define DRIVER_LICENSE		"Dual BSD/GPL"
#define DRIVER_VERSION		"1.0"
#define DRIVER_AUTHOR		"Dave Jiang <dave.jiang@intel.com>"

#define MAX_THREADS		32
#define MAX_TEST_SIZE		1024 * 1024	/* 1M */
#define DMA_CHANNELS_PER_NODE	8

MODULE_LICENSE(DRIVER_LICENSE);
MODULE_VERSION(DRIVER_VERSION);
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_AUTHOR("Changpeng Liu <changpeng.liu@intel.com>");
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);

static struct dentry *perf_debugfs_dir;
static struct perf_ctx *g_perf = NULL;

static unsigned int seg_order = 12; /* 4K */
static unsigned int queue_depth = 256;
static unsigned int run_order = 32; /* 4G */

struct perf_mw {
	size_t		buf_size;
	void		*virt_addr;
};

struct perf_ctx;

struct pthr_ctx {
	struct dentry		*debugfs_thr_dir;
	struct dentry		*debugfs_copied;
	struct dentry		*debugfs_elapsed_time;
	struct device		*dev;
	int			node;
	wait_queue_head_t	wq;
	struct perf_mw		mw;
	struct task_struct	*thread;
	struct perf_ctx		*perf;
	atomic_t		dma_sync;
	struct dma_chan		*dma_chan;
	int			dma_up;
	int			dma_down;
	int			dma_prep_err;
	u64			copied;
	u64			elapsed_time;
};

struct perf_ctx {
	spinlock_t		db_lock;
	struct dentry		*debugfs_node_dir;
	struct dentry		*debugfs_run;
	struct dentry		*debugfs_threads;
	struct dentry		*debugfs_queue_depth;
	struct dentry		*debugfs_transfer_size_order;
	struct dentry		*debugfs_total_size_order;
	struct dentry		*debugfs_status;
	u8			numa_nodes;
	u8			perf_threads;
	bool			run;
	struct pthr_ctx		pthr_ctx[MAX_THREADS];
	atomic_t		tsync;
};

static void perf_free_mw(struct pthr_ctx *pctx);
static int perf_set_mw(struct pthr_ctx *pctx, size_t size);

static void perf_copy_callback(void *data)
{
	struct pthr_ctx *pctx = data;

	atomic_dec(&pctx->dma_sync);
	pctx->dma_down++;

	wake_up(&pctx->wq);
}

static ssize_t perf_copy(struct pthr_ctx *pctx, char *dst,
			 char *src, size_t size)
{
	struct dma_async_tx_descriptor *txd;
	struct dma_chan *chan = pctx->dma_chan;
	struct dma_device *device;
	struct dmaengine_unmap_data *unmap;
	dma_cookie_t cookie;
	size_t src_off, dst_off;
	int retries = 0;

	if (!chan) {
		printk("DMA engine does not exist\n");
		return -EINVAL;
	}

	device = chan->device;
	src_off = (size_t)src & ~PAGE_MASK;
	dst_off = (size_t)dst & ~PAGE_MASK;

	if (!is_dma_copy_aligned(device, src_off, dst_off, size))
		return -ENODEV;

	unmap = dmaengine_get_unmap_data(device->dev, 2, GFP_NOWAIT);
	if (!unmap)
		return -ENOMEM;

	unmap->len = size;
	unmap->addr[0] = dma_map_page(device->dev, virt_to_page(src),
				      src_off, size, DMA_TO_DEVICE);
	if (dma_mapping_error(device->dev, unmap->addr[0]))
		goto err_get_unmap;

	unmap->to_cnt = 1;

	unmap->addr[1] = dma_map_page(device->dev, virt_to_page(dst),
				      dst_off, size, DMA_FROM_DEVICE);
	if (dma_mapping_error(device->dev, unmap->addr[1]))
		goto err_get_unmap;
	unmap->from_cnt = 1;

dma_prep_retry:
	txd = device->device_prep_dma_memcpy(chan, unmap->addr[1],
					     unmap->addr[0],
					     size, DMA_PREP_INTERRUPT);
	if (!txd) {
		if (retries++ > 20) {
			pctx->dma_prep_err++;
			goto err_get_unmap;
		} else {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(50);
			goto dma_prep_retry;
		}
	}

	txd->callback = perf_copy_callback;
	txd->callback_param = pctx;
	dma_set_unmap(txd, unmap);

	cookie = dmaengine_submit(txd);
	if (dma_submit_error(cookie))
		goto err_set_unmap;

	atomic_inc(&pctx->dma_sync);

	pctx->dma_up++;
	dma_async_issue_pending(chan);

	return size;

err_set_unmap:
	dmaengine_unmap_put(unmap);
err_get_unmap:
	dmaengine_unmap_put(unmap);
	return 0;
}

static int perf_move_data(struct pthr_ctx *pctx, char *dst, char *src,
			  u64 buf_size, u64 win_size, u64 total)
{
	int chunks, total_chunks, i;
	int copied_chunks = 0;
	u64 result;
	char *tmp = dst;
	u64 perf, diff_us;
	ktime_t kstart, kstop, kdiff;

	chunks = win_size / buf_size;
	total_chunks = total / buf_size;

	printk("%s: chunks: %d total_chunks: %d\n", current->comm, chunks, total_chunks);

	kstart = ktime_get();

	for (i = 0; i < total_chunks; i++) {

		wait_event_interruptible(pctx->wq, atomic_read(&pctx->dma_sync) < queue_depth);

		result = perf_copy(pctx, tmp, src, buf_size);
		pctx->copied += result;
		copied_chunks++;
		if (copied_chunks == chunks) {
			tmp = dst;
			copied_chunks = 0;
		} else
			tmp += buf_size;
	}

	printk("%s: All DMA descriptors submitted\n", current->comm);

	/* FIXME: need a timeout here eventually */
	while (atomic_read(&pctx->dma_sync) != 0)
		msleep(1);

	pr_info("%s: dma_up: %d  dma_down: %d dma_prep_err: %d\n",
		current->comm, pctx->dma_up, pctx->dma_down,
		pctx->dma_prep_err);

	kstop = ktime_get();
	kdiff = ktime_sub(kstop, kstart);
	diff_us = ktime_to_us(kdiff);

	pr_info("%s: copied %Lu bytes\n", current->comm, pctx->copied);

	pr_info("%s: lasted %Lu usecs\n", current->comm, diff_us);

	perf = pctx->copied / diff_us;

	pr_info("%s: MBytes/s: %Lu\n", current->comm, perf);

	pctx->elapsed_time = diff_us;

	return 0;
}

static bool perf_dma_filter_fn(struct dma_chan *chan, void *node)
{
	return dev_to_node(&chan->dev->device) == (int)(unsigned long)node;
}

static int dma_perf_thread(void *data)
{
	struct pthr_ctx *pctx = data;
	struct perf_ctx *perf = pctx->perf;
	struct perf_mw *mw = &pctx->mw;
	char *dst;
	u64 win_size, buf_size, total;
	void *src;
	int rc, node;
	struct dma_chan *dma_chan = NULL;

	pr_info("kthread %s starting...\n", current->comm);

	node = pctx->node;

	if (!pctx->dma_chan) {
		dma_cap_mask_t dma_mask;

		dma_cap_zero(dma_mask);
		dma_cap_set(DMA_MEMCPY, dma_mask);
		dma_chan = dma_request_channel(dma_mask, perf_dma_filter_fn,
					       (void *)(unsigned long)node);
		if (!dma_chan) {
			pr_warn("%s: cannot acquire DMA channel, quitting\n",
				current->comm);
			return -ENODEV;
		}
		pctx->dma_chan = dma_chan;
		pctx->dev = dma_chan->device->dev;
	}

	src = kmalloc_node(MAX_TEST_SIZE, GFP_KERNEL, node);
	if (!src) {
		rc = -ENOMEM;
		goto err;
	}

	rc = perf_set_mw(pctx, MAX_TEST_SIZE);
	if (rc < 0) {
		pr_err("%s: set mw failed\n", current->comm);
		rc = -ENXIO;
		goto err;
	}

	win_size = mw->buf_size;
	buf_size = 1ULL << seg_order;
	total = 1ULL << run_order;

	if (buf_size > MAX_TEST_SIZE)
		buf_size = MAX_TEST_SIZE;

	dst = (char *)mw->virt_addr;

	atomic_inc(&perf->tsync);
	while (atomic_read(&perf->tsync) != perf->perf_threads)
		schedule();

	rc = perf_move_data(pctx, dst, src, buf_size, win_size, total);

	atomic_dec(&perf->tsync);

	if (rc < 0) {
		pr_err("%s: failed\n", current->comm);
		rc = -ENXIO;
		goto err;
	}

	return 0;

err:
	if (src)
		kfree(src);

	if (dma_chan) {
		dma_release_channel(dma_chan);
		pctx->dma_chan = NULL;
	}

	return rc;
}

static void perf_free_mw(struct pthr_ctx *pctx)
{
	struct perf_mw *mw = &pctx->mw;

	if (!mw->virt_addr)
		return;

	kfree(mw->virt_addr);
	mw->buf_size = 0;
	mw->virt_addr = NULL;
}

static int perf_set_mw(struct pthr_ctx *pctx, size_t size)
{
	struct perf_mw *mw = &pctx->mw;

	if (!size)
		return -EINVAL;

	mw->buf_size = size;

	mw->virt_addr = kmalloc_node(size, GFP_KERNEL, pctx->node);

	if (!mw->virt_addr) {
		mw->buf_size = 0;
		return -EINVAL;
	}

	return 0;
}

static ssize_t debugfs_run_read(struct file *filp, char __user *ubuf,
				size_t count, loff_t *offp)
{
	struct perf_ctx *perf = filp->private_data;
	char *buf;
	ssize_t ret, out_offset;

	if (!perf)
		return 0;

	buf = kmalloc(64, GFP_KERNEL);
	out_offset = snprintf(buf, 64, "%d\n", perf->run);
	ret = simple_read_from_buffer(ubuf, count, offp, buf, out_offset);
	kfree(buf);

	return ret;
}

static ssize_t debugfs_run_write(struct file *filp, const char __user *ubuf,
				 size_t count, loff_t *offp)
{
	struct perf_ctx *perf = filp->private_data;
	int node, i;

	if (perf->perf_threads == 0)
		return 0;

	if (atomic_read(&perf->tsync) == 0)
		perf->run = false;

	if (perf->run == true) {
		/* lets stop the threads */
		perf->run = false;
		for (i = 0; i < MAX_THREADS; i++) {
			if (perf->pthr_ctx[i].thread) {
				kthread_stop(perf->pthr_ctx[i].thread);
				perf->pthr_ctx[i].thread = NULL;
			} else
				break;
		}
	} else {
		perf->run = true;

		if (perf->perf_threads > MAX_THREADS) {
			perf->perf_threads = MAX_THREADS;
			pr_info("Reset total threads to: %u\n", MAX_THREADS);
		}

		/* no greater than 1M */
		if (seg_order > 20) {
			seg_order = 20;
			pr_info("Fix seg_order to %u\n", seg_order);
		}

		if (run_order < seg_order) {
			run_order = seg_order;
			pr_info("Fix run_order to %u\n", run_order);
		}

		/* launch kernel thread */
		for (i = 0; i < perf->perf_threads; i++) {
			struct pthr_ctx *pctx;

			pctx = &perf->pthr_ctx[i];
			atomic_set(&pctx->dma_sync, 0);
			pctx->perf = perf;
			pctx->elapsed_time = 0;
			pctx->copied = 0;

			init_waitqueue_head(&pctx->wq);

			/* NUMA socket node */
			pctx->node = i / DMA_CHANNELS_PER_NODE;
			node = pctx->node;

			pctx->thread =
				kthread_create_on_node(dma_perf_thread,
						       (void *)pctx,
						       node, "dma_perf %d", i);
			if (pctx->thread)
				wake_up_process(pctx->thread);
			else {
				perf->run = false;
				for (i = 0; i < MAX_THREADS; i++) {
					if (pctx->thread) {
						kthread_stop(pctx->thread);
						pctx->thread = NULL;
					} else
						break;
				}
			}

			if (perf->run == false)
				return -ENXIO;
		}

	}

	return count;
}

static const struct file_operations dma_perf_debugfs_run = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = debugfs_run_read,
	.write = debugfs_run_write,
};

static ssize_t debugfs_status_read(struct file *filp, char __user *ubuf,
				   size_t count, loff_t *offp)
{
	struct perf_ctx *perf = filp->private_data;
	char *buf;
	ssize_t ret, out_offset;

	if (!perf)
		return 0;

	buf = kmalloc(64, GFP_KERNEL);
	out_offset = snprintf(buf, 64, "%s\n", atomic_read(&perf->tsync) ? "running" : "idle");
	ret = simple_read_from_buffer(ubuf, count, offp, buf, out_offset);
	kfree(buf);

	return ret;
}

static const struct file_operations dma_perf_debugfs_status = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = debugfs_status_read,
};

static int perf_debugfs_setup(struct perf_ctx *perf)
{

	int i;
	char temp_name[64];

	if (!perf_debugfs_dir)
		return -ENODEV;

	perf->debugfs_node_dir = debugfs_create_dir("dmaperf",
				 perf_debugfs_dir);
	if (!perf->debugfs_node_dir)
		return -ENODEV;

	perf->debugfs_run = debugfs_create_file("run", S_IRUSR | S_IWUSR,
						perf->debugfs_node_dir, perf,
						&dma_perf_debugfs_run);
	if (!perf->debugfs_run)
		return -ENODEV;

	perf->debugfs_status = debugfs_create_file("status", S_IRUSR,
			       perf->debugfs_node_dir, perf,
			       &dma_perf_debugfs_status);
	if (!perf->debugfs_status)
		return -ENODEV;

	perf->debugfs_threads = debugfs_create_u8("threads", S_IRUSR | S_IWUSR,
				perf->debugfs_node_dir,
				&perf->perf_threads);
	if (!perf->debugfs_threads)
		return -ENODEV;

	perf->debugfs_queue_depth = debugfs_create_u32("queue_depth", S_IRUSR | S_IWUSR,
				    perf->debugfs_node_dir,
				    &queue_depth);
	if (!perf->debugfs_queue_depth)
		return -ENODEV;

	perf->debugfs_transfer_size_order = debugfs_create_u32("transfer_size_order", S_IRUSR | S_IWUSR,
					    perf->debugfs_node_dir,
					    &seg_order);
	if (!perf->debugfs_transfer_size_order)
		return -ENODEV;

	perf->debugfs_total_size_order = debugfs_create_u32("total_size_order", S_IRUSR | S_IWUSR,
					 perf->debugfs_node_dir,
					 &run_order);
	if (!perf->debugfs_total_size_order)
		return -ENODEV;

	for (i = 0; i < MAX_THREADS; i++) {
		struct pthr_ctx *pctx = &perf->pthr_ctx[i];
		sprintf(temp_name, "thread_%d", i);

		pctx->debugfs_thr_dir = debugfs_create_dir(temp_name, perf->debugfs_node_dir);
		if (!pctx->debugfs_thr_dir)
			return -ENODEV;

		pctx->debugfs_copied = debugfs_create_u64("copied", S_IRUSR,
				       pctx->debugfs_thr_dir,
				       &pctx->copied);
		if (!pctx->debugfs_copied)
			return -ENODEV;

		pctx->debugfs_elapsed_time = debugfs_create_u64("elapsed_time", S_IRUSR,
					     pctx->debugfs_thr_dir,
					     &pctx->elapsed_time);
		if (!pctx->debugfs_elapsed_time)
			return -ENODEV;
	}

	return 0;
}

static int perf_probe(void)
{
	struct perf_ctx *perf;
	int rc = 0;

	perf = kzalloc_node(sizeof(*perf), GFP_KERNEL, 0);
	if (!perf) {
		rc = -ENOMEM;
		goto err_perf;
	}

	perf->numa_nodes = num_online_nodes();
	perf->perf_threads = 1;
	atomic_set(&perf->tsync, 0);
	perf->run = false;
	spin_lock_init(&perf->db_lock);

	if (debugfs_initialized() && !perf_debugfs_dir) {
		perf_debugfs_dir = debugfs_create_dir(KBUILD_MODNAME, NULL);
		if (!perf_debugfs_dir)
			goto err_ctx;

		rc = perf_debugfs_setup(perf);
		if (rc)
			goto err_ctx;
	}

	g_perf = perf;
	return 0;

err_ctx:
	kfree(perf);
err_perf:
	return rc;
}

static void perf_remove(void)
{
	int i;
	struct perf_ctx *perf = g_perf;

	if (perf_debugfs_dir) {
		debugfs_remove_recursive(perf_debugfs_dir);
		perf_debugfs_dir = NULL;
	}

	for (i = 0; i < MAX_THREADS; i++) {
		struct pthr_ctx *pctx = &perf->pthr_ctx[i];
		if (pctx->dma_chan)
			dma_release_channel(pctx->dma_chan);
		perf_free_mw(pctx);
	}

	kfree(perf);
}

static int __init perf_init_module(void)
{
	printk("DMA Performance Test Init\n");
	return perf_probe();
}
module_init(perf_init_module);

static void __exit perf_exit_module(void)
{
	printk("DMA Performance Test Exit\n");
	perf_remove();
}
module_exit(perf_exit_module);
