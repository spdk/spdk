# System Configuration User Guide {#system_config}

The system configuration guide is intended for the SPDK target system
configuration in user side.

# IOMMU configuration {#system_config_iommu}

IOMMU is enabled in many platforms, it is recommended that SPDK applications
are deployed with PCI devices attached to the `vfio-pci` kernel driver.

But if the devices must be attached to the `uio_pci_generic` kernel driver
in the system, users should make sure that the IOMMU is disabled or passthrough.
Otherwise the DMA transmission of the device will be invalid because IOMMU
changes the memory mapping of the `virtual` address and the `physical`
address.

To disable or passthrough IOMMU in system, users can add `intel_iommu=off`
or `amd_iommu=off` or `intel_iommu=on iommu=pt` in GRUB command line on
x86_64 system, or add `iommu.passthrough=1` on arm64 system.

# Running SPDK as non-priviledged user {#system_config_nonroot}

One of the benefits of using the `VFIO` Linux kernel driver is the ability to
perform DMA operations with peripheral devices as a common system user. The
permissions to access particular devices still need to be granted by the system
administrator, but only on a one-time basis.

BSD doesn't provide such functionality yet.

## DPDK setup {#system_config_nonroot_dpdk}

First of all, running SPDK as a non-priviledged user is still an ongoing effort
and needs an out-of-tree patch in the DPDK library to work at all. The patch
should be no longer needed with DPDK 18.11, but for earlier versions we require
to apply it from the following source:

`$ git fetch x && git cherry-pick y`

## Device access {#system_config_nonroot_device_access}

`VFIO` device access is protected with simple sysfs file permissions. The
following command gives the user `spdk` an access to PCI device `04:00.0`.

`$ chown spdk /sys/bus/pci/devices/<todo>`

## Hugetlbfs access

Make sure the hugepage mount has RW access to at least one hugepage mount.
A good idea is to create a new mount specifically for SPDK:

`# mount -t hugetlbfs -o uid=spdk,size=<value> none /mnt/spdk_hugetlbfs`

SPDK will use it automatically. No additional configuration is required.

Full guide on configuring hugepage mounts is available in the
[Linux Hugetlbpage Documentation](https://www.kernel.org/doc/Documentation/vm/hugetlbpage.txt)

## Memory constraints {#system_config_nonroot_memory_constraints}

As soon as the first device is attached to SPDK, all SPDK's memory will be
mapped via the VFIO APIs. VFIO will try to mlock that memory and will likely
exceed the user's ulimit on locked memory. Besides having various SPDK errors
and failures, this would also pollute the syslog with following entries:

`vfio_pin_pages: RLIMIT_MEMLOCK (16384)`

The limit can be checked by running the following command as the target user:
(output in kilobytes)

`$ ulimit -l`

On Ubuntu 18.04 this returns 16384 (16MB) by default, which is way below of
what SPDK needs.

As a brief introduction, ulimit is a standard POSIX tool which defines two
types of limits - hard limit and soft limit. A hard limit is the absolute
maximum that can be only raised by root. Non-root users can only restrict
themselves further by setting another - soft - limit. In this guide we'll
modify both hard and soft limit at the same time.

The limit can be increased with one the methods below. Keep in mind SPDK will
try to map not only its reserved hugepages, but also all the memory that's
shared by its vhost clients as described in the [Vhost processing guide](https://spdk.io/doc/vhost_processing.html#vhost_processing_init).

### Increasing the memlock limit permanently

Open the `/etc/security/limits.conf` file as root and append the following:

```
spdk     hard   memlock           unlimited
spdk     soft   memlock           unlimited
```

Then logout the target user. The changes will take effect after the next login.

### Increasing the memlock for a specific process

Linux offers a `prlimit` utility that can override limits of any given process.
On Ubuntu, it is a part of the `util-linux` package.

`# prlimit --pid <pid> --memlock=<soft>:<hard>`

Note that the above needs to be executed before the first device is attached to
the SPDK application.
