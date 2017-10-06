Terminology and naming
==================

Logical volume store
------------------------

A logical volume store will be implemented as an SPDK blobstore with a super blob denoting
the blobstore (to differentiate from SPDK blobstores used for BlobFS).

Shorthand:  lvolstore
Type name:  struct spdk_lvol_store

An lvolstore will generate a GUID on creation, so that it can be uniquely identified from
other lvolstores.

lvolstore should be implemented without dependencies on the SPDK app and event framework.
This will allow it to be unit tested in isolation.

Logical volume
-----------------

A logical volume will be implemented as an SPDK blob created from an lvolstore.

Shorthand: lvol
Type name: struct spdk_lvol

An lvol will be uniquely identified by its blob ID and the GUID of the lvolstore from
which it was created.

lvol should be implemented without dependencies on the SPDK app and event framework.
This will allow it to be unit tested in isolation.

Logical volume block device
---------------------------------

Representation of an SPDK block device (spdk_bdev) with an lvol implementation.
A logical volume block device translates generic SPDK block device I/O (spdk_bdev_io)
operations into the equivalent SPDK blob operations.

Shorthand: lvol_bdev
Type name: struct spdk_lvol_bdev

lvol_bdev will be dependent on the SPDK bdev framework.

# RPC overview
There are few logical volumes specific calls.

- construct_lvol_store bdev_name
	Constructs lvolstore on specified bdev
- destroy_lvol_store bdev_name
	Destroy lvolstore on specified bdev
- get_lvol_stores
	Display current logical volume store list
- construct_lvol_bdev uuid size
	Constructs lvol bdev on lvolstore specified by uuid with specified size

Note that destroying lvol store requires using call destroy_lvol_store,
while deleting single lvol requires using delete_bdev rpc call.
