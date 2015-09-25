SPDK Porting Guide
==================

SPDK is ported to new environments by implementing the nvme_impl
interface.  The nvme_impl interface provides APIs for the driver
to allocate physically contiguous and pinned memory, perform PCI
operations (config cycles and mapping BARs), virtual to physical
address translation and allocating per I/O data structures.

SPDK includes a default implementation of the nvme_impl API based
on the Data Plane Development Kit ([DPDK](dpdk.org)) and
libpciaccess.  This DPDK implementation can be found in
lib/nvme/nvme_impl.h.  DPDK is currently supported on Linux and
FreeBSD only.

Users who want to use SPDK on other operating system, or in
userspace driver frameworks other than DPDK, will need to implement
a new version of nvme_impl.h.  The new nvme_impl.h can be
integrated into the SPDK build by updating the following line
in CONFIG:

    CONFIG_NVME_IMPL=nvme_impl.h

