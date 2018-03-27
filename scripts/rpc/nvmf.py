def get_nvmf_subsystems(client, args):
    return client.call('get_nvmf_subsystems')


def construct_nvmf_subsystem(client, args):
    params = {
        'nqn': args.nqn,
        'serial_number': args.serial_number,
    }

    if args.max_namespaces:
        params['max_namespaces'] = args.max_namespaces

    if args.listen:
        params['listen_addresses'] = [dict(u.split(":", 1) for u in a.split(" "))
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

    return client.call('construct_nvmf_subsystem', params)


def nvmf_subsystem_add_listener(client, args):
    listen_address = {'trtype': args.trtype,
                      'traddr': args.traddr,
                      'trsvcid': args.trsvcid}

    if args.adrfam:
        listen_address['adrfam'] = args.adrfam

    params = {'nqn': args.nqn,
              'listen_address': listen_address}

    return client.call('nvmf_subsystem_add_listener', params)


def nvmf_subsystem_remove_listener(client, args):
    listen_address = {'trtype': args.trtype,
                      'traddr': args.traddr,
                      'trsvcid': args.trsvcid}

    if args.adrfam:
        listen_address['adrfam'] = args.adrfam

    params = {'nqn': args.nqn,
              'listen_address': listen_address}

    return client.call('nvmf_subsystem_remove_listener', params)


def nvmf_subsystem_add_ns(client, args):
    ns = {'bdev_name': args.bdev_name}

    if args.nsid:
        ns['nsid'] = args.nsid

    if args.nguid:
        ns['nguid'] = args.nguid

    if args.eui64:
        ns['eui64'] = args.eui64

    params = {'nqn': args.nqn,
              'namespace': ns}

    return client.call('nvmf_subsystem_add_ns', params)


def nvmf_subsystem_remove_ns(client, args):

    params = {'nqn': args.nqn,
              'nsid': args.nsid}

    return client.call('nvmf_subsystem_remove_ns', params)


def nvmf_subsystem_add_host(client, args):
    params = {'nqn': args.nqn,
              'host': args.host}

    return client.call('nvmf_subsystem_add_host', params)


def nvmf_subsystem_remove_host(client, args):
    params = {'nqn': args.nqn,
              'host': args.host}

    return client.call('nvmf_subsystem_remove_host', params)


def nvmf_subsystem_allow_any_host(client, args):
    params = {'nqn': args.nqn}
    params['allow_any_host'] = False if args.disable else True

    return client.call('nvmf_subsystem_allow_any_host', params)


def delete_nvmf_subsystem(client, args):
    params = {'nqn': args.subsystem_nqn}
    return client.call('delete_nvmf_subsystem', params)
