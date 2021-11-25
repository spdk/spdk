# iSCSI Target {#iscsi}

## iSCSI Target Getting Started Guide {#iscsi_getting_started}

The Storage Performance Development Kit iSCSI target application is named `iscsi_tgt`.
This following section describes how to run iscsi from your cloned package.

## Prerequisites {#iscsi_prereqs}

This guide starts by assuming that you can already build the standard SPDK distribution on your
platform.

Once built, the binary will be in `build/bin`.

If you want to kill the application by using signal, make sure use the SIGTERM, then the application
will release all the shared memory resource before exit, the SIGKILL will make the shared memory
resource have no chance to be released by applications, you may need to release the resource manually.

## Introduction

The following diagram shows relations between different parts of iSCSI structure described in this
document.

![iSCSI structure](iscsi.svg)

### Assigning CPU Cores to the iSCSI Target {#iscsi_config_lcore}

SPDK uses the [DPDK Environment Abstraction Layer](http://dpdk.org/doc/guides/prog_guide/env_abstraction_layer.html)
to gain access to hardware resources such as huge memory pages and CPU core(s). DPDK EAL provides
functions to assign threads to specific cores.
To ensure the SPDK iSCSI target has the best performance, place the NICs and the NVMe devices on the
same NUMA node and configure the target to run on CPU cores associated with that node. The following
command line option is used to configure the SPDK iSCSI target:

~~~bash
-m 0xF000000
~~~

This is a hexadecimal bit mask of the CPU cores where the iSCSI target will start polling threads.
In this example, CPU cores 24, 25, 26 and 27 would be used.

## Configuring iSCSI Target via RPC method {#iscsi_rpc}

The iSCSI target is configured via JSON-RPC calls. See @ref jsonrpc for details.

### Portal groups

- iscsi_create_portal_group -- Add a portal group.
- iscsi_delete_portal_group -- Delete an existing portal group.
- iscsi_target_node_add_pg_ig_maps -- Add initiator group to portal group mappings to an existing iSCSI target node.
- iscsi_target_node_remove_pg_ig_maps -- Delete initiator group to portal group mappings from an existing iSCSI target node.
- iscsi_get_portal_groups -- Show information about all available portal groups.

~~~bash
/path/to/spdk/scripts/rpc.py iscsi_create_portal_group 1 10.0.0.1:3260
~~~

### Initiator groups

- iscsi_create_initiator_group -- Add an initiator group.
- iscsi_delete_initiator_group -- Delete an existing initiator group.
- iscsi_initiator_group_add_initiators -- Add initiators to an existing initiator group.
- iscsi_get_initiator_groups -- Show information about all available initiator groups.

~~~bash
/path/to/spdk/scripts/rpc.py iscsi_create_initiator_group 2 ANY 10.0.0.2/32
~~~

### Target nodes

- iscsi_create_target_node -- Add an iSCSI target node.
- iscsi_delete_target_node -- Delete an iSCSI target node.
- iscsi_target_node_add_lun -- Add a LUN to an existing iSCSI target node.
- iscsi_get_target_nodes -- Show information about all available iSCSI target nodes.

~~~bash
/path/to/spdk/scripts/rpc.py iscsi_create_target_node Target3 Target3_alias MyBdev:0 1:2 64 -d
~~~

## Configuring iSCSI Initiator {#iscsi_initiator}

The Linux initiator is open-iscsi.

Installing open-iscsi package
Fedora:
~~~bash
yum install -y iscsi-initiator-utils
~~~

Ubuntu:
~~~bash
apt-get install -y open-iscsi
~~~

### Setup

Edit /etc/iscsi/iscsid.conf
~~~bash
node.session.cmds_max = 4096
node.session.queue_depth = 128
~~~

iscsid must be restarted or receive SIGHUP for changes to take effect. To send SIGHUP, run:
~~~bash
killall -HUP iscsid
~~~

Recommended changes to /etc/sysctl.conf
~~~bash
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

Assume target is at 10.0.0.1

~~~bash
iscsiadm -m discovery -t sendtargets -p 10.0.0.1
~~~

### Connect to target

~~~bash
iscsiadm -m node --login
~~~

At this point the iSCSI target should show up as SCSI disks. Check dmesg to see what
they came up as.

### Disconnect from target

~~~bash
iscsiadm -m node --logout
~~~

### Deleting target node cache

~~~bash
iscsiadm -m node -o delete
~~~

This will cause the initiator to forget all previously discovered iSCSI target nodes.

### Finding /dev/sdX nodes for iSCSI LUNs

~~~bash
iscsiadm -m session -P 3 | grep "Attached scsi disk" | awk '{print $4}'
~~~

This will show the /dev node name for each SCSI LUN in all logged in iSCSI sessions.

### Tuning

After the targets are connected, they can be tuned. For example if /dev/sdc is
an iSCSI disk then the following can be done:
Set noop to scheduler

~~~bash
echo noop > /sys/block/sdc/queue/scheduler
~~~

Disable merging/coalescing (can be useful for precise workload measurements)

~~~bash
echo "2" > /sys/block/sdc/queue/nomerges
~~~

Increase requests for block queue

~~~bash
echo "1024" > /sys/block/sdc/queue/nr_requests
~~~

### Example: Configure simple iSCSI Target with one portal and two LUNs

Assuming we have one iSCSI Target server with portal at 10.0.0.1:3200, two LUNs (Malloc0 and Malloc1),
 and accepting initiators on 10.0.0.2/32, like on diagram below:

![Sample iSCSI configuration](iscsi_example.svg)

#### Configure iSCSI Target

Start iscsi_tgt application:

```bash
./build/bin/iscsi_tgt
```

Construct two 64MB Malloc block devices with 512B sector size "Malloc0" and "Malloc1":

```bash
./scripts/rpc.py bdev_malloc_create -b Malloc0 64 512
./scripts/rpc.py bdev_malloc_create -b Malloc1 64 512
```

Create new portal group with id 1, and address 10.0.0.1:3260:

```bash
./scripts/rpc.py iscsi_create_portal_group 1 10.0.0.1:3260
```

Create one initiator group with id 2 to accept any connection from 10.0.0.2/32:

```bash
./scripts/rpc.py iscsi_create_initiator_group 2 ANY 10.0.0.2/32
```

Finally construct one target using previously created bdevs as LUN0 (Malloc0) and LUN1 (Malloc1)
with a name "disk1" and alias "Data Disk1" using portal group 1 and initiator group 2.

```bash
./scripts/rpc.py iscsi_create_target_node disk1 "Data Disk1" "Malloc0:0 Malloc1:1" 1:2 64 -d
```

#### Configure initiator

Discover target

~~~bash
$ iscsiadm -m discovery -t sendtargets -p 10.0.0.1
10.0.0.1:3260,1 iqn.2016-06.io.spdk:disk1
~~~

Connect to the target

~~~bash
iscsiadm -m node --login
~~~

At this point the iSCSI target should show up as SCSI disks.

Check dmesg to see what they came up as. In this example it can look like below:

~~~bash
...
[630111.860078] scsi host68: iSCSI Initiator over TCP/IP
[630112.124743] scsi 68:0:0:0: Direct-Access     INTEL    Malloc disk      0001 PQ: 0 ANSI: 5
[630112.125445] sd 68:0:0:0: [sdd] 131072 512-byte logical blocks: (67.1 MB/64.0 MiB)
[630112.125468] sd 68:0:0:0: Attached scsi generic sg3 type 0
[630112.125926] sd 68:0:0:0: [sdd] Write Protect is off
[630112.125934] sd 68:0:0:0: [sdd] Mode Sense: 83 00 00 08
[630112.126049] sd 68:0:0:0: [sdd] Write cache: enabled, read cache: disabled, doesn't support DPO or FUA
[630112.126483] scsi 68:0:0:1: Direct-Access     INTEL    Malloc disk      0001 PQ: 0 ANSI: 5
[630112.127096] sd 68:0:0:1: Attached scsi generic sg4 type 0
[630112.127143] sd 68:0:0:1: [sde] 131072 512-byte logical blocks: (67.1 MB/64.0 MiB)
[630112.127566] sd 68:0:0:1: [sde] Write Protect is off
[630112.127573] sd 68:0:0:1: [sde] Mode Sense: 83 00 00 08
[630112.127728] sd 68:0:0:1: [sde] Write cache: enabled, read cache: disabled, doesn't support DPO or FUA
[630112.128246] sd 68:0:0:0: [sdd] Attached SCSI disk
[630112.129789] sd 68:0:0:1: [sde] Attached SCSI disk
...
~~~

You may also use simple bash command to find /dev/sdX nodes for each iSCSI LUN
in all logged iSCSI sessions:

~~~bash
$ iscsiadm -m session -P 3 | grep "Attached scsi disk" | awk '{print $4}'
sdd
sde
~~~

## iSCSI Hotplug {#iscsi_hotplug}

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

@sa spdk_nvme_probe

## iSCSI Login Redirection {#iscsi_login_redirection}

The SPDK iSCSI target application supports iSCSI login redirection feature.

A portal refers to an IP address and TCP port number pair, and a portal group
contains a set of portals. Users for the SPDK iSCSI target application configure
portals through portal groups.

To support login redirection feature, we utilize two types of portal groups,
public portal group and private portal group.

The SPDK iSCSI target application usually has a discovery portal. The discovery
portal is connected by an initiator to get a list of targets, as well as the list
of portals on which these target may be accessed, by a discovery session.

Public portal groups have their portals returned by a discovery session. Private
portal groups do not have their portals returned by a discovery session. A public
portal group may optionally have a redirect portal for non-discovery logins for
each associated target. This redirect portal must be from a private portal group.

Initiators configure portals in public portal groups as target portals. When an
initiator logs in to a target through a portal in an associated public portal group,
the target sends a temporary redirection response with a redirect portal. Then the
initiator logs in to the target again through the redirect portal.

Users set a portal group to public or private at creation using the
`iscsi_create_portal_group` RPC, associate portal groups with a target using the
`iscsi_create_target_node` RPC or the `iscsi_target_node_add_pg_ig_maps` RPC,
specify a up-to-date redirect portal in a public portal group for a target using
the `iscsi_target_node_set_redirect` RPC, and terminate the corresponding connections
by asynchronous logout request using the `iscsi_target_node_request_logout` RPC.

Typically users will use the login redirection feature in scale out iSCSI target
system, which runs multiple SPDK iSCSI target applications.
