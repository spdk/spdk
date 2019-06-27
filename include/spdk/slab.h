
#ifndef SPDK_SLAB_H_
#define SPDK_SLAB_H_

#include "spdk/stdinc.h"
#include "spdk/cpuset.h"

#if 0




int spdk_memcached_diskitem_storev(struct disk_item *ditem, struct iovec *iov, int iovcnt,
				   spdk_memcached_diskitem_cb cb, void *cb_arg);
int spdk_memcached_diskitem_obtainv(struct disk_item *ditem, struct iovec *iov, int iovcnt,
				    spdk_memcached_diskitem_cb cb, void *cb_arg);
#endif

/* Slab Initializtion API */
struct spdk_slab_manager;
struct spdk_slab_opts {

};

typedef void (* spdk_slab_mgr_op_with_handle_complete)(void *cb_arg, int slab_errno);

int spdk_slab_mgr_create(const char *bdev_name, struct spdk_cpuset *core_mask,
			 struct spdk_slab_opts *o,
			 spdk_slab_mgr_op_with_handle_complete cb_fn, void *cb_arg);


/* Runtime API */
#if 1
struct spdk_slot_item;

// operate the slab_item in memory
int spdk_slab_get_item(int size, struct spdk_slot_item **item);
int spdk_slab_put_item(struct spdk_slot_item *item);

bool spdk_slab_item_is_valid(struct spdk_slot_item *item);
int spdk_slab_item_get_data_size(struct spdk_slot_item *item);

// used to store and obtain data to/from disk
typedef void (*spdk_slab_item_rw_cb)(void *cb_arg, int err);

int spdk_slab_item_store(struct spdk_slot_item *item, const char *buf, uint32_t len,
			 spdk_slab_item_rw_cb cb, void *cb_arg);
int spdk_slab_item_obtain(struct spdk_slot_item *item, char *buf, uint32_t len,
			  spdk_slab_item_rw_cb cb, void *cb_arg);

#endif



#endif /* SPDK_SLAB_H_ */
