# Block Device Layer Programming Guide {#bdev_pg}

## Target Audience

This programming guide is intended for developers authoring applications that
use the SPDK bdev library to access block devices.

## Introduction

A block device is a storage device that supports reading and writing data in
fixed-size blocks. These blocks are usually 512 or 4096 bytes. The
devices may be logical constructs in software or correspond to physical
devices like NVMe SSDs.

The block device layer consists of a single generic library in `lib/bdev`,
plus a number of optional modules (as separate libraries) that implement
various types of block devices. The public header file for the generic library
is bdev.h, which is the entirety of the API needed to interact with any type
of block device. This guide will cover how to interact with bdevs using that
API. For a guide to implementing a bdev module, see @ref bdev_module.

The bdev layer provides a number of useful features in addition to providing a
common abstraction for all block devices:

- Automatic queueing of I/O requests in response to queue full or out-of-memory conditions
- Hot remove support, even while I/O traffic is occurring.
- I/O statistics such as bandwidth and latency
- Device reset support and I/O timeout tracking

## Basic Primitives

Users of the bdev API interact with a number of basic objects.

struct spdk_bdev, which this guide will refer to as a *bdev*, represents a
generic block device. struct spdk_bdev_desc, heretofore called a *descriptor*,
represents a handle to a given block device. Descriptors are used to establish
and track permissions to use the underlying block device, much like a file
descriptor on UNIX systems. Requests to the block device are asynchronous and
represented by spdk_bdev_io objects. Requests must be submitted on an
associated I/O channel. The motivation and design of I/O channels is described
in @ref concurrency.

Bdevs can be layered, such that some bdevs service I/O by routing requests to
other bdevs. This can be used to implement caching, RAID, logical volume
management, and more. Bdevs that route I/O to other bdevs are often referred
to as virtual bdevs, or *vbdevs* for short.

## Initializing The Library

The bdev layer depends on the generic message passing infrastructure
abstracted by the header file include/spdk/thread.h. See @ref concurrency for a
full description. Most importantly, calls into the bdev library may only be
made from threads that have been allocated with SPDK by calling
spdk_thread_create().

From an allocated thread, the bdev library may be initialized by calling
spdk_bdev_initialize(), which is an asynchronous operation. Until the completion
callback is called, no other bdev library functions may be invoked. Similarly,
to tear down the bdev library, call spdk_bdev_finish().

## Discovering Block Devices

All block devices have a simple string name. At any time, a pointer to the
device object can be obtained by calling spdk_bdev_get_by_name(), or the entire
set of bdevs may be iterated using spdk_bdev_first() and spdk_bdev_next() and
their variants.

Some block devices may also be given aliases, which are also string names.
Aliases behave like symlinks - they can be used interchangeably with the real
name to look up the block device.

## Preparing To Use A Block Device

In order to send I/O requests to a block device, it must first be opened by
calling spdk_bdev_open_ext(). This will return a descriptor. Multiple users may have
a bdev open at the same time, and coordination of reads and writes between
users must be handled by some higher level mechanism outside of the bdev
layer. Opening a bdev with write permission may fail if a virtual bdev module
has *claimed* the bdev. Virtual bdev modules implement logic like RAID or
logical volume management and forward their I/O to lower level bdevs, so they
mark these lower level bdevs as claimed to prevent outside users from issuing
writes.

When a block device is opened, a callback and context must be provided that
will be called with appropriate spdk_bdev_event_type enum as an argument when
the bdev triggers asynchronous event such as bdev removal. For example,
the callback will be called on each open descriptor for a bdev backed by
a physical NVMe SSD when the NVMe SSD is hot-unplugged. In this case
the callback can be thought of as a request to close the open descriptor so
other memory may be freed. A bdev cannot be torn down while open descriptors
exist, so it is required that a callback is provided.

When a user is done with a descriptor, they may release it by calling
spdk_bdev_close().

Descriptors may be passed to and used from multiple threads simultaneously.
However, for each thread a separate I/O channel must be obtained by calling
spdk_bdev_get_io_channel(). This will allocate the necessary per-thread
resources to submit I/O requests to the bdev without taking locks. To release
a channel, call spdk_put_io_channel(). A descriptor cannot be closed until
all associated channels have been destroyed.

## Sending I/O

Once a descriptor and a channel have been obtained, I/O may be sent by calling
the various I/O submission functions such as spdk_bdev_read(). These calls each
take a callback as an argument which will be called some time later with a
handle to an spdk_bdev_io object. In response to that completion, the user
must call spdk_bdev_free_io() to release the resources. Within this callback,
the user may also use the functions spdk_bdev_io_get_nvme_status() and
spdk_bdev_io_get_scsi_status() to obtain error information in the format of
their choosing.

I/O submission is performed by calling functions such as spdk_bdev_read() or
spdk_bdev_write(). These functions take as an argument a pointer to a region of
memory or a scatter gather list describing memory that will be transferred to
the block device. This memory must be allocated through spdk_dma_malloc() or
its variants. For a full explanation of why the memory must come from a
special allocation pool, see @ref memory. Where possible, data in memory will
be *directly transferred to the block device* using
[Direct Memory Access](https://en.wikipedia.org/wiki/Direct_memory_access).
That means it is not copied.

All I/O submission functions are asynchronous and non-blocking. They will not
block or stall the thread for any reason. However, the I/O submission
functions may fail in one of two ways. First, they may fail immediately and
return an error code. In that case, the provided callback will not be called.
Second, they may fail asynchronously. In that case, the associated
spdk_bdev_io will be passed to the callback and it will report error
information.

Some I/O request types are optional and may not be supported by a given bdev.
To query a bdev for the I/O request types it supports, call
spdk_bdev_io_type_supported().

## Resetting A Block Device

In order to handle unexpected failure conditions, the bdev library provides a
mechanism to perform a device reset by calling spdk_bdev_reset(). This will pass
a message to every other thread for which an I/O channel exists for the bdev,
pause it, then forward a reset request to the underlying bdev module and wait
for completion. Upon completion, the I/O channels will resume and the reset
will complete. The specific behavior inside the bdev module is
module-specific. For example, NVMe devices will delete all queue pairs,
perform an NVMe reset, then recreate the queue pairs and continue. Most
importantly, regardless of device type, *all I/O outstanding to the block
device will be completed prior to the reset completing.*
