from .helpers import deprecated_alias


@deprecated_alias('ioat_scan_copy_engine')
@deprecated_alias('scan_ioat_copy_engine')
def ioat_scan_accel_engine(client, pci_whitelist):
    """Scan and enable IOAT accel engine.

    Args:
        pci_whitelist: Python list of PCI addresses in
                       domain:bus:device.function format or
                       domain.bus.device.function format
    """
    params = {}
    if pci_whitelist:
        params['pci_whitelist'] = pci_whitelist
    return client.call('ioat_scan_accel_engine', params)
