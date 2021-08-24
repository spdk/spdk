# Block Device User Guide {#bdev}

## Target Audience {#bdev_ug_targetaudience}

This user guide is intended for software developers who have knowledge of block storage, storage drivers, issuing JSON-RPC
commands and storage services such as RAID, compression, crypto, and others.

## Introduction {#bdev_ug_introduction}

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

## Prerequisites {#bdev_ug_prerequisites}

This guide assumes that you can already build the standard SPDK distribution
on your platform. The block device layer is a C library with a single public
header file named bdev.h. All SPDK configuration described in following
chapters is done by using JSON-RPC commands. SPDK provides a python-based
command line tool for sending RPC commands located at `scripts/rpc.py`. User
can list available commands by running this script with `-h` or `--help` flag.
Additionally user can retrieve currently supported set of RPC commands
directly from SPDK application by running `scripts/rpc.py rpc_get_methods`.
Detailed help for each command can be displayed by adding `-h` flag as a
command parameter.

## Configuring Block Device Modules {#bdev_ug_general_rpcs}

Block devices can be configured using JSON RPCs. A complete list of available RPC commands
with detailed information can be found on the @ref jsonrpc_components_bdev page.

## Common Block Device Configuration Examples

## Ceph RBD {#bdev_config_rbd}

The SPDK RBD bdev driver provides SPDK block layer access to Ceph RADOS block
devices (RBD). Ceph RBD devices are accessed via librbd and librados libraries
to access the RADOS block device exported by Ceph. To create Ceph bdev RPC
command `bdev_rbd_create` should be used.

Example command

`rpc.py bdev_rbd_create rbd foo 512`

This command will create a bdev that represents the 'foo' image from a pool called 'rbd'.

To remove a block device representation use the bdev_rbd_delete command.

`rpc.py bdev_rbd_delete Rbd0`

To resize a bdev use the bdev_rbd_resize command.

`rpc.py bdev_rbd_resize Rbd0 4096`

This command will resize the Rbd0 bdev to 4096 MiB.

## Compression Virtual Bdev Module {#bdev_config_compress}

The compression bdev module can be configured to provide compression/decompression
services for an underlying thinly provisioned logical volume. Although the underlying
module can be anything (i.e. NVME bdev) the overall compression benefits will not be realized
unless the data stored on disk is placed appropriately. The compression vbdev module
relies on an internal SPDK library called `reduce` to accomplish this, see @ref reduce
for detailed information.

The vbdev module relies on the DPDK CompressDev Framework to provide all compression
functionality. The framework provides support for many different software only
compression modules as well as hardware assisted support for Intel QAT. At this
time the vbdev module supports the DPDK drivers for ISAL, QAT and mlx5_pci.

mlx5_pci driver works with BlueField 2 SmartNIC and requires additional configuration of DPDK
environment to enable compression function. It can be done via SPDK event library by configuring
`env_context` member of `spdk_app_opts` structure or by passing corresponding CLI arguments in the
following form: `--allow=BDF,class=compress`, e.g. `--allow=0000:01:00.0,class=compress`.

Persistent memory is used to store metadata associated with the layout of the data on the
backing device. SPDK relies on [PMDK](http://pmem.io/pmdk/) to interface persistent memory so any hardware
supported by PMDK should work. If the directory for PMEM supplied upon vbdev creation does
not point to persistent memory (i.e. a regular filesystem) performance will be severely
impacted.  The vbdev module and reduce libraries were designed to use persistent memory for
any production use.

Example command

`rpc.py bdev_compress_create -p /pmem_files -b myLvol`

In this example, a compression vbdev is created using persistent memory that is mapped to
the directory `pmem_files` on top of the existing thinly provisioned logical volume `myLvol`.
The resulting compression bdev will be named `COMP_LVS/myLvol` where LVS is the name of the
logical volume store that `myLvol` resides on.

The logical volume is referred to as the backing device and once the compression vbdev is
created it cannot be separated from the persistent memory file that will be created in
the specified directory.  If the persistent memory file is not available, the compression
vbdev will also not be available.

By default the vbdev module will choose the QAT driver if the hardware and drivers are
available and loaded.  If not, it will revert to the software-only ISAL driver. By using
the following command, the driver may be specified however this is not persistent so it
must be done either upon creation or before the underlying logical volume is loaded to
be honored. In the example below, `0` is telling the vbdev module to use QAT if available
otherwise use ISAL, this is the default and if sufficient the command is not required. Passing
a value of 1 tells the driver to use QAT and if not available then the creation or loading
the vbdev should fail to create or load.  A value of '2' as shown below tells the module
to use ISAL and if for some reason it is not available, the vbdev should fail to create or load.

`rpc.py compress_set_pmd -p 2`

To remove a compression vbdev, use the following command which will also delete the PMEM
file.  If the logical volume is deleted the PMEM file will not be removed and the
compression vbdev will not be available.

`rpc.py bdev_compress_delete COMP_LVS/myLvol`

To list compression volumes that are only available for deletion because their PMEM file
was missing use the following. The name parameter is optional and if not included will list
all volumes, if used it will return the name or an error that the device does not exist.

`rpc.py bdev_compress_get_orphans --name COMP_Nvme0n1`

## Crypto Virtual Bdev Module {#bdev_config_crypto}

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

`rpc.py bdev_crypto_create NVMe1n1 CryNvmeA crypto_aesni_mb 0123456789123456`

This command will create a crypto vbdev called 'CryNvmeA' on top of the NVMe bdev
'NVMe1n1' and will use the DPDK software driver 'crypto_aesni_mb' and the key
'0123456789123456'.

To remove the vbdev use the bdev_crypto_delete command.

`rpc.py bdev_crypto_delete CryNvmeA`

## Delay Bdev Module {#bdev_config_delay}

The delay vbdev module is intended to apply a predetermined additional latency on top of a lower
level bdev. This enables the simulation of the latency characteristics of a device during the functional
or scalability testing of an SPDK application. For example, to simulate the effect of drive latency when
processing I/Os, one could configure a NULL bdev with a delay bdev on top of it.

The delay bdev module is not intended to provide a high fidelity replication of a specific NVMe drive's latency,
instead it's main purpose is to provide a "big picture" understanding of how a generic latency affects a given
application.

A delay bdev is created using the `bdev_delay_create` RPC. This rpc takes 6 arguments, one for the name
of the delay bdev and one for the name of the base bdev. The remaining four arguments represent the following
latency values: average read latency, average write latency, p99 read latency, and p99 write latency.
Within the context of the delay bdev p99 latency means that one percent of the I/O will be delayed by at
least by the value of the p99 latency before being completed to the upper level protocol. All of the latency values
are measured in microseconds.

Example command:

`rpc.py bdev_delay_create -b Null0 -d delay0 -r 10 --nine-nine-read-latency 50 -w 30 --nine-nine-write-latency 90`

This command will create a delay bdev with average read and write latencies of 10 and 30 microseconds and p99 read
and write latencies of 50 and 90 microseconds respectively.

A delay bdev can be deleted using the `bdev_delay_delete` RPC

Example command:

`rpc.py bdev_delay_delete delay0`

## GPT (GUID Partition Table) {#bdev_config_gpt}

The GPT virtual bdev driver is enabled by default and does not require any configuration.
It will automatically detect @ref bdev_ug_gpt on any attached bdev and will create
possibly multiple virtual bdevs.

### SPDK GPT partition table {#bdev_ug_gpt}

The SPDK partition type GUID is `7c5222bd-8f5d-4087-9c00-bf9843c7b58c`. Existing SPDK bdevs
can be exposed as Linux block devices via NBD and then can be partitioned with
standard partitioning tools. After partitioning, the bdevs will need to be deleted and
attached again for the GPT bdev module to see any changes. NBD kernel module must be
loaded first. To create NBD bdev user should use `nbd_start_disk` RPC command.

Example command

`rpc.py nbd_start_disk Malloc0 /dev/nbd0`

This will expose an SPDK bdev `Malloc0` under the `/dev/nbd0` block device.

To remove NBD device user should use `nbd_stop_disk` RPC command.

Example command

`rpc.py nbd_stop_disk /dev/nbd0`

To display full or specified nbd device list user should use `nbd_get_disks` RPC command.

Example command

`rpc.py nbd_stop_disk -n /dev/nbd0`

### Creating a GPT partition table using NBD {#bdev_ug_gpt_create_part}

~~~bash
# Expose bdev Nvme0n1 as kernel block device /dev/nbd0 by JSON-RPC
rpc.py nbd_start_disk Nvme0n1 /dev/nbd0

# Create GPT partition table.
parted -s /dev/nbd0 mklabel gpt

# Add a partition consuming 50% of the available space.
parted -s /dev/nbd0 mkpart MyPartition '0%' '50%'

# Change the partition type to the SPDK GUID.
# sgdisk is part of the gdisk package.
sgdisk -t 1:7c5222bd-8f5d-4087-9c00-bf9843c7b58c /dev/nbd0

# Stop the NBD device (stop exporting /dev/nbd0).
rpc.py nbd_stop_disk /dev/nbd0

# Now Nvme0n1 is configured with a GPT partition table, and
# the first partition will be automatically exposed as
# Nvme0n1p1 in SPDK applications.
~~~

## iSCSI bdev {#bdev_config_iscsi}

The SPDK iSCSI bdev driver depends on libiscsi and hence is not enabled by default.
In order to use it, build SPDK with an extra `--with-iscsi-initiator` configure option.

The following command creates an `iSCSI0` bdev from a single LUN exposed at given iSCSI URL
with `iqn.2016-06.io.spdk:init` as the reported initiator IQN.

`rpc.py bdev_iscsi_create -b iSCSI0 -i iqn.2016-06.io.spdk:init --url iscsi://127.0.0.1/iqn.2016-06.io.spdk:disk1/0`

The URL is in the following format:
`iscsi://[<username>[%<password>]@]<host>[:<port>]/<target-iqn>/<lun>`

## Linux AIO bdev {#bdev_config_aio}

The SPDK AIO bdev driver provides SPDK block layer access to Linux kernel block
devices or a file on a Linux filesystem via Linux AIO. Note that O_DIRECT is
used and thus bypasses the Linux page cache. This mode is probably as close to
a typical kernel based target as a user space target can get without using a
user-space driver. To create AIO bdev RPC command `bdev_aio_create` should be
used.

Example commands

`rpc.py bdev_aio_create /dev/sda aio0`

This command will create `aio0` device from /dev/sda.

`rpc.py bdev_aio_create /tmp/file file 4096`

This command will create `file` device with block size 4096 from /tmp/file.

To delete an aio bdev use the bdev_aio_delete command.

`rpc.py bdev_aio_delete aio0`

## OCF Virtual bdev {#bdev_config_cas}

OCF virtual bdev module is based on [Open CAS Framework](https://github.com/Open-CAS/ocf) - a
high performance block storage caching meta-library.
To enable the module, configure SPDK using `--with-ocf` flag.
OCF bdev can be used to enable caching for any underlying bdev.

Below is an example command for creating OCF bdev:

`rpc.py bdev_ocf_create Cache1 wt Malloc0 Nvme0n1`

This command will create new OCF bdev `Cache1` having bdev `Malloc0` as caching-device
and `Nvme0n1` as core-device and initial cache mode `Write-Through`.
`Malloc0` will be used as cache for `Nvme0n1`, so  data written to `Cache1` will be present
on `Nvme0n1` eventually.
By default, OCF will be configured with cache line size equal 4KiB
and non-volatile metadata will be disabled.

To remove `Cache1`:

`rpc.py bdev_ocf_delete Cache1`

During removal OCF-cache will be stopped and all cached data will be written to the core device.

Note that OCF has a per-device RAM requirement. More details can be found in the
[OCF documentation](https://open-cas.github.io/guide_system_requirements.html).

## Malloc bdev {#bdev_config_malloc}

Malloc bdevs are ramdisks. Because of its nature they are volatile. They are created from hugepage memory given to SPDK
application.

Example command for creating malloc bdev:

`rpc.py bdev_malloc_create -b Malloc0 64 512`

Example command for removing malloc bdev:

`rpc.py bdev_malloc_delete Malloc0`

## Null {#bdev_config_null}

The SPDK null bdev driver is a dummy block I/O target that discards all writes and returns undefined
data for reads.  It is useful for benchmarking the rest of the bdev I/O stack with minimal block
device overhead and for testing configurations that can't easily be created with the Malloc bdev.
To create Null bdev RPC command `bdev_null_create` should be used.

Example command

`rpc.py bdev_null_create Null0 8589934592 4096`

This command will create an 8 petabyte `Null0` device with block size 4096.

To delete a null bdev use the bdev_null_delete command.

`rpc.py bdev_null_delete Null0`

## NVMe bdev {#bdev_config_nvme}

There are two ways to create block device based on NVMe device in SPDK. First
way is to connect local PCIe drive and second one is to connect NVMe-oF device.
In both cases user should use `bdev_nvme_attach_controller` RPC command to achieve that.

Example commands

`rpc.py bdev_nvme_attach_controller -b NVMe1 -t PCIe -a 0000:01:00.0`

This command will create NVMe bdev of physical device in the system.

`rpc.py bdev_nvme_attach_controller -b Nvme0 -t RDMA -a 192.168.100.1 -f IPv4 -s 4420 -n nqn.2016-06.io.spdk:cnode1`

This command will create NVMe bdev of NVMe-oF resource.

To remove an NVMe controller use the bdev_nvme_detach_controller command.

`rpc.py bdev_nvme_detach_controller Nvme0`

This command will remove NVMe bdev named Nvme0.

### NVMe bdev character device {#bdev_config_nvme_cuse}

This feature is considered as experimental. You must configure with --with-nvme-cuse
option to enable this RPC.

Example commands

`rpc.py bdev_nvme_cuse_register -n Nvme3

This command will register a character device under /dev/spdk associated with Nvme3
controller. If there are namespaces created on Nvme3 controller, a namespace
character device is also created for each namespace.

For example, the first controller registered will have a character device path of
/dev/spdk/nvmeX, where X is replaced with a unique integer to differentiate it from
other controllers.  Note that this 'nvmeX' name here has no correlation to the name
associated with the controller in SPDK.  Namespace character devices will have a path
of /dev/spdk/nvmeXnY, where Y is the namespace ID.

Cuse devices are removed from system, when NVMe controller is detached or unregistered
with command:

`rpc.py bdev_nvme_cuse_unregister -n Nvme0`

## Logical volumes {#bdev_ug_logical_volumes}

The Logical Volumes library is a flexible storage space management system. It allows
creating and managing virtual block devices with variable size on top of other bdevs.
The SPDK Logical Volume library is built on top of @ref blob. For detailed description
please refer to @ref lvol.

### Logical volume store {#bdev_ug_lvol_store}

Before creating any logical volumes (lvols), an lvol store has to be created first on
selected block device. Lvol store is lvols vessel responsible for managing underlying
bdev space assignment to lvol bdevs and storing metadata. To create lvol store user
should use using `bdev_lvol_create_lvstore` RPC command.

Example command

`rpc.py bdev_lvol_create_lvstore Malloc2 lvs -c 4096`

This will create lvol store named `lvs` with cluster size 4096, build on top of
`Malloc2` bdev. In response user will be provided with uuid which is unique lvol store
identifier.

User can get list of available lvol stores using `bdev_lvol_get_lvstores` RPC command (no
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

To delete lvol store user should use `bdev_lvol_delete_lvstore` RPC command.

Example commands

`rpc.py bdev_lvol_delete_lvstore -u 330a6ab2-f468-11e7-983e-001e67edf35d`

`rpc.py bdev_lvol_delete_lvstore -l lvs`

### Lvols {#bdev_ug_lvols}

To create lvols on existing lvol store user should use `bdev_lvol_create` RPC command.
Each created lvol will be represented by new bdev.

Example commands

`rpc.py bdev_lvol_create lvol1 25 -l lvs`

`rpc.py bdev_lvol_create lvol2 25 -u 330a6ab2-f468-11e7-983e-001e67edf35d`

## Passthru {#bdev_config_passthru}

The SPDK Passthru virtual block device module serves as an example of how to write a
virtual block device module. It implements the required functionality of a vbdev module
and demonstrates some other basic features such as the use of per I/O context.

Example commands

`rpc.py bdev_passthru_create -b aio -p pt`

`rpc.py bdev_passthru_delete pt`

## Pmem {#bdev_config_pmem}

The SPDK pmem bdev driver uses pmemblk pool as the target for block I/O operations. For
details on Pmem memory please refer to PMDK documentation on http://pmem.io website.
First, user needs to configure SPDK to include PMDK support:

`configure --with-pmdk`

To create pmemblk pool for use with SPDK user should use `bdev_pmem_create_pool` RPC command.

Example command

`rpc.py bdev_pmem_create_pool /path/to/pmem_pool 25 4096`

To get information on created pmem pool file user can use `bdev_pmem_get_pool_info` RPC command.

Example command

`rpc.py bdev_pmem_get_pool_info /path/to/pmem_pool`

To remove pmem pool file user can use `bdev_pmem_delete_pool` RPC command.

Example command

`rpc.py bdev_pmem_delete_pool /path/to/pmem_pool`

To create bdev based on pmemblk pool file user should use `bdev_pmem_create` RPC
command.

Example command

`rpc.py bdev_pmem_create /path/to/pmem_pool -n pmem`

To remove a block device representation use the bdev_pmem_delete command.

`rpc.py bdev_pmem_delete pmem`

## RAID {#bdev_ug_raid}

RAID virtual bdev module provides functionality to combine any SPDK bdevs into
one RAID bdev. Currently SPDK supports only RAID 0. RAID functionality does not
store on-disk metadata on the member disks, so user must recreate the RAID
volume when restarting application. User may specify member disks to create RAID
volume event if they do not exists yet - as the member disks are registered at
a later time, the RAID module will claim them and will surface the RAID volume
after all of the member disks are available. It is allowed to use disks of
different sizes - the smallest disk size will be the amount of space used on
each member disk.

Example commands

`rpc.py bdev_raid_create -n Raid0 -z 64 -r 0 -b "lvol0 lvol1 lvol2 lvol3"`

`rpc.py bdev_raid_get_bdevs`

`rpc.py bdev_raid_delete Raid0`

## Split {#bdev_ug_split}

The split block device module takes an underlying block device and splits it into
several smaller equal-sized virtual block devices. This serves as an example to create
more vbdevs on a given base bdev for user testing.

Example commands

To create four split bdevs with base bdev_b0 use the `bdev_split_create` command.
Each split bdev will be one fourth the size of the base bdev.

`rpc.py bdev_split_create bdev_b0 4`

The `split_size_mb`(-s) parameter restricts the size of each split bdev.
The total size of all split bdevs must not exceed the base bdev size.

`rpc.py bdev_split_create bdev_b0 4 -s 128`

To remove the split bdevs, use the `bdev_split_delete` command with the base bdev name.

`rpc.py bdev_split_delete bdev_b0`

## Uring {#bdev_ug_uring}

The uring bdev module issues I/O to kernel block devices using the io_uring Linux kernel API. This module requires liburing.
For more information on io_uring refer to kernel [IO_uring] (https://kernel.dk/io_uring.pdf)

The user needs to configure SPDK to include io_uring support:

`configure --with-uring`

To create a uring bdev with given filename, bdev name and block size use the `bdev_uring_create` RPC.

`rpc.py  bdev_uring_create /path/to/device bdev_u0 512`

To remove a uring bdev use the `bdev_uring_delete` RPC.

`rpc.py bdev_uring_delete bdev_u0`

## Virtio Block {#bdev_config_virtio_blk}

The Virtio-Block driver allows creating SPDK bdevs from Virtio-Block devices.

The following command creates a Virtio-Block device named `VirtioBlk0` from a vhost-user
socket `/tmp/vhost.0` exposed directly by SPDK @ref vhost. Optional `vq-count` and
`vq-size` params specify number of request queues and queue depth to be used.

`rpc.py bdev_virtio_attach_controller --dev-type blk --trtype user --traddr /tmp/vhost.0 --vq-count 2 --vq-size 512 VirtioBlk0`

The driver can be also used inside QEMU-based VMs. The following command creates a Virtio
Block device named `VirtioBlk0` from a Virtio PCI device at address `0000:00:01.0`.
The entire configuration will be read automatically from PCI Configuration Space. It will
reflect all parameters passed to QEMU's vhost-user-scsi-pci device.

`rpc.py bdev_virtio_attach_controller --dev-type blk --trtype pci --traddr 0000:01:00.0 VirtioBlk1`

Virtio-Block devices can be removed with the following command

`rpc.py bdev_virtio_detach_controller VirtioBlk0`

## Virtio SCSI {#bdev_config_virtio_scsi}

The Virtio-SCSI driver allows creating SPDK block devices from Virtio-SCSI LUNs.

Virtio-SCSI bdevs are created the same way as Virtio-Block ones.

`rpc.py bdev_virtio_attach_controller --dev-type scsi --trtype user --traddr /tmp/vhost.0 --vq-count 2 --vq-size 512 VirtioScsi0`

`rpc.py bdev_virtio_attach_controller --dev-type scsi --trtype pci --traddr 0000:01:00.0 VirtioScsi0`

Each Virtio-SCSI device may export up to 64 block devices named VirtioScsi0t0 ~ VirtioScsi0t63,
one LUN (LUN0) per SCSI device. The above 2 commands will output names of all exposed bdevs.

Virtio-SCSI devices can be removed with the following command

`rpc.py bdev_virtio_detach_controller VirtioScsi0`

Removing a Virtio-SCSI device will destroy all its bdevs.
