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

Before any  I/O operations can be done on bdev it has to be opened first.
spdk_bdev_open performs such operation.
To close a previously opened block device spdk_bdev_close has to be called.

### Information about bdev

spdk_bdev_get_name
spdk_bdev_get_product_name
spdk_bdev_get_block_size
spdk_bdev_get_num_blocks


### Threads and channels

Submit a read request to the bdev on the given channel.
spdk_bdev_get_io_channel


### I/O Operations

Bdev supports multiple I/O types, including: read, write, unmap, flush, reset, write_zeroes. You can find out which operations are supported by calling spdk_bdev_io_type_supported.
For each io operation there exists appropriate function, i.e. spdk_bdev_read,  spdk_bdev_write, spdk_bdev_unmap, spdk_bdev_flush, spdk_bdev_reset.

### Other

In order to get bdev working you have first write your application in event model. For more information refer to Event framework {#event}


After your application is initialized with spdk_app_start and proper callback for tasks are set you can use bdev abstraction layer in following ways:
- add exisiting bdev backend
- add your device to spdk instance using call you created in previous part.

# Bdev implementation

Optional callbacks. In order to create usable spdk bdev programmer should implement following functions.
 - destruct - Destructor of bdev
 - submit_request - Handles I/O for that bdev
 - io_type_supported - Return types of supported I/O
 - get_io_channel - Return io channel for current thread
 - dump_config_json - Provide information about this bdev to RPC output


SPDK block device is represented as spdk_bdev structure. In order to register spd_bdev to spdk subsystem those fields are required:

    ctxt - Context provided for this device. It will be provided in callbacks.
    name - Unique name for this block device
    product_name - Unique product name for this kind of block device
    blocklen - Size in bytes of a logical block for this device
    blockcnt -  Number of blocks
    write_cache - flag that indicated if write cache is enabled

Filling this structure allows to create bdev in spdk subsystem. spdk_bdev can be registered with spdk_bdev_register call.
