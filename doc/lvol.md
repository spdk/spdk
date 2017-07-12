# Logical volume store {#lvolstore}

A logical volume store is an SPDK blobstore with a special super blob denoting the blobstore.
This super blob is to different from SPDK blobstores used for BlobFS.

# Logical volume {#lvol}

A logical volume is an SPDK blob created from an lvolstore.

An lvol is uniquely identified by its blob ID and the UUID of the lvolstore from which it was created.

# Logical volume block device {#vdev_lvol}

Representation of an SPDK block device (spdk_bdev) with an lvol implementation. A logical volume block device translates generic SPDK block device I/O (spdk_bdev_io) operations into the equivalent SPDK blob operations.

# RPC overview
There are few logical volumes specific calls.

- construct_lvol_store bdev_name
	Constructs lvolstore on specified bdev
- destroy_lvol_store bdev_name
	Destroy lvolstore on specified bdev
- construct_lvol_bdev uuid size
	Constructs lvol bdev on lvolstore specified by uuid with specified size
- resize_lvol_bdev bdev_name size
	Resizes specified lvol bdev

Note that destroying lvol store requires using call destroy_lvol_store, while deleting single lvol requires using delete_bdev rpc call.
