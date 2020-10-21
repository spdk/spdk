# Writing a Custom Block Device Module {#bdev_module}

## Target Audience

This programming guide is intended for developers authoring their own block
device modules to integrate with SPDK's bdev layer. For a guide on how to use
the bdev layer, see @ref bdev_pg.

## Introduction

A block device module is SPDK's equivalent of a device driver in a traditional
operating system. The module provides a set of function pointers that are
called to service block device I/O requests. SPDK provides a number of block
device modules including NVMe, RAM-disk, and Ceph RBD. However, some users
will want to write their own to interact with either custom hardware or to an
existing storage software stack. This guide is intended to demonstrate exactly
how to write a module.

## Creating A New Module

Block device modules are located in subdirectories under module/bdev today. It is not
currently possible to place the code for a bdev module elsewhere, but updates
to the build system could be made to enable this in the future. To create a
module, add a new directory with a single C file and a Makefile. A great
starting point is to copy the existing 'null' bdev module.

The primary interface that bdev modules will interact with is in
include/spdk/bdev_module.h. In that header a macro is defined that registers
a new bdev module - SPDK_BDEV_MODULE_REGISTER. This macro take as argument a
pointer spdk_bdev_module structure that is used to register new bdev module.

The spdk_bdev_module structure describes the module properties like
initialization (`module_init`) and teardown (`module_fini`) functions,
the function that returns context size (`get_ctx_size`) - scratch space that
will be allocated in each I/O request for use by this module, and a callback
that will be called each time a new bdev is registered by another module
(`examine_config` and `examine_disk`). Please check the documentation of
struct spdk_bdev_module for more details.

## Creating Bdevs

New bdevs are created within the module by calling spdk_bdev_register(). The
module must allocate a struct spdk_bdev, fill it out appropriately, and pass
it to the register call. The most important field to fill out is `fn_table`,
which points at this data structure:

~~~{.c}
/*
 * Function table for a block device backend.
 *
 * The backend block device function table provides a set of APIs to allow
 * communication with a backend. The main commands are read/write API
 * calls for I/O via submit_request.
 */
struct spdk_bdev_fn_table {
	/* Destroy the backend block device object */
	int (*destruct)(void *ctx);

	/* Process the IO. */
	void (*submit_request)(struct spdk_io_channel *ch, struct spdk_bdev_io *);

	/* Check if the block device supports a specific I/O type. */
	bool (*io_type_supported)(void *ctx, enum spdk_bdev_io_type);

	/* Get an I/O channel for the specific bdev for the calling thread. */
	struct spdk_io_channel *(*get_io_channel)(void *ctx);

	/*
	 * Output driver-specific configuration to a JSON stream. Optional - may be NULL.
	 *
	 * The JSON write context will be initialized with an open object, so the bdev
	 * driver should write a name (based on the driver name) followed by a JSON value
	 * (most likely another nested object).
	 */
	int (*dump_config_json)(void *ctx, struct spdk_json_write_ctx *w);

	/* Get spin-time per I/O channel in microseconds.
	 *  Optional - may be NULL.
	 */
	uint64_t (*get_spin_time)(struct spdk_io_channel *ch);
};
~~~

The bdev module must implement these function callbacks.

The `destruct` function is called to tear down the device when the system no
longer needs it. What `destruct` does is up to the module - it may just be
freeing memory or it may be shutting down a piece of hardware.

The `io_type_supported` function returns whether a particular I/O type is
supported. The available I/O types are:

~~~{.c}
/** bdev I/O type */
enum spdk_bdev_io_type {
	SPDK_BDEV_IO_TYPE_INVALID = 0,
	SPDK_BDEV_IO_TYPE_READ,
	SPDK_BDEV_IO_TYPE_WRITE,
	SPDK_BDEV_IO_TYPE_UNMAP,
	SPDK_BDEV_IO_TYPE_FLUSH,
	SPDK_BDEV_IO_TYPE_RESET,
	SPDK_BDEV_IO_TYPE_NVME_ADMIN,
	SPDK_BDEV_IO_TYPE_NVME_IO,
	SPDK_BDEV_IO_TYPE_NVME_IO_MD,
	SPDK_BDEV_IO_TYPE_WRITE_ZEROES,
};
~~~

For the simplest bdev modules, only `SPDK_BDEV_IO_TYPE_READ` and
`SPDK_BDEV_IO_TYPE_WRITE` are necessary. `SPDK_BDEV_IO_TYPE_UNMAP` is often
referred to as "trim" or "deallocate", and is a request to mark a set of
blocks as no longer containing valid data. `SPDK_BDEV_IO_TYPE_FLUSH` is a
request to make all previously completed writes durable. Many devices do not
require flushes. `SPDK_BDEV_IO_TYPE_WRITE_ZEROES` is just like a regular
write, but does not provide a data buffer (it would have just contained all
0's). If it isn't supported, the generic bdev code is capable of emulating it
by sending regular write requests.

`SPDK_BDEV_IO_TYPE_RESET` is a request to abort all I/O and return the
underlying device to its initial state. Do not complete the reset request
until all I/O has been completed in some way.

`SPDK_BDEV_IO_TYPE_NVME_ADMIN`, `SPDK_BDEV_IO_TYPE_NVME_IO`, and
`SPDK_BDEV_IO_TYPE_NVME_IO_MD` are all mechanisms for passing raw NVMe
commands through the SPDK bdev layer. They're strictly optional, and it
probably only makes sense to implement those if the backing storage device is
capable of handling NVMe commands.

The `get_io_channel` function should return an I/O channel. For a detailed
explanation of I/O channels, see @ref concurrency. The generic bdev layer will
call `get_io_channel` one time per thread, cache the result, and pass that
result to `submit_request`. It will use the corresponding channel for the
thread it calls `submit_request` on.

The `submit_request` function is called to actually submit I/O requests to the
block device. Once the I/O request is completed, the module must call
spdk_bdev_io_complete(). The I/O does not have to finish within the calling
context of `submit_request`.

Integrating a new bdev module into the build system requires updates to various
files in the /mk directory.

## Creating Bdevs in an External Repository

A User can build their own bdev module and application on top of existing SPDK libraries. The example in
test/external_code serves as a template for creating, building and linking an external
bdev module. Refer to test/external_code/README.md and @ref so_linking for further information.

## Creating Virtual Bdevs

Block devices are considered virtual if they handle I/O requests by routing
the I/O to other block devices. The canonical example would be a bdev module
that implements RAID. Virtual bdevs are created in the same way as regular
bdevs, but take one additional step. The module can look up the underlying
bdevs it wishes to route I/O to using spdk_bdev_get_by_name(), where the string
name is provided by the user via an RPC. The module
then may proceed is normal by opening the bdev to obtain a descriptor, and
creating I/O channels for the bdev (probably in response to the
`get_io_channel` callback). The final step is to have the module use its open
descriptor to call spdk_bdev_module_claim_bdev(), indicating that it is
consuming the underlying bdev. This prevents other users from opening
descriptors with write permissions. This effectively 'promotes' the descriptor
to write-exclusive and is an operation only available to bdev modules.
