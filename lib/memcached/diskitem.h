
#ifndef LIB_MEMCACHED_DISKITEM_H_
#define LIB_MEMCACHED_DISKITEM_H_

#include "memcached/memcached_cmd.h"

/* each item in disk contains diskrecord + key + data */
struct spdk_memcached_diskitem {
	//TODO: add attributes to each diskrecord
	uint64_t attribute[7];
	uint32_t key_len;	/* strlen of key */
	uint32_t data_len;

	char key[0];
	/* key string + '\0' + data + '\0' */
};

static inline int memcached_diskitem_required_size(struct spdk_memcached_cmd_header *hd);
static inline int memcached_diskitem_total_size(struct spdk_memcached_diskitem *ditem);

static inline uint32_t memcached_diskitem_get_key_len(struct spdk_memcached_diskitem *ditem);
static inline uint32_t memcached_diskitem_get_data_len(struct spdk_memcached_diskitem *ditem);
static inline char *memcached_diskitem_get_key(struct spdk_memcached_diskitem *ditem);
static inline char *memcached_diskitem_get_data(struct spdk_memcached_diskitem *ditem);

static inline void memcached_diskitem_set_head_key(struct spdk_memcached_diskitem *ditem,
		struct spdk_memcached_cmd_header *hd);


/* API implementations */
static inline int
memcached_diskitem_required_size(struct spdk_memcached_cmd_header *hd)
{
	int len = 0;

	len += sizeof(struct spdk_memcached_diskitem);
	len += hd->key_len;
	len += hd->data_len;

	/* reserve '\0' for key and data */
	len += 2;

	return len;
}

static inline int
memcached_diskitem_total_size(struct spdk_memcached_diskitem *ditem)
{
	int len = 0;

	len += sizeof(struct spdk_memcached_diskitem);
	len += ditem->key_len;
	len += ditem->data_len;

	/* reserve '\0' for key and data */
	len += 2;

	return len;
}

static inline uint32_t
memcached_diskitem_get_key_len(struct spdk_memcached_diskitem *ditem)
{
	return ditem->key_len;
}

static inline uint32_t
memcached_diskitem_get_data_len(struct spdk_memcached_diskitem *ditem)
{
	return ditem->data_len;
}
static inline char *
memcached_diskitem_get_key(struct spdk_memcached_diskitem *ditem)
{
	return ditem->key;
}

static inline char *
memcached_diskitem_get_data(struct spdk_memcached_diskitem *ditem)
{
	return ditem->key + ditem->key_len + 1;
}

static inline void
memcached_diskitem_set_head_key(struct spdk_memcached_diskitem *ditem,
				struct spdk_memcached_cmd_header *hd)
{
	ditem->key_len = hd->key_len;
	ditem->data_len = hd->data_len;
	memcpy(ditem->key, hd->key, ditem->key_len);
	ditem->key[ditem->key_len] = '\0';
}

#endif /* LIB_MEMCACHED_DISKITEM_H_ */
