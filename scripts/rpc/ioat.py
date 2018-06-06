def scan_ioat_copy_engine(client, pci_whitelist):
    """Scan and enable IOAT copy engine.

    Args:
        pci_whitelist: Whitespace-separated list of PCI addresses in
                       domain:bus:device.function format or
                       domain.bus.device.function format
    """
    params = {}
    if pci_whitelist:
        _pci_whitelist = []
        for w in pci_whitelist.strip().split(" "):
            _pci_whitelist.append(w)
        params['pci_whitelist'] = _pci_whitelist
    return client.call('scan_ioat_copy_engine', params)
