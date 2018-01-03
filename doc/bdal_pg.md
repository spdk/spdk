#SPDK BDAL Programmer's Guide

## Target Audience

This programming guide is intended for developers authoring applications that use the SPDK
bdev library or libraries that want to implement block device abstraction layer.
First part is intended to supplement the source code to provide an overall understanding of how to
integrate bdev library into an application. Second part is intended explain how to write backend
library to fulfill block device abstraction layer requirements. It is not intended to serve as a design
document or an API reference, but in some cases source code snippets and high level sequences
will be discussed.

For the latest source code reference, refer to the [repo](https://github.com/spdk).

## Introduction

The SPDK BDAL Programming guide includes two parts:

1. bdev library uasge in application
2. library implementing bdev interface

Block device abstraction layer provides interface for spdk based applications to perform block i/o operations.
Note that API is created in such way that it provides block operations it means that blocks have constant length.


### Callbacks

Bdev library is callback driven. In the event that any bdev API is unable to make forward progress it will not block
but instead return control at that point and make a call to the callback function provided in the API, along with
arguments, when the original call was made. The callback will be made on the same thread that the call was made from, more on
threads later. Some API, however, offer no callback arguments; in these cases the calls are fully synchronous. Examples of
asynchronous calls that utilize callbacks include those that involve disk IO, for example, where some amount of polling
is required before the IO is completed.

### Management

spdk_bdev_open
spdk_bdev_close


### Information about bdev

spdk_bdev_get_name
spdk_bdev_get_product_name
spdk_bdev_get_block_size
spdk_bdev_get_num_blocks


### Threads and channels

spdk_bdev_get_io_channel


### I/O Operations

spdk_bdev_read
spdk_bdev_read_blocks
spdk_bdev_readv
spdk_bdev_readv_blocks
spdk_bdev_write
spdk_bdev_write_blocks
spdk_bdev_writev
spdk_bdev_writev_blocks
spdk_bdev_write_zeroes
spdk_bdev_write_zeroes_blocks
spdk_bdev_unmap
spdk_bdev_unmap_blocks
spdk_bdev_flush
spdk_bdev_flush_blocks
spdk_bdev_reset

### Other

In order to get bdev working you have first write your application in event model. For more information refer to Event framework {#event}


After your application is initialized with spdk_app_start and proper callback for tasks are set you can use bdev abstraction layer in following ways:
- add exisiting bdev backend
- add your device to spdk instance using call you created in previous part.

spdk_bdev_initialize
spdk_bdev_finish
spdk_bdev_initialize
spdk_bdev_finish

spdk_bdev *spdk_bdev_get_by_name
spdk_bdev *spdk_bdev_first
spdk_bdev *spdk_bdev_next
spdk_bdev *spdk_bdev_first_leaf
spdk_bdev *spdk_bdev_next_leaf

spdk_bdev *spdk_bdev_desc_get_bdev
bool spdk_bdev_io_type_supported
spdk_bdev_dump_config_json

size_t spdk_bdev_get_buf_align
spdk_bdev_get_optimal_io_boundary
bool spdk_bdev_has_write_cache

spdk_bdev_nvme_admin_passthru
spdk_bdev_nvme_io_passthru
spdk_bdev_nvme_io_passthru_md
spdk_bdev_get_io_stat
spdk_bdev_io_get_scsi_status

# Bdev implementation

Optional callbacks. In order to create usable spdk bdev programmer should implement following functions.
 - destruct - Destructor of bdev
 - submit_request - Handles I/O for that bdev
 - io_type_supported - Return types of supported I/O
 - get_io_channel - Return io channel for current thread
 - dump_config_json - Provide information about this bdev to RPC output


Filling bdev structure. In order to register spdk bdev to spdk subsystem we need to fill following fields for spdk_bdev structure:

	.ctxt - Context provided for this device. It will be provided in callbacks.
	.name - Unique name for this block device
    .aliases -
    .product_name - Unique product name for this kind of block device
    .blocklen - Size in bytes of a logical block for this device
    .blockcnt -  Number of blocks
    .write_cache - flag that indicated if write cache is enabled
	need_aligned_buffer
	optimal_io_boundary
    spdk_bdev_module_if *module - Pointer to the bdev module that registered this bdev.
    fn_table - function table
    mutex
    spdk_bdev_status status
    base_bdevs
    base_bdev_link
    vbdevs
    vbdev_link
    claim_module
    unregister_cb
    unregister_ctx
    open_descs
    link
    spdk_bdev_io *reset_in_progress
}

Filling this structure allows you to create bdev in spdk subsystem. You can register such structure with spdk_bdev_register call.
