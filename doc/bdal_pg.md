#SPDK BDAL Programmer's Guide {#bdal_pg}

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

Block device abstraction layer provides interface for SPDK based applications to perform block i/o
operations. Note that API is created in such way that it provides block operations it means that
blocks have constant length.

### Basic Primitives

*spdk_bdev* or *bdev*
 SPDK block device spdk_bdev represents a virtual block device that is exported by the back end.

*spdk_bdev_desc* or *bdev descriptor*
Handle to an SPDK block device opened for I/O operations.

*spdk_io_channel* or *io channel*
 I/O channel for the block device assigned to specified block device descriptor. Each I/O channel
 is bound to specific thread, so it may only be used from that thread only.

### Callbacks

Bdev library is callback driven. In the event that any bdev API is unable to make forward progress it
will not block but instead return control at that point and make a call to the callback function provided
in the API, along with arguments, when the original call was made. The callback will be made on the same
thread that the call was made from, more on threads later. Some API, however, offer no callback arguments;
in these cases the calls are fully synchronous. Examples of asynchronous calls that utilize callbacks
include those that involve disk IO, for example, where some amount of polling is required before the IO is
completed.

Callback types:

*spdk_bdev_init_cb*
Given for spdk_bdev_initialize call, which initializes bdev modules.

*spdk_bdev_fini_cb*
Given for spdk_bdev_finish call, which finishes bdev modules.

*spdk_bdev_io_completion_cb*
Given for I/O opperations

*spdk_bdev_remove_cb_t*
Hot remove callback

### Initialization

Block device abstraction layer has to be initialized before use. It can be done with
spdk_bdev_initialize call. When modules are initialized spdk provides access to all bdev's specified
trough config file. It is possible to either iterate trough all available block devices with
spdk_bdev_first and spdk_bdev_next calls or get specific bdev by name using spdk_bdev_get_by_name call.

Note: spdk_bdev_first_leaf and spdk_bdev_next_leaf

Above calls return handle to spdk_bdev required for per-bdev specific calls described below.

### Bdev information

Below calls provides metadata about specified bdev.

*spdk_bdev_get_name*
Get block device name.

*spdk_bdev_get_product_name*
Get block device product name.

*spdk_bdev_get_block_size*
Get block device logical block size.

*spdk_bdev_get_num_blocks*
Get size of block device in logical blocks.

*spdk_bdev_get_buf_align*
Get minimum I/O buffer address alignment for a bdev.

*spdk_bdev_get_optimal_io_boundary*
Get optimal I/O boundary for a bdev.

*spdk_bdev_has_write_cache*
Query whether block device has an enabled write cache.

*spdk_bdev_io_type_supported*
Check whether the block device supports the specified I/O type.
Following types of I/O are supported:
 - SPDK_BDEV_IO_TYPE_INVALID
 - SPDK_BDEV_IO_TYPE_READ
 - SPDK_BDEV_IO_TYPE_WRITE
 - SPDK_BDEV_IO_TYPE_UNMAP
 - SPDK_BDEV_IO_TYPE_FLUSH
 - SPDK_BDEV_IO_TYPE_RESET
 - SPDK_BDEV_IO_TYPE_NVME_ADMIN
 - SPDK_BDEV_IO_TYPE_NVME_IO
 - SPDK_BDEV_IO_TYPE_NVME_IO_MD
 - SPDK_BDEV_IO_TYPE_WRITE_ZEROES

*spdk_bdev_dump_config_json*


### IO Operations

#### Opening device for I/O Operations

*spdk_bdev_open*
Open a block device for I/O operations. Parameters specify if device is opened for read
or read/write operations. It requires callback function that will be called when device
is hot removed as well as pointer for bdev descriptor, which is required for all
IO-related operations.

#### List of I/O Operations

*spdk_bdev_get_io_channel*
Obtain an I/O channel for the block device opened by the specified descriptor.

*spdk_bdev_desc_get_bdev*
Get the bdev associated with a bdev descriptor.




*spdk_bdev_finish*
Performs cleanup work to remove the registered block device modules.


### I/O Operations

Bdev supports multiple I/O types, including: read, write, unmap, flush, reset, write_zeroes. You can find out which operations are supported by calling spdk_bdev_io_type_supported.
For each io operation there exists appropriate function, i.e. spdk_bdev_read,  spdk_bdev_write, spdk_bdev_unmap, spdk_bdev_flush, spdk_bdev_reset.

### Other

In order to get bdev working you have first write your application in event model. For more information refer to Event framework {#event}

After your application is initialized with spdk_app_start and proper callback for tasks are set you can use bdev abstraction layer in following ways:
- add existing bdev backend
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
