# SPDK virtio bdev module

This directory contains an experimental SPDK virtio bdev module.
It currently supports very basic enumeration capabilities for
virtio-scsi devices as well as read/write operations to any
SCSI LUNs discovered during enumeration.

It supports two different usage models:
* PCI - This is the standard mode of operation when used in a guest virtual
machine, where QEMU has presented the virtio-scsi controller as a virtual
PCI device.  The virtio-scsi controller might be implemented in the host OS
by SPDK vhost-scsi, kernel vhost-scsi, or a QEMU virtio-scsi backend.
* User vhost - Can be used to connect to an SPDK vhost-scsi target running on
the same host.

Note that 1GB hugepages is pretty much required to use this driver in
user-vhost mode.  vhost protocol requires passing a file descriptor for
each region of memory being shared with the vhost target.  Since DPDK opens
every huge page explicitly, it is fairly limited on how many file descriptors
it can pass due to the VHOST_MEMORY_MAX_NREGIONS limit of 8.

Use the following configuration file snippet to enumerate a virtio-scsi PCI
device and present its LUNs as bdevs.  Currently it will only work with
a single PCI device.

~~~{.sh}
[Virtio]
  Dev Pci
~~~

Use the following configuration file snippet to enumerate an SPDK vhost-scsi
controller and present its LUNs as bdevs.  In this case, the SPDK vhost-scsi
target has created an SPDK vhost-scsi controller which is accessible through
the /tmp/vhost.0 domain socket.

~~~{.sh}
[Virtio]
  Dev User /tmp/vhost.0
~~~

Todo:
* Support multiple PCI devices, including specifying the PCI device by PCI
  bus/domain/function.
* Define SPDK virtio bdev request structure and report it as the context
  size during module initialization.  This will allow the module to build
  its request and response in per-bdev_io memory.
* Asynchronous I/O - currently the driver polls inline for all completions.
  Asynchronous I/O should be used for both enumeration (INQUIRY, READ CAPACITY,
  etc.) as well as read/write I/O.
* Add unmap support.
* Add I/O channel support.  Includes requesting correct number of queues
  (based on core count).  Fail device initialization if not enough queues 
  can be allocated.
* Add RPCs.
* Break out the "rte_virtio" code into a separate library that is not
  linked directly to the bdev module.  This would allow that part of the
  code to potentially get used and tested outside of the SPDK bdev framework.
* Check for allocation failures in bdev_virtio.c code.
* Change printfs to SPDK_TRACELOGs (or just remove altogether).
* Add virtio-blk support.  This will require some rework in the core
  virtio code (in the rte_virtio subdirectory) to allow for multiple
  device types.
* Bottom out on whether we should have one virtio driver to cover both
  scsi and blk.  If these should be separate, then this driver should be
  renamed to something scsi specific.
* Add reset support.
* Finish cleaning up "eth" references.  This includes filenames like
  virtio_ethdev.c and "eth" in various API calls.
* Improve the virtio_xmit_pkts and virtio_recv_pkts interfaces.  Should not
  reference the virtio_hw tx_queues directly.  Should have a more opaque API.
* Understand and handle queue full conditions.
* Clear interrupt flag for completions - since we are polling, we do not
  need the virtio-scsi backend to signal completion.
* Check interrupt flag for submission.  If the backend requires an interrupt,
  we need to signal it.
* Change read/write to use READ_16/WRITE_16 to handle LBA > 4G.  We can add
  a basic check and bail during enumeration if INQUIRY indicates the LUN does
  not support >= SBC-3.
* Automated test scripts for both PCI and vhost-user scenarios.
* Document Virtio config file section in examples.  Should wait on this until
  enough of the above items are implemented to consider this module as ready
  for more general use.
* Specify the name of the bdev in the config file (and RPC) - currently we
  just hardcode a single bdev name "Virtio0".
