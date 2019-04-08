/*
 * hashtable.h
 *
 *  Created on: Apr 11, 2019
 *      Author: root
 */

#ifndef LIB_MEMCACHED_HASHTABLE_H_
#define LIB_MEMCACHED_HASHTABLE_H_

struct mem_item;
struct disk_item;

int spdk_memcached_get_memitem(uint64_t key_hash, struct mem_item **mitem);

bool spdk_memcached_memitem_is_valid(struct mem_item *mitem);
void spdk_memcached_invalid_memitem(struct mem_item *mitem);

struct disk_item *spdk_memcached_memitem_get_record(struct mem_item *mitem);
void spdk_memcached_memitem_set_record(struct mem_item *mitem, struct disk_item *ditem);

/* dynamic expansion after enough hash collision rate */


#endif /* LIB_MEMCACHED_HASHTABLE_H_ */
