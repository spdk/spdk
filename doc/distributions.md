# distributions {#distributions}

## In this document {#distros_toc}

* @ref distros_overview
* @ref linux_list

## Overview {#distros_overview}

CI pool uses Fedora Linux as primary OS distribution to run SPDK tests.
Below is the listing which covers all currently supported versions and the
related CI jobs.

See [spdk/spdk-ci](https://github.com/spdk/spdk-ci) for the most up-to-date
reference for SPDK CI configuration.

## Linux distributions {#linux_list}

### Fedora

## Fedora 40

Fedora 40 is used to run virtualized and containerized tests in
Github Actions workflows.

Tests run using Fedora 40 in virtualized environment:
```list
- bdev-vm-autotest
- ftl-vm-autotest
- nvme-vm-autotest
- nvmf-tcp-uring-vm-autotest
- nvmf-tcp-vm-autotest
- raid-vm-autotest
```

Tests run using Fedora 40 in containerized environment:
```list
- build-files-container-autotest
- check-format-container-autotest
- check-so-deps-container-autotest
- doc-container-autotest
- release-build-gcc-container-autotest
- scan-build-container-autotest
- unittest-gcc-container-autotest
```

## Fedora 39

Fedora 39 is used to run tests in virtualized environment, but
using actual, physical hardware passed into virtual machines.

Tests run using Fedora 39:
```list
- hpe-nvmf-rdma
```
