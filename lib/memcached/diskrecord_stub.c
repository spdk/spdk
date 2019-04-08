/*
 * hashtable.c
 *
 *  Created on: Apr 11, 2019
 *      Author: root
 */

#include "spdk/stdinc.h"
#include "spdk/util.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk_internal/log.h"

#include "memcached/diskrecord.h"

struct disk_item {
	uint64_t blk_offset;
	uint64_t blk_num;
	uint64_t data_size;
	struct disk_item *next;
};

int spdk_memcached_diskitem_get_data_size(struct disk_item *ditem)
{
	return ditem->data_size;
}

int spdk_memcached_get_diskitem(int size, struct disk_item **_ditem)
{
	struct disk_item *ditem = calloc(1, sizeof(ditem));
	char *buf;

	ditem->blk_num = (size + 511) >> 9;

	buf = malloc(ditem->blk_num * 512);
	ditem->blk_offset = (uint64_t)buf;

	*_ditem = ditem;
	return 0;
}

int spdk_memcached_put_diskitem(struct disk_item *ditem)
{
	if (ditem == NULL) {
		return 0;
	}

	free((char *)ditem->blk_offset);
	free(ditem);
	return 0;
}

bool spdk_memcached_diskitem_is_valid(struct disk_item *ditem)
{
	if (ditem == NULL) {
		return false;
	}

	return ditem->blk_offset != 0;
}

int spdk_memcached_diskitem_store(struct disk_item *ditem, const char *buf, uint32_t len,
				  spdk_memcached_diskitem_cb cb, void *cb_arg)
{
	assert(ditem);
	assert(buf);

	if (len > ditem->blk_num * 512) {
		return -1;
	}

	memcpy((char *)ditem->blk_offset, buf, len);
	ditem->data_size = len;
	cb(cb_arg, 0);

	return 0;
}

int spdk_memcached_diskitem_obtain(struct disk_item *ditem, char *buf, uint32_t len,
				   spdk_memcached_diskitem_cb cb, void *cb_arg)
{
	assert(ditem);
	assert(buf);

	if (len < ditem->data_size) {
		return -1;
	}

	memcpy(buf, (char *)ditem->blk_offset, len);
	cb(cb_arg, 0);

	return 0;
}


