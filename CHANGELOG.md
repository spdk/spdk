# Changelog

## v22.01.2

### dpdk

DPDK submodule is now updated to 21.11.2, it contains fixes up to DPDK 22.07
as well as fixes for CVE-2022-28199 and CVE-2022-2132.

Note that SPDK 22.01 will not work with DPDK 22.11 and later due to changes in PCI DPDK API.

(#2460) Temporarily disabled stringop-overread for DPDK compilation.

(#2456) Removed assert from `pci_device_fini()`.

### bdev_nvme

(#2632) Fail reset sequence immediately if controller is already removed.

### bdev_ocf

OCF submodule is now updated to 21.6.3.1.

### nvme

(#2647) Added  `spdk_nvme_ctrlr_disable_read_changed_ns_list_log_page` to allow an
application to tell the driver to not read the CHANGED_NS_LIST log page in
response to a NS_ATTR_CHANGED AEN.  Applications that are consuming data from
the CHANGED_NS_LIST log page *must* call this new API when upgrading to SPDK
v22.01.  Otherwise the driver will perform the read to clear the AEN before
notifying the application, and when the application reads it the log page will
be empty.

(#2201) Check CSTS.CFS when initializing controller.

### nvmf_rdma

(#2541) Fix data_wr_pool corruption.

### nvmf_tcp

(#2527) Unregister timeout poller in qpair_destroy.

(#2445) Delay completion for zcopy requests with in-progress writes.

### vfio-user

libvfio-user submodule is now updated to support building with meson.

(#2530) Take endpoint as input parameter in quiesce_done.

### vhost

(#2452) Copy virtio_blk_outhdr to local struct.

(#2518) Do not kick VM when there are outstanding vhost-user messages.

## v22.01.1

### dpdk

DPDK submodule is now updated to DPDK 21.11.1. This includes fixes for DPDK vhost:
CVE-2021-3839
CVE-2022-0669
Users are strongly encouraged to upgrade.

(#2374) Added `rte_dmadev` as vhost library dependency.

### bdev_crypto

(#2500) Fixed bdev_io double completion.

### bdev_nvme

(#2440) Fixed NULL pointer access by DTRACE when creating qpair.

(#2335) Set `ana_state_updating` only after starting read ANA log page.

### blobstore

(#2347) Avoid recursion when split IO immediately complete.

### iscsi

(#2393) Fix a memory leak in `create_iscsi_sess()` error handling.

### nvme

(#2371) Allocate extra request for fabrics connect.

(#2465) Increase min admin queue size to 256

### nvmf

(#2335) `ctrlr_get_ana_state()` now always returns optimized if ana_reporting is disabled.

(#2395) Avoid a null dereference before null check in FC transport when freeing a request.

(#2471) Delay NVMe-oF TCP qpair destruction till after the socket write callback.

### thread

(#2378) Fix double unlock in `spdk_io_device_register()`.

### vmd

(#2413) Reset root port config before enumeration.

(#2451) Use `config_bus_number` when resetting root ports

## v22.01: NVMe multipath, NVMe TCP zero-copy, NVMe-oF discovery service

### accel

The batching capability was removed. Batching is now considered an implementation
detail of the low level drivers.

### bdev

The NVMe bdev module supports multipath and improved I/O error resiliency.

The parameter `retry_count` of the RPC `bdev_nvme_set_options` was deprecated and will be
removed in SPDK 22.04, and the parameter `transport_retry_count` is added and used instead.

An new parameter `bdev_retry_count` is added to the RPC `bdev_nvme_set_options`.

New parameters, `ctrlr_loss_timeout_sec`, `reconnect_delay_sec`, and `fast_io_fail_timeout_sec`, are
added to the RPC `bdev_nvme_attach_controller`.

An new parameter `num_io_queues` is added to `bdev_nvme_attach_controller` RPC to allow specifying amount
of requested IO queues.

Added `key_file` parameter to the `rbd_register_cluster` RPC.  It is an optional parameter to
specify a keyring file to connect to a RADOS cluster.

Added `spdk_bdev_get_zone_id` API to retrieve zone_id for a certain LBA.

### bdev_aio

Added `bdev_aio_rescan` RPC to allow rescanning the size of aio bdev.

### bdev_nvme

Added discovery service to bdev_nvme module, which now can connect to a discovery controller.
bdev_nvme will connect to all current and future subsystems in the discovery controller.
Added `bdev_nvme_start_discovery` and `bdev_nvme_stop_discovery` RPC.

### dpdk

Updated DPDK submodule to DPDK 21.11.

### env

Added `spdk_pci_for_each_device`.

Removed `spdk_pci_get_first_device` and `spdk_pci_get_next_device`.  These APIs were unsafe, because
they did not account for PCI devices being inserted or removed while the caller was using handles
returned from these APIs.  Existing users of these APIs should switch to `spdk_pci_for_each_device`
instead.

Added 3 experimental APIs to handle PCI device interrupts (`spdk_pci_device_enable_interrupt`,
`spdk_pci_device_disable_interrupt`, `spdk_pci_device_get_interrupt_efd`).

Added `framework_get_pci_devices` RPC to list PCIe devices attached to an SPDK application.

### idxd

Many APIs are now vectored rather than scalar, meaning they take iovecs instead of individual pointers.

### json

Added `spdk_json_write_bytearray` API to serialize a buffer as a hex string.

### nvme

API `spdk_nvme_trtype_is_fabrics` was added to return existing transport type
is fabric or not.

API `spdk_nvme_poll_group_remove` was limited to be available only for a
disconnected qpair in the group.

New APIs, `spdk_nvme_ctrlr_disconnect`, `spdk_nvme_ctrlr_reconnect_async`, and
`spdk_nvme_ctrlr_reconnect_poll_async`, have been added to improve error recovery, and
the existing APIs,`spdk_nvme_ctrlr_reset_async` and `spdk_nvme_ctrlr_reset_poll_async`
were deprecated.

Added `spdk_nvme_ctrlr_get_discovery_log_page` API for getting the full discovery log page
from a discovery controller.

### nvmf

Added support for zero-copy operations in the NVMe-oF TCP target. It can be enabled via
the `zcopy` parameter when creating a transport. The zero-copy operations are only used
for requests not using in-capsule data.

Added a `subsystem` parameter to `spdk_nvmf_transport_stop_listen_async`. When not NULL,
it will only disconnect qpairs for controllers associated with the specified subsystem.

Removed accept poller from transport layer. Each transport can have its own policy of
handling new connections. To notify transport layer `spdk_nvmf_poll_group_add` and
`spdk_nvmf_tgt_new_qpair` can be used.

### scsi

Structure `spdk_scsi_lun` has been extended with new member `resizing` so that SCSI layer now reports
unit attention for disk resize.

### trace

Added `spdk_trace_create_tpoint_group_mask` to return tracepoint group mask from
a tracepoint group name.

Added `trace_set_tpoint_mask` and `trace_clear_tpoint_mask` RPC to allow enabling
individual traces.

### util

Added `spdk_ioviter_first` and `spdk_ioviter_next` to iterate over two iovecs and
yield pointers to common length segments.

## v21.10

Structure `spdk_nvmf_target_opts` has been extended with new member `discovery_filter` which allows to specify
filtering rules applied during discovery log generation. Refer to `enum spdk_nvmf_tgt_discovery_filter` for more info.

### bdev

New API `spdk_bdev_get_memory_domains` has been added, it allows to get SPDK memory domains used by bdev.

New API functions `spdk_bdev_readv_blocks_ext` and `spdk_bdev_writev_blocks_ext` have been added.
These functions accept `spdk_bdev_ext_io_opts` structure with extended IO request
options, e.g. DMA memory domain which describes data that may belong to another memory domain and
can't be accessed directly.

Added `async_fini_start` to allow bdev modules to complete the `fini_start` asynchronously,
with new `spdk_bdev_module_fini_start_done` API.

Deprecated `spdk_bdev_module_finish_done()` API, which will be removed in SPDK 22.01.
Bdev modules should use `spdk_bdev_module_fini_done()` instead.

The `ocssd` bdev has been removed. The Open Channel specification has been largely superseded by
zoned namespaces, few if any devices have been brought to market, and there is little reason to
continue to support this. OCSSD support in the nvme driver will remain for now.

### dma

A new library, lib/dma, has been added. This library provides the necessary infrastructure for
handling systems and devices with multiple memory domains. For example, a PCIe add-in card with an
SoC may be running SPDK on the SoC. That SoC has its own local memory, but SPDK may be controlling
devices that can also access the host system memory. This library provides infrastructure to enumerate
the memory domains and request hardware perform DMA transfers between them.

### dpdk

Updated DPDK submodule to DPDK 21.08.

### event

Added the `disable_signal_handlers` flag to the `spdk_app_opts` struct.

### idxd

Added `spdk_idxd_get_socket` to query the socket that the idxd device
is on.

### json

Added API to allow for writing and decoding of new types:

- `spdk_json_write_named_uint16`
- `spdk_json_write_named_uint8`
- `spdk_json_write_uint16`
- `spdk_json_write_uint8`
- `spdk_json_number_to_uint8`
- `spdk_json_decode_uint8`

### log

Added API `spdk_log_to_syslog_level` to return syslog level based on SPDK's
log level.

### nvme

Added new functions `spdk_nvme_zns_set_zone_desc_ext` and `spdk_nvme_zns_ext_report_zones`
to set zone descriptor extension and to get extended zone report respectively.

New API `spdk_nvme_ctrlr_get_memory_domains` has been added, it allows to get SPDK memory domains used by nvme controller.

New API functions `spdk_nvme_ns_cmd_readv_ext` and `spdk_nvme_ns_cmd_writev_ext`
have been added. These functions accept `spdk_nvme_ns_cmd_ext_io_opts` structure with extended IO request
options, e.g. DMA memory domain which describes data that may belong to another memory domain and
can't be accessed directly.

Added a new function `spdk_nvme_ctrlr_get_regs_bpinfo` to get boot partition info of a controller.
Added new functions `spdk_nvme_ctrlr_write_boot_partition`,
`spdk_nvme_ctrlr_read_boot_partition_start` and `spdk_nvme_ctrlr_read_boot_partition_poll`
to write and read the boot partitions of a controller.

Added `spdk_nvme_ctrlr_get_opts` to retrieve the current controller options.

Added `async_mode` to `spdk_nvme_io_qpair_opts` to enable creation of submission and completion
queues asynchronously. This mode is currently supported at PCIe layer,
which tracks the qpair creation with state machine and returns to the user immediately.
Default mode is set to false to create io qpairs synchronously.

Added `spdk_nvme_ctrlr_get_regs_cc` to retrieve NVMe controller CC (Configuration) register.

Added `spdk_nvme_ctrlr_prepare_for_reset` to inform the driver that the application is preparing
to reset the specified NVMe controller. This function allows the driver to make decisions
knowing that a reset is about to happen.

### nvmf

Added `oncs` to `struct spdk_nvmf_ctrlr_data` so that the transport layer
can decide support RESERVATION feature or not.

An `opts_size` element was added in the `spdk_nvmf_ns_opts` structure to solve the
ABI compatibility issue between different SPDK version. An new option `anagrpid` was
added in the `spdk_nvmf_ns_opts` structure.

An new parameter `anagrpid` was added to the RPC `nvmf_subsystem_add_ns`.

An new parameter `anagrpid` was added to the RPC `nvmf_subsystem_listener_set_ana_state`.

`spdk_nvmf_subsystem_destroy` is now can be asynchronous, it accepts a callback and callback argument.

A new parameter, `poll_groups_mask` was added to the `nvmf_set_config` RPC that allows specifying
a subset of cores for the nvmf poll groups. This helps to avoid imbalances when some cores are
busy with periodic timer tasks that run very frequently.

A new parameter, `commit` was added to `nvmf_bdev_ctrlr_end_zcopy` that allows indicating whether
the buffers should be committed.

Added `listen_dump_opts` to `spdk_nvmf_transport_ops` so that the transport layer can display
listen opts.

Added `fabrics_connect_timeout_us` to `bdev_nvme_attach_controller` RPC to allow specifying
timeout for connect operation.

Added `multipath` to `bdev_nvme_attach_controller` RPC to allow specifying multipath behavior.

### scheduler

New API for implementing schedulers and governors. Please see `include/spdk/scheduler.h` for details.

### scsi

New functions, `spdk_scsi_dev_get_first_lun` and `spdk_scsi_dev_get_next_lun`
have been added to iterate LUNs of a SCSI device.

Each SCSI device supports 256 LUNs at the maximum now and the macro constant
`SPDK_SCSI_DEV_MAX_LUN` was removed.

### trace_parser

A new library, lib/trace_parser, has been added. This library provides functions that parse traces
recorded by an SPDK application. That includes merging traces from multiple cores,
sorting them by their timestamp and constructing trace entries spanning across multiple buffers.

### util

The `spdk_fd_group_add` API now takes a `name` parameter.

## v21.07

### accel_fw

Added API `spdk_accel_submit_copy_crc32c` to perform a CRC32C while copying data.

Added API `spdk_accel_batch_prep_copy_crc32c` to batch CRC32C + copy commands.

Added API `spdk_accel_submit_copy_crc32cv` to submit chained CRC32C + copy commands.

### bdev

Change `spdk_bdev_read_blocks_with_md` arg offset definition from int64_t to uint64_t.

Red-black tree has been used to organize the bdev names and aliases uniformly
to provide faster lookup.

Removed ZCOPY emulation: The bdev module can be checked to see if it supports ZCOPY
and if not supported then use existing READ/WRITE commands.

Added iov to spdk_bdev_zcopy_start.

### dpdk

Updated DPDK submodule to DPDK 21.05.

### idxd

Remove the `probe_cb` parameter in `spdk_idxd_probe` function. And remove the definition
of `spdk_idxd_probe_cb` function pointer. It should be implemented in idxd_user.c.

Added API `spdk_idxd_submit_copy_crc32c` to perform a CRC32C while copying data.

Added API `spdk_idxd_batch_prep_copy_crc32c` to prepare a batch operation to perform
a CRC32C while copying data.

We have userspace idxd driver under accel_engine library (module/accel/idxd/accel_engine).
And kernel idxd driver usage is also added under the accel_engine library. This approach
implemented a wrapper library to use IDXD device by leveraging the kernel DSA driver in
SPDK idxd library (lib/idxd). Then users can leverage the RPC later to configure how to
use the DSA device by user space driver or kernel driver.

### init

Added new `init` library that initializes the SPDK subsystems, which previously was
internal to application framework. That functionality has been made public and for
use by applications which don't leverage SPDK's application framework.

### iscsi

New parameters, `pdu_pool_size`, `immediate_data_pool_size`, and `data_out_pool_size`,
were added to the RPC `iscsi_set_options` to run iSCSI target with varying amount of
available memory.

### json

Added API `spdk_json_write_named_uint128` and `spdk_json_write_uint128` to perform
the uint128 related data.

### net

Removed deprecated `net` library.

Added support for ZCOPY.  ZCOPY is used in preference to READ and WRITE if the
bdev module supports ZCOPY and the bdev module has ZCOPY enabled.

### nvme

`spdk_nvme_map_prps` and `spdk_nvme_map_cmd` were moved to nvmf/vfio-user as internal APIs
as vfio-user is the only user for the above two APIs.

Added a new function `spdk_nvme_ns_cmd_copy` to submit a Simple Copy Command to a Namespace.

Update the `spdk_nvme_generic_command_status_code` structure with new status code
according to the definition in NVMe 1.4 spec.

`spdk_nvme_ctrlr_get_default_ctrlr_opts` now sets `use_cmb_sqs` to false. This means
that if a controller has a CMB and supports SQs in the CMB, SPDK will not use
the CMB for SQs by default - the user must set `use_cmb_sqs` to true in
the `spdk_nvme_ctrlr_opts` structure prior to controller attach.

Add a new function `spdk_nvme_detach_poll` to simplify a common use case to continue
polling until all detachments complete.

Added new argument `timeout_admin_us` to `spdk_nvme_ctrlr_register_timeout_callback` so callers
can specify a different timeout for admin commands vs. io commands.

An existing function `spdk_nvme_detach_async` was updated to add one or more detachments
to an active context while it is being polled.

Rename a variable in the member `cmic` of the struct `spdk_nvme_ctrlr_data` from
`multi_host` to `multi_ctrlr`. The variable means that the NVM subsystem may have two or
more controllers if set to 1. However `multi_host` had indicated a particular use case
such that the NVM subsystem is used by multiple hosts.

A new option `disable_read_ana_log_page` was added to struct `spdk_nvme_ctrlr_opts` to disable
reading ANA log page. The upper layer is expected to read ANA log page instead if `true`.
The default value is `false`.

New APIs, `spdk_nvme_ctrlr_reset_async` and `spdk_nvme_ctrlr_reset_poll_async`, have been added to
reset a controller asynchronously.

New RPC `bdev_nvme_reset_controller` was added, to reset an NVMe controller.

Added `spdk_nvme_ns_get_nguid` function to get NGUID for the given namespace.

Added `spdk_nvme_ctrlr_is_fabrics` function to indicate whether a ctrlr handle
is associated with a fabrics controller.

### nvmf

Added `min_cntlid` and `max_cntlid` to `nvmf_create_subsystem` to limit the controller ID range.
Added `spdk_nvmf_subsystem_get_min_cntlid` and `spdk_nvmf_subsystem_get_max_cntlid` to request those values.

`spdk_nvmf_request_get_buffers_multi` API is removed.

Added the `nvmf_set_crdt` RPC for setting command retry delay times.

Expanded `spdk_nvmf_poll_group_stat` with current qpair count statistics.

Removed following deprecated APIs:

- `spdk_nvmf_poll_group_get_stat` (function in `nvmf.h`),
- `spdk_nvmf_transport_poll_group_get_stat` (function in `nvmf.h`),
- `spdk_nvmf_transport_poll_group_free_stat`(function in `nvmf.h`),
- `spdk_nvmf_rdma_device_stat` (struct in `nvmf.h`),
- `spdk_nvmf_transport_poll_group_stat` (struct in `nvmf.h`),
- `poll_group_get_stat` (transport op in `nvmf_transport.h`),
- `poll_group_free_stat` (transport op in `nvmf_transport.h`).

### rpc

New RPC `bdev_rbd_register_cluster` and `bdev_rbd_unregister_cluster` was added, it allows to create
and delete the rados object cluster, then users can choose the cluster to create related rbd
device.

Revised `bdev_rbd_create` parameter, it allows to use an optional parameter `--cluster-name`
to create a rbd bdev with  an already registered Rados Cluster Object.

New RPC `bdev_rbd_get_clusters_info` was added, it allows to get the info of the registered
Rados Cluster names.

New optional parameter, `timeout_admin_us`, added to the bdev_nvme_set_options RPC.

Revised a parameter `--stripe-size_kb` to `--stripe-size-kb` of `bdev_raid_create` method
provided in `scripts/rpc.py` for consistency.

An new optional parameter `config_kernel_mode` was added to the RPC `idxd_scan_accel_engine`,
and this is used to enable using the kernel mode IDXD driver.

Removed deprecated parameter `enable-zerocopy-send` of RPC `sock_impl_set_options`,
use `enable-zerocopy-send-server` or `enable-zerocopy-send-client` instead.
Removed deprecated parameter `disable-zerocopy-send` of RPC `sock_impl_set_options`,
use `disable-zerocopy-send-server` or `disable-zerocopy-send-client` instead.

### rpm

Removed deprecated `pkg/spdk.spec`, please use `rpmbuild/spdk.spec` instead.
See [RPM documentation](https://spdk.io/doc/rpm.html) for more details.

### thread

Red-black tree has been used for timed pollers to provide faster insertion and deletion
and for io_devices to provide faster lookup.

Removed `spdk_io_channel` structure from public header and moved it to thread_internal.h.

### trace

Added `spdk_trace_register_description_ext` function to register variable number of tracepoint arguments.

Added ability to chain multiple trace entries together to extend the size of the argument buffer.

### util

Red-black tree macros has been added by using the macros provided by the FreeBSD operating system
under the same BSD license.

Add an new macro `SPDK_SIZEOF_MEMBER` to get the size of a member of a struct.

`spdk_crc32c_iov_update` function was added to support calculating the crc32c of the iovs.

Added zipf random number generator with power law probability distribution.
When applied to performance testing of block devices, it will select blocks over
the full range of LBAs, but will more frequently select lower-numbered LBAs.

## v21.04

### accel

Two new accelerated crc32 functions `spdk_accel_submit_crc32cv` and
`spdk_accel_batch_prep_crc32cv` are added in order to provide the
chained accelerated CRC32 computation support.

### bdev

For `bdev_ocssd_create` RPC, the optional parameter `range` was removed.
Only one OCSSD bdev can be created for one OCSSD namespace.

Removed the `spdk_bdev_open` from bdev library API.
Removed the `spdk_vbdev_register` and `spdk_bdev_part_base_construct` from bdev module API.
Removed the `config_text` function for bdev modules to report legacy config.

Added `spdk_bdev_get_max_active_zones` API to display maximum number active zones of a given bdev.

Added `spdk_bdev_get_max_zone_append_size` API to display maximum zone append data transfer size.

### bdev_nvme

Added support for zoned namespaces.

### blobstore

Removed the `spdk_bdev_create_bs_dev_from_desc` and `spdk_bdev_create_bs_dev` API.

### env

Added `spdk_pci_device_allow` API to allow applications to add PCI addresses to
the allowed list after the application has started.

Removed the `pci_whitelist`, `pci_blacklist` and `master_core` members of struct `spdk_env_opts`.

Added hotplug support based on uevent in `pci_event.c`. Added usage of this functionality in
nvme, virtio-scsi and virtio_blk libraries. Please see the new API `spdk_pci_event_listen`,
`spdk_pci_get_event`, `spdk_pci_register_error_handler` and `spdk_pci_unregister_error_handler`.

### event

Removed the `config_file`, `max_delay_us`, `pci_whitelist`
and `pci_blacklist` members of struct `spdk_app_opts`.

### idxd

A new API `spdk_idxd_device_needs_rebalance` was added so that users of the library
can know whether they need to rebalance the flow control for the channel
that was just added/removed.  This is based on how the low level library
shares devices amongst channels.

The API `spdk_idxd_reconfigure_chan` had the `num_channels` removed as this
is now tracked in the library.  The app makes use the new API above to
determine whether to rebalance or not. This applies to `spdk_idxd_configure_chan`
as well.

The API `spdk_idxd_put_channel` now returns the rebalance state for the
underlying device.

### iscsi

A security vulnerability has been identified and fixed in the SPDK iSCSI target.
A TEXT PDU with no data, but CONTINUE flag set, would result in a NULL pointer dereference
and crash the SPDK iSCSI target process. All users of the SPDK iSCSI target
are recommended to update. All SPDK versions <= v21.01 are affected.

### net

The net library is deprecated and will be removed in the 21.07 release.

### nvme

Added a new function `spdk_nvme_ctrlr_get_regs_pmrcap` to get the PMR capabilities.

Added an accelerated table pointer in `spdk_nvme_poll_group`
which can be used to provide the accelerated functions by users with
hardware engine, such as crc32c accelerated function.

Added new functions `spdk_nvme_ctrlr_get_pmrsz`, `spdk_nvme_ctrlr_enable_pmr`,
`spdk_nvme_ctrlr_disable_pmr`, `spdk_nvme_ctrlr_map_pmr` and `spdk_nvme_ctrlr_unmap_pmr`.

Added NVMe transport operations to enable, disable, map and unmap the PMR.

Added `spdk_nvme_qpair_get_optimal_poll_group` function and `qpair_get_optimal_poll_group`
function pointer in spdk_nvmf_transport_ops structure in order to add the qpair to the most
suitable polling group.

Added OPTPERF and namespace optimal performance fields to nvme_spec.h.

Added `spdk_nvme_set_hotplug_filter` API to allow applications to choose which
hot-inserted SSDs should be probed. This is useful for use cases where multiple
independent SPDK processes are running on one node. The filter function can
then be implemented in these processes to decide which SSDs to probe based on
the new SSD's PCI address.

New functions `spdk_nvme_poll_group_get_stats` and `spdk_nvme_poll_group_free_stats`
were added. These functions allow to get transport statistics per NVME poll group.

Added `spdk_nvme_map_cmd` API to map the NVMe command with SGL cases.

Added support for vector variant of ZNS zone append commands with new API
`spdk_nvme_zns_zone_appendv` and `spdk_nvme_zns_zone_appendv_with_md`.

Added `spdk_nvme_zns_ns_get_max_open_zones` and `spdk_nvme_zns_ns_get_max_active_zones` API,
to display maximum number of open and active zones of the given namespace.

Added `spdk_nvme_zns_ns_get_zone_size_sectors` API to provide size of zone in number of
sectors.

Added `spdk_nvme_qpair_get_id` API to display the ID of the specified qpair.

### nvmf

Removed the `spdk_nvmf_tgt_listen` and `spdk_nvmf_subsystem_add_ns` API.

Added new APIs:

- `spdk_nvmf_poll_group_dump_stat` (function in `nvmf.h`).
- `poll_group_dump_stat` (transport op in `nvmf_transport.h`).

The following APIs have been deprecated and will be removed in SPDK 21.07:

- `spdk_nvmf_poll_group_get_stat` (function in `nvmf.h`),
- `spdk_nvmf_transport_poll_group_get_stat` (function in `nvmf.h`),
- `spdk_nvmf_transport_poll_group_free_stat`(function in `nvmf.h`),
- `spdk_nvmf_rdma_device_stat` (struct in `nvmf.h`),
- `spdk_nvmf_transport_poll_group_stat` (struct in `nvmf.h`),
- `poll_group_get_stat` (transport op in `nvmf_transport.h`),
- `poll_group_free_stat` (transport op in `nvmf_transport.h`).

See header files for details.

The `trtype` field in JSON returned by `nvmf_get_stats` RPC contains now the name of the transport,
which is the same as the type for defined transports and more informative for a custom transport.

Added `hdgst` and `ddgst` parameters to `bdev_nvme_attach_controller` RPC in order change
state of TCP header and data digest.

Added `num_cqe` parameter to `nvmf_create_transport` RPC to set number of completion queues (CQ)
for RDMA transport. Useful when CQ resize operation is not supported, for example iWARP.

### ocf

Updated OCF submodule to v20.12.2

Added `bdev_ocf_set_cache_mode` RPC to dynamically switch cache mode of OCF bdev.

### opal

Removed the `spdk_opal_supported` API.

### raid

For `bdev_raid_create` RPC, the deprecated parameter `strip_size` was removed.

### rpc

New RPC `bdev_nvme_get_transport_statistics` was added, it allows to get transport statistics
of nvme poll groups.

Parameter `enable-zerocopy-send` of RPC `sock_impl_set_options` is deprecated and will be removed in SPDK 21.07,
use `enable-zerocopy-send-server` or `enable-zerocopy-send-client` instead.
Parameter `disable-zerocopy-send` of RPC `sock_impl_set_options` is deprecated and will be removed in SPDK 21.07,
use `disable-zerocopy-send-server` or `disable-zerocopy-send-client` instead.

Added cmd_parser.py used to parse the args from argparse. There are
two benefit to use command parser:

- Simplify the definition of rpc method. It will reduce the rpc method code.
- Make the rpc call more versatile. User can add private args into rpc method.

### rpm

Added support for new RPM spec, rpmbuild/spdk.spec, which can be used for packaging the
SPDK. The pkg/spdk.spec is considered to be deprecated and scheduled for removal in SPDK 21.07.
See [RPM documentation](https://spdk.io/doc/rpm.html) for more details.

### sock

The type of `enable_placement_id` in struct `spdk_sock_impl_opts` is changed from
bool to int. We can use RPC to configure different value of `enable_placement_id`.
Then we can leverage SO_INCOMING_CPU to get placement_id, which aims to utilize
CPU cache locality, enabled by setting enable_placement_id=2.

A new socket placement mode called PLACEMENT_MARK has been added. Some NICs allow
sockets to be marked using the SO_MARK socket option as a hint for which hardware
queue they should be associated with. This mode leverages that by setting the same
value for all sockets within a poll group.

New parameters `enable_zerocopy_send_server` and `enable_zerocopy_send_client` were added
to struct spdk_sock_impl_opts, these parameters enable or disable zero copy send for server
and client sockets which are created using `spdk_sock_listen` and `spdk_sock_listen_ext` (server);
`spdk_sock_connect` and `spdk_sock_connect_ext` (client) functions. Existing parameter
`enable_zerocopy_send`  enables or disables zero copy send for both server and client sockets.

### thread

A new API `spdk_io_channel_get_io_device` was added to get the io_device for the specified
I/O channel.

Added `spdk_thread_set_interrupt_mode` function in order to set present spdk_thread into
interrupt mode or back to poll mode. It is valid only when thread interrupt facility is
enabled by `spdk_interrupt_mode_enable`.

Added `spdk_poller_register_interrupt` function to mark that the poller is capable of
entering interrupt mode. Callback function will be called when the poller must transition
into or out of interrupt mode.

### virtio

Added the `bdev_virtio_blk_set_hotplug` RPC for the virtio blk pci device.

## v21.01

### bdev

An `opts_size` element was added in the `spdk_bdev_opts` structure to solve the
ABI compatibility issue between different SPDK version. And also add `opts_size`
parameter in spdk_bdev_get_opts function. Two fields `small_buf_pool_size` and
`large_buf_pool_size` were added into spdk_bdev_opts, which were used to determine
the small and large buffer pool size of the whole bdev module.

A new API `spdk_bdev_wait_for_examine` was added to allow for checking state of
examine process. Along with corresponding `bdev_wait_for_examine` RPC, which
is now always called during `spdk_bdev_subsystem_config_json` making sure
every bdev is ready to be used.

A new API `spdk_bdev_io_get_aio_status` was added for getting the status of
bdev_io as Linux AIO errno. Also `spdk_bdev_io_complete_aio_status` function
and `SPDK_BDEV_IO_STATUS_AIO_ERROR` were added for bdev module to complete
a bdev_io with Linux AIO errno.

A new API `spdk_bdev_get_module_name` was added for getting block device module name.
Also `spdk_bdev_get_module_ctx` was added to obtain a bdev module context for
the block device opened by the specified descriptor.

Added `max_segment_size` and `max_num_segments` to `spdk_bdev` structure for
bdev modules to specify splitting requirements.

### blob

An `opts_size` element was added in the `spdk_bs_opts` structure to solve the
ABI compatibility issue between different SPDK version. And also add `opts_size`
parameter in `spdk_bs_opts_init` function.

An `opts_size` element was added in the `spdk_blob_opts` structure to solve the
ABI compatibility issue between different SPDK version. And also add `opts_size`
parameter in `spdk_blob_opts_init` function.

An `opts_size` element was added in the `spdk_blob_open_opts` structure to solve the
ABI compatibility issue between different SPDK version. And also add `opts_size`
parameter in `spdk_blob_open_opts_init` function.

### build

SPDK now generates pkg-config files to simplify the process of determining which
libraries must be linked into an SPDK application.

### dpdk

Updated DPDK submodule to DPDK 20.11.

Removed `--with-igb-uio-driver` configure option. Since DPDK 20.11 igb_uio driver
was moved to separate dpdk-kmods repository. If required, users need to refer to
dpdk-kmods repository to build the driver.

### env

The `pci_whitelist`, `pci_blacklist` and `master_core` members of struct `spdk_env_opts`
have been deprecated.  The new members are named `pci_allowed`, `pci_blocked` and
`main_core` respectively.

An `opts_size` element was added in the `spdk_app_opts` structure
to solve the ABI compatibility issue between different SPDK version. An `opts_size`
parameter is added into `spdk_app_opts_init` function.

### event

The `pci_whitelist` and `pci_blacklist` members of struct `spdk_app_opts` have been
deprecated.  The new members are named `pci_allowed` and `pci_blocked` respectively.

The `--pci-blacklist` command line option has been deprecated, replaced with
`--pci-blocked`.

The `--pci-whitelist/-W` command line options have been deprecated, replaced with
`--pci-allowed/-A`.

Added new experimental `dynamic` scheduler that rebalances idle threads, adjusts CPU frequency
using dpdk_governor and turns idle reactor cores to interrupt mode. Please see
[scheduler documentation](https://www.spdk.io/doc/scheduler.html) for details.

## ioat

The PCI BDF whitelist option has been removed from the `ioat_scan_accel_engine` RPC.
IOAT PCI functions can still be allowed or denied using SPDK application command
line options.

### isa-l

Updated ISA-L submodule to v2.30.0.

### json

A new API `spdk_jsonrpc_send_bool_response` was added to allow sending response for
writing json bool results into one function.
Update API `bdev_nvme_set_options` and add a `keep_alive_timeout_ms` parameter. Now you
can specify the `keep_alive_timeout` before creating NVMe bdev.

### nbd

Change the return type of function `spdk_nbd_stop` from void to int. And update the
`spdk_nbd_fini` with two parameters to make its behavior from sync to async.

### nvme

Directives support was added to the NVMe driver.

Two async APIs `spdk_nvme_ctrlr_cmd_directive_receive` and `spdk_nvme_ctrlr_cmd_directive_send`
are added for Directive Send and Directive Receive command, respectively.

Added a new function `spdk_nvme_ctrlr_reset_subsystem` to perform a NVMe
subsystem reset. Note: The NVMe-oF target does not support the subsystem reset yet.

Add a new function `spdk_nvme_bytes_to_numd` to transfer bytes to number of
dwords.

Added a new custom transport `SPDK_NVME_TRANSPORT_VFIOUSER` to enable NVMe
driver running with NVMe over vfio-user target.

Added the vfio-user custom transport implementation in NVMe driver which can connect
to NVMe over vfio-user target via vfio-user transport.

Added a new function `spdk_nvme_ctrlr_set_remove_cb` to remove or override
`remove_cb` and `remove_ctx` specified when the controller was probed.

Added support for ZNS zone append command with new API `spdk_nvme_zns_zone_append` and
`spdk_nvme_zns_zone_append_with_md`.

### nvmf

`nvmf_fc_lld_fini` now takes callback and hence updating FC Broadcom LLD driver
to the latest is required.

FC transport supported primary tagging and VMID.

Broadcom FC LLD driver and SPDK NVMe-oF FC transport consolidated one LLD API,
`nvmf_fc_init_poller_queues` into another LLD API `nvmf_fc_init_q`.
Hence updating Broadcom FC LLD driver to the latest is required.

The functions `destroy` and `qpair_fini` in the transport interface now accept a
`cb_fn` and `cb_arg` to call upon completion, and their execution can be asynchronous.

The SPDK NVMe-oF target now supports async event notification for discovery log changes.
This allows the initiator to create persistent connection to discovery controller and
be notified of any discovery log changes.

An `opts_size` element was added in the `spdk_nvmf_transport_opts` structure
to solve the ABI compatibility issue between different SPDK version. And also add
`opts_size` parameter in `spdk_nvmf_transport_opts_init` function.

Added a new custom vfio-user transport implementation in NVMe-oF which can provide
emulated NVMe devices to QEMU and SPDK NVMe driver.

Added new API `spdk_nvmf_tgt_listen_ext` that uses `spdk_nvmf_listen_opts` to allow
passing generic options to specific transport. This functionality replaces one provided
with `nvmf_subsystem_set_options`. Also removed `nvmf_subsystem_set_options` RPC
that allowed transport specific options within subsystem.

The `trsvcid` in `nvmf_subsystem_add_listener` RPC is now optional.

Pausing a subsystem now only pauses all admin queues. This allows for the
management of hosts and listeners, plus the addition of namespaces without a
full subsystem pause. Additionally, the target now allows for pausing
individual namespaces within a subsystem. To remove a namespace from a
subsystem, only the specific namespace must be paused. I/O will continue to
other namespaces while these operations execute.

### rpc

An new optional parameter `wait` was added to the RPC `iscsi_create_portal_group`,
and an new RPC `iscsi_start_portal_group` was added. They will be used not to start
listening on portals for a portal group until all associated target nodes are created
at startup, otherwise some iSCSI initiators may fail to re-login when SPDK iSCSI
target application restarts.

Two optional parameter `--small-buf-pool-size` and `--large-buf-pool-size` were added
into `bdev_set_options` function.

### vhost

Added optional `packed_ring_recovery` parameter to `vhost_create_blk_controller` RPC
enabling packed ring live recovery.

## v20.10

### accel

New APIs `spdk_accel_batch_cancel` and `spdk_idxd_batch_cancel` were added to allow
cancelling a batch sequence.

### bdev

A new `spdk_bdev_part_base_construct_ext` function has been added and the
`spdk_bdev_part_base_construct` has been deprecated.  The
`spdk_bdev_part_base_construct_ext` function takes bdev name as an argument instead
of bdev structure to avoid a race condition that can happen when the bdev is being
removed between a call to get its structure based on a name and actually opening it.

Removed `spdk_bdev_config_text` function for bdev modules to report legacy config.

### blobstore

A new `spdk_bdev_create_bs_dev_ext` function has been added and `spdk_bdev_create_bs_dev_from_desc`
function has been deprecated.

### dpdk

Updated DPDK submodule to DPDK 20.08.

### env

A new API `spdk_mem_get_fd_and_offset` was added to get file descriptor and offset for
a given virtual address.

### event

Removed `spdk_subsystem_config` callback for submodules as part of legacy config removal.

Removed `spdk_app_get_running_config` function that printed configuration in legacy format,
and removed `usr1_handler` from `struct spdk_app_opts` callback that was used to call it.

Added SPDK thread scheduler framework which is used to rebalance load among CPU cores.
The scheduler implementation is pluggable and two schedulers are added first, `static` scheduler
and `gscheduler` scheduler.

`static` scheduler is used by default and does not reschedule threads and keeps the initial
assignments.

CPU frequency governor framework and implementation are also added.
`dpdk_governor` is the first implementation and is based on rte_power library of DPDK.
`gscheduler` scheduler utilizes the CPU frequency governor.

Scheduler and governor frameworks are experimental features.

### fio

Bdev fio_plugin no longer supports legacy configuration files. Options `spdk_conf` and
`spdk_json_conf` have to point to valid JSON configuration.

### gpt

Removed option to disable GPT probing via legacy configuration. Please use generic
bdev functionality to replicate the behavior. See `bdev_set_options --disable-auto-examine`
and `bdev_examine` RPC.

### intel-ipsec-mb

Updated intel-ipsec-mb submodule to v0.54

### isa-l

Updated ISA-L submodule to v2.29.0.

### iscsi

The iSCSI target supports iSCSI login redirection feature to achieve fault tolerance
and load balancing particularly in scale out iSCSI target system, which runs multiple
SPDK iSCSI targets.

### json

A new API `spdk_json_decode_object_relaxed` was added to allow parsing json values
even if decoder for a given key is not found.

A new API `spdk_json_free_object` was added to free memory allocated by `spdk_json_decode_object`.

### log

A log flag structure, `spdk_log_register_flag`, `spdk_log_get_first_flag`,
`spdk_log_get_next_flag` and macro `SPDK_LOG_REGISTER_COMPONENT`, have been added which
enable registering and interacting with log flags. Added `SPDK_INFOLOG`, `SPDK_DEBUGLOG`
and `SPDK_LOGDUMP` macros to display logs for specified log flag.

The log flags in the event framework `-L` and several SPDK applications has been enabled on
release builds of SPDK. On debug builds this option will additionally set
log print level to `SPDK_LOG_DEBUG`.

A new API `spdk_log_enable_timestamps` and new RPC `log_enable_timestamps` was added to
toggle log timestamps.

### miscellaneous

The contents of the log_rpc library have been moved to the event library. The log_rpc
library now no longer exists.

The contents of the app_rpc library have been moved to the event library. The app_rpc
library now no longer exists.

The contents of the bdev_rpc library have been moved to the bdev library. The app_rpc
library now no longer exists.

The bdevperf application now disables the zcopy API by default. Prior to this change,
bdevperf enabled using the zcopy API by default which caused a performance impact of
up to 25% on bdevs that don't natively support zcopy because the API emulates zero-copy
by allocating a buffer. The bdevperf `-x` param was renamed to `-Z` and the default
value changed to false. For bdevs that support zcopy, use the -Z flag to enable
using zcopy API.

### net

Net framework intended for use by user-space TCP stacks was removed.

### nvme

Zoned Namespace Command Set support has been added to NVMe driver, see `nvme_zns.h` for details.

New APIs, `spdk_nvme_detach_async` and `spdk_nvme_detach_poll_async`, have been added to
detach multiple controllers in parallel to mitigate lengthy shutdown notification of
a few NVMe SSDs.

New APIs, `spdk_nvme_ns_get_ana_group_id` and `spdk_nvme_ns_get_ana_state`, have been
added to get ANA group ID and ANA state of the given namespace, respectively.

If no specific command set is requested (by setting the command_set member in the
`spdk_nvme_ctrlr_opts` structure), SPDK will automatically select the most appropriate
command set based on what the controller supports.

### nvmf

A new API `spdk_nvmf_subsystem_add_ns_ext` has been added and the API `spdk_nvmf_subsystem_add_ns`
has been deprecated.

The NVMe-oF target now supports Asymmetric Namespace Access (ANA) Reporting to provide
multipath to NVMe-oF initiator.

Add `no_wr_batching` parameter to `spdk_nvmf_transport_opts` struct to disable
Work Requests batching in RDMA transport.

NVMf Target transports can now parse any additional JSON params in the nvmf_create_transport RPC
through the JSON context provided via spdk_nvmf_target_opts->transport_specific.

A new RPC `nvmf_subsystem_set_options` was added to allow transport specific options within subsystem.

A new API `spdk_nvmf_transport_stop_listen_async` was added to stop accepting new connections
at the provided address. `nvmf_subsystem_remove_listener` RPC now uses this function.

A new API `spdk_nvmf_subsystem_disconnect_host` was added to disconnect all connections
originating from the provided hostnqn. `nvmf_subsystem_remove_host` RPC now uses this function.

### ocf

Updated OCF submodule to v20.03.1

### rpc

New optional parameters, `enable_placement_id` and `enable_quickack` were added to the
`sock_impl_set_options` RPC.

A new RPC `bdev_examine_bdev` was added to allow users to examine a bdev explicitly.
It can be used only if bdev_auto_examine is set to false by the RPC `bdev_set_options`.

New optional parameters `no_wr_batching` and `control_msg_num` were added to the RPC
'nvmf_create_transport'.

New RPCs, `iscsi_target_node_set_redirect` and `iscsi_target_node_request_logout`, have
been added, and a new optional parameter `private` has been added to the RPC
`iscsi_create_portal_group` to support iSCSI login redirection feature.

A new optional parameter `ana_reporting` was added to the RPC `nvmf_create_subsystem`, and
a new RPC `nvmf_subsystem_listener_set_ana_state` was added for ANA reporting.

New RPCs, `nvmf_subsystem_get_listeners` and `nvmf_subsystem_get_qpairs`, were added to
retrieve configuration of the NVMe-oF subsystem.

### scsi

Two new APIs have been added `spdk_scsi_dev_construct_ext` and
`spdk_scsi_dev_add_lun_ext` that allow the upper layer(e.g. vhost-scsi) to
receive the notification when the scsi bdev has been resized.

The `spdk_scsi_dev_construct` and `spdk_scsi_dev_add_lun` eventually may be
deprecated and removed.

### sock

The `enable_placement_id` field was added in the `struct spdk_sock_impl_opts` to
make the placement_id feature configurable by users. The default setting is
not enabled.

The `enable_quick_ack` field was added in the `struct spdk_sock_impl_opts` to enable
or disable quick ack for the POSIX sock module. The default setting is not enabled.

### thread

`fd_group` is applied to support interrupt mode.
New APIs were added to support an experimental interrupt mode.  This allows modules or
libraries to selectively register file descriptors that the spdk_thread can wait on,
as an alternative to polling. In v20.10, this functionality is enabled in a very small
subset of SPDK libraries and modules.

### util

A new utility named `fd_group` was add. It is now
implemented by epoll on Linux platform. It can be used by
spdk_thread and reactor to implement interrupt mode.

Added `spdk_bit_pool` utility, see `bit_pool.h` for details.

### vhost

SPDK has switched to DPDK's rte_vhost library since 19.07 release, removed the internal
rte_vhost library which is used for DPDK older than 19.05, removed the experimental vhost
nvme target which depends on the internal rte_vhost library.

### vpp

Removed socket abstraction for VPP.

## v20.07: SPDK CSI driver, new accel_fw commands, I/O abort support

spdk_json_free_object() was added to free memory allocated by spdk_json_decode_object().

### accel

A new API was added `spdk_accel_get_capabilities` that allows applications to
query the capabilities of the currently enabled accel engine back-end.

A new capability, CRC-32C, was added via `spdk_accel_submit_crc32c`.

The software accel engine implementation has added support for CRC-32C.

A new capability, compare, was added via `spdk_accel_submit_compare`.

The software accel engine implementation has added support for compare.

Several APIs were added to `accel_engine.h` to support batched submission
of operations.

Several APIs were added to `accel_engine.h` to support dualcast operations.

### accel_fw

The accel_fw was updated to no longer require the app to allocate an
accel_task on its behalf. All public APIs now take a callback arg as
the parameter that used to be the accel_task.

The accel_fw API `spdk_accel_task_size` was removed as it is no longer
required.

The accel_fw was updated to support compare, dualcast, crc32c.

The accel_fw introduced batching support for all commands in all plug-ins.
See docs for detailed information.

### bdev

A new API `spdk_bdev_abort` has been added to submit abort requests to abort all I/Os
whose callback context match to the bdev on the given channel.

### build

The fio plugins now compile to `build/fio` and are named `spdk_bdev` and `spdk_nvme`.
Existing fio configuration files will need to be updated.

### dpdk

Updated DPDK submodule to DPDK 20.05.

### env

Several new APIs have been added to provide greater flexibility in registering and
accessing polled mode PCI drivers. See `env.h` for more details.

### idxd

The idxd library and plug-in module for the accel_fw were updated to support
all accel_fw commands as well as batching.  Batching is supported both
through the library and the plug-in module.

IDXD engine support for CRC-32C has been added.

### ioat

A new API `spdk_ioat_get_max_descriptors` was added.

### nvme

An `opts_size`element was added in the  `spdk_nvme_ctrlr_opts` structure
to solve the ABI compatibility issue between different SPDK version.

A new API `spdk_nvme_ctrlr_cmd_abort_ext` has been added to abort previously submitted
commands whose callback argument match.

Convenience functions, `spdk_nvme_print_command` and `spdk_nvme-print_completion` were added
to the public API.

A new function, `spdk_nvmf_cuse_update_namespaces`, updates the cuse representation of an NVMe
controller.

A new function `qpair_iterate_requests` has been added to the nvme transport interface. ALl
implementations of the transport interface will have to implement that function.

### nvmf

The NVMe-oF target no longer supports connecting scheduling configuration and instead
always uses what was previously called "transport" scheduling.

`spdk_nvmf_tgt_accept` no longer exists. The accept process now occurs automatically after
the creation of an nvmf target and queue pairs are assigned to poll groups by the underlying
transport. Further, `spdk_nvmf_transport_ops` has changed such that the accept function
pointer no longer takes a function pointer as an argument. Instead, transports should call
`spdk_nvmf_tgt_new_qpair` whenever they previously would have called that callback.

The NVMe-oF target now supports aborting any submitted NVM or Admin command. Previously,
the NVMe-oF target could abort only Asynchronous Event Request commands.

### rdma

A new `rdma` library has been added. It is an abstraction layer over different RDMA providers.
Two providers are available - verbs (used by default when RDMA is enabled or enabled explicitly
using --with-rdma=verbs) and mlx5 Direct Verbs aka DV (enabled by --with-rdma=mlx5_dv).
Using mlx5_dv requires libmlx5 installed on the system.

### rpc

Parameter `-p` or `--max-qpairs-per-ctrlr` of `nvmf_create_transport` RPC command accepted by the
rpc.py script is deprecated, new parameter `-m` or `--max-io-qpairs-per-ctrlr` was added.

Added `sock_impl_get_options` and `sock_impl_set_options` RPC methods.

Command line parameters `-r` and `--rpc-socket` will longer accept TCP ports. RPC server
must now be started on a Unix domain socket. Exposing RPC on the network, as well as providing
proper authentication (if needed) is now a responsibility of the user.

The `bdev_set_options` RPC has a new option, `bdev_auto_examine` to control the auto examine function
of bdev modules.

New RPCs `sock_impl_get_options` and `sock_impl_set_options` been added to expose new socket features.
See `sock` section for more details.

### sock

Added `spdk_sock_impl_get_opts` and `spdk_sock_impl_set_opts` functions to set/get socket layer configuration
options. Options can be set independently for each implementation.

Added `recv_buf_size` and 'send_buf_size' socket layer options. They are used only in posix implementation.

Added `uring` based socket implementation, the code is located in module/sock/uring. This feature is only
available in Linux which requires kernel version is greater than 5.4.3. Currently, our CI pool added the uring
based socket tests for iSCSI target and also the tests for SPDK NVMe-oF tcp transport.

Added `enable_recv_pipe` socket layer option to allow disabling of double buffering on receive.
New option is used only in posix implementation.

Added `enable_zerocopy_send` socket layer option to allow disabling of zero copy flow on send.
New option is used only in posix implementation.

### util

Some previously exposed CRC32 functions have been removed from the public API -
`spdk_crc32_update`, `spdk_crc32_table_init`, and the `spdk_crc32_table` struct.

### vhost

The function `spdk_vhost_blk_get_dev` has been removed.

## v20.04: SPDK Top, IDXD, NVMe qpair groups

IDXD engine support for compare has been added.

### configuration

Legacy INI style configuration for SPDK applications has been deprecated and will be
removed in future release.
Please switch to JSON-RPC configuration files and/or RPC driven run-time configuration.

### copy

The copy engine library, modules and public APIs have been renamed. Use of the word `copy`
has been replaced with the word `accel` short for accelerator in preparation for adding new
capabilities in the future. Additionally, APIs for what was previously called the `memcpy`
engine have been renamed to identify the engine as a software accelerator.

### crypto

Support for AES_XTS was added for the QAT polled mode driver (pmd).  The create RPC
`bdev_crypto_create` has 2 new optional parameters: cipher and key2. Cipher can be either
AES_CBC (default) or AES_XTS. AES_XTS is only valid when using the QAT polled mode driver.
The key2 parameter is the second key required for AES_XTS.

### event

Reactors now accumulate CPU stats and they are retrieved by the RPC `framework_get_reactors`.

### idxd

IDXD support was added in the form of a low level library that can directly
utilize the IDXD offload engine as well as an accel plug-in module allowing for use
with the generic accel framework API. IDXD is the first in a family of offload
engines that share the same interface, specifically DSA is added here. More info
can be found here: https://01.org/blogs/2019/introducing-intel-data-streaming-accelerator

Much of the implementation models IOAT, however the low level interfaces are very
different. The RPC to enable IDXD requires a configuration number as well. The
code includes two pre-defined configurations of IDXD groups/work queues/engines. A future
version will provide an interface to allow for setting of individual configuration
parameters.

IDXD is not yet available so this feature should be considered experimental. It will
be built up with additional documentation as an ongoing activity.

### iscsi

The iSCSI target now creates a lightweight thread per poll group instead of assuming a pool
of lightweight threads already exist at start up time. A poll group is a collection of
unrelated iSCSI connections. Each poll group is only accessed from the associated
lightweight thread.

### ftl

Several changes have been made to the `spdk_ftl_conf`, `spdk_ftl_dev_init_ops`, and
`spdk_ftl_attrs` structs. Please see `include/spdk/ftl.h` for more details.

### miscellaneous

The `--json-ignore-init-errors` command line parameter has been added to ignore
initialization errors on JSON config load.

The public header file io_channel.h has been removed. Please use thread.h which has the
exact same API.

### nvme

Exported internal nvme_ctrlr_cmd_security_receive/send() APIs as public APIs with "the spdk_"
prefix.

Added `priority` field in `spdk_nvme_transport_id`, this field is used to specify the priority
of the NVMe-oF connection, and currently it is used for NVMe-oF tcp connection.

A new poll group API has been added to allow for pooling of nvme qpairs across a single
entity which can be polled for completions. This new API consists of the `spdk_nvme_poll_group`
family of functions. As a result of this new API, all NVMe transports are expected to implement
several poll group related functions.

A new flag, `create_only`, has been added to the `spdk_nvme_io_qpair_opts` structure. This flag
allows a user to call `spdk_nvme_ctrlr_get_default_io_qpair` without also connecting the qpair
within the context of that call.

As a result of the `create_only` flag, two new API functions, `spdk_nvme_ctrlr_connect_io_qpair`
and `spdk_nvme_ctrlr_disconnect_io_qpair`, have been added to facilitate connecting newly created
qpairs (for example, after they have been added to a poll group) and disconnecting qpairs without
destroying them (for example to disconnect a qpair before migrating it to a new poll group and
reconnecting it).

The functions `spdk_nvme_ctrlr_alloc_cmb_io_buffer` and `spdk_nvme_ctrlr_free_cmb_io_buffer`
have been changed to `spdk_nvme_ctrlr_map_cmb` and `spdk_nvme_ctrlr_unmap_cmb` respectively.

An additional function, `spdk_nvme_ctrlr_reserve_cmb`, has been added to facilitate reserving
the entire size of the controller memory buffer for data transfer.

### nvme_cuse

`spdk_nvme_cuse_get_ctrlr_name` now takes two additional parameters, `char *name` which
stores the pointer to the controller name, and `size_t *size` which stores the length of
the name. The return type has also been changed from char * to int.

`spdk_nvme_cuse_get_ns_name` now takes two additional parameters, `char *name` which
stores the pointer to the namespace name, and `size_t *size` which stores the length of
the name. The return type has also been changed from char * to int.

### nvme_opal

Several public OPAL structure definitions have been changed since the last release.

### nvmf

`spdk_nvmf_poll_group_destroy()` is now asynchronous and accepts a completion callback.

The NVMe-oF target now creates a lightweight thread per poll group instead of assuming a pool
of lightweight threads already exist at start up time. A poll group is a collection of
unrelated NVMe-oF connections. Each poll group is only accessed from the associated
lightweight thread.

A new struct, `spdk_nvmf_subsystem_listener`, has been added to encapsulate the subsystem specific
nature of a listener object.

`spdk_nvmf_tgt_listen` no longer accepts a callback function or argument. It also returns an
int to indicate the status of the listen call.

The execution of `spdk_nvme_poll_group_destroy` is now asynchronous and the function accepts
a cb_fn and cb_arg to call upon completion.

The execution of `spdk_nvmf_subsystem_add_listener` is now asynchronous and the function accepts
a cb_fn and cb_arg to call upon completion.

The `nvmf_transport.h` header has been made public to allow custom NVMe-oF transports to integrate
with NVMe-oF libraries without using internal APIs.

### ocf

Updated the OCF submodule to OCF v20.03

New version of OCF comes with API changes and bug fixes

### rpc

A new RPC `thread_set_cpumask` has been added to set the cpumask of the thread
to the specified value.

A new RPC `thread_get_pollers` has been added to retrieve pollers from SPDK threads.

A new RPC `thread_get_io_channels` has been added to retrieve I/O channels from SPDK threads.

A new RPC `bdev_rbd_resize` has been added to resize Ceph RBD bdevs.

### sock

The `spdk_sock_set_priority` function has been removed since the feature to set the sock priority
will be contained in two new functions, i.e., `spdk_sock_listen_ext` and `spdk_sock_connect_ext`.
Users may now specify the priority of the socket in the opts that they want to use.

### spdk_top

A new application, `spdk_top`, has been added which allows users to monitor resource consumption
by a running SPDK application. More information on this application can be found in
`app/spdk_top/README`.

### thread

A new function `spdk_thread_lib_init_ext` has been added, and the function
`spdk_thread_lib_init` has been deprecated. The user of `spdk_thread_lib_init_ext` is
expected to implement both functions `spdk_thread_op_fn` and `spdk_thread_op_supported_fn`.
`spdk_thread_op_supported_fn` is called to check whether the SPDK thread operation
is supported. `spdk_thread_op_fn` is called to execute the SPDK thread operation.
Current SPDK operation types are `SPDK_THREAD_OP_NEW` and `SPDK_THREAD_OP_RESCHED`.
The operation `SPDK_THREAD_OP_NEW` is called each time a new thread is created.
The operation `SPDK_THREAD_OP_RESCHED` is called when SPDK thread needs to be rescheduled.

A unique ID has been added for each created SPDK thread, it is retrieved by a new function
`spdk_thread_get_id`, and the SPDK thread which has the specific ID is got by
a new function `spdk_thread_get_by_id`.

A new function `spdk_thread_cpumask` has been added to set the current thread's cpumask
to the specified value. The function requires the operation `SPDK_THREAD_OP_RESCHED`
is supported.

A new function `spdk_poller_register_named` has been added to set arbitrary name to the
created poller. If NULL, the name is set to the pointer of the poller function.

The function `spdk_thread_poll` now measures run time per call correctly on multiple SPDK
threads configuration, and a new function `spdk_thread_get_last_tsc` has been added to use together.

Voluntary termination of SPDK thread has been supported by refining the functions `spdk_thread_exit`
and `spdk_thread_poll`.

### util

New functions `spdk_sn32_lt` and `spdk_sn32_gt` have been added. They compare two sequence
numbers based on serial number arithmetic.

### vhost

Poll groups per session have been replaced by SPDK threads per vhost controller.

### vmd

A new function, `spdk_vmd_fini`, has been added. It releases all resources acquired by the VMD
library through the `spdk_vmd_init` call.

## v20.01: Optimized thin provisioning, FTL bdev, VMD hot plug, FUSED support

### bdev

A new function, `spdk_bdev_set_timeout`, has been added to set per descriptor I/O timeouts.

A new class of functions `spdk_bdev_compare*`, have been added to allow native bdev support
of block comparisons and compare-and-write.

A new class of bdev events, `SPDK_BDEV_EVENT_MEDIA_MANAGEMENT`, has been added to allow bdevs
which expose raw media to alert all I/O channels of pending media management events.

A new API was added `spdk_bdev_io_get_aux_buf` allowing the caller to request
an auxiliary buffer for its own private use. The API is used in the same manner that
`spdk_bdev_io_get_buf` is used and the length of the buffer is always the same as the
bdev_io primary buffer. 'spdk_bdev_io_put_aux_buf' frees the allocated auxiliary
buffer.

### blobfs

Added boolean return value for function spdk_fs_set_cache_size to indicate its operation result.

Added `blobfs_set_cache_size` RPC method to set cache size for blobstore filesystem.

### blobstore

Added new `use_extent_table` option to `spdk_blob_opts` for creating blobs with Extent Table descriptor.
Using this metadata format, dramatically decreases number of writes required to persist each cluster allocation
for thin provisioned blobs. Extent Table descriptor is enabled by default.
See the [Blobstore Programmer's Guide](https://spdk.io/doc/blob.html#blob_pg_cluster_layout) for more details.

### dpdk

Updated DPDK submodule to DPDK 19.11.

### env_dpdk

`spdk_env_dpdk_post_init` now takes a boolean, `legacy_mem`, as an argument.

A new function, `spdk_env_dpdk_dump_mem_stats`, prints information about the memory consumed by DPDK to a file specified by
the user. A new utility, `scripts/dpdk_mem_info.py`, wraps this function and prints the output in an easy to read way.

### event

The functions `spdk_reactor_enable_framework_monitor_context_switch()` and
`spdk_reactor_framework_monitor_context_switch_enabled()` have been changed to
`spdk_framework_enable_context_switch_monitor()` and
`spdk_framework_context_switch_monitor_enabled()`, respectively.

### ftl

All NVMe dependencies were removed from ftl library.
From now ftl library is using bdev_zone API.
bdev_ftl becomes virtual bdev.

`ctrlr` and `trid` fields in `spdk_ftl_dev_init_opts` structure  were replaced by
`base_bdev_desc`.

`bdev_ftl_create` RPC method `trtype` and `traddr` parameters were replaced by `base_bdev`
parameter.

`spdk_ftl_module_init` and `spdk_ftl_module_fini` functions were removed.

`spdk_ftl_punit_range` and `ftl_module_init_opts` structures were removed.

### isa-l

Updated ISA-L submodule to commit f3993f5c0b6911 which includes implementation and
optimization for aarch64.

Enabled ISA-L on aarch64 by default in addition to x86.

### nvme

`delayed_pcie_doorbell` parameter in `spdk_nvme_io_qpair_opts` was renamed to `delay_cmd_submit`
to allow reuse in other transports.

Added RDMA WR batching to NVMf RDMA initiator. Send and receive WRs are chained together
and posted with a single call to ibv_post_send(receive) in the next call to qpair completion
processing function. Batching is controlled by 'delay_cmd_submit' qpair option.

The NVMe-oF initiator now supports plugging out of tree NVMe-oF transports. In order
to facilitate this feature, several small API changes have been made:

The `spdk_nvme_transport_id` struct now contains a trstring member used to identify the transport.
A new function, `spdk_nvme_transport_available_by_name`, has been added.
A function table, `spdk_nvme_transport_ops`, and macro, `SPDK_NVME_TRANSPORT_REGISTER`, have been added which
enable registering out of tree transports.

A new function, `spdk_nvme_ns_supports_compare`, allows a user to check whether a given namespace supports the compare
operation.

A new family of functions, `spdk_nvme_ns_compare*`, give the user access to submitting compare commands to NVMe namespaces.

A new function, `spdk_nvme_ctrlr_cmd_get_log_page_ext`, gives users more granular control over the command dwords sent in
log page requests.

### nvmf

Add SockPriority option in [Transport] section, this can be used for NVMe-oF target
on TCP transport to set sock priority for the incoming TCP connections.

The NVMe-oF target now supports plugging out of tree NVMe-oF transports. In order
to facilitate this feature, several small API changes have been made:

The `spdk_nvme_transport_id` struct now contains a trstring member used to identify the transport.
`spdk_nvmf_tgt_get_transport`, `spdk_nvmf_transport_opts_init`, and `spdk_nvmf_transport_create` all have been
modified to take a string.
A function table, `spdk_nvmf_transport_ops`, and macro, `SPDK_NVMF_TRANSPORT_REGISTER`, have been added which
enable registering out of tree transports.

Add `spdk_nvmf_tgt_stop_listen()` that can be used to stop listening for
incoming connections for specified target and trid. Listener is not stopped
implicitly upon destruction of a subsystem any more.

A custom NVMe admin command handler has been added which allows the user to use the real drive
attributes from one of the target NVMe drives when reporting drive attributes to the initiator.
This handler can be enabled via the `nvmf_set_config` RPC.
Note: In a future version of SPDK, this handler will be enabled by default.

The SPDK target and initiator both now include compare-and-write functionality with one caveat. If using the RDMA transport,
the target expects the initiator to send both the compare command and write command either with, or without inline data. The
SPDK initiator currently respects this requirement, but this note is included as a flag for other initiators attempting
compatibility with this version of SPDK.

The `spdk_nvme_ctrlr_opts` struct has been extended with new field `transport_ack_timeout` which allows
to configure transport ACK timeout. Applicable for RDMA transport only.

### rpc

A new RPC, `bdev_zone_block_create`, enables creating an emulated zoned bdev on top of a standard block device.

A new RPC, `bdev_ocssd_create`, enables creating an emulated zoned bdev on top of an Open Channel SSD.

A new RPC, `blobfs_set_cache_size`, enables managing blobfs cache size.

A new RPC, `env_dpdk_get_mem_stats`, has been added to facilitate reading DPDK related memory
consumption stats. Please see the env_dpdk section above for more details.

A new RPC, `framework_get_reactors`, has been added to retrieve a list of all reactors.

`bdev_ftl_create` now takes a `base_bdev` argument in lieu of `trtype`, `traddr`, and `punits`.

`bdev_nvme_set_options` now allows users to disable I/O submission batching with the `-d` flag

`bdev_nvme_cuse_register` now accepts a `name` parameter.

`bdev_uring_create` now takes arguments for `bdev_name` and `block_size`

`nvmf_set_config` now takes an argument to enable passthru of identify commands to base NVMe devices.
Please see the nvmf section above for more details.

### scsi

`spdk_scsi_lun_get_dif_ctx` now takes an additional argument of type `spdk_scsi_task`.

### sock

Added spdk_sock_writev_async for performing asynchronous writes to sockets. This call will
never return EAGAIN, instead queueing internally until the data has all been sent. This can
simplify many code flows that create pollers to continue attempting to flush writes
on sockets.

Added `impl_name` parameter in spdk_sock_listen and spdk_sock_connect functions. Users may now
specify the sock layer implementation they'd prefer to use. Valid implementations are currently
"vpp" and "posix" and NULL, where NULL results in the previous behavior of the functions.

### thread

`spdk_thread_send_msg` now returns int indicating if the message was successfully
sent.

A new function `spdk_thread_send_critical_msg`, has been added to support sending a single message from
a context that may be interrupted, e.g. a signal handler.

Two new functions, `spdk_poller_pause`, and `spdk_poller_resume`, have been added to give greater control
of pollers to the application owner.

### util

`spdk_pipe`, a new utility for buffering data from sockets or files for parsing
has been added. The public API is available at `include/spdk/pipe.h`.

### bdev

Added spdk_bdev_io_get_nvme_fused_status function for translating bdev_io status to NVMe status
code for fused compare-and-write operation.

Added spdk_bdev_get_acwu function for getting block device atomic compare and write unit size.

## v19.10: Zoned bdev API, Opal bdev, NVMe character devices

### rpc

Many of SPDK's RPCs were renamed to be more consistent and intuitive in this release.
The old names will continue to function, but will display a deprecation warning.

Added optional parameters '--arbitration-burst' and '--low/medium/high-priority-weight' to
'bdev_nvme_set_options' RPC method.

Added optional parameter '--md-size' to 'construct_null_bdev' RPC method.

Added optional parameters '--dif-type' and '--dif-is-head-of-md' to 'construct_null_bdev'
RPC method.

Added `blobfs_detect` RPC method to detect whether a blobfs exists on given bdev.

Added `blobfs_create` RPC method to build blobfs on given bdev.

Added `blobfs_mount` RPC method to mount blobfs on given bdev to a host path by FUSE.
Then on the host path, user can directly do some file operations which will be mapped
to blobfs.

### bdev

Added new parameter `cdw0` to `spdk_bdev_io_complete_nvme_status()` and
`spdk_bdev_io_get_nvme_status()` that allows setting/getting
the NVMe completion queue DW0 entry. This allows vendor specific IO commands
to return command specific completion info back to the initiator.

Added `spdk_bdev_get_write_unit_size()` function for retrieving required number
of logical blocks for write operation.

New zone-related fields were added to the result of the `get_bdevs` RPC call:

- `zoned`: indicates whether the device is zoned or a regular
  block device
- `zone_size`: number of blocks in a single zone
- `max_open_zones`: maximum number of open zones
- `optimal_open_zones`: optimal number of open zones

The `zoned` field is a boolean and is always present, while the rest is only available for zoned
bdevs.

A new `spdk_bdev_open_ext` function has been added and `spdk_bdev_open` function has been deprecated.
The new open function introduces requirement to provide callback function that will be called by
asynchronous event such as bdev removal. `spdk_bdev_open_ext` function takes bdev name as
an argument instead of bdev structure to avoid a race condition that can happen when the bdev
is being removed between a call to get its structure based on a name and actually openning it.

New 'resize' event has been added to notify about change of block count property of block device.
Event is delivered only if block device was opened with `spdk_bdev_open_ext` function.

### bdev zone

Added new public header for zoned bdev. Zoned bdev is an extension
of the bdev interface.

`spdk_bdev_get_zone_size()`, `spdk_bdev_get_max_open_zones()`, `spdk_bdev_get_optimal_open_zones()`
APIs were added for retrieving zoned device information.
`spdk_bdev_get_zone_info()` API was added for retrieving information about zones in zoned
device.
Added `spdk_bdev_zone_management()` API for changing zone state.
`spdk_bdev_zone_append()` and `spdk_bdev_zone_append_with_md()` APIs were added for
appending data to a zone.
Added `spdk_bdev_io_get_append location()` function for retrieving append location for I/O.
Added `spdk_bdev_is_zoned()` function for checking if bdev supports zoned namespace semantics.

### bdev opal

EXPERIMENTAL: A new opal bdev has been added to support management of
NVMe self-encrypting drives through the Opal specification. Users can
create opal bdevs from an NVMe namespace bdev, if the controller
containing that namespace supports Opal. Currently this is only
supported for namespace ID=1. The following RPCs have been added to
support Opal: `bdev_nvme_opal_init`, `bdev_nvme_opal_revert`,
`bdev_opal_create`, `bdev_opal_delete`, `bdev_opal_get_info`,
`bdev_opal_new_user`, `bdev_opal_set_lock_state`.
It does not yet support recreating the opal bdevs after application restart.
This bdev module should be considered very experimental, and the RPCs may
change significantly in future releases.

### delay bdev

The `bdev_delay_update_latency` has been added to allow users to update
a latency value for a given delay bdev.

### compress bdev

A new RPC `bdev_compress_get_orphans` has been added to list compress bdevs
that were not loaded due to a missing pm metadata file. In this state they
can only be deleted.

### null bdev

Metadata support has been added to Null bdev module.

Protection information support has been added to Null bdev module.

### nvme

Added `no_shn_notification` to NVMe controller initialization options, users can enable
it for NVMe controllers.  When the option is enabled, the controller will not do the
shutdown process and just disable the controller, users can start their application
later again to initialize the controller to the ready state.

A controller flag `SPDK_NVME_CTRLR_WRR_SUPPORTED` was added to indicate the controller
can support weighted round robin arbitration feature with submission queue.

Added `arbitration_burst` option for arbitration feature, and added three
`low/medium/high_priority_weight` options for weighted round robin arbitration.

Added `spdk_nvme_ns_cmd_write_uncorrectable`.

Added new error handling and reporting functionality. This includes several
new API functions to facilitate applications recovering when a qpair or
controller fails.

`spdk_nvme_ctrlr_reconnect_io_qpair` - Reconnects a failed I/O qpair.
`spdk_nvme_ctrlr_set_trid` - Sets the trid of an existing controller. Can be used to
change the trid for failover cases.
`spdk_nvme_ctrlr_is_failed` - Returns the failed state of a controller.
`spdk_nvme_ctrlr_fail` - Forces a controller into a failed state.

Modified the return behavior of several API functions to better indicate to
applications when a qpair is failed. This list of functions includes:

`spdk_nvme_qpair_process_completions`
`spdk_nvme_ns_cmd_*`
`spdk_nvme_ctrlr_process_admin_completions`
`spdk_nvme_ctrlr_cmd_*`

These functions now return -ENXIO when the qpair or controller on which they
operate is failed.

EXPERIMENTAL: Added NVMe character device support to allow to create NVMe device nodes in Linux
kernel for controller as well as for namespace and process ioctl requests as usual
from linux environment.

### nvmf

The `spdk_nvmf_tgt_create` function now accepts an object of type `spdk_nvmf_target_opts`
as its only parameter. This new structure contains the max_subsystems parameter previously
passed into that function.

A new public API function `spdk_nvmf_get_tgt` has been added which allows users to
retrieve a pointer to an `spdk_nvmf_tgt` object by supplying its name. In the special
case where an RPC or application only creates a single target, this function can accept
a null name parameter and will return the only available target.

The majority of the NVMe-oF RPCs now accept an optional tgt_name parameter. This will
allow those RPCs to work with applications that create more than one target.

Three new NVMe-oF RPCs have been added `nvmf_create_target`, `nvmf_delete_target`, and
`nvmf_get_targets`. These new RPCs provide a basic interface for managing multiple target
objects. In SPDK the target object defines a unique discovery service. As of this release,
these RPCs are not intended to be used with the in-tree SPDK target applications, spdk_tgt and
nvmf_tgt, which use a single, global target structure. As such, they are not included in scripts/rpc.py

Three new header functions have also been added to help deal with multiple targets.
`spdk_nvmf_tgt_get_name` takes a target pointer as an argument and returns its human readable name.
`spdk_nvmf_get_first_target` takes no arguments and returns the first target in the global list.
`spdk_nvmf_get_next_tgt` takes a target pointer as an argument and returns the next one in the global list.

The `spdk_nvmf_tgt_accept` takes additional argument allowing to pass arbitrary context
information to the `new_qpair` callback. This will simplify the code when having multiple
nvmf targets or when retrieving the context information from globals is not suitable.

### blobstore

A new `spdk_bdev_create_bs_dev_from_desc` function has been added and `spdk_bdev_create_bs_dev`
function has been deprecated.
The new create function can cowork with `spdk_bdev_open_ext` function, which provides callback
function that will be called by asynchronous event such as bdev removal.

### blobfs_bdev

A new blobfs module `bdev` has been added to simplify the operations of blobfs on bdev.

Function `spdk_blobfs_bdev_detect` is added to detect whether blobfs exists on the given block device.

Function `spdk_blobfs_bdev_create` is added to create a blobfs on the given block device.

Function `spdk_blobfs_bdev_mount` is added to mount a blobfs on the given block device to
a host path by FUSE. Then, a new thread is created dedicatedly for one mountpoint to handle
FUSE request by blobfs API.

### build

Option to build FUSE components into blobfs_bdev module for mounting a blobfs filesystem.
It requires the installation of libfuse3. By default, it is disabled. And it will be
enabled if run `./configure` with `--with-fuse` option.

### iSCSI

Portals may no longer be associated with a cpumask. The scheduling of
connections is moving to a more dynamic model.

An new RPC `iscsi_portal_group_set_auth` has been added to set CHAP authentication
for discovery sessions specific for the existing iSCSI portal group. This RPC overwrites
the setting by the global parameters for the iSCSI portal group.

### socket

Added `spdk_sock_is_connected` to check whether the socket is currently connected.
`spdk_sock_group_poll` now returns number of events on success.

### env

Added `spdk_pci_device_unclaim()` function to cleanup pci claim file.

### event

`framework_start_init` RPC no longer stops the application on error during
initialization.

### DPDK

Updated DPDK submodule to DPDK 19.08.

### ocf

Updated OCF submodule to OCF v19.06

Along with update, new cache mode 'write only' was added.

New cache modes added to use via RPC, wi - write invalidate and wa - write around.

New version of OCF provides fully asynchronous management API.

## v19.07: NVMe-oF FC Transport, VMD, NVMe-oF Persistent reservations, Bdev I/O with separate metadata

### ftl

EXPERIMENTAL: Added ability to mirror writes to persistent write buffer cache
to allow for recovery from dirty shutdown event.

Added handling of Asynchronous Nand Management Events (ANM).

### vmd

EXPERIMENTAL: Added Intel Volume Management Device (VMD) driver. VMD is an integrated
controller inside the CPU PCIe root complex. It enables virtual HBAs for the connected
NVMe SSDs. `spdk_vmd_init()` enumerates NVMe SSDs behind VMD device and hook them into
SPDK PCI subsystem. `spdk_nvme_probe()` or `spdk_nvme_connect()` can be used to connect
NVMe driver to the device located at the given transport ID.

To obtain transport ID of NVMe SSD behind VMD `spdk_lspci` can be used.

Current implementation does not support hotplug.

### blobfs

Blobfs file asynchronous operations were added to public APIs.

### util

A new file API `spdk_posix_file_load` was added to load file content into a data buffer.

New APIs `spdk_dif_ctx_set_data_offset`, `spdk_dif_verify_stream`,
`spdk_dif_get_range_with_md`, `spdk_dif_get_length_with_md` have been added,
and existing APIs `spdk_dif_ctx_init`, `spdk_dif_set_md_interleave_iovs`, and
`spdk_dif_generate_stream` have been refined to insert or strip DIF into iovs
windows of any alignment.

New APIs `spdk_dif_ctx_set_remapped_init_ref_tag`, `spdk_dif_remap_ref_tag`,
and `spdk_dix_remap_ref_tag` have been added to remap DIF reference tag.

New APIs `spdk_dif_update_crc32c` and `spdk_dif_update_crc32c_stream` have been
added to compute CRC-32C checksum for extended LBA payload.

Bdevperf and bdevio applications now support starting tests with application specific
RPCs. Please see helper Python scripts in their respective directories.
This is a move towards simpler RPC-only configuration for all main
and auxiliary applications.

Legacy INI style configuration for SPDK applications will become deprecated in SPDK 19.10,
and removed in SPDK 20.01. Please consider moving to JSON-RPC configuration files and/or
RPC driven run-time configuration.

### nvmf

EXPERIMENTAL: A Fibre Channel transport that supports Broadcom HBAs has been
added. This depends on the FC HBA driver at
https://github.com/ecdufcdrvr/bcmufctdrvr. See [the documentation](https://spdk.io/doc/nvmf.html#nvmf_fc_transport)
for more information.

Persistent reservation emulation has been added to the NVMe-oF target. Persistent reservation
state is stored in a JSON file on the local filesystem between target restart. To support this,
an optional parameter to the RPC method `nvmf_subsystem_add_ns` called `--ptpl-file` was added.
This allows the user to specify which file to store the persistent reservation state in.  Note
that this is done per namespace.

The c2h success optimization under which a command capsule response is not sent
for reads is turned on by default. A config knob was added to allow disabling
the optimization. This will mostly be used for integration testing with 5.0.x kernels
while some compatibility fixes make their way down the pipeline for 5.1.x kernels.

The sock priority setting of the TCP connection owned by the tcp transport is added. It is
used to optimize the TCP connection performance under designated traffic classes. And the
priority is used to differentiate the sock priority between SPDK NVMe-oF TCP target application
and other TCP based applications.

Shared receive queue can now be disabled even for NICs that support it using the
`nvmf_create_transport` RPC method parameter `no_srq`. The actual use of a shared
receive queue is predicated on hardware support when this flag is not used.

spdk_nvmf_get_optimal_poll_group was added, which is used to return the optimal
poll group for the qpair. And `ConnectionScheduler` configuration is added into the
[Nvmf] section in etc/spdk/nvmf.conf.in to demonstrate how to configure the connection
scheduling strategy among different spdk threads.

Added infrastructure to retrieve global and per poll group NVMf statistics.

DIF strip and insert is now supported for TCP transport. When it is enabled, DIF
setting is not exposed to the NVMe-oF initiator, and DIF is attached into data
for write I/O and stripped from data for read I/O.

Added a field `dif_insert_or_strip` to struct spdk_nvmf_transport_opts, and
updated the related rpc function nvmf_create_transport to make this
configurable parameter available to users. The `dif_insert_or_strip` is relevant
for TCP transport for now and used to configure the DIF strip and insert.

Added infrastructure to retrieve NVMf transport statistics.

### notify

The function `spdk_notify_get_types()` and `spdk_notify_get_events()` were
renamed to `spdk_notify_foreach_type()` and `spdk_notify_foreach_event()`,
respectively. And update type name of callback accordingly.

### bdev

The format of the data returned by the get_bdevs_iostat RPC has changed to
make it easier to parse.  It now returns an object with a "ticks" object
and "bdevs" array with the per-bdev statistics.

A new bdev module `delay` has been added which simulates a drive latency when placed
on top of a Null bdev. This module is intended only for testing and can be created using
the new RPC `bdev_delay_create`. That RPC takes the name of the underlying bdev as well
as average and p99 latency arguments for both read and write operations. Average latency is
defined as a value close to what you would expect a perf tool such as FIO to report back as
the mean latency of all I/O submitted to the drive. p99 latency is defined as the value one
would expect the drive to see the slowest 1% of I/O report. For underlying drives with already
significant latency, the latency values provided to the drive will be additive. This should be
taken into account if trying to achieve an artificial latency on top of an nvme drive or aio device.

DIF reference tag remapping is now supported for partition type virtual bdev
modules. When using partition type virtual bdevs, block address space is
remapped during I/O processing and DIF reference tag is remapped accordingly.

Added spdk_bdev_*_with_md() functions allowing for IO with metadata being transferred in
separate buffer. To check support for separate metadata, use spdk_bdev_is_md_separate().

All bdevs now have a UUID. For devices whose backing hardware does not provide a UUID,
one is automatically generated. Across runs of SPDK, bdevs whose UUID is automatically
generated may change.

A new virtual bdev module `compress` has been added to provide compression services on top of
a thinly provisioned logical volume.  See documentation for complete details.

### nvme

Added an optional parameter `--io-queue-requests` to RPC `set_bdev_nvme_options`, which
can be used to change the number of requests allocated for one NVMe I/O queue.  For
very big I/O size, e.g. 128MiB, with this option user will not get an error due to
limited requests in NVMe driver layer.

Added spdk_nvme_ctrlr_get_transport_id() to get the transport ID from a
previously attached controller.

Nvme Opal library spdk_opal_cmd deprecated. Adding separate command APIs.
NVMe Opal library add support for activating locking SP which will make the transaction
from "Manufactured-Inactive" state to "Manufactured" state. Upon successfully invoking
of this method, lock and unlock features will be enabled.
NVMe Opal library add support for locking/unlocking range and list locking range info.
NVMe opal library add support for multiuser. Admin can enable user and add user to specific
locking range and the user can lock/unlock his range.

Added spdk_nvme_ctrlr_io_cmd_raw_no_payload_build() allowing a caller to pass
a completely formed command to an NVMe submission queue (buffer addresses and all).
This is supported on the PCIe transport only.

Added spdk_nvme_get_ctrlr_registers() to return a pointer to the virtual address
of the NVMe controller registers.  This is supported on the PCIe transport only.

Added additional options to the spdk_nvme_ctrlr_alloc_qpair() option parameter
structure to allow caller to override the virtual and optionally physical address
of the submission and completion queue pair to be created.  This is supported on
the PCIe transport only.

Added `disable_error_logging` to struct spdk_nvme_ctrlr_opts, which disables
logging of failed requests. By default logging is enabled.

Added spdk_nvme_qpair_print_command(), spdk_nvme_qpair_print_completion() and
spdk_nvme_cpl_get_status_string(). Allowing for easier display of error messages.

Added support for NVMe Sanitize command.

### env

The parameter `free_space` has been added to spdk_ring_enqueue() to wait when
the ring is almost full and resume when there is enough space available in
the ring.

A new API `spdk_mempool_lookup` has been added to lookup the memory pool created
by the primary process.

Added spdk_pci_get_first_device() and spdk_pci_get_next_device() to allow
iterating over PCI devices detected by SPDK. Because of this, all SPDK APIs
to attach/detach PCI devices are no longer thread safe. They are now meant to
be called from only a single thread only, the same only that called spdk_env_init().
This applies to the newly added APIs as well.

### vpp

SPDK now supports VPP version 19.04.2, up from VPP 18.01.

VPP socket abstraction now uses VPP Session API, instead of VLC (VPP Communications Library).
This allows for better control over sessions and queues.
Please see VPP documentation for more details:
[VPP Host Stack](https://wiki.fd.io/view/VPP/HostStack)

### sock

Add spdk_sock_get_optimal_sock_group(), which returns the optimal sock group for
this socket. When a socket is created, it is often assigned to a sock group using
spdk_sock_group_add_sock so that a set of sockets can be polled more efficiently.
For some network devices, it is optimal to assign particular sockets to specific
sock groups. This API is intended to provide the user with that information.

spdk_sock_group_get_ctx() was added to return the context of the spdk_sock_group.
spdk_sock_group_create() is updated to allow input the user provided ctx.
spdk_sock_set_priority() is added to set the priority of the socket.

### rpc

Added thread_get_stats RPC method to retrieve existing statistics.

Added nvmf_get_stats RPC method to retrieve NVMf subsystem statistics.

Response buffers for RPC requests are now always pre-allocated, which implies
that all spdk_jsonrpc_begin_result() calls always succeed and return a valid
buffer for JSON response. RPC calls no longer need to check if the buffer is
non-NULL.

Added SPDK_RPC_REGISTER_ALIAS_DEPRECATED to help with deprecation process when
renaming existing RPC. First time a deprecated alias is used, it will print
a warning message.

RPC `get_rpc_methods` was renamed `rpc_get_methods`. The old name is still usable,
but is now deprecated.

### blobstore

A snapshot can now be deleted if there is only a single clone on top of it.

### build

Preliminary support for cross compilation is now available. Targeting an older
CPU on the same architecture using your native compiler can be accomplished by
using the `--target-arch` option to `configure` as follows:

~~~bash
./configure --target-arch=broadwell
~~~

Additionally, some support for cross-compiling to other architectures has been
added via the `--cross-prefix` argument to `configure`. To cross-compile, set CC
and CXX to the cross compilers, then run configure as follows:

~~~bash
./configure --target-arch=aarm64 --cross-prefix=aarch64-linux-gnu
~~~

### vhost

A security vulnerability has been identified and fixed in SPDK Vhost-SCSI target.
A malicious client (e.g. a virtual machine) could send a carefully prepared,
invalid I/O request to crash the entire SPDK process. All users of SPDK Vhost-SCSI
target are strongly recommended to update. All SPDK versions < 19.07 are affected.

By default, SPDK will now rely on upstream DPDK's rte_vhost instead of its fork
located inside SPDK repo. The internal fork is still kept around to support older
DPDK versions, but is considered legacy and will be eventually removed.

`configure` will now automatically use the upstream rte_vhost if the used DPDK
version is >= 19.05.

spdk_vhost_init() is now asynchronous and accepts a completion callback.

### iscsi target

A security vulnerability has been identified and fixed in SPDK iSCSI target.
A malicious client (e.g. an iSCSI initiator) could send a carefully prepared,
invalid I/O request to crash the entire SPDK process. All users of SPDK iSCSI
target are strongly recommended to update. All SPDK versions < 19.07 are affected.

### thread

Exposed spdk_set_thread() in order for applications to associate
with SPDK thread when necessary.

Added spdk_thread_destroy() to allow framework polling the thread to
release resources associated with that thread.

## v19.04: Compression bdev, Notification library, NVMe Opal support

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

The NVMe-oF initiator is now able to transfer IO whose size is more than 128KiB
if the corresponding NVMe-oF target allows.

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

### ocf

Updated OCF submodule to OCF v19.3.2

Added support for many-to-one configuration for OCF bdev.
Multiple core devices can now be cached on single cache device.

Added persistent metadata support, allowing to restore cache state after shutdown.
During start of SPDK application, the devices are examined and if OCF metadata
is present - appropriate OCF bdevs will be recreated.

Added Write-Back mode support. In this mode, data is first written to
caching device and periodically synchronized to the core devices.
Dirty data is saved as persistent metadata on cache device,
allowing for safe restore during application restart.
For more details please see OCF documentation:
[OpenCAS cache configuration](https://open-cas.github.io/cache_configuration.html)

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
compressed blocks of storage and persistent memory for metadata. Please see
[compression](https://spdk.io/doc/bdev.html) for more details.

## v19.01: NVMe-oF TCP/IP Transport, Open Channel SSD Flash Translation Layer, Caching bdev based on OCF, ISA-L Support, DIF/DIX library

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

Add a new TCP/IP transport (located in lib/nvmf/tcp.c). With this transport,
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

Bdev I/O statistics now track unmap operations.

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
JSON RPC server can now receive a callback on connection termination or server shutdown using `spdk_jsonrpc_conn_add_close_cb`
and `spdk_jsonrpc_conn_del_close_cb`.

## v18.10: Dynamic memory allocation, Crypto Virtual Bdev, jsonrpc-client, SPDKCLI iSCSI and NVMe-oF support

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
Scripts without shebang mustn't be executable.

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

## v18.07: Raid, Infrastructure Improvements, Bug Fixes

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

A JSON RPC listener is now enabled by default using a UNIX domain socket at /var/tmp/spdk.sock.
A -r option command line option has been added to enable an alternative UNIX domain socket location,
or a TCP port in the format ip_addr:tcp_port (i.e. 127.0.0.1:5260).  The Rpc configuration file
section is now deprecated and will be removed in the v18.04 release.

### I/O Channels

spdk_poller_register() and spdk_poller_unregister() were moved from the event
framework (include/spdk/event.h) to the I/O channel library
(include/spdk/io_channel.h). This allows code that doesn't depend on the event
framework to request registration and unregistration of pollers.

spdk_for_each_channel() now allows asynchronous operations during iteration.
Instead of immediately continuing the interaction upon returning from the iteration
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

NVMe-oF subsystems have a new configuration option, AllowAnyHost, to control
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

See the [Virtio SCSI](http://www.spdk.io/doc/virtio.html) documentation and
[Getting Started](http://www.spdk.io/doc/bdev.html#bdev_config_virtio_scsi) guide for more information.

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

This release adds a user-space driver with support for the Intel I/O Acceleration Technology
(I/OAT, also known as "Crystal Beach") DMA offload engine.

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
