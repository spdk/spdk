# Logical Volumes Introduction {#logical_volumes}

The Logical Volumes library is a flexible storage space management system. It provides creating and managing virtual block devices with variable size. The SPDK Logical Volume library is built on top of @ref blob.

# Terminology {#lvol_terminology}

## Logical volume store {#lvs}

* Shorthand:  lvolstore, lvs
* Type name:  struct spdk_lvol_store

A logical volume store uses the super blob feature of blobstore to hold uuid (and in future other metadata). Blobstore types are implemented in blobstore itself, and saved on disk. An lvolstore will generate a UUID on creation, so that it can be uniquely identified from other lvolstores.

## Logical volume {#lvol}

* Shorthand: lvol
* Type name: struct spdk_lvol

A logical volume is implemented as an SPDK blob created from an lvolstore. An lvol is uniquely identified by its lvol ID and lvolstore UUID from which it was created.

## Logical volume block device {#lvol_bdev}

* Shorthand: lvol_bdev
* Type name: struct spdk_lvol_bdev

Representation of an SPDK block device (spdk_bdev) with an lvol implementation.
A logical volume block device translates generic SPDK block device I/O (spdk_bdev_io) operations into the equivalent SPDK blob operations. Combination of lvol ID and lvolstore UUID gives lvol_bdev name in a form "uuid/lvolid". block_size of the created bdev is always 4096, due to blobstore page size. Cluster_size is configurable by parameter.
Size of the new bdev will be rounded up to nearest multiple of cluster_size.
By default lvol bdevs claim part of lvol store equal to their set size. When thin provision option is enabled, no space is taken from lvol store until data is written to lvol bdev.

# Configuring Logical Volumes

There is no static configuration available for logical volumes. All configuration is done trough RPC. Information about logical volumes is kept on block devices.

# RPC overview {#lvol_rpc}

RPC regarding lvolstore:

```
construct_lvol_store [-h] [-c CLUSTER_SZ] base_name
    Constructs lvolstore on specified bdev. During construction bdev is unmapped
    at initialization and all data is erased. Then original bdev is claimed by
    SPDK, but no additional spdk bdevs are created.
    Returns uuid of created lvolstore.
    Optional paramters:
    -h show help
    -c CLUSTER_SZ Specifies the size of cluster. By default its 4MiB.
destroy_lvol_store [-h] uuid
    Destroy lvolstore on specified bdev. Removes lvolstore along with lvols on
    it. Note that destroying lvolstore requires using this call, while deleting
    single lvol requires using delete_bdev rpc call.
    optional arguments:
    -h, --help  show help
get_lvol_stores [-h] [NAME]
    Display current logical volume store list
    optional arguments:
    -h, --help  show help
    NAME, show details of specified lvol store
```

RPC regarding lvol and spdk bdev:

```
construct_lvol_bdev [-h] [-t] uuid size
    Creates lvol with specified size on lvolstore specified by its uuid.
    Then constructs spdk bdev on top of that lvol and presents it as spdk bdev.
    Returns the name of new spdk bdev
get_bdevs [-h] [-b NAME]
    User can view created bdevs using this call including those created on top of lvols.
    optional arguments:
    -h, --help  show help
    -b NAME, --name NAME  Name of the block device. Example: Nvme0n1
delete_bdev [-h] bdev_name
    Deletes spdk bdev
    optional arguments:
    -h, --help  show help
```

# Restrictions

- Nesting logical volumes on each other is not supported.
- Resizing lvol bdev is experimental. Code is present but not used.
