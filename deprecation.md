# ABI and API Deprecation {#deprecation}

This document details the policy for maintaining stability of SPDK ABI and API.

Major ABI version can change at most once for each quarterly SPDK release.
ABI versions are managed separately for each library and follow [Semantic Versoning](https://semver.org/).

API and ABI deprecation notices shall be posted in the next section.
Each entry must describe what will be removed and can suggest the future use or alternative.
Specific future SPDK release for the removal must be provided.
ABI cannot be removed without providing deprecation notice for at least single SPDK release.

# Deprecation Notices {#deprecation-notices}

## nvmf

The following APIs have been deprecated and will be removed in SPDK 21.07:
- `spdk_nvmf_poll_group_get_stat` (function in `nvmf.h`),
- `spdk_nvmf_transport_poll_group_get_stat` (function in `nvmf.h`),
- `spdk_nvmf_transport_poll_group_free_stat`(function in `nvmf.h`),
- `spdk_nvmf_rdma_device_stat` (struct in `nvmf.h`),
- `spdk_nvmf_transport_poll_group_stat` (struct in `nvmf.h`),
- `poll_group_get_stat` (transport op in `nvmf_transport.h`),
- `poll_group_free_stat` (transport op in `nvmf_transport.h`).
Please use `spdk_nvmf_poll_group_dump_stat` and `poll_group_dump_stat` instead.

## rpc

Parameter `enable-zerocopy-send` of RPC `sock_impl_set_options` is deprecated and will be removed in SPDK 21.07,
use `enable-zerocopy-send-server` or `enable-zerocopy-send-client` instead.
Parameter `disable-zerocopy-send` of RPC `sock_impl_set_options` is deprecated and will be removed in SPDK 21.07,
use `disable-zerocopy-send-server` or `disable-zerocopy-send-client` instead.

## rpm

`pkg/spdk.spec` is considered to be deprecated and scheduled for removal in SPDK 21.07.
Please use `rpmbuild/spdk.spec` instead and see
[RPM documentation](https://spdk.io/doc/rpm.html) for more details.
