# Block Device Layer {#bdev}

# Introduction {#bdev_getting_started}

The SPDK block device layer, often simply called *bdev*, is a C library
intended to be equivalent to the operating system block storage layer that
often sits immediately above the device drivers in a traditional kernel
storage stack. Specifically, this library provides the following
functionality:

* A pluggable module API for implementing block devices that interface with different types of block storage devices.
* Driver modules for NVMe, malloc (ramdisk), Linux AIO, virtio-scsi, Ceph RBD, and more.
* An application API for enumerating and claiming SPDK block devices and then performing operations (read, write, unmap, etc.) on those devices.
* Facilities to stack block devices to create complex I/O pipelines, including logical volume management (lvol) and partition support (GPT).
* Configuration of block devices via JSON-RPC and a configuration file.
* Request queueing, timeout, and reset handling.
* Multiple, lockless queues for sending I/O to block devices.

# Configuring block devices {#bdev_config}

The block device layer is a C library with a single public header file named
bdev.h. Upon initialization, the library will read in a configuration file that
defines the block devices it will expose. The configuration file is a text
format containing sections denominated by square brackets followed by keys with
optional values. It is often passed as a command line argument to the
application. Refer to the help facility of your application for more details.

## NVMe {#bdev_config_nvme}

The SPDK nvme bdev driver provides SPDK block layer access to NVMe SSDs via the SPDK userspace
NVMe driver.  The nvme bdev driver binds only to devices explicitly specified.  These devices
can be either locally attached SSDs or remote NVMe subsystems via NVMe-oF.

~~~
[Nvme]
  # NVMe Device Whitelist
  # Users may specify which NVMe devices to claim by their transport id.
  # See spdk_nvme_transport_id_parse() in spdk/nvme.h for the correct format.
  # The devices will be assigned names in the format <YourName>nY, where YourName is the
  # name specified at the end of the TransportId line and Y is the namespace id, which starts at 1.
  TransportID "trtype:PCIe traddr:0000:00:00.0" Nvme0
  TransportID "trtype:RDMA adrfam:IPv4 subnqn:nqn.2016-06.io.spdk:cnode1 traddr:192.168.100.1 trsvcid:4420" Nvme1
~~~

This exports block devices for all namespaces attached to the two controllers.  Block devices
for namespaces attached to the first controller will be in the format Nvme0nY, where Y is
the namespace ID.  Most NVMe SSDs have a single namespace with ID=1.  Block devices attached to
the second controller will be in the format Nvme1nY.

## Malloc {#bdev_config_malloc}

The SPDK malloc bdev driver allocates a buffer of memory in userspace as the target for block I/O
operations.  This effectively serves as a userspace ramdisk target.

Configuration file syntax:
~~~
[Malloc]
  NumberOfLuns 4
  LunSizeInMB  64
~~~

This exports 4 malloc block devices, named Malloc0 through Malloc3.  Each malloc block device will
be 64MB in size.

## Pmem {#bdev_config_pmem}

The SPDK pmem bdev driver uses pmemblk pool as the the target for block I/O operations.

First, you need to compile SPDK with NVML:
~~~
./configure --with-nvml
~~~
To create pmemblk pool for use with SPDK use pmempool tool included with NVML:
Usage: pmempool create [<args>] <blk|log|obj> [<bsize>] <file>

Example:
~~~
./nvml/src/tools/pmempool/pmempool create -s 32000000 blk 512 /path/to/pmem_pool
~~~

There is also pmem management included in SPDK RPC, it contains three calls:
- create_pmem_pool - Creates pmem pool file
- delete_pmem_pool - Deletes pmem pool file
- pmem_pool_info - Show information if specified file is proper pmem pool file and some detailed information about pool like block size and number of blocks

Example:
~~~
./scripts/rpc.py create_pmem_pool /path/to/pmem_pool
~~~
It is possible to create pmem bdev using SPDK RPC:
~~~
./scripts/rpc.py construct_pmem_bdev -n bdev_name /path/to/pmem_pool
~~~

## Null {#bdev_config_null}

The SPDK null bdev driver is a dummy block I/O target that discards all writes and returns undefined
data for reads.  It is useful for benchmarking the rest of the bdev I/O stack with minimal block
device overhead and for testing configurations that can't easily be created with the Malloc bdev.

Configuration file syntax:
~~~
[Null]
  # Dev <name> <size_in_MiB> <block_size>

  # Create an 8 petabyte null bdev with 4K block size called Null0
  Dev Null0 8589934592 4096
 ~~~

## Linux AIO {#bdev_config_aio}

The SPDK aio bdev driver provides SPDK block layer access to Linux kernel block devices via Linux AIO.
Note that O_DIRECT is used and thus bypasses the Linux page cache. This mode is probably as close to
a typical kernel based target as a user space target can get without using a user-space driver.

Configuration file syntax:

~~~
[AIO]
  # AIO <file name> <bdev name> [<block size>]
  # The file name is the backing device
  # The bdev name can be referenced from elsewhere in the configuration file.
  # Block size may be omitted to automatically detect the block size of a disk.
  AIO /dev/sdb AIO0
  AIO /dev/sdc AIO1
  AIO /tmp/myfile AIO2 4096
~~~

This exports 2 aio block devices, named AIO0 and AIO1.

## Ceph RBD {#bdev_config_rbd}

The SPDK rbd bdev driver provides SPDK block layer access to Ceph RADOS block devices (RBD).  Ceph
RBD devices are accessed via librbd and librados libraries to access the RADOS block device
exported by Ceph.

Configuration file syntax:

~~~
[Ceph]
  # The format of provided rbd info should be: Ceph rbd_pool_name rbd_name size.
  # In the following example, rbd is the name of rbd_pool; foo is the name of
  # rbd device exported by Ceph; value 512 represents the configured block size
  # for this rbd, the block size should be a multiple of 512.
  Ceph rbd foo 512
~~~

This exports 1 rbd block device, named Ceph0.

## Virtio SCSI {#bdev_config_virtio_scsi}

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

~~~
[VirtioUser0]
  # Path to the Unix domain socket using vhost-user protocol.
  Path /tmp/vhost.0
  # Maximum number of request queues to use. Default value is 1.
  Queues 1

#[VirtioUser1]
  #Path /tmp/vhost.1
~~~

Each Virtio-SCSI device may export up to 64 block devices named VirtioScsi0t0 ~ VirtioScsi0t63.

## GPT (GUID Partition Table) {#bdev_config_gpt}

The GPT virtual bdev driver examines all bdevs as they are added and exposes partitions
with a SPDK-specific partition type as bdevs.
The SPDK partition type GUID is `7c5222bd-8f5d-4087-9c00-bf9843c7b58c`.

Configuration file syntax:

~~~
[Gpt]
  # If Gpt is disabled, it will not automatically expose GPT partitions as bdevs.
  Disable No
~~~

### Creating a GPT partition table using NBD

The bdev NBD app can be used to temporarily expose an SPDK bdev through the Linux kernel
block stack so that standard partitioning tools can be used.

~~~
# Assumes bdev.conf is already configured with a bdev named Nvme0n1 -
# see the NVMe section above.
test/app/bdev_svc/bdev_svc -c bdev.conf &
nbd_pid=$!

# Expose bdev Nvme0n1 as kernel block device /dev/nbd0 by JSON-RPC
scripts/rpc.py start_nbd_disk Nvme0n1 /dev/nbd0

# Create GPT partition table.
parted -s /dev/nbd0 mklabel gpt

# Add a partition consuming 50% of the available space.
parted -s /dev/nbd0 mkpart MyPartition '0%' '50%'

# Change the partition type to the SPDK GUID.
# sgdisk is part of the gdisk package.
sgdisk -t 1:7c5222bd-8f5d-4087-9c00-bf9843c7b58c /dev/nbd0

# Kill the NBD application (stop exporting /dev/nbd0).
kill $nbd_pid

# Now Nvme0n1 is configured with a GPT partition table, and
# the first partition will be automatically exposed as
# Nvme0n1p1 in SPDK applications.
~~~

## Logical Volumes

The SPDK lvol driver allows to dynamically partition other SPDK backends.
No static configuration for this driver. Refer to @ref lvol for detailed RPC configuration.
