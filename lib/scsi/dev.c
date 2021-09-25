/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
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

#include "scsi_internal.h"

static struct spdk_scsi_dev g_devs[SPDK_SCSI_MAX_DEVS];

struct spdk_scsi_dev *
scsi_dev_get_list(void)
{
	return g_devs;
}

static struct spdk_scsi_dev *
allocate_dev(void)
{
	struct spdk_scsi_dev *dev;
	int i;

	for (i = 0; i < SPDK_SCSI_MAX_DEVS; i++) {
		dev = &g_devs[i];
		if (!dev->is_allocated) {
			memset(dev, 0, sizeof(*dev));
			dev->id = i;
			dev->is_allocated = 1;
			TAILQ_INIT(&dev->luns);
			return dev;
		}
	}

	return NULL;
}

static void
free_dev(struct spdk_scsi_dev *dev)
{
	assert(dev->is_allocated == 1);
	assert(dev->removed == true);

	dev->is_allocated = 0;

	if (dev->remove_cb) {
		dev->remove_cb(dev->remove_ctx, 0);
		dev->remove_cb = NULL;
	}
}

void
spdk_scsi_dev_destruct(struct spdk_scsi_dev *dev,
		       spdk_scsi_dev_destruct_cb_t cb_fn, void *cb_arg)
{
	struct spdk_scsi_lun *lun, *tmp_lun;

	if (dev == NULL) {
		if (cb_fn) {
			cb_fn(cb_arg, -EINVAL);
		}
		return;
	}

	if (dev->removed) {
		if (cb_fn) {
			cb_fn(cb_arg, -EINVAL);
		}
		return;
	}

	dev->removed = true;
	dev->remove_cb = cb_fn;
	dev->remove_ctx = cb_arg;

	if (TAILQ_EMPTY(&dev->luns)) {
		free_dev(dev);
		return;
	}

	TAILQ_FOREACH_SAFE(lun, &dev->luns, tailq, tmp_lun) {
		/*
		 * LUN will remove itself from this dev when all outstanding IO
		 * is done. When no more LUNs, dev will be deleted.
		 */
		scsi_lun_destruct(lun);
	}
}

/*
 * Search the lowest free LUN ID if the LUN ID is default, or check if the LUN ID is free otherwise,
 * and also return the LUN which comes just before where we want to insert an new LUN.
 */
static int
scsi_dev_find_free_lun(struct spdk_scsi_dev *dev, int lun_id,
		       struct spdk_scsi_lun **prev_lun)
{
	struct spdk_scsi_lun *lun, *_prev_lun = NULL;

	if (prev_lun == NULL) {
		return -EINVAL;
	}

	if (lun_id == -1) {
		lun_id = 0;

		TAILQ_FOREACH(lun, &dev->luns, tailq) {
			if (lun->id > lun_id) {
				break;
			}
			lun_id = lun->id + 1;
			_prev_lun = lun;
		}

		if (lun_id >= SPDK_SCSI_DEV_MAX_LUN) {
			return -ENOSPC;
		}
	} else {
		TAILQ_FOREACH(lun, &dev->luns, tailq) {
			if (lun->id == lun_id) {
				return -EEXIST;
			} else if (lun->id > lun_id) {
				break;
			}
			_prev_lun = lun;
		}
	}

	*prev_lun = _prev_lun;
	return 0;
}

int
spdk_scsi_dev_add_lun(struct spdk_scsi_dev *dev, const char *bdev_name, int lun_id,
		      void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
		      void *hotremove_ctx)
{
	return spdk_scsi_dev_add_lun_ext(dev, bdev_name, lun_id,
					 NULL, NULL,
					 hotremove_cb, hotremove_ctx);
}

int
spdk_scsi_dev_add_lun_ext(struct spdk_scsi_dev *dev, const char *bdev_name, int lun_id,
			  void (*resize_cb)(const struct spdk_scsi_lun *, void *),
			  void *resize_ctx,
			  void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
			  void *hotremove_ctx)
{
	struct spdk_scsi_lun *lun, *prev_lun = NULL;
	int rc;

	if (lun_id >= SPDK_SCSI_DEV_MAX_LUN) {
		SPDK_ERRLOG("LUN ID %d is more than the maximum.\n", lun_id);
		return -1;
	}

	rc = scsi_dev_find_free_lun(dev, lun_id, &prev_lun);
	if (rc != 0) {
		SPDK_ERRLOG("%s\n", rc == -EEXIST ? "LUN ID is duplicated" : "Free LUN ID is not found");
		return rc;
	}

	lun = scsi_lun_construct(bdev_name, resize_cb, resize_ctx, hotremove_cb, hotremove_ctx);
	if (lun == NULL) {
		return -1;
	}

	lun->dev = dev;

	if (lun_id != -1) {
		lun->id = lun_id;
	} else if (prev_lun == NULL) {
		lun->id = 0;
	} else {
		lun->id = prev_lun->id + 1;
	}

	if (prev_lun == NULL) {
		TAILQ_INSERT_HEAD(&dev->luns, lun, tailq);
	} else {
		TAILQ_INSERT_AFTER(&dev->luns, prev_lun, lun, tailq);
	}
	return 0;
}

void
spdk_scsi_dev_delete_lun(struct spdk_scsi_dev *dev,
			 struct spdk_scsi_lun *lun)
{
	TAILQ_REMOVE(&dev->luns, lun, tailq);

	if (dev->removed && TAILQ_EMPTY(&dev->luns)) {
		free_dev(dev);
	}
}

struct spdk_scsi_dev *spdk_scsi_dev_construct(const char *name, const char *bdev_name_list[],
		int *lun_id_list, int num_luns, uint8_t protocol_id,
		void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
		void *hotremove_ctx)
{
	return spdk_scsi_dev_construct_ext(name, bdev_name_list, lun_id_list,
					   num_luns, protocol_id,
					   NULL, NULL,
					   hotremove_cb, hotremove_ctx);
}

struct spdk_scsi_dev *spdk_scsi_dev_construct_ext(const char *name, const char *bdev_name_list[],
		int *lun_id_list, int num_luns, uint8_t protocol_id,
		void (*resize_cb)(const struct spdk_scsi_lun *, void *),
		void *resize_ctx,
		void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
		void *hotremove_ctx)
{
	struct spdk_scsi_dev *dev;
	size_t name_len;
	bool found_lun_0;
	int i, rc;

	name_len = strlen(name);
	if (name_len > sizeof(dev->name) - 1) {
		SPDK_ERRLOG("device %s: name longer than maximum allowed length %zu\n",
			    name, sizeof(dev->name) - 1);
		return NULL;
	}

	if (num_luns == 0) {
		SPDK_ERRLOG("device %s: no LUNs specified\n", name);
		return NULL;
	}

	found_lun_0 = false;
	for (i = 0; i < num_luns; i++) {
		if (lun_id_list[i] == 0) {
			found_lun_0 = true;
			break;
		}
	}

	if (!found_lun_0) {
		SPDK_ERRLOG("device %s: no LUN 0 specified\n", name);
		return NULL;
	}

	for (i = 0; i < num_luns; i++) {
		if (bdev_name_list[i] == NULL) {
			SPDK_ERRLOG("NULL spdk_scsi_lun for LUN %d\n",
				    lun_id_list[i]);
			return NULL;
		}
	}

	dev = allocate_dev();
	if (dev == NULL) {
		return NULL;
	}

	memcpy(dev->name, name, name_len + 1);

	dev->num_ports = 0;
	dev->protocol_id = protocol_id;

	for (i = 0; i < num_luns; i++) {
		rc = spdk_scsi_dev_add_lun_ext(dev, bdev_name_list[i], lun_id_list[i],
					       resize_cb, resize_ctx,
					       hotremove_cb, hotremove_ctx);
		if (rc < 0) {
			spdk_scsi_dev_destruct(dev, NULL, NULL);
			return NULL;
		}
	}

	return dev;
}

void
spdk_scsi_dev_queue_mgmt_task(struct spdk_scsi_dev *dev,
			      struct spdk_scsi_task *task)
{
	assert(task != NULL);

	scsi_lun_execute_mgmt_task(task->lun, task);
}

void
spdk_scsi_dev_queue_task(struct spdk_scsi_dev *dev,
			 struct spdk_scsi_task *task)
{
	assert(task != NULL);

	scsi_lun_execute_task(task->lun, task);
}

static struct spdk_scsi_port *
scsi_dev_find_free_port(struct spdk_scsi_dev *dev)
{
	int i;

	for (i = 0; i < SPDK_SCSI_DEV_MAX_PORTS; i++) {
		if (!dev->port[i].is_used) {
			return &dev->port[i];
		}
	}

	return NULL;
}

int
spdk_scsi_dev_add_port(struct spdk_scsi_dev *dev, uint64_t id, const char *name)
{
	struct spdk_scsi_port *port;
	int rc;

	if (dev->num_ports == SPDK_SCSI_DEV_MAX_PORTS) {
		SPDK_ERRLOG("device already has %d ports\n", SPDK_SCSI_DEV_MAX_PORTS);
		return -1;
	}

	port = spdk_scsi_dev_find_port_by_id(dev, id);
	if (port != NULL) {
		SPDK_ERRLOG("device already has port(%" PRIu64 ")\n", id);
		return -1;
	}

	port = scsi_dev_find_free_port(dev);
	if (port == NULL) {
		assert(false);
		return -1;
	}

	rc = scsi_port_construct(port, id, dev->num_ports, name);
	if (rc != 0) {
		return rc;
	}

	dev->num_ports++;
	return 0;
}

int
spdk_scsi_dev_delete_port(struct spdk_scsi_dev *dev, uint64_t id)
{
	struct spdk_scsi_port *port;

	port = spdk_scsi_dev_find_port_by_id(dev, id);
	if (port == NULL) {
		SPDK_ERRLOG("device does not have specified port(%" PRIu64 ")\n", id);
		return -1;
	}

	scsi_port_destruct(port);

	dev->num_ports--;

	return 0;
}

struct spdk_scsi_port *
spdk_scsi_dev_find_port_by_id(struct spdk_scsi_dev *dev, uint64_t id)
{
	int i;

	for (i = 0; i < SPDK_SCSI_DEV_MAX_PORTS; i++) {
		if (!dev->port[i].is_used) {
			continue;
		}
		if (dev->port[i].id == id) {
			return &dev->port[i];
		}
	}

	/* No matching port found. */
	return NULL;
}

void
spdk_scsi_dev_free_io_channels(struct spdk_scsi_dev *dev)
{
	struct spdk_scsi_lun *lun, *tmp_lun;

	TAILQ_FOREACH_SAFE(lun, &dev->luns, tailq, tmp_lun) {
		scsi_lun_free_io_channel(lun);
	}
}

int
spdk_scsi_dev_allocate_io_channels(struct spdk_scsi_dev *dev)
{
	struct spdk_scsi_lun *lun, *tmp_lun;
	int rc;

	TAILQ_FOREACH_SAFE(lun, &dev->luns, tailq, tmp_lun) {
		rc = scsi_lun_allocate_io_channel(lun);
		if (rc < 0) {
			spdk_scsi_dev_free_io_channels(dev);
			return -1;
		}
	}

	return 0;
}

const char *
spdk_scsi_dev_get_name(const struct spdk_scsi_dev *dev)
{
	return dev->name;
}

int
spdk_scsi_dev_get_id(const struct spdk_scsi_dev *dev)
{
	return dev->id;
}

struct spdk_scsi_lun *
spdk_scsi_dev_get_lun(struct spdk_scsi_dev *dev, int lun_id)
{
	struct spdk_scsi_lun *lun;

	TAILQ_FOREACH(lun, &dev->luns, tailq) {
		if (lun->id == lun_id) {
			if (!spdk_scsi_lun_is_removing(lun)) {
				return lun;
			} else {
				return NULL;
			}
		}
	}

	return NULL;
}

struct spdk_scsi_lun *
spdk_scsi_dev_get_first_lun(struct spdk_scsi_dev *dev)
{
	struct spdk_scsi_lun *lun;

	TAILQ_FOREACH(lun, &dev->luns, tailq) {
		if (!spdk_scsi_lun_is_removing(lun)) {
			return lun;
		}
	}

	return NULL;
}

struct spdk_scsi_lun *
spdk_scsi_dev_get_next_lun(struct spdk_scsi_lun *prev_lun)
{
	struct spdk_scsi_dev *dev;
	struct spdk_scsi_lun *lun;

	if (prev_lun == NULL) {
		return NULL;
	}

	dev = prev_lun->dev;

	lun = TAILQ_NEXT(prev_lun, tailq);
	if (lun == NULL) {
		return NULL;
	}

	TAILQ_FOREACH_FROM(lun, &dev->luns, tailq) {
		if (!spdk_scsi_lun_is_removing(lun)) {
			break;
		}
	}

	return lun;
}

bool
spdk_scsi_dev_has_pending_tasks(const struct spdk_scsi_dev *dev,
				const struct spdk_scsi_port *initiator_port)
{
	struct spdk_scsi_lun *lun;

	TAILQ_FOREACH(lun, &dev->luns, tailq) {
		if (scsi_lun_has_pending_tasks(lun, initiator_port) ||
		    scsi_lun_has_pending_mgmt_tasks(lun, initiator_port)) {
			return true;
		}
	}

	return false;
}
