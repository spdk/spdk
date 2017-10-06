Terminology and naming
==================

Logical volume store
------------------------

A logical volume store is implemented as an SPDK blobstore with a super blob denoting the blobstore (to differentiate from SPDK blobstores used for BlobFS).
For more information about blobstore look at [blobstore](http://www.spdk.io/doc/blob.md)

Shorthand:  lvolstore, lvs
Type name:  struct spdk_lvol_store

An lvolstore will generate a UUID on creation, so that it can be uniquely identified from
other lvolstores.

Logical volume
-----------------

A logical volume is implemented as an SPDK blob created from an lvolstore.

Shorthand: lvol
Type name: struct spdk_lvol

An lvol is uniquely identified by its blob ID and the UUID of the lvolstore from which it was created.

Logical volume block device
---------------------------------

Representation of an SPDK block device (spdk_bdev) with an lvol implementation.
A logical volume block device translates generic SPDK block device I/O (spdk_bdev_io) operations into the equivalent SPDK blob operations.

Shorthand: lvol_bdev
Type name: struct spdk_lvol_bdev

lvol_bdev is dependent on the SPDK bdev framework.
block_size of the created is always 4096, due to blobstore page size.
cluster_size is configurable by parameter. By default its 1GiB.
Size of the new bdev is will be rounded up to nearest multiple of cluster_size.

RPC overview
-----------------

There are few logical volumes specific calls.

- construct_lvol_store bdev_name
    Constructs lvolstore on specified bdev
    bdev is unmapped at initialization
    bdev after construct is claimed
    --cluster-size
- destroy_lvol_store uuid
    Destroy lvolstore on specified bdev. Removes lvol store along with lvols on it
- get_lvol_stores
    Display current logical volume store list
- construct_lvol_bdev uuid size
    Constructs lvol bdev on lvolstore specified by uuid with specified size
- get_bdevs
    User can view created lvols using this call

Note that destroying lvol store requires using call destroy_lvol_store,
while deleting single lvol requires using delete_bdev rpc call.

Restrictions
----------------

Nesting logical volumes on each other is currently not possible
resize lvol bdev, code is present but not used as more work is to be done
