# SPDK virtio bdev module

This directory contains an experimental SPDK virtio bdev module.
It currently supports very basic read/write operations for
virtio-scsi drive. Currently it will only work with a single
target with a single LUN.

It supports two different usage models:
* PCI - This is the standard mode of operation when used in a guest virtual
machine, where QEMU has presented the virtio-scsi controller as a virtual
PCI device.  The virtio-scsi controller might be implemented in the host OS
by SPDK vhost-scsi. Kernel/QEMU vhost-scsi are not supported yet.
* User vhost - Can be used to connect to an SPDK vhost-scsi target running on
the same host.

Note that 1GB hugepages is pretty much required to use this driver in
user-vhost mode.  vhost protocol requires passing a file descriptor for
each region of memory being shared with the vhost target.  Since DPDK opens
every huge page explicitly, it is fairly limited on how many file descriptors
it can pass due to the VHOST_MEMORY_MAX_NREGIONS limit of 8.

Use the following configuration file snippet to enumerate a virtio-scsi PCI
device and present its LUNs as bdevs.

~~~{.sh}
[VirtioPci]
  Enable Yes
~~~

Use the following configuration file snippet to enumerate an SPDK vhost-scsi
controller and present its LUNs as bdevs.  In this case, the SPDK vhost-scsi
target has created an SPDK vhost-scsi controller which is accessible through
the /tmp/vhost.0 domain socket.

~~~{.sh}
[VirtioUser0]
  Path /tmp/vhost.0
~~~

## Todo:
* Add I/O channel support.  Includes requesting correct number of queues
  (based on core count).  Fail device initialization if not enough queues 
  can be allocated.
* Add RPCs.
* Break out the "rte_virtio" code into a separate library that is not
  linked directly to the bdev module.  This would allow that part of the
  code to potentially get used and tested outside of the SPDK bdev framework.
* Check for allocation failures in bdev_virtio.c code.
* Add SPDK_TRACELOGs.
* Add virtio-blk support.  This will require some rework in the core
  virtio code (in the rte_virtio subdirectory) to allow for multiple
  device types.
* Bottom out on whether we should have one virtio driver to cover both
  scsi and blk.  If these should be separate, then this driver should be
  renamed to something scsi specific.
* Add reset support.
* Understand and handle queue full conditions.
* Automated test scripts for both PCI and vhost-user scenarios.
* Document Virtio config file section in examples.  Should wait on this until
  enough of the above items are implemented to consider this module as ready
  for more general use.
* Specify the name of the bdev in the config file (and RPC) - currently we
  just hardcode a single bdev name "Virtio0".
