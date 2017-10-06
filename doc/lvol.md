# Logical Volumes Introduction

A logical volume is a flexible storage space managamanet system. It provides creating and magaging virtual block devices with variable size. SPDK Logical Volume library is built on top of [blobstore](http://www.spdk.io/doc/blob.md).

# Terminology

## Logical volume store {#lvs}

A logical volume store is implemented as an SPDK blobstore. A special super blob denotes the lvolstore to differentiate from SPDK blobstores used for BlobFS.

Shorthand:  lvolstore, lvs
Type name:  struct spdk_lvol_store

An lvolstore will generate a UUID on creation, so that it can be uniquely identified from
other lvolstores.

## Logical volume {#lvol}

A logical volume is implemented as an SPDK blob created from an lvolstore.

Shorthand: lvol
Type name: struct spdk_lvol

An lvol is uniquely identified by its lvol ID and lvolstore UUID from which it was created.

## Logical volume block device {#lvol_bdev}

Representation of an SPDK block device (spdk_bdev) with an lvol implementation.
A logical volume block device translates generic SPDK block device I/O (spdk_bdev_io) operations into the equivalent SPDK blob operations.

Shorthand: lvol_bdev
Type name: struct spdk_lvol_bdev

Combination of lvol ID and lvolstore UUID gives lvol_bdev name in a form "uuid/lvolid". block_size of the created bdev is always 4096, due to blobstore page size. Cluster_size is configurable by parameter. By default its 1GiB.
Size of the new bdev is will be rounded up to nearest multiple of cluster_size.

# RPC overview {#lvol_rpc}

RPC regarding lvolstore:

- `construct_lvol_store [-h] [-c CLUSTER_SZ] base_name`
    Constructs lvolstore on specified bdev. During construction bdev is unmapped at initialization and all data is erased. Then original bdev is claimed by SPDK, but no additional spdk bdevs are created.
    Returns uuid of created lvolstore.  
    Optional paramters:  
    -h show help  
    -c CLUSTER_SZ Specifies the size of cluster. By default its 1GB.  
- `destroy_lvol_store [-h] uuid`
    Destroy lvolstore on specified bdev. Removes lvol store along with lvols on it. Note that destroying lvol store requires using this call, while deleting single lvol requires using delete_bdev rpc call.  
    optional arguments:  
    -h, --help  show help  
- `get_lvol_stores [-h]`
    Display current logical volume store list  
    optional arguments:  
    -h, --help  show help  

RPC regarding lvol and spdk bdev:

- `construct_lvol_bdev [-h] uuid size`
    Creates lvol with specified size on lvolstore specified by its uuid. Then constructs spdk bdev on top of that lvol and presents it as spdk bdev.
    Returns the name of new spdk bdev
- `get_bdevs [-h] [-b NAME]`
    User can view created bdevs using this call including those created on top of lvols.  
    optional arguments:  
    -h, --help  show help  
    -b NAME, --name NAME  Name of the Blockdev. Example: Nvme0n1
- `delete_bdev [-h] bdev_name`
    Deletes spdk bdev  
    optional arguments:  
    -h, --help  show help  

# Restrictions

- Unmap is not supported yet.
- Nesting logical volumes on each other is currently not possible.
- Resizing lvol bdev is experimental. Code is present but not used.
  More work is to be done.
