# Block Device User Guide {#bdev}

# Introduction {#bdev_ug_introduction}

The SPDK block device layer, often simply called *bdev*, is a C library
intended to be equivalent to the operating system block storage layer that
often sits immediately above the device drivers in a traditional kernel
storage stack. Specifically, this library provides the following
functionality:

* A pluggable module API for implementing block devices that interface with different types of block storage devices.
* Driver modules for NVMe, malloc (ramdisk), Linux AIO, virtio-scsi, Ceph RBD, Pmem and Vhost-SCSI Initiator and more.
* An application API for enumerating and claiming SPDK block devices and then performing operations (read, write, unmap, etc.) on those devices.
* Facilities to stack block devices to create complex I/O pipelines, including logical volume management (lvol) and partition support (GPT).
* Configuration of block devices via JSON-RPC.
* Request queueing, timeout, and reset handling.
* Multiple, lockless queues for sending I/O to block devices.

Bdev module creates abstraction layer that provides common API for all devices.
User can use available bdev modules or create own module with any type of
device underneath (please refer to @ref bdev_module for details). SPDK
provides also vbdev modules which creates block devices on existing bdev. For
example @ref bdev_ug_logical_volumes or @ref bdev_ug_gpt

# Prerequisites {#bdev_ug_prerequisites}

This guide assumes that you can already build the standard SPDK distribution
on your platform. The block device layer is a C library with a single public
header file named bdev.h. All SPDK configuration described in following
chapters is done by using JSON-RPC commands. SPDK provides a python-based
command line tool for sending RPC commands located at `scripts/rpc.py`. User
can list available commands by running this script with `-h` or `--help` flag.
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
layers. As an argument user should provide bdev name.

# NVMe bdev {#bdev_config_nvme}

There are two ways to create block device based on NVMe device in SPDK. First
way is to connect local PCIe drive and second one is to connect NVMe-oF device.
In both cases user should use `construct_nvme_bdev` RPC command to achieve that.

Example commands

`rpc.py construct_nvme_bdev -b NVMe1 -t PCIe -a 0000:01:00.0`

This command will create NVMe bdev of physical device in the system.

`rpc.py construct_nvme_bdev -b Nvme0 -t RDMA -a 192.168.100.1 -f IPv4 -s 4420 -n nqn.2016-06.io.spdk:cnode1`

This command will create NVMe bdev of NVMe-oF resource.

# Null {#bdev_config_null}

The SPDK null bdev driver is a dummy block I/O target that discards all writes and returns undefined
data for reads.  It is useful for benchmarking the rest of the bdev I/O stack with minimal block
device overhead and for testing configurations that can't easily be created with the Malloc bdev.
To create Null bdev RPC command `construct_null_bdev` should be used.

Example command

`rpc.py construct_null_bdev Null0 8589934592 4096`

This command will create an 8 petabyte `Null0` device with block size 4096.

# Linux AIO bdev {#bdev_config_aio}

The SPDK AIO bdev driver provides SPDK block layer access to Linux kernel block
devices or a file on a Linux filesystem via Linux AIO. Note that O_DIRECT is
used and thus bypasses the Linux page cache. This mode is probably as close to
a typical kernel based target as a user space target can get without using a
user-space driver. To create AIO bdev RPC command `construct_aio_bdev` should be
used.

Example commands

`rpc.py construct_aio_bdev /dev/sda aio0`

This command will create `aio0` device from /dev/sda.

`rpc.py construct_aio_bdev /tmp/file file 8192`

This command will create `file` device with block size 8192 from /tmp/file.

# Ceph RBD {#bdev_config_rbd}

The SPDK RBD bdev driver provides SPDK block layer access to Ceph RADOS block
devices (RBD). Ceph RBD devices are accessed via librbd and librados libraries
to access the RADOS block device exported by Ceph. To create Ceph bdev RPC
command `construct_rbd_bdev` should be used.

Example command

`rpc.py construct_rbd_bdev rbd foo 512`

This command will create a bdev that represents the 'foo' image from a pool called 'rbd'.

# GPT (GUID Partition Table) {#bdev_config_gpt}

The GPT virtual bdev driver is enabled by default and does not require any configuration.
It will automatically detect @ref bdev_ug_gpt_table on any attached bdev and will create
possibly multiple virtual bdevs.

## SPDK GPT partition table {#bdev_ug_gpt_table}
The SPDK partition type GUID is `7c5222bd-8f5d-4087-9c00-bf9843c7b58c`. Existing SPDK bdevs
can be exposed as Linux block devices via NBD and then ca be partitioned with
standard partitioning tools. After partitioning, the bdevs will need to be deleted and
attached again fot the GPT bdev module to see any changes. NBD kernel module must be
loaded first. To create NBD bdev user should use `start_nbd_disk` RPC command.

Example command

`rpc.py start_nbd_disk Malloc0 /dev/nbd0`

This will expose an SPDK bdev `Malloc0` under the `/dev/nbd0` block device.

To remove NBD device user should use `stop_nbd_disk` RPC command.

Example command

`rpc.py stop_nbd_disk /dev/nbd0`

To display full or specified nbd device list user should use `get_nbd_disks` RPC command.

Example command

`rpc.py stop_nbd_disk -n /dev/nbd0`

## Creating a GPT partition table using NBD {#bdev_ug_gpt_create_part}

~~~
# Expose bdev Nvme0n1 as kernel block device /dev/nbd0 by JSON-RPC
rpc.py start_nbd_disk Nvme0n1 /dev/nbd0

# Create GPT partition table.
parted -s /dev/nbd0 mklabel gpt

# Add a partition consuming 50% of the available space.
parted -s /dev/nbd0 mkpart MyPartition '0%' '50%'

# Change the partition type to the SPDK GUID.
# sgdisk is part of the gdisk package.
sgdisk -t 1:7c5222bd-8f5d-4087-9c00-bf9843c7b58c /dev/nbd0

# Stop the NBD device (stop exporting /dev/nbd0).
rpc.py stop_nbd_disk /dev/nbd0

# Now Nvme0n1 is configured with a GPT partition table, and
# the first partition will be automatically exposed as
# Nvme0n1p1 in SPDK applications.
~~~

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

# Pmem {#bdev_config_pmem}

The SPDK pmem bdev driver uses pmemblk pool as the target for block I/O operations. For
details on Pmem memory please refer to PMDK documentation on http://pmem.io website.
First, user needs to compile SPDK with NVML library:

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

# Virtio SCSI {#bdev_config_virtio_scsi}

The @ref virtio allows creating SPDK block devices from Virtio-SCSI LUNs.

The following command creates a Virtio-SCSI device named `VirtioScsi0` from a vhost-user
socket `/tmp/vhost.0` exposed directly by SPDK @ref vhost. Optional `vq-count` and
`vq-size` params specify number of request queues and queue depth to be used.

`rpc.py construct_virtio_user_scsi_bdev /tmp/vhost.0 VirtioScsi0 --vq-count 2 --vq-size 512`

The driver can be also used inside QEMU-based VMs. The following command creates a Virtio
SCSI device named `VirtioScsi0` from a Virtio PCI device at address `0000:00:01.0`.
The entire configuration will be read automatically from PCI Configuration Space. It will
reflect all parameters passed to QEMU's vhost-user-scsi-pci device.

`rpc.py construct_virtio_pci_scsi_bdev 0000:00:01.0 VirtioScsi0`

Each Virtio-SCSI device may export up to 64 block devices named VirtioScsi0t0 ~ VirtioScsi0t63.
The above 2 commands will output names of all exposed bdevs.

Virtio-SCSI devices can be removed with the following command

`rpc.py remove_virtio_scsi_bdev VirtioScsi0`

Removing a Virtio-SCSI device will destroy all its bdevs.
