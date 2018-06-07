def get_nvmf_subsystems(client):
    return client.call('get_nvmf_subsystems')


def construct_nvmf_subsystem(
        client,
        nqn,
        listen_addresses,
        hosts,
        allow_any_host,
        serial_number,
        namespaces,
        max_namespaces):
    """Construct an NVMe over Fabrics target subsystem.

        Args:
            nqn: Subsystem NQN.
            listen_addresses: Array of listen_address objects.
            hosts: Array of strings containing allowed host NQNs. Default: No hosts allowed.
            allow_any_host: Allow any host (true) or enforce allowed host whitelist (false). Default: false.
            serial_number: Serial number of virtual controller.
            namespaces: Array of namespace objects. Default: No namespaces.
            max_namespaces: Maximum number of namespaces that can be attached to the subsystem. Default: 0 (Unlimited).

        Returns:
            True or False
        """
    params = {
        'nqn': nqn,
        'serial_number': serial_number,
    }

    if max_namespaces:
        params['max_namespaces'] = max_namespaces

    if listen_addresses:
        params['listen_addresses'] = [
            dict(
                u.split(
                    ":",
                    1) for u in a.split(" ")) for a in listen_addresses.split(",")]

    if hosts:
        hosts = []
        for u in hosts.strip().split(" "):
            hosts.append(u)
        params['hosts'] = hosts

    if allow_any_host:
        params['allow_any_host'] = True

    if namespaces:
        namespaces = []
        for u in namespaces.strip().split(" "):
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


def nvmf_subsystem_add_listener(client, trtype, traddr, trsvcid, adrfam, nqn):
    """Add a new listen address to an NVMe-oF subsystem.

            Args:
                trtype: Transport type ("RDMA").
                traddr: Transport address.
                trsvcid: Transport service ID.
                adrfam: Address family ("IPv4", "IPv6", "IB", or "FC").
                nqn: Subsystem NQN.

            Returns:
                True or False
            """
    listen_address = {'trtype': trtype,
                      'traddr': traddr,
                      'trsvcid': trsvcid}

    if adrfam:
        listen_address['adrfam'] = adrfam

    params = {'nqn': nqn,
              'listen_address': listen_address}

    return client.call('nvmf_subsystem_add_listener', params)


def nvmf_subsystem_remove_listener(client, trtype, traddr, trsvcid, adrfam, nqn):
    """Remove existing listen address from an NVMe-oF subsystem.

                Args:
                    trtype: Transport type ("RDMA").
                    traddr: Transport address.
                    trsvcid: Transport service ID.
                    adrfam: Address family ("IPv4", "IPv6", "IB", or "FC").
                    nqn: Subsystem NQN.

                Returns:
                    True or False
                """
    listen_address = {'trtype': trtype,
                      'traddr': traddr,
                      'trsvcid': trsvcid}

    if adrfam:
        listen_address['adrfam'] = adrfam

    params = {'nqn': nqn,
              'listen_address': listen_address}

    return client.call('nvmf_subsystem_remove_listener', params)


def nvmf_subsystem_add_ns(client, bdev_name, nsid, nguid, eui64, nqn):
    """Add a namespace to a subsystem.

                Args:
                    bdev_name: Transport type ("RDMA").
                    nsid: Transport address.
                    nguid: Transport service ID.
                    eui64:
                    nqn: Subsystem NQN.

                Returns:
                    The namespace ID
                """
    ns = {'bdev_name': bdev_name}

    if nsid:
        ns['nsid'] = nsid

    if nguid:
        ns['nguid'] = nguid

    if eui64:
        ns['eui64'] = eui64

    params = {'nqn': nqn,
              'namespace': ns}

    return client.call('nvmf_subsystem_add_ns', params)


def nvmf_subsystem_remove_ns(client, nqn, nsid):
    """Remove a existing namespace from a subsystem.

                    Args:
                        nsid: Transport address.
                        nqn: Subsystem NQN.

                    Returns:
                        True or False
                    """
    params = {'nqn': nqn,
              'nsid': nsid}

    return client.call('nvmf_subsystem_remove_ns', params)


def nvmf_subsystem_add_host(client, nqn, host):
    """Add a host NQN to the whitelist of allowed hosts.

                    Args:
                        nqn: Subsystem NQN.
                        host: Host NQN to add to the list of allowed host NQNs

                    Returns:
                        True or False
                    """
    params = {'nqn': nqn,
              'host': host}

    return client.call('nvmf_subsystem_add_host', params)


def nvmf_subsystem_remove_host(client, nqn, host):
    """Remove a host NQN from the whitelist of allowed hosts.

                    Args:
                        nqn: Subsystem NQN.
                        host: Host NQN to add to the list of allowed host NQNs

                    Returns:
                        True or False
                    """
    params = {'nqn': nqn,
              'host': host}

    return client.call('nvmf_subsystem_remove_host', params)


def nvmf_subsystem_allow_any_host(client, nqn, disable):
    """Configure a subsystem to allow any host to connect or to enforce the host NQN whitelist.

                    Args:
                        nqn: Subsystem NQN.
                        disable: Allow any host (true) or enforce allowed host whitelist (false).

                    Returns:
                        True or False
                    """
    params = {'nqn': nqn, 'allow_any_host': False if disable else True}

    return client.call('nvmf_subsystem_allow_any_host', params)


def delete_nvmf_subsystem(client, subsystem_nqn):
    """Delete an existing NVMe-oF subsystem.

            Args:
                subsystem_nqn: Subsystem NQN.

            Returns:
                True or False
            """
    params = {'nqn': subsystem_nqn}
    return client.call('delete_nvmf_subsystem', params)
