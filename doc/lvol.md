# Logical volume store {#lvolstore}

A logical volume store is an SPDK blobstore with a special super blob denoting the blobstore.
This super blob is to different from SPDK blobstores used for BlobFS.

# Logical volume {#lvol}

A logical volume is an SPDK blob created from an lvolstore.

An lvol is uniquely identified by its blob ID and the UUID of the lvolstore from which it was created.

# Logical volume block device {#vdev_lvol}

Representation of an SPDK block device (spdk_bdev) with an lvol implementation. A logical volume block device translates generic SPDK block device I/O (spdk_bdev_io) operations into the equivalent SPDK blob operations.

# RPC overview

lvolstore_create(bdev) - create lvolstore on bdev

lvolstore_destroy(bdev) - destroy lvolstore on bdev

lvol_add(lvolstore, size) - create lvol

lvol_resize(lvol, size) - resize lvol

lvol_remove(lvol) - remove lvol
