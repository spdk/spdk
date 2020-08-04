from .helpers import deprecated_alias


@deprecated_alias('set_nvmf_target_max_subsystems')
def nvmf_set_max_subsystems(client,
                            max_subsystems=None):
    """Set NVMe-oF target options.

    Args:
        max_subsystems: Maximum number of NVMe-oF subsystems (e.g. 1024)

    Returns:
        True or False
    """
    params = {}

    params['max_subsystems'] = max_subsystems
    return client.call('nvmf_set_max_subsystems', params)


@deprecated_alias('set_nvmf_target_config')
def nvmf_set_config(client,
                    acceptor_poll_rate=None,
                    conn_sched=None,
                    passthru_identify_ctrlr=None):
    """Set NVMe-oF target subsystem configuration.

    Args:
        acceptor_poll_rate: Acceptor poll period in microseconds (optional)
        conn_sched: (Deprecated) Ignored

    Returns:
        True or False
    """
    params = {}

    if acceptor_poll_rate:
        params['acceptor_poll_rate'] = acceptor_poll_rate
    if conn_sched:
        print("WARNING: conn_sched is deprecated and ignored.")
    if passthru_identify_ctrlr:
        admin_cmd_passthru = {}
        admin_cmd_passthru['identify_ctrlr'] = passthru_identify_ctrlr
        params['admin_cmd_passthru'] = admin_cmd_passthru

    return client.call('nvmf_set_config', params)


def nvmf_create_target(client,
                       name,
                       max_subsystems=0):
    """Create a new NVMe-oF Target.

    Args:
        name: Must be unique within the application
        max_subsystems: Maximum number of NVMe-oF subsystems (e.g. 1024). default: 0 (Uses SPDK_NVMF_DEFAULT_MAX_SUBSYSTEMS).

    Returns:
        The name of the new target.
    """
    params = {}

    params['name'] = name
    params['max_subsystems'] = max_subsystems
    return client.call("nvmf_create_target", params)


def nvmf_delete_target(client,
                       name):
    """Destroy an NVMe-oF Target.

    Args:
        name: The name of the target you wish to destroy

    Returns:
        True on success or False
    """
    params = {}

    params['name'] = name
    return client.call("nvmf_delete_target", params)


def nvmf_get_targets(client):
    """Get a list of all the NVMe-oF targets in this application

    Returns:
        An array of target names.
    """

    return client.call("nvmf_get_targets")


def nvmf_create_transport(client,
                          trtype,
                          tgt_name=None,
                          max_queue_depth=None,
                          max_qpairs_per_ctrlr=None,
                          max_io_qpairs_per_ctrlr=None,
                          in_capsule_data_size=None,
                          max_io_size=None,
                          io_unit_size=None,
                          max_aq_depth=None,
                          num_shared_buffers=None,
                          buf_cache_size=None,
                          max_srq_depth=None,
                          no_srq=False,
                          c2h_success=True,
                          dif_insert_or_strip=None,
                          sock_priority=None,
                          acceptor_backlog=None,
                          abort_timeout_sec=None,
                          no_wr_batching=None,
                          control_msg_num=None):
    """NVMf Transport Create options.

    Args:
        trtype: Transport type (ex. RDMA)
        max_queue_depth: Max number of outstanding I/O per queue (optional)
        max_qpairs_per_ctrlr: Max number of SQ and CQ per controller (optional, deprecated, use max_io_qpairs_per_ctrlr)
        max_io_qpairs_per_ctrlr: Max number of IO qpairs per controller (optional)
        in_capsule_data_size: Maximum in-capsule data size in bytes (optional)
        max_io_size: Maximum I/O data size in bytes (optional)
        io_unit_size: I/O unit size in bytes (optional)
        max_aq_depth: Max size admin quque per controller (optional)
        num_shared_buffers: The number of pooled data buffers available to the transport (optional)
        buf_cache_size: The number of shared buffers to reserve for each poll group (optional)
        max_srq_depth: Max number of outstanding I/O per shared receive queue - RDMA specific (optional)
        no_srq: Boolean flag to disable SRQ even for devices that support it - RDMA specific (optional)
        c2h_success: Boolean flag to disable the C2H success optimization - TCP specific (optional)
        dif_insert_or_strip: Boolean flag to enable DIF insert/strip for I/O - TCP specific (optional)
        acceptor_backlog: Pending connections allowed at one time - RDMA specific (optional)
        abort_timeout_sec: Abort execution timeout value, in seconds (optional)
        no_wr_batching: Boolean flag to disable work requests batching - RDMA specific (optional)
        control_msg_num: The number of control messages per poll group - TCP specific (optional)
    Returns:
        True or False
    """
    params = {}

    params['trtype'] = trtype
    if tgt_name:
        params['tgt_name'] = tgt_name
    if max_queue_depth:
        params['max_queue_depth'] = max_queue_depth
    if max_qpairs_per_ctrlr:
        print("WARNING: max_qpairs_per_ctrlr is deprecated, please use max_io_qpairs_per_ctrlr.")
        params['max_qpairs_per_ctrlr'] = max_qpairs_per_ctrlr
    if max_io_qpairs_per_ctrlr:
        params['max_io_qpairs_per_ctrlr'] = max_io_qpairs_per_ctrlr
    if in_capsule_data_size is not None:
        params['in_capsule_data_size'] = in_capsule_data_size
    if max_io_size:
        params['max_io_size'] = max_io_size
    if io_unit_size:
        params['io_unit_size'] = io_unit_size
    if max_aq_depth:
        params['max_aq_depth'] = max_aq_depth
    if num_shared_buffers:
        params['num_shared_buffers'] = num_shared_buffers
    if buf_cache_size is not None:
        params['buf_cache_size'] = buf_cache_size
    if max_srq_depth:
        params['max_srq_depth'] = max_srq_depth
    if no_srq:
        params['no_srq'] = no_srq
    if c2h_success is not None:
        params['c2h_success'] = c2h_success
    if dif_insert_or_strip:
        params['dif_insert_or_strip'] = dif_insert_or_strip
    if sock_priority is not None:
        params['sock_priority'] = sock_priority
    if acceptor_backlog is not None:
        params['acceptor_backlog'] = acceptor_backlog
    if abort_timeout_sec:
        params['abort_timeout_sec'] = abort_timeout_sec
    if no_wr_batching is not None:
        params['no_wr_batching'] = no_wr_batching
    if control_msg_num is not None:
        params['control_msg_num'] = control_msg_num
    return client.call('nvmf_create_transport', params)


@deprecated_alias('get_nvmf_transports')
def nvmf_get_transports(client, tgt_name=None):
    """Get list of NVMe-oF transports.
    Args:
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        List of NVMe-oF transport objects.
    """

    params = {}

    if tgt_name:
        params = {
            'tgt_name': tgt_name,
        }

    return client.call('nvmf_get_transports', params)


@deprecated_alias('get_nvmf_subsystems')
def nvmf_get_subsystems(client, tgt_name=None):
    """Get list of NVMe-oF subsystems.
    Args:
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        List of NVMe-oF subsystem objects.
    """

    params = {}

    if tgt_name:
        params = {
            'tgt_name': tgt_name,
        }

    return client.call('nvmf_get_subsystems', params)


@deprecated_alias('nvmf_subsystem_create')
def nvmf_create_subsystem(client,
                          nqn,
                          serial_number,
                          tgt_name=None,
                          model_number='SPDK bdev Controller',
                          allow_any_host=False,
                          max_namespaces=0,
                          ana_reporting=False):
    """Construct an NVMe over Fabrics target subsystem.

    Args:
        nqn: Subsystem NQN.
        tgt_name: name of the parent NVMe-oF target (optional).
        serial_number: Serial number of virtual controller.
        model_number: Model number of virtual controller.
        allow_any_host: Allow any host (True) or enforce allowed host whitelist (False). Default: False.
        max_namespaces: Maximum number of namespaces that can be attached to the subsystem (optional). Default: 0 (Unlimited).
        ana_reporting: Enable ANA reporting feature. Default: False.


    Returns:
        True or False
    """
    params = {
        'nqn': nqn,
    }

    if serial_number:
        params['serial_number'] = serial_number

    if model_number:
        params['model_number'] = model_number

    if allow_any_host:
        params['allow_any_host'] = True

    if max_namespaces is not None:
        params['max_namespaces'] = max_namespaces

    if tgt_name:
        params['tgt_name'] = tgt_name

    if ana_reporting:
        params['ana_reporting'] = ana_reporting

    return client.call('nvmf_create_subsystem', params)


def nvmf_subsystem_set_options(client, nqn, trtype, tgt_name=None):
    """Set a transport specific options for an NVMe-oF subsystem.

    Args:
        nqn: Subsystem NQN.
        trtype: NVMe-oF transport type: e.g., rdma, tcp, pcie.
        tgt_name: The name of the parent NVMe-oF target (optional).

    Returns:
        True or False
    """
    params = {'nqn': nqn,
              'trtype': trtype}

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_subsystem_set_options', params)


def nvmf_subsystem_add_listener(client, nqn, trtype, traddr, trsvcid, adrfam, tgt_name=None):
    """Add a new listen address to an NVMe-oF subsystem.

    Args:
        nqn: Subsystem NQN.
        trtype: Transport type ("RDMA").
        traddr: Transport address.
        trsvcid: Transport service ID.
        tgt_name: name of the parent NVMe-oF target (optional).
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

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_subsystem_add_listener', params)


def nvmf_subsystem_remove_listener(
        client,
        nqn,
        trtype,
        traddr,
        trsvcid,
        adrfam,
        tgt_name=None):
    """Remove existing listen address from an NVMe-oF subsystem.

    Args:
        nqn: Subsystem NQN.
        trtype: Transport type ("RDMA").
        traddr: Transport address.
        trsvcid: Transport service ID.
        tgt_name: name of the parent NVMe-oF target (optional).
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

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_subsystem_remove_listener', params)


def nvmf_subsystem_listener_set_ana_state(
        client,
        nqn,
        ana_state,
        trtype,
        traddr,
        trsvcid,
        adrfam,
        tgt_name=None):
    """Set ANA state of a listener for an NVMe-oF subsystem.

    Args:
        nqn: Subsystem NQN.
        ana_state: ANA state to set ("optimized", "non_optimized", or "inaccessible").
        trtype: Transport type ("RDMA").
        traddr: Transport address.
        trsvcid: Transport service ID.
        tgt_name: name of the parent NVMe-oF target (optional).
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
              'listen_address': listen_address,
              'ana_state': ana_state}

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_subsystem_listener_set_ana_state', params)


def nvmf_subsystem_add_ns(client, nqn, bdev_name, tgt_name=None, ptpl_file=None, nsid=None, nguid=None, eui64=None, uuid=None):
    """Add a namespace to a subsystem.

    Args:
        nqn: Subsystem NQN.
        bdev_name: Name of bdev to expose as a namespace.
        tgt_name: name of the parent NVMe-oF target (optional).
        nsid: Namespace ID (optional).
        nguid: 16-byte namespace globally unique identifier in hexadecimal (optional).
        eui64: 8-byte namespace EUI-64 in hexadecimal (e.g. "ABCDEF0123456789") (optional).
        uuid: Namespace UUID (optional).

    Returns:
        The namespace ID
    """
    ns = {'bdev_name': bdev_name}

    if ptpl_file:
        ns['ptpl_file'] = ptpl_file

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

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_subsystem_add_ns', params)


def nvmf_subsystem_remove_ns(client, nqn, nsid, tgt_name=None):
    """Remove a existing namespace from a subsystem.

    Args:
        nqn: Subsystem NQN.
        nsid: Namespace ID.
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        True or False
    """
    params = {'nqn': nqn,
              'nsid': nsid}

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_subsystem_remove_ns', params)


def nvmf_subsystem_add_host(client, nqn, host, tgt_name=None):
    """Add a host NQN to the whitelist of allowed hosts.

    Args:
        nqn: Subsystem NQN.
        host: Host NQN to add to the list of allowed host NQNs
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        True or False
    """
    params = {'nqn': nqn,
              'host': host}

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_subsystem_add_host', params)


def nvmf_subsystem_remove_host(client, nqn, host, tgt_name=None):
    """Remove a host NQN from the whitelist of allowed hosts.

    Args:
        nqn: Subsystem NQN.
        host: Host NQN to remove to the list of allowed host NQNs
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        True or False
    """
    params = {'nqn': nqn,
              'host': host}

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_subsystem_remove_host', params)


def nvmf_subsystem_allow_any_host(client, nqn, disable, tgt_name=None):
    """Configure a subsystem to allow any host to connect or to enforce the host NQN whitelist.

    Args:
        nqn: Subsystem NQN.
        disable: Allow any host (true) or enforce allowed host whitelist (false).
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        True or False
    """
    params = {'nqn': nqn, 'allow_any_host': False if disable else True}

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_subsystem_allow_any_host', params)


@deprecated_alias('delete_nvmf_subsystem')
def nvmf_delete_subsystem(client, nqn, tgt_name=None):
    """Delete an existing NVMe-oF subsystem.

    Args:
        nqn: Subsystem NQN.
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        True or False
    """
    params = {'nqn': nqn}

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_delete_subsystem', params)


def nvmf_subsystem_get_controllers(client, nqn, tgt_name=None):
    """Get list of controllers of an NVMe-oF subsystem.

    Args:
        nqn: Subsystem NQN.
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        List of controller objects of an NVMe-oF subsystem.
    """
    params = {'nqn': nqn}

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_subsystem_get_controllers', params)


def nvmf_subsystem_get_qpairs(client, nqn, tgt_name=None):
    """Get list of queue pairs of an NVMe-oF subsystem.

    Args:
        nqn: Subsystem NQN.
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        List of queue pair objects of an NVMe-oF subsystem.
    """
    params = {'nqn': nqn}

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_subsystem_get_qpairs', params)


def nvmf_subsystem_get_listeners(client, nqn, tgt_name=None):
    """Get list of listeners of an NVMe-oF subsystem.

    Args:
        nqn: Subsystem NQN.
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        List of listener objects of an NVMe-oF subsystem.
    """
    params = {'nqn': nqn}

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_subsystem_get_listeners', params)


def nvmf_get_stats(client, tgt_name=None):
    """Query NVMf statistics.

    Args:
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        Current NVMf statistics.
    """

    params = {}

    if tgt_name:
        params = {
            'tgt_name': tgt_name,
        }

    return client.call('nvmf_get_stats', params)
