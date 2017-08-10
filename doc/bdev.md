# Block Device Abstraction Layer {#bdev}

# SPDK bdev Getting Started Guide {#bdev_getting_started}

Block storage in SPDK applications is provided by the SPDK bdev layer.  SPDK bdev consists of:

* a driver module API for implementing bdev drivers
* an application API for enumerating and claiming SPDK block devices and performance operations
(read, write, unmap, etc.) on those devices
* bdev drivers for NVMe, malloc (ramdisk), Linux AIO and Ceph RBD
* configuration via SPDK configuration files or JSON RPC

# Configuring block devices {#bdev_config}

SPDK block devices are typically configured via an SPDK configuration file.  These block devices
can then be associated with higher level abstractions such as iSCSI target nodes, NVMe-oF namespaces
or vhost-scsi controllers.  This section will describe how to configure block devices for the
SPDK bdev drivers included with SPDK.

The SPDK configuration file is typically passed to your SPDK-based application via the command line.
Refer to the help facility of your application for more details.

## NVMe

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

## Malloc

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

## Linux AIO

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

## Ceph RBD

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
# Expose bdev Nvme0n1 as kernel block device /dev/nbd0
# Assumes bdev.conf is already configured with a bdev named Nvme0n1 -
# see the NVMe section above.
test/lib/bdev/nbd/nbd -c bdev.conf -b Nvme0n1 -n /dev/nbd0 &
nbd_pid=$!

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
