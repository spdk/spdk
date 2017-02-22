# Getting Started Guide {#iscsi_getting_started}

The Intel(R) Storage Performance Development Kit iSCSI target application is named `iscsi_tgt`.
This following section describes how to run iscsi from your cloned package.

# Prerequisites {#iscsi_prereqs}

This guide starts by assuming that you can already build the standard SPDK distribution on your
platform. The SPDK iSCSI target has been known to work on several Linux distributions, namely
Ubuntu 14.04, 15.04, and 15.10, Fedora 21, 22, and 23, and CentOS 7.

Once built, the binary will be in `app/iscsi_tgt`.

# Configuring iSCSI Target {#iscsi_config}

A `iscsi_tgt` specific configuration file is used to configure the iSCSI target. A fully documented
example configuration file is located at `etc/spdk/iscsi.conf.in`.

The configuration file is used to configure the SPDK iSCSI target. This file defines the following:
TCP ports to use as iSCSI portals; general iSCSI parameters; initiator names and addresses to allow
access to iSCSI target nodes; number and types of storage backends to export over iSCSI LUNs; iSCSI
target node mappings between portal groups, initiator groups, and LUNs.

The SPDK iSCSI target supports several different types of storage backends. These backends will create
SCSI LUNs which can be exported via iSCSI target nodes.

The storage backends can be configured in the iscsi.conf configuration file to specify the number or
size of LUNs, block device names (for exporting in-kernel block devices), or other parameters.

Currently there are 3 types of storage backends supported by the iSCSI target:

## Malloc

Configuration file syntax:
~~~
[Malloc]
  NumberOfLuns 4
  LunSizeInMB  64
~~~

Other TargetNode parameters go here (TargetName, Mapping, etc.):
~~~
[TargetNodeX]
  LUN0 Malloc0
~~~

This exports a malloc'd target. The disk is a RAM disk that is a chunk of memory allocated by iscsi in
user space. It will use offload engine to do the copy job instead of memcpy if the system has enough DMA
channels.

## Block Device

AIO devices are accessed using Linux AIO. O_DIRECT is used and thus unaligned writes will be double buffered.
This option also bypasses the Linux page cache. This mode is probably as close to a typical kernel based
target as a user space target can get without using a user-space driver.

Configuration file syntax:

~~~
[AIO]
  #normal file or block device
  AIO /dev/sdb
~~~

Other TargetNode parameters go here (TargetName, Mapping, etc.):
~~~
[TargetNodeX]
  LUN0 AIO0
~~~

## Ceph RBD

Ceph RBD devices are accessed via librbd and librados libraries to access the RADOS block device
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

Other TargetNode parameters go here (TargetName, Mapping, etc.):
~~~
[TargetNodeX]
  LUN0 Ceph0
~~~

## NVMe

The SPDK NVMe driver by default binds to all NVMe controllers which are not bound to the kernel-mode
nvme driver. Users can choose to bind to fewer controllers by setting the NumControllers parameter.
Then the NVMe backend controls NVMe controller(s) directly from userspace and completely bypasses
the kernel to avoid interrupts and context switching.

~~~
[Nvme]
  # NVMe Device Whitelist
  # Users may specify which NVMe devices to claim by their PCI
  # domain, bus, device, and function. The format is dddd:bb:dd.f, which is
  # the same format displayed by lspci or in /sys/bus/pci/devices. The second
  # argument is a "name" for the device that can be anything. The name
  # is referenced later in the Subsystem section.
  #
  # Alternatively, the user can specify ClaimAllDevices. All
  # NVMe devices will be claimed.
  BDF 0000:00:00.0
  BDF 0000:01:00.0

  # The number of attempts per I/O when an I/O fails. Do not include
  # this key to get the default behavior.
  NvmeRetryCount 4
  # The maximum number of NVMe controllers to claim. Do not include this key to
  # claim all of them.
  NumControllers 2

[TargetNodeX]
  # other TargetNode parameters go here (TargetName, Mapping, etc.)
  # nvme with the following format: NvmeXnY, where X = the controller ID
  # and Y = the namespace ID
  # Note: NVMe namespace IDs always start at 1, not 0 - and most
  #  controllers have only 1 namespace.
  LUN0 Nvme0n1
~~~

You should make a copy of the example configuration file, modify it to suit your environment, and
then run the iscsi_tgt application and pass it the configuration file using the -c option. Right now,
the target requires elevated privileges (root) to run.

~~~
app/iscsi_tgt/iscsi_tgt -c /path/to/iscsi.conf
~~~

# Configuring iSCSI Initiator {#iscsi_initiator}

The Linux initiator is open-iscsi.

Installing open-iscsi package
Fedora:
~~~
yum install -y iscsi-initiator-utils
~~~

Ubuntu:
~~~
apt-get install -y open-iscsi
~~~

## Setup

Edit /etc/iscsi/iscsid.conf
~~~
node.session.cmds_max = 4096
node.session.queue_depth = 128
~~~

iscsid must be restarted or receive SIGHUP for changes to take effect. To send SIGHUP, run:
~~~
killall -HUP iscsid
~~~

Recommended changes to /etc/sysctl.conf
~~~
net.ipv4.tcp_timestamps = 1
net.ipv4.tcp_sack = 0

net.ipv4.tcp_rmem = 10000000 10000000 10000000
net.ipv4.tcp_wmem = 10000000 10000000 10000000
net.ipv4.tcp_mem = 10000000 10000000 10000000
net.core.rmem_default = 524287
net.core.wmem_default = 524287
net.core.rmem_max = 524287
net.core.wmem_max = 524287
net.core.optmem_max = 524287
net.core.netdev_max_backlog = 300000
~~~

### Discovery

Assume target is at 192.168.1.5
~~~
iscsiadm -m discovery -t sendtargets -p 192.168.1.5
~~~

### Connect to target

~~~
iscsiadm -m node --login
~~~

At this point the iSCSI target should show up as SCSI disks. Check dmesg to see what
they came up as.

### Disconnect from target

~~~
iscsiadm -m node --logout
~~~

### Deleting target node cache

~~~
iscsiadm -m node -o delete
~~~

This will cause the initiator to forget all previously discovered iSCSI target nodes.

### Finding /dev/sdX nodes for iSCSI LUNs

~~~
iscsiadm -m session -P 3 | grep "Attached scsi disk" | awk '{print $4}'
~~~

This will show the /dev node name for each SCSI LUN in all logged in iSCSI sessions.

### Tuning

After the targets are connected, they can be tuned. For example if /dev/sdc is
an iSCSI disk then the following can be done:
Set noop to scheduler

~~~
echo noop > /sys/block/sdc/queue/scheduler
~~~

Disable merging/coalescing (can be useful for precise workload measurements)

~~~
echo "2" > /sys/block/sdc/queue/nomerges
~~~

Increase requests for block queue

~~~
echo "1024" > /sys/block/sdc/queue/nr_requests
~~~
