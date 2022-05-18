# Deprecation

## ABI and API Deprecation {#deprecation}

This document details the policy for maintaining stability of SPDK ABI and API.

Major ABI version can change at most once for each quarterly SPDK release.
ABI versions are managed separately for each library and follow [Semantic Versioning](https://semver.org/).

API and ABI deprecation notices shall be posted in the next section.
Each entry must describe what will be removed and can suggest the future use or alternative.
Specific future SPDK release for the removal must be provided.
ABI cannot be removed without providing deprecation notice for at least single SPDK release.

## Deprecation Notices {#deprecation-notices}

### nvme

Deprecated `spdk_nvme_ctrlr_prepare_for_reset` API, which will be removed in SPDK 22.01.
For PCIe transport, `spdk_nvme_ctrlr_disconnect` should be used before freeing I/O qpairs.
