from client import print_dict, print_array, int_arg


def get_nvmf_subsystems(args):
    print_dict(args.client.call('get_nvmf_subsystems'))


def construct_nvmf_subsystem(args):
    params = {
        'nqn': args.nqn,
        'serial_number': args.serial_number,
    }

    if args.listen:
        params['listen_addresses'] = [dict(u.split(":") for u in a.split(" "))
                                      for a in args.listen.split(",")]

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

    args.client.call('construct_nvmf_subsystem', params)


def nvmf_subsystem_add_listener(args):
    listen_address = {'trtype': args.trtype,
                      'traddr': args.traddr,
                      'trsvcid': args.trsvcid}

    if args.adrfam:
        listen_address['adrfam'] = args.adrfam

    params = {'subnqn': args.nqn,
              'listen_address': listen_address}

    args.client.call('nvmf_subsystem_add_listener', params)


def delete_nvmf_subsystem(args):
    params = {'nqn': args.subsystem_nqn}
    args.client.call('delete_nvmf_subsystem', params)
