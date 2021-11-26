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

### bdev

Deprecated `spdk_bdev_module_finish_done()` API, which will be removed in SPDK 22.01.
Bdev modules should use `spdk_bdev_module_fini_done()` instead.

### nvme

Deprecated `spdk_nvme_ctrlr_reset_async` and `spdk_nvme_ctrlr_reset_poll_async` APIs,
which will be removed in SPDK 22.01. `spdk_nvme_ctrlr_disconnect`, `spdk_nvme_ctrlr_reconnect_async`,
and `spdk_nvme_ctrlr_reconnect_poll_async` should be used instead.
