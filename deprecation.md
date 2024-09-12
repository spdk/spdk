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

### nvme

#### `spdk_nvme_accel_fn_table.submit_accel_crc32c`

This callback is now deprecated and will be removed in the v25.01 release.  Please use the append
API (`append_crc32c`, `finish_sequence`, `reverse_sequence`, `abort_sequence`) instead.

#### `multipath_config` `failover_config`

All controllers created with the same name shall be configured either for multipath or for failover.
Otherwise we have configuration mismatch. Currently, using bdev_nvme_attach_controller RPC call, it
is possible to create multiple controllers with the same name but different '-x' options. Starting
from 25.01 we are going to perform controller configuration consistency check, and all controllers
created with the same name will be forced to have consistent setting, either '-x multipath' or
'-x failover'. No mixing of '-x' options will be allowed.
Please also note there is planned default mode change in SPDK release 25.01: if no '-x' option will
be specified in bdev_nvme_attach_controller RPC call, the multipath mode will be assigned
as a default.

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

### rpc

#### `spdk_rpc_listen` `spdk_rpc_accept` `spdk_rpc_close`

These functions are deprecated and will be removed in 24.09 release. Please use
`spdk_rpc_server_listen`, `spdk_rpc_server_accept` and `spdk_rpc_server_close` instead.

### env

#### `spdk_env_get_socket_id`, `spdk_pci_device_get_socket_id`

These functions are deprecated and will be removed in 25.05 release. Please use
`spdk_env_get_numa_id` and `spdk_pci_device_get_numa_id` instead.
