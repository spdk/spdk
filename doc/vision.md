# The SPDK Vision {#vision}

The Storage Performance Development Kit is primarily intended to be used within
data centers to accelerate access to SSDs based on next generation media. These
data centers are assumed to be built from mostly commodity hardware running
Linux or FreeBSD on bare metal. The systems are arranged in racks and networked
together primarily via ethernet. On top of each of the base systems several
services and some number of virtual machines then run. The services provide the
virtual machines with access to their hardware - storage, network,
accelerators, etc., and may provide additional functionality such as cluster
orchestration. These services, in our vision, are regular user space processes.
The storage service in particular may provide a wide range of functionality
such as local and remote access to disks, logical volume management, snapshots,
quality of service management, usage-based billing, replication, and more.

The virtual machines communicate with the local storage service, at least in
the case of QEMU-based VMs, using virtio. Specifically, we recommend that the
storage service incorporate the SPDK
[vhost target](http://www.spdk.io/doc/vhost.html)
for fast, zero-copy communication. The storage service then may choose to
connect to local disks through the SPDK NVMe driver or to remote disks via
iSCSI or NVMe-oF. This strategy has a number of advantages over having the
guest in the virtual machine connect directly to a remote disk. First, QEMU VMs
can consume disks exposed via virtio without any modification or guest kernel
support. This is critical when the VM image is actually from a third party.
Second, this allows the storage service provider to insert intelligence like
quality of service control, or billing, into the storage path.

Disks exposed over the network may be physically attached to one of the regular
compute nodes, in a
[JBOD](https://en.wikipedia.org/wiki/Non-RAID_drive_architectures) attached to
a dedicated storage node, or be some virtual disk exposed by a
[SAN](https://en.wikipedia.org/wiki/Storage_area_network). In all three cases,
the disks will almost certainly be exported using either iSCSI or NVMe-oF, and
the SPDK iSCSI and NVMe-oF targets are an excellent fit to accelerate the I/O
path.

Today, the vhost, iSCSI, and NVMe-oF targets have a fairly minimal feature set
and can't simply be deployed to the data center as a full solution.
Importantly, we don't intend for them to be the full and final solution.
Instead, we aim to create a foundational toolkit for building such services,
such that implementing quality of service tracking, or logical volume
management, or access to NVMe-oF disks, or whatever is required, is reasonably
simple to assemble and yields the absolute best performance.

There are other, more niche uses for SPDK as well. For cases where databases or
distributed storage systems are running directly on bare metal and need every
last bit of performance, there is work going on to provide
[enough file-like functionality](http://www.spdk.io/doc/blob.html) to fully
bypass the kernel filesystems. SPDK may also be an excellent fit for embedded
and/or real-time software, due both to its license and its base architecture.
