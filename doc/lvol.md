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

A logical volume is implemented as an SPDK blob created from an lvolstore. An lvol is uniquely identified by its UUID. Lvol additional can have alias name.

## Logical volume block device {#lvol_bdev}

* Shorthand: lvol_bdev
* Type name: struct spdk_lvol_bdev

Representation of an SPDK block device (spdk_bdev) with an lvol implementation.
A logical volume block device translates generic SPDK block device I/O (spdk_bdev_io) operations into the equivalent SPDK blob operations. Combination of lvol name and lvolstore name gives lvol_bdev alias name in a form "lvs_name/lvol_name". block_size of the created bdev is always 4096, due to blobstore page size. Cluster_size is configurable by parameter.
Size of the new bdev will be rounded up to nearest multiple of cluster_size.
By default lvol bdevs claim part of lvol store equal to their set size. When thin provision option is enabled, no space is taken from lvol store until data is written to lvol bdev.

## Snapshots and clone {#lvol_snapshots}

Logical volumes support snapshots and clones functionality. User may at any given time create snapshot of existing logical volume to save a backup of current volume state.
When creating snapshot original volume becomes thin provisioned and saves only incremental differences from its underlying snapshot. This means that every read from unallocated cluster is actually a read from the snapshot and
every write to unallocated cluster triggers new cluster allocation and data copy from corresponding cluster in snapshot to the new cluster in logical volume before the actual write occurs.
User may also create clone of existing snapshot that will be thin provisioned and it will behave in the same way as logical volume from which snapshot is created.
There is no limit of clones and snapshots that may be created as long as there is enough space on logical volume store. Snapshots are read only. Clones may be created only from snapshots.

# Configuring Logical Volumes

There is no static configuration available for logical volumes. All configuration is done trough RPC. Information about logical volumes is kept on block devices.

# RPC overview {#lvol_rpc}

RPC regarding lvolstore:

```
construct_lvol_store [-h] [-c CLUSTER_SZ] bdev_name lvs_name
    Constructs lvolstore on specified bdev with specified name. During
    construction bdev is unmapped at initialization and all data is
    erased. Then original bdev is claimed by
    SPDK, but no additional spdk bdevs are created.
    Returns uuid of created lvolstore.
    Optional paramters:
    -h  show help
    -c  CLUSTER_SZ Specifies the size of cluster. By default its 4MiB.
destroy_lvol_store [-h] [-u UUID] [-l LVS_NAME]
    Destroy lvolstore on specified bdev. Removes lvolstore along with lvols on
    it. User can identify lvol store by UUID or its name. Note that destroying
    lvolstore requires using this call, while deleting single lvol requires
    using destroy_lvol_bdev rpc call.
    optional arguments:
    -h, --help  show help
get_lvol_stores [-h] [-u UUID] [-l LVS_NAME]
    Display current logical volume store list
    optional arguments:
    -h, --help  show help
    -u UUID, --uuid UUID  show details of specified lvol store
    -l LVS_NAME, --lvs_name LVS_NAME  show details of specified lvol store
rename_lvol_store [-h] old_name new_name
    Change logical volume store name
    optional arguments:
    -h, --help  show this help message and exit
```

RPC regarding lvol and spdk bdev:

```
construct_lvol_bdev [-h] [-u UUID] [-l LVS_NAME] [-t] lvol_name size
    Creates lvol with specified size and name on lvolstore specified by its uuid
    or name. Then constructs spdk bdev on top of that lvol and presents it as spdk bdev.
    User may use -t switch to create thin provisioned lvol.
    Returns the name of new spdk bdev
    optional arguments:
    -h, --help  show help
get_bdevs [-h] [-b NAME]
    User can view created bdevs using this call including those created on top of lvols.
    optional arguments:
    -h, --help  show help
    -b NAME, --name NAME  Name of the block device. Example: Nvme0n1
destroy_lvol_bdev [-h] bdev_name
    Deletes a logical volume previously created by construct_lvol_bdev.
    optional arguments:
    -h, --help  show help
snapshot_lvol_bdev [-h] lvol_name snapshot_name
    Create a snapshot with snapshot_name of a given lvol bdev.
    optional arguments:
    -h, --help  show help
clone_lvol_bdev [-h] snapshot_name clone_name
    Create a clone with clone_name of a given lvol snapshot.
    optional arguments:
    -h, --help  show help
rename_lvol_bdev [-h] old_name new_name
    Change lvol bdev name
    optional arguments:
    -h, --help  show help
resize_lvol_bdev [-h] name size
    Resize existing lvol bdev
    optional arguments:
    -h, --help  show help
```
