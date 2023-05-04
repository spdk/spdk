# Deprecation

## ABI and API Deprecation

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

## Deprecation Notices

### PMDK

PMDK is no longer supported and integrations with it in SPDK are now deprecated, and will be removed in SPDK 23.05.
Please see: [UPDATE ON PMDK AND OUR LONG TERM SUPPORT STRATEGY](https://pmem.io/blog/2022/11/update-on-pmdk-and-our-long-term-support-strategy/).

### VTune

#### `vtune_support`

VTune integration is in now deprecated and will be removed in SPDK 23.05.

### nvmf

#### `spdk_nvmf_qpair_disconnect`

Parameters `cb_fn` and `ctx` of `spdk_nvmf_qpair_disconnect` API are deprecated. These parameters
will be removed in 23.09 release.

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

### lvol

#### `vbdev_lvol_rpc_req_size`

Param `size` in rpc commands `rpc_bdev_lvol_create` and `rpc_bdev_lvol_resize` is deprecated and
replace by `size_in_mib`.

See GitHub issue [2346](https://github.com/spdk/spdk/issues/2346) for additional details.
