# Changelog

## v19.07: (Upcoming Release)

## v19.04:

### nvme

Added asynchronous probe support. New APIs spdk_nvme_probe_async(),
spdk_nvme_connect_async() and spdk_nvme_probe_poll_async() were added to enable
this feature. spdk_nvme_probe_async() and spdk_nvme_connect_async() return a
context associated with the specified controllers. Users then call
spdk_nvme_probe_poll_async() until it returns 0, indicating that the operation
completed.

A new qpair creation option, delay_pcie_doorbell, was added. This can be passed
to spdk_nvme_alloc_io_qpair(). This makes the I/O submission functions, such as
spdk_nvme_ns_writev(), skip ringing the submission queue doorbell. Instead the
doorbell will be rung as necessary inside spdk_nvme_qpair_process_completions().
This can result in significantly fewer MMIO writes to the doorbell register
under heavy load, greatly improving performance.

spdk_nvme_ctrlr_get_regs_cmbsz() was added to report the size of the controller
memory buffer, if available.

spdk_nvme_ctrlr_get_flags() was added to return controller feature
flags. Two flags are currently tracked:
SPDK_NVME_CTRLR_SGL_SUPPORTED
SPDK_NVME_CTRLR_SECURITY_SEND_RECV_SUPPORTED

The NVMe hotplug poller is now able to detach devices hot-removed from the system
via `/sys/bus/pci/devices/<bdf>/remove` and `/sys/bus/pci/devices/<bdf>/driver/unbind`.

Opal support was added for scan, take ownership, revert TPer, and dumping device
info. The nvme_manage tool can be used to perform these operations. The public
API functions are spdk_nvme_ctrlr_security_receive() and
spdk_nvme_ctrlr_security_send(). This module should be considered experimental
pending additional features and tests.

### raid

Added new strip_size_kb rpc param on create to replace the more ambiguous
strip_size. The strip_size rpc param is deprecated.

Changed the raid bdev product_name from "Pooled Device" to "Raid Volume"

### thread

Added spdk_thread_has_pollers() function to verify if there are any registered
pollers to be run on the thread. Added spdk_thread_is_idle() function to check
if there are any scheduled operations to be performed on the thread at given
time.

spdk_thread_create() now takes an optional CPU affinity mask that is passed to
the scheduler.

spdk_thread_lib_init() now takes an optional context size. For each thread
created, an additional region of memory of this size will be allocated. A
pointer to this region of memory can be obtained by calling
spdk_thread_get_ctx(). The inverse operation is also available via
spdk_thread_get_from_ctx().

spdk_thread_poll() now optionally accepts the current time, in ticks. This can
be used to avoid some calls to spdk_get_ticks() internally.

spdk_app_start() now only accepts a single context argument.

### bdev

An new API `spdk_bdev_get_data_block_size` has been added to get size of data
block except for metadata.

spdk_vbdev_register() has been deprecated.  spdk_bdev_register() should be used
instead.

A mechanism for acquiring and releasing data buffers from bdev modules, used
to perform zero copy operations, was added.

New APIs spdk_bdev_get_md_size(), spdk_bdev_is_md_interleaved(), spdk_bdev_get_dif_type(),
spdk_bdev_is_dif_head_of_md(), and spdk_bdev_is_dif_check_enabled() have been
added to get metadata and DIF settings.

Bdevs claimed by the `examine_config` callback will be now further examined in the
`examine_disk` callback.

spdk_bdev_io_get_io_channel() was added as a convenient way to get an io_channel
from a bdev_io.

### NVMe-oF Target

Support for per-device shared receive queues in the RDMA transport has been added.
It is enabled by default for any device that supports it.

The size of a shared receive queue is defined by transport configuration file parameter
`MaxSRQDepth` and `nvmf_create_transport` RPC method parameter `max_srq_depth`.
Default size is 4096.

Add model number as parameter to construct_nvmf_subsystem (-d option),
rather than using hardcoded define.

DIF passthrough feature has been added. DIF setting of the allocated bdevs is
exposed to the NVMe-oF initiator and data with DIF from the NVMe-oF initiator is
passed through to the allocated bdevs.

### env

The `phys_addr` parameter in spdk_malloc() and spdk_zmalloc() has been deprecated.
For retrieving physical addresses, spdk_vtophys() should be used instead.

spdk_realloc() has been added to reallocate DMA/shared memory.

spdk_pci_device_is_removed() has been added to let the upper-layer SPDK drivers know
that device has a pending external hotremove request.

spdk_env_fini() and spdk_env_dpdk_post_fini() were added to release any resources
allocated by spdk_env_init() or spdk_env_dpdk_post_init() respectively. It is expected
that common usage of those functions is to call them just before terminating the process.

Added spdk_iommu_is_enabled() to report if SPDK application is using IOMMU for DMA.

### DPDK

Dropped support for DPDK 17.07 and earlier, which SPDK won't even compile with right now.

Updated DPDK submodule to DPDK 19.02.

### rpc

New `get_spdk_version` RPC method is introduced to get version info of the running SPDK application.

The `start_nbd_disk` RPC method now take nbd_device as an optional parameter. If nbd_device
is specified, use that specified nbd device. If it's not specified, pick available one.

### iSCSI target

DIF strip and insert is now supported. DIF settings are not exposed to the iSCSI initiator.
DIF is attached into data for write I/O and stripped from data for read I/O.

### vhost

Added experimental support for running with the external, upstream rte_vhost library.
This can be enabled by configuring SPDK with an `--without-internal-vhost-lib` flag.
The minimum supported rte_vhost version (DPDK version) is 19.05-rc1.

As a result of fuzz testing, a lot of data races in vhost-scsi LUN hotplug path were identified and
fixed. Those data races could have potentially resulted in SPDK crashes, RPC hangs, or memory leaks
if Vhost-SCSI LUN hotplug RPCs were executed while connected VMs were in the middle of restarting.

The SCSI target id in `add_vhost_scsi_lun` RPC is now optional. If `-1` is passed, the first
unoccupied target id will be used.

### AIO

AIO bdev module can now reap I/O completions directly from userspace, significantly improving
the overall performance.

### blobfs

Synchronous IO operations no longer use spdk_io_channel, but instead use
spdk_fs_thread_ctx. The behavior is otherwise identical.

### OCF

Added support for caching multiple bdevs using a single bdev as a cache.

### notify

Added the notify library that provides a high performance local event bus
between libraries. Example usage was added to bdev module, which reports
notifications for added and removed bdevs via RPC.

### sock

Added new API spdk_sock_readv() to the sock library for performing vectored
reads.

### event

The function spdk_subsystem_init() no longer requires spdk_event as an argument.

Changed API of spdk_subsystem_config_json() to no longer be asynchronous.

### io_uring

A bdev module that issues I/O to kernel block devices using the new io_uring Linux kernel
API was added. This module requires liburing.

### build

Options to easily compile with profile guided optimization have been added to
`configure`. To use profile guided optimization with SPDK, run
`./configure --with-pgo-capture`, build SPDK, then run a workload of your
choosing. Then, simply run `./configure --with-pgo-enable` and recompile to
build using the generated profile data. Profile guided optimization can yield
very large performance improvements, especially on GCC 8 and clang 7. This may
be combined with link time optimization which has been available under the
`--enable-lto` configure option for several releases.

### compression bdev/reduce library

Added "reduce" block compression scheme based on using SSDs for storing
compressed blocks of storage and presistent memory for metadata. Please see
[compression](https://spdk.io/doc/bdev.html) for more details.

## v19.01:

### ocf bdev

New virtual bdev module based on [Open CAS Framework](https://open-cas.github.io/) has been added.
This module allows for the use of one bdev to act as a high performance cache in front of another bdev.
Please see [documentation](https://spdk.io/doc/bdev.html#bdev_config_cas) for more details.
Only write through mode is currently supported and this feature is considered experimental.

### event framework

For `spdk_app_parse_args`, add return value to the callback which parses application
specific command line parameters to protect SPDK applications from crashing by invalid
values from user input.

By default, all SPDK applications will now reserve all hugepages at runtime. The pre-reserved
memory size can be still set with `-s` or `--mem-size` option, although the default value
was reduced down to 0.

A custom hugetlbfs directory can now be specified via spdk_app_opts.
This can be used to configure hugepages with different sizes, a different size limit,
or different access permissions than the system's default hugepage pool.
SPDK applications can specify a custom hugetlbfs mount with the `--huge-dir` option.

### environment

spdk_vtophys() has been refactored to accept length of the translated region as a new
parameter. The function will now update that parameter with the largest possible value
for which the memory is contiguous in the physical memory address space.

The following functions were removed:
 - spdk_pci_nvme_device_attach()
 - spdk_pci_nvme_enumerate()
 - spdk_pci_ioat_device_attach()
 - spdk_pci_ioat_enumerate()
 - spdk_pci_virtio_device_attach()
 - spdk_pci_virtio_enumerate()

They were replaced with generic spdk_pci_device_attach() and spdk_pci_enumerate() which
require a new spdk_pci_driver object to be provided. It can be one of the following:
 - spdk_pci_nvme_get_driver()
 - spdk_pci_ioat_get_driver()
 - spdk_pci_virtio_get_driver()

spdk_pci_hook_device() and spdk_pci_unhook_device() were added. Those allow adding a virtual
spdk_pci_device into the SPDK PCI subsystem. A virtual device calls provided callbacks for
each BAR mapping request or PCI config access. It's attachable with spdk_pci_device_attach()
or spdk_pci_enumerate() like any other device.

A new spdk_pause() function was added to pause CPU execution for an implementation specific
amount of time. Quoting from DPDK function this is based on: "This call is intended for
tight loops which poll a shared resource or wait for an event. A short pause within the loop
may reduce the power consumption."

A new public header file env_dpdk.h has been introduced, and function spdk_env_dpdk_post_init
is added into it. If user is using DPDK, and already called rte_eal_init, then include
include/spdk/env_dpdk.h, and call spdk_env_dpdk_post_init() instead of spdk_env_init.

ISA-L has been added as an SPDK submodule.  ISA-L is enabled by default on x86 architecture
to accelerate algorithms such as CRC for iSCSI and NVMe-oF.  Users may still disable ISA-L
by explicitly passing --without-isal to the configure script.

### util

A new uuid API `spdk_uuid_copy` was added to make a copy of the source uuid.

An new parameter `init_crc` representing the initial CRC value was added to
`spdk_crc16_t10dif`. The parameter can be used to calculate a CRC value spanning
multiple separate buffers.

New DIF APIs were added to generate and verify DIF by byte granularity for both DIF and DIX
formats. Among them, DIF with copy APIs will be usable to emulate DIF operations such as DIF
insert and strip.

Added `spdk_strtol` and `spdk_strtoll` to provide additional error checking around `strtol`
and `strtoll`.

Added `spdk_sprintf_append_realloc` and `spdk_vsprintf_append_realloc` for appending a string
with automatic buffer re-allocation.

### nvme

Wrapper functions spdk_nvme_ctrlr_security_send() and spdk_nvme_ctrlr_security_receive() are
introduced to support further security protocol development.

admin_timeout_ms was added to NVMe controller initialization options, users
can change the default value when probing a controller.

Add two new fields "header_digest" and "data_digest" in struct spdk_nvme_ctrlr_opts,
it will be used to enable the digest support for the NVMe/TCP transport.

Add a new TCP/IP transport(located in lib/nvme/nvme_tcp.c) in nvme driver. With
this new transport, it can be used to connect the NVMe-oF target with the
same TCP/IP support.

Added API, spdk_nvme_ctrlr_is_discovery(), to indicate whether the ctrlr
arg refers to a Discovery Controller or not.

Added an API function `spdk_nvme_host_id_parse` and corresponding object `spdk_nvme_host_id`
for parsing host address and host service ID arguments on a per connection basis.

The RPC `construct_nvme_bdev` now allows a user to specify a source address and service id for the host to
use when connecting to the controller backing the NVMe bdev.

### NVMe-oF Target

The `spdk_nvmf_tgt_opts` struct has been deprecated in favor of `spdk_nvmf_transport_opts`.
Users will no longer be able to specify target wide I/O parameters. `spdk_nvmf_tgt_listen`
will also no longer implicitly initialize a transport with the default target options (since
there are none). Instead, a user must manually instantiate the transport with `spdk_nvmf_transport_create`
prior to calling `spdk_nvmf_tgt_listen`.

Related to the previous change, the rpc `set_nvmf_target_options` has been renamed to
`set_nvmf_target_max_subsystems` to indicate that this is the only target option available for the user to edit.

Added fields `num_shared_buffers` and `buf_cache_size` in struct spdk_nvmf_transport_opts,
and also updated the related rpc function nvmf_create_transport, to make this
configurable parameter available to users. The `num_shared_buffers` is used to
configure the shared buffer numbers of the transport used by RDMA or TCP transport.
`buf_cache_size` configures number of shared buffers to cache per poll group.

### nvmf

Add a new TCP/IP transport (located in lib/nvmf/tcp.c). With this tranport,
the SPDK NVMe-oF target can have a new transport, and can serve the NVMe-oF
protocol via TCP/IP from the host.

Added optional mechanism to modify the RDMA transport's behavior when creating protection domains and registering memory.
By default, the RDMA transport will use the ibverbs library to create protection domains and register memory.
Using `spdk_nvme_rdma_init_hooks` will subvert that and use an existing registration.

### bdev

Added `enable_bdev_histogram` and `get_bdev_histogram` RPC commands to allow gathering latency data for specified bdev.
Please see [documentation](https://spdk.io/doc/bdev.html#rpc_bdev_histogram) for more details.

Added `required_alignment` field to `spdk_bdev`, that specifies an alignment requirement for data buffers associated with an spdk_bdev_io.
Bdev layer will automatically double buffer any spdk_bdev_io that violates this alignment, before the spdk_bdev_io is submitted to the bdev module.

On shutdown, bdev unregister now proceeds in top-down fashion, with
claimed bdevs skipped (these will be unregistered later, when virtual
bdev built on top of the respective base bdev unclaims it). This
allows virtual bdevs to be shut down cleanly as opposed to the
previous behavior that didn't differentiate between hotremove and
planned shutdown.

The `delete_bdev` RPC is now deprecated. Users should instead use the specific deletion RPC
for the bdev type to be removed (i.e. delete_malloc_bdev).

Added support for separate bandwidth rate limits for read and write to QoS in bdev layer.

Bdev I/O statistics now track unmap opertations.

### logical volumes

Logical volume bdev can now be marked as read only using `set_read_only_lvol_bdev` RPC.
This allows for basing clones on top of lvol_bdev without first creating a snapshot.

Added option to change method for data erasure when deleting lvol or resizing down.
Default of unmapping clusters can now be changed to writing zeroes or no operation.

Added option to change method for erasing data region on lvol store creation.
Default of unmapping can now be changed to writing zeroes or no operation.

### log

"trace flags" are now referred to as "log flags" in the SPDK log API.  The
set_trace_flag, clear_trace_flag and get_trace_flags RPCs are now deprecated,
and set_log_flag, clear_log_flag and get_log_flags RPCs have been added.

### trace

New `trace_record` application was added. It can be used to poll spdk trace shm file and
append any new trace entries into another specified file. This can help retain those entries
that would otherwise be overwritten in the shm file. See
[Capturing sufficient trace events](https://spdk.io/doc/nvmf_tgt_tracepoints.html#capture_trace_events)
for more details.

Number of trace entries in circular buffer per lcore can now be assigned by starting SPDK app
with argument "--num-trace-entries <NUM>" provided.

New `get_tpoint_group_mask` RPC was added to get current tpoint_group_mask, and
each tpoint group status.
New `enable_tpoint_group` and `disable_tpoint_group` RPC were added to enable or
disable a specific tpoint group.

### ftl

EXPERIMENTAL: Added basic flash translation layer module allowing for using Open Channel SSDs as
block devices. The module is split into the library (located in lib/ftl) and bdev_ftl
(lib/bdev/ftl). See the [documentation](https://spdk.io/doc/ftl.html) for more details.

### vhost

A security vulnerability has been identified and fixed in the SPDK vhost target.  A malicious
vhost client (i.e. virtual machine) could carefully construct a circular descriptor chain which
would result in a partial denial of service in the SPDK vhost target.  These types of descriptor
chains are now properly detected by the vhost target.  All SPDK vhost users serving untrusted
vhost clients are strongly recommended to upgrade. (Reported by Dima Stepanov and Evgeny
Yakovlev.)

Vhost SCSI and Vhost Block devices can now accept multiple connections on the same socket file.
Each connection (internally called a vhost session) will have access to the same storage, but
will use different virtqueues, different features and possibly different memory.

### vhost scsi

SCSI target hotremove can now be performed even without the VIRTIO_SCSI_F_HOTPLUG feature negotiated.
Regardless of VIRTIO_SCSI_F_HOTPLUG support, the hotremoval will be still reported through SCSI sense codes.

### DPDK

DPDK submodule was updated to DPDK 18.11. Note that SPDK does not fully leverage the new
multi-process device hotplug yet and continues to work the same way it always did.

Dropped support for DPDK 16.07 and earlier, which SPDK won't even compile with right now.

### RPC

The following RPC commands deprecated in the previous release are now removed:
 - construct_virtio_user_scsi_bdev
 - construct_virtio_pci_scsi_bdev
 - construct_virtio_user_blk_bdev
 - construct_virtio_pci_blk_bdev
 - remove_virtio_scsi_bdev
 - construct_nvmf_subsystem

### Miscellaneous

The configure options `--with-raid` and `--without-raid` that were deprecated in the previous
release are now removed.

### nbd

Starting nbd using `spdk_nbd_start` is now performed asynchronously.

### net framework

Net framework initialization and finish is now done asynchronously.

### rpc

Added `spdk_rpc_is_method_allowed` function for checking whether method is permitted in a given state.
Added `spdk_rpc_get_state` to check current state of RPC server.
RPC `wait_subsystem_init` has been added to allow clients to block untill all subsystems are initialized.

### json rpc

JSON RPC client is now running in non-blocking mode. Requests are sent and received during spdk_jsonrpc_client_poll.
JSON RPC server can now recieve a callback on connection termination or server shutdown using `spdk_jsonrpc_conn_add_close_cb`
and `spdk_jsonrpc_conn_del_close_cb`.

## v18.10:

### nvme

spdk_nvme_ctrlr_cmd_security_send() and spdk_nvme_ctrlr_cmd_security_receive()
were added to support sending or receiving security protocol data to or from
nvme controller.

spdk_nvme_ns_get_extended_sector_size() was added.  This function includes
the metadata size per sector (if any).  spdk_nvme_ns_get_sector_size() still
returns only the data size per sector, not including metadata.

New `send_nvme_cmd` RPC was added to allow sending NVMe commands directly to NVMe controller.
See the [send_nvme_cmd](http://spdk.io/doc/jsonrpc.html#rpc_send_nvme_cmd) documentation
for more details.

### Build System

New `configure` options, `--with-shared` and `--without-shared`
[default], provide the capability to build, or not, SPDK shared libraries.
This includes the single SPDK shared lib encompassing all of the SPDK
static libs as well as individual SPDK shared libs corresponding to
each of the SPDK static ones.  Although the production of the shared
libs conforms with conventional version naming practices, such naming
does not at this time confer any SPDK ABI compatibility claims.

### bdev

spdk_bdev_alias_del_all() was added to delete all alias from block device.

A new virtual bdev module has been added to perform at rest data encryption using the DPDK CryptoDev
Framework.  The module initially uses a software AESNI CBC cipher with experimental support for the
Intel QAT hardware accelerator also currently implemented with support for CBC cipher. Future work
may include additional ciphers as well as consideration for authentication.

The RAID virtual bdev module is now always enabled by default.  The configure --with-raid and
--without-raid options are now ignored and deprecated and will be removed in the next release.

Enforcement of bandwidth limits for quality of service (QoS) has been added to the bdev layer.
See the new [set_bdev_qos_limit](http://www.spdk.io/doc/jsonrpc.html#rpc_set_bdev_qos_limit)
documentation for more details. The previous set_bdev_qos_limit_iops RPC method introduced at
18.04 release has been deprecated. The new set_bdev_qos_limit RPC method can support both
bandwidth and IOPS limits.

spdk_bdev_config_json() and corresponding `get_bdevs_config` RPC was removed.

### Environment Abstraction Layer and Event Framework

The size parameter of spdk_mem_map_translate is now a pointer. This allows the
function to report back the actual size of the translation relative to the original
request made by the user.

A new structure spdk_mem_map_ops has been introduced to hold memory map related
callbacks. This structure is now passed as the second argument of spdk_mem_map_alloc
in lieu of the notify callback.

### DPDK 18.08

The DPDK submodule has been updated to the DPDK 18.08 release. SPDK will now automatically
utilize DPDK's dynamic memory management with DPDK versions >= 18.05.1.

Hugepages can be still reserved with `[-s|--mem-size <size>]` option at application startup,
but once we use them all up, instead of failing user allocations with -ENOMEM, we'll try
to dynamically reserve even more. This allows starting SPDK with `--mem-size 0` and using
only as many hugepages as it is really needed.

Due to this change, the memory buffers returned by `spdk_*malloc()` are no longer guaranteed
to be physically contiguous.

### I/OAT

I/OAT driver can now reinitialize I/OAT channels after encountering DMA errors.

### iscsi target

Parameter names of `set_iscsi_options` and `get_iscsi_global_params` RPC
method for CHAP authentication in discovery sessions have been changed to
align with `construct_target_node` RPC method. Old names are still usable
but will be removed in future release.

`set_iscsi_discovery_auth` and `set_iscsi_target_node_auth` RPC methods have
been added to set CHAP authentication for discovery sessions and existing
target nodes, respectively.

The SPDK iSCSI target supports an AuthFile which can be used to load CHAP
shared secrets when the iSCSI target starts. SPDK previously provided a
default location for this file (`/usr/local/etc/spdk/auth.conf`) if none was
specified. This default has been removed. Users must now explicitly specify
the location of this file to load CHAP shared secrets from a file, or use
the related iSCSI RPC methods to add them at runtime.

### iscsi initiator

The SPDK iSCSI initiator is no longer considered experimental and becomes
a first-class citizen among bdev modules. The basic usage has been briefly
described in the bdev user guide: [iSCSI bdev](https://spdk.io/doc/bdev.html#bdev_config_iscsi)

### Miscellaneous

The SPDK application framework can now parse long name command line parameters.
Most single-character parameters have a long name equivalent now. See the
[Command Line Parameters](https://spdk.io/doc/app_overview.html) documentation
for details or use the `--help` command line parameter to list all available
params.

bdevperf `-s` param (io size) was renamed to `-o` as `-s` had been already
used by existing apps for memory size.

bdevio can now accept all SPDK command line parameters. The config now has to
be provided with `-c` or `--config` param.

The following ioat/perf and nvme/perf parameters were renamed as well:
 `-s` (io size) to `-o`
 `-d` (mem size) to `-s`

The ReactorMask config file parameter has been deprecated.  Users should
use the -m or --cpumask command line option to specify the CPU core mask
for the application.

Default config file pathnames have been removed from iscsi_tgt, nvmf_tgt
and vhost.  Config file pathnames may now only be specified using the
-c command line option.

Users may no longer set DPDK_DIR in their environment to specify the
location of the DPDK installation used to build SPDK.  Using DPDK_DIR
has not been the documented nor recommended way to specify the DPDK
location for several releases, but removing it ensures no unexpected
surprises for users who may have DPDK_DIR defined for other reasons.
Users should just use the "configure" script to specify the DPDK
location before building SPDK.

Although we know that many developers still use Python 2 we are officially
switching to Python3 with requirement that all new code must be valid also
for Python 2 up to the EOL which is year 2020.

Invoking interpreter explicitly is forbidden for executable scripts. There
is no need to use syntax like "python ./scripts/rpc.py". All executable
scripts must contain proper shebang pointing to the right interpreter.
Scripts without shebang musn't be executable.

A Python script has been added to enable conversion of old INI config file
to new JSON-RPC config file format. This script can be found at
scripts/config_converter.py. Example how this script can be used:
~~~{.sh}
cat old_format.ini | scripts/config_converter.py > new_json_format.json
~~~

### Sock

Two additional parameters were added to spdk_sock_get_addr() for the server
port and client port. These parameters are named "sport" and "cport"
respectively.

### Virtio

The following RPC commands have been deprecated:
 - construct_virtio_user_scsi_bdev
 - construct_virtio_pci_scsi_bdev
 - construct_virtio_user_blk_bdev
 - construct_virtio_pci_blk_bdev
 - remove_virtio_scsi_bdev

The `construct_virtio_*` ones were replaced with a single `construct_virtio_dev`
command that can create any type of Virtio bdev(s). `remove_virtio_scsi_bdev`
was replaced with `remove_virtio_bdev` that can delete both Virtio Block and SCSI
devices.

### Blobfs

spdk_file_get_id() returning unique ID for the file was added.

### JSON

Added jsonrpc-client C library intended for issuing RPC commands from applications.

Added API enabling iteration over JSON object:
 - spdk_json_find()
 - spdk_json_find_string()
 - spdk_json_find_array()
 - spdk_json_object_first()
 - spdk_json_array_first()
 - spdk_json_next()

### Blobstore

Blobstore I/O operations are now based on io_units, instead of blobstore page size.
The io_unit size is now the same as the underlying block device's block size.
Logical volumes built on a block device with 512B block size can now be used as boot devices
in QEMU.

### SPDKCLI

The SPDKCLI interactive command tool for managing SPDK is no longer considered experimental.
Support for the iSCSI and NVMe-oF targets has been added.

## v18.07:

### bdev

A new public header file bdev_module.h has been introduced to facilitate the
development of new bdev modules. This header includes an interface for the
spdk_bdev_part and spdk_bdev_part_base objects to enable the creation of
multiple virtual bdevs on top of a single base bdev and should act as the
primary API for module authors.

spdk_bdev_get_opts() and spdk_bdev_set_opts() were added to set bdev-wide
options.

A mechanism for handling out of memory condition errors (ENOMEM) returned from
I/O submission requests at the bdev layer has been added. See
spdk_bdev_queue_io_wait().

The spdk_bdev_get_io_stat() function now returns cumulative totals instead of
resetting on each call. This allows multiple callers to query I/O statistics
without conflicting with each other. Existing users will need to adjust their
code to record the previous I/O statistics to calculate the delta between calls.

I/O queue depth tracking and samples options have been added. See
spdk_bdev_get_qd(), spdk_bdev_get_qd_sampling_period(), and
spdk_bdev_set_qd_sampling_period().

### RAID module
A new bdev module called "raid" has been added as experimental module which
aggregates underlying NVMe bdevs and exposes a single raid bdev. Please note
that vhost will not work with this module because it does not yet have support
for multi-element io vectors.

### Log

The debug log component flag available on several SPDK applications has been
renamed from `-t` to `-L` to prevent confusion with tracepoints and to allow the
option to be added to tools that already use `-t` to mean something else.

### Blobstore

A new function, spdk_bs_dump(), has been added that dumps all of the contents of
a blobstore to a file pointer. This includes the metadata and is very useful for
debugging.

Two new operations have been added for thin-provisioned blobs.
spdk_bs_inflate_blob() will allocate clusters for all thinly provisioned regions
of the blob and populate them with the correct data by reading from the backing
blob(s). spdk_bs_blob_decouple_parent() works similarly, but will only allocate
clusters that correspond to data in the blob's immediate parent. Clusters
allocated to grandparents or that aren't allocated at all will remain
thin-provisioned.

### BlobFS

Changed the return type of spdk_file_truncate() from void to int to allow the
propagation of `ENOMEM` errors.

### NVMe Driver

The new API functions spdk_nvme_qpair_add_cmd_error_injection() and
spdk_nvme_qpair_remove_cmd_error_injection() have been added for NVMe error
emulation. Users can set a specified command to fail with a particular error
status.

Changed the name `timeout_sec` parameter to `timeout_us` in
spdk_nvme_ctrlr_register_timeout_callback(), and also changed the type from
uint32_t to uint64_t. This will give users more fine-grained control over the
timeout period.

Basic support for Open Channel SSDs was added. See nvme_ocssd.h

### NVMe Over Fabrics

The spdk_nvmf_tgt_destroy() function is now asynchronous and takes a callback
as a parameter.

spdk_nvmf_qpair_disconnect() was added to allow the user to disconnect qpairs.

spdk_nvmf_subsystem_get_max_namespaces() was added to query the maximum allowed
number of namespaces for a given subsystem.

### Build System

The build system now generates a combined shared library (libspdk.so) that may
be used in place of the individual static libraries (libspdk_*.a). The combined
library includes all components of SPDK and is intended to make linking against
SPDK easier. The static libraries are also still provided for users that prefer
to link only the minimal set of components required.

### git pre-commit and pre-push hooks

The pre-commit hook will run `scripts/check_format.sh` and verify there are no
formating errors before allowing `git commit` to run. The pre-push hook runs
`make CONFIG_WERROR=y` with and without `CONFIG_DEBUG=y` using both the gcc and
clang compiler before allowing `git push` to run. Following each DEBUG build
`test/unit/unittest.sh` is run and verified. Results are recorded in the
`make.log` file.

To enable type: 'git config core.hooksPath .githooks'. To override after
configuration use the `git --no-verify` flag.

### RPC

The `start_nbd_disk` RPC method now returns the path to the kernel NBD device node
rather than always returning `true`.

### DPDK 18.05

The DPDK submodule has been rebased on the DPDK 18.05 release.  DPDK 18.05 supports
dynamic memory allocation, but due to some issues found after the DPDK 18.05 release,
that support is not enabled for SPDK 18.07.  Therefore, SPDK 18.07 will continue to use
the legacy memory allocation model.  The plan is to enable dynamic memory allocation
after the DPDK 18.08 release which should fix these issues.

### Environment Abstraction Layer and Event Framework

The spdk_mem_map_translate() function now takes a size parameter to indicate the size of
the memory region.  This can be used by environment implementations to validate the
requested translation.

The I/O Channel implementation has been moved to its own library - lib/thread. The
public API that was previously in spdk/io_channel.h is now in spdk/thread.h The
file spdk/io_channel.h remains and includes spdk/thread.h.

spdk_reactor_get_tsc_stats was added to return interesting statistics for each
reactor.

### IOAT

IOAT for copy engine is disabled by default. It can be enabled by specifying the Enable
option with "Yes" in `[Ioat]` section of the configuration file. The Disable option is
now deprecated and will be removed in a future release.

## v18.04: Logical Volume Snapshot/Clone, iSCSI Initiator, Bdev QoS, VPP Userspace TCP/IP

### vhost

The SPDK vhost-scsi, vhost-blk and vhost-nvme applications have fixes to address the
DPDK rte_vhost vulnerability [CVE-2018-1059](http://cve.mitre.org/cgi-bin/cvename.cgi?name=CVE-2018-1059).
Please see this [security advisory](https://access.redhat.com/security/cve/cve-2018-1059)
for additional information on the DPDK vulnerability.

Workarounds have been added to ensure vhost compatibility with QEMU 2.12.

EXPERIMENTAL: Support for vhost-nvme has been added to the SPDK vhost target. See the
[vhost documentation](http://www.spdk.io/doc/vhost.html) for more details.

### Unified Target Application

A new unified SPDK target application, `spdk_tgt`, has been added. This application combines the
functionality of several existing SPDK applications, including the iSCSI target, NVMe-oF target,
and vhost target. The new application can be managed through the existing configuration file and
[JSON-RPC](http://www.spdk.io/doc/jsonrpc.html) methods.

### Env

spdk_mempool_get_bulk() has been added to wrap DPDK rte_mempool_get_bulk().

New memory management functions spdk_malloc(), spdk_zmalloc(), and spdk_free() have been added.
These new functions have a `flags` parameter that allows the user to specify whether the allocated
memory needs to be suitable for DMA and whether it should be shared across processes with the same
shm_id. The new functions are intended to replace spdk_dma_malloc() and related functions, which will
eventually be deprecated and removed.

### Bdev

A new optional bdev module interface function, `init_complete`, has been added to notify bdev modules
when the bdev subsystem initialization is complete. This may be useful for virtual bdevs that require
notification that the set of initialization examine() calls is complete.

The bdev layer now allows modules to provide an optional per-bdev UUID, which can be retrieved with
the spdk_bdev_get_uuid() function.

Enforcement of IOPS limits for quality of service (QoS) has been added to the bdev layer. See the
[set_bdev_qos_limit_iops](http://www.spdk.io/doc/jsonrpc.html#rpc_set_bdev_qos_limit_iops) documentation
for more details.

### RPC

The `[Rpc]` configuration file section, which was deprecated in v18.01, has been removed.
Users should switch to the `-r` command-line parameter instead.

The JSON-RPC server implementation now allows up to 32 megabyte responses, growing as
needed; previously, the response was limited to 32 kilobytes.

### SPDKCLI

EXPERIMENTAL: New SPDKCLI interactive command tool for managing SPDK is available.
See the [SPDKCLI](http://www.spdk.io/doc/spdkcli.html) documentation for more details.

### NVMe Driver

EXPERIMENTAL: Support for WDS and RDS capable CMBs in NVMe controllers has been added. This support is
experimental pending a functional allocator to free and reallocate CMB buffers.

spdk_nvme_ns_get_uuid() has been added to allow retrieval of per-namespace UUIDs when available.

New API functions spdk_nvme_ctrlr_get_first_active_ns() and spdk_nvme_ctrlr_get_next_active_ns()
have been added to iterate active namespaces, as well as spdk_nvme_ctrlr_is_active_ns() to check if
a namespace ID is active.

### NVMe-oF Target

Namespaces may now be assigned unique identifiers via new optional `eui64` and `nguid` parameters
to the `nvmf_subsystem_add_ns` RPC method. Additionally, the NVMe-oF target automatically exposes
the backing bdev's UUID as the namespace UUID when available.

spdk_nvmf_subsystem_remove_ns() is now asynchronous and requires a callback to indicate completion.

### Blobstore

A number of functions have been renamed:

- spdk_bs_io_write_blob() => spdk_blob_io_write()
- spdk_bs_io_read_blob() => spdk_blob_io_read()
- spdk_bs_io_writev_blob() => spdk_blob_io_writev()
- spdk_bs_io_readv_blob() => spdk_blob_io_readv()
- spdk_bs_io_unmap_blob() => spdk_blob_io_unmap()
- spdk_bs_io_write_zeroes_blob() => spdk_blob_io_write_zeroes()

The old names still exist but are deprecated.  They will be removed in the v18.07 release.

spdk_blob_resize() is now an asynchronous operation to enable resizing a blob while I/O
are in progress to that blob on other threads.  An explicit spdk_blob_sync_md() is still
required to sync the updated metadata to disk.

### Logical Volumes

A new `destroy_lvol_bdev` RPC method to delete logical volumes has been added.

Lvols now have their own UUIDs which replace previous LvolStoreUUID_BlobID combination.

New Snapshot and Clone functionalities have been added. User may create Snapshots of existing Lvols
and Clones of existing Snapshots.
See the [lvol snapshots](http://www.spdk.io/doc/logical_volumes.html#lvol_snapshots) documentation
for more details.

Resizing logical volumes is now supported via the `resize_lvol_bdev` RPC method.

### Lib

A set of changes were made in the SPDK's lib code altering
instances of calls to `exit()` and `abort()` to return a failure instead
wherever reasonably possible.

spdk_app_start() no longer exit()'s on an internal failure, but
instead returns a non-zero error status.

spdk_app_parse_args() no longer exit()'s on help, '-h', or an invalid
option, but instead returns SPDK_APP_PARSE_ARGS_HELP and
SPDK_APP_PARSE_ARGS_FAIL, respectively, and SPDK_APP_PARSE_ARGS_SUCCESS
on success.

spdk_pci_get_device() has been deprecated and will be removed in SPDK v18.07.

### I/O Channels

The prototype for spdk_poller_fn() has been modified; it now returns a value indicating
whether or not the poller did any work.  Existing pollers will need to be updated to
return a value.

### iSCSI Target

The SPDK iSCSI target now supports the fd.io Vector Packet Processing (VPP) framework userspace
TCP/IP stack. See the [iSCSI VPP documentation](http://www.spdk.io/doc/iscsi.html#vpp) for more
details.

### iSCSI initiator

An iSCSI initiator bdev module has been added to SPDK.  This module should be considered
experimental pending additional features and tests.  More details can be found in
lib/bdev/iscsi/README.

### PMDK

The persistent memory (PMDK) bdev module is now enabled using `--with-pmdk` instead of
`--with-nvml`.  This reflects the renaming of the persistent memory library from NVML to
PMDK.

### Virtio Block driver

A userspace driver for Virtio Block devices has been added. It was built on top of the
[Virtio](http://www.spdk.io/doc/virtio.html) library and can be managed similarly to
the Virtio SCSI driver. See the
[Virtio Block](http://www.spdk.io/doc/bdev.html#bdev_config_virtio_blk) reference for
more information.

### Virtio with 2MB hugepages

The previous 1GB hugepage limitation has now been lifted. A new `-g` command-line option
enables SPDK Virtio to work with 2MB hugepages.
See [2MB hugepages](http://www.spdk.io/doc/virtio.html#virtio_2mb) for details.

## v18.01: Blobstore Thin Provisioning

### Build System

The build system now includes a `make install` rule, including support for the common
`DESTDIR` and `prefix` variables as used in other build systems.  Additionally, the prefix
may be set via the configure `--prefix` option.  Example: `make install prefix=/usr`.

### RPC

A JSON RPC listener is now enabled by default using a UNIX domain socket at /var/run/spdk.sock.
A -r option command line option has been added to enable an alternative UNIX domain socket location,
or a TCP port in the format ip_addr:tcp_port (i.e. 127.0.0.1:5260).  The Rpc configuration file
section is now deprecated and will be removed in the v18.04 release.

### I/O Channels

spdk_poller_register() and spdk_poller_unregister() were moved from the event
framework (include/spdk/event.h) to the I/O channel library
(include/spdk/io_channel.h). This allows code that doesn't depend on the event
framework to request registration and unregistration of pollers.

spdk_for_each_channel() now allows asynchronous operations during iteration.
Instead of immediately continuing the interation upon returning from the iteration
callback, the user must call spdk_for_each_channel_continue() to resume iteration.

### Block Device Abstraction Layer (bdev)

The poller abstraction was removed from the bdev layer. There is now a general purpose
abstraction for pollers available in include/spdk/io_channel.h

### Lib

A set of changes were made in the SPDK's lib code altering,
instances of calls to `exit()` and `abort()` to return a failure instead
wherever reasonably possible.  This has resulted in return type changes of
the API for:

- spdk_env_init() from type `void` to `int`.
- spdk_mem_map_init() from type `void` to `int`.

Applications making use of these APIs should be modified to check for
a non-zero return value instead of relying on them to fail without return.

### NVMe Driver

SPDK now supports hotplug for vfio-attached devices. But there is one thing keep in mind:
Only physical removal events are supported; removing devices via the sysfs `remove` file will not work.

### NVMe-oF Target

Subsystems are no longer tied explicitly to CPU cores. Instead, connections are handed out to the available
cores round-robin. The "Core" option in the configuration file has been removed.

### Blobstore

A number of functions have been renamed:

- spdk_bs_md_resize_blob() => spdk_blob_resize()
- spdk_bs_md_sync_blob() => spdk_blob_sync_md()
- spdk_bs_md_close_blob() => spdk_blob_close()
- spdk_bs_md_get_xattr_names() => spdk_blob_get_xattr_names()
- spdk_bs_md_get_xattr_value() => spdk_blob_get_xattr_value()
- spdk_blob_md_set_xattr() => spdk_blob_set_xattr()
- spdk_blob_md_remove_xattr() => spdk_blob_remove_xattr()
- spdk_bs_md_create_blob() => spdk_bs_create_blob()
- spdk_bs_md_open_blob() => spdk_bs_open_blob()
- spdk_bs_md_delete_blob() => spdk_bs_delete_blob()
- spdk_bs_md_iter_first() => spdk_bs_iter_first()
- spdk_bs_md_iter_next() => spdk_bs_iter_next()

The function signature of spdk_blob_close() has changed.  It now takes a struct spdk_blob * argument
rather than struct spdk_blob **.

The function signature of spdk_bs_iter_next() has changed.  It now takes a struct spdk_blob * argument
rather than struct spdk_blob **.

Thin provisioning support has been added to the blobstore.  It can be enabled by setting the
`thin_provision` flag in struct spdk_blob_opts when calling spdk_bs_create_blob_ext().

### NBD device

The NBD application (test/lib/bdev/nbd) has been removed; Same functionality can now be
achieved by using the test/app/bdev_svc application and start_nbd_disk RPC method.
See the [GPT](http://www.spdk.io/doc/bdev.html#bdev_config_gpt) documentation for more details.

### FIO plugin

SPDK `fio_plugin` now supports FIO 3.3. The support for previous FIO 2.21 has been dropped,
although it still remains to work for now. The new FIO contains huge amount of bugfixes and
it's recommended to do an update.

### Virtio library

Previously a part of the bdev_virtio module, now a separate library. Virtio is now available
via `spdk_internal/virtio.h` file. This is an internal interface to be used when implementing
new Virtio backends, namely Virtio-BLK.

### iSCSI

The MinConnectionIdleInterval parameter has been removed, and connections are no longer migrated
to an epoll/kqueue descriptor on the master core when idle.

## v17.10: Logical Volumes

### New dependencies

libuuid was added as new dependency for logical volumes.

libnuma is now required unconditionally now that the DPDK submodule has been updated to DPDK 17.08.

### Block Device Abstraction Layer (bdev)

An [fio](http://github.com/axboe/fio) plugin was added that can route
I/O to the bdev layer. See the [plugin documentation](https://github.com/spdk/spdk/tree/master/examples/bdev/fio_plugin/)
for more information.

spdk_bdev_unmap() was modified to take an offset and a length in bytes as
arguments instead of requiring the user to provide an array of SCSI
unmap descriptors. This limits unmaps to a single contiguous range.

spdk_bdev_write_zeroes() was introduced.  It ensures that all specified blocks will be zeroed out.
If a block device doesn't natively support a write zeroes command, the bdev layer emulates it using
write commands.

New API functions that accept I/O parameters in units of blocks instead of bytes
have been added:
- spdk_bdev_read_blocks(), spdk_bdev_readv_blocks()
- spdk_bdev_write_blocks(), spdk_bdev_writev_blocks()
- spdk_bdev_write_zeroes_blocks()
- spdk_bdev_unmap_blocks()

The bdev layer now handles temporary out-of-memory I/O failures internally by queueing the I/O to be
retried later.

### Linux AIO bdev

The AIO bdev now allows the user to override the auto-detected block size.

### NVMe driver

The NVMe driver now recognizes the NVMe 1.3 Namespace Optimal I/O Boundary field.
NVMe 1.3 devices may report an optimal I/O boundary, which the driver will take
into account when splitting I/O requests.

The HotplugEnable option in `[Nvme]` sections of the configuration file is now
"No" by default. It was previously "Yes".

The NVMe library now includes a spdk_nvme_ns_get_ctrlr() function which returns the
NVMe Controller associated with a given namespace.

The NVMe library now allows the user to specify a host identifier when attaching
to a controller.  The host identifier is used as part of the Reservations feature,
as well as in the NVMe-oF Connect command.  The default host ID is also now a
randomly-generated UUID, and the default host NQN uses the host ID to generate
a UUID-based NQN.

spdk_nvme_connect() was added to allow the user to connect directly to a single
NVMe or NVMe-oF controller.

### NVMe-oF Target (nvmf_tgt)

The NVMe-oF target no longer requires any in-capsule data buffers to run, and
the feature is now entirely optional. Previously, at least 4 KiB in-capsule
data buffers were required.

NVMe-oF subsytems have a new configuration option, AllowAnyHost, to control
whether the host NQN whitelist is enforced when accepting new connections.
If no Host options have been specified and AllowAnyHost is disabled, the
connection will be denied; this is a behavior change from previous releases,
which allowed any host NQN to connect if the Host list was empty.
AllowAnyHost is disabled by default.

NVMe-oF namespaces may now be assigned arbitrary namespace IDs, and the number
of namespaces per subsystem is no longer limited.

The NVMe-oF target now supports the Write Zeroes command.

### Environment Abstraction Layer

A new default value, SPDK_MEMPOOL_DEFAULT_CACHE_SIZE, was added to provide
additional clarity when constructing spdk_mempools. Previously, -1 could be
passed and the library would choose a reasonable default, but this new value
makes it explicit that the default is being used.

### Blobstore

The blobstore super block now contains a bstype field to identify the type of the blobstore.
Existing code should be updated to fill out bstype when calling spdk_bs_init() and spdk_bs_load().

spdk_bs_destroy() was added to allow destroying blobstore on device
with an initialized blobstore.

spdk_bs_io_readv_blob() and spdk_bs_io_writev_blob() were added to enable
scattered payloads.

A CLI tool for blobstore has been added, allowing basic operations through either command
line or shell interface.  See the [blobcli](https://github.com/spdk/spdk/tree/master/examples/blob/cli)
documentation for more details.

### Event Framework

The ability to set a thread name, previously only used by the reactor code, is
now part of the spdk_thread_allocate() API.  Users may specify a thread name
which will show up in tools like `gdb`.

### Log

The spdk_trace_dump() function now takes a new parameter to allow the caller to
specify an output file handle (stdout or stderr, for example).

### Logical Volumes

Logical volumes library built on top of SPDK blobstore has been added.
It is possible to create logical volumes on top of other devices using RPC.

See the [logical volumes](http://www.spdk.io/doc/logical_volumes.html) documentation for more information.

### Persistent Memory

A new persistent memory bdev type has been added.
The persistent memory block device is built on top of [libpmemblk](http://pmem.io/nvml/libpmemblk/).
It is possible to create pmem devices on top of pmem pool files using RPC.

See the [Pmem Block Device](http://www.spdk.io/doc/bdev.html#bdev_config_pmem) documentation for more information.

### Virtio SCSI driver

A userspace driver for Virtio SCSI devices has been added.
The driver is capable of creating block devices on top of LUNs exposed by another SPDK vhost-scsi application.

See the [Virtio SCSI](http://www.spdk.io/doc/virtio.html) documentation and [Getting Started](http://www.spdk.io/doc/bdev.html#bdev_config_virtio_scsi) guide for more information.

### Vhost target

The vhost target application now supports live migration between QEMU instances.


## v17.07: Build system improvements, userspace vhost-blk target, and GPT bdev

### Build System

A `configure` script has been added to simplify the build configuration process.
The existing CONFIG file and `make CONFIG_...` options are also still supported.
Run `./configure --help` for information about available configuration options.

A DPDK submodule has been added to make building SPDK easier.  If no `--with-dpdk`
option is specified to configure, the SPDK build system will automatically build a
known-good configuration of DPDK with the minimal options enabled.  See the Building
section of README.md for more information.

A [Vagrant](https://www.vagrantup.com/) setup has been added to make it easier to
develop and use SPDK on systems without suitable NVMe hardware.  See the Vagrant
section of README.md for more information.

### Userspace vhost-blk target

The vhost library and example app have been updated to support the vhost-blk
protocol in addition to the existing vhost-scsi protocol.
See the [vhost documentation](http://www.spdk.io/doc/vhost.html) for more details.

### Block device abstraction layer (bdev)

A GPT virtual block device has been added, which automatically exposes GPT partitions
with a special SPDK-specific partition type as bdevs.
See the [GPT bdev documentation](http://www.spdk.io/doc/bdev.md#bdev_config_gpt) for
more information.

### NVMe driver

The NVMe driver has been updated to support recent Intel SSDs, including the Intel®
Optane™ SSD DC P4800X series.

A workaround has been added for devices that failed to recognize register writes
during controller reset.

The NVMe driver now allocates request tracking objects on a per-queue basis.  The
number of requests allowed on an I/O queue may be set during `spdk_nvme_probe()` by
modifying `io_queue_requests` in the opts structure.

The SPDK NVMe `fio_plugin` has been updated to support multiple threads (`numjobs`).

spdk_nvme_ctrlr_alloc_io_qpair() has been modified to allow the user to override
controller-level options for each individual I/O queue pair.
Existing callers with qprio == 0 can be updated to:
~~~
... = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
~~~
Callers that need to specify a non-default qprio should be updated to:
~~~
struct spdk_nvme_io_qpair_opts opts;
spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
opts.qprio = SPDK_NVME_QPRIO_...;
... = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, &opts, sizeof(opts));
~~~

### Environment Abstraction Layer

The environment abstraction layer has been updated to include several new functions
in order to wrap additional DPDK functionality. See `include/spdk/env.h` for the
current set of functions.

### SPDK Performance Analysis with Intel® VTune™ Amplifier

Support for SPDK performance analysis has been added to Intel® VTune™ Amplifier 2018.

This analysis provides:
- I/O performance monitoring (calculating standard I/O metrics like IOPS, throughput, etc.)
- Tuning insights on the interplay of I/O and compute devices by estimating how many cores
  would be reasonable to provide for SPDK to keep up with a current storage workload.

See the VTune Amplifier documentation for more information.


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
