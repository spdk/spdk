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

#### `bdev_get_iostat`

`--name` option will be removed in the 26.05 release. `--names` option should be used instead,
it allows providing array of devices names to obtain statistics from.

### bdev/nvme

The `BDEV_NVME_MP_POLICY_ACTIVE_PASSIVE`, `BDEV_NVME_MP_POLICY_ACTIVE_ACTIVE`,
`BDEV_NVME_MP_SELECTOR_ROUND_ROBIN`, and `BDEV_NVME_MP_SELECTOR_QUEUE_DEPTH` enum
value names are deprecated and will be removed in v25.09. Use the
`SPDK_BDEV_NVME_MULTIPATH_POLICY_*` and `SPDK_BDEV_NVME_MULTIPATH_SELECTOR_*`
names instead.

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

### app/spdk_nvme_perf

#### perf_g_option

The `-G` command line option is deprecated and will be removed in the v26.05 release.
Use `--log-level debug -T nvme` instead.
