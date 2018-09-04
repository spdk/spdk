# System Configuration User Guide {#system_configuration}

The system configuration guide is intended for the SPDK target system
configuration in user side.

## IOMMU configuration {#iommu_config}

IOMMU is enabled in many platforms, it is recommended that applications are
deployed using `vfio-pci` kernel driver.

But if the devices must be charged by the `uio_pci_generic` kernel driver
in system, users should make sure that the IOMMU is disabled or passthrough.
Otherwise the DMA transmission of the device will be invalid because IOMMU
charges the memory mapping of the `virtual` address and the `physical`
address.

To disable or passthrough IOMMU in system, users can add `intel_iommu=off`
or `amd_iommu=off` or `intel_iommu=on iommu=pt` in GRUB command line on
x86_64 system, or add `iommu.passthrough=1` on arm64 system.
