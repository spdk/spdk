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
spdk_scsi_dev_get_list(void)
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
			return dev;
		}
	}

	return NULL;
}

static void
free_dev(struct spdk_scsi_dev *dev)
{
	dev->is_allocated = 0;
}

void
spdk_scsi_dev_destruct(struct spdk_scsi_dev *dev)
{
	int i;

	if (dev == NULL) {
		return;
	}

	for (i = 0; i < SPDK_SCSI_DEV_MAX_LUN; i++) {
		if (dev->lun[i] == NULL) {
			continue;
		}

		spdk_scsi_lun_unclaim(dev->lun[i]);
		spdk_scsi_lun_destruct(dev->lun[i]);
		dev->lun[i] = NULL;
	}

	free_dev(dev);
}

static int
spdk_scsi_dev_add_lun(struct spdk_scsi_dev *dev,
		      struct spdk_scsi_lun *lun, int id)
{
	int rc;

	rc = spdk_scsi_lun_claim(lun);
	if (rc < 0) {
		return rc;
	}

	lun->id = id;
	lun->dev = dev;
	dev->lun[id] = lun;

	return 0;
}

void
spdk_scsi_dev_delete_lun(struct spdk_scsi_dev *dev,
			 struct spdk_scsi_lun *lun)
{
	int i;

	for (i = 0; i < SPDK_SCSI_DEV_MAX_LUN; i++) {
		if (dev->lun[i] == lun)
			dev->lun[i] = NULL;
	}
}

/* This typedef exists to work around an astyle 2.05 bug.
 * Remove it when astyle is fixed.
 */
typedef struct spdk_scsi_dev _spdk_scsi_dev;

_spdk_scsi_dev *
spdk_scsi_dev_construct(const char *name, char *lun_name_list[], int *lun_id_list, int num_luns,
			uint8_t protocol_id, void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
			void *hotremove_ctx)
{
	struct spdk_scsi_dev *dev;
	struct spdk_bdev *bdev;
	struct spdk_scsi_lun *lun = NULL;
	int i, rc;

	if (num_luns == 0) {
		SPDK_ERRLOG("device %s: no LUNs specified\n", name);
		return NULL;
	}

	if (lun_id_list[0] != 0) {
		SPDK_ERRLOG("device %s: no LUN 0 specified\n", name);
		return NULL;
	}

	for (i = 0; i < num_luns; i++) {
		if (lun_name_list[i] == NULL) {
			SPDK_ERRLOG("NULL spdk_scsi_lun for LUN %d\n",
				    lun_id_list[i]);
			return NULL;
		}
	}

	dev = allocate_dev();
	if (dev == NULL) {
		return NULL;
	}

	strncpy(dev->name, name, SPDK_SCSI_DEV_MAX_NAME);

	dev->num_ports = 0;
	dev->protocol_id = protocol_id;

	for (i = 0; i < num_luns; i++) {
		bdev = spdk_bdev_get_by_name(lun_name_list[i]);
		if (bdev == NULL) {
			goto error;
		}

		lun = spdk_scsi_lun_construct(spdk_bdev_get_name(bdev), bdev, hotremove_cb, hotremove_ctx);
		if (lun == NULL) {
			goto error;
		}

		rc = spdk_scsi_dev_add_lun(dev, lun, lun_id_list[i]);
		if (rc < 0) {
			spdk_scsi_lun_destruct(lun);
			goto error;
		}
	}

	return dev;

error:
	spdk_scsi_dev_destruct(dev);

	return NULL;
}

void
spdk_scsi_dev_queue_mgmt_task(struct spdk_scsi_dev *dev,
			      struct spdk_scsi_task *task,
			      enum spdk_scsi_task_func func)
{
	assert(task != NULL);

	task->function = func;
	spdk_scsi_lun_task_mgmt_execute(task, func);
}

void
spdk_scsi_dev_queue_task(struct spdk_scsi_dev *dev,
			 struct spdk_scsi_task *task)
{
	assert(task != NULL);

	if (spdk_scsi_lun_append_task(task->lun, task) == 0) {
		/* ready to execute, disk is valid for LUN access */
		spdk_scsi_lun_execute_tasks(task->lun);
	}
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

	port = &dev->port[dev->num_ports];

	rc = spdk_scsi_port_construct(port, id, dev->num_ports, name);
	if (rc != 0) {
		return rc;
	}

	dev->num_ports++;
	return 0;
}

struct spdk_scsi_port *
spdk_scsi_dev_find_port_by_id(struct spdk_scsi_dev *dev, uint64_t id)
{
	int i;

	for (i = 0; i < dev->num_ports; i++) {
		if (dev->port[i].id == id) {
			return &dev->port[i];
		}
	}

	/* No matching port found. */
	return NULL;
}

void
spdk_scsi_dev_print(struct spdk_scsi_dev *dev)
{
	struct spdk_scsi_lun *lun;
	int i;

	printf("device %d HDD UNIT\n", dev->id);

	for (i = 0; i < SPDK_SCSI_DEV_MAX_LUN; i++) {
		lun = dev->lun[i];
		if (lun == NULL)
			continue;
		printf("device %d: LUN%d %s\n", dev->id, i, lun->name);
	}
}

void
spdk_scsi_dev_free_io_channels(struct spdk_scsi_dev *dev)
{
	int i;

	for (i = 0; i < SPDK_SCSI_DEV_MAX_LUN; i++) {
		if (dev->lun[i] == NULL) {
			continue;
		}
		spdk_scsi_lun_free_io_channel(dev->lun[i]);
	}
}

int
spdk_scsi_dev_allocate_io_channels(struct spdk_scsi_dev *dev)
{
	int i, rc;

	for (i = 0; i < SPDK_SCSI_DEV_MAX_LUN; i++) {
		if (dev->lun[i] == NULL) {
			continue;
		}
		rc = spdk_scsi_lun_allocate_io_channel(dev->lun[i]);
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
	if (lun_id < 0 || lun_id >= SPDK_SCSI_DEV_MAX_LUN) {
		return NULL;
	}

	return dev->lun[lun_id];
}

bool
spdk_scsi_dev_has_pending_tasks(const struct spdk_scsi_dev *dev)
{
	int i;

	for (i = 0; i < SPDK_SCSI_DEV_MAX_LUN; ++i) {
		if (dev->lun[i] && spdk_scsi_lun_has_pending_tasks(dev->lun[i])) {
			return true;
		}
	}

	return false;
}
