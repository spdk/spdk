# System Configuration User Guide {#system_configuration}

This system configuration guide describes how to configure a system for use with SPDK.

# IOMMU configuration {#iommu_config}

An IOMMU may be present and enabled on many platforms. When an IOMMU is present and enabled, it is
recommended that SPDK applications are deployed with the `vfio-pci` kernel driver. SPDK's
`scripts/setup.sh` script will automatically select `vfio-pci` in this case.

However, some devices do not function correctly when bound to `vfio-pci` and instead must be
attached to the `uio_pci_generic` kernel driver. In that case, users should take care to disable
the IOMMU or to set it into passthrough mode prior to running `scripts/setup.sh`.

To disable the IOMMU or place it into passthrough mode, add `intel_iommu=off`
or `amd_iommu=off` or `intel_iommu=on iommu=pt` to the GRUB command line on
x86_64 system, or add `iommu.passthrough=1` on arm64 systems.
