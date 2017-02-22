# NVMe over Fabrics Host Support {#nvme_fabrics_host}

The NVMe driver supports connecting to remote NVMe-oF targets and
interacting with them in the same manner as local NVMe controllers.

# Specifying Remote NVMe over Fabrics Targets {#nvme_fabrics_trid}

The method for connecting to a remote NVMe-oF target is very similar
to the normal enumeration process for local PCIe-attached NVMe devices.
To connect to a remote NVMe over Fabrics subsystem, the user may call
spdk_nvme_probe() with the `trid` parameter specifying the address of
the NVMe-oF target.
The caller may fill out the spdk_nvme_transport_id structure manually
or use the spdk_nvme_transport_id_parse() function to convert a
human-readable string representation into the required structure.

The spdk_nvme_transport_id may contain the address of a discovery service
or a single NVM subsystem.  If a discovery service address is specified,
the NVMe library will call the spdk_nvme_probe() `probe_cb` for each
discovered NVM subsystem, which allows the user to select the desired
subsystems to be attached.  Alternatively, if the address specifies a
single NVM subsystem directly, the NVMe library will call `probe_cb`
for just that subsystem; this allows the user to skip the discovery step
and connect directly to a subsystem with a known address.
