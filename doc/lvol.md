# Logical Volumes {#logical_volumes}

The Logical Volumes library is a flexible storage space management system.
It provides creating and managing virtual block devices with variable size.

The SPDK Logical Volume library is built on top of Blobstore and does not
necessarily depend on the SPDK bdev library. There is, however, a bdev
module exposing logical volumes as bdevs and that's what this document will
describe.

For low-level technical details, theory of operation, and library design
considerations, please check out @ref blob.

# Terminology {#lvol_terminology}

## Logical volume store {#lvs}

Just like blobstore, a logical volume store is a persistent, power-fail safe
block allocator. It's built on top of any bdev, occupying it entirely and
storing metadata on its first few blocks. Lvolstore groups underlying blocks
into clusters, which are the minimum units used to create logical volumes.
They're typically 4MiB in size, but can be set to any other value upon
lvolstore creation.

Any registered bdev in SPDK will be searched for the lvolstore header, and if
detected, the lvolstore and its logical volumes will be loaded automatically.

## Logical volume {#lvol}

Logical volumes are visible to the user as SPDK bdevs. They persist across power
failures and reboots, can be dynamically created, resized, removed, cloned, etc.

Logical volumes occupy a number of lvolstore's clusters, all of which will
be exposed as addressable lvol blocks. Note that block size of lvol bdevs is
always equal to the block size of the bdev which lvolstore is built on, i.e.
512B or 4KiB.

## Thin provisioning {#lvol_thin_provisioning}

This provisioned lvols reserve lvolstore clusters only when the first write to
those clusters is performed. This allows for overprovisioning - creating lvols
with total size bigger than the size of the underlying lvolstore.

This provisioning is a per-lvol feature. A single lvolstore may contain both
thin and thick provisioned lvols.

Sample write operations of thin provisioned blob are shown on the diagram below:

![Writing clusters to the thin provisioned blob](lvol_thin_provisioning_write.svg)

Sample read operations and the structure of thin provisioned blob are shown on the diagram below:

![Reading clusters from thin provisioned blob](lvol_thin_provisioning.svg)

## Snapshots and clone {#lvol_snapshots} //TODO

Logical volumes support snapshots and clones functionality. User may at any given time create snapshot of existing logical volume to save a backup of current volume state.
When creating snapshot original volume becomes thin provisioned and saves only incremental differences from its underlying snapshot. This means that every read from unallocated cluster is actually a read from the snapshot and
every write to unallocated cluster triggers new cluster allocation and data copy from corresponding cluster in snapshot to the new cluster in logical volume before the actual write occurs.

The read operation is performed as shown in the diagram below:
![Reading cluster from clone](lvol_clone_snapshot_read.svg)

The write operation is performed as shown in the diagram below:
![Writing cluster to the clone](lvol_clone_snapshot_write.svg)

User may also create clone of existing snapshot that will be thin provisioned and it will behave in the same way as logical volume from which snapshot is created.
There is no limit of clones and snapshots that may be created as long as there is enough space on logical volume store. Snapshots are read only. Clones may be created only from snapshots or read only logical volumes.

## Inflation {#lvol_inflation} //TODO

Blobs can be inflated to copy data from backing devices (e.g. snapshots) and allocate all remaining clusters. As a result of this operation all dependencies for the blob are removed.

![Removing backing blob and bdevs relations using inflate call](lvol_inflate_clone_snapshot.svg)

## Decoupling {#lvol_decoupling} //TODO

Blobs can be decoupled from their parent blob by copying data from backing devices (e.g. snapshots) for all allocated clusters. Remaining unallocated clusters are kept thin provisioned.
Note: When decouple is performed, only single dependency is removed. To remove all dependencies in a chain of blobs depending on each other, multiple calls need to be issued.

# Getting Started {#lvol_getting_started}

Let's assume we've got a single NVMe controller with a single namespace exposed
as bdev named Nvme0n1 with 64GiB size and 512B block size and we would like to
create multiple logical volumes on top of it.

First, we need to create an lvolstore. Let's use 4MiB as a cluster size:

Note that this will erase all data on the underlying bdev by default. We can
specify --clear-method parameter to either "none", "unmap", or "write_zeroes".
"unmap" is the default, which will result in sending TRIM/UNMAP/DISCARD to the
entire disk. "write_zeroes" is self-explanatory, and "none" will not touch the
existing data at all (except for the few first blocks to be used by lvs
metadata).

`rpc.py construct_lvol_store Nvme0n1 MyLvolstore -n 4MiB //TODO --clear-method unmap`

The call returns a 36-byte UUID on success. It's a unique identifier of the lvs
and it will persist across SPDK application restarts or system reboots. The
lvolstore can be also referenced using a friendly name `MyLvolstore`. It persists
across restarts and reboots as well, but unlike UUID, the name can be changed:

`rpc.py rename_lvol_store "MyLvolstore" "MyLvolstore2"`

At any point of time we can check details of the lvolstore:

`rpc.py get_lvol_stores`

or specifically:

`rpc.py get_lvol_stores -l MyLvolstore`
`rpc.py get_lvol_stores -u <UUID>`

Which returns the following structure:

```
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "uuid": "a9959197-b5e2-4f2d-8095-251ffb6985a5",
      "base_bdev": "Nvme0",
      "free_clusters": 31,
      "cluster_size": 4194304,
      "total_data_clusters": 31,
      "block_size": 4096,
      "name": "MyLvolstore"
    }
  ]
}
```

Note that "block_size" is reported as 4096 instead of 512. //TODO

If we shut down the SPDK application at this point at started it again, the
moment the NVMe bdev is registered, our lvolstore would automatically appear
again.

Now, to create logical volumes:

`rpc.py construct_lvol_bdev -u <UUID> MyLvol1 7MiB`

or

`rpc.py construct_lvol_bdev -l MyLvolstore MyLvol1 7MiB`

We could also provide `-t` flag to create the lvol as thin provisioned.

We wanted to create a 7MiB lvol, but since it's not a multiple of the
lvolstore's cluster size (4Mib), it will be rounded up to 8MiB.

The new lvol will appear as a bdev with another randomly-generated UUID
as its name, we can check its parameters with a regular get_bdevs RPC call:

`rpc.py get_bdevs -b <UUID>`

```
//TODO
```

The driver_specific field contains e.g. lvol's human-friendly names and
thin-provisioning status.

Additional aliases can be assigned dynamically:

`rpc.py rename_lvol_bdev <UUID> new_alias`

To resize an lvol:

`rpc.py resize_lvol_bdev <UUID> new_size`

Lvols can be enlarged at any time, but right now to shrink them there must
be no bdev descriptors open.

To remove lvols:

`rpc.py destroy_lvol_bdev <UUID>`

Note that this removes lvols for good, their underlying data won't be
accessible again.

We can remove lvolstore with a similar pattern:

`rpc.py destroy_lvol_store --uuid <UUID>`
`rpc.py destroy_lvol_store --lvs-name MyLvolstore`

//to be continued

RPC regarding lvol and spdk bdev:

```
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
resize_lvol_bdev [-h] name size
    Resize existing lvol bdev
    optional arguments:
    -h, --help  show help
inflate_lvol_bdev [-h] name
    Inflate lvol bdev
    optional arguments:
    -h, --help  show help
decouple_parent_lvol_bdev [-h] name
    Decouple parent of a logical volume
    optional arguments:
    -h, --help  show help
```
