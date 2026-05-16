#pragma once

#include <spdk/bdev.h>
#include "spdk/bdev_module.h"

#define max(a, b) (((a) > (b)) ? (a) : (b))

typedef void (*wal_bdev_create_cb)(void *cb_arg, int rc);

int wal_bdev_create_disk(char *main_bdev_name,
			 char *journal_bdev_name,
			 char *name,
			 uint32_t *block_sz,
			 uint64_t *size_mb,
			 wal_bdev_create_cb cb_fn,
			 void *cb_arg);
int wal_bdev_delete_disk(char *name,
			 spdk_bdev_unregister_cb cb_fn,
			 void *cb_arg);
int wal_bdev_recover(const char *name,
		     spdk_bdev_unregister_cb cb_fn,
		     void *cb_arg);
