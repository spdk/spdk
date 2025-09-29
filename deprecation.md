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

#### `bdev_get_iostat`

`--name` option will be removed in the 26.05 release. `--names` option should be used instead,
it allows providing array of devices names to obtain statistics from.

### python/cli

#### `framework_monitor_context_switch`

deprecate `--enable` and `--disable` in favor of single `--monitor/--no-monitor` option

#### `bdev_set_options`

deprecate `--enable-auto-examine` and `--disable-auto-examine` in favor of single `--auto-examine/--no-auto-examine` option
deprecate `--enable-rdma-umr-per-io` and `--disable-rdma-umr-per-io` in favor of single `--rdma-umr-per-io/--no-rdma-umr-per-io` option

#### `bdev_nvme_set_hotplug`

deprecate `--enable` and `--disable` in favor of single `--hotplug/--no-hotplug` option

#### `bdev_enable_histogram`

deprecate `--enable` and `--disable` in favor of single `--histogram/--no-histogram` option

#### `fsdev_aio_create`

deprecate `--enable-xattr` and `--disable-xattr` in favor of single `--xattr/--no-xattr` option
deprecate `--enable-writeback-cache` and `--disable-writeback-cache` in favor of single `--writeback-cache/--no-writeback-cache` option

#### `iscsi_enable_histogram`

deprecate `--enable` and `--disable` in favor of single `--histogram/--no-histogram` option

#### `log_enable_timestamps`

deprecate `--enable` and `--disable` in favor of single `--timestamps/--no-timestamps` option

#### `nvmf_subsystem_allow_any_host`

deprecate `--enable` and `--disable` in favor of single `--allow-any-host/--no-allow-any-host` option

#### `sock_impl_set_options`

deprecate `--enable-recv-pipe` and `--disable-recv-pipe` in favor of single `--recv-pipe/--no-recv-pipe` option
deprecate `--enable-quickack` and `--disable-quickack` in favor of single `--quickack/--no-quickack` option
deprecate `--enable-zerocopy-send-server` and `--disable-zerocopy-send-server` in favor of single `--zerocopy-send-server/--no-zerocopy-send-server` option
deprecate `--enable-zerocopy-send-client` and `--disable-zerocopy-send-client` in favor of single `--zerocopy-send-client/--no-zerocopy-send-client` option
deprecate `--enable-ktls` and `--disable-ktls` in favor of single `--ktls/--no-ktls` option

#### `bdev_virtio_blk_set_hotplug`

deprecate `--enable` and `--disable` in favor of single `--hotplug/--no-hotplug` option

### util/net

#### `spdk_net_getaddr`

Returning -1 and setting errno on this function is deprecated and will be changed in the 26.01
release. This function will return negative errno values instead.

### sock

#### `spdk_sock_\*`

`spdk_sock_getaddr`, `spdk_sock_close`, `spdk_sock_flush`, `spdk_sock_recv`, `spdk_sock_writev`,
`spdk_sock_readv`, `spdk_sock_recv_next`, `spdk_sock_set_recvlowat`, `spdk_sock_set_recvbuf`,
`spdk_sock_set_sendbuf`, `spdk_sock_group_add_sock`, `spdk_sock_group_remove_sock`,
`spdk_sock_group_provide_buf`, `spdk_sock_group_poll`, `spdk_sock_group_poll_count`,
`spdk_sock_group_close`, `spdk_sock_impl_get_opts`, `spdk_sock_impl_set_opts`,
`spdk_sock_set_default_impl`, `spdk_sock_group_register_interrupt`

Returning -1 and setting errno on these functions is deprecated and will be changed in the 26.01
release. These functions will return negative errno values instead.

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

`spdk_nvme_ctrlr_data`, `spdk_nvme_cdata_oacs`

LPA and OACS bits are updated to NVMe 2.2 definitions. The old bit names will be removed in 26.05 release.
