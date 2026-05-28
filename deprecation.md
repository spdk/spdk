# Deprecation

## ABI and API Deprecation

This document details the policy for maintaining stability of SPDK ABI and API.

Major ABI version can change at most once for each SPDK release.
ABI versions are managed separately for each library and follow [Semantic Versioning](https://semver.org/).

API and ABI deprecation notices shall be posted in the next section.
Each entry must describe what will be removed and can suggest the future use or alternative.
Specific future SPDK release for the removal must be provided.
ABI cannot be removed without providing deprecation notice for at least single SPDK release.

Deprecated code paths must be registered with `SPDK_LOG_DEPRECATION_REGISTER()` and logged with
`SPDK_LOG_DEPRECATED()`. The tag used with these macros will appear in the SPDK
log at the warn level when `SPDK_LOG_DEPRECATED()` is called, subject to rate limits.
The tags can be matched with the level 4 headers below.

## Deprecation Notices

### bdev

#### `bdev_get_memory_domains`

`spdk_bdev_get_memory_domains` and the `get_memory_domains` fn_table entry are deprecated.
Use `spdk_bdev_get_memory_domain_types` and `get_memory_domain_types` instead.
Will be removed in the v26.09 release.

#### `bdev_nvme_set_multipath_policy`

The `spdk_bdev_nvme_set_multipath_policy` function and the `bdev_nvme_set_multipath_policy` RPC
are deprecated and will be removed in v26.09. Use `spdk_bdev_nvme_create()` with multipath
options, or the `multipath_opts` parameter in the `bdev_nvme_attach_controller` RPC instead.

### fsdev

The current `fsdev` layer and its consumers are deprecated in preparation for
replacing with a new implementation in the v26.09 release.

#### `fsdev`

The `fsdev` library, including the public APIs in `include/spdk/fsdev.h` and
`include/spdk/fsdev_module.h`, the `fsdev` event subsystem, and the `aio` fsdev module
(the `fsdev_aio_create`/`fsdev_aio_delete` RPCs and the `--with-aio-fsdev` configure
option) are deprecated and will be replaced in the v26.09 release.

#### `fuse_dispatcher`

The `fuse_dispatcher` library and its public API (`include/spdk/fuse_dispatcher.h`) are
deprecated and will be removed in the v26.09 release. There will be no replacement;
the new fsdev implementation will not require a fuse_dispatcher.

#### `vfu_virtio_create_fs_endpoint`

The `virtio-fs` vfu_device support, exposed via the `vfu_virtio_create_fs_endpoint` RPC,
is deprecated and will be removed in the v26.09 release.

### gpt

#### `old_gpt_guid`

Deprecated the SPDK partition type GUID `7c5222bd-8f5d-4087-9c00-bf9843c7b58c`. Partitions of this
type have bdevs created that are one block less than the actual size of the partition. Existing
partitions using the deprecated GUID can continue to use that GUID; support for the deprecated GUID
will remain in SPDK indefinitely, and will continue to exhibit the off-by-one bug so that on-disk
metadata layouts based on the incorrect size are not affected.

See GitHub issue [2801](https://github.com/spdk/spdk/issues/2801) for additional details on the bug.

New SPDK partition types should use GUID `6527994e-2c5a-4eec-9613-8f5944074e8b` which will create
a bdev of the correct size.

### nvme

#### nvme_spec.h

`spdk_nvme_cdata_ctratt`

Updated bit definitions to NVMe 2.3. Old bit names will be removed in the v26.09 release.

#### `nvme_cpl_without_opc`

`spdk_nvme_cpl_get_status_string`, `spdk_nvme_print_completion`, and
`spdk_nvme_qpair_print_completion` are deprecated and will be removed in v26.09.
Use `spdk_nvme_cpl_get_status_string_ext`, `spdk_nvme_print_completion_ext`, and
`spdk_nvme_qpair_print_completion_ext` instead. The new APIs accept the command opcode
to correctly distinguish fabric command-specific status codes from NVMe command-specific
status codes.

#### `nvme_ns_get_format_index`

`spdk_nvme_ns_get_format_index` is deprecated and will be removed in v27.01.
Use `spdk_nvme_ns_get_active_format_index` instead.

### nvmf

`spdk_nvmf_subsystem_create`, `spdk_nvmf_subsystem_set_sn`, `spdk_nvmf_subsystem_set_mn`,
`spdk_nvmf_subsystem_set_ana_reporting` are deprecated and will be removed in v26.09.
Use `spdk_nvmf_subsystem_create_ext` with subsystem options instead.

`spdk_nvmf_subsystem_get_sn`, `spdk_nvmf_subsystem_get_mn`, `spdk_nvmf_subsystem_get_max_nsid`,
`spdk_nvmf_subsystem_get_max_namespaces`, `spdk_nvmf_subsystem_get_ana_reporting`, `spdk_nvmf_subsystem_get_type`
are deprecated and will be removed in v26.09. Use `spdk_nvmf_subsystem_get_opts` instead.

#### `nvmf_create_subsystem_max_discard_size_kib`

The `max_discard_size_kib` parameter of `nvmf_create_subsystem` RPC is deprecated and will be
removed in v26.09. Use `dmrsl` instead.

#### `nvmf_create_subsystem_max_write_zeroes_size_kib`

The `max_write_zeroes_size_kib` parameter of `nvmf_create_subsystem` RPC is deprecated and will be
removed in v26.09. Use `wzsl` instead.

#### `nvmf_namespace_hide_metadata`

The `hide_metadata` parameter of `nvmf_subsystem_add_ns` RPC is deprecated and will be removed in
v26.09. Metadata visibility on a namespace is determined by the transport's `dif_insert_or_strip`
option: when any transport on the target has `dif_insert_or_strip` enabled, namespaces are opened
with metadata hidden so the bdev layer can own DIF generate/verify.

#### `nvmf_tgt_mixed_dif_insert_or_strip`

Adding multiple transports to the same target with disagreeing `dif_insert_or_strip` values is
deprecated and will be rejected starting in v26.09. All transports on a target must share the
same `dif_insert_or_strip` setting. Because `dif_insert_or_strip` is applied when a namespace is
added, a transport that disagrees with an already-attached namespace's setting will also be
rejected; create the transport (or set the desired value on an existing one) before adding
namespaces.

#### `nvmf_transport.h`

`struct spdk_nvmf_dif_info`, `struct spdk_nvmf_stripped_data`, `spdk_nvmf_request_get_dif_ctx`,
and the `dif`, `dif_enabled`, and `stripped_data` fields of `struct spdk_nvmf_request` are
deprecated and will be removed in v26.09. DIF handling has moved to the bdev layer via the
namespace's `hide_metadata` open flag, so transports no longer touch DIF context directly. As a
side effect, the POSIX sock impl without a DIF-capable accelerator now incurs one extra data copy
per I/O on the DIF path (the old in-place transport-side path is gone).

### sock

#### Zero Copy Receive API Removals

`spdk_sock_group_poll_count`, `spdk_sock_recv_next`, `spdk_sock_group_provide_buf` and
`spdk_sock_group_get_buf` are deprecated and will be removed in v26.09. A new zero copy
API will replace them.

#### `spdk_sock_group_add_sock`, `spdk_sock_group_create`

The `cb_fn` and `cb_arg` parameters of `spdk_sock_group_add_sock` are deprecated and will be
removed in v26.09. Instead, pass `cb_fn` and `cb_arg` via the new `spdk_sock_group_opts` struct
passed to `spdk_sock_group_create`.

#### `spdk_sock_connect`, `spdk_sock_connect_ext`, `spdk_sock_connect_async`

These 3 APIs will be collapsed into a single replacement starting in v26.09.

#### `spdk_sock_listen`, `spdk_sock_listen_ext`

These two APIs will be collapsed into a single replacement starting in v26.09.

#### `nvmf_create_transport`

buf-cache-size parameter is deprecated in favor of iobuf-small-cache-size and will be removed in 26.09 release.
num-shared-buffers parameter is deprecated and will be removed in 26.09 release. Instead the user can use
`iobuf_set_options` to specify the number of small and large pool entries and use `iobuf-small-cache-size` and
`iobuf-large-cache-size` parameters of `nvmf_create_transport` RPC to configure desired buffers caches.
io-unit-size is NOP, it is deprecated and will be removed in v26.09 release.
