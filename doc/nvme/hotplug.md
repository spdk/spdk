# NVMe Hotplug {#nvme_hotplug}

At the NVMe driver level, we provide the following support for Hotplug:

1. Hotplug events detection:
The user of the NVMe library can call spdk_nvme_probe() periodically to detect
hotplug events. The probe_cb, followed by the attach_cb, will be called for each
new device detected. The user may optionally also provide a remove_cb that will be
called if a previously attached NVMe device is no longer present on the system.
All subsequent I/O to the removed device will return an error.

2. Hot remove NVMe with IO loads:
When a device is hot removed while I/O is occurring, all access to the PCI BAR will
result in a SIGBUS error. The NVMe driver automatically handles this case by installing
a SIGBUS handler and remapping the PCI BAR to a new, placeholder memory location.
This means I/O in flight during a hot remove will complete with an appropriate error
code and will not crash the application.

@sa spdk_nvme_probe
