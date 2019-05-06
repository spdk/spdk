# Direct Memory Access (DMA) From User Space {#memory}

The following is an attempt to explain why all data buffers passed to SPDK must
be allocated using spdk_dma_malloc() or its siblings, and why SPDK relies on
DPDK's proven base functionality to implement memory management.

Computing platforms generally carve physical memory up into 4KiB segments
called pages. They number the pages from 0 to N starting from the beginning of
addressable memory. Operating systems then overlay 4KiB virtual memory pages on
top of these physical pages using arbitrarily complex mappings. See
[Virtual Memory](https://en.wikipedia.org/wiki/Virtual_memory) for an overview.

Physical memory is attached on channels, where each memory channel provides
some fixed amount of bandwidth. To optimize total memory bandwidth, the
physical addressing is often set up to automatically interleave between
channels. For instance, page 0 may be located on channel 0, page 1 on channel
1, page 2 on channel 2, etc. This is so that writing to memory sequentially
automatically utilizes all available channels. In practice, interleaving is
done at a much more granular level than a full page.

Modern computing platforms support hardware acceleration for virtual to
physical translation inside of their Memory Management Unit (MMU). The MMU
often supports multiple different page sizes. On recent x86_64 systems, 4KiB,
2MiB, and 1GiB pages are supported. Typically, operating systems use 4KiB pages
by default.

NVMe devices transfer data to and from system memory using Direct Memory Access
(DMA). Specifically, they send messages across the PCI bus requesting data
transfers. In the absence of an IOMMU, these messages contain *physical* memory
addresses. These data transfers happen without involving the CPU, and the MMU
is responsible for making access to memory coherent.

NVMe devices also may place additional requirements on the physical layout of
memory for these transfers. The NVMe 1.0 specification requires all physical
memory to be describable by what is called a *PRP list*. To be described by a
PRP list, memory must have the following properties:

* The memory is broken into physical 4KiB pages, which we'll call device pages.
* The first device page can be a partial page starting at any 4-byte aligned
  address. It may extend up to the end of the current physical page, but not
  beyond.
* If there is more than one device page, the first device page must end on a
  physical 4KiB page boundary.
* The last device page begins on a physical 4KiB page boundary, but is not
  required to end on a physical 4KiB page boundary.

The specification allows for device pages to be other sizes than 4KiB, but all
known devices as of this writing use 4KiB.

The NVMe 1.1 specification added support for fully flexible scatter gather lists,
but the feature is optional and most devices available today do not support it.

User space drivers run in the context of a regular process and so have access
to virtual memory. In order to correctly program the device with physical
addresses, some method for address translation must be implemented.

The simplest way to do this on Linux is to inspect `/proc/self/pagemap` from
within a process. This file contains the virtual address to physical address
mappings. As of Linux 4.0, accessing these mappings requires root privileges.
However, operating systems make absolutely no guarantee that the mapping of
virtual to physical pages is static. The operating system has no visibility
into whether a PCI device is directly transferring data to a set of physical
addresses, so great care must be taken to coordinate DMA requests with page
movement. When an operating system flags a page such that the virtual to
physical address mapping cannot be modified, this is called **pinning** the
page.

There are several reasons why the virtual to physical mappings may change, too.
By far the most common reason is due to page swapping to disk. However, the
operating system also moves pages during a process called compaction, which
collapses identical virtual pages onto the same physical page to save memory.
Some operating systems are also capable of doing transparent memory
compression. It is also increasingly possible to hot-add additional memory,
which may trigger a physical address rebalance to optimize interleaving.

POSIX provides the `mlock` call that forces a virtual page of memory to always
be backed by a physical page. In effect, this is disabling swapping. This does
*not* guarantee, however, that the virtual to physical address mapping is
static. The `mlock` call should not be confused with a **pin** call, and it
turns out that POSIX does not define an API for pinning memory. Therefore, the
mechanism to allocate pinned memory is operating system specific.

SPDK relies on DPDK to allocate pinned memory. On Linux, DPDK does this by
allocating `hugepages` (by default, 2MiB). The Linux kernel treats hugepages
differently than regular 4KiB pages. Specifically, the operating system will
never change their physical location. This is not by intent, and so things
could change in future versions, but it is true today and has been for a number
of years (see the later section on the IOMMU for a future-proof solution).

With this explanation, hopefully it is now clear why all data buffers passed to
SPDK must be allocated using spdk_dma_malloc() or its siblings. The buffers
must be allocated specifically so that they are pinned and so that physical
addresses are known.

# IOMMU Support

Many platforms contain an extra piece of hardware called an I/O Memory
Management Unit (IOMMU). An IOMMU is much like a regular MMU, except it
provides virtualized address spaces to peripheral devices (i.e. on the PCI
bus). The MMU knows about virtual to physical mappings per process on the
system, so the IOMMU associates a particular device with one of these mappings
and then allows the user to assign arbitrary *bus addresses* to virtual
addresses in their process. All DMA operations between the PCI device and
system memory are then translated through the IOMMU by converting the bus
address to a virtual address and then the virtual address to the physical
address. This allows the operating system to freely modify the virtual to
physical address mapping without breaking ongoing DMA operations. Linux
provides a device driver, `vfio-pci`, that allows a user to configure the IOMMU
with their current process.

This is a future-proof, hardware-accelerated solution for performing DMA
operations into and out of a user space process and forms the long-term
foundation for SPDK and DPDK's memory management strategy. We highly recommend
that applications are deployed using vfio and the IOMMU enabled, which is fully
supported today.
