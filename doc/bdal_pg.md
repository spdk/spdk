#SPDK BDAL Programmer's Guide

Block device abstraction layer provides interface for spdk based applications to perform block i/o operations. Note that API is created is created in such way that it provides block operations it means that blocks have constant lenght.


# Bdev implementation

Optional callbacks. In order to create usable spdk bdev programmer should implement following functions.
 - de- Destructor of bdev
 - submit_request - Handles I/O for that bdev
 - io_type_supported - Return types of supported I/O
 - get_io_channel - Return io channel for current thread
 - dump_config_json - Provide information about this bdev to RPC output
 

Filling bdev structure. In order to register spdk bdev to spdk subsystem we need to fill following fields for spdk_bdev structure:

	.ctxt - Context provided for this device. It will be provided in callbacks.
	.name - Unique name for this block device
    .aliases - 
    .product_name - Unique product name for this kind of block device
    .blocklen - Size in bytes of a logical block for this device;
    .blockcnt -  Number of blocks;
    .write_cache - flag that indicated if write cache is enabled
	need_aligned_buffer;
	uint32_t optimal_io_boundary;
    spdk_bdev_module_if *module - Pointer to the bdev module that registered this bdev.
    fn_table - function table
    mutex;
    spdk_bdev_status status;
    base_bdevs;
    base_bdev_link;
    vbdevs;
    vbdev_link;
    claim_module;
    unregister_cb;
    unregister_ctx;
    open_descs;
    link;
    spdk_bdev_io *reset_in_progress;
};

Filling this structure allows you to create bdev in spdk subsystem. You can register such structure with spdk_bdev_register call. 

## Interfaces:
Second part of this document focuses on using bdev in your application.

In order to get bdev working you have first write your application in event model. For more information refer to Event framework {#event}


After your application is initialized with spdk_app_start and proper callback for tasks are set you can use bdev abstraction layer in following ways:
- add exisiting bdev backend 
- add your device to spdk instance using call you created in previous part. 

spdk_bdev_initialize
spdk_bdev_finish
spdk_bdev_initialize;
spdk_bdev_finish;
spdk_bdev_config_text;
spdk_bdev *spdk_bdev_get_by_name;
spdk_bdev *spdk_bdev_first;
spdk_bdev *spdk_bdev_next;
spdk_bdev *spdk_bdev_first_leaf;
spdk_bdev *spdk_bdev_next_leaf;
spdk_bdev_open
spdk_bdev_close;
spdk_bdev *spdk_bdev_desc_get_bdev;
bool spdk_bdev_io_type_supported;
spdk_bdev_dump_config_json;
*spdk_bdev_get_name;
*spdk_bdev_get_product_name;
uint32_t spdk_bdev_get_block_size;
uint64_t spdk_bdev_get_num_blocks;
size_t spdk_bdev_get_buf_align;
uint32_t spdk_bdev_get_optimal_io_boundary;
bool spdk_bdev_has_write_cache;
spdk_io_channel *spdk_bdev_get_io_channel;
spdk_bdev_read
spdk_bdev_read_blocks
spdk_bdev_readv
spdk_bdev_readv_blocks
spdk_bdev_write

