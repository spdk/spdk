# Deprecation

## ABI and API Deprecation {#deprecation}

This document details the policy for maintaining stability of SPDK ABI and API.

Major ABI version can change at most once for each quarterly SPDK release.
ABI versions are managed separately for each library and follow [Semantic Versioning](https://semver.org/).

API and ABI deprecation notices shall be posted in the next section.
Each entry must describe what will be removed and can suggest the future use or alternative.
Specific future SPDK release for the removal must be provided.
ABI cannot be removed without providing deprecation notice for at least single SPDK release.

Deprecated code paths must be registered with `SPDK_DEPRECATION_REGISTER()` and logged with
`SPDK_LOG_DEPRECATED()`. The tag used with these macros will appear in the SPDK
log at the warn level when `SPDK_LOG_DEPRECATED()` is called, subject to rate limits.
The tags can be matched with the level 4 headers below.

## Deprecation Notices {#deprecation-notices}

### VTune

VTune integration is in now deprecated and will be removed in SPDK 23.05.

### nvme

#### `nvme_ctrlr_prepare_for_reset`

Deprecated `spdk_nvme_ctrlr_prepare_for_reset` API, which will be removed in SPDK 22.01.
For PCIe transport, `spdk_nvme_ctrlr_disconnect` should be used before freeing I/O qpairs.

### bdev

#### `bdev_register_examine_thread`

Deprecated calling `spdk_bdev_register()` and `spdk_bdev_examine()` from a thread other than the
app thread. See `spdk_thread_get_app_thread()`. Starting in SPDK 23.05, calling
`spdk_bdev_register()` or `spdk_bdev_examine()` from a thread other than the app thread will return
an error.

With the removal of this deprecation, calls to vbdev modules' `examine_disk()` and
`examine_config()` callbacks will happen only on the app thread. This means that vbdev module
maintainers will not need to make any changes to examine callbacks that call `spdk_bdev_register()`
on the same thread as the examine callback uses.

### gpt

### `old_gpt_guid`

Deprecated the SPDK partition type GUID `7c5222bd-8f5d-4087-9c00-bf9843c7b58c`. Partitions of this
type have bdevs created that are one block less than the actual size of the partition. Existing
partitions using the deprecated GUID can continue to use that GUID; support for the deprecated GUID
will remain in SPDK indefinitely, and will continue to exhibit the off-by-one bug so that on-disk
metadata layouts based on the incorrect size are not affected.

See GitHub issue [2801](https://github.com/spdk/spdk/issues/2801) for additional details on the bug.

New SPDK partition types should use GUID `6527994e-2c5a-4eec-9613-8f5944074e8b` which will create
a bdev of the correct size.
