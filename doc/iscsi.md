# iSCSI Target {#iscsi}

# Getting Started Guide {#iscsi_getting_started}

The Intel(R) Storage Performance Development Kit iSCSI target application is named `iscsi_tgt`.
This following section describes how to run iscsi from your cloned package.

## Prerequisites {#iscsi_prereqs}

This guide starts by assuming that you can already build the standard SPDK distribution on your
platform. The SPDK iSCSI target has been known to work on several Linux distributions, namely
Ubuntu 14.04, 15.04, and 15.10, Fedora 21, 22, and 23, and CentOS 7.

Once built, the binary will be in `app/iscsi_tgt`.

If you want to kill the application by using signal, make sure use the SIGTERM, then the application
will release all the shared memory resource before exit, the SIGKILL will make the shared memory
resource have no chance to be released by applications, you may need to release the resource manually.

## Configuring iSCSI Target {#iscsi_config}

A `iscsi_tgt` specific configuration file is used to configure the iSCSI target. A fully documented
example configuration file is located at `etc/spdk/iscsi.conf.in`.

The configuration file is used to configure the SPDK iSCSI target. This file defines the following:
TCP ports to use as iSCSI portals; general iSCSI parameters; initiator names and addresses to allow
access to iSCSI target nodes; number and types of storage backends to export over iSCSI LUNs; iSCSI
target node mappings between portal groups, initiator groups, and LUNs.

Each LUN in an iSCSI target node is associated with an SPDK block device.  See @ref bdev_getting_started
for details on configuring SPDK block devices.  The block device to LUN mappings are specified in the
configuration file as:

~~~~
[TargetNodeX]
  LUN0 Malloc0
  LUN1 Nvme0n1
~~~~

This exports a malloc'd target. The disk is a RAM disk that is a chunk of memory allocated by iscsi in
user space. It will use offload engine to do the copy job instead of memcpy if the system has enough DMA
channels.

You should make a copy of the example configuration file, modify it to suit your environment, and
then run the iscsi_tgt application and pass it the configuration file using the -c option. Right now,
the target requires elevated privileges (root) to run.

~~~
app/iscsi_tgt/iscsi_tgt -c /path/to/iscsi.conf
~~~

## Configuring iSCSI Initiator {#iscsi_initiator}

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

### Setup

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


# iSCSI Hotplug {#iscsi_hotplug}

At the iSCSI level, we provide the following support for Hotplug:

1. bdev/nvme:
At the bdev/nvme level, we start one hotplug monitor which will call
spdk_nvme_probe() periodically to get the hotplug events. We provide the
private attach_cb and remove_cb for spdk_nvme_probe(). For the attach_cb,
we will create the block device base on the NVMe device attached, and for the
remove_cb, we will unregister the block device, which will also notify the
upper level stack (for iSCSI target, the upper level stack is scsi/lun) to
handle the hot-remove event.

2. scsi/lun:
When the LUN receive the hot-remove notification from block device layer,
the LUN will be marked as removed, and all the IOs after this point will
return with check condition status. Then the LUN starts one poller which will
wait for all the commands which have already been submitted to block device to
return back; after all the commands return back, the LUN will be deleted.

## Known bugs and limitations {#iscsi_hotplug_bugs}

For write command, if you want to test hotplug with write command which will
cause r2t, for example 1M size IO, it will crash the iscsi tgt.
For read command, if you want to test hotplug with large read IO, for example 1M
size IO, it will probably crash the iscsi tgt.

@sa spdk_nvme_probe
