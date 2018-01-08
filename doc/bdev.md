# Block Device User Guide {#bdev}

# Introduction {#bdev_ug_introduction}

This guide describes how to setup and use block devices supported by the
Storage Performance Development Kit.
Block storage in SPDK applications is provided by the SPDK bdev layer. SPDK
bdev consists of:

* a driver module API for implementing bdev drivers
* an application API for enumerating and claiming SPDK block devices and
performance operations (read, write, unmap, etc.) on those devices
* bdev drivers for NVMe, malloc (ramdisk), Linux AIO, Ceph RBD, GPT, Logical
Volumes, Pmem and Virtion-SCSI Initiator
* configuration via JSON RPC

Bdev module creates abstraction layer that provides common API for all devices.
User can use available bdev modules or create own module with any type of
device underneath (please refer to BDAL Programming Guide for details). SPDK
provides also vbdev modules which creates block devices on existing bdev. For
example @ref bdev_ug_logical_volumes or @ref bdev_ug_gpt

# Prerequisites {#bdev_ug_prerequisites}

This guide assumes that you can already build the standard SPDK distribution
on your platform. All SPDK configuration described in following chapters is
done by using JSON-RPC commands. SPDK provides a python-based command line
tool for sending RPC commands located at `scripts/rpc.py`. User can list
available commands by running this script with `-h` or `--help` flag.
Additionally user can retrieve currently supported set of RPC commands
directly from SPDK application by running `scripts/rpc.py get_rpc_methods`.
Detailed help for each command can be displayed by adding `-h` flag as a
command parameter.

# General Purpose RPCs {#bdev_ug_general_rpcs}

## get_bdevs {#bdev_ug_get_bdevs}

List of currently available block devices including detailed information about
them can be get by using `get_bdevs` RPC command. User can add optional
parameter `name` to get details about specified by that name bdev.

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

## delete_bdev {#bdev_ug_delete_bdev}

To remove previously created bdev user can use `delete_bdev` RPC command.
Bdev can be deleted at any time and this will be fully handled by any upper
layers (except for iSCSI target). As an argument user should provide bdev name.

# NVMe bdev {#bdev_ug_nvme}

There are two ways to create block device based on NVMe device in SPDK. First
way is to connect local PCIe drive and second one is to connect NVMe-oF device.
In both cases user should use `construct_nvme_bdev` RPC command to achieve that.

Example command

`rpc.py construct_nvme_bdev -b NVMe1 -t pcie -a 0000:01:00.0`

# Linux AIO bdev {#bdev_ug_aio}

The SPDK AIO bdev driver provides SPDK block layer access to Linux kernel block
devices or a file on a Linux filesystem via Linux AIO. Note that O_DIRECT is
used and thus bypasses the Linux page cache. This mode is probably as close to
a typical kernel based target as a user space target can get without using a
user-space driver.To create AIO bdev RPC command `construct_aio_bdev` should be
used.

Example commands

`rpc.py construct_aio_bdev /dev/sda aio0 4096`

This command will create `aio0` device with block_size 4096 from /dev/sda.

`rpc.py construct_aio_bdev /tmp/file file 8192`

This command will create `file` device with block size 8192 from /tmp/file.

# Ceph RBD {#bdev_ug_ceph}

The SPDK RBD bdev driver provides SPDK block layer access to Ceph RADOS block
devices (RBD). Ceph RBD devices are accessed via librbd and librados libraries
to access the RADOS block device exported by Ceph. To create Ceph bdev RPC
command `construct_rbd_bdev` should be used.

Example command

`rpc.py construct_rbd_bdev rbd foo 512`

This command will create `rbd` pool with `foo` rbd image and block size 512.

# GPT (GUID Partition Table) {#bdev_ug_gpt}

The GPT virtual bdev driver is enabled by default and does not require any configuration.
It will automatically detect @ref bdev_ug_gpt_table on any attached bdev and will create
possibly multiple virtual bdevs.

## SPDK GPT partition table {#bdev_ug_gpt_table}
The SPDK partition type GUID is 7c5222bd-8f5d-4087-9c00-bf9843c7b58c. Existing SPDK bdevs
can be exposed as Linux block devices via NBD and then ca be partitioned with
standard partitioning tools. After partitioning, the bdevs will need to be deleted and
attached again fot the GPT bdev module to see any changes. NBD kernel module must be
loaded first. To create NBD bdev user should use `start_nbd_disk` RPC command.

Example command

`rpc.py start_nbd_disk Malloc0 /dev/nbd0`

This will expose an SPDK bdev `Malloc0` under the `/dev/nbd0' block device.

To remove NBD device user should use `stop_nbd_disk` RPC command.

Example command

`rpc.py stop_nbd_disk /dev/nbd0`

To display full or specified nbd device list user should use `get_nbd_disks` RPC command.

Example command

`rpc.py stop_nbd_disk -n /dev/nbd0`

# Logical volumes {#bdev_ug_logical_volumes}

The Logical Volumes library is a flexible storage space management system. It allows
creating and managing virtual block devices with variable size on top of other bdevs.
The SPDK Logical Volume library is built on top of @ref blob. For detailed description
please refer to @ref lvol.

## Logical volume store {#bdev_ug_lvol_store}

Before creating any logical volumes (lvols), an lvol store has to be created first on
selected block device. Lvol store is lvols vessel responsible for managing underlying
bdev space assigment to lvol bdevs and storing metadata. To create lvol store user
should use using `construct_lvol_store` RPC command.

Example command

`rpc.py construct_lvol_store Malloc2 lvs -c 4096`

This will create lvol store named `lvs` with cluster size 4096, build on top of
`Malloc2` bdev. In response user will be provided with uuid which is unique lvol store
identifier.

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

Example commands

`rpc.py destroy_lvol_store -u 330a6ab2-f468-11e7-983e-001e67edf35d`

`rpc.py destroy_lvol_store -l lvs`

## Lvols {#bdev_ug_lvols}

To create lvols on existing lvol store user should use `construct_lvol_bdev` RPC command.
Each created lvol will be represented by new bdev.

Example commands

`rpc.py construct_lvol_bdev lvol1 25 -l lvs`

`rpc.py construct_lvol_bdev lvol2 25 -u 330a6ab2-f468-11e7-983e-001e67edf35d`

# Pmem {#bdev_ug_pmem}

The SPDK pmem bdev driver uses pmemblk pool as the target for block I/O operations. For
details on Pmem memory please refer to PMDK documentation. First, user needs to compile
SPDK with NVML library:

`configure --with-nvml`

To create pmemblk pool for use with SPDK user should use `create_pmem_pool` RPC command.

Example command

`rpc.py create_pmem_pool /path/to/pmem_pool 25 4096`

To get information on created pmem pool file user can use `pmem_pool_info` RPC command.

Example command

`rpc.py pmem_pool_info /path/to/pmem_pool`

To remove pmem pool file user can use `delete_pmem_pool` RPC command.

Example command

`rpc.py delete_pmem_pool /path/to/pmem_pool`

To create bdev based on pmemblk pool file user should use `construct_pmem_bdev ` RPC
command.

Example command

`rpc.py construct_pmem_bdev /path/to/pmem_pool -n pmem`

# Virtio SCSI {#bdev_ug_virtio_scsi}

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
