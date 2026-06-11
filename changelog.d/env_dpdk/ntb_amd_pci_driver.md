Registered a PCI driver for the AMD EPYC 4th-Generation Non-Transparent Bridge
(NTB). Its handle is obtained through the new `spdk_pci_ntb_amd_get_driver()`,
and the NTB primary/secondary device IDs are mapped to the "ntb" device type in
the setup scripts so the devices are bound and reported for use by the ntb_amd
driver.
