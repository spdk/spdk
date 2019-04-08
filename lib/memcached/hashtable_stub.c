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

#include "memcached/hashtable.h"

struct mem_item {
	uint64_t key_hash;
	struct disk_item *ditem; // disk_record
};

#define NUM_MITEM_STUB	16
static struct mem_item g_stub_mitems[NUM_MITEM_STUB] = {0};

int
spdk_memcached_get_memitem(uint64_t key_hash, struct mem_item **_mitem)
{
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "keyhash 0x%lx\n", key_hash);
	int i;
	struct mem_item *mitem;

	for (i = 0; i < NUM_MITEM_STUB; i++) {
		printf("i %d, item record %p\n", i, g_stub_mitems[i].ditem);
		if (spdk_memcached_memitem_is_valid(&g_stub_mitems[i]) == true &&
		    g_stub_mitems[i].key_hash == key_hash) {
			*_mitem = &g_stub_mitems[i];

			return 0;
		}
	}

	for (i = 0; i < NUM_MITEM_STUB; i++) {
		if (spdk_memcached_memitem_is_valid(&g_stub_mitems[i]) == false) {
			g_stub_mitems[i].key_hash = key_hash;
			*_mitem = &g_stub_mitems[i];

			return 0;
		}
	}

	// invalid the last one and return it
	mitem = &g_stub_mitems[NUM_MITEM_STUB - 1];
	spdk_memcached_invalid_memitem(mitem);
	mitem->key_hash = key_hash;
	*_mitem = mitem;

	return 0;
}


struct disk_item *
spdk_memcached_memitem_get_record(struct mem_item *mitem)
{
	return mitem->ditem;
}

void
spdk_memcached_memitem_set_record(struct mem_item *mitem, struct disk_item *ditem)
{
	mitem->ditem = ditem;
}


bool
spdk_memcached_memitem_is_valid(struct mem_item *mitem)
{
	return spdk_memcached_memitem_get_record(mitem) != NULL;
}

void
spdk_memcached_invalid_memitem(struct mem_item *mitem)
{
	mitem->key_hash = 0;
	spdk_memcached_memitem_set_record(mitem, NULL);
}


