def add_memcpy_copy_engine(args):
    return args.client.call('add_memcpy_copy_engine')


def add_ioat_copy_engine(args):
    whitelist = []
    for w in args.pci_whitelist.strip().split(" "):
        whitelist.append(w)

    params = {
        'pci_whitelist': whitelist,
    }

    if args.disable:
        params['disable'] = args.disable
    return args.client.call('add_ioat_copy_engine', params)
