def scan_ioat_copy_engine(client, args):
    params = {}
    if args.pci_whitelist:
        pci_whitelist = []
        for w in args.pci_whitelist.strip().split(" "):
            pci_whitelist.append(w)
        params['pci_whitelist'] = pci_whitelist
    return client.call('scan_ioat_copy_engine', params)
