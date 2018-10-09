

def set_nvmf_target_options(client,
                            max_queue_depth=None,
                            max_qpairs_per_ctrlr=None,
                            in_capsule_data_size=None,
                            max_io_size=None,
                            max_subsystems=None,
                            io_unit_size=None):
    """Set NVMe-oF target options.

    Args:
        max_queue_depth: Max number of outstanding I/O per queue (optional)
        max_qpairs_per_ctrlr: Max number of SQ and CQ per controller (optional)
        in_capsule_data_size: Maximum in-capsule data size in bytes (optional)
        max_io_size: Maximum I/O data size in bytes (optional)
        max_subsystems: Maximum number of NVMe-oF subsystems (optional)
        io_unit_size: I/O unit size in bytes (optional)

    Returns:
        True or False
    """
    params = {}

    if max_queue_depth:
        params['max_queue_depth'] = max_queue_depth
    if max_qpairs_per_ctrlr:
        params['max_qpairs_per_ctrlr'] = max_qpairs_per_ctrlr
    if in_capsule_data_size:
        params['in_capsule_data_size'] = in_capsule_data_size
    if max_io_size:
        params['max_io_size'] = max_io_size
    if max_subsystems:
        params['max_subsystems'] = max_subsystems
    if io_unit_size:
        params['io_unit_size'] = io_unit_size
    return client.call('set_nvmf_target_options', params)


def set_nvmf_target_config(client,
                           acceptor_poll_rate=None,
                           conn_sched=None):
    """Set NVMe-oF target subsystem configuration.

    Args:
        acceptor_poll_rate: Acceptor poll period in microseconds (optional)
        conn_sched: Scheduling of incoming connections (optional)

    Returns:
        True or False
    """
    params = {}

    if acceptor_poll_rate:
        params['acceptor_poll_rate'] = acceptor_poll_rate
    if conn_sched:
        params['conn_sched'] = conn_sched
    return client.call('set_nvmf_target_config', params)


def nvmf_create_transport(client,
                          trtype,
                          max_queue_depth=None,
                          max_qpairs_per_ctrlr=None,
                          in_capsule_data_size=None,
                          max_io_size=None,
                          io_unit_size=None,
                          max_aq_depth=None,
                          max_srq_depth=None):
    """NVMf Transport Create options.

    Args:
        trtype: Transport type (ex. RDMA)
        max_queue_depth: Max number of outstanding I/O per queue (optional)
        max_qpairs_per_ctrlr: Max number of SQ and CQ per controller (optional)
        in_capsule_data_size: Maximum in-capsule data size in bytes (optional)
        max_io_size: Maximum I/O data size in bytes (optional)
        io_unit_size: I/O unit size in bytes (optional)
        max_aq_depth: Max size admin quque per controller (optional)
        max_srq_depth: Max number of outstanding I/O per shared receive queue (optional)

    Returns:
        True or False
    """
    params = {}

    params['trtype'] = trtype
    if max_queue_depth:
        params['max_queue_depth'] = max_queue_depth
    if max_qpairs_per_ctrlr:
        params['max_qpairs_per_ctrlr'] = max_qpairs_per_ctrlr
    if in_capsule_data_size:
        params['in_capsule_data_size'] = in_capsule_data_size
    if max_io_size:
        params['max_io_size'] = max_io_size
    if io_unit_size:
        params['io_unit_size'] = io_unit_size
    if max_aq_depth:
        params['max_aq_depth'] = max_aq_depth
    if max_srq_depth:
        params['max_srq_depth'] = max_srq_depth
    return client.call('nvmf_create_transport', params)


def get_nvmf_subsystems(client):
    """Get list of NVMe-oF subsystems.

    Returns:
        List of NVMe-oF subsystem objects.
    """
    return client.call('get_nvmf_subsystems')


def construct_nvmf_subsystem(client,
                             nqn,
                             serial_number,
                             listen_addresses=None,
                             hosts=None,
                             allow_any_host=False,
                             namespaces=None,
                             max_namespaces=0):
    """Construct an NVMe over Fabrics target subsystem.

    Args:
        nqn: Subsystem NQN.
        serial_number: Serial number of virtual controller.
        listen_addresses: Array of listen_address objects (optional).
        hosts: Array of strings containing allowed host NQNs (optional). Default: No hosts allowed.
        allow_any_host: Allow any host (True) or enforce allowed host whitelist (False). Default: False.
        namespaces: Array of namespace objects (optional). Default: No namespaces.
        max_namespaces: Maximum number of namespaces that can be attached to the subsystem (optional). Default: 0 (Unlimited).

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
        params['listen_addresses'] = listen_addresses

    if hosts:
        params['hosts'] = hosts

    if allow_any_host:
        params['allow_any_host'] = True

    if namespaces:
        params['namespaces'] = namespaces

    return client.call('construct_nvmf_subsystem', params)


def nvmf_subsystem_create(client,
                          nqn,
                          serial_number,
                          allow_any_host=False,
                          max_namespaces=0,
                          offload=False):
    """Construct an NVMe over Fabrics target subsystem.

    Args:
        nqn: Subsystem NQN.
        serial_number: Serial number of virtual controller.
        allow_any_host: Allow any host (True) or enforce allowed host whitelist (False). Default: False.
        max_namespaces: Maximum number of namespaces that can be attached to the subsystem (optional). Default: 0 (Unlimited).
        offload: Enable offload to HCA for the subsystem (optional). Default: False.

    Returns:
        True or False
    """
    params = {
        'nqn': nqn,
    }

    if serial_number:
        params['serial_number'] = serial_number

    if allow_any_host:
        params['allow_any_host'] = True

    if max_namespaces:
        params['max_namespaces'] = max_namespaces

    if offload:
        params['offload'] = offload

    return client.call('nvmf_subsystem_create', params)


def nvmf_subsystem_add_listener(client, nqn, trtype, traddr, trsvcid, adrfam):
    """Add a new listen address to an NVMe-oF subsystem.

    Args:
        nqn: Subsystem NQN.
        trtype: Transport type ("RDMA").
        traddr: Transport address.
        trsvcid: Transport service ID.
        adrfam: Address family ("IPv4", "IPv6", "IB", or "FC").

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


def nvmf_subsystem_remove_listener(
        client,
        nqn,
        trtype,
        traddr,
        trsvcid,
        adrfam):
    """Remove existing listen address from an NVMe-oF subsystem.

    Args:
        nqn: Subsystem NQN.
        trtype: Transport type ("RDMA").
        traddr: Transport address.
        trsvcid: Transport service ID.
        adrfam: Address family ("IPv4", "IPv6", "IB", or "FC").

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


def nvmf_subsystem_add_ns(client, nqn, bdev_name, nsid=None, nguid=None, eui64=None, uuid=None):
    """Add a namespace to a subsystem.

    Args:
        nqn: Subsystem NQN.
        bdev_name: Name of bdev to expose as a namespace.
        nsid: Namespace ID (optional).
        nguid: 16-byte namespace globally unique identifier in hexadecimal (optional).
        eui64: 8-byte namespace EUI-64 in hexadecimal (e.g. "ABCDEF0123456789") (optional).
        uuid: Namespace UUID (optional).

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

    if uuid:
        ns['uuid'] = uuid

    params = {'nqn': nqn,
              'namespace': ns}

    return client.call('nvmf_subsystem_add_ns', params)


def nvmf_subsystem_remove_ns(client, nqn, nsid):
    """Remove a existing namespace from a subsystem.

    Args:
        nqn: Subsystem NQN.
        nsid: Namespace ID.

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
        host: Host NQN to remove to the list of allowed host NQNs

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


def delete_nvmf_subsystem(client, nqn):
    """Delete an existing NVMe-oF subsystem.

    Args:
        nqn: Subsystem NQN.

    Returns:
        True or False
    """
    params = {'nqn': nqn}
    return client.call('delete_nvmf_subsystem', params)
