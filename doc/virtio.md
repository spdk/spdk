# Virtio driver {#virtio}

# Introduction {#virtio_intro}

SPDK Virtio driver is a C library that allows communicating with Virtio devices.
It allows any SPDK application to become an initiator for (SPDK) vhost targets.

The driver supports two different usage models:

* PCI - This is the standard mode of operation when used in a guest virtual
  machine, where QEMU has presented the virtio controller as a virtual PCI device.
* vhost-user - Can be used to connect to a vhost socket directly on the same host.

The driver, just like the SPDK @ref vhost, is using pollers instead of standard
interrupts to check for an I/O response. If used inside a VM, it bypasses interrupt
and context switching overhead of QEMU and guest kernel, significantly boosting
the overall I/O performance.

This Virtio library is currently used to implement two bdev modules:
@ref bdev_config_virtio_scsi and @ref bdev_config_virtio_blk.
These modules will export generic SPDK block devices usable by any SPDK application.

# 2MB hugepages {#virtio_2mb}

vhost-user specification puts a limitation on the number of "memory regions" used (8).
Each region corresponds to one file descriptor, and DPDK - as SPDK's memory allocator -
uses one file per hugepage by default. So *by default* this makes SPDK Virtio practical
with only 1GB hugepages. To run an SPDK app using Virtio initiator with 2MB hugepages
it is required to pass '-g' command-line option . This forces DPDK to create a single
non-physically-contiguous hugetlbfs file for all its memory.
