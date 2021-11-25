# System Configuration User Guide {#system_configuration}

This system configuration guide describes how to configure a system for use with SPDK.

## IOMMU configuration {#iommu_config}

An IOMMU may be present and enabled on many platforms. When an IOMMU is present and enabled, it is
recommended that SPDK applications are deployed with the `vfio-pci` kernel driver. SPDK's
`scripts/setup.sh` script will automatically select `vfio-pci` in this case.

However, some devices do not function correctly when bound to `vfio-pci` and instead must be
attached to the `uio_pci_generic` kernel driver. In that case, users should take care to disable
the IOMMU or to set it into passthrough mode prior to running `scripts/setup.sh`.

To disable the IOMMU or place it into passthrough mode, add `intel_iommu=off`
or `amd_iommu=off` or `intel_iommu=on iommu=pt` to the GRUB command line on
x86_64 system, or add `iommu.passthrough=1` on arm64 systems.

There are also some instances where a user may not want to use `uio_pci_generic` or the kernel
version they are using has a bug where `uio_pci_generic` [fails to bind to NVMe drives](https://github.com/spdk/spdk/issues/399).
In these cases, users can build the `igb_uio` kernel module which can be found in dpdk-kmods repository.
To ensure that the driver is properly bound, users should specify `DRIVER_OVERRIDE=/path/to/igb_uio.ko`.

## Running SPDK as non-privileged user {#system_configuration_nonroot}

One of the benefits of using the `VFIO` Linux kernel driver is the ability to
perform DMA operations with peripheral devices as unprivileged user. The
permissions to access particular devices still need to be granted by the system
administrator, but only on a one-time basis. Note that this functionality
is supported with DPDK starting from version 18.11.

### Hugetlbfs access

Make sure the target user has RW access to at least one hugepage mount.
A good idea is to create a new mount specifically for SPDK:

~~~{.sh}
# mkdir /mnt/spdk_hugetlbfs
# mount -t hugetlbfs -o uid=spdk,size=<value> none /mnt/spdk_hugetlbfs
~~~

Then start SPDK applications with an additional parameter `--huge-dir /mnt/spdk_hugetlbfs`

Full guide on configuring hugepage mounts is available in the
[Linux Hugetlbpage Documentation](https://www.kernel.org/doc/Documentation/vm/hugetlbpage.txt)

### Device access {#system_configuration_nonroot_device_access}

`VFIO` device access is protected with sysfs file permissions and can be
configured with chown/chmod.

Please note that the VFIO device isolation is based around IOMMU groups and it's
only possible to change permissions of the entire group, which might possibly
consist of more than one device. (You could also apply a custom kernel patch to
further isolate those devices in the kernel, but it comes with potential risks
as described on
[Alex Williamson's VFIO blog](https://vfio.blogspot.com/2014/08/iommu-groups-inside-and-out.html),
with the patch in question available here:
[[PATCH] pci: Enable overrides for missing ACS capabilities](https://lkml.org/lkml/2013/5/30/513))

Let's assume we want to use PCI device `0000:04:00.0`. First of all, verify
that it has an IOMMU group assigned:

~~~{.sh}
readlink "/sys/bus/pci/devices/0000:00:04.0/iommu_group"
~~~

The output should be e.g.
`../../../kernel/iommu_groups/5`

Which means that the device is a part of the IOMMU group 5. We can check if
there are any other devices in that group.

~~~{.sh}
$ ls /sys/kernel/iommu_groups/5/devices/
0000:00:04.0  0000:00:04.1  0000:00:04.2  0000:00:04.3  0000:00:04.4  0000:00:04.5  0000:00:04.6  0000:00:04.7
~~~

In this case `0000:04:00.0` is an I/OAT channel which comes with 7 different
channels associated with the same IOMMU group.

To give the user `spdk` full access to the VFIO IOMMU group 5 and all its
devices, use the following:

~~~{.sh}
# chown spdk /dev/vfio/5
~~~

### Memory constraints {#system_configuration_nonroot_memory_constraints}

As soon as the first device is attached to SPDK, all of SPDK memory will be
mapped to the IOMMU through the VFIO APIs. VFIO will try to mlock that memory and
will likely exceed user ulimit on locked memory. Besides having various
SPDK errors and failures, this would also pollute the syslog with the following
entries:

`vfio_pin_pages: RLIMIT_MEMLOCK`

The limit can be checked by running the following command as target user:
(output in kilobytes)

~~~{.sh}
ulimit -l
~~~

On Ubuntu 18.04 this returns 16384 (16MB) by default, which is way below
what SPDK needs.

The limit can be increased with one of the methods below. Keep in mind SPDK will
try to map not only its reserved hugepages, but also all the memory that's
shared by its vhost clients as described in the
[Vhost processing guide](https://spdk.io/doc/vhost_processing.html#vhost_processing_init).

#### Increasing the memlock limit permanently

Open the `/etc/security/limits.conf` file as root and append the following:

```bash
spdk     hard   memlock           unlimited
spdk     soft   memlock           unlimited
```

Then logout from the target user account. The changes will take effect after the next login.

#### Increasing the memlock for a specific process

Linux offers a `prlimit` utility that can override limits of any given process.
On Ubuntu, it is a part of the `util-linux` package.

~~~{.sh}
# prlimit --pid <pid> --memlock=<soft>:<hard>
~~~

Note that the above needs to be executed before the first device is attached to
the SPDK application.
