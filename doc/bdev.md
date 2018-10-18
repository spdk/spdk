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
  "assigned_rate_limits": {
    "rw_ios_per_sec": 10000,
    "rw_mbytes_per_sec": 20
  },
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

## set_bdev_qos_limit {#set_bdev_qos_limit}

Users can use the `set_bdev_qos_limit` RPC command to enable, adjust, and disable
rate limits on an existing bdev.  Two types of rate limits are supported:
IOPS and bandwidth.  The rate limits can be enabled, adjusted, and disabled at any
time for the specified bdev.  The bdev name is a required parameter for this
RPC command and at least one of `rw_ios_per_sec` and `rw_mbytes_per_sec` must be
specified.  When both rate limits are enabled, the first met limit will
take effect.  The value 0 may be specified to disable the corresponding rate
limit. Users can run this command with `-h` or `--help` for more information.

## delete_bdev {#bdev_ug_delete_bdev}

To remove previously created bdev user can use `delete_bdev` RPC command.
Bdev can be deleted at any time and this will be fully handled by any upper
layers. As an argument user should provide bdev name. This RPC command
should be used only for debugging purpose. To remove a particular bdev please
use the delete command specific to its bdev module.

# Ceph RBD {#bdev_config_rbd}

The SPDK RBD bdev driver provides SPDK block layer access to Ceph RADOS block
devices (RBD). Ceph RBD devices are accessed via librbd and librados libraries
to access the RADOS block device exported by Ceph. To create Ceph bdev RPC
command `construct_rbd_bdev` should be used.

Example command

`rpc.py construct_rbd_bdev rbd foo 512`

This command will create a bdev that represents the 'foo' image from a pool called 'rbd'.

To remove a block device representation use the delete_rbd_bdev command.

`rpc.py delete_rbd_bdev Rbd0`

# Crypto Virtual Bdev Module {#bdev_config_crypto}

The crypto virtual bdev module can be configured to provide at rest data encryption
for any underlying bdev. The module relies on the DPDK CryptoDev Framework to provide
all cryptographic functionality. The framework provides support for many different software
only cryptographic modules as well hardware assisted support for the Intel QAT board. The
framework also provides support for cipher, hash, authentication and AEAD functions. At this
time the SPDK virtual bdev module supports cipher only as follows:

- AESN-NI Multi Buffer Crypto Poll Mode Driver: RTE_CRYPTO_CIPHER_AES128_CBC
- Intel(R) QuickAssist (QAT) Crypto Poll Mode Driver: RTE_CRYPTO_CIPHER_AES128_CBC
(Note: QAT is functional however is marked as experimental until the hardware has
been fully integrated with the SPDK CI system.)

In order to support using the bdev block offset (LBA) as the initialization vector (IV),
the crypto module break up all I/O into crypto operations of a size equal to the block
size of the underlying bdev.  For example, a 4K I/O to a bdev with a 512B block size,
would result in 8 cryptographic operations.

For reads, the buffer provided to the crypto module will be used as the destination buffer
for unencrypted data.  For writes, however, a temporary scratch buffer is used as the
destination buffer for encryption which is then passed on to the underlying bdev as the
write buffer.  This is done to avoid encrypting the data in the original source buffer which
may cause problems in some use cases.

Example command

`rpc.py construct_crypto_bdev -b NVMe1n1 -c CryNvmeA -d crypto_aesni_mb -k 0123456789123456`

This command will create a crypto vbdev called 'CryNvmeA' on top of the NVMe bdev
'NVMe1n1' and will use the DPDK software driver 'crypto_aesni_mb' and the key
'0123456789123456'.

To remove the vbdev use the delete_crypto_bdev command.

`rpc.py delete_crypto_bdev CryNvmeA`

# GPT (GUID Partition Table) {#bdev_config_gpt}

The GPT virtual bdev driver is enabled by default and does not require any configuration.
It will automatically detect @ref bdev_ug_gpt on any attached bdev and will create
possibly multiple virtual bdevs.

## SPDK GPT partition table {#bdev_ug_gpt}

The SPDK partition type GUID is `7c5222bd-8f5d-4087-9c00-bf9843c7b58c`. Existing SPDK bdevs
can be exposed as Linux block devices via NBD and then ca be partitioned with
standard partitioning tools. After partitioning, the bdevs will need to be deleted and
attached again for the GPT bdev module to see any changes. NBD kernel module must be
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

# iSCSI bdev {#bdev_config_iscsi}

The SPDK iSCSI bdev driver depends on libiscsi and hence is not enabled by default.
In order to use it, build SPDK with an extra `--with-iscsi-initiator` configure option.

The following command creates an `iSCSI0` bdev from a single LUN exposed at given iSCSI URL
with `iqn.2016-06.io.spdk:init` as the reported initiator IQN.

`rpc.py construct_iscsi_bdev -b iSCSI0 -i iqn.2016-06.io.spdk:init --url iscsi://127.0.0.1/iqn.2016-06.io.spdk:disk1/0`

The URL is in the following format:
`iscsi://[<username>[%<password>]@]<host>[:<port>]/<target-iqn>/<lun>`

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

To delete an aio bdev use the delete_aio_bdev command.

`rpc.py delete_aio_bdev aio0`

# Malloc bdev {#bdev_config_malloc}

Malloc bdevs are ramdisks. Because of its nature they are volatile. They are created from hugepage memory given to SPDK
application.

# Null {#bdev_config_null}

The SPDK null bdev driver is a dummy block I/O target that discards all writes and returns undefined
data for reads.  It is useful for benchmarking the rest of the bdev I/O stack with minimal block
device overhead and for testing configurations that can't easily be created with the Malloc bdev.
To create Null bdev RPC command `construct_null_bdev` should be used.

Example command

`rpc.py construct_null_bdev Null0 8589934592 4096`

This command will create an 8 petabyte `Null0` device with block size 4096.

To delete a null bdev use the delete_null_bdev command.

`rpc.py delete_null_bdev Null0`

# NVMe bdev {#bdev_config_nvme}

There are two ways to create block device based on NVMe device in SPDK. First
way is to connect local PCIe drive and second one is to connect NVMe-oF device.
In both cases user should use `construct_nvme_bdev` RPC command to achieve that.

Example commands

`rpc.py construct_nvme_bdev -b NVMe1 -t PCIe -a 0000:01:00.0`

This command will create NVMe bdev of physical device in the system.

`rpc.py construct_nvme_bdev -b Nvme0 -t RDMA -a 192.168.100.1 -f IPv4 -s 4420 -n nqn.2016-06.io.spdk:cnode1`

This command will create NVMe bdev of NVMe-oF resource.

To remove a NVMe controller use the delete_nvme_controller command.

`rpc.py delete_nvme_controller Nvme0`

This command will remove NVMe controller named Nvme0.

# Logical volumes {#bdev_ug_logical_volumes}

The Logical Volumes library is a flexible storage space management system. It allows
creating and managing virtual block devices with variable size on top of other bdevs.
The SPDK Logical Volume library is built on top of @ref blob. For detailed description
please refer to @ref lvol.

## Logical volume store {#bdev_ug_lvol_store}

Before creating any logical volumes (lvols), an lvol store has to be created first on
selected block device. Lvol store is lvols vessel responsible for managing underlying
bdev space assignment to lvol bdevs and storing metadata. To create lvol store user
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

# Passthru {#bdev_config_passthru}

The SPDK Passthru virtual block device module serves as an example of how to write a
virtual block device module. It implements the required functionality of a vbdev module
and demonstrates some other basic features such as the use of per I/O context.

Example commands

`rpc.py construct_passthru_bdev -b aio -p pt`

`rpc.py delete_passthru_bdev pt`

# Pmem {#bdev_config_pmem}

The SPDK pmem bdev driver uses pmemblk pool as the target for block I/O operations. For
details on Pmem memory please refer to PMDK documentation on http://pmem.io website.
First, user needs to configure SPDK to include PMDK support:

`configure --with-pmdk`

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

To remove a block device representation use the delete_pmem_bdev command.

`rpc.py delete_pmem_bdev pmem`

# Virtio Block {#bdev_config_virtio_blk}

The Virtio-Block driver allows creating SPDK bdevs from Virtio-Block devices.

The following command creates a Virtio-Block device named `VirtioBlk0` from a vhost-user
socket `/tmp/vhost.0` exposed directly by SPDK @ref vhost. Optional `vq-count` and
`vq-size` params specify number of request queues and queue depth to be used.

`rpc.py construct_virtio_dev --dev-type blk --trtype user --traddr /tmp/vhost.0 --vq-count 2 --vq-size 512 VirtioBlk0`

The driver can be also used inside QEMU-based VMs. The following command creates a Virtio
Block device named `VirtioBlk0` from a Virtio PCI device at address `0000:00:01.0`.
The entire configuration will be read automatically from PCI Configuration Space. It will
reflect all parameters passed to QEMU's vhost-user-scsi-pci device.

`rpc.py construct_virtio_dev --dev-type blk --trtype pci --traddr 0000:01:00.0 VirtioBlk1`

Virtio-Block devices can be removed with the following command

`rpc.py remove_virtio_bdev VirtioBlk0`

# Virtio SCSI {#bdev_config_virtio_scsi}

The Virtio-SCSI driver allows creating SPDK block devices from Virtio-SCSI LUNs.

Virtio-SCSI bdevs are constructed the same way as Virtio-Block ones.

`rpc.py construct_virtio_dev --dev-type scsi --trtype user --traddr /tmp/vhost.0 --vq-count 2 --vq-size 512 VirtioScsi0`

`rpc.py construct_virtio_dev --dev-type scsi --trtype pci --traddr 0000:01:00.0 VirtioScsi0`

Each Virtio-SCSI device may export up to 64 block devices named VirtioScsi0t0 ~ VirtioScsi0t63,
one LUN (LUN0) per SCSI device. The above 2 commands will output names of all exposed bdevs.

Virtio-SCSI devices can be removed with the following command

`rpc.py remove_virtio_bdev VirtioScsi0`

Removing a Virtio-SCSI device will destroy all its bdevs.
