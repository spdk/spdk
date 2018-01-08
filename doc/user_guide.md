# Bdev User Guide {#user_guide}

# Introduction {#introduction}

This guide describes how to setup and use block devices supported by the
Storage Performance Development Kit.
Block storage in SPDK applications is provided by the SPDK bdev layer. SPDK
bdev consists of:

* a driver module API for implementing bdev drivers
* an application API for enumerating and claiming SPDK block devices and
performance operations (read, write, unmap, etc.) on those devices
* bdev drivers for NVMe, malloc (ramdisk), Linux AIO, Ceph RBD, GPT, Logical
Volumes, Pmem and Virtion SCSI Initiator
* configuration via JSON RPC

Bdev module creates abstraction layer that provides common API for all devices.
User can use available bdev modules or create own module with any type of
device underneath (please refer to BDAL Programming Guide for details). SPDK
provides also vbdev modules which creates block devices on existing bdev. For
example Logical Volumes or GPT

# Prerequisites {#prerequisites}

This guide assumes that you can already build the standard SPDK distribution
on your platform. All SPDK configuration described in following chapters is
done by using JSON-RPC commands. SPDK provides a python-based command line
tool for sending RPC commands located at `scripts/rpc.py`. User can list
available commands by running this script with `-h` or `--help` flag.
Additionally user can retrieve currently supported set of RPC commands
directly from SPDK application by running `scripts/rpc.py get_rpc_methods`.
Detailed help for each command can be displayed by adding `-h` flag as a
command parameter.

# General Purpose RPCs {#general_rpcs}

## get_bdevs {#get_bdevs}

List of currently available block devices including detailed information about
them can be get by using `get_bdevs` RPC command.

Parameters

The user may specify no parameters in order to list all block devices, or a
block device may be specified by name.

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Optional | string      | Block device name

Example response

~~~
{
  "num_blocks": 32768,
  "supported_io_types": {
    "reset": true,
    "nvme_admin": false,
    "unmap": true,
    "read": true,
    "write_zeroes": true,
    "write": true,
    "flush": true,
    "nvme_io": false
  },
  "driver_specific": {},
  "claimed": false,
  "block_size": 4096,
  "product_name": "Malloc disk",
  "name": "Malloc0"
}
~~~

## delete_bdev {#delete_bdev}

To remove previously created bdev user can use `delete_bdev` RPC command.
Bdev can be deleted at any time and this will be fully handled by any upper
layers (except for iSCSI target).

Parameters

The user have to specify a block device name.

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
bdev_name               | Required | string      | Block device name

# NVMe bdev {#nvme}

There are two ways to create block device based on NVMe device in SPDK. First
way is to connect local PCIe drive and second one is to connect NVMe-oF device.
In both cases user should use `construct_nvme_bdev` RPC command to achieve that.

Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
NAME                    | Required | string      | Name of the bdev
TRTYPE                  | Required | string      | NVMe-oF target trtype: e.g., rdma, pcie
TRADDR                  | Required | string      | NVMe-oF target address: e.g., an ip address or BDF
ADRFAM                  | Optional | string      | NVMe-oF target adrfam: e.g., ipv4, ipv6, ib, fc, intra_host
TRSVCID                 | Optional | string      | NVMe-oF target trsvcid: e.g., a port number
SUBNQN                  | Optional | string      | NVMe-oF target subnqn

Example command

`rpc.py construct_nvme_bdev -b NVMe1 -t pcie -a 0000:01:00.0`

# Linux AIO bdev {#aio}

The SPDK aio bdev driver provides SPDK block layer access to Linux kernel block
devices via Linux AIO. Note that O_DIRECT is used and thus bypasses the Linux
page cache. This mode is probably as close to a typical kernel based target as
a user space target can get without using a user-space driver.To create AIO bdev
RPC command `construct_aio_bdev` should be used.

Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
filename                | Required | string      | Path to device or file (ex: /dev/sda)
name                    | Required | string      | Block device name
block_size              | Required | number      | Block size for this bdev

Example command

`rpc.py construct_nvme_bdev /dev/sda aio0 4096`

# Ceph RBD {#ceph}

The SPDK rbd bdev driver provides SPDK block layer access to Ceph RADOS block
devices (RBD). Ceph RBD devices are accessed via librbd and librados libraries
to access the RADOS block device exported by Ceph. To create Ceph bdev RPC
command `construct_rbd_bdev` should be used.

Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
pool_name               | Required | string      | Rbd pool name
rbd_name                | Required | string      | Rbd image name
block_size              | Required | number      | Rbd block size

Example command

`rpc.py construct_rbd_bdev rbd foo 512`

# GPT (GUID Partition Table) {#gpt}

The GPT virtual bdev driver examines all bdevs as they are added and exposes
partitions with a SPDK-specific partition type as bdevs. The SPDK partition type GUID
is 7c5222bd-8f5d-4087-9c00-bf9843c7b58c.

# Logical volumes {#logical_volumes}

The Logical Volumes library is a flexible storage space management system. It provides
creating and managing virtual block devices with variable size. The SPDK Logical Volume
library is built on top of @ref blob. For detailed description please refer to @ref lvol.

## Logical valume store {#lvol_store}

To create lvols (logical volumes) user should create lvol store first on selected block
device using `construct_lvol_store` RPC command.

Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
bdev_name               | Required | string      | Base bdev name
lvs_name                | Required | string      | Name for lvol store
cluster_sz              | Optional | number      | Size of cluster (in bytes)

Example command

`rpc.py construct_lvol_store Malloc2 lvs -c 4096`

User can get list of available lvol stores using `get_lvol_stores` RPC command (no
parameters available).

Example response

~~~
{
  "uuid": "330a6ab2-f468-11e7-983e-001e67edf35d",
  "base_bdev": "Malloc2",
  "free_clusters": 8190,
  "cluster_size": 8192,
  "total_data_clusters": 8190,
  "block_size": 4096,
  "name": "lvs"
}
~~~

To delete lvol store user should use `destroy_lvol_store` RPC command.

Parameters

Name                    | Optional  | Type        | Description
----------------------- | --------- | ----------- | -----------
UUID                    | Required* | string      | Lvol store UUID
lvs_name                | Required* | string      | Lvol store name
*Only one of required parameters must be provided

Example commands

`rpc.py destroy_lvol_store -u 330a6ab2-f468-11e7-983e-001e67edf35d`

`rpc.py destroy_lvol_store -l lvs`

## Lvols {#lvols}

To create lvols on existing lvol store user should use `construct_lvol_bdev` RPC command.
Each created lvol will be represented by new bdev.

Parameters

Name                    | Optional  | Type        | Description
----------------------- | --------- | ----------- | -----------
lvol_name               | Required  | string      | Name for this lvol
size                    | Required  | number      | Size in MiB for this bdev
UUID                    | Required* | string      | Lvol store UUID
lvs_name                | Required* | string      | Lvol store name
*Only one of required parameters must be provided

Example commands

`rpc.py construct_lvol_bdev lvol1 25 -l lvs`

`rpc.py construct_lvol_bdev lvol2 25 -u 330a6ab2-f468-11e7-983e-001e67edf35d`

# Pmem {#pmem}

The SPDK pmem bdev driver uses pmemblk pool as the the target for block I/O operations.
First, user needs to compile SPDK with NVML library:

`configure --with-nvml`

To create pmemblk pool for use with SPDK user should use `create_pmem_pool` RCP command.

Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
pmem_file               | Required | string      | Path to pmemblk pool file
total_size              | Required | number      | Size of malloc bdev in MB
block_size              | Required | number      | Block size for this pmem pool

Example command

`rpc.py create_pmem_pool /path/to/pmem_pool 25 4096`

To get information on created pmem pool file user can use `pmem_pool_info` RPC command.

Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
pmem_file               | Required | string      | Path to pmemblk pool file

Example command

`rpc.py pmem_pool_info /path/to/pmem_pool`

To remove pmem pool file user can use `delete_pmem_pool` RPC command.

Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
pmem_file               | Required | string      | Path to pmemblk pool file

Example command

`rpc.py delete_pmem_pool /path/to/pmem_pool`

To create bdev based on pmemblk pool file user should use `construct_pmem_bdev ` RPC
command.

Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
pmem_file               | Required | string      | Path to pmemblk pool file
name                    | Required | string      | Block device name

Example command

`rpc.py construct_pmem_bdev /path/to/pmem_pool -n pmem`

# Virtio SCSI {#virtio_scsi}

The SPDK Virtio SCSI driver allows creating SPDK block devices from Virtio SCSI LUNs.

Use the following configuration file snippet to bind all available Virtio-SCSI PCI
devices on a virtual machine. The driver will perform a target scan on each device
and automatically create block device for each LUN.

~~~
[VirtioPci]
  # If enabled, the driver will automatically use all available Virtio-SCSI PCI
  # devices. Disabled by default.
  Enable Yes
~~~

The driver also supports connecting to vhost-user devices exposed on the same host.
In the following case, the host app has created a vhost-scsi controller which is
accessible through the /tmp/vhost.0 domain socket.
