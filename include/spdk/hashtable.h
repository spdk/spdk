/*
 * hashtable.h
 *
 *  Created on: Jul 1, 2019
 *      Author: root
 */

#ifndef SPDK_HASHTABLE_H_
#define SPDK_HASHTABLE_H_


#include "spdk/stdinc.h"
#include "spdk/cpuset.h"

//TODO: concurrent requests from different conns may occur
enum hashitem_stat {
	IN_READ,	/* Read type operations are on item */
	WAIT_FOR_WRITE,	/* Write type operation is waiting for the completion of on-going read type operations */
	IN_WRITE,	/* Write type operation is on item, further write operations should wait */
};

struct hashitem;
struct slot_item;


/*
 * Check which thread the key_hash belongs to.
 */
struct spdk_thread *spdk_hashtable_locate_thread(uint64_t key_hash);

/*
 * Check whether the item with the key_hash is existed.
 */
bool spdk_hashtable_is_existed_item(uint64_t key_hash);

/*
 * Output all items with the key_hash
 *
 * Return Number of items founded for key_hash.
 */
int spdk_hashtable_locate_existed_items(uint64_t key_hash, struct hashitem **existed_items,
					int existed_num);

/*
 * Existed items will also be returned, in order to verify which existed item should be recycled.
 *
 * Return Number of items founded for key_hash. (new_item is excluded)
 */
int spdk_hashtable_locate_new_items(uint64_t key_hash, struct hashitem **new_item,
				    struct hashitem **existed_items, int existed_num);

int spdk_hashtable_release_item(struct hashitem *item);


int spdk_hashtable_create(struct spdk_cpuset *core_mask);

void spdk_hashtable_item_set_info(struct hashitem *item, struct slot_item *slot,
				  uint32_t stored_size);
void spdk_hashtable_item_get_info(struct hashitem *item, struct slot_item **slot,
				  uint32_t *stored_size);



#if 0

struct hashitem *spdk_hashtable_locate_item(uint64_t key_hash);
int spdk_hashtable_claim_item(struct hashitem *item, uint64_t key_hash);
int spdk_hashtable_unclaim_item(struct hashitem *item);

bool spdk_hashtable_item_is_claimed(struct hashitem *item);
/* Even key_hash is matched, the actual key may be still different */
bool spdk_hashtable_item_is_matched(struct hashitem *item, uint64_t key_hash);


int spdk_hashtable_create(struct spdk_cpuset *core_mask);
#endif

#if 0
struct mem_item;
struct disk_item;

int spdk_memcached_get_memitem(uint64_t key_hash, struct mem_item **mitem);

bool spdk_memcached_memitem_is_valid(struct mem_item *mitem);
void spdk_memcached_invalid_memitem(struct mem_item *mitem);

struct disk_item *spdk_memcached_memitem_get_record(struct mem_item *mitem);
void spdk_memcached_memitem_set_record(struct mem_item *mitem, struct disk_item *ditem);


/* dynamic expansion after enough hash collision rate */
#endif


#endif /* SPDK_HASHTABLE_H_ */
