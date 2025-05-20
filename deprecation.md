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

### sock

#### `spdk_sock_flush`

This function returnes number of bytes sent on success, whereas this behavior is deprecated and
will be changed in 25.09 release in the way it will return 0 on success.

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

### env

#### `spdk_env_get_socket_id`, `spdk_pci_device_get_socket_id`

These functions are deprecated and will be removed in 25.09 release. Please use
`spdk_env_get_numa_id` and `spdk_pci_device_get_numa_id` instead.

### reduce

#### 'spdk_reduce_vol_init', 'spdk_reduce_vol_load'

The entire reduce library is deprecated and will be removed in 25.09 release.
All functions in this library are effectively deprecated, but only these two
are officially marked as such to ensure the library's deprecation is noticed.

### bdev_compress

#### 'bdev_compress_create', 'bdev_compress_delete', 'bdev_compress_get_orphans' RPCs

The entire bdev compress module is deprecated and will be removed in 25.09
release. The C module exports no public APIs, so none are listed here, but
the module will emit deprecation warnings when usage is detected.

### blobfs

#### 'spdk_fs_init', 'spdk_fs_load', 'blobfs_\*' RPCs

This entire blobfs library is deprecated and will be removed in 25.09 release.
All functions in this library are effectively deprecated, but only these two
are officially marked as such to ensure the library's deprecation is noticed.

### rocksdb

The SPDK rocksdb plugin is deprecated and will be removed in 25.09 release.
This C++ plugin exports no public APIs, so none are listed here, but
the pluging will emit deprecation warnings when usage is detected.
