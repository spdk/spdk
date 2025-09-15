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
