from client import print_dict, print_array, int_arg


def get_nvmf_subsystems(args):
    print_dict(args.client.call('get_nvmf_subsystems', verbose=args.verbose))


def construct_nvmf_subsystem(args):
    params = {
        'nqn': args.nqn,
        'listen_addresses': [],
        'serial_number': args.serial_number,
    }

    if args.hosts:
        hosts = []
        for u in args.hosts.strip().split(" "):
            hosts.append(u)
        params['hosts'] = hosts

    if args.allow_any_host:
        params['allow_any_host'] = True

    if args.namespaces:
        namespaces = []
        for u in args.namespaces.strip().split(" "):
            bdev_name = u
            nsid = 0
            if ':' in u:
                (bdev_name, nsid) = u.split(":")

            ns_params = {'bdev_name': bdev_name}

            nsid = int(nsid)
            if nsid != 0:
                ns_params['nsid'] = nsid

            namespaces.append(ns_params)
        params['namespaces'] = namespaces

    args.client.call('construct_nvmf_subsystem', params, verbose=args.verbose)


def nvmf_subsystem_add_listener(args):
    params = {'subnqn': args.nqn,
              'trtype': args.trtype,
              'traddr': args.traddr,
              'trsvcid': args.trsvcid}

    if args.adrfam:
        params['adrfam'] = args.adrfam

    args.client.call('nvmf_subsystem_add_listener', params)


def delete_nvmf_subsystem(args):
    params = {'nqn': args.subsystem_nqn}
    args.client.call('delete_nvmf_subsystem', params, verbose=args.verbose)
