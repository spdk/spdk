# Changelog

## v17.03: Blobstore and userspace vhost-scsi target

### Blobstore and BlobFS

The blobstore is a persistent, power-fail safe block allocator designed to be
used as the local storage system backing a higher-level storage service.
See the [blobstore documentation](http://www.spdk.io/doc/blob.html) for more details.

BlobFS adds basic filesystem functionality like filenames on top of the blobstore.
This release also includes a RocksDB Env implementation using BlobFS in place of the
kernel filesystem.
See the [BlobFS documentation](http://www.spdk.io/doc/blobfs.html) for more details.

### Userspace vhost-scsi target

A userspace implementation of the QEMU vhost-scsi protocol has been added.
The vhost target is capable of exporting SPDK bdevs to QEMU-based VMs as virtio devices.
See the [vhost documentation](http://www.spdk.io/doc/vhost.html) for more details.

### Event framework

The overhead of the main reactor event loop was reduced by optimizing the number of
calls to spdk_get_ticks() per iteration.

### NVMe library

The NVMe library will now automatically split readv/writev requests with scatter-gather
lists that do not map to valid PRP lists when the NVMe controller does not natively
support SGLs.

The `identify` and `perf` NVMe examples were modified to add a consistent format for
specifying remote NVMe over Fabrics devices via the `-r` option.
This is implemented using the new `spdk_nvme_transport_id_parse()` function.

### iSCSI Target

The [Nvme] section of the configuration file was modified to remove the `BDF` directive
and replace it with a `TransportID` directive. Both local (PCIe) and remote (NVMe-oF)
devices can now be specified as the backing block device. A script to generate an
entire [Nvme] section based on the local NVMe devices attached was added at
`scripts/gen_nvme.sh`.

### NVMe-oF Target

The [Nvme] section of the configuration file was modified to remove the `BDF` directive
and replace it with a `TransportID` directive. Both local (PCIe) and remote (NVMe-oF)
devices can now be specified as the backing block device. A script to generate an
entire [Nvme] section based on the local NVMe devices attached was added at
`scripts/gen_nvme.sh`.

## v16.12: NVMe over Fabrics host, hotplug, and multi-process

### NVMe library

The NVMe library has been changed to create its own request memory pool rather than
requiring the user to initialize the global `request_mempool` variable.  Apps can be
updated by simply removing the initialization of `request_mempool`.  Since the NVMe
library user no longer needs to know the size of the internal NVMe request
structure to create the pool, the `spdk_nvme_request_size()` function was also removed.

The `spdk_nvme_ns_cmd_deallocate()` function was renamed and extended to become
`spdk_nvme_ns_cmd_dataset_management()`, which allows access to all of the NVMe
Dataset Management command's parameters.  Existing callers can be updated to use
`spdk_nvme_ns_cmd_dataset_management()` with `SPDK_NVME_DSM_ATTR_DEALLOCATE` as the
`type` parameter.

The NVMe library SGL callback prototype has been changed to return virtual addresses
rather than physical addresses.  Callers of `spdk_nvme_ns_cmd_readv()` and
`spdk_nvme_ns_cmd_writev()` must update their `next_sge_fn` callbacks to match.

The NVMe library now supports NVMe over Fabrics devices in addition to the existing
support for local PCIe-attached NVMe devices.  For an example of how to enable
NVMe over Fabrics support in an application, see `examples/nvme/identify` and
`examples/nvme/perf`.

Hot insert/remove support for NVMe devices has been added.  To enable NVMe hotplug
support, an application should call the `spdk_nvme_probe()` function on a regular
basis to probe for new devices (reported via the existing `probe_cb` callback) and
removed devices (reported via a new `remove_cb` callback).  Hotplug is currently
only supported on Linux with the `uio_pci_generic` driver, and newly-added NVMe
devices must be bound to `uio_pci_generic` by an external script or tool.

Multiple processes may now coordinate and use a single NVMe device simultaneously
using [DPDK Multi-process Support](http://dpdk.org/doc/guides/prog_guide/multi_proc_support.html).

### NVMe over Fabrics target (`nvmf_tgt`)

The `nvmf_tgt` configuration file format has been updated significantly to enable
new features.  See the example configuration file `etc/spdk/nvmf.conf.in` for
more details on the new and changed options.

The NVMe over Fabrics target now supports virtual mode subsystems, which allow the
user to export devices from the SPDK block device abstraction layer as NVMe over
Fabrics subsystems.  Direct mode (raw NVMe device access) is also still supported,
and a single `nvmf_tgt` may export both types of subsystems simultaneously.

### Block device abstraction layer (bdev)

The bdev layer now supports scatter/gather read and write I/O APIs, and the NVMe
blockdev driver has been updated to support scatter/gather.  Apps can use the
new scatter/gather support via the `spdk_bdev_readv()` and `spdk_bdev_writev()`
functions.

The bdev status returned from each I/O has been extended to pass through NVMe
or SCSI status codes directly in cases where the underlying device can provide
a more specific status code.

A Ceph RBD (RADOS Block Device) blockdev driver has been added.  This allows the
`iscsi_tgt` and `nvmf_tgt` apps to export Ceph RBD volumes as iSCSI LUNs or
NVMe namespaces.

### General changes

`libpciaccess` has been removed as a dependency and DPDK PCI enumeration is
used instead. Prior to DPDK 16.07 enumeration by class code was not supported,
so for earlier DPDK versions, only Intel SSD DC P3x00 devices will be discovered
by the NVMe library.

The `env` environment abstraction library has been introduced, and a default
DPDK-based implementation is provided as part of SPDK.  The goal of the `env`
layer is to enable use of alternate user-mode memory allocation and PCI access
libraries.  See `doc/porting.md` for more details.

The build process has been modified to produce all of the library files in the
`build/lib` directory.  This is intended to simplify the use of SPDK from external
projects, which can now link to SPDK libraries by adding the `build/lib` directory
to the library path via `-L` and linking the SPDK libraries by name (for example,
`-lspdk_nvme -lspdk_log -lspdk_util`).

`nvmf_tgt` and `iscsi_tgt` now have a JSON-RPC interface, which allows the user
to query and modify the configuration at runtime.  The RPC service is disabled by
default, since it currently does not provide any authentication or security
mechanisms; it should only be enabled on systems with controlled user access
behind a firewall. An example RPC client implemented in Python is provided in
`scripts/rpc.py`.

## v16.08: iSCSI target, NVMe over Fabrics maturity

This release adds a userspace iSCSI target. The iSCSI target is capable of exporting
NVMe devices over a network using the iSCSI protocol. The application is located
in app/iscsi_tgt and a documented configuration file can be found at etc/spdk/spdk.conf.in.

This release also significantly improves the existing NVMe over Fabrics target.
  - The configuration file format was changed, which will require updates to
    any existing nvmf.conf files (see `etc/spdk/nvmf.conf.in`):
    - `SubsystemGroup` was renamed to `Subsystem`.
    - `AuthFile` was removed (it was unimplemented).
    - `nvmf_tgt` was updated to correctly recognize NQN (NVMe Qualified Names)
      when naming subsystems.  The default node name was changed to reflect this;
      it is now "nqn.2016-06.io.spdk".
    - `Port` and `Host` sections were merged into the `Subsystem` section
    - Global options to control max queue depth, number of queues, max I/O
      size, and max in-capsule data size were added.
    - The Nvme section was removed. Now a list of devices is specified by
      bus/device/function directly in the Subsystem section.
    - Subsystems now have a Mode, which can be Direct or Virtual. This is an attempt
      to future-proof the interface, so the only mode supported by this release
      is "Direct".
  - Many bug fixes and cleanups were applied to the `nvmf_tgt` app and library.
  - The target now supports discovery.

This release also adds one new feature and provides some better examples and tools
for the NVMe driver.
  - The Weighted Round Robin arbitration method is now supported. This allows
    the user to specify different priorities on a per-I/O-queue basis.  To
    enable WRR, set the `arb_mechanism` field during `spdk_nvme_probe()`.
  - A simplified "Hello World" example was added to show the proper way to use
    the NVMe library API; see `examples/nvme/hello_world/hello_world.c`.
  - A test for measuring software overhead was added. See `test/lib/nvme/overhead`.

## v16.06: NVMf userspace target

This release adds a userspace NVMf (NVMe over Fabrics) target, conforming to the
newly-released NVMf 1.0/NVMe 1.2.1 specification.  The NVMf target exports NVMe
devices from a host machine over the network via RDMA.  Currently, the target is
limited to directly exporting physical NVMe devices, and the discovery subsystem
is not supported.

This release includes a general API cleanup, including renaming all declarations
in public headers to include a `spdk` prefix to prevent namespace clashes with
user code.

- NVMe
  - The `nvme_attach()` API was reworked into a new probe/attach model, which
  moves device detection into the NVMe library.  The new API also allows
  parallel initialization of NVMe controllers, providing a major reduction in
  startup time when using multiple controllers.
  - I/O queue allocation was changed to be explicit in the API.  Each function
  that generates I/O requests now takes a queue pair (`spdk_nvme_qpair *`)
  argument, and I/O queues may be allocated using
  `spdk_nvme_ctrlr_alloc_io_qpair()`.  This allows more flexible assignment of
  queue pairs than the previous model, which only allowed a single queue
  per thread and limited the total number of I/O queues to the lowest number
  supported on any attached controller.
  - Added support for the Write Zeroes command.
  - `examples/nvme/perf` can now report I/O command latency from the
   the controller's viewpoint using the Intel vendor-specific read/write latency
   log page.
  - Added namespace reservation command support, which can be used to coordinate
  sharing of a namespace between multiple hosts.
  - Added hardware SGL support, which enables use of scattered buffers that
   don't conform to the PRP list alignment and length requirements on supported
   NVMe controllers.
  - Added end-to-end data protection support, including the ability to write and
  read metadata in extended LBA (metadata appended to each block of data in the
  buffer) and separate metadata buffer modes.
  See `spdk_nvme_ns_cmd_write_with_md()` and `spdk_nvme_ns_cmd_read_with_md()`
  for details.
- IOAT
  - The DMA block fill feature is now exposed via the `ioat_submit_fill()`
  function.  This is functionally similar to `memset()`, except the memory is
  filled with an 8-byte repeating pattern instead of a single byte like memset.
- PCI
  - Added support for using DPDK for PCI device mapping in addition to the
  existing libpciaccess option.  Using the DPDK PCI support also allows use of
  the Linux VFIO driver model, which means that SPDK userspace drivers will work
  with the IOMMU enabled.  Additionally, SPDK applications may be run as an
  unprivileged user with access restricted to a specific set of PCIe devices.
  - The PCI library API was made more generic to abstract away differences
  between the underlying PCI access implementations.

## v1.2.0: IOAT user-space driver

This release adds a user-space driver with support for the Intel I/O Acceleration Technology (I/OAT, also known as "Crystal Beach") DMA offload engine.

- IOAT
  - New user-space driver supporting DMA memory copy offload
  - Example programs `ioat/perf` and `ioat/verify`
  - Kernel-mode DMA engine test driver `kperf` for performance comparison
- NVMe
  - Per-I/O flags for Force Unit Access (FUA) and Limited Retry
  - Public API for retrieving log pages
  - Reservation register/acquire/release/report command support
  - Scattered payload support - an alternate API to provide I/O buffers via a sequence of callbacks
  - Declarations and `nvme/identify` support for Intel SSD DC P3700 series vendor-specific log pages and features
- Updated to support DPDK 2.2.0


## v1.0.0: NVMe user-space driver

This is the initial open source release of the Storage Performance Development Kit (SPDK).

Features:
- NVMe user-space driver
- NVMe example programs
  - `examples/nvme/perf` tests performance (IOPS) using the NVMe user-space driver
  - `examples/nvme/identify` displays NVMe controller information in a human-readable format
- Linux and FreeBSD support
