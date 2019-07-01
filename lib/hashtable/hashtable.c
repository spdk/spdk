/*
 * hashtable.c
 *
 *  Created on: Jul 1, 2019
 *      Author: root
 */

#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/uuid.h"
#include "spdk/bdev_module.h"
#include "spdk/env.h"

#include "spdk_internal/log.h"
#include "spdk/hashtable.h"

#define SPDK_HASHTABLE_MAX_LCORE		64
#define SPDK_HASHTABLE_ITEM_POOL_SIZE		1024
#define SPDK_HASHTABLE_GAINING_POLL_IN_USEC	1000 * 1000

#define HASHTABLE_ITEM_MASK		0xFFFFF /* One hash block has 1M (2^20) hash items */
#define HASHTABLE_BLOCK_OFFSET		20
#define HASHTABLE_BLOCK_MASK		0xFFF	/* One thread has 4K (2^12) hash blocks */
#define HASHTABLE_THREAD_MASK_OFFSET	(20 + 12)


struct hashitem {
	uint64_t key_hash;	/* Item is decided only by key_hash */

	/* link all items which have a same hash part or hash value */
	struct hashitem *next;
	struct hashitem
		*prev;	/* If item is inside block, then prev should always be NULL, while collision items are not */

//	union {			/* link all items which have a same hash part or hash value */
//		TAILQ_ENTRY(hashitem) tailq;
//		TAILQ_HEAD(, hashitem) head;
//	};

	/* whether slot is NULL is used to indicate whether this item is in using */
	struct slot_item *slot; /* Point to the slot of storage info */
	uint32_t stored_size;	/* Give an indication to prepare receive buffer */

	uint32_t flags;		/* Reserved for concurrent operations */
};

struct hashblock {
	struct hashitem items[1 << HASHTABLE_BLOCK_OFFSET];
};

enum hashblock_type {
	EXCLUSIVE_BLK,	/* block whose items are only belonging to itself */
	SLIMMING_BLK,	/* block whose items are partially belonging to a partner */
	GAINING_BLK,	/* block whose items are not still existed inside its partner */
	SNAPSHOT_BLK,	/* block which is waiting for memory allocation, so it still points to its partner */
};

struct hashblock_stat {
	uint32_t items_used_num;
	enum hashblock_type type;
};

struct hashtable_percore {
	/* hashblock array should be 2^N size for addressing */
	struct hashblock **blocks;
	struct hashblock_stat **states;
	uint32_t size_blocks;

	struct spdk_mempool *collision_pool;	/* Only used to contain hash collisioned items */

	struct spdk_poller *gaining_poller;

	/* blocks statistics */
	uint32_t snapshot_num;	/* Before it becomes 0, hashtable expansion should not be permitted */
	int current_gaining_blk;	/* -1 means no block is in progress of gaining */
};

struct spdk_hashtable {
	struct spdk_thread **threads;
	uint32_t threads_size;

	struct hashtable_percore percores[SPDK_HASHTABLE_MAX_LCORE];
};

struct spdk_hashtable g_hashtable;


static int
spdk_thread_get_cpu_index(void)
{
	struct spdk_cpuset *thd_cpumask;
	int cpu_idx;

	thd_cpumask = spdk_thread_get_cpumask(spdk_get_thread());
	SPDK_DEBUGLOG(SPDK_LOG_HASHTABLE, "thd cpumask is %s\n", spdk_cpuset_fmt(thd_cpumask));

	cpu_idx = spdk_cpuset_first_index(thd_cpumask);
	assert(cpu_idx >= 0);

	return cpu_idx;
}

static struct hashtable_percore *
spdk_thread_get_hashtable_percore(void)
{
	struct hashtable_percore *percore;
	struct spdk_cpuset *thd_cpumask;
	int cpu_idx;

	thd_cpumask = spdk_thread_get_cpumask(spdk_get_thread());
//	SPDK_DEBUGLOG(SPDK_LOG_HASHTABLE, "thd cpumask is %s\n", spdk_cpuset_fmt(thd_cpumask));

	cpu_idx = spdk_cpuset_first_index(thd_cpumask);
	assert(cpu_idx >= 0);
	if (cpu_idx > 3) {
		return NULL;
	}

	percore = &g_hashtable.percores[cpu_idx];

	return percore;
}

struct spdk_thread *
spdk_hashtable_locate_thread(uint64_t key_hash)
{
	uint32_t hash_thd_part;
	uint32_t thd_idx;
	struct spdk_thread *hash_thd;

	hash_thd_part = key_hash >> HASHTABLE_THREAD_MASK_OFFSET;
	thd_idx = hash_thd_part & (g_hashtable.threads_size - 1);

	hash_thd = g_hashtable.threads[thd_idx];

	SPDK_DEBUGLOG(SPDK_LOG_HASHTABLE, "keyhash is 0x%lx; thd_idx is %d; thd is %p\n", key_hash, thd_idx, hash_thd);
	return hash_thd;
}

static struct hashblock *
spdk_hashtable_locate_block(struct hashtable_percore *percore, uint64_t key_hash)
{
	uint32_t hashtable_blocks_size = percore->size_blocks;
	uint32_t hash_block_part;
	uint32_t block_idx;
	struct hashblock *block;

	assert(percore);
	hash_block_part = key_hash >> HASHTABLE_BLOCK_OFFSET;
	block_idx = hash_block_part & (hashtable_blocks_size - 1);

	block = percore->blocks[block_idx];

	return block;
}

int
spdk_hashtable_locate_new_items(uint64_t key_hash, struct hashitem **new_item,
				struct hashitem **existed_items, int existed_num)
{
	struct hashtable_percore *percore;
	struct hashblock *block;
	uint32_t item_idx;
	struct hashitem *first_item;
	int item_count = 0;
	bool found_new = false;

	percore = spdk_thread_get_hashtable_percore();
	block = spdk_hashtable_locate_block(percore, key_hash);

	item_idx = key_hash & HASHTABLE_ITEM_MASK;
	first_item = &block->items[item_idx];

	/* the first item is also the head for item list */
	if (first_item->slot == NULL) {
		if (new_item != NULL) {
			/* set the first item with NULL slot as the new item */
			*new_item = first_item;
			found_new = true;
		}
	} else {
		if (first_item->key_hash == key_hash) {
			if (existed_items != NULL && item_count < existed_num) {
				existed_items[item_count] = first_item;
			}
			item_count++;
		}
	}

	struct hashitem *item;
	for (item = first_item->next; item != NULL; item = item->next) {
		/* In case the item is unclaimed, but haven' been removed from list */
		if (item->slot == NULL) {
			if (found_new == false && new_item != NULL) {
				*new_item = item;
				found_new = true;
			}

			continue;
		}

		if (item->key_hash != key_hash) {
			continue;
		}


		if (existed_items != NULL && item_count < existed_num) {
			existed_items[item_count] = item;
		}

		item_count++;
	}

	if (found_new == false && new_item != NULL) {
		item = spdk_mempool_get(percore->collision_pool);
		if (item == NULL) {
			//TODO: expansion is required
			assert(0);
			*new_item = NULL;
		} else {
//			TAILQ_INSERT_TAIL(&item->head, item, tailq);
			item->next = first_item->next;
			item->prev = first_item;
			first_item->next = item;
			if (item->next != NULL) {
				item->next->prev = item;
			}

			*new_item = item;
		}
	}

	if (new_item != NULL && *new_item != NULL) {
		(*new_item)->key_hash = key_hash;
	}

	return item_count;
}


int
spdk_hashtable_locate_existed_items(uint64_t key_hash, struct hashitem **existed_items,
				    int existed_num)
{
	return spdk_hashtable_locate_new_items(key_hash, NULL, existed_items, existed_num);
}


bool
spdk_hashtable_is_existed_item(uint64_t key_hash)
{
	struct hashtable_percore *percore;
	struct hashblock *block;
	uint32_t item_idx;
	struct hashitem *first_item;

	percore = spdk_thread_get_hashtable_percore();
	block = spdk_hashtable_locate_block(percore, key_hash);

	item_idx = key_hash & HASHTABLE_ITEM_MASK;
	first_item = &block->items[item_idx];

	/* the first item is also the head for item list */
	if (first_item->slot != NULL && first_item->key_hash == key_hash) {
			return true;
	}

	struct hashitem *item;
	for (item = first_item->next; item != NULL; item = item->next) {
		/* In case the item is unclaimed, but haven' been removed from list */
		if (item->slot == NULL) {
			continue;
		}

		if (item->key_hash != key_hash) {
			continue;
		}

		return true;
	}

	return false;
}

int
spdk_hashtable_release_item(struct hashitem *item)
{
	struct hashtable_percore *percore;
	item->key_hash = 0;
	item->slot = NULL;
	item->stored_size = 0;


	percore = spdk_thread_get_hashtable_percore();

	/* Operations for collision items */
	if (item->prev != NULL) {
		item->prev->next = item->next;
		if (item->next != NULL) {
			item->next->prev = item->prev;
		}

		item->next = NULL;
		item->prev = (void *)0xF;

		spdk_mempool_put(percore->collision_pool, (void *)item);
	}

	return 0;
}

/* hashtable expansion logic */
#if 0
static int
spdk_hashtable_expansion(struct hashtable_percore *percore)
{
	struct hashblock **blocks, **new_blocks;
	uint32_t size_blocks;

	blocks = realloc(percore->blocks, percore->size_blocks * sizeof(struct hashblock *) * 2);
	assert(blocks);

	new_blocks = &blocks[percore->size_blocks];
	memcpy(new_blocks, blocks, percore->size_blocks * sizeof(struct hashblock *));

	percore->blocks = blocks;
	percore->size_blocks = percore->size_blocks * 2;

	return 0;
}

static int
_head_hashitem_migration()
{
	struct hashitem *gaining_head_item, *slimming_head_item;
	struct hashitem *item;

	item = slimming_head_item
	       for ()
	}

static int
spdk_hashtable_block_migration()
{
	int item_idx_previous, item_idx_expected;
	struct hashblock *gaining_blk, *slimming_blk;
	int item_idx;
	int total_items = 1 << HASHTABLE_BLOCK_OFFSET;

	for (item_idx = item_idx_previous; item_idx < item_idx_expected; item_idx++) {

	}
}
#endif


static int
hashtable_blocks_init(struct hashtable_percore *percore, int thd_idx)
{
	struct hashblock **blocks_array;
	struct hashblock_stat **states;
	uint32_t size_blocks = 2;
	uint32_t i;
	char mempool_name[64];

	snprintf(mempool_name, sizeof(mempool_name), "ht_collision_pool_%d", thd_idx);

	blocks_array = calloc(size_blocks, sizeof(struct hashblock *));
	assert(blocks_array);

	states = calloc(size_blocks, sizeof(struct hashblock_stat *));
	assert(states);

	percore->size_blocks = size_blocks;
	percore->blocks = blocks_array;
	percore->states = states;
	percore->collision_pool = spdk_mempool_create(mempool_name,
				  SPDK_HASHTABLE_ITEM_POOL_SIZE, sizeof(struct hashitem),
				  4, SPDK_ENV_SOCKET_ID_ANY);
	assert(percore->collision_pool);

	for (i = 0; i < size_blocks; i++) {
		blocks_array[i] = calloc(1, sizeof(struct hashblock));
		assert(blocks_array[i]);

		states[i] = calloc(1, sizeof(struct hashblock_stat));
		assert(states[i]);
	}

	percore->current_gaining_blk = -1;

	return 0;
}


static int
spdk_hashtable_poll_expansion(void *arg)
{
	struct hashtable_percore *percore = arg;

	(void)percore;

	return 0;
}

static void
_hashtable_percore_start_cpl(void *ctx)
{
	SPDK_DEBUGLOG(SPDK_LOG_HASHTABLE, "All pollers are registerred\n");
}

static void
_hashtable_percore_start(void *ctx)
{
	struct hashtable_percore *percore;

	int cpu_idx;

	cpu_idx = spdk_thread_get_cpu_index();
	g_hashtable.threads[cpu_idx] = spdk_get_thread();

	percore = spdk_thread_get_hashtable_percore();
	if (percore == NULL) {
		return;
	}

	SPDK_DEBUGLOG(SPDK_LOG_HASHTABLE, "Register poller for percore %p\n", percore);
	percore->gaining_poller = spdk_poller_register(spdk_hashtable_poll_expansion,
				  percore, SPDK_HASHTABLE_GAINING_POLL_IN_USEC);

	assert(percore->gaining_poller);
}

int
spdk_hashtable_create(struct spdk_cpuset *core_mask)
{
	struct hashtable_percore *percore;
	int i;
	int thd_count = 4;

	g_hashtable.threads_size = thd_count;
	g_hashtable.threads = calloc(thd_count, sizeof(struct spdk_thread *));
	assert(g_hashtable.threads);

	for (i = 0; i < thd_count; i++) {
		percore = &g_hashtable.percores[i];
		hashtable_blocks_init(percore, i);
	}

	/* register poller for hashtable expansion work */
	spdk_for_each_thread(_hashtable_percore_start, NULL, _hashtable_percore_start_cpl);

	return 0;
}

#if 0

struct hashitem *
spdk_hashtable_locate_item(uint64_t key_hash)
{
	struct hashblock *block;
	uint32_t item_idx;
	struct hashitem *item;

	block = spdk_hashtable_locate_block(key_hash);

	item_idx = key_hash & HASHTABLE_ITEM_MASK;
	item = block[item_idx];

	return item;
}
#endif

void
spdk_hashtable_item_set_info(struct hashitem *item, struct slot_item *slot,
				  uint32_t stored_size)
{
	item->slot = slot;
	item->stored_size = stored_size;
}

void
spdk_hashtable_item_get_info(struct hashitem *item, struct slot_item **slot,
				  uint32_t *stored_size)
{
	*slot = item->slot;
	*stored_size = item->stored_size;
}


SPDK_LOG_REGISTER_COMPONENT("hashtable", SPDK_LOG_HASHTABLE)

