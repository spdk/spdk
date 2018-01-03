# Bdev Programmer's Guide {#bdal_pg}

# In this document {#bdal_pg_toc}


* @ref bdal_pg_audience
* @ref bdal_basic_primitives
* @ref bdal_flow_of_operations
* @ref bdal_async_model
* @ref bdal_bdev_init
* @ref bdal_bdev_info
* @ref bdal_bdev_desc
* @ref bdal_error_handling
* @ref bdal_bdev_registration

## Target Audience {#bdal_pg_audience}

This programming guide is intended for developers authoring applications that use the SPDK
bdev library or libraries that want to implement block device module.
The first part is intended to supplement the source code to provide an overall understanding of how to
integrate bdev library into an application. The second part is intended explain how to integrate the bdev
to fulfill block device abstraction layer requirements. It is not intended to serve as a design
document or an API reference, but in some cases source code snippets and high level sequences
will be discussed.

For the latest source code reference, refer to the [repo](https://github.com/spdk).

## Basic Primitives  {#bdal_basic_primitives}

*spdk_bdev* or *bdev*
SPDK block device spdk_bdev represents a virtual block device that is exposed by spdk to application.

*spdk_bdev_desc* or *bdev descriptor*
Handle to an SPDK block device opened for I/O operations.

*spdk_io_channel* or *I/O channel*
I/O channel for the block device assigned to specified block device descriptor. Each I/O channel
is bound to specific thread, so it may only be used from that thread only.

*spdk_bdev_io* or *Block device I/O*
I/O context that is passed to an spdk_bdev.

## Flow of operations {#bdal_flow_of_operations}

1. Library initialization
Library have to init all dependent spdk modules, read information about back ends available in system.
Refer to @ref bdal_bdev_init
2. Opening a descriptor
This is analogy to linux API, where we obtain file descriptor during "open".
All I/O operations need to have context on which device they work on.
Refer to @ref bdal_bdev_desc
3. Creating an io_channel
User have to obtain I/O Channel.
Refer to @ref bdal_io_channels
4. Submitting I/O
Refer to @ref bdal_io
5. Handling errors
Refer to @ref bdal_error_handling

TODO: Insert code snippet here

## Asynchronous model {#bdal_async_model}

Bdev library is callback driven. In the event that any bdev API is unable to make forward progress it
will not block but instead return control at that point and make a call to the callback function provided
in the API, along with arguments, when the original call was made. The callback will be made on the same
thread that the call was made from, more on threads later. Some API, however, offer no callback arguments;
in these cases the calls are fully synchronous. Examples of asynchronous calls that utilize callbacks
include those that involve disk IO, for example, where some amount of polling is required before the IO is
completed.

Callback types:

*spdk_bdev_init_cb*
Called for spdk_bdev_initialize call, which initializes bdev modules.

*spdk_bdev_fini_cb*
Given for spdk_bdev_finish call, which finishes bdev modules.

*spdk_bdev_io_completion_cb*
Given for I/O opperations

*spdk_bdev_remove_cb_t*
Hot remove callback

## Bdev layer initialization {#bdal_bdev_init}

Block device abstraction layer provides interface for SPDK based applications to perform block i/o
operations in userspace.

Block device abstraction layer has to be initialized before use. It can be done with
spdk_bdev_initialize call. When modules are initialized spdk provides access to all bdev's specified
trough config file. It is possible to either iterate trough all available block devices with
spdk_bdev_first and spdk_bdev_next calls or get specific bdev by name using spdk_bdev_get_by_name call.

Note: spdk_bdev_first_leaf and spdk_bdev_next_leaf

Above calls return handle to spdk_bdev required for per-bdev specific calls described below.

*spdk_bdev_finish*
Performs cleanup work to remove the registered block device modules.

## Bdev information {#bdal_bdev_info}

Block device is logically divided into number of blocks with equal size.
The spdk_bdev_get_num_blocks call returns number of logical blocks and the
spdk_bdev_get_block_size call returns size of block.
The product of num_blocks and block_size is size of bdev in bytes.

Various other operations provide metadata about specified bdev, that includes:
- block device name (spdk_bdev_get_name)
- block device product name (spdk_bdev_get_product_name)
- Minimum I/O buffer address alignment for a bdev (spdk_bdev_get_buf_align)
- Optimal I/O boundary for a bdev (spdk_bdev_get_optimal_io_boundary)
- Write cache support (spdk_bdev_has_write_cache).
- Driver-specific configuration (spdk_bdev_dump_config_json)

It is also possible to find out which types of I/O operations are supported by device
using spdk_bdev_io_type_supported call.
Following types of I/O can be queried:
General:
 - read (SPDK_BDEV_IO_TYPE_READ)
 - write (SPDK_BDEV_IO_TYPE_WRITE)
 - unmap (SPDK_BDEV_IO_TYPE_UNMAP)
 - flush (SPDK_BDEV_IO_TYPE_FLUSH)
 - reset (SPDK_BDEV_IO_TYPE_RESET)
 - write zeroes (SPDK_BDEV_IO_TYPE_WRITE_ZEROES)
NVMe specific:
 - SPDK_BDEV_IO_TYPE_NVME_ADMIN
 - SPDK_BDEV_IO_TYPE_NVME_IO
 - SPDK_BDEV_IO_TYPE_NVME_IO_MD
Special:
 - SPDK_BDEV_IO_TYPE_INVALID


## Descriptor for I/O Operations {#bdal_bdev_desc}

Before doing any I/O operations bdev descriptor has to be opened like in Linux.

*spdk_bdev_open*
Open a block device for I/O operations. Parameters specify if device is opened for read
or read/write operations. It requires callback function that will be called when device
is hot removed as well as pointer for bdev descriptor, which is required for all
IO-related operations.
TODO: Describe how to handle spdk_bdev_remove_cb_t

*spdk_bdev_desc_get_bdev*
Get the bdev associated with a bdev descriptor.

spdk_bdev_close
 Close a previously opened block device.

## I/O Channels {#bdal_io_channels}

Channels are an SPDK-wide abstraction. Channel is a queue for i/o to spdk_bdev.
Each spdk_bdev can have multiple io channels. The application will perform IO to
the channel and channels are associated 1:1 with a thread.
To obtain an I/O channel for the block device you should call spdk_bdev_get_io_channel.


TODO: *spdk_bdev_get_io_stat*

## I/O Operations {#bdal_io}

SPDK tries to avoid copying of data buffers. Therefore if a buffer is passed from
the bdev layer to the application, the application becomes the owner of that buffer and it
is application responsibility to free that buffer. Following that idea passing
buffer in different direction, from application to bdev layer requires to pass
ownership of that buffer to bdev layer as well. Such buffer cannot be freed before
ownership returns to application layer. Such pass occurs when spdk_bdev_io_completion_cb
callback is called after each I/O operation.

I/O operations differ by multiple factors:

1. Direction of data flow:
- From buffer to block device (write)
- From block device to buffer (read)
- Operation on block device (unmap, write_zeroes, flush, reset)

2. Type of operation:
- read (spdk_bdev_read, spdk_bdev_read_blocks, spdk_bdev_readv, spdk_bdev_readv_blocks)
- write (spdk_bdev_write, spdk_bdev_write_blocks, spdk_bdev_writev, spdk_bdev_writev_blocks)
- unmap (spdk_bdev_unmap, spdk_bdev_unmap_blocks)
- write_zeroes (spdk_bdev_write_zeroes, spdk_bdev_write_zeroes_blocks)
- flush (spdk_bdev_flush, spdk_bdev_flush_blocks)
- reset (spdk_bdev_reset)


3. buffer type:
- continuous buffer (spdk_bdev_read, spdk_bdev_read_blocks, spdk_bdev_write, spdk_bdev_write_blocks)
- i/o vector (spdk_bdev_readv, spdk_bdev_readv_blocks, spdk_bdev_writev, spdk_bdev_writev_blocks)

4. length of data:
- bytes (spdk_bdev_read, spdk_bdev_readv, spdk_bdev_write, spdk_bdev_writev, spdk_bdev_unmap,
            spdk_bdev_write_zeroes, spdk_bdev_flush)
- blocks (spdk_bdev_read_blocks, spdk_bdev_readv_blocks, spdk_bdev_write_blocks, spdk_bdev_writev_blocks,
        spdk_bdev_unmap_blocks, spdk_bdev_write_zeroes_blocks, spdk_bdev_flush_blocks)

## Error handling {#bdal_error_handling}

Some of the bdev layer function return error codes.
TODO: Explain errno == 0, refer to linux error codes.
TODO: Handling I/O errors

*spdk_bdev_free_io*
Free an I/O request.

*spdk_bdev_io_get_iovec*
Get the iovec describing the data buffer of a bdev_io.

NVMe specific
*spdk_bdev_io_get_nvme_status*
Get the status of bdev_io as an NVMe status code.

SCSI specific
*spdk_bdev_io_get_scsi_status*
Get the status of bdev_io as a SCSI status code.


## Bdev registration {#bdal_bdev_registration}

### Module registration

*SPDK_BDEV_MODULE_REGISTER*

### I/O Device registartion

TODO: *spdk_io_device_register*


*spdk_bdev_register*
TODO: how to fill struct spdk_bdev
Optional callbacks. In order to create usable spdk bdev programmer should implement following functions.
 - destruct - Destructor of bdev
 - submit_request - Handles I/O for that bdev
 - io_type_supported - Return types of supported I/O
 - get_io_channel - Return io channel for current thread
 - dump_config_json - Provide information about this bdev to RPC output

registering io_device

SPDK block device is represented as spdk_bdev structure.
In order to register spd_bdev to spdk subsystem those fields are required:

ctxt - Context provided for this device. It will be provided in callbacks.
name - Unique name for this block device
product_name - Unique product name for this kind of block device
blocklen - Size in bytes of a logical block for this device
blockcnt -  Number of blocks
write_cache - flag that indicated if write cache is enabled

Filling this structure allows to create bdev in spdk subsystem. spdk_bdev can be registered with spdk_bdev_register call.

### I/O Handling

*spdk_bdev_io_get_buf*
TODO:
properly signal proper I/O completion statuses

handle I/O errors

BDEV_IO_STATUS_NOMEM

virtual bdevs
