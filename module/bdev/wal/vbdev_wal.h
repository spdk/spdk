#pragma once

#include <spdk/bdev.h>
#include "spdk/bdev_module.h"

#define max(a, b) (((a) > (b)) ? (a) : (b))

/**
 * @brief Callback function for WAL bdev creation.
 *
 * @param cb_arg User-provided argument.
 * @param rc 0 on success, or a negative errno on failure.
 */
typedef void (*wal_bdev_create_cb)(void *cb_arg, int rc);

/**
 * @brief Create a new WAL (Write Ahead Log) virtual block device.
 *
 * This function initializes the WAL module, linking a main data bdev and a
 * journal bdev. It performs asynchronous initialization of the journal superblock.
 *
 * @param main_bdev_name Name of the main underlying bdev.
 * @param journal_bdev_name Name of the journal underlying bdev.
 * @param name Name of the new WAL virtual bdev to be created.
 * @param block_sz Pointer to store the resulting block size (optional).
 * @param size_mb Pointer to store the resulting device size in MB (optional).
 * @param cb_fn Callback function to be called upon completion.
 * @param cb_arg Argument to pass to the callback.
 *
 * @return 0 if the creation process started successfully, negative errno otherwise.
 */
int wal_bdev_create_disk(char *main_bdev_name,
			 char *journal_bdev_name,
			 char *name,
			 uint32_t *block_sz,
			 uint64_t *size_mb,
			 wal_bdev_create_cb cb_fn,
			 void *cb_arg);

/**
 * @brief Delete an existing WAL virtual block device.
 *
 * @param name Name of the WAL bdev to delete.
 * @param cb_fn Callback function to be called upon successful deletion.
 * @param cb_arg Argument to pass to the callback.
 *
 * @return 0 if the deletion started successfully, negative errno otherwise.
 */
int wal_bdev_delete_disk(char *name,
			 spdk_bdev_unregister_cb cb_fn,
			 void *cb_arg);

/**
 * @brief Initiate recovery of the main bdev using the journal.
 *
 * Replays valid, uncommitted records from the journal device to the main device.
 *
 * @param name Name of the WAL bdev to recover.
 * @param cb_fn Callback function to be called upon recovery completion.
 * @param cb_arg Argument to pass to the callback.
 *
 * @return 0 if the recovery process started successfully, negative errno otherwise.
 */
int wal_bdev_recover(const char *name,
		     spdk_bdev_unregister_cb cb_fn,
		     void *cb_arg);
