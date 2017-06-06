from client import print_dict, print_array, int_arg


def get_nvmf_subsystems(args):
    print_dict(args.client.call('get_nvmf_subsystems'))


def construct_nvmf_subsystem(args):
    listen_addresses = [dict(u.split(":") for u in a.split(" "))
                        for a in args.listen.split(",")]

    params = {
        'core': args.core,
        'mode': args.mode,
        'nqn': args.nqn,
        'listen_addresses': listen_addresses,
        'serial_number': args.serial_number,
    }

    if args.hosts:
        hosts = []
        for u in args.hosts.strip().split(" "):
            hosts.append(u)
        params['hosts'] = hosts

    if args.namespaces:
        namespaces = []
        for u in args.namespaces.strip().split(" "):
            namespaces.append(u)
        params['namespaces'] = namespaces

    if args.pci_address:
        params['pci_address'] = args.pci_address

    args.client.call('construct_nvmf_subsystem', params)


def delete_nvmf_subsystem(args):
    params = {'nqn': args.subsystem_nqn}
    args.client.call('delete_nvmf_subsystem', params)
