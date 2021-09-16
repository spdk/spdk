# distributions {#distributions}

## In this document {#distros_toc}

* @ref distros_overview
* @ref linux_list
* @ref freebsd_list

## Overview {#distros_overview}

CI pool uses different flavors of `Linux` and `FreeBSD` distributions which are
used as a base for all the tests run against submitted patches. Below is the
listing which covers all currently supported versions and the related CI
jobs (see [status](https://ci.spdk.io) as a reference).

## Linux distributions {#linux_list}

* Fedora: Trying to follow new release as per the release cycle whenever
          possible. Currently at `Fedora33`.

```list
- autobuild-vg-autotest
- clang-vg-autotest
- iscsi*-vg-autotest
- nvme-vg-autotest
- nvmf*-vg-autotest
- scanbuild-vg-autotest
- unittest-vg-autotest
- vhost-initiator-vg-autotest
```

Jobs listed below are run on bare-metal systems where version of
Fedora may vary. In the future these will be aligned with the
`vg` jobs.

```list
- BlobFS-autotest
- crypto-autotest
- nvme-phy-autotest
- nvmf*-phy-autotest
- vhost-autotest
```

* Ubuntu: Last two LTS releases. Currently `20.04` and `18.04`.

```list
- ubuntu18-vg-autotest
- ubuntu20-vg-autotest
```

* CentOS: Maintained releases. Currently `7.9` and `8.2`.

```list
- centos7-vg-autotest
- centos8-vg-autotest
```

## FreeBSD distributions {#freebsd_list}

* FreeBSD: Production release. Currently `12.2`.

```list
- freebsd-vg-autotest
```
