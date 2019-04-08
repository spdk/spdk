/*
 * diskrecord.h
 *
 *  Created on: Apr 11, 2019
 *      Author: root
 */

#ifndef LIB_MEMCACHED_DISKRECORD_H_
#define LIB_MEMCACHED_DISKRECORD_H_

//TODO: reference memcached slab and slab_class

struct disk_item;

// operate the disk_item in memory
int spdk_memcached_get_diskitem(int size, struct disk_item **ditem);
int spdk_memcached_put_diskitem(struct disk_item *ditem);
bool spdk_memcached_diskitem_is_valid(struct disk_item *ditem);
int spdk_memcached_diskitem_get_data_size(struct disk_item *ditem);

// used to store and obtain data to/from disk
typedef void (*spdk_memcached_diskitem_cb)(void *cb_arg, int err);

int spdk_memcached_diskitem_store(struct disk_item *ditem, const char *buf, uint32_t len,
				  spdk_memcached_diskitem_cb cb, void *cb_arg);
int spdk_memcached_diskitem_obtain(struct disk_item *ditem, char *buf, uint32_t len,
				   spdk_memcached_diskitem_cb cb, void *cb_arg);


int spdk_memcached_diskitem_storev(struct disk_item *ditem, struct iovec *iov, int iovcnt,
				   spdk_memcached_diskitem_cb cb, void *cb_arg);
int spdk_memcached_diskitem_obtainv(struct disk_item *ditem, struct iovec *iov, int iovcnt,
				    spdk_memcached_diskitem_cb cb, void *cb_arg);


#endif /* LIB_MEMCACHED_DISKRECORD_H_ */
