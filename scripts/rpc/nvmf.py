from client import print_dict, print_array, int_arg


def get_nvmf_subsystems(args):
    print_dict(args.client.call('get_nvmf_subsystems', verbose=args.verbose))


def construct_nvmf_subsystem(args):
    params = {
        'nqn': args.nqn,
        'listen_addresses': [],
        'hosts': [],
        'serial_number': args.serial_number,
        'namespaces': [],
    }

    args.client.call('construct_nvmf_subsystem', params, verbose=args.verbose)


def nvmf_subsystem_add_listener(args):
    params = {'subnqn': args.nqn,
              'trtype': args.trtype,
              'traddr': args.traddr,
              'trsvcid': args.trsvcid}

    if args.adrfam:
        params['adrfam'] = args.adrfam

    args.client.call('nvmf_subsystem_add_listener', params)


def nvmf_subsystem_add_host(args):
    params = {'subnqn': args.subnqn,
              'hostnqn': args.hostnqn}

    args.client.call('nvmf_subsystem_add_host', params)


def nvmf_subsystem_add_host(args):
    params = {'subnqn': args.subnqn,
              'bdev_name': args.bdev_name}

    if args.nsid:
        params['nsid'] = args.nsid

    args.client.call('nvmf_subsystem_add_nsid', params)


def delete_nvmf_subsystem(args):
    params = {'nqn': args.subsystem_nqn}
    args.client.call('delete_nvmf_subsystem', params, verbose=args.verbose)
